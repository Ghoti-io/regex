#!/usr/bin/env python3
"""Compare Java replacement probes with the pinned JDK 25 answers.

JDK replacement exceptions expose a class and message, but no template
index. Those offsets are therefore checked in the C unit suite, not silently
counted as oracle agreements here.
"""

import argparse
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "tests", "data", "probe")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", default=os.path.join(
        ROOT, "build", "linux", "release", "apps", "tools", "grx_replace"))
    args = parser.parse_args()
    baseline, forward = [json.load(open(os.path.join(DATA, pin + ".json"),
                                        encoding="utf-8"))
                         for pin in ("java25", "java27")]
    if baseline["cases"] != forward["cases"]:
        raise SystemExit("JDK inventories differ")
    indices = [i for i, row in enumerate(baseline["cases"])
               if row["area"] == "replacement"]
    lines = []
    for i in indices:
        row = baseline["cases"][i]
        mode = row["mode"].removeprefix("replace-")
        if mode.endswith("-literal"):
            mode = mode.removesuffix("-literal") + "-literal"
        lines.append("\t".join([mode, row["flags"]] +
                      [row[k].encode("utf-8").hex()
                       for k in ("pattern", "subject", "argument")]))
    result = subprocess.run([args.driver, "java"], input="\n".join(lines) + "\n",
                            capture_output=True, text=True, check=True)
    answers = result.stdout.splitlines()
    if len(answers) != len(indices):
        raise SystemExit(f"{len(answers)} answers for {len(indices)} cases")
    passed = unsupported = excluded = split_pair = errors_without_oracle_offset = 0
    failures = []
    for i, line in zip(indices, answers):
        wanted = baseline["answers"][i]
        if wanted["kind"] in ("offset", "input"):
            excluded += 1
            if wanted.get("message") == "unpaired surrogate":
                split_pair += 1
            continue
        if line.startswith("compile "):
            unsupported += 1
            failures.append((i, line, wanted))
        elif wanted["kind"] == "replace" and line.startswith("ok "):
            actual = bytes.fromhex(line[3:]).decode("utf-8")
            if actual == wanted["text"]:
                passed += 1
            else:
                failures.append((i, actual, wanted))
        elif wanted["kind"] == "template" and line.startswith("template "):
            parts = line.split()
            # The C API has diagnostics instead of Java exception classes.
            # Missing names/numbers are UNKNOWN_GROUP; malformed syntax is
            # INVALID_TEMPLATE. The JDK supplies no template source index.
            diagnostic = 64 if wanted["message"].startswith("No group") else 63
            if (len(parts) == 4 and int(parts[1]) == diagnostic
                    and 0 <= int(parts[2]) < len(baseline["cases"][i]["argument"].encode())):
                passed += 1
                errors_without_oracle_offset += 1
            else:
                failures.append((i, line, wanted))
        else:
            failures.append((i, line, wanted))
    changed = sum(baseline["answers"][i] != forward["answers"][i]
                  for i in indices)
    for i, actual, wanted in failures[:30]:
        print(f"case {i} {baseline['cases'][i]}: {actual} != {wanted}",
              file=sys.stderr)
    print(f"JDK25 replacement: {len(indices)} total, {passed} pass, "
          f"{len(failures)} failure, {unsupported} unsupported, "
          f"{excluded} excluded ({split_pair} split-pair output), "
          f"{errors_without_oracle_offset} error offsets "
          f"not exposed by JDK, JDK27 differences {changed}")
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
