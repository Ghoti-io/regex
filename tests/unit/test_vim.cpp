/**
 * @file
 *
 * The Vim front end: four grammars chosen inside the pattern, and what a
 * string subject makes of a dialect written for a buffer.
 *
 * Every expectation here was asked of vim 9.1 before it was written down,
 * through `matchstrpos()` in a headless `vim -es` - which is what
 * `tools/oracle/vim_diff.py` drives, one process for a whole run. What is
 * written here is the *shape* of each rule, so that a failure names the
 * rule; the differential is what says the rule holds over patterns nobody
 * wrote.
 *
 * The last case is not about Vim at all. A lookaround restored the live
 * capture slots when its body's path was abandoned and left the shadow
 * spans - the ones a backreference reads - where the body had written them,
 * which is a defect in shared engine code that this dialect's differential
 * was simply the first gate able to reach. It is stated here, against
 * ECMAScript and PCRE2, because this is the file whose gate found it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <string>

#include "test_helpers.h"

#include "../../src/unicode/unicode_internal.h"

namespace {

struct Attempt {
  GRX_Result result;
  GRX_Diag diag;
  GRX_Regex * regex;
};

Attempt compile(const std::string & pattern, GRX_Syntax syntax,
    uint32_t options = 0) {
  Attempt attempt = {GRX_OK, GRX_DIAG_NONE, nullptr};
  GRX_Error error;
  grx_error_clear(&error);
  attempt.result = grx_regex_compile_with_allocator(pattern.data(),
      pattern.size(), syntax, options, nullptr, nullptr, &error,
      &attempt.regex);
  attempt.diag = error.diag;
  return attempt;
}

/** Whether a pattern compiles at all, as a word a failure message can print. */
std::string accepts(const std::string & pattern) {
  Attempt attempt = compile(pattern, GRX_SYNTAX_VIM);
  grx_regex_free(attempt.regex);
  return attempt.result == GRX_OK ? "ok" : "refused";
}

GRX_Diag why(const std::string & pattern) {
  Attempt attempt = compile(pattern, GRX_SYNTAX_VIM);
  grx_regex_free(attempt.regex);
  return attempt.diag;
}

/** Where a pattern matches, as `start-end`, or "nomatch", or "error". */
std::string span(const std::string & pattern, const std::string & subject,
    GRX_Syntax syntax = GRX_SYNTAX_VIM) {
  Attempt attempt = compile(pattern, syntax);
  if (attempt.result != GRX_OK) {
    grx_regex_free(attempt.regex);
    return "error";
  }
  GRX_Match * match = nullptr;
  grx_match_create(attempt.regex, nullptr, &match);
  int matched = 0;
  std::string answer = "nomatch";
  if (grx_regex_search(attempt.regex, subject.data(), subject.size(), 0,
          GRX_ENGINE_AUTO, nullptr, match, &matched) == GRX_OK
      && matched) {
    GRX_Capture whole;
    grx_match_group(match, 0, &whole);
    answer = std::to_string(whole.start) + "-" + std::to_string(whole.end);
  }
  grx_match_destroy(match);
  grx_regex_free(attempt.regex);
  return answer;
}

/** One group's span, as `start-end`, or "unset"/"nomatch"/"error". */
std::string group(const std::string & pattern, const std::string & subject,
    size_t index, GRX_Syntax syntax = GRX_SYNTAX_VIM) {
  Attempt attempt = compile(pattern, syntax);
  if (attempt.result != GRX_OK) {
    grx_regex_free(attempt.regex);
    return "error";
  }
  GRX_Match * match = nullptr;
  grx_match_create(attempt.regex, nullptr, &match);
  int matched = 0;
  std::string answer = "nomatch";
  if (grx_regex_search(attempt.regex, subject.data(), subject.size(), 0,
          GRX_ENGINE_AUTO, nullptr, match, &matched) == GRX_OK
      && matched) {
    GRX_Capture capture;
    grx_match_group(match, index, &capture);
    answer = capture.start == GRX_NPOS
        ? "unset"
        : std::to_string(capture.start) + "-" + std::to_string(capture.end);
  }
  grx_match_destroy(match);
  grx_regex_free(attempt.regex);
  return answer;
}

/** One code point, as UTF-8. Every byte of it is the subject to match. */
std::string utf8(uint32_t codepoint) {
  std::string out;
  if (codepoint < 0x80) {
    out += (char)codepoint;
  }
  else if (codepoint < 0x800) {
    out += (char)(0xC0 | (codepoint >> 6));
    out += (char)(0x80 | (codepoint & 0x3F));
  }
  else if (codepoint < 0x10000) {
    out += (char)(0xE0 | (codepoint >> 12));
    out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out += (char)(0x80 | (codepoint & 0x3F));
  }
  else {
    out += (char)(0xF0 | (codepoint >> 18));
    out += (char)(0x80 | ((codepoint >> 12) & 0x3F));
    out += (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out += (char)(0x80 | (codepoint & 0x3F));
  }
  return out;
}

} // namespace

// --------------------------------------------------------------------------
// The magic levels
// --------------------------------------------------------------------------

TEST(Vim, TheMagicLevelDecidesWhichCharactersAreOperators) {
  // The table from `:help magic`, one column at a time. `\m` is the default
  // and needs no marker; the other three are named in the pattern and take
  // effect where they stand.
  EXPECT_EQ(span("a(b)c", "abc"), "nomatch");      // magic: literal parens
  EXPECT_EQ(span("a(b)c", "a(b)c"), "0-5");
  EXPECT_EQ(span("\\va(b)c", "abc"), "0-3");       // very magic: a group
  EXPECT_EQ(span("a\\(b\\)c", "abc"), "0-3");      // magic: the escaped form

  EXPECT_EQ(span("a.c", "abc"), "0-3");            // magic: any character
  EXPECT_EQ(span("\\Ma.c", "abc"), "nomatch");     // nomagic: a literal dot
  EXPECT_EQ(span("\\Ma.c", "a.c"), "0-3");
  EXPECT_EQ(span("\\Ma\\.c", "abc"), "0-3");       // ...and `\.` is any

  EXPECT_EQ(span("[ab]", "a"), "0-1");             // magic: a collection
  EXPECT_EQ(span("\\M[ab]", "[ab]"), "0-4");       // nomagic: four literals
  EXPECT_EQ(span("\\M\\[ab]", "a"), "0-1");        // ...and `\[` opens one
}

TEST(Vim, ALevelChangeIsNotScopedToAnything) {
  // `\v(a\m)b` is "E54: Unmatched \(" in vim: the `\m` makes the `)` an
  // ordinary character before the group it would have closed is closed. So
  // the level belongs to a position in the pattern and not to a group.
  EXPECT_EQ(accepts("\\v(a\\m)b"), "refused");
  // The close is spelled the way the level in force at the *close* spells
  // it, which is the same rule read the other way round.
  EXPECT_EQ(span("\\(a\\v)b", "ab"), "0-2");
}

