#!/usr/bin/env python3
r"""Reproducible .NET semantic matrix; never consults libregex.

`java_probe.py`'s shape, and for the same reason: the answers a dialect is
built from should be a committed file that a later run can contradict, not a
measurement somebody took once and wrote into a table by hand. The .NET row
of `documentation/dialects.md` said ".NET 8" for months because nothing could
contradict it.

Use `--record` only when intentionally refreshing the pin's answers. The
normal gate compares every answer against the committed file.

**This probe runs before the dialect exists**, which is what makes it worth
writing first. The eight `dotnet_*` differentials that will follow each
compare this library against the reference, so none of them can say anything
until there is a parser; this file asks only .NET, so every answer it records
is a fact the parser will then be built against.

Three of its areas earn their place:

  * **`unit`** is WP-49's inventory. .NET matches over UTF-16 code units
    with no code-point mode, so a match boundary can fall inside a character, and this library's
    spans are byte offsets into UTF-8 where it cannot. What was known before
    this file was that `.` splits a surrogate pair; what WP-49 needs is which
    *other* constructs reach an interior position - lookaround, `\b`, the
    empty-match advance, split and the replacement template. The driver
    answers `{"kind":"offset"}` for exactly those, so the error is the datum
    rather than a failure, and an area of them is the inventory.

  * **`iteration`** settles the two cells `dialects.md` still marks
    **probe** for this dialect: `BREAK_ON_EMPTY` at section 5.5 and
    `ADVANCE_ONE` at 5.10. A `probe` cell is a value nobody measured, and
    readiness condition 1 does not accept one.

  * **`profile`** pins the `GRX_Profile` axes, the way Java's `profile` area
    does. The Java audit's finding is the instruction here: a case that
    cannot tell two values of a field apart pins nothing, so each row below
    names the field it separates and, where one row could pass for an
    unrelated reason, comes as a pair.

What it does not cover, said plainly rather than left to be discovered:
balancing groups (out of scope, `GRX_ERR_UNSUPPORTED`), the whole of the
`\p{...}` namespace (that is `ranges` mode and a differential, not a matrix),
`RegexOptions.NonBacktracking` and `Compiled` (documented not to change an
answer, and if they do that is a determinism finding), and culture-sensitive
folding beyond the `C` letter.
"""
import argparse
import json
import os
import subprocess
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dotnet_runner
import oracle_env
import wtf8 as wtf8_codec

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "tests", "data", "probe")

# One astral character: two UTF-16 code units to .NET, four UTF-8 bytes here.
# Every row of the `unit` area is about the position between its halves.
ASTRAL = "\U0001F600"


def case(area, pattern, subject="", flags="", mode="first", argument=""):
    return dict(area=area, pattern=pattern, subject=subject, flags=flags,
                mode=mode, argument=argument)


CASES = [
    # The spine. Each is an answer that would change shape if the protocol
    # broke, so a wrong one reads as a broken driver rather than as a finding.
    case("control", "a", "a"),
    case("control", "a", "b"),
    case("control", "a(", ""),
    case("control", "(a)?b", "b"),
    case("control", "a", ASTRAL + "a", mode="find-from", argument="4"),
    case("control", "a", "a", mode="compile"),
]

# WP-49's inventory: which constructs reach a position inside a character.
# An answer of {"kind":"offset"} means .NET reported a boundary this library's
# byte offsets cannot name, and that is the fact being collected.
CASES += [case("unit", pattern, subject, flags, mode, argument)
          for pattern, subject, flags, mode, argument in (
    # `.` and its relatives: one code unit, so half the character.
    (".", ASTRAL, "", "first", ""),
    ("..", ASTRAL, "", "first", ""),
    (r"[\s\S]", ASTRAL, "", "first", ""),
    (r"[^x]", ASTRAL, "", "first", ""),
    (".{2}", ASTRAL, "", "first", ""),
    ("^.$", ASTRAL, "", "first", ""),
    ("^..$", ASTRAL, "", "first", ""),
    (".", "a" + ASTRAL + "b", "", "all", ""),
    # Which general category a lone half falls in. `\p{Cs}` is the question:
    # if a half is `Cs` to .NET then the categories are over code units too,
    # and not only the quantifiers.
    (r"\p{L}", ASTRAL, "", "first", ""),
    (r"\p{Cs}", ASTRAL, "", "first", ""),
    (r"\p{So}", ASTRAL, "", "first", ""),
    # Zero-width assertions, which are the rows that make WP-49 a defect
    # rather than a concession: a boundary with no width still has a position.
    (r"\B", "0" + ASTRAL + "B", "", "all", ""),
    (r"\b", "0" + ASTRAL + "B", "", "all", ""),
    (r"(?=.)", ASTRAL, "", "all", ""),
    (r"(?<=.)", ASTRAL, "", "all", ""),
    (r"(?<=.)x", ASTRAL + "x", "", "first", ""),
    # The empty-match advance. This is the one `dialects.md` section 5.10
    # names directly - "this API advances over a valid UTF-8 scalar and cannot
    # represent a boundary between surrogate halves" - so the positions .NET
    # stops at are what that sentence is about.
    ("", ASTRAL, "", "all", ""),
    ("x*", ASTRAL, "", "all", ""),
    # Split and replace at an interior position, where the deviation stops
    # being an offset nobody reads and becomes bytes in the caller's output.
    (".", ASTRAL, "", "split", ""),
    ("", ASTRAL, "", "split", ""),
    (".", ASTRAL, "", "replace-all", "X"),
    ("", ASTRAL, "", "replace-all", "-"),
    # Right to left, because .NET alone has it and it walks the subject the
    # other way; whether the interior position is the same one is not obvious.
    (".", ASTRAL, "R", "first", ""),
)]

