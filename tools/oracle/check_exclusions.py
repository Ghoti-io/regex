#!/usr/bin/env python3
"""Controls for the differentials' exclusions.

An exclusion narrows a gate: it names rows where this library and a
reference differ for a reason already decided, so that the gate can stay
red for everything else. The hazard is the obvious one - an exclusion that
is too wide swallows a defect, and a gate that swallows defects is green
for the same reason a gate that works is green. Nothing else in this tree
can tell those two apart, because the generators only ever produce rows the
exclusions were written for.

So each predicate here is put a table: the rows it exists for, which it has
to recognise, and rows a hand's breadth away, which it has to refuse. The
near misses are the point. A table of positives alone would pass for any
predicate that always says yes, which is precisely the failure being
guarded against.

Every row is taken from a measurement already in the predicate's own
docstring, so this file states no new fact about any reference - it makes
the facts already claimed into something that can fail. It needs no
reference implementation and no build, so it runs with `make check-oracles`
rather than only in a soak.
"""

import sys
import contextlib
import io
import json
import tempfile
from unittest.mock import patch
import os

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import iregexp_diff
import java_match_diff
import iterate_diff
import perl_diff
import posix_diff
import replace_diff
import script_run_diff
import syntax_diff
import vim_diff
import vim_protocol
import check_vim_protocol
import make_vim_vectors
import window_diff


# Each block is (label, call, rows). A row is (reason, args, want), and
# `call` is handed the args.
BLOCKS = []


def block(label, call, rows):
    BLOCKS.append((label, call, rows))


block("java_match_diff.excluded", lambda pattern, subject, flags:
      java_match_diff.excluded({"pattern": pattern, "subject": subject,
                                "flags": flags}), [
    ("LITERAL uppercase answer", ("a", "A", "l"), True),
    ("CANON_EQ composed and decomposed", ("é", "e\u0301", "c"), True),
    ("LITERAL metacharacter", ("a+", "a+", "l"), True),
    ("same literal with comparable flags", ("a", "A", "i"), False),
    ("same pattern without LITERAL", ("a+", "a+", ""), False),
    ("unmeasured LITERAL row", ("b", "B", "l"), False),
])


# --------------------------------------------------------------------
# perl: a match beginning before the position `\G` reads.
# --------------------------------------------------------------------
block("iterate_diff.search_start_before_pos",
    lambda pattern, ours, theirs:
        bool(iterate_diff.search_start_before_pos("perl", pattern,
            ours, theirs)),
    [
    ("an extra match perl reached back for",
     (r"\w\Ga|b", "all 2 1:2 3:4", "all 3 1:2 1:3 3:4"), True),
    ("the reach-back compounding, perl's pos() running ahead",
     (r"\w\Ga|b", "all 2 1:2 3:4", "all 6 1:2 1:3 3:4 3:5 4:6 5:7"), True),
    ("an earlier start carrying a group that was empty at ours",
     (r"(a|)\G(?[ [a] ])", "all 2 0:1,0:0 1:2,1:1",
      "all 2 0:1,0:0 0:2,0:1"), True),
    ("a match perl found at the pos() it was searching from",
     (r"\w\Ga|b", "all 2 1:2 3:4", "all 3 1:2 2:3 3:4"), False),
    ("a match perl found past everything, at a fresh start",
     (r"\w\Ga|b", "all 2 1:2 3:4", "all 3 1:2 3:4 5:6"), False),
    ("a match of ours perl does not report",
     (r"\w\Ga|b", "all 3 1:2 2:3 3:4", "all 2 1:2 3:4"), False),
    ("an end that differs",
     (r"\w\Ga|b", "all 2 1:2 3:5", "all 3 1:2 1:3 3:4"), False),
    ("a group differing inside the span the two share",
     (r"\w\Gab", "all 2 1:3,1:2 3:5,3:4", "all 2 1:3,2:3 3:5,3:4"), False),
    ("no `\\G` in the pattern at all",
     (r"a|b", "all 2 1:2 3:4", "all 3 1:2 1:3 3:4"), False),
    ("a `\\G` that leads, so there is nothing to reach behind",
     (r"\Ga|b", "all 2 1:2 3:4", "all 3 1:2 1:3 3:4"), False),
])


# --------------------------------------------------------------------
# ECMAScript: a replacement written between the halves of a pair.
#
# Two blocks stood here, for replace_diff.heal_split_pairs() and
# replace_diff.splits_a_surrogate_pair(), and both predicates are gone. The
# deviation is now reported by node itself - node_replace.mjs and
# node_split.mjs answer "surrogate" when a match begins or ends inside a
# pair - so there is no predicate to put a table and no near miss to refuse.
#
# Worth keeping as the shape rather than only as a deletion: the predicates
# recognised the deviation by its *consequence*, an unpaired surrogate in
# the result, on the premise that nothing else could produce one. An option
# that let a subject hold one falsified the premise, and the first run with
# such subjects produced a row the predicate read as a split pair when
# nothing had been split. An exclusion identified by a side effect is an
# exclusion that widens when something else starts producing that side
# effect, and the fix was to ask the oracle where its own match boundaries
# fell.
# --------------------------------------------------------------------

# --------------------------------------------------------------------
# glibc: the empty final iteration under a stacked quantifier.
#
# All eighteen pairs, which is the measurement the predicate's docstring
# reports. Written as the pattern the row is generated as, so that a change
# to QUANTIFIER is asked the same question as a change to the rule.
# --------------------------------------------------------------------
KEEPS_EMPTY = ("?+", "*+", "++", "{1,2}+", "{0,2}+", "+*", "{1,2}*",
    "?{1,2}", "*{1,2}", "+{1,2}", "+{0,2}", "{1,2}{1,2}")
DOES_NOT = ("?*", "**", "+?", "*?", "??", "{1,2}?")

block("posix_diff.stacked_keeps_empty",
    lambda pattern: posix_diff.stacked_keeps_empty(pattern),
    [("glibc keeps the empty iteration under `%s`" % pair,
      ("(a|)" + pair,), True) for pair in KEEPS_EMPTY]
    + [("glibc does not, under `%s`" % pair,
        ("(a|)" + pair,), False) for pair in DOES_NOT]
    + [
    ("one quantifier is not a stack", ("(a|)+",), False),
    ("no quantifier at all", ("(a|)",), False),
])

