#!/usr/bin/env python3
"""Check the seven properties of *strings* against the reference.

property_diff.py walks every code point and compares membership for the 454
character properties. It cannot do the same here, because a property of
strings has no universe to walk: its members are sequences, and there are
more sequences than there are atoms.

So this walks a universe that is small and is exactly the one that matters.
`emoji-test.txt` lists every emoji sequence UTS #51 knows about, including
the minimally-qualified and unqualified spellings that are deliberately *not*
RGI - which is what makes it a two-sided test rather than a spot check. If
this library says a sequence is in RGI_Emoji and Node says it is not, or the
other way round, that is a disagreement and the run fails.

Usage:
    python3 tools/oracle/string_property_diff.py [--ucd DIR]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import json
import os
import subprocess
import sys
import node_runner

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# Every property of strings ECMAScript's `v` mode defines
# (documentation/unicode.md section 3).
PROPERTIES = [
    "RGI_Emoji",
    "Basic_Emoji",
    "Emoji_Keycap_Sequence",
    "RGI_Emoji_Flag_Sequence",
    "RGI_Emoji_Modifier_Sequence",
    "RGI_Emoji_Tag_Sequence",
    "RGI_Emoji_ZWJ_Sequence",
]


def read_universe(path):
    """Every sequence in emoji-test.txt, qualified or not."""
    sequences = []
    with open(path, "r", encoding="utf-8") as handle:
        for line in handle:
            body = line.split("#")[0].strip()
            if not body:
                continue
            fields = [field.strip() for field in body.split(";")]
            if len(fields) < 2:
                continue
            sequences.append(
                "".join(chr(int(code, 16)) for code in fields[0].split()))
    return sequences


def ask_node(rows):
    payload = json.dumps([[f, p, s] for f, p, s in rows])
    finished = subprocess.run(
        node_runner.command(os.path.join(HERE, "node_match.mjs")),
        input=payload, capture_output=True, text=True, check=True)
    return json.loads(finished.stdout)


def ask_library(driver, rows):
    lines = "".join("%s\t%s\t%s\n" % (
        flags, pattern.encode("utf-8").hex(), subject.encode("utf-8").hex())
        for flags, pattern, subject in rows)
    finished = subprocess.run([driver, "ecmascript"], input=lines,
        capture_output=True, text=True, check=True)
    return finished.stdout.splitlines()


def find_driver():
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(
                ROOT, "build", platform, build, "apps", "tools", "grx_match")
            if os.path.exists(path):
                return path
    return None


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ucd", default=None)
    args = parser.parse_args(argv[1:])

    with open(os.path.join(ROOT, "tools", "unicode", "UCD_VERSION"),
              "r", encoding="utf-8") as handle:
        version = handle.read().strip()
    ucd = args.ucd or os.path.join(ROOT, "third_party", "ucd", version)
    universe_path = os.path.join(ucd, "emoji-test.txt")
    if not os.path.exists(universe_path):
        sys.stderr.write(
            "emoji-test.txt is not in %s; run tools/unicode/fetch.sh\n" % ucd)
        return 2

    driver = find_driver()
    if not driver:
        sys.stderr.write(
            "the grx_match tool was not found; run `make tools` first\n")
        return 2

    universe = read_universe(universe_path)
    rows = []
    for name in PROPERTIES:
        # Anchored at both ends, so "matched" means "is a member" rather than
        # "starts with a member" - `\p{RGI_Emoji}` against a ZWJ sequence
        # would otherwise match its first character and prove nothing.
        pattern = "^\\p{%s}$" % name
        for sequence in universe:
            rows.append(("v", pattern, sequence))

    reference = ask_node(rows)
    ours = ask_library(driver, rows)
    if len(reference) != len(rows) or len(ours) != len(rows):
        sys.stderr.write("a driver did not answer every row\n")
        return 2

    disagreements = 0
    shown = 0
    for (flags, pattern, sequence), expected, line in zip(
            rows, reference, ours):
        theirs = expected is not None and not isinstance(expected, str)
        mine = line.startswith("match ")
        if theirs == mine:
            continue
        disagreements += 1
        if shown < 12:
            shown += 1
            sys.stderr.write("%-30s %-28s reference=%s ours=%s\n" % (
                pattern, " ".join("%04X" % ord(c) for c in sequence),
                "member" if theirs else "not", "member" if mine else "not"))

    sys.stderr.write(
        "\n%d sequences x %d properties = %d cases; %d disagreements\n" % (
            len(universe), len(PROPERTIES), len(rows), disagreements))
    return 1 if disagreements else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
