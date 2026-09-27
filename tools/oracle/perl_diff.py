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

import oracle_env
import pcre2_runner

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
    # A case class, which `[[:alpha:]]` cannot stand in for: a caseless
    # mode makes `[:lower:]` and `[:upper:]` name one set, and whether it
    # does so at the *Unicode* width is where perl and pcre2 part - an
    # axis, GRX_Profile::posix_case_classes_collapse_wide, so both rows of
    # this gate compare it against their own reference.
    "[[:lower:]]",
    # `graph`, `print` and `word`, which had no atom at all until
    # 2026-09-24. The three of them are where perl and pcre2 disagree with
    # each other by the widest margins in this library - 137,468 code points
    # for `graph` and 1,513 for `word` - and both rows of this gate ran
    # clean for as long as nothing here spelled them. `[[:word:]]` beside
    # `\w` on purpose: they are one set in both references, and a profile
    # axis that reached one spelling and missed the other would pass every
    # `\w` row.
    "[[:graph:]]", "[[:print:]]", "[[:word:]]", "[[:^graph:]]",
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
    # The modifiers that are not `i`, each of which is an option bit with a
    # spec row and, until these lines, no generating gate at all: the
    # driver spells flags as letters and has none of these among them, so
    # the *only* way the vocabulary can ask about them is the inline form.
    # `(?n)` found a defect the first time it was generated - a number that
    # no-capture mode leaves without a group was not refused.
    #
    # `(?l)` is deliberately absent and is the one exclusion here: it asks
    # for the locale's semantics, this library has only the C locale
    # (documentation/dialects.md section 6), and perl's answer depends on
    # the locale the shell started it in - `(?l)\\b` against "é" is 0-0
    # there under a UTF-8 locale. A row whose answer moves with the
    # environment is not a rule either side can be held to.
    "(?n)a", "(?n:(a))", "(?x)a", "(?x: a )", "(?xx)a", "(?xx:[a b])",
    "(?a)\\w", "(?aa)\\w", "(?u)\\w", "(?a:\\w)", "(?d)\\w", "(?p)a",
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
    # `\Q...\E`, which this list used to leave out on the ground that perl
    # could not be asked about it through this transport. It can: `\Q` is
    # double-quotish processing, done when the *source* is tokenised, so a
    # pattern arriving in a variable never goes through it and perl reads
    # the escape as the letter - `\Qa.b\E` there matches "QaXbE" and not
    # "a.b", which is not "no answer" but a different one. The note had
    # tried two subjects and neither was the one that says so.
    #
    # pcre2test reads its pattern as source, quotes it, and matches "a.b"
    # alone; this library does the same for both dialects. So these rows
    # agree on the pcre run and are the deviation below on the perl one,
    # where `quoting_reads_as_letters()` checks each against what perl's
    # own reading would be rather than excluding it by its spelling.
    "\\Qa.b\\E", "\\Q*\\E", "\\Qa\\E", "a\\Q\\Eb", "[\\Qa-z\\E]",
    "\\Qa", "a\\E",
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
    # A call into a group defined inside a lookbehind, refused until
    # 2026-09-24 and now a second copy of the block laid out the other way
    # round. Both halves are in one atom on purpose: the generator composes
    # one to three atoms, and over 400 draws it had produced a non-atomic
    # lookbehind in four patterns and a call in forty-four and the two
    # together in *none* - so the run had been reporting "0 this library
    # does not implement" about a shape it never built.
    "(*naplb:(a))(?1)", "(*naplb:((a)b))(?1)", "(*naplb:(?<x>a))(?&x)",
    "(*naplb:(a|bc))(?1)", "(?<=(a))(?1)", "(?<=(a))x(?1)",
    # The two mixtures that always agreed, kept beside them so that a
    # regression in either is a row rather than a memory: a call from inside
    # a lookbehind to a group outside it, and a lookaround written inside
    # the called group, whose own direction must survive the copy.
    "(a)(*naplb:(?1))", "(*naplb:(a(?=b)))(?1)", "(*naplb:(a(?<=xa)))(?1)",
    # Scan-substring, which re-runs an assertion over what a group captured.
    "(a)(*scs:(1)a)", "(?<n>a)(*scs:(<n>)a)",
    # PCRE2's own `\\g` spelling, and the callouts.
    "(a)\\g{1}", "(?C)a", "(?C1)a",
    # `\\C`, one *code unit* - and the one construct in this dialect that
    # pcre2test accepts and this library refuses. It was not in any generator
    # and not in dialects.md either until 2026-09-26, so the gap was invisible
    # rather than counted: `make check-oracle-perl --dialect pcre` reported "0
    # this library does not implement" while a construct sat outside every
    # list. Here so that the number is a number.
    #
    # Three atoms rather than one. Under `u` over "é" pcre2 matches 0:1, which
    # is *half a character*, and that is the whole reason this is refused
    # rather than built: this library's spans are byte offsets into UTF-8 and
    # a match may not end inside a character. The middle atom is the same
    # construct where it is harmless, and the third is the quantified form,
    # which is what a caller writes when they mean "any bytes".
    "\\C", "a\\Cb", "\\C+",
    # The `a` charset modifiers, WP-46. One letter narrows one thing, and
    # each of these is here with the subject that separates it from the
    # other four: U+0661 for `D`, U+00A0 for `S`, "é" for `W`, and the two
    # POSIX letters against the names that tell `P` from `T`. The clear
    # forms are here too, because the hyphen goes in front of the `a` and
    # `(?-aP)` reaches `T` while `(?-aT)` does not reach `P`.
    "(?aD)\\d", "(?aS)\\s", "(?aW)\\w", "(?aP)[[:alpha:]]",
    "(?aT)[[:digit:]]", "(?aT)[[:xdigit:]]", "(?aP)[[:digit:]]",
    "(?aT)[[:alpha:]]", "(?aD)[[:digit:]]", "(?aW)[[:word:]]",
    "(?a)(?-aD)\\d", "(?a)(?-aW)\\w", "(?a)(?-a)\\d",
    "(?aP)(?-aT)[[:digit:]]", "(?aT)(?-aP)[[:digit:]]",
    "(?aD:\\d)", "(?aD:x)\\d", "(?:(?aD))\\d", "(?aW)\\bx", "(?aDi)\\d",
]


