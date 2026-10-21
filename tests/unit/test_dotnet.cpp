/**
 * @file
 *
 * The .NET dialect, as far as it is built: the feature mask and the option
 * alphabet. Neither needs a parser, which is why they come first - and both
 * were claims read off Microsoft's reference page until `dotnet_probe.py`
 * could contradict them.
 *
 * Every expectation here cites a case in `tests/data/probe/dotnet.json` by
 * index, and the case is one that *separates* the two answers rather than one
 * that merely exercises the construct: a row the reference answers the same
 * way under either value of the thing being asserted pins nothing. That rule
 * is the Java profile audit's, where four of its first-round discriminators
 * turned out to discriminate nothing.
 */
#include <cstdint>
#include <cstring>
#include <string>

#include "test_helpers.h"

namespace {

GRX_Result compile_dotnet(const char * text, GRX_Error * error = nullptr) {
  GRX_Regex * regex = nullptr;
  GRX_Error local;
  GRX_Result result = grx_regex_compile_with_allocator(text, std::strlen(text),
      GRX_SYNTAX_DOTNET, GRX_OPT_NONE, nullptr, nullptr,
      error ? error : &local, &regex);
  grx_regex_free(regex);
  return result;
}

bool matches_dotnet(const char * pattern, const char * subject,
    uint32_t options = GRX_OPT_NONE) {
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  if (grx_regex_compile_with_allocator(pattern, std::strlen(pattern),
          GRX_SYNTAX_DOTNET, options, nullptr, nullptr, &error,
          &regex) != GRX_OK) return false;
  GRX_Match * match = nullptr;
  if (grx_match_create(regex, nullptr, &match) != GRX_OK) {
    grx_regex_free(regex);
    return false;
  }
  int matched = 0;
  GRX_Result result = grx_regex_search(regex, subject, std::strlen(subject),
      0, GRX_ENGINE_AUTO, nullptr, match, &matched);
  grx_match_destroy(match);
  grx_regex_free(regex);
  return result == GRX_OK && matched;
}

GRX_SyntaxSpec SpecFor(GRX_Syntax syntax) {
  GRX_SyntaxSpec spec;
  EXPECT_EQ(grx_syntax_spec(syntax, &spec), GRX_OK);
  return spec;
}

uint32_t OptionsFor(const char * flags) {
  uint32_t options = 0;
  GRX_Error error;
  EXPECT_EQ(grx_options_parse(GRX_SYNTAX_DOTNET, flags, &options, &error),
      GRX_OK) << flags << ": " << error.message;
  return options;
}

} // namespace

// The bit the probe found missing. `(?i:a)` and `(?n:(a))` both compile on
// .NET 10.0.13 (probe cases in the `features` area), and the mask denied
// scoped flags until that was measured - a construct withheld from a dialect
// that has it, which is the direction that misleads a caller into writing
// the pattern a different way.
TEST(DotNetFeatures, ScopedFlagsArePresentBecauseTheReferenceCompilesThem) {
  EXPECT_TRUE(SpecFor(GRX_SYNTAX_DOTNET).features & GRX_FEATURE_SCOPED_FLAGS);
}

// The four the probe confirmed absent, each with the reference's own refusal
// beside it. Asserted together because a mask is as wrong when it claims a
// construct as when it withholds one, and only the second kind has a
// natural witness in a parse test.
TEST(DotNetFeatures, TheFourConstructsTheReferenceRefusesAreNotClaimed) {
  const uint32_t features = SpecFor(GRX_SYNTAX_DOTNET).features;
  // `a*+` is "Nested quantifier '+'" rather than a possessive quantifier.
  EXPECT_FALSE(features & GRX_FEATURE_POSSESSIVE);
  // `(a)(?1)` and `a(?R)?b` are both "Unrecognized grouping construct".
  EXPECT_FALSE(features & GRX_FEATURE_RECURSION);
  EXPECT_FALSE(features & GRX_FEATURE_SUBROUTINE);
}

// Conditionals are claimed, and the probe asked both spellings: `(a)(?(1)b|c)`
// by group number and `(?(?=a)b|c)` by assertion.
TEST(DotNetFeatures, ConditionalsAreClaimedInBothSpellings) {
  EXPECT_TRUE(SpecFor(GRX_SYNTAX_DOTNET).features & GRX_FEATURE_CONDITIONAL);
}

// The subject is text rather than bytes, which is what a UTF-16 engine's
// default has to be: a .NET string is UTF-16 and `Regex` matches over it as
// characters.
TEST(DotNetOptions, TheSubjectIsUnicodeByDefault) {
  EXPECT_TRUE(SpecFor(GRX_SYNTAX_DOTNET).default_options & GRX_OPT_UTF);
}

// The five letters `(?imnsx-imnsx:...)` takes, and no others.
TEST(DotNetOptions, TheAlphabetIsTheFiveLettersSpelledInline) {
  EXPECT_EQ(OptionsFor("i"), (uint32_t)GRX_OPT_CASELESS);
  EXPECT_EQ(OptionsFor("m"), (uint32_t)GRX_OPT_MULTILINE);
  EXPECT_EQ(OptionsFor("s"), (uint32_t)GRX_OPT_DOTALL);
  EXPECT_EQ(OptionsFor("imsx"),
      (uint32_t)(GRX_OPT_CASELESS | GRX_OPT_MULTILINE | GRX_OPT_DOTALL
          | GRX_OPT_EXTENDED));
}

// `n` is RegexOptions.ExplicitCapture, and the probe says what it does rather
// than only that it parses: `(a)(b)` under `n` reports **only** group 0, so
// an unnamed group does not merely stop capturing, it stops being a group.
TEST(DotNetOptions, ExplicitCaptureIsTheLetterN) {
  EXPECT_EQ(OptionsFor("n"), (uint32_t)GRX_OPT_NO_CAPTURE);
}