TEST(Vim, AMarkerIsNotAnAtomAndIsTransparentToWhatFollows) {
  // vim strips the markers while scanning, so what stands on either side of
  // one is adjacent. Three consequences, and no two of them would be found
  // by the same probe:
  //
  //   - a repeat after a marker has the atom before it to land on, and vim
  //     refuses the pair as "multi follow a multi";
  EXPECT_EQ(accepts("\\va+\\v*"), "refused");
  EXPECT_EQ(why("a\\v+"), GRX_DIAG_NOTHING_TO_REPEAT);
  //   - a postfix assertion after a marker finds no atom and is "Misplaced";
  EXPECT_EQ(why("a\\m\\@="), GRX_DIAG_NOTHING_TO_REPEAT);
  //   - and a marker with nothing before it leaves a `*` with no atom to
  //     repeat, which makes it the literal asterisk it is at every level.
  EXPECT_EQ(span("\\v*a", "*a"), "0-2");
}

TEST(Vim, CaretAndDollarArePositionalInTheMiddleLevels) {
  // A basic RE's rule: an anchor at the edge of a branch and an ordinary
  // character anywhere else. `a^b` matches "a^b".
  EXPECT_EQ(span("^a", "a"), "0-1");
  EXPECT_EQ(span("a^b", "a^b"), "0-3");
  EXPECT_EQ(span("\\(^a\\)", "a"), "0-1");
  EXPECT_EQ(span("x\\|^a", "a"), "0-1");
  EXPECT_EQ(span("a$b", "a$b"), "0-3");
  EXPECT_EQ(span("\\Ma^", "a^"), "0-2");
}

TEST(Vim, VeryMagicAnchorsAreAlwaysAssertionsAndVeryNomagicOnesAreEscaped) {
  // The two ends of the table. In `\v` the bare character is an assertion
  // wherever it stands, so `\va^` can match nothing; in `\V` the bare
  // character is a literal and `\^` is the assertion - and that assertion
  // is *not* positional, which is the asymmetry: `\Va\^` matches neither
  // "a" nor "a^", while `\Va\{-}\^` matches the empty string at 0.
  EXPECT_EQ(span("\\va^", "a^"), "nomatch");
  EXPECT_EQ(span("\\V^a", "^a"), "0-2");
  EXPECT_EQ(span("\\V\\^a", "a"), "0-1");
  EXPECT_EQ(span("\\Va\\^", "a^"), "nomatch");
  EXPECT_EQ(span("\\Va\\{-}\\^", "a"), "0-0");
}

TEST(Vim, AStarAfterACaretIsALiteralAsterisk) {
  // POSIX's basic-RE rule, and vim has it at every level: `^*a` matches
  // "*a" and not "a". Only after `^` - `\v$*a` matches "a", where the `*`
  // really is a repeat of an assertion that holds zero times.
  EXPECT_EQ(span("^*a", "*a"), "0-2");
  EXPECT_EQ(span("^*a", "a"), "nomatch");
  EXPECT_EQ(span("\\v$*a", "a"), "0-1");
}

// --------------------------------------------------------------------------
// Case
// --------------------------------------------------------------------------

TEST(Vim, TheCaseMarkersReachBackwards) {
  // `\c` and `\C` are not scoped and not positional: either one anywhere in
  // the pattern decides the whole of it, so `x\ca` matches "XA" - the option
  // reaches text the reader has already passed. And the precedence is not
  // "last one wins": a `\c` anywhere beats a `\C` anywhere.
  EXPECT_EQ(span("x\\ca", "XA"), "0-2");
  EXPECT_EQ(span("\\Cab", "Ab"), "nomatch");
  EXPECT_EQ(span("\\Ca\\cb", "AB"), "0-2");
  EXPECT_EQ(span("\\ca\\Cb", "aB"), "0-2");
  // Inside a collection both are ordinary characters, so a pattern that
  // only wanted the letter does not turn caseless matching on.
  EXPECT_EQ(span("[\\c]x", "cx"), "0-2");
  EXPECT_EQ(span("[\\c]x", "cX"), "nomatch");
}

TEST(Vim, ANamedClassIsAPredicateAndCaselessMatchingDoesNotFoldIt) {
  // The same set written two ways gets two answers: `\c[a-z]` matches "A"
  // and `\c\l` does not, because a collection is a set of code points and a
  // named class is a predicate. The POSIX bracket classes go with the named
  // ones.
  EXPECT_EQ(span("\\c[a-z]", "A"), "0-1");
  EXPECT_EQ(span("\\c\\l", "A"), "nomatch");
  EXPECT_EQ(span("\\c\\L", "A"), "0-1");
  EXPECT_EQ(span("\\c[[:lower:]]", "A"), "nomatch");
  EXPECT_EQ(span("\\c[[:upper:]]", "a"), "nomatch");
}

TEST(Vim, CaselessFoldingIsUnicode) {
  // `\cÉ` matches "é" in vim 9.1. The dialect table recorded ASCII-only
  // folding for this row until it was asked.
  EXPECT_EQ(span("\\c\u00c9", "\u00e9"), "0-2");
  EXPECT_EQ(span("\\c\\%u00c9", "\u00e9"), "0-2");
}

// --------------------------------------------------------------------------
// Classes
// --------------------------------------------------------------------------

TEST(Vim, TheNamedClassesAreItsOwnSetsAndNotThisLibrarysShorthands) {
  // vim's `\s` is space and tab alone - no newline, no vertical tab - which
  // is why the eleven named classes are written out by the front end rather
  // than mapped onto GRX_ShorthandKind.
  EXPECT_EQ(span("\\s", " "), "0-1");
  EXPECT_EQ(span("\\s", "\t"), "0-1");
  EXPECT_EQ(span("\\s", "\n"), "nomatch");
  EXPECT_EQ(span("\\S", "\n"), "0-1");
  EXPECT_EQ(span("\\w", "\u00e9"), "nomatch");
  EXPECT_EQ(span("\\h", "0"), "nomatch");
  EXPECT_EQ(span("\\a", "0"), "nomatch");
  EXPECT_EQ(span("\\x", "f"), "0-1");
  EXPECT_EQ(span("\\o", "8"), "nomatch");
}

TEST(Vim, TheUnderscoreFormsAddTheLineBreak) {
  // `\_x` is "x, or a line break", and it is not redundant even where the
  // break is an ordinary character: `\s` refuses a newline and `\_s`
  // accepts one.
  EXPECT_EQ(span("\\_s", "\n"), "0-1");
  EXPECT_EQ(span("\\_d", "\n"), "0-1");
  EXPECT_EQ(span("\\_[ab]", "\n"), "0-1");
  // Added to the *positive* set, so a negated collection takes it too.
  EXPECT_EQ(span("\\_[^a]", "\n"), "0-1");
}

TEST(Vim, ABackslashInACollectionIsAMemberOfIt) {
  // POSIX's rule with four characters carved out of it: `[\w]` matches "w"
  // and it matches a backslash, because the backslash is a member in its
  // own right and the "w" is the next one. `[a-\z]` is a reverse range for
  // the same reason - it runs from "a" to the backslash.
  EXPECT_EQ(span("[\\w]", "w"), "0-1");
  EXPECT_EQ(span("[\\w]", "\\"), "0-1");
  EXPECT_EQ(accepts("[a-\\z]"), "refused");
  // The four it does escape, which is how a collection holds its own
  // delimiters, plus the five control spellings and the numeric ones.
  EXPECT_EQ(span("[\\]]", "]"), "0-1");
  EXPECT_EQ(span("[\\^]", "^"), "0-1");
  EXPECT_EQ(span("[\\-]", "-"), "0-1");
  EXPECT_EQ(span("[\\t]", "\t"), "0-1");
  EXPECT_EQ(span("[\\d65]", "A"), "0-1");
  EXPECT_EQ(span("[\\x41]", "A"), "0-1");
}

