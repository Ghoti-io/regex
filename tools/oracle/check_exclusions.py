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
import os

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import iregexp_diff
import iterate_diff
import perl_diff
import posix_diff
import replace_diff
import script_run_diff
import syntax_diff
import vim_diff
import window_diff


# Each block is (label, call, rows). A row is (reason, args, want), and
# `call` is handed the args.
BLOCKS = []


def block(label, call, rows):
    BLOCKS.append((label, call, rows))


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
# --------------------------------------------------------------------
MARKER = replace_diff.SURROGATE_PROBE

block("replace_diff.heal_split_pairs",
    lambda theirs, ours:
        replace_diff.heal_split_pairs(theirs, MARKER) == ours,
    [
    ("node split a pair and nothing else differs",
     ("a" + MARKER + "b\ud83d" + MARKER + "\ude00c" + MARKER,
      "a" + MARKER + "b\U0001F600c" + MARKER), True),
    ("no pair was split and the two still agree",
     ("ab" + MARKER + "\U0001F600" + MARKER + "c",
      "ab" + MARKER + "\U0001F600" + MARKER + "c"), True),
    ("node split a pair and this library also missed one elsewhere",
     ("a" + MARKER + "b\ud83d" + MARKER + "\ude00c" + MARKER,
      "a" + MARKER + "b\U0001F600c"), False),
    ("node split a pair and this library wrote one node did not",
     ("a" + MARKER + "b\ud83d" + MARKER + "\ude00c" + MARKER,
      "a" + MARKER + "b\U0001F600" + MARKER + "c" + MARKER), False),
    ("no pair was split and the answers differ anyway",
     ("a" + MARKER + "b\U0001F600c" + MARKER,
      "a" + MARKER + "b\U0001F600c"), False),
])

block("replace_diff.splits_a_surrogate_pair",
    lambda text: replace_diff.splits_a_surrogate_pair(text),
    [
    ("a high half left on its own", ("ab\ud83dxc",), True),
    ("a low half left on its own", ("abx\ude00c",), True),
    ("the halves still together", ("ab\U0001F600c",), False),
    ("no astral character at all", ("abc",), False),
])


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
# vim: the forward backreference an unbounded lookbehind makes legal.
# --------------------------------------------------------------------
block("vim_diff.is_forward_reference_artifact",
    lambda pattern, them, us:
        vim_diff.is_forward_reference_artifact(pattern, them, us),
    [
    ("the positive lookbehind vim compiles it in front of",
     (r"\1\(a\)\@<=", "match 1:1 \"a\"", "compile"), True),
    ("the negative one",
     (r"\1\(a\)\@<!", "match 0:0", "compile"), True),
    ("the byte-bounded lookbehind, which vim refuses too",
     (r"\1\(a\)\@2<=", "compile", "compile"), False),
    ("a lookahead, which vim refuses",
     (r"\1\(a\)\@=", "compile", "compile"), False),
    ("an atomic group, which vim refuses",
     (r"\1\(a\)\@>", "compile", "compile"), False),
    ("a plain group, which vim refuses",
     (r"\1\(a\)", "compile", "compile"), False),
    ("a lookbehind with no reference in front of it",
     (r"\(a\)\@<=a", "match 1:2 \"a\"", "match 1:2 \"a\""), False),
    ("this library compiled it too, so there is nothing to excuse",
     (r"\1\(a\)\@<=", "match 1:1 \"a\"", "match 1:1 \"a\""), False),
])

block("vim_diff.marks_in_a_lookbehind_and_after_it",
    lambda pattern: vim_diff.marks_in_a_lookbehind_and_after_it(pattern),
    [
    ("a mark in the lookbehind and one in the operator after it",
     (r"\(a\zeb\)\@<=\(a\zeb\)\@>",), True),
    ("the `\\zs` spelling of the same shape",
     (r"\(a\zsb\)\@<=\(a\zsb\)\@>",), True),
    ("only the lookbehind's mark, which every engine agrees about",
     (r"\(a\zeb\)\@<=\(ab\)\@>",), False),
    ("only the operator's mark, which is item 9's ordinary case",
     (r"\(ab\)\@<=\(a\zeb\)\@>",), False),
    ("the second mark not inside an operator, where the engines differ "
     "and the gate settles it by asking",
     (r"\(a\zeb\)\@<=a\zeb",), False),
    ("a negative lookbehind, where all three agree",
     (r"\(a\zeb\)\@<!a\zeb",), False),
    ("a lookahead rather than a lookbehind",
     (r"\(a\zeb\)\@=\(a\zeb\)\@>",), False),
    ("no mark at all", (r"\(ab\)\@<=\(ab\)\@>",), False),
])

