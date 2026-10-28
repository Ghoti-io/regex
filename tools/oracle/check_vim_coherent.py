#!/usr/bin/env python3
"""Prove matching references use coherent fresh results, including history traps.

Requires the pinned Vim reference. These are reference controls, independent of
any regex-library result. The deliberate extra-call mutant must be detected.

Copyright 2026 by Corey Pennycuff
"""
import argparse
import gzip
import json
from pathlib import Path
import vim_diff


CASES = [
    (r'^\(\1a\)*b[@<=]\{0}$', 'aab'),
    (r'^\%(\(a\1\)\)\{2}aab[@<=]\{0}$', 'aaaab'),
    (r'\(é\)', 'é'),
    ('^b', 'a'),
    (r'\(', 'a'),
]
EXPECTED = ['match 0:3 "a"', 'match 0:5 "a"', 'match 0:2 "\\u00e9"',
            'nomatch', 'compile']
for mode, group in (('', r'\(a\)'), (r'\v', '(a)')):
    for count in (9, 10, 11):
        CASES.append((mode + group * count, 'a' * count))
        EXPECTED.append('match 0:9 ' + ' '.join(['"a"'] * 9)
                        if count == 9 else 'compile')
CASES.append((r'\_%', 'a'))
EXPECTED.append('compile')


def check():
    evidence = dict(complete=False, normative={}, mutation={})
    for syntax, engine in vim_diff.PROFILES.items():
        raw = vim_diff.ask_vim(CASES, engine)
        evidence['normative'][syntax] = raw.evidence
        actual = [vim_diff.normalise_theirs(a, subject)
                  for a, (_, subject) in zip(raw, CASES, strict=True)]
        if actual != EXPECTED:
            raise AssertionError((syntax, EXPECTED, actual))
    # The first NFA query changes the next operation's state even when all
    # cases already have separate processes. These witnesses distinguish that
    # corruption from a syntax or library disagreement.
    script = vim_diff.VIM_GROUP_SCRIPT.replace('  try\n',
        '  try\n    let ignored = matchstrpos(c[1], c[0])\n', 1)
    raw = vim_diff._ask(CASES[:2], 'set regexpengine=2\n' + script)
    evidence['mutation']['extra-prior-call'] = raw.evidence
    actual = [vim_diff.normalise_theirs(a, subject)
              for a, (_, subject) in zip(raw, CASES[:2], strict=True)]
    wanted_groups = ['match 0:0 \"a\"', 'match 0:0 \"a\"']
    if any(observed == wanted for observed, wanted in zip(actual, wanted_groups)):
        raise AssertionError(('history traps failed to distinguish extra query', actual))
    evidence['complete'] = True
    return evidence


def join_controls():
    """Hermetic controls for separately validated span/group composition."""
    from unittest.mock import patch
    import vim_protocol
    pairs = [('a', 'a'), ('b', 'a'), ('(', 'a')]
    span_line = 'match ' + json.dumps(['0:1'] + [''] * 9)
    group_line = 'match ' + json.dumps(['0:0', 'a'] + [''] * 8)
    checked = 0
    for fault in ('valid', 'short-span', 'invalid-span', 'incomplete-span',
                  'short-group', 'group-nomatch', 'group-refused',
                  'missing-groups', 'incomplete-group', 'group-failure'):
        spans = [span_line, 'nomatch', 'compile']
        groups = [group_line]
        if fault == 'short-span':
            spans.pop()
        if fault == 'invalid-span':
            spans[0] = 'error'
        if fault == 'short-group':
            groups = []
        if fault == 'group-nomatch':
            groups = ['nomatch']
        if fault == 'group-refused':
            groups = ['compile']
        if fault == 'missing-groups':
            groups = ['match ["0:0"]']
        replies = [vim_protocol.Answers(spans, dict(complete=fault != 'incomplete-span')),
                   vim_protocol.Answers(groups, dict(complete=fault != 'incomplete-group'))]
        if fault == 'group-failure':
            replies[1] = vim_protocol.Failure('group failed', dict(complete=False, raw='retained'))
        with patch.object(vim_diff, '_ask', side_effect=replies) as query:
            try:
                result = vim_diff.ask_vim(pairs, engine=2)
            except vim_protocol.Failure as error:
                if fault == 'valid' or error.evidence['complete'] is not False:
                    raise AssertionError((fault, error.evidence))
                if error.evidence['inputs'] != [['a', 'a'], ['b', 'a'], ['(', 'a']]:
                    raise AssertionError('failed join lost exact input population')
                if fault == 'group-failure' and error.evidence['failed_operation']['raw'] != 'retained':
                    raise AssertionError('failed join lost raw operation failure')
            else:
                if fault != 'valid' or result != [
                        'match ' + json.dumps(['0:1', 'a'] + [''] * 8), 'nomatch', 'compile']:
                    raise AssertionError((fault, result))
                if query.call_args_list[0].args != (pairs, 'set regexpengine=2\n' + vim_diff.VIM_SPAN_SCRIPT):
                    raise AssertionError('span operation changed')
                if query.call_args_list[1].args != ([pairs[0]], 'set regexpengine=2\n' + vim_diff.VIM_GROUP_SCRIPT):
                    raise AssertionError('group operation lost selected input identity')
                if result.evidence['aggregation'] != [
                        dict(index=0, input=['a', 'a'], span_process=0, group_process=0),
                        dict(index=1, input=['b', 'a'], span_process=1, group_process=None),
                        dict(index=2, input=['(', 'a'], span_process=2, group_process=None)]:
                    raise AssertionError('operation aggregation changed')
        checked += 1
    return checked


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--evidence', type=Path)
    args = parser.parse_args()
    evidence = check()
    if args.evidence:
        args.evidence.write_bytes(gzip.compress(json.dumps(evidence,
            ensure_ascii=False, separators=(',', ':')).encode(), mtime=0))
    print(len(CASES) * len(vim_diff.PROFILES) + 2,
          'coherent Vim reference controls passed, including two detected history mutations')


if __name__ == '__main__':
    main()
