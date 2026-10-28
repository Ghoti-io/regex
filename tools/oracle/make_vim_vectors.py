#!/usr/bin/env python3
r"""Generate the Vim dialect's `.rxt` conformance vectors from vim itself.

`vim_diff.py` is the differential: it needs the pinned vim image and runs
fresh every time. This is the other half - a checked-in corpus that runs in
`make test` on a machine with no containers, and that turns a rule confirmed
once into a regression test forever. Of the ten dialects that compile and
match, Vim and Python were the two with nothing at all committed.

**vim states group text, not group offsets, and it has no API that does.**
`matchstrpos()` gives the span of the whole match; `matchlist()` gives the
submatches as strings; `matchstrlist()`'s `submatches` are strings too. So a
vector generated from vim can pin the whole match exactly and each group only
by its contents, which is what the `groups:` field is for - see
`tests/conformance/rxt.h`. Taking the group spans from *this library* instead
would write today's behaviour down as the rule, which is the one thing a
vector must never do. `-` in that field is a group that did not participate
**or** matched empty, because vim answers `''` for both and cannot be asked
which.

Every generated postfix assertion and atomic group is retained. Expectations
are obtained only from the corresponding forced reference engine, including
compile refusals. Literal `@` characters are retained too. Generated syntax
shapes are never omitted: each input receives its forced engine's answer.

Each named profile is answered by its corresponding forced Vim engine.
Engine disagreements are retained as independent expectations in two corpora.

Usage:
    tools/oracle/make_vim_vectors.py [--out VECTOR_ROOT] [--seed N] [--patterns N]

`--out` names the parent of the profile directories: output is written under
`VECTOR_ROOT/vim-backtrack/` and `VECTOR_ROOT/vim-nfa/` (or only the selected
`--syntax` profile). The default parent is `tests/data/vectors/`.

Copyright 2026 by Corey Pennycuff
"""

import argparse
import json
import os
import random
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

sys.path.insert(0, HERE)

import make_vectors
import vim_diff
import vim_runner

