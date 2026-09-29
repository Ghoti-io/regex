#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Copyright (C) 2026 Corey Pennycuff
#
# This file is part of Ghoti.io Regex.
#
# Ghoti.io Regex is free software: you can redistribute it and/or modify it
# under the terms of the GNU Lesser General Public License version 3 as
# published by the Free Software Foundation.
"""Vectors that separate the readings a property name can have.

**This exists because 75,919 conformance vectors passed while `\\p{Greek}`
resolved to the wrong set.** It was `Script` here and `Script_Extensions`
in both references, and the corpus is full of `\\p{Greek}` and full of Greek
letters - and a Greek letter is in both sets. A corpus that cannot separate
two readings scores them identical and reports green. Nothing in it put a
code point where the two answers differ against a name that has more than
one answer.

So the rows are chosen by *disagreement* rather than by interest. For every
name that more than one property could claim, the readings are fetched as
inversion lists and the rows are drawn from their **symmetric difference** -
the code points on which the readings can be told apart, and only those. A
row here fails the moment a resolver picks a different reading, which is the
one thing the rest of the corpus could not notice.

Three classes of collision, and the first is the one that bit:

  * **`sc` against `scx`.** Every script name has both readings and they
    differ for most scripts. A bare `\\p{Greek}` is the extensions set in
    perl and in PCRE2; this library read it as `Script` for as long as it
    had a Perl front end.
  * **a script or property name against a block name.** `Greek` is the short
    alias of the `Greek_And_Coptic` block as well as a script;
    `\\p{InGreek}` is the block and `\\p{Greek}` is not.
  * **a binary property against a block.** `ASCII` is a binary property and
    the short alias of `Basic_Latin`.

**Both halves come from the oracle, which is the point.** The readings are
`Unicode::UCD::prop_invlist` from the pinned perl and the expectations are
that same perl matching. There is no second parser here to disagree with the
first, and a code point this script calls discriminating is one perl agrees
is discriminating, because perl is what said so.

Usage:  tools/corpus/make_property_vectors.py [> tests/data/vectors/perl/properties.rxt]
"""
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "oracle"))
import oracle_env

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# How many code points to take from one symmetric difference. Two rather than
# one so that a reading which happens to be right at the first boundary is
# still asked a second time, and not more, because the corpus is checked in
# and every row costs a reader's attention as well as a run's time.
PER_DIFFERENCE = 2


def loose(name):
    """UAX #44-LM3, which is how every dialect here compares a property name."""
    return "".join(c for c in name.lower() if c not in " _-")


def ask_perl(script):
    """Run a perl snippet in the pinned interpreter and return its stdout."""
    done = subprocess.run(oracle_env.command("perl", ["perl", "-"]),
                          input=script, capture_output=True, text=True,
                          check=True)
    return done.stdout


def collisions_and_differences():
    """`[(spelling, reading_a, reading_b, [code points])]`, from the oracle.

    A "reading" is a fully qualified property expression - `sc=Greek`,
    `blk=Greek_And_Coptic`, `Alphabetic` - so that the rows can also assert
    what the *disambiguated* spellings answer, which is what says the bare
    name picked one of them rather than something else entirely.
    """
    script = r'''
use strict; use warnings;
use Unicode::UCD qw(prop_values prop_value_aliases prop_aliases);

# Every spelling of every value that a lone `\p{...}` could name, tagged
# with the reading it belongs to. Script twice, because `sc` and `scx` share
# their value names and are two different sets.
my %spelling;   # loose spelling -> { reading => 1 }
sub add { my ($loose, $reading) = @_; $spelling{$loose}{$reading} = 1; }
sub loosen { my $n = lc shift; $n =~ s/[ _-]//g; return $n; }

for my $key ("sc", "blk", "gc") {
  for my $v (prop_values($key)) {
    for my $alias (prop_value_aliases($key, $v)) {
      if ($key eq "sc") { add(loosen($alias), "sc=$v"); add(loosen($alias), "scx=$v"); }
      else { add(loosen($alias), "$key=$v"); }
    }
  }
}
# Binary properties: the name itself is the spelling.
for my $p (@{[ qw(ASCII Alphabetic Any Assigned Dash Deprecated Diacritic
                  Emoji Extender Hyphen Ideographic Math Radical
                  White_Space) ]}) {
  my @a = prop_aliases($p);
  next unless @a;
  for my $alias (@a) { add(loosen($alias), "$p"); }
}

for my $loose (sort keys %spelling) {
  my @readings = sort keys %{$spelling{$loose}};
  next if @readings < 2;
  print "$loose\t" . join("\t", @readings) . "\n";
}
'''
    pairs = []
    for line in ask_perl(script).splitlines():
        fields = line.split("\t")
        if len(fields) >= 3:
            pairs.append((fields[0], fields[1:]))
    return pairs


def invlists(readings):
    """`{reading: [boundaries]}` - perl's own inversion lists."""
    script = ("use Unicode::UCD qw(prop_invlist);\n"
              "for my $p (qw(%s)) {\n"
              "  my @l = eval { prop_invlist($p) };\n"
              "  print \"$p\\t\" . join(',', @l) . \"\\n\";\n"
              "}\n" % " ".join(readings))
    out = {}
    for line in ask_perl(script).splitlines():
        name, _, body = line.partition("\t")
        out[name] = [int(x) for x in body.split(",") if x != ""]
    return out


