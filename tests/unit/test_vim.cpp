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

// --------------------------------------------------------------------------
// What it refuses
// --------------------------------------------------------------------------

TEST(Vim, TheConstructsItRefusesAndWhy) {
  // Each of these is a deviation recorded in documentation/dialects.md
  // section 6, and each is refused rather than guessed at.
  EXPECT_EQ(why("a~"), GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED);   // last `:s`
  EXPECT_EQ(why("\\Za"), GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED); // combining
  EXPECT_EQ(why("a\\zeb"), GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED); // match end
  EXPECT_EQ(why("\\%V"), GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED); // Visual area
  EXPECT_EQ(why("\\%23l"), GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED); // buffer line
  // These two vim refuses itself, outside a syntax file.
  EXPECT_EQ(why("\\z(a\\)"), GRX_DIAG_NOT_IN_DIALECT);
  EXPECT_EQ(why("\\z1"), GRX_DIAG_NOT_IN_DIALECT);
}

TEST(Vim, TheReplacementGrammarIsNotBuilt) {
  // vim's `:s` replacement has two rules nothing else here has - `\r`
  // inserts a line break where `\n` inserts a NUL, and `\u`, `\U`, `\l`,
  // `\L`, `\e` and `\E` change the case of what follows - so the row is
  // left zeroed and grx_regex_replace() refuses it rather than applying
  // sed's grammar and getting both wrong.
  Attempt attempt = compile("a", GRX_SYNTAX_VIM);
  ASSERT_EQ(attempt.result, GRX_OK);
  GRX_Text out;
  EXPECT_EQ(grx_regex_replace(attempt.regex, "a", 1, "b", 1, 0, nullptr,
                nullptr, nullptr, &out),
      GRX_ERR_UNSUPPORTED);
  grx_regex_free(attempt.regex);
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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
