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

import iterate_diff
import perl_diff
import posix_diff
import replace_diff
import vim_diff


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


def main():
    failures = 0
    checked = 0
    for label, call, rows in BLOCKS:
        for reason, args, want in rows:
            checked += 1
            got = call(*args)
            if bool(got) != want:
                failures += 1
                print("%s: %s\n  %s\n  wanted %s, got %s"
                    % (label, reason, "  ".join(repr(a) for a in args),
                        want, bool(got)))
    print("check_exclusions: %d controls over %d predicates, %d failed"
        % (checked, len(BLOCKS), failures))
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
