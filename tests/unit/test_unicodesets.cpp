/**
 * @file
 *
 * UnicodeSets mode: the `v` flag.
 *
 * `v` is not `u` with extras. Inside a class it is a different grammar:
 * `[a-z]--[aeiou]` is a subtraction where `u` reads eight characters,
 * `[a|b]` is a syntax error where `u` reads three, and a class may contain
 * *strings* - `[\q{abc}]` matches three characters, which no set of code
 * points can express.
 *
 * Every expectation here came from Node 22.23 being asked the same thing.
 * The broad check is the differential harness, which runs 960,000 patterns
 * through both implementations with `v` and `iv` among the flag sets; these
 * are the rows worth naming, either because they are the rule or because
 * getting them wrong would be silent.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <string>

#include "test_helpers.h"

namespace {

uint32_t options_for(const char * flags) {
  uint32_t options = 0;
  GRX_Error error;
  grx_error_clear(&error);
  EXPECT_EQ(grx_options_parse(GRX_SYNTAX_ECMASCRIPT, flags, &options, &error),
      GRX_OK)
      << flags << ": " << error.message;
  return options;
}

/** Whether a pattern compiles, and what it matches. */
class Compiled {
public:
  Compiled(const char * pattern, const char * flags = "v") {
    GRX_Error error;
    grx_error_clear(&error);
    result_ = grx_regex_compile_with_allocator(pattern, strlen(pattern),
        GRX_SYNTAX_ECMASCRIPT, options_for(flags), nullptr, nullptr, &error,
        &regex_);
    diag_ = error.diag;
  }
  Compiled(const Compiled &) = delete;
  Compiled & operator=(const Compiled &) = delete;
  ~Compiled() { grx_regex_free(regex_); }

  bool ok() const { return result_ == GRX_OK; }
  GRX_Result result() const { return result_; }
  GRX_Diag diag() const { return diag_; }

  /** "start..end" of the first match, or "none". */
  std::string find(const std::string & subject) const {
    if (!regex_) {
      return "<did not compile>";
    }
    GRX_Match * match = nullptr;
    if (grx_match_create(regex_, nullptr, &match) != GRX_OK) {
      return "<oom>";
    }
    int matched = 0;
    GRX_Result result = grx_regex_search(regex_, subject.data(),
        subject.size(), 0, GRX_ENGINE_AUTO, nullptr, match, &matched);
    std::string out = "<error>";
    if (result == GRX_OK) {
      GRX_Capture span {};
      grx_match_span(match, &span);
      out = matched ? subject.substr(span.start, span.end - span.start)
                    : std::string("none");
    }
    grx_match_destroy(match);
    return out;
  }

private:
  GRX_Regex * regex_ = nullptr;
  GRX_Result result_ = GRX_ERR_INTERNAL;
  GRX_Diag diag_ = GRX_DIAG_NONE;
};

/** What `new RegExp(pattern, "v")` does to `subject`, as a string. */
std::string finds(const char * pattern, const std::string & subject,
    const char * flags = "v") {
  Compiled regex(pattern, flags);
  if (!regex.ok()) {
    return "<syntax error>";
  }
  return regex.find(subject);
}

/** Whether `new RegExp(pattern, "v")` throws. */
bool rejects(const char * pattern, const char * flags = "v") {
  return !Compiled(pattern, flags).ok();
}

} // namespace

// --------------------------------------------------------------------------
// The flag itself
// --------------------------------------------------------------------------

TEST(UnicodeSets, TheFlagImpliesUnicodeAndExcludesIt) {
  uint32_t options = options_for("v");
  EXPECT_TRUE(options & GRX_OPT_UNICODE_SETS);
  EXPECT_TRUE(options & GRX_OPT_UTF);

  // `u` and `v` together are a flag-string error, in either order.
  uint32_t ignored = 0;
  GRX_Error error;
  grx_error_clear(&error);
  EXPECT_NE(grx_options_parse(GRX_SYNTAX_ECMASCRIPT, "uv", &ignored, &error),
      GRX_OK);
  EXPECT_EQ(error.diag, GRX_DIAG_CONFLICTING_FLAGS);
  EXPECT_NE(grx_options_parse(GRX_SYNTAX_ECMASCRIPT, "vu", &ignored, &error),
      GRX_OK);
  EXPECT_EQ(error.diag, GRX_DIAG_CONFLICTING_FLAGS);
}