TEST(Vim, ABracketThatOpensNothingIsALiteral) {
  // vim reads a `[` that does not open a well-formed collection as the
  // character, so `[` matches "[" and so does `[]`.
  EXPECT_EQ(span("[", "["), "0-1");
  EXPECT_EQ(span("[]", "[]"), "0-2");
  EXPECT_EQ(span("[]a]", "]"), "0-1");
}

TEST(Vim, ThePosixCaseClassesAreUnicodeAndTheRestAreNot) {
  // vim answers `[[:lower:]]` and `[[:upper:]]` from its own Unicode tables
  // and the other ten from the C library's ASCII predicates, which is a
  // split no single width can express.
  EXPECT_EQ(span("[[:lower:]]", "\u00e9"), "0-2");
  EXPECT_EQ(span("[[:upper:]]", "\u00c9"), "0-2");
  EXPECT_EQ(span("[[:alpha:]]", "\u00e9"), "nomatch");
  EXPECT_EQ(span("[[:alnum:]]", "\u00e9"), "nomatch");
  EXPECT_EQ(span("[[:digit:]]", "5"), "0-1");
}

TEST(Vim, WordBoundariesUseTheKeywordSet) {
  // `\<` and `\>` are defined from 'iskeyword', whose default takes in
  // U+00C0 and everything above it - so there is no word start between "a"
  // and "é", where a boundary defined from an ASCII `\w` would find one.
  EXPECT_EQ(span("\\<a", "a b"), "0-1");
  EXPECT_EQ(span("a\\>", "a b"), "0-1");
  EXPECT_EQ(span("\\<\u00e9", "a\u00e9b"), "nomatch");
  EXPECT_EQ(span("\\<\u00e9", " \u00e9b"), "1-3");
}

// --------------------------------------------------------------------------
// Repeats
// --------------------------------------------------------------------------

TEST(Vim, TheLazyRepeatIsSpelledInsideTheBrace) {
  // `\{-n,m}`, and there is no suffix form at all: `a\{1,2}\?` is "E871:
  // Can't have a multi follow a multi".
  EXPECT_EQ(span("x\\{-}", "xxx"), "0-0");
  EXPECT_EQ(span("x\\{-1,}", "xxx"), "0-1");
  EXPECT_EQ(span("x\\{}", "xxx"), "0-3");
  EXPECT_EQ(span("x\\{2,}", "xxx"), "0-3");
  EXPECT_EQ(span("x\\{,2}", "xxx"), "0-2");
  EXPECT_EQ(accepts("a\\{1,2}\\?"), "refused");
  // The close may be `}` or `\}`; anything else is a syntax error.
  EXPECT_EQ(span("x\\{2\\}", "xx"), "0-2");
  EXPECT_EQ(why("x\\{2"), GRX_DIAG_UNMATCHED_OPEN_BRACE);
}

TEST(Vim, AMaximumBelowTheMinimumIsDiscardedRatherThanRefused) {
  // `x\{3,1}` matches three characters of "xxxxx" and `x\{4,2}` four, so
  // the upper bound is dropped rather than the pattern refused - which is
  // what GRX_SyntaxSpec::allow_impossible_repeat would have said, and is a
  // third answer again.
  EXPECT_EQ(span("x\\{3,1}", "xxxxx"), "0-3");
  EXPECT_EQ(span("x\\{4,2}", "xxxxxx"), "0-4");
}

TEST(Vim, ARepeatOfARepeatIsRefused) {
  EXPECT_EQ(accepts("a**"), "refused");
  EXPECT_EQ(accepts("a\\{2}\\{3}"), "refused");
  EXPECT_EQ(accepts("a\\{1}\\+"), "refused");
}

// --------------------------------------------------------------------------
// The postfix assertions
// --------------------------------------------------------------------------

TEST(Vim, TheLookaroundOperatorsArePostfix) {
  // `\(foo\)\@=` is a lookahead over the group written *before* it, which
  // nothing else in this library is shaped like.
  EXPECT_EQ(span("\\(ab\\)\\@=a", "ab"), "0-1");
  EXPECT_EQ(span("\\(ab\\)\\@!a", "ac"), "0-1");
  EXPECT_EQ(span("\\(a\\)\\@<=b", "ab"), "1-2");
  EXPECT_EQ(span("\\(a\\)\\@<!b", "cb"), "1-2");
  EXPECT_EQ(span("\\(a\\+\\)\\@>a", "aaa"), "nomatch");
  EXPECT_EQ(span("\\v(a)@<=b", "ab"), "1-2");
}

TEST(Vim, OnePostfixOperatorPerAtom) {
  // vim counts a `\@` operator as a multi, so a second one is "multi follow
  // a multi" - and so is a repeat after one.
  EXPECT_EQ(accepts("\\(a\\+\\)\\@>\\@="), "refused");
  EXPECT_EQ(accepts("\\(a\\)\\@=*"), "refused");
  // And the operator itself needs an atom that is not a repeat.
  EXPECT_EQ(why("a\\{2,}\\@="), GRX_DIAG_NOTHING_TO_REPEAT);
}

// --------------------------------------------------------------------------
// Groups and references
// --------------------------------------------------------------------------

TEST(Vim, TheNonCapturingGroupIsSpelledWithAPercent) {
  EXPECT_EQ(span("\\%(ab\\)\\+", "abab"), "0-4");
  EXPECT_EQ(span("\\v%(ab)+", "abab"), "0-4");
  EXPECT_EQ(group("\\%(a\\)\\(b\\)", "ab", 1), "1-2");
}

TEST(Vim, AReferenceMustNameAGroupThatHasClosed) {
  // "E65: Illegal back reference", which vim reports for a forward
  // reference and for a reference to the group it is inside alike.
  EXPECT_EQ(span("\\(a\\)\\1", "aa"), "0-2");
  EXPECT_EQ(why("\\1\\(a\\)"), GRX_DIAG_INVALID_BACKREFERENCE);
  EXPECT_EQ(why("\\(a\\1\\)"), GRX_DIAG_INVALID_BACKREFERENCE);
  EXPECT_EQ(why("\\(a\\)\\2"), GRX_DIAG_INVALID_BACKREFERENCE);
}

TEST(Vim, AReferenceToAnUnsetGroupMatchesTheEmptyString) {
  // The probe that settles GRX_BACKREF_UNSET_EMPTY, and the one a
  // differential driven by `matchlist()` cannot make: vim reports "" for an
  // unset group and for one that matched empty alike, so only the *width of
  // the whole match* separates the two answers.
  EXPECT_EQ(span("\\(a\\)\\?\\1", "b"), "0-0");
}

TEST(Vim, NineGroupsIsTheLimit) {
  // vim's own: a tenth `\(` is "E872: Too many '('".
  EXPECT_EQ(accepts("\\(a\\)\\(a\\)\\(a\\)\\(a\\)\\(a\\)"
      "\\(a\\)\\(a\\)\\(a\\)\\(a\\)"), "ok");
  EXPECT_EQ(why("\\(a\\)\\(a\\)\\(a\\)\\(a\\)\\(a\\)"
      "\\(a\\)\\(a\\)\\(a\\)\\(a\\)\\(a\\)"), GRX_DIAG_LIMIT_CAPTURES);
}