// **`x` is EXTENDED alone, where Java's is EXTENDED | EXTENDED_MORE**, and
// this is the test that would fail if the neighbouring table had been copied:
// `[a b]` under `x` matches a space on .NET 10.0.13, so whitespace inside a
// class is kept. A mask with EXTENDED_MORE would drop it.
TEST(DotNetOptions, ExtendedDoesNotReachInsideAClass) {
  EXPECT_EQ(OptionsFor("x"), (uint32_t)GRX_OPT_EXTENDED);
  EXPECT_FALSE(OptionsFor("x") & GRX_OPT_EXTENDED_MORE);
}

// The letters RegexOptions has and the pattern syntax does not. A letter for
// one of these would be a letter the reference does not take; the oracle
// driver gives them capitals of its own and says why.
TEST(DotNetOptions, OptionsWithNoInlineSpellingAreNotLetters) {
  uint32_t options = 0;
  GRX_Error error;
  for (const char * flags : {"E", "C", "R", "u", "d", "U", "l", "a"}) {
    EXPECT_EQ(grx_options_parse(GRX_SYNTAX_DOTNET, flags, &options, &error),
        GRX_ERR_SYNTAX) << "accepted " << flags;
    EXPECT_EQ(error.diag, GRX_DIAG_UNKNOWN_FLAG) << flags;
  }
}

// Java's alphabet is the near miss, and it is worth one row of its own
// because the two dialects share `imsx`: a table that drifted toward Java's
// would satisfy every assertion above. Adding Java's `d` to dotnet_flags
// fails this test and the one before it.
//
// What this row does *not* guard, though the first draft of this comment
// claimed it: the dispatch returning `java_flags` outright. That mutation
// does not build - `-Werror=unused-const-variable` rejects the orphaned
// `dotnet_flags` - so the compiler already holds that case, more firmly than
// a test could. Worth recording, because a test justified by a scenario that
// cannot occur reads as coverage it is not providing.
TEST(DotNetOptions, TheAlphabetIsNotJavas) {
  uint32_t options = 0;
  GRX_Error error;
  // `d` is Java's UNIX_LINES and `U` its UNICODE_CHARACTER_CLASS.
  EXPECT_EQ(grx_options_parse(GRX_SYNTAX_DOTNET, "d", &options, &error),
      GRX_ERR_SYNTAX);
  EXPECT_EQ(grx_options_parse(GRX_SYNTAX_DOTNET, "U", &options, &error),
      GRX_ERR_SYNTAX);
  // And `n` is .NET's where Java has no such letter. An unknown letter is
  // GRX_ERR_SYNTAX with GRX_DIAG_UNKNOWN_FLAG rather than GRX_ERR_INVALID,
  // which is reserved for a bad argument - a NULL output or a dialect out of
  // range. This assertion was written the other way first and the test caught
  // it, which is the distinction worth keeping: a caller who hands this
  // function a letter the dialect does not have has written a bad pattern,
  // not made a bad call.
  EXPECT_EQ(grx_options_parse(GRX_SYNTAX_JAVA, "n", &options, &error),
      GRX_ERR_SYNTAX);
  EXPECT_EQ(error.diag, GRX_DIAG_UNKNOWN_FLAG);
}

// The dialect is selectable. It answered GRX_ERR_UNSUPPORTED with
// GRX_DIAG_DIALECT_NOT_IMPLEMENTED until the front end was routed, which is
// what `kUnbuiltDialect` in test_pattern.cpp and test_compile.cpp used it
// for; those now name Ruby.
TEST(DotNetParser, TheDialectIsSelectable) {
  EXPECT_EQ(compile_dotnet("a"), GRX_OK);
  EXPECT_TRUE(matches_dotnet("a", "a"));
}

// Class subtraction, every row an answer the pinned .NET 10.0.13 gives
// (probe `classes` area). The three subjects together are the control: "b"
// is in the base and not the subtrahend, "a" is in both, and "-" is in
// neither - and that last one is the row that separates a subtraction from
// a class that merely contains a literal dash, which is what the PCRE
// reader would have made of it.
TEST(DotNetClasses, SubtractionRemovesTheOperandFromTheBase) {
  EXPECT_TRUE(matches_dotnet("[a-z-[aeiou]]", "b"));
  EXPECT_FALSE(matches_dotnet("[a-z-[aeiou]]", "a"));
  EXPECT_FALSE(matches_dotnet("[a-z-[aeiou]]", "-"));
}

// Nested, and the nesting is what says the operand is parsed as a class
// rather than scanned to the next `]`: c is subtracted from b-d, which is
// subtracted from a-z, so c survives.
TEST(DotNetClasses, ASubtrahendMayItselfSubtract) {
  EXPECT_TRUE(matches_dotnet("[a-z-[b-d-[c]]]", "c"));
  EXPECT_FALSE(matches_dotnet("[a-z-[b-d-[c]]]", "b"));
}

// Shorthands are sets, so they can stand on either side.
TEST(DotNetClasses, SubtractionWorksOnShorthands) {
  EXPECT_TRUE(matches_dotnet("[\\w-[\\d]]", "a"));
  EXPECT_FALSE(matches_dotnet("[\\w-[\\d]]", "5"));
}

// .NET's own rule, with .NET's own diagnostic: "A subtraction must be the
// last element". Both spellings it refuses are here, because one of them -
// a second subtraction - is the shape a reader that treated `-[` as an
// ordinary operator would accept.
TEST(DotNetClasses, ASubtractionMustBeTheLastElement) {
  GRX_Error error;
  EXPECT_EQ(compile_dotnet("[a-z-[aeiou]x]", &error), GRX_ERR_SYNTAX);
  EXPECT_EQ(error.diag, GRX_DIAG_INVALID_CLASS_SET_OP);
  EXPECT_EQ(compile_dotnet("[a-z-[aeiou]-[b]]", &error), GRX_ERR_SYNTAX);
  EXPECT_EQ(error.diag, GRX_DIAG_INVALID_CLASS_SET_OP);
}

