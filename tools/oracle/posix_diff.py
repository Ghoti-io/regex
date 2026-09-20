#!/usr/bin/env python3
"""Compare the POSIX and GNU front ends against glibc, pattern by pattern.

The vectors imported from Spencer's test set are 429 cases somebody chose.
This is the other half: patterns built by combination, so that the pairs
nobody thought to write down are asked too. It is what found the one place
`^` disagrees - documentation/dialects.md section 6 - which no imported
vector reaches.

Both sides are asked through the same line protocol:
`tools/oracle/posix_match.c` is glibc's regcomp and regexec, and
`tools/oracle/grx_match.c` is this library. The comparison is on the answer
string, so a disagreement about *where* a match is counts as one, not only a
disagreement about whether there is one.

Only patterns with no flags: POSIX's options are arguments to regcomp and
grx_match spells its options as a dialect's flag letters, of which these
dialects have none. Caseless and newline behaviour is covered by the
imported vectors, which carry `options:` instead.

Usage:
    tools/oracle/posix_diff.py [--seed N] [--patterns N] [--examples N]

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
                "()", "(a)\\1", "\\w", "[]a]", "(a|)", "\\<", "\\>"],
    "gnu-bre": ["a", "b", ".", "[ab]", "[^a]", "[a-c]", "\\(a\\)",
                "\\(a\\|b\\)", "a*", "a\\+", "a\\?", "a\\{2\\}", "^", "$",
                "\\.", "[[:alpha:]]", "\\(a\\)\\1", "\\w", "[]a]", "\\<",
                "\\>"],
}

SUBJECTS = ["", "a", "b", "ab", "aab", "abc", "aaa", "a.b", "[a]", "()",
            "\n", "a\nb", "AB", "abab", "a)b"]

# The flag letter posix_match wants for a basic RE; grx_match takes the
# dialect by name instead.
BASIC_FLAG = {"gnu-ere": "", "gnu-bre": "b"}


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
    """grx_match names the engine it used; glibc has no such field."""
    if line.startswith("match "):
        return "match " + " ".join(line.split()[2:])
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
    glibc = find("posix_match")
    ours = find("grx_match")
    if not glibc or not ours:
        sys.stderr.write("run `make tools` first\n")
        return None

    rng = random.Random(seed)
    atoms = ATOMS[dialect]
    built = set()
    for _ in range(patterns):
        built.add("".join(
            rng.choice(atoms) for _ in range(rng.randint(1, 3))))

    flag = BASIC_FLAG[dialect]
    cases = [(flag, pattern, subject)
             for pattern in sorted(built) for subject in SUBJECTS]
    theirs = ask([glibc], cases)
    mine = ask([ours, dialect], cases)
    if len(theirs) != len(cases) or len(mine) != len(cases):
        sys.stderr.write("a driver answered a different number of requests\n")
        return None

    disagreements = []
    compared = 0
    declined = 0
    known = 0
    for (_, pattern, subject), them, us in zip(cases, theirs, mine):
        if them.startswith("skip"):
            declined += 1
            continue
        compared += 1
        if them == normalise_ours(us):
            continue
        if is_known_deviation(pattern, subject):
            known += 1
            continue
        disagreements.append((pattern, subject, them, normalise_ours(us)))

    for pattern, subject, them, us in disagreements[:examples]:
        print("  %-24s on %-8s glibc=%-20s ours=%s"
              % (repr(pattern), repr(subject), them, us))
    print("%s: %d patterns x %d subjects = %d cases, %d compared, "
          "%d the anchor deviation, %d disagreements"
          % (dialect, len(built), len(SUBJECTS), len(cases), compared,
             known, len(disagreements)))
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
