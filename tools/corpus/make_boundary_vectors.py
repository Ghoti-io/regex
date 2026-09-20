#!/usr/bin/env python3
"""Vectors for Perl's segmentation boundaries and `\\X`, answered by Perl.

The Unicode Consortium's own conformance files (tests/unit/test_break.cpp)
gate the four *algorithms*. This gates the *dialect*: where a boundary falls
is UAX #29's and UAX #14's business, but whether `\\b{lb}` holds at the start
of a line, and what `\\b{sb}` does to an empty subject, are Perl's - and Perl
is free to tailor line breaking, which UAX #14 explicitly permits.

The sixteen `\\b{...}` records in the imported corpus all use the empty
subject, so they could pass against an implementation that was wrong
everywhere else. These do not.

Usage:  tools/corpus/make_boundary_vectors.py > tests/data/vectors/perl/boundaries.rxt
"""
import subprocess
import sys
import os

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))

# The five spellings, positive and negative, plus `\X` on its own.
PATTERNS = []
for kind in ("gcb", "g", "wb", "sb", "lb"):
    for sigil in ("b", "B"):
        PATTERNS.append("x\\%s{%s}y" % (sigil, kind))
        PATTERNS.append("\\%s{%s}" % (sigil, kind))
        PATTERNS.append("(?s).\\%s{%s}." % (sigil, kind))
PATTERNS += ["\\X", "\\X\\X", "a?\\X", "(?s)^\\X$", "\\X+"]

# Subjects chosen for the corners each algorithm has: a combining sequence, a
# Hangul syllable, a flag (two regional indicators), an emoji ZWJ sequence, a
# CR LF, an Indic conjunct, a sentence that does not end at its full stop, a
# number with a decimal mark, and a line with a hyphen in it.
SUBJECTS = [
    "", "x", "xy", "x y",
    "é", "xéy", "각",
    "\U0001F1E6\U0001F1E7", "\U0001F1E6\U0001F1E7\U0001F1E8",
    "\U0001F468‍\U0001F469", "a\r\nb", "a\rb", "a\nb",
    "क्ष", "क्क",
    "Mr. Smith left.  He did.", "subtract .5 now", "x-y", "a­y",
    "12.5%", "$12", "日本語。次",
    "x​z", "x z", "x⁠z", "can't stop", "א-ב",
]


# Rows this oracle cannot answer for this library, and why.
#
# Perl 5.40.1 carries UCD 15.0.0 - `Unicode::UCD::UnicodeVersion()` says so -
# and these tables are 17.0.0, so three rules exist here that Perl has never
# seen. That is not a disagreement about the algorithm; it is two different
# editions of it, and documentation/unicode.md section 1 already warns about
# the same skew in the other direction. Each entry names the rule, the
# version that introduced it, and the subject it shows up on, so that a later
# Perl catching up is a matter of deleting a line and regenerating.
#
# That day has arrived and is a decision rather than a chore: perl 5.44.0 is
# the current stable release and `lib/unicore/version` in it reads 17.0.0,
# which is exactly the UCD these tables are generated from. Raising the pin
# in tools/corpus/VERSIONS would retire the first three entries here. It
# would also re-import t/re/re_tests from a different release, which is why
# it is not done in passing.
#
# The last entry is not a version difference but a defect in the oracle, and
# it is written down rather than worked around silently. Searched for on
# 2026-09-20 and not found: the perl5 issue tracker has nothing matching
# `\b{lb}`, nothing matching the other bound types that is this, and nothing
# about the start-position optimiser skipping a valid position. Not found is
# not the same as not reported, and the reproducer is small enough to check
# against a newer perl before concluding anything:
#
#     perl -e 'print "x" =~ /\b{lb}/      ? "match" : "NO MATCH", "\n"'  # NO MATCH
#     perl -e 'print "x" =~ /\b{lb}|(?!)/ ? "match" : "NO MATCH", "\n"'  # match
#
# `(?!)` never matches, so the two patterns cannot differ - and they do. The
# engine has it right and the optimisation in front of it does not.
EXCLUSIONS = [
    (("gcb", "g"), "\u0915\u094d\u0937",
     "GB9c, the Indic conjunct break, is Unicode 15.1 and Perl has 15.0"),
    (("gcb", "g"), "\u0915\u094d\u0915",
     "GB9c, the Indic conjunct break, is Unicode 15.1 and Perl has 15.0"),
    (("lb",), "subtract .5 now",
     "LB15c, the decimal mark after a space, is Unicode 16"),
    (("lb",), "\u05d0-\u05d1",
     "LB21a's HH class, the unambiguous hyphen, is Unicode 17"),
    (("lb",), "x",
     "perl finds no \\b{lb} at all in a one-character subject, though the "
     "boundary is there: `.\\b{lb}` matches at that same position. A defect "
     "in the oracle's unanchored search, not a rule"),
    (("lb",), ".",
     "the same one-character defect as \"x\""),
]