// A bare `[` inside a class is a literal here and opens a nested set in
// Java, and this is the pair that proves it rather than asserting it: `[[a]`
// matches both of its members. It is also why `[[:alpha:]]` is no match -
// a class of six characters followed by a literal `]`, so it needs two
// characters - and so why POSIX bracket names are absent from this dialect
// by rule rather than by omission.
TEST(DotNetClasses, ABareBracketInsideAClassIsALiteral) {
  EXPECT_TRUE(matches_dotnet("[[a]", "["));
  EXPECT_TRUE(matches_dotnet("[[a]", "a"));
  EXPECT_FALSE(matches_dotnet("[[:alpha:]]", "a"));
  EXPECT_TRUE(matches_dotnet("[[:alpha:]]", "a]"));
}

// Replace and split answer now. They refused until their rules were set,
// because both read a profile field whose zero value is *another dialect*'s
// rule - GRX_SPLIT_ECMASCRIPT, and a template with no sigil - so a routed
// dialect that had not set them would have answered wrongly rather than
// failing. This test was that refusal and is now its removal.
TEST(DotNetOperations, ReplaceAndSplitAnswer) {
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  ASSERT_EQ(grx_regex_compile_with_allocator("a", 1, GRX_SYNTAX_DOTNET,
      GRX_OPT_NONE, nullptr, nullptr, &error, &regex), GRX_OK);
  GRX_Text text = {};
  // GRX_REPLACE_GLOBAL, because .NET's Regex.Replace replaces every match
  // and this API's default is the first one. The probe asks in `replace-all`
  // mode for the same reason; a first-match call compared against it would
  // be comparing two different questions, and the template tests above only
  // happen to agree because each of their subjects holds one match.
  EXPECT_EQ(grx_regex_replace(regex, "aa", 2, "[$0]", 4, GRX_REPLACE_GLOBAL,
      nullptr, nullptr, &error, &text), GRX_OK);
  EXPECT_EQ(std::string(text.data, text.length), "[a][a]");
  grx_text_free(&text);
  GRX_Split split = {};
  EXPECT_EQ(grx_regex_split(regex, "xax", 3, 0, nullptr, nullptr,
      &error, &split), GRX_OK);
  EXPECT_EQ(split.count, 2u);
  grx_split_free(&split);
  grx_regex_free(regex);
}

namespace {

// Every group's span, in the dialect's own numbering, as "lo:hi" joined by
// spaces with "-" for a group that did not take part. The numbering is the
// thing under test, so the helper reports it positionally rather than by
// name.
std::string groups_dotnet(const char * pattern, const char * subject,
    uint32_t options = GRX_OPT_NONE) {
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  if (grx_regex_compile_with_allocator(pattern, std::strlen(pattern),
          GRX_SYNTAX_DOTNET, options, nullptr, nullptr, &error,
          &regex) != GRX_OK) return "compile-failed";
  GRX_Match * match = nullptr;
  if (grx_match_create(regex, nullptr, &match) != GRX_OK) {
    grx_regex_free(regex);
    return "match-create-failed";
  }
  int matched = 0;
  std::string out;
  if (grx_regex_search(regex, subject, std::strlen(subject), 0,
          GRX_ENGINE_AUTO, nullptr, match, &matched) == GRX_OK && matched) {
    size_t count = grx_match_count(match);
    for (size_t i = 0; i < count; i++) {
      GRX_Capture capture;
      if (!out.empty()) out += " ";
      if (grx_match_group(match, i, &capture) == GRX_OK
          && capture.start != GRX_NPOS) {
        out += std::to_string(capture.start) + ":"
            + std::to_string(capture.end);
      }
      else {
        out += "-";
      }
    }
  }
  else {
    out = "nomatch";
  }
  grx_match_destroy(match);
  grx_regex_free(regex);
  return out;
}

} // namespace

// **.NET numbers unnamed groups before named ones**, which is the reverse of
// every other dialect here. Both rows are measured answers from the pinned
// .NET 10.0.13 (probe `captures` area), and the second is the one that shows
// the rule is a partition rather than a swap: `(a)(?<b>c)(d)` gives the two
// unnamed groups 1 and 2 in source order and the named one 3, so `d` is
// numbered before `c` although it is written after it.
TEST(DotNetCaptures, UnnamedGroupsAreNumberedBeforeNamedOnes) {
  EXPECT_EQ(groups_dotnet("(?<a>x)(y)", "xy"), "0:2 1:2 0:1");
  EXPECT_EQ(groups_dotnet("(a)(?<b>c)(d)", "acd"), "0:3 0:1 2:3 1:2");
}

// The two homogeneous cases, which the partition must leave in source order.
// They are the control: a rule that simply reversed the two kinds, or that
// numbered named groups from 1 regardless, would pass the rows above and
// fail one of these.
TEST(DotNetCaptures, OneKindOfGroupIsStillSourceOrder) {
  EXPECT_EQ(groups_dotnet("(a)(b)", "ab"), "0:2 0:1 1:2");
  EXPECT_EQ(groups_dotnet("(?<x>a)(?<y>b)", "ab"), "0:2 0:1 1:2");
}

// Under ExplicitCapture the base is zero rather than the count of unnamed
// groups, because the letter stops an unnamed group capturing at all - so
// there is nothing for the named ones to come after, and a named group is
// group 1. Measured: `(?<n>a)(b)` under `n` reports one group and it is "a".
TEST(DotNetCaptures, ExplicitCaptureLeavesTheNamedGroupsNumberedFromOne) {
  EXPECT_EQ(groups_dotnet("(?<n>a)(b)", "ab", GRX_OPT_NO_CAPTURE),
      "0:2 0:1");
  EXPECT_EQ(groups_dotnet("(a)(b)", "ab", GRX_OPT_NO_CAPTURE), "0:2");
}

