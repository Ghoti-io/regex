#!/usr/bin/env python3
"""Compare the Perl-family front ends against their references, on patterns
nobody wrote.

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

**Each dialect has one definition**, the way glibc is the definition for
`gnu-bre` and `gnu-ere`. `GRX_SYNTAX_PERL` means "what perl does" and
`GRX_SYNTAX_PCRE` means "what pcre2 does", so one oracle decides each and
there is no agreement to take. The two vocabularies are separate for the
same reason: `(?J)` is PCRE2's and perl refuses it, `(*scs:` and `(*pla:`
are PCRE2's, and asking one reference about the other's spelling measures
nothing.

Usage:
    tools/oracle/perl_diff.py [--seed N] [--patterns N] [--examples N]
                              [--dialect perl|pcre|all]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import binascii
import os
import random
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# One entry per construct the dialect has, so that a combination exercises the
# interactions rather than one rule at a time. Ordered the way dialects.md
# describes them, and deliberately including several whose branches are
# different lengths - the shape that found WP-24.
SHARED_ATOMS = [
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
    # Lookaround, both directions and both senses - and one *inside*
    # another, which nothing here spelled until a defect in that shape was
    # found by the Vim work. A lookbehind's body is matched backwards and a
    # lookaround written in it still looks forwards; this library inherited
    # the direction, so `(?<=a(?=b))b` against "ab" did not match.
    "(?=a)", "(?!a)", "(?<=a)", "(?<!a)",
    "(?<=a(?=b))", "(?<=(?=a)a)", "(?<=a(?!c))", "(?<!a(?=b))",
    "(?=(?<=a)b)", "(?<=(?<=a)b)",
    # Atomic grouping and inline modifiers.
    "(?>a)", "(?i)a", "(?i:a)", "(?-i:a)", "(?^i:a)",
    # The extended class. It was PCRE2-only here on the belief that perl's
    # grammar differs; over 13,440 generated rows the operands, operators
    # and precedence all agree, and what differs is which characters are
    # ignorable - which is the next list.
    "(?[ [a-z] & [b-d] ])", "(?[ [ab] | [cd] ])", "(?[ ! [a] ])",
    "(?[ ( [a] | [b] ) - [b] ])", "(?[ \\w - [a] ])", "(?[ [a] ^ [ab] ])",
    # Script runs. Both dialects have them, and what this vocabulary adds
    # over tools/oracle/script_run_diff.py is the *interaction*: a script
    # run beside a quantifier, inside a group, next to an anchor. The
    # subjects here are Latin and Common, so the rule itself is never the
    # question - whether backtracking into one behaves is.
    "(*sr:a+)", "(*asr:a+)", "(*sr:\\w+)", "(*sr:a)*",
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
    # The two recursion conditions, which need a recursion to be true in -
    # a bare `(?(R)a|b)` can only ever take its false branch, and a
    # vocabulary holding only that would generate the condition without
    # ever asking it. `GRX_COND_RECURSION_ANY` and its numbered form were
    # reached by no differential at all until these four.
    "(?(R)a|b)", "(a(?(R)b|c))(?1)", "(?(R1)a|b)", "(a(?(R1)b|c))(?1)",
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
SHARED_ILL_FORMED = [
    "(", ")", "[", "a{1", "[a-", "(?", "(?<", "\\k<nope>", "(?<1a>b)",
    "*", "+", "?", "a**", "(?<n>a)(?<n>b)", "\\g{99}", "(?P<n>a)(?P<n>b)",
]

# What only perl has, or only perl spells this way.
PERL_ONLY = [
    # Duplicate names, ordinary in perl and needing `(?J)` in PCRE2.
    "(?<n>a)|(?<n>b)", "(?<n>a)|(?<n>b)\\k<n>",
    # What an extended class ignores, which is the one place the two
    # dialects' `(?[...])` differ: perl skips all of Pattern_White_Space
    # and takes `#` comments to the next line feed, and pcre2test refuses
    # a literal newline there with error 216 and refuses `#` outright.
    "(?[ [a]\n])", "(?[ [a] # c\n | [b] ])", "(?[\n[a]\n])",
    "(?[ [a]\x0b])", "(?[ [a]\x0c])", "(?[ [a]\r])",
    "(?[ [a] # c\r ])", "(?[ [a] # no line feed ])",
    # The charset modifiers, which pick which alphabet `\\w` and friends
    # mean. PCRE2 has no such letter.
    "(?a:\\w)", "(?u:\\w)", "(?aa:\\w)", "(?d:\\w)",
    # The four segmentation boundaries, which are perl's alone: pcre2test
    # reads `\\b{wb}` as a word boundary and then a literal brace. They
    # compile here and `perl_syntax_diff.py` says so, and until now nothing
    # asked where they *hold* over a subject - the tests state the rule and
    # a test is not a differential.
    "\\b{wb}", "\\B{wb}", "\\b{gcb}", "\\B{gcb}", "\\b{sb}",
    "\\B{sb}", "\\b{lb}", "\\B{lb}",
]

# What only PCRE2 has.
PCRE_ONLY = [
    # The duplicate-name switch perl does not need.
    "(?J)(?<n>a)(?<n>b)", "(?J)(?<n>a)|(?<n>b)",
    # The alphabetic spellings of the lookarounds, which are also the only
    # `(*...)` constructs a conditional accepts.
    "(*pla:a)", "(*nla:a)", "(*plb:a)", "(*nlb:a)",
    "(*positive_lookahead:a)", "(a)(?(*pla:a)b|c)",
    # Non-atomic lookaround, which can be re-entered where an ordinary one
    # cannot - a difference visible only in what a backreference then sees.
    "(*napla:a|(.))\\1", "(*naplb:(.)|x)\\1", "(?*a|(.))\\1",
    # Scan-substring, which re-runs an assertion over what a group captured.
    "(a)(*scs:(1)a)", "(?<n>a)(*scs:(<n>)a)",
    # PCRE2's own `\\g` spelling, and the callouts.
    "(a)\\g{1}", "(?C)a", "(?C1)a",
]


SUBJECTS = ["", "a", "b", "ab", "aab", "abc", "aaa", "a.b", "A", "AB", "aA",
            "\n", "a\nb", "abab", "ababaaa", "aaaa", "a b", "é", "ab\n"]

# `x` is left out on purpose: grx_match maps flag letters to GRX_Option bits
# and has no GRX_OPT_EXTENDED among them, so a row with `x` would be asking
# perl one question and this library another.
#
# `u` and `P` are UTF and UCP, and they are *separate* letters because
# pcre2 separates them - `(*UTF)\w` does not match "é" and
# `(*UTF)(*UCP)\w` does. They were one letter in pcre2_match, which made
# the only flag combination that shows the difference unaskable; this
# library widened its shorthands on UTF and no differential could see it.
# For the perl row both are no-ops: its subject is a Unicode string and its
# shorthands are Unicode either way, which is itself worth asserting.
FLAG_SETS = ["", "i", "m", "s", "im", "ims", "u", "uP", "iu", "iuP"]

ATOMS = {
    "perl": SHARED_ATOMS + PERL_ONLY,
    "pcre": SHARED_ATOMS + PCRE_ONLY,
}

ILL_FORMED = {
    "perl": SHARED_ILL_FORMED,
    # A duplicate name without `(?J)` is PCRE2's error 143 and perl's
    # ordinary Tuesday, so it is ill formed in one vocabulary only.
    "pcre": SHARED_ILL_FORMED
        + ["(?<n>a)(?<n>b)", "(?[ [a] & ])", "(*scs:(9)a)"],
}

# Which reference decides each dialect. perl is a script this repository
# owns; pcre2 is a C driver linked against the pcre2 on the machine, because
# pcre2test reports matched text rather than offsets and omits a trailing
# group that did not participate.
REFERENCE = {"perl": "perl", "pcre": "pcre2"}


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


BOUNDARY_FIRST = re.compile(r"\\[bB]\{\s*(?:wb|gcb|g|sb|lb)\s*\}")


def boundary_end_of_subject(pattern, them, subject, ours):
    """Whether the only difference is the empty match at the end.

    Two shapes, one defect, because two gates ask the same question in two
    ways: `perl_diff.py` asks for one match and `iterate_diff.py` asks for
    every match. In the first, perl finds nothing where this library finds
    the empty span at the end; in the second, perl's list is this library's
    without its last entry, and that entry is the empty span at the end.
    """
    if subject is None or ours is None or not BOUNDARY_FIRST.match(pattern):
        return False
    end = "%d:%d" % (len(subject.encode()), len(subject.encode()))
    if them == "nomatch" and ours.startswith("match "):
        # The whole match's span, and the groups after it are whatever an
        # empty match at the end gives them - a pattern holding a group is
        # still a pattern that found the boundary perl's search missed, and
        # comparing the whole field is what made `\b{lb}(a|)` look like a
        # different defect.
        return ours.split()[1] == end
    if ours.startswith("all ") and them.startswith("all "):
        mine = ours.split()
        theirs = them.split()
        # A field is `whole` or `whole,group,group`, so the last one is
        # compared by its whole-match span alone - the groups of an empty
        # match at the end are that same empty span, and writing them out
        # here would be a second spelling of the same thing.
        return (len(mine) == len(theirs) + 1
            and mine[-1].split(",")[0] == end
            and mine[2:-1] == theirs[2:])
    return False


CALL_SPELLING = re.compile(r"\(\?(?:R|[0-9]|&|P>|\+|-)")
BEHIND_NON_ATOMIC = ("(*naplb:", "(*non_atomic_positive_lookbehind:")


def library_deviation(pattern, them, us):
    """A pattern this library refuses on purpose, recorded in section 6.

    One shape today: a subroutine call to a group defined inside a
    *non-atomic* lookbehind. The block a call enters is generated once and
    steps the way its definition does, so a group written inside a
    backwards-running body has a backwards block and a call from outside it
    would walk the subject the wrong way - `(*naplb:(a))(?1)` against "aa" is
    1-2 in pcre2test and was 1-1 here, with group one holding a span outside
    the match. Refused rather than answered; see src/compile/codegen.c.

    Both halves are required, so a pattern holding a non-atomic lookbehind
    and no call, or a call and no such lookbehind, is compared as usual.
    """
    if not us.startswith("compile") or them.startswith("compile"):
        return False
    return (any(spelling in pattern for spelling in BEHIND_NON_ATOMIC)
        and CALL_SPELLING.search(pattern) is not None)


def reference_defect(dialect, pattern, them, subject=None, ours=None):
    r"""Rows where the *reference* is known to be wrong.

    Counted and reported rather than silently dropped, and written as
    narrowly as the defect allows, because an exclusion is the one thing in a
    differential that can hide what it exists to find. Both of these were
    found by this tool and confirmed by hand against the other reference.

    **perl, a branch reset whose group is later read.** Two shapes, one
    defect. `(?|(a)|(b))(a)\g{-1}` does not match "aaaa" in perl 5.40.1 where
    pcre2 matches it, and `(?|(a)|(b))(?(1)x|y)` takes the false arm on "by"
    although perl itself reports group 1 as "b" - pcre2 takes "bx", and so
    does this library. That is Perl/perl5#24577, a regression introduced in
    5.38 and closed 2026-07-22, already described in tools/corpus/VERSIONS,
    which pins 5.40.1 because that is what this machine ships. Two rows of
    known-gaps.txt are the named form of the same question.

    The rule asks for a branch reset *and* a construct that reads a group,
    because that is the boundary: `(?|(a)|(b))\1` and
    `(?|(a)|(b))(a)\g{-2}` both agree with pcre2, and `(?:(a)|(b))(?(1)x|y)`
    without the branch reset agrees too. It excludes nothing from the pcre
    run, so a real defect of this library in this family would still be
    caught there, against the reference that has it right.

    When the pin moves past the fix this exclusion should start catching
    nothing, and the count printed each run is how that will be noticed.

    **pcre2, a lookbehind with an extended class.** Any pattern holding both
    a lookbehind and a `(?[...])` whose body uses an operator - `|`, `&`,
    `-`, `^` or `!` - fails to compile with "error 170: internal error:
    unknown meta code in check_lookbehinds()". A lookahead does not do it and
    an extended class with no operator does not do it; the order of the two
    does not matter. pcre2test 10.46 reports the same internal error, so it
    is the library and not this driver. This library compiles those patterns
    and matches them.

    The pcre2 rule is gated on the reference having *refused* the pattern, so
    a row pcre2 actually compiled can never be excluded by it.

    **perl misses the boundary at the end of a one-character subject** when
    the pattern begins with one of the four segmentation assertions. The
    boundary is there and perl's own answers say so from every other
    direction: `a\b{lb}` against "a" is 0-1, `\b{lb}$` against "a" is 1-1,
    `\b{gcb}` against "ab" is 0-0, 1-1 and 2-2 under `/g`, and
    `x|\b{gcb}` against "a" is 0-0 and 1-1 - the same assertion, in an
    alternation that defeats whatever optimisation this is. Alone it gives
    0-0 and stops, and `\b{lb}`, whose LB2 forbids a break at the start,
    gives nothing at all.

    UAX #29's GB2 and SB2 and UAX #14's LB3 all break at the end of text,
    so the boundary exists in every one of those. The rule is narrow in
    both dimensions rather than one: the pattern has to *begin* with such
    an assertion, **and** the difference has to be exactly the empty match
    at the end of the subject - a row where the two differ anywhere else is
    still a disagreement.
    """
    if dialect == "perl":
        if boundary_end_of_subject(pattern, them, subject, ours):
            return True
        return "(?|" in pattern and ("\\g{-" in pattern or "(?(" in pattern)
    return (them.startswith("compile") and "(?[" in pattern
        and ("(?<=" in pattern or "(?<!" in pattern or "(*nlb:" in pattern
            or "(*plb:" in pattern or "(*naplb:" in pattern))


def reference_command(dialect):
    """How to run the reference for a dialect, or None with a reason said."""
    if dialect == "perl":
        script = os.path.join(ROOT, "tools", "corpus", "perl_match.pl")
        if not os.path.exists(script):
            print("%s: skipped (tools/corpus/perl_match.pl is missing)"
                  % dialect)
            return None
        return ["perl", script]
    driver = find("pcre2_match")
    if not driver:
        # Not an error. The driver needs pcre2's header, which arrives with
        # the corpus, and a libpcre2-8 to link; a clone that has neither
        # builds everything else and says this was not run.
        print("%s: skipped (no pcre2_match; run tools/corpus/fetch.sh pcre2 "
              "and `make tools`)" % dialect)
        return None
    return [driver]


def compare(dialect, ours, seed, patterns, examples):
    """One dialect against its reference. None means the run was not made."""
    command = reference_command(dialect)
    if not command:
        return 0

    rng = random.Random(seed)
    atoms = ATOMS[dialect] + ILL_FORMED[dialect]
    built = set()
    for _ in range(patterns):
        built.add("".join(rng.choice(atoms) for _ in range(rng.randint(1, 3))))

    cases = [(flags, pattern, subject)
             for pattern in sorted(built)
             for flags in FLAG_SETS
             for subject in SUBJECTS]

    theirs = ask(command, cases)
    mine = ask([ours, dialect], cases)
    if len(theirs) != len(cases) or len(mine) != len(cases):
        sys.stderr.write(
            "%s: the drivers answered %d and %d of %d requests\n"
            % (dialect, len(theirs), len(mine), len(cases)))
        return None

    disagreements = []
    compared = 0
    unsupported = 0
    declined = 0
    known = 0

    for (flags, pattern, subject), them, us in zip(cases, theirs, mine):
        # The reference declining to answer - a match limit, a subject it
        # will not read - is not an opinion this library can be held to.
        if them.startswith("skip"):
            declined += 1
            continue
        # A construct this library does not implement is a gap, not a
        # disagreement about what the construct means. It is counted and
        # printed rather than dropped, because a rising count is the tool
        # saying the generator has found new ground.
        if us.startswith("unsupported"):
            unsupported += 1
            continue
        if library_deviation(pattern, them, us):
            unsupported += 1
            continue
        compared += 1
        if trim_unset(normalise_ours(us)) == trim_unset(them):
            continue
        if reference_defect(dialect, pattern, them, subject,
                trim_unset(normalise_ours(us))):
            known += 1
            continue
        disagreements.append((flags, pattern, subject, them, us))

    for flags, pattern, subject, them, us in disagreements[:examples]:
        print("  /%s/%-4s on %-12s %s=%-22s ours=%s"
              % (pattern, flags, repr(subject), REFERENCE[dialect], them, us))

    print("%-5s %d patterns x %d flag sets x %d subjects = %d cases against "
          "%s, %d compared, %d this library does not implement, %d the "
          "reference declined, %d a known %s defect, %d disagreements"
          % (dialect + ":", len(built), len(FLAG_SETS), len(SUBJECTS),
             len(cases), REFERENCE[dialect], compared, unsupported, declined,
             known, REFERENCE[dialect], len(disagreements)))
    return len(disagreements)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--patterns", type=int, default=400)
    parser.add_argument("--examples", type=int, default=12)
    parser.add_argument("--dialect", default="all",
        help="perl, pcre, or all")
    args = parser.parse_args(argv[1:])

    ours = find("grx_match")
    if not ours:
        sys.stderr.write("run `make tools` first\n")
        return 2

    dialects = ("perl", "pcre") if args.dialect == "all" else (args.dialect,)
    total = 0
    for dialect in dialects:
        if dialect not in ATOMS:
            sys.stderr.write("unknown dialect: %s\n" % dialect)
            return 2
        # A seed per dialect, so that adding one does not renumber the other.
        found = compare(dialect, ours, args.seed, args.patterns, args.examples)
        if found is None:
            return 2
        total += found
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))


# Why pcre2 is linked rather than driven through pcre2test
# --------------------------------------------------------
#
# `pcre2test` is installed here and cannot answer the question a match
# comparison asks. It reports the matched *text* rather than byte offsets,
# and it omits a trailing group that did not participate rather than naming
# it - so "which span did group two get" is not something it says.
#
# `tools/oracle/pcre2_match.c` asks `pcre2_match()` and reads the ovector
# instead, which is the same question every other oracle here is asked. It
# links the pcre2 already on the machine through the public header of the
# release pinned in `tools/corpus/VERSIONS`, fetched alongside the corpus
# because Debian ships `libpcre2-8.so.0` without the `-dev` package's
# `pcre2.h`. The pin, the header, the installed library and the imported
# `testinput` files are therefore one version.
