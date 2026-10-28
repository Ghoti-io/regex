"""Normative Vim transport: one fresh, bounded process per input.

The shell is only a process supervisor. Every raw reply keeps its process-local
id zero; the returned list uses an explicit, validated input-to-process mapping.
"""
import json
import math
import os
import signal
import shutil
from pathlib import Path
import shlex
import subprocess
import tempfile
import uuid

import oracle_env
import vim_protocol
import vim_runner


DEFAULT_DEADLINE = 900.0


def deadline():
    """The whole-run deadline, `GRX_VIM_DEADLINE_SECONDS` if the caller set it.

    A full generation of the vim vectors is some ten minutes on an idle
    machine, so a fixed fifteen minutes fails on a loaded one without the
    reference having done anything wrong. A run that sets it says so in the
    evidence, which records `timeout`, and a malformed value is an error and
    never the default.
    """
    value = os.environ.get('GRX_VIM_DEADLINE_SECONDS')
    if value is None:
        return DEFAULT_DEADLINE
    try:
        seconds = float(value)
    except ValueError:
        raise ValueError('GRX_VIM_DEADLINE_SECONDS must be a number')
    return seconds


def ask(inputs, script, input_variable, output_variable,
        process_timeout=5, timeout=None):
    if timeout is None:
        timeout = deadline()
    if not inputs:
        raise ValueError('isolated reference requires a nonempty population')
    if any(not math.isfinite(value) or value <= 0 for value in (process_timeout, timeout)):
        raise ValueError('isolated reference deadlines must be finite and positive')
    evidence = dict(complete=False, transport='fresh-process-per-case-v1',
        inputs=inputs, executed_script=script, pins=vim_runner.PINS,
        process_timeout=process_timeout, timeout=timeout, processes=[],
        aggregation=[], raw_answers='', raw_answers_hex='')
    work = None
    failure_active = True
    try:
        work = Path(tempfile.mkdtemp(prefix='vim_isolated.'))
        evidence['scratch_path'] = str(work)
        result = _ask_in_work(inputs, script, input_variable, output_variable,
                              process_timeout, timeout, work, evidence)
        failure_active = False
        return result
    except (Exception, KeyboardInterrupt) as error:
        evidence['complete'] = False
        if isinstance(error, KeyboardInterrupt):evidence['interrupted'] = True
        evidence.setdefault('error', str(error) or type(error).__name__)
        raise vim_protocol.Failure(evidence['error'], evidence) from error
    finally:
        if work is not None:
            try:
                shutil.rmtree(work)
                evidence['scratch_cleanup'] = dict(path=str(work), complete=True)
            except OSError as error:
                evidence['complete'] = False
                evidence['scratch_cleanup'] = dict(path=str(work), complete=False,
                                                   error=str(error))
                if not failure_active:
                    evidence['error'] = 'owned reference scratch cleanup failed'
                    raise vim_protocol.Failure(evidence['error'], evidence) from error


