/**
 * @file
 *
 * The defaults, against the patterns that have to keep working.
 *
 * Every field of GRX_Limits is a promise, and a promise is only worth making
 * if somebody has checked what real patterns actually need. WP-14's method
 * is two corpora pulling in opposite directions: patterns that must compile
 * and run at the defaults, and pairs that must be refused by them. The
 * second half is tests/conformance/test_redos.cpp. This is the first.
 *
 * `tools/limits/measure.py` reports the whole distribution - median, p99,
 * maximum and the headroom over each default - and is what the numbers in
 * documentation/dialects.md section 7 came from. What is here is the part
 * that has to keep being true: not "the headroom is 7x" but "nothing in the
 * corpus is refused", which is the claim a future default would break.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <fstream>
#include <string>
#include <vector>

#include "test_helpers.h"

#include "../conformance/rxt.h"

namespace {

struct Row {
  std::string flags;
  std::string pattern;
  std::string source;
};

uint32_t options_for(const std::string & flags) {
  uint32_t options = 0;
  for (char f : flags) {
    switch (f) {
      case 'i': options |= GRX_OPT_CASELESS; break;
      case 'm': options |= GRX_OPT_MULTILINE; break;
      case 's': options |= GRX_OPT_DOTALL; break;
      case 'u': options |= GRX_OPT_UTF; break;
      case 'v': options |= GRX_OPT_UNICODE_SETS | GRX_OPT_UTF; break;
      default: break;
    }
  }
  return options;
}

/** `tools/limits/real_world.txt`: flags, a tab, the pattern. */
std::vector<Row> read_real_world() {
  std::vector<Row> rows;
  std::ifstream file(grxtest::repo("tools/limits/real_world.txt"));
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    size_t tab = line.find('\t');
    if (tab == std::string::npos) {
      continue;
    }
    rows.push_back({line.substr(0, tab), line.substr(tab + 1),
        "tools/limits/real_world.txt"});
  }
  return rows;
}

} // namespace

TEST(Limits, EveryRealWorldPatternCompilesAtTheDefaults) {
  // The patterns that appear in JSON Schemas and configuration files. A
  // default below what one of these needs is a default that would reject
  // working software, which is the failure this corpus exists to prevent.
  std::vector<Row> rows = read_real_world();
  ASSERT_GT(rows.size(), 20u)
      << "the corpus was not found or is too small to be measuring anything";

  GRX_Limits limits;
  grx_limits_default(&limits);

  for (const Row & row : rows) {
    GRX_Error error;
    grx_error_clear(&error);
    GRX_Regex * regex = nullptr;
    GRX_Result result = grx_regex_compile_with_allocator(row.pattern.data(),
        row.pattern.size(), GRX_SYNTAX_ECMASCRIPT, options_for(row.flags),
        &limits, nullptr, &error, &regex);
    EXPECT_EQ(result, GRX_OK)
        << "/" << row.pattern << "/" << row.flags << ": " << error.message;
    grx_regex_free(regex);
  }
}

TEST(Limits, EveryConformanceVectorCompilesAtTheDefaults) {
  // The same claim over the vectors, which are a much larger and much
  // stranger corpus: they came from a generator aimed at the grammar's
  // corners rather than at what anybody would write.
  grxtest::VectorFile named;
  grxtest::VectorFile generated;
  std::string error;
  ASSERT_TRUE(grxtest::read_vector_file(
      grxtest::data("vectors/ecmascript/named.rxt"), &named, &error))
      << error;
  ASSERT_TRUE(grxtest::read_vector_file(
      grxtest::data("vectors/ecmascript/generated.rxt"), &generated, &error))
      << error;

  GRX_Limits limits;
  grx_limits_default(&limits);

  size_t compiled = 0;
  size_t refused_as_syntax = 0;
  for (const grxtest::VectorFile * file : {&named, &generated}) {
    for (const grxtest::Record & record : file->records) {
      GRX_Error compile_error;
      grx_error_clear(&compile_error);
      GRX_Regex * regex = nullptr;
      GRX_Result result = grx_regex_compile_with_allocator(
          record.pattern.data(), record.pattern.size(), record.syntax,
          record.options, &limits, nullptr, &compile_error, &regex);
      grx_regex_free(regex);

      if (result == GRX_OK) {
        compiled++;
        continue;
      }
      // A vector whose expectation is a syntax error is meant not to
      // compile. What must never happen is a *limit*.
      EXPECT_NE(result, GRX_ERR_LIMIT)
          << "/" << record.pattern << "/ hit " << compile_error.message;
      refused_as_syntax++;
    }
  }

  EXPECT_GT(compiled, 100u) << "almost nothing compiled; the corpus is wrong";
  printf("\nlimits: %zu vectors compiled at the defaults, %zu refused as "
         "syntax\n",
      compiled, refused_as_syntax);
}

