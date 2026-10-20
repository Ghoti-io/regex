#!/usr/bin/env python3
"""Reproducible JDK 25/27 semantic matrix; never consults libregex.

Use --record only when intentionally refreshing a pin's answers. The normal
strict gate compares every answer with the committed version-specific file.
"""
import argparse
import json
import os
import subprocess
import sys

import oracle_env
import wtf8 as wtf8_codec
from probe import CASES as SHARED

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "tests", "data", "probe")


def case(area, pattern, subject="", flags="", mode="first", argument=""):
    return dict(area=area, pattern=pattern, subject=subject, flags=flags,
                mode=mode, argument=argument)


CASES = [case("shared", p, s, f) for p, f, s, _ in SHARED] + [
    case("control", "a", "a"), case("control", "a", "b"),
    case("control", "(", ""), case("control", "(a)?b", "b"),
    case("control", ".", "😀"),
    case("control", "a", "😀a", mode="find-from", argument="4"),
    case("flags", "a", "A", "i"), case("flags", "é", "É", "iu"),
    case("flags", "(?U)\\w", "é"), case("flags", "\\w", "é"),
    case("flags", "(?d).", "\r"), case("flags", "(?s).", "\n"),
    case("flags", "(?m)^b", "a\nb"), case("flags", "(?x)a # hi\n b", "ab"),
    case("flags", "(?i:a)b", "Ab"), case("flags", "(?i:a)b", "AB"),
    case("flags", "a", "A", "l"), case("flags", "é", "e\u0301", "c"),
    case("classes", "[a&&[a-z]]", "a"), case("classes", "[a&&[^a]]", "a"),
    case("classes", "[[a-z][0-9]]", "5"), case("classes", "[a-z&&[^aeiou]]", "b"),
    case("classes", "[a-z&&[^aeiou]]", "a"), case("classes", "[[:alpha:]]", "a"),
    case("classes", "[a-\\d]", "a"), case("classes", "[]]", "]"),
    case("classes", "[-a]", "-"), case("classes", "[a-]", "-"),
    case("lookbehind", "(?<=ab)c", "abc"),
    case("lookbehind", "(?<=a|ab)c", "abc"),
    case("lookbehind", "(?<=a{1,3})b", "aaab"),
    case("lookbehind", "(?<=a+)b", "aab"),
    case("lookbehind", "(?<=(a)\\1)b", "aab"),
    case("captures", "((a)|b)+", "ab"),
    case("captures", "(a)?b\\1", "b"),
    case("captures", "(?=(a))a", "a"),
    case("captures", "\\2(a)(b)", "ab"),
    case("captures", "(?<n>a)\\k<n>", "aa"),
    case("iteration", "", "ab", mode="all"),
    case("iteration", "a*", "ba", mode="all"),
    case("iteration", "\\G.", "abc", mode="all"),
    case("iteration", "a", "ba", mode="find-from", argument="1"),
    case("iteration", "a", "a", mode="matches"),
    case("iteration", "a", "ab", mode="looking-at"),
    case("escapes", "\\Q[a]+\\E", "[a]+"),
    case("escapes", "\\x{1F600}", "😀"),
    case("escapes", "\\u00E9", "é"),
    case("escapes", "\\0101", "A"),
    case("escapes", "\\cA", "\u0001"),
    case("escapes", "\\h", "\u00a0"),
    case("escapes", "\\v", "\n"),
    case("escapes", "\\R", "\r\n"),
    case("escapes", "\\X", "e\u0301"),
    case("escapes", "\\b{g}", "e\u0301"),
    case("unicode-sensitive", "\\p{IsGreek}", "Ω"),
    case("unicode", "\\p{InGreek}", "Ω"),
    case("unicode", "\\p{javaLowerCase}", "é"),
    case("unicode", "\\p{NotAProperty}", "a"),
    case("unicode", "\\d", "١"),
    case("unicode", "(?U)\\d", "١"),
    case("unicode", "(?iu)k", "K"),
    case("replacement", "(a)", "a", mode="replace-all", argument="$1$1"),
    case("replacement", "(?<x>a)", "a", mode="replace-first", argument="${x}"),
    case("replacement", "(a)?b", "b", mode="replace-all", argument="<$1>"),
    case("replacement", "(a)", "a", mode="replace-all", argument="$2"),
    case("replacement", "a", "a", mode="replace-all", argument="\\$"),
    case("replacement", "a", "a", mode="replace-all", argument="$"),
    case("replacement", "", "é", mode="replace-all", argument="X"),
    case("split", ",", "a,b,", mode="split", argument="0"),
    case("split", ",", "a,b,", mode="split", argument="-1"),
    case("split", ",", "a,b,", mode="split", argument="2"),
    case("split", "(?=a)", "abc", mode="split", argument="0"),
    case("split", "(,)", "a,b", mode="split", argument="0"),
    case("split", ",", "", mode="split", argument="-1"),
    case("refusal", "(?R)", "a"),
    case("refusal", "(?(1)a|b)", "a"),
    case("refusal", "(*SKIP)", "a"),
    case("refusal", "\\g{1}", "a"),
    case("refusal", "(?P<n>a)", "a"),
    case("lines", ".", "\n"), case("lines", ".", "\u0085"),
    case("lines", ".", "\u2028"), case("lines", ".", "\u2029"),
    case("lines", "\\R", "\r\n"), case("lines", "\\R", "\u0085"),
    case("lines", "(?d).", "\u0085"),
    case("lines", "(?m)^b", "a\rb"),
    case("lines", "(?dm)^b", "a\rb"),
    case("flags", "(?x)[a b]", " "),
    case("flags", "(?x)[a b]", "b"),
    case("flags", "(?-i:A)", "a", "i"),
    case("replacement", "(a)(b)", "ab", mode="replace-all", argument="$12"),
    case("replacement", "(a)(b)", "ab", mode="replace-all", argument="${missing}"),
    case("split", "", "ab", mode="split", argument="-1"),
    case("split", "$", "ab", mode="split", argument="-1"),
    case("split", ",", ",a,", mode="split", argument="0"),
    case("split", ",", ",a,", mode="split", argument="-1"),
    case("control", "a+", mode="compile"),
    case("classes", "[[:alpha:]]", "Z"),
    case("flags", "a+", "a+", "l"),
    case("iteration", "\\Ga", "aba", mode="all"),
    case("replacement", "a", "aa", mode="replace-first", argument="<$0>"),
    case("replacement", "a", "a", mode="replace-all", argument="<$0>"),
    case("flags", ".", "\r", "d"),
    case("flags", "^b", "a\nb", "m"),
    case("flags", ".", "\n", "s"),
    case("flags", "a # hi\n b", "ab", "x"),
    case("flags", "\\w", "é", "U"),
    case("control", "😀(", mode="compile"),
    case("replacement", "a", "a", mode="replace-first", argument="$"),
    # Flag interactions and Character predicates, with separate JDK 25/27
    # answers in the two committed records.
    case("flags", "k", "K", "i"),
    case("flags", "k", "K", "iu"),
    case("flags", "k", "K", "iU"),
    case("flags", "k", "K", "U"),
    case("flags", "(?-u:k)", "K", "iu"),
    case("flags", "(?u:k)", "K", "i"),
    case("flags", "\\d", "١"),
    case("flags", "\\d", "١", "U"),
    case("flags", "[\\w]", "é"),
    case("flags", "[\\w]", "é", "U"),
    case("flags", "\\bé", "é"),
    case("flags", "\\Bé", "é"),
    case("flags", "\\bé", "é", "U"),
    case("flags", "\\Bé", "é", "U"),
    case("lines", "a.b", "a\rb", "d"),
    case("lines", "^b", "a\rb", "md"),
    case("lines", "^\\n", "\r\n", "m"),
    case("lines", "(?d:a.\\nb)", "a\r\nb"),
    case("lines", "(?-d:a.\\nb)", "a\r\nb", "d"),
    case("flags", "a b", "ab", "x"),
    case("flags", "[a b]", "b", "x"),
    case("unicode-sensitive", "\\p{IsGreek}", "ʹ"),
    case("unicode-sensitive", "\\p{sc=Greek}", "ʹ"),
    case("unicode-sensitive", "\\p{sc=Greek}", "ἀ"),
    case("unicode-sensitive", "\\p{InGreek}", "ʹ"),
    case("unicode-sensitive", "\\p{javaWhitespace}", "\u001c"),
    case("unicode-sensitive", "\\p{javaWhitespace}", "\u00a0"),
    case("unicode-sensitive", "\\p{javaWhitespace}", "\u0085"),
    case("unicode-sensitive", "\\p{javaWhitespace}", "\u2003"),
    case("unicode-sensitive", "\\p{javaWhitespace}", "\u2007"),
    case("unicode-sensitive", "\\p{javaWhitespace}", "\u202f"),
    case("flags", "(k)", "K", "iu"),
    case("flags", "(a).(b)", "a\rb", "d"),
    case("unicode-sensitive", "(\\p{javaWhitespace})", "\u2003"),
    case("unicode-sensitive", "(?U)(\\w+)", "é"),
    case("unicode-sensitive", "\\p{javaLowerCase}", "ʕ"),
    case("flags", "𐐀", "𐐨", "i"),
    case("flags", "𐐀", "𐐨", "iu"),
    case("flags", "\\w", "𐐨"),
    case("flags", "\\w", "𐐨", "U"),
    case("unicode-sensitive", "\\p{javaLowerCase}", "𐐨"),
    case("unicode-sensitive", "\\p{javaDigit}", "𝟎"),
    # Search-state matrix. Arguments and all answer spans are UTF-8 bytes.
    case("search", "", "ab", mode="all"),
    case("search", "a*", "baac", mode="all"),
    case("search", "a*", "a", mode="all"),
    case("search", "\\Ga*", "baac", mode="all"),
    case("search", "\\Ga*", "aab", mode="all"),
    case("search", "(?:\\Ga|b)", "cba", mode="all"),
    case("search", "(?:\\Ga|b)", "aba", mode="all"),
    case("search", "(?=(a))a|(b)", "ab", mode="all"),
    case("search", "((a)|b)+", "ab", mode="all"),
    case("search", "(a*)*", "bc", mode="all"),
    case("search", "(?<=(a+))b", "aaabaaab", mode="all"),
    case("search", "(?<=a{1,3})(b)", "aabaaab", mode="all"),
    case("search", "(?=(a))(a)", "aa", mode="all"),
    case("search", "(?<=a(?=(b)))b", "abab", mode="all"),
    case("search", "(?<!a{2,})b", "ab aab aaab b", mode="all"),
    case("search", "\\Ga*", "cbaa", mode="all-from", argument="1"),
    case("search", "", "😀é", mode="all"),
    case("search", ".", "😀é", mode="all"),
    case("search", "(?<=😀)(é)", "😀é", mode="first"),
    case("search", "\\G.", "abc", mode="find-from", argument="1"),
    case("search", "(?<=😀)a", "😀a", mode="find-from", argument="4"),
    case("search", "a", "ba", mode="find-from", argument="2"),
    case("search", "a+", "aa", mode="matches"),
    case("search", "a", "aa", mode="matches"),
    case("search", "a", "ab", mode="looking-at"),
    case("search", "a", "ba", mode="looking-at"),
    case("search", "(?=a)(a*)", "a", mode="matches"),
    case("search", "(?:)", "", mode="matches"),
    case("search", "(?=a)a", "a", mode="matches"),
    case("search", "(?=a)(a*)", "ab", mode="looking-at"),
    case("search", "(?=a)", "abc", mode="looking-at"),
    case("search", "(?<=a)b", "ab", mode="looking-at"),
    # Region and bounds are measured, then classified separately from the
    # current C search window (which has intentionally different semantics).
    case("region", "^a", "ba", mode="region-first", argument="1,2,0,1"),
    case("region", "^a", "ba", mode="region-first", argument="1,2,0,0"),
    case("region", "(?<=b)a", "ba", mode="region-first", argument="1,2,0,1"),
    case("region", "(?<=b)a", "ba", mode="region-first", argument="1,2,1,1"),
    case("region", "a$", "ab", mode="region-first", argument="0,1,0,1"),
    case("region", "a", "ba", mode="region-matches", argument="1,2,0,1"),
    case("region", "a", "ba", mode="region-looking-at", argument="1,2,0,1"),
    # Replacement grammar and progression, with the JDK 25 record fixed as
    # the baseline and JDK 27 held in a separate file.
    case("replacement", "(a)", "a", mode="replace-all", argument="$0/$1/$12"),
    case("replacement", "(a)(b)", "ab", mode="replace-all", argument="$12"),
    case("replacement", "(a)", "a", mode="replace-all", argument="$01"),
    case("replacement", "(a)", "a", mode="replace-all", argument="$00"),
    case("replacement", "(a)", "a", mode="replace-all", argument="$123"),
    case("replacement", "(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)",
         "abcdefghijkl", mode="replace-all", argument="$123"),
    case("replacement", "(?<x>a)", "a", mode="replace-all", argument="${x}$1"),
    case("replacement", "(a)?b", "b", mode="replace-all", argument="<$1>"),
    case("replacement", "a", "a", mode="replace-all", argument="\\$\\\\\\q"),
    case("replacement", "a", "a", mode="replace-all", argument="\\"),
    case("replacement", "a", "a", mode="replace-all", argument="$"),
    case("replacement", "a", "a", mode="replace-all", argument="$x"),
    case("replacement", "(a)", "a", mode="replace-all", argument="$2"),
    case("replacement", "(a)", "a", mode="replace-all", argument="${missing}"),
    case("replacement", "(a)", "a", mode="replace-all", argument="${}"),
    case("replacement", "(a)", "a", mode="replace-all", argument="${1}"),
    case("replacement", "(?<x>a)", "a", mode="replace-all", argument="${x\0}"),
    case("replacement", "(a)", "a", mode="replace-all", argument="${x"),
    case("replacement", "a", "bbb", mode="replace-all", argument="$"),
    case("replacement", "a", "bbb", mode="replace-first", argument="$"),
    case("replacement", "a", "bbb", mode="replace-all", argument="\\"),
    case("replacement", "a", "bbb", mode="replace-first", argument="\\"),
    case("replacement", "a", "bbb", mode="replace-all", argument="${x"),
    case("replacement", "a", "bbb", mode="replace-first", argument="${}"),
    case("replacement", "a", "aa", mode="replace-first", argument="<$0>"),
    case("replacement", "a", "aa", mode="replace-all", argument="<$0>"),
    case("replacement", "", "é", mode="replace-all", argument="X"),
    case("replacement", "(é)", "é", mode="replace-all", argument="<$1>"),
    case("replacement", "(😀)", "😀😀", mode="replace-first", argument="<$1>"),
    case("replacement", "(😀)", "😀😀", mode="replace-all", argument="<$1>"),
    # Java finds an empty match between the UTF-16 halves of this scalar and
    # writes an unpaired-surrogate result; the byte API cannot represent it.
    case("replacement", "", "😀", mode="replace-all", argument="X"),
    case("replacement", "😀", "😀", mode="replace-all", argument="\\😀"),
    case("replacement", "a", "a\0a", mode="replace-all", argument="X"),
    case("replacement", "a", "a", mode="replace-all-literal", argument="$0\\q"),
    case("replacement", "a", "aa", mode="replace-first-literal", argument="$0\\q"),
    # Signed split limits and boundary combinations. Keep these after the
    # existing inventory so prior pinned case numbers remain stable.
    case("split", ",", "a,b,", mode="split", argument="1"),
    case("split", ",", "a,b,", mode="split", argument="3"),
    case("split", ",", "a,b,", mode="split", argument="-2147483648"),
    case("split", ",", "a,b,", mode="split", argument="2147483647"),
    case("split", ",", "", mode="split", argument="0"),
    case("split", "", "", mode="split", argument="0"),
    case("split", "", "", mode="split", argument="-1"),
    case("split", "", "ab", mode="split", argument="0"),
    case("split", "", "ab", mode="split", argument="2"),
    case("split", "(?=a)", "abc", mode="split", argument="-1"),
    case("split", "(?=b)", "abc", mode="split", argument="-1"),
    case("split", "(?=b)", "abc", mode="split", argument="2"),
    case("split", "$", "ab", mode="split", argument="0"),
    case("split", "$", "ab", mode="split", argument="2"),
    case("split", ",", ",a,", mode="split", argument="2"),
    case("split", "(,)", "a,b,", mode="split", argument="-1"),
    case("split", ",", "a,,", mode="split", argument="0"),
    case("split", ",", "a,,", mode="split", argument="-1"),
    case("split", "", "😀", mode="split", argument="-1"),
    # Integer.parseInt accepts signs and leading zeroes, but rejects spaces,
    # malformed digits, and values outside the signed 32-bit domain.
    case("split-limit-valid", ",", "a,b", mode="split", argument="+1"),
    case("split-limit-valid", ",", "a,b", mode="split", argument="01"),
    case("split-limit-invalid", ",", "a,b", mode="split", argument=""),
    case("split-limit-invalid", ",", "a,b", mode="split", argument="-"),
    case("split-limit-invalid", ",", "a,b", mode="split", argument="+"),
    case("split-limit-invalid", ",", "a,b", mode="split", argument=" 1"),
    case("split-limit-invalid", ",", "a,b", mode="split", argument="1 "),
    case("split-limit-invalid", ",", "a,b", mode="split", argument="1x"),
    case("split-limit-invalid", ",", "a,b", mode="split", argument="2147483648"),
    case("split-limit-invalid", ",", "a,b", mode="split", argument="-2147483649"),
]

