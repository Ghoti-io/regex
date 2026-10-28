/**
 * @file
 *
 * Vim string-start arguments and the public subject/window distinction.
 * Expectations come from isolated forced Vim 9.2.1129 processes. A Vim start
 * argument resets subject context; a GRX search begin preserves it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <memory>
#include <string>

#include "test_helpers.h"

namespace {

class VimContext : public ::testing::TestWithParam<GRX_Syntax> {};

INSTANTIATE_TEST_SUITE_P(Compatibility, VimContext, ::testing::Values(
    GRX_SYNTAX_VIM_BACKTRACK, GRX_SYNTAX_VIM_NFA));

TEST_P(VimContext, SubjectSlicesReproduceVimStartContext) {
  struct Row {
    const char * pattern;
    const char * subject;
    size_t offset;
    size_t start;
    size_t end;
  };
  const Row rows[] = {
      {R"(^.)", "xyaaab", 2, 2, 3},
      {R"(\_^.)", "xyaaab", 2, 2, 3},
      {R"(\%1c.)", "xyaaab", 2, 2, 3},
      {R"(\%3c.)", "xyaaab", 2, 4, 5},
      {R"(\%3v.)", "xyaaab", 2, 4, 5},
      {R"(.\@<=.)", "xyaaab", 2, 3, 4},
      {R"(\(y\)\@<=.)", "xyaaab", 2, GRX_NPOS, GRX_NPOS},
      {R"(\%3cé\+\(é\)\@>\(é\)\@<=b)", "xyéééb", 2, 4, 9},
      {R"(\%3c€\+\(€\)\@>\(€\)\@<=b)", "xy€€€b", 2, GRX_NPOS, GRX_NPOS},
      {R"(\%3c😀*\zs\(😀\)\%(😀\)\@>\zeb)", "xy😀😀😀b",
          2, GRX_NPOS, GRX_NPOS},
  };
  for (const Row & row : rows) {
    SCOPED_TRACE(row.pattern);
    GRX_Regex * raw_regex = nullptr;
    ASSERT_EQ(grx_regex_compile(row.pattern, GetParam(), 0, &raw_regex), GRX_OK);
    std::unique_ptr<GRX_Regex, decltype(&grx_regex_free)> regex(
        raw_regex, grx_regex_free);
    GRX_Match * raw_match = nullptr;
    ASSERT_EQ(grx_match_create(regex.get(), nullptr, &raw_match), GRX_OK);
    std::unique_ptr<GRX_Match, decltype(&grx_match_destroy)> match(
        raw_match, grx_match_destroy);
    const size_t length = std::strlen(row.subject);
    for (GRX_Engine engine : {GRX_ENGINE_AUTO, GRX_ENGINE_BACKTRACK}) {
      SCOPED_TRACE(engine);
      int matched = 0;
      ASSERT_EQ(grx_regex_search(regex.get(), row.subject + row.offset,
          length - row.offset, 0, engine, nullptr, match.get(), &matched), GRX_OK);
      ASSERT_EQ(matched != 0, row.start != GRX_NPOS);
      if (!matched) {
        continue;
      }
      GRX_Capture whole;
      ASSERT_EQ(grx_match_group(match.get(), 0, &whole), GRX_OK);
      EXPECT_EQ(whole.start + row.offset, row.start);
      EXPECT_EQ(whole.end + row.offset, row.end);
      if (grx_regex_capture_count(regex.get()) == 2) {
        for (size_t group = 1; group <= 2; group++) {
          GRX_Capture capture;
          ASSERT_EQ(grx_match_group(match.get(), group, &capture), GRX_OK);
          ASSERT_NE(capture.start, GRX_NPOS);
          EXPECT_EQ(std::string(row.subject + row.offset + capture.start,
              capture.end - capture.start), "é");
        }
      }
    }
  }
}

TEST_P(VimContext, WindowBeginRetainsSubjectOriginAndLookbehindVisibility) {
  struct Row {
    const char * pattern;
    size_t start;
    size_t end;
  };
  const Row rows[] = {
      {R"(^.)", GRX_NPOS, GRX_NPOS},
      {R"(\%1c.)", GRX_NPOS, GRX_NPOS},
      {R"(\%3c.)", 2, 3},
      {R"(\%3v.)", 2, 3},
      {R"(\(y\)\@<=.)", 2, 3},
  };
  for (const Row & row : rows) {
    SCOPED_TRACE(row.pattern);
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile(row.pattern, GetParam(), 0, &regex), GRX_OK);
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
    int matched = 0;
    EXPECT_EQ(grx_regex_search(regex, "xyaaab", 6, 2, GRX_ENGINE_BACKTRACK,
        nullptr, match, &matched), GRX_OK);
    EXPECT_EQ(matched != 0, row.start != GRX_NPOS);
    if (matched) {
      GRX_Capture whole;
      EXPECT_EQ(grx_match_group(match, 0, &whole), GRX_OK);
      EXPECT_EQ(whole.start, row.start);
      EXPECT_EQ(whole.end, row.end);
    }
    grx_match_destroy(match);
    grx_regex_free(regex);
  }
}


// Isolated global Vim operations supply whole spans and capture text only.
TEST_P(VimContext, RepeatedSearchAndReplacementKeepFullSubjectContext) {
  struct Outcome { size_t start; size_t end; const char * group; };
  struct Row {
    const char * pattern;
    const char * subject;
    Outcome matches[2][2];
    const char * replacements[2][3];
  };
  const Row rows[] = {
      {"a*\\%(a\\)\\@>\\(\\1a\\)\\@<=.", "aaaab#aaaab",
          {{{0, 5, "a"}, {6, 11, "a"}}, {{0, 5, "aa"}, {6, 11, "aa"}}},
          {{"X#X", "<a>#<a>", "<aaaab/a>#<aaaab/a>"}, {"X#X", "<aa>#<aa>", "<aaaab/aa>#<aaaab/aa>"}}},
      {"a*\\%(a\\)\\@>\\(\\1a\\)\\@<=.", "aaaaaab#aaaaaab",
          {{{0, 7, "a"}, {8, 15, "a"}}, {{0, 7, "aaaaaa"}, {8, 15, "aaaaaab#aaaaaa"}}},
          {{"X#X", "<a>#<a>", "<aaaaaab/a>#<aaaaaab/a>"}, {"X#X", "<aaaaaa>#<aaaaaab#aaaaaa>", "<aaaaaab/aaaaaa>#<aaaaaab/aaaaaab#aaaaaa>"}}},
      {"a*\\%(aa\\)\\@>\\(\\1a\\)\\@<=.", "aaaaaab#aaaaaab",
          {{{0, 7, "a"}, {8, 15, "a"}}, {{0, 7, "aa"}, {8, 15, "aa"}}},
          {{"X#X", "<a>#<a>", "<aaaaaab/a>#<aaaaaab/a>"}, {"X#X", "<aa>#<aa>", "<aaaaaab/aa>#<aaaaaab/aa>"}}},
      {"a*\\%(a\\ze\\)\\@>\\(\\1a\\)\\@<=.", "aaaab#aaaab",
          {{{0, 4, "a"}, {6, 10, "a"}}, {{0, 5, "aa"}, {6, 11, "aa"}}},
          {{"Xb#Xb", "<a>b#<a>b", "<aaaa/a>b#<aaaa/a>b"}, {"X#X", "<aa>#<aa>", "<aaaab/aa>#<aaaab/aa>"}}},
      {"a*\\%(a\\ze\\)\\@>\\(\\1a\\)\\@<=.", "aaaaaab#aaaaaab",
          {{{0, 6, "a"}, {8, 14, "a"}}, {{0, 7, "aaaaaa"}, {8, 15, "aaaaaab#aaaaaa"}}},
          {{"Xb#Xb", "<a>b#<a>b", "<aaaaaa/a>b#<aaaaaa/a>b"}, {"X#X", "<aaaaaa>#<aaaaaab#aaaaaa>", "<aaaaaab/aaaaaa>#<aaaaaab/aaaaaab#aaaaaa>"}}},
      {"a\\+\\%(a\\)\\@>\\(\\1a\\)\\@<=.", "aaaaab#aaaaab",
          {{{0, 6, "a"}, {7, 13, "a"}}, {{0, 6, "aa"}, {7, 13, "aa"}}},
          {{"X#X", "<a>#<a>", "<aaaaab/a>#<aaaaab/a>"}, {"X#X", "<aa>#<aa>", "<aaaaab/aa>#<aaaaab/aa>"}}},
      {"a\\+\\%(a\\ze\\)\\@>\\(\\1a\\)\\@<=.", "aaaaab#aaaaab",
          {{{0, 5, "a"}, {7, 12, "a"}}, {{0, 6, "aa"}, {7, 13, "aa"}}},
          {{"Xb#Xb", "<a>b#<a>b", "<aaaaa/a>b#<aaaaa/a>b"}, {"X#X", "<aa>#<aa>", "<aaaaab/aa>#<aaaaab/aa>"}}},
  };
  const char * templates[] = {"X", R"(<\1>)", R"(<&/\1>)"};
  const size_t profile = GetParam() == GRX_SYNTAX_VIM_NFA ? 1 : 0;
  for (const Row & row : rows) {
    SCOPED_TRACE(row.pattern);
    SCOPED_TRACE(row.subject);
    GRX_Regex * raw_regex = nullptr;
    ASSERT_EQ(grx_regex_compile(row.pattern, GetParam(), 0, &raw_regex), GRX_OK);
    std::unique_ptr<GRX_Regex, decltype(&grx_regex_free)> regex(raw_regex, grx_regex_free);
    const size_t length = std::strlen(row.subject);
    for (GRX_Engine engine : {GRX_ENGINE_AUTO, GRX_ENGINE_BACKTRACK}) {
      SCOPED_TRACE(engine);
      GRX_SearchOptions options;
      grx_search_options_default(&options);
      options.engine = engine;
      GRX_Match * raw_match = nullptr;
      ASSERT_EQ(grx_match_create(regex.get(), nullptr, &raw_match), GRX_OK);
      std::unique_ptr<GRX_Match, decltype(&grx_match_destroy)> match(raw_match, grx_match_destroy);
      int matched = 0;
      for (size_t index = 0; index < 3; index++) {
        ASSERT_EQ(grx_regex_search_next(regex.get(), row.subject, length,
            &options, match.get(), &matched), GRX_OK);
        ASSERT_EQ(matched != 0, index < 2);
        if (!matched) break;
        GRX_Capture whole, group;
        ASSERT_EQ(grx_match_group(match.get(), 0, &whole), GRX_OK);
        ASSERT_EQ(grx_match_group(match.get(), 1, &group), GRX_OK);
        EXPECT_EQ(whole.start, row.matches[profile][index].start);
        EXPECT_EQ(whole.end, row.matches[profile][index].end);
        ASSERT_NE(group.start, GRX_NPOS);
        ASSERT_LE(group.start, group.end);
        ASSERT_LE(group.end, length);
        EXPECT_EQ(std::string(row.subject + group.start, group.end - group.start),
            row.matches[profile][index].group);
      }
      for (size_t index = 0; index < 3; index++) {
        GRX_Text text {};
        ASSERT_EQ(grx_regex_replace(regex.get(), row.subject, length,
            templates[index], std::strlen(templates[index]), GRX_REPLACE_GLOBAL,
            &options, nullptr, nullptr, &text), GRX_OK);
        const std::string actual(text.data, text.length);
        grx_text_free(&text);
        EXPECT_EQ(actual, row.replacements[profile][index]);
      }
    }
  }
}

} // namespace

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
