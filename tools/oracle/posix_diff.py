#!/usr/bin/env python3
"""Compare the POSIX and GNU front ends against glibc and musl, pattern by pattern.

The vectors imported from Spencer's test set are 429 cases somebody chose.
This is the other half: patterns built by combination, so that the pairs
nobody thought to write down are asked too. It is what found the one place
`^` disagrees - documentation/dialects.md section 6 - which no imported
vector reaches.

Every side is asked through the same line protocol: `posix_match.c` is
glibc's regcomp and regexec, `musl_match.c` is musl's, and `grx_match.c` is
this library. The comparison is on the answer string, so a disagreement about
*where* a match is counts as one, not only a disagreement about whether there
is one, and a disagreement about whether the pattern compiles at all counts
too.

Which oracle decides depends on which dialect is being asked about, and the
difference is the point:

  - `gnu-bre` and `gnu-ere` *are* glibc. glibc is the definition of those two
    rows, so it alone decides and musl is not consulted.
  - `posix-bre` and `posix-ere` have no implementation on this machine.
    Neither oracle is strict POSIX - glibc's regcomp is GNU, and musl's BRE
    takes `\\|`, `\\+` and `\\?` while refusing the `[[.x.]]` POSIX requires -
    so neither can decide alone. What counts is their *agreement*: where two
    implementations sharing no code answer the same way, this library is
    expected to answer that way too, and where they differ the question is
    recorded as unsettled and nobody is judged by it. The atoms below are
    chosen so that the constructs these dialects do not have never arise.

Only patterns with no flags: POSIX's options are arguments to regcomp and
grx_match spells its options as a dialect's flag letters, of which these
dialects have none. Caseless and newline behaviour is covered by the
imported vectors, which carry `options:` instead.

Usage:
    tools/oracle/posix_diff.py [--seed N] [--patterns N] [--examples N]

Without musl_match the two POSIX dialects are skipped and said to be skipped
- run `tools/corpus/fetch.sh musl` and `make tools` for them.

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

# One entry per construct the dialect has, so that a combination exercises
# the interactions rather than one rule at a time.
ATOMS = {
    "gnu-ere": ["a", "b", ".", "[ab]", "[^a]", "[a-c]", "(a)", "(a|b)", "a*",
                "a+", "a?", "a{2}", "a{1,2}", "^", "$", "\\.", "[[:alpha:]]",
                "()", "(a)\\1", "\\w", "[]a]", "(a|)", "\\<", "\\>",
                "a|ab", "ab|a", "(a|ab)", "(ab|a)", "a|aa"],
    "gnu-bre": ["a", "b", ".", "[ab]", "[^a]", "[a-c]", "\\(a\\)",
                "\\(a\\|b\\)", "a*", "a\\+", "a\\?", "a\\{2\\}", "^", "$",
                "\\.", "[[:alpha:]]", "\\(a\\)\\1", "\\w", "[]a]", "\\<",
                "\\>", "a\\|ab", "ab\\|a", "\\(a\\|ab\\)",
                "\\(ab\\|a\\)\\1"],
    "posix-ere": ["a", "b", ".", "[ab]", "[^a]", "[a-c]", "(a)", "(a|b)",
                  "a*", "a+", "a?", "a{2}", "a{1,2}", "^", "$", "\\.",
                  "[[:alpha:]]", "()", "(a|)", "[]a]",
                  "a|ab", "ab|a", "(a|ab)", "(ab|a)", "a|aa"],
    "posix-bre": ["a", "b", ".", "[ab]", "[^a]", "[a-c]", "\\(a\\)", "a*",
                  "a\\{2\\}", "^", "$", "\\.", "[[:alpha:]]",
                  "\\(a\\)\\1", "[]a]", "\\(ab*\\)\\1", "\\(a*\\)\\1"],
}

# Which oracles decide for which dialect, and whether one of them is the
# definition. See the module docstring: glibc *is* gnu-bre and gnu-ere, and
# neither oracle is posix-bre or posix-ere.
DECIDED_BY = {
    "gnu-ere": ("glibc",),
    "gnu-bre": ("glibc",),
    "posix-ere": ("glibc", "musl"),
    "posix-bre": ("glibc", "musl"),
}

# Atoms that are not well formed on their own, so that the generated
# patterns include ones an implementation should *refuse*. Until these were
# added no generated pattern was rejected by anybody, which meant the whole
# accept-or-reject half of the comparison had never once been exercised - the
# tool reported zero disagreements without having asked the question. Adding
# them found the anchor-quantifier rules in src/syntax/posix.c, which had been
# written for `^` and `$` and left every other anchor alone.
#
# Deliberately no lone backslash. Concatenated with the `b` atom next to it, a
# bare `\\` would spell `\\b` and put a GNU escape into a POSIX pattern - the
# generator would then be asking about a construct the dialect does not have
# and blaming the front end for the answer.
ILL_FORMED = {
    "gnu-ere": ["(", ")", "[", "a{", "{1}", "*", "+", "?", "a{1", "[a-"],
    "posix-ere": ["(", ")", "[", "a{", "{1}", "*", "+", "?", "a{1", "[a-"],
    "gnu-bre": ["\\(", "\\)", "[", "a\\{", "\\{1\\}", "*", "a\\{1", "[a-"],
    "posix-bre": ["\\(", "\\)", "[", "a\\{", "\\{1\\}", "*", "a\\{1", "[a-"],
}

SUBJECTS = ["", "a", "b", "ab", "aab", "abc", "aaa", "a.b", "[a]", "()",
            "\n", "a\nb", "AB", "abab", "a)b", "ababaaa", "aaaa"]

# The flag letter posix_match wants for a basic RE; grx_match takes the
# dialect by name instead.
BASIC_FLAG = {"gnu-ere": "", "gnu-bre": "b",
              "posix-ere": "", "posix-bre": "b"}


def ask(command, cases):
    lines = []
    for flags, pattern, subject in cases:
        lines.append("%s\t%s\t%s" % (flags,
            binascii.hexlify(pattern.encode()).decode(),
            binascii.hexlify(subject.encode()).decode()))
    finished = subprocess.run(command, input="\n".join(lines) + "\n",
        capture_output=True, text=True)
    return finished.stdout.splitlines()


def normalise_ours(line):
    """grx_match's answer in the shape the POSIX drivers write theirs.

    Three differences, not one. grx_match names the engine it used and the
    POSIX drivers have no such field; it names the diagnostic when it rejects
    a pattern, where they say only that regcomp failed; and it distinguishes
    a run-time error from a compile-time one, which a POSIX driver cannot.

    The second of those was missing until the musl oracle was added, and its
    absence made this tool blind to exactly half of what it exists to check:
    a pattern this library rejected could never compare equal to `compile`,
    so an accept/reject disagreement would have been reported - but a *match*
    of opinions about rejection could never be confirmed either, and no
    generated pattern happened to be rejected by both, so the gate passed
    without ever having asked the question.
    """
    if line.startswith("match "):
        return "match " + " ".join(line.split()[2:])
    if line.startswith("compile"):
        return "compile"
    if line.startswith("error") or line == "unsupported":
        return "compile"
    return line


def is_known_deviation(pattern, subject):
    """The one place these dialects knowingly differ from glibc.

    documentation/dialects.md section 6: without REG_NEWLINE a `^` is the
    start of the subject and a `$` its end, wherever in the pattern they
    stand. glibc answers that question two ways - `^b` against "a\nb" is
    nomatch, and `.*^b` against the same subject matches - and the
    consistent reading is the one implemented here.

    Excluded rather than left to fail, because a gate that always fails is a
    gate nobody reads. The shape is an anchor evaluated at a position next to
    a newline, which is any pattern holding one beside something that
    consumes - `$.` reaches it from the left and `.^` from the right - so the
    filter cannot be narrower than "an anchor, and a newline in the subject"
    without encoding glibc's own inconsistency. A pattern that is *only* an
    anchor is still compared, and so is every pattern against a subject with
    no newline in it.
    """
    return ("\n" in subject and len(pattern) > 1
        and ("^" in pattern or "$" in pattern))


def find(name):
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(ROOT, "build", platform, build, "apps",
                "tools", name)
            if os.path.exists(path):
                return path
    return None


def compare(dialect, seed, patterns, examples):
    drivers = {"glibc": find("posix_match"), "musl": find("musl_match")}
    ours = find("grx_match")
    if not drivers["glibc"] or not ours:
        sys.stderr.write("run `make tools` first\n")
        return None
    wanted = DECIDED_BY[dialect]
    if "musl" in wanted and not drivers["musl"]:
        print("%s: skipped (no musl_match; run tools/corpus/fetch.sh musl "
              "and `make tools`)" % dialect)
        return 0

    rng = random.Random(seed)
    atoms = ATOMS[dialect] + ILL_FORMED[dialect]
    built = set()
    for _ in range(patterns):
        built.add("".join(
            rng.choice(atoms) for _ in range(rng.randint(1, 3))))

    flag = BASIC_FLAG[dialect]
    cases = [(flag, pattern, subject)
             for pattern in sorted(built) for subject in SUBJECTS]
    answers = {name: ask([drivers[name]], cases) for name in wanted}
    mine = ask([ours, dialect], cases)
    if any(len(rows) != len(cases) for rows in answers.values()) \
            or len(mine) != len(cases):
        sys.stderr.write("a driver answered a different number of requests\n")
        return None

    disagreements = []
    compared = 0
    declined = 0
    unsettled = 0
    known = 0
    for index, ((_, pattern, subject), us) in enumerate(zip(cases, mine)):
        theirs = [answers[name][index] for name in wanted]
        if any(answer.startswith("skip") for answer in theirs):
            declined += 1
            continue
        # With two oracles the standard is their agreement; where they differ
        # there is no answer to hold this library to, and saying so is more
        # useful than picking one of them.
        if len(set(theirs)) != 1:
            unsettled += 1
            continue
        expected = theirs[0]
        compared += 1
        if expected == normalise_ours(us):
            continue
        if is_known_deviation(pattern, subject):
            known += 1
            continue
        disagreements.append((pattern, subject, expected, normalise_ours(us)))

    for pattern, subject, them, us in disagreements[:examples]:
        print("  %-24s on %-8s oracle=%-20s ours=%s"
              % (repr(pattern), repr(subject), them, us))
    print("%s: %d patterns x %d subjects = %d cases, %d compared against %s, "
          "%d the oracles left unsettled, %d the anchor deviation, "
          "%d disagreements"
          % (dialect, len(built), len(SUBJECTS), len(cases), compared,
             " and ".join(wanted), unsettled, known, len(disagreements)))
    return len(disagreements)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--patterns", type=int, default=400)
    parser.add_argument("--examples", type=int, default=12)
    args = parser.parse_args(argv[1:])

    total = 0
    for index, dialect in enumerate(sorted(ATOMS)):
        found = compare(dialect, args.seed + index, args.patterns,
            args.examples)
        if found is None:
            return 2
        total += found
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