block("posix_diff.stacked_quantifier",
    lambda pattern: posix_diff.stacked_quantifier(pattern),
    [
    ("two in a row", ("(a|)+*",), True),
    ("a bound stacked on a star", ("a*{1,2}",), True),
    ("one on its own", ("(a|)+",), False),
    ("two with an atom between them", ("a+b*",), False),
])


# --------------------------------------------------------------------
# perl: `\Q` is interpolation there, so a pattern arriving as text has an
# unknown escape where this library has PCRE2's construct.
# --------------------------------------------------------------------
block("perl_diff.holds_quoting",
    lambda pattern: perl_diff.holds_quoting(pattern),
    [
    ("a quoted run", (r"\Qa.b\E",), True),
    ("an opening mark with no close", (r"a\Qb",), True),
    ("a closing mark on its own", (r"a\Eb",), True),
    ("no mark at all", (r"a.b",), False),
    ("a literal backslash in front of a Q", (r"a\\Qb",), False),
])


# --------------------------------------------------------------------
# pcre2: a nested lookbehind reaching further than the offset allows, and
# a `$` after a scan substring forgetting NOTEOL.
# --------------------------------------------------------------------
block("window_diff.nested_lookbehind_reach",
    lambda pattern, window, ours, theirs:
        window_diff.nested_lookbehind_reach(pattern, window, ours, theirs),
    [
    ("pcre2 found nothing where the assertion holds",
     (r"(?<=(?<=a)b)", (4, None, ""), "match 2:2", "nomatch"), True),
    ("pcre2 found the same match later",
     (r"(?<=(?<=a)b)", (2, None, ""), "match 2:2", "match 4:4"), True),
    ("the window begins at zero, where the two agree",
     (r"(?<=(?<=a)b)", (0, None, ""), "match 2:2", "nomatch"), False),
    ("one lookbehind, the flat spelling of the same width",
     (r"(?<=ab)", (2, None, ""), "match 2:2", "nomatch"), False),
    ("pcre2 found the *earlier* match, which is still a disagreement",
     (r"(?<=(?<=a)b)", (2, None, ""), "match 4:4", "match 2:2"), False),
    ("this library found nothing",
     (r"(?<=(?<=a)b)", (2, None, ""), "nomatch", "match 2:2"), False),
])

block("window_diff.scan_substring_forgets_noteol",
    lambda pattern, subject, window, ours, theirs:
        window_diff.scan_substring_forgets_noteol(pattern, subject, window,
            ours, theirs),
    [
    ("the match pcre2 makes end where NOTEOL forbids",
     (r"(a)(*scs:(1)a)a*+$", "a", (0, None, "E"), "nomatch", "match 0:1"),
     True),
    ("the long spelling of the same verb",
     (r"(a)(*scan_substring:(1)a)a*+$", "a", (0, None, "E"), "nomatch",
      "match 0:1"), True),
    ("ending on the newline the window ends with",
     (r"(a)(*scs:(1)a)a*+$", "a\n", (0, None, "E"), "nomatch",
      "match 0:1"), True),
    ("no NOTEOL, so there is nothing to have been forgotten",
     (r"(a)(*scs:(1)a)a*+$", "a", (0, None, ""), "nomatch", "match 0:1"),
     False),
    ("the scan substring written after the `$`",
     (r"(a)$(*scs:(1)a)", "a", (0, None, "E"), "nomatch", "match 0:1"),
     False),
    ("a bracketed verb that is not a scan substring",
     (r"(a)(*atomic:a*)$", "a", (0, None, "E"), "nomatch", "match 0:1"),
     False),
    ("pcre2's match ends somewhere NOTEOL has no opinion about",
     (r"(a)(*scs:(1)a)a*+$", "ab", (0, None, "E"), "nomatch",
      "match 0:1"), False),
    ("this library matched too, so the difference is elsewhere",
     (r"(a)(*scs:(1)a)a*+$", "a", (0, None, "E"), "match 0:1",
      "match 0:1"), False),
])


# --------------------------------------------------------------------
# glibc: the anchor it reads two ways, and the group it loses.
# --------------------------------------------------------------------
block("posix_diff.is_known_deviation",
    lambda pattern, subject, newline:
        posix_diff.is_known_deviation(pattern, subject, newline),
    [
    ("an anchor beside something that consumes, from the left",
     ("$.", "a\nb", False), True),
    ("and from the right",
     (".^", "a\nb", False), True),
    ("with REG_NEWLINE, where both read the anchor as a line anchor",
     ("$.", "a\nb", True), False),
    ("a subject with no newline in it",
     ("$.", "ab", False), False),
    ("a pattern that is only an anchor",
     ("^", "a\nb", False), False),
    ("no anchor at all",
     ("a.b", "a\nb", False), False),
])

block("posix_diff.is_glibc_backreference_defect",
    lambda pattern, them, us:
        posix_diff.is_glibc_backreference_defect(pattern, them, us),
    [
    ("a repeated group in front of a backreference",
     (r"()+(a)\1", "match 0:1 0:0 -", "match 0:1 0:0 0:1"), True),
    ("the same, where glibc reports no match at all",
     (r"(){2}(a)\1", "nomatch", "match 0:1 0:0 0:1"), True),
    ("a stacked quantifier, which needs no group to repeat",
     (r"a?+(a)\1", "match 0:2 -", "match 0:2 0:1"), True),
    ("one quantifier instead of two",
     (r"a?(a)\1", "match 0:1 -", "match 0:1 0:1"), False),
    ("the backreference taken off",
     (r"()+(a)", "match 0:1 0:0 -", "match 0:1 0:0 0:1"), False),
    ("this library is the one that lost a group",
     (r"()+(a)\1", "match 0:1 0:0 0:1", "match 0:1 0:0 -"), False),
])

