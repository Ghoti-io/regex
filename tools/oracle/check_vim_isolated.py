"""Execute isolated transport against disposable processes and fault controls."""
import json
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest.mock import patch
import vim_isolated
import vim_protocol


def check():
    checked = 0
    for mode in ('valid', 'local-id', 'wrong-input', 'missing', 'duplicate',
                 'unknown', 'nonzero', 'stderr', 'stdout', 'timeout'):
        with tempfile.TemporaryDirectory(prefix='vim-isolated-controls.') as temporary:
            script = Path(temporary) / 'reference.py'
            script.write_text('''import sys,json,pathlib,time
mode=%r
args=sys.argv
p=next(json.loads(a.split('g:input=',1)[1]) for a in args if 'g:input=' in a)
o=next(json.loads(a.split('g:output=',1)[1]) for a in args if 'g:output=' in a)
c=json.loads(pathlib.Path(p).read_text())
a=dict(id=0,input=c,status='nomatch')
if mode=='timeout':time.sleep(3)
if mode=='local-id':a['id']=1
if mode=='wrong-input':a['input']=['wrong','input']
if mode=='unknown':a.update(status='exception',exception='Vim(let):E9999: injected',phase='pattern',throwpoint='fixture')
if mode!='missing':pathlib.Path(o).write_text(json.dumps(a)+'\\n'+(json.dumps(a)+'\\n' if mode=='duplicate' else ''))
if mode=='stderr':sys.stderr.write('injected')
if mode=='stdout':print('injected')
sys.exit(7 if mode=='nonzero' else 0)
''' % mode)
            def command(name, argv, scratch=None):return argv
            error = None
            answers = None
            with patch.object(vim_isolated.oracle_env, 'MODE', 'host'), \
                 patch.object(vim_isolated.oracle_env, 'command', side_effect=command), \
                 patch.object(vim_isolated.vim_runner, 'BASE', [sys.executable, str(script)]):
                try:
                    answers = vim_isolated.ask([['a','a'],['b','b']], 'fixture-script',
                        'input', 'output', process_timeout=.1 if mode=='timeout' else 2, timeout=10)
                except vim_protocol.Failure as caught:error = caught
            if (error is None) != (mode == 'valid'):
                raise AssertionError((mode,error))
            evidence = error.evidence if error else answers.evidence
            if Path(evidence['scratch_path']).exists() or evidence['scratch_cleanup']['complete'] is not True:
                raise AssertionError('owned scratch leaked after reference execution')
            if not evidence.get('supervisor_script'):
                raise AssertionError('scratch cleanup lost the exact supervisor script')
            if len(evidence['processes']) != 2:
                raise AssertionError('lost incomplete process evidence')
            if mode == 'valid':
                if [r['id'] for r in answers] != [0,0] or evidence['aggregation'] != [
                    dict(index=0,process=0,local_id=0,input=['a','a']),
                    dict(index=1,process=1,local_id=0,input=['b','b'])]:
                    raise AssertionError('process identity was silently changed')
                if any(not r['complete'] for r in evidence['processes']):
                    raise AssertionError('unvalidated process')
            elif evidence['complete'] is not False:
                raise AssertionError('failure accepted')
            checked += 1
    for acquisition_failed in (False, True):
        for cleanup_fault in ('os-error', 'timeout', 'nonzero'):
            def command(name, argv, scratch=None):
                root = Path(scratch)
                (root / '0.status').write_text('0')
                (root / '0.out').write_text(json.dumps(dict(id=0,
                    input=['a', 'a'], status='nomatch')) + '\n')
                return ['fake', 'run', '--rm', 'sh', argv[-1]]
            class Process:
                returncode = 7 if acquisition_failed else 0
                def __enter__(self):return self
                def __exit__(self, *args):pass
                def communicate(self, timeout=None):return b'', b''
            fault = (OSError('injected cleanup error') if cleanup_fault == 'os-error'
                     else subprocess.TimeoutExpired(['cleanup'], 30))
            def cleanup(command, **kwargs):
                if cleanup_fault != 'nonzero':raise fault
                return subprocess.CompletedProcess(command, 7, b'', b'injected cleanup failure')
            error = None
            with patch.object(vim_isolated.oracle_env, 'MODE', 'container'), \
                 patch.object(vim_isolated.oracle_env, 'command', side_effect=command), \
                 patch.object(vim_isolated.subprocess, 'Popen', return_value=Process()), \
                 patch.object(vim_isolated.subprocess, 'run', side_effect=cleanup):
                try:vim_isolated.ask([['a', 'a']], 'fixture', 'input', 'output')
                except vim_protocol.Failure as caught:error = caught
            if error is None or error.evidence['complete'] is not False:
                raise AssertionError('cleanup failure accepted or lost evidence')
            detail = error.evidence
            if not detail.get('cleanup_error') or len(detail['processes']) != 1:
                raise AssertionError('cleanup failure lost raw process evidence')
            if acquisition_failed and detail.get('error') != 'isolated Vim supervisor failed':
                raise AssertionError('cleanup masked original acquisition failure')
            if json.loads(detail['processes'][0]['raw_answers'])['id'] != 0:
                raise AssertionError('cleanup changed local raw identity')
            checked += 1
    return checked + scratch_controls() + portable_deadline_controls() + cancellation_controls() + outer_cancellation_controls() + retained_failure_channels()


