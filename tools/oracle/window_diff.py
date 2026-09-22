#!/usr/bin/env python3
"""Compare the search window and the subject-side flags against pcre2.

`GRX_SearchOptions` carries five things a compiled pattern cannot know:
`begin`, `end`, and the NOTBOL / NOTEOL / NOTEMPTY / NOTEMPTY_ATSTART flags.
They decide answers - the same pattern against the same bytes matches or does
not depending on them - and until this file nothing generated varied any of
them. Every other differential in this suite searches the whole subject with
no flags, so all six knobs sat at one value across every one of the hundreds
of thousands of rows the suite runs.

That is the shape of failure `engine_diff.py` was already caught by once: a
generator that varies the input but not the mode that decides what the right
answer is. `tests/unit/test_search.cpp` has thirteen hand-written cases for
these five fields, and thirteen cases is what a reader checks by hand, which
is exactly the set a generator is for.

**pcre2 is the oracle, and it is the only one that can be.** The mapping is
exact: `begin` is `pcre2_match()`'s `startoffset`, `end` is the `length` it is
given, and the four flags are PCRE2_NOTBOL, PCRE2_NOTEOL, PCRE2_NOTEMPTY and
PCRE2_NOTEMPTY_ATSTART. More than that, PCRE2 is the one reference here whose
offsets are bytes and whose `^` and `\\A` mean the start of the *subject*
rather than the start of the search - which is what
`GRX_SearchOptions::begin` means too, and is the asymmetry exec.h calls
deliberate.

The references that cannot answer, and why not:

- **node** has `lastIndex`, which is `begin`, and slicing, which is `end` -
  but no NOTBOL or NOTEOL at all, and its offsets are UTF-16 code units, so
  two thirds of the axis would go unasked through a conversion layer that is
  itself a source of disagreement.
- **glibc** has `REG_NOTBOL` and `REG_NOTEOL`, and `REG_STARTEND` looks like
  a window until you read what it does to `^`: it matches at `rm_so`, where
  this library's `^` and PCRE2's stay at offset 0. It is a different
  question with the same name.

Every row runs in byte mode - `FLAG_SETS` has no UTF option, because PCRE2's
UTF mode is an option rather than a pattern flag - so every offset this
generates is a valid one and none has to be excluded.

Usage:
    tools/oracle/window_diff.py [--seed N] [--patterns N] [--windows N]
                               [--examples N]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import json
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

sys.path.insert(0, HERE)

import perl_diff

# The constructs whose answer the window and the flags actually change.
# perl_diff's vocabulary builds patterns to be matched; these are the ones
# that ask *where* the subject starts and ends, which is the question this
# file exists to put.
WINDOW_ATOMS = [
    "^", "$", "\\A", "\\Z", "\\z", "\\b", "\\B", "\\G",
    "(?<=a)", "(?<!a)", "(?<=ab)", "(?=a)", "(?!a)", "(?<=\\n)",
    "a*", "a*?", "(?:)", "()", "a?", "[^a]*", ".*", ".*?",
    "^a", "a$", "\\Ga", "\\ba", "a\\b", "\\Aa", "a\\z", "a\\Z",
]

# Every combination of the four subject-side flags is cheap enough to take
# whole: there are sixteen of them, and leaving any out would be choosing
# which interaction not to test. NOTEMPTY and NOTEMPTY_ATSTART together is
# the one the header calls out - the stronger wins - and it is in here
# because the list is exhaustive rather than because somebody remembered it.
FLAG_LETTERS = ["", "B", "E", "M", "A", "BE", "BM", "BA", "EM", "EA", "MA",
                "BEM", "BEA", "BMA", "EMA", "BEMA"]


def make_pattern(rng):
    """Half a window construct on its own, half one inside a real pattern."""
    if rng.random() < 0.4:
        return rng.choice(WINDOW_ATOMS)
    parts = [rng.choice(WINDOW_ATOMS if rng.random() < 0.5
                        else perl_diff.ATOMS["pcre"])
             for _ in range(rng.randint(1, 3))]
    return "".join(parts)


def windows_for(subject, rng, count):
    """Windows to try against one subject, always with begin <= end."""
    length = len(subject.encode("utf-8"))
    begins = sorted({0, 1, length // 2, max(length - 1, 0), length})
    ends = sorted({None, length, length // 2, max(length - 1, 0)},
                  key=lambda value: -1 if value is None else value)
    out = set()
    for _ in range(count):
        begin = rng.choice(begins)
        end = rng.choice(ends)
        if end is not None and begin > end:
            begin = end
        out.add((begin, end, rng.choice(FLAG_LETTERS)))
    return sorted(out, key=lambda w: (w[0], -1 if w[1] is None else w[1], w[2]))


def spell(window):
    begin, end, letters = window
    return "%d,%s,%s" % (begin, "-" if end is None else end, letters)


def ask(driver, rows, extra=()):
    lines = "".join("%s\t%s\t%s\t%s\n" % (
        flags, pattern.encode("utf-8").hex(), subject.encode("utf-8").hex(),
        spell(window))
        for flags, pattern, subject, window in rows)
    finished = subprocess.run([driver, *extra], input=lines,
        capture_output=True, text=True, check=True)
    return finished.stdout.splitlines()


def normalise(line):
    """A driver line, with the engine name that only one side prints removed."""
    if line.startswith("match "):
        fields = line.split()
        if fields[1] in ("pike", "backtrack", "bitstate"):
            return " ".join(["match"] + fields[2:])
    return line


def find(name):
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(ROOT, "build", platform, build, "apps",
                "tools", name)
            if os.path.exists(path):
                return path
    return None


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--patterns", type=int, default=300)
    parser.add_argument("--windows", type=int, default=4)
    parser.add_argument("--examples", type=int, default=8)
    args = parser.parse_args(argv[1:])

    ours = find("grx_match")
    theirs = find("pcre2_match")
    if not ours:
        sys.stderr.write("the grx_match tool was not found; run `make tools`\n")
        return 2
    if not theirs:
        sys.stderr.write(
            "pcre2_match was not built, so the window axis has no oracle; "
            "see the pcre2 block in the Makefile\n")
        return 2

    rng = random.Random(args.seed)
    rows = []
    for _ in range(args.patterns):
        pattern = make_pattern(rng)
        for flags in perl_diff.FLAG_SETS:
            for subject in perl_diff.SUBJECTS:
                for window in windows_for(subject, rng, args.windows):
                    rows.append((flags, pattern, subject, window))

    mine = ask(ours, rows, ("pcre",))
    yours = ask(theirs, rows)
    if len(mine) != len(rows) or len(yours) != len(rows):
        sys.stderr.write("a driver did not answer every row (%d/%d of %d)\n"
            % (len(mine), len(yours), len(rows)))
        return 2

    disagreements = []
    compared = 0
    refused = 0
    declined = 0

    for index, (flags, pattern, subject, window) in enumerate(rows):
        ours_line = normalise(mine[index])
        theirs_line = normalise(yours[index])

        if ours_line == "compile" or theirs_line == "compile" \
                or ours_line.startswith("compile "):
            refused += 1
            continue
        # pcre2 declining - a match limit, a bad offset - is not an opinion
        # about the window that this library can be held to.
        if theirs_line.startswith("skip") or ours_line.startswith("unsupported") \
                or ours_line.startswith("error"):
            declined += 1
            continue

        compared += 1
        if ours_line != theirs_line:
            disagreements.append((flags, pattern, subject, window,
                ours_line, theirs_line))

    for flags, pattern, subject, window, a, b in disagreements[:args.examples]:
        print("/%s/%s on %s window %s"
              % (pattern, flags, json.dumps(subject), spell(window)))
        print("    ours:  %s" % a)
        print("    pcre2: %s" % b)

    print("window: %d rows, %d compared, %d disagreements"
          % (len(rows), compared, len(disagreements)))
    if refused:
        print("%-7s %d the pattern was refused" % ("", refused))
    if declined:
        print("%-7s %d one side declined to answer" % ("", declined))
    if compared < len(rows) // 2:
        sys.stderr.write(
            "fewer than half the rows were compared; the generator is "
            "measuring itself\n")
        return 2
    return 1 if disagreements else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