# Rules from documentation/dialects.md that a random corpus reaches only by
# accident. `(level, pattern, [subjects])`, the level written as the marker
# that selects it so that a reader sees the pattern vim was given.
#
# Named cases supplement generated postfix coverage with independent examples.
NAMED_CASES = [
    ("", "\\(a\\)\\@!\\|a\\%[\\_s]", ["a\nb"]),
    ("", "[^ab]\\(\\w\\+\\)\\@2<=\\(a\\zsb\\)\\@>", ["abcabc"]),
    ("", "\\(é\\)\\@1<=x", ["éx"]),
    ("", "\\(a\\)\\@!\\|a\\1", ["aa"]),
    ("", "[^ab]\\(\\w\\+\\)\\@<=", ["abcabc"]),
    # The four magic levels, section 5.16: the same text means different
    # things at each, which is the construct this dialect is about.
    ("", "a\\+", ["aaa", "a+"]),
    ("\\v", "a+", ["aaa", "a+"]),
    ("\\M", "a\\+", ["aaa", "a+"]),
    ("\\V", "a\\+", ["aaa", "a+"]),
    ("", "a*", ["aaa", "a*"]),
    ("\\V", "a*", ["aaa", "a*"]),
    ("\\v", "(a|b)+", ["ab", "ba"]),
    ("", "\\(a\\|b\\)\\+", ["ab", "ba"]),
    # A level change part-way through, which moves the line between operator
    # and literal for everything after it.
    ("", "a\\+\\Vb+", ["aab+", "aabb"]),
    ("\\v", "a+\\Mb\\+", ["aabb", "aab+"]),
    # `\zs` and `\ze` set where the reported match begins and ends. The last
    # write wins, which is the rule both of vim's engines follow for plain
    # marks.
    ("", "a\\zsb", ["ab"]),
    ("", "a\\zeb", ["ab"]),
    ("", "a\\zeb\\zec", ["abc"]),
    ("", "a\\zsb\\zsc", ["abc"]),
    # The nine character classes behind `\<`, `\>` and `\k`, section 6. `\>`
    # holds between two keyword characters in different classes, which is
    # what no word set could see.
    ("", "\\<x", ["\u65e5x", "ax", "x"]),
    ("", "x\\>", ["x\u65e5", "xa", "x"]),
    ("", "\\k\\+", ["a\u00d7b", "\u00d7", "a_0"]),
    ("", "\\<\\k\\+\\>", ["\u65e5\u65e5", "a\u3042"]),
    # `[[:lower:]]` and `[[:upper:]]` ask vim for a case *counterpart*, not
    # for a Unicode property: these six code points are where the two
    # readings part company.
    #
    ("", "[[:lower:]]", ["\u02b0", "\u2170", "\u01c5", "\u00df", "\u0149"]),
    ("", "[[:upper:]]", ["\u24b6", "\u01c5", "\u0149", "A"]),
    # `\c` and `\C` are pattern syntax rather than flags, and `\c` folds
    # beyond ASCII.
    ("", "\\c\u00c9", ["\u00e9", "\u00c9"]),
    ("", "\\C\u00c9", ["\u00e9", "\u00c9"]),
    ("", "\\ca", ["A"]),
    ("", "a\\Cb", ["aB", "ab"]),
    # `\_.`, `\_s` and friends add the line break to a class; `\n` is a
    # character in a string subject.
    ("", "a\\_.b", ["a\nb", "ab"]),
    ("", "\\_^b", ["a\nb"]),
    ("", "a\\_$", ["a\nb"]),
    # `\%23l`, `\%23c` and `\%23v`: a line number never matches over a
    # string, a column counts bytes and a virtual column counts cells.
    ("", "\\%2cb", ["ab"]),
    ("", "\\%2vx", ["\t\tx", "ax"]),
    ("", "\\%1v\\%2vx", ["ax"]),
    # `\%[...]` is an optional sequence, and it is vim's alone.
    ("", "r\\%[ead]", ["read", "rea", "r"]),
    ("", "a\\%[bc]d", ["abcd", "ad"]),
    # `\Z` ignores composing characters; without it a cluster is one
    # character, which is section 6's model.
    ("", "a", ["\u00e1b"]),
    ("", "\\Za", ["\u00e1b"]),
    ("", ".", ["\u00e1b"]),
    ("", "\\Z.", ["\u00e1b"]),
    # A `*` with nothing to repeat is a literal asterisk, which is a POSIX
    # basic RE's rule and vim's - at every level, and in the spellings vim
    # does accept.
    ("", "*a", ["*a"]),
    ("", "\\(*\\)", ["*"]),
    ("\\v", "%(*)", ["*"]),
    ("\\M", "\\%(*\\)", ["*"]),
    # Backreferences, and `~`, which is E33 in the only state a library ever
    # has and is refused here as well.
    ("", "\\(a\\)\\1", ["aa", "ab"]),
    ("", "\\(a\\|b\\)\\1", ["aa", "ab", "bb"]),
    ("", "\\~", ["~"]),
    # Paired grammar controls: each profile retains its own compile verdict.
    ("", "a\\%[\\(b\\)c]", ["a", "ab", "abc"]),
    ("", "a\\%[b\\%[cd]]", ["a", "ab", "abc", "abcd"]),
    ("", "a\\%[\\(b\\|c\\)d]", ["ab", "ac", "acd"]),
    ("", "a\\%[\\(b\\c\\)d]", ["aB", "aBd"]),
    ("", "a\\%[b\\cd]", ["aB", "abc", "abcd"]),
    ("", "\\9[@<=]", ["", "@", "<", "="]),
    ("", "\\9[@1<=]", ["@"]),
    ("", "[@<=]\\9", ["@"]),
    ("", "\\1\\(a\\)\\@<=", ["", "a", "aa"]),
    ("", "\\(a\\1\\)\\@<=", ["", "a", "aa"]),
    ("", "[[:foo:]]", ["[[:foo:]]", ":]", "f]", "[]", "a"]),
    ("", "[a[:foo:]]*a", ["a", "aa", "a]a"]),
    ("", "\\_^*", ["", "*"]),
    ("\\v", "\\_^*", ["", "*"]),
    ("", "\\%23l*", ["", "a"]),
    ("", "\\%23l\\m*", ["", "a"]),
    ("", "a\\%23l*", ["", "a", "ab"]),
    ("", "a\\|\\%23l*", ["", "b"]),
    ("", "a\\%23l**", ["a"]),
    ("\\v", "%23l*", ["", "a"]),
    ("\\v", "a%23l*", ["a", "ab"]),
    ("\\v", "[ab]%23l*", ["a", "b"]),
    ("\\v", ".%23l*", ["a", "b"]),
    ("\\v", "x.%23l*", ["xa", "xb"]),
    ("\\v", "\\w%23l*", ["a", "b"]),
    ("\\v", "(a%23l*)", ["a"]),
    ("\\v", "a\\c%23l*", ["a"]),
    ("\\v", "a%23l{0,}", ["a"]),
    # Physical EOF permits a partial literal candidate, but not a missing
    # composing mark or a conflicting scalar that is present in the subject.
    ("", "\\ć\\?bb", ["áb", "ábcd", "ábbcd"]),
    ("", "\\ć\\?é̂b", ["áé", "áé̂", "áé̂b"]),
    ("", "\\c\\Ź\\?é̂b", ["áé", "áé̂", "áé̂b"]),
    ("", "\\_%", ["a"]),
    ("", "^\\m*", ["", "*"]),
    ("", "\\%(*\\)", ["", "*"]),
    # Span and capture operations must each be fresh. Calling a second API
    # in the same NFA process changes these results.
    ("", "^\\(\\1a\\)*b[@<=]\\{0}$", ["aab"]),
    ("", "^\\%(\\(a\\1\\)\\)\\{2}aab[@<=]\\{0}$", ["aaaab"]),

]

