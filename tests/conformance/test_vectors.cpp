/**
 * @file
 *
 * The conformance runner: every `.rxt` vector, on every engine that can run
 * it.
 *
 * documentation/testing.md section 4. One gtest binary that discovers the
 * vector files, runs each record, checks it against what the oracle said, and
 * checks every eligible engine against every other.
 *
 * Two properties this file exists to have, and the reason for each:
 *
 * **A failure prints the record verbatim.** A conformance failure is a bug
 * report, and a bug report that says "vector 4127 failed" costs whoever reads
 * it a trip to the file. The record is four lines; printing them is free.
 *
 * **The gate is itself tested.** testing.md section 9: a vector file with a
 * deliberately wrong expectation must fail, and `tests/data/vectors/selftest`
 * holds one that does - run by a test that expects it to fail, so that the
 * runner reporting success over a corpus it silently skipped is not a
 * possible outcome.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdio>
#include <map>
#include <string>
#include <vector>

#include "test_helpers.h"

#include "rxt.h"

namespace {

/** What running one record produced. */
struct Outcome {
  bool passed = false;
  bool skipped = false;
  std::string reason;   ///< Why it failed, or why it was skipped.
};

/** Spans as text, for a failure message. */
std::string describe(const GRX_Match * match) {
  if (!match) {
    return "nomatch";
  }
  std::string out;
  for (size_t i = 0; i < grx_match_count(match); i++) {
    GRX_Capture capture;
    grx_match_group(match, i, &capture);
    if (i) {
      out += " ";
    }
    out += capture.start == GRX_NPOS
        ? std::string("-")
        : std::to_string(capture.start) + "-" + std::to_string(capture.end);
  }
  return out;
}

/** The expectation as text, for a failure message. */
std::string describe(const grxtest::Record & record) {
  switch (record.expectation) {
    case grxtest::Expectation::NoMatch:
      return "nomatch";
    case grxtest::Expectation::Limit:
      return "limit";
    case grxtest::Expectation::Error:
      return std::string("error ") + grx_result_string(record.error);
    case grxtest::Expectation::Spans:
    default: {
      std::string out;
      for (size_t i = 0; i < record.spans.size(); i++) {
        if (i) {
          out += " ";
        }
        out += record.spans[i].set
            ? std::to_string(record.spans[i].start) + "-"
                + std::to_string(record.spans[i].end)
            : std::string("-");
      }
      return out;
    }
  }
}

/** Whether a match agrees with what the record expects. */
bool spans_agree(const grxtest::Record & record, const GRX_Match * match) {
  if (grx_match_count(match) != record.spans.size()) {
    return false;
  }
  for (size_t i = 0; i < record.spans.size(); i++) {
    GRX_Capture capture;
    grx_match_group(match, i, &capture);
    bool set = capture.start != GRX_NPOS;
    if (set != record.spans[i].set) {
      return false;
    }
    if (set
        && (capture.start != record.spans[i].start
            || capture.end != record.spans[i].end)) {
      return false;
    }
  }
  return true;
}

