#!/usr/bin/env python3
"""Keep compile-resource failures distinct from refusals in both real drivers."""
import argparse
from pathlib import Path
import re

import replace_diff
import vim_diff
import vim_protocol


def check(match_driver, replace_driver):
    checked = 0
    faults = [
        ('syntax', None, None),
        ('unavailable', None, None),
        ('oom', 'error Out of memory', 'error 6'),
        ('limit', 'error Limit exceeded', 'error 4'),
        ('unsupported', 'error Unsupported feature', 'error 3'),
        ('unbuilt_construct', 'error Unsupported feature', 'error 3'),
    ]
    # Healthy compiles before and after each failure exercise cache recovery.
    # Consecutive healthy rows also exercise the match driver's cache hit.
    rows = [('a', 'a', 'X', 'match')]
    for name, _, _ in faults:
        rows.extend([('__grx_test_compile_' + name + '__', 'a', 'X', name),
                     ('a', 'a', 'X', 'match'),
                     ('a', 'a', 'X', 'match')])
    rows.extend([(r'\(a\)', 'a', r'<\1>', 'capture'),
                 ('a', 'b', 'X', 'nomatch'),
                 (r'\(', 'a', 'X', 'syntax'),
                 ('~', '~', 'X', 'unavailable'),
                 (r'\~', '~', 'X', 'match')])
    errors = {name: (match, replace) for name, match, replace in faults if match}
    for profile in vim_diff.PROFILES:
        for replacement, driver in [(False, match_driver), (True, replace_driver)]:
            payload = ''.join('\t' + '\t'.join(value.encode().hex() for value in
                ((pattern, subject, template) if replacement else (pattern, subject))) + '\n'
                for pattern, subject, template, _ in rows)
            finished = vim_protocol.run_driver([str(driver), profile], payload)
            lines = finished.stdout.splitlines()
            if len(lines) != len(rows):
                raise AssertionError('compile-result fixture returned a short population')
            for row, line in zip(rows, lines):
                pattern, subject, _, wanted = row
                detail = (profile, replacement, pattern, line)
                if wanted in ('syntax', 'unavailable'):
                    prefix = 'compile' if wanted == 'syntax' else 'unavailable'
                    if re.fullmatch(prefix + r' [0-9]+', line) is None:
                        raise AssertionError(detail)
                    if replacement:
                        if replace_diff.parse_vim_reply(line) != dict(status='refused'):
                            raise AssertionError(detail)
                    elif vim_diff.normalise_ours(line, subject) != 'compile':
                        raise AssertionError(detail)
                elif wanted in errors:
                    if line != errors[wanted][int(replacement)]:
                        raise AssertionError(detail)
                    if replacement:
                        if replace_diff.parse_vim_reply(line)['status'] != 'error':
                            raise AssertionError(detail)
                    else:
                        try:
                            vim_diff.normalise_ours(line, subject)
                        except ValueError:
                            pass
                        else:
                            raise AssertionError('match error normalized as a verdict: %r' % (detail,))
                elif replacement:
                    text = dict(match='X', capture='<a>', nomatch='b')[wanted]
                    if replace_diff.parse_vim_reply(line) != dict(status='ok', text=text):
                        raise AssertionError(detail)
                else:
                    expected = dict(match='match 0:1 ', capture='match 0:1 "a"', nomatch='nomatch')[wanted]
                    if vim_diff.normalise_ours(line, subject) != expected:
                        raise AssertionError(detail)
                checked += 1
    return checked


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--match-driver', type=Path, required=True)
    parser.add_argument('--replace-driver', type=Path, required=True)
    args = parser.parse_args()
    count = check(args.match_driver.resolve(), args.replace_driver.resolve())
    print('oracle compile results: %d checks passed' % count)


if __name__ == '__main__':
    main()
