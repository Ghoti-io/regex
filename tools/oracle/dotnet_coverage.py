#!/usr/bin/env python3
r"""Every pinned .NET row is asked by some gate, or is named as not asked.

Written because two holes were found by accident rather than by looking.
`dotnet_match_diff.py` selects on `not row["flags"]`, so every flagged row
was compared by nothing until `dotnet_flags_diff.py` existed; and the
`search_start` discriminator written for the profile audit landed in `all`
mode, which no gate reads at all - so it was recorded, it agreed, and it
proved nothing.

A row in `tests/data/probe/dotnet.json` that no gate consumes is worse than
a row that is missing. A missing row is a gap somebody can see in the
inventory; a recorded row nobody asks is a measurement that *looks* like
coverage. The count at the end of a differential is only meaningful against
a denominator, and this file is the denominator.

The rule: each row is claimed by the gate whose selection predicate matches
it, the predicates here mirroring the ones in those files, and a row no
predicate claims must appear in UNASKED with a reason. The mirroring is the
weak point and is deliberate - a predicate that drifts from its gate shows
up here as a row claimed by a gate that does not run it, which the gate's
own count then contradicts.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dotnet_probe

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "tests", "data", "probe")

# One row, and the same shape of gap: `dotnet_replace_diff.py` asks
# `replace-all` because every other replacement row is one, and this single
# `replace-first` row would need the gate to drive a second driver mode.
REPLACE_FIRST_REASON = (
    "the one `replace-first` row; the replacement gate drives `replace-all` "
    "only, and one row did not seem worth a second mode until something "
    "else needs it")

MATCH_AREAS = ("control", "generated-match", "classes", "flags", "captures",
               "anchors", "lookbehind", "profile", "profile-audit",
               "ecmascript", "iteration", "escape-semantics",
               "quantified-anchor")
REPLACE_AREAS = ("template", "generated-replacement")


def claimant(row, answer):
    """The gate that asks this row, or None."""
    area, mode, flags = row["area"], row["mode"], row["flags"]
    matchable = answer.get("kind") in ("match", "nomatch")
    if mode == "first" and flags and matchable:
        return "flags"
    if mode == "first" and area in MATCH_AREAS and matchable:
        return "match"
    # A `first`-mode row the reference refused has no match to compare, so
    # it falls through to the syntax gate rather than being unclaimed. This
    # branch used to `return None` here and three such rows read as holes
    # while the syntax gate was asking all three - the mirror being wrong in
    # the direction that invents work rather than hides it, which is the
    # better direction but still wrong.
    if mode == "compile" and flags:
        return "flags"
    if not flags and (mode == "compile"
                      or (mode == "first"
                          and answer.get("kind") == "syntax")):
        return "syntax"
    if mode == "replace-all" and area in REPLACE_AREAS:
        return "replace"
    if mode in ("split", "split-count"):
        return "split"
    if mode in ("all", "all-from", "find-from") and not flags:
        return "search"
    if mode == "names":
        return "names"
    if mode == "w:first":
        return "wtf8"
    return None


# Rows no gate asks, each with the reason. Keyed by (area, mode) where the
# whole group is unasked and by (area, mode, pattern) where it is not, so
# that widening one is a visible edit.
UNASKED = {
    ("template", "replace-first"): REPLACE_FIRST_REASON,
    ("unit",):
        "WP-49's inventory, in every mode it uses. .NET matches over UTF-16 "
        "code units and reports boundaries inside a character; these rows "
        "answer `offset` from the driver, which is the datum. They are "
        "documentation/dialects.md section 6's deviation, not a "
        "disagreement a gate should score. The whole area is named, "
        "because the deviation is the area's subject and not a property of "
        "one mode",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()

    with open(os.path.join(DATA, "dotnet.json"), encoding="utf-8") as source:
        pinned = json.load(source)
    if pinned["cases"] != dotnet_probe.CASES:
        raise SystemExit("the inventory has changed; re-record the probe")

    counts = {}
    unasked = {}
    orphans = []
    for index, row in enumerate(pinned["cases"]):
        gate = claimant(row, pinned["answers"][index])
        if gate:
            counts[gate] = counts.get(gate, 0) + 1
            continue
        keys = [(row["area"], row["mode"], row["pattern"]),
                (row["area"], row["mode"]), (row["area"],)]
        named = next((k for k in keys if k in UNASKED), None)
        if named:
            unasked[named] = unasked.get(named, 0) + 1
            continue
        orphans.append((index, row))

    for index, row in orphans:
        print("case %d area %r mode %r pattern %r: no gate asks it and "
              "UNASKED does not name it"
              % (index, row["area"], row["mode"], row["pattern"]),
              file=sys.stderr)

    # An UNASKED key that matches nothing is the other failure: a reason kept
    # for rows that have gone.
    stale = [key for key in UNASKED if key not in unasked]
    for key in stale:
        print("UNASKED names %r, which matches no row" % (key,),
              file=sys.stderr)

    print("inventory %s; .NET coverage: %d rows, %s, %d named as not asked, "
          "%d asked by nothing, %d stale reason"
          % (dotnet_probe.DOTNET_INVENTORY_REVISION, len(pinned["cases"]),
             ", ".join("%s %d" % (g, counts[g]) for g in sorted(counts)),
             sum(unasked.values()), len(orphans), len(stale)))
    return bool(orphans) or bool(stale)


if __name__ == "__main__":
    sys.exit(main())