# U+017F and U+212A are here for one question the other eighteen cannot
# ask: whether a caseless mode widens a *shorthand* the way it widens a
# literal. Both fold to an ASCII letter, so a `\w` closed under folding
# reaches them and an unclosed one does not, and "x" U+212A puts the same
# question to `\b` from the other side. Neither this gate nor the
# conformance corpus held such a character, and the library widened the
# shorthands in every dialect - ECMA-262's rule applied to perl, pcre2 and
# CPython alike - with both gates green throughout.
#
# U+0661, U+00A0 and U+FF10 arrived with WP-46, for the same reason and one
# step further out: the `a` modifiers narrow `\d`, `\s` and `[[:xdigit:]]`
# one at a time, and the other twenty-one subjects hold no character that
# any of those three sets takes and its ASCII form does not. A modifier with
# no subject to separate it is a pattern that compiles and proves nothing.
SUBJECTS = ["", "a", "b", "ab", "aab", "abc", "aaa", "a.b", "A", "AB", "aA",
            "\n", "a\nb", "abab", "ababaaa", "aaaa", "a b", "é", "ab\n",
            "\u017f", "x\u212a", "\u0661", "\u00a0", "\uff10",
            # The code points where perl and pcre2 disagree with each other
            # about `\w` and `[[:graph:]]`, which is what the profile's
            # word_set and the two graph fields exist to answer. Every one
            # of them was added on 2026-09-24 *after* a hand sweep found the
            # defects, because this run reported 0 disagreements over 256,320
            # cases while three of the four classes were wrong: none of the
            # 24 subjects above was a letter-number, a non-spacing mark, a
            # connector, a private-use code point or a bidi isolate.
            #
            # Paired with an "a" so that `\b` is asked as well as `\w` -
            # a boundary needs two sides, and a one-character subject can
            # only ever be the start of the string.
            "\u00b2", "a\u00b2",   # No: pcre2's `\w`, not perl's
            "\u0903", "a\u0903",   # Mc: perl's `\w`, not pcre2's
            "\u203f", "a\u203f",   # Pc: both, and not `re`'s
            "\u24b6", "a\u24b6",   # So and Alphabetic: perl's, not pcre2's
            "\u2160",               # Nl: both, and the 2026-09-24 `L` fix
            "\ue000",               # Co: perl's `[[:graph:]]`, not pcre2's
            "\u2066", "\u180e"]    # Cf: dropped from pcre2's graph by name

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