# The two cells dialects.md still marks **probe** for .NET: section 5.5's
# BREAK_ON_EMPTY and section 5.10's ADVANCE_ONE, plus the capture rules that
# sit beside them.
CASES += [case("iteration", pattern, subject, flags, mode, argument)
          for pattern, subject, flags, mode, argument in (
    ("", "ab", "", "all", ""),
    ("a*", "ba", "", "all", ""),
    ("a*", "", "", "all", ""),
    (r"\G.", "abc", "", "all", ""),
    ("(a*)*", "", "", "first", ""),
    ("(a*)+", "b", "", "first", ""),
    # The three cases section 5.5 names as the ones that separate its five
    # empty-iteration values, rather than three that merely exercise a loop.
    # `(a*)*` over "b" divides FAIL_IF_EMPTY_AFTER_MIN, where the group goes
    # unset, from every value where the empty iteration takes part; `(a|)*`
    # over "aaaa" divides BREAK_ON_EMPTY at 4-4, the empty body having written
    # its captures on the way out, from BREAK_IF_UNMOVED at 3-4, where once
    # the repeat has consumed the empty iteration does not run; and
    # `(a*?)+?b` over "aab" divides ALREADY_THERE at 0-2, which no rule about
    # what an empty iteration *does* can produce, from every backtracking
    # engine's 1-2.
    ("(a*)*", "b", "", "first", ""),
    ("(a|)*", "aaaa", "", "first", ""),
    ("(a*?)+?b", "aab", "", "first", ""),
    ("((a)|b)+", "ab", "", "first", ""),
    ("a", "ba", "", "find-from", "1"),
    ("a", "ba", "", "all-from", "1"),
)]

# .NET's own rules, each of which this library has to implement rather than
# approximate. The group renumbering is first because no other dialect here
# has it.
CASES += [case("captures", pattern, subject, flags, mode, argument)
          for pattern, subject, flags, mode, argument in (
    ("(?<a>x)(y)", "", "", "names", ""),
    ("(?<a>x)(y)", "xy", "", "first", ""),
    ("(a)(?<b>c)(d)", "", "", "names", ""),
    ("(?<2>a)(b)", "", "", "names", ""),
    ("(?<n>a)(?<n>b)", "", "", "names", ""),
    # The numbering, which is .NET's alone and is **sparse**. Each row below
    # is a rule the parser has to reproduce, and together they are what
    # "unnamed groups are numbered before named ones" leaves out:
    #
    #   `(?<5>a)(b)`     numbers are 0, 1 and 5 - 2 to 4 do not exist, and
    #                    Groups.Count is 3, which is what made this driver
    #                    report a group 2 and never report group 5.
    #   `(a)(?<1>b)`     an explicit number may *collide* with an unnamed
    #                    group's, and the two become one group holding the
    #                    later value: group 1 is "b".
    #   `(?<n>a)(?<n>b)` a repeated name is one group, and the pattern still
    #                    needs both halves to match in sequence.
    ("(?<5>a)(b)", "", "", "names", ""),
    ("(?<5>a)(b)", "ab", "", "first", ""),
    ("(a)(?<1>b)", "", "", "names", ""),
    ("(a)(?<1>b)", "ab", "", "first", ""),
    ("(?<n>a)(?<n>b)", "ab", "", "first", ""),
    ("(?<n>a)(?<n>b)", "b", "", "first", ""),
    ("(a)(?<b>c)(d)", "acd", "", "first", ""),
    (r"(?<n>a)\k<n>", "aa", "", "first", ""),
    ("(a)(b)", "ab", "n", "names", ""),
    ("(?<n>a)(b)", "ab", "n", "first", ""),
    ("(?!(a))b", "b", "", "first", ""),
    (r"(a)?b\1", "b", "", "first", ""),
)]

# Class syntax, where .NET's subtraction is its own spelling.
CASES += [case("classes", pattern, subject, flags) for pattern, subject, flags in (
    ("[a-z-[aeiou]]", "b", ""),
    ("[a-z-[aeiou]]", "a", ""),
    ("[a-z-[b-d-[c]]]", "c", ""),
    ("[a-z-[aeiou]]", "-", ""),
    (r"[\p{L}-[\p{Lu}]]", "a", ""),
    (r"[\p{L}-[\p{Lu}]]", "A", ""),
    (r"\p{IsGreek}", "α", ""),
    (r"\p{IsBasicLatin}", "a", ""),
    ("[[:alpha:]]", "a", ""),
    (r"\d", "١", ""),
    (r"\d", "١", "E"),
    (r"\w", "é", ""),
    (r"\w", "é", "E"),
    (r"\s", " ", ""),
    (r"\s", " ", "E"),
)]

# The flag alphabet, each letter on a case that separates it from None. The
# Java audit's lesson: a row that answers the same either way pins nothing.
CASES += [case("flags", pattern, subject, flags) for pattern, subject, flags in (
    ("a", "A", "i"), ("a", "A", ""),
    ("^b", "a\nb", "m"), ("^b", "a\nb", ""),
    (".", "\n", "s"), (".", "\n", ""),
    ("a # hi\n b", "ab", "x"), ("a # hi\n b", "ab", ""),
    ("(a)", "a", "n"), ("(a)", "a", ""),
    ("ſ", "S", "i"), ("ſ", "S", "iC"),
    ("(?i)a", "A", ""), ("(?-i)a", "A", "i"),
)]

# Anchors and the end-of-subject family, where `$` before a final newline is
# the row every dialect differs on.
CASES += [case("anchors", pattern, subject, flags, mode) for pattern, subject, flags, mode in (
    ("a$", "a\n", "", "first"),
    (r"a\Z", "a\n", "", "first"),
    (r"a\z", "a\n", "", "first"),
    (r"\Aa", "a", "", "first"),
    ("a$", "a\n", "m", "first"),
    ("$", "a\n", "", "all"),
)]

# Lookbehind, which .NET makes unbounded and matches right to left.
CASES += [case("lookbehind", pattern, subject) for pattern, subject in (
    ("(?<=ab)c", "abc"),
    ("(?<=a|ab)c", "abc"),
    ("(?<=a{1,3})b", "aaab"),
    ("(?<=a+)b", "aab"),
    ("(?<=a*)b", "aaab"),
    (r"(?<=(a)\1)b", "aab"),
    ("(?<!a)b", "cb"),
)]