def scratch_controls():
    checked = 0
    for phase in ('success', 'acquisition-failure', 'setup-failure'):
        for removal_fails in (False, True):
            with tempfile.TemporaryDirectory(prefix='vim-scratch-controls.') as folder:
                root = Path(folder)
                owned, neighbor = root / 'owned', root / 'neighbor'
                owned.mkdir()
                neighbor.mkdir()
                (neighbor / 'sentinel').write_bytes(b'intact')
                def command(name, argv, scratch=None):
                    if phase == 'setup-failure':
                        raise OSError('injected setup error')
                    work = Path(scratch)
                    (work / '0.status').write_text('0')
                    (work / '0.out').write_text(json.dumps(dict(id=0,
                        input=['a', 'a'], status='nomatch')) + '\n')
                    return ['fixture', 'run', '--rm', 'sh', argv[-1]]
                class Process:
                    returncode = 7 if phase == 'acquisition-failure' else 0
                    def __enter__(self):return self
                    def __exit__(self, *args):pass
                    def communicate(self, timeout=None):return b'', b''
                original_remove = vim_isolated.shutil.rmtree
                def remove(path):
                    if Path(path) != owned:
                        raise AssertionError('wrong scratch owner')
                    if removal_fails:
                        raise OSError('injected scratch removal error')
                    return original_remove(path)
                error = None
                with patch.object(vim_isolated.tempfile, 'mkdtemp', return_value=str(owned)), \
                     patch.object(vim_isolated.oracle_env, 'MODE', 'container'), \
                     patch.object(vim_isolated.oracle_env, 'command', side_effect=command), \
                     patch.object(vim_isolated.subprocess, 'Popen', return_value=Process()), \
                     patch.object(vim_isolated.subprocess, 'run', return_value=
                         subprocess.CompletedProcess([], 0, b'', b'')), \
                     patch.object(vim_isolated.shutil, 'rmtree', side_effect=remove):
                    try:
                        answers = vim_isolated.ask([['a', 'a']], 'fixture', 'input', 'output')
                        evidence = answers.evidence
                    except vim_protocol.Failure as caught:
                        error, evidence = caught, caught.evidence
                if (error is None) != (phase == 'success' and not removal_fails):
                    raise AssertionError((phase, removal_fails, error))
                if phase == 'acquisition-failure' and str(error) != 'isolated Vim supervisor failed':
                    raise AssertionError('scratch cleanup masked acquisition failure')
                if phase == 'setup-failure' and str(error) != 'injected setup error':
                    raise AssertionError('scratch cleanup masked setup failure')
                if owned.exists() != removal_fails or (neighbor / 'sentinel').read_bytes() != b'intact':
                    raise AssertionError('scratch cleanup ownership failure')
                if evidence['executed_script'] != 'fixture' or not evidence['supervisor_script']:
                    raise AssertionError('scratch cleanup lost exact scripts')
                if phase != 'setup-failure':
                    if json.loads(evidence['processes'][0]['raw_answers'])['id'] != 0:
                        raise AssertionError('scratch cleanup lost raw local envelope')
                    if not evidence.get('command') or not evidence['processes'][0]['command']:
                        raise AssertionError('scratch cleanup lost commands')
                if phase == 'success' and evidence['aggregation'] != [
                        dict(index=0, process=0, local_id=0, input=['a', 'a'])]:
                    raise AssertionError('scratch cleanup lost aggregation')
                if removal_fails and evidence['scratch_cleanup']['complete'] is not False:
                    raise AssertionError('scratch removal failure accepted')
                checked += 1
    return checked


