#!/usr/bin/env python3
"""Compare focused Java flag/property spans with separate pinned JDK records.

The first-search rows in java_probe.py are the corpus. LITERAL and CANON_EQ
belong to other stories and are counted as exclusions, never as agreements.
"""

import argparse
from collections import Counter
from itertools import product
import json
import os
import subprocess
import sys
import java_probe

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "tests", "data", "probe")
# Every first-search area, with a floor below it rather than a hand-written
# list. A named list scores the rows that exist in it, not the rows that
# should: `java_syntax_diff.py` read one area and asked 9 of 624 rows, and
# `escape-semantics` would have been invisible here the moment it was added
# for exactly that reason. The floor is what makes a silent narrowing fail.
# **Every first-search row, not a named list of areas.** A named list scores
# the rows that exist in it, not the rows that should: `java_syntax_diff.py`
# read one area and so asked 9 of 624 rows, and the five areas named here
# reached 109 of the 370 first-search rows the inventory holds. Worse, the
# list is silent about its own omissions - `escape-semantics` would have been
# invisible the moment it was added, which is how the gap it exists to close
# was created in the first place.
# Rows the corpus must keep reaching, so that a later edit to the predicate or the
# exclusions cannot quietly shrink it back.
MIN_ROWS = 250
ENGINES = ("auto", "pike", "backtrack", "bitstate", "dfa")
EXCLUDED = {("a", "A", "l"), ("é", "e\u0301", "c"), ("a+", "a+", "l")}
GENERATED_PATTERNS = ("(a|b)+", "(?i:a)b", "\\d+", "[a&&[a-z]]",
                      "(?<=a)b", "(?=(a))a")
GENERATED_SUBJECTS = ("ab", "aab", "b", "1", "é")
GENERATED = set(product(GENERATED_PATTERNS, GENERATED_SUBJECTS, ("",),
                        ("first",), ("",)))
GENERATED_ELIGIBLE = {
    "auto": GENERATED,
    "backtrack": GENERATED,
    "pike": set(product(GENERATED_PATTERNS[:4], GENERATED_SUBJECTS, ("",),
                        ("first",), ("",))),
    "bitstate": set(product(GENERATED_PATTERNS[:4], GENERATED_SUBJECTS, ("",),
                            ("first",), ("",))),
    "dfa": set(),
}


def identity(row):
    return row["pattern"], row["subject"], row["flags"], row["mode"], row["argument"]


def exclusion_identity(row):
    return row["pattern"], row["subject"], row["flags"]


def excluded(row):
    """Only the measured LITERAL/CANON_EQ probes lack a C flag mapping."""
    return (row["pattern"], row["subject"], row["flags"]) in EXCLUDED


def parse(line):
    if line == "nomatch":
        return {"kind": "nomatch"}
    fields = line.split()
    if fields and fields[0] == "match":
        groups = []
        for span in fields[2:]:
            groups.append(None if span == "-" else [int(x) for x in span.split(":")])
        return {"kind": "match", "groups": groups}
    if fields and fields[0] == "compile":
        return {"kind": "syntax"}
    return {"kind": fields[0] if fields else "empty"}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", default=os.path.join(
        ROOT, "build", "linux", "release", "apps", "tools", "grx_match"))
    args = parser.parse_args()
    records = [json.load(open(os.path.join(DATA, pin + ".json"), encoding="utf-8"))
               for pin in ("java25", "java27")]
    baseline, forward = records
    if baseline["cases"] != forward["cases"]:
        raise SystemExit("JDK 25 and 27 case inventories differ")

    indices = [i for i, row in enumerate(baseline["cases"])
               if row["mode"] == "first"]
    excluded_indices = [i for i in indices if excluded(baseline["cases"][i])]
    if Counter(exclusion_identity(baseline["cases"][i]) for i in excluded_indices) != Counter(EXCLUDED):
        raise SystemExit("stale Java LITERAL/CANON_EQ exclusion")
    generated = [identity(row) for row in baseline["cases"]
                 if row["area"] == "generated-match"]
    if Counter(generated) != Counter(GENERATED):
        raise SystemExit("generated Java match inventory changed; update eligibility")
    indices = [i for i in indices if i not in excluded_indices]
    if len(indices) < MIN_ROWS:
        raise SystemExit(
            "Java match corpus is %d rows, below the floor of %d. A gate "
            "scores the rows it asks, so raise the floor when the inventory "
            "grows and never lower it to make a narrowing pass."
            % (len(indices), MIN_ROWS))
    changed = [i for i in indices
               if baseline["answers"][i] != forward["answers"][i]]
    payload = "".join("%s\t%s\t%s\n" % (
        row["flags"], row["pattern"].encode().hex(), row["subject"].encode().hex())
        for i in indices for row in [baseline["cases"][i]])
    failures = []
    counts = {}
    for engine in ENGINES:
        command = [args.driver, "java"]
        if engine != "auto":
            command.append(engine)
        result = subprocess.run(command, input=payload, text=True,
                                capture_output=True, check=True)
        answers = result.stdout.splitlines()
        if len(answers) != len(indices):
            raise SystemExit(f"{engine}: {len(answers)} answers for {len(indices)} rows")
        passed = unsupported = 0
        for index, line in zip(indices, answers):
            actual = parse(line)
            wanted = baseline["answers"][index]
            row = baseline["cases"][index]
            if row["area"] == "generated-match":
                eligible = identity(row) in GENERATED_ELIGIBLE[engine]
                if (actual["kind"] != "unsupported") != eligible:
                    failures.append((engine, index, actual, wanted))
                    continue
            if engine == "auto" and actual["kind"] not in ("match", "nomatch", "syntax"):
                failures.append((engine, index, actual, wanted))
                continue
            if actual["kind"] == "unsupported":
                unsupported += 1
                continue
            if wanted["kind"] == "syntax":
                wanted = {"kind": "syntax"}
            if actual != wanted:
                failures.append((engine, index, actual, wanted))
            else:
                passed += 1
        counts[engine] = (passed, unsupported)
    for engine, index, actual, wanted in failures[:20]:
        print(f"{engine} case {index} {baseline['cases'][index]}: {actual} != {wanted}",
              file=sys.stderr)
    print(f"fixed inventory revision {java_probe.GENERATED_INVENTORY_REVISION}; JDK25 match: {len(indices) + len(excluded_indices)} "
          f"total, {len(excluded_indices)} excluded (LITERAL/CANON_EQ), "
          f"0 stale exclusion, JDK27 differences {len(changed)}")
    for engine, (passed, unsupported) in counts.items():
        print(f"{engine}: {passed} pass, {unsupported} unsupported, "
              f"{len(indices) - passed - unsupported} mismatch")
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