def record_for(pattern, subject, answer):
    """One `.rxt` record from one complete Vim answer; fail on unknown output."""
    if not isinstance(answer, str):
        raise ValueError("Vim answer must be text")
    lines = ["pattern: " + make_vectors.escape(pattern)]

    if answer == "compile":
        # vim throws, and a caught exception says only that it refused. The
        # code this library answers with is swept per construct by
        # `Vim.EveryRefusalIsASyntaxRefusal`; see rxt.h.
        lines.append("expect: refused")
        return "\n".join(lines) + "\n"
    if not answer.startswith("match ") and answer != "nomatch":
        raise ValueError("unrecordable Vim answer: %r" % answer)

    lines.append("subject:" + (" " + make_vectors.escape(subject) if subject else ""))
    if answer == "nomatch":
        lines.append("expect: nomatch")
        return "\n".join(lines) + "\n"

    try:
        fields = json.loads(answer[len("match "):])
    except (ValueError, TypeError) as error:
        raise ValueError("malformed Vim match answer") from error
    if (not isinstance(fields, list) or not 1 <= len(fields) <= 10
            or any(not isinstance(field, str) for field in fields)
            or re.fullmatch(r"[0-9]+:[0-9]+", fields[0]) is None):
        raise ValueError("malformed Vim match fields: %r" % fields)
    try:
        for capture in fields[1:]:
            capture.encode("utf-8")
    except UnicodeEncodeError as error:
        raise ValueError("Vim capture is not valid UTF-8 text") from error
    start, end = map(int, fields[0].split(":"))
    raw = subject.encode("utf-8")
    if not 0 <= start <= end <= len(raw):
        raise ValueError("Vim match span outside subject: %r" % fields[0])
    # Byte offsets must also lie at complete UTF-8 boundaries.
    try:
        raw[:start].decode("utf-8")
        raw[:end].decode("utf-8")
    except UnicodeDecodeError as error:
        raise ValueError("Vim match span splits a UTF-8 character") from error
    lines.append("expect: %s-%s" % (start, end))

    # Vim cannot distinguish an unset group from an empty capture. Explicit
    # TEXT mode is required even when every reported group is empty: omitting
    # `groups:` would instead require exact unset capture spans.
    groups = list(fields[1:]) or [""]
    lines.append("groups: "
        + " ".join(make_vectors.escape_group(g) for g in groups))
    return "\n".join(lines) + "\n"


