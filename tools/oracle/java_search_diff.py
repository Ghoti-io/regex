#!/usr/bin/env python3
r"""Compare pinned Java search sequences and every capture on eligible engines.

Matcher.region bounds controls are counted as exclusions where the C search
window differs. Literal region matches()/lookingAt() use equivalent windows.
"""

import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "tests", "data", "probe")
ENGINES = ("auto", "pike", "backtrack", "bitstate", "dfa")
# The committed corpus has at least these eligible rows for each named
# engine. A regression that turns them all into "unsupported" must fail.
SUPPORT_FLOOR = {"pike": 27, "bitstate": 26}


def parse_spans(text):
    return [None if span == "-" else [int(v) for v in span.split(":")]
            for span in text.split(",")]


def parse(line, mode):
    if line == "unsupported":
        return {"kind": "unsupported"}
    if line == "nomatch":
        return {"kind": "nomatch"}
    if line.startswith("compile "):
        return {"kind": "syntax"}
    if line.startswith("match "):
        return {"kind": "match", "groups":
                parse_spans(" ".join(line.split()[2:]).replace(" ", ","))}
    if line.startswith("all ") and mode == "all":
        parts = line.split()
        if len(parts) != int(parts[1]) + 2:
            raise ValueError(f"bad all count: {line}")
        return {"kind": "all", "matches": [parse_spans(p) for p in parts[2:]]}
    return {"kind": "error", "detail": line}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", default=os.path.join(
        ROOT, "build", "linux", "release", "apps", "tools", "grx_match"))
    args = parser.parse_args()
    baseline, forward = [json.load(open(os.path.join(DATA, pin + ".json"),
                                        encoding="utf-8"))
                         for pin in ("java25", "java27")]
    if baseline["cases"] != forward["cases"]:
        raise SystemExit("JDK inventories differ")
    comparable_region = {"region-matches", "region-looking-at"}
    indices = [i for i, row in enumerate(baseline["cases"])
               if row["area"] in ("search", "iteration")
               or row["area"] == "region" and row["mode"] in comparable_region]
    region = sum(row["area"] == "region"
                 and row["mode"] not in comparable_region
                 for row in baseline["cases"])
    split_pair = [i for i in indices if baseline["answers"][i].get("kind")
                  == "offset"]
    indices = [i for i in indices if i not in split_pair]
    if not indices or not region:
        raise SystemExit("search or region probe corpus is empty")
    changed = sum(baseline["answers"][i] != forward["answers"][i]
                  for i in indices)
    region_changed = sum(baseline["answers"][i] != forward["answers"][i]
                         for i, row in enumerate(baseline["cases"])
                         if row["area"] == "region")
    failures = []
    for engine in ENGINES:
        passed = unsupported = 0
        for mode in ("first", "all", "all-from", "find-from", "matches",
                     "looking-at", "region-matches", "region-looking-at"):
            selected = [i for i in indices if baseline["cases"][i]["mode"] == mode]
            if not selected:
                continue
            driver_mode = mode.removeprefix("region-")
            if driver_mode == "all-from":
                driver_mode = "all"
            if driver_mode == "find-from":
                driver_mode = "first"
            payload = "".join("%s\t%s\t%s%s\n" % (
                row["flags"], row["pattern"].encode().hex(),
                row["subject"].encode().hex(),
                ("\t%s,-," % row["argument"]
                 if mode in ("find-from", "all-from") else
                 "\t%s,%s," % tuple(row["argument"].split(",")[:2])
                 if mode in comparable_region else ""))
                for i in selected for row in [baseline["cases"][i]])
            command = [args.driver, "java", engine, driver_mode]
            result = subprocess.run(command, input=payload, text=True,
                                    capture_output=True, check=True)
            lines = result.stdout.splitlines()
            if len(lines) != len(selected):
                raise SystemExit(f"{engine}/{mode}: {len(lines)} answers for "
                                 f"{len(selected)} cases")
            for index, line in zip(selected, lines):
                actual = parse(line, driver_mode)
                wanted = baseline["answers"][index]
                if actual["kind"] == "unsupported" and engine != "auto":
                    unsupported += 1
                elif actual == wanted:
                    passed += 1
                else:
                    failures.append((engine, index, actual, wanted))
        if engine == "backtrack" and passed != len(indices):
            failures.append((engine, "support", passed, len(indices)))
        elif engine in SUPPORT_FLOOR and passed < SUPPORT_FLOOR[engine]:
            failures.append((engine, "support", passed, SUPPORT_FLOOR[engine]))
        print(f"{engine}: {passed} pass, {unsupported} unsupported, "
              f"{len(indices) - passed - unsupported} mismatch")
    for engine, index, actual, wanted in failures[:30]:
        row = baseline["cases"][index] if isinstance(index, int) else index
        print(f"{engine} case {index} {row}: {actual} != {wanted}",
              file=sys.stderr)
    print(f"JDK25 comparable search rows {len(indices)}, "
          f"region/bounds exclusions {region}, split-pair exclusions "
          f"{len(split_pair)}, "
          f"JDK27 search differences {changed}, region differences "
          f"{region_changed}; DFA requires leftmost-longest and is "
          "ineligible for Java's leftmost-first program")
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
