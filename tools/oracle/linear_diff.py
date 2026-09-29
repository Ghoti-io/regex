#!/usr/bin/env python3
r"""Compare the two guaranteed-linear front ends against their references.

documentation/plan.md WP-34 and WP-35. One tool for both dialects because
they are one front end - src/syntax/re2.c reads both, deny-by-default, with
a two-valued `flavour()` - and a gate that asked only one of them would leave
every `flavour()` branch with a side nobody checks.

    tools/oracle/linear_diff.py --dialect re2   [--seed N] [--strict]
    tools/oracle/linear_diff.py --dialect rust  [--seed N] [--strict]

**Most of the vocabulary below is what each dialect does NOT have**, which is
the opposite balance from every other differential here and is the shape the
subject demands. RE2 and Rust are defined by subtraction: a front end for
them is mostly refusals, so a corpus made of what they accept would leave the
larger half of the code with no gate on it. A row where this library accepts
a pattern the reference rejects is the failure that matters - it is this
library telling a caller their pattern is valid for an engine that rejects
it, which documentation/design.md section 4 names as the thing a dialect
table exists to stop.

**The two dialects do not share a vocabulary and must not.** Nine constructs
belong to exactly one of them - `\Q..\E` and octal are RE2's, `(?x)`, `\u`,
`\U`, the class set operators and `\b{start}` are the crate's - so asking one
reference about the other's spellings would measure nothing, and the ACCEPTS
and REFUSES tables below are per dialect for that reason.

**The Unicode skew is real here and is bounded by the subject list.** Go
1.25.14 carries UCD 15.0.0 and the crate carries 16.0.0, against this
library's 17.0.0. Every subject below is a character assigned long before
15.0.0 and in a general category that has not moved, so a disagreement this
tool reports is about matching rather than about the Consortium - but that is
a property of the list rather than of the comparison, which is why the
reference's version is printed on every run.

Copyright 2026 by Corey Pennycuff
"""

import argparse
import binascii
import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
ROOT = os.path.dirname(os.path.dirname(HERE))

import go_runner
import oracle_env
import rust_runner

UCD_PIN = os.path.join(ROOT, "tools", "unicode", "UCD_VERSION")
UCD_VERSION = (open(UCD_PIN).read().strip()
               if os.path.exists(UCD_PIN) else "unknown")

# What both dialects have. Alternations with branches of different lengths
# are here on purpose: leftmost-first is the cell that shape asks about, and
# it is the one these two engines could most plausibly have got from POSIX
# instead - they are automata, and an automaton that reported the longest
# match would be within its rights.
SHARED = [
    "a", "b", ".", "\\.", "\\n", "\\t", "\\x41", "\\x{1F600}",
    "[ab]", "[^ab]", "[a-c]", "[\\d]", "[a-b-c]", "[-a]", "[a-]", "[[:alpha:]]",
    "[[:^alpha:]]", "[[:word:]]", "[\\x41-\\x5A]",
    "\\d", "\\D", "\\w", "\\W", "\\s", "\\S",
    "\\p{L}", "\\P{L}", "\\pL", "\\p{Greek}", "\\p{Nd}", "\\p{Any}",
    "^", "$", "\\A", "\\z", "\\b", "\\B",
    "(a)", "(?:a)", "(?P<n>a)", "(?<m>a)", "(?i:a)", "(?-i:a)", "(?i)a",
    "a|ab", "ab|a", "(a|ab)", "(ab|a)", "(|a)", "(a|)",
    "a*", "a+", "a?", "a*?", "a+?", "a??",
    "a{2}", "a{1,3}", "a{2,}", "a{1,3}?",
    "(a*)*", "(a*)+", "(a*){2}", "((a)|b)+", "(?:(a)|b){2}",
    "^*", "\\b*",
]

