/**
 * @file
 *
 * The POSIX and GNU front ends: what a basic RE spells differently, and what
 * the two grammars do not share.
 *
 * Every expectation here was asked of glibc through
 * tools/oracle/posix_match.c before it was written down. That matters more
 * than usual for these dialects: POSIX leaves a great deal undefined - a
 * backslash before an ordinary character, a `{` that begins nothing, a `)`
 * with no `(` - and "undefined" is not a rule a second implementation can
 * follow. What glibc does with them is.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <string>

#include "test_helpers.h"

namespace {

/** Compile under a dialect, and report the result and the diagnostic. */
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

GRX_Result compile_result(const std::string & pattern, GRX_Syntax syntax,
    uint32_t options = 0) {
  Attempt attempt = compile(pattern, syntax, options);
  grx_regex_free(attempt.regex);
  return attempt.result;
}

/**
 * Where a pattern matches, as `start-end`, or "nomatch", or "error".
 *
 * A string rather than a struct because that is what a failure message
 * should say: `EXPECT_EQ(span(...), "0-3")` prints both spans when it fails,
 * where a pair of size_t prints two numbers nobody can place.
 */
std::string span(const std::string & pattern, const std::string & subject,
    GRX_Syntax syntax, uint32_t options = 0) {
  Attempt attempt = compile(pattern, syntax, options);
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

/** Every match replaced, or "template" when the template is refused. */
std::string replace_all(const std::string & pattern,
    const std::string & subject, const std::string & tmpl, GRX_Syntax syntax) {
  Attempt attempt = compile(pattern, syntax);
  if (attempt.result != GRX_OK) {
    grx_regex_free(attempt.regex);
    return "error";
  }
  GRX_Text out;
  std::memset(&out, 0, sizeof(out));
  GRX_Error error;
  grx_error_clear(&error);
  GRX_Result result = grx_regex_replace(attempt.regex, subject.data(),
      subject.size(), tmpl.data(), tmpl.size(), GRX_REPLACE_GLOBAL, nullptr,
      nullptr, &error, &out);
  std::string answer = result == GRX_OK
      ? std::string(out.data, out.length)
      : (result == GRX_ERR_SYNTAX ? "template" : "error");
  if (result == GRX_OK) {
    grx_text_free(&out);
  }
  grx_regex_free(attempt.regex);
  return answer;
}

/** One group's span, as `start-end`, or "unset"/"nomatch"/"error". */
std::string group(const std::string & pattern, const std::string & subject,
    GRX_Syntax syntax, size_t index, uint32_t options = 0) {
  Attempt attempt = compile(pattern, syntax, options);
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

const GRX_Syntax kBre = GRX_SYNTAX_GNU_BRE;
const GRX_Syntax kEre = GRX_SYNTAX_GNU_ERE;

} // namespace

TEST(Posix, TheOperatorsABasicReSpellsWithABackslash) {
  // The whole of what "basic" means: `\(` groups and `(` is a character,
  // `\{` begins an interval and `{` is a brace, GNU's `\|` alternates and
  // `|` is a pipe. Each pair is asserted both ways round, because a front
  // end that read *both* spellings as the operator would pass a test that
  // only checked the escaped one.
  EXPECT_EQ(span("a\\(b\\)c", "abc", kBre), "0-3");
  EXPECT_EQ(span("a(b)c", "a(b)c", kBre), "0-5");

  EXPECT_EQ(span("a\\{2\\}", "aa", kBre), "0-2");
  EXPECT_EQ(span("a{2}", "a{2}", kBre), "0-4");

  EXPECT_EQ(span("a\\|b", "b", kBre), "0-1");
  EXPECT_EQ(span("a|b", "a|b", kBre), "0-3");

  // And an extended RE is the other way round in every case.
  EXPECT_EQ(span("a(b)c", "abc", kEre), "0-3");
  EXPECT_EQ(span("a{2}", "aa", kEre), "0-2");
  EXPECT_EQ(span("a|b", "b", kEre), "0-1");
}

TEST(Posix, WhereAnAnchorIsAnAnchorAndWhereItIsACharacter) {
  // An extended RE's `^` and `$` are anchors wherever they stand, so `a^b`
  // is a pattern nothing can satisfy rather than three characters. A basic
  // RE's are anchors only where one could be meant - first in the RE, in a
  // subexpression or in an alternative, and last in the same three places -
  // and ordinary characters everywhere else.
  EXPECT_EQ(span("a^b", "a^b", kBre), "0-3");
  EXPECT_EQ(span("a$b", "a$b", kBre), "0-3");
  EXPECT_EQ(span("a^b", "a^b", kEre), "nomatch");
  EXPECT_EQ(span("a$b", "a$b", kEre), "nomatch");

  EXPECT_EQ(span("^a", "a", kBre), "0-1");
  EXPECT_EQ(span("a$", "a", kBre), "0-1");
  EXPECT_EQ(span("\\(^a\\)", "a", kBre), "0-1");
  EXPECT_EQ(span("\\(a$\\)", "a", kBre), "0-1");
  EXPECT_EQ(span("a\\|^b", "b", kBre), "0-1");
}

TEST(Posix, AnAsteriskWithNothingToRepeatIsAnAsterisk) {
  // POSIX: an `*` first in the RE or in a subexpression, after an initial
  // `^` if there is one, is an ordinary character. The first two need
  // nothing - with no atom before it the parser reads it as one - and `^*`
  // is the case that does, because by then the `^` is an anchor.
  EXPECT_EQ(span("*a", "*a", kBre), "0-2");
  EXPECT_EQ(span("^*a", "*a", kBre), "0-2");
  EXPECT_EQ(span("\\(*a\\)", "*a", kBre), "0-2");
  EXPECT_EQ(span("x\\|*a", "*a", kBre), "0-2");

  // An extended RE refuses the same three characters outright rather than
  // reading the `*` as a character, which is the one place the two grammars
  // disagree about what `^*` even is.
  EXPECT_EQ(compile_result("^*", kEre), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("*a", kEre), GRX_ERR_SYNTAX);
}

TEST(Posix, NoAnchorIsAQuantifiersTarget) {
  // The rule `*a` and `^*a` state is not about `^`, it is about anchors. An
  // asterisk after any of them is an ordinary character in a basic RE, which
  // is how glibc reads it: `\>*` against "a*" matches 1-2, and `\>**`
  // against "a**" matches 1-3 - a literal asterisk, then a quantifier over
  // it. Read as a repeat of the anchor, both would be nomatch.
  EXPECT_EQ(span("\\>*", "a*", kBre), "1-2");
  EXPECT_EQ(span("\\>**", "a**", kBre), "1-3");
  EXPECT_EQ(span("\\b*", "a*", kBre), "1-2");
  EXPECT_EQ(span("\\<*", "*a", kBre), "nomatch");

  // An extended RE refuses instead, for every anchor and not only for the
  // two that POSIX spells with a punctuation mark.
  EXPECT_EQ(compile_result("\\<*", kEre), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("\\>*", kEre), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("\\b*", kEre), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("\\B*", kEre), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("\\<{1}", kEre), GRX_ERR_SYNTAX);

  // An interval is not made ordinary the way an asterisk is, in either
  // grammar, so a basic RE refuses it too.
  EXPECT_EQ(compile_result("^\\{1\\}", kBre), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("\\<\\{1\\}", kBre), GRX_ERR_SYNTAX);

  // A *group* holding an anchor is an ordinary target, and stays one.
  EXPECT_EQ(span("\\(\\<\\)*", "ab", kBre), "0-0");
  EXPECT_EQ(span("(\\<)*", "ab", kEre), "0-0");
}

TEST(Posix, AnOptionalAnchorIsRefusedRatherThanMadeUnmatchable) {
  // documentation/dialects.md section 6. glibc compiles `\<\?` and then
  // never matches with it - against "" it answers nomatch, and an optional
  // assertion that declines to match the empty string cannot be a rule.
  // musl compiles the same pattern and matches, so the two references
  // disagree and there is nothing here to reproduce. Refusing says what is
  // true: the construct means nothing.
  EXPECT_EQ(compile_result("\\<\\?", kBre), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("\\>\\+", kBre), GRX_ERR_SYNTAX);
}

TEST(Posix, TheLongestMatchAtTheLeftmostStart) {
  // plan.md's WP-24. POSIX takes the longest match at the leftmost start
  // where every other dialect here takes the one the alternation reaches
  // first, and the profile has said GRX_PREFER_LEFTMOST_LONGEST for these
  // four rows since the table was written. glibc and musl agree on every
  // case below, which is what makes it a rule rather than glibc's habit.
  EXPECT_EQ(span("a|ab", "ab", kEre), "0-2");
  EXPECT_EQ(span("a|ab|abc", "abc", kEre), "0-3");
  EXPECT_EQ(span("x(a|ab)", "xab", kEre), "0-3");
  EXPECT_EQ(span("a\\|ab", "ab", kBre), "0-2");

  // On the backtracker, which is where a pattern with a backreference goes.
  // It gets there by searching *past* each match rather than stopping at
  // one, so this is the case that says the exhaustive mode runs.
  EXPECT_EQ(span("\\(ab*\\)[ab]*\\1", "ababaaa", kBre), "0-7");

  // And the first-match dialects are untouched: the preference is the
  // profile's, not the engine's.
  EXPECT_EQ(span("a|ab", "ab", GRX_SYNTAX_ECMASCRIPT), "0-1");
  EXPECT_EQ(span("a|ab", "ab", GRX_SYNTAX_PERL), "0-1");
}

TEST(Posix, AnEmptyIterationRunsOnlyWhileTheRepeatHasNotMoved) {
  // Neither "the iteration fails" nor "it succeeds and the loop stops", but
  // the two of them divided by whether the repeat has consumed anything yet.
  // Both references agree on both halves.
  //
  // Nothing consumed, so the empty iteration runs and the group takes part:
  EXPECT_EQ(group("(a*)*", "bc", kEre, 1), "0-0");
  EXPECT_EQ(group("(a*)+", "bc", kEre, 1), "0-0");

  // Something consumed, so a trailing empty iteration does not run and the
  // group keeps the last iteration that did something:
  EXPECT_EQ(group("(a|)*", "aaaa", kEre, 1), "3-4");
  EXPECT_EQ(group("(a|)+", "aaaa", kEre, 1), "3-4");
  EXPECT_EQ(group("(a*)*", "aaa", kEre, 1), "0-3");
  EXPECT_EQ(group("a(b|c?)+d", "abcd", kEre, 1), "2-3");

  // The boundary between those two is the *repeat*, not its optional tail.
  // `(b+|(c)*)+` has one mandatory copy, which consumes the "b"; if the tail
  // counted as the loop, its first iteration would be the loop's first and
  // would take the empty alternative, reporting 1-1.
  EXPECT_EQ(group("(b+|(c)*)+", "b", kEre, 1), "0-1");

  // Nested, which is what says the two registers this rule needs belong to
  // the loop that allocated them. They are used as a pair at `x` and `x - 1`
  // and are only adjacent because codegen.c takes both before generating any
  // body; an inner loop allocating between them would silently make the
  // outer loop read the inner one's position.
  EXPECT_EQ(group("((a*)*b*)+", "ab", kEre, 2), "0-1");
  EXPECT_EQ(group("(x(a*)*)+", "xx", kEre, 2), "2-2");
  EXPECT_EQ(group("(a(b*)*)+", "ab", kEre, 2), "1-2");
  EXPECT_EQ(group("((a*)+)*", "aa", kEre, 2), "0-2");

  // ECMAScript's rule is its own and does not move.
  EXPECT_EQ(group("(a*)*", "bc", GRX_SYNTAX_ECMASCRIPT, 1), "unset");
}

TEST(Posix, AnEmptyAlternativeYieldsToTheBranchBesideIt) {
  // The tie POSIX answers and this library's engines do not. `(|a)(a|)`
  // against "a" can put the `a` in either group and finish at the same
  // place; both engines take whichever path they reach first, and with the
  // empty branch written first that is the one giving group 1 nothing.
  // glibc and musl both give it the `a`, so this is not glibc's habit.
  //
  // What the rule is, exactly, was read off tools/oracle/submatch_diff.py:
  // an alternative with nothing in it is considered after the one beside
  // it. Not moved to the end - one position - and not a rule about matching
  // empty. See documentation/dialects.md section 6.
  EXPECT_EQ(group("(|a)(a|)", "a", kEre, 1), "0-1");
  EXPECT_EQ(group("(|a)(a|)", "a", kEre, 2), "1-1");
  EXPECT_EQ(group("(|ab)([ab]*)", "ab", kEre, 1), "0-2");
  EXPECT_EQ(group("(|a)a?", "a", kEre, 1), "0-1");
  EXPECT_EQ(group("\\(\\|a\\)\\(a\\|\\)", "a", kBre, 1), "0-1");

  // "Nothing in it" is about what the branch generates, not how it is
  // spelled: a repeat that runs zero times leaves no instruction behind,
  // and both references treat such a branch as the empty one it is - even
  // when it holds a group, which stays unset either way.
  EXPECT_EQ(group("(a{0}|a)(a|)", "a", kEre, 1), "0-1");
  EXPECT_EQ(group("(a{0}b{0}|a)(a|)", "a", kEre, 1), "0-1");
  EXPECT_EQ(group("((a){0}|a)(a|)", "a", kEre, 1), "0-1");
  EXPECT_EQ(group("((a){0}|a)(a|)", "a", kEre, 2), "unset");

  // And not about what a branch *can* match. `b*` and `()` can both match
  // nothing, and both references leave them exactly where they were - so
  // group 1 keeps the empty match here.
  EXPECT_EQ(group("(b*|a)(a|)", "a", kEre, 1), "0-0");
  EXPECT_EQ(group("(()|a)(a|)", "a", kEre, 1), "0-0");

  // One position, not a sort. `(|b|a)` against "a" becomes `(b||a)`, whose
  // first branch fails and whose second matches nothing, so group 1 is
  // still empty. musl answers 0-1 here; glibc answers this, and where the
  // two references differ this library follows glibc. That set is WP-26.
  EXPECT_EQ(group("(|b|a)(a|)", "a", kEre, 1), "0-0");

  // The first-match dialects are untouched, because there the order the
  // branches are written in is the whole of the meaning.
  EXPECT_EQ(group("(|a)(a|)", "a", GRX_SYNTAX_ECMASCRIPT, 1), "0-0");
  EXPECT_EQ(group("(|a)(a|)", "a", GRX_SYNTAX_PERL, 1), "0-0");
}

TEST(Posix, TheReplacementTemplateIsSedsBecausePosixHasNone) {
  // POSIX's regular expressions say nothing about substitution, so the
  // reference for these four rows is sed's `s` command rather than a regex
  // standard. Every expectation here was asked of sed 4.9 first, and
  // tools/oracle/sed_diff.py asks it again on every `make check-oracles`.
  EXPECT_EQ(replace_all("(a)(b)", "ab", "[&]", kEre), "[ab]");
  EXPECT_EQ(replace_all("(a)(b)", "ab", "[\\1]", kEre), "[a]");
  EXPECT_EQ(replace_all("(a)(b)", "ab", "\\1\\2", kEre), "ab");
  EXPECT_EQ(replace_all("(a)(b)", "ab", "&&", kEre), "abab");

  // A bare `&` is the whole match and the *escaped* one is the literal,
  // which is the reverse of every other dialect here - ECMAScript's `$&` is
  // the whole match and a bare `&` is an ampersand.
  EXPECT_EQ(replace_all("(a)(b)", "ab", "\\&", kEre), "&");
  EXPECT_EQ(replace_all("(a)(b)", "ab", "$&", GRX_SYNTAX_ECMASCRIPT), "ab");
  EXPECT_EQ(replace_all("(a)(b)", "ab", "&", GRX_SYNTAX_ECMASCRIPT), "&");

  // One digit, never two: `\10` is group one and a zero, which sed answers
  // that way even for a pattern that has ten groups.
  EXPECT_EQ(replace_all("(a)(b)", "ab", "\\10", kEre), "a0");

  // The escape is total rather than a list, so `\q` is a `q`.
  EXPECT_EQ(replace_all("(a)(b)", "ab", "[\\q]", kEre), "[q]");
  EXPECT_EQ(replace_all("(a)(b)", "ab", "[\\\\]", kEre), "[\\]");

  // A group the pattern has not got is refused, which is sed's answer too -
  // it reports "invalid reference \\3 on `s' command's RHS" and compiles
  // nothing.
  EXPECT_EQ(replace_all("(a)(b)", "ab", "\\3", kEre), "template");

  // `\0` is GNU's, not POSIX's.
  EXPECT_EQ(replace_all("(a)(b)", "ab", "[\\0]", kEre), "[ab]");
  EXPECT_EQ(replace_all("(a)(b)", "ab", "[\\0]", GRX_SYNTAX_POSIX_ERE),
      "[0]");

  // And a basic RE spells its groups with backslashes on the pattern side
  // while the template side is identical.
  EXPECT_EQ(replace_all("\\(a\\)\\(b\\)", "ab", "[\\2\\1]", kBre),
      "[ba]");
}

TEST(Posix, WhichSecondQuantifierEachGrammarAccepts) {
  // An extended RE stacks them freely: `a**` is `(a*)*` and `a{2}{3}` is
  // thirty-six a's. A basic one accepts only `\+` and `\?` as the second,
  // which is a rule found by asking glibc rather than by reading POSIX.
  EXPECT_EQ(span("a**", "aa", kEre), "0-2");
  EXPECT_EQ(span("a{2}{3}", "aaaaaa", kEre), "0-6");
  EXPECT_EQ(span("a*?", "aa", kEre), "0-2"); // `?` applied to `a*`, not lazy.

  EXPECT_EQ(span("a*\\?", "aa", kBre), "0-2");
  EXPECT_EQ(span("a*\\+", "aa", kBre), "0-2");
  EXPECT_EQ(compile_result("a**", kBre), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("a\\+*", kBre), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("a*\\{1\\}", kBre), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("a\\{1\\}\\{1\\}", kBre), GRX_ERR_SYNTAX);
}

TEST(Posix, ThereAreNoEscapesInsideABracketExpression) {
  // The rule that surprises everyone coming from Perl. `[\]]` is the class
  // holding a backslash, followed by a literal `]` - so it matches the two
  // characters `\]` and neither one alone.
  EXPECT_EQ(span("[\\]]", "\\]", kEre), "0-2");
  EXPECT_EQ(span("[\\]]", "]", kEre), "nomatch");
  EXPECT_EQ(span("[a\\]", "\\", kEre), "0-1");

  // Which is why a `]` that is to be a member is written first instead.
  EXPECT_EQ(span("[]a]", "]", kEre), "0-1");
  EXPECT_EQ(span("[^]a]", "b", kEre), "0-1");

  // A `-` first or last is itself.
  EXPECT_EQ(span("[a-]", "-", kEre), "0-1");
  EXPECT_EQ(span("[-a]", "-", kEre), "0-1");

  // A range may not be an endpoint of another, and may not run backwards.
  EXPECT_EQ(compile_result("a[1-3-5]c", kEre), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("[z-a]", kEre), GRX_ERR_SYNTAX);
}

TEST(Posix, CollatingElementsAndEquivalenceClassesInTheCLocale) {
  // POSIX has both constructs, unlike Perl and PCRE2, which is why they are
  // read here rather than refused. In the C locale - the only locale this
  // library has - each names the one character inside it, and neither
  // multi-character collating elements nor equivalence classes exist. glibc
  // answers the same way.
  EXPECT_EQ(span("[[.a.]]", "a", kEre), "0-1");
  EXPECT_EQ(span("[[=a=]]", "a", kEre), "0-1");
  EXPECT_EQ(span("[[=a=]]", "A", kEre), "nomatch");
  EXPECT_EQ(span("[[.a.]-z]", "b", kEre), "0-1");

  // More than one character names a collating element that does not exist,
  // which is a syntax error rather than an unbuilt feature - the distinction
  // documentation/dialects.md section 6 draws for the Perl family, answered
  // the other way here because POSIX has the construct.
  EXPECT_EQ(compile_result("a[[.ab.]]", kEre), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("a[[=b,=]]", kEre), GRX_ERR_SYNTAX);

  EXPECT_EQ(span("[[:alpha:]]", "x", kEre), "0-1");
  EXPECT_EQ(compile_result("[[:nosuch:]]", kEre), GRX_ERR_SYNTAX);
}

TEST(Posix, WhatEachDialectHasAndTheOthersDoNot) {
  // The feature table, asserted through the grammar rather than read back
  // from the table that declares it. A basic POSIX RE has no alternation at
  // all; an extended POSIX RE has no backreference; GNU adds both to both.
  EXPECT_EQ(compile_result("a\\|b", GRX_SYNTAX_POSIX_BRE), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("a\\|b", GRX_SYNTAX_GNU_BRE), GRX_OK);

  EXPECT_EQ(span("\\(a\\)\\1", "aa", kBre), "0-2");
  EXPECT_EQ(span("(a)\\1", "aa", kEre), "0-2");
  // POSIX's extended RE has no backreference, so `\1` is the character.
  EXPECT_EQ(span("(a)\\1", "a1", GRX_SYNTAX_POSIX_ERE), "0-2");

  // GNU's word escapes, in both of its grammars.
  EXPECT_EQ(span("\\w", "a", kBre), "0-1");
  EXPECT_EQ(span("\\w", "a", kEre), "0-1");
  // `\<` and `\>` are the two halves of a word boundary rather than the
  // whole of one: `\b` asks whether the sides differ, these ask which way.
  EXPECT_EQ(span("\\<a", "x a", kBre), "2-3");
  EXPECT_EQ(span("\\<a", "xa a", kBre), "3-4");
  EXPECT_EQ(span("a\\>", "a x", kBre), "0-1");
  EXPECT_EQ(span("\\<abc\\>", "x abc y", kEre), "2-5");
  EXPECT_EQ(compile_result("\\w", GRX_SYNTAX_POSIX_ERE), GRX_OK);
  // Without the feature it is the character `w`, not a shorthand.
  EXPECT_EQ(span("\\w", "w", GRX_SYNTAX_POSIX_ERE), "0-1");
}

TEST(Posix, ABackreferenceNeedsAGroupThatHasClosed) {
  // POSIX: a backreference refers to a *completed* subexpression. The
  // prescan counts every group before parsing starts, so the count alone
  // cannot tell an open one from a closed one.
  EXPECT_EQ(compile_result("a\\(b\\)\\1c", kBre), GRX_OK);
  EXPECT_EQ(compile_result("a\\(b\\1\\)c", kBre), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("a\\(b\\)\\2c", kBre), GRX_ERR_SYNTAX);
}

TEST(Posix, AnUnmatchedRightParenthesisIsACharacter) {
  // Spencer's own corpus calls this one out - "gag me with a right
  // parenthesis -- 1003.2 goofed here" - and glibc does it in both
  // grammars. A basic RE's `\)` is still an operator, so an unmatched one
  // is still an error: the two questions are about two spellings.
  EXPECT_EQ(span("a)", "a)", kEre), "0-2");
  EXPECT_EQ(span(")", ")", kEre), "0-1");
  EXPECT_EQ(span("(a))", "a)", kEre), "0-2");
  EXPECT_EQ(span("a)", "a)", kBre), "0-2");
  EXPECT_EQ(compile_result("\\)", kBre), GRX_ERR_SYNTAX);
}

TEST(Posix, AnIntervalWithNothingBeforeItIsRefused) {
  // The one place POSIX's "undefined" is not resolved the permissive way:
  // glibc refuses `{1}`, `{1` and `a{`, where it reads `\y` as `y`.
  EXPECT_EQ(compile_result("{1}", kEre), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("{1", kEre), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("({1}a)", kEre), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("\\{", kBre), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("^\\{1\\}", kBre), GRX_ERR_SYNTAX);

  // A backslash before an ordinary character is that character, which is
  // what both references do with the rest of POSIX's undefined space.
  EXPECT_EQ(span("\\y", "y", kEre), "0-1");
  EXPECT_EQ(span("\\.", ".", kEre), "0-1");
  EXPECT_EQ(span("\\.", "x", kEre), "nomatch");
}

TEST(Posix, DotMatchesANewlineBecauseThereIsNoOptionSayingSo) {
  // POSIX has no "dot-all" option because dot-all is what it does: `.`
  // against a newline matches in glibc with no flags at all. REG_NEWLINE is
  // what turns it off, and what makes `^` and `$` line anchors - which is
  // two rules in one option, and the reason the second is spelled
  // `options: multiline` in a vector.
  EXPECT_EQ(span(".", "\n", kEre), "0-1");
  EXPECT_EQ(span("b$", "ab\nc", kEre), "nomatch");
  EXPECT_EQ(span("b$", "ab\nc", kEre, GRX_OPT_MULTILINE), "1-2");
  EXPECT_EQ(span("^c", "ab\nc", kEre, GRX_OPT_MULTILINE), "3-4");
  // The empty run after a final newline is a line here, where PCRE2 and
  // Perl say it is not.
  EXPECT_EQ(span("^$", "abc\n", kEre, GRX_OPT_MULTILINE), "4-4");
}

TEST(Posix, RegNewlineTakesTheNewlineOutOfDotAndOutOfANegatedList) {
  // REG_NEWLINE's *other* rule, which this library spelled nowhere until
  // GRX_OPT_NEWLINE_TERMINATES existed: "a <newline> shall not be matched
  // by a period outside a bracket expression or by any form of a
  // non-matching list". Every expectation below is what glibc 2.41 and
  // musl 1.2.6 both answer; they agree on all of it.
  const uint32_t newline = GRX_OPT_MULTILINE | GRX_OPT_NEWLINE_TERMINATES;

  for (GRX_Syntax dialect : {kBre, kEre}) {
    // `.` stops reaching across a line.
    EXPECT_EQ(span(".", "\n", dialect), "0-1");
    EXPECT_EQ(span(".", "\n", dialect, newline), "nomatch");
    EXPECT_EQ(span("a.b", "a\nb", dialect), "0-3");
    EXPECT_EQ(span("a.b", "a\nb", dialect, newline), "nomatch");

    // So does a negated bracket expression, which is the half with no
    // vector of its own: the corpus has eleven cases for the anchors and
    // none for this.
    EXPECT_EQ(span("[^a]", "\n", dialect), "0-1");
    EXPECT_EQ(span("[^a]", "\n", dialect, newline), "nomatch");
    EXPECT_EQ(span("[^a-z]", "\n", dialect, newline), "nomatch");

    // A *positive* list is untouched. The standard's rule is about a
    // non-matching list, and glibc and musl both still match here - so
    // "subtract the newline from every class" would be the wrong fix.
    EXPECT_EQ(span("[\n]", "\n", dialect, newline), "0-1");
  }

  // The two bits are independent, which is why there are two of them: the
  // anchor half alone leaves `.` as it was, and this half alone leaves the
  // anchors as they were.
  EXPECT_EQ(span(".", "\n", kEre, GRX_OPT_MULTILINE), "0-1");
  EXPECT_EQ(span("b$", "ab\nc", kEre, GRX_OPT_NEWLINE_TERMINATES), "nomatch");
  EXPECT_EQ(span("[^a]", "\n", kEre, GRX_OPT_NEWLINE_TERMINATES), "nomatch");
}

TEST(Posix, TheseDialectsHaveNoFlagLetters) {
  // Their options are arguments to regcomp - REG_ICASE, REG_NEWLINE - and
  // not letters a pattern author writes, so the alphabet is empty and any
  // letter is unknown. The vector format's `options:` field exists for
  // exactly this.
  uint32_t options = 0;
  GRX_Error error;
  EXPECT_EQ(grx_options_parse(kEre, "", &options, &error), GRX_OK);
  EXPECT_EQ(options, 0u);
  EXPECT_EQ(grx_options_parse(kEre, "i", &options, &error), GRX_ERR_SYNTAX);
  EXPECT_EQ(error.diag, GRX_DIAG_UNKNOWN_FLAG);

  EXPECT_EQ(span("ABC", "xabc", kEre, GRX_OPT_CASELESS), "1-4");
  EXPECT_EQ(span("[[:lower:]]", "A", kEre, GRX_OPT_CASELESS), "0-1");
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