block("vim_diff.lookbehind_with_backreference",
    lambda pattern: vim_diff.lookbehind_with_backreference(pattern),
    [
    ("the span row, an unbounded lookbehind",
     (r"\(a\)\@<=\(a\)\1\l",), True),
    ("the byte-bounded spelling, which a substring test misses",
     (r"\(a\)\@2<=\(a\)\1",), True),
    ("the negative lookbehind",
     (r"\(a\)\@<!\1",), True),
    ("the replacement row, where the gate next door meets it",
     (r"\Ma\%[\d\w]\(\(a\)\@=a\)\@<=\(a\)\1",), True),
    ("a lookahead rather than a lookbehind",
     (r"\(a\)\@=\1",), False),
    ("a lookbehind with nothing referring back",
     (r"\(a\)\@<=x",), False),
    ("neither",
     (r"\(a\)x",), False),
])


# --------------------------------------------------------------------
# vim: two constructs that mean nothing there and the obvious thing in
# every neighbouring spelling.
# --------------------------------------------------------------------
block("vim_diff.is_very_magic_line_start_repeat",
    lambda pattern, them, us:
        vim_diff.is_very_magic_line_start_repeat(pattern, them, us),
    [
    ("the one spelling vim finds nothing in",
     (r"\v\_^*", "nomatch", "match 0:0"), True),
    ("the magic level, where vim matches the empty string",
     (r"\m\_^*", "match 0:0", "match 0:0"), False),
    ("the end-of-line form, where vim matches it",
     (r"\v\_$*", "match 0:0", "match 0:0"), False),
    ("the bound spelling, where vim matches it",
     (r"\v\_^\{0,1}", "match 0:0", "match 0:0"), False),
    ("a group around it, where vim matches it",
     (r"\v(\_^)*", "match 0:0", "match 0:0"), False),
    ("vim found something, so it is not the empty-repeat question",
     (r"\v\_^*", "match 0:1", "match 0:0"), False),
])

block("vim_diff.is_line_number_star",
    lambda pattern, them, us:
        vim_diff.is_line_number_star(pattern, them, us),
    [
    ("the bare star vim finds nothing in",
     (r"\%23l*", "nomatch", "match 0:0"), True),
    ("another branch winning because this one offers nothing",
     (r"\%23l*\|\x", "match 0:1", "match 0:0"), True),
    ("a marker between, which vim compiles and this library refuses",
     (r"\%23l\v*", "nomatch", "compile"), True),
    ("the `\\{}` spelling, where vim matches the empty string",
     (r"\%23l\{}", "match 0:0", "match 0:0"), False),
    ("the lazy bound",
     (r"\%23l\{-}", "match 0:0", "match 0:0"), False),
    ("a group around it",
     (r"\(\%23l\)*", "match 0:0", "match 0:0"), False),
    ("the very magic spelling, which has no `\\%`",
     (r"\v%23l*", "match 0:0", "match 0:0"), False),
    ("a column marker rather than a line one",
     (r"\%23c*", "nomatch", "match 0:0"), False),
    ("vim found nothing and the rest of the pattern matched around it",
     (r"\%23l*a", "nomatch", "match 0:1"), True),
    ("vim matched and ours is a span of its own, not the empty repeat",
     (r"\%23l*", "match 0:1", "match 0:1"), False),
    ("neither side matched, so there is nothing to excuse",
     (r"\%23l*", "nomatch", "nomatch"), False),
])

