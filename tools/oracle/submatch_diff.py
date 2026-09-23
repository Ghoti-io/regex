#!/usr/bin/env python3
"""Compare *which* spans the groups get, where more than one answer fits.

`posix_diff.py` asks whether the same text matched. This asks the question
underneath it: when two ways of matching produce the same overall extent and
different group spans, which does POSIX want? That question has no answer in
the imported vectors - Spencer's cases do not contain a single one - and the
patterns `posix_diff.py` builds reach it only by accident, so the axis went
unasked until this tool existed.

It is asked by building patterns out of *ambiguous* pieces on purpose: groups
whose branches overlap, groups that can match empty, and quantified groups
next to each other, so that almost every generated case has two or more
assignments to choose between.

The oracles and who decides are `posix_diff.py`'s - glibc alone is the
definition of `gnu-bre` and `gnu-ere`, and for `posix-bre` and `posix-ere`
what counts is glibc and musl agreeing. The difference here is that their
disagreements are not merely skipped: they are counted and named, because
they are the open question. Where the two references answer differently it
is glibc that is not following POSIX, musl's answer is the one POSIX's rule
gives, and this library answers as glibc does. That set is WP-26 in
documentation/plan.md, and this tool exists partly to keep measuring it:
`--strict` fails if it ever empties, because a claim about an open question
should not outlive the question.

`posix-bre` is the thin row and says so here rather than in a comment
nobody reads: POSIX basic REs have no alternation, so the empty-branch half
of this question cannot be spelled in one at all. What its cases ask is the
other half - two quantified groups next to each other, and which of them the
text belongs to.

Usage:
    tools/oracle/submatch_diff.py [--examples N] [--strict]

Without musl_match the two POSIX dialects are skipped and said to be skipped
- run `tools/corpus/fetch.sh musl` and `make tools` for them.

Copyright 2026 by Corey Pennycuff
"""

import argparse
import binascii
import itertools
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# Pieces chosen for ambiguity rather than for coverage. Every one of them can
# match more than one way, or can match empty, or both - concatenate two and
# the question "which of these got the `a`" almost always has two answers.
#
# The empty *branches* are the ones that matter most and are easiest to get
# wrong, because an engine that resolves a tie by which path it reached first
# will hand the empty branch everything whenever it is written first.
GROUPS = {
    "posix-ere": ["(|a)", "(a|)", "(|ab)", "(ab|)", "(|b|a)", "(a|b|)",
                  "(a|ab)", "(ab|a)", "(a|aa)", "(aa|a)", "(a*)", "(a?)",
                  "(a+)", "([ab]*)", "(a|b)", "(()|a)", "(b*|a)",
                  "(a{0,2})", "((a)|(ab))", "(a*b*)",
                  "(a{0}|a)", "(a{0}b{0}|a)", "((a){0}|a)"],
    "posix-bre": ["\\(a*\\)", "\\(aa*\\)", "\\(a\\{0,1\\}\\)",
                  "\\(a\\{0,2\\}\\)", "\\([ab]*\\)", "\\(a*b*\\)",
                  "\\(\\(a\\)*\\)", "\\(b*a*\\)"],
    "gnu-ere": ["(|a)", "(a|)", "(|ab)", "(ab|)", "(|b|a)", "(a|b|)",
                "(a|ab)", "(ab|a)", "(a|aa)", "(aa|a)", "(a*)", "(a?)",
                "(a+)", "([ab]*)", "(a|b)", "(()|a)", "(b*|a)",
                "(a{0,2})", "((a)|(ab))", "(a*b*)",
                "(a{0}|a)", "(a{0}b{0}|a)", "((a){0}|a)"],
    "gnu-bre": ["\\(\\|a\\)", "\\(a\\|\\)", "\\(\\|b\\|a\\)",
                "\\(a\\|ab\\)", "\\(ab\\|a\\)", "\\(a\\|aa\\)",
                "\\(a*\\)", "\\(a\\?\\)", "\\(a\\+\\)",
                "\\([ab]*\\)", "\\(a\\|b\\)", "\\(\\(\\)\\|a\\)",
                "\\(b*\\|a\\)", "\\(a\\{0,2\\}\\)", "\\(a*b*\\)",
                "\\(a\\{0\\}\\|a\\)", "\\(\\(a\\)\\{0\\}\\|a\\)"],
}

SUBJECTS = ["", "a", "b", "aa", "ab", "ba", "bb", "aaa", "aab", "aba",
            "abb", "baa", "bab", "abab", "aabb", "abc", "aabc"]

