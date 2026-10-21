#!/usr/bin/env python3
r"""Compare this library's .NET replacement output against the pinned .NET.

The `generated-replacement` and `template` rows of `dotnet_probe.py` are the
corpus. Both were asked of the reference in `replace-all` mode, and
`grx_replace` is global by default for the same reason - `s///g` is what a
differential wants - so the two sides answer the same question.

The reference is not re-run: a disagreement is this library's until somebody
re-records the probe.
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
AREAS = ("template", "generated-replacement")

# Nothing yet. The grammar agreed on every row the first time it was asked,
# which is worth stating rather than leaving an empty dict to be read as an
# oversight: `$+` and the literal-missing rule were built *from* these
# measurements two commits ago, so this differential is confirming an
# implementation rather than discovering one. Its value is the next change.
EXCLUDED = {}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", default=os.path.join(
        ROOT, "build", "linux", "release", "apps", "tools", "grx_replace"))
    args = parser.parse_args()

    with open(os.path.join(DATA, "dotnet.json"), encoding="utf-8") as source:
        pinned = json.load(source)
    if pinned["cases"] != dotnet_probe.CASES:
        raise SystemExit("the inventory has changed; re-record the probe")

    indices = [i for i, row in enumerate(pinned["cases"])
               if row["area"] in AREAS and row["mode"] == "replace-all"
               and pinned["answers"][i].get("kind") == "replace"]
    if not indices:
        raise SystemExit("the .NET replacement corpus is empty")

    payload = "".join(
        "\t%s\t%s\t%s\n" % (pinned["cases"][i]["pattern"].encode().hex(),
                            pinned["cases"][i]["subject"].encode().hex(),
                            pinned["cases"][i]["argument"].encode().hex())
        for i in indices)
    finished = subprocess.run([args.driver, "dotnet"], input=payload,
                              text=True, capture_output=True, check=True)
    lines = finished.stdout.splitlines()
    if len(lines) != len(indices):
        raise SystemExit("%d answers for %d cases" % (len(lines), len(indices)))

    passed = excluded = 0
    failures = []
    stale = []
    for index, line in zip(indices, lines):
        key = (pinned["cases"][index]["pattern"],
               pinned["cases"][index]["argument"])
        wanted = pinned["answers"][index]["text"]
        if line.startswith("ok "):
            actual = bytes.fromhex(line[3:]).decode("utf-8", "replace")
        else:
            actual = "<" + line + ">"
        agree = actual == wanted
        if key in EXCLUDED:
            (stale if agree else failures).append((index, actual, wanted))
            if not agree:
                excluded += 1
                failures.pop()
            continue
        if agree:
            passed += 1
        else:
            failures.append((index, actual, wanted))

    for index, actual, wanted in stale:
        print("case %d: the exclusion is stale, both now say %r"
              % (index, actual), file=sys.stderr)
    failures += stale
    for index, actual, wanted in failures[:12]:
        row = pinned["cases"][index]
        print("case %d %r over %r with %r: %r != %r"
              % (index, row["pattern"], row["subject"], row["argument"],
                 actual, wanted), file=sys.stderr)
    print("inventory %s; .NET replacement: %d asked, %d agree, %d excluded "
          "and named, %d stale exclusion, %d disagreement"
          % (dotnet_probe.DOTNET_INVENTORY_REVISION, len(indices), passed,
             excluded, len(stale), len(failures)))
    return bool(failures)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, oracle_env.OracleUnavailable) as error:
        sys.stderr.write("dotnet_replace_diff: %s\n" % error)
        sys.exit(1)
