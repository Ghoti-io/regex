#!/usr/bin/env python3
"""Vectors for Perl's full case folding, answered by Perl.

Full folding is the one folding that maps a code point to a *sequence*, so
`\\x{df}` matches "ss" under `/i`. Two things follow that a corpus of
imported rows does not reach, and both were wrong here until this file
existed:

- A **character class** takes part in it. `[\\x{df}]` matches "ss" in Perl,
  which contradicts "a class matches one character" and is why the rule has
  to be written down rather than derived. This library answered nomatch for
  every one of the 104 code points concerned; `re_tests` was at 2,592 of
  2,592 throughout, because it has no case for it.
- The rule has **edges that look arbitrary and are not**. A negated class
  does not fold fully, a range does not, a shorthand and a property do not,
  and `/aa` drops the whole thing. Each of those is a row here rather than a
  sentence in a comment, so that a later change cannot quietly take one of
  them with it.

The code points are not a chosen sample: they are every `F` line of the UCD's
CaseFolding.txt, which is the definition of "has a full fold". 104 in UCD
17.0.0, folding to 73 distinct sequences.

Usage:  tools/corpus/make_fold_vectors.py > tests/data/vectors/perl/folding.rxt
"""
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def full_folds():
    """Every (code point, fold) pair from CaseFolding.txt's F lines."""
    path = None
    for root, _dirs, files in os.walk(os.path.join(ROOT, "third_party", "ucd")):
        if "CaseFolding.txt" in files:
            path = os.path.join(root, "CaseFolding.txt")
            break
    if not path:
        sys.stderr.write("no CaseFolding.txt under third_party/ucd; "
                         "run tools/unicode/fetch.sh\n")
        sys.exit(2)
    pairs = []
    for line in open(path, encoding="utf-8"):
        line = line.split("#")[0].strip()
        if not line:
            continue
        fields = [f.strip() for f in line.split(";")]
        if len(fields) < 3 or fields[1] != "F":
            continue
        pairs.append((chr(int(fields[0], 16)),
                      "".join(chr(int(x, 16)) for x in fields[2].split())))
    return pairs


