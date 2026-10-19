#!/usr/bin/env python3
"""Compare focused Java flag/property spans with separate pinned JDK records.

The first-search rows in java_probe.py are the corpus. LITERAL and CANON_EQ
belong to other stories and are counted as exclusions, never as agreements.
"""

import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "tests", "data", "probe")
AREAS = {"flags", "lines", "unicode", "unicode-sensitive"}
ENGINES = ("auto", "pike", "backtrack", "bitstate", "dfa")


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
               if row["mode"] == "first" and row["area"] in AREAS]
    excluded = [i for i in indices if any(c in baseline["cases"][i]["flags"]
                                          for c in "lc")]
    indices = [i for i in indices if i not in excluded]
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
    print(f"JDK25 focused rows {len(indices)}, exclusions {len(excluded)} "
          "(LITERAL/CANON_EQ), JDK27 differences " + str(len(changed)))
    for engine, (passed, unsupported) in counts.items():
        print(f"{engine}: {passed} pass, {unsupported} unsupported, "
              f"{len(indices) - passed - unsupported} mismatch")
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