// A name still resolves to its renumbered group, which is what says the
// renumbering reached the reference and not only the span list.
TEST(DotNetCaptures, ANamedBackreferenceFollowsTheRenumbering) {
  EXPECT_EQ(groups_dotnet("(?<n>a)\\k<n>", "aa"), "0:2 0:1");
  EXPECT_EQ(groups_dotnet("(y)(?<n>a)\\k<n>", "yaa"), "0:3 0:1 1:2");
}

namespace {

std::string replace_dotnet(const char * pattern, const char * subject,
    const char * tmpl) {
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  if (grx_regex_compile_with_allocator(pattern, std::strlen(pattern),
          GRX_SYNTAX_DOTNET, GRX_OPT_NONE, nullptr, nullptr, &error,
          &regex) != GRX_OK) return "compile-failed";
  GRX_Text text = {};
  std::string out = "error";
  if (grx_regex_replace(regex, subject, std::strlen(subject), tmpl,
          std::strlen(tmpl), GRX_REPLACE_GLOBAL, nullptr, nullptr, &error,
          &text) == GRX_OK) {
    out.assign(text.data, text.length);
  }
  grx_text_free(&text);
  grx_regex_free(regex);
  return out;
}

std::string split_dotnet(const char * pattern, const char * subject,
    size_t limit = 0) {
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  if (grx_regex_compile_with_allocator(pattern, std::strlen(pattern),
          GRX_SYNTAX_DOTNET, GRX_OPT_NONE, nullptr, nullptr, &error,
          &regex) != GRX_OK) return "compile-failed";
  GRX_Split split = {};
  std::string out = "error";
  if (grx_regex_split(regex, subject, std::strlen(subject), limit, nullptr,
          nullptr, &error, &split) == GRX_OK) {
    out.clear();
    for (size_t i = 0; i < split.count; i++) {
      if (i) out += "|";
      if (split.pieces[i].start != GRX_NPOS) {
        out.append(subject + split.pieces[i].start,
            split.pieces[i].end - split.pieces[i].start);
      }
    }
  }
  grx_split_free(&split);
  grx_regex_free(regex);
  return out;
}

} // namespace

// The replacement grammar, every row a measured answer from the pinned
// .NET 10.0.13 (probe `template` area).
TEST(DotNetTemplate, TheGrammarIsTheMeasuredOne) {
  EXPECT_EQ(replace_dotnet("b", "abc", "[$0]"), "a[b]c");
  EXPECT_EQ(replace_dotnet("(b)", "abc", "[$1]"), "a[b]c");
  EXPECT_EQ(replace_dotnet("(b)", "abc", "[${1}]"), "a[b]c");
  EXPECT_EQ(replace_dotnet("(?<n>b)", "abc", "[${n}]"), "a[b]c");
  EXPECT_EQ(replace_dotnet("b", "abc", "[$&]"), "a[b]c");
  EXPECT_EQ(replace_dotnet("b", "abc", "[$`]"), "a[a]c");
  EXPECT_EQ(replace_dotnet("b", "abc", "[$\']"), "a[c]c");
  EXPECT_EQ(replace_dotnet("b", "abc", "[$_]"), "a[abc]c");
  EXPECT_EQ(replace_dotnet("b", "abc", "[$$]"), "a[$]c");
}

// **A missing reference stands as written**, which is where this dialect and
// Java part company on a whole rule rather than a spelling: `$9` with no
// group 9 is the two characters `$9` here and an error on the JDK.
//
// The braced *name* is the row that found a gap. GRX_TMPL_MISSING_LITERAL
// had only ever reached the numeric spellings, because .NET is the first
// dialect here to pair it with a braced name; an unresolvable `${nope}`
// substituted the empty string, which is MISSING_EMPTY's answer under
// another name.
TEST(DotNetTemplate, AMissingReferenceStandsAsWritten) {
  EXPECT_EQ(replace_dotnet("b", "abc", "[$9]"), "a[$9]c");
  EXPECT_EQ(replace_dotnet("b", "abc", "[${nope}]"), "a[${nope}]c");
  EXPECT_EQ(replace_dotnet("b", "abc", "[$"), "a[$c");
}

// `$+` is the highest-*numbered* group, not the highest that participated,
// and the second row is the one that says so: `(a)(b)?` over "a" leaves
// group 2 unset and substitutes nothing, where a search for the last group
// that matched would substitute "a". The third row shows it reads the
// dialect's own numbering - the named group is number 2 under .NET's
// renumbering, so `$+` is "a" and not "b". The fourth is group 0 standing
// in when a pattern has no groups at all.
TEST(DotNetTemplate, LastGroupIsTheHighestNumberAndMayBeUnset) {
  EXPECT_EQ(replace_dotnet("(a)(b)", "ab", "[$+]"), "[b]");
  EXPECT_EQ(replace_dotnet("(a)(b)?", "a", "[$+]"), "[]");
  EXPECT_EQ(replace_dotnet("(?<n>a)(b)", "ab", "[$+]"), "[a]");
  EXPECT_EQ(replace_dotnet("a", "a", "[$+]"), "[a]");
}

// Split is two existing readings crossed, and each test below is one half.
// The walk is Python's: an empty match separates and neither end of the
// subject is special.
TEST(DotNetSplit, TheWalkKeepsBothEndsAndEmptyMatches) {
  EXPECT_EQ(split_dotnet(",", "a,b,c"), "a|b|c");
  EXPECT_EQ(split_dotnet(",", ",a,"), "|a|");
  EXPECT_EQ(split_dotnet("", "ab"), "|a|b|");
  EXPECT_EQ(split_dotnet("x*", "abc"), "|a|b|c|");
}