TEST(UnicodeSets, TheOrdinaryClassGrammarStillWorks) {
  EXPECT_EQ(finds("[a-z]", "Q9m"), "m");
  EXPECT_EQ(finds("[abc]", "cx"), "c");
  EXPECT_EQ(finds("[^a-c]", "abcd"), "d");
  EXPECT_EQ(finds("[\\d]", "x7"), "7");
  EXPECT_EQ(finds("[\\p{Lu}]", "aB"), "B");
  // `[]` is empty and `[^]` is everything, as without `v`.
  EXPECT_EQ(finds("[]", "a"), "none");
  EXPECT_EQ(finds("[^]", "a"), "a");
}

// --------------------------------------------------------------------------
// Set operations
// --------------------------------------------------------------------------

TEST(UnicodeSets, SubtractionAndIntersection) {
  EXPECT_EQ(finds("[[a-z]--[aeiou]]", "aeiobcd"), "b");
  EXPECT_EQ(finds("[[a-z]&&[b-d]]", "abcde"), "b");
  EXPECT_EQ(finds("[[a-z]--[aeiou]--[xyz]]", "xyzb"), "b");
  EXPECT_EQ(finds("[[a-z]&&[b-y]&&[c-x]]", "abc"), "c");
  // An empty intersection matches nothing, which is a result and not an
  // error: `[a&&b]` is a legal class with no members.
  EXPECT_EQ(finds("[a&&b]", "a"), "none");
}

TEST(UnicodeSets, NestedClassesUnion) {
  EXPECT_EQ(finds("[[a-c][x-z]]", "by"), "b");
  EXPECT_EQ(finds("[[a-c]x[y-z]]", "x"), "x");
  EXPECT_EQ(finds("[^[a-c]]", "abcd"), "d");
}

TEST(UnicodeSets, AnOperatorMayNotBeMixedWithUnion) {
  // ECMA-262 gives the operators no precedence at all, so that a reader
  // never has to work one out. The cost is that mixing them is an error
  // rather than an association.
  EXPECT_TRUE(rejects("[[a][b]&&[b]]"));
  EXPECT_TRUE(rejects("[a&&b--c]"));
  EXPECT_TRUE(rejects("[a-b--c]"));
  EXPECT_TRUE(rejects("[a&&&&b]"));
  EXPECT_TRUE(rejects("[&&]"));
}

// --------------------------------------------------------------------------
// Strings in a class
// --------------------------------------------------------------------------

TEST(UnicodeSets, StringDisjunctionsMatchLongestFirst) {
  // The rule that leftmost-first would otherwise get wrong: without the
  // ordering, `[\q{abc|ab|a}]` against "abc" reports "a".
  EXPECT_EQ(finds("[\\q{abc|ab|a}]", "abcd"), "abc");
  EXPECT_EQ(finds("[\\q{ab}c]", "abc"), "ab");
  EXPECT_EQ(finds("[\\q{a|bc}--\\q{a}]", "abc"), "bc");
  // The empty alternative is a member and sorts last.
  EXPECT_EQ(finds("[\\q{}]", "x"), "");
  EXPECT_EQ(finds("[\\q{|ab}]", "ab"), "ab");
}

TEST(UnicodeSets, AOneCharacterStringIsAnOrdinaryCharacter) {
  // Which is why `[^\q{a}]` is legal where `[^\q{ab}]` is not: the member is
  // a code point, and the complement of a set of code points is a set of
  // code points.
  EXPECT_EQ(finds("[^\\q{a}]", "ab"), "b");
  EXPECT_EQ(finds("[\\q{a}]", "ba"), "a");
  EXPECT_TRUE(rejects("[^\\q{ab}]"));
}

TEST(UnicodeSets, AClassThatMayContainStringsMayNotBeNegated) {
  EXPECT_TRUE(rejects("[^\\q{ab}]"));
  EXPECT_TRUE(rejects("[^[\\q{ab}]]"));
  EXPECT_TRUE(rejects("[^\\p{RGI_Emoji}]"));
  // Subtraction keeps the left operand's strings, so this may not be
  // negated either.
  EXPECT_TRUE(rejects("[^\\q{ab}--x]"));

  // Intersection keeps strings only where *every* operand has them, so
  // these may: whatever the result is, it has no strings in it.
  EXPECT_FALSE(rejects("[^x&&\\q{ab}]"));
  EXPECT_FALSE(rejects("[^[a]&&[\\q{ab}]]"));
}