TEST(Vim, TheBranchOperatorIsAnAssertionOverTheEarlierConcatenations) {
  // `A\&B` holds where both match at this position and reports B's text, so
  // it is `(?=A)B` and is built as exactly that.
  EXPECT_EQ(span("ab\\&a.", "ab"), "0-2");
  EXPECT_EQ(span("ab\\&ac", "ab"), "nomatch");
  EXPECT_EQ(span("\\vab&a.", "ab"), "0-2");
}

// --------------------------------------------------------------------------
// The `\%` family, and what a string subject makes of a buffer dialect
// --------------------------------------------------------------------------

TEST(Vim, ThePercentFamilyIsCodePointsAndTheSubjectEdges) {
  EXPECT_EQ(span("\\%d65", "A"), "0-1");
  EXPECT_EQ(span("\\%x41", "A"), "0-1");
  EXPECT_EQ(span("\\%o101", "A"), "0-1");
  EXPECT_EQ(span("\\%u00e9", "\u00e9"), "0-2");
  EXPECT_EQ(span("\\%^a", "ab"), "0-1");
  EXPECT_EQ(span("b\\%$", "ab"), "1-2");
  // Very magic drops the backslash from the whole family, and then `\%` is
  // a literal percent.
  EXPECT_EQ(span("\\v%d65", "A"), "0-1");
  EXPECT_EQ(span("\\v\\%$", "a%"), "1-2");
}

TEST(Vim, TheOptionalSequenceIsOptionalAllTheWayDown) {
  // `\%[abc]` matches the empty string: `r\%[ead]` requires the "r" only
  // because the "r" is written outside the brackets.
  EXPECT_EQ(span("r\\%[ead]", "rea"), "0-3");
  EXPECT_EQ(span("r\\%[ead]", "r"), "0-1");
  EXPECT_EQ(span("\\%[abc]", ""), "0-0");
  // And the whole expansion is one atom, so it can be repeated.
  EXPECT_EQ(span("\\%[abc]*", "abc"), "0-3");
}

TEST(Vim, AnOptionalSequencesMembersAreAtoms) {
  // vim's help calls them atoms and means it: a class, a collection, a
  // `\%` escape, a mark and the underscore forms are all members.
  EXPECT_EQ(span("a\\%[\\d\\w]", "a5x"), "0-3");
  EXPECT_EQ(span("a\\%[[bc]d]", "abd"), "0-3");
  EXPECT_EQ(span("a\\%[\\%d98]", "ab"), "0-2");
  EXPECT_EQ(span("a\\%[\\_s]", "a\n"), "0-2");
  EXPECT_EQ(span("a\\%[\\<b]", "ab"), "0-1");
  // A zero-width member still counts, which is why the expansion is an
  // alternation and not a repeat: a repeat discards an iteration that
  // consumed nothing and this one would lose the mark.
  EXPECT_EQ(span("a\\%[\\zsb]", "rea"), "3-3");
  // Seven escapes are the bare letter in here and nowhere else. `\vb` is
  // "v" then "b", so this matches "av" and not "ab".
  EXPECT_EQ(span("a\\%[\\vb]", "av"), "0-2");
  EXPECT_EQ(span("a\\%[\\vb]", "ab"), "0-1");
  EXPECT_EQ(span("a\\%[\\Z]", "aZ"), "0-2");
}

TEST(Vim, WhatAnOptionalSequenceMemberMayNotBe) {
  // No multi, in either spelling, and vim counts its postfix operators as
  // multis for this rule too.
  EXPECT_EQ(why("a\\%[b*]"), GRX_DIAG_NOTHING_TO_REPEAT);
  EXPECT_EQ(why("a\\%[b\\=]"), GRX_DIAG_NOTHING_TO_REPEAT);
  EXPECT_EQ(why("a\\%[b\\{2}]"), GRX_DIAG_NOTHING_TO_REPEAT);
  EXPECT_EQ(why("\\va%[b+]"), GRX_DIAG_NOTHING_TO_REPEAT);
  // No alternation, no branch operator, and no group - the last of which
  // is `set re=1`; the default engine takes one. dialects.md section 6.
  EXPECT_EQ(why("a\\%[b\\|c]"), GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED);
  EXPECT_EQ(why("a\\%[b\\&c]"), GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED);
  EXPECT_EQ(why("a\\%[\\(bc\\)]"), GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED);
  EXPECT_EQ(why("a\\%[\\%(bc\\)]"), GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED);
  EXPECT_EQ(why("a\\%[b\\%[cd]]"), GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED);
  // And not empty, where a collection's `[]` is two characters.
  EXPECT_EQ(why("a\\%[]"), GRX_DIAG_EMPTY_CLASS);
}

TEST(Vim, AStringSubjectHasNoLines) {
  // vim's help is written for a buffer, where `.` refuses the line break.
  // Over a *string* - which is the subject this library has, and what
  // `matchstrpos()` is asked about - the break is an ordinary character.
  EXPECT_EQ(span("a.b", "a\nb"), "0-3");
  EXPECT_EQ(span("a[^x]b", "a\nb"), "0-3");
  EXPECT_EQ(span("^b", "a\nb"), "nomatch");
  EXPECT_EQ(span("a$", "a\nb"), "nomatch");
  EXPECT_EQ(span("\\_^b", "a\nb"), "nomatch");
}

TEST(Vim, KeepMovesTheReportedStart) {
  EXPECT_EQ(span("a\\zsb", "ab"), "1-2");
}

TEST(Vim, KeepEndMovesTheReportedEnd) {
  // `\ze`: the text after it still has to match and the span stops here.
  EXPECT_EQ(span("a\\zeb", "ab"), "0-1");
  EXPECT_EQ(span("a\\zeb", "a"), "nomatch");
  EXPECT_EQ(span("a*\\zeb", "aab"), "0-2");
  // The last one reached on the winning path decides, which is what a
  // repeat makes visible: two iterations, and the second one's mark.
  EXPECT_EQ(span("a\\zeb\\zec", "abc"), "0-2");
  EXPECT_EQ(span("\\%(a\\zeb\\)\\{2}", "abab"), "0-3");
  // A mark on a branch that was abandoned leaves nothing behind.
  EXPECT_EQ(span("\\(a\\zex\\|ab\\)c", "abc"), "0-3");
}

TEST(Vim, AMarkInsideAnAssertionStillCounts) {
  // A mark writes one register for the whole pattern, and an assertion or
  // an atomic group does not take it back. This follows `set re=1`: vim's
  // two engines disagree here and only the old one can be stated as a rule
  // - see documentation/dialects.md section 6, deviation (6).
  EXPECT_EQ(span("\\(a\\zeb\\)\\@=ab", "ab"), "0-1");
  EXPECT_EQ(span("\\(a\\zsb\\)\\@=ab", "ab"), "1-2");
  EXPECT_EQ(span("\\(a\\zeb\\)\\@>c", "abc"), "0-1");
  EXPECT_EQ(span("\\(a\\zsb\\)\\@>c", "abc"), "1-3");
  // `\&` reaches it the same way, because A\&B is built as (?=A)B.
  EXPECT_EQ(span("a\\zeb\\&abc", "abc"), "0-1");
  EXPECT_EQ(span("a\\zsb\\&abc", "abc"), "1-3");
  // An assertion at the end of the pattern is the one case the *new*
  // engine agrees about, which is what says this is not a question vim
  // leaves open: both answer 0-1 here, and only the register rule does.
  EXPECT_EQ(span("\\(a\\zeb\\)\\@=", "ab"), "0-1");
}