block("posix_diff.is_glibc_stacked_plus_defect",
    lambda pattern, them, us:
        posix_diff.is_glibc_stacked_plus_defect(pattern, them, us),
    [
    ("glibc put the group at the empty span the match ends on",
     (r"(a|)?+", "match 0:4 4:4", "match 0:4 3:4"), True),
    ("the same shape with the other stacked pair",
     (r"(a|)+*", "match 0:2 2:2", "match 0:2 1:2"), True),
    ("a pair glibc handles like every other spelling",
     (r"(a|)?*", "match 0:4 3:4", "match 0:4 3:4"), False),
    ("glibc's group is not the empty span at the end",
     (r"(a|)?+", "match 0:4 2:3", "match 0:4 3:4"), False),
    ("the whole match differs, which is not this question",
     (r"(a|)?+", "match 0:3 3:3", "match 0:4 3:4"), False),
    ("one quantifier, so nothing is stacked",
     (r"(a|)+", "match 0:4 4:4", "match 0:4 3:4"), False),
])

block("posix_diff.quantifier_bounds",
    lambda text: posix_diff.quantifier_bounds(text),
    [
    ("a star", ("*",), (0, None)),
    ("a basic RE's star, which needs no backslash", ("\\*",), (0, None)),
    ("a plus", ("+",), (1, None)),
    ("a basic RE's plus", ("\\+",), (1, None)),
    ("an option", ("?",), (0, 1)),
    ("a bound with both ends", ("{1,2}",), (1, 2)),
    ("a bound with one number", ("{2}",), (2, 2)),
    ("a bound with no ceiling", ("{0,}",), (0, None)),
    ("a basic RE's bound", ("\\{1,2\\}",), (1, 2)),
])


# --------------------------------------------------------------------
# vim: the captures and the marks an abandoned path wrote.
# --------------------------------------------------------------------


block("vim_diff.holds_composing",
    lambda subject: vim_diff.holds_composing(subject),
    [
    ("a mark after a base", ("á",), True),
    ("a mark later in the subject", ("ab́c",), True),
    ("a mark that begins the subject, which starts no cluster",
     ("́a",), False),
    ("no mark at all", ("ab",), False),
])


# --------------------------------------------------------------------
# The two references, where each is the one that is wrong - and the one
# construct this library refuses on purpose beside the one it has not built.
# --------------------------------------------------------------------
block("perl_diff.reference_defect (pcre2)",
    lambda pattern, them: perl_diff.reference_defect("pcre", pattern, them),
    [
    ("a lookbehind and an extended class whose body uses an operator",
     (r"(?<=a)(?[\w|\d])", "compile"), True),
    ("the non-atomic lookbehind spelling of the same",
     (r"(*naplb:a)(?[\w|\d])", "compile"), True),
    ("pcre2 compiled it, so there is no internal error to excuse",
     (r"(?<=a)(?[\w|\d])", "match 0:1"), False),
    ("a lookahead, which does not do it",
     (r"(?=a)(?[\w|\d])", "compile"), False),
    ("no extended class",
     (r"(?<=a)\w", "compile"), False),
])

block("perl_diff.reference_defect (perl)",
    lambda pattern, them: perl_diff.reference_defect("perl", pattern, them),
    [
    ("a branch reset whose group a relative reference reads",
     (r"(?|(a)|(b))(a)\g{-1}", "nomatch"), True),
    ("a branch reset whose group a conditional reads",
     (r"(?|(a)|(b))(?(1)x|y)", "match 0:2"), True),
    ("a branch reset with a plain backreference, which agrees",
     (r"(?|(a)|(b))\1", "nomatch"), False),
    ("a conditional with no branch reset, which agrees",
     (r"(?:(a)|(b))(?(1)x|y)", "match 0:2"), False),
])

# `perl_diff.library_deviation` was here until 2026-09-24 and is gone with
# the predicate: both shapes it named were built that day - PCRE2's `a`
# charset modifiers (WP-46) and a subroutine call into a non-atomic
# lookbehind's group - and a predicate with nothing left to exclude is not a
# predicate to keep controls for. What replaces it is the differential
# itself: `make check-oracle-perl` now compares those rows, and its
# "0 this library does not implement" is the assertion that the bucket is
# empty rather than merely unexamined.


# `replace_diff.es2025_duplicate_named_groups` was here until 2026-09-26 and
# is gone with the predicate, for the same reason: the rule it excused rows
# for is built, so `check-oracle-replace` compares those 576 rows instead of
# counting them. The bucket it fed was the last writer of that gate's
# "this library does not implement" column, which is gone too rather than
# left printing a zero nothing can change.


# --------------------------------------------------------------------
# ECMAScript: the duplicate group name V8 over-accepts.
# --------------------------------------------------------------------
# syntax_diff's exclusion is a second *measurement* - it renames the duplicate
# and asks this library again - so what needs controls is the rewrite, not the
# verdict. **These two blocks used to control a `MODIFIER_GROUP` pattern and a
# `neutralise()` that rewrote modifiers as well**; RegExp Modifiers are built
# now, so half of each control was describing a rewrite that no longer has a
# reason to exist. What is left is the name half, and the sign has flipped:
# the bucket excuses the *reference*, not this library.
#
# The rewrite has to keep the pattern's shape - same parentheses, same atoms -
# or a row could pass the re-ask for a reason that is not the duplicate name.
block("syntax_diff.rename_duplicates",
    lambda pattern, expected: syntax_diff.rename_duplicates(pattern)
        == expected,
    [
    ("a repeated name gets a fresh one",
     (r"(?<n>a)|(?<n>b)", r"(?<n>a)|(?<n_2>b)"), True),
    ("the third occurrence too",
     (r"(?<n>a)|(?<n>b)|(?<n>c)", r"(?<n>a)|(?<n_2>b)|(?<n_3>c)"), True),
    ("the shape V8 over-accepts",
     (r"(?<n>a)(?:b|(?<n>c))", r"(?<n>a)(?:b|(?<n_2>c))"), True),
    ("a modifier group is left alone, being built",
     (r"(?i:a)b", r"(?i:a)b"), True),
    ("a pattern with no repeat is unchanged",
     (r"(a)|(b)", r"(a)|(b)"), True),
    ("distinct names are left alone",
     (r"(?<n>a)|(?<m>b)", r"(?<n>a)|(?<m>b)"), True),
])