# The replacement template grammar, section 5.11's .NET column.
CASES += [case("template", pattern, subject, "", mode, argument)
          for pattern, subject, mode, argument in (
    ("b", "abc", "replace-all", "[$0]"),
    ("(b)", "abc", "replace-all", "[$1]"),
    ("(b)", "abc", "replace-all", "[${1}]"),
    ("(?<n>b)", "abc", "replace-all", "[${n}]"),
    ("b", "abc", "replace-all", "[$&]"),
    ("b", "abc", "replace-all", "[$`]"),
    ("b", "abc", "replace-all", "[$']"),
    ("(a)(b)", "ab", "replace-all", "[$+]"),
    ("b", "abc", "replace-all", "[$_]"),
    ("b", "abc", "replace-all", "[$$]"),
    ("b", "abc", "replace-all", "[$9]"),
    ("b", "abc", "replace-all", "[${nope}]"),
    ("b", "abc", "replace-all", "[$"),
    ("b", "abcb", "replace-first", "[$0]"),
)]

# Splitting, section 5.16. .NET's two overloads are two rules and the probe
# asks both: Split(input) and Split(input, count), the second of which
# *throws* on a negative where Java reads one as "no limit".
CASES += [case("split", pattern, subject, "", mode, argument)
          for pattern, subject, mode, argument in (
    (",", "a,b,c", "split", ""),
    ("(,)", "a,b,c", "split", ""),
    (",", "a,b,c", "split-count", "2"),
    (",", "a,b,c", "split-count", "0"),
    (",", "a,b,c", "split-count", "-1"),
    (",", ",a,", "split", ""),
    ("", "ab", "split", ""),
    ("x*", "abc", "split", ""),
    (",", "a,b,c", "split-count", "99"),
)]

# The feature bits in `GRX_SyntaxSpec::features`, asked as "does this
# construct compile" rather than read off Microsoft's reference page. The
# mask for this dialect was written before there was a .NET to ask, so every
# bit in it was a claim; `compile` mode turns each into an answer. A syntax
# error means the construct is absent, which is what a missing bit asserts.
CASES += [case("features", pattern, "", flags, "compile")
          for pattern, flags in (
    ("a|b", ""),                 # ALT
    ("a*", ""),                  # REP
    ("a*?", ""),                 # LAZY
    ("a*+", ""),                 # POSS: absent in .NET, so expect an error
    ("(?:a)", ""),               # NCAP
    ("(?<n>a)", ""),             # NAME, angle spelling
    ("(?'n'a)", ""),             # NAME, quote spelling - .NET's second one
    (r"(a)\1", ""),              # BREF
    (r"(?<n>a)\k<n>", ""),       # BREF by name
    ("(?=a)", ""),               # LAH
    ("(?!a)", ""),
    ("(?<=a)", ""),              # LBH
    ("(?<!a)", ""),
    ("(?>a)", ""),               # ATOM
    ("(a)(?(1)b|c)", ""),        # COND, by group number
    ("(?(?=a)b|c)", ""),         # COND, by assertion
    ("(a)(?1)", ""),             # SUBR: absent, expect an error
    ("a(?R)?b", ""),             # RECU: absent, expect an error
    ("(?i)a", ""),               # FLAG
    ("(?i:a)", ""),              # SFLG, which the mask does not claim
    ("(?n:(a))", ""),            # SFLG with .NET's own `n`
    ("(?#x)a", ""),              # CMNT
    (r"\p{L}", ""),              # UPRP
    (r"\p{IsGreek}", ""),        # UPRP, block spelling
    ("[a-z-[aeiou]]", ""),       # CSET
    (r"\b", ""),                 # WORD
    (r"\A", ""),                 # ANCH
    (r"\Z", ""),
    (r"\z", ""),
    (r"\G", ""),
    (r"\Qa.b\E", ""),           # QUOT: absent in .NET, expect an error
    (r"\x41", ""),               # HEX
    (r"\u0041", ""),
    (r"\052", ""),               # OCT
    (r"\cA", ""),                # CTRL
    (r"\e", ""),                 # .NET has `\e` for escape; perl does not
    (r"\a", ""),
    # Balancing groups, which are out of scope and deferred to their own
    # package. Asked anyway, because "deferred" should rest on the reference
    # having the construct rather than on nobody having looked.
    ("(?<n>a)(?<-n>b)", ""),
    (r"(?<a>x)(?<-a>y)(?(a)z)", ""),
)]

# Generated interaction inventory, which the differentials read. Append only:
# a case's index is cited by `dialects.md` and by the diff drivers' own
# output, so inserting one in the middle renumbers answers somebody has
# quoted. Each cross product stays small enough that a failing case prints as
# a reproducer rather than as a row number.
#
# The subjects are BMP throughout. A subject holding a character above U+FFFF
# is the §6 deviation rather than a comparison - .NET matches over code units
# there and this library cannot name a position inside a character - so the
# differentials would be scoring a difference already recorded. The astral
# rows live in the `unit` area, which is WP-49's inventory and not a gate.
DOTNET_INVENTORY_REVISION = "2026-10-21w"

# The escape alphabet, every letter of it, bare and inside a class.
#
# This area exists because the inventory had no `\X`, `\K` or `\R` row at all
# and this library accepted all three where .NET answers "Unrecognized escape
# sequence". Eleven green differentials and a coverage check reporting "0 rows
# asked by nothing" sat on top of that, because a coverage figure over the
# rows that exist says nothing about the rows that should exist. Seven letters
# were wrong: `\h`, `\H`, `\V`, `\K`, `\R`, `\X` and `\N`.
#
# Swept rather than sampled, for the reason the profile sweep is: a letter
# nobody thought to ask about is exactly the one that is wrong. 52 letters in
# both contexts is 104 rows, and the five letters that take an argument get
# one - `\c`, `\x`, `\u`, `\p` and `\k` are refused for *lacking* an argument,
# which a bare probe cannot tell apart from being outside the alphabet. `\k`
# needs its group to exist as well, so its rows carry a `(?<n>a)` prefix.
ESCAPE_ARGUMENT = {"c": "A", "x": "41", "u": "0041", "p": "{L}", "P": "{L}",
                   "k": "<n>"}
ESCAPE_LETTERS = ("abcdefghijklmnopqrstuvwxyz"
                  "ABCDEFGHIJKLMNOPQRSTUVWXYZ")


def _escape_case(letter, in_class):
    body = "\\" + letter + ESCAPE_ARGUMENT.get(letter, "")
    prefix = "(?<n>a)" if letter == "k" else ""
    return "%s[%s]" % (prefix, body) if in_class else prefix + body


CASES += [case("escape-alphabet", _escape_case(letter, in_class),
               mode="compile")
          for in_class in (0, 1)
          for letter in ESCAPE_LETTERS]

