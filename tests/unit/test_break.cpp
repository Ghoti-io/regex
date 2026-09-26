/**
 * @file
 *
 * The segmentation boundaries, against the Unicode Consortium's own data.
 *
 * `GraphemeBreakTest.txt`, `WordBreakTest.txt`, `SentenceBreakTest.txt` and
 * `LineBreakTest.txt` are the conformance files UAX #29 and UAX #14 publish
 * alongside the rules. Each line is a sequence of code points with `÷` where
 * a boundary falls and `×` where one does not, so a line is an assertion
 * about *every* position in a string rather than about one of them.
 *
 * This is the gate for the four boundary algorithms, and it is a better one
 * than the corpus: the sixteen `\b{...}` vectors in the Perl corpus all use
 * the empty subject, so they could pass against an implementation that was
 * wrong everywhere else. The differential against Perl in
 * tests/data/vectors is the second gate and answers a different question -
 * what the *dialect* does, tailoring included.
 *
 * The files are not committed: they live under `third_party/ucd/`, which
 * .gitignore excludes, so a checkout without them skips with a message the
 * way `make check-unicode-tables` does rather than failing.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdio>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "test_helpers.h"

extern "C" {
#include "../../src/unicode/break_internal.h"
#include "../../src/unicode/unicode_internal.h"
}

namespace {

/** Where the UCD lives, if it was fetched. */
std::string ucd_path(const std::string & name) {
  return std::string(GRX_REPO_ROOT) + "/third_party/ucd/17.0.0/" + name;
}

/** One test line: the subject, and whether a boundary falls at each offset. */
struct Row {
  size_t line = 0;
  std::string comment;
  std::string subject;
  std::vector<bool> boundary; ///< Indexed by byte offset, 0..subject.size().
};

void encode(uint32_t codepoint, std::string * out) {
  char buffer[4];
  size_t written = grx_unicode_utf8_encode(codepoint, buffer);
  out->append(buffer, written);
}

/**
 * Read one of the four files.
 *
 * The `÷`/`×` marks are UTF-8 in the file itself, so the parse is over the
 * decoded text rather than over bytes: `÷` is U+00F7 and `×` is U+00D7.
 */
bool read_break_test(const std::string & path, std::vector<Row> * out) {
  std::ifstream file(path);
  if (!file) {
    return false;
  }

  std::string line;
  size_t number = 0;
  while (std::getline(file, line)) {
    number++;
    std::string body = line.substr(0, line.find('#'));
    if (body.find_first_not_of(" \t\r") == std::string::npos) {
      continue;
    }

    Row row;
    row.line = number;
    row.comment = line;
    std::istringstream fields(body);
    std::string token;
    bool expect_mark = true;
    bool ok = true;
    while (fields >> token) {
      if (token == "\xC3\xB7") { // U+00F7 DIVISION SIGN: a boundary.
        row.boundary.resize(row.subject.size() + 1, false);
        row.boundary[row.subject.size()] = true;
        expect_mark = false;
        continue;
      }
      if (token == "\xC3\x97") { // U+00D7 MULTIPLICATION SIGN: no boundary.
        row.boundary.resize(row.subject.size() + 1, false);
        expect_mark = false;
        continue;
      }
      if (expect_mark) {
        ok = false;
        break;
      }
      encode((uint32_t)std::stoul(token, nullptr, 16), &row.subject);
      expect_mark = true;
    }
    if (!ok || row.subject.empty()) {
      continue;
    }
    row.boundary.resize(row.subject.size() + 1, false);
    out->push_back(row);
  }

  return true;
}

/** Run one file against one kind, reporting the first few disagreements. */
void check_file(const char * name, GRX_BreakKind kind, size_t expected_rows) {
  std::vector<Row> rows;
  if (!read_break_test(ucd_path(name), &rows)) {
    // GTEST_SKIP() and not a bare `return`. This was a bare `return` from a
    // `void` helper until 2026-09-24, which records nothing, so gtest
    // reported the test PASSED - and `third_party/` is gitignored, so on a
    // fresh clone or any CI without the fetch step all four UAX files were
    // compared against nothing and this binary printed "[ PASSED ] 4 tests"
    // and exited 0. Confirmed by moving the directory aside and running it.
    // Indistinguishable from a real pass in both the exit status and the
    // test count, which is the one shape of missing gate that no amount of
    // reading the summary will catch.
    GTEST_SKIP() << name << ": no third_party/ucd; run tools/unicode/fetch.sh";
  }

  // The denominator, printed whether it is right or not, because "0
  // disagreements" means nothing without it. Reported even on success: a
  // file that parsed as forty rows would otherwise pass in silence.
  printf("%s: %zu rows\n", name, rows.size());

  // Exact, not a floor. UCD 17.0.0's counts, which is the version
  // ucd_path() pins. A truncated download, a half-written file or a silently bumped UCD
  // each change this number, and each of those used to read as a pass:
  // `ASSERT_GT(rows.size(), 100u)` let a file lose 99% of its cases and
  // still count. Bumping the UCD is meant to fail here - the new counts are
  // part of the bump.
  ASSERT_EQ(rows.size(), expected_rows)
      << name << " parsed " << rows.size() << " rows, expected "
      << expected_rows << " - a truncated file, or a UCD version change";

  size_t wrong = 0;
  size_t shown = 0;
  for (const Row & row : rows) {
    for (size_t at = 0; at <= row.subject.size(); at++) {
      // Only positions on a character boundary are asked about; the file
      // says nothing about the inside of a UTF-8 sequence.
      if (at < row.subject.size()
          && ((unsigned char)row.subject[at] & 0xC0) == 0x80) {
        continue;
      }
      // The ends are the dialect's business rather than the algorithm's:
      // this library answers them from Perl's rule, which differs from both
      // standards for an empty subject and from UAX #14 at the start.
      if (!at || at == row.subject.size()) {
        continue;
      }

      bool want = row.boundary[at];
      bool got = grx_unicode_break_at(kind, row.subject.data(),
                     row.subject.size(), at)
          != 0;
      if (want == got) {
        continue;
      }
      wrong++;
      if (shown++ < 8) {
        ADD_FAILURE() << name << ":" << row.line << " at byte " << at
                      << ": expected " << (want ? "a break" : "no break")
                      << ", got " << (got ? "a break" : "no break") << "\n"
                      << row.comment;
      }
    }
  }

  EXPECT_EQ(wrong, 0u) << name << ": " << wrong << " disagreements over "
                       << rows.size() << " lines";
  if (!wrong) {
    printf("%s: %zu lines, every position agrees\n", name, rows.size());
  }
}

} // namespace

TEST(Break, GraphemeClustersMatchTheUnicodeTestFile) {
  check_file("GraphemeBreakTest.txt", GRX_BREAK_GRAPHEME, 766);
}

TEST(Break, WordsMatchTheUnicodeTestFile) {
  check_file("WordBreakTest.txt", GRX_BREAK_WORD, 1944);
}

TEST(Break, SentencesMatchTheUnicodeTestFile) {
  check_file("SentenceBreakTest.txt", GRX_BREAK_SENTENCE, 512);
}

TEST(Break, LinesMatchTheUnicodeTestFile) {
  check_file("LineBreakTest.txt", GRX_BREAK_LINE, 19338);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