# And the predicate that decides which rows are candidates at all.
block("syntax_diff._has_repeated_name",
    lambda pattern: syntax_diff._has_repeated_name(pattern),
    [
    ("two groups with one name", (r"(?<n>a)|(?<n>b)",), True),
    ("the shape V8 over-accepts", (r"(?<n>a)(?:b|(?<n>c))",), True),
    ("two distinct names", (r"(?<n>a)|(?<m>b)",), False),
    ("one named group", (r"(?<n>a)",), False),
    ("no named group", (r"(a)|(b)",), False),
    ("a modifier group, which is not a name", (r"(?i:a)(?i:b)",), False),
])


# --------------------------------------------------------------------
# Perl: the three shapes the replacement arm excuses.
# --------------------------------------------------------------------
# All three are the *reference's* side of a difference this library chose, and
# each is a predicate rather than a verdict, so what needs controls is what the
# predicate matches and - more to the point - what it must not.
block("replace_diff.quotemeta_above_ascii",
    lambda theirs, ours: replace_diff.quotemeta_above_ascii(theirs, ours),
    [
    ("a backslash before U+2028, which perl adds and this does not",
     ("a\\\u2028b", "a\u2028b"), True),
    ("two of them", ("\\\u2028\\\u2029", "\u2028\u2029"), True),
    ("nothing above U+007F to strip", ("a\\.b", "a\\.b"), True),
    ("identical answers", ("abc", "abc"), True),
    ("a backslash before an ASCII character is not stripped",
     ("a\\.b", "a.b"), False),
    ("a difference that is not a backslash at all", ("abc", "abd"), False),
    ("an extra character beside the backslash",
     ("a\\\u2028bX", "a\u2028b"), False),
    ("the other direction, where *we* added the backslash",
     ("a\u2028b", "a\\\u2028b"), False),
])

# `\E` is matched to its openers by perl's tokenizer, over source text, and the
# model here reproduces that for one and two operators. Three is where they
# part, and the count is what the predicate reads - so the controls are about
# counting markers and nothing else.
block("replace_diff.nested_case_operators",
    lambda template: replace_diff.nested_case_operators(template),
    [
    ("three", (r"\U\Q$1\E",), True),
    ("four", (r"\U\Q$1\U$1\E",), True),
    ("two is not three", (r"\Q$1\E",), False),
    ("one", (r"\U$1",), False),
    ("none", ("$1$2",), False),
    ("a backslash that is not a marker", (r"\n\t\x41",), False),
    ("three of the same marker", (r"\Q\Q\Q",), True),
])


block("script_run_diff.is_pcre2_han_defect",
    lambda subject: script_run_diff.is_pcre2_han_defect(subject),
    [
    ("Han with two companion families, which pcre2's manual denies",
     ("漢한ㄅ",), True),
    ("Han with Hiragana and Hangul",
     ("漢か한",), True),
    ("Han with one companion family, which the manual allows",
     ("漢か",), False),
    ("Hiragana and Katakana, one family, with Han",
     ("漢かカ",), False),
    ("two companion families and no Han at all",
     ("か한",), False),
])


# --------------------------------------------------------------------
# ECMAScript: a literal astral character in the pattern source.
# --------------------------------------------------------------------
block("syntax_diff.holds_astral",
    lambda pattern: syntax_diff.holds_astral(pattern),
    [
    ("a character above the BMP", ("[a-\U0001F41F]",), True),
    ("a lone high surrogate, which is one unit on both sides",
     ("[a-\ud83d]",), False),
    ("a lone low surrogate", ("[a-\udc1f]",), False),
    ("nothing above the BMP at all", ("[a-z]",), False),
])

block("syntax_diff.without_astral",
    lambda pattern: syntax_diff.without_astral(pattern),
    [
    ("the astral character is replaced and nothing else is",
     ("[\udc1f-\U0001F41F]",), "[\udc1f-�]"),
    ("a pattern with none of them is unchanged",
     ("[a-z]",), "[a-z]"),
])

block("syntax_diff.ASTRAL_STAND_IN",
    lambda: ord(syntax_diff.ASTRAL_STAND_IN) > 0xDFFF
        and ord(syntax_diff.ASTRAL_STAND_IN) <= 0xFFFF,
    [
    ("the stand-in must outrank every surrogate, or it would turn an "
     "ascending range descending and hide the row it is testing", (), True),
])


# --------------------------------------------------------------------
# i-regexp: iregexp-check accepts a quantifier of one digit only.
# --------------------------------------------------------------------
block("iregexp_diff.multi_digit_quantifier",
    lambda pattern: iregexp_diff.multi_digit_quantifier(pattern),
    [
    ("the family the reference refuses and Figure 1 admits",
     ("a{10}",), True),
    ("a leading zero is two digits too", ("a{07}",), True),
    ("the upper bound counts as well as the lower", ("a{1,10}",), True),
    ("the RFC's own section 8 example", ("a{20,200000}",), True),
    # The near misses. A predicate that said yes to a single digit would
    # swallow every quantifier there is, which is the whole corpus.
    ("one digit is what the reference accepts", ("a{9}",), False),
    ("one digit on both sides", ("a{1,9}",), False),
    ("an open upper bound with one digit", ("a{9,}",), False),
    ("a brace that is not a quantifier at all", ("a{,3}",), False),
    ("an escaped brace", (chr(92) + "{10" + chr(92) + "}",), False),
    ("no quantifier anywhere", ("[a-z]+",), False),
])

# --------------------------------------------------------------------
# i-regexp: a class range the wrong way round, which Figure 1 admits and
# XSD refuses.
# --------------------------------------------------------------------
block("iregexp_diff.descending_range",
    lambda pattern: iregexp_diff.descending_range(pattern),
    [
    ("the plain case", ("[z-a]",), True),
    ("negated, which changes nothing about the endpoints", ("[^z-a]",), True),
    ("an escaped endpoint still has a code point", ("[a-" + chr(92) + "-]",), True),
    ("the escape on the low end", ("[" + chr(92) + "|-" + "!]",), True),
    ("a member before the range", ("[qz-a]",), True),
    # The near misses.
    ("an ascending range", ("[a-z]",), False),
    ("equal endpoints are not descending", ("[a-a]",), False),
    ("a dash that is a member, not an operator", ("[-a]",), False),
    ("a dash last", ("[a-]",), False),
    ("an escaped ascending range", ("[" + chr(92) + "--a]",), False),
    ("a quantifier, which this predicate must not claim", ("a{3,1}",), False),
    ("a property endpoint, which is a different refusal",
     (r"[\p{L}-z]",), False),
])