# The two rules a letter table cannot carry, and the construct that is not an
# escape at all.
#
# `\x` *is* in .NET's alphabet, so it cannot be closed out the way `\o` and
# `\g` are - what differs is the argument, and .NET's is exactly two hex
# digits like Python's. `(*...)` is here because .NET has no verb of any kind
# and reaches its refusal by a different route: `(` opens an ordinary group
# and the `*` is then a quantifier with nothing to repeat.
CASES += [case("escape-alphabet", pattern, mode="compile") for pattern in (
    r"\x41", r"\x4", r"\xg", r"\x{41}", r"\x", r"\x4142",
    r"A", r"\u41", r"\cA", r"\c",
    r"\o{101}", r"(a)\g1", r"\N{U+0041}", r"a\Xb",
    "(*FAIL)", "(*SKIP)", "(*ACCEPT)", "(*:n)",
)]

# What the letters both sides *accept* actually mean.
#
# The escape-alphabet sweep above asks accept-or-reject, and that is
# structurally unable to catch a letter the two read differently: `\v`
# compiles in both readings, and it was the Perl family's vertical-space
# shorthand here - five code points, U+000A among them - against .NET's
# vertical tab. The compile sweep reported agreement and the escape was
# wrong. So the letters that survive the alphabet get their semantics asked
# too, against the subjects that divide one reading from another.
ESCAPE_SEMANTICS = (
    # The single-code-point escapes, each against its own character and the
    # one a wider reading would also take.
    (r"\a", "\a"), (r"\a", "\n"),
    (r"\e", "\x1b"), (r"\e", "e"),
    (r"\f", "\f"), (r"\f", "\n"),
    (r"\n", "\n"), (r"\n", "\r"),
    (r"\r", "\r"), (r"\r", "\n"),
    (r"\t", "\t"), (r"\t", " "),
    # The one that was wrong: U+000B against U+000A, U+0085, U+2028 and
    # U+2029, which the Perl family's `\v` takes and .NET's does not.
    (r"\v", "\x0b"), (r"\v", "\n"), (r"\v", "\r"),
    (r"\v", "\u0085"), (r"\v", " "), (r"\v", " "),
    # The shorthands against the code points that divide ASCII from Unicode
    # and one word-set reading from another.
    (r"\d", "0"), (r"\d", "١"), (r"\d", "a"),
    (r"\D", "0"), (r"\D", "١"),
    (r"\s", " "), (r"\s", "\x0b"), (r"\s", " "),
    (r"\s", "\u0085"), (r"\s", " "), (r"\s", " "),
    (r"\S", " "), (r"\S", " "),
    (r"\w", "a"), (r"\w", "_"), (r"\w", "é"), (r"\w", "ͅ"),
    (r"\W", "a"), (r"\W", "é"),
    # The boundaries and the anchors, where a trailing newline is what
    # divides `\Z` from `\z`.
    (r"a\b", "a "), (r"a\b", "ab"), (r"a\B", "ab"), (r"a\B", "a "),
    (r"\Aa", "a"), (r"\Aa", "ba"),
    (r"a\Z", "a"), (r"a\Z", "a\n"), (r"a\Z", "a\nb"),
    (r"a\z", "a"), (r"a\z", "a\n"),
    (r"\Ga", "a"), (r"\Ga", "ba"),
    # The escapes with an argument.
    (r"\cA", "\x01"), (r"\cA", "A"),
    (r"\x41", "A"), (r"\x41", "a"),
    (r"A", "A"), (r"A", "a"),
    (r"\p{L}", "a"), (r"\p{L}", "0"), (r"\P{L}", "a"), (r"\P{L}", "0"),
    (r"(?<n>a)\k<n>", "aa"), (r"(?<n>a)\k<n>", "ab"),
    # The backspace, which `\b` is inside a class and the boundary outside.
    (r"[\b]", "\b"), (r"[\b]", "b"),
)

CASES += [case("escape-semantics", pattern, subject)
          for pattern, subject in ESCAPE_SEMANTICS]

# A quantifier on a zero-width assertion, which .NET allows and this library
# refused.
#
# Found by chasing one row - `\b{3}`, where .NET compiles and this library
# answered "nothing to repeat" - and the rule turned out to cover every
# anchor and every quantifier spelling. The brace really is a quantifier
# there and not a literal: `\b{3}` over "ab" is 0-0, which a literal `{3}`
# could not match, so the compile rows alone would not have settled it and
# the match rows below are the half that does.
#
# `{3}` with nothing before it and `a{3}{3}` are refused by .NET as well,
# and are here so the rule is bounded from both sides.
CASES += [case("quantified-anchor", pattern, mode="compile") for pattern in (
    r"\b{3}", r"\B{3}", r"^{3}", r"${3}", r"\A{3}", r"\Z{3}", r"\z{3}",
    r"\G{3}", r"\b*", r"\b+", r"\b?", r"^*", r"a$?", r"\b{2,}",
    r"(?=a){3}", r"(?!a){3}", r"(?<=a){3}", r"(?=a)*",
    r"{3}", r"a{3}{3}",
)]

CASES += [case("quantified-anchor", pattern, subject)
          for pattern, subject in (
    (r"\b{3}", "ab"), (r"\b{3}", "a{3}"), (r"a\b{3}", "a{3}"),
    (r"\b*", "ab"), (r"^{3}", "ab"), (r"a$?", "a"), (r"\B{3}", "ab"),
)]


CASES += [case("generated-syntax", pattern, mode="compile") for pattern in (
    "a", "(a|b)+", "(?:a)", "(?<n>a)", "(?'n'a)", r"(?<n>a)\k<n>",
    "[a-z-[aeiou]]", "[a-z-[b-d-[c]]]", r"[\w-[\d]]", "[[a]",
    # Each of these the reference refuses, and for its own reason: a
    # subtraction that is not last, a possessive quantifier, a subroutine
    # call, recursion, and `\Q`. A differential with no refusals in it
    # measures only the half of a parser that says yes.
    "[a-z-[aeiou]x]", "a*+", "(a)(?1)", "a(?R)?b", r"\Qa.b\E",
    "(?#c)a", "(?i:a)", "(?n:(a))", "(a)(?(1)b|c)", "(?(?=a)b|c)",
    r"\p{IsGreek}", r"\p{L}", r"\cA", r"\x41", r"\u0041", r"\052",
    # Balancing groups: the reference compiles them and this library refuses
    # them, which is the one row here whose disagreement is expected and
    # recorded. It is in the inventory so the deferral stays visible.
    "(?<n>a)(?<-n>b)",
)]