/** Run one record and say what happened. */
Outcome run_record(const grxtest::Record & record) {
  Outcome outcome;

  if (!record.skip.empty()) {
    outcome.skipped = true;
    outcome.reason = record.skip;
    return outcome;
  }

  const GRX_Limits * limits = record.has_limits ? &record.limits : nullptr;

  GRX_Error error;
  grx_error_clear(&error);
  GRX_Regex * regex = nullptr;
  GRX_Result compiled = grx_regex_compile_with_allocator(
      record.pattern.data(), record.pattern.size(), record.syntax,
      record.options, limits, nullptr, &error, &regex);

  if (record.expectation == grxtest::Expectation::Error) {
    outcome.passed = compiled == record.error;
    if (!outcome.passed) {
      outcome.reason = std::string("expected error ")
          + grx_result_string(record.error) + ", got "
          + grx_result_string(compiled);
    }
    grx_regex_free(regex);
    return outcome;
  }

  if (compiled != GRX_OK) {
    outcome.reason
        = std::string("compile failed: ") + grx_result_string(compiled) + " ("
        + grx_diag_string(error.diag) + ")";
    grx_regex_free(regex);
    return outcome;
  }

  // Which engines to ask. The record may name them; otherwise every engine
  // that can run this program is asked, and they must agree - which is the
  // equivalence invariant of design.md section 3.5.4, checked here on every
  // vector rather than only on the fuzzer's random ones.
  std::vector<GRX_Engine> engines = record.engines;
  if (engines.empty()) {
    GRX_Facts facts;
    grx_regex_facts(regex, &facts);
    if (facts.is_regular) {
      engines.push_back(GRX_ENGINE_PIKE);
    }
    // The bit-state engine is the backtracker with a memo, so asking it on
    // every vector it is eligible for is how "the memo never changes an
    // answer" gets checked - on a thousand real patterns rather than on the
    // handful a unit test would think to write. A program it cannot run
    // comes back GRX_ERR_UNSUPPORTED below, so asking costs nothing.
    if (!facts.has_backreference && !facts.has_lookaround
        && !facts.has_recursion) {
      engines.push_back(GRX_ENGINE_BITSTATE);
    }
    engines.push_back(GRX_ENGINE_BACKTRACK);
  }

  std::string first_spans;
  bool first = true;
  outcome.passed = true;

  for (GRX_Engine engine : engines) {
    GRX_Match * match = nullptr;
    if (grx_match_create(regex, nullptr, &match) != GRX_OK) {
      outcome.passed = false;
      outcome.reason = "out of memory";
      break;
    }

    int matched = 0;
    GRX_Result result = grx_regex_search(regex, record.subject.data(),
        record.subject.size(), 0, engine, limits, match, &matched);

    std::string engine_name = engine == GRX_ENGINE_PIKE ? "pike"
        : engine == GRX_ENGINE_BITSTATE                   ? "bitstate"
                                                          : "backtrack";

    // A program whose behaviour depends on more than (instruction,
    // position) is refused by the bit-state engine rather than run without
    // its memo. That is an answer, not a failure, and the other engines
    // have already covered the vector.
    if (engine == GRX_ENGINE_BITSTATE && result == GRX_ERR_UNSUPPORTED) {
      grx_match_destroy(match);
      continue;
    }

    if (record.expectation == grxtest::Expectation::Limit) {
      if (result != GRX_ERR_LIMIT) {
        outcome.passed = false;
        outcome.reason = engine_name + ": expected a limit, got "
            + grx_result_string(result);
      }
      grx_match_destroy(match);
      continue;
    }

    if (result != GRX_OK) {
      outcome.passed = false;
      outcome.reason = engine_name + ": " + grx_result_string(result);
      grx_match_destroy(match);
      break;
    }

    if (record.expectation == grxtest::Expectation::NoMatch) {
      if (matched) {
        outcome.passed = false;
        outcome.reason
            = engine_name + ": expected nomatch, got " + describe(match);
      }
    }
    else if (!matched) {
      outcome.passed = false;
      outcome.reason = engine_name + ": expected " + describe(record)
          + ", got nomatch";
    }
    else if (!spans_agree(record, match)) {
      outcome.passed = false;
      outcome.reason = engine_name + ": expected " + describe(record)
          + ", got " + describe(match);
    }

    std::string spans = matched ? describe(match) : "nomatch";
    if (first) {
      first_spans = spans;
      first = false;
    }
    else if (spans != first_spans) {
      outcome.passed = false;
      outcome.reason = "the engines disagree: " + first_spans + " and "
          + spans + " (" + engine_name + ")";
    }

    grx_match_destroy(match);
    if (!outcome.passed) {
      break;
    }
  }

  grx_regex_free(regex);
  return outcome;
}

/** Counts, per dialect and per file, for the summary the README publishes. */
struct Tally {
  size_t passed = 0;
  size_t failed = 0;
  size_t skipped = 0;
};

/**
 * Run every vector under a directory.
 *
 * `out_failures` receives a readable report of each failure; the counts come
 * back so that a caller can insist the corpus was not empty.
 */
Tally run_directory(const std::string & directory,
    std::vector<std::string> * out_failures,
    std::map<std::string, Tally> * out_by_dialect) {
  Tally total;

  for (const std::string & path : grxtest::find_vector_files(directory)) {
    grxtest::VectorFile file;
    std::string error;
    if (!grxtest::read_vector_file(path, &file, &error)) {
      total.failed++;
      out_failures->push_back("unreadable: " + error);
      continue;
    }

    for (const grxtest::Record & record : file.records) {
      Outcome outcome = run_record(record);
      const char * dialect = grx_syntax_name(record.syntax);
      Tally & tally = (*out_by_dialect)[dialect];

      if (outcome.skipped) {
        total.skipped++;
        tally.skipped++;
      }
      else if (outcome.passed) {
        total.passed++;
        tally.passed++;
      }
      else {
        total.failed++;
        tally.failed++;
        out_failures->push_back(record.source + ":"
            + std::to_string(record.line) + "\n" + record.text + "  -> "
            + outcome.reason);
      }
    }
  }

  return total;
}

} // namespace

TEST(Conformance, EveryVectorAgreesWithItsOracle) {
  std::vector<std::string> failures;
  std::map<std::string, Tally> by_dialect;
  Tally total = run_directory(
      grxtest::data("vectors"), &failures, &by_dialect);

  for (const std::string & failure : failures) {
    ADD_FAILURE() << failure;
  }

  printf("\nconformance: %zu passed, %zu failed, %zu skipped\n", total.passed,
      total.failed, total.skipped);
  for (const auto & entry : by_dialect) {
    size_t run = entry.second.passed + entry.second.failed;
    printf("  %-14s %zu/%zu", entry.first.c_str(), entry.second.passed, run);
    if (run) {
      printf("  (%.2f%%)", 100.0 * (double)entry.second.passed / (double)run);
    }
    if (entry.second.skipped) {
      printf("  %zu skipped", entry.second.skipped);
    }
    printf("\n");
  }

  // An empty corpus passing is the failure mode this whole file exists to
  // avoid: a runner that discovered nothing would report success forever.
  EXPECT_GT(total.passed + total.failed, 0u)
      << "no vectors were found under " << grxtest::data("vectors");
}

