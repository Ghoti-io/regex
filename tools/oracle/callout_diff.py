#!/usr/bin/env python3
"""Compare the callout trace against pcre2's, callout by callout.

A callout is the one construct here whose answer is not "did it match" but
"where were you, and when". That makes it the one construct a match
differential is blind to: `a(?C1)b` and `ab` match the same subjects, and
every gate in tools/oracle/ that compares spans would report agreement
about a feature that was not built.

So this compares the *trace*: for each row, the sequence of callouts both
implementations reported, each one as its number, the offset the attempt
began at, how far matching had got, the pattern offset, pcre2's capture_top
and the string and mark it carried. A single character out of place in any
of those is a disagreement, and the order is backtracking order - which is
what makes this a test of the engine and not only of the parser.

**Two pcre2 optimisations are off**, and the comparison would be
meaningless with them on. PCRE2_NO_START_OPTIMIZE stops pcre2 rejecting a
subject before it runs - with it on, `/a(?C1)b(?C2)c/` against "abd" prints
no callouts at all, because the required code unit `c` is absent.
PCRE2_NO_AUTO_POSSESS stops it turning `a*` before `b` into `a*+` - with it
on, `a*(?C1)b` against four a's prints five callouts where a plain
backtracker prints fifteen. Neither changes what pcre2 *matches*, and this
library has neither, so leaving them on would measure pcre2's start-up
analysis rather than the construct. tools/oracle/pcre2_match.c sets them in
its `callout` mode and in no other.

The corpus is every callout spelling inserted at every position of every
skeleton, against every subject. Most insertions are syntax errors -
`a(?C1)*b` has nothing to repeat, `(?C1)` inside a class is not a callout
at all - and those rows are kept: that both sides refuse them is half of
what "the same grammar" means, and a generator that only produced valid
patterns would never ask.

The backtracking control verbs are deliberately absent from the skeletons.
pcre2api says NO_START_OPTIMIZE changes what `(*COMMIT)`, `(*SKIP)` and
`(*PRUNE)` do, so a row containing one would be comparing two different
patterns.

**Nothing is excluded, and two shapes used to be.**

A callout written *where the condition goes* - `(?(?C9)(?=a)b|c)` - was
dropped by the parser, because a conditional's children are the condition
and the branches positionally and there was no fourth slot to carry it in.
Our trace was empty where pcre2's had the callout, 150 rows in 25,600, and
the match was identical throughout - which is exactly the kind of
difference only a gate like this one can see. The parser keeps them now and
lowering hoists them in front of the conditional, which is where pcre2test
prints them.

The other was a callout *inside* an assertion condition: lowering rewrote
`(?(?=A)X|Y)` as `(?:(?=A)X|(?!A)Y)`, which is exact and compiles the
assertion twice, so a callout in it fired twice where pcre2 fires it once.
This gate is what turned that rewrite's documented cost - "time and not
meaning" - into a visible one, and it is gone: the condition is now one
`GRX_INST_COND_ELSE` assertion that chooses a branch instead of failing.

So a run with any disagreement in it is a defect, and the corpus check
below is what says the shape that used to fail is still being generated.

Usage:
    tools/oracle/callout_diff.py [--examples N]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import binascii
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# Every spelling of a callout, valid and not. `(?C256)` is over pcre2's
# one-byte bound, `(?C"a"")` is the greedy doubling rule read to its end,
# and `(?C}})` is a closer used as an opener - which pcre2 refuses with a
# diagnostic of its own.
CALLOUTS = [
    "(?C)", "(?C0)", "(?C1)", "(?C9)", "(?C255)", "(?C256)", "(?C1x)",
    '(?C"")', '(?C"s")', '(?C"a""b")', "(?C{p}}q})", "(?C'q')", "(?C^r^)",
    "(?C%t%)", "(?C#u#)", "(?C$v$)", "(?C`w`)", '(?C"a"")', "(?C}})",
    "(?C{a)",
]

# No control verbs; see the module docstring. `(*MARK:m)` is here because a
# callout reports the mark standing over it and nothing else would ask.
SKELETONS = [
    "a", "ab", "a*b", "a+b?", "(a)(b)", "(a|b)+", "a{2,3}", "[ab]c",
    "(?:ab)*c", "a(?=b)b", "(?<=a)b", "(a)\\1", "x(*MARK:m)y", ".a",
    "\\d\\w", "a??b", "(a)?b", "(?(?=a)ab|c)", "a|b", "(?>a*)b",
]

SUBJECTS = ["", "a", "b", "ab", "aab", "abc", "aaa", "xaby", "ba", "xy"]


def find(name):
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(
                ROOT, "build", platform, build, "apps", "tools", name)
            if os.path.exists(path):
                return path
    return None


def patterns():
    """Every callout at every position of every skeleton, deduplicated."""
    seen = set()
    for skeleton in SKELETONS:
        for callout in CALLOUTS:
            for at in range(len(skeleton) + 1):
                pattern = skeleton[:at] + callout + skeleton[at:]
                if pattern not in seen:
                    seen.add(pattern)
                    yield pattern


def rows():
    for pattern in patterns():
        for subject in SUBJECTS:
            yield "u", pattern, subject


def ask(command, cases):
    lines = "".join(
        "%s\t%s\t%s\n" % (flags,
            binascii.hexlify(pattern.encode()).decode(),
            binascii.hexlify(subject.encode()).decode())
        for flags, pattern, subject in cases)
    finished = subprocess.run(command, input=lines, capture_output=True,
        text=True)
    return finished.stdout.splitlines()


def normalise(line):
    """One driver names the diagnostic it refused with and the other does not.

    Which diagnostic pcre2 chose for a malformed pattern is a question
    tools/oracle/syntax_diff.py already asks, row by row, against its own
    corpus. Here the question is the trace, and "both refused it" is the
    whole of what a refusal contributes.
    """
    if line.startswith("compile"):
        return "compile"
    return line


def show(text):
    return "".join(
        c if " " <= c <= "~" else "\\x%02x" % ord(c) for c in text)


def parts(line):
    """A trace line split into its outcome and its callouts."""
    # `trace <outcome> <count> <callout>...`, so the callouts begin at 3.
    # Slicing from 2 would make the count itself look like a callout, and
    # a row with none would then read as a row with one.
    fields = line.split()
    if len(fields) < 3 or fields[0] != "trace":
        return None, None
    return fields[1], fields[3:]




def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--examples", type=int, default=12)
    args = parser.parse_args(argv[1:])

    ours = find("grx_match")
    theirs = find("pcre2_match")
    if not ours or not theirs:
        sys.stderr.write("run `make tools` first\n")
        return 2

    cases = list(rows())
    mine = ask([ours, "pcre", "callout"], cases)
    reference = ask([theirs, "callout"], cases)
    if len(mine) != len(cases) or len(reference) != len(cases):
        sys.stderr.write("a driver answered %d and %d of %d requests\n"
                         % (len(mine), len(reference), len(cases)))
        return 2

    disagreements = []
    skipped = 0
    # The shape that used to be excluded, counted so that a corpus which
    # stopped producing it cannot read as agreement.
    in_condition = 0
    for (flags, pattern, subject), us, them in zip(cases, mine, reference):
        if "(?(?C" in pattern:
            in_condition += 1
        if them.startswith("skip") or us == "unsupported":
            # pcre2 declining to answer, or a program no engine here runs.
            # Counted rather than hidden: a corpus that quietly stopped
            # asking would read as agreement.
            skipped += 1
            continue
        if normalise(us) == normalise(them):
            continue
        disagreements.append((flags, pattern, subject, them, us))

    for flags, pattern, subject, them, us in disagreements[:args.examples]:
        print("  %-4s %-24s %-6s" % (flags, show(pattern), show(subject)))
        print("       pcre2=%s" % them)
        print("       ours =%s" % us)
    print("callouts: %d patterns x %d subjects = %d rows, %d skipped, "
          "%d with a callout where the condition goes, %d disagreements"
          % (len(cases) // len(SUBJECTS), len(SUBJECTS), len(cases), skipped,
             in_condition, len(disagreements)))
    # A run that produced *none* of the shape that used to fail would be a
    # gate that had quietly stopped asking rather than one that had been
    # satisfied.
    if not in_condition:
        print("  no row put a callout where the condition goes: "
              "the corpus changed")
        return 2
    return 1 if disagreements else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
