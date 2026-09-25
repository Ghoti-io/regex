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

Every pattern is asked twice, once without `REG_NEWLINE` and once with it.
That flag is two rules - `^` and `$` become line anchors, and a newline is
matched by neither `.` nor a negated bracket expression - and it is the one
compile option these dialects have that changes what a *pattern* means.
Leaving it out made this tool blind to half of what the front end does:
the imported vectors carry eleven cases for the anchor half and none at all
for the other, so the second rule had no check anywhere until this axis
existed. `REG_ICASE` is still left to the vectors, which carry `options:`.

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
import re
import subprocess
import sys

import posix_runner

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

# The compile options to sweep. All three drivers spell REG_NEWLINE `n`.
NEWLINE_MODES = ["", "n"]


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


def is_known_deviation(pattern, subject, newline):
    """The one place these dialects knowingly differ from glibc.

    documentation/dialects.md section 6: without REG_NEWLINE a `^` is the
    start of the subject and a `$` its end, wherever in the pattern they
    stand. glibc answers that question two ways - `^b` against "a\nb" is
    nomatch, and `.*^b` against the same subject matches - and the
    consistent reading is the one implemented here.

    It is a deviation *without* REG_NEWLINE only. With it, `^` and `$` are
    line anchors in both, there is nothing for glibc to be inconsistent
    about, and excluding these cases there would hide real disagreements
    behind a filter written for the other mode.

    Excluded rather than left to fail, because a gate that always fails is a
    gate nobody reads. The shape is an anchor evaluated at a position next to
    a newline, which is any pattern holding one beside something that
    consumes - `$.` reaches it from the left and `.^` from the right - so the
    filter cannot be narrower than "an anchor, and a newline in the subject"
    without encoding glibc's own inconsistency. A pattern that is *only* an
    anchor is still compared, and so is every pattern against a subject with
    no newline in it.
    """
    if newline:
        return False
    return ("\n" in subject and len(pattern) > 1
        and ("^" in pattern or "$" in pattern))


REPEATED_GROUP = re.compile(r"\\?\)(?:\*|\\?\+|\\?\{)")
BACKREFERENCE = re.compile(r"\\[1-9]")


def is_glibc_backreference_defect(pattern, them, us):
    """glibc loses a group when a repeated group precedes a backreference.

    `()+(a)\1` against "aaaa" is 0-1 in glibc with group two **unset**,
    though the match it reports is one character long and group two is the
    only thing in the pattern that consumes one. `()(a)\1` - the same
    pattern with the repeat taken off - reports group two as 0-1, and so
    does `()+(a)` with the backreference taken off, so it is the two
    together that do it. `(){2}(a)\1` is worse: glibc reports *no match*
    where its own answer to `()(a)\1` is a match.

    A *stacked quantifier* does it as well, and needs no group to repeat:
    `a?+(a)\1` against "aab" is 0-2 in glibc with group one unset, where
    `a?(a)\1` and `a*(a)\1` - one quantifier instead of two - both report
    it as 0-1 there, and `a?+(a)` with the backreference taken off reports
    1-2. Found at seed 1015 of the soak, where the shape this asked for was
    the repeated group alone.

    An answer that contradicts the same implementation's answer to a
    neighbouring pattern is not a rule to follow, so these rows are counted
    rather than compared. Both halves of the shape are required, and the
    exclusion fires only where glibc reported *less* than this library did -
    an unset group where this library has a span, or no match at all -
    so a row where this library loses one is still a disagreement.
    """
    if not BACKREFERENCE.search(pattern):
        return False
    if not REPEATED_GROUP.search(pattern) and not stacked_quantifier(pattern):
        return False
    if them == "nomatch" and us.startswith("match"):
        return True
    if not them.startswith("match") or not us.startswith("match"):
        return False
    theirs = them.split()
    mine = us.split()
    return (len(theirs) == len(mine)
        and any(a == "-" and b != "-" for a, b in zip(theirs, mine)))


# A quantifier, and the one that may stand stacked on it. Both spellings,
# because a basic RE writes `\+`, `\?` and `\{m,n\}`.
QUANTIFIER = re.compile(r"\\?[*+?]|\\?\{([0-9]*)(,?)([0-9]*)\\?\}")


def quantifier_bounds(text):
    """The (min, max) a quantifier asks for, with None for no ceiling."""
    if text in ("*", "\\*"):
        return 0, None
    if text in ("+", "\\+"):
        return 1, None
    if text in ("?", "\\?"):
        return 0, 1
    inside = text.strip("\\{}").replace("\\", "")
    low, comma, high = inside.partition(",")
    if not comma:
        value = int(low or 0)
        return value, value
    return int(low or 0), (int(high) if high else None)


def stacked_quantifier(pattern):
    """Whether two quantifiers stand next to each other anywhere in it."""
    for match in QUANTIFIER.finditer(pattern):
        if QUANTIFIER.match(pattern, match.end()):
            return True
    return False

