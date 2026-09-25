#!/usr/bin/env python3
"""Compare this library's case-fold orbits against perl's, as partitions.

What this adds over check-oracle-properties, which already compares 441
property tables with no disagreement: that gate checks *which* code points
fold - Changes_When_Casefolded is one of the 441 - and nothing checked which
ones fold *together*. A table that folded U+0041 to U+0062 would pass every
property comparison in the suite.

Two things have to be controlled or this measures something else:

  * **Folding depth.** perl's fc() is the *full* casefold and this library's
    orbit table is the *simple* one, which is not a disagreement but two
    different questions. A code point whose only fold is multi-code-point -
    U+00DF to "ss", U+0149 to U+02BC U+006E - is a singleton here and a
    group member there. They are excluded by asking perl which keys are
    multi-code-point, never by a list in this file: a list would go stale
    silently at the next UCD, which is how an exclusion bucket starts
    absorbing defects.

  * **Unicode version.** This library is UCD 17.0.0 and perl 5.40.1 is
    older, so it has never heard of some of these code points and reports
    them as unassigned rather than as uncased. Every comparison is
    restricted to what perl calls assigned. Unrestricted, 110 code points
    differ and 106 of them are simply newer, which measures the release
    rather than the tables.

Exit 1 on any disagreement, and print the counts either way - a "0
disagreements" with no denominator beside it says nothing.
"""

import argparse
import os
import subprocess
import sys

import oracle_env

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# fc() is the full casefold, so grouping by it is the case-equivalence
# partition without asking the regex engine anything - one pass, no pattern
# compiled, and nothing that a matching bug could bend. \p{Cn} comes back in
# the same pass so the version restriction costs nothing extra.
PERL_PROGRAM = r"""
use strict; use warnings; use feature 'fc';
for my $cp (0 .. 0x10FFFF) {
  next if $cp >= 0xD800 && $cp <= 0xDFFF;
  my $s = chr($cp); utf8::upgrade($s);
  my $unassigned = ($s =~ /\p{Cn}/) ? 1 : 0;
  my $k = fc($s);
  next if $k eq $s && $unassigned == 0;
  printf("%X %d %s\n", $cp, $unassigned,
      join(",", map { sprintf "%X", ord } split //, $k));
}
"""


def perl_partition():
    """Perl's fold groups, its multi-key members, and its unassigned set."""
    done = subprocess.run(oracle_env.command("perl", ["perl", "-e", PERL_PROGRAM]),
                          capture_output=True, text=True)
    if done.returncode != 0:
        sys.stderr.write("perl failed: %s\n" % done.stderr[:400])
        return None, None, None

    groups = {}
    unassigned = set()
    for line in done.stdout.splitlines():
        code, flag, key = line.split()
        codepoint = int(code, 16)
        if flag == "1":
            unassigned.add(codepoint)
        if key != "%X" % codepoint:
            groups.setdefault(key, set()).add(codepoint)

    # A fold target never appears as a source - "a" folds to itself - so the
    # single-code-point keys join their own groups here.
    multi = set()
    for key, members in groups.items():
        parts = key.split(",")
        if len(parts) == 1:
            members.add(int(parts[0], 16))
        else:
            multi |= members

    partition = {}
    for members in groups.values():
        frozen = frozenset(members)
        for member in members:
            partition[member] = frozen
    return partition, multi, unassigned


def our_partition(driver):
    done = subprocess.run([driver], capture_output=True, text=True)
    if done.returncode != 0:
        sys.stderr.write("%s failed\n" % driver)
        return None
    partition = {}
    for line in done.stdout.splitlines():
        parts = [int(x, 16) for x in line.split()]
        partition[parts[0]] = frozenset(parts[1:])
    return partition


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", required=True)
    parser.add_argument("--examples", type=int, default=20)
    args = parser.parse_args(argv[1:])

    if not os.path.exists(args.driver):
        sys.stderr.write("run `make tools` first\n")
        return 2
    theirs, multi, unassigned = perl_partition()
    if theirs is None:
        return 2
    ours = our_partition(args.driver)
    if ours is None:
        return 2

    # Restrict once, here, so no comparison below can forget to - and
    # restrict on the whole *orbit*, not on the code point.
    #
    # Restricting the code point alone is not enough, and the difference is
    # not academic: U+019B is assigned in perl 5.40.1 and its partner
    # U+A7DC is not, so perl cannot know the pair exists and calls U+019B
    # uncased. Four code points reported as disagreements that way, every
    # one of them a UCD 17.0.0 addition. A relation is only comparable when
    # the reference has heard of both ends of it.
    def comparable(codepoint):
        if codepoint in multi:
            return False
        orbit = ours.get(codepoint) or theirs.get(codepoint) or {codepoint}
        return not (orbit & unassigned)

    ours_keys = {c for c in ours if comparable(c)}
    their_keys = {c for c in theirs if comparable(c)}
    skipped_version = len([c for c in set(ours) | set(theirs)
                           if c not in multi and not comparable(c)])

    disagreements = []
    for codepoint in sorted(ours_keys | their_keys):
        mine = ours.get(codepoint)
        yours = theirs.get(codepoint)
        if mine == yours:
            continue
        disagreements.append((codepoint, mine, yours))

    def spell(orbit):
        if orbit is None:
            return "alone"
        return "{%s}" % " ".join("U+%04X" % c for c in sorted(orbit))

    for codepoint, mine, yours in disagreements[:args.examples]:
        print("  U+%04X  perl=%-28s ours=%s"
              % (codepoint, spell(yours), spell(mine)))

    orbits = len(set(ours[c] for c in ours_keys))
    print("folds: %d code points in a shared orbit, %d orbits, %d compared "
          "against perl, %d skipped as a multi-code-point fold, %d skipped "
          "as unassigned there, %d disagreements"
          % (len(ours), orbits, len(ours_keys | their_keys), len(multi),
             skipped_version, len(disagreements)))
    return 1 if disagreements else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