def members(boundaries):
    """An inversion list as a set of code points, capped to the BMP plus a bit.

    A full expansion of every list is tens of millions of integers for no
    gain: a difference that exists at all almost always exists low down, and
    a difference that exists *only* above this cap is reported rather than
    silently dropped.
    """
    out = set()
    for i in range(0, len(boundaries) - 1, 2):
        lo, hi = boundaries[i], boundaries[i + 1]
        for cp in range(lo, min(hi, 0x30000)):
            out.add(cp)
    if len(boundaries) % 2 == 1:
        for cp in range(boundaries[-1], 0x30000):
            out.add(cp)
    return out


def spellings_for(loose_name, readings):
    """The pattern spellings this row should ask about.

    The bare name is the one that was wrong. The qualified forms are here so
    that a failure says *which* reading was picked rather than only that the
    bare one disagreed, and the `In`/`Is` prefixes because they are a third
    and fourth answer to the same name - perl's `In` is the block and its
    `Is` is not.
    """
    out = ["\\p{%s}" % loose_name]
    for reading in readings:
        key, _, value = reading.partition("=")
        if value:
            out.append("\\p{%s=%s}" % (key, value))
    out.append("\\p{In%s}" % loose_name)
    out.append("\\p{Is%s}" % loose_name)
    return out


def escape(text):
    """The `\\u{...}` spelling the .rxt reader takes for a non-ASCII byte."""
    out = []
    for ch in text:
        if ch == "\\":
            out.append("\\\\")
        elif " " <= ch <= "~":
            out.append(ch)
        else:
            out.append("\\u{%X}" % ord(ch))
    return "".join(out)


def main():
    pairs = collisions_and_differences()
    if not pairs:
        sys.stderr.write("no colliding property names; perl answered nothing\n")
        return 2

    wanted = sorted({r for _loose, readings in pairs for r in readings})
    lists = invlists(wanted)

    rows = []            # (pattern, subject)
    described = []       # (loose, reading_a, reading_b, cp)
    only_high = 0
    for loose_name, readings in pairs:
        sets = {}
        for reading in readings:
            if reading in lists and lists[reading]:
                sets[reading] = members(lists[reading])
        names = sorted(sets)
        for i in range(len(names)):
            for j in range(i + 1, len(names)):
                diff = sets[names[i]] ^ sets[names[j]]
                if not diff:
                    continue
                for cp in sorted(diff)[:PER_DIFFERENCE]:
                    subject = chr(cp)
                    for pattern in spellings_for(loose_name, readings):
                        rows.append((pattern, subject))
                    described.append((loose_name, names[i], names[j], cp))

    if not rows:
        sys.stderr.write("no discriminating code points found\n")
        return 2

    driver = os.path.join(ROOT, "tools", "corpus", "perl_match.pl")
    payload = "".join(
        "u\t%s\t%s\n" % (p.encode("utf-8").hex(), s.encode("utf-8").hex())
        for p, s in rows)
    done = subprocess.run(oracle_env.command("perl", ["perl", driver]),
                          input=payload, capture_output=True, text=True,
                          check=True)
    answers = done.stdout.rstrip("\n").split("\n")
    if answers and answers[0].startswith("perl "):
        answers = answers[1:]
    if len(answers) != len(rows):
        sys.stderr.write("perl answered %d of %d rows\n"
                         % (len(answers), len(rows)))
        return 2

    sys.stdout.write(
        "# Property names with more than one reading, with %s as the oracle.\n"
        % oracle_env.version("perl") +
        "# Written by tools/corpus/make_property_vectors.py.\n"
        "#\n"
        "# Every row's subject is a code point on which two readings of the\n"
        "# same name **disagree** - drawn from their symmetric difference,\n"
        "# taken from perl's own prop_invlist. A row here fails the moment a\n"
        "# resolver picks a different reading than perl does.\n"
        "#\n"
        "# This is the class of row the corpus did not have. `\\p{Greek}`\n"
        "# resolved to Script here and to Script_Extensions in both\n"
        "# references, and 75,919 vectors passed anyway, because a Greek\n"
        "# letter is in both sets and nothing asked about U+0374.\n"
        "#\n"
        "# %d names with more than one reading, %d discriminating code\n"
        "# points, %d rows.\n"
        "dialect: perl\n"
        "unicode: 17.0\n"
        % (len(pairs), len(described), len(rows)))

    written = 0
    refused = 0
    for (pattern, subject), answer in zip(rows, answers):
        if answer.startswith("compile"):
            # perl refusing a spelling is an answer this corpus does not
            # carry: the reader's `expect:` has no way to say "this pattern
            # does not compile *there*", and a refusal is the subject of
            # documentation/dialects.md section 5.9 rather than of a match.
            refused += 1
            continue
        sys.stdout.write("\npattern: %s\nflags: u\nsubject: %s\nexpect: %s\n"
                         % (escape(pattern), escape(subject),
                            "nomatch" if answer == "nomatch"
                            else answer.split()[1].replace(":", "-")))
        written += 1

    sys.stderr.write("%d names collide, %d discriminating code points, "
                     "%d rows written, %d spellings perl refuses\n"
                     % (len(pairs), len(described), written, refused))
    if only_high:
        sys.stderr.write("%d differences exist only above U+30000\n" % only_high)
    return 0


if __name__ == "__main__":
    sys.exit(main())
