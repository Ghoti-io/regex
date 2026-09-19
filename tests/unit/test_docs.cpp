/**
 * @file
 *
 * The documents that quote the code, checked against the code.
 *
 * Two things in `documentation/` and `README.md` claim to show real output:
 * the README's worked example, and development.md's three dump formats. Both
 * are the first thing a new reader meets, and both rot silently - a format
 * changes, the page does not, and the next person spends an afternoon
 * wondering why their output looks different from the manual's.
 *
 * This file makes them fail instead. It is not a test of the library; it is a
 * test of the pages, and it belongs with the code because that is what it is
 * comparing them against.
 *
 * The rule the tests enforce is deliberately narrow: the *fenced blocks that
 * claim to be output* must be output. Prose is not checked and should not be -
 * a page that has to be word-for-word correct is a page nobody edits.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdio>
#include <string>

#include "test_helpers.h"

#include "../../src/compile/compile_internal.h"
#include "../../src/ir/lower_internal.h"

namespace {

/**
 * The fenced block that follows a marker in a markdown file.
 *
 * Finds `marker`, then the next ``` fence after it, and returns everything up
 * to the closing fence. Enough for this job and no more: a full markdown
 * parser here would be a second thing to get wrong.
 */
std::string fenced_block_after(
    const std::string & document, const std::string & marker) {
  size_t at = document.find(marker);
  if (at == std::string::npos) {
    return std::string();
  }
  size_t open = document.find("```", at);
  if (open == std::string::npos) {
    return std::string();
  }
  size_t body = document.find('\n', open);
  if (body == std::string::npos) {
    return std::string();
  }
  body++;
  size_t close = document.find("```", body);
  if (close == std::string::npos) {
    return std::string();
  }
  return document.substr(body, close - body);
}

/** Trim trailing whitespace from every line, and blank lines from the end. */
std::string normalise(const std::string & text) {
  std::string out;
  size_t start = 0;
  while (start <= text.size()) {
    size_t end = text.find('\n', start);
    std::string line = text.substr(start,
        end == std::string::npos ? std::string::npos : end - start);
    while (!line.empty() && (line.back() == ' ' || line.back() == '\r')) {
      line.pop_back();
    }
    out += line;
    out += "\n";
    if (end == std::string::npos) {
      break;
    }
    start = end + 1;
  }
  while (out.size() >= 2 && out[out.size() - 1] == '\n'
      && out[out.size() - 2] == '\n') {
    out.pop_back();
  }
  return out;
}

/** The pattern every example on the development page is built from. */
const char * const kExamplePattern = "(a)+";

} // namespace

TEST(Documents, TheDumpExamplesAreRealOutput) {
  const std::string page
      = grxtest::read_file(grxtest::repo("documentation/development.md"));
  ASSERT_FALSE(page.empty())
      << "documentation/development.md was not found; the test needs "
      << "GRX_REPO_ROOT baked in";

  GRX_Error error;
  grx_error_clear(&error);
  GRX_Pattern * parsed = nullptr;
  ASSERT_EQ(grx_pattern_parse_with_allocator(kExamplePattern, 4,
                GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, nullptr, nullptr, &error,
                &parsed),
      GRX_OK);

  // The AST dump.
  std::string actual = normalise(grxtest::capture_dump(
      [parsed](FILE * out) { grx_pattern_dump(parsed, out); }));
  std::string documented
      = normalise(fenced_block_after(page, "`grx_pattern_dump()` walks"));
  EXPECT_EQ(documented, actual)
      << "documentation/development.md's AST dump example is out of date.\n"
      << "The real output for /" << kExamplePattern << "/u is:\n"
      << actual;

  // The IR dump. Lowering is internal, so this reaches past the public API
  // on purpose: the page documents the format and the format has to be
  // checkable.
  GRX_Limits limits;
  grx_limits_default(&limits);
  GRX_IR * ir = nullptr;
  ASSERT_EQ(grx_lower_pattern(parsed, &limits, grx_allocator_default(),
                &error, &ir),
      GRX_OK);
  actual = normalise(
      grxtest::capture_dump([ir](FILE * out) { grx_ir_dump(ir, out); }));
  documented = normalise(fenced_block_after(page, "`grx_ir_dump()` is the"));
  EXPECT_EQ(documented, actual)
      << "documentation/development.md's IR dump example is out of date.\n"
      << "The real output for /" << kExamplePattern << "/u is:\n"
      << actual;
  grx_ir_free(ir);

  // The disassembly.
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(
      grx_regex_compile_pattern(parsed, nullptr, nullptr, &error, &regex),
      GRX_OK);
  actual = normalise(grxtest::capture_dump(
      [regex](FILE * out) { grx_regex_dump(regex, out); }));
  documented
      = normalise(fenced_block_after(page, "All three examples on this page"));
  EXPECT_EQ(documented, actual)
      << "documentation/development.md's disassembly example is out of "
      << "date.\nThe real output for /" << kExamplePattern << "/u is:\n"
      << actual;

  grx_regex_free(regex);
  grx_pattern_free(parsed);
}

TEST(Documents, TheReadmeExampleDoesWhatItSays) {
  // The README's example is the first code a reader meets. It cannot be
  // *run* from here without a compiler, but it can be checked for the two
  // things that actually went wrong last time: that it names a dialect with
  // a front end, and that the pattern and subject it shows really do produce
  // the output the prose around it implies.
  const std::string page = grxtest::read_file(grxtest::repo("README.md"));
  ASSERT_FALSE(page.empty());

  const std::string example = fenced_block_after(page, "## Example");
  ASSERT_FALSE(example.empty()) << "the README has no example block";

  EXPECT_EQ(example.find("GRX_SYNTAX_PCRE"), std::string::npos)
      << "the README example asks for a dialect that has no front end, so it "
      << "returns GRX_ERR_UNSUPPORTED at its first call";
  EXPECT_NE(example.find("GRX_SYNTAX_ECMASCRIPT"), std::string::npos);

  // And the example's own claim: this pattern against this subject reports
  // "Corey" as group 1.
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile_with_allocator("(\\w+)@(\\w+)", 11,
                GRX_SYNTAX_ECMASCRIPT, GRX_OPT_CASELESS | GRX_OPT_UTF,
                nullptr, nullptr, nullptr, &regex),
      GRX_OK);

  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
  const std::string subject = "write to Corey@example";
  int matched = 0;
  ASSERT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                GRX_ENGINE_AUTO, nullptr, match, &matched),
      GRX_OK);
  ASSERT_TRUE(matched);

  GRX_Capture user;
  ASSERT_EQ(grx_match_group(match, 1, &user), GRX_OK);
  EXPECT_EQ(subject.substr(user.start, user.end - user.start), "Corey");

  grx_match_destroy(match);
  grx_regex_free(regex);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