def write_file(path, rows, answers, version, dropped, syntax):
    if len(rows) != len(answers):
        raise ValueError("vector answer population is incomplete")
    body = []
    refused = set()
    duplicate_refusals = 0
    for (pattern, subject), answer in zip(rows, answers):
        if answer == "compile":
            # A refusal has no subject in it, so the other subjects of one
            # refused pattern would be exact duplicates - and a duplicate
            # counts one fact many times in the rate the suite publishes.
            if pattern in refused:
                duplicate_refusals += 1
                continue
            refused.add(pattern)
        record = record_for(pattern, subject, answer)
        if record:
            body.append(record)

    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write("# generated by tools/oracle/make_vim_vectors.py\n")
        out.write("# oracle: %s\n" % version)
        out.write("# The expectations are the oracle's, not this library's: "
                  "a vector\n")
        out.write("# generated from what this library does would record a bug "
                  "rather than\n# a rule.\n")
        out.write("#\n")
        out.write("# `groups:` states each group's TEXT: vim has no API that "
                  "gives a submatch\n")
        out.write("# a position, so the whole match is exact here and a group "
                  "is pinned by its\n")
        out.write("# contents. `-` is a group that did not participate or "
                  "matched empty, which\n")
        out.write("# vim spells the same way.\n")
        out.write("#\n")
        for reason, count in sorted(dropped.items()):
            out.write("# not written: %d %s\n" % (count, reason))
        out.write("# population: %d reference answers, %d records, "
            "%d duplicate refusal rows\n"
            % (len(answers), len(body), duplicate_refusals))
        out.write("dialect: %s\n\n" % syntax)
        out.write("\n".join(body))

    sys.stderr.write("%s: %d/%d reference answers, %d records, "
        "%d duplicate refusal rows\n"
        % (path, len(answers), len(rows), len(body), duplicate_refusals))
    return len(body)