// --------------------------------------------------------------------------
// Properties of strings
// --------------------------------------------------------------------------

TEST(UnicodeSets, PropertiesOfStringsMatchSequences) {
  // U+1F1FA U+1F1F8, the flag of the United States: two regional indicators
  // that are one RGI emoji and are not in any character property.
  const std::string flag = "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8";
  EXPECT_EQ(finds("\\p{RGI_Emoji}", flag), flag);
  EXPECT_EQ(finds("[\\p{RGI_Emoji}]", flag), flag);
  EXPECT_EQ(finds("[\\p{RGI_Emoji_Flag_Sequence}]", flag), flag);
  // It is a flag sequence and not a keycap, so the wrong property misses it
  // rather than matching half of it.
  EXPECT_EQ(finds("[\\p{Emoji_Keycap_Sequence}]", flag), "none");

  // U+0031 U+FE0F U+20E3, keycap digit one.
  const std::string keycap = "1\xEF\xB8\x8F\xE2\x83\xA3";
  EXPECT_EQ(finds("[\\p{Emoji_Keycap_Sequence}]", keycap), keycap);
  EXPECT_EQ(finds("[\\p{RGI_Emoji}]", keycap), keycap);
}

TEST(UnicodeSets, APropertyOfStringsIsAnAtomAsWellAsAnOperand) {
  // `\p{RGI_Emoji}` outside a class is the same set; it was rejected there
  // until the differential harness found 620 patterns Node accepted and
  // this library did not.
  const std::string flag = "\xF0\x9F\x87\xBA\xF0\x9F\x87\xB8";
  EXPECT_EQ(finds("^\\p{RGI_Emoji}$", flag), flag);
  EXPECT_FALSE(rejects("\\p{Basic_Emoji}"));
}

TEST(UnicodeSets, APropertyOfStringsMayNotBeNegatedOrUsedWithoutV) {
  EXPECT_TRUE(rejects("[\\P{RGI_Emoji}]"));
  EXPECT_TRUE(rejects("\\P{RGI_Emoji}"));
  // Without `v` the name is not a property at all.
  EXPECT_TRUE(rejects("\\p{RGI_Emoji}", "u"));
  EXPECT_TRUE(rejects("[\\p{RGI_Emoji}]", "u"));
}

// --------------------------------------------------------------------------
// What must be escaped
// --------------------------------------------------------------------------

TEST(UnicodeSets, SyntaxCharactersMustBeEscaped) {
  // `-` is a syntax character in `v` mode, where in `u` a trailing one is a
  // literal. This is the change most likely to break a pattern that was
  // written for `u` and had its flag changed.
  EXPECT_TRUE(rejects("[-]"));
  EXPECT_TRUE(rejects("[a-]"));
  EXPECT_TRUE(rejects("[a|b]"));
  EXPECT_TRUE(rejects("[a{]"));
  EXPECT_EQ(finds("[\\-]", "a-b"), "-");
  EXPECT_EQ(finds("[\\|]", "a|b"), "|");

  // The same patterns without `v`.
  EXPECT_FALSE(rejects("[-]", "u"));
  EXPECT_FALSE(rejects("[a-]", "u"));
  EXPECT_FALSE(rejects("[a|b]", "u"));
}

TEST(UnicodeSets, ReservedDoublePunctuatorsMustBeEscaped) {
  // Reserved rather than forbidden: `&&` and `--` mean something already,
  // and the other twelve are held back so a future edition can give them a
  // meaning without breaking a pattern that wrote them as two characters.
  for (const char * pattern : {"[!!]", "[~~]", "[##]", "[$$]", "[::]",
           "[<<]", "[==]", "[>>]", "[??]", "[@@]", "[%%]", "[**]", "[++]",
           "[,,]", "[..]", "[;;]", "[``]"}) {
    EXPECT_TRUE(rejects(pattern)) << pattern;
  }
  // One of them is fine, and so is an escaped pair.
  EXPECT_EQ(finds("[!]", "a!"), "!");
  EXPECT_EQ(finds("[&]", "a&"), "&");
  EXPECT_EQ(finds("[\\!\\!]", "a!"), "!");
  // `[^^]` is a negated class containing one `^`, not a reserved pair: the
  // first `^` is the negation.
  EXPECT_FALSE(rejects("[^^]"));
}