# Generated interaction inventory. Append only: the preceding pinned case IDs
# are cited by the semantic ledger and must remain stable. Keep each cross
# product small enough that a failing case prints as a useful reproducer.
GENERATED_INVENTORY_REVISION = "2026-10-20d"
CASES += [case("generated-syntax", pattern, mode="compile") for pattern in (
    "a", "(a|b)+", "[a&&[a-z]]", "\\Q[a]+\\E", "(?<n>a)",
    "(?R)", "(?P<n>a)", "[a-\\d]", "\\g{1}")]
CASES += [case("generated-match", pattern, subject)
          for pattern in ("(a|b)+", "(?i:a)b", "\\d+", "[a&&[a-z]]",
                          "(?<=a)b", "(?=(a))a")
          for subject in ("ab", "aab", "b", "1", "é")]
CASES += [case("generated-search", pattern, subject, mode="all")
          for pattern in ("a*", "\\G.", "(?=a)", "(a)?b")
          for subject in ("ab", "baa", "")]
CASES += [case("generated-replacement", pattern, subject,
               mode="replace-all", argument=template)
          for pattern, template in (("(a)", "<$1>"),
                                    ("(?<n>a)", "<${n}>"),
                                    ("a", "\\$"))
          for subject in ("a", "aa", "ba")]
