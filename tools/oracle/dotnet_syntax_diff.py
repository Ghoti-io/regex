#!/usr/bin/env python3
r"""Compare this library's .NET accept-or-reject against the pinned .NET.

`java_syntax_diff.py`'s shape: the generated inventory in
`dotnet_probe.py` is asked of the reference once and committed to
`tests/data/probe/dotnet.json`, and this reads our own `grx_syntax` against
those answers. The reference is not re-run here, so a disagreement is this
library's until somebody re-records - which is the point of a pin.

**One disagreement is expected and is named rather than counted as a pass.**
`(?<n>a)(?<-n>b)` is a balancing group: .NET compiles it and this library
refuses it, deferred to its own work package. It is in the inventory so that
the deferral stays visible - a construct nobody asks about is indistinguishable
from one nobody noticed - and it is excluded here by *pattern*, so a second
construct going the same way would not join it silently.

The exclusion is checked in both directions, which is `check_exclusions.py`'s
rule applied locally: if the reference ever refuses that pattern, or this
library ever accepts it, the exclusion has outlived its reason and this says
so instead of passing.
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

# Patterns whose disagreement is recorded elsewhere. Keyed by the pattern so
# that a renumbered inventory cannot silently widen it.
EXCLUDED = {
    "(?<n>a)(?<-n>b)":
        "a balancing group: the reference compiles it, this library refuses "
        "it, and it is deferred to its own work package (dialects.md section 6)",
    r"\p{IsGreek}":
        "an `Is`-prefixed block name. The short category names work - "
        r"`\p{L}`, `\p{Lu}`, `\p{Nd}`, `\p{Cs}` all compile here and there - "
        "and the `Is` namespace does not, because it is .NET's own and not "
        r"Unicode's: measured on 10.0.13, `\p{IsGreek}` and "
        r"`\p{IsBasicLatin}` compile while `\p{IsGreekAndCoptic}` does not, "
        r"where Java accepts all three. `\p{InGreek}`, `\p{IsL}`, "
        r"`\p{Letter}` and `\p{lu}` are refused there too, so this is not "
        "Perl's `Is` re-running the chain and not a loose match either. A "
        "block list cannot be inferred from four names, so it is excluded "
        "and named rather than implemented from a guess",
}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", default=os.path.join(
        ROOT, "build", "linux", "release", "apps", "tools", "grx_syntax"))
    args = parser.parse_args()

    with open(os.path.join(DATA, "dotnet.json"), encoding="utf-8") as source:
        pinned = json.load(source)
    if pinned["cases"] != dotnet_probe.CASES:
        raise SystemExit("the inventory has changed; re-record the probe")
    indices = [i for i, row in enumerate(pinned["cases"])
               if row["area"] == "generated-syntax"]
    if not indices:
        raise SystemExit("the generated .NET syntax inventory is empty")

    payload = "".join(
        "%s\t%s\n" % (pinned["cases"][i]["flags"],
                      pinned["cases"][i]["pattern"].encode().hex())
        for i in indices)
    finished = subprocess.run([args.driver, "dotnet"], input=payload,
                              text=True, capture_output=True, check=True)
    lines = finished.stdout.splitlines()
    if len(lines) != len(indices):
        raise SystemExit("%d answers for %d cases"
                         % (len(lines), len(indices)))

    passed = excluded = stale = 0
    failures = []
    for index, line in zip(indices, lines):
        pattern = pinned["cases"][index]["pattern"]
        wanted = pinned["answers"][index]["kind"]
        if wanted not in ("compile", "syntax"):
            raise SystemExit("case %d: the oracle answered %r, which is "
                             "neither an accept nor a refusal"
                             % (index, wanted))
        # Three outcomes, not two. `unsupported` is this library saying it
        # has not built the construct, which is a different finding from
        # `syntax` - the reference refuses it too - and folding them loses
        # exactly the distinction a new dialect needs. grx_syntax prints the
        # three separately for this dialect; see its own comment.
        actual = ("compile" if line == "ok"
                  else "syntax" if line.startswith("syntax ")
                  else "unsupported" if line.startswith("unsupported ")
                  else "limit" if line.startswith("limit ")
                  else "unknown:" + line)
        if pattern in EXCLUDED:
            # Both directions. The exclusion claims the reference accepts and
            # this library does not; either half ceasing to hold makes it an
            # exclusion for a disagreement that no longer exists.
            if wanted != "compile" or actual == "compile":
                stale += 1
                failures.append((index, actual, wanted,
                                 "exclusion is stale: " + EXCLUDED[pattern]))
            else:
                excluded += 1
            continue
        if actual == wanted:
            passed += 1
        else:
            failures.append((index, actual, wanted, ""))

    for index, actual, wanted, note in failures:
        print("case %d %r: %s, the reference says %s%s"
              % (index, pinned["cases"][index]["pattern"], actual, wanted,
                 "; " + note if note else ""), file=sys.stderr)
    print("inventory %s; .NET generated syntax: %d asked, %d agree, "
          "%d excluded and named, %d stale exclusion, %d disagreement"
          % (dotnet_probe.DOTNET_INVENTORY_REVISION, len(indices), passed,
             excluded, stale, len(failures)))
    return bool(failures)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, oracle_env.OracleUnavailable) as error:
        sys.stderr.write("dotnet_syntax_diff: %s\n" % error)
        sys.exit(1)