# --------------------------------------------------------------------
# i-regexp: libxml2 reads a range whose low end is escaped as a union.
# --------------------------------------------------------------------
block("iregexp_diff.escaped_range_endpoint",
    lambda pattern: iregexp_diff.escaped_range_endpoint(pattern),
    [
    ("the measured case", ("[" + chr(92) + "--a]",), True),
    ("the minimal pair's escaped half", ("[" + chr(92) + ".-a]",), True),
    ("after a member", ("[q" + chr(92) + "--a]",), True),
    ("negated", ("[^" + chr(92) + "--a]",), True),
    # The near misses. The unescaped half of the minimal pair is the row the
    # reference gets *right*, and a predicate that swallowed it would hide
    # every range there is.
    ("the minimal pair's unescaped half", ("[.-a]",), False),
    ("an ordinary range", ("[a-z]",), False),
    ("an escape that is a member rather than an endpoint",
     ("[" + chr(92) + "-a]",), False),
    ("an escape on the *high* end, which the reference reads correctly",
     ("[a-" + chr(92) + "-]",), False),
    ("no class at all", (chr(92) + "-a",), False),
])

# --------------------------------------------------------------------
# i-regexp: the general-category tables differ, which is *measured* - so the
# control feeds the predicate a map rather than a reference.
# --------------------------------------------------------------------
IREGEXP_CATEGORIES = {
    "a": ({"L", "Ll"}, {"L", "Ll"}),
    chr(0x0378): (set(), {"C", "Cn"}),
}

block("iregexp_diff.table_skew",
    lambda pattern, subject:
        iregexp_diff.table_skew(IREGEXP_CATEGORIES, pattern, subject),
    [
    ("a property row about a code point the two tables disagree on",
     (r"\p{C}", chr(0x0378)), True),
    ("the complement is a property row too",
     (r"\P{Cn}", chr(0x0378)), True),
    ("a property named inside a class",
     (r"[\p{C}a]", chr(0x0378)), True),
    # The near misses, and both halves have to hold: a row is excluded only
    # where a property meets a code point the tables disagree about.
    ("a property row about a code point they agree on",
     (r"\p{L}", "a"), False),
    ("a pattern with no property, on the skewed subject",
     (".", chr(0x0378)), False),
    ("a literal that looks like one", ("p{C}", chr(0x0378)), False),
    ("a subject the map has never heard of", (r"\p{L}", "zz"), False),
])


def require(condition, context):
    """Unlike assert, controls and their calls execute under optimized Python."""
    if not condition:
        raise AssertionError(context)


