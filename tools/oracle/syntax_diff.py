#!/usr/bin/env python3
"""Compare this library's accept/reject against a reference implementation.

The cheapest strong test the front end has. A parser can be read against a
grammar and still be wrong, because the interesting rules are the ones that
hang off a production parameter three pages away - Annex B's
`SourceCharacterIdentityEscape[+N]`, which makes `[\\k]` a syntax error in a
pattern that names a group *anywhere*, was found here and not by reading.

Two corpora, because they fail differently:

- **Exhaustive**, over every string of up to three characters drawn from an
  alphabet of the dialect's punctuation. This finds the corners: a two-
  character pattern is where a lexer's lookahead is wrong.
- **Random**, from a token soup, up to fourteen tokens. This finds the
  interactions: a rule that is right alone and wrong after a named group.

A pattern this library refuses for a *limit* is not counted either way. Its
syntax was never read, so the comparison has nothing to say about it, and
counting it as accepted would hide a real disagreement behind a cap.

Usage:
    tools/oracle/syntax_diff.py [--seed N] [--count N] [--driver PATH]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import itertools
import json
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# The punctuation the exhaustive corpus is drawn from: every character that
# means something to the ECMAScript grammar, plus the few letters that only
# mean something after a backslash.
ALPHABET = list("ab()[]{}|*+?.^$\\-,0129dwsupPkcxq<>=!:&")

# The random corpus's vocabulary. Deliberately includes constructs ECMAScript
# does *not* have - `(?>`, `(?#`, `(?P<` - because "we reject what the
# reference rejects" is half of conformance.
TOKENS = [
    "a", "b", "(", ")", "(?:", "(?=", "(?!", "(?<=", "(?<!", "(?<n>",
    "[", "]", "[^", "|", "*", "+", "?", "??", "*?", "{2}", "{2,}", "{2,3}",
    "{", "}", "\\d", "\\w", "\\s", "\\b", "\\B", "\\1", "\\2", "\\k<n>",
    "\\p{L}", "\\p{Lu}", "\\P{L}", "\\u0041", "\\u{41}", "\\x41", "\\cA",
    "\\0", "\\01", "\\8", "\\-", "\\/", "-", ".", "^", "$", "\\", "\\\\",
    ":", "=", "!", "<", ">", "&&", "--", "é", "ſ", "\U0001f41f",
    "\\q{ab}", "(?P<n>", "(?'n'", "(?#", "(?>", "(?i:", "(?(", "\\A", "\\z",
    "\\Z", "\\G", "\\K", "\\Q", "\\E", "[[:alpha:]]", "\\g{1}", "\\9",
    "\\377", "\\400", "{0,}", "{,3}", "{1", "\\u{110000}", "\\uD83D",
    "\\uDC1F", "\\k", "\\p{Script=Greek}", "\\p{Nosuch}", "\\p{", "(?<n>a)",
    # UnicodeSets mode's own vocabulary. `&&` and `--` are already above,
    # because they are two ordinary characters without `v` and an operator
    # with it - which is exactly the kind of row this harness is for.
    "\\q{}", "\\q{a|bc}", "\\q{|}", "[\\q{ab}]", "\\p{RGI_Emoji}",
    "\\p{Basic_Emoji}", "\\P{RGI_Emoji}", "[a-z]", "[^a]", "!!", "~~",
    "^^", "\\&", "\\!", "\\-", "[[a][b]]", "&", "##", "$$", "::",
]

FLAG_SETS = ("", "u", "i", "iu", "m", "s", "v", "iv")


def corpus(seed, count):
    patterns = set()

    for length in (1, 2, 3):
        for combination in itertools.product(ALPHABET, repeat=length):
            patterns.add("".join(combination))

    rng = random.Random(seed)
    while len(patterns) < count:
        patterns.add("".join(
            rng.choice(TOKENS) for _ in range(rng.randint(1, 14))))

    return sorted(patterns)


def ask_node(rows):
    payload = json.dumps([[flags, pattern] for flags, pattern in rows])
    finished = subprocess.run(
        ["node", os.path.join(HERE, "node_syntax.mjs")],
        input=payload, capture_output=True, text=True, check=True)
    return json.loads(finished.stdout)


def ask_library(driver, rows):
    lines = "".join("%s\t%s\n" % (flags, pattern.encode("utf-8").hex())
                    for flags, pattern in rows)
    finished = subprocess.run([driver, "ecmascript"], input=lines,
        capture_output=True, text=True, check=True)
    return finished.stdout.splitlines()


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--count", type=int, default=120000,
        help="patterns before the flag sets multiply them")
    parser.add_argument("--driver", default=None,
        help="path to the grx_syntax tool")
    parser.add_argument("--examples", type=int, default=4)
    args = parser.parse_args(argv[1:])

    driver = args.driver
    if not driver:
        for candidate in ("linux", "mac", "win64", "win32"):
            for build in ("release", "debug"):
                path = os.path.join(ROOT, "build", candidate, build, "apps",
                    "tools", "grx_syntax")
                if os.path.exists(path):
                    driver = path
                    break
            if driver:
                break
    if not driver or not os.path.exists(driver):
        sys.stderr.write(
            "the grx_syntax tool was not found; run `make tools` first\n")
        return 2

    patterns = corpus(args.seed, args.count)
    rows = [(flags, pattern) for pattern in patterns for flags in FLAG_SETS]

    reference = ask_node(rows)
    ours = ask_library(driver, rows)
    if len(reference) != len(rows) or len(ours) != len(rows):
        sys.stderr.write("a driver did not answer every pattern\n")
        return 2

    disagreements = {}
    capped = 0
    for (flags, pattern), expected, verdict in zip(rows, reference, ours):
        # The driver refuses a pattern longer than its buffer rather than
        # parsing the prefix. That must stop the gate rather than count as a
        # rejection: a comparison against what Node said about the whole
        # pattern is not a comparison this run made.
        if verdict == "toolong":
            sys.stderr.write(
                "the driver could not hold a pattern this run generated; "
                "raise MAX_PATTERN in tools/oracle/grx_syntax.c\n")
            return 2
        if verdict.startswith("limit"):
            capped += 1
            continue
        accepted = verdict == "ok"
        if accepted != bool(expected):
            key = (verdict if not accepted else "accepted", bool(expected),
                   flags)
            disagreements.setdefault(key, []).append(pattern)

    total = 0
    for key in sorted(disagreements, key=lambda k: -len(disagreements[k])):
        examples = disagreements[key]
        total += len(examples)
        direction = ("we reject, the reference accepts" if key[1]
                     else "we accept, the reference rejects")
        print("%-26s /%-2s %-32s %6d  e.g. %s" % (
            key[0], key[2], direction, len(examples),
            "  ".join(json.dumps(e) for e in examples[:args.examples])))

    print("\n%d patterns x %d flag sets = %d cases; %d capped by a limit and "
          "not compared; %d disagreements"
          % (len(patterns), len(FLAG_SETS), len(rows), capped, total))
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
