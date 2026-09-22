#!/usr/bin/env python3
"""Compare `grx_regex_split()` against ECMAScript's, on splits nobody wrote.

`grx_regex_split()` was the last documented surface of this library with no
generated comparison behind it. Matching has `match_diff.py`, replacement has
`replace_diff.py` and `sed_diff.py`, syntax has `syntax_diff.py`; splitting
had six hand-written tests, and every one of them asserts a rule the author
had already decided was right.

That matters more here than the count suggests, because ECMA-262 22.2.6.14 is
not a loop over matches. It walks the *subject*, keeps a `p` for where the
current piece began, and discards a match whose end equals `p`. Three
separate rules fall out of that walk and none of them is guessable:

- The empty subject is decided before the walk starts: one empty piece, or
  none at all if the pattern matches the empty string.
- `limit` counts pieces *including* the capture groups spliced between them,
  and is checked after each append, so `(,)` with a limit of 2 stops in the
  middle of a separator.
- A match starting at or past the end of the subject is not a separator, and
  the piece after the last real separator runs to the end regardless.

A hand-written test for each of those is a test of what the author believed.
What a generator adds is the *interaction*: a limit that runs out between a
piece and its captures, an empty match immediately after a non-empty one, a
capture that participates on one branch and not the other, an anchor that
makes a pattern match only at a position the walk has already passed.

**One definition.** node is ECMA-262; there is no second opinion to take and
none is wanted. The other dialects in documentation/dialects.md section 5.16
are marked **probe** and this library applies ECMAScript's rule to all of
them, so running perl's `split` against this would compare two different
functions - see notes, not this file.

Usage:
    tools/oracle/split_diff.py [--seed N] [--patterns N] [--subjects N]
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

import match_diff

# Patterns whose whole point is what they do to the *walk* rather than what
# they match. match_diff's vocabulary builds patterns to be matched against a
# subject; these are built to land on a piece boundary, and a generator that
# only combined the former would reach them by accident and rarely.
#
# The empty-matching ones are the reason the rule exists at all, the anchored
# ones only ever match where the walk has already been, and the capturing
# ones are what `limit` counts.
SPLIT_ATOMS = [
    "", "x*", "a*", "\\s*", "\\b", "\\B", "(?=b)", "(?!x)", "(?<=a)",
    "$", "^", "(?:)", "(|a)", "(a|)", "()", "(x*)", "(a)|(b)", "(,)",
    ",", ",+", ",*", "[,;]", "(,)(;)?", "(?<n>,)", ",|;", "(?:,|(;))",
]

# `limit` 0 is its own rule, 1 stops before any capture can be appended, and
# the small values are where a limit runs out *between* a piece and the
# captures that follow it - the case the API's own documentation calls out
# and the one a round number would step over.
LIMITS = [None, None, None, 0, 1, 2, 3, 4, 5, 7, 100]

SUBJECT_PIECES = match_diff.SUBJECT_PIECES + [",", ",,", ";", "a,b", ",a", "a,"]


def make_subject(rng, unicode_mode):
    pieces = SUBJECT_PIECES + (match_diff.ASTRAL_PIECES if unicode_mode else [])
    return "".join(rng.choice(pieces) for _ in range(rng.randint(0, 6)))


def make_pattern(rng, unicode_sets):
    """Half from the split vocabulary, half from the matching one."""
    if rng.random() < 0.5:
        return rng.choice(SPLIT_ATOMS)
    return match_diff.make_pattern(rng, unicode_sets)


def ask_node(rows):
    payload = json.dumps([[f, p, s, l] for f, p, s, l in rows])
    finished = subprocess.run(["node", os.path.join(HERE, "node_split.mjs")],
        input=payload, capture_output=True, text=True, check=True)
    return json.loads(finished.stdout)


def ask_library(driver, rows):
    lines = "".join("%s\t%s\t%s\t%s\n" % (
        flags, pattern.encode("utf-8").hex(), subject.encode("utf-8").hex(),
        "-" if limit is None else limit)
        for flags, pattern, subject, limit in rows)
    finished = subprocess.run([driver, "ecmascript"], input=lines,
        capture_output=True, text=True, check=True)
    return finished.stdout.splitlines()


def parse_ours(line):
    """A driver line as a list of pieces, or the word it printed instead."""
    if not line.startswith("ok "):
        return line.split()[0]
    fields = line.split(" ", 2)
    count = int(fields[1])
    if not count:
        return []
    return [None if field == "-" else bytes.fromhex(field).decode("utf-8")
            for field in fields[2].split("|")]


def theirs(answer):
    """The oracle's answer in the same shape, or the word it gave instead."""
    return answer


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--patterns", type=int, default=400)
    parser.add_argument("--subjects", type=int, default=12)
    parser.add_argument("--examples", type=int, default=8)
    parser.add_argument("--driver", default=None)
    args = parser.parse_args(argv[1:])

    driver = args.driver
    if not driver:
        for platform in ("linux", "mac", "win64", "win32"):
            for build in ("release", "debug"):
                path = os.path.join(ROOT, "build", platform, build, "apps",
                    "tools", "grx_split")
                if os.path.exists(path):
                    driver = path
                    break
            if driver:
                break
    if not driver or not os.path.exists(driver):
        sys.stderr.write(
            "the grx_split tool was not found; run `make tools` first\n")
        return 2

    rng = random.Random(args.seed)
    rows = []
    for _ in range(args.patterns):
        flags = rng.choice(match_diff.FLAG_SETS)
        pattern = make_pattern(rng, "v" in flags)
        for _ in range(args.subjects):
            rows.append((flags, pattern, make_subject(rng, "u" in flags or
                "v" in flags), rng.choice(LIMITS)))

    ours = ask_library(driver, rows)
    if len(ours) != len(rows):
        sys.stderr.write("the driver did not answer every row\n")
        return 2
    reference = ask_node(rows)

    disagreements = []
    compared = 0
    refused = 0
    one_sided = 0
    surrogates = 0

    for index, (flags, pattern, subject, limit) in enumerate(rows):
        mine = parse_ours(ours[index])
        yours = reference[index]

        if yours == "surrogate":
            surrogates += 1
            continue
        # A pattern both sides refuse says nothing. One that only one side
        # refuses is a syntax disagreement, which syntax_diff.py is the tool
        # for; counting it here would report the same defect twice and bury
        # the split rule under it.
        if yours == "syntax" or mine == "compile":
            # Both refusing says nothing. One refusing is a syntax
            # disagreement, which syntax_diff.py is the tool for - but it is
            # counted separately here rather than folded into the same
            # number, because a generator whose patterns this library alone
            # rejects is a generator quietly measuring less than it looks
            # like it is.
            if (yours == "syntax") != (mine == "compile"):
                one_sided += 1
            else:
                refused += 1
            continue
        if yours == "error":
            refused += 1
            continue

        compared += 1
        if mine != yours:
            disagreements.append((flags, pattern, subject, limit, mine, yours))

    for flags, pattern, subject, limit, mine, yours in \
            disagreements[:args.examples]:
        print("/%s/%s on %s limit %s"
              % (pattern, flags, json.dumps(subject),
                 "none" if limit is None else limit))
        print("    ours:  %s" % json.dumps(mine))
        print("    node:  %s" % json.dumps(yours))

    print("split: %d rows, %d compared, %d disagreements"
          % (len(rows), compared, len(disagreements)))
    if refused:
        print("%-6s %d refused by both" % ("", refused))
    if one_sided:
        print("%-6s %d refused by one side only - a syntax question, and "
              "syntax_diff.py's" % ("", one_sided))
    if surrogates:
        print("%-6s %d hold half a surrogate pair and cannot be compared"
              % ("", surrogates))
    # A run that compared almost nothing reports zero disagreements and looks
    # exactly like a run that compared everything, which is the failure this
    # suite has already been caught by twice.
    if compared < len(rows) // 2:
        sys.stderr.write(
            "fewer than half the rows were compared; the generator is "
            "measuring itself\n")
        return 2
    return 1 if disagreements else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
