#!/usr/bin/env python3
r"""Sweep every POSIX class and `\w` against perl and pcre2, code point by
code point.

`[[:alpha:]]` and its thirteen siblings are **table rules**, and a table rule
is not tested by examples. Fourteen of them were swept by hand on 2026-09-24
and three were wrong - `punct` held 7,766 code points neither reference has,
`graph` was missing 164, and `word` was missing 236 because a comment
paraphrased UTS #18 Annex C's `\p{alpha}` as "letters" and the code followed
the paraphrase. None of the three was visible from the conformance corpus:
37,212 cases hold no letter-number, no format character inside
`[[:graph:]]` and no non-ASCII symbol.

That sweep was a script somebody ran once. This is it as a gate, and the
difference matters for a second reason: **the figures it produced went stale**.
dialects.md section 5.9 quotes how far perl's answer and PCRE2's are apart -
1,694 code points for `alpha`, 1,513 for `\w`, 137,468 for `graph` - over an
intersection of 286,719 code points that perl 5.40.1 defined, and says in the
same paragraph "re-take the figures before quoting them". A number whose own
page says that is a number with no instrument behind it.

**Each dialect is compared against its own reference, and that is the
assertion.** The two references disagree with each other about three things,
so one shared set cannot satisfy both: pcre2's `\w` takes `\p{No}` and perl's
does not, perl's takes `Mc`, `Me` and alphabetic `So` and pcre2's does not,
and perl's `[[:graph:]]` counts private use where pcre2's does not. This
library carries those as a profile axis - `GRX_WordSet` and
`posix_graph_takes_private_use` - so `perl` must answer perl's set exactly and
`pcre` must answer pcre2's, and a row where either does not is a defect rather
than a difference of opinion.

**Restricted to what all three call assigned.** Unrestricted, a sweep measures
the Unicode release: this library is UCD 17.0.0 and pcre2 10.46 carries 16.0.0,
so thousands of code points exist for one side and not the other and every one
would read as a disagreement. The restriction is measured rather than assumed -
each side is asked for its own `\p{Cn}` - and its size is printed, because it
is the denominator every figure below is over.

Usage:
    tools/oracle/wide_class_diff.py [--driver PATH] [--class NAME ...]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

sys.path.insert(0, HERE)

import oracle_env

MAX = 0x10FFFF

# The classes, and which dialect of ours answers for which reference. `\w` is
# in the list because it is the same table read through a different spelling
# and the two must not be allowed to drift: `[[:word:]]` and `\w` returned the
# identical 139,612 code points in perl and the identical 139,929 in pcre2test
# when that was last measured, and this is what keeps saying so.
CLASSES = ["alnum", "alpha", "ascii", "blank", "cntrl", "digit", "graph",
           "lower", "print", "punct", "space", "upper", "word", "xdigit", "w"]

# `w` is `\w` and is spelled without the backslash everywhere in the protocol,
# so that no caller has to quote one. This is the only place it is displayed.
def shown(name):
    return "\\w" if name == "w" else name

# How far the two references are apart, per class, and the denominator.
#
# Held here **and** quoted in dialects.md section 5.9, which is the point: a
# figure with no instrument beside it is unfalsifiable at the point of reading,
# and three of these were quoted for two days after the pin that produced them
# moved. The gate fails when a measurement stops matching, so raising a pin
# turns the prose red instead of leaving it plausible.
#
# Re-taken 2026-09-26 against perl 5.44.0 and pcre2 10.46.
ASSIGNED_IN_ALL_THREE = 292531
EXPECTED_DIFFERENCE = {
    "alnum": 2410, "alpha": 1731, "ascii": 0, "blank": 1, "cntrl": 0,
    "digit": 0, "graph": 137474, "lower": 312, "print": 137473, "punct": 0,
    "space": 1, "upper": 120, "word": 1528, "xdigit": 0, "w": 1528,
}

PERL_PROGRAM = r'''
use strict; use warnings;
# One class per argument; the whole plane for each, as runs.
#
# `utf8::upgrade` on every subject. Without it a one-character string below
# U+0100 is a byte string and perl gives it ASCII semantics, so `\w` would
# answer for the wrong alphabet over the first 256 code points - which is a
# defect this repository has recorded before (notes: a probe subject below
# U+0100 gets ASCII semantics).
for my $name (@ARGV) {
  my $regex = $name eq 'w' ? qr/^\w$/u : qr/^[[:$name:]]$/u;
  my $start = -1;
  for my $cp (0 .. 0x110000) {
    my $member = 0;
    if ($cp <= 0x10FFFF) {
      my $c = chr($cp);
      utf8::upgrade($c);
      $member = $c =~ $regex ? 1 : 0;
    }
    if ($member && $start < 0) { $start = $cp; }
    elsif (!$member && $start >= 0) {
      printf "%s %X %X\n", $name, $start, $cp - 1;
      $start = -1;
    }
  }
}
'''

GC_PROGRAM = r'''
use strict; use warnings;
use Unicode::UCD qw(charinfo);
# perl's own answer for the category, from its own tables, so that "the
# reference is older" is a reading rather than an assumption.
for my $hex (@ARGV) {
  my $cp = hex $hex;
  my $info = charinfo($cp);
  printf "gc %X %s\n", $cp, $info ? $info->{category} : "?";
}
'''

ASSIGNED_PROGRAM = r'''
use strict; use warnings;
my $start = -1;
for my $cp (0 .. 0x110000) {
  my $member = 0;
  if ($cp <= 0x10FFFF) {
    my $c = chr($cp);
    utf8::upgrade($c);
    $member = $c =~ /^\P{Cn}$/ ? 1 : 0;
  }
  if ($member && $start < 0) { $start = $cp; }
  elsif (!$member && $start >= 0) {
    printf "assigned %X %X\n", $start, $cp - 1;
    $start = -1;
  }
}
'''

PCRE2_ASSIGNED = r'''
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <stdio.h>
static size_t encode(unsigned c, char * out) {
  if (c < 0x80) { out[0] = (char)c; return 1; }
  if (c < 0x800) { out[0] = (char)(0xC0 | (c >> 6));
    out[1] = (char)(0x80 | (c & 0x3F)); return 2; }
  if (c < 0x10000) { out[0] = (char)(0xE0 | (c >> 12));
    out[1] = (char)(0x80 | ((c >> 6) & 0x3F));
    out[2] = (char)(0x80 | (c & 0x3F)); return 3; }
  out[0] = (char)(0xF0 | (c >> 18)); out[1] = (char)(0x80 | ((c >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((c >> 6) & 0x3F)); out[3] = (char)(0x80 | (c & 0x3F));
  return 4;
}
int main(void) {
  int error; PCRE2_SIZE where;
  pcre2_code * code = pcre2_compile((PCRE2_SPTR)"^\\P{Cn}$",
      PCRE2_ZERO_TERMINATED, PCRE2_UTF | PCRE2_UCP, &error, &where, NULL);
  if (!code) { return 1; }
  pcre2_match_data * data = pcre2_match_data_create_from_pattern(code, NULL);
  long start = -1;
  for (unsigned cp = 0; cp <= 0x110000; cp++) {
    int member = 0;
    if (cp <= 0x10FFFF) {
      char bytes[4]; size_t n = encode(cp, bytes);
      member = pcre2_match(code, (PCRE2_SPTR)bytes, n, 0, 0, data, NULL) >= 0;
    }
    if (member && start < 0) { start = (long)cp; }
    else if (!member && start >= 0) {
      printf("assigned %lX %lX\n", start, (long)cp - 1); start = -1;
    }
  }
  return 0;
}
'''


def runs_to_set(text, wanted=None):
    """`<name> <lo> <hi>` lines into {name: set of code points}."""
    sets = {}
    for line in text.split("\n"):
        parts = line.split()
        if len(parts) != 3:
            continue
        name, lo, hi = parts[0], int(parts[1], 16), int(parts[2], 16)
        if wanted is not None and name not in wanted:
            continue
        sets.setdefault(name, set()).update(range(lo, hi + 1))
    return sets


def ask_perl(classes):
    # `w` rather than `\w` on the command line: the shell inside the container
    # is not in the way here (oracle_env passes argv through), but a leading
    # backslash in an argument is one fewer thing to get wrong than it is
    # worth, and the program maps the name back.
    finished = subprocess.run(
        oracle_env.command("perl", ["perl", "-e", PERL_PROGRAM]
            + list(classes)),
        capture_output=True, text=True)
    if finished.returncode:
        sys.stderr.write(oracle_env.reference_stderr(finished.stderr))
        return None
    return runs_to_set(finished.stdout, set(classes))


def ask_perl_assigned():
    finished = subprocess.run(
        oracle_env.command("perl", ["perl", "-e", ASSIGNED_PROGRAM]),
        capture_output=True, text=True)
    if finished.returncode:
        sys.stderr.write(oracle_env.reference_stderr(finished.stderr))
        return None
    return runs_to_set(finished.stdout).get("assigned", set())


def ask_pcre2(classes, source, arguments):
    """Compile `source` inside the pinned image and run it there."""
    path = "/tmp/%s" % os.path.basename(source).replace(".c", "")
    build = "cc -std=c17 -O2 -o %s %s -lpcre2-8" % (path, source)
    script = "%s && %s %s" % (build, path, " ".join(arguments))
    finished = subprocess.run(
        oracle_env.command("pcre2", ["sh", "-c", script]),
        capture_output=True, text=True)
    if finished.returncode:
        sys.stderr.write(oracle_env.reference_stderr(finished.stderr))
        return None
    return finished.stdout


def ask_ours(driver, dialect, classes):
    finished = subprocess.run([driver, dialect] + classes,
        capture_output=True, text=True)
    if finished.returncode:
        sys.stderr.write(finished.stderr)
        return None
    return runs_to_set(finished.stdout, set(classes))


def categories(text):
    """`gc <hex> <Xx>` lines into {code point: category}."""
    out = {}
    for line in text.split("\n"):
        parts = line.split()
        if len(parts) == 3 and parts[0] == "gc":
            out[int(parts[1], 16)] = parts[2]
    return out


def version_difference(reference, driver, dialect, points):
    r"""Which of these code points the reference and this library *categorise*
    differently.

    The one exclusion this gate has, and it is a second measurement rather
    than a rule: a class difference is a difference of rule only where both
    sides agree about the code point's General_Category. Where they do not,
    the reference is carrying an older UCD and the comparison is measuring the
    release. U+0295 is the case it was written for - `Ll` in perl 5.44 and in
    pcre2 10.46, `Lo` in UCD 17.0.0 - and node, whose Unicode is also 17.0,
    sides with this library.

    Returns the subset to excuse, and prints each one with both categories, so
    that a row excused here can be read rather than taken on trust.
    """
    if not points:
        return set()
    hexes = ["%X" % point for point in sorted(points)]
    if reference == "perl":
        finished = subprocess.run(
            oracle_env.command("perl", ["perl", "-e", GC_PROGRAM] + hexes),
            capture_output=True, text=True)
        if finished.returncode:
            sys.stderr.write(oracle_env.reference_stderr(finished.stderr))
            return set()
        theirs = categories(finished.stdout)
    else:
        text = ask_pcre2([], os.path.join(HERE, "pcre2_classes.c"),
            ["gc"] + hexes)
        if text is None:
            return set()
        theirs = categories(text)
    finished = subprocess.run([driver, dialect, "gc"] + hexes,
        capture_output=True, text=True)
    if finished.returncode:
        sys.stderr.write(finished.stderr)
        return set()
    mine = categories(finished.stdout)

    excused = set()
    for point in sorted(points):
        their_gc = theirs.get(point, "?")
        my_gc = mine.get(point, "?")
        if their_gc != my_gc and their_gc != "?" and my_gc != "?":
            excused.add(point)
            print("    U+%04X is %s in %s and %s here - the reference's UCD "
                "is older" % (point, their_gc, reference, my_gc))
    return excused


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--driver", default=None)
    parser.add_argument("--class", dest="classes", action="append",
        default=None)
    args = parser.parse_args(argv[1:])

    classes = args.classes or CLASSES
    driver = args.driver
    if not driver:
        for platform in ("linux", "mac", "win64", "win32"):
            for build in ("release", "debug"):
                path = os.path.join(ROOT, "build", platform, build, "apps",
                    "tools", "grx_classes")
                if os.path.exists(path):
                    driver = path
                    break
            if driver:
                break
    if not driver or not os.path.exists(driver):
        sys.stderr.write("the grx_classes tool was not found; run "
            "`make tools` first\n")
        return 2

    perl = ask_perl(classes)
    perl_assigned = ask_perl_assigned()
    pcre_source = os.path.join(HERE, "pcre2_classes.c")
    pcre_text = ask_pcre2(classes, pcre_source, list(classes))
    assigned_source = os.path.join(ROOT, "build", "pcre2_assigned.c")
    os.makedirs(os.path.dirname(assigned_source), exist_ok=True)
    with open(assigned_source, "w", encoding="utf-8") as handle:
        handle.write(PCRE2_ASSIGNED)
    pcre_assigned_text = ask_pcre2([], assigned_source, [])
    ours_perl = ask_ours(driver, "perl", classes)
    ours_pcre = ask_ours(driver, "pcre", classes)
    if (perl is None or perl_assigned is None or pcre_text is None
            or pcre_assigned_text is None or ours_perl is None
            or ours_pcre is None):
        return 2

    pcre = runs_to_set(pcre_text, set(classes))
    pcre_assigned = runs_to_set(pcre_assigned_text).get("assigned", set())

    # This library's half of the restriction comes from this library, asked
    # through the driver's `assigned` pseudo-class - `\P{Cn}` - rather than
    # assumed to be every code point. Assuming it would make the intersection
    # the two references' and quietly hide the case this restriction exists
    # for: a code point one side has never heard of.
    ours_assigned_sets = ask_ours(driver, "perl", ["assigned"])
    if ours_assigned_sets is None:
        return 2
    ours_assigned = ours_assigned_sets.get("assigned", set())

    intersection = perl_assigned & pcre_assigned & ours_assigned
    print("assigned in all three: %d code points" % len(intersection))
    if classes == CLASSES and len(intersection) != ASSIGNED_IN_ALL_THREE:
        # Only when the whole set was asked for: a `--class` run compares the
        # same denominator but the prose is about the full sweep.
        sys.stderr.write("the assigned intersection is %d and dialects.md "
            "section 5.9 says %d - re-take every figure, not only this one\n"
            % (len(intersection), ASSIGNED_IN_ALL_THREE))
        return 1

    failures = []
    version_excused = {"perl": 0, "pcre2": 0}
    for name in classes:
        theirs_perl = perl.get(name, set()) & intersection
        theirs_pcre = pcre.get(name, set()) & intersection
        mine_perl = ours_perl.get(name, set()) & intersection
        mine_pcre = ours_pcre.get(name, set()) & intersection
        apart = len(theirs_perl ^ theirs_pcre)
        note = ""
        for reference, dialect, theirs, mine in (
                ("perl", "perl", theirs_perl, mine_perl),
                ("pcre2", "pcre", theirs_pcre, mine_pcre)):
            differing = (mine - theirs) | (theirs - mine)
            if not differing:
                continue
            print("  %-7s %s: %d code points differ" % (shown(name),
                reference, len(differing)))
            excused = version_difference(reference, driver, dialect, differing)
            left = differing - excused
            version_excused[reference] += len(excused)
            if left:
                failures.append("%-7s %s: %d code points ours has and %s has "
                    "not, %d the other way, and none of them is a category "
                    "difference: %s" % (shown(name), reference,
                    len(mine - theirs - excused),
                    reference, len(theirs - mine - excused),
                    " ".join("U+%04X" % p for p in sorted(left)[:8])))
                note += "  %s DISAGREES" % reference.upper()
        expected = EXPECTED_DIFFERENCE.get(name)
        if expected is not None and apart != expected:
            failures.append("%-7s the two references are %d code points "
                "apart and dialects.md section 5.9 and this file both say "
                "%d - re-take the figure and move it in both places"
                % (shown(name), apart, expected))
            note += "  FIGURE MOVED"
        print("  %-7s perl %6d  pcre2 %6d  %6d apart%s"
            % (shown(name), len(theirs_perl), len(theirs_pcre), apart, note))

    for failure in failures:
        sys.stderr.write(failure + "\n")
    print("wide classes: %d classes over %d code points assigned in all "
          "three, %d excused as a category the reference reads from an older "
          "UCD (%d perl, %d pcre2), %d disagreements"
          % (len(classes), len(intersection),
             version_excused["perl"] + version_excused["pcre2"],
             version_excused["perl"], version_excused["pcre2"],
             len(failures)))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
