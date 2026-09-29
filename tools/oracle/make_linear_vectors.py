#!/usr/bin/env python3
r"""Generate the RE2 and Rust `.rxt` conformance vectors from their engines.

`linear_diff.py` is the differential: it needs the two pinned images and runs
fresh every time. This is the other half, the one `make_python_vectors.py` is
for Python - a checked-in corpus that runs in `make test` on a machine with
no containers, no network, no Go and no Rust, and that turns a rule confirmed
once into a regression test forever.

The expectation in every record is **the reference's**. A vector generated
from what this library currently does would record the bug rather than the
rule, which is `make_vectors.py`'s note and the reason no generator here ever
asks this library anything.

**The named cases below are the point of the file.** Every one of them is a
cell the differential moved while these two dialects were being built, and a
random corpus reaches most of them only by accident:

  - `(a|){1,2}` against "a", which is the only shape that separates a
    counted repeat from a loop;
  - `x\b` against "x" U+212A under `i`, which is the only pattern that tells
    Go's caseless rule from the crate's;
  - `[[:^alpha:]]` under `i`, which decides whether the fold runs before or
    after the negation;
  - `(?-u)\D`, which the crate refuses and RE2 cannot spell.

Usage:
    tools/oracle/make_linear_vectors.py [--out DIR] [--seed N] [--patterns N]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import binascii
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)

import linear_diff
import make_vectors
import oracle_env

# Per dialect, because the two do not share a vocabulary: nine constructs
# belong to exactly one of them, and a named case naming the other's spelling
# would assert a refusal rather than the rule it was written for.
NAMED_CASES = {
    "re2": [
        # The loop cells. The first three were what chose BREAK_FIRST over
        # FAIL and over BREAK, one each, and they are kept because they are
        # still true and are what a reader reaches for first:
        #
        #   `(a*)*` over "b"      group 1 is 0-0: an empty iteration runs
        #                         when nothing else has.
        #   `(a|)*` over "aaaa"   group 1 is 3-4: a trailing empty one does
        #                         not.
        #   `(a|){1,2}` over "a"  group 1 is 1-1: a counted repeat is copies
        #                         rather than a loop, so no rule applies.
        #
        # And the fourth is what says none of the three was the question.
        # `(a*?)+b` over "aab" reports group 1 as 0-2, which no empty-
        # iteration rule gives: the reference is a simulation, the fresh
        # iteration arrives at a state the walk has already reached, and it
        # is dropped there rather than allowed, broken or failed. The rows
        # below it are the minimal pair - `(a+?)+b` has no empty body and
        # `(a*)+b` unflagged has no lazy loop - and each agrees under every
        # rule, which is why neither alone would have found this.
        ("", "(a*)*", ["b", "aab", ""]),
        ("", "(a*)+", ["a", "b", "aab"]),
        ("", "(a|)*", ["aaaa", ""]),
        ("", "(a|){1,2}", ["a", "ab", "b"]),
        ("", "(a|){1,3}", ["aa"]),
        ("", "(a|){3}", ["a"]),
        ("U", "(a*)+b", ["aab", "ab", "aaab", "b"]),
        ("U", "(a*)*b", ["aab", "aabb"]),
        ("U", "(a+)+b", ["aab"]),
        ("", "(a*?)+b", ["aab", "aaab"]),
        ("", "(a*?)*?b", ["aab"]),
        ("U", "((a)*)+b", ["aab"]),
        ("U", "(a*b*)+?b", ["abab", "aabb"]),
        ("U", "(a|b*)+?", ["aabb"]),
        # Leftmost-first, which an automaton could plausibly not be.
        ("", "(a|ab)", ["ab"]),
        ("", "(a|ab)(c|bcd)", ["abcd"]),
        ("", "a|ab", ["ab"]),
        # The caseless split: `\w` widens and `\b` does not. RE2 is the only
        # dialect here on that side of the line.
        ("i", "\\w", ["\\u017f", "a"]),
        ("i", "x\\b", ["x\\u212a", "x"]),
        ("i", "k", ["\\u212a", "K"]),
        ("i", "[[:alpha:]]", ["\\u017f", "\\u212a", "a"]),
        ("i", "[[:^alpha:]]", ["\\u017f", "0"]),
        ("i", "[[:lower:]]", ["\\u017f", "A"]),
        # The shorthands are ASCII while the folding is Unicode, which is
        # the clearest case of those two axes being independent.
        ("", "\\w", ["\\u00e9", "a"]),
        ("", "\\d", ["\\u0663", "0"]),
        ("", "\\s", ["\\u00a0", " "]),
        ("", "[[:alpha:]]", ["\\u00e9", "a"]),
        # `$` is the end of the subject and nothing else; `^` under `m`
        # matches after a newline that ends it.
        ("", "a$", ["a", "a\\n"]),
        ("m", "a$", ["a\\nb"]),
        ("m", "^b", ["a\\nb", "a\\rb"]),
        ("", "\\p{Any}^", ["a\\n"]),
        ("m", "\\p{Any}^", ["a\\n"]),
        # `\Q...\E`, octal, and `\p{^L}` - RE2's three, each an error in the
        # crate.
        ("", "\\Qa+b\\E", ["a+b", "ab"]),
        ("", "a\\Q+\\Eb", ["a+b"]),
        ("", "\\101", ["A"]),
        ("", "\\0", ["\\u0000"]),
        ("", "\\p{^L}", ["0", "a"]),
        ("", "\\P{^L}", ["a", "0"]),
        # A `{` that begins no quantifier is a literal here.
        ("", "a{,3}", ["a{,3}", "aaa"]),
        ("", "a{2,", ["a{2,"]),
        ("", "{", ["{"]),
        # A class escape at the low end makes the `-` a member; at the high
        # end it is an error. The asymmetry is RE2's.
        ("", "[\\d-x]", ["-", "5", "x", "a"]),
        # Duplicate names compile, which the crate refuses.
        ("", "(?P<n>a)(?P<n>b)", ["ab"]),
        ("", "(?P<1a>a)", ["a"]),
        # The constructs the guarantee costs. Each is a refusal and the
        # refusal *is* the promise: a pattern this dialect accepts is one
        # the linear engines can run.
        ("", "(a)\\1", ["aa"]),
        ("", "(?=a)", ["a"]),
        ("", "(?<=a)b", ["ab"]),
        ("", "(?>a)", ["a"]),
        ("", "(?#c)a", ["a"]),
        ("", "\\Z", ["a"]),
        ("", "a*+", ["aa"]),
        ("", "a**", ["aa"]),
        ("", "a{1001}", ["a"]),
        ("", "\\1", ["a"]),
        ("", "\\cA", ["\\u0001"]),
        ("", "[[:nosuch:]]", ["a"]),
        ("", "(?x) a b", ["ab"]),
    ],
    "rust": [
        # The same loop cells, including the one that was a known gap until
        # the shape battery showed it was 62 rows rather than one. The crate
        # answers as RE2 does on every one of them - all 5,040 shapes, not
        # only these - which is why the two dialects share a mode and is
        # worth recording rather than assuming.
        ("", "(a*)*", ["b", "aab", ""]),
        ("", "(a|)*", ["aaaa"]),
        ("", "(a|){1,2}", ["a"]),
        ("", "(a|ab)(c|bcd)", ["abcd"]),
        ("U", "(a*)+b", ["aab", "ab", "aaab", "b"]),
        ("U", "(a*)*b", ["aab", "aabb"]),
        ("U", "(a+)+b", ["aab"]),
        ("", "(a*?)+b", ["aab", "aaab"]),
        ("", "(a*?)*?b", ["aab"]),
        ("U", "((a)*)+b", ["aab"]),
        ("U", "(a*b*)+?b", ["abab", "aabb"]),
        ("U", "(a|b*)+?", ["aabb"]),
        # A defect of the crate, carried on purpose so that a regeneration
        # cannot quietly adopt its answer. The crate reports 0-0 here and
        # `regexp`, perl and CPython all report 0-3; the first alternative
        # matches at the same start, so leftmost-first requires it. See the
        # reference-defect row in known-gaps.txt.
        ("", "a??b*c|a??c*", ["abc"]),
        # Unicode shorthands, ASCII POSIX classes: the one profile cell
        # where the crate and RE2 part company over every subject above
        # U+007F.
        ("", "\\w", ["\\u00e9", "a"]),
        ("", "\\d", ["\\u0663", "0"]),
        ("", "[[:word:]]", ["\\u00e9", "a"]),
        ("", "[[:alpha:]]", ["\\u00e9", "a"]),
        ("i", "[[:^alpha:]]", ["\\u017f", "0"]),
        ("i", "x\\b", ["x\\u212a", "x"]),
        ("i", "\\w", ["\\u017f"]),
        # `(?-u)`: it narrows the classes, cuts the fold orbit at U+0080,
        # and refuses every atom that could match half a character.
        #
        # Written inline rather than as a flag letter, and that is the
        # crate's own shape: `u` is on by default and there is no letter
        # that turns it off, only `(?-u)`. The driver spells the clearing
        # form as an `a` for the protocol's sake; a vector file may not,
        # because its `flags:` line is checked against the dialect's real
        # alphabet - and `a` is not in it.
        ("", "(?-u)\\w", ["\\u00e9", "a"]),
        ("i", "(?-u)k", ["\\u212a", "K"]),
        ("i", "k", ["\\u212a"]),
        ("", "(?-u)\\D", ["a"]),
        ("", "(?-u)\\W", ["a"]),
        ("", "(?-u)[^a]", ["b"]),
        ("", "(?-u).", ["a"]),
        ("", "(?-u)\\p{L}", ["a"]),
        ("", "(?-u)\\b", ["a"]),
        # The class set operators, which no other dialect here spells all
        # four of. Left-associative at one precedence level, which
        # `[a-c--b&&a]` is the only case that shows.
        ("", "[a-z--[aeiou]]", ["q", "e"]),
        ("", "[\\w&&\\d]", ["5", "x"]),
        ("", "[ab~~bc]", ["a", "b", "c"]),
        ("", "[a||b]", ["a", "b", "c"]),
        ("", "[a-c--b&&a]", ["a", "c", "b"]),
        ("", "[[a-c]--[b]]", ["a", "b"]),
        ("", "[^a--b]", ["b", "a"]),
        ("", "[[:alp(*FAIL)ha:]]*", ["", "a"]),
        # Extended mode, the escapes RE2 has not got, and the bound types.
        ("", "(?x) a  b", ["ab"]),
        ("", "(?x)a # c\nb", ["ab"]),
        ("", "\\u0041", ["A"]),
        ("", "\\u{41}", ["A"]),
        ("", "\\U0001F600", ["\\U0001F600"]),
        ("", "\\b{start}a", ["a", "ba"]),
        ("", "a\\b{end}", ["a", "ab"]),
        ("", "\\b{1,2}a", ["a"]),
        # A second quantifier is legal and means what a group would.
        ("", "a**", ["aa"]),
        ("", "a{2}{3}", ["aaaaaa"]),
        # What the crate refuses that RE2 takes, and what both refuse.
        ("", "\\Qa+b\\E", ["a+b"]),
        ("", "\\101", ["A"]),
        ("", "\\p{^L}", ["a"]),
        ("", "a{,3}", ["a{,3}"]),
        ("", "{", ["{"]),
        ("", "[\\d-x]", ["-"]),
        ("", "(?P<n>a)(?P<n>b)", ["ab"]),
        ("", "(?P<1a>a)", ["a"]),
        ("", "(a)\\1", ["aa"]),
        ("", "(?=a)", ["a"]),
        ("", "(?<=a)b", ["ab"]),
        ("", "\\Z", ["a"]),
        ("", "\\b{g}", ["a"]),
        ("", "\\p{Script=Greek}", ["\\u03b1", "a"]),
        ("", "\\p{gc=L}", ["a", "0"]),
        ("", "\\p{InGreek}", ["\\u03b1"]),
    ],
}


def ask(dialect, cases):
    """The pinned reference, in `grx_match`'s output vocabulary."""
    lines = "".join("%s\t%s\t%s\n" % (flags,
        binascii.hexlify(pattern.encode()).decode(),
        binascii.hexlify(subject.encode()).decode())
        for flags, pattern, subject in cases)
    finished = subprocess.run(linear_diff.RUNNERS[dialect].command(),
        input=lines, capture_output=True, text=True)
    if finished.returncode != 0:
        sys.stderr.write("the %s driver failed:\n%s\n" % (dialect,
            oracle_env.reference_stderr(finished.stderr).strip()[:600]))
        return None
    answers = finished.stdout.splitlines()
    if len(answers) != len(cases):
        sys.stderr.write("the %s driver answered %d of %d rows\n"
            % (dialect, len(answers), len(cases)))
        return None
    return answers


