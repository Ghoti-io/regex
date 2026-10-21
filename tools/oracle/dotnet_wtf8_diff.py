#!/usr/bin/env python3
r"""Compare pinned .NET answers over subjects holding an unpaired surrogate.

**The axis the inventory did not ask at all.** The driver has had a `w:`
mode since it was written - `check_dotnet_driver.py` exercises the
mid-character refusal at both its UTF-8 and WTF-8 sites - and no probe row
used it, so .NET treating a lone surrogate as ordinary text was measured
nowhere. That is also why the condition-2 sweep could not catch
`GRX_Profile::subject_is_text` or `subject_is_utf16`: both govern exactly
this, and there were no rows for them to govern. Unasked, not unobservable.

A .NET string is UTF-16 and a lone surrogate is a legal `char`, so these
answer rather than erroring. This library reads such a subject under
GRX_OPT_WTF8, which grx_match spells `W`, and reports **WTF-8 byte offsets**
- three bytes per surrogate - which is what makes the comparison direct
rather than needing a translation table.

The rows deliberately include `\p{Cs}` matching a surrogate and `\p{L}`
declining one, because that pair is the whole reason .NET's `\p{...}` cannot
see a character above U+FFFF: the engine is looking at two surrogates, and a
surrogate's category is `Cs`. Section 6's .NET row states that, and until
now it rested on measurements taken by hand.

A surrogate *pair* is still required to arrive as the four-byte scalar: this
is WTF-8 and not CESU-8, so `..` over an astral character plus "x" is one
row here to pin that the pair is one character and not two.
"""
import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dotnet_probe
import oracle_env
import wtf8 as wtf8_codec

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "tests", "data", "probe")


# Keyed by (pattern, subject), and checked in both directions. The one row
# here is section 6's .NET deviation rather than a defect of this gate's
# finding: `..` takes two UTF-16 *code units* on .NET, which over a
# character above U+FFFF is that character's two halves and four WTF-8
# bytes, where this library takes two *code points* - the character and the
# "x" after it - and five. WP-49 is the package; the row stays because the
# deviation is worth a committed measurement and not only a paragraph.
EXCLUDED = {
    ("..", "\U00010000x"):
        "code units against code points: .NET's `..` is the surrogate pair "
        "and this library's is the character plus the next one. "
        "documentation/dialects.md section 6, WP-49",
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
               if row["mode"] == "w:first"
               and pinned["answers"][i].get("kind") in ("match", "nomatch")]
    if not indices:
        raise SystemExit("no .NET WTF-8 rows: the axis is untested here")

    # `W` is GRX_OPT_WTF8 and brings GRX_OPT_UTF with it, which the driver
    # lifts out of the flag string before the dialect's own alphabet sees it.
    # The text fields are WTF-8 and not UTF-8, the same encoding the probe
    # sent the reference, because the encoding and the offsets reported are
    # one decision.
    payload = "".join(
        "W\t%s\t%s\n"
        % (wtf8_codec.encode(pinned["cases"][i]["pattern"]).hex(),
           wtf8_codec.encode(pinned["cases"][i]["subject"]).hex())
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
        actual = parse_answer(line)
        wanted = pinned["answers"][index]
        row = pinned["cases"][index]
        key = (row["pattern"], row["subject"])
        if key in EXCLUDED:
            # An exclusion that has stopped being a disagreement is an
            # exclusion that has outlived its reason - and here it would
            # mean WP-49 had landed, which is a thing to notice rather than
            # to pass over.
            if actual == wanted:
                stale += 1
                failures.append((index, actual, wanted))
            else:
                excluded += 1
            continue
        if actual == wanted:
            passed += 1
        else:
            failures.append((index, actual, wanted))

    for index, actual, wanted in failures:
        print("case %d %r over %r: %s, the reference says %s"
              % (index, pinned["cases"][index]["pattern"],
                 pinned["cases"][index]["subject"], json.dumps(actual),
                 json.dumps(wanted)), file=sys.stderr)
    print("inventory %s; .NET WTF-8: %d asked, %d agree, %d excluded and "
          "named, %d stale exclusion, %d disagreement"
          % (dotnet_probe.DOTNET_INVENTORY_REVISION, len(indices), passed,
             excluded, stale, len(failures)))
    return bool(failures)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, oracle_env.OracleUnavailable) as error:
        sys.stderr.write("dotnet_wtf8_diff: %s\n" % error)
        sys.exit(1)