TEST(Limits, NestingIsBoundedWellInsideTheSmallestStack) {
  // max_nesting_depth is the C-stack bound and the one limit design.md says
  // a caller should not lift casually. Measured rather than assumed: on the
  // 256 KB stack the fuzzers run under, a pattern nested 480 deep parses and
  // one nested 496 deep overflows - about 525 bytes of stack per level. The
  // default of 128 is therefore a factor of about four inside the smallest
  // stack this library claims to work on, and thirty inside the usual one.
  //
  // What is checked here is the half that does not need a `ulimit`: that the
  // default is enforced exactly, one level either side of it.
  GRX_Limits limits;
  grx_limits_default(&limits);
  ASSERT_GT(limits.max_nesting_depth, 0u);

  const std::string at(limits.max_nesting_depth, '(');
  const std::string close(limits.max_nesting_depth, ')');
  GRX_Pattern * parsed = nullptr;
  GRX_Error error;
  grx_error_clear(&error);

  std::string ok = at + "a" + close;
  EXPECT_EQ(grx_pattern_parse_with_allocator(ok.data(), ok.size(),
                GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, &limits, nullptr, &error,
                &parsed),
      GRX_OK)
      << error.message;
  grx_pattern_free(parsed);
  parsed = nullptr;

  std::string over = at + "(a)" + close;
  EXPECT_EQ(grx_pattern_parse_with_allocator(over.data(), over.size(),
                GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, &limits, nullptr, &error,
                &parsed),
      GRX_ERR_LIMIT);
  EXPECT_EQ(error.diag, GRX_DIAG_LIMIT_NESTING_DEPTH);
  grx_pattern_free(parsed);
}

TEST(Limits, TheCostliestRealPatternIsWellInsideTheProgramLimit) {
  // `\p{RGI_Emoji}` is the largest thing ECMAScript can name: nearly four
  // thousand sequences, which lower to an alternation. It is the pattern
  // that decides max_nodes and max_program_size, and the headroom it leaves
  // is the headroom those two have.
  GRX_Limits limits;
  grx_limits_default(&limits);

  GRX_Regex * regex = nullptr;
  GRX_Error error;
  grx_error_clear(&error);
  const char * pattern = "^\\p{RGI_Emoji}+$";
  ASSERT_EQ(grx_regex_compile_with_allocator(pattern, strlen(pattern),
                GRX_SYNTAX_ECMASCRIPT,
                GRX_OPT_UTF | GRX_OPT_UNICODE_SETS, &limits, nullptr, &error,
                &regex),
      GRX_OK)
      << error.message;

  GRX_Facts facts;
  ASSERT_EQ(grx_regex_facts(regex, &facts), GRX_OK);
  EXPECT_LT(facts.program_size, limits.max_program_size);
  EXPECT_GT(facts.program_size, 10000u)
      << "it should be the costliest thing in the corpus; if it is not, the "
         "measurement in dialects.md section 7 is about a different pattern";
  printf("limits: /%s/v is %zu instructions against a cap of %zu\n", pattern,
      facts.program_size, limits.max_program_size);
  grx_regex_free(regex);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