CASES += [case("generated-split", pattern, subject, mode="split",
               argument=str(limit))
          for pattern in (",", "(?=a)", "")
          for subject in ("a,b,", "aa", "")
          for limit in (-1, 0, 2)]


def wtf8(text):
    """WTF-8 bytes: a surrogate pair as the four-byte scalar, an unpaired
    surrogate as its own three bytes.

    One line, because the encoder moved to `wtf8.py` when the ECMAScript
    differential needed the same one and reproduced the CESU-8 mistake
    writing its own. The name stays here because the callers use it.
    """
    return wtf8_codec.encode(text)


# What the reference does with an unpaired UTF-16 surrogate.
#
# These are the cases that decided the lone-surrogate option's shape, and
# they are here rather than in a note because the answers are the reference
# for an implementation that does not exist yet. `\p{Cs}` is the one that
# matters most: the JDK matches it and this library compiles it to an empty
# set, so without this row the first surrogate a decoder accepted would
# produce a silent no-match.
#
# `w:first` puts them through the driver's WTF-8 transport; strict UTF-8
# cannot carry the subject at all. A lone surrogate in a Python `str` is
# legal, which is why the inventory can hold them.
LONE_HIGH = "\ud800"
# Spelled as the scalar it is, not as the two code units that make it.
# `json.loads` *combines* two consecutive `\uXXXX` escapes into one astral
# character, so an inventory holding the pair as units does not survive its
# own round trip - recorded as `\ud800\udc61`, read back as `\U00010061`,
# and `--record` then fails its own "case inventory changed" check on the
# next run. Both spellings encode to the same four WTF-8 bytes, so only the
# inventory cares, and this is the spelling that is stable in it.
SURROGATE_PAIR = "\U00010061"
CASES += [case("surrogate", pattern, subject, flags, "w:first")
    for pattern, subject, flags in (
        (".", LONE_HIGH, ""),             # one position, three bytes
        (".", SURROGATE_PAIR, ""),        # one position, four bytes
        (".", LONE_HIGH, "iu"),           # and nothing here folds
        ("\\w", LONE_HIGH, ""),           # not a word character
        ("\\W", LONE_HIGH, ""),           # and the complement says so
        ("\\p{Cs}", LONE_HIGH, ""),       # category Cs: the JDK matches
        ("\\p{Cn}", LONE_HIGH, ""),       # and they are not unassigned
        ("\\p{L}", LONE_HIGH, ""),        # not letters
        ("\\p{all}", LONE_HIGH, ""),      # Java's `all` includes them
        ("[\\uD800]", LONE_HIGH, ""),     # named by escape in a class
        ("[\\uD7FF-\\uE000]", LONE_HIGH, ""),  # a range spanning the block
        ("\\uD800", LONE_HIGH, ""),       # a bare escape
        ("[^a]", LONE_HIGH, ""),          # a negated class
        ("[^a]", SURROGATE_PAIR, ""),     # and over a pair
    )]