# Exact input spellings supplement the generated combining grammar. Answers
# are acquired below from each forced reference, never from the unit fixture.
NAMED_CASES.extend([
    ("", "\\(a\\)\\@>\\1", ["a\u0301a\u0301"]),
    ("", "\\(a\\)\\(\\1\\)\\@>x", ["aa\u0301x"]),
    ("", "\\(a\\)\\@>a\\1", ["aaa\u0301"]),
    ("", "\\(a\\)\\@=a\\(\\1\\)\\@=.", ["a\u0301a\u0301"]),
    ("", "\\(a\\)\\@=a\\(\\1\\)\\@>x", ["aa\u0301x"]),
    ("", "\\(a\\)\\@>\\(\\1\\)\\@<=x", ["a\u0301x"]),
    ("", "\\(a\\)\\@>.\\1", ["aaa\u0301"]),
    ("", "\\(.\\+\\)\\@1<=x", ["a\u0301x"]),
    ("", "\\(.\\+\\)\\@<=x", ["ba\u0301x"]),
    ("", "\\_s", ["\n\u0301b"]),
    ("", "\\(\\_s\\)b", ["\n\u0301b"]),
    ("", "\\_s\\zs$", ["\n\u0301"]),
    ("", "\\_[ab]b", ["\n\u0301b"]),
    ("", "\\_[^x]b", ["\n\u0301b"]),
    ("", "[a\u0301]", ["a\u0302\u0301"]),
    ("", "[a\u0301\u0301]", ["a\u0301"]),
    ("", "[ab\u0301]", ["b"]),
    ("", "\u0301b", ["a\u0301x"]),
    ("", "\u0301\\?b", ["a\u0301xb"]),
    ("", "\u0301\\?bb", ["a\u0301bxb"]),
    ("", "\u0301\\?[b]", ["a\u0301x"]),
    ("", "\\Za\u0301$", ["a\u0301\u0302"]),
    ("", "\\Za\u0301b", ["a\u0301\u0302b"]),
    ("", "\\Z\\(a\u0301\\)b", ["a\u0301\u0302b"]),
    ("", "\\%u0301b", ["a\u0301x"]),
    ("", "\\Za\\%u0301\u0302", ["a\u0301"]),
    ("", "\\w.", ["a\u0301b"]),
    ("", "\\w*.", ["a\u0301b"]),
    ("", "\\W*.", ["a\u0301b"]),
    ("", "\\W\\?.", ["a\u0301b"]),
    ("", "\\(\\W\\{1}\\).", ["a\u0301\u0302b"]),
    ("", "\\(\\W\\)\\{1}.", ["a\u0301\u0302b"]),
    ("", "\\%(a\\|[ab]\\)", ["a\u0301b"]),
    ("", "^\\%(a\\|[ab]\\)", ["a\u0301"]),
    ("", "\\%9ca\\|[ab]", ["a\u0301b"]),
    ("", "a\\%9c\\|[ab]", ["a\u0301b"]),
    ("", "^\\%([ab].\\|a\\nX\\)", ["a\n\u0301b"]),
    ("", "^\\%([ab]..\\|a\\nX\\)", ["a\n\u0301b"]),
    ("", "\\_[ab].", ["a\n\u0301b"]),
    ("", "^\\%(\\_[ab]\\).", ["a\n\u0301b"]),
    ("", "\\(\\_[^x]\\).", ["\n\u0301b"]),
    ("", "\\_[^x]\\zs.", ["\n\u0301b"]),
    ("", "\\_[^\\n]\\zs.", ["\n\u0301b"]),
    ("", "b.\u0301", ["ba\u0301x"]),
    ("", "b.\u0301", ["b.\u0301"]),
    ("", "b.\u0301\\?", ["b..\u0301"]),
    ("", "b\\.\u0301", ["b.\u0301"]),
    ("", "b\\%u002e\u0301", ["b..\u0301"]),
    ("", "\\(a\\)\\@>x", ["a\u0301x"]),
    ("", "\\(a\\)\\@>\\zsx", ["a\u0301x"]),
    ("", "\\(a\\).", ["a\u0301x"]),
    ("", "\\([ab].\\|abX\\)\\@=.", ["a\u0301"]),
    ("", "^\\([ab]\\)\\(\\%(\\1aX\\|[ab].x\\)\\)\\@=.", ["aaa\u0301x"]),
    ("", "^\\([ab]\\)\\(\\%(\\1aX\\|[ab].x\\)\\)\\@=.", ["baa\u0301x"]),
    ("", "[\\n].", ["\n\u0301b"]),
    ("", "[\\n]*.", ["\n\u0301b"]),
    ("", "^\\(a\\)\\?a\\?b\\(\\%(\\1aX\\|[ab].x\\)\\)\\@>", ["abaa\u0301x"]),
    ("", "^\\(a\\)\\?a\\?b\\(\\%(\\1aX\\|[ab].x\\)\\)\\@>", ["abaa\u0301\u0301x"]),
    ("", "^\\(a\\)\\?a\\?b\\(\\%([ab].x\\|\\1aX\\)\\)\\@>", ["abaa\u0301x"]),
    ("", "^\\(a\\)\\?a\\?b\\(\\%([ab].x\\|\\1aX\\)\\)\\@>", ["abaa\u0301\u0301x"]),
    ("", "^\\(a\\)\\?a*b\\(\\%(\\1aX\\|[ab].x\\)\\)\\@>", ["abaa\u0301x"]),
    ("", "^\\(a\\)\\?a*b\\(\\%(\\1aX\\|[ab].x\\)\\)\\@>", ["abaa\u0301\u0301x"]),
    ("", "^\\(a\\)\\?a*b\\(\\%([ab].x\\|\\1aX\\)\\)\\@>", ["abaa\u0301x"]),
    ("", "^\\(a\\)\\?a*b\\(\\%([ab].x\\|\\1aX\\)\\)\\@>", ["abaa\u0301\u0301x"]),
    ("", "\\(a*.\\)", ["a\u0301x"]),
    ("", "\\(a*.\\)\\@=.", ["a\u0301x"]),
    ("", "\\(a*.\\)\\@>.", ["a\u0301x"]),
    ("", "\\(a*.\\)\\@<=x", ["a\u0301x"]),
    ("", "\\%(\\(a*.\\)\\@=.\\)\\@=.", ["a\u0301x"]),
    ("", "\\(a\\?.\\)", ["a\u0301x"]),
    ("", "\\(a\\?.\\)\\@=.", ["a\u0301x"]),
    ("", "\\(a\\?.\\)\\@>.", ["a\u0301x"]),
    ("", "\\(a\\?.\\)\\@<=x", ["a\u0301x"]),
    ("", "\\(a\\?.\\)\\@<=x", ["aa\u0301x"]),
    ("", "\\%(\\(a\\?.\\)\\@=.\\)\\@=.", ["a\u0301x"]),
    ("", "\\(\\(a\\zsb\\)\\@>\\)\\@>", ["ab"]),
    ("", "\\(\\(a\\zsb\\)\\@>\\)\\@>", ["xab"]),
    ("", "\\(\\(a\\zsb\\)\\@>\\)\\@>", ["abab"]),
    ("", "\\(\\(a\\zeb\\)\\@>\\)\\@>", ["ab"]),
    ("", "\\(\\(a\\zeb\\)\\@>\\)\\@>", ["xab"]),
    ("", "\\(\\(a\\zeb\\)\\@>\\)\\@>", ["abab"]),
    ('', '\\(x\\)\\%(\\(\\(ab\\)\\@>\\)\\@>\\)\\@=.', ['xab']),
    ('', '\\(x\\)\\%(\\(\\(ab\\)\\@>\\)\\@>\\)\\@=.', ['xab́']),
    ('', '\\(x\\)\\%(\\(\\(\\(ab\\)\\@>\\)\\@>\\)\\@>\\)\\@=.', ['xab']),
    ('', '\\(\\%(a\\)\\)\\@=\\(a\\)\\@=a', ['aaab']),
    ('', '\\(\\%(a\\)\\)\\@=\\(a\\)\\@=a', ['áab']),
    ('', '\\(a\\+\\)\\@=\\(a\\)\\@=a', ['áaaa']),
    ('', '\\(\\%(a\\|b\\)\\)\\@=\\(a\\)\\@=a', ['b́a']),
    ('', '\\(a\\)\\@=\\1', ['á']),
    ('', '\\(a\\)\\@=.\\1', ['áá']),
    ('', '\\(a\\)\\@=a\\1', ['aáa']),
    ('', '\\(a\\)\\@=\\(a\\)\\@=\\1', ['á']),
    ('', '\\(\\)\\@=\\1', ['́']),
    ('', '\\(a\\?\\)\\@=\\1.', ['á']),
    ('', '\\(a\\)\\@=\\(aX\\|\\1\\)\\@=.', ['á']),
    ('', '\\(a\\)\\@=\\(\\1\\|aX\\)\\@=.', ['á']),
    ('', '\\(a\\)\\@=\\(aX\\|\\1\\)\\@>x', ['áx']),
    ('', '\\(a\\)\\@=\\(\\1\\|aX\\)\\@>x', ['áx']),
    ('', '\\(a\\)\\@=\\(a\\%9cX\\|\\1\\)\\@=.', ['á']),
    ('', '\\(a\\)\\@=\\(\\%9caX\\|\\1\\)\\@=.', ['á']),
])


