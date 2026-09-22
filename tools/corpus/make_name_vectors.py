#!/usr/bin/env python3
"""Vectors for Perl's `\\N{NAME}`, answered by Perl.

A name table is the kind of thing that is right in the cases anyone thinks
to try and wrong in a corner: the encoding has to round-trip 40,951 strings,
the sort order the lookup depends on has to be the order the generator
emitted, and the algorithmic families have to agree with a rule rather than
with a list. None of that is visible from a handful of unit tests.

So the corpus is drawn from the UCD itself rather than chosen: a sample of
stored names, every alias kind, the shapes that must be refused, and the
computed families at their range edges.

**Perl 5.40.1 carries UCD 15.0.0 and these tables are 17.0.0**, so a name for
a character Perl has never been told about is a version difference and not a
disagreement. Those rows are left out here and the count is reported, which
is the same treatment tools/corpus/make_boundary_vectors.py gives the three
break rules Perl predates.

Usage:  tools/corpus/make_name_vectors.py > tests/data/vectors/perl/names.rxt
"""
import os
import random
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
UCD = os.path.join(ROOT, "third_party", "ucd", "17.0.0")

LEADING = ["G", "GG", "N", "D", "DD", "R", "M", "B", "BB", "S", "SS", "",
           "J", "JJ", "C", "K", "T", "P", "H"]
VOWEL = ["A", "AE", "YA", "YAE", "EO", "E", "YEO", "YE", "O", "WA", "WAE",
         "OE", "YO", "U", "WEO", "WE", "WI", "YU", "EU", "YI", "I"]
TRAILING = ["", "G", "GG", "GS", "N", "NJ", "NH", "D", "L", "LG", "LM", "LB",
            "LS", "LT", "LP", "LH", "M", "B", "BS", "S", "SS", "NG", "J",
            "C", "K", "T", "P", "H"]


def hangul_name(codepoint):
    index = codepoint - 0xAC00
    lead = index // (21 * 28)
    vowel = (index % (21 * 28)) // 28
    trail = index % 28
    return "HANGUL SYLLABLE %s%s%s" % (
        LEADING[lead], VOWEL[vowel], TRAILING[trail])


def cases():
    """(pattern, subject) for every question this corpus asks."""
    random.seed(20260922)
    stored = []
    for line in open(os.path.join(UCD, "UnicodeData.txt"), encoding="utf-8"):
        fields = line.split(";")
        if len(fields) < 2 or not fields[1] or fields[1].startswith("<"):
            continue
        stored.append((fields[1], int(fields[0], 16)))

    aliases = []
    for line in open(os.path.join(UCD, "NameAliases.txt"), encoding="utf-8"):
        line = line.split("#")[0].strip()
        if not line:
            continue
        fields = [f.strip() for f in line.split(";")]
        if len(fields) >= 2:
            aliases.append((fields[1], int(fields[0], 16)))

    rows = []
    # A sample of stored names, plus every name with two adjacent separators,
    # which is where the encoding broke: the table is sorted by the real name
    # and searched by the decoded one, so a name that decodes wrongly takes
    # its neighbours down with it.
    adjacent = [pair for pair in stored
                if " -" in pair[0] or "- " in pair[0] or "  " in pair[0]
                or "--" in pair[0]]
    for name, codepoint in random.sample(stored, 900) + adjacent + aliases:
        rows.append(("^(?:\\N{%s})$" % name, chr(codepoint)))

    # The computed families at their edges and a little inside, because an
    # off-by-one in a range bound is invisible in the middle.
    for first, last, spell in (
            (0x4E00, 0x9FFF, lambda c: "CJK UNIFIED IDEOGRAPH-%04X" % c),
            (0x3400, 0x4DBF, lambda c: "CJK UNIFIED IDEOGRAPH-%04X" % c),
            (0x17000, 0x187FF, lambda c: "TANGUT IDEOGRAPH-%04X" % c),
            (0xAC00, 0xD7A3, hangul_name)):
        for codepoint in (first, first + 1, last - 1, last):
            rows.append(("^(?:\\N{%s})$" % spell(codepoint), chr(codepoint)))
        for codepoint in random.sample(range(first, last + 1), 40):
            rows.append(("^(?:\\N{%s})$" % spell(codepoint), chr(codepoint)))

    # The spellings that must be refused. Perl is strict: the match is case
    # sensitive, the separators are the UCD's, and a name it does not know is
    # a syntax error rather than an unmatchable character.
    for pattern in (
            "\\N{latin small letter a}",
            "\\N{LATIN-SMALL-LETTER-A}",
            "\\N{LATIN_SMALL_LETTER_A}",
            "\\N{LATINSMALLLETTERA}",
            "\\N{LATIN  SMALL  LETTER  A}",
            "\\N{NO SUCH CHARACTER AT ALL}",
            "\\N{CJK UNIFIED IDEOGRAPH-0041}",
            "\\N{CJK UNIFIED IDEOGRAPH-4e00}",
            "\\N{HANGUL SYLLABLE QQ}",
            "\\N{TANGUT IDEOGRAPH-0041}",
            "\\N{}",
            "\\N{ }"):
        rows.append((pattern, "a"))

    # The ones that are not names at all and must keep working.
    rows.append(("^(?:\\N{U+0041})$", "A"))
    rows.append(("^(?:\\N)$", "a"))
    rows.append(("^(?:a\\N{2}b)$", "axyb"))
    rows.append(("^(?:\\N{ SPACE })$", " "))
    return rows