def portable_deadline_controls():
    checked = 0
    for engine in ('docker', 'podman'):
        for deadline in (.1, 900):
            class Process:
                returncode = 0
                def __init__(self, command, **kwargs):
                    root = Path(command[-1]).parent
                    (root / '0.status').write_text('0')
                    (root / '0.out').write_text(json.dumps(dict(id=0,
                        input=['a', 'a'], status='nomatch')) + '\n')
                def __enter__(self):return self
                def __exit__(self, *args):pass
                def communicate(self, timeout=None):return b'', b''
            with patch.object(vim_isolated.oracle_env, 'MODE', 'container'), \
                 patch.object(vim_isolated.oracle_env, 'ENGINE', engine), \
                 patch.object(vim_isolated.oracle_env, 'ensure'), \
                 patch.object(vim_isolated.subprocess, 'Popen', side_effect=Process), \
                 patch.object(vim_isolated.subprocess, 'run', return_value=
                     subprocess.CompletedProcess([], 0, b'', b'')):
                answers = vim_isolated.ask([['a', 'a']], 'fixture', 'input', 'output',
                                          timeout=deadline)
            evidence = answers.evidence
            command = evidence['command']
            if (command[:2] != [engine, 'run'] or '--name' not in command or
                    '--timeout' in command or command[-5:-1] != [
                        'timeout', '--kill-after=1s', str(deadline) + 's', 'sh'] or
                    evidence['supervisor_timeout_seconds'] != deadline or
                    Path(evidence['scratch_path']).exists()):
                raise AssertionError('unbounded or nonportable supervisor command')
            checked += 1
    return checked


def cancellation_controls():
    checked = 0
    for partial in (False, True):
        def command(name, argv, scratch=None):
            root = Path(scratch)
            if partial:
                (root / '0.status').write_text('0')
                (root / '0.out').write_text(json.dumps(dict(id=0,
                    input=['a', 'a'], status='nomatch')) + '\n')
                (root / '1.out').write_bytes(b'{"status"')
                (root / '1.stderr').write_bytes(b'partial diagnostic\xff')
            return ['fixture', 'run', '--rm', 'sh', argv[-1]]
        class Process:
            pid, returncode, calls = 777, -9, 0
            def __enter__(self):return self
            def __exit__(self, *args):pass
            def communicate(self, timeout=None):
                self.calls += 1
                if self.calls == 1:raise KeyboardInterrupt()
                return b'partial supervisor output', b'partial supervisor error'
        error = None
        with patch.object(vim_isolated.oracle_env, 'MODE', 'container'), \
             patch.object(vim_isolated.oracle_env, 'command', side_effect=command), \
             patch.object(vim_isolated.subprocess, 'Popen', return_value=Process()), \
             patch.object(vim_isolated.os, 'killpg') as killed, \
             patch.object(vim_isolated.subprocess, 'run', return_value=
                 subprocess.CompletedProcess([], 0, b'', b'')):
            try:vim_isolated.ask([['a', 'a'], ['b', 'b']], 'fixture', 'input', 'output')
            except vim_protocol.Failure as caught:error = caught
        if error is None:raise AssertionError('cancellation escaped without evidence')
        evidence = error.evidence
        if (not evidence.get('interrupted') or evidence['complete'] or
                evidence['stdout_hex'] != b'partial supervisor output'.hex() or
                evidence['stderr_hex'] != b'partial supervisor error'.hex() or
                len(evidence['processes']) != 2 or Path(evidence['scratch_path']).exists() or
                evidence['cleanup']['returncode'] != 0):
            raise AssertionError('cancellation lost partial evidence or owned cleanup')
        if partial and (evidence['processes'][1]['raw_answers_hex'] != b'{"status"'.hex() or
                        evidence['processes'][1]['stderr_hex'] != b'partial diagnostic\xff'.hex()):
            raise AssertionError('cancellation discarded process files')
        if killed.call_args.args != (777, vim_isolated.signal.SIGKILL):
            raise AssertionError('cancellation did not stop owned process group')
        checked += 1
    return checked