def excluded(pattern, subject):
    """The reason this row is left out, or None to keep it."""
    for kinds, skip_subject, reason in EXCLUSIONS:
        if subject != skip_subject:
            continue
        for kind in kinds:
            if "{%s}" % kind in pattern:
                return reason
        if "\\X" in pattern and "gcb" in [k for ks, _, _ in EXCLUSIONS
                                          for k in ks if k == "gcb"]:
            # `\X` is built out of the grapheme boundary, so it inherits that
            # boundary's exclusions and nothing else.
            if kinds[0] in ("gcb", "g"):
                return reason
    return None


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
            # The reader takes exactly four hex digits after a bare `\u`, so
            # anything above the basic plane has to use the braced form. Five
            # digits would be read as four and a stray character, which is
            # how a flag sequence became a letter and a `6`.
            out.append("\\u{%X}" % ord(ch))
        elif ord(ch) > 0x7E:
            out.append("\\u%04X" % ord(ch))
        else:
            out.append(ch)
    return "".join(out)


def main():
    driver = os.path.join(ROOT, "tools", "corpus", "perl_match.pl")
    rows = [(p, s) for p in PATTERNS for s in SUBJECTS]
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
        "# Perl's segmentation boundaries and `\\X`, with Perl 5.40 as the\n"
        "# oracle. Written by tools/corpus/make_boundary_vectors.py.\n"
        "#\n"
        "# The algorithms are gated against the Unicode Consortium's own\n"
        "# conformance files in tests/unit/test_break.cpp; this is the other\n"
        "# half, which is what the *dialect* does with them - the ends of the\n"
        "# subject, the empty subject, and any tailoring Perl applies to line\n"
        "# breaking, which UAX #14 permits and does not describe.\n"
        "dialect: perl\n"
        "unicode: 17.0\n")

    written = 0
    skipped = {}
    for (pattern, subject), answer in zip(rows, answers):
        if answer.startswith("compile"):
            continue # Not a disagreement this corpus is for.
        reason = excluded(pattern, subject)
        if reason:
            skipped.setdefault(reason, 0)
            skipped[reason] += 1
            continue
        sys.stdout.write("\npattern: %s\nflags: \nsubject: %s\n"
                         % (escape(pattern), escape(subject)))
        if answer == "nomatch":
            sys.stdout.write("expect: nomatch\n")
        else:
            spans = answer.split()[1:]
            sys.stdout.write("expect: %s\n"
                             % " ".join(s.replace(":", "-") if s != "-" else "-"
                                        for s in spans))
        written += 1

    if skipped:
        sys.stdout.write(
            "\n# Left out, with the reason each time. See EXCLUSIONS in\n"
            "# tools/corpus/make_boundary_vectors.py.\n")
        for reason in sorted(skipped):
            sys.stdout.write("#   %d row%s: %s\n"
                             % (skipped[reason],
                                "" if skipped[reason] == 1 else "s", reason))

    sys.stderr.write("%d vectors from %d rows, %d left out\n"
                     % (written, len(rows), sum(skipped.values())))
    return 0


if __name__ == "__main__":
    sys.exit(main())