# `\z\s` reads as `\zs`: found by a seed no earlier run had used, and the
# whole `\z\X` family swept on vim 9.2 to show it is the only spelling.
NAMED_CASES.extend([
    ("", "a\\z\\s", ["xab", "a", "aa"]),
    ("\\v", "a\\z\\s", ["xab"]),
    ("\\M", "a\\z\\s", ["xab"]),
    ("\\V", "a\\z\\s", ["xab"]),
    ("", "\\z\\s", ["a", ""]),
    ("", "\\V\\%$\\M\\(a\\)\\@<=\\z\\s", ["a", "ab"]),
    ("", "\\z\\S", ["a"]),
    ("", "\\z\\_s", ["a"]),
    ("", "\\z\\s\\+", ["a b"]),
])


# The `\%` family, the trailing backslash and `\%[*]`, found by sweeping every
# printable character after each escape prefix in all five magic levels against
# both engines. The mark atoms (`\%'m`, `\%.l`) need a buffer and are not rows.
NAMED_CASES.extend([
    ("", "a\\", ["a\\", "ab", ""]),
    ("", "\\", ["a\\b", "ab"]),
    ("\\v", "\\", ["a\\b"]),
    ("\\M", "\\", ["a\\b"]),
    ("\\V", "\\", ["a\\b"]),
    ("", "\\(a\\)\\", ["a\\", "a"]),
    ("", "a\\|\\", ["x\\", "a"]),
    ("", "a*\\", ["aa\\", "a"]),
    ("", "\\%\\(a\\)", ["xab 1", "a"]),
    ("", "\\%\\1l", ["a"]),
    ("", "\\%[*]", ["a", ""]),
    ("\\M", "\\%[*]", ["*"]),
    ("", "\\%", ["a"]),
    ("", "\\% ", ["a"]),
    ("", "\\%0", ["a"]),
    ("", "\\%1", ["a"]),
    ("", "\\%c", ["a"]),
    ("", "\\%<", ["a"]),
    ("", "\\%\\a", ["a"]),
])


