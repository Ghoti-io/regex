#!/usr/bin/env python3
"""Compare what this library matches against a reference implementation.

The syntax comparison (`syntax_diff.py`) asks whether a pattern is valid. This
one asks what it *does*: every group's span, for every pattern against every
subject, against the spans the reference reports.

Spans rather than substrings, because substrings hide the failures that
matter. `(a*)*` against "b" and `(a*)+` against "b" both match the empty
string; what distinguishes ECMAScript from Perl is whether group 1 is unset or
empty, and only the span says which.

A pattern whose program no implemented engine can run is skipped and counted,
not failed: a backreference has nowhere to run until work-packages.md WP-08, and
counting that as a disagreement would bury the real ones.

Usage:
    tools/oracle/match_diff.py [--seed N] [--patterns N] [--subjects N]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import json
import os
import random
import subprocess
import sys
import node_runner

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# Pattern fragments that combine into something a dialect will accept. Kept
# separate from the syntax fuzzer's vocabulary because the question is
# different: there, an invalid pattern is a test; here it is a wasted row.
# Non-ASCII is written as escapes and built at import, never as literal
# characters in this file. A source file that carries U+017F or U+2028 in it
# is a file whose diffs and terminals lie about what it says, and this suite
# has been bitten by exactly that once already.
LONG_S = chr(0x017F)
E_ACUTE = chr(0x00E9)
E_ACUTE_UPPER = chr(0x00C9)
ALPHA = chr(0x03B1)
LINE_SEPARATOR = chr(0x2028)
FISH = chr(0x1F41F)
GRIN = chr(0x1F600)

# Pattern fragments that combine into something a dialect will accept. Kept
# separate from the syntax fuzzer's vocabulary because the question is
# different: there, an invalid pattern is a test; here it is a wasted row.
ATOMS = [
    "a", "b", "c", "x", ".", "\\d", "\\w", "\\s", "\\D", "\\W", "\\S",
    "[abc]", "[^abc]", "[a-c]", "[\\d-]", "[^\\W]", "\\u0061", "\\x62",
    # The braced escape, which this vocabulary had in neither spelling
    # though `syntax_diff.py` has long asked whether both sides *accept*
    # it. Without `u` Annex B reads `\u{2}` as "u, twice" and `\u{1F41F}`
    # as literal text, with `u` they are a code point and an error - four
    # readings of two atoms, and the matching half of all four was
    # untested. A literal astral character is deliberately not added here:
    # that one *is* a deviation (section 6, the pattern source is code
    # units in ECMAScript) and would bury this gate in excluded rows.
    "\\u{2}", "\\u{1F41F}",
    "(a)", "(b|c)", "(?:ab)", "(a|)", "()", "(?<n>a)", "(a)(b)",
    "(a*)", "(a|b)*", "((a)|b)", "(?:(a)|b)", "(a?)", "(?:a|)",
    "(?=a)", "(?!a)", "(?<=a)", "(?<!a)", "(a)\\1", "(a|b)\\1",
    "(?=(a))", "(?<=(a))", "(?!(a)b)", "(?<=ab|c)", "(?=a*)", "(?<=[a-c]+)",
    "(?:(?=a)a)", "((a)|(b))\\2", "(?<n>a)\\k<n>",
    # A lookaround *inside* a lookbehind, which nothing here spelled until
    # a defect in that shape was found by a different dialect's work. The
    # body of a lookbehind is matched backwards; a lookaround written in it
    # still looks forwards from where it stands, and this library inherited
    # the direction instead - `(?<=a(?=b))b` against "ab" was no match, and
    # `(?<=a(?!c))b` was a match for the wrong reason, the negation holding
    # because its body looked for "c" behind the position.
    "(?<=a(?=b))", "(?<=(?=a)a)", "(?<=a(?!c))", "(?<!a(?=b))",
    "(?=(?<=a)b)", "(?<=(?<=a)b)", "(?<=a(?=b)b)",
    "\\p{L}", "\\p{Lu}", "\\P{L}", "\\p{Script=Greek}",
    E_ACUTE, LONG_S, "[" + chr(0x00E0) + "-" + chr(0x00FF) + "]",
]
QUANTIFIERS = ["", "", "", "*", "+", "?", "*?", "+?", "??", "{2}", "{1,2}",
               "{0,3}", "{2,}", "{2,}?", "{0,2}?"]
JOINERS = ["", "", "|"]
ANCHORS = ["", "", "", "^", "$", "\\b", "\\B"]

SUBJECT_PIECES = ["a", "b", "c", "x", "ab", "abc", "aab", "", " ", "\t", "\n",
                  "0", "9", "_", "A", "B", E_ACUTE, E_ACUTE_UPPER, LONG_S, "S",
                  ALPHA, "\r", LINE_SEPARATOR]

# Astral characters go only into subjects matched with the `u` flag. Without
# it, ECMAScript matches UTF-16 code units and this library matches code
# points, so `.` against an emoji gives 1 there and 2 here - a deviation
# recorded in documentation/dialects.md section 6, not a defect, and running
# it forty times per seed would bury the defects that are.
ASTRAL_PIECES = [FISH, GRIN]

FLAG_SETS = ("", "u", "i", "iu", "m", "s", "imsu", "v", "iv")

# The atoms that exist only with `v`. Mixed into a pattern only when the row
# is going to be run with `v`, because every one of them is a syntax error
# without it and a syntax error is a wasted row here.
#
# **No `\q{}` whose alternatives are one character long**, and that single
# exclusion is what is left of a wider one. Node 22.23 did not case-fold a
# class-set operand that was a bare character *or* a one-character `\q{}`, so
# `[a&&a]` under `iv` did not match "A" there while `[[a]&&a]` did - the same
# intersection written the other way round - and the generator avoided both
# shapes.
#
# **V8 13.6 fixed the bare-character half.** Re-probed 2026-09-26, the day
# after the pin moved: `[a&&[a]]`, `[a&&a]` and `[a--b]` all match "A" under
# `iv` now, agreeing with this library and with ECMA-262, so they are in the
# corpus. `[\q{a}]` over "A" still does not match there, which is the row the
# exclusion is now for, and `documentation/dialects.md` section 8.6.1 carries
# it alone. An exclusion whose reference has moved measures nothing and costs
# coverage: three shapes went untested for a release longer than they needed
# to.
SETS_ATOMS = [
    "[[a-c]--[b]]", "[[a-z]&&[b-d]]", "[\\q{ab|cd}]", "[\\q{}]",
    "[\\q{abc|ab|xy}]", "[[a-c][x-z]]", "[^[a-c]]", "[\\q{ab}[c]]",
    "\\p{RGI_Emoji}", "[\\p{Basic_Emoji}]", "[[\\w]--[a-c]]",
    "[[a-z]--[aeiou]--[xyz]]", "[[a]&&[a]]", "[[^a]&&[^b]]", "[\\q{aa}[a]]",
    # The three V8 13.6 fixed, which had never been asked.
    "[a&&[a]]", "[a&&a]", "[a--b]", "[[a]&&a]",
]


def make_pattern(rng, unicode_sets=False):
    atoms = ATOMS + (SETS_ATOMS if unicode_sets else [])
    parts = []
    for _ in range(rng.randint(1, 6)):
        parts.append(rng.choice(ANCHORS))
        parts.append(rng.choice(atoms) + rng.choice(QUANTIFIERS))
        parts.append(rng.choice(JOINERS))
    return "".join(parts)


def make_subject(rng, unicode_mode):
    pieces = SUBJECT_PIECES + (ASTRAL_PIECES if unicode_mode else [])
    return "".join(rng.choice(pieces) for _ in range(rng.randint(0, 6)))


def ask_node(rows):
    payload = json.dumps([[f, p, s] for f, p, s in rows])
    finished = subprocess.run(
        node_runner.command(os.path.join(HERE, "node_match.mjs")),
        input=payload, capture_output=True, text=True, check=True)
    return json.loads(finished.stdout)


def ask_library(driver, rows, engine):
    lines = "".join("%s\t%s\t%s\n" % (
        flags, pattern.encode("utf-8").hex(), subject.encode("utf-8").hex())
        for flags, pattern, subject in rows)
    command = [driver, "ecmascript"]
    if engine:
        command.append(engine)
    finished = subprocess.run(command, input=lines, capture_output=True,
        text=True, check=True)
    return finished.stdout.splitlines()


def parse_ours(line):
    """A driver line to the same shape the oracle's answers have."""
    if line == "nomatch":
        return None
    if line.startswith("match "):
        spans = []
        for field in line.split()[2:]:
            if field == "-":
                spans.append(None)
            else:
                start, end = field.split(":")
                spans.append([int(start), int(end)])
        return spans
    return line.split()[0]  # "unsupported", "compile", "error"


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--patterns", type=int, default=400)
    parser.add_argument("--subjects", type=int, default=12)
    parser.add_argument("--engine", default=None,
        help="force one engine instead of letting selection choose")
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
        # The flags are chosen first, because whether `v` is among them
        # decides which atoms the pattern may be built from.
        flags = rng.choice(FLAG_SETS)
        pattern = make_pattern(rng, "v" in flags)
        for _ in range(args.subjects):
            rows.append((flags, pattern,
                         make_subject(rng, "u" in flags or "v" in flags)))

    reference = ask_node(rows)
    ours = [parse_ours(line) for line in ask_library(driver, rows, args.engine)]
    if len(reference) != len(rows) or len(ours) != len(rows):
        sys.stderr.write("a driver did not answer every row\n")
        return 2

    skipped = {"unsupported": 0, "rejected by both": 0, "error": 0,
               "surrogate": 0}
    disagreements = []
    compared = 0

    for (flags, pattern, subject), expected, actual in zip(
            rows, reference, ours):
        # "unsupported" is a program no implemented engine can run, and
        # "surrogate" is a position UTF-8 does not have. Both are skipped.
        if actual == "unsupported":
            skipped["unsupported"] += 1
            continue
        if expected == "surrogate":
            skipped["surrogate"] += 1
            continue

        # A rejection is only a skip when *both* sides reject. One side
        # rejecting alone is a syntax disagreement, and it is reported here
        # rather than counted away - syntax_diff.py is the tool for finding
        # those, but this one must not hide them.
        ours_rejected = actual == "compile"
        reference_rejected = expected == "syntax"
        if ours_rejected and reference_rejected:
            skipped["rejected by both"] += 1
            continue
        if actual == "error":
            skipped["error"] += 1
            continue

        compared += 1
        if ours_rejected or reference_rejected or expected != actual:
            disagreements.append((flags, pattern, subject, expected, actual))

    for flags, pattern, subject, expected, actual in \
            disagreements[:args.examples]:
        print("/%s/%s on %s" % (pattern, flags, json.dumps(subject)))
        print("    reference: %s" % json.dumps(expected))
        print("    ours:      %s" % json.dumps(actual))

    print("\n%d rows, %d compared, %d disagreements" % (
        len(rows), compared, len(disagreements)))
    print("skipped: " + ", ".join(
        "%s %d" % (name, count) for name, count in sorted(skipped.items())
        if count))
    return 1 if disagreements else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