CASES += [case("generated-match", pattern, subject)
          for pattern in ("(a|b)+", "(?i:a)b", r"\d+", "[a-z-[aeiou]]",
                          "(?<=a)b", "(?=(a))a", "(?<n>x)(y)", r"\w+")
          for subject in ("ab", "aab", "b", "1", "\u00e9", "xy")]

CASES += [case("generated-search", pattern, subject, mode="all")
          for pattern in ("a*", r"\G.", "(?=a)", "(a)?b", "")
          for subject in ("ab", "baa", "")]

CASES += [case("generated-replacement", pattern, subject,
               mode="replace-all", argument=template)
          for pattern, template in (("(a)", "<$1>"),
                                    ("(?<n>a)", "<${n}>"),
                                    ("(a)(b)", "<$+>"),
                                    ("a", "<$_>"),
                                    ("a", "<$&>"),
                                    ("a", "<$$>"),
                                    ("a", "<$9>"),
                                    ("a", "<${nope}>"))
          for subject in ("a", "ab", "ba", "")]

CASES += [case("generated-split", pattern, subject, mode=mode,
               argument=argument)
          for pattern in (",", "(,)", "x*", "", r"(\d)")
          for subject, mode, argument in (("a,b,c", "split", ""),
                                          (",a,", "split", ""),
                                          ("", "split", ""),
                                          ("a1b2c", "split", ""),
                                          ("a,b,c", "split-count", "2"),
                                          ("a,b,c", "split-count", "1"))]

# The GRX_Profile axes. Each row names the field it separates, and the rule
# from the Java audit applies: a case that answers the same under both values
# of a field pins nothing, so several come as pairs.
CASES += [case("profile", pattern, subject) for pattern, subject in (
    # preference and submatch: leftmost-first, not leftmost-longest.
    ("a|ab", "ab"), ("(a|ab)(c|bcd)", "abcd"),
    # composing: a combining mark is its own character, not part of the base.
    ("^.$", "é"), ("^..$", "é"),
    # caret_after_final_newline.
    ("(?m)^$", "a\n"), ("(?m)^", "a\n"),
    # word_set, as a pair so the default and the ECMAScript reading separate.
    (r"\w", "²"), (r"\w", "ͅ"), (r"\w", "Ⓐ"), (r"\w", "_"),
    # fold_utf and caseless_widens_shorthands.
    ("(?i)ſ", "s"), ("(?i)\\w", "ſ"),
    # newlines: which characters `.` declines and `$` accepts.
    (".", "\r"), (".", " "), ("a$", "a "),
    # mongolian_separator_is_space.
    (r"\s", "᠎"), (r"\s", "​"),
)]


