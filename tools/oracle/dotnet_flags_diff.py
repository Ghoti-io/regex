#!/usr/bin/env python3
r"""Compare this library against the pinned .NET for every *flagged* row.

The match differential deliberately asks only unflagged rows:
`dotnet_match_diff.py` selects on `not row["flags"]`, because grx_match's
flag table is the POSIX/PCRE2 letter set and `n` means REG_NEWLINE there
while .NET's `n` is ExplicitCapture. That left a hole the size of every
option the dialect has - and phase 4 adds `RegexOptions.ECMAScript`, which
is nothing *but* a flag, so the hole had to be closed before the option
could be said to work.

Three kinds of letter, and the difference between them is the whole content
of this file:

  * **`i`, `m`, `s`, `x`, `n`** are .NET's own alphabet. grx_match hands the
    letters to `grx_options_parse()` for this dialect, so they mean what the
    dialect says they mean.
  * **`E` is `RegexOptions.ECMAScript`, which has no letter anywhere**: it is
    a constructor argument, there is no `(?E)`, and no pattern can ask for
    it. It becomes caller-set option bits, named on grx_match's command line
    (`--options=`), which is why the rows are run in two groups.
  * **`C` is `RegexOptions.CultureInvariant`, which this library has no
    counterpart for** because it has no culture at all: folding is always
    the invariant one. The letter is dropped rather than translated, and the
    rows that carry it are compared anyway - if dropping it ever changes an
    answer, that is a finding and this says so instead of passing.

`R` is `RegexOptions.RightToLeft`, which this library does not have. Its one
row is excluded by letter and named.
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

# `RegexOptions.ECMAScript`, as the four caller-set bits it is. Measured, not
# inferred: the three shorthands and `\b` narrow to ASCII (probe area
# `ecmascript`, the `\d`/`\w`/`\s`/`\b` rows), and a numeric escape becomes
# the longest digit prefix that names an open group (the `\1`, `\8`, `\10`
# and `\1(a)` rows). Nothing else moves - `\p{L}`, the `[a-z-[aeiou]]`
# subtraction, `\k<n>` and the group *numbering* all answer the same with it
# and without.
ECMASCRIPT_OPTIONS = (
    "ascii-digit,ascii-space,ascii-word,prefix-backreference")

# A letter this library has no counterpart for excludes its row. Keyed by
# letter, with the reason, so a new row carrying it is excluded for a stated
# reason rather than silently.
UNSUPPORTED_LETTERS = {
    "R": "RegexOptions.RightToLeft: this library matches forwards only, and "
         "a reversed search is not an option but a different engine. Its "
         "rows are in the inventory so the absence stays visible",
}

# Letters that are correctly dropped. The row is still compared.
DROPPED_LETTERS = {
    "C": "RegexOptions.CultureInvariant: this library has no locale and "
         "folds invariantly always (documentation/dialects.md section 6), so "
         "the letter asks for what is already true",
}

# Caseless pairs this library answers differently, excluded by (pattern,
# subject) and checked in both directions. **This is one finding, and it is
# not the one `dialects.md` recorded.** Section 5.8's `CULTURE` row said the
# deviation was .NET's culture-sensitive `ToLower`, "implemented as
# `SIMPLE_FOLD`, i.e. `CultureInvariant`" - so a caller asking for
# `CultureInvariant` was promised an exact answer. It is not exact: U+017F
# declines `s` in .NET *with* `CultureInvariant` set, where simple folding
# matches, because `IgnoreCase` is equality under .NET's invariant simple
# *lowercase mapping* and not under case folding. Measured on 10.0.13, with
# eight pairs chosen to separate the two rules in both directions:
#
#   U+017F vs `s`        folds together, maps apart   -> .NET says no
#   U+0345 vs U+03B9     folds together, maps apart   -> .NET says no
#   U+212A vs `k`        both                         -> .NET says yes
#   U+1E9E vs U+00DF     both                         -> .NET says yes
#   U+13A0 vs U+AB70     both                         -> .NET says yes
#   U+0131 vs `I`        neither                      -> .NET says no
#   U+00DF vs "ss"       full folding only            -> .NET says no
#   U+0130 vs `i`        maps together in UnicodeData -> .NET says no
#
# The last is the one that stops this being UnicodeData's
# Simple_Lowercase_Mapping outright: .NET's invariant casing leaves the
# dotted and dotless I alone deliberately, so the rule is that mapping with
# those two excluded.
#
# The work is a new fold kind with an orbit table built from
# Simple_Lowercase_Mapping, which is locale-free and so is *not* blocked by
# the missing locale the old row blamed. It is out of this slice and written
# up rather than left here: see documentation/work-packages.md.
#
# **Only the "folds together, maps apart" rows disagree**, which is what
# makes the characterization above a measurement rather than a story: of
# the eight pairs, six agree with this library already - including U+0130
# against `i`, which both decline - and the two that do not are exactly
# the two where a simple fold orbit joins what the lowercase mapping
# separates.
EXCLUDED_PAIRS = {
    ("\u017f", "S"): "folds together, maps apart: see above",
    ("\u017f", "s"): "folds together, maps apart: see above",
    ("\u0345", "\u03b9"): "folds together, maps apart: see above",
}


# The flagged rows that answer `compile` rather than a match, and what
# answers them. A compile row is not asked here - this compares matches - so
# each one has to be covered by a match row that carries the same pattern and
# the same flags, because a pattern that matches has compiled. The ones that
# cannot be are named, and the gate fails on a flagged compile row that is
# neither.
#
# All four of these are `RegexOptions.ECMAScript` combined with an option
# .NET's *constructor* refuses, throwing ArgumentOutOfRangeException before
# any pattern is parsed. This library has no such validation and should not
# grow one: the refusal is not a property of the pattern language, and .NET
# itself proves that by accepting the same combinations written inline -
# `(?s).` matches a newline, `(?x)a b` matches "ab" and `(?n)(a)` has no
# group one, all with the option set (probe cases in this area). A library
# that refused the pair would refuse what the reference accepts.
UNASKED_COMPILE = {
    ("a", "sE"): "RegexOptions.Singleline with ECMAScript: the "
                 "constructor's own argument check, and `(?s)` under the "
                 "option is accepted by .NET",
    ("a", "xE"): "RegexOptions.IgnorePatternWhitespace with ECMAScript: as "
                 "above, and `(?x)` under the option is accepted",
    ("a", "nE"): "RegexOptions.ExplicitCapture with ECMAScript: as above, "
                 "and `(?n)` under the option is accepted",
    ("a", "RE"): "RegexOptions.RightToLeft with ECMAScript: as above, and "
                 "RightToLeft is excluded by letter besides",
}


def parse_answer(line):
    fields = line.split()
    if fields[0] == "match":
        spans = []
        for field in fields[2:]:
            if field == "-":
                spans.append(None)
            else:
                start, _, end = field.partition(":")
                spans.append([int(start), int(end)])
        return {"kind": "match", "groups": spans}
    if fields[0] == "nomatch":
        return {"kind": "nomatch"}
    if fields[0] == "compile":
        return {"kind": "syntax"}
    return {"kind": fields[0]}


def ask(driver, rows, options):
    """Run one group of rows under one caller-option set."""
    if not rows:
        return []
    payload = "".join(
        "%s\t%s\t%s\n" % (row["letters"], row["pattern"].encode().hex(),
                          row["subject"].encode().hex())
        for row in rows)
    command = [driver, "dotnet"]
    if options:
        command.append("--options=" + options)
    finished = subprocess.run(command, input=payload, text=True,
                              capture_output=True, check=True)
    lines = finished.stdout.splitlines()
    if len(lines) != len(rows):
        raise SystemExit("%d answers for %d rows" % (len(lines), len(rows)))
    return lines


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", default=os.path.join(
        ROOT, "build", "linux", "release", "apps", "tools", "grx_match"))
    args = parser.parse_args()

    with open(os.path.join(DATA, "dotnet.json"), encoding="utf-8") as source:
        pinned = json.load(source)
    if pinned["cases"] != dotnet_probe.CASES:
        raise SystemExit("the inventory has changed; re-record the probe")

    plain, ecma, excluded, dropped = [], [], 0, 0
    pairs, stale = 0, []
    for index, row in enumerate(pinned["cases"]):
        if row["mode"] != "first" or not row["flags"]:
            continue
        # The letter check comes before the answer-kind filter on purpose.
        # `R`'s one row answers `offset` - it is in the WP-49 inventory - so
        # with the filter first, UNSUPPORTED_LETTERS was a control that could
        # not fire and the line read "0 excluded by letter" whether the table
        # held anything or not.
        refused = [c for c in row["flags"] if c in UNSUPPORTED_LETTERS]
        if refused:
            excluded += 1
            continue
        if pinned["answers"][index].get("kind") not in ("match", "nomatch"):
            continue
        letters = "".join(c for c in row["flags"]
                          if c not in DROPPED_LETTERS and c != "E")
        # Only a letter in DROPPED_LETTERS counts as dropped. `E` is not
        # dropped - it becomes the option bits below - and counting it here
        # said "60 with a dropped letter" for two rows that had one, which
        # is the instrument flattering itself.
        if any(c in DROPPED_LETTERS for c in row["flags"]):
            dropped += 1
        entry = dict(index=index, letters=letters, pattern=row["pattern"],
                     subject=row["subject"],
                     pair=(row["pattern"], row["subject"]))
        (ecma if "E" in row["flags"] else plain).append(entry)

    # Every flagged compile row is covered by a match row or named.
    compared = {(row["pattern"], pinned["cases"][row["index"]]["flags"])
                for row in plain + ecma}
    uncovered = []
    for index, row in enumerate(pinned["cases"]):
        if row["mode"] != "compile" or not row["flags"]:
            continue
        key = (row["pattern"], row["flags"])
        if key in compared or key in UNASKED_COMPILE:
            continue
        uncovered.append((index, key))
    for index, key in uncovered:
        print("case %d %r flags %r: a flagged compile row that no match row "
              "covers and UNASKED_COMPILE does not name"
              % (index, key[0], key[1]), file=sys.stderr)

    if not ecma:
        raise SystemExit("no ECMAScript rows: the option is untested here")
    if not plain:
        raise SystemExit("no plain flagged rows: the dialect's own alphabet "
                         "is untested here")

    failures = []
    passed = 0
    for rows, options in ((plain, ""), (ecma, ECMASCRIPT_OPTIONS)):
        for row, line in zip(rows, ask(args.driver, rows, options)):
            wanted = pinned["answers"][row["index"]]
            actual = parse_answer(line)
            if row["pair"] in EXCLUDED_PAIRS:
                # Both directions, `check_exclusions.py`'s rule: an exclusion
                # for a disagreement that has stopped happening is an
                # exclusion that has outlived its reason.
                if actual == wanted:
                    stale.append((row, actual, wanted))
                else:
                    pairs += 1
                continue
            if actual == wanted:
                passed += 1
            else:
                failures.append((row, actual, wanted))

    for row, actual, wanted in stale:
        print("case %d %r over %r: the exclusion is stale, both now say %s"
              % (row["index"], row["pattern"], row["subject"],
                 json.dumps(actual)), file=sys.stderr)
    failures += stale
    for row, actual, wanted in failures:
        print("case %d %r flags %r over %r: %s, the reference says %s"
              % (row["index"], row["pattern"],
                 pinned["cases"][row["index"]]["flags"], row["subject"],
                 json.dumps(actual), json.dumps(wanted)), file=sys.stderr)
    print("inventory %s; .NET flags: %d asked (%d of them ECMAScript), "
          "%d agree, %d excluded by letter, %d excluded as a named caseless "
          "pair, %d with a dropped letter, %d stale exclusion, "
          "%d disagreement, %d flagged compile row covered by nothing"
          % (dotnet_probe.DOTNET_INVENTORY_REVISION, len(plain) + len(ecma),
             len(ecma), passed, excluded, pairs, dropped, len(stale),
             len(failures), len(uncovered)))
    return bool(failures) or bool(uncovered)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, oracle_env.OracleUnavailable) as error:
        sys.stderr.write("dotnet_flags_diff: %s\n" % error)
        sys.exit(1)
