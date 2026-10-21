#!/usr/bin/env python3
r"""Compare this library's .NET spans against the pinned .NET, on every engine.

`java_match_diff.py`'s shape, with one deliberate difference. That file
carries a hardcoded table of which generated patterns each engine is
expected to be able to run, and a table like that is a second copy of a
fact the tool already reports: `grx_match` answers `unsupported` for a
pattern the named engine cannot run. So eligibility is read from the answer
here rather than declared, which means a pattern becoming runnable on one
more engine - or stopping - changes the counts and not the source.

What that costs is the thing the hardcoded table buys: an engine that
quietly stopped running *everything* would report a clean sweep of
`unsupported` rather than a failure. The floor below is what replaces it -
each engine must run at least one case, and `auto` must run every case,
since `auto` by definition picks an engine that can.

Every answer compared is a pinned reference answer from
`tests/data/probe/dotnet.json`. The reference is not re-run, so a
disagreement is this library's.
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

# The areas whose `first` rows are a span comparison. `unit` is deliberately
# absent: those are the astral rows, where .NET matches over code units and
# this library cannot name a position inside a character. They are §6's
# deviation and WP-49's inventory, not a disagreement a gate should score.
AREAS = ("control", "generated-match", "classes", "flags", "captures",
         "anchors", "lookbehind", "profile", "profile-audit", "ecmascript",
         "iteration")
ENGINES = ("auto", "pike", "backtrack", "bitstate", "dfa")

# Rows whose disagreement is recorded elsewhere, keyed by (pattern, subject)
# so a renumbered inventory cannot widen them. Each says what the reference
# answers and what this library answers, and both halves are checked: an
# exclusion that stops being a disagreement is reported rather than passed.
EXCLUDED = {
    # .NET's numbering is sparse and collides, and this library refuses the
    # two spellings that need more than a partition: an explicit number
    # (`(?<5>a)`), which makes the numbering sparse, and a number colliding
    # with an unnamed group's (`(a)(?<1>b)`), which .NET merges. The plain
    # unnamed-before-named case is implemented and compared.
    #
    # **A repeated name was a third and is not any more.** It was excluded
    # here as "refused: a repeated group name" until the naming differential
    # existed and asked what .NET actually does with one: it merges the
    # occurrences into a single group, where PCRE2's `J` would give two
    # sharing a name. That is implemented, so these rows are compared - and
    # this gate is what said so, by reporting the exclusions as stale on the
    # first run after the fix rather than passing them.
    ("(?<5>a)(b)", "ab"): "refused: an explicit group number",
    ("(a)(?<1>b)", "ab"): "refused: an explicit number colliding with an "
                          "unnamed group's",
    # The `Is` namespace, which is .NET's own block list; the syntax
    # differential carries the measurement.
    (r"\p{IsGreek}", "\u03b1"): "refused: an `Is`-prefixed block name",
    (r"\p{IsBasicLatin}", "a"): "refused: an `Is`-prefixed block name",
    # `\w` over U+24B6, CIRCLED LATIN CAPITAL LETTER A, category So. .NET's
    # `\w` is `[\p{L}\p{Mn}\p{Nd}\p{Pc}]` and this library's is UTS #18
    # Annex C, which takes So. Neither of the other two GRX_WordSet values
    # fits either: CATEGORIES would newly disagree on U+00B2, a No that .NET
    # also declines. So this wants a fourth value, and four measured code
    # points do not define a set - pinning it needs the code-point sweep the
    # other three values were measured with, through the driver's `ranges`
    # mode. That is the profile audit's work.
    (r"\w", "\u24b6"): r"`\w` takes So here and not on .NET; GRX_WordSet "
                        r"needs a fourth value and a code-point sweep to "
                        r"justify it",
    # Case folding. `(?i)` on .NET is ToLower, not case folding, so U+017F
    # does not fold to "s" there and does here. A deviation already
    # recorded, not a defect: dialects.md section 5.8.
    ("(?i)\u017f", "s"): "`(?i)` is ToLower on .NET, implemented here as "
                          "SIMPLE_FOLD and documented as a deviation",
}


def parse(line):
    """One `grx_match` answer as the shape the probe records."""
    if line == "nomatch":
        return {"kind": "nomatch"}
    fields = line.split()
    if not fields:
        return {"kind": "empty"}
    if fields[0] == "match":
        groups = []
        for span in fields[2:]:
            groups.append(None if span == "-"
                          else [int(x) for x in span.split(":")])
        return {"kind": "match", "groups": groups}
    if fields[0] == "compile":
        return {"kind": "syntax"}
    return {"kind": fields[0]}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", default=os.path.join(
        ROOT, "build", "linux", "release", "apps", "tools", "grx_match"))
    args = parser.parse_args()

    with open(os.path.join(DATA, "dotnet.json"), encoding="utf-8") as source:
        pinned = json.load(source)
    if pinned["cases"] != dotnet_probe.CASES:
        raise SystemExit("the inventory has changed; re-record the probe")

    indices = [i for i, row in enumerate(pinned["cases"])
               if row["mode"] == "first" and row["area"] in AREAS
               and not row["flags"]
               and pinned["answers"][i].get("kind") in ("match", "nomatch")]
    if not indices:
        raise SystemExit("the .NET span corpus is empty")

    payload = "".join(
        "\t%s\t%s\n" % (pinned["cases"][i]["pattern"].encode().hex(),
                        pinned["cases"][i]["subject"].encode().hex())
        for i in indices)

    failures = []
    stale = []
    excluded = 0
    ran = {}
    for engine in ENGINES:
        command = [args.driver, "dotnet"]
        if engine != "auto":
            command.append(engine)
        finished = subprocess.run(command, input=payload, text=True,
                                  capture_output=True, check=True)
        lines = finished.stdout.splitlines()
        if len(lines) != len(indices):
            raise SystemExit("%s: %d answers for %d cases"
                             % (engine, len(lines), len(indices)))
        ran[engine] = 0
        for index, line in zip(indices, lines):
            actual = parse(line)
            if actual["kind"] in ("unsupported", "error"):
                continue
            ran[engine] += 1
            wanted = pinned["answers"][index]
            agree = (actual.get("kind") == wanted.get("kind")
                     and (actual.get("kind") != "match"
                          or actual.get("groups") == wanted.get("groups")))
            key = (pinned["cases"][index]["pattern"],
                   pinned["cases"][index]["subject"])
            if key in EXCLUDED:
                # Both directions. An exclusion claims these two disagree;
                # if they now agree it has outlived its reason and saying so
                # is the only way anyone finds out.
                if agree:
                    stale.append((engine, index, actual, wanted))
                else:
                    excluded += 1
                continue
            if not agree:
                failures.append((engine, index, actual, wanted))

    # The floor that replaces a hardcoded eligibility table. `auto` picks an
    # engine that can run the pattern, so a case it skips is a case no engine
    # can run - which for this corpus would be a defect rather than a fact
    # about one engine.
    if ran["auto"] != len(indices):
        failures.append(("auto", -1,
                         {"ran": ran["auto"]}, {"ran": len(indices)}))
    for engine in ENGINES:
        if engine != "dfa" and not ran[engine]:
            failures.append((engine, -1, {"ran": 0}, {"ran": "at least 1"}))

    for engine, index, actual, wanted in stale:
        print("%s case %d %r over %r: the exclusion is stale, both now say %s"
              % (engine, index, pinned["cases"][index]["pattern"],
                 pinned["cases"][index]["subject"], actual), file=sys.stderr)
    failures += stale
    for engine, index, actual, wanted in failures[:12]:
        if index < 0:
            print("%s ran %s of %s cases" % (engine, actual, wanted),
                  file=sys.stderr)
        else:
            print("%s case %d %r over %r: %s != %s"
                  % (engine, index, pinned["cases"][index]["pattern"],
                     pinned["cases"][index]["subject"], actual, wanted),
                  file=sys.stderr)
    print("inventory %s; .NET spans: %d cases, %s; %d comparisons excluded "
          "and named, %d stale exclusion, %d disagreement"
          % (dotnet_probe.DOTNET_INVENTORY_REVISION, len(indices),
             ", ".join("%s %d" % (e, ran[e]) for e in ENGINES),
             excluded, len(stale), len(failures)))
    return bool(failures)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, oracle_env.OracleUnavailable) as error:
        sys.stderr.write("dotnet_match_diff: %s\n" % error)
        sys.exit(1)