DECIDED_BY = {
    "gnu-ere": ("glibc",),
    "gnu-bre": ("glibc",),
    "posix-ere": ("glibc", "musl"),
    "posix-bre": ("glibc", "musl"),
}

BASIC_FLAG = {"gnu-ere": "", "gnu-bre": "b",
              "posix-ere": "", "posix-bre": "b"}


def normalise_ours(line):
    """grx_match's answer in the shape the POSIX drivers write theirs."""
    if line.startswith("match "):
        return "match " + " ".join(line.split()[2:])
    if line.startswith("compile") or line.startswith("error") \
            or line == "unsupported":
        return "compile"
    return line


def ask(command, cases):
    lines = []
    for flags, pattern, subject in cases:
        lines.append("%s\t%s\t%s" % (flags,
            binascii.hexlify(pattern.encode()).decode(),
            binascii.hexlify(subject.encode()).decode()))
    process = subprocess.run(command, input="\n".join(lines) + "\n",
        capture_output=True, text=True)
    return process.stdout.splitlines()


def find(name):
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(ROOT, "build", platform, build, "apps",
                "tools", name)
            if os.path.exists(path):
                return path
    return None


def compare(dialect, examples):
    drivers = {"glibc": find("posix_match"), "musl": find("musl_match")}
    ours = find("grx_match")
    if not drivers["glibc"] or not ours:
        sys.stderr.write("run `make tools` first\n")
        return None
    wanted = DECIDED_BY[dialect]
    if "musl" in wanted and not drivers["musl"]:
        print("%s: skipped (no musl_match; run tools/corpus/fetch.sh musl "
              "and `make tools`)" % dialect)
        return (0, 0)

    patterns = ["".join(pair)
                for pair in itertools.product(GROUPS[dialect], repeat=2)]
    flag = BASIC_FLAG[dialect]
    cases = [(flag, pattern, subject)
             for pattern in patterns for subject in SUBJECTS]
    answers = {name: ask([drivers[name]], cases) for name in wanted}
    mine = ask([ours, dialect], cases)
    if any(len(rows) != len(cases) for rows in answers.values()) \
            or len(mine) != len(cases):
        sys.stderr.write("a driver answered a different number of requests\n")
        return None

    disagreements = []
    splits = []
    compared = 0
    declined = 0
    for index, (case, us) in enumerate(zip(cases, mine)):
        theirs = [answers[name][index] for name in wanted]
        if any(answer.startswith("skip") for answer in theirs):
            declined += 1
            continue
        if len(set(theirs)) != 1:
            # glibc and musl each answering a tie their own way: the open
            # question, not a defect of this library. Which side we came
            # down on is recorded, because siding with neither would be.
            splits.append((case[1], case[2], theirs[0], theirs[1],
                normalise_ours(us)))
            continue
        compared += 1
        if theirs[0] != normalise_ours(us):
            disagreements.append(
                (case[1], case[2], theirs[0], normalise_ours(us)))

    for pattern, subject, them, us in disagreements[:examples]:
        print("  %-26s on %-8s oracle=%-24s ours=%s"
              % (repr(pattern), repr(subject), them, us))
    tally = ""
    if splits:
        sided = sum(1 for row in splits if row[4] == row[2])
        other = sum(1 for row in splits if row[4] == row[3])
        tally = (", %d the two references answer differently (ours sides "
                 "with glibc %d, with musl %d, with neither %d)"
                 % (len(splits), sided, other,
                    len(splits) - sided - other))
    print("%s: %d patterns x %d subjects = %d cases, %d compared against "
          "%s, %d disagreements%s"
          % (dialect, len(patterns), len(SUBJECTS), len(cases), compared,
             " and ".join(wanted), len(disagreements), tally))
    return (len(disagreements), len(splits))


def main(argv):
    parser = argparse.ArgumentParser(
        description="compare group spans against glibc and musl")
    parser.add_argument("--examples", type=int, default=8)
    parser.add_argument("--strict", action="store_true",
        help="also fail if the references stop disagreeing with each other")
    args = parser.parse_args(argv[1:])

    total = 0
    splits = 0
    for index, dialect in enumerate(sorted(GROUPS)):
        if index:
            print()
        outcome = compare(dialect, args.examples)
        if outcome is None:
            return 2
        total += outcome[0]
        splits += outcome[1]

    if args.strict and not splits:
        sys.stderr.write(
            "glibc and musl agreed everywhere: the open question this tool "
            "describes is gone, and what it says about WP-26 is stale\n")
        return 2
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
