#!/usr/bin/env python3
"""Compare every Unicode property table against a reference implementation.

The most thorough check the Unicode tables can be given, and the one
documentation/plan.md WP-09 calls "the real check on WP-02". For each
property this library accepts, it asks both implementations which code points
match `\\p{...}` - all 1,114,112 of them - and reports the first differences.

A name one side cannot spell is reported by *which* side could not, in three
buckets rather than one. That matters more than it sounds: the single bucket
this replaced was printed as "the reference does not spell", and every one of
the 16 names in it is refused by this library too - so the line named node
for a gap both share, and the third case, a name the reference resolves and
this library does not, had nowhere to be reported and would have been counted
as agreement.

This is stronger than importing test262's generated property-escapes files,
and needs nothing cloned. Those files are themselves generated from the UCD,
so they check that a table agrees with the UCD; this checks that it agrees
with the UCD *as a shipping engine reads it*, which is the question a
conformance rate is about. It also covers every property rather than the
subset test262 happens to have generated.

Both sides answer in ranges rather than per code point, so a property costs
one pass over the code space on each side and the comparison is on the range
arrays.

Usage:
    tools/oracle/property_diff.py [--driver PATH] [--limit N]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import json
import os
import subprocess
import sys
import node_runner

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

MAX_CODEPOINT = 0x10FFFF

NODE_SOURCE = r"""
const names = JSON.parse(require("fs").readFileSync(0, "utf8"));
const out = {};
for (const name of names) {
  let regex;
  try {
    regex = new RegExp("\\p{" + name + "}", "u");
  }
  catch {
    out[name] = "unsupported";
    continue;
  }
  const ranges = [];
  let start = -1;
  for (let c = 0; c <= 0x10FFFF; c++) {
    // Surrogates cannot be tested: String.fromCodePoint gives a lone
    // surrogate, which no property matches and which UTF-8 cannot hold
    // either, so both sides agree by not being asked.
    const inside = (c >= 0xD800 && c <= 0xDFFF)
      ? false
      : regex.test(String.fromCodePoint(c));
    if (inside && start < 0) {
      start = c;
    }
    else if (!inside && start >= 0) {
      ranges.push([start, c - 1]);
      start = -1;
    }
  }
  if (start >= 0) {
    ranges.push([start, 0x10FFFF]);
  }
  out[name] = ranges;
}
process.stdout.write(JSON.stringify(out));
"""


def drop_surrogates(ranges):
    """Remove U+D800..U+DFFF, which neither side can be asked about."""
    out = []
    for low, high in ranges:
        if high < 0xD800 or low > 0xDFFF:
            out.append([low, high])
            continue
        if low < 0xD800:
            out.append([low, 0xD7FF])
        if high > 0xDFFF:
            out.append([0xE000, high])
    return out


def ask_library(driver, names):
    lines = "".join(name + "\n" for name in names)
    finished = subprocess.run([driver], input=lines, capture_output=True,
        text=True, check=True)

    result = {}
    for line in finished.stdout.splitlines():
        fields = line.split("\t")
        if len(fields) != 2:
            continue
        if fields[1] == "unknown":
            result[fields[0]] = "unsupported"
            continue
        ranges = []
        for pair in fields[1].split():
            low, high = pair.split("-")
            ranges.append([int(low, 16), int(high, 16)])
        result[fields[0]] = ranges
    return result


def ask_node(names):
    finished = subprocess.run(node_runner.command("-e", NODE_SOURCE),
        input=json.dumps(names), capture_output=True, text=True, check=True)
    return json.loads(finished.stdout)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", default=None)
    parser.add_argument("--limit", type=int, default=0,
        help="check only the first N properties, for a quick run")
    parser.add_argument("--examples", type=int, default=5)
    args = parser.parse_args(argv[1:])

    driver = args.driver
    if not driver:
        for platform in ("linux", "mac", "win64", "win32"):
            for build in ("release", "debug"):
                path = os.path.join(ROOT, "build", platform, build, "apps",
                    "tools", "grx_properties")
                if os.path.exists(path):
                    driver = path
                    break
            if driver:
                break
    if not driver or not os.path.exists(driver):
        sys.stderr.write(
            "the grx_properties tool was not found; run `make tools` first\n")
        return 2

    names = subprocess.run([driver, "--list"], capture_output=True, text=True,
        check=True).stdout.split()
    if args.limit:
        names = names[:args.limit]

    ours = ask_library(driver, names)
    reference = ask_node(names)

    disagreements = []
    compared = 0
    neither = []
    reference_only = []
    ours_only = []

    for name in names:
        theirs = reference.get(name)
        mine = ours.get(name)
        # A name that one side cannot spell yields no comparison, and *which*
        # side could not is three different findings. This used to be one
        # counter printed as "the reference does not spell", which named node
        # for a gap that is mostly shared: all 16 of the names it covered are
        # refused here too, so the line blamed the reference for something
        # this library does equally. An exclusion bucket that names the wrong
        # cause is one nobody thinks to look inside.
        if theirs == "unsupported" and mine == "unsupported":
            neither.append(name)
            continue
        if theirs == "unsupported":
            reference_only.append(name)
            continue
        if mine == "unsupported":
            ours_only.append(name)
            continue

        compared += 1
        theirs = drop_surrogates(theirs)
        mine = drop_surrogates(mine)
        if theirs != mine:
            disagreements.append((name, theirs, mine))

    for name, theirs, mine in disagreements[:args.examples]:
        print("\\p{%s}" % name)
        print("  reference: %d ranges, %d code points"
              % (len(theirs), sum(h - l + 1 for l, h in theirs)))
        print("  ours:      %d ranges, %d code points"
              % (len(mine), sum(h - l + 1 for l, h in mine)))
        # The first code point they differ about, which is what a fix starts
        # from.
        theirs_set = set()
        mine_set = set()
        for low, high in theirs:
            theirs_set.update(range(low, min(high, low + 0x20000) + 1))
        for low, high in mine:
            mine_set.update(range(low, min(high, low + 0x20000) + 1))
        only_theirs = sorted(theirs_set - mine_set)[:4]
        only_mine = sorted(mine_set - theirs_set)[:4]
        if only_theirs:
            print("  only in the reference: "
                  + " ".join("U+%04X" % c for c in only_theirs))
        if only_mine:
            print("  only here:             "
                  + " ".join("U+%04X" % c for c in only_mine))

    # `ours_only` is the one of the three that is a defect. `--list` prints
    # this library's own property table, so a name in it that the reference
    # resolves and this library does not is a gap here by construction -
    # there is no benign reading of it, and it is 0 today.
    if ours_only:
        print("\nnames the reference resolves and this library does not:")
        for name in ours_only:
            print("  \\p{%s}" % name)

    print("\n%d properties, %d compared, %d neither side spells, "
          "%d only the reference refuses, %d only this library refuses, "
          "%d disagreements"
          % (len(names), compared, len(neither), len(reference_only),
             len(ours_only), len(disagreements)))
    if neither:
        # Named rather than counted, because the count alone reads as the
        # reference's gap. These are records in this library's property table
        # that no pattern in any dialect here can reach: real UCD properties
        # outside ECMA-262's closed binary list, which the strict resolver is
        # right to reject and the loose table inherits the rejection from.
        # `check-unicode-agreement` still compares their code points, because
        # it walks the table by index rather than by name.
        print("  neither side spells: " + " ".join(neither))
    if reference_only:
        print("  only the reference refuses: " + " ".join(reference_only))
    return 1 if disagreements or ours_only else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