TEST(Vim, AnEndBeforeTheStartIsTheEmptySpanAtTheStart) {
  // The two marks are independent and either may come last. vim's own
  // `matchend()` answers 3 for this pattern against "abcd", so the empty
  // span is its answer and not a repair of one.
  EXPECT_EQ(span("ab\\zec\\zsd", "abcd"), "3-3");
  EXPECT_EQ(span("a\\zeb\\zsc", "abc"), "2-2");
}

TEST(Vim, OnlyTheOptionalMultiMayFollowAMark) {
  // "E888: Can not repeat \zs or \ze" takes `*`, `\+` and every brace
  // form - `\{0,1}` included, which is what says the rule is about the
  // spelling and not about the bounds it asks for.
  EXPECT_EQ(why("a\\zs*b"), GRX_DIAG_NOTHING_TO_REPEAT);
  EXPECT_EQ(why("a\\ze*b"), GRX_DIAG_NOTHING_TO_REPEAT);
  EXPECT_EQ(why("a\\ze\\{0,1}b"), GRX_DIAG_NOTHING_TO_REPEAT);
  EXPECT_EQ(why("\\va\\zs+b"), GRX_DIAG_NOTHING_TO_REPEAT);
  // `\=` and `\?` are accepted, and the mark still counts under them.
  EXPECT_EQ(span("a\\zs\\=b", "ab"), "1-2");
  EXPECT_EQ(span("a\\ze\\?b", "ab"), "0-1");
  EXPECT_EQ(span("\\va\\zs?b", "ab"), "1-2");
}

// --------------------------------------------------------------------------
// What it refuses
// --------------------------------------------------------------------------

TEST(Vim, TheConstructsItRefusesAndWhy) {
  // Each of these is a deviation recorded in documentation/dialects.md
  // section 6, and each is refused rather than guessed at.
  // `~` is the last `:s` replacement, and there has never been one - which
  // is the state vim answers "E33" in, so this refusal is vim's own answer
  // and not a deviation. `\~` is the literal tilde in both.
  EXPECT_EQ(why("a~"), GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED);
  EXPECT_EQ(span("a\\~", "a~"), "0-2");
  EXPECT_EQ(span("\\M~", "a~"), "1-2");
  // These two vim refuses itself, outside a syntax file.
  EXPECT_EQ(why("\\z(a\\)"), GRX_DIAG_NOT_IN_DIALECT);
  EXPECT_EQ(why("\\z1"), GRX_DIAG_NOT_IN_DIALECT);
}

TEST(Vim, ALookbehindsCountBoundsHowFarBackItMayStart) {
  // `\@123<=`: the match may start at most that many *bytes* back, so a
  // body that cannot fit inside the bound is one the assertion can never
  // satisfy - and zero means no bound, which is what an unwritten number
  // leaves too.
  EXPECT_EQ(span("\\(ab\\)\\@<=c", "abc"), "2-3");
  EXPECT_EQ(span("\\(ab\\)\\@2<=c", "abc"), "2-3");
  EXPECT_EQ(span("\\(ab\\)\\@0<=c", "abc"), "2-3");
  EXPECT_EQ(span("\\(ab\\)\\@1<=c", "abc"), "nomatch");
  // Bytes, not characters: U+00E9 is two of them.
  EXPECT_EQ(span("\\(\u00e9\\)\\@2<=x", "\u00e9x"), "2-3");
  EXPECT_EQ(span("\\(\u00e9\\)\\@1<=x", "\u00e9x"), "nomatch");
  // The bound makes a variable-length body finite, so a dialect whose
  // lookbehind is otherwise unbounded still runs this one forwards.
  EXPECT_EQ(span("\\(\\w\\+\\)\\@1<=c", "abc"), "2-3");
  // And the negative form: a body that cannot fit inside the bound cannot
  // be found, so the assertion holds.
  EXPECT_EQ(span("\\(ab\\)\\@1<!c", "abc"), "2-3");
  EXPECT_EQ(span("\\(ab\\)\\@2<!c", "abc"), "nomatch");
}

TEST(Vim, TheBufferPositionsAreBuiltAndThreeOfThemNeverMatch) {
  // `\%V`, `\%#` and the three `l` forms name something a subject has
  // not got. vim compiles them all and none of them matches over a
  // string, so that is what is built - a pattern that compiles and finds
  // nothing, not a refusal of a pattern vim accepts.
  EXPECT_EQ(span("a\\%Vb", "ab"), "nomatch");
  EXPECT_EQ(span("\\%#", "ab"), "nomatch");
  EXPECT_EQ(span("\\%1l", "ab"), "nomatch");
  EXPECT_EQ(span("\\%<9l", "ab"), "nomatch");
  EXPECT_EQ(span("\\%>0l", "ab"), "nomatch");
  // A zero-iteration repeat of one still matches the empty string, which
  // is `\%V*` in vim as well.
  EXPECT_EQ(span("\\%V*", "ab"), "0-0");

  // `\%23c` is the *byte* column, counted from one: `\%4c` holds after a
  // three-byte character and `\%2c` holds nowhere in that subject.
  EXPECT_EQ(span("\\%1c", "abc"), "0-0");
  EXPECT_EQ(span("a\\%2cb", "abc"), "0-2");
  EXPECT_EQ(span("\\%4c", "\u65e5x"), "3-3");
  EXPECT_EQ(span("\\%2c", "\u65e5x"), "nomatch");
  // A line break does not reset it: over a string there is one line.
  EXPECT_EQ(span("\\%1c", "ab\ncd"), "0-0");
  EXPECT_EQ(span("\\%4c", "ab\ncd"), "3-3");
  // The comparisons, and the two ranges with nothing in them.
  EXPECT_EQ(span("a\\%<3cb", "abc"), "0-2");
  EXPECT_EQ(span("\\%>2c", "abc"), "2-2");
  EXPECT_EQ(span("\\%>0c", "abc"), "0-0");
  EXPECT_EQ(span("\\%0c", "abc"), "nomatch");
  EXPECT_EQ(span("\\%<1c", "abc"), "nomatch");
}

/** Replace every match, as a string, or "error". */
std::string replaced(const std::string & pattern, const std::string & subject,
    const std::string & templ) {
  Attempt attempt = compile(pattern, GRX_SYNTAX_VIM);
  if (attempt.result != GRX_OK) {
    grx_regex_free(attempt.regex);
    return "error";
  }
  GRX_Text out;
  memset(&out, 0, sizeof(out));
  std::string answer = "error";
  if (grx_regex_replace(attempt.regex, subject.data(), subject.size(),
          templ.data(), templ.size(), GRX_REPLACE_GLOBAL, nullptr, nullptr,
          nullptr, &out)
      == GRX_OK) {
    answer = std::string(out.data, out.length);
    grx_text_free(&out);
  }
  grx_regex_free(attempt.regex);
  return answer;
}

