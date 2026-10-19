/**
 * @file
 *
 * JDK 25 parser checkpoint: spellings that Java accepts and rejects.
 * The pinned cases live in tests/data/probe/java25.json.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>

#include "test_helpers.h"

namespace {

GRX_Result compile_java(const char * text, GRX_Error * error = nullptr) {
  GRX_Regex * regex = nullptr;
  GRX_Error local;
  GRX_Result result = grx_regex_compile_with_allocator(text, std::strlen(text),
      GRX_SYNTAX_JAVA, GRX_OPT_NONE, nullptr, nullptr,
      error ? error : &local, &regex);
  grx_regex_free(regex);
  return result;
}

bool matches_java(const char * pattern, const char * subject,
    uint32_t options = GRX_OPT_NONE) {
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  if (grx_regex_compile_with_allocator(pattern, std::strlen(pattern),
          GRX_SYNTAX_JAVA, options, nullptr, nullptr, &error,
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

} // namespace

TEST(JavaParser, AcceptsMeasuredForms) {
  for (const char * pattern : {
           "(?<n>a)\\k<n>", "\\Q[a]+\\E", "\\x{1F600}", "\\u00E9",
           "\\0101", "\\cA", "\\h", "\\v", "\\R", "\\X",
           "\\b{g}", "[a&&[a-z]]", "[a&&[^a]]", "[[a-z][0-9]]",
           "[a-z&&[^aeiou]]", "[[:alpha:]]", "[\\d-z]", "[]]",
           "[-a]", "[a-]", "(?=a)*", "\\1", "(?i:a)", "(?U)\\w"}) {
    GRX_Error error;
    EXPECT_EQ(compile_java(pattern, &error), GRX_OK)
        << pattern << ": " << error.message;
  }
}

TEST(JavaParser, AcceptsRemainingPinnedSyntaxForms) {
  for (const char * pattern : {
           "(?d).", "(?s).", "(?m)^b", "(?x)a # hi\n b",
           "(?<=ab)c", "(?<=a|ab)c", "(?<=a{1,3})b", "(?<=a+)b",
           "\\p{IsGreek}", "\\p{InGreek}", "\\p{javaLowerCase}",
           "(?U)\\d", "(?iu)k", "(?dm)^b", "(?x)[a b]", "(?-i:A)",
           "[&&a]", "[a&&]", "[a-[b]]", "\\N{LATIN CAPITAL LETTER A}",
           "a{65536}", "\\p{javaUpperCase}", "\\p{javaDigit}",
           "\\p{javaWhitespace}", "\\p{javaMirrored}"}) {
    GRX_Error error;
    EXPECT_EQ(compile_java(pattern, &error), GRX_OK)
        << pattern << ": " << error.message;
  }
}

TEST(JavaParser, RefusesMeasuredForeignAndAmbiguousForms) {
  for (const char * pattern : {
           "(?R)", "(?(1)a|b)", "(*SKIP)", "\\g{1}", "(?P<n>a)",
           "[a-\\d]", "a{,3}", "a**", "\\0", "(?<=(a)\\1)b",
           "\\p{NotAProperty}", "[\\1]", "[\\k<n>]", "\\xA",
           "\\N", "\\N{U+0041}", "\\x{100000000}", "a{}",
           "\\p{Lowercase}", "\\p{Greek}", "\\p{scx=Greek}"}) {
    GRX_Error error;
    EXPECT_EQ(compile_java(pattern, &error), GRX_ERR_SYNTAX)
        << pattern << ": " << error.message;
    EXPECT_NE(error.diag, GRX_DIAG_DIALECT_NOT_IMPLEMENTED) << pattern;
  }
}

TEST(JavaParser, DistinguishesMeasuredClassAndReferenceMatches) {
  EXPECT_FALSE(matches_java("[a&&[^a]]", "b"));
  EXPECT_FALSE(matches_java("[a&&[^a]]", "a"));
  EXPECT_TRUE(matches_java("[a&&[a-z]]", "a"));
  EXPECT_FALSE(matches_java("[a&&[a-z]]", "b"));
  EXPECT_TRUE(matches_java("(a)\\1", "aa"));
  EXPECT_FALSE(matches_java("(a)\\1", "ab"));
  EXPECT_TRUE(matches_java("[&&a]", "a"));
  EXPECT_FALSE(matches_java("[&&a]", "&"));
  EXPECT_TRUE(matches_java("[a&&]", "a"));
  EXPECT_FALSE(matches_java("[a&&]", "&"));
  for (const char * subject : {"a", "-", "b"}) {
    EXPECT_TRUE(matches_java("[a-[b]]", subject)) << subject;
  }
  EXPECT_TRUE(matches_java("\\N{LATIN CAPITAL LETTER A}", "A"));
}

TEST(JavaParser, ReportsUtf8ByteOffsets) {
  GRX_Error error;
  EXPECT_EQ(compile_java("\xF0\x9F\x98\x80(", &error), GRX_ERR_SYNTAX);
  EXPECT_EQ(error.offset, 4u);
}

TEST(JavaFlags, ExternalAndScopedCaseWidth) {
  uint32_t i = 0, iu = 0, iU = 0, U = 0;
  ASSERT_EQ(grx_options_parse(GRX_SYNTAX_JAVA, "i", &i, nullptr), GRX_OK);
  ASSERT_EQ(grx_options_parse(GRX_SYNTAX_JAVA, "iu", &iu, nullptr), GRX_OK);
  ASSERT_EQ(grx_options_parse(GRX_SYNTAX_JAVA, "iU", &iU, nullptr), GRX_OK);
  ASSERT_EQ(grx_options_parse(GRX_SYNTAX_JAVA, "U", &U, nullptr), GRX_OK);
  EXPECT_FALSE(matches_java("k", "\xE2\x84\xAA", i));
  EXPECT_TRUE(matches_java("k", "\xE2\x84\xAA", iu));
  EXPECT_TRUE(matches_java("k", "\xE2\x84\xAA", iU));
  EXPECT_FALSE(matches_java("k", "\xE2\x84\xAA", U));
  EXPECT_FALSE(matches_java("\\d", "\xD9\xA1"));
  EXPECT_TRUE(matches_java("\\d", "\xD9\xA1", U));
  EXPECT_FALSE(matches_java("\\w", "\xF0\x90\x90\xA8"));
  EXPECT_TRUE(matches_java("\\w", "\xF0\x90\x90\xA8", U));
  EXPECT_FALSE(matches_java("\xF0\x90\x90\x80", "\xF0\x90\x90\xA8", i));
  EXPECT_TRUE(matches_java("\xF0\x90\x90\x80", "\xF0\x90\x90\xA8", iu));
  EXPECT_FALSE(matches_java("\\b\xC3\xA9", "\xC3\xA9"));
  EXPECT_TRUE(matches_java("\\B\xC3\xA9", "\xC3\xA9"));
  EXPECT_TRUE(matches_java("\\b\xC3\xA9", "\xC3\xA9", U));
  EXPECT_FALSE(matches_java("\\B\xC3\xA9", "\xC3\xA9", U));
  EXPECT_FALSE(matches_java("(?-u:k)", "\xE2\x84\xAA", iu));
  EXPECT_TRUE(matches_java("(?u:k)", "\xE2\x84\xAA", i));
}

TEST(JavaFlags, UnixLinesAndVerbose) {
  uint32_t d = 0, m = 0, x = 0;
  ASSERT_EQ(grx_options_parse(GRX_SYNTAX_JAVA, "d", &d, nullptr), GRX_OK);
  ASSERT_EQ(grx_options_parse(GRX_SYNTAX_JAVA, "m", &m, nullptr), GRX_OK);
  ASSERT_EQ(grx_options_parse(GRX_SYNTAX_JAVA, "x", &x, nullptr), GRX_OK);
  EXPECT_FALSE(matches_java("a.b", "a\rb"));
  EXPECT_TRUE(matches_java("a.b", "a\rb", d));
  EXPECT_TRUE(matches_java("(?d:a.b)", "a\rb"));
  EXPECT_FALSE(matches_java("(?-d:a.b)", "a\rb", d));
  EXPECT_TRUE(matches_java("^b", "a\rb", m));
  EXPECT_FALSE(matches_java("^b", "a\rb", m | d));
  EXPECT_FALSE(matches_java("^\\n", "\r\n", m));
  EXPECT_TRUE(matches_java("a b", "ab", x));
  EXPECT_TRUE(matches_java("[a b]", "b", x));
}

TEST(JavaProperties, CharacterWhitespaceDiffersFromUnicodeWhiteSpace) {
  const char * property = "\\p{javaWhitespace}";
  for (const char * subject : {" ", "\t", "\xE2\x80\x83", "\x1C"}) {
    EXPECT_TRUE(matches_java(property, subject)) << subject;
  }
  for (const char * subject : {"\xC2\xA0", "\xC2\x85",
           "\xE2\x80\x87", "\xE2\x80\xAF"}) {
    EXPECT_FALSE(matches_java(property, subject)) << subject;
  }
  EXPECT_FALSE(matches_java("\\p{IsGreek}", "\xCD\xB4"));
  EXPECT_TRUE(matches_java("\\p{IsGreek}", "\xE1\xBC\x80"));
  EXPECT_FALSE(matches_java("\\p{sc=Greek}", "\xCD\xB4"));
  EXPECT_TRUE(matches_java("\\p{sc=Greek}", "\xE1\xBC\x80"));
  EXPECT_TRUE(matches_java("\\p{InGreek}", "\xCD\xB4"));
  EXPECT_TRUE(matches_java("\\p{javaLowerCase}", "\xCA\x95"));
  EXPECT_TRUE(matches_java("\\p{javaLowerCase}", "\xF0\x90\x90\xA8"));
  EXPECT_TRUE(matches_java("\\p{javaDigit}", "\xF0\x9D\x9F\x8E"));
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
