#!/usr/bin/env python3
"""Compare every Unicode property table against a reference implementation.

The most thorough check the Unicode tables can be given, and the one
documentation/plan.md WP-09 calls "the real check on WP-02". For each
property this library accepts, it asks both implementations which code points
match `\\p{...}` - all 1,114,112 of them - and reports the first differences.

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
    unsupported = 0

    for name in names:
        theirs = reference.get(name)
        mine = ours.get(name)
        if theirs == "unsupported" or mine == "unsupported":
            # The reference does not have this spelling. That is not a
            # disagreement about the *data*: this library's loose resolver
            # accepts names ECMAScript does not, and syntax_diff.py is what
            # checks which spellings each side accepts.
            unsupported += 1
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

    print("\n%d properties, %d compared, %d the reference does not spell, "
          "%d disagreements"
          % (len(names), compared, unsupported, len(disagreements)))
    return 1 if disagreements else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
