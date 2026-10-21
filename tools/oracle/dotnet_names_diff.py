#!/usr/bin/env python3
r"""Compare pinned .NET group numbering and names against this library.

The gap `dotnet_coverage.py` named and would not let be forgotten: ten rows
asking which numbers and names a pattern's groups have, compared by nothing,
because no driver reported them. .NET's numbering is the part of this dialect
most likely to be wrong and the part a span differential cannot see - every
group can hold the right text under the wrong number and every span still
match. Until this existed it rested on `tests/unit/test_dotnet.cpp` alone,
whose expectations and the implementation were written by the same hand from
the same reading.

Two conventions meet here, and reconciling them in one place is the point of
doing it here rather than in the driver. **The reference names every group**,
an unnamed one by its own number as a string, so `(a)` reports group 1 called
"1". **This library reports that a group has no name**, which is what it
knows. A reference pair whose name equals its number is therefore an unnamed
group, and any other is a named one.

**Three spellings are excluded and named**, all of them .NET numbering this
library refuses rather than approximates:

  * an explicit number, `(?<5>a)(b)`, which makes the numbering **sparse** -
    the reference reports 0, 1, 5 while `Groups.Count` is 3;
  * an explicit number that **collides** with an unnamed group's,
    `(a)(?<1>b)`, which the reference *merges* into one group holding "b";
  * `(?<2>a)(b)`, the same collision reached the other way round.

Each is `GRX_DIAG_INVALID_GROUP_NAME` here. A partition cannot express a
sparse or merged numbering, and guessing would be worse than refusing; the
rows stay in the inventory so the refusal is visible, and the exclusion is
checked in both directions.
"""
import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dotnet_probe
import oracle_env

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "tests", "data", "probe")

# Keyed by pattern, so a fourth spelling going the same way cannot join them
# silently.
EXCLUDED = {
    "(?<5>a)(b)":
        "an explicit group number, which makes .NET's numbering sparse: it "
        "reports 0, 1 and 5 where a partition has no way to leave a gap. "
        "GRX_DIAG_INVALID_GROUP_NAME here",
    "(a)(?<1>b)":
        "an explicit number colliding with an unnamed group's, which .NET "
        "merges into one group holding \"b\". A merge is not a partition "
        "either. GRX_DIAG_INVALID_GROUP_NAME here",
    "(?<2>a)(b)":
        "the same collision reached the other way round, and listed "
        "separately because the exclusion is keyed by pattern",
}

# Letters this comparison cannot express; their rows are excluded by letter.
# `E` is not one: RegexOptions.ECMAScript does not move the numbering, which
# is itself one of the measurements worth keeping (probe cases 366 and 367
# answer alike), so those rows are compared.
UNSUPPORTED_LETTERS = {
    "R": "RegexOptions.RightToLeft: this library matches forwards only",
}
DROPPED_LETTERS = {"C": "no locale here; folding is always invariant"}
ECMASCRIPT_OPTIONS = (
    "ascii-digit,ascii-space,ascii-word,prefix-backreference")


def wanted_shape(answer):
    """The reference's answer as (numbers, {number: name or None})."""
    numbers = list(answer["numbers"])
    names = {}
    for number, name in answer["names"]:
        names[number] = None if name == str(number) else name
    return numbers, names


def actual_shape(line):
    fields = line.split()
    if fields[0] != "names":
        return None, None
    numbers, names = [], {}
    for field in fields[2:]:
        number, _, name = field.partition(":")
        numbers.append(int(number))
        names[int(number)] = None if name == "-" else name
    return numbers, names


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", default=os.path.join(
        ROOT, "build", "linux", "release", "apps", "tools", "grx_match"))
    args = parser.parse_args()

    with open(os.path.join(DATA, "dotnet.json"), encoding="utf-8") as source:
        pinned = json.load(source)
    if pinned["cases"] != dotnet_probe.CASES:
        raise SystemExit("the inventory has changed; re-record the probe")

    groups = {}
    excluded_letter = 0
    for index, row in enumerate(pinned["cases"]):
        if row["mode"] != "names":
            continue
        if pinned["answers"][index].get("kind") != "names":
            continue
        if any(c in UNSUPPORTED_LETTERS for c in row["flags"]):
            excluded_letter += 1
            continue
        letters = "".join(c for c in row["flags"]
                          if c not in DROPPED_LETTERS and c != "E")
        key = ECMASCRIPT_OPTIONS if "E" in row["flags"] else ""
        groups.setdefault(key, []).append((index, letters))
    if not groups:
        raise SystemExit("no .NET naming rows to compare")

    passed = excluded = stale = 0
    failures = []
    for options, rows in sorted(groups.items()):
        payload = "".join(
            "%s\t%s\t\n" % (letters,
                            pinned["cases"][i]["pattern"].encode().hex())
            for i, letters in rows)
        command = [args.driver, "dotnet", "auto", "names"]
        if options:
            command.append("--options=" + options)
        finished = subprocess.run(command, input=payload, text=True,
                                  capture_output=True, check=True)
        lines = finished.stdout.splitlines()
        if len(lines) != len(rows):
            raise SystemExit("%d answers for %d rows" % (len(lines),
                                                         len(rows)))
        for (index, _), line in zip(rows, lines):
            pattern = pinned["cases"][index]["pattern"]
            want = wanted_shape(pinned["answers"][index])
            got = actual_shape(line)
            if pattern in EXCLUDED:
                # Both directions: the exclusion claims this library refuses
                # the pattern. If it ever compiles one, or the reference ever
                # refuses it, the exclusion has outlived its reason.
                if got[0] is not None:
                    stale += 1
                    failures.append((index, got, want,
                                     "exclusion is stale, the pattern "
                                     "compiled: " + EXCLUDED[pattern]))
                else:
                    excluded += 1
                continue
            if got[0] is None:
                failures.append((index, got, want,
                                 "this library refused the pattern"))
            elif got == want:
                passed += 1
            else:
                failures.append((index, got, want, ""))

    for index, got, want, note in failures:
        print("case %d %r: numbers %s names %s, the reference says numbers %s "
              "names %s%s"
              % (index, pinned["cases"][index]["pattern"],
                 json.dumps(got[0]), json.dumps(got[1]),
                 json.dumps(want[0]), json.dumps(want[1]),
                 "; " + note if note else ""), file=sys.stderr)
    print("inventory %s; .NET naming: %d asked, %d agree, %d excluded and "
          "named, %d excluded by letter, %d stale exclusion, %d disagreement"
          % (dotnet_probe.DOTNET_INVENTORY_REVISION,
             sum(len(v) for v in groups.values()), passed, excluded,
             excluded_letter, stale, len(failures)))
    return bool(failures)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, oracle_env.OracleUnavailable) as error:
        sys.stderr.write("dotnet_names_diff: %s\n" % error)
        sys.exit(1)
