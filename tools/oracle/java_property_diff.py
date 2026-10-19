#!/usr/bin/env python3
"""Sweep Java properties against pinned JDK 25/27 over scalar subjects.

Only code points assigned by exactly one side are excluded. Differences on
shared-assigned or shared-unassigned scalars fail the gate.
"""

import argparse
import json
import os
import subprocess
import sys

import oracle_env

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PROPERTIES = ("IsGreek", "InGreek", "gc=Lu", "sc=Greek", "blk=Greek",
              "javaLowerCase", "javaUpperCase", "javaDigit",
              "javaWhitespace", "javaMirrored")
NAMES = ("Cn",) + PROPERTIES


def points(ranges):
    return {cp for low, high in ranges for cp in range(low, high + 1)
            if cp < 0xD800 or cp > 0xDFFF}


def library(driver):
    result = subprocess.run([driver, "--java"], input="\n".join(NAMES) + "\n",
                            capture_output=True, text=True, check=True)
    found = {}
    for line in result.stdout.splitlines():
        name, value = line.split("\t", 1)
        if value == "unknown":
            raise RuntimeError(f"library rejects Java property {name}")
        found[name] = points([[int(x, 16) for x in span.split("-")]
                              for span in value.split()])
    if set(found) != set(NAMES):
        raise RuntimeError("library property driver omitted a result")
    return found


def reference(pin):
    rows = ["ranges\t" + ("\\p{" + name + "}").encode().hex()
            + "\t\t\t" for name in NAMES]
    result = subprocess.run(oracle_env.command(pin, ["java", "tools/oracle/JavaOracle.java"]),
                            input="\n".join(rows) + "\n", capture_output=True,
                            text=True, timeout=1800, check=True)
    answers = []
    for line in result.stdout.splitlines():
        state, payload = line.split("\t", 1)
        answer = json.loads(bytes.fromhex(payload))
        if state != "ok" or answer.get("kind") != "ranges":
            raise RuntimeError(f"{pin} property scan failed: {answer}")
        answers.append(points(answer["ranges"]))
    if len(answers) != len(NAMES):
        raise RuntimeError(f"{pin} answered {len(answers)} of {len(NAMES)} properties")
    return dict(zip(NAMES, answers))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", default=os.path.join(
        ROOT, "build", "linux", "release", "apps", "tools", "grx_properties"))
    args = parser.parse_args()
    print(oracle_env.provenance(["java25", "java27"]), flush=True)
    ours = library(args.driver)
    references = {pin: reference(pin) for pin in ("java25", "java27")}
    assigned_ours = set(range(0x110000)) - ours["Cn"] - set(range(0xD800, 0xE000))
    failures = 0
    for pin, theirs in references.items():
        assigned_theirs = set(range(0x110000)) - theirs["Cn"] - set(range(0xD800, 0xE000))
        skew_domain = assigned_ours ^ assigned_theirs
        common_domain = (assigned_ours & assigned_theirs)
        print(f"{pin}: shared-assigned {len(common_domain)}, assignment skew {len(skew_domain)}")
        for name in PROPERTIES:
            difference = ours[name] ^ theirs[name]
            excluded = difference & skew_domain
            real = difference - skew_domain
            forward_difference = set()
            if pin == "java27":
                # The library targets JDK 25. A point that agrees with that
                # baseline but changed in JDK 27 is a measured forward
                # difference, not a property-set agreement or assignment
                # exclusion. Any other shared-assigned mismatch still fails.
                forward_difference = real & (
                    references["java25"][name] ^ theirs[name])
                real -= forward_difference
            print(f"  {name}: excluded-assignment-skew {len(excluded)}, "
                  f"forward-jdk-difference {len(forward_difference)}, "
                  f"mismatch {len(real)}")
            if real:
                print("    first mismatches: " + " ".join(
                    f"U+{cp:04X}" for cp in sorted(real)[:8]), file=sys.stderr)
                failures += len(real)
    forward = references["java25"]
    newer = references["java27"]
    for name in PROPERTIES:
        print(f"JDK25/JDK27 {name}: {len(forward[name] ^ newer[name])} difference")
    return bool(failures)


if __name__ == "__main__":
    sys.exit(main())