block("vim_diff.is_leading_star_artifact",
    lambda pattern, them, us:
        vim_diff.is_leading_star_artifact(pattern, them, us),
    [
    ("a marker between the caret and the star, which vim refuses",
     (r"^\m*", "compile", "match 0:1"), True),
    ("the `\\%(` spelling, which vim refuses",
     (r"\%(*\)", "compile", "match 0:1"), True),
    ("the caret without the marker, which vim matches",
     (r"^*", "match 0:1", "match 0:1"), False),
    ("the capturing spelling, which vim matches",
     (r"\(*\)", "match 0:1", "match 0:1"), False),
    ("a star with no caret and no group",
     (r"*a", "match 0:2", "match 0:2"), False),
    ("this library refused it too, so there is nothing to excuse",
     (r"^\m*", "compile", "compile"), False),
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
block("vim_diff.is_abandoned_mark_artifact",
    lambda pattern, them, us:
        vim_diff.is_abandoned_mark_artifact(pattern, them, us),
    [
    ("the empty match wearing the end a dead branch left",
     (r"\(a\zeb\)\@>\d\|\&", "match 0:2", "match 0:0"), True),
    ("the `\\zs` spelling from the other side",
     (r"\(a\zsb\)\@=\d\|\&", "match 1:1", "match 0:0"), True),
    ("without the mark, which is the row above this one",
     (r"\(ab\)\@>\d\|\&", "match 0:0", "match 0:0"), False),
    ("without the postfix operator, where the mark alone does nothing",
     (r"a\zeb\d\|\&", "match 0:0", "match 0:0"), False),
    ("without the alternation, where neither engine matches",
     (r"\(a\zeb\)\@>\d", "match 0:2", "match 0:0"), False),
    ("this library reported a span of its own",
     (r"\(a\zeb\)\@>\d\|\&", "match 0:2", "match 0:1"), False),
])

block("vim_diff.is_postfix_capture_artifact",
    lambda pattern, them, us:
        vim_diff.is_postfix_capture_artifact(pattern, them, us),
    [
    ("vim lost a capture, which is allowed for any `\\@` operator",
     (r"\(a\)\(a\)\@=a\{2,}", 'match 0:3 "" "a"', 'match 0:3 "a" "a"'),
     True),
    ("vim kept one an atomic group had written",
     (r"\(a\)\@>x\|\A", 'match 0:1 "a"', "match 0:1"), True),
    ("vim kept one an abandoned branch had written",
     (r"\(a\)\@=a$\|b", 'match 0:2 "a"', "match 0:2"), True),
    ("vim kept one where the branch died at a character",
     (r"\(a\)\@=ax", 'match 0:2 "a"', "match 0:2"), False),
    ("no postfix operator at all",
     (r"\(a\)x\|b", 'match 0:1 "a"', "match 0:1"), False),
    ("the whole match differs, which is not a capture question",
     (r"\(a\)\@>x\|\A", 'match 0:2 "a"', "match 0:1"), False),
    ("two groups that differ and neither is empty",
     (r"\(a\)\@>x\|\A", 'match 0:1 "a"', 'match 0:1 "b"'), False),
])

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
# vim, through a substitution: the same writes seen as text.
# --------------------------------------------------------------------
block("replace_diff.is_abandoned_path_artifact",
    lambda pattern, template:
        replace_diff.is_abandoned_path_artifact(pattern, template),
    [
    ("the negative-lookbehind row this was written for",
     (r"\%>2v\(a\(b\)\@=\)\@<!\(a\zsb\)\@=\V\m", r"\n\2\U\&"), True),
    ("an alternation and a template that names a group",
     (r"\(a\)\@>x\|\A", r"\1"), True),
    ("an alternation and a mark, where the span is what differs",
     (r"\(a\zeb\)\@>\d\|\&", "X"), True),
    ("no postfix operator",
     (r"\(a\)x\|b", r"\1"), False),
    ("no path that can be abandoned",
     (r"\(a\)\@>x", r"\1"), False),
    ("neither a group in the template nor a mark in the pattern",
     (r"\(a\)\@>x\|\A", "X"), False),
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


def main():
    failures = 0
    checked = 0
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