TEST(Conformance, TheRunnerFailsAVectorThatIsWrong) {
  // testing.md section 9: the gates are themselves tested. This corpus holds
  // one record whose expectation is deliberately wrong; the runner must
  // notice. Without this, "the conformance suite passes" would be
  // indistinguishable from "the conformance suite ran nothing".
  std::vector<std::string> failures;
  std::map<std::string, Tally> by_dialect;
  Tally total = run_directory(
      grxtest::data("vectors_selftest"), &failures, &by_dialect);

  EXPECT_EQ(total.failed, 1u)
      << "the deliberately wrong vector was not caught";
  EXPECT_GT(total.passed, 0u) << "the correct vectors beside it should pass";
  ASSERT_EQ(failures.size(), 1u) << [&] {
    std::string joined;
    for (const std::string & failure : failures) {
      joined += "\n" + failure;
    }
    return joined;
  }();
  EXPECT_NE(failures[0].find("expected"), std::string::npos) << failures[0];
}

// --------------------------------------------------------------------------
// The reader's own tests
// --------------------------------------------------------------------------

TEST(Rxt, DecodesTheEscapesTheFormatDefines) {
  struct {
    const char * field;
    const char * expected;
  } cases[] = {
    {"abc", "abc"},
    {"a\\nb", "a\nb"},
    {"\\t\\r\\f\\v", "\t\r\f\v"},
    {"\\\\", "\\"},
    {"\\x41", "A"},
    {"\\x00", "\x00"},
    {"\\u0041", "A"},
    {"\\u{41}", "A"},
    {"\\u{1F41F}", "\xF0\x9F\x90\x9F"},
    {"\\u00e9", "\xC3\xA9"},
    // Not an escape the format defines, so both characters come through: a
    // vector holding `\d` means `\d`, and does not need the reader to know
    // what `\d` is.
    {"\\d", "\\d"},
    {"(a|b)\\1", "(a|b)\\1"},
  };

  for (const auto & test : cases) {
    std::string value;
    std::string error;
    ASSERT_TRUE(grxtest::decode_field(test.field, &value, &error))
        << test.field << ": " << error;
    EXPECT_EQ(value, std::string(test.expected,
                         test.expected[0] == '\0' ? 1 : strlen(test.expected)))
        << test.field;
  }
}

TEST(Rxt, RejectsAMalformedEscapeRatherThanGuessing) {
  const char * bad[] = {"a\\", "\\x4", "\\xZZ", "\\u00", "\\u{", "\\u{}"};

  for (const char * field : bad) {
    std::string value;
    std::string error;
    EXPECT_FALSE(grxtest::decode_field(field, &value, &error)) << field;
    EXPECT_FALSE(error.empty()) << field;
  }
}

TEST(Rxt, ReadsARecordAndItsOptionalFields) {
  const std::string path = grxtest::data("vectors_selftest/reader.rxt");
  grxtest::VectorFile file;
  std::string error;
  ASSERT_TRUE(grxtest::read_vector_file(path, &file, &error)) << error;

  ASSERT_GE(file.records.size(), 4u);
  EXPECT_EQ(file.records[0].syntax, GRX_SYNTAX_ECMASCRIPT);
  EXPECT_EQ(file.records[0].pattern, "a(b|c)*d");
  EXPECT_EQ(file.records[0].subject, "abcd");
  EXPECT_EQ(file.records[0].expectation, grxtest::Expectation::Spans);
  ASSERT_EQ(file.records[0].spans.size(), 2u);
  EXPECT_TRUE(file.records[0].spans[0].set);
  EXPECT_EQ(file.records[0].spans[0].end, 4u);
  EXPECT_TRUE(file.records[0].spans[1].set);
  EXPECT_EQ(file.records[0].spans[1].start, 2u);

  // A record without a subject is how an `error` expectation is written.
  EXPECT_EQ(file.records[1].expectation, grxtest::Expectation::Error);
  EXPECT_EQ(file.records[1].error, GRX_ERR_SYNTAX);
  EXPECT_FALSE(file.records[1].has_subject);

  EXPECT_EQ(file.records[2].flags, "u");
  EXPECT_NE(file.records[2].options & GRX_OPT_UTF, 0u);

  ASSERT_EQ(file.records[3].engines.size(), 1u);
  EXPECT_EQ(file.records[3].engines[0], GRX_ENGINE_BACKTRACK);
  EXPECT_TRUE(file.records[3].has_limits);
  EXPECT_EQ(file.records[3].limits.max_steps, 1000u);
}

TEST(Rxt, RejectsAFileItCannotRead) {
  grxtest::VectorFile file;
  std::string error;
  EXPECT_FALSE(
      grxtest::read_vector_file(grxtest::data("vectors/nosuch.rxt"), &file,
          &error));
  EXPECT_FALSE(error.empty());
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
