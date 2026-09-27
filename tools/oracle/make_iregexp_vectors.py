#!/usr/bin/env python3
r"""Write the I-Regexp conformance vectors from the two pinned references.

The dialect shipped with no vectors at all, because it had no oracle: RFC 9485
names no implementation, so there was nothing to generate expectations from and
the README's conformance table carried a rate of `-`. This file is what fills
that column, and every expectation in it is a reference's answer.

**Two references, and each writes a different kind of record.**

    iregexp-check   a pattern it refuses becomes `expect: error syntax`
    libxml2         a pattern it accepts becomes one record per subject,
                    `expect: 0-<length>` when the whole subject matches and
                    `nomatch` when it does not

Both kinds are needed for the same reason a checking implementation exists: a
corpus of patterns that compile measures half of a dialect whose other half is
everything it refuses.

**The records are anchored, and say so.** `options: anchored anchored-end` is
what asks the whole-string question - XSD's Boolean, which RFC 9485 section 4
adopts - and it is the only question the semantic reference can answer, a
pattern facet having no unanchored form. So a `0-<length>` span here is not a
leftmost-first search result; it is "the whole of this subject matches", and the
span is therefore always the whole subject.

**Three things are deliberately left out, and none of them silently.**

- **Patterns with a capturing group.** XSD has no captures, so there is no
  reference for a group span - and a record that listed one taken from this
  library would be writing this library's answer down as a requirement, which
  is the defect this whole arrangement exists to avoid. The runner treats a
  record that lists fewer spans than the pattern has groups as asserting that
  the rest are *unset*, so a partial record would be a wrong record rather
  than a quiet one.
- **Rows the two references disagree about**, each for a reason
  `iregexp_diff.py` names and counts: iregexp-check accepts a quantifier of
  one digit only, libxml2's general-category tables are older than this
  library's, and libxml2 reads a class range whose low end is escaped as a
  union. A vector for one of those would record a defect as a rule.
- **Subjects and patterns XML cannot carry**, which is a C0 control other than
  tab, LF or CR: the semantic reference is reached through a document, and
  there is no spelling of those characters that an XML 1.0 parser will read.

Usage:
    tools/oracle/make_iregexp_vectors.py [--seed N] [--count N] [--out PATH]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import os
import random
import sys

import iregexp_diff
import iregexp_match
import oracle_env

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
OUT = os.path.join(ROOT, "tests", "data", "vectors", "iregexp", "generated.rxt")

# What a field may hold literally. Two exclusions beyond the obvious:
#
# - **The space.** A field's value is trimmed, so a subject that is one space
#   would be written as a line ending in a space and read back as the empty
#   string - a record about a different question, passing or failing for a
#   reason that has nothing to do with the dialect.
# - **The backslash**, because it introduces every escape.
PRINTABLE = set(range(0x21, 0x7F)) - {ord(chr(92))}


def encode(text):
    r"""One rxt field: printable ASCII as itself, everything else escaped.

    **`\u{...}` above U+FFFF and not `\U0001F41F`.** The format's escapes are
    `\n`, `\r`, `\t`, `\f`, `\v`, `\0`, `\\`, `\xHH`, `\uHHHH` and
    `\u{...}`, and anything else *keeps both characters* so that a hand-written
    vector can say `pattern: \d+` and mean it. So the first version of this
    function, which invented `\U0001F41F`, wrote records whose subject was the
    ten literal characters of the escape rather than the fish - and the vector
    then said `expect: 0-$` about a ten-byte subject the oracle never saw. It
    failed loudly, which was luck: `.` cannot match ten characters. A generator
    and a reader that disagree about an encoding produce vectors that are
    *valid* and about the wrong question.
    """
    out = []
    for character in text:
        code = ord(character)
        if code in PRINTABLE:
            out.append(character)
        elif code <= 0xFFFF:
            out.append(chr(92) + "u%04X" % code)
        else:
            out.append(chr(92) + "u{%X}" % code)
    return "".join(out)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--count", type=int, default=4000,
                        help="patterns drawn before the subjects multiply them")
    parser.add_argument("--patterns", type=int, default=300,
                        help="accepted patterns crossed with the subjects")
    parser.add_argument("--out", default=OUT)
    args = parser.parse_args(argv[1:])

    # The pins are checked here and the provenance line is `run-oracle`'s:
    # two lines naming the same two references is a line that stops being read.
    oracle_env.check_pin("iregexp")
    oracle_env.check_pin("libxml2")

    patterns = iregexp_diff.corpus(args.seed, args.count)
    verdicts = iregexp_diff.ask_check(patterns)

    # **The named differences are applied to the refusals too**, and that is not
    # symmetry for its own sake: iregexp-check refuses `a{10}`, this library
    # accepts it, and the grammar says this library is right - so writing its
    # verdict down as `expect: error syntax` would put a reference defect in
    # the corpus as a requirement. The first run of this generator did exactly
    # that for six records, which is what a corpus is for.
    refused, refused_excluded = [], 0
    for pattern, verdict in zip(patterns, verdicts):
        if verdict != "no":
            continue
        if iregexp_diff.multi_digit_quantifier(pattern):
            refused_excluded += 1
            continue
        refused.append(pattern)
    accepted = [p for p, verdict in zip(patterns, verdicts) if verdict == "ok"]

    # The exclusions, applied to what goes in rather than to what comes out, so
    # that a row this cannot have an expectation for is never written down with
    # a guess in it.
    excluded = {
        "capturing group": 0,
        "escaped range endpoint": 0,
        "range order": 0,
    }
    keep = []
    for pattern in accepted:
        if iregexp_diff.descending_range(pattern):
            # `[z-a]`: Figure 1 admits it and XSD does not, so iregexp-check
            # accepts where libxml2 refuses the pattern outright. Excluded here
            # rather than left to be skipped later, so that the remaining
            # "xsd refused the pattern" count stays what it should be - a
            # tripwire for the subset relation of section 5.2 failing, which
            # over 16,907 patterns it does not.
            excluded["range order"] += 1
            continue
        if "(" in pattern:
            excluded["capturing group"] += 1
            continue
        if iregexp_diff.escaped_range_endpoint(pattern):
            excluded["escaped range endpoint"] += 1
            continue
        keep.append(pattern)

    shapes = set(iregexp_diff.SHAPES)
    shaped = [p for p in keep if p in shapes]
    others = [p for p in keep if p not in shapes]
    rng = random.Random(args.seed)
    rng.shuffle(others)
    chosen = shaped + others[:max(0, args.patterns - len(shaped))]

    subjects = iregexp_diff.SUBJECTS
    pairs = [(p, s) for p in chosen for s in subjects]
    answers = iregexp_diff.ask_xsd(pairs)

    # The category skew is measured, not assumed: the same question
    # iregexp_diff.py asks, so the two cannot drift apart about which subjects
    # have no comparable answer.
    driver = iregexp_diff.find_driver("grx_iregexp", None)
    if not driver:
        raise SystemExit("make_iregexp_vectors: build the tools first")
    categories = iregexp_diff.category_map(driver, subjects)

    rows = []
    skipped = {"declined (XML)": 0, "xsd refused the pattern": 0,
               "category tables differ": 0}
    for (pattern, subject), answer in zip(pairs, answers):
        if answer == "badsubject":
            skipped["declined (XML)"] += 1
            continue
        if answer == "badpattern":
            skipped["xsd refused the pattern"] += 1
            continue
        if iregexp_diff.table_skew(categories, pattern, subject):
            skipped["category tables differ"] += 1
            continue
        # `0-$` rather than a byte count this file worked out for itself:
        # the reader resolves `$` to the subject's length, so a record cannot
        # be wrong about the one number it is not asserting anything about.
        expectation = "0-$" if answer == "true" else "nomatch"
        rows.append((pattern, subject, expectation))

    lines = [
        "# generated by tools/oracle/make_iregexp_vectors.py",
        "# oracle: %s" % oracle_env.version("iregexp"),
        "# oracle: %s" % oracle_env.version("libxml2"),
        "# The expectations are the references', not this library's: RFC 9485",
        "# names no implementation, so the syntax half is a second reading of",
        "# its ABNF (iregexp-check) and the matching half is XSD's own",
        "# semantics, which section 4 adopts and section 5.2 makes reachable",
        "# through any XSD engine (libxml2, through lxml).",
        "#",
        "# Every record is anchored at both ends, because a pattern facet has",
        "# no unanchored form: a span here is the whole subject or there is no",
        "# match. JSONPath's search() has no reference at all and is not here;",
        "# tests/unit/test_iregexp.cpp states that rule instead.",
        "#",
        "# Not written down, and why:",
    ]
    lines.append("#   %-28s %6d patterns"
                 % ("multi-digit quantifier", refused_excluded))
    for name, count in sorted(excluded.items()):
        lines.append("#   %-28s %6d patterns" % (name, count))
    for name, count in sorted(skipped.items()):
        lines.append("#   %-28s %6d rows" % (name, count))
    lines += [
        "#",
        "# %d patterns refused by the reference, %d patterns matched against"
        % (len(refused), len(chosen)),
        "# %d subjects." % len(subjects),
        "dialect: i-regexp",
        "",
    ]

    for pattern in refused:
        lines.append("pattern: %s" % encode(pattern))
        lines.append("expect: error syntax")
        lines.append("")

    for pattern, subject, expectation in rows:
        lines.append("pattern: %s" % encode(pattern))
        lines.append("options: anchored anchored-end")
        lines.append("subject: %s" % encode(subject))
        lines.append("expect: %s" % expectation)
        lines.append("")

    os.makedirs(os.path.dirname(args.out), exist_ok=True)
    with open(args.out, "w", encoding="utf-8") as handle:
        handle.write(chr(10).join(lines))

    sys.stderr.write("wrote %d records to %s%s"
                     % (len(refused) + len(rows), args.out, chr(10)))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