TEST(Vim, TheScreenColumnCountsCellsAndTheByteColumnCountsBytes) {
  // `\%23v` is the *screen* column: a tab reaches the next multiple of
  // the tabstop, a wide character takes two cells and a combining one
  // takes none. Every number here was asked of vim 9.1 first.
  EXPECT_EQ(span("\\%1v", "a\tb"), "0-0");
  EXPECT_EQ(span("\\%2v", "a\tb"), "1-1");
  EXPECT_EQ(span("\\%5v", "a\tb"), "nomatch");  // inside the tab
  EXPECT_EQ(span("\\%9v", "a\tb"), "2-2");
  EXPECT_EQ(span("\\%10v", "a\tb"), "3-3");
  // U+65E5 is two cells and three bytes, which is what tells the two
  // columns apart.
  EXPECT_EQ(span("\\%3v", "\u65e5x"), "3-3");
  EXPECT_EQ(span("\\%2v", "\u65e5x"), "nomatch");
  EXPECT_EQ(span("\\%4c", "\u65e5x"), "3-3");
  // A combining character has no column of its own: `\%2v` holds at the
  // "x" and not at the mark, though both are column 2 by arithmetic.
  EXPECT_EQ(span("\\%2v", "a\u0301x"), "3-3");
  // A line break is two cells, because vim draws it as "^J".
  EXPECT_EQ(span("\\%3v", "ab\ncd"), "2-2");
  EXPECT_EQ(span("\\%4v", "ab\ncd"), "nomatch");
  EXPECT_EQ(span("\\%5v", "ab\ncd"), "3-3");
  // The comparisons, and the two ranges with nothing in them.
  EXPECT_EQ(span("a\\%<3vb", "abc"), "0-2");
  EXPECT_EQ(span("\\%>3v", "abc"), "3-3");
  EXPECT_EQ(span("\\%>0v", "abc"), "0-0");
  EXPECT_EQ(span("\\%0v", "abc"), "nomatch");
  EXPECT_EQ(span("\\%<1v", "abc"), "nomatch");
}

TEST(Vim, TheReplacementTemplateIsVimsOwn) {
  // `&` is the whole match and `\&` a literal one; `\0` is the whole
  // match again and `\1` to `\9` name groups a digit at a time; `~` is a
  // literal tilde, there having been no previous substitution; and a
  // reference to a group the pattern has not got substitutes nothing.
  EXPECT_EQ(replaced("\\(a\\)\\(b\\)", "ab", "&"), "ab");
  EXPECT_EQ(replaced("\\(a\\)\\(b\\)", "ab", "\\&"), "&");
  EXPECT_EQ(replaced("\\(a\\)\\(b\\)", "ab", "\\0"), "ab");
  EXPECT_EQ(replaced("\\(a\\)\\(b\\)", "ab", "\\2\\1"), "ba");
  EXPECT_EQ(replaced("\\(a\\)\\(b\\)", "ab", "\\9"), "");
  EXPECT_EQ(replaced("\\(a\\)\\(b\\)", "ab", "~"), "~");
  EXPECT_EQ(replaced("\\(a\\)\\(b\\)", "ab", "\\q"), "q");
  // Four escapes decode. Over a *string*, which is the subject here: in a
  // buffer `:s` writes a line break for `\r` and a NUL for `\n`.
  EXPECT_EQ(replaced("a", "a", "\\n"), "\n");
  EXPECT_EQ(replaced("a", "a", "\\r"), "\r");
  EXPECT_EQ(replaced("a", "a", "\\t"), "\t");
  EXPECT_EQ(replaced("a", "a", "\\b"), "\b");
}

TEST(Vim, TheReplacementCaseMarkers) {
  // `\u` and `\l` take the next character; `\U` and `\L` run until
  // `\E` or `\e`. A one-character modifier *suspends* a run for one
  // character and the run resumes: "ABcD", measured.
  EXPECT_EQ(replaced("x", "x", "\\uabc"), "Abc");
  EXPECT_EQ(replaced("x", "x", "\\Uabc"), "ABC");
  EXPECT_EQ(replaced("x", "x", "\\LABC"), "abc");
  EXPECT_EQ(replaced("x", "x", "\\Uab\\lcd"), "ABcD");
  EXPECT_EQ(replaced("x", "x", "\\Uab\\Ecd"), "ABcd");
  EXPECT_EQ(replaced("x", "x", "\\Uab\\ecd"), "ABcd");
  // `\E` clears a pending one-shot as well as the run.
  EXPECT_EQ(replaced("x", "x", "\\u\\Ex"), "x");
  // The one-shot is spent on the next character whatever it is, so the tab
  // takes it and the "x" after it is left alone.
  EXPECT_EQ(replaced("x", "x", "\\u\\tx"), "\tx");
  // It applies to what a group substitutes, not only to literal text.
  EXPECT_EQ(replaced("\\(a\\)\\(b\\)", "ab", "\\u\\1\\2"), "Ab");
  EXPECT_EQ(replaced("\\(a\\)\\(b\\)", "ab", "\\U\\1\\E\\2"), "Ab");
  // The *simple* mapping, which is what vim applies: U+00DF has none and
  // U+01F3 uppercases to U+01F1 rather than to the titlecase U+01F2.
  EXPECT_EQ(replaced("x", "x", "\\u\u00e9y"), "\u00c9y");
  EXPECT_EQ(replaced("x", "x", "\\u\u00df"), "\u00df");
  EXPECT_EQ(replaced("x", "x", "\\u\u01f3"), "\u01f1");
}

TEST(Vim, ASearchLoopAdvancesAfterAnEmptyMatchAndStopsAtTheEnd) {
  // Two clauses, and vim differs from node, perl and `re` on both. After
  // an empty match it advances rather than retrying without one...
  EXPECT_EQ(replaced("\\|a", "aab", "<>"), "<>a<>a<>b<>");
  // ...and a match reaching the end of the subject ends the loop, so the
  // empty match that would otherwise follow it is not reported. Every
  // other reference here answers "<>a<><>".
  EXPECT_EQ(replaced("b*", "ab", "<>"), "<>a<>");
  EXPECT_EQ(replaced("b*", "abX", "<>"), "<>a<><>X<>");
}

TEST(Vim, AnEmptyFirstIterationRunsAndItsMarkSticks) {
  // The empty-iteration cell had never been probed and so read
  // ECMA-262's, where an iteration that consumed nothing fails. In vim it
  // runs while the repeat still stands where it began, and what it wrote
  // stays: `a\%(\zs\)*b` is 1-2 there, in both engines, and would be
  // 0-2 under the rule this row used to carry. `matchlist()` cannot show
  // it - a group that did not take part and one that matched empty are
  // both "" there - so the mark is the only probe that can.
  EXPECT_EQ(span("a\\%(\\zs\\)*b", "ab"), "1-2");
  EXPECT_EQ(span("a\\(\\zs\\)*", "ab"), "1-1");
  EXPECT_EQ(span("a\\%[\\zsb]*", "aaab"), "1-1");
  // And a trailing empty iteration, after one has consumed, does not run.
  EXPECT_EQ(span("\\%(\\zsa\\)*", "aa"), "1-2");
}

