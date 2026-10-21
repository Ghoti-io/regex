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
    # Every unflagged accept-or-reject row, not just the generated ones.
    #
    # This asked `area == "generated-syntax"` and nothing else, which was
    # nine rows out of a six-hundred-row inventory. The identical hole in
    # .NET's syntax gate is how thirteen divergent escapes went unasked
    # while eleven gates reported clean, so the population here is now the
    # question rather than one area of it: a row is in if it is a compile
    # row, carries no flags, and the oracle answered accept or reject.
    indices = [i for i, row in enumerate(baseline["cases"])
               if row["mode"] == "compile" and not row["flags"]
               and baseline["answers"][i]["kind"] in ("compile", "syntax")]
    if len(indices) < 100:
        raise SystemExit("Java syntax inventory is %d rows; that is too few "
                         "to be the whole grammar and means this gate has "
                         "stopped seeing most of it" % len(indices))
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