def collect(cases, dropped, syntax):
    """Ask precisely the engine named by this profile, retaining every split."""
    raw = vim_diff.ask_vim(cases, engine=vim_diff.PROFILES[syntax])
    if len(raw) != len(cases):
        sys.stderr.write("%s answered %d of %d rows\n"
            % (syntax, len(raw), len(cases)))
        return None, None
    return cases, raw


# `\%C`, from a matrix of thirty patterns by nine subjects and a second of
# the counted repeats, asked of both engines. `\\_s\\(\\%C\\)*` is not a row: the NFA
# reference captures the marks there and this library does not, which
# documentation/dialects.md records, and the vim vectors do not carry gaps. U is U+0301 and V is U+0302.
_U = "\u0301"
_V = "\u0302"
_C_SUBJECTS = ["ab", "a" + _U + "b", "a" + _U + _V + "b", _U + "b", "a",
               "a" + _U, _U, "b" + _U, "a" + _U + _U + "b"]
NAMED_CASES.extend([
    ("", pattern, _C_SUBJECTS) for pattern in (
        "\\%C", "a\\%C", "\\%Ca", "a\\%Cb", "\\%Cb", "a\\%C*", "a\\%C\\+",
        "a\\%C\\?", "\\%C*", "\\%C\\+", "\\%C\\{2}", "a.\\%C", "a\\zs\\%Cb",
        "[ab]\\%C", ".\\%C", "\\w\\%C", "\\Za\\%C", "a\\%C$", "^\\%C", "\\%C$",
        "\\(a\\%C\\)b", "\\(\\%C\\)b", "a\\|\\%C", "\\%C\\|a", "a\\%C\\%Cb",
        "\\%Cb\\@=", "a\\(\\%C\\)", "x\\@!\\%Cb",
        "\\%C\\{0}", "\\%C\\{1}", "\\%C\\{3}", "\\%C\\{0,1}", "\\%C\\{1,2}",
        "\\%C\\{2,3}", "\\%C\\{-1}", "\\%C\\{-2}", "\\%C\\{-0,1}", "\\%C\\{-1,2}",
        "\\%C\\{}", "\\%C\\{-}", "\\%C\\{,2}", "a\\%C\\{0}", "a\\%C\\{1}",
        "a\\%C\\{2}", "a\\%C\\{0,1}", "a\\%C\\{1,2}", "a\\%C\\{-1}", "a\\%C\\{-0,1}",
        "a\\%C\\{}", "a\\%C\\{-}", "a\\%C\\=", "a\\%C\\@=", "a\\%C\\@!",
        "\\(\\%C\\)*", "\\(\\%C\\)\\{2}", "a\\(\\%C\\)*", "a\\(\\%C\\)\\+",
        "a\\(\\%C\\)\\?", "a\\(\\%C\\)\\{2}", "\\%C\\{1,}", "a\\%C\\{1,}",
        "\\%C\\{1,}b", "\\%C\\{0,}", "a\\%C\\{0,}", "a\\%C\\{1,3}", "\\%C\\{-1,}",
        "a\\%C\\{-1,}",
    )
] + [
    ("", pattern, ["a\n" + _U + "b", "a\n" + _U, "\n" + _U + "b",
                   "a\n" + _U + _V + "b", "a\nb"])
    for pattern in (
        "\\_s\\%C", "\\_s\\%Cb", "\\_d\\%C", "\\_w\\%C", "\\_a\\%C", "\\_[ab]\\%C",
        "\\_[^x]\\%C", "\\_S\\%C", "\\_W\\%C", "\\_s", "\\_s.", "\\_s\\%C.",
        "\\(\\_s\\)\\%C", "\\(\\_s\\%C\\)b", "\\_.\\%C", "\\n\\%C", "\\_s\\%C\\+",
        "\\_s\\%C*", "\\_s\\(\\%C\\)", "\\_s\\%C$", "\\_s\\%C\\=", "\\_s\\%C\\?",
        "\\_s\\%C\\{0}", "\\_s\\%C\\{1}", "\\_s\\%C\\{2}", "\\_s\\%C\\{0,1}",
        "\\_s\\%C\\{0,}", "\\_s\\%C\\{1,}", "\\_s\\%C\\{-}", "\\_s\\%C\\{-1}",
        "\\_s\\%C\\{-0,1}", "\\_s\\%C\\{-1,}", "\\_s\\%C*b", "\\_s\\%C\\=b",
        "\\_s\\%C\\{0,}b", "\\_s\\(\\%C\\)\\=", "\\_s\\%C\\%C*",
        "\\_s\\%C*\\%C", "\\n\\%C*", "\\n\\%C\\=", "\\_.\\%C*",
    )
] + [
    ("\\v", pattern, _C_SUBJECTS) for pattern in ("%C", "%Cb", "b%C", "a%C+", "a%C*")
] + [
    ("\\M", pattern, _C_SUBJECTS) for pattern in ("\\%C", "a\\%C", "a\\%Cb")
] + [
    ("\\V", pattern, _C_SUBJECTS) for pattern in ("\\%C", "a\\%C", "a\\%Cb")
])


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=None, metavar="VECTOR_ROOT",
        help="parent of vim-backtrack/ and vim-nfa/ output directories "
             "(default: tests/data/vectors)")
    parser.add_argument("--syntax", choices=tuple(vim_diff.PROFILES) + ("all",),
        default="all")
    parser.add_argument("--seed", type=int, default=20260926)
    parser.add_argument("--patterns", type=int, default=400)
    parser.add_argument("--subjects", type=int, default=10)
    args = parser.parse_args(argv[1:])

    vector_root = args.out or os.path.join(ROOT, "tests", "data", "vectors")

    encoding = vim_diff.vim_encoding()
    if encoding != "utf-8":
        sys.stderr.write("vim answers in %s, not utf-8; the subjects here are "
            "UTF-8 and every non-ASCII row would be written down wrong.\n"
            % encoding)
        return 2
    version = vim_diff.vim_version()

    named = []
    for level, pattern, subjects in NAMED_CASES:
        for subject in subjects:
            named.append((level + pattern, subject))

    rng = random.Random(args.seed)
    generated = [vim_diff.make_pattern(rng) for _ in range(args.patterns)]
    rows = []
    for pattern in generated:
        for subject in rng.sample(vim_diff.SUBJECTS,
                min(args.subjects, len(vim_diff.SUBJECTS))):
            rows.append((pattern, subject))
    rows.extend(vim_diff.composing_cases())
    rows.extend(vim_diff.combining_grammar_cases())
    rows.sort()


    profiles = vim_diff.PROFILES if args.syntax == "all" else (args.syntax,)
    for syntax in profiles:
        out_dir = os.path.join(vector_root, syntax)
        os.makedirs(out_dir, exist_ok=True)
        for name, cases, dropped in (
                ("named.rxt", named, {}),
                ("generated.rxt", rows, {})):
            kept_cases, kept_answers = collect(cases, dropped, syntax)
            if kept_cases is None:
                return 2
            write_file(os.path.join(out_dir, name), kept_cases, kept_answers,
                "%s; regexpengine=%d" % (version, vim_diff.PROFILES[syntax]),
                dropped, syntax)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))

