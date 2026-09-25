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

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
    "oracle"))
import oracle_env
import perl_ucd

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
# **Four entries were here until 2026-09-25 and the pin raise retired them.**
# Perl 5.40.1 carried UCD 15.0.0 against these tables' 17.0.0, so three rules
# existed here that it had never seen - GB9c (Unicode 15.1), LB15c (16) and
# LB21a's HH class (17), one of them on two subjects, which is why the count
# of entries and the count of rules were never the same number. That was not
# a disagreement about the algorithm; it was two editions of it, and
# documentation/unicode.md section 1 warns about the same skew in the other
# direction.
#
# 5.44.0 reads 17.0.0 in `lib/unicore/version`, which is exactly the UCD
# these tables are generated from, so the four are gone and **46 rows are
# compared that were not**. check_oracle_ucd() below is what made that a
# deletion rather than an archaeology: it named the four entries the raise
# retired, from the data, before anything was changed.
#
# The two entries left are not version differences but a defect in the
# oracle, and
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
# engine has it right and the optimisation in front of it does not. Re-run
# against 5.44.0 on 2026-09-25 when the pin moved: **still present**, the
# same two answers. So these two entries are not waiting on an upgrade.
# The fourth field is the Unicode version that introduced the rule, or None
# where the row is out for a reason no upgrade fixes. check_oracle_ucd()
# reads it, so raising the pin names its own consequences instead of leaving
# them to be counted by hand.
EXCLUSIONS = [
    (("lb",), "x",
     "perl finds no \\b{lb} at all in a one-character subject, though the "
     "boundary is there: `.\\b{lb}` matches at that same position. A defect "
     "in the oracle's unanchored search, not a rule",
     None),
    (("lb",), ".",
     "the same one-character defect as \"x\"",
     None),
]


def excluded(pattern, subject):
    """The reason this row is left out, or None to keep it."""
    for kinds, skip_subject, reason, _since in EXCLUSIONS:
        if subject != skip_subject:
            continue
        for kind in kinds:
            if "{%s}" % kind in pattern:
                return reason
        if "\\X" in pattern and "gcb" in [k for ks, _, _, _ in EXCLUSIONS
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


# The UCD edition the exclusions above were written against.
#
# Every version-skew entry is a claim about *this* number, and until now the
# number lived only in the prose above it. Nothing asked perl. That is the
# same shape as the locale the vim oracle was reading and no file recorded:
# a generated corpus whose contents depend on a property of the environment
# that nothing verifies. It fails in the direction that hides work - upgrade
# perl, regenerate, and rows perl can now answer are still dropped, with the
# exclusion note still confidently naming a version perl no longer carries.
# An exclusion outliving its reason is a blindfold, and it reads exactly like
# a considered decision.
ORACLE_UCD_VERSION = "17.0.0"


def check_oracle_ucd():
    """Refuse to generate against a perl the exclusions were not written for.

    Refuse rather than warn: a warning on stderr is lost in a redirect, and
    the output of this script is a committed corpus. There is no fallback to
    "generate anyway and hope" for the same reason the oracle drivers have no
    fallback mode - a corpus generated against an unknown edition is worse
    than none, because it looks the same.
    """
    found = perl_ucd.perl_ucd_version()
    if found is None:
        sys.stderr.write(
            "could not ask perl for its UCD version. The exclusions in this "
            "file are claims about that number, so it has to be known "
            "before a corpus is written.\n")
        return 2
    if found == ORACLE_UCD_VERSION:
        return 0
    sys.stderr.write(
        "perl carries UCD %s; the exclusions here were written for %s.\n"
        % (found, ORACLE_UCD_VERSION))
    retired = [(reason, since) for _kinds, _subject, reason, since in EXCLUSIONS
        if since is not None and perl_ucd.version_tuple(since) <= perl_ucd.version_tuple(found)]
    if retired:
        sys.stderr.write("%d of the %d exclusions are no longer version skew "
            "and would be dropped silently:\n" % (len(retired), len(EXCLUSIONS)))
        for reason, since in retired:
            sys.stderr.write("  - (Unicode %s) %s\n" % (since, reason))
    else:
        sys.stderr.write("no exclusion here is retired by that edition, so "
            "this is skew in the other direction: an oracle *ahead* of these "
            "tables disagrees across the whole corpus rather than on named "
            "rows. See documentation/unicode.md section 1.\n")
    sys.stderr.write("Raise ORACLE_UCD_VERSION with tools/corpus/VERSIONS, "
        "delete the entries named above, and regenerate.\n")
    return 2


def main():
    failure = check_oracle_ucd()
    if failure:
        return failure
    driver = os.path.join(ROOT, "tools", "corpus", "perl_match.pl")
    rows = [(p, s) for p in PATTERNS for s in SUBJECTS]
    payload = "".join(
        "\t%s\t%s\n" % (p.encode("utf-8").hex(), s.encode("utf-8").hex())
        for p, s in rows)
    # Through the pinned perl, not through the script's shebang. Running
    # `perl_match.pl` as a program hands the interpreter choice to `#!`, so
    # these three generators were reaching for /usr/bin/perl while every
    # differential beside them had been converted - a whole class the first
    # sweep missed, because a shebang-executed script has no "perl" in its
    # argv to grep for. check_oracle_env.py sweeps for it now.
    done = subprocess.run(
        oracle_env.command("perl", ["perl", driver]), input=payload,
        capture_output=True, text=True, check=True)
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