// The captures interleave, as Python's and ECMAScript's do and Java's and
// the two linear dialects' do not.
TEST(DotNetSplit, TheSeparatorsCapturesAppearBetweenThePieces) {
  EXPECT_EQ(split_dotnet("(,)", "a,b,c"), "a|,|b|,|c");
}

// The limit is perl's and Java's reading - pieces, with the last the unsplit
// remainder - and *not* Python's count of splits, although the walk above is
// Python's. Both rows are needed: Python's reading of 2 would give "a|b|c",
// and a rule that stopped dead at two pieces would give "a|b".
TEST(DotNetSplit, TheLimitCountsPiecesAndLeavesTheRemainderWhole) {
  EXPECT_EQ(split_dotnet(",", "a,b,c", 2), "a|b,c");
  EXPECT_EQ(split_dotnet(",", "a,b,c", 99), "a|b|c");
  EXPECT_EQ(split_dotnet(",", "a,b,c", 0), "a|b|c");
}

// --------------------------------------------------------------------------
// RegexOptions.ECMAScript
// --------------------------------------------------------------------------

// The option has no inline spelling - there is no `(?E)` - so these are
// caller-set bits, and that is the whole reason it is four option bits
// rather than a flag letter. Measured on .NET 10.0.13; the probe's
// `ecmascript` area holds the readings and dotnet_flags_diff.py compares
// every one of them.
namespace {
// `RegexOptions.ECMAScript`, as this library spells it.
const uint32_t kEcmaScript = GRX_OPT_ASCII_DIGIT | GRX_OPT_ASCII_SPACE
    | GRX_OPT_ASCII_WORD | GRX_OPT_PREFIX_BACKREFERENCE;
}  // namespace

// The three shorthands narrow to ASCII, inside a class and out. U+0661 is
// `Nd`, U+00E9 is `Ll` and U+00A0 is `Zs`, so each is in the default set and
// out of the ASCII one - which is what makes the pair of rows a measurement
// rather than a restatement.
TEST(DotNetEcmaScript, TheShorthandsNarrowToAscii) {
  EXPECT_TRUE(matches_dotnet("\\d", "١"));
  EXPECT_FALSE(matches_dotnet("\\d", "١", kEcmaScript));
  EXPECT_TRUE(matches_dotnet("\\w", "é"));
  EXPECT_FALSE(matches_dotnet("\\w", "é", kEcmaScript));
  EXPECT_TRUE(matches_dotnet("\\s", " "));
  EXPECT_FALSE(matches_dotnet("\\s", " ", kEcmaScript));
  EXPECT_FALSE(matches_dotnet("[\\w]", "é", kEcmaScript));
  EXPECT_TRUE(matches_dotnet("[^\\w]", "é", kEcmaScript));
}

// `\s` is **ASCII's** whitespace and not ECMAScript's, which is the one
// place the option's name is misleading: U+00A0, U+2028 and U+FEFF are
// whitespace to ECMAScript and none of them matches here.
TEST(DotNetEcmaScript, TheSpaceSetIsAsciisAndNotEcmaScripts) {
  for (const char * one : { " ", "\t", "\n", "\v", "\f", "\r" }) {
    EXPECT_TRUE(matches_dotnet("\\s", one, kEcmaScript)) << one;
  }
  for (const char * one : { " ", " ", "﻿", " " }) {
    EXPECT_FALSE(matches_dotnet("\\s", one, kEcmaScript)) << one;
  }
}

// `\b` is defined from `\w`, so it moves with it: there is no word boundary
// between U+00E9 and `x` by default and there is one under the option.
TEST(DotNetEcmaScript, TheWordBoundaryMovesWithTheWordSet) {
  EXPECT_FALSE(matches_dotnet("\\bx", "éx"));
  EXPECT_TRUE(matches_dotnet("\\bx", "éx", kEcmaScript));
  EXPECT_FALSE(matches_dotnet("\\Bx", "éx", kEcmaScript));
}

// What it does *not* narrow. `\p{...}` keeps its Unicode reading, the
// subtraction still subtracts, and a named backreference still refers - so
// the option is the three shorthands and the escape rule, and nothing else.
TEST(DotNetEcmaScript, ItNarrowsNothingElse) {
  EXPECT_TRUE(matches_dotnet("\\p{L}", "é", kEcmaScript));
  EXPECT_TRUE(matches_dotnet("\\p{Nd}", "١", kEcmaScript));
  EXPECT_TRUE(matches_dotnet("[a-z-[aeiou]]", "b", kEcmaScript));
  EXPECT_FALSE(matches_dotnet("[a-z-[aeiou]]", "a", kEcmaScript));
  EXPECT_TRUE(matches_dotnet("(?<n>a)\\k<n>", "aa", kEcmaScript));
}

// A numeric escape becomes the longest digit prefix that names an open
// group. `(a)\10` is group one and a literal zero under the option and octal
// U+0008 without it, and ten groups make the same text group ten.
TEST(DotNetEcmaScript, ANumericEscapeIsTheLongestPrefixThatNamesAGroup) {
  EXPECT_TRUE(matches_dotnet("(a)\\10", "a\b"));
  EXPECT_FALSE(matches_dotnet("(a)\\10", "aa0"));
  EXPECT_TRUE(matches_dotnet("(a)\\10", "aa0", kEcmaScript));
  EXPECT_FALSE(matches_dotnet("(a)\\10", "a\b", kEcmaScript));
  EXPECT_TRUE(matches_dotnet("(a)\\11", "aa1", kEcmaScript));
  const char * ten = "(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)\\10";
  EXPECT_TRUE(matches_dotnet(ten, "abcdefghijj"));
  EXPECT_TRUE(matches_dotnet(ten, "abcdefghijj", kEcmaScript));
}

