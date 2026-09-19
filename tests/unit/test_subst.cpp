/**
 * @file
 *
 * Replacement and splitting, against Node.
 *
 * Every expectation here was produced by running the same pattern, subject
 * and template through Node 22.23 and writing down what came back. That
 * matters more than usual for this package, because a replacement template
 * is a grammar nobody remembers correctly: `$12` means group 1 followed by
 * the digit 2 in a pattern with two groups and group 12 in a pattern with
 * twelve, `$<x>` is literal text in a pattern with no named groups and the
 * empty string in a pattern that has some, and `$0` is never a reference at
 * all. None of those would have been written this way from memory.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <string>
#include <vector>

#include "test_helpers.h"

namespace {

/** A compiled regex that frees itself. */
class Regex {
public:
  Regex(const char * pattern, uint32_t options = GRX_OPT_UTF) {
    result_ = grx_regex_compile(pattern, GRX_SYNTAX_ECMASCRIPT, options,
        &regex_);
  }
  Regex(const Regex &) = delete;
  Regex & operator=(const Regex &) = delete;
  ~Regex() { grx_regex_free(regex_); }

  GRX_Regex * get() const { return regex_; }
  bool ok() const { return result_ == GRX_OK; }

private:
  GRX_Regex * regex_ = nullptr;
  GRX_Result result_ = GRX_ERR_INTERNAL;
};

/** `subject.replace(new RegExp(pattern, flags), replacement)`. */
std::string replaced(const char * pattern, const std::string & subject,
    const std::string & replacement, uint32_t flags = GRX_REPLACE_GLOBAL,
    GRX_Engine engine = GRX_ENGINE_AUTO) {
  Regex regex(pattern);
  if (!regex.ok()) {
    return "<compile failed>";
  }

  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.engine = engine;

  GRX_Text text {};
  GRX_Error error;
  grx_error_clear(&error);
  GRX_Result result = grx_regex_replace(regex.get(), subject.data(),
      subject.size(), replacement.data(), replacement.size(), flags,
      &options, nullptr, &error, &text);
  if (result != GRX_OK) {
    return std::string("<error ") + grx_diag_string(error.diag) + ">";
  }

  std::string out(text.data, text.length);
  // The buffer is NUL-terminated as well as counted, and a test is the right
  // place to say so: a caller will printf it.
  EXPECT_EQ(text.data[text.length], '\0');
  grx_text_free(&text);
  EXPECT_EQ(text.data, nullptr);
  return out;
}

/** `subject.split(new RegExp(pattern, "u"))`, rendered like JSON. */
std::string split_into(const char * pattern, const std::string & subject,
    size_t limit = GRX_NPOS, GRX_Engine engine = GRX_ENGINE_AUTO) {
  Regex regex(pattern);
  if (!regex.ok()) {
    return "<compile failed>";
  }

  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.engine = engine;

  GRX_Split split {};
  GRX_Result result = grx_regex_split(regex.get(), subject.data(),
      subject.size(), limit, &options, nullptr, nullptr, &split);
  if (result != GRX_OK) {
    return "<error>";
  }

  std::string out = "[";
  for (size_t i = 0; i < split.count; i++) {
    if (i) {
      out += ",";
    }
    if (split.pieces[i].start == GRX_NPOS) {
      out += "null";
      continue;
    }
    out += "\"";
    out += subject.substr(split.pieces[i].start,
        split.pieces[i].end - split.pieces[i].start);
    out += "\"";
  }
  out += "]";
  grx_split_free(&split);
  EXPECT_EQ(split.pieces, nullptr);
  return out;
}

} // namespace

// --------------------------------------------------------------------------
// The owned results
// --------------------------------------------------------------------------

TEST(Subst, FreeingToleratesNullAndZero) {
  grx_text_free(nullptr);
  grx_split_free(nullptr);

  GRX_Text text {};
  GRX_Split split {};
  grx_text_free(&text);
  grx_split_free(&split);
  EXPECT_EQ(text.data, nullptr);
  EXPECT_EQ(split.pieces, nullptr);
}