def outer_cancellation_controls():
    checked = 0
    for phase in ('setup', 'validation'):
        def command(name, argv, scratch=None):
            if phase == 'setup':raise KeyboardInterrupt()
            root = Path(scratch)
            (root / '0.status').write_text('0')
            (root / '0.out').write_text(json.dumps(dict(id=0,
                input=['a', 'a'], status='nomatch')) + '\n')
            return ['fixture', 'run', '--rm', 'sh', argv[-1]]
        class Process:
            returncode = 0
            def __enter__(self):return self
            def __exit__(self, *args):pass
            def communicate(self, timeout=None):return b'', b''
        error = None
        with patch.object(vim_isolated.oracle_env, 'MODE', 'container'), \
             patch.object(vim_isolated.oracle_env, 'command', side_effect=command), \
             patch.object(vim_isolated.subprocess, 'Popen', return_value=Process()), \
             patch.object(vim_isolated.subprocess, 'run', return_value=
                 subprocess.CompletedProcess([], 0, b'', b'')), \
             patch.object(vim_isolated.vim_protocol, 'validate_process', side_effect=KeyboardInterrupt()):
            try:vim_isolated.ask([['a', 'a']], 'fixture', 'input', 'output')
            except vim_protocol.Failure as caught:error = caught
        if error is None:raise AssertionError('outer interruption escaped without evidence')
        evidence = error.evidence
        if (not evidence.get('interrupted') or evidence['error'] != 'KeyboardInterrupt' or
                evidence['complete'] or Path(evidence['scratch_path']).exists() or
                evidence['executed_script'] != 'fixture'):
            raise AssertionError('outer cancellation lost evidence or scratch cleanup')
        if phase == 'validation' and not evidence['processes'][0]['raw_answers']:
            raise AssertionError('validation cancellation discarded retained raw envelope')
        checked += 1
    return checked