# `RegexOptions.ECMAScript`, which is a constructor option and has no inline
# spelling: there is no `(?E)`, so a caller is the only thing that can ask for
# it. Phase 4 of the plan implements it, and this is what it is implemented
# against. Three questions, and the third is the one worth asking:
#
#   * which characters the three shorthands take, inside a class and out,
#     and whether `\b` moves with `\w`. The documentation says ECMAScript's
#     definitions; ECMAScript's own `\s` includes U+00A0, U+2028 and U+FEFF,
#     so "ECMAScript's" and "ASCII" are two different claims and the rows
#     below separate them.
#   * whether it narrows anything it is not documented to narrow -
#     `\p{L}`, the `[a-z-[aeiou]]` subtraction, the named-group syntax.
#   * **whether it renumbers groups.** ECMAScript numbers named and unnamed
#     groups together in order of appearance; .NET's default numbers every
#     unnamed group first and the named ones after. If this option moves
#     .NET onto ECMAScript's rule, it is not a class-narrowing option at all
#     and the dialect cannot implement it as one.
#
# The refusals are their own question: .NET rejects some option combinations
# with this one rather than ignoring them, so each letter is asked in
# `compile` mode beside it.
CASES += [case("ecmascript", pattern, subject, flags, mode) for
          pattern, subject, flags, mode in (
    # `\d`: the documented narrowing, with the control beside it.
    (r"\d", "0", "E", "first"), (r"\d", "١", "E", "first"),
    (r"\d", "١", "", "first"),
    (r"\D", "١", "E", "first"),
    (r"\d", "\U0001D7CE", "E", "first"),
    # `\w`: four subjects that each separate a candidate set. U+00B2 is `No`,
    # U+0345 is `Mn`, U+24B6 is `So`, U+0065 is plain.
    (r"\w", "e", "E", "first"), (r"\w", "é", "E", "first"),
    (r"\w", "²", "E", "first"), (r"\w", "ͅ", "E", "first"),
    (r"\w", "Ⓐ", "E", "first"), (r"\w", "_", "E", "first"),
    (r"\W", "é", "E", "first"),
    # `\s`: the rows that separate ECMAScript's set from ASCII's. U+00A0,
    # U+2028 and U+FEFF are whitespace to ECMAScript and not to ASCII.
    (r"\s", " ", "E", "first"), (r"\s", "\t", "E", "first"),
    (r"\s", "\v", "E", "first"), (r"\s", "\f", "E", "first"),
    (r"\s", "\u00a0", "E", "first"), (r"\s", "\u2028", "E", "first"),
    (r"\s", "\ufeff", "E", "first"), (r"\s", "\u1680", "E", "first"),
    (r"\S", "\u00a0", "E", "first"),
    # Inside a class, and negated: a narrowing that does not reach the class
    # is a different option from one that does.
    (r"[\d]", "١", "E", "first"), (r"[\w]", "é", "E", "first"),
    (r"[\s]", " ", "E", "first"), (r"[^\w]", "é", "E", "first"),
    # `\b` is defined from `\w`, so it either moves with it or it does not.
    (r"\bx", "éx", "E", "first"), (r"\bx", "éx", "", "first"),
    (r"\Bx", "éx", "E", "first"),
    # Things it is not documented to narrow.
    (r"\p{L}", "é", "E", "first"), (r"\p{Nd}", "١", "E", "first"),
    ("[a-z-[aeiou]]", "b", "E", "first"),
    ("[a-z-[aeiou]]", "a", "E", "first"),
    (r"(?<n>a)\k<n>", "aa", "E", "first"),
    # **Group numbering.** If this moves to ECMAScript's rule the answer is
    # ECMAScript-shaped and the dialect's renumbering is wrong
    # under the option; if it keeps .NET's, the named group is group 2.
    ("(?<a>x)(y)", "xy", "E", "first"),
    ("(?<a>x)(y)", "xy", "", "first"),
    ("(?<a>x)(y)", "xy", "E", "names"),
    ("(?<a>x)(y)", "xy", "", "names"),
    # Backreferences and octal, where ECMAScript's reading of `\1` with no
    # group one is a literal and .NET's default is an error.
    (r"\1", "1", "E", "compile"), (r"\1", "1", "", "compile"),
    (r"(a)\1", "aa", "E", "first"),
    (r"\12", "\n2", "E", "compile"), (r"\12", "\n2", "", "compile"),
    # Folding under the option, with and without the culture letter.
    ("ſ", "s", "iE", "first"), ("ſ", "s", "i", "first"),
    ("ſ", "s", "iEC", "first"),
    # Which *rule* `IgnoreCase` is, which the rows above turned up as a
    # question rather than answering: U+017F declines `s` here and shares a
    # simple-fold orbit with it, so this is not simple case folding - and it
    # declines it under `CultureInvariant` too, so it is not culture either.
    # The candidate is equality under the simple *lowercase mapping*, and
    # these pairs separate the two rules in both directions: U+212A and
    # U+0130 lowercase into ASCII where folding leaves one of them alone,
    # and U+0131 is the reverse.
    ("\u212a", "k", "iC", "first"), ("\u0130", "i", "iC", "first"),
    ("\u0131", "I", "iC", "first"), ("\u00df", "ss", "iC", "first"),
    ("\u1e9e", "\u00df", "iC", "first"),
    ("\u13a0", "\uab70", "iC", "first"),
    ("\u0345", "\u03b9", "iC", "first"),
    # Which letters may be combined with it at all.
    # The combinations .NET accepts are asked as matches rather than as
    # compiles, so that the flags differential compares them instead of
    # having to name them as covered by nothing.
    ("a", "a", "E", "first"), ("a", "a", "iE", "first"),
    ("a", "a", "mE", "first"), ("a", "a", "CE", "first"),
    ("a", "a", "sE", "compile"), ("a", "a", "xE", "compile"),
    ("a", "a", "nE", "compile"), ("a", "a", "RE", "compile"),
    # The rest of ASCII whitespace, because the four rows above establish
    # that the set is ASCII's and not ECMAScript's and a set is not pinned
    # by four of its members.
    (r"\s", "\n", "E", "first"), (r"\s", "\r", "E", "first"),
    # What the escapes that only ECMAScript reads actually match. `\1` with
    # no group one compiles under the option and is a refusal without it, so
    # the option changes how the pattern is *parsed* and not only which
    # characters a shorthand takes - which is the finding that decides how it
    # can be implemented.
    (r"\1", "1", "E", "first"), (r"\1", "\x01", "E", "first"),
    (r"\12", "\n2", "E", "first"), (r"\12", "\n2", "", "first"),
    (r"\12", "12", "E", "first"),
    (r"\8", "8", "E", "compile"), (r"\8", "8", "", "compile"),
    (r"\08", "\x008", "E", "compile"),
    (r"[\b]", "\x08", "E", "first"), (r"[\b]", "\x08", "", "first"),
    # An inline group that spells an option the constructor refuses beside
    # this one: the refusal is the constructor's, or it is the dialect's.
    ("(?s).", "\n", "E", "first"), ("(?x)a b", "ab", "E", "first"),
    ("(?n)(a)", "a", "E", "names"),
    # `\8` compiles under the option and `\1` reads as octal, so what the
    # non-octal digits mean is the remaining half of the rule: ECMAScript's
    # Annex B makes `\8` an identity escape, and a compile alone does not
    # say that.
    (r"\8", "8", "E", "first"), (r"\9", "9", "E", "first"),
    (r"\08", "\x008", "E", "first"), (r"\08", "08", "E", "first"),
    (r"\81", "81", "E", "first"),
    # A number above the group count with a group present, which is the
    # branch the default rule already reads as octal: this says the option
    # does not move it.
    (r"(a)\10", "a\x08", "E", "first"), (r"(a)\10", "a\x08", "", "first"),
    # `(a)\10` is octal U+0008 by default and is *not* under the option, so
    # the option does not merely add a fallback: it changes how many digits
    # a reference takes. ECMAScript's own Annex B reads the whole number and
    # falls back to octal when it exceeds the group count, which is the
    # default's answer - so whatever this is, it is not Annex B either. The
    # candidate is a longest-valid-prefix scan: group one, then a literal
    # zero.
    (r"(a)\10", "aa0", "E", "first"),
    (r"(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)\10", "abcdefghijj", "E", "first"),
    (r"(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)\10", "abcdefghijj", "", "first"),
    (r"(a)\11", "aa1", "E", "first"), (r"(a)\11", "a\t", "", "first"),
    # The ceiling the prefix scan is measured against: a group that exists
    # but has not been opened yet. If it is the whole pattern's count, a
    # forward reference is a reference and matches the empty string an unset
    # group gives; if it is the count so far, `\1` here is U+0001.
    # - and the subject has to separate them, because a reference to a group
    # that has not participated *fails* in .NET rather than matching empty,
    # so "a" is a nomatch under both readings and pins nothing.
    (r"\1(a)", "\x01a", "E", "first"), (r"\1(a)", "\x01a", "", "first"),
    (r"\2(a)", "\x02a", "E", "first"), (r"\2(a)", "\x02a", "", "first"),
    # Inside a class there is no reference to be a prefix of, so the option
    # has nothing to change.
    (r"[\1]", "\x01", "E", "first"), (r"[\1]", "\x01", "", "first"),
)]