TEST(Subst, RejectsNullArguments) {
  Regex regex("a");
  ASSERT_TRUE(regex.ok());
  GRX_Text text {};
  GRX_Split split {};

  EXPECT_EQ(grx_regex_replace(nullptr, "a", 1, "b", 1, 0, nullptr, nullptr,
                nullptr, &text),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_regex_replace(regex.get(), "a", 1, "b", 1, 0, nullptr,
                nullptr, nullptr, nullptr),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_regex_replace(regex.get(), nullptr, 1, "b", 1, 0, nullptr,
                nullptr, nullptr, &text),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_regex_replace(regex.get(), "a", 1, nullptr, 1, 0, nullptr,
                nullptr, nullptr, &text),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_regex_replace(regex.get(), "a", 1, "b", 1, 0x40u, nullptr,
                nullptr, nullptr, &text),
      GRX_ERR_INVALID);

  EXPECT_EQ(grx_regex_split(nullptr, "a", 1, GRX_NPOS, nullptr, nullptr,
                nullptr, &split),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_regex_split(regex.get(), "a", 1, GRX_NPOS, nullptr, nullptr,
                nullptr, nullptr),
      GRX_ERR_INVALID);
}

// --------------------------------------------------------------------------
// The template grammar
// --------------------------------------------------------------------------

TEST(Replace, PlainTextAndTheFirstMatchOnly) {
  EXPECT_EQ(replaced("a", "banana", "X", GRX_REPLACE_NONE), "bXnana");
  EXPECT_EQ(replaced("a", "banana", "X"), "bXnXnX");
  EXPECT_EQ(replaced("z", "banana", "X"), "banana");
  EXPECT_EQ(replaced("a", "banana", ""), "bnn");
}

TEST(Replace, GroupsByNumber) {
  EXPECT_EQ(replaced("(a)(b)", "abab", "[$1|$2]"), "[a|b][a|b]");
  // A group that did not participate substitutes nothing, and is not an
  // error: `(a)|(b)` has two groups and each match has one of them.
  EXPECT_EQ(replaced("(a)|(b)", "ab", "[$1$2]"), "[a][b]");
}

TEST(Replace, TwoDigitsAreTriedBeforeOne) {
  // The rule nobody remembers. With two groups, `$12` is group 1 then a
  // literal "2"; with eleven, `$10` is group 10 and `$11` is group 11.
  EXPECT_EQ(replaced("(a)(b)", "abab", "$0 $3 $12"), "$0 $3 a2$0 $3 a2");
  EXPECT_EQ(replaced("(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)", "abcdefghijk",
                "$10|$11|$1"),
      "j|k|a");
}

TEST(Replace, AReferenceToAGroupThatDoesNotExistIsLiteralText) {
  // ECMAScript's rule, and a good one: a caller who mistypes gets their
  // template back rather than silence. Group 0 is never addressable by
  // number, so `$0` is literal too.
  EXPECT_EQ(replaced("b", "abc", "$99"), "a$99c");
  EXPECT_EQ(replaced("b", "abc", "$0"), "a$0c");
  EXPECT_EQ(replaced("b", "abc", "$"), "a$c");
  EXPECT_EQ(replaced("b", "abc", "$z"), "a$zc");
}

TEST(Replace, TheWholeMatchAndItsSurroundings) {
  EXPECT_EQ(replaced("(a)", "ab", "$$ $& $` $'"), "$ a  bb");
}

TEST(Replace, GroupsByName) {
  // Three spellings, three different meanings, all of them Node's.
  EXPECT_EQ(replaced("(?<x>a)", "ab", "$<x>"), "ab");
  // A name the pattern does not have, in a pattern that has names: empty.
  EXPECT_EQ(replaced("(?<x>a)", "ab", "$<y>"), "b");
  // No closing `>`: literal text.
  EXPECT_EQ(replaced("(?<x>a)", "ab", "$<x"), "$<xb");
  // No named groups in the pattern at all: literal text, not a reference.
  EXPECT_EQ(replaced("(a)", "ab", "$<x>"), "$<x>b");
}

