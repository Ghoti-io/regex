#!/usr/bin/env python3
r"""Compare pinned .NET search *sequences* - every match, not just the first.

`dotnet_match_diff.py` asks `first`-mode rows, so the whole of iteration was
compared by nothing: 24 `all`, `all-from` and `find-from` rows sat in
`tests/data/probe/dotnet.json` with answers nobody read.
`dotnet_coverage.py` is what said so, and this is the gate that closes it.

**It found the field the dialect had left unset.** `GRX_Profile::search_start`
says what `\G` asserts while a search-all loop runs, and syntax.c's comment
said no case divided PREVIOUS_END from ATTEMPT - which was true of the rows
the probe held, because the two differ only after a *failure* forces the loop
to advance and no row did that. `\Ga*` over "baac" does: .NET reports one
match and this library reported four.

Five engines, as the span differential does, because an iteration rule lives
in the search loop and not in the pattern: an engine that cannot run a row
answers `unsupported` and is counted as not having run it, and the floor
below refuses a run where an engine silently stopped being eligible for
everything.
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
ENGINES = ("auto", "pike", "backtrack", "bitstate", "dfa")

# The `unit` area is WP-49's and is excluded wholesale by area rather than by
# row: those subjects hold a character above U+FFFF, .NET's loop steps one
# UTF-16 code unit at a time, and the positions it reports are inside a
# character. Section 6 of documentation/dialects.md carries it.
EXCLUDED_AREAS = ("unit",)

# `auto` must run every row, and no engine but dfa may run none. Without this
# a regression that made every row `unsupported` would print a clean score.
FLOOR_AUTO_RUNS_ALL = True


def spans(fields):
    out = []
    for field in fields:
        if field == "-":
            out.append(None)
        else:
            start, _, end = field.partition(":")
            out.append([int(start), int(end)])
    return out


def parse(line):
    fields = line.split()
    # Two output shapes, because two driver modes. `first` prints
    # `match <engine> <spans>` and `all` prints `all <count> <match>...`;
    # reading the second shape for the first lost every span and read as
    # eight disagreements that were this file's.
    if fields[0] == "match":
        return {"kind": "match", "groups": spans(fields[2:])}
    if fields[0] != "all":
        return {"kind": fields[0]}
    matches = [spans(group.split(",")) for group in fields[2:]]
    if len(matches) != int(fields[1]):
        raise SystemExit("driver said %s matches and printed %d"
                         % (fields[1], len(matches)))
    return {"kind": "all", "matches": matches}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", default=os.path.join(
        ROOT, "build", "linux", "release", "apps", "tools", "grx_match"))
    args = parser.parse_args()

    with open(os.path.join(DATA, "dotnet.json"), encoding="utf-8") as source:
        pinned = json.load(source)
    if pinned["cases"] != dotnet_probe.CASES:
        raise SystemExit("the inventory has changed; re-record the probe")

    # `find-from` answers one match and `all`/`all-from` answer a sequence,
    # so they are two payloads and two driver modes.
    groups = {}
    excluded = 0
    for index, row in enumerate(pinned["cases"]):
        if row["mode"] not in ("all", "all-from", "find-from"):
            continue
        if row["area"] in EXCLUDED_AREAS:
            excluded += 1
            continue
        if row["flags"]:
            excluded += 1
            continue
        groups.setdefault(row["mode"], []).append(index)
    if not groups.get("all"):
        raise SystemExit("no .NET search sequences to compare")

    failures = []
    passed = 0
    ran = {engine: 0 for engine in ENGINES}
    for mode, indices in sorted(groups.items()):
        payload = "".join(
            "\t%s\t%s%s\n"
            % (pinned["cases"][i]["pattern"].encode().hex(),
               pinned["cases"][i]["subject"].encode().hex(),
               "\t%s,-," % pinned["cases"][i]["argument"]
               if mode in ("all-from", "find-from") else "")
            for i in indices)
        driver_mode = "all" if mode in ("all", "all-from") else "first"
        for engine in ENGINES:
            finished = subprocess.run(
                [args.driver, "dotnet", engine, driver_mode], input=payload,
                text=True, capture_output=True, check=True)
            lines = finished.stdout.splitlines()
            if len(lines) != len(indices):
                raise SystemExit("%s/%s: %d answers for %d cases"
                                 % (engine, mode, len(lines), len(indices)))
            for index, line in zip(indices, lines):
                actual = parse(line)
                if actual["kind"] == "unsupported":
                    continue
                ran[engine] += 1
                wanted = pinned["answers"][index]
                if actual == wanted:
                    passed += 1
                else:
                    failures.append((engine, index, actual, wanted))

    if FLOOR_AUTO_RUNS_ALL:
        asked = sum(len(v) for v in groups.values())
        if ran["auto"] != asked:
            failures.append(("auto", -1, {"ran": ran["auto"]},
                             {"ran": asked}))
            print("auto ran %d of %d rows; it must run every one"
                  % (ran["auto"], asked), file=sys.stderr)
        for engine in ENGINES:
            if engine != "dfa" and ran[engine] == 0:
                failures.append((engine, -1, {"ran": 0}, {"ran": "some"}))
                print("%s ran no rows at all" % engine, file=sys.stderr)

    for engine, index, actual, wanted in failures:
        if index < 0:
            continue
        print("%s case %d %r over %r: %s, the reference says %s"
              % (engine, index, pinned["cases"][index]["pattern"],
                 pinned["cases"][index]["subject"], json.dumps(actual),
                 json.dumps(wanted)), file=sys.stderr)
    print("inventory %s; .NET searches: %d asked, %s; %d comparisons agree, "
          "%d excluded, %d disagreement"
          % (dotnet_probe.DOTNET_INVENTORY_REVISION,
             sum(len(v) for v in groups.values()),
             ", ".join("%s %d" % (e, ran[e]) for e in ENGINES),
             passed, excluded, len(failures)))
    return bool(failures)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, oracle_env.OracleUnavailable) as error:
        sys.stderr.write("dotnet_search_diff: %s\n" % error)
        sys.exit(1)