def stacked_keeps_empty(pattern):
    r"""Whether the pattern stacks two quantifiers in the shape glibc mishandles.

    Measured over all eighteen pairs of `*`, `+`, `?` and a bound, put to
    glibc as `(a|)XY` against "a" and "aaa". The rule its answers describe
    is: the **outer** quantifier can run more than once, and at least one
    of the two asks for an iteration - `?+`, `*+`, `++`, `{1,2}+`,
    `{0,2}+`, `+*`, `{1,2}*`, `?{1,2}`, `*{1,2}`, `+{1,2}`, `+{0,2}` and
    `{1,2}{1,2}` all keep the empty final iteration there, while `?*`,
    `**`, `+?`, `*?`, `??` and `{1,2}?` do not.

    The row this replaces asked only for an outer `+`, which is six of the
    twelve; `(a|)+*` was a disagreement at seed 1015 of the soak until the
    other six were measured.
    """
    for match in QUANTIFIER.finditer(pattern):
        following = QUANTIFIER.match(pattern, match.end())
        if not following:
            continue
        inner_min, _ = quantifier_bounds(match.group(0))
        outer_min, outer_max = quantifier_bounds(following.group(0))
        if outer_max is not None and outer_max <= 1:
            continue
        if inner_min >= 1 or outer_min >= 1:
            return True
    return False


def is_glibc_stacked_plus_defect(pattern, them, us):
    """glibc keeps an empty final iteration under a stacked `+` and not
    otherwise.

    `(a|)?+` over "aaaa" is 0-4 in glibc with group one at **4-4**, the
    empty span at the end. The same construct written out - `((a|)?)+` - is
    0-4 with group one at 3-4 there, and so is `(a|)?*`, and so is
    `(a|)+`. An extended RE stacks quantifiers freely and `a?+` *is*
    `(a?)+`, so those are two spellings of one pattern and glibc answers
    them differently.

    This library answers every spelling the same way, which is the one
    glibc gives for all but this one. The rule is narrow: the pattern holds
    a quantifier stacked on a quantifier whose outer one is `+`, glibc put
    the group at the empty span where the match ends, and this library put
    it somewhere else.
    """
    if not stacked_keeps_empty(pattern):
        return False
    if not them.startswith("match ") or not us.startswith("match "):
        return False
    theirs = them.split()
    mine = us.split()
    if len(theirs) < 2 or len(theirs) != len(mine) or theirs[1] != mine[1]:
        return False
    end = theirs[1].split(":")[1]
    empty_at_end = "%s:%s" % (end, end)
    return any(a == empty_at_end and a != b
        for a, b in zip(theirs[2:], mine[2:]))


def find(name):
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(ROOT, "build", platform, build, "apps",
                "tools", name)
            if os.path.exists(path):
                return path
    return None


def compare(dialect, seed, patterns, examples):
    # Both reference drivers are compiled inside the pinned image against
    # that image's glibc, so there is nothing here for `make tools` to have
    # built and nothing to look for on disk. musl is the one that can still
    # be genuinely absent: its sources are fetched rather than committed, so
    # a clone that has not run `tools/corpus/fetch.sh musl` has no second
    # POSIX opinion to offer - the property cannot exist, which is the one
    # shape of skip CONTAINERS.md 2.5 allows.
    drivers = {"glibc": posix_runner.command("posix_match"),
               "musl": posix_runner.command("musl_match")}
    ours = find("grx_match")
    if not ours:
        sys.stderr.write("run `make tools` first\n")
        return None
    wanted = DECIDED_BY[dialect]
    if "musl" in wanted and not drivers["musl"]:
        print("%s: skipped (musl's sources are not fetched; run "
              "tools/corpus/fetch.sh musl)" % dialect)
        return 0

    rng = random.Random(seed)
    atoms = ATOMS[dialect] + ILL_FORMED[dialect]
    built = set()
    for _ in range(patterns):
        built.add("".join(
            rng.choice(atoms) for _ in range(rng.randint(1, 3))))

    flag = BASIC_FLAG[dialect]
    cases = [(flag + newline, pattern, subject)
             for newline in NEWLINE_MODES
             for pattern in sorted(built) for subject in SUBJECTS]
    answers = {name: ask(drivers[name], cases) for name in wanted}
    mine = ask([ours, dialect], cases)
    if any(len(rows) != len(cases) for rows in answers.values()) \
            or len(mine) != len(cases):
        sys.stderr.write("a driver answered a different number of requests\n")
        return None

    disagreements = []
    compared = 0
    declined = 0
    unsettled = 0
    defect = 0
    known = 0
    for index, ((flags, pattern, subject), us) in enumerate(zip(cases, mine)):
        newline = "n" in flags
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
        if is_known_deviation(pattern, subject, newline):
            known += 1
            continue
        if is_glibc_backreference_defect(
                pattern, expected, normalise_ours(us)):
            defect += 1
            continue
        if is_glibc_stacked_plus_defect(
                pattern, expected, normalise_ours(us)):
            defect += 1
            continue
        disagreements.append(
            (pattern, subject, newline, expected, normalise_ours(us)))

    for pattern, subject, newline, them, us in disagreements[:examples]:
        print("  %-24s on %-8s %-12s oracle=%-20s ours=%s"
              % (repr(pattern), repr(subject),
                 "REG_NEWLINE" if newline else "", them, us))
    print("%s: %d patterns x %d subjects x %d newline modes = %d cases, "
          "%d compared against %s, %d the oracles left unsettled, "
          "%d the anchor deviation, %d a glibc defect, %d disagreements"
          % (dialect, len(built), len(SUBJECTS), len(NEWLINE_MODES),
             len(cases), compared, " and ".join(wanted), unsettled, known,
             defect, len(disagreements)))
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