// A number that names no open group is a character rather than a refusal:
// octal where the first digit is octal, and the digit itself where it is 8
// or 9. Without the option both are GRX_DIAG_INVALID_BACKREFERENCE, which
// is the diagnostic the assertions name so that a change of refusal shows
// up as one.
TEST(DotNetEcmaScript, AnUndefinedNumberIsACharacterRatherThanARefusal) {
  GRX_Error error;
  EXPECT_EQ(compile_dotnet("\\1", &error), GRX_ERR_SYNTAX);
  EXPECT_EQ(error.diag, GRX_DIAG_INVALID_BACKREFERENCE);
  EXPECT_EQ(compile_dotnet("\\8", &error), GRX_ERR_SYNTAX);
  EXPECT_EQ(error.diag, GRX_DIAG_INVALID_BACKREFERENCE);

  EXPECT_TRUE(matches_dotnet("\\1", "\x01", kEcmaScript));
  EXPECT_TRUE(matches_dotnet("\\12", "\n", kEcmaScript));
  EXPECT_TRUE(matches_dotnet("\\8", "8", kEcmaScript));
  EXPECT_TRUE(matches_dotnet("\\9", "9", kEcmaScript));
  EXPECT_TRUE(matches_dotnet("\\81", "81", kEcmaScript));
}

// **The ceiling is the groups opened so far**, so the option takes forward
// references away: `\1(a)` is U+0001 with it and a reference that cannot
// participate - and so fails - without it. This is the one thing the option
// removes, and it is also where .NET parts company with ECMAScript: Annex B
// reads the whole number against the pattern's total count, which is why
// GRX_SYNTAX_ECMASCRIPT answers `\1(a)` differently from this.
TEST(DotNetEcmaScript, TheCeilingIsTheGroupsOpenedSoFar) {
  EXPECT_FALSE(matches_dotnet("\\1(a)", "\x01" "a"));
  EXPECT_TRUE(matches_dotnet("\\1(a)", "\x01" "a", kEcmaScript));
  GRX_Error error;
  EXPECT_EQ(compile_dotnet("\\2(a)", &error), GRX_ERR_SYNTAX);
  EXPECT_TRUE(matches_dotnet("\\2(a)", "\x02" "a", kEcmaScript));
}

// Inside a bracket expression there is no reference for a prefix to be part
// of, so the option has nothing to change and `[\1]` is U+0001 either way.
TEST(DotNetEcmaScript, AClassDigitEscapeIsOctalEitherWay) {
  EXPECT_TRUE(matches_dotnet("[\\1]", "\x01"));
  EXPECT_TRUE(matches_dotnet("[\\1]", "\x01", kEcmaScript));
}

// --------------------------------------------------------------------------
// The two profile fields the audit's discriminators turned up
// --------------------------------------------------------------------------

namespace {
// Every match's span, as "s:e s:e", through the documented search loop.
std::string all_dotnet(const char * pattern, const char * subject) {
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  if (grx_regex_compile_with_allocator(pattern, std::strlen(pattern),
          GRX_SYNTAX_DOTNET, GRX_OPT_NONE, nullptr, nullptr, &error,
          &regex) != GRX_OK) return "compile-error";
  GRX_Match * match = nullptr;
  if (grx_match_create(regex, nullptr, &match) != GRX_OK) {
    grx_regex_free(regex);
    return "match-error";
  }
  GRX_SearchOptions options;
  grx_search_options_default(&options);
  int matched = 0;
  std::string out;
  size_t length = std::strlen(subject);
  GRX_Result result = grx_regex_search_ex(regex, subject, length, &options,
      match, &matched);
  // A guard rather than a bare loop: the field under test is the one that
  // decides whether this loop terminates, so a wrong value must fail the
  // assertion and not hang the suite.
  for (int guard = 0; result == GRX_OK && matched && guard < 32; guard++) {
    GRX_Capture capture;
    grx_match_group(match, 0, &capture);
    if (!out.empty()) out += " ";
    out += std::to_string(capture.start) + ":" + std::to_string(capture.end);
    result = grx_regex_search_next(regex, subject, length, &options, match,
        &matched);
  }
  grx_match_destroy(match);
  grx_regex_free(regex);
  return out;
}
}  // namespace

// `\G` holds where the previous match *ended*, so a failed attempt does not
// move it and the loop stops - perl's rule, not PCRE2's. This field was left
// unset on the stated ground that no case divided the two values, which was
// true of the cases the probe held: section 5.10 says they differ only after
// a failure forces the loop to advance, and `\G.` over "abc" never fails.
// `\Ga*` over "baac" does, and .NET answers it with one match where ATTEMPT
// gives four.
TEST(DotNetProfile, TheContiguousAnchorHoldsAtThePreviousMatchEnd) {
  EXPECT_EQ(all_dotnet("\\Ga*", "baac"), "0:0");
  EXPECT_EQ(all_dotnet("\\Gb*", "baac"), "0:1 1:1");
  // The control: a row where both rules agree, so that the two above are
  // seen to be about the field and not about `\G` in general.
  EXPECT_EQ(all_dotnet("\\Ga", "aab"), "0:1 1:2");
}