def protocol_controls(work, report_path):
    """Decode actual Vim transports before exercising CLI comparison/reporting."""
    restored = [r"\_s", r"\_d", r"\_w", r"\_[ab]", r"\_[^ab]", r"\_[^\n]",
        r"a\%[\_s]", r"\(a\)\@1<=", r"\(ab\)\@1<=", r"\(ab\)\@2<=",
        r"\(\w\+\)\@2<=", r"\(ab\)\@1<!", "(a|ab)@2<=",
        r"\(a\zsb\)\@>", r"\(a\zeb\)\@>", r"\(a\zsb\)\@=",
        r"\(a\zeb\)\@=", r"\(a\zeb\)\@<=", r"\(a\+\)\@>"]
    expected_restored = {(prefix + atom, subject) for atom in restored
        for prefix in ("", r"\Z") for subject in vim_diff.COMPOSING_SUBJECTS}
    require(len(expected_restored) == 380, "restored composing population")
    for syntax in vim_diff.PROFILES:
        emitted = []
        def reference(cases, engine):
            emitted.extend(cases)
            return ["nomatch"] * len(cases)
        with contextlib.ExitStack() as stack:
            stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
            stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
            stack.enter_context(patch.object(sys, "argv", ["vim_diff", "--syntax", syntax,
                "--patterns", "1", "--subjects", "1", "--strict", "--failures", report_path]))
            for name, value in {"vim_encoding": "utf-8", "vim_version": "test",
                    "find": "test", "make_pattern": "a"}.items():
                stack.enter_context(patch.object(vim_diff, name, return_value=value))
            stack.enter_context(patch.object(vim_diff, "ask_vim", side_effect=reference))
            stack.enter_context(patch.object(vim_diff, "ask_ours", side_effect=lambda driver, cases: ["nomatch"] * len(cases)))
            stack.enter_context(patch.object(vim_diff.escape_floor, "check", return_value=(1, 1)))
            require(vim_diff.main() == 0, "emitted composing CLI population")
        require(expected_restored <= set(emitted), "a restored composing row was not emitted")
        grammar = vim_diff.combining_grammar_cases()
        require(grammar and set(grammar) <= set(emitted), "combining grammar rows were not emitted")
    checked = 0
    for syntax, engine in vim_diff.PROFILES.items():
        match_cases = [
            ('match ["0:2", "é"]', "match backtrack 0:2 0:2", True),
            ('match ["2:3"]', "match backtrack 2:3 -", True),
            ('match ["2:3"]', "match backtrack 2:3 2:2", True),
            ('match ["2:3", "é"]', "match backtrack 2:3 0:2", True),
        ] + [("compile", reply, False) for reply in (
            "error 5", "match", "match backtrack", "match backtrack -",
            "match backtrack 2:1", "match backtrack 0:4", "match backtrack 0:1",
            "match backtrack 2:3 1:2", "match backtrack 2:3 0:9",
            "match backtrack 2:3 2:1", "match backtrack 2:3 0:2 garbage",
            "match invalid-engine 2:3", "nomatch extra", "compile bogus")]
        for reference, own, valid in match_cases:
            with contextlib.ExitStack() as stack:
                stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
                stack.enter_context(patch.object(sys, "argv", ["vim_diff",
                    "--syntax", syntax, "--patterns", "1", "--subjects", "1",
                    "--strict", "--failures", report_path]))
                for name, value in {"vim_encoding": "utf-8", "vim_version": "test",
                        "find": "test", "make_pattern": "a", "composing_cases": [], "combining_grammar_cases": [],
                        "ask_vim": [reference], "ask_ours": [own]}.items():
                    stack.enter_context(patch.object(vim_diff, name, return_value=value))
                stack.enter_context(patch.object(vim_diff, "SUBJECTS", ["éa"]))
                stack.enter_context(patch.object(vim_diff.escape_floor, "check", return_value=(1, 1)))
                try:
                    result = vim_diff.main()
                except ValueError:
                    require(not valid, (syntax, reference, own))
                else:
                    require(valid and result == 0, (syntax, reference, own, result))
            with open(report_path, encoding="utf-8") as source:
                report = json.load(source)
            require(report["complete"] == valid and report["failures"] == [], report)
            require((report["error"] is None) == valid, report)
            checked += 1

        ref_dir = os.path.join(work, "replacement-reference")
        os.makedirs(ref_dir, exist_ok=True)
        transport_cases = [("syntax", "compile 1", 0), ("syntax", "error 5", 1),
            ("ok ", "ok ", 0), ("ok c3a9", "ok c3a9", 0),
            ("error 5", "ok 61", None)]
        for text in ("syntax", "error", "template", "surrogate", "ok "):
            encoded = "ok " + text.encode("utf-8").hex()
            transport_cases.extend([(encoded, encoded, 0), ("syntax", encoded, 1),
                (encoded, "compile 1", 1), (encoded, "error 5", 1)])
        transport_cases.extend([(malformed, "ok 61", None)
            for malformed in ("ok zz", "ok 6", "ok ff", "ok eda080", "unknown")])
        transport_cases.extend([("ok 61", malformed, None)
            for malformed in ("ok zz", "ok 6", "ok ff", "ok eda080", "unknown")])
        def reference_payload(line):
            if line == 'syntax':
                return dict(status='exception', exception='Vim(let):E54: Unmatched \\(', throwpoint='control', phase='replacement', pattern_exception='Vim(call):E54: Unmatched \\(', pattern_throwpoint='isolation')
            if line.startswith('ok '):
                return dict(status='ok', hex=line[3:])
            if line.startswith('error '):
                return dict(status='exception', exception='Vim(let):E9999: injected unknown error', throwpoint='control', phase='replacement')
            return dict(status='unknown')
        for reference_line, own_line, wanted in transport_cases:
            def command(extra, scratch):
                require("set re=%d" % engine in extra, (syntax, extra))
                return ["vim-test"]
            def run(argv, **kwargs):
                if argv == ["vim-test"]:
                    with open(os.path.join(ref_dir, "answers.txt"), "w", encoding="utf-8") as output:
                        for index, line in enumerate((reference_line, 'syntax')):
                            answer = reference_payload(line)
                            answer.update(id=index, input=['a' if index == 0 else '[', 'aaa', 'X'])
                            output.write(json.dumps(answer) + '\n')
                    return type('Result', (), dict(returncode=0, stdout=b'', stderr=b''))()
                require(argv == ["library-test", syntax], argv)
                return type('Result', (), dict(returncode=0, stderr='', stdout=own_line + "\ncompile 1\n"))()
            with contextlib.ExitStack() as stack:
                stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
                stack.enter_context(patch.object(replace_diff, "find", return_value="library-test"))
                stack.enter_context(patch.object(replace_diff.vim_runner, "version", return_value="test"))
                stack.enter_context(patch.object(replace_diff.vim_runner, "command", side_effect=command))
                stack.enter_context(patch.object(replace_diff, "ask_vim", side_effect=replace_diff.ask_vim_batch_diagnostic))
                stack.enter_context(patch.object(replace_diff.tempfile, "mkdtemp", return_value=ref_dir))
                stack.enter_context(patch.object(replace_diff.subprocess, "run", side_effect=run))
                stack.enter_context(patch.object(replace_diff, "VIM_NAMED_PATTERNS", ["a"]))
                stack.enter_context(patch.object(replace_diff, "make_pattern", return_value="["))
                stack.enter_context(patch.object(replace_diff, "make_template", return_value="X"))
                stack.enter_context(patch.object(replace_diff.match_diff, "make_subject", return_value="aaa"))
                try:
                    result = replace_diff.main(["replace_diff", "--dialect", syntax,
                        "--patterns", "2", "--templates", "1", "--subjects", "1",
                        "--examples", "0", "--failures", report_path])
                except (ValueError, RuntimeError):
                    require(wanted is None, (syntax, reference_line, own_line))
                else:
                    require(wanted is not None and result == wanted,
                        (syntax, reference_line, own_line, result))
            with open(report_path, encoding="utf-8") as source:
                report = json.load(source)
            require(report["complete"] == (wanted is not None), report)
            if wanted is None:
                require(report["error"] and not report["completed_profiles"], report)
            else:
                require(len(report["failures"]) == wanted, report)
                if wanted:
                    row = report["failures"][0]
                    require(row["reference"] == vim_protocol.replacement_answer(reference_payload(reference_line))
                        and row["actual"] == replace_diff.parse_vim_reply(own_line), row)
            checked += 1
    return checked


