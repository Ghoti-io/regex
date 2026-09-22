#!/usr/bin/env python3
"""Compare the Perl-family front end against perl, on patterns nobody wrote.

WP-20 landed with its conformance rates measured against two imported
corpora - pcre2test's `testinput1` and `testinput2`, and Perl's own
`re_tests`. What it did not land was this: a generator, so that the
comparison is not limited to the cases whoever wrote those files thought of.

The difference matters more than it sounds. A corpus is a set of questions
somebody already knew to ask, and a front end passes it by handling the
constructs that appear in it. `tools/oracle/posix_diff.py` is the precedent:
the POSIX corpus was at 100% while `a|ab` against "ab" answered 0-1 in four
shipped dialects, because every alternation in the corpus happened to have
branches of the same length. A generator does not know what is interesting
and so does not skip it.

**perl is the definition here**, the way glibc is the definition for
`gnu-bre` and `gnu-ere`: `GRX_SYNTAX_PERL` means "what perl does", so one
oracle decides and there is no agreement to take. That is not true of
`GRX_SYNTAX_PCRE`, whose reference is pcre2 and which this tool does not
cover - see the note at the bottom of the file.

Usage:
    tools/oracle/perl_diff.py [--seed N] [--patterns N] [--examples N]

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

# One entry per construct the dialect has, so that a combination exercises the
# interactions rather than one rule at a time. Ordered the way dialects.md
# describes them, and deliberately including several whose branches are
# different lengths - the shape that found WP-24.
ATOMS = [
    # Literals and the dot.
    "a", "b", ".", "\\.", "\\n", "\\t",
    # Classes, in all three spellings the dialect has.
    "[ab]", "[^a]", "[a-c]", "[]a]", "[[:alpha:]]", "[[:^digit:]]",
    "\\d", "\\D", "\\w", "\\W", "\\s", "\\S",
    # Groups, named and not.
    "(a)", "(?:a)", "(?<n>a)", "(?'m'a)", "(a|b)", "(a|)", "()",
    # Quantifiers: greedy, lazy and possessive.
    "a*", "a+", "a?", "a{2}", "a{1,2}", "a{1,}",
    "a*?", "a+?", "a??", "a{1,2}?",
    "a*+", "a++", "a?+",
    # Anchors and boundaries.
    "^", "$", "\\b", "\\B", "\\A", "\\z", "\\Z", "\\G",
    # References.
    "(a)\\1", "(?<n>a)\\k<n>", "(?<n>a)\\g{n}", "(a)\\g1", "(a)\\g{-1}",
    # Lookaround, both directions and both senses.
    "(?=a)", "(?!a)", "(?<=a)", "(?<!a)",
    # Atomic grouping and inline modifiers.
    "(?>a)", "(?i)a", "(?i:a)", "(?-i:a)", "(?^i:a)",
    # A comment, which changes how the rest is read.
    "(?#c)",
    # `\Q...\E` is deliberately absent. perl cannot be asked about it
    # through this transport: `\Q` is double-quotish processing, done when
    # the *source* is tokenised, so a pattern that arrives in a variable -
    # which is the only way a driver can pass one - never goes through it.
    # `qr/$p/` with $p holding `\Qa.b\E` matches neither "a.b" nor "axb" nor
    # anything else that was tried. It is not that perl disagrees; it is that
    # the question cannot be put. pcre2test, which reads its pattern as
    # source, quotes it and matches "a.b" alone, which is what this library
    # does - so the construct is checked in tests/unit/test_perl.cpp
    # (AQuotedRunIsLiteralAndNotAnAtom) rather than here.
    # Alternations whose branches are different lengths. Leftmost-first says
    # the first branch wins even when a later one is longer, and an engine
    # that quietly took the longest would pass a corpus without these.
    "a|ab", "ab|a", "(a|ab)", "(ab|a)", "a|aa", "(a|ab)*",
    # Escapes with their own grammar inside the braces.
    "\\x{61}", "\\o{141}", "\\x61", "\\N{U+0061}", "\\p{L}", "\\P{L}",
    "\\p{Latin}", "\\p{^L}", "[\\p{L}]",
    # The single-letter classes that are not \d, \w or \s.
    "\\h", "\\v", "\\H", "\\V", "\\R", "\\N",
    # Conditionals. Each carries its own group, so that a generated pattern
    # is a whole question rather than a reference to something that may not
    # be there.
    "(a)(?(1)b|c)", "(a)?(?(1)b|c)", "(?<n>a)?(?(<n>)b|c)", "(a)(?(?=a)b|c)",
    "(a)(?(1)b)",
    # Branch reset, which gives two groups one number.
    "(?|(a)|(b))", "(?|(a)|(b))\\1",
    # Duplicate names, which perl allows with no modifier - `(?J)` is
    # PCRE2's spelling of a thing perl does not need and does not accept, so
    # it belongs in the pcre vocabulary that does not exist rather than here.
    "(?<n>a)|(?<n>b)", "(?<n>a)|(?<n>b)\\k<n>",
    # `\K`, which moves where the match is reported to start.
    "a\\Kb", "(a)\\Kb",
    # The control verbs. `(*FAIL)` is `(?!)` written short; the rest steer
    # the backtracker, and what they do is visible only in where a match
    # ends up - exactly the kind of thing a compiles-only test cannot see.
    "(*FAIL)", "a(*FAIL)|a", "(*ACCEPT)", "a(*ACCEPT)b",
    "a(*PRUNE)b", "a(*SKIP)b", "a(*COMMIT)b", "(*MARK:x)a", "a+(*PRUNE)b",
    # Recursion and subroutine calls, each with something to call. A bare
    # `(?R)` is deliberately absent: it recurses with nothing to stop it and
    # the question it asks is about a limit rather than about a grammar.
    "(a)(?1)", "(?<n>a)(?&n)", "(a|b(?1))", "(?(DEFINE)(?<n>a))(?&n)",
]

# Atoms that are not well formed on their own, so that some generated pattern
# is one perl *refuses*. Without these the accept-or-reject half of the
# comparison is never exercised and the tool reports zero having never asked -
# which is exactly what posix_diff.py did until it was given the same
# treatment.
#
# Deliberately no lone backslash: concatenated with the atom after it, `\` and
# `b` would spell `\b`, and the generator would be asking a question about a
# boundary while believing it had asked about a trailing escape.
ILL_FORMED = [
    "(", ")", "[", "a{1", "[a-", "(?", "(?<", "\\k<nope>", "(?<1a>b)",
    "*", "+", "?", "a**", "(?<n>a)(?<n>b)", "\\g{99}", "(?P<n>a)(?P<n>b)",
]

SUBJECTS = ["", "a", "b", "ab", "aab", "abc", "aaa", "a.b", "A", "AB", "aA",
            "\n", "a\nb", "abab", "ababaaa", "aaaa", "a b", "é", "ab\n"]

# `x` is left out on purpose: grx_match maps flag letters to GRX_Option bits
# and has no GRX_OPT_EXTENDED among them, so a row with `x` would be asking
# perl one question and this library another.
FLAG_SETS = ["", "i", "m", "s", "im", "ims"]


def find(name):
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(ROOT, "build", platform, build, "apps",
                "tools", name)
            if os.path.exists(path):
                return path
    return None


def ask(command, cases):
    lines = []
    for flags, pattern, subject in cases:
        lines.append("%s\t%s\t%s" % (flags,
            binascii.hexlify(pattern.encode()).decode(),
            binascii.hexlify(subject.encode()).decode()))
    # A timeout, because the vocabulary now contains recursion and control
    # verbs: a generated pattern that makes perl or this library run for ever
    # should stop the tool and say so, not look like a hang in the build.
    try:
        finished = subprocess.run(command, input="\n".join(lines) + "\n",
            capture_output=True, text=True, timeout=600)
    except subprocess.TimeoutExpired:
        sys.stderr.write("%s did not finish within 600s\n" % command[0])
        return []
    return finished.stdout.splitlines()


def normalise_ours(line):
    """A grx_match line in the shape perl_match.pl answers in.

    Two differences. grx_match names the engine it ran on and perl has no
    such field, and grx_match names the diagnostic when it rejects a pattern
    where perl says only that the pattern would not compile.

    The second is what lets a rejection be compared at all. Leaving it out is
    what kept posix_diff.py from ever comparing one: `compile 42` can never
    equal `compile`, so no rejection could ever be confirmed *agreed on*, and
    the tool reported zero disagreements on a question it had not asked.
    """
    if line.startswith("match "):
        return "match " + " ".join(line.split()[2:])
    if line.startswith("compile"):
        return "compile"
    return line


def trim_unset(line):
    """Drop trailing `-` fields from a match line.

    perl reports nothing at all for a group that did not participate and is
    after every group that did: `(a)(b)|c` matching "c" gives `match 0:1`,
    not `match 0:1 - -`. This library names every group the pattern has.

    Both spellings say the same thing - the group is unset - so the shorter
    one is the common ground and both sides are trimmed to it. Only the tail
    goes: an unset group *between* two set ones is reported by both, and a
    difference there is a real difference about which branch ran.
    """
    if not line.startswith("match "):
        return line
    fields = line.split()
    while len(fields) > 2 and fields[-1] == "-":
        fields.pop()
    return " ".join(fields)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--patterns", type=int, default=400)
    parser.add_argument("--examples", type=int, default=12)
    args = parser.parse_args(argv[1:])

    perl = os.path.join(ROOT, "tools", "corpus", "perl_match.pl")
    ours = find("grx_match")
    if not ours:
        sys.stderr.write("run `make tools` first\n")
        return 2
    if not os.path.exists(perl):
        sys.stderr.write("tools/corpus/perl_match.pl is missing\n")
        return 2

    rng = random.Random(args.seed)
    atoms = ATOMS + ILL_FORMED
    built = set()
    for _ in range(args.patterns):
        built.add("".join(rng.choice(atoms) for _ in range(rng.randint(1, 3))))

    cases = [(flags, pattern, subject)
             for pattern in sorted(built)
             for flags in FLAG_SETS
             for subject in SUBJECTS]

    theirs = ask(["perl", perl], cases)
    mine = ask([ours, "perl"], cases)
    if len(theirs) != len(cases) or len(mine) != len(cases):
        sys.stderr.write(
            "a driver answered %d and %d of %d requests\n"
            % (len(theirs), len(mine), len(cases)))
        return 2

    disagreements = []
    compared = 0
    unsupported = 0

    for (flags, pattern, subject), them, us in zip(cases, theirs, mine):
        # A construct this library does not implement is a gap, not a
        # disagreement about what the construct means. It is counted and
        # printed rather than dropped, because a rising count is the tool
        # saying the generator has found new ground.
        if us.startswith("unsupported"):
            unsupported += 1
            continue
        compared += 1
        if trim_unset(normalise_ours(us)) != trim_unset(them):
            disagreements.append((flags, pattern, subject, them, us))

    for flags, pattern, subject, them, us in disagreements[:args.examples]:
        print("  /%s/%-4s on %-12s perl=%-22s ours=%s"
              % (pattern, flags, repr(subject), them, us))

    print("perl: %d patterns x %d flag sets x %d subjects = %d cases, "
          "%d compared, %d this library does not implement, %d disagreements"
          % (len(built), len(FLAG_SETS), len(SUBJECTS), len(cases), compared,
             unsupported, len(disagreements)))
    return 1 if disagreements else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))


# Why there is no pcre2 half
# --------------------------
#
# `GRX_SYNTAX_PCRE`'s reference is pcre2, and `pcre2test` is installed here,
# but it cannot be driven in the shape every other oracle in this directory
# uses. It reports the matched *text* rather than byte offsets, and it omits
# a trailing group that did not participate rather than naming it - so "which
# span did group 2 get" is not a question it answers, and that is most of
# what a match comparison is for.
#
# The alternative is a small C driver linking libpcre2-8, as posix_match.c
# links glibc's regex. This machine has the shared library and not the
# header, so that is a fetch-and-build away rather than a file away, and it
# is written down here rather than left as an absence somebody has to notice.
#
# What this does *not* mean is that the PCRE2 front end is unchecked: it has
# pcre2test's own `testinput1` and `testinput2` imported as vectors, which is
# where its published rate comes from. It means the generator - the part that
# asks questions nobody thought to write down - reaches perl and not pcre2.