TEST(Replace, LiteralModeTakesNoTemplateAtAll) {
  // What a caller substituting text they did not write needs: `$&` in user
  // input must not become the match.
  EXPECT_EQ(replaced("b", "abc", "$& $1 $$",
                GRX_REPLACE_GLOBAL | GRX_REPLACE_LITERAL),
      "a$& $1 $$c");
}

TEST(Replace, EmptyMatchesFollowTheDialectsIterationRule) {
  // The reason WP-15 came first. These are Node's, and each is a loop that
  // would not terminate under a rule that failed to advance.
  EXPECT_EQ(replaced("a*", "aab", "X"), "XXbX");
  EXPECT_EQ(replaced("a*", "ba", "X"), "XbXX");
  EXPECT_EQ(replaced("", "abc", "-"), "-a-b-c-");
}

TEST(Replace, BothEnginesAgree) {
  const char * const patterns[] = {"a", "a*", "(a)(b)", "(a)|(b)", "a??"};
  for (const char * pattern : patterns) {
    EXPECT_EQ(replaced(pattern, "abab", "[$&:$1]", GRX_REPLACE_GLOBAL,
                  GRX_ENGINE_PIKE),
        replaced(pattern, "abab", "[$&:$1]", GRX_REPLACE_GLOBAL,
            GRX_ENGINE_BACKTRACK))
        << pattern;
  }
}

TEST(Replace, TheWindowBoundsWhatIsReplacedAndWhatIsReturned) {
  Regex regex("a");
  ASSERT_TRUE(regex.ok());

  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.begin = 1;
  options.end = 4;

  GRX_Text text {};
  ASSERT_EQ(grx_regex_replace(regex.get(), "aaaaa", 5, "X", 1,
                GRX_REPLACE_GLOBAL, &options, nullptr, nullptr, &text),
      GRX_OK);
  // The first `a` is before `begin` and is copied through; the last is past
  // `end` and is not part of the subject for this call.
  EXPECT_EQ(std::string(text.data, text.length), "aXXX");
  grx_text_free(&text);
}

TEST(Replace, ASubjectContainingNulSurvives) {
  // `length` is authoritative, not the terminator: a subject may hold a NUL
  // and a caller counting bytes must get all of them back.
  Regex regex("b");
  ASSERT_TRUE(regex.ok());
  const std::string subject("a\0b\0c", 5);

  GRX_Text text {};
  ASSERT_EQ(grx_regex_replace(regex.get(), subject.data(), subject.size(),
                "X", 1, GRX_REPLACE_GLOBAL, nullptr, nullptr, nullptr, &text),
      GRX_OK);
  EXPECT_EQ(text.length, 5u);
  EXPECT_EQ(std::string(text.data, text.length), std::string("a\0X\0c", 5));
  grx_text_free(&text);
}

TEST(Replace, AStrayLimitIsReported) {
  Regex regex("(a+)+b");
  ASSERT_TRUE(regex.ok());

  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_steps = 500;

  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.limits = &limits;
  options.engine = GRX_ENGINE_BACKTRACK;

  const std::string subject(40, 'a');
  GRX_Text text {};
  EXPECT_EQ(grx_regex_replace(regex.get(), subject.data(), subject.size(),
                "X", 1, GRX_REPLACE_GLOBAL, &options, nullptr, nullptr,
                &text),
      GRX_ERR_LIMIT);
  // Nothing was allocated for the caller to free, which is the suite's rule
  // for a failed call.
  EXPECT_EQ(text.data, nullptr);
}

// --------------------------------------------------------------------------
// Splitting
// --------------------------------------------------------------------------

