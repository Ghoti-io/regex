#!/usr/bin/env python3
"""Compare the Numeric_Value tables against perl, the only engine that has them.

`\\p{nv=...}` is Perl's alone: pcre2test 10.46 and V8 both refuse it as an
unknown property, so tools/oracle/property_diff.py - which asks Node - cannot
check a single one of its 144 values. This is the check that does, and
perl is the reference because it is the only implementation there is.

The comparison is by set membership rather than by value string. Perl will
report a code point's numeric value through Unicode::UCD::charprop, but as a
decimal: 1/3 comes back as 0.33333333, which DerivedNumericValues.txt's own
header warns is a repeating fraction printed to a fixed width. Two values
that differ past that width would compare equal. So each side is asked which
code points a value *matches*, which is exact on both sides.

The pinned perl carries an older UCD than the tables do - 15.0.0 against
17.0.0, see tools/corpus/VERSIONS - so a straight equality check would fail
on data rather than on a defect. The invariant checked instead is the one
that version skew cannot break:

  every code point perl gives a numeric value must get the same value here.

A code point this library assigns and perl calls NaN is skew in the safe
direction, counted and reported. A code point they both assign and disagree
about, or one perl assigns and this library does not, is a defect and fails.

Usage:
    tools/oracle/numeric_property_diff.py [--driver PATH] [--examples N]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import os
import subprocess
import sys

import oracle_env

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# Asks perl, for each value on stdin, which code points match it, and for the
# whole set which code points have any numeric value at all. Ranges out, the
# same shape the C driver prints, so that both sides parse the same way.
#
# The candidate universe is `\P{nv=NaN}` - under two thousand code points -
# rather than the whole code space, because a value tested against 1.1
# million code points 144 times over is a minute of work to learn nothing:
# no code point outside that set can match any value.
PERL_SOURCE = r"""
use strict; use warnings;
my @values = map { chomp; $_ } <STDIN>;
my @universe = grep { chr($_) =~ /\P{nv=NaN}/ } 0 .. 0x10FFFF;
print "universe\t", join(" ", map { sprintf "%X", $_ } @universe), "\n";
for my $value (@values) {
    my $re = eval { qr{\p{nv=$value}} };
    if ($@) { print "$value\tunknown\n"; next; }
    my @hit = grep { chr($_) =~ $re } @universe;
    print "$value\t", join(" ", map { sprintf "%X", $_ } @hit), "\n";
}
"""


def ask_library(driver, values):
    """The code points this library gives each value, as a set."""
    lines = "".join(value + "\n" for value in values)
    finished = subprocess.run([driver, "--perl"], input=lines,
        capture_output=True, text=True, check=True)

    result = {}
    for line in finished.stdout.splitlines():
        fields = line.split("\t")
        if len(fields) != 2:
            continue
        if fields[1] == "unknown":
            result[fields[0]] = None
            continue
        points = set()
        for pair in fields[1].split():
            low, high = pair.split("-")
            points.update(range(int(low, 16), int(high, 16) + 1))
        result[fields[0]] = points
    return result


def ask_perl(values):
    """The code points perl gives each value, and every numeric code point."""
    names = [value.split("=", 1)[1] for value in values]
    finished = subprocess.run(
        oracle_env.command("perl", ["perl", "-e", PERL_SOURCE]),
        input="".join(name + "\n" for name in names),
        capture_output=True, text=True, check=True)

    universe = set()
    result = {}
    for line in finished.stdout.splitlines():
        fields = line.split("\t")
        if len(fields) != 2:
            continue
        if fields[0] == "universe":
            universe = set(int(point, 16) for point in fields[1].split())
            continue
        if fields[1] == "unknown":
            result["nv=" + fields[0]] = None
            continue
        result["nv=" + fields[0]] = set(
            int(point, 16) for point in fields[1].split())
    return universe, result


def find_driver(explicit):
    if explicit:
        return explicit
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(ROOT, "build", platform, build, "apps",
                "tools", "grx_properties")
            if os.path.exists(path):
                return path
    return None


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", default=None)
    parser.add_argument("--examples", type=int, default=5)
    args = parser.parse_args(argv[1:])

    driver = find_driver(args.driver)
    if not driver or not os.path.exists(driver):
        sys.stderr.write(
            "the grx_properties tool was not found; run `make tools` first\n")
        return 2
    # No `command -v perl` here, and nothing that turns a missing reference
    # into `return 0`. The reference is resolved and versioned by
    # tools/oracle/oracle_run.py before this runs, and `oracle_env.command`
    # raises rather than falling back if it cannot be reached - so a run that
    # gets this far has a perl whose version has been checked against
    # tools/oracle/containers/IMAGES. What used to be here asked whether
    # something called perl was on PATH and exited 0 when it was not.

    values = subprocess.run([driver, "--list-numeric"], capture_output=True,
        text=True, check=True).stdout.split()
    ours = ask_library(driver, values)
    universe, theirs = ask_perl(values)

    disagreements = []
    absent = 0
    compared = 0

    # Where this library and perl both give a code point a value, it has to be
    # the same value.
    covered = set()
    for value in values:
        mine = ours.get(value) or set()
        reference = theirs.get(value)
        if reference is None:
            # perl has no such value at all: every code point carrying it was
            # given one by a UCD newer than perl's.
            absent += 1
            continue
        compared += 1
        covered |= reference
        wrong = reference - mine
        if wrong:
            disagreements.append((value, "perl gives it this value", wrong))

    # And every code point perl gives a value to has to have been claimed by
    # one of the values this library knows. A code point in perl's universe
    # that no value above accounted for is a value missing from the tables.
    unclaimed = universe - covered
    if unclaimed:
        disagreements.append(
            ("(any)", "perl gives these a value no table here claims",
             unclaimed))

    ours_universe = set()
    for value in values:
        ours_universe |= ours.get(value) or set()

    for value, why, points in disagreements[:args.examples]:
        print("\\p{%s}: %s" % (value, why))
        print("  " + " ".join("U+%04X" % c for c in sorted(points)[:8])
              + (" ..." if len(points) > 8 else ""))

    print("\n%d values, %d compared, %d perl's UCD does not have, "
          "%d code points here that perl calls NaN, %d disagreements"
          % (len(values), compared, absent,
             len(ours_universe - universe), len(disagreements)))
    return 1 if disagreements else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
