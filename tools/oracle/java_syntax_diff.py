#!/usr/bin/env python3
"""Compare generated Java syntax verdicts with separate pinned JDK records."""

import argparse
import json
import os
import subprocess
import sys
import java_probe

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "tests", "data", "probe")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", default=os.path.join(
        ROOT, "build", "linux", "release", "apps", "tools", "grx_syntax"))
    args = parser.parse_args()
    baseline, forward = [json.load(open(os.path.join(DATA, pin + ".json"),
                                        encoding="utf-8"))
                         for pin in ("java25", "java27")]
    if baseline["cases"] != forward["cases"]:
        raise SystemExit("JDK inventories differ")
    indices = [i for i, row in enumerate(baseline["cases"])
               if row["area"] == "generated-syntax"]
    if not indices:
        raise SystemExit("generated Java syntax inventory is empty")
    payload = "".join(f'{baseline["cases"][i]["flags"]}\t'
                      f'{baseline["cases"][i]["pattern"].encode().hex()}\n'
                      for i in indices)
    result = subprocess.run([args.driver, "java"], input=payload, text=True,
                            capture_output=True, check=True)
    lines = result.stdout.splitlines()
    if len(lines) != len(indices):
        raise SystemExit(f"{len(lines)} answers for {len(indices)} cases")
    passed = unsupported = stale = 0
    failures = []
    for index, line in zip(indices, lines):
        wanted = baseline["answers"][index]["kind"]
        if wanted not in ("compile", "syntax"):
            raise SystemExit(f"case {index}: unexpected oracle answer {wanted}")
        actual = "compile" if line == "ok" else "syntax" if line.startswith("syntax ") else "unsupported"
        if actual == "unsupported":
            unsupported += 1
            failures.append((index, line, wanted))
        elif actual == wanted:
            passed += 1
        else:
            failures.append((index, line, wanted))
    changed = sum(baseline["answers"][i] != forward["answers"][i]
                  for i in indices)
    for i, actual, wanted in failures:
        print(f"case {i} {baseline['cases'][i]}: {actual} != {wanted}",
              file=sys.stderr)
    print(f"fixed inventory revision {java_probe.GENERATED_INVENTORY_REVISION}; JDK25 generated syntax: {len(indices)} total, "
          f"{passed} pass, 0 excluded, {stale} stale exclusion, "
          f"{unsupported} unsupported, {len(failures)} failure; "
          f"JDK27 differences {changed}")
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