# The profile audit's condition 1, the Java audit's instrument applied here:
# `GRX_Profile` has 41 fields, .NET's entry assigns 13 and the rest take the
# zero value, and a value that is right and a value nobody checked look
# identical in the table. So for each unassigned field, a pattern and subject
# whose *answer changes* if the value were different.
#
# The `profile` area above already covers eight of them - preference,
# submatch, composing, word_set, fold_utf, caseless_widens_shorthands,
# newlines and mongolian_separator_is_space - and the `flags` area covers
# multiline_by_default. These are the rest, each row named with its field.
CASES += [case("profile-audit", pattern, subject, flags, mode) for
          pattern, subject, flags, mode in (
    # posix_* (eight fields). .NET has no POSIX class at all, so none of them
    # is reachable - and that is a measurement, not an assumption: the
    # bracket reads as the literal set `[[:alph` with a `]` after it, which
    # these three rows pin. `[[:alpha:]]` declines "a" alone, takes "a]",
    # and declines "x]" because `x` is not one of the letters inside.
    ("[[:alpha:]]", "a", "", "first"),
    ("[[:alpha:]]", "a]", "", "first"),
    ("[[:alpha:]]", "x]", "", "first"),
    # word_boundary. `\<` and `\>` are not constructs .NET has, so the field
    # has nothing to compare; an unknown escape says so.
    # - and it compiles, which was not the expected answer, so what it
    # *matches* is the question. A literal `<` means the field is still
    # unreachable and `\<` is an identity escape; anything else would mean
    # .NET has a boundary construct nothing had recorded.
    (r"\<a", "a", "", "compile"),
    (r"\<a", "<a", "", "first"), (r"\<a", "a", "", "first"),
    (r"\>a", ">a", "", "first"),
    # capture_reset: what an outer iteration does to a group the previous
    # iteration set. Over "ab" the first iteration sets group 1 and the
    # second sets group 2, so a rule that resets leaves group 1 unset and a
    # rule that keeps leaves it holding "a".
    ("(?:(a)|(b))+", "ab", "", "first"),
    # backref_unset: a reference to a group that exists and has not
    # participated. Matching empty gives a match here, failing gives none.
    (r"(?:(a)|b)\1", "b", "", "first"),
    # negative_look: whether a failed negative body's captures survive. The
    # body `(a)c` captures "a" and then fails, so group 1 holds "a" if they
    # are kept and nothing if they are discarded.
    ("(?!(a)c)ab", "ab", "", "first"),
    # search_start: what `\G` asserts while iterating, where the comment in
    # syntax.c says no case divides PREVIOUS_END from ATTEMPT. This is the
    # attempt to find one: with no previous match, PREVIOUS_END holds
    # position 0 and `\Gb` cannot reach the "b" at 1, while ATTEMPT moves
    # with the scan and can.
    # - and the `first` row divides nothing, which is the mistake worth
    # keeping visible: one search has `\G` at the start offset under both
    # rules. The field only speaks after a *failure* forces the loop to
    # advance, so the discriminator has to be a search-all. Section 5.10's
    # own example: `\Ga*` over "baac" is four matches under ATTEMPT, where
    # `\G` moves with the scan, and one under PREVIOUS_END, where it stays
    # at the end of the empty match at 0.
    (r"\Gb", "cb", "", "first"),
    (r"\Gb", "cb", "", "all"),
    (r"\Ga*", "baac", "", "all"),
    (r"\Gb*", "baac", "", "all"),
    (r"\Ga", "aab", "", "all"),
    # iteration, which the condition-2 sweep found *no* row divided -
    # including `a*` over "ba", the row section 5.10's .NET cell cites. A
    # pattern that can match non-empty where it just matched empty is what
    # separates the two rules, and `a*` is not one: after the empty match at
    # 0 of "ba" a retry still cannot match, so RETRY_THEN_ADVANCE and
    # ADVANCE_ONE both step to 1. A *lazy* quantifier is one: after the
    # empty match at 0 of "aa", a retry forbidding the empty match takes
    # "a", where ADVANCE_ONE steps to 1 and matches empty again.
    ("a*?", "aa", "", "all"),
    # A repeated group name, which the naming differential found this library
    # refuses and .NET accepts. .NET *merges* the two into one group rather
    # than letting two groups share a name the way PCRE2's `J` does, so the
    # numbering and the match are both at stake: these rows say which slot
    # the text lands in and what the alternation idiom does.
    ("(?<n>a)(?<n>b)", "ab", "", "first"),
    ("(?<n>a)|(?<n>b)", "b", "", "first"),
    ("(?<n>a)|(?<n>b)", "a", "", "first"),
    ("(?<n>a)(?<n>b)", "ab", "", "names"),
    ("(?<n>a)|(?<n>b)", "b", "", "names"),
    ("(x)(?<n>a)|(y)(?<n>b)", "yb", "", "names"),
    ("(x)(?<n>a)|(y)(?<n>b)", "yb", "", "first"),
    ("a??", "aa", "", "all"),
    ("(?:|a)", "aa", "", "all"),
    # empty_subject_is_a_boundary: `\b` with nothing on either side.
    (r"\b", "", "", "first"),
    (r"\B", "", "", "first"),
    # property_caret_is_error: `\p{^L}` is Perl's negation and may be a
    # refusal instead.
    (r"\p{^L}", "1", "", "compile"),
    (r"\p{^L}", "1", "", "first"),
    (r"\P{^L}", "1", "", "compile"),
    (r"[\p{^L}]", "1", "", "compile"),
    (r"\P{L}", "1", "", "first"),
    # property_match, which the condition-2 sweep found no row divided -
    # and the reason is worse than a missing case. `dialects.md` says .NET's
    # `Is` prefix is block-only against .NET's own list and that the loose
    # spellings are refused, and those measurements were taken by hand and
    # never pinned: nothing in the inventory asked for a long category name,
    # a lower-cased one, Perl's `In` prefix or `Is` in front of a category.
    # Mutating the field to LOOSE makes `\p{lu}` compile here, so the axis
    # is observable and was simply unasked.
    (r"\p{Lu}", "A", "", "compile"),
    (r"\p{Letter}", "a", "", "compile"),
    (r"\p{lu}", "A", "", "compile"),
    (r"\p{InGreek}", "\u03b1", "", "compile"),
    (r"\p{IsL}", "a", "", "compile"),
    (r"\p{IsGreekAndCoptic}", "\u03b1", "", "compile"),
    # The other two families a dialect might take here, so the rule is
    # bounded on both sides rather than inferred from the refusals above: a
    # binary property name, and a `name=value` pair.
    (r"\p{Alphabetic}", "a", "", "compile"),
    (r"\p{Script=Greek}", "\u03b1", "", "compile"),
    (r"\p{Nd}", "0", "", "compile"),
    # dollar, which the exhaustive sweep caught with only one of its two
    # alternatives: END_ONLY fails something and ALWAYS_LINE does not, so
    # nothing divided .NET's BEFORE_FINAL_NEWLINE from "`$` is always a line
    # anchor". A newline in the *middle* of the subject divides them - a
    # final one does not, which is why every row the inventory held was
    # blind to it.
    ("a$", "a\nb", "", "first"),
    ("b$", "a\nb", "", "first"),
    # **The WTF-8 axis, which the inventory did not ask at all.** The driver
    # has had a `w:` mode since it was written and nothing used it, so .NET
    # treating an unpaired surrogate as ordinary text was measured nowhere -
    # and that is why the sweep could not catch `subject_is_text` or
    # `subject_is_utf16`: both govern exactly this, and there were no rows
    # for them to govern. Unasked, not unobservable.
    #
    # A .NET string is UTF-16 and a lone surrogate is a legal char, so these
    # answer rather than erroring. The offsets are WTF-8 bytes, three per
    # surrogate.
    (".", "\ud800", "", "w:first"),
    (".", "\ud800a", "", "w:first"),
    ("a", "\ud800a", "", "w:first"),
    (r"\p{Cs}", "\ud800", "", "w:first"),
    (r"\p{L}", "\ud800", "", "w:first"),
    (r"[\s\S]", "\ud800", "", "w:first"),
    ("^.$", "\udfff", "", "w:first"),
    (r"\ud800", "\ud800", "", "w:first"),
    ("..", "𐀀x", "", "w:first"),
    # ascii_classes_fold_ascii: whether the dialect's own ASCII narrowing
    # takes the *folding* with it. U+00E9 and U+00C9 are one orbit and both
    # outside ASCII, so a narrowing that reached the folding would part them.
    ("é", "É", "iE", "first"), ("é", "É", "i", "first"),
    # caseless_leaves_boundary_narrow: whether a caseless mode re-widens the
    # `\b` that the narrowing made ASCII.
    (r"\bx", "éx", "iE", "first"),
    # newline_lf_option: whether any option narrows the terminator set to LF
    # alone. .NET's `.` declines only `\n`, so the set is LF already and
    # there is nothing for an option to narrow - these two rows are the
    # bound.
    (".", "\r", "", "first"), (".", "\n", "", "first"),
)]