BOUNDARY_FIRST = re.compile(
    r"(?:\(\?:\)|\(\?#[^)]*\))*\\[bB]\{\s*(?:wb|gcb|g|sb|lb)\s*\}")


def boundary_end_of_subject(pattern, them, subject, ours):
    """Whether the only difference is the empty match at the end.

    Two shapes, one defect, because two gates ask the same question in two
    ways: `perl_diff.py` asks for one match and `iterate_diff.py` asks for
    every match. In the first, perl finds nothing where this library finds
    the empty span at the end; in the second, perl's list is this library's
    without its last entry, and that entry is the empty span at the end.

    What may stand in front of the assertion is measured rather than
    assumed, and it is exactly what the reader passes over without building
    a node: an empty *non-capturing* group and a comment. `(?:)\\b{lb}`,
    `(?#c)\\b{lb}` and `(?#c)(?:)\\b{lb}` against "a" are all no match in
    perl, where `\\b{lb}` alone is - the same defect. It stops there:
    `()\\b{lb}` is 1-1 in perl, `x*\\b{lb}` is 1-1, and `(?#c)()\\b{lb}`
    and `(?#c)x*\\b{lb}` are 1-1 too, so a capturing group or a quantifier
    in that position defeats whatever optimisation this is and those rows
    are compared as usual.
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


# The double-quotish operators: the ones perl applies when it tokenises its
# *source* and this library applies when it reads the pattern text. `\Q` and
# `\E` are quoting; `\L`, `\U`, `\F`, `\l` and `\u` are the case transforms
# (documentation/dialects.md section 9, GRX_FEATURE_CASE_TRANSFORM).
#
# All seven are here because all seven have the same shape of deviation, and
# writing only the two that the generator's alphabet happens to reach would
# leave a false disagreement waiting for whoever adds the others to it. This
# driver asks perl for the `quoted` reading - see documentation/testing.md
# section 5 for why - and under that reading perl passes each of them through
# as the letter.
SOURCE_OPERATORS = "QELUFlu"


def as_letters(pattern):
    r"""The pattern perl reads, where the source operators are unknown escapes.

    perl passes an unrecognized alphabetic escape through as the letter -
    with a warning - so `[\Qa-z\E]` is `[Qa-zE]` there, which is why it
    matches "Q" and "b" and not "-". A backslash pair is stepped over
    rather than read, so `\\Q` keeps its literal backslash.
    """
    out = []
    index = 0
    while index < len(pattern):
        character = pattern[index]
        if character == "\\" and index + 1 < len(pattern):
            following = pattern[index + 1]
            out.append(following if following in SOURCE_OPERATORS
                else character + following)
            index += 2
            continue
        out.append(character)
        index += 1
    return "".join(out)


def holds_quoting(pattern):
    r"""Whether the pattern writes one of the source operators as an escape."""
    return as_letters(pattern) != pattern


def construct_not_implemented():
    r"""The number `GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED` has in this build.

    Read out of the header rather than typed, the way tools/check_diagnostics.py
    reads the same enum: the value is a position in a list and inserting an
    enumerator above it would move it silently.
    """
    path = os.path.join(ROOT, "include", "ghoti.io", "regex", "core.h")
    with open(path, encoding="utf-8") as handle:
        names = re.findall(r"^\s*(GRX_DIAG_[A-Z0-9_]+)", handle.read(), re.M)
    ordered = []
    for name in names:
        if name != "GRX_DIAG_COUNT" and name not in ordered:
            ordered.append(name)
    try:
        return ordered.index("GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED")
    except ValueError:
        return None


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
        return oracle_env.command("perl", ["perl", script])
    # No `find("pcre2_match")` and no skip. The driver is compiled inside the
    # pinned image against that image's pcre2 and run there, so there is
    # nothing for `make tools` to have failed to build and nothing for a fresh
    # clone to be missing - and the reference is resolved and versioned by
    # oracle_run.py before this runs.
    return pcre2_runner.command()


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
    not_implemented = construct_not_implemented()
    declined = 0
    known = 0
    quoting = []
    quoted = 0

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
        #
        # **Two spellings, and for a year this read only the first.**
        # `unsupported` is the driver saying no engine can run the *program*;
        # a construct the parser has not built is `compile <diag>` with
        # GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, which is the diagnostic whose
        # name is this bucket's name. Nothing had shown it up because no atom
        # in either list reached that diagnostic - and the first one that did,
        # `\C` on 2026-09-26, arrived as 5,760 disagreements rather than as a
        # count. A bucket that cannot see the answer it is named for is worse
        # than no bucket: it is a green line over an unasked question.
        if us.startswith("unsupported") or (not_implemented is not None
                and us == "compile %d" % not_implemented):
            unsupported += 1
            continue
        compared += 1
        if trim_unset(normalise_ours(us)) == trim_unset(them):
            continue
        if reference_defect(dialect, pattern, them, subject,
                trim_unset(normalise_ours(us))):
            known += 1
            continue
        if dialect == "perl" and holds_quoting(pattern):
            # Asked again below rather than excluded here: the row is the
            # quoting deviation only if perl's answer is this library's
            # answer to the pattern perl actually read.
            quoting.append((flags, pattern, subject, them, us))
            continue
        disagreements.append((flags, pattern, subject, them, us))

    # The second pass, and what makes the quoting exclusion checkable.
    # This library implements `\Q...\E` as PCRE2 does, because a pattern
    # is text here and there is no interpolation to have done it earlier;
    # perl reads the escape as the letter. A row is that deviation only
    # when this library's answer to `as_letters(pattern)` *is* perl's
    # answer to the pattern as written - anything else is a disagreement
    # and is reported as one, so a defect inside a quoted run is still
    # this tool's to find.
    if quoting:
        rows = [(flags, as_letters(pattern), subject)
                for flags, pattern, subject, _, _ in quoting]
        again = ask([ours, dialect], rows)
        if len(again) != len(rows):
            disagreements.extend(quoting)
        else:
            for row, answer in zip(quoting, again):
                if trim_unset(normalise_ours(answer)) == trim_unset(row[3]):
                    quoted += 1
                else:
                    disagreements.append(row)

    for flags, pattern, subject, them, us in disagreements[:examples]:
        print("  /%s/%-4s on %-12s %s=%-22s ours=%s"
              % (pattern, flags, repr(subject), REFERENCE[dialect], them, us))

    print("%-5s %d patterns x %d flag sets x %d subjects = %d cases against "
          "%s, %d compared, %d this library does not implement, %d the "
          "reference declined, %d a known %s defect, %d perl's quoting, "
          "%d disagreements"
          % (dialect + ":", len(built), len(FLAG_SETS), len(SUBJECTS),
             len(cases), REFERENCE[dialect], compared, unsupported, declined,
             known, REFERENCE[dialect], quoted, len(disagreements)))
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
