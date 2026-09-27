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

**What this corpus deliberately does not cover, and why it is decided from
the pattern text alone.** `vim_diff.py` excludes a row from its disagreement
count when the row's shape is one of seven measured vim artifacts, and each
of its predicates looks at *both* answers - vim's and this library's. A
generator cannot use those: a corpus whose contents depend on what this
library answered at generation time is one that quietly shrinks after a
regression, so the row that would have reported the regression is the row that
stops being written. The exclusion here is therefore a function of the pattern
only, and it is coarser on purpose:

  - **any pattern with a `@` in it.** That is every postfix operator vim has,
    and it is where five of the seven artifacts live: a forward backreference
    in front of an unbounded postfix lookbehind, captures an abandoned branch
    left behind, marks kept from a lookbehind, a lookbehind mis-accounted when
    a backreference follows. Those are real and measured against pcre2test,
    and none of them can be told from a defect here by looking at the pattern.
    It costs the corpus vim's lookaround and atomic groups entirely;
    `check-oracle-vim` still covers them, and this file says so rather than
    recording answers nobody could follow.
  - `\%23l*` and its spellings, which mean nothing at all in vim.
  - `\v\_^*`, which matches nothing where three other spellings of it match
    the empty string.
  - a bare `*` after `\%(` or after `^` with markers between, the two
    spellings vim refuses where six others accept.

And, separately from shape: **a row where vim's two engines disagree is
dropped**, because vim has no one answer for it. That is asked of vim rather
than inferred, and it is `vim_diff.py`'s own rule. The exception is a subject
carrying a composing character, where `set re=1` has no cluster model at all
and its answer is a third opinion rather than a second - those rows are the
default engine's, which is the reference.

Usage:
    tools/oracle/make_vim_vectors.py [--out DIR] [--seed N] [--patterns N]

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
# Every one of them is free of a `@`, because the generated half cannot carry
# those and a named case that the exclusion above then dropped would be a case
# nobody notices is missing.
NAMED_CASES = [
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
    # **Four of these nine rows are not in the corpus**, and they are worth
    # naming rather than letting the header's count carry them: vim's two
    # engines disagree about U+2170, U+01C5 and U+24B6 - `set re=2` matches
    # and `set re=1` does not - so vim has no one answer to write down.
    # `check-oracle-vim` owns them, where they land in its engine-split
    # bucket with `set re=1` giving this library's answer. They stay here so
    # that a regeneration after vim settles the question picks them up.
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
]

# A bare multi after `\%(`, or after a `^` with only level and case markers
# between. The two spellings vim refuses where six others accept one - see
# `vim_diff.is_leading_star_artifact()`, of which this is the half that reads
# the pattern and not the answers.
MARKERS = ("\\v", "\\m", "\\M", "\\V", "\\c", "\\C")


def leading_star_shape(pattern):
    for index, char in enumerate(pattern):
        if char != "*" or index == 0:
            continue
        head = pattern[:index]
        if head.endswith("\\%("):
            return True
        while head[-2:] in MARKERS:
            head = head[:-2]
            if head.endswith("^"):
                return True
    return False


def artifact_shape(pattern):
    """Which known vim artifact this pattern could reach, or None.

    Named rather than counted, so that a regeneration whose distribution
    moved says which bucket moved.
    """
    if "@" in pattern:
        return "postfix operator"
    if vim_diff.LINE_NUMBER_STAR.search(pattern):
        return "line number with a multi"
    if "\\_^*" in pattern:
        return "very magic line start with a star"
    if leading_star_shape(pattern):
        return "a bare multi vim refuses in this spelling"
    return None


def record_for(pattern, subject, answer):
    """One `.rxt` record from one of vim's answers, or None."""
    lines = ["pattern: " + make_vectors.escape(pattern)]

    if answer == "compile":
        # vim throws, and a caught exception says only that it refused. The
        # code this library answers with is swept per construct by
        # `Vim.EveryRefusalIsASyntaxRefusal`; see rxt.h.
        lines.append("expect: refused")
        return "\n".join(lines) + "\n"
    if not answer.startswith("match ") and answer != "nomatch":
        return None

    lines.append("subject: " + make_vectors.escape(subject))
    if answer == "nomatch":
        lines.append("expect: nomatch")
        return "\n".join(lines) + "\n"

    fields = json.loads(answer[len("match "):])
    start, end = fields[0].split(":")
    lines.append("expect: %s-%s" % (start, end))

    groups = list(fields[1:])
    while groups and groups[-1] == "":
        groups.pop()
    # An empty `groups:` line would be a line saying nothing, and the reader
    # would read it as "no groups stated" anyway. Written only when vim named
    # at least one.
    if groups:
        lines.append("groups: "
            + " ".join(make_vectors.escape_group(g) for g in groups))
    return "\n".join(lines) + "\n"