// --------------------------------------------------------------------------
// Case folding
// --------------------------------------------------------------------------

TEST(UnicodeSets, CaselessFoldingAppliesToStringsToo) {
  // Each code point of a string member is folded as it lowers, the same way
  // a caseless literal is, so `[\q{AB}]` under `iv` matches "ab".
  EXPECT_EQ(finds("[\\q{AB}]", "ab", "iv"), "ab");
  EXPECT_EQ(finds("[\\q{ab}]", "AB", "iv"), "AB");
  EXPECT_EQ(finds("[\\q{ab}]", "AB", "v"), "none");
  EXPECT_EQ(finds("[[a-z]--[aeiou]]", "AEIOB", "iv"), "B");
}

TEST(UnicodeSets, CaseFoldingAppliesToEveryOperandShape) {
  // This is the one place in the suite where Node is *not* followed. Under
  // `iv` it does not fold a class-set operand that is a bare character or a
  // one-character `\q{}`, so it answers "no match" for four of these six -
  // and it answers "match" for `[[a]&&a]` while answering "no match" for
  // `[a&&[a]]`, which is the same intersection written the other way round.
  // Intersection is commutative; that asymmetry is a defect rather than a
  // rule, and following it would mean disagreeing with ECMA-262 and with
  // every other engine in order to agree with this one.
  //
  // documentation/dialects.md section 8.6.1 has the table and the reasoning.
  // Node 22.23's answer is in the comment beside each row so that a later
  // Node can be compared against it without re-deriving any of this.
  EXPECT_EQ(finds("[abc]", "A", "iv"), "A");        // Node: matches
  EXPECT_EQ(finds("[[a]&&a]", "A", "iv"), "A");     // Node: matches
  EXPECT_EQ(finds("[a&&[a]]", "A", "iv"), "A");     // Node: no match
  EXPECT_EQ(finds("[a&&a]", "A", "iv"), "A");       // Node: no match
  EXPECT_EQ(finds("[a--b]", "A", "iv"), "A");       // Node: no match
  EXPECT_EQ(finds("[\\q{a}]", "A", "iv"), "A");     // Node: no match
  EXPECT_EQ(finds("[\\q{ss}]", "SS", "iv"), "SS");  // Node: matches
}

TEST(UnicodeSets, TheNegationRuleDiffersBetweenUAndV) {
  // dialects.md section 8.4 predicted this before either mode was written:
  // `v` applies MaybeSimpleCaseFolding to an operand before complementing it
  // and `u` does not, so `[^\P{Lu}]` means different things under the two.
  // Both columns are Node 22.23's.
  //
  // It also found a defect in `u` mode, which had been folding before the
  // complement for *every* item. A shorthand is right to - ECMA-262
  // 22.2.2.9.3 builds the widening into WordCharacters itself, which is why
  // `\W` under `iu` excludes U+017F - but a property is not.
  EXPECT_EQ(finds("[^\\P{Lu}]", "aAK", "iu"), "none");
  EXPECT_EQ(finds("[^\\P{Lu}]", "aAK", "iv"), "a");
  EXPECT_EQ(finds("[\\P{Lu}]", "A", "iu"), "A");
  EXPECT_EQ(finds("[\\P{Lu}]", "A", "iv"), "none");

  // Without `i` the two agree, because there is no folding to differ about.
  EXPECT_EQ(finds("[^\\P{Lu}]", "aA", "u"), "A");
  EXPECT_EQ(finds("[^\\P{Lu}]", "aA", "v"), "A");

  // And the shorthand keeps its widening in both.
  const std::string long_s = "\xC5\xBF"; // U+017F
  EXPECT_EQ(finds("\\W", long_s, "iu"), "none");
  EXPECT_EQ(finds("\\W", long_s, "u"), long_s);
  EXPECT_EQ(finds("\\w", long_s, "iu"), long_s);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