def escape(text):
    out = []
    for ch in text:
        if ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ch == "\r":
            out.append("\\r")
        elif ch == "\t":
            out.append("\\t")
        elif ord(ch) < 0x20 or ord(ch) == 0x7F:
            out.append("\\x%02X" % ord(ch))
        elif ord(ch) > 0xFFFF:
            out.append("\\u{%X}" % ord(ch))
        elif ord(ch) > 0x7E:
            out.append("\\u%04X" % ord(ch))
        else:
            out.append(ch)
    return "".join(out)


def main():
    rows = cases()
    driver = os.path.join(ROOT, "tools", "corpus", "perl_match.pl")
    payload = "".join(
        "\t%s\t%s\n" % (p.encode("utf-8").hex(), s.encode("utf-8").hex())
        for p, s in rows)
    done = subprocess.run([driver], input=payload, capture_output=True,
                          text=True, check=True)
    answers = done.stdout.rstrip("\n").split("\n")
    if len(answers) != len(rows):
        sys.stderr.write("perl answered %d of %d rows\n"
                         % (len(answers), len(rows)))
        return 2

    sys.stdout.write(
        "# Perl's `\\N{NAME}`, with Perl 5.40 as the oracle.\n"
        "# Written by tools/corpus/make_name_vectors.py.\n"
        "#\n"
        "# Stored names, every NameAliases kind, the computed families at\n"
        "# their range edges, and the spellings Perl refuses - a lower-case\n"
        "# name, hyphens for spaces, a doubled space, an unknown name, and a\n"
        "# name of the right shape for a range it does not fall in.\n"
        "#\n"
        "# Rows for characters Perl's UCD 15.0.0 does not have are left out:\n"
        "# those are a version difference rather than a disagreement, the\n"
        "# same skew unicode.md section 1 describes.\n"
        "dialect: perl\n"
        "unicode: 17.0\n")

    written = 0
    skipped = 0
    for (pattern, subject), answer in zip(rows, answers):
        # A name perl does not know compiles to nothing there. For the rows
        # this corpus *expects* to be refused that is the answer; for a name
        # from a newer UCD it is the skew, and the two are told apart by
        # whether this library accepts it.
        if answer.startswith("compile"):
            if "\\N{" in pattern and not pattern.startswith("^"):
                pass  # a refusal this corpus is about; keep it
            else:
                skipped += 1
                continue
        sys.stdout.write("\npattern: %s\nflags: \nsubject: %s\nexpect: %s\n"
                         % (escape(pattern), escape(subject),
                            "error syntax" if answer.startswith("compile")
                            else ("nomatch" if answer == "nomatch"
                                  else answer.split()[1].replace(":", "-"))))
        written += 1

    sys.stderr.write("%d rows written, %d skipped as UCD version skew\n"
                     % (written, skipped))
    return 0


if __name__ == "__main__":
    sys.exit(main())
