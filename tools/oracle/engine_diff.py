#!/usr/bin/env python3
"""Check that two engines given the same program report the same match.

documentation/design.md section 3.5.4, the equivalence invariant: any pattern
and subject that two engines can both run must yield the same `matched`, the
same group 0, and the same spans for every group.

It is the cheapest strong test this library has, because the two engines
share nothing below the instruction set. The Pike VM merges threads in
lockstep and the backtracker walks one path at a time with an explicit undo
stack; the only thing they have in common is the program they are reading. A
disagreement is therefore a defect in one of them, and there is nowhere for a
shared mistake to hide.

Where the reference oracle checks that the library matches what ECMAScript
says, this checks that the library agrees with *itself* - which catches the
cases the oracle corpus happens not to reach, and needs no oracle installed.

Usage:
    tools/oracle/engine_diff.py [--seed N] [--patterns N] [--subjects N]

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


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--patterns", type=int, default=400)
    parser.add_argument("--subjects", type=int, default=12)
    parser.add_argument("--driver", default=None)
    parser.add_argument("--examples", type=int, default=6)
    args = parser.parse_args(argv[1:])

    driver = args.driver
    if not driver:
        for platform in ("linux", "mac", "win64", "win32"):
            for build in ("release", "debug"):
                path = os.path.join(ROOT, "build", platform, build, "apps",
                    "tools", "grx_match")
                if os.path.exists(path):
                    driver = path
                    break
            if driver:
                break
    if not driver or not os.path.exists(driver):
        sys.stderr.write(
            "the grx_match tool was not found; run `make tools` first\n")
        return 2

    rng = random.Random(args.seed)
    rows = []
    for _ in range(args.patterns):
        pattern = match_diff.make_pattern(rng)
        flags = rng.choice(match_diff.FLAG_SETS)
        for _ in range(args.subjects):
            rows.append(
                (flags, pattern, match_diff.make_subject(rng, "u" in flags)))

    pike = match_diff.ask_library(driver, rows, "pike")
    backtrack = match_diff.ask_library(driver, rows, "backtrack")
    if len(pike) != len(rows) or len(backtrack) != len(rows):
        sys.stderr.write("a run did not answer every row\n")
        return 2

    disagreements = []
    compared = 0
    skipped = 0

    for (flags, pattern, subject), first, second in zip(rows, pike, backtrack):
        # A record the driver could not hold. Both engines would answer
        # "toolong" and the invariant would look satisfied by a comparison
        # that never happened, so stop instead.
        if first == "toolong" or second == "toolong":
            sys.stderr.write(
                "the driver could not hold a record this run generated; "
                "raise MAX_PATTERN/MAX_SUBJECT in tools/oracle/grx_match.c\n")
            return 2

        # A program the Pike VM cannot run is not a program both engines can
        # run, so the invariant says nothing about it.
        if first.startswith("unsupported") or first.startswith("compile") \
                or second.startswith("compile"):
            skipped += 1
            continue

        compared += 1
        # The engine name is part of each line and is expected to differ.
        left = first.replace("match pike", "match")
        right = second.replace("match backtrack", "match")
        if left != right:
            disagreements.append((flags, pattern, subject, first, second))

    for flags, pattern, subject, first, second in \
            disagreements[:args.examples]:
        print("/%s/%s on %s" % (pattern, flags, json.dumps(subject)))
        print("    pike:      %s" % first)
        print("    backtrack: %s" % second)

    print("\n%d rows, %d run on both engines, %d disagreements"
          % (len(rows), compared, len(disagreements)))
    if skipped:
        print("%d skipped: only one engine can run them" % skipped)
    return 1 if disagreements else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