def injected_answer_controls():
    """Exercise actual compare paths, reports and corpus emission independently."""
    checked = 0
    shapes = [r"\(a\)\@<=\(a\)\1", r"\(a\zeb\)\@<=\(a\zeb\)\@>",
        r"\(a\)\@=a\{2,}", r"\(a\)\@>x\|a", r"\1\(a\)\@<!",
        r"\v\_^*", r"\%23l*", r"^\m*", r"\(\(a\)\@=a\)\@<="]
    match_answers = [
        ('match ["0:1"]', "match backtrack 0:1", 0),
        ('match ["0:1", "a"]', "match backtrack 0:1 0:1", 0),
        ("nomatch", "nomatch", 0), ("compile", "compile 1", 0),
        ('match ["0:1"]', "compile 1", 1),
        ('match ["0:1"]', "match backtrack 0:2", 1),
        ('match ["0:1", "a"]', "match backtrack 0:1 0:2", 1),
        ('match ["0:1"]', "nomatch", 1),
    ]
    with tempfile.TemporaryDirectory() as work:
        report_path = os.path.join(work, "failures.json")
        checked += protocol_controls(work, report_path)
        checked += check_vim_protocol.check()
        for syntax, engine in vim_diff.PROFILES.items():
            for pattern in shapes:
                for reference_answer, own_answer, wanted in match_answers:
                    with contextlib.ExitStack() as stack:
                        stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                        stack.enter_context(patch.object(sys, "argv", ["vim_diff",
                            "--syntax", syntax, "--patterns", "1", "--subjects", "1",
                            "--strict", "--failures", report_path]))
                        for name, value in {"vim_encoding": "utf-8", "vim_version": "test",
                                "find": "test", "make_pattern": pattern, "composing_cases": [], "combining_grammar_cases": []}.items():
                            stack.enter_context(patch.object(vim_diff, name, return_value=value))
                        stack.enter_context(patch.object(vim_diff, "SUBJECTS", ["aaa"]))
                        reference = stack.enter_context(patch.object(vim_diff, "ask_vim", return_value=[reference_answer]))
                        stack.enter_context(patch.object(vim_diff, "ask_ours", return_value=[own_answer]))
                        stack.enter_context(patch.object(vim_diff.escape_floor, "check", return_value=(1, 1)))
                        require(vim_diff.main() == wanted, (syntax, pattern, own_answer))
                        reference.assert_called_once_with([(pattern, "aaa")], engine=engine)
                        with open(report_path, encoding="utf-8") as source:
                            report = json.load(source)
                        require(report["schema_version"] == 1 and report["complete"]
                            and len(report["failures"]) == wanted, report)
                    checked += 1
                for ref, own, raw, wanted in [("expected", "expected", "ok", 0),
                        ("syntax", "syntax", "compile 1", 0),
                        ("expected", "syntax", "compile 1", 1),
                        ("expected", "syntax", "compile 999", 1),
                        ("expected", "changed", "ok", 1)]:
                    with contextlib.ExitStack() as stack:
                        stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                        stack.enter_context(patch.object(replace_diff.vim_runner, "version", return_value="test"))
                        stack.enter_context(patch.object(replace_diff.perl_diff, "construct_not_implemented", return_value=999))
                        stack.enter_context(patch.object(replace_diff, "VIM_NAMED_PATTERNS", [pattern]))
                        stack.enter_context(patch.object(replace_diff, "make_pattern", return_value="["))
                        stack.enter_context(patch.object(replace_diff, "make_template", return_value=r"\1X"))
                        stack.enter_context(patch.object(replace_diff.match_diff, "make_subject", return_value="aaa"))
                        reference = stack.enter_context(patch.object(replace_diff, "ask_vim", return_value=[dict(status="refused") if ref == "syntax" else dict(status="ok", text=ref), dict(status="refused")]))
                        stack.enter_context(patch.object(replace_diff, "ask_library",
                            return_value=([dict(status="refused") if raw.startswith("compile ") else dict(status="ok", text=own), dict(status="refused")], [raw, "compile 1"])))
                        require(replace_diff.compare(syntax, "test", 1, 2, 1, 1, 0) == wanted,
                            (syntax, pattern, ref, own))
                        require(reference.call_count == 1 and reference.call_args.kwargs == {"engine": engine},
                            reference.call_args)
                    checked += 1

        for syntax in vim_diff.PROFILES:
            # Neither a short reference nor a short library match population
            # may reach result comparison or be reported as a complete run.
            for short_reference in (True, False):
                with contextlib.ExitStack() as stack:
                    stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                    stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
                    stack.enter_context(patch.object(sys, "argv", ["vim_diff",
                        "--syntax", syntax, "--patterns", "1", "--subjects", "1",
                        "--strict", "--failures", report_path]))
                    for name, value in {"vim_encoding": "utf-8", "vim_version": "test",
                            "find": "test", "make_pattern": "a", "composing_cases": [], "combining_grammar_cases": [],
                            "ask_vim": [] if short_reference else ['match ["0:1"]'],
                            "ask_ours": ["match backtrack 0:1"] if short_reference else []}.items():
                        stack.enter_context(patch.object(vim_diff, name, return_value=value))
                    stack.enter_context(patch.object(vim_diff, "SUBJECTS", ["aaa"]))
                    stack.enter_context(patch.object(vim_diff.escape_floor, "check", return_value=(1, 1)))
                    require(vim_diff.main() == 2, (syntax, "short match population", short_reference))
                with open(report_path, encoding="utf-8") as source:
                    report = json.load(source)
                require(report == dict(schema_version=1, kind="match", complete=False,
                    completed_profiles=[], failures=[], error="comparison incomplete"), report)
                checked += 1

            # Replacement has separate reference, decoded-library and raw
            # library populations. Test each through compare and the real CLI.
            for short_index in range(3):
                populations = [[dict(status="ok", text="expected"), dict(status="refused")],
                    [dict(status="ok", text="expected"), dict(status="refused")], ["ok", "compile 1"]]
                populations[short_index] = populations[short_index][:1]
                with contextlib.ExitStack() as stack:
                    stack.enter_context(contextlib.redirect_stdout(io.StringIO()))
                    stack.enter_context(contextlib.redirect_stderr(io.StringIO()))
                    stack.enter_context(patch.object(replace_diff, "find", return_value="test"))
                    stack.enter_context(patch.object(replace_diff.vim_runner, "version", return_value="test"))
                    stack.enter_context(patch.object(replace_diff, "VIM_NAMED_PATTERNS", ["a"]))
                    stack.enter_context(patch.object(replace_diff, "make_pattern", return_value="["))
                    stack.enter_context(patch.object(replace_diff, "make_template", return_value="X"))
                    stack.enter_context(patch.object(replace_diff.match_diff, "make_subject", return_value="aaa"))
                    stack.enter_context(patch.object(replace_diff, "ask_vim", return_value=populations[0]))
                    stack.enter_context(patch.object(replace_diff, "ask_library", return_value=tuple(populations[1:])))
                    require(replace_diff.compare(syntax, "test", 1, 2, 1, 1, 0) is None,
                        (syntax, "short replacement population", short_index))
                    require(replace_diff.main(["replace_diff", "--dialect", syntax,
                        "--patterns", "2", "--templates", "1", "--subjects", "1",
                        "--failures", report_path]) == 2, (syntax, short_index))
                with open(report_path, encoding="utf-8") as source:
                    report = json.load(source)
                require(report == dict(schema_version=1, kind="replacement", complete=False,
                    completed_profiles=[], failures=[], error="comparison incomplete"), report)
                checked += 2

        # Start with a stale successful report. Failure after one completed
        # dialect must replace it with the rows already collected, marked incomplete.
        for unavailable in (None, RuntimeError("reference unavailable")):
            with open(report_path, "w", encoding="utf-8") as output:
                output.write('{"complete": true, "failures": ["stale"]}')
            row = dict(syntax="ecmascript", pattern="a", subject="a",
                template="X", reference="X", actual="Y", flags="")
            def partial(dialect, driver, seed, patterns, templates, subjects, examples, failures, evidence=None):
                if dialect == "ecmascript":
                    failures.append(row)
                    return 1
                if isinstance(unavailable, Exception):
                    raise unavailable
                return unavailable
            with patch.object(replace_diff, "find", return_value="test"), \
                    patch.object(replace_diff, "compare", side_effect=partial):
                try:
                    result = replace_diff.main(["replace_diff", "--failures", report_path])
                except RuntimeError:
                    require(isinstance(unavailable, RuntimeError), "unexpected exception")
                else:
                    require(result == 2 and unavailable is None, result)
            with open(report_path, encoding="utf-8") as source:
                report = json.load(source)
            require(report == dict(schema_version=1, kind="replacement", complete=False,
                completed_profiles=["ecmascript"], failures=[row],
                error="reference unavailable" if unavailable else "comparison incomplete"), report)
            checked += 1
        with patch.object(sys, "argv", ["vim_diff", "--failures", report_path]), \
                patch.object(vim_diff, "vim_encoding", return_value="utf-8"), \
                patch.object(vim_diff, "vim_version", return_value="test"), \
                patch.object(vim_diff, "find", return_value=None), \
                contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            require(vim_diff.main() == 2, "unavailable match driver")
        with open(report_path, encoding="utf-8") as source:
            report = json.load(source)
        require(not report["complete"] and report["failures"] == [] and report["error"], report)
        checked += 1

        postfix = [r"\(a\)\@=", r"\(a\)\@!", r"\(a\)\@<=",
            r"\(a\)\@<!", r"\(a\)\@>", "literal@"]
        for syntax, engine in vim_diff.PROFILES.items():
            cases = [(pattern, "a") for pattern in postfix]
            span = "0:1" if engine == 1 else "1:1"
            answers = ['match ["%s", "a"]' % span] * len(cases)
            with patch.object(vim_diff, "ask_vim", return_value=answers) as reference:
                retained, expected = make_vim_vectors.collect(cases, {}, syntax)
                reference.assert_called_once_with(cases, engine=engine)
            path = os.path.join(work, syntax + ".rxt")
            with contextlib.redirect_stderr(io.StringIO()):
                count = make_vim_vectors.write_file(path, retained, expected, "test", {}, syntax)
            with open(path, encoding="utf-8") as source:
                written = source.read()
            require(count == len(cases), (syntax, count))
            for pattern in postfix:
                require(make_vim_vectors.artifact_shape(pattern) is None, pattern)
                # Independent expected records, including capture text and each
                # forced engine's distinct answer, must survive actual emission.
                record = "pattern: %s\nsubject: a\nexpect: %s\ngroups: a\n" % (
                    pattern.replace("\\", "\\\\"), span.replace(":", "-"))
                require(record in written, (syntax, record, written))
                checked += 1

    for fields in (["0:0"] + [""] * 9, ["0:1", "a"] + [""] * 8):
        record = make_vim_vectors.record_for("a?", "a", "match " + json.dumps(fields))
        groups = next((line for line in record.splitlines()
                       if line.startswith("groups: ")), None)
        require(groups is not None, ("missing explicit TEXT mode", fields, record))
        require(groups.split()[1:] == ["-" if not text else text for text in fields[1:]],
                ("capture texts changed", fields, record))
        checked += 1

    malformed = [None, 1, {}, "error", 'match ["99:1"]', 'match ["0:2"]',
        'match ["-1:1"]', 'match []', 'match {}', 'match [1]',
        'match ["0:1", 1]', 'match ["0:1", null]', 'match ["0:1", []]',
        'match ["0:1", "\\ud800"]', 'match ["0:1"] garbage',
        'match ["0:1", "a", "a", "a", "a", "a", "a", "a", "a", "a", "a"]']
    for answer in malformed:
        try:
            make_vim_vectors.record_for("a", "a", answer)
        except ValueError:
            checked += 1
        else:
            raise AssertionError("malformed oracle answer accepted: %r" % answer)
    try:
        make_vim_vectors.record_for("é", "é", 'match ["0:1"]')
    except ValueError:
        checked += 1
    else:
        raise AssertionError("mid-character byte bound accepted")
    return checked


def main():
    failures = 0
    checked = injected_answer_controls()
    for label, call, rows in BLOCKS:
        for reason, args, want in rows:
            checked += 1
            got = call(*args)
            # A predicate answers yes or no and is compared as such; the
            # handful of helpers under them answer a value, and `want` says
            # which by its own type.
            seen = bool(got) if isinstance(want, bool) else got
            if seen != want:
                failures += 1
                print("%s: %s\n  %s\n  wanted %s, got %s"
                    % (label, reason, "  ".join(repr(a) for a in args),
                        want, seen))
    print("check_exclusions: %d controls over %d predicates, %d failed"
        % (checked, len(BLOCKS), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