# The counted closure, which is one rule the JDK applies where every other
# dialect in this family applies two. An unescaped `{` always begins one
# there - `a{foo}` is "Illegal repetition" where perl, pcre2 and CPython all
# match the text "a{foo}" - and a closure with no atom to its left is
# accepted and repeats nothing: `{3}` matches empty, `a{3}{4}` matches three
# rather than twelve, and `a{3}*` is still an error.
#
# Asked as compile *and* match rows, because the two halves fail
# differently: a parser that refused `{3}` would be caught by the verdict
# alone, while one that nested `a{3}{4}` into twelve would compile and give
# the wrong span. The subject is long enough for either reading to show.
CASES += [case("brace", pattern, mode="compile") for pattern in (
    # Malformed, and each is a syntax error in the JDK and a literal brace
    # everywhere else here.
    "a{^InGreek}", "a{foo}", "a{", "a{1", "a{1,", "a{,3}", "a{}", "a{ 1}",
    "a{1,2,3}", "a{-1}", "{foo}", "{", "{}", "{,3}",
    # Well formed, including where there is nothing to repeat.
    "a{1}", "a{1,}", "a{1,2}", "{3}", "{3,}", "(?:{3})", "a|{3}", "(){3}",
    "^{3}", "a${3}", "(?=a){3}", "\\b{3}", "\\B{3}", "\\A{3}", "\\b{3,}",
    "a*{3}", "a+{3}", "a?{3}", "a{3}{4}", "a{3}{4}{5}", "(?:a){3}{4}",
    # Where a brace is a literal even in the JDK.
    "[{3}]", "\\{3}", "[a{]", "a\\{foo}", "\\Q{3}\\E",
    # And the orderings and suffixes the rule must not swallow: `a{3}*` is
    # "Dangling meta character" there, while `a{3}+` and `a{3}?` are the
    # possessive and lazy suffixes.
    "a**", "a{3}*", "a{3}+", "a{3}?", "a{2,1}",
)]
CASES += [case("brace", pattern, subject)
    for pattern, subject in (
        ("{3}", "aaa"), ("{3}", ""), ("{3}a", "aaa"),
        ("a{3}{4}", "aaaaaaaaaaaa"),      # three, not twelve
        ("x{2}{3}", "xxxxxx"),            # two, not six
        ("a*{3}", "aaaa"), ("a+{3}", "aaaa"), ("a?{3}", "aaaa"),
        ("^{3}", "aaa"), ("(?=a){3}", "aaa"), ("\\b{3}", "aaa"),
        ("(){3}", ""), ("{3}{4}", "aaa"), ("a{1,2}{3}", "aaa"),
        ("\\{3}", "{3}"), ("[{3}]", "{"),
    )]