def rows(pairs):
    """(pattern, subject) for every question this corpus asks."""
    out = []

    # Every code point, on the three shapes that separate the rule from its
    # two neighbours: a literal folds fully, a class member folds fully too
    # (the part that was missing), and a negated class does not. Anchored,
    # because the question is whether the whole fold is consumed - unanchored,
    # `[X]` against "ss" matches the first "s" and answers something else.
    for cp, fold in pairs:
        for pattern in ("^(?:%s)$" % cp, "^(?:[%s])$" % cp, "^(?:[^%s])$" % cp):
            out.append((pattern, fold))
            out.append((pattern, fold.upper()))
            out.append((pattern, cp))

    # `/aa` over every code point too, and not over a sample, because which
    # folds it drops is a property of each fold's own code points: Perl keeps
    # a full fold under `/aa` exactly when none of the fold is ASCII, which
    # is 87 of these 104. Sampling this axis is what under-measured it when
    # this file was first written - of the seven code points the shape list
    # below happens to use, only U+0390 has an all-non-ASCII fold, so four
    # rows stood for eighty-seven. The third pattern is the reverse
    # direction: `(?aa)ff` must stop being matched by the ff ligature.
    for cp, fold in pairs:
        for pattern in ("^(?:(?aa)%s)$" % cp, "^(?:(?aa)[%s])$" % cp,
                        "^(?:(?aa)%s)$" % fold):
            out.append((pattern, fold))
            out.append((pattern, cp))

    # The shapes, on a few code points that differ in what they fold to: two
    # characters, three characters, a fold that is itself cased, and the two
    # that share one fold. One row per shape per code point rather than a
    # cross product - the shapes are independent of which code point is in
    # them, and the sweep above is what covers the code points.
    sample = ["ß",  # LATIN SMALL LETTER SHARP S -> "ss"
              "ẞ",  # LATIN CAPITAL LETTER SHARP S -> "ss", same fold
              "ﬀ",  # ff ligature -> "ff"
              "ﬃ",  # ffi ligature -> "ffi", three
              "ΐ",  # GREEK SMALL IOTA WITH DIALYTIKA AND TONOS, three
              "ǰ",  # j with caron -> j + combining caron
              "ŉ"]  # 'n -> modifier apostrophe + n
    for cp in sample:
        fold = dict(pairs)[cp]
        shapes = [
            "^(?:[%sq])$" % cp,          # beside another member
            "^(?:[q%s])$" % cp,          # ...and on the other side of it
            "^(?:[%s-%s])$" % (cp, cp),  # a degenerate range: Perl keeps it
            "^(?:[a-%s])$" % cp,         # a real range: Perl does not
            "^(?:[\\w])$",               # a shorthand: nor for one of these
            "^(?:[[:alpha:]])$",         # nor a POSIX class
            "^(?:[\\p{L}])$",            # nor a property
            "^(?:[%s]{2})$" % cp,        # it composes like any other branch
            "^(?:[%s])+$" % cp,
            "^(?:(?aa)[%s])$" % cp,      # `/aa` drops full folding entirely
            "^(?:(?aa)%s)$" % cp,
            "^(?:%s)$" % fold,           # the other direction, for contrast:
            "^(?:[%s])$" % fold[0],      # a literal run folds, a class does not
        ]
        for pattern in shapes:
            out.append((pattern, fold))
            out.append((pattern, fold * 2))
            out.append((pattern, cp))
            out.append((pattern, fold.upper()))
    return out


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
    pairs = full_folds()
    table = rows(pairs)
    driver = os.path.join(ROOT, "tools", "corpus", "perl_match.pl")
    payload = "".join(
        "i\t%s\t%s\n" % (p.encode("utf-8").hex(), s.encode("utf-8").hex())
        for p, s in table)
    done = subprocess.run([driver], input=payload, capture_output=True,
                          text=True, check=True)
    answers = done.stdout.rstrip("\n").split("\n")
    if answers and answers[0].startswith("perl "):
        answers = answers[1:]
    if len(answers) != len(table):
        sys.stderr.write("perl answered %d of %d rows\n"
                         % (len(answers), len(table)))
        return 2

    distinct = len({fold for _cp, fold in pairs})
    sys.stdout.write(
        "# Perl's full case folding, with Perl 5.40 as the oracle.\n"
        "# Written by tools/corpus/make_fold_vectors.py.\n"
        "#\n"
        "# Every code point with a full fold - all %d `F` lines of\n"
        "# CaseFolding.txt, folding to %d distinct sequences - as a literal,\n"
        "# as a character class member, and inside a negated class, against\n"
        "# its fold, its fold uppercased and itself. Then the shapes that\n"
        "# bound the rule: a range, a shorthand, a POSIX class, a property,\n"
        "# `/aa`, and the reverse direction.\n"
        "#\n"
        "# `re_tests` has no case for a class member folding fully, so this\n"
        "# library answered every one of them wrongly while the imported\n"
        "# corpus read 2,592 of 2,592. documentation/dialects.md section 5.8\n"
        "# states the rule; this is what holds it.\n"
        "dialect: perl\n"
        "unicode: 17.0\n" % (len(pairs), distinct))

    written = 0
    refused = 0
    for (pattern, subject), answer in zip(table, answers):
        if answer.startswith("compile"):
            refused += 1
            continue
        sys.stdout.write("\npattern: %s\nflags: i\nsubject: %s\nexpect: %s\n"
                         % (escape(pattern), escape(subject),
                            "nomatch" if answer == "nomatch"
                            else answer.split()[1].replace(":", "-")))
        written += 1

    sys.stderr.write("%d rows written, %d refused by perl\n"
                     % (written, refused))
    return 0


if __name__ == "__main__":
    sys.exit(main())
