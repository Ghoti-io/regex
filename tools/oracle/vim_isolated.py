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


def ask(inputs, script, input_variable, output_variable,
        process_timeout=5, timeout=900):
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
                    os.killpg(process.pid, signal.SIGKILL)
                    remaining_stdout, remaining_stderr = process.communicate()
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
            status = Path(str(prefix)+'.status')
            output = Path(str(prefix)+'.out')
            stdout = Path(str(prefix)+'.stdout')
            stderr = Path(str(prefix)+'.stderr')
            raw = output.read_bytes() if output.exists() else b''
            detail = dict(command=commands[index], inputs=[case], complete=False,
                returncode=int(status.read_text()) if status.exists() else None,
                **vim_protocol.channels(stdout.read_bytes() if stdout.exists() else b'',
                                        stderr.read_bytes() if stderr.exists() else b''),
                raw_answers=raw.decode('utf-8', 'replace'), raw_answers_hex=raw.hex(),
                output_exists=output.exists())
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