// `\p{^L}` is Java's answer and not Perl's: .NET refuses the caret inside
// the braces and wants `\P{L}`. All three spellings compiled here until the
// field was set, and the diagnostic is asserted so that a change of refusal
// shows up as one rather than as a still-passing test.
TEST(DotNetProfile, ACaretInsideAPropertyNameIsARefusal) {
  GRX_Error error;
  EXPECT_EQ(compile_dotnet("\\p{^L}", &error), GRX_ERR_SYNTAX);
  EXPECT_EQ(error.diag, GRX_DIAG_INVALID_PROPERTY_SYNTAX);
  EXPECT_EQ(compile_dotnet("\\P{^L}"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("[\\p{^L}]"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("\\P{L}"), GRX_OK);
}

namespace {
// The group numbers and names a pattern has, as "n:name" with `-` for an
// unnamed group. The numbering is what no span can reveal: every group can
// hold the right text under the wrong number and every span still match.
std::string naming_dotnet(const char * pattern) {
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  if (grx_regex_compile_with_allocator(pattern, std::strlen(pattern),
          GRX_SYNTAX_DOTNET, GRX_OPT_NONE, nullptr, nullptr, &error,
          &regex) != GRX_OK) return "compile-error";
  std::string out;
  size_t count = grx_regex_capture_count(regex);
  for (size_t i = 0; i <= count; i++) {
    const char * name = grx_regex_capture_name(regex, i);
    if (i) out += " ";
    out += std::to_string(i) + ":" + (name ? name : "-");
  }
  grx_regex_free(regex);
  return out;
}
}  // namespace

// **A repeated name is one group on .NET, not two groups sharing a name.**
// PCRE2's `J` gives two slots with one name; .NET allocates the name once,
// at its first occurrence in the named-group ordering, and every later
// occurrence writes to that slot. So the count does not grow and the group
// holds whichever occurrence last participated.
//
// This library refused the construct until the naming differential existed
// and asked. Nothing else could have: no driver reported group numbers, so
// the whole of .NET's numbering rested on this file, whose expectations and
// the implementation were written by the same hand from the same reading.
TEST(DotNetCaptures, ARepeatedNameIsOneGroup) {
  EXPECT_EQ(naming_dotnet("(?<n>a)(?<n>b)"), "0:- 1:n");
  EXPECT_EQ(groups_dotnet("(?<n>a)(?<n>b)", "ab"), "0:2 1:2");
  // Either branch of an alternation writes the same slot.
  EXPECT_EQ(groups_dotnet("(?<n>a)|(?<n>b)", "a"), "0:1 0:1");
  EXPECT_EQ(groups_dotnet("(?<n>a)|(?<n>b)", "b"), "0:1 0:1");
}

// The merge happens inside .NET's own ordering, so the unnamed groups still
// number first and the one named slot comes after both of them: four groups
// and not five, with only the last named.
TEST(DotNetCaptures, TheMergedNameStillComesAfterEveryUnnamedGroup) {
  EXPECT_EQ(naming_dotnet("(x)(?<n>a)|(y)(?<n>b)"), "0:- 1:- 2:- 3:n");
  EXPECT_EQ(groups_dotnet("(x)(?<n>a)|(y)(?<n>b)", "yb"), "0:2 - 0:1 1:2");
}

// The two spellings that are still refused, and why they are different from
// a repeated name: a partition can express one slot written twice, and
// cannot express a numbering with a gap in it or one where an explicit
// number merges with an unnamed group's.
TEST(DotNetCaptures, AnExplicitGroupNumberIsStillRefused) {
  GRX_Error error;
  EXPECT_EQ(compile_dotnet("(?<5>a)(b)", &error), GRX_ERR_SYNTAX);
  EXPECT_EQ(error.diag, GRX_DIAG_INVALID_GROUP_NAME);
  EXPECT_EQ(compile_dotnet("(a)(?<1>b)"), GRX_ERR_SYNTAX);
}

// **.NET takes short General_Category names and its own `Is` prefix, and
// nothing else.** `GRX_PROPERTY_STRICT` is not this rule: that is
// ECMAScript's, where `\p{Letter}` is a canonical alias and so is right to
// accept, which is why the strictest value of that enum still let two
// spellings through here. Each expectation is a measured .NET 10.0.13
// answer; the long name and the lower-cased one are the two this library
// accepted before the gate existed.
TEST(DotNetClasses, OnlyShortCategoryNamesAndTheIsPrefix) {
  EXPECT_EQ(compile_dotnet("\\p{Lu}"), GRX_OK);
  EXPECT_EQ(compile_dotnet("\\p{Nd}"), GRX_OK);
  EXPECT_EQ(compile_dotnet("\\p{L}"), GRX_OK);
  GRX_Error error;
  EXPECT_EQ(compile_dotnet("\\p{Letter}", &error), GRX_ERR_SYNTAX);
  EXPECT_EQ(error.diag, GRX_DIAG_UNKNOWN_PROPERTY);
  EXPECT_EQ(compile_dotnet("\\p{lu}"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("\\p{Alphabetic}"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("\\p{Script=Greek}"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("\\p{InGreek}"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("\\p{IsL}"), GRX_ERR_SYNTAX);
}

// --------------------------------------------------------------------------
// The two axes the exhaustive sweep found unasked
// --------------------------------------------------------------------------

// `$` without multiline holds before a *final* newline and nowhere else, so
// a newline in the middle of the subject is what divides .NET's rule from
// "`$` is always a line anchor". Every row the inventory held used a final
// newline, which both readings answer alike - the sweep caught this field
// with one of its two alternative values and that is what said so.
TEST(DotNetAnchors, DollarHoldsOnlyBeforeAFinalNewline) {
  EXPECT_FALSE(matches_dotnet("a$", "a\nb"));
  EXPECT_TRUE(matches_dotnet("b$", "a\nb"));
  EXPECT_TRUE(matches_dotnet("a$", "a\n"));
}

// An unpaired surrogate is a legal `char` in a .NET string, so the engine
// matches over one without complaint - and this library reads such a
// subject as WTF-8 under GRX_OPT_WTF8, reporting three bytes for it. The
// `\p{Cs}` and `\p{L}` pair is the whole reason .NET's properties cannot
// see a character above U+FFFF: the engine is looking at two surrogates,
// and a surrogate's category is `Cs`.
//
// The inventory asked none of this until 2026-10-21: the driver had a `w:`
// mode nothing used, which is why mutating `subject_is_text` and
// `subject_is_utf16` failed nothing.
TEST(DotNetUnicode, AnUnpairedSurrogateIsText) {
  const uint32_t wtf8 = GRX_OPT_WTF8 | GRX_OPT_UTF;
  // "\xED\xA0\x80" is U+D800 in WTF-8.
  EXPECT_TRUE(matches_dotnet(".", "\xED\xA0\x80", wtf8));
  EXPECT_TRUE(matches_dotnet("\\p{Cs}", "\xED\xA0\x80", wtf8));
  EXPECT_FALSE(matches_dotnet("\\p{L}", "\xED\xA0\x80", wtf8));
  EXPECT_TRUE(matches_dotnet("[\\s\\S]", "\xED\xA0\x80", wtf8));
  // Without the option the subject is not well-formed UTF-8 and is refused
  // rather than answered, which is the bit doing something.
  EXPECT_FALSE(matches_dotnet(".", "\xED\xA0\x80"));
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// --------------------------------------------------------------------------
// The closed escape alphabet
// --------------------------------------------------------------------------

// **.NET's escape alphabet is closed, and seven letters were wrong.**
//
// A letter `Regex` has no meaning for is "Unrecognized escape sequence \X"
// there, where the Perl family reads the identity escape. `\X` reached this
// library's parser through a `case 'K': case 'R': case 'X':` arm with no
// flavour guard at all, and `\h`, `\H`, `\V`, `\K`, `\R`, `\X` and `\N` were
// all accepted here and all refused on .NET 10.0.13.
//
// The inventory had no row for any of them, which is the part worth
// remembering: eleven green differentials and a coverage check reporting
// "0 rows asked by nothing" coexisted with the divergence, because that
// check scores the rows that exist and cannot speak about the rows that
// should. `tests/data/probe/dotnet.json`'s `escape-alphabet` area is now the
// whole alphabet in both contexts, 122 rows, so this is a gate rather than a
// reading - these tests are the offline half of it.
TEST(DotNetEscapes, TheSevenLettersThisFamilyReadsAndDotNetRefuses) {
  GRX_Error error;
  EXPECT_EQ(compile_dotnet("\\X", &error), GRX_ERR_SYNTAX);
  EXPECT_EQ(error.diag, GRX_DIAG_INVALID_ESCAPE);
  EXPECT_EQ(compile_dotnet("\\K"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("\\R"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("\\h"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("\\H"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("\\V"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("\\N"), GRX_ERR_SYNTAX);
  // The named code point and the relative backreference go with them, and
  // `\o` has no .NET spelling at all.
  EXPECT_EQ(compile_dotnet("\\N{U+0041}"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("(a)\\g1"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("\\o{101}"), GRX_ERR_SYNTAX);
  // `\v` is *not* one of them, and routing it through the shared shorthand
  // table is what the refusal above must not do: it is the vertical tab on
  // .NET, one code point, where the Perl family's vertical-space shorthand
  // is five.
  EXPECT_EQ(compile_dotnet("\\v"), GRX_OK);
  EXPECT_TRUE(matches_dotnet("\\v", "\v"));
  EXPECT_FALSE(matches_dotnet("\\v", "\n"));
}

// Inside a class the alphabet is smaller: the anchors and the named
// backreference drop out, and `\b` does not, being the backspace there and
// the word boundary outside - which is the Perl family's rule too.
TEST(DotNetEscapes, TheClassAlphabetDropsTheAnchorsAndTheNamedBackreference) {
  EXPECT_EQ(compile_dotnet("[\\b]"), GRX_OK);
  EXPECT_EQ(compile_dotnet("[\\d]"), GRX_OK);
  EXPECT_EQ(compile_dotnet("[\\p{L}]"), GRX_OK);
  GRX_Error error;
  EXPECT_EQ(compile_dotnet("[\\h]", &error), GRX_ERR_SYNTAX);
  EXPECT_EQ(error.diag, GRX_DIAG_INVALID_CLASS_ITEM);
  EXPECT_EQ(compile_dotnet("[\\H]"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("[\\V]"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("[\\g]"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("(?<n>a)[\\k<n>]"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("(?<n>a)\\k<n>"), GRX_OK);
}

// `\x` takes *exactly* two hex digits, which is Python's rule and not the
// Perl family's. The letter itself is in .NET's alphabet, so the closed
// table cannot carry this - what differs is the argument.
TEST(DotNetEscapes, HexTakesExactlyTwoDigitsAndHasNoBracedForm) {
  EXPECT_EQ(compile_dotnet("\\x41"), GRX_OK);
  // `\x41` then the two literal characters "42", which is why this compiles
  // and is not a four-digit escape.
  EXPECT_TRUE(matches_dotnet("\\x4142", "A42"));
  EXPECT_EQ(compile_dotnet("\\x4"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("\\x{41}"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("\\x"), GRX_ERR_SYNTAX);
  // `\u` is four digits and is .NET's, where `\U` is not.
  EXPECT_EQ(compile_dotnet("\\u0041"), GRX_OK);
  EXPECT_EQ(compile_dotnet("\\u41"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("\\U0001F600"), GRX_ERR_SYNTAX);
}

// .NET has no `(*...)` construct, and *how* it has none is the point. `(`
// opens an ordinary group there and the `*` is then a quantifier with
// nothing to repeat, so the refusal is a quantifier diagnostic rather than
// an invalid-group one. Java's arm refuses `(*` outright; reproducing
// .NET's route costs nothing and keeps the diagnostic a reader sees honest.
TEST(DotNetEscapes, ThereAreNoVerbsAndTheRefusalIsAQuantifierError) {
  GRX_Error error;
  EXPECT_EQ(compile_dotnet("(*FAIL)", &error), GRX_ERR_SYNTAX);
  EXPECT_EQ(error.diag, GRX_DIAG_NOTHING_TO_REPEAT);
  EXPECT_EQ(compile_dotnet("(*SKIP)"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("(*ACCEPT)"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_dotnet("(*:n)"), GRX_ERR_SYNTAX);
}