def record_for(flags, pattern, subject, answer):
    """One `.rxt` record, or None for a row with no expectation to write."""
    lines = ["pattern: " + make_vectors.escape(pattern)]
    if flags:
        lines.append("flags: " + flags)

    if answer == "compile":
        # Every way the reference refuses a pattern is one verdict here, the
        # same fold linear_diff.py makes: the two sides name their errors
        # differently and the question is whether both refuse. Writing the
        # sharper `expect: error syntax` would assert something the
        # reference did not say.
        lines.append("expect: refused")
        return "\n".join(lines) + "\n"
    if answer == "error":
        return None

    lines.append("subject: " + make_vectors.escape(subject))
    if answer == "nomatch":
        lines.append("expect: nomatch")
        return "\n".join(lines) + "\n"
    if not answer.startswith("match "):
        return None

    spans = []
    for field in answer.split()[1:]:
        if field == "-":
            spans.append("-")
            continue
        start, end = field.split(":")
        spans.append("%s-%s" % (start, end))
    lines.append("expect: " + " ".join(spans))
    return "\n".join(lines) + "\n"


def ucd_of(version):
    """The UCD the reference carries, out of its version line.

    The oracle's and not this library's, because that is what the `unicode:`
    header means - and here the two differ by more than anywhere else in this
    directory. Go 1.25.14 says 15.0.0 and the crate says 16.0.0, against
    tools/unicode/UCD_VERSION at 17.0.0. Writing 17.0.0 into these files
    would claim the corpus was answered from this library's release, which is
    the one thing the header is for saying.
    """
    for field in version.split(","):
        field = field.strip()
        if field.startswith("UCD "):
            return field[4:].strip()
        if field.startswith("Unicode "):
            return field[8:].strip()
    return "unknown"