def _ask_in_work(inputs, script, input_variable, output_variable,
                 process_timeout, timeout, work, evidence):
    script_path = work / 'run.vim'
    script_path.write_text(script, newline='\n')
    commands = []
    supervisor = ['#!/bin/sh', 'set -eu']
    for index, case in enumerate(inputs):
        prefix = work / str(index)
        in_path = str(prefix) + '.in'
        out_path = str(prefix) + '.out'
        Path(in_path).write_text(json.dumps(case) + '\n', newline='\n')
        command = ['timeout', '--kill-after=1s', str(process_timeout) + 's']
        command += vim_runner.BASE + vim_runner.PINS + [
            '--cmd', 'let g:%s=%s' % (input_variable, json.dumps(in_path)),
            '--cmd', 'let g:%s=%s' % (output_variable, json.dumps(out_path)),
            '-c', 'source ' + str(script_path)]
        commands.append(command)
        supervisor += ['set +e', shlex.join(command) + ' >' +
            shlex.quote(str(prefix)+'.stdout') + ' 2>' +
            shlex.quote(str(prefix)+'.stderr'), 'status=$?', 'set -e',
            'printf "%s\\n" "$status" >' + shlex.quote(str(prefix)+'.status'),
            '[ "$status" = 0 ] || exit "$status"']
    supervisor_path = work / 'supervise.sh'
    evidence['supervisor_script'] = '\n'.join(supervisor) + '\n'
    supervisor_path.write_text(evidence['supervisor_script'])
    evidence['supervisor_timeout_seconds'] = timeout
    command = oracle_env.command('vim', [
        'timeout', '--kill-after=1s', str(timeout) + 's',
        'sh', str(supervisor_path)], scratch=str(work))
    container_name = None
    if oracle_env.MODE != 'host':
        container_name = 'grx-vim-isolated-' + uuid.uuid4().hex
        command.remove('--rm')
        command[command.index('run')+1:command.index('run')+1] = ['--name', container_name]
    evidence['command'] = command
    try:
        try:
            with subprocess.Popen(command, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                    stderr=subprocess.PIPE, start_new_session=True) as process:
                try:
                    stdout, stderr = process.communicate(timeout=timeout)
                except BaseException as error:
                    evidence['interruption_channels'] = vim_protocol.channels(
                        getattr(error, 'stdout', b''), getattr(error, 'stderr', b''))
                    try:
                        os.killpg(process.pid, signal.SIGKILL)
                    except ProcessLookupError:
                        pass
                    try:
                        remaining_stdout, remaining_stderr = process.communicate(timeout=10)
                    except (OSError, subprocess.TimeoutExpired) as drain_error:
                        evidence['post_kill_error'] = str(drain_error)
                        remaining_stdout = getattr(drain_error, 'stdout', None)
                        remaining_stderr = getattr(drain_error, 'stderr', None)
                    if remaining_stdout is None:
                        remaining_stdout = getattr(error, 'stdout', b'')
                    if remaining_stderr is None:
                        remaining_stderr = getattr(error, 'stderr', b'')
                    evidence['post_kill_channels'] = vim_protocol.channels(
                        remaining_stdout, remaining_stderr)
                    if isinstance(error, subprocess.TimeoutExpired):
                        raise subprocess.TimeoutExpired(error.cmd, error.timeout,
                            output=remaining_stdout, stderr=remaining_stderr) from error
                    if not isinstance(error, KeyboardInterrupt):
                        raise
                    stdout, stderr = remaining_stdout, remaining_stderr
                    evidence.update(error='KeyboardInterrupt', interrupted=True)
                evidence.update(returncode=process.returncode,
                    **vim_protocol.channels(stdout, stderr))
        except (OSError, subprocess.TimeoutExpired, KeyboardInterrupt) as error:
            evidence.update(returncode=None, error=type(error).__name__ + ': ' + str(error),
                **vim_protocol.channels(getattr(error, 'stdout', ''), getattr(error, 'stderr', '')))
        # Preserve all completed and partial process evidence before validating.
        for index, case in enumerate(inputs):
            prefix = work / str(index)
            read_errors = {}
            def saved(suffix):
                path = Path(str(prefix) + '.' + suffix)
                try:
                    return path.read_bytes()
                except FileNotFoundError:
                    return None
                except OSError as error:
                    read_errors[suffix] = str(error)
                    return None
            status = saved('status')
            raw = saved('out')
            stdout = saved('stdout') or b''
            stderr = saved('stderr') or b''
            returncode = None
            status_error = None
            if status is not None:
                try:
                    returncode = int(status)
                    if not 0 <= returncode <= 255:
                        raise ValueError('status outside shell exit range')
                except ValueError as error:
                    status_error = str(error)
                    returncode = None
            detail = dict(command=commands[index], inputs=[case], complete=False,
                returncode=returncode,
                status_raw=vim_protocol.text(status),
                status_hex=status.hex() if status is not None else '',
                **vim_protocol.channels(stdout, stderr),
                raw_answers=(raw or b'').decode('utf-8', 'replace'),
                raw_answers_hex=(raw or b'').hex(), output_exists=raw is not None)
            if status_error or read_errors:
                detail.update(error=status_error or 'process evidence read failed',
                              read_errors=read_errors, status_error=status_error)
            evidence['processes'].append(detail)
        if (evidence.get('error') or evidence['returncode'] != 0 or
                evidence['stdout'] or vim_protocol.diagnostics(evidence['stderr'])):
            raise vim_protocol.Failure('isolated Vim supervisor failed', evidence)
        answers = []
        for index, (case, detail) in enumerate(zip(inputs, evidence['processes'])):
            local = vim_protocol.validate_process(detail, [case], detail['output_exists'])
            if len(local) != 1:
                raise ValueError('isolated process population')
            answers.append(local[0])
            evidence['aggregation'].append(dict(index=index, process=index, local_id=0, input=case))
        evidence['complete'] = True
        return vim_protocol.Answers(answers, evidence)
    except (ValueError, vim_protocol.Failure) as error:
        evidence.setdefault('error', str(error))
        raise vim_protocol.Failure(str(error), evidence) from error
    finally:
        if container_name:
            # The name is unique to this acquisition. Kill only owned work,
            # including a container whose CLI timed out before cleanup.
            cleanup_command = [oracle_env.ENGINE, 'rm', '--force', container_name]
            cleanup_error = None
            try:
                cleanup = subprocess.run(cleanup_command, capture_output=True, timeout=30)
                evidence['cleanup'] = dict(command=cleanup_command,
                    returncode=cleanup.returncode,
                    **vim_protocol.channels(cleanup.stdout, cleanup.stderr))
                if cleanup.returncode:
                    cleanup_error = 'owned reference container cleanup returned nonzero'
            except (OSError, subprocess.TimeoutExpired) as error:
                cleanup_error = str(error)
                evidence['cleanup'] = dict(command=cleanup_command, returncode=None,
                    error=cleanup_error, **vim_protocol.channels(
                        getattr(error, 'stdout', ''), getattr(error, 'stderr', '')))
            if cleanup_error is not None:
                # Keep the original acquisition error and every raw process
                # record even when cleanup itself fails or times out.
                evidence['complete'] = False
                evidence['cleanup_error'] = cleanup_error
                raise vim_protocol.Failure('owned reference container cleanup failed', evidence)