# One dialect each, probed against that dialect's reference before being put
# here. A construct in the wrong list would be asking a reference about a
# spelling it has never had.
ONLY = {
    "re2": ["\\Qa+b\\E", "a\\Q+\\Eb", "\\Qa", "\\Q\\E", "\\101", "\\0",
            "\\p{^L}", "a{,3}",
            "a{2,", "[\\d-x]", "{", "(?P<n>a)(?P<n>b)", "(?P<1a>a)"],
    "rust": ["(?x) a  b", "(?x)a # c\nb", "\\u0041", "\\U0001F600",
             "\\u{41}", "[a-z--[aeiou]]", "[\\w&&\\d]", "[ab~~bc]",
             "(?R)^b", "(?mR)a$", "[a||b]", "[a-c--b&&a]", "[^a--b]",
             "[^\\w&&\\d]",
             "[^[a-c]--[b]]", "\\b{start}", "\\b{end}", "a**",
             "a{2}{3}", "(?-u)\\w", "(?i-u)k"],
}

# What neither has, and every one of them is a construct some other dialect
# in this library does - which is what makes them worth generating. Each was
# probed against both references.
REFUSED_BOTH = [
    "(a)\\1", "\\1", "\\g{1}", "\\k<n>", "(?P=n)",
    "(?=a)", "(?!a)", "(?<=a)", "(?<!a)", "(?>a)",
    "(?#c)", "(?(1)a|b)", "(?1)", "(?&n)", "(?P>n)", "(?|a|b)",
    "(?'n'a)", "(?C1)", "(*FAIL)", "(*ACCEPT)", "(*UTF)",
    "\\Z", "\\G", "\\K", "\\R", "\\X", "\\N", "\\h", "\\H", "\\V", "\\C",
    "\\cA", "\\e", "\\y", "\\x4", "\\8", "\\9",
    "[]", "[^]", "[a-\\d]", "[\\b]", "[\\A]",
    "a{1001}", "a{3,2}", "a*+", "(?L)a", "\\p{Foo}", "\\p{InGreek}",
]

# And what each dialect alone refuses, which is the half a shared list cannot
# reach: `\Q` is RE2's and an error in the crate, `(?x)` is the crate's and
# an error in RE2. A front end that answered from one list would accept each
# dialect's constructs under both names.
REFUSED_ONLY = {
    "re2": ["(?x) a  b", "\\u0041", "\\U0001F600",
            "a**", "a{2}{3}", "(?-u)\\w",
            # A `\E` that closes nothing, which perl accepts and `regexp`
            # does not - and which no other entry in these tables spells.
            "a\\Eb", "\\E"],
    "rust": ["\\Qa+b\\E", "\\101", "\\0", "\\p{^L}", "a{,3}", "a{2,",
             "[\\d-x]", "{", "(?P<n>a)(?P<n>b)", "(?P<1a>a)"],
}

# The shape battery: a cross-product rather than a sample, and it is here
# because the random pass above could not see the cell it covers. Both
# references end a loop over a body that can match empty by meeting a state
# the walk has already reached, which is not any of the three empty-iteration
# rules a backtracking dialect can hold - and 62 of these 5,040 rows moved
# when this library learned that. The random pass reached exactly one of the
# 62 in 105,000 rows, because the shape needs a nested potentially-empty
# loop *and* something after it that forces the loop to give ground, and
# concatenating four atoms from a vocabulary reaches that pairing by luck.
#
# So it is enumerated. Sampling a region this small is the same mistake as
# not testing it: the cell is 22 patterns wide, and a gate that meets a fifth
# of it reports a fifth of a defect.
SHAPE_BODIES = ["(a*)", "(a*?)", "(a|)", "(|a)", "(a?)", "((a)*)", "(a*b*)",
                "(a|b*)", "(a*)(b*)", "(?:a*)", "(a{0,2})", "(a*|b)"]
SHAPE_QUANTIFIERS = ["*", "+", "*?", "+?", "{0,3}", "{1,3}", "{2,4}"]
SHAPE_TAILS = ["", "b", "c"]
SHAPE_SUBJECTS = ["", "b", "a", "aa", "aab", "ab", "aaab", "abab", "bb",
                  "aabb"]
