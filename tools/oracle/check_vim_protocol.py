"""Execution controls for the shared Vim reference protocol (no oracle needed)."""
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
from unittest.mock import patch

import replace_diff
import vim_diff
import vim_protocol


def check():
    checked = 0

    def require(condition, detail):
        if not condition:
            raise AssertionError(detail)

    def envelope(index, inputs, **answer):
        return dict(id=index, input=inputs[index], **answer)

    for engine in (1, 2):
        for replacement in (False, True):
            cases = [('a', 'a\u0085\u2028\u2029'), ('b', 'a')]
            inputs = [[p, s, 'X'] if replacement else [p, s] for p, s in cases]
            valid = [envelope(i, inputs, **(dict(status='ok', hex='58') if replacement
                     else dict(status='match', fields=['0:1'] + [''] * 9))) for i in range(2)]
            refusal = dict(status='exception', exception='Vim(let):E65: Illegal back reference',
                           throwpoint='control line 1', phase='replacement' if replacement else 'pattern')
            if replacement:
                refusal.update(pattern_exception='Vim(call):E65: Illegal back reference', pattern_throwpoint='isolation')
            runtime = dict(status='exception', exception="Vim(let):E363: Pattern uses more memory than 'maxmempattern'",
                           throwpoint='control line 2', phase='replacement' if replacement else 'pattern')
            unknown = dict(status='exception', exception='Vim(let):E9999: injected unknown',
                           throwpoint='control line 3', phase='replacement' if replacement else 'pattern')
            malformed = dict(status='ok', hex='ff') if replacement else dict(status='match', fields=['0:2'] + [''] * 9)
            modes = [
                ('valid', valid, 0, '', True),
                ('refused', [envelope(0, inputs, **refusal), valid[1]], 0, '', True),
                ('runtime', [envelope(0, inputs, **runtime), valid[1]], 0, '', False),
                ('nested-E65', [envelope(0, inputs, status='exception', exception='Vim(let):E65: Illegal back reference', phase='replacement', throwpoint='nested'), valid[1]], 0, '', False),
                ('nested-E54', [envelope(0, inputs, status='exception', exception='Vim(let):E54: Unmatched \\(', phase='replacement', throwpoint='nested'), valid[1]], 0, '', False),
                ('unknown', [envelope(0, inputs, **unknown), valid[1]], 0, '', False),
                ('unknown-thrown', [envelope(0, inputs, status='exception', exception='unrecognized', throwpoint='control', phase='pattern'), valid[1]], 0, '', False),
                ('nonzero-complete', valid, 9, '', False),
                ('diagnostic-complete', valid, 0, 'injected diagnostic', False),
                ('wrapper-substring', valid, 0, 'failure Emulate Docker CLI using podman.', False),
                ('short', valid[:1], 0, '', False),
                ('long', valid + valid[:1], 0, '', False),
                ('reordered', valid[::-1], 0, '', False),
                ('wrong-input', [dict(valid[0], input=['other']), valid[1]], 0, '', False),
                ('wrong-index-type', [dict(valid[0], id=False), valid[1]], 0, '', False),
                ('malformed', [envelope(0, inputs, **malformed), valid[1]], 0, '', False),
                ('untagged', ['compile', valid[1]], 0, '', False),
                ('duplicate-status', ['{"status":"error",' + json.dumps(valid[0])[1:], valid[1]], 0, '', False),
                ('duplicate-id', ['{"id":99,' + json.dumps(valid[0])[1:], valid[1]], 0, '', False),
                ('duplicate-input', ['{"input":["wrong"],' + json.dumps(valid[0])[1:], valid[1]], 0, '', False),
                ('wrong-status-type', [dict(valid[0], status=[]), valid[1]], 0, '', False),
                ('extra-fields', [dict(valid[0], exception='hidden error'), valid[1]], 0, '', False),
                ('missing-file', valid, 0, '', False),
                ('unexpected-stdout', valid, 0, '', False),
                ('missing-exception', [envelope(0, inputs, status='exception'), valid[1]], 0, '', False),
            ]
            for name, population, code, diagnostic, wanted in modes:
                def command(extra, scratch=None):
                    # An actual process writes the file transport, then returns
                    # status/diagnostics independently of the answer population.
                    if replacement:
                        require('--cmd' in extra and 'set re=%d' % engine in extra, extra)
                    else:
                        source = extra[extra.index('-c') + 1]
                        require(source.startswith('source '), extra)
                        require(Path(source[7:]).read_text().splitlines()[0] ==
                                'set regexpengine=%d' % engine, source)
                    target = 'g:o=' if replacement else 'g:vimdiff_out='
                    out = next(json.loads(arg.split(target, 1)[1]) for arg in extra if target in arg)
                    script = ('import pathlib,sys; pathlib.Path(sys.argv[1]).write_text(sys.argv[2]); '
                              'sys.stderr.write(sys.argv[3]); sys.exit(int(sys.argv[4]))')
                    if name == 'missing-file':
                        script = 'import sys; sys.exit(0)'
                    elif name == 'unexpected-stdout':
                        script = script.replace('sys.exit(int(sys.argv[4]))', 'sys.stdout.write("unexpected stdout")')
                    payload = '\n'.join(json.dumps(row, ensure_ascii=False) if isinstance(row, dict) else row for row in population) + '\n'
                    return [sys.executable, '-c', script, out, payload, diagnostic, str(code)]
                with tempfile.TemporaryDirectory() as work:
                    report_path = os.path.join(work, 'report.json')
                    report = vim_diff.failure_report('replacement' if replacement else 'match')
                    def run():
                        if replacement:
                            answers = replace_diff.ask_vim([('', p, s, 'X') for p, s in cases], engine=engine)
                        else:
                            answers = vim_diff.ask_vim(cases, engine=engine)
                        report['reference_evidence'] = answers.evidence
                        report['complete'] = True
                        return 0
                    with patch.object(vim_diff.vim_runner, 'command', side_effect=command):
                        try:
                            result = vim_diff.run_reporting(report_path, report, run)
                        except vim_protocol.Failure:
                            require(not wanted, (engine, replacement, name))
                        else:
                            require(wanted and result == 0, (engine, replacement, name))
                    saved = json.load(open(report_path, encoding='utf-8'))
                    require(saved['complete'] == wanted, saved)
                    evidence = saved.get('reference_evidence', saved.get('execution_evidence'))
                    require(evidence['returncode'] == code and evidence['stderr'] == diagnostic, saved)
                    require((name == 'missing-file') or (bool(evidence['raw_answers']) and ('raw_answers_hex' in evidence)), saved)
                    if not wanted:
                        require(saved['error'] and not saved['completed_profiles'], saved)
                checked += 1

        # The library can also print every answer and then fail. Neither side
        # may turn that complete-looking population into a successful report.
        for code, diagnostic in ((7, ''), (0, 'driver diagnostic')):
            script = 'import sys; print("nomatch"); sys.stderr.write(sys.argv[1]); sys.exit(int(sys.argv[2]))'
            try:
                vim_diff.ask_ours([sys.executable, '-c', script, diagnostic, str(code)], [('a', 'a')])
            except vim_protocol.Failure as error:
                require(error.evidence['stdout'] == 'nomatch\n', error.evidence)
                require(error.evidence['returncode'] == code, error.evidence)
            else:
                raise AssertionError('failed matching process accepted')
            checked += 1

        # Exact UTF-8, empty and status-like literal successes stay successful.
        for literal in ('syntax', 'compile', 'error', ''):
            answer = vim_protocol.replacement_answer(dict(status='ok', hex=literal.encode().hex()))
            require(answer == dict(status='ok', text=literal), answer)
            checked += 1

    for line in ('syntax', 'compile', 'error', '{}', '[]',
                 '{"status":"exception","exception":"Vim(let):E54: Unmatched ("}'):
        try:
            replace_diff.parse_vim_reply(line, reference=True)
        except ValueError:
            checked += 1
        else:
            raise AssertionError('unqualified reference refusal accepted: ' + line)

    for code in vim_protocol.REFUSALS:
        require(vim_protocol.classify_exception('Vim(let):E%d: measured syntax' % code) == 'refused', code)
        checked += 1
    for exception in ('Vim(let):E363: memory', 'Vim(let):E342: Out of memory',
                      'Vim(let):E9999: unknown', 'E54: incomplete exception', '', None):
        require(vim_protocol.classify_exception(exception) == 'error', exception)
        checked += 1

    # Reporting cannot certify an incomplete callback that returned success,
    # or a fatal return code accompanied by a complete-looking report.
    for complete, code, wanted, final_complete in ((False, 0, 2, False), (False, 1, 2, False),
            (True, 2, 2, False), (True, 1, 1, True)):
        report = vim_diff.failure_report('control')
        def report_run():
            report['complete'] = complete
            return code
        require(vim_diff.run_reporting(None, report, report_run) == wanted, report)
        require(report['complete'] == final_complete, report)
        checked += 1

    report = vim_diff.failure_report('callback-exception')
    def fail_after_complete():
        report['complete'] = True
        raise RuntimeError('failure after completion assignment')
    try:
        vim_diff.run_reporting(None, report, fail_after_complete)
    except RuntimeError:
        require(not report['complete'] and report['error'], report)
    else:
        raise AssertionError('callback exception accepted')
    checked += 1

    with tempfile.TemporaryDirectory() as work:
        for code, stderr in ((5, b''), (0, b'\xffencoding diagnostic')):
            def encoding_command(extra, scratch):
                open(os.path.join(scratch, 'encoding.txt'), 'w', encoding='utf-8').write('utf-8\n')
                return ['encoding-control']
            finished = subprocess.CompletedProcess(['encoding-control'], code, b'', stderr)
            with patch.object(vim_diff.vim_runner, 'command', side_effect=encoding_command), \
                    patch.object(vim_diff.vim_runner.subprocess, 'run', return_value=finished):
                try:
                    vim_diff.vim_runner.encoding(work)
                except vim_protocol.Failure as error:
                    require(error.evidence['stderr_hex'] == stderr.hex(), error.evidence)
                    require(error.evidence['raw_answers'] == 'utf-8\n', error.evidence)
                else:
                    raise AssertionError('failed encoding probe accepted')
            checked += 1

    with tempfile.TemporaryDirectory() as work:
        raw = b'utf-8\n\xff'
        def timeout_encoding(command, **kwargs):
            Path(work, 'encoding.txt').write_bytes(raw)
            raise subprocess.TimeoutExpired(command, 120, output=b'partial', stderr=b'\xfe')
        with patch.object(vim_diff.vim_runner, 'command', return_value=['encoding-control']), \
                patch.object(vim_diff.vim_runner.subprocess, 'run', side_effect=timeout_encoding):
            try:
                vim_diff.vim_runner.encoding(work)
            except vim_protocol.Failure as error:
                require(error.evidence['raw_answers_hex'] == raw.hex(), error.evidence)
                require(error.evidence['stdout_hex'] == b'partial'.hex() and
                        error.evidence['stderr_hex'] == b'\xfe'.hex(), error.evidence)
            else:
                raise AssertionError('write-then-timeout accepted')
        checked += 1

    valid_version = b'VIM - Vi IMproved 9.2 (2026 Feb 14, compiled Sep 25 2026)\nIncluded patches: 1-1129\n'
    for raw in (b'', b'arbitrary metadata', b'\xff', valid_version + b'\xff', valid_version):
        finished = subprocess.CompletedProcess(['version-control'], 0, raw, b'')
        with patch.object(vim_diff.vim_runner.oracle_env, 'command', return_value=['version-control']), \
                patch.object(vim_diff.vim_runner.subprocess, 'run', return_value=finished):
            try:
                version = vim_diff.vim_runner.version()
            except vim_protocol.Failure as error:
                require(raw != valid_version and error.evidence['stdout_hex'] == raw.hex(), error.evidence)
            else:
                require(raw == valid_version and 'Included patches: 1-1129' in version, version)
        checked += 1

    # Invalid UTF-8 process channels invalidate execution and survive exactly.
    with tempfile.TemporaryDirectory() as work:
        out = os.path.join(work, 'answers')
        open(out, 'w', encoding='utf-8').write('[]\n')
        for stdout, stderr in ((b'\xffstdout', b''), (b'', b'\xffstderr')):
            finished = subprocess.CompletedProcess(['control'], 0, stdout, stderr)
            with patch.object(vim_protocol.subprocess, 'run', return_value=finished):
                try:
                    vim_protocol.run(['control'], out, [['a', 'a']])
                except vim_protocol.Failure as error:
                    require(error.evidence['stdout_hex'] == stdout.hex(), error.evidence)
                    require(error.evidence['stderr_hex'] == stderr.hex(), error.evidence)
                else:
                    raise AssertionError('invalid UTF-8 diagnostic accepted')
            checked += 1

    # Timeout retains diagnostics and any file output produced before the kill.
    with tempfile.TemporaryDirectory() as work:
        out = os.path.join(work, 'answers')
        with open(out, 'w', encoding='utf-8') as output:
            output.write('partial raw evidence\n')
        timeout = subprocess.TimeoutExpired(['control'], 1, output=b'partial stdout\xff', stderr=b'partial stderr\xfe')
        with patch.object(vim_protocol.subprocess, 'run', side_effect=timeout):
            try:
                vim_protocol.run(['control'], out, [['a', 'a']])
            except vim_protocol.Failure as error:
                require(error.evidence['raw_answers'] == 'partial raw evidence\n', error.evidence)
                require(error.evidence['stdout_hex'] == b'partial stdout\xff'.hex() and error.evidence['stderr_hex'] == b'partial stderr\xfe'.hex(), error.evidence)
            else:
                raise AssertionError('timeout accepted')
        checked += 1
    return checked


if __name__ == '__main__':
    print('%d Vim protocol controls passed' % check())