# The grapheme boundary, where the JDK and perl answer the ends of an *empty*
# subject differently. UAX #29 breaks at the start of text and at the end of
# text and does not say which applies when they are the same offset, so this
# is a corner two conforming implementations may split on - and they do:
# `\b{g}` against "" is 0-0 here and no match in perl 5.44.
#
# `\B{g}` is in the list because the JDK has no negated bound at all, and the
# *message* is what says so: "Illegal repetition" rather than an unknown bound
# type, which means the `{g}` went to the counted-closure reader. `\B{3}`
# compiling is the other half of that.
CASES += [case("grapheme-bound", pattern, subject, "", "all")
    for pattern, subject in (
        ("\\b{g}", ""),            # the empty subject: a boundary at 0
        ("\\b{g}", "a"),
        ("\\b{g}", "ab"),
        ("\\b{g}", "e\u0301"),     # one cluster, so 0 and 3
        ("\\b{g}", "\r\n"),        # CRLF is one cluster too
        ("a\\b{g}", "ab"),
        ("\\b{g}a", "a"),
        ("\\b{g}$", "a"),
        ("^\\b{g}$", ""),
        ("\\b{g}\\b{g}", ""),      # twice over, still the one position
        ("()\\b{g}", ""),
        ("\\A\\b{g}\\z", ""),
        ("\\b{g}{3}", ""),          # a closure on the bound, not a name
        ("\\b", ""),                # and the word boundary, which has none
        ("\\B", ""),
        ("\\B{g}", "a"),            # refused: no negated bound
        ("\\B{3}", "aaa"),          # while a closure after `\B` is fine
        ("\\b{gcb}", "a"),          # perl's spelling; not Java's
        ("\\b{wb}", "a"),
    )]