SHAPE_FLAGSETS = ["", "U"]


def shape_cases():
    """Every combination, because the cell is small enough to enumerate."""
    for body in SHAPE_BODIES:
        for quantifier in SHAPE_QUANTIFIERS:
            for tail in SHAPE_TAILS:
                for subject in SHAPE_SUBJECTS:
                    for flags in SHAPE_FLAGSETS:
                        yield (flags, body + quantifier + tail, subject)


SUBJECTS = [
    "", "a", "b", "ab", "ba", "aa", "aab", "abab", "abc", "c",
    "a\n", "\na", "a\nb", "\n", "aaa", "aaaa", "a\r\nb",
    "é", "aé", "٣", " ", " ", "ſ", "K", "K",
    "A", "AB", "aB", "-", "a-b", "]", "[", "{", "a{,3}", "α",
]

# `x` is absent even for the crate: it changes what the *pattern text* means,
# and a generated pattern is not written with verbose mode's whitespace rules
# in mind. A pattern that asks for it does so with an inline `(?x)`, which is
# in the ONLY table above and carries its own text.
FLAGSETS = ["", "i", "m", "s", "im", "is", "ms", "ims", "U"]

RUNNERS = {"re2": go_runner, "rust": rust_runner}


def find(name):
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(ROOT, "build", platform, build, "apps",
                                "tools", name)
            if os.path.exists(path):
                return path
    return None


def make_pattern(rng, dialect):
    """Build one pattern by concatenating, quantifying and alternating."""
    vocabulary = SHARED + ONLY[dialect]
    pieces = []
    for _ in range(rng.randint(1, 4)):
        atom = rng.choice(vocabulary)
        if rng.random() < 0.25 and not atom.endswith(("*", "+", "?", "}")):
            atom += rng.choice(["*", "+", "?", "*?", "+?", "{1,2}"])
        pieces.append(atom)
    pattern = "".join(pieces)
    if rng.random() < 0.2:
        pattern = pattern + "|" + rng.choice(vocabulary)
    # One row in six carries something the dialect refuses, so that the
    # refusals are exercised in combination rather than alone - which is
    # where a reader that refuses the right construct for the wrong reason
    # shows up.
    if rng.random() < 0.167:
        refused = REFUSED_BOTH + REFUSED_ONLY[dialect]
        where = rng.randint(0, len(pattern))
        pattern = pattern[:where] + rng.choice(refused) + pattern[where:]
    return pattern


def encode(cases):
    return "".join("%s\t%s\t%s\n"
                   % (flags,
                      binascii.hexlify(pattern.encode()).decode(),
                      binascii.hexlify(subject.encode()).decode())
                   for flags, pattern, subject in cases)


def ask_reference(dialect, cases):
    finished = subprocess.run(RUNNERS[dialect].command(), input=encode(cases),
                              capture_output=True, text=True)
    if finished.returncode != 0:
        sys.stderr.write(
            "the %s driver failed:\n%s\n"
            % (dialect,
               oracle_env.reference_stderr(finished.stderr).strip()[:800]))
        return []
    return finished.stdout.splitlines()


def ask_ours(driver, dialect, cases):
    try:
        finished = subprocess.run([driver, dialect], input=encode(cases),
                                  capture_output=True, text=True, timeout=900)
    except subprocess.TimeoutExpired:
        sys.stderr.write("grx_match did not finish within 900s\n")
        return []
    return finished.stdout.splitlines()


def normalise_ours(line):
    """A grx_match line in the shape the reference answers in.

    Both halves of this matter. grx_match names the engine it ran on, which
    the reference has no equivalent of; and it names the diagnostic when it
    refuses, where the reference says only that it refused. Folding the
    diagnostic away is what lets a *refusal* be compared at all - and since
    most of this corpus is refusals, leaving it in would make the gate agree
    about almost nothing while reporting a number.
    """
    if line.startswith("match "):
        return "match " + " ".join(line.split()[2:])
    if line.startswith("compile"):
        return "compile"
    if line.startswith("error"):
        return "error"
    if line.startswith("unsupported"):
        return "unsupported"
    return line