TEST(Vim, AMarkMakesTheAdvanceAboutWhatWasWalked) {
  // The four shapes an empty-span test gets wrong. What decides is whether
  // the next attempt would start where this one began, and the pair that
  // answers it is the *walked* start against the *reported* end.
  //
  // `a\zs` reports 1-1 having walked 0-1, so it arrived there and the
  // loop goes on from 1 without advancing.
  EXPECT_EQ(replaced("a\\zs", "aab", "X"), "aXaXb");
  // `\zea` reports 1-1 having walked 1-2: still where it began, so the
  // loop advances. Without that this pattern never terminates.
  EXPECT_EQ(replaced("\\zea", "xaby", "X"), "xXaby");
  EXPECT_EQ(replaced("\\zea", "aaa", "X"), "XaXaXa");
  EXPECT_EQ(replaced("\\zeab", "abab", "X"), "XabXab");
  // And one that walked nothing at all, which advances as it always did.
  EXPECT_EQ(replaced("\\(b\\)\\@<=", "abcb", "X"), "abXcbX");
}

TEST(Vim, TheFlagAlphabetIsEmptyBecauseTheFlagsAreInThePattern) {
  // `\c`, `\v` and the rest are pattern syntax, not a flag string, so the
  // alphabet is empty - an empty string is still valid and any letter is
  // unknown.
  uint32_t options = 0;
  EXPECT_EQ(grx_options_parse(GRX_SYNTAX_VIM, "", &options, nullptr), GRX_OK);
  EXPECT_EQ(options, 0u);
  EXPECT_EQ(grx_options_parse(GRX_SYNTAX_VIM, "i", &options, nullptr),
      GRX_ERR_SYNTAX);
}

// --------------------------------------------------------------------------
// Not about Vim
// --------------------------------------------------------------------------

TEST(Vim, AnAssertionPutsBackTheShadowSpansItsBodyWrote) {
  // A lookaround restored the live capture slots when its body's path was
  // abandoned and left the *shadow* spans - the ones a backreference reads
  // - where the body had written them. `(?=(a))$|(a)\1` against "aa"
  // matched two characters here, because the `\1` of the second branch read
  // what the first branch's assertion had written on its way to failing.
  //
  // node answers ["a", null, "a"] and pcre2test answers "No match", and
  // both are reproduced now. The live captures were right throughout -
  // group one is unset either way - so the only visible symptom was the
  // width of the match, which is why no conformance vector caught it.
  EXPECT_EQ(span("(?=(a))$|(a)\\1", "aa", GRX_SYNTAX_ECMASCRIPT), "0-1");
  EXPECT_EQ(group("(?=(a))$|(a)\\1", "aa", 1, GRX_SYNTAX_ECMASCRIPT), "unset");
  EXPECT_EQ(group("(?=(a))$|(a)\\1", "aa", 2, GRX_SYNTAX_ECMASCRIPT), "0-1");
  // PCRE2 fails on a reference to a group that did not participate, so the
  // same pattern has no match there at all.
  EXPECT_EQ(span("(?=(a))$|(a)\\1", "aa", GRX_SYNTAX_PCRE), "nomatch");
  EXPECT_EQ(span("\\(a\\)\\@=$\\|\\(a\\)\\1", "aa"), "0-1");
}

// --------------------------------------------------------------------------
// The word classes
// --------------------------------------------------------------------------

TEST(Vim, WordBoundariesHoldWhereTheCharacterClassChanges) {
  // `\<` and `\>` are not the word set's two halves in vim: it sorts every
  // code point into one of nine classes and they hold where the *class*
  // changes. So a boundary falls between two characters that are both
  // 'iskeyword', which no set can see. Every row was asked of vim 9.1.
  //
  // U+65E5 is CJK and "x" is Latin: one word ends and another begins
  // between them, three bytes in.
  EXPECT_EQ(span("\\>", "\u65e5x"), "3-3");
  EXPECT_EQ(span("\\<x", "\u65e5x"), "3-4");
  // Two characters of the same class, and there is no boundary between them
  // at all - the end of the subject is the only place `\>` holds.
  EXPECT_EQ(span("^a\\>", "ab"), "nomatch");
  EXPECT_EQ(span("^\u65e5\\>", "\u65e5\u65e5"), "nomatch");
  EXPECT_EQ(span("\u65e5\u65e5\\>", "\u65e5\u65e5"), "0-6");
  // Hiragana, Katakana, Braille, Hangul and emoji are five more classes, so
  // each of these is a boundary though both sides are word characters.
  EXPECT_EQ(span("\\>", "\u3042\u30a2"), "3-3");
  EXPECT_EQ(span("\\>", "\u2801a"), "3-3");
  EXPECT_EQ(span("\\>", "a\U0001F600"), "1-1");
  EXPECT_EQ(span("\\>", "\ud55c\u3131"), "3-3");
  // And the rule the word set already gave: a keyword character next to a
  // blank or a punctuation mark, and the two ends of the subject.
  EXPECT_EQ(span("\\<", "!ab"), "1-1");
  EXPECT_EQ(span("\\>", "ab!"), "2-2");
  EXPECT_EQ(span("\\>", "ab"), "2-2");
  EXPECT_EQ(span("\\<", "ab"), "0-0");
  // Class one and class zero are both "not a word", so nothing holds
  // between a punctuation mark and a space either way.
  EXPECT_EQ(span("\\<", "! "), "nomatch");
  EXPECT_EQ(span("\\>", "! "), "nomatch");
  // "é" and "µ" are keyword characters at vim's default 'iskeyword', which
  // is the measurement the class table shares with `\k`.
  EXPECT_EQ(span("^a\\>", "a\u00e9"), "nomatch");
  EXPECT_EQ(span("\\>", "\u00b5!"), "2-2");
}

TEST(Vim, EveryClassFromTwoUpIsAKeywordCharacterAndNothingElseIs) {
  // Two tables measured separately from the same vim: `\k` is 'iskeyword'
  // enumerated in the front end, and src/unicode/vim_class.c is
  // `charclass()` enumerated for the boundaries. The boundaries are only
  // right if the two agree, because "class two or more" is what stands
  // where the word set used to - so this asks all 1,114,112 code points
  // rather than trusting that two enumerations of one option landed in the
  // same place.
  Attempt attempt = compile("\\k", GRX_SYNTAX_VIM);
  ASSERT_EQ(attempt.result, GRX_OK);
  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(attempt.regex, nullptr, &match), GRX_OK);
  size_t disagreements = 0;
  uint32_t first = 0;
  for (uint32_t codepoint = 0; codepoint <= 0x10FFFF; codepoint++) {
    // The surrogates are written through in both tables: a UTF-8 subject
    // has none, and vim cannot make one to be asked about.
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
      continue;
    }
    std::string subject = utf8(codepoint);
    int matched = 0;
    ASSERT_EQ(grx_regex_search(attempt.regex, subject.data(), subject.size(),
                  0, GRX_ENGINE_AUTO, nullptr, match, &matched), GRX_OK);
    int keyword = matched != 0;
    int word = grx_vim_char_class(codepoint) >= GRX_VIM_CLASS_KEYWORD;
    if (keyword != word) {
      if (!disagreements) {
        first = codepoint;
      }
      disagreements++;
    }
  }
  grx_match_destroy(match);
  grx_regex_free(attempt.regex);
  EXPECT_EQ(disagreements, 0u) << "first at U+" << std::hex << first;
}