# One row per profile field that no other case here discriminates.
#
# The audit behind this: GRX_Profile has 41 fields, Java's entry assigns 19
# of them, and the other 22 take the zero value. A zero that is the measured
# answer and a zero that nobody decided look identical in the table, so each
# of those fields needs a case whose answer would *change* if the value
# were different. 28 of the 29 confirmed the default; the twenty-ninth found
# a defect - `(?U)\p{Graph}` matches U+E000 on JDK 25 and this library said
# no, because posix_graph_takes_private_use had been left at the value for
# "a dialect with no reference on this machine".
#
# Four fields have no case because no Java pattern can reach them:
# ascii_classes_fold_ascii wants a narrowing ASCII flag Java does not have,
# and the three posix_case_classes_* fields describe vim's split between its
# own case tables and the C library's.
CASES += [case("profile", pattern, subject) for pattern, subject in (
    # preference and submatch: leftmost-first, not leftmost-longest.
    ("a|ab", "ab"), ("(a|ab)(c|bcd)", "abcd"),
    # empty_loop: what a loop over an empty body does.
    ("(a*)*", ""), ("(a*)+", "b"),
    # negative_look: the captures a failed body made.
    ("(?!(a))b", "b"), ("(?!(a)|(b))c", "c"),
    # composing: a combining mark is its own character.
    ("^.$", "e\u0301"), ("^..$", "e\u0301"),
    # caret_after_final_newline.
    ("(?m)^$", "a\n"), ("(?m)^", "a\n"),
    # word_set: which categories `\w` gains under `U`.
    ("(?U)\\w", "\u00b2"), ("(?U)\\w", "\u0345"),
    ("(?U)\\w", "\u24b6"), ("(?U)\\w", "_"),
    # posix_classes_stay_ascii: they widen under `U` and are ASCII without.
    ("(?U)\\p{Alpha}", "\u00e9"), ("\\p{Alpha}", "\u00e9"),
    # posix_wide_general_category: which reading `Alpha` and `Alnum` take.
    ("(?U)\\p{Alpha}", "\u0345"), ("(?U)\\p{Alpha}", "\u24b6"),
    ("(?U)\\p{Alnum}", "\u2160"),
    # posix_graph_takes_private_use, and the rows that bound it: assigned-ness
    # is still excluded and the ASCII reading never reaches it.
    ("(?U)\\p{Graph}", "\ue000"), ("(?U)\\p{Print}", "\ue000"),
    ("(?U)\\p{Graph}", "\U000F0000"), ("\\p{Graph}", "\ue000"),
    ("(?U)\\p{Graph}", "\u0378"),
    # posix_graph_drops_invisibles.
    ("(?U)\\p{Graph}", "\u00ad"), ("(?U)\\p{Graph}", "\u200b"),
    ("(?U)\\p{Print}", "\u00ad"),
    # and the two `Z` rows that say which categories `Print` keeps.
    ("(?U)\\p{Graph}", "\u2028"), ("(?U)\\p{Print}", "\u2028"),
    # mongolian_separator_is_space.
    ("(?U)\\p{Space}", "\u180e"), ("(?U)\\s", "\u180e"),
    # caseless_widens_shorthands and caseless_leaves_boundary_narrow.
    ("(?i)\\w", "\u017f"), ("(?iu)\\w", "\u017f"), ("(?i)\\bs", "\u017fs"),
    # posix_classes_fold.
    ("(?i)\\p{Alpha}", "\u017f"), ("(?i)\\p{Lower}", "S"),
)]