def _encode(mode, text):
    """A `w:` mode's text fields are WTF-8, matching the driver.

    The encoding and the offsets reported are one decision, so one thing
    selects both - the same rule JavaOracle.java states for its own `w:`.
    """
    if mode.startswith("w:"):
        return wtf8_codec.encode(text)
    return text.encode("utf-8")


def run(rows):
    command = dotnet_runner.command()
    lines = []
    for row in rows:
        values = [row["mode"]] + [
            _encode(row["mode"], row[key]).hex()
            for key in ("pattern", "flags", "subject", "argument")]
        lines.append("\t".join(values))
    finished = subprocess.run(command, input="\n".join(lines) + "\n",
                              capture_output=True, text=True, timeout=600)
    if finished.returncode:
        raise RuntimeError("driver failed: %s" % finished.stderr)
    answers = []
    for line in finished.stdout.splitlines():
        state, sep, payload = line.partition("\t")
        if not sep or state not in ("ok", "error"):
            raise RuntimeError("bad driver response: %r" % line)
        answer = json.loads(bytes.fromhex(payload).decode("utf-8"))
        if state == "error":
            answer["driver_error"] = True
        answers.append(answer)
    if len(answers) != len(rows):
        raise RuntimeError("%d answers for %d cases" % (len(answers), len(rows)))
    return answers


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--record", action="store_true")
    parser.add_argument("--wrong-expected-control", action="store_true")
    parser.add_argument("--area", help="only this area, for reading by hand")
    args = parser.parse_args()
    if args.record and (oracle_env.MODE != "container"
                        or os.environ.get("GHOTI_ORACLE_ALIAS", "").strip()):
        raise RuntimeError(
            "recording requires pinned container mode without aliases")
    print(oracle_env.provenance(["dotnet"]), flush=True)
    if args.area:
        rows = [row for row in CASES if row["area"] == args.area]
        for row, answer in zip(rows, run(rows)):
            print("%-14s %-24r %-14r %s" % (row["mode"], row["pattern"],
                                            row["subject"], answer))
        return 0
    answers = run(CASES)
    path = os.path.join(DATA, "dotnet.json")
    payload = {"cases": CASES, "answers": answers}
    if args.record:
        os.makedirs(DATA, exist_ok=True)
        with open(path, "w", encoding="utf-8") as out:
            # ensure_ascii, for java_probe.py's reason: an unpaired surrogate
            # has no UTF-8 encoding, so writing one into a UTF-8 file raises -
            # after --record has already truncated it. JSON's own \uXXXX is
            # the mechanism for exactly that and round-trips.
            json.dump(payload, out, ensure_ascii=True, indent=2)
            out.write("\n")
        print("recorded %d answers in %s" % (len(answers), path))
        return 0
    with open(path, encoding="utf-8") as source:
        expected = json.load(source)
    if args.wrong_expected_control:
        expected["answers"][0] = {"kind": "planted-wrong-answer"}
    if expected["cases"] != CASES:
        raise RuntimeError("case inventory changed; record and review")
    if len(expected["answers"]) != len(answers):
        raise RuntimeError("answer count changed")
    wrong = [(i, actual, wanted) for i, (actual, wanted)
             in enumerate(zip(answers, expected["answers"])) if actual != wanted]
    for i, actual, wanted in wrong[:10]:
        print("case %d %s: %s != %s" % (i, CASES[i], actual, wanted),
              file=sys.stderr)
    print("%d cases, %d agree, %d differ"
          % (len(answers), len(answers) - len(wrong), len(wrong)))
    return bool(wrong)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (RuntimeError, oracle_env.OracleUnavailable) as error:
        sys.stderr.write("dotnet_probe: %s\n" % error)
        sys.exit(1)
