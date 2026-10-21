#!/usr/bin/env python3
r"""Compare this library's .NET split against the pinned .NET.

The `split` and `generated-split` rows of `dotnet_probe.py` are the corpus.
Two of the reference's modes are compared and a third is not:

  * `split` is `Regex.Split(input)`, and maps to a limit of 0 here, which
    this dialect reads as "no limit".
  * `split-count` is `Regex.Split(input, count)`, a count of pieces whose
    last is the unsplit remainder.
  * A negative count throws on .NET and cannot be expressed through this
    API - `limit` is a size_t and GRX_NPOS already means no limit - so the
    row is counted as unaskable rather than compared. It is in the probe
    because the refusal is a fact about the reference worth pinning even
    where this side has no question to ask.
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
AREAS = ("split", "generated-split")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", default=os.path.join(
        ROOT, "build", "linux", "release", "apps", "tools", "grx_split"))
    args = parser.parse_args()

    with open(os.path.join(DATA, "dotnet.json"), encoding="utf-8") as source:
        pinned = json.load(source)
    if pinned["cases"] != dotnet_probe.CASES:
        raise SystemExit("the inventory has changed; re-record the probe")

    rows = []
    unaskable = 0
    for i, row in enumerate(pinned["cases"]):
        if row["area"] not in AREAS:
            continue
        if row["mode"] not in ("split", "split-count"):
            continue
        if pinned["answers"][i].get("kind") != "split":
            # The negative count, which the reference refuses and this API
            # has no value for. Counted, not compared, and not silent.
            unaskable += 1
            continue
        limit = row["argument"] if row["mode"] == "split-count" else "0"
        rows.append((i, limit))
    if not rows:
        raise SystemExit("the .NET split corpus is empty")

    payload = "".join(
        "\t%s\t%s\t%s\n" % (pinned["cases"][i]["pattern"].encode().hex(),
                            pinned["cases"][i]["subject"].encode().hex(),
                            limit)
        for i, limit in rows)
    finished = subprocess.run([args.driver, "dotnet"], input=payload,
                              text=True, capture_output=True, check=True)
    lines = finished.stdout.splitlines()
    if len(lines) != len(rows):
        raise SystemExit("%d answers for %d cases" % (len(lines), len(rows)))

    passed = 0
    failures = []
    for (index, limit), line in zip(rows, lines):
        wanted = pinned["answers"][index]["pieces"]
        if line.startswith("ok "):
            # **The count decides how many pieces, not the text.** `ok 1 `
            # is one piece that is empty and `ok 0 ` is no pieces at all,
            # and both have an empty third field - so splitting the text and
            # letting its length speak reads the first as the second. The
            # first draft of this did exactly that and reported five
            # disagreements on an empty subject, two of which were this
            # parser rather than the library.
            head, count_text, _, rest = line.partition(" ")[0], \
                line.split(" ", 2)[1], None, line.split(" ", 2)[2] \
                if len(line.split(" ", 2)) > 2 else ""
            count = int(count_text)
            pieces = rest.split("|") if count else []
            if len(pieces) != count:
                raise SystemExit("case %d: %r names %d pieces and carries %d"
                                 % (index, line, count, len(pieces)))
            actual = ["<unset>" if piece == "-"
                      else bytes.fromhex(piece).decode("utf-8", "replace")
                      for piece in pieces]
        else:
            actual = "<" + line + ">"
        if actual == wanted:
            passed += 1
        else:
            failures.append((index, limit, actual, wanted))

    for index, limit, actual, wanted in failures[:12]:
        row = pinned["cases"][index]
        print("case %d %r over %r limit %s: %r != %r"
              % (index, row["pattern"], row["subject"], limit, actual,
                 wanted), file=sys.stderr)
    print("inventory %s; .NET split: %d asked, %d agree, %d unaskable "
          "through this API, %d disagreement"
          % (dotnet_probe.DOTNET_INVENTORY_REVISION, len(rows), passed,
             unaskable, len(failures)))
    return bool(failures)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, oracle_env.OracleUnavailable) as error:
        sys.stderr.write("dotnet_split_diff: %s\n" % error)
        sys.exit(1)