def run(pin, rows):
    command = oracle_env.command(pin, ["java", "tools/oracle/JavaOracle.java"])
    lines = []
    for row in rows:
        # A `w:` mode's text fields are WTF-8, matching the driver. The
        # encoding and the offsets it reports are one decision, so they are
        # selected by one thing.
        encode = wtf8 if row["mode"].startswith("w:") \
            else (lambda s: s.encode("utf-8"))
        values = [row["mode"]] + [encode(row[k]).hex()
            for k in ("pattern", "flags", "subject", "argument")]
        lines.append("\t".join(values))
    finished = subprocess.run(command, input="\n".join(lines) + "\n",
                              capture_output=True, text=True, timeout=180)
    if finished.returncode:
        raise RuntimeError(f"driver failed: {finished.stderr}")
    answers = []
    for line in finished.stdout.splitlines():
        state, sep, payload = line.partition("\t")
        if not sep or state not in ("ok", "error"):
            raise RuntimeError(f"bad driver response: {line!r}")
        answer = json.loads(bytes.fromhex(payload).decode("utf-8"))
        if state == "error":
            answer["driver_error"] = True
        answers.append(answer)
    if len(answers) != len(rows):
        raise RuntimeError(f"{len(answers)} answers for {len(rows)} cases")
    return answers


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pin", choices=("java25", "java27"), required=True)
    parser.add_argument("--record", action="store_true")
    parser.add_argument("--wrong-expected-control", action="store_true")
    args = parser.parse_args()
    if args.record and (oracle_env.MODE != "container"
                        or os.environ.get("GHOTI_ORACLE_ALIAS", "").strip()):
        raise RuntimeError("recording requires pinned container mode without aliases")
    print(oracle_env.provenance([args.pin]), flush=True)
    answers = run(args.pin, CASES)
    path = os.path.join(DATA, args.pin + ".json")
    payload = {"cases": CASES, "answers": answers}
    if args.record:
        os.makedirs(DATA, exist_ok=True)
        with open(path, "w", encoding="utf-8") as out:
            # ensure_ascii, which this did not use until the surrogate area
            # arrived. An unpaired surrogate has no UTF-8 encoding, so
            # writing one into a UTF-8 file raises - and `--record` had
            # already truncated both pins' files before it did, which is how
            # this was found. JSON's own `\uXXXX` escape is the mechanism for
            # exactly this and round-trips the surrogate back out.
            #
            # The cost is that the twenty-one other non-ASCII characters in
            # here are escaped too, where they used to read as themselves.
            # Worth it over inventing a second escaping convention inside a
            # file that already has one.
            json.dump(payload, out, ensure_ascii=True, indent=2)
            out.write("\n")
        print(f"recorded {len(answers)} answers in {path}")
        return 0
    with open(path, encoding="utf-8") as source:
        expected = json.load(source)
    if args.wrong_expected_control:
        expected["answers"][0] = {"kind": "planted-wrong-answer"}
    if expected["cases"] != CASES:
        raise RuntimeError("case inventory changed; record and review both pins")
    wrong = [(i, actual, wanted) for i, (actual, wanted) in enumerate(
        zip(answers, expected["answers"])) if actual != wanted]
    if len(expected["answers"]) != len(answers):
        raise RuntimeError("answer count changed")
    for i, actual, wanted in wrong[:10]:
        print(f"case {i} {CASES[i]}: {actual} != {wanted}", file=sys.stderr)
    print(f"{len(answers)} cases, {len(answers)-len(wrong)} agree, {len(wrong)} differ")
    return bool(wrong)


if __name__ == "__main__":
    sys.exit(main())
