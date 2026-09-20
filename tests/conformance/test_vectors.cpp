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
#include <fstream>
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

/**
 * Whether a match agrees with what the record expects.
 *
 * A record may list fewer spans than the pattern has groups, and that is not
 * a short record: Perl's `@-` and `@+` stop at the highest-numbered group
 * that participated, so the oracle for `(x)?(?(1)b|a)` against "a" reports
 * one span and not two. Every group the record does not mention must
 * therefore be *unset* - which is the assertion, not an exemption from one.
 * A record that listed more spans than the match has is still wrong.
 */
bool spans_agree(const grxtest::Record & record, const GRX_Match * match) {
  if (grx_match_count(match) < record.spans.size()) {
    return false;
  }
  for (size_t i = record.spans.size(); i < grx_match_count(match); i++) {
    GRX_Capture capture;
    grx_match_group(match, i, &capture);
    if (capture.start != GRX_NPOS) {
      return false;
    }
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

/**
 * Whether this dialect has a front end at all.
 *
 * Asked by compiling the simplest pattern there is. A dialect that refuses
 * `a` is one plan.md has not built yet, which is a different thing from a
 * dialect that refuses a particular construct - and the difference decides
 * whether a record is a failure or a skip. Cached, because the answer cannot
 * change during a run.
 */
bool dialect_is_built(GRX_Syntax syntax) {
  static std::map<int, bool> known;
  auto found = known.find((int)syntax);
  if (found != known.end()) {
    return found->second;
  }
  GRX_Error error;
  grx_error_clear(&error);
  GRX_Regex * regex = nullptr;
  bool built = grx_regex_compile_with_allocator(
                   "a", 1, syntax, 0, nullptr, nullptr, &error, &regex)
      == GRX_OK;
  grx_regex_free(regex);
  known[(int)syntax] = built;
  return built;
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

  if (record.expectation == grxtest::Expectation::Compiles) {
    outcome.passed = compiled == GRX_OK;
    if (!outcome.passed) {
      // A dialect with no front end at all is a skip rather than a failure:
      // the corpus is imported before the front end that reads it, on
      // purpose, so that the front end's first run has something to be
      // measured against. `dialect_is_built` distinguishes that from a
      // construct this dialect does not support.
      if (compiled == GRX_ERR_UNSUPPORTED && !dialect_is_built(record.syntax)) {
        outcome.passed = false;
        outcome.skipped = true;
        outcome.reason = std::string(grx_syntax_name(record.syntax))
            + ": no front end yet";
      }
      else {
        outcome.reason = std::string("expected it to compile, got ")
            + grx_result_string(compiled) + " (" + grx_diag_string(error.diag)
            + ")";
      }
    }
    grx_regex_free(regex);
    return outcome;
  }

  if (compiled == GRX_ERR_UNSUPPORTED && !dialect_is_built(record.syntax)) {
    outcome.skipped = true;
    outcome.reason
        = std::string(grx_syntax_name(record.syntax)) + ": no front end yet";
    grx_regex_free(regex);
    return outcome;
  }

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
  size_t gaps = 0;
};

/**
 * The records this library is known not to answer the way its oracle does.
 *
 * A dialect arrives one construct at a time, and between the first commit of
 * a front end and the last there are patterns the reference compiles and this
 * library does not. Two ways to hold that were rejected: letting the suite be
 * red, which makes a gate nobody reads, and publishing a percentage with no
 * list behind it, which makes a number nobody can check. This is the third -
 * every gap named in a file, with the construct that is missing written
 * beside it.
 *
 * The file is a gate in both directions. An unlisted failure fails the suite,
 * and a *listed* record that starts passing fails it too: the entry has to be
 * deleted, so the list can only shrink by someone noticing.
 *
 * The value is "was it seen", so that an entry naming a pattern the corpus no
 * longer holds is reported rather than left to rot.
 */
using KnownGaps = std::map<std::string, bool>;

/** Escape a field so that it cannot split a tab-separated line. */
std::string escape_field(const std::string & value) {
  std::string out;
  for (char c : value) {
    switch (c) {
      case '\\': out += "\\\\"; break;
      case '\t': out += "\\t"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      default: out += c; break;
    }
  }
  return out;
}

/**
 * The key: the dialect, the flags, the pattern and the subject.
 *
 * The subject is part of it because one pattern appears with several, and a
 * key without it listed a gap for all of them: `^(a\1?){4}$` answers three of
 * its four subjects correctly, and an entry naming only the pattern excused
 * the three that were already right.
 *
 * Both text fields are escaped, because a decoded one may hold a tab or a
 * newline. The same escaping is written into the file, so reading it needs no
 * decoder - two keys are equal when their text is.
 */
std::string gap_key(const grxtest::Record & record) {
  return std::string(grx_syntax_name(record.syntax)) + "\t" + record.flags
      + "\t" + escape_field(record.pattern) + "\t"
      + escape_field(record.subject);
}

KnownGaps read_known_gaps(const std::string & directory) {
  KnownGaps gaps;
  std::ifstream file(directory + "/known-gaps.txt");
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    // dialect TAB flags TAB pattern TAB subject TAB reason. The reason is for
    // the reader and is not part of the key.
    size_t cut = 0;
    int fields = 0;
    for (; fields < 4 && cut != std::string::npos; fields++) {
      cut = line.find('\t', fields ? cut + 1 : 0);
    }
    if (cut == std::string::npos) {
      continue;
    }
    gaps[line.substr(0, cut)] = false;
  }
  return gaps;
}

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
  KnownGaps gaps = read_known_gaps(directory);

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
        if (gaps.count(gap_key(record))) {
          // It passes now. The entry has to go, or the file stops being a
          // list of what is missing and becomes a list of what once was.
          total.failed++;
          tally.failed++;
          out_failures->push_back(record.source + ":"
              + std::to_string(record.line) + "\n" + record.text
              + "  -> listed in known-gaps.txt and passes; remove the entry");
          gaps[gap_key(record)] = true;
          continue;
        }
        total.passed++;
        tally.passed++;
      }
      else if (gaps.count(gap_key(record))) {
        // A record this library is known not to answer the way the oracle
        // does, listed by hand in `known-gaps.txt` with the construct that
        // is missing. Counted, never silent, and never a pass: the rate the
        // README publishes is the rate without these.
        total.gaps++;
        tally.gaps++;
        gaps[gap_key(record)] = true;
      }
      else {
        total.failed++;
        tally.failed++;
        out_failures->push_back(record.source + ":"
            + std::to_string(record.line) + "\n" + record.text + "  -> "
            + outcome.reason + "\nknown-gaps.txt line, if it is one:\n"
            + gap_key(record) + "\t" + outcome.reason);
      }
    }
  }

  // An entry naming a record that is no longer in the corpus is as stale as
  // one whose record now passes, and nothing above can notice it: the loop
  // only ever sees records. A corpus shrinks - an importer that learns a
  // modifier it cannot express drops the cases carrying it - and the entries
  // those cases left behind would otherwise sit in the file for ever,
  // counted in no denominator and excusing nothing.
  for (const auto & entry : gaps) {
    if (!entry.second) {
      out_failures->push_back("known-gaps.txt names a record the corpus does "
          "not have; remove the entry:\n" + entry.first);
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

  printf("\nconformance: %zu passed, %zu failed, %zu skipped, %zu known gaps\n",
      total.passed, total.failed, total.skipped, total.gaps);
  for (const auto & entry : by_dialect) {
    // The known gaps are in the denominator. A rate that left them out would
    // be a rate over the vectors this library already answers, which is a
    // number that can only go up and means nothing.
    size_t run = entry.second.passed + entry.second.failed + entry.second.gaps;
    printf("  %-14s %zu/%zu", entry.first.c_str(), entry.second.passed, run);
    if (run) {
      printf("  (%.2f%%)", 100.0 * (double)entry.second.passed / (double)run);
    }
    if (entry.second.skipped) {
      printf("  %zu skipped", entry.second.skipped);
    }
    if (entry.second.gaps) {
      printf("  %zu known gaps", entry.second.gaps);
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

  // `options:` exists for POSIX and GNU, whose options are arguments to
  // regcomp rather than letters a pattern author writes, so there is no
  // alphabet for `flags:` to spell them in. It has to *combine* with
  // `flags:` rather than replace it, and neither order may win - the flags
  // are turned into options when the record closes, which is after both
  // lines have been read whichever way round they came.
  ASSERT_GE(file.records.size(), 6u);
  EXPECT_EQ(file.records[4].flags, "i");
  EXPECT_NE(file.records[4].options & GRX_OPT_CASELESS, 0u);
  EXPECT_NE(file.records[4].options & GRX_OPT_DOTALL, 0u);
  EXPECT_NE(file.records[4].options & GRX_OPT_MULTILINE, 0u);

  EXPECT_NE(file.records[5].options & GRX_OPT_CASELESS, 0u);
  EXPECT_NE(file.records[5].options & GRX_OPT_DOTALL, 0u);
}

TEST(Rxt, RejectsAnOptionItDoesNotKnow) {
  // The same rule the engine and limit fields follow: a name the reader does
  // not know is a failure rather than a silently ignored line, because a
  // vector that quietly lost its option is a vector asserting the wrong
  // thing and passing.
  const std::string path = grxtest::data("vectors_selftest/reader.rxt");
  grxtest::VectorFile file;
  std::string error;
  ASSERT_TRUE(grxtest::read_vector_file(path, &file, &error)) << error;

  const std::string bad = grxtest::data("vectors_selftest/bad-option.rxt");
  std::ofstream out(bad);
  ASSERT_TRUE(out.good());
  out << "dialect: ecmascript\n\npattern: a\noptions: nosuchoption\n"
      << "subject: a\nexpect: 0-1\n";
  out.close();

  grxtest::VectorFile broken;
  EXPECT_FALSE(grxtest::read_vector_file(bad, &broken, &error));
  EXPECT_NE(error.find("unknown option"), std::string::npos) << error;
  std::remove(bad.c_str());
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