def retained_failure_channels():
    checked = 0
    for status in (b'bad', b'', b'\xff', b'256'):
        def command(name, argv, scratch=None):
            work = Path(scratch)
            (work / '0.status').write_bytes(status)
            (work / '0.out').write_bytes(b'partial raw answer')
            (work / '0.stderr').write_bytes(b'partial diagnostic\xff')
            return ['fixture', 'sh', argv[-1]]
        class Process:
            returncode = 0
            def __enter__(self):return self
            def __exit__(self, *args):pass
            def communicate(self, timeout=None):return b'', b''
        with patch.object(vim_isolated.oracle_env, 'MODE', 'host'), \
             patch.object(vim_isolated.oracle_env, 'command', side_effect=command), \
             patch.object(vim_isolated.subprocess, 'Popen', return_value=Process()):
            try:vim_isolated.ask([['a', 'a']], 'fixture', 'input', 'output')
            except vim_protocol.Failure as error:detail = error.evidence
            else:raise AssertionError('malformed process status accepted')
        if len(detail['processes']) != 1 or detail['complete']:
            raise AssertionError('malformed status discarded raw process')
        process = detail['processes'][0]
        if (process['raw_answers_hex'] != b'partial raw answer'.hex() or
                process['stderr_hex'] != b'partial diagnostic\xff'.hex() or
                process['status_hex'] != status.hex() or process['returncode'] is not None):
            raise AssertionError('malformed status lost channels')
        checked += 1

    for unreadable in ('status', 'stdout'):
        def command(name, argv, scratch=None):
            work = Path(scratch)
            (work / '0.status').write_text('0')
            (work / '0.out').write_bytes(b'partial raw answer')
            (work / '0.stdout').write_bytes(b'partial stdout')
            (work / '0.stderr').write_bytes(b'partial stderr')
            return ['fixture', 'sh', argv[-1]]
        original = Path.read_bytes
        def read(path):
            if path.name == '0.' + unreadable:
                raise OSError('injected channel read failure')
            return original(path)
        with patch.object(vim_isolated.oracle_env, 'MODE', 'host'), \
             patch.object(vim_isolated.oracle_env, 'command', side_effect=command), \
             patch.object(vim_isolated.subprocess, 'Popen', return_value=Process()), \
             patch.object(Path, 'read_bytes', read):
            try:vim_isolated.ask([['a', 'a']], 'fixture', 'input', 'output')
            except vim_protocol.Failure as error:detail = error.evidence
            else:raise AssertionError('unreadable process channel accepted')
        process = detail['processes'][0]
        if (process['raw_answers_hex'] != b'partial raw answer'.hex() or
                process['stderr_hex'] != b'partial stderr'.hex() or
                unreadable not in process['read_errors']):
            raise AssertionError('unreadable channel discarded other evidence')
        checked += 1

    for drain_failure in (False, True):
        def command(name, argv, scratch=None):return ['fixture', 'sh', argv[-1]]
        class TimedProcess:
            pid = 12345
            returncode = None
            calls = 0
            timeouts = []
            def __enter__(self):return self
            def __exit__(self, *args):pass
            def communicate(self, timeout=None):
                self.calls += 1
                self.timeouts.append(timeout)
                if self.calls == 1:
                    raise subprocess.TimeoutExpired(['fixture'], timeout,
                        output=b'initial\xff', stderr=b'early')
                if drain_failure:
                    raise subprocess.TimeoutExpired(['fixture'], timeout,
                        output=b'full supervisor output\xfe', stderr=b'late\xfd')
                return b'full supervisor output\xfe', b'late\xfd'
        process = TimedProcess()
        process.timeouts = []
        with patch.object(vim_isolated.oracle_env, 'MODE', 'host'), \
             patch.object(vim_isolated.oracle_env, 'command', side_effect=command), \
             patch.object(vim_isolated.subprocess, 'Popen', return_value=process), \
             patch.object(vim_isolated.os, 'killpg'):
            try:vim_isolated.ask([['a', 'a']], 'fixture', 'input', 'output', timeout=1)
            except vim_protocol.Failure as error:detail = error.evidence
            else:raise AssertionError('supervisor timeout accepted')
        if (detail['stdout_hex'] != b'full supervisor output\xfe'.hex() or
                detail['stderr_hex'] != b'late\xfd'.hex() or
                detail['interruption_channels']['stdout_hex'] != b'initial\xff'.hex() or
                detail['post_kill_channels']['stdout_hex'] != b'full supervisor output\xfe'.hex() or
                process.timeouts != [1, 10]):
            raise AssertionError('post-kill diagnostics lost or drain unbounded')
        if drain_failure and not detail.get('post_kill_error'):
            raise AssertionError('failed output drain not recorded')
        checked += 1
    return checked


if __name__ == '__main__':print('%d isolated process controls passed' % check())