TEST(Split, MatchesNode) {
  EXPECT_EQ(split_into(",", "a,b,c"), "[\"a\",\"b\",\"c\"]");
  EXPECT_EQ(split_into(",", ""), "[\"\"]");
  EXPECT_EQ(split_into("", "abc"), "[\"a\",\"b\",\"c\"]");
  EXPECT_EQ(split_into("", ""), "[]");
  EXPECT_EQ(split_into(",", ",a,"), "[\"\",\"a\",\"\"]");
  EXPECT_EQ(split_into(",", "a,,b"), "[\"a\",\"\",\"b\"]");
  EXPECT_EQ(split_into("b", "b"), "[\"\",\"\"]");
}

TEST(Split, CapturingGroupsAppearBetweenThePieces) {
  EXPECT_EQ(split_into("(,)", "a,b"), "[\"a\",\",\",\"b\"]");
  EXPECT_EQ(split_into("(\\d)", "a1b2c"),
      "[\"a\",\"1\",\"b\",\"2\",\"c\"]");
  EXPECT_EQ(split_into("(b)", "b"), "[\"\",\"b\",\"\"]");
  // A group that did not participate is `undefined` in ECMAScript, and an
  // unset span here - which is why the pieces are GRX_Capture and not
  // pointers into the subject.
  EXPECT_EQ(split_into("(a)|(b)", "xaybz"),
      "[\"x\",\"a\",null,\"y\",null,\"b\",\"z\"]");
}

TEST(Split, AnEmptyMatchWhereAPieceBeginsIsNotASeparator) {
  // Without this rule `x*` would split "abc" into an empty piece before
  // every character as well as the characters.
  EXPECT_EQ(split_into("x*", "abc"), "[\"a\",\"b\",\"c\"]");
  EXPECT_EQ(split_into("a*", "baac"), "[\"b\",\"c\"]");
  EXPECT_EQ(split_into("\\s*", "a b"), "[\"a\",\"b\"]");
}

TEST(Split, TheLimitCountsPiecesIncludingCaptures) {
  EXPECT_EQ(split_into(",", "a,b,c", 0), "[]");
  EXPECT_EQ(split_into(",", "a,b,c", 1), "[\"a\"]");
  EXPECT_EQ(split_into(",", "a,b,c", 2), "[\"a\",\"b\"]");
  EXPECT_EQ(split_into(",", "a,b,c", 99), "[\"a\",\"b\",\"c\"]");
  EXPECT_EQ(split_into("(,)", "a,b,c", 2), "[\"a\",\",\"]");
}

TEST(Split, BothEnginesAgree) {
  const char * const patterns[] = {",", "", "x*", "(a)|(b)", "a*", "(,)"};
  const char * const subjects[] = {"a,b,c", "", "abc", "xaybz", ",a,"};
  for (const char * pattern : patterns) {
    for (const char * subject : subjects) {
      EXPECT_EQ(split_into(pattern, subject, GRX_NPOS, GRX_ENGINE_PIKE),
          split_into(pattern, subject, GRX_NPOS, GRX_ENGINE_BACKTRACK))
          << "/" << pattern << "/ on \"" << subject << "\"";
    }
  }
}

TEST(Split, AstralCharactersAreNotCutInHalf) {
  // Splitting on the empty pattern walks the subject one *character* at a
  // time; a byte at a time would hand back offsets inside a UTF-8 sequence.
  const std::string subject = "\xF0\x9F\x98\x80\xF0\x9F\x98\x81";
  Regex regex("");
  ASSERT_TRUE(regex.ok());

  GRX_Split split {};
  ASSERT_EQ(grx_regex_split(regex.get(), subject.data(), subject.size(),
                GRX_NPOS, nullptr, nullptr, nullptr, &split),
      GRX_OK);
  ASSERT_EQ(split.count, 2u);
  EXPECT_EQ(split.pieces[0].start, 0u);
  EXPECT_EQ(split.pieces[0].end, 4u);
  EXPECT_EQ(split.pieces[1].start, 4u);
  EXPECT_EQ(split.pieces[1].end, 8u);
  grx_split_free(&split);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
