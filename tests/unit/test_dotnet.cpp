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

// Replace and split refuse, and refusing is the point: both dispatch on a
// profile field whose zero value is *another dialect*'s rule, so a routed
// dialect that has not set them answers wrongly instead of failing.
// `GRX_SyntaxSpec::split`'s zero is GRX_SPLIT_ECMASCRIPT and an unset
// template has sigil 0, which would copy `$1` through as literal text.
TEST(DotNetOperations, ReplaceAndSplitRefuseUntilTheyAreBuilt) {
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  ASSERT_EQ(grx_regex_compile_with_allocator("a", 1, GRX_SYNTAX_DOTNET,
      GRX_OPT_NONE, nullptr, nullptr, &error, &regex), GRX_OK);
  GRX_Text text = {};
  EXPECT_EQ(grx_regex_replace(regex, "aa", 2, "$0", 2, 0, nullptr, nullptr,
      &error, &text), GRX_ERR_UNSUPPORTED);
  GRX_Split split = {};
  EXPECT_EQ(grx_regex_split(regex, "aa", 2, GRX_NPOS, nullptr, nullptr,
      &error, &split), GRX_ERR_UNSUPPORTED);
  grx_regex_free(regex);
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