def trim_unset(line):
    """Drop trailing `-` fields, which neither side is consistent about."""
    if not line.startswith("match "):
        return line
    fields = line.split()
    while len(fields) > 2 and fields[-1] == "-":
        fields.pop()
    return " ".join(fields)


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--dialect", choices=sorted(RUNNERS), default="re2")
    parser.add_argument("--seed", type=int, default=20260929)
    # 3,000 rather than 1,200: at 1,200 the seed reached a `\E` with no run
    # open and at 800 it did not, which is the whole argument for a bigger
    # default. `make check-oracle-soak` is what sweeps the seeds.
    parser.add_argument("--patterns", type=int, default=3000)
    parser.add_argument("--subjects", type=int, default=0,
                        help="subjects per pattern; 0 means all of them")
    parser.add_argument("--examples", type=int, default=12)
    parser.add_argument("--strict", action="store_true",
                        help="exit 1 if the two disagree anywhere")
    parser.add_argument("--no-shapes", dest="shapes", action="store_false",
                        help="skip the enumerated nested-loop battery")
    parser.set_defaults(shapes=True)
    args = parser.parse_args()

    driver = find("grx_match")
    if not driver:
        sys.stderr.write("grx_match not built; run `make tools`\n")
        return 2

    rng = random.Random(args.seed)
    cases = []
    for _ in range(args.patterns):
        pattern = make_pattern(rng, args.dialect)
        flags = rng.choice(FLAGSETS)
        subjects = (SUBJECTS if args.subjects <= 0
                    else rng.sample(SUBJECTS,
                                    min(args.subjects, len(SUBJECTS))))
        for subject in subjects:
            cases.append((flags, pattern, subject))
    if args.shapes:
        cases.extend(shape_cases())

    theirs = ask_reference(args.dialect, cases)
    if not theirs:
        return 2
    mine = [normalise_ours(line)
            for line in ask_ours(driver, args.dialect, cases)]
    if len(mine) != len(cases) or len(theirs) != len(cases):
        sys.stderr.write("answered %d and %d of %d rows\n"
                         % (len(theirs), len(mine), len(cases)))
        return 2

    disagreements = []
    # Counted apart because they are not the same defect. "We accept, they
    # refuse" is this library promising a caller an engine will run their
    # pattern when it will not; "we refuse, they accept" is a dialect this
    # library has not finished. Both are failures and a reader triaging them
    # needs to know which arrived.
    over = 0
    under = 0
    for case, them, us in zip(cases, theirs, mine):
        if trim_unset(them) == trim_unset(us):
            continue
        if them == "compile" and us != "compile":
            over += 1
        elif us == "compile" and them != "compile":
            under += 1
        disagreements.append((case, them, us))

    # What the reference actually answered. "0 disagreements" over rows it
    # refused outright would be a gate agreeing about nothing, and a run
    # whose `match` share collapses has stopped asking the question even
    # while its disagreement count stays at zero.
    kinds = {}
    for line in theirs:
        head = line.split()[0] if line else "empty"
        kinds[head] = kinds.get(head, 0) + 1
    shape = ", ".join("%d %s" % (kinds[k], k) for k in sorted(kinds))

    print("linear_diff: %s against %s (our tables: UCD %s)"
          % (args.dialect, RUNNERS[args.dialect].version(), UCD_VERSION))
    print("linear_diff: %d rows (%s), %d disagreements"
          " (%d we accept and it refuses, %d the other way)"
          % (len(cases), shape, len(disagreements), over, under))
    for (flags, pattern, subject), them, us in disagreements[:args.examples]:
        print("  /%s/%s on %r" % (pattern, flags, subject))
        print("    %6s: %s" % (args.dialect, them))
        print("      ours: %s" % us)
    if len(disagreements) > args.examples:
        print("  ... and %d more" % (len(disagreements) - args.examples))

    if args.strict and disagreements:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