// --------------------------------------------------------------------------
// Composing clusters
// --------------------------------------------------------------------------

TEST(Vim, ABaseAndTheMarksAfterItAreOneCharacter) {
  // Vim matches a *cluster*: a character and the composing characters
  // written after it are one character to every atom that consumes
  // anything. Every number here was asked of vim 9.1 first.
  //
  // "a" U+0301 is three bytes and one character, so `.` takes all of it
  // and a literal `a` matches none of it - the literal is the one atom
  // that does not take the marks, and a match may not end between them.
  EXPECT_EQ(span(".", "a\u0301b"), "0-3");
  EXPECT_EQ(span("[a]", "a\u0301b"), "0-3");
  EXPECT_EQ(span("\\w", "a\u0301b"), "0-3");
  EXPECT_EQ(span("[[:alpha:]]", "a\u0301b"), "0-3");
  EXPECT_EQ(span("a", "a\u0301b"), "nomatch");
  EXPECT_EQ(span("\\(a\\)", "a\u0301b"), "nomatch");
  // Two marks are one character with the base, and `.` after a literal
  // takes the marks the literal left: the literal consumes its own code
  // point and the cluster ends where the marks stop.
  EXPECT_EQ(span(".", "a\u0301\u0302b"), "0-5");
  EXPECT_EQ(span("a.", "a\u0301b"), "0-3");
  EXPECT_EQ(span("a.", "a\u0301\u0302b"), "0-5");
  EXPECT_EQ(group("\\(a\\)\\(.\\)", "a\u0301b", 2), "1-3");
  // `..` finds one character inside a cluster and the next one after it,
  // and `.\{2}` over a subject that holds only the cluster finds nothing:
  // the marks cannot be given back, which is what makes the run of them
  // possessive rather than greedy.
  EXPECT_EQ(span("..", "a\u0301b"), "0-4");
  EXPECT_EQ(span(".\\{2}", "a\u0301"), "nomatch");
  // Any character can be a base: a line break, a space and a wide
  // character all carry marks in vim.
  EXPECT_EQ(span("[^a]", "a\n\u0301b"), "1-4");
  EXPECT_EQ(span("[^a]", "a \u0301b"), "1-4");
  EXPECT_EQ(span(".", "\u65e5\u0301b"), "0-5");
}

TEST(Vim, AMatchBeginsAndEndsWhereAClusterDoes) {
  // A composing character with something before it is in the middle of a
  // character, and no match starts or ends there. `\%2c` is the position
  // one byte in, which is inside the cluster, and `[^a]` skips it for the
  // "b" rather than matching the mark.
  EXPECT_EQ(span("\\%2c", "a\u0301b"), "nomatch");
  EXPECT_EQ(span("\\%4c", "a\u0301b"), "3-3");
  EXPECT_EQ(span("[^a]", "a\u0301b"), "3-4");
  // A composing character with *nothing* before it is a character of its
  // own, so the empty match at offset 0 stands and `.` takes it.
  EXPECT_EQ(span("a*", "\u0301a"), "0-0");
  EXPECT_EQ(span(".", "\u0301a"), "0-2");
  EXPECT_EQ(span(".", "\u0301\u0302a"), "0-4");
  // The screen column agrees with the rule from the other side: a mark has
  // no column of its own, so `\%2v` holds at the character after the
  // cluster and nowhere inside it.
  EXPECT_EQ(span("\\%2v", "a\u0301b"), "3-3");
}

TEST(Vim, EveryMarkThePatternWritesMustBeInTheCluster) {
  // A base with marks written after it in the *pattern* is one atom, and
  // the marks it names must all be among the ones the text carries. Order
  // does not matter and the text may carry more, so a pattern with one
  // mark matches a cluster with two - and consumes both.
  EXPECT_EQ(span("a\u0301", "a\u0301b"), "0-3");
  EXPECT_EQ(span("a\u0301", "a\u0301\u0302b"), "0-5");
  EXPECT_EQ(span("a\u0302", "a\u0301\u0302b"), "0-5");
  EXPECT_EQ(span("a\u0302", "a\u0301b"), "nomatch");
  EXPECT_EQ(span("a\u0301\u0302", "a\u0301b"), "nomatch");
  EXPECT_EQ(span("a\u0301", "ab"), "nomatch");
  // The cluster is the atom a repeat applies to, not the mark.
  EXPECT_EQ(span("a\u0301*", "a\u0301a\u0301"), "0-6");
  EXPECT_EQ(span("a\u0301*", "ab"), "0-0");
  EXPECT_EQ(group("\\(a\u0301\\)", "a\u0301\u0302b", 1), "0-5");
}

TEST(Vim, IgnoreCombiningCarriesTheMarksInsteadOfMatchingThem) {
  // `\Z` anywhere in the pattern decides the whole of it, as `\c` does.
  // With it every atom takes the composing characters after what it
  // matched - the literal included, which is the only difference - and a
  // composing character written beside a base in the pattern is ignored.
  EXPECT_EQ(span("\\Za", "a\u0301b"), "0-3");
  EXPECT_EQ(span("\\Za", "ab"), "0-1");
  EXPECT_EQ(span("\\Za\u0301", "ab"), "0-1");
  EXPECT_EQ(span("\\Zab", "a\u0301b"), "0-4");
  EXPECT_EQ(span("a\\Zb", "a\u0301b"), "0-4");
  EXPECT_EQ(span("\\Z[^a]", "a\u0301b"), "3-4");
  EXPECT_EQ(group("\\Z\\(a\\)", "a\u0301b", 1), "0-3");
  // It is a marker and not an atom, so a multi may not follow it - `a\Z*`
  // is an error in vim exactly as `a\c*` is - and one with nothing before
  // it leaves the `*` a literal asterisk.
  EXPECT_EQ(accepts("a\\Z*"), "refused");
  EXPECT_EQ(accepts("a\\Z\\+"), "refused");
  EXPECT_EQ(span("\\Z*", "aaa"), "nomatch");
}

TEST(Vim, AComposingCharacterThatBeginsAnAtomIsRefused) {
  // A mark written after `.`, after `\w`, after a group, or with nothing
  // before it at all is a second matching rule in vim - the atom then
  // matches any cluster carrying that mark, base ignored - and its own two
  // engines disagree about the collection. Refused rather than answered
  // either way; documentation/dialects.md section 6.
  EXPECT_EQ(accepts(".\u0301"), "refused");
  EXPECT_EQ(accepts("\\w\u0301"), "refused");
  EXPECT_EQ(accepts("\\(a\\)\u0301"), "refused");
  EXPECT_EQ(accepts("\u0301"), "refused");
  EXPECT_EQ(accepts("[a\u0301]"), "refused");
  EXPECT_EQ(accepts("x\\|\u0301"), "refused");
  EXPECT_EQ(why(".\u0301"), GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED);
  EXPECT_EQ(why("[a\u0301]"), GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED);
  // And `\Z` does not make them legal: under it a composing character
  // beside a base is dropped, but one that begins an atom still asks for
  // the rule above.
  EXPECT_EQ(accepts("\\Z.\u0301"), "refused");
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