def write_file(path, rows, answers, version, dropped):
    body = []
    refused = set()
    for (pattern, subject), answer in zip(rows, answers):
        if answer == "compile":
            # A refusal has no subject in it, so the other subjects of one
            # refused pattern would be exact duplicates - and a duplicate
            # counts one fact many times in the rate the suite publishes.
            if pattern in refused:
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
        out.write("dialect: vim\n\n")
        out.write("\n".join(body))

    sys.stderr.write("%s: %d records\n" % (path, len(body)))
    return len(body)


def collect(cases, dropped):
    """Ask vim, and keep the rows vim has one answer for.

    Two passes rather than one, because "vim's two engines disagree" cannot
    be read off a pattern - it is the one exclusion here that is a
    measurement. Only the rows that came back different are asked twice.
    """
    raw = vim_diff.ask_vim(cases)
    if len(raw) != len(cases):
        sys.stderr.write("vim answered %d of %d rows\n" % (len(raw), len(cases)))
        return None, None
    # Two readings of one answer, and the records are written from the raw
    # one. `normalise_theirs()` renders the groups for a *comparison* - it
    # json-encodes each and joins them with spaces, which a group containing a
    # space cannot be taken back out of. It is exactly right for asking
    # whether vim's two engines said the same thing and wrong for writing a
    # field down.
    answers = [vim_diff.normalise_theirs(line, case[1])
        for line, case in zip(raw, cases)]

    # The composing rows are the default engine's outright; `set re=1` has no
    # cluster model, so asking it there would drop rows over a difference
    # that is not vim disagreeing with itself.
    asked = [index for index, case in enumerate(cases)
        if not vim_diff.holds_composing(case[1])]
    second = vim_diff.ask_old_engine([cases[i] for i in asked])
    if len(second) != len(asked):
        sys.stderr.write("vim's old engine answered %d of %d rows\n"
            % (len(second), len(asked)))
        return None, None

    split = set()
    for index, line in zip(asked, second):
        if vim_diff.normalise_theirs(line, cases[index][1]) != answers[index]:
            split.add(index)

    kept_cases = []
    kept_answers = []
    for index, (case, answer) in enumerate(zip(cases, raw)):
        if index in split:
            dropped["rows where vim's two engines disagree"] = dropped.get(
                "rows where vim's two engines disagree", 0) + 1
            continue
        kept_cases.append(case)
        kept_answers.append(answer)
    return kept_cases, kept_answers


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=None)
    parser.add_argument("--seed", type=int, default=20260926)
    parser.add_argument("--patterns", type=int, default=400)
    parser.add_argument("--subjects", type=int, default=10)
    args = parser.parse_args(argv[1:])

    out_dir = args.out or os.path.join(ROOT, "tests", "data", "vectors", "vim")
    os.makedirs(out_dir, exist_ok=True)

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
    generated = []
    shapes = {}
    while len(generated) < args.patterns:
        pattern = vim_diff.make_pattern(rng)
        shape = artifact_shape(pattern)
        if shape:
            shapes[shape] = shapes.get(shape, 0) + 1
            continue
        generated.append(pattern)

    rows = []
    for pattern in generated:
        for subject in rng.sample(vim_diff.SUBJECTS,
                min(args.subjects, len(vim_diff.SUBJECTS))):
            rows.append((pattern, subject))
    for case in vim_diff.composing_cases():
        if not artifact_shape(case[0]):
            rows.append(case)
    rows.sort()

    # Counted in *patterns* rather than rows, because a pattern is refused
    # before its subjects are chosen. The engine-split count `collect()` adds
    # is in rows, and each line says which.
    shape_counts = {"patterns not generated: %s" % reason: count
        for reason, count in shapes.items()}

    for name, cases, dropped in (
            ("named.rxt", named, {}),
            ("generated.rxt", rows, dict(shape_counts))):
        kept_cases, kept_answers = collect(cases, dropped)
        if kept_cases is None:
            return 2
        write_file(os.path.join(out_dir, name), kept_cases, kept_answers,
            version, dropped)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
