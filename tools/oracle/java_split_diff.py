#!/usr/bin/env python3
"""Compare Java split spans with the pinned JDK 25 and report JDK 27 drift."""

import argparse
from collections import Counter
import json
import os
import subprocess
import sys
import java_probe

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "tests", "data", "probe")
EXCLUSIONS = {("split", "", "😀", "", "split", "-1", "offset", "unpaired surrogate")}


def exclusion_identity(row, answer):
    return (row["area"], row["pattern"], row["subject"], row["flags"],
            row["mode"], row["argument"], answer["kind"], answer.get("message", ""))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", default=os.path.join(
        ROOT, "build", "linux", "release", "apps", "tools", "grx_split"))
    args = parser.parse_args()
    baseline, forward = [json.load(open(os.path.join(DATA, pin + ".json"),
                                        encoding="utf-8"))
                         for pin in ("java25", "java27")]
    if baseline["cases"] != forward["cases"]:
        raise SystemExit("JDK inventories differ")
    indices = [i for i, row in enumerate(baseline["cases"])
               if row["area"] in ("split", "split-limit-valid",
                                    "split-limit-invalid", "generated-split")]
    actual_exclusions = [exclusion_identity(baseline["cases"][i], baseline["answers"][i])
                         for i in indices if baseline["answers"][i]["kind"] == "offset"]
    stale = sum((Counter(EXCLUSIONS) - Counter(actual_exclusions)).values())
    if Counter(actual_exclusions) != Counter(EXCLUSIONS):
        raise SystemExit(f"stale Java split exclusion: {stale} missing; "
                         "unexpected or duplicate exclusions present")
    lines = ["\t".join([baseline["cases"][i]["flags"]] +
                       [baseline["cases"][i][key].encode("utf-8").hex()
                        for key in ("pattern", "subject")] +
                       [baseline["cases"][i]["argument"]]) for i in indices]
    result = subprocess.run([args.driver, "java"],
                            input="\n".join(lines) + "\n", text=True,
                            capture_output=True, check=True)
    answers = result.stdout.splitlines()
    if len(answers) != len(indices):
        raise SystemExit(f"{len(answers)} answers for {len(indices)} cases")
    passed = rejected = excluded = split_pair = unsupported = 0
    failures = []
    for i, line in zip(indices, answers):
        wanted = baseline["answers"][i]
        if wanted["kind"] == "input":
            if (baseline["cases"][i]["area"] == "split-limit-invalid"
                    and line == "error limit"):
                rejected += 1
            else:
                failures.append((i, line, wanted))
            continue
        if wanted["kind"] == "offset":
            excluded += 1
            split_pair += wanted.get("message") == "unpaired surrogate"
            continue
        if wanted["kind"] != "split":
            unsupported += 1
            failures.append((i, line, wanted))
            continue
        if not line.startswith("ok "):
            unsupported += line.startswith("compile ")
            failures.append((i, line, wanted))
            continue
        head, _, encoded = line.partition(" ")
        count_text, _, fields = encoded.partition(" ")
        count = int(count_text)
        pieces = ([] if count == 0 else
                  [bytes.fromhex(part).decode("utf-8") for part in fields.split("|")])
        if len(pieces) == count and pieces == wanted["pieces"]:
            passed += 1
        else:
            failures.append((i, pieces, wanted))
    changed = sum(baseline["answers"][i] != forward["answers"][i]
                  for i in indices)
    for i, actual, wanted in failures[:30]:
        print(f"case {i} {baseline['cases'][i]}: {actual} != {wanted}",
              file=sys.stderr)
    print(f"fixed inventory revision {java_probe.GENERATED_INVENTORY_REVISION}; JDK25 split: {len(indices)} total, {passed} pass, "
          f"{len(failures)} failure, {excluded} excluded "
          f"({split_pair} split-pair output), {rejected} invalid-limit "
          f"rejections, "
          f"{stale} stale exclusion, "
          f"{unsupported} unsupported, JDK27 differences {changed}")
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