def write_file(path, dialect, rows, answers, version):
    body = []
    refused = set()
    for (flags, pattern, subject), answer in zip(rows, answers):
        if answer == "compile":
            # One record per refused pattern rather than one per subject: a
            # refusal record carries no subject, so the rest would be exact
            # duplicates and a duplicate counts one fact twice in the rate
            # the suite publishes.
            if (flags, pattern) in refused:
                continue
            refused.add((flags, pattern))
        record = record_for(flags, pattern, subject, answer)
        if record:
            body.append(record)

    with open(path, "w", encoding="utf-8", newline="\n") as out:
        out.write("# generated by tools/oracle/make_linear_vectors.py\n")
        out.write("# oracle: %s\n" % version.replace("\n", "; "))
        out.write("# The expectations are the oracle's, not this library's: "
                  "a vector\n")
        out.write("# generated from what this library does would record a bug "
                  "rather than\n# a rule.\n")
        out.write("#\n")
        out.write("# A refused pattern has no subject in it, so it is one "
                  "record rather\n# than one per subject.\n")
        out.write("dialect: %s\n" % dialect)
        out.write("unicode: %s\n\n" % ucd_of(version))
        out.write("\n".join(body))

    sys.stderr.write("%s: %d records\n" % (path, len(body)))
    return len(body)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", default=None)
    parser.add_argument("--dialect", choices=sorted(NAMED_CASES), default=None)
    parser.add_argument("--seed", type=int, default=20260929)
    parser.add_argument("--patterns", type=int, default=400)
    parser.add_argument("--subjects", type=int, default=8)
    args = parser.parse_args(argv[1:])

    for dialect in ([args.dialect] if args.dialect else sorted(NAMED_CASES)):
        out_dir = args.out or os.path.join(
            ROOT, "tests", "data", "vectors", dialect)
        os.makedirs(out_dir, exist_ok=True)

        named = []
        for flags, pattern, subjects in NAMED_CASES[dialect]:
            for subject in subjects:
                named.append((flags, pattern, make_vectors.unescape(subject)))

        rng = random.Random(args.seed)
        generated = []
        for _ in range(args.patterns):
            pattern = linear_diff.make_pattern(rng, dialect)
            flags = rng.choice(linear_diff.FLAGSETS)
            for subject in rng.sample(linear_diff.SUBJECTS,
                    min(args.subjects, len(linear_diff.SUBJECTS))):
                generated.append((flags, pattern, subject))
        generated.sort()

        version = linear_diff.RUNNERS[dialect].version() or "unknown"

        for name, rows in (("named.rxt", named), ("generated.rxt", generated)):
            answers = ask(dialect, rows)
            if answers is None:
                return 2
            write_file(os.path.join(out_dir, name), dialect, rows, answers,
                       version)

    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
