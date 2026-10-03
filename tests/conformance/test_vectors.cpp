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
  /**
   * Whether the lazy DFA is what answered, and not merely what was asked.
   *
   * Asking an engine and counting the ask is how a population comes to hold
   * an engine that answers nothing: GRX_ENGINE_DFA resolves to the Pike VM
   * for a request it cannot serve, so "we asked for four engines" and "four
   * engines answered" are different claims and only the second is worth
   * printing.
   */
  bool dfa_answered = false;
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
    case grxtest::Expectation::Refused:
      return "refused";
    case grxtest::Expectation::Compiles:
      return "compiles";
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
  // ...unless the record states the groups as text instead, in which case
  // groups_agree() below owns every group past the ones spelled here and
  // this rule would contradict it: a vim record says `expect: 0-3` for the
  // whole match and `groups:` for the rest, and the groups it names are
  // exactly the ones that did participate.
  for (size_t i = record.spans.size();
      !record.has_groups && i < grx_match_count(match); i++) {
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

/** One group's text, or the empty string when it did not participate. */
std::string group_text(
    const grxtest::Record & record, const GRX_Match * match, size_t index) {
  if (index >= grx_match_count(match)) {
    return std::string();
  }
  GRX_Capture capture;
  grx_match_group(match, index, &capture);
  if (capture.start == GRX_NPOS) {
    return std::string();
  }
  return record.subject.substr(capture.start, capture.end - capture.start);
}

/**
 * Whether the groups agree with a record that states their text.
 *
 * Only for a `groups:` record - see rxt.h for why vim needs one. Group 0 is
 * not checked here: the whole match's span is in `expect:` and is exact, so
 * checking its text as well would assert less about the same thing.
 *
 * A group past the end of the list must be unset or empty, which is the same
 * rule the listed ones are held to. That is what stops a record with two
 * groups written down from being silent about a third.
 */
bool groups_agree(const grxtest::Record & record, const GRX_Match * match) {
  size_t highest = grx_match_count(match);
  if (record.groups.size() + 1 > highest) {
    highest = record.groups.size() + 1;
  }
  for (size_t i = 1; i < highest; i++) {
    std::string expected = i - 1 < record.groups.size()
        ? record.groups[i - 1]
        : std::string();
    if (group_text(record, match, i) != expected) {
      return false;
    }
  }
  return true;
}

/** The group texts as a record writes them, for a failure message. */
std::string describe_groups(const std::vector<std::string> & groups) {
  std::string out;
  for (size_t i = 0; i < groups.size(); i++) {
    if (i) {
      out += " ";
    }
    out += groups[i].empty() ? std::string("-") : "'" + groups[i] + "'";
  }
  return out.empty() ? std::string("(none)") : out;
}

/** The match's group texts, in the same shape. */
std::string describe_groups(
    const grxtest::Record & record, const GRX_Match * match) {
  std::vector<std::string> texts;
  for (size_t i = 1; i < grx_match_count(match); i++) {
    texts.push_back(group_text(record, match, i));
  }
  while (!texts.empty() && texts.back().empty()) {
    texts.pop_back();
  }
  return describe_groups(texts);
}

/**
 * Whether this dialect has a front end at all.
 *
 * Asked by compiling the simplest pattern there is. A dialect that refuses
 * `a` is one work-packages.md has not built yet, which is a different thing a
 * from dialect that refuses a particular construct - and the difference decides
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
/**
 * The allocator every vector is run through.
 *
 * NULL is the library default, and is what the ordinary pass uses. The
 * second pass points it at a grxtest::MovingAllocator so that every arena
 * growth in the whole corpus relocates the block - see
 * tests/unit/test_allocator.cpp for why that is worth doing, and why the
 * default allocator hides what it hides.
 */
const GRX_Allocator * g_allocator = nullptr;

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
      record.options, limits, g_allocator, &error, &regex);

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

  if (record.expectation == grxtest::Expectation::Refused) {
    // Which refusal is deliberately not asserted - see rxt.h. A dialect with
    // no front end at all refuses everything, so it would pass every record
    // here while answering nothing; that is a skip, exactly as it is below.
    if (compiled == GRX_ERR_UNSUPPORTED && !dialect_is_built(record.syntax)) {
      outcome.skipped = true;
      outcome.reason
          = std::string(grx_syntax_name(record.syntax)) + ": no front end yet";
    }
    else {
      outcome.passed = compiled != GRX_OK;
      if (!outcome.passed) {
        outcome.reason = "expected it to be refused, and it compiled";
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
    // The lazy DFA, which WP-41 added to the library and to
    // `tools/oracle/engine_diff.py` and not to this file - so until now no
    // checked-in vector was ever answered by it, and this is the only
    // population made of patterns a reference implementation was actually
    // asked about rather than ones a generator invented.
    //
    // Asked for every regular program and refused with GRX_ERR_UNSUPPORTED
    // for the rest, which is how the bit-state engine is asked two lines up:
    // the conditions are the program's, the library owns them, and a list
    // here that tried to restate them would be a second copy to drift.
    if (facts.is_regular) {
      engines.push_back(GRX_ENGINE_DFA);
    }
  }

  std::string first_spans;
  bool first = true;
  outcome.passed = true;

  for (GRX_Engine engine : engines) {
    GRX_Match * match = nullptr;
    if (grx_match_create(regex, g_allocator, &match) != GRX_OK) {
      outcome.passed = false;
      outcome.reason = "out of memory";
      break;
    }

    int matched = 0;
    GRX_Result result = grx_regex_search(regex, record.subject.data(),
        record.subject.size(), 0, engine, limits, match, &matched);

    std::string engine_name = engine == GRX_ENGINE_PIKE ? "pike"
        : engine == GRX_ENGINE_BITSTATE                   ? "bitstate"
        : engine == GRX_ENGINE_DFA                        ? "dfa"
                                                          : "backtrack";

    // A program whose behaviour depends on more than (instruction,
    // position) is refused by the bit-state engine rather than run without
    // its memo. That is an answer, not a failure, and the other engines
    // have already covered the vector.
    if (engine == GRX_ENGINE_BITSTATE && result == GRX_ERR_UNSUPPORTED) {
      grx_match_destroy(match);
      continue;
    }

    // Same rule for the DFA, and it refuses far more: it runs the regular
    // subset under leftmost-longest only, so every leftmost-first dialect -
    // which is most of them - comes back GRX_ERR_UNSUPPORTED here. That is
    // why the count of rows it *did* answer is reported rather than assumed.
    if (engine == GRX_ENGINE_DFA && result == GRX_ERR_UNSUPPORTED) {
      grx_match_destroy(match);
      continue;
    }
    if (engine == GRX_ENGINE_DFA && grx_match_engine(match) == GRX_ENGINE_DFA) {
      outcome.dfa_answered = true;
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
    else if (record.has_groups && !groups_agree(record, match)) {
      outcome.passed = false;
      outcome.reason = engine_name + ": expected groups "
          + describe_groups(record.groups) + ", got "
          + describe_groups(record, match);
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
  size_t reference_defects = 0;
  /**
   * Records the lazy DFA answered, which is not a score and is a denominator.
   *
   * It is printed for the reason the excluded count is printed: a population
   * that holds an engine answering nothing reports zero disagreements, and
   * so does one that holds an engine answering everything correctly. The
   * number is the only thing that tells them apart, and a reader who sees
   * 100.00% should be able to see what the DFA contributed to it.
   */
  size_t dfa_rows = 0;
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
 * Two kinds of entry, because two different things were being recorded
 * identically. A `gap` is this library answering differently from a reference
 * that is *right*, and it counts as a failure in the published rate. A
 * `reference-defect` is the reference being wrong - demonstrably, by an
 * argument written into the entry - and it leaves the denominator, because a
 * wrong expectation is not a question this library can be scored against.
 *
 * That second category is dangerous in one specific way: it is a lever that
 * raises the score, and the check on it is that the reason must *demonstrate*
 * the defect rather than assert it. The rule adopted with it is that the
 * excluded count is published on the same line as the rate, never silently
 * dropped, so a rate that rose because rows left the denominator says so.
 *
 * `seen` exists so that an entry naming a pattern the corpus no longer holds
 * is reported rather than left to rot.
 */
struct GapEntry {
  bool reference_defect = false;
  bool seen = false;
};

using KnownGaps = std::map<std::string, GapEntry>;

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

KnownGaps read_known_gaps(
    const std::string & directory, std::vector<std::string> * out_failures) {
  KnownGaps gaps;
  std::ifstream file(directory + "/known-gaps.txt");
  std::string line;
  size_t number = 0;
  while (std::getline(file, line)) {
    number++;
    if (line.empty() || line[0] == '#') {
      continue;
    }
    // dialect TAB flags TAB pattern TAB subject TAB category TAB reason. The
    // first four are the key; the category decides which denominator the
    // record lands in, and the reason is for the reader.
    size_t cut = 0;
    int fields = 0;
    for (; fields < 4 && cut != std::string::npos; fields++) {
      cut = line.find('\t', fields ? cut + 1 : 0);
    }
    if (cut == std::string::npos) {
      continue;
    }

    const std::string where
        = "known-gaps.txt:" + std::to_string(number) + ": ";
    const size_t category_end = line.find('\t', cut + 1);
    const std::string category = line.substr(cut + 1,
        category_end == std::string::npos ? std::string::npos
                                          : category_end - (cut + 1));

    GapEntry entry;
    if (category == "gap") {
      entry.reference_defect = false;
    }
    else if (category == "reference-defect") {
      entry.reference_defect = true;
    }
    else {
      // Never guessed at. An unreadable category that defaulted to `gap`
      // would be a silent demotion, and one that defaulted to
      // `reference-defect` would quietly raise the published rate - which is
      // the whole reason this field is not simply a prefix on the reason.
      out_failures->push_back(where + "unknown category \"" + category
          + "\"; it must be `gap` or `reference-defect`");
      continue;
    }

    // A category with no argument behind it is the rot this file exists to
    // prevent, and it matters most for the category that removes a row from
    // the denominator.
    if (category_end == std::string::npos
        || line.find_first_not_of(" \t", category_end) == std::string::npos) {
      out_failures->push_back(where + "a `" + category
          + "` entry with no reason; the reason is what makes it checkable");
      continue;
    }

    gaps[line.substr(0, cut)] = entry;
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
  KnownGaps gaps = read_known_gaps(directory, out_failures);

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

      // Counted before the pass/fail branching, because this is about which
      // engines the corpus reached and not about whether they were right.
      if (outcome.dfa_answered) {
        total.dfa_rows++;
        tally.dfa_rows++;
      }

      if (outcome.skipped) {
        total.skipped++;
        tally.skipped++;
      }
      else if (outcome.passed) {
        auto listed = gaps.find(gap_key(record));
        if (listed != gaps.end()) {
          // It passes now. The entry has to go, or the file stops being a
          // list of what is missing and becomes a list of what once was.
          //
          // For a reference defect "passes" means this library and the
          // reference now agree, which has two very different causes: the
          // reference was fixed and the vectors regenerated, or this library
          // started reproducing the defect. Both need a person, so both stop
          // the suite.
          total.failed++;
          tally.failed++;
          out_failures->push_back(record.source + ":"
              + std::to_string(record.line) + "\n" + record.text
              + (listed->second.reference_defect
                     ? "  -> listed in known-gaps.txt as a reference defect "
                       "and now agrees with the reference; either the "
                       "reference was fixed (remove the entry) or this "
                       "library now reproduces the defect (fix that)"
                     : "  -> listed in known-gaps.txt and passes; remove the "
                       "entry"));
          listed->second.seen = true;
          continue;
        }
        total.passed++;
        tally.passed++;
      }
      else if (gaps.count(gap_key(record))) {
        // A record this library is known not to answer the way the oracle
        // does, listed by hand in `known-gaps.txt`. Counted, never silent,
        // and never a pass.
        //
        // A `gap` stays in the denominator: the reference is right and this
        // library is not, so it is a question this library got wrong. A
        // `reference-defect` leaves it: the reference's answer is
        // demonstrably wrong, so agreeing with it would be the defect and
        // scoring against it measures nothing. The excluded count travels
        // with the rate so the exclusion is never invisible.
        GapEntry & entry = gaps[gap_key(record)];
        if (entry.reference_defect) {
          total.reference_defects++;
          tally.reference_defects++;
        }
        else {
          total.gaps++;
          tally.gaps++;
        }
        entry.seen = true;
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
    if (!entry.second.seen) {
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

  printf("\nconformance: %zu passed, %zu failed, %zu skipped, %zu known gaps, "
         "%zu excluded (reference defects), %zu answered by the lazy DFA\n",
      total.passed, total.failed, total.skipped, total.gaps,
      total.reference_defects, total.dfa_rows);
  for (const auto & entry : by_dialect) {
    // The known gaps are in the denominator. A rate that left them out would
    // be a rate over the vectors this library already answers, which is a
    // number that can only go up and means nothing.
    //
    // Reference defects are the one exception, and the reason they are safe
    // to exclude is the reason they are dangerous: the expectation itself is
    // wrong, so the row asks a question with no right answer. The protection
    // is that the count is printed here beside the rate whenever it is not
    // zero - a reader who sees 100% also sees what it is 100% of, on the
    // same line, and can go read the argument for each excluded row.
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
    if (entry.second.reference_defects) {
      printf("  %zu excluded (reference defects)",
          entry.second.reference_defects);
    }
    if (entry.second.dfa_rows) {
      printf("  %zu on the dfa", entry.second.dfa_rows);
    }
    printf("\n");
  }

  // An empty corpus passing is the failure mode this whole file exists to
  // avoid: a runner that discovered nothing would report success forever.
  EXPECT_GT(total.passed + total.failed, 0u)
      << "no vectors were found under " << grxtest::data("vectors");

  // And the same argument one engine down. The DFA was in this list for no
  // time at all before it was discovered to be missing from it, and the way
  // that happens again is a condition drifting until GRX_ERR_UNSUPPORTED is
  // the answer for every row - which reads as a clean sweep over an engine
  // that never ran. There is no right number here, only a wrong one.
  EXPECT_GT(total.dfa_rows, 0u)
      << "no vector was answered by the lazy DFA; it is in the engine list "
         "and something is refusing all of them";
}

TEST(Conformance, TheRateReadmePublishesIsTheRateTheRunnerFinds) {
  // design.md invariant 5 and work-packages.md §2's third condition both say
  // the pass rate is *published*. For a long time it was published only in
  // work-packages.md, in the past tense of a work package - `gnu-ere` at 98.15%
  // and three more like it - and every one of them had stopped being true, the
  // twelve known-gap entries behind them having been closed one at a time
  // with nothing to notice that the number three directories away had moved.
  //
  // So the table moved to README.md, and this is what stops it going the same
  // way. It is `check-status-line`'s argument applied to a second claim on
  // the same page: a figure whose trigger is somewhere else entirely - a
  // vector file regenerated, a known gap closed - is exactly the figure
  // nobody has a reason to re-read.
  std::vector<std::string> failures;
  std::map<std::string, Tally> by_dialect;
  run_directory(grxtest::data("vectors"), &failures, &by_dialect);

  const std::string readme = grxtest::read_file(grxtest::repo("README.md"));
  ASSERT_FALSE(readme.empty()) << "README.md could not be read";

  size_t rows = 0;
  for (const auto & entry : by_dialect) {
    size_t run = entry.second.passed + entry.second.failed + entry.second.gaps;
    if (!run) {
      continue;
    }

    // `| `<dialect>` | <count> | <rate>% |`, with the count written with
    // thousands separators the way a person reads it.
    std::string marker = "| `" + entry.first + "` |";
    size_t at = readme.find(marker);
    ASSERT_NE(at, std::string::npos)
        << entry.first << " has vectors and no row in README's table";
    size_t end = readme.find('\n', at);
    std::string row = readme.substr(at, end - at);

    std::string digits;
    for (char c : row.substr(marker.size())) {
      if (c == '|') {
        break;
      }
      if (c >= '0' && c <= '9') {
        digits += c;
      }
    }
    EXPECT_EQ(digits, std::to_string(run))
        << entry.first << ": README says " << digits << " vectors, the runner "
        << "ran " << run << " (" << row << ")";

    char rate[32];
    snprintf(rate, sizeof(rate), "%.2f%%",
        100.0 * (double)entry.second.passed / (double)run);
    EXPECT_NE(row.find(rate), std::string::npos)
        << entry.first << ": README does not say " << rate << " (" << row
        << ")";
    rows++;
  }

  // Arming, and the direction that matters: a table with no rows in it at all
  // would agree with every dialect vacuously.
  EXPECT_EQ(rows, by_dialect.size());
  EXPECT_GE(rows, 9u) << "fewer dialects checked than the tree has corpora";
}

TEST(Conformance, EveryVectorAgreesAgainWhenEveryArenaMoves) {
  // The same corpus through an allocator whose `realloc` always relocates
  // the block, scribbling over the old one first.
  //
  // What it is for: an arena that grows invalidates every pointer into it,
  // and the system allocator almost never lets that show - `realloc` grows a
  // small block in place whenever the bytes after it are free, which for the
  // mostly sequential allocations a compile makes is nearly every time. A
  // pointer held across a growth therefore passes every test, valgrind and
  // ASan, until one day a pattern is a few nodes longer.
  //
  // The whole corpus is the widest net this library has, so it is the one
  // worth pointing at the question. Under ASan the stale read is a
  // use-after-free; here it is 0xDD, and a vector whose answer is made of
  // 0xDD does not match its oracle.
  grxtest::MovingAllocator allocator;
  g_allocator = allocator.get();

  std::vector<std::string> failures;
  std::map<std::string, Tally> by_dialect;
  Tally total = run_directory(
      grxtest::data("vectors"), &failures, &by_dialect);

  g_allocator = nullptr;

  for (const std::string & failure : failures) {
    ADD_FAILURE() << "with a moving allocator: " << failure;
  }
  printf("\nmoving allocator: %zu passed, %zu failed, %ld blocks relocated\n",
      total.passed, total.failed, allocator.moves());

  EXPECT_GT(total.passed + total.failed, 0u);
  // A run in which no realloc moved anything would pass while testing
  // nothing at all.
  EXPECT_GT(allocator.moves(), 0);
  EXPECT_EQ(allocator.live(), 0);
}

TEST(Conformance, TheRunnerFailsAVectorThatIsWrong) {
  // testing.md section 9: the gates are themselves tested. This corpus holds
  // records whose expectations are deliberately wrong; the runner must
  // notice. Without this, "the conformance suite passes" would be
  // indistinguishable from "the conformance suite ran nothing".
  //
  // Three of them, and they are wrong about different things. One has the
  // wrong span, which is what every corpus here states. One has the right
  // span and the wrong `groups:` - the text form a vim vector has to use -
  // because a runner that parsed that field and never compared it would pass
  // every vim vector in the tree while checking only their whole matches. And
  // one claims a pattern is refused when it compiles, because `expect:
  // refused` asserts less than the other expectations and a check that
  // asserts little is the one to make sure asserts something.
  //
  // Beside them, `gaps.rxt` and a `known-gaps.txt` of its own arm the three
  // things the real known-gaps file no longer can. It held five `gap` rows
  // until the Perl case transform closed the last of them, and what is left
  // is reference defects - so the `gap` category, the check that a listed
  // record which has started passing is a failure, and the check that an
  // entry naming no record at all is a failure were about to become code no
  // test reaches. Two more failures come from there, which is why this
  // expects five.
  std::vector<std::string> failures;
  std::map<std::string, Tally> by_dialect;
  Tally total = run_directory(
      grxtest::data("vectors_selftest"), &failures, &by_dialect);

  // Four records fail; the fifth failure is the entry that names no record
  // at all, which the counter does not see because the counter counts
  // records. Both halves matter, so both are asserted: the suite fails on
  // the *list*, not on the tally.
  EXPECT_EQ(total.failed, 4u)
      << "the deliberately wrong vectors were not caught";
  EXPECT_GT(total.passed, 0u) << "the correct vectors beside it should pass";
  // One counted gap and one exclusion, from records that are wrong and
  // listed. These are the two denominators the real corpus's rate is built
  // from, and a runner that stopped counting either would still be green.
  EXPECT_EQ(total.gaps, 1u) << "a listed gap was not counted as one";
  EXPECT_EQ(total.reference_defects, 1u)
      << "a listed reference defect was not excluded";
  ASSERT_EQ(failures.size(), 5u) << [&] {
    std::string joined;
    for (const std::string & failure : failures) {
      joined += "\n" + failure;
    }
    return joined;
  }();
  // Searched rather than indexed: the files are read in directory order, so
  // which failure is first is not this test's to know.
  bool named_a_span = false;
  for (const std::string & failure : failures) {
    if (failure.find("expected 0-3") != std::string::npos) {
      named_a_span = true;
    }
  }
  EXPECT_TRUE(named_a_span)
      << "the wrong-span record failed for some other reason";
  bool named_the_groups = false;
  for (const std::string & failure : failures) {
    if (failure.find("expected groups") != std::string::npos) {
      named_the_groups = true;
    }
  }
  EXPECT_TRUE(named_the_groups)
      << "the wrong `groups:` record failed for some other reason";
  bool named_the_refusal = false;
  for (const std::string & failure : failures) {
    if (failure.find("expected it to be refused") != std::string::npos) {
      named_the_refusal = true;
    }
  }
  EXPECT_TRUE(named_the_refusal)
      << "the wrong `expect: refused` record failed for some other reason";

  bool named_the_stale_entry = false;
  bool named_the_absent_record = false;
  for (const std::string & failure : failures) {
    if (failure.find("listed in known-gaps.txt and passes")
        != std::string::npos) {
      named_the_stale_entry = true;
    }
    if (failure.find("names a record the corpus does not have")
        != std::string::npos) {
      named_the_absent_record = true;
    }
  }
  EXPECT_TRUE(named_the_stale_entry)
      << "an entry whose record passes was not reported";
  EXPECT_TRUE(named_the_absent_record)
      << "an entry naming no record was not reported";
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

  // `groups:` - the text form. The sentinel and the two escapes it forces
  // are the whole of what a reader can get wrong here: a `-` read as a group
  // whose text is "-", or a `\x20` left as four characters, would each make
  // a vim vector assert something other than what vim said.
  ASSERT_GE(file.records.size(), 8u);
  EXPECT_FALSE(file.records[0].has_groups);
  EXPECT_TRUE(file.records[6].has_groups);
  ASSERT_EQ(file.records[6].groups.size(), 3u);
  EXPECT_EQ(file.records[6].groups[0], "a");
  EXPECT_EQ(file.records[6].groups[1], "");
  EXPECT_EQ(file.records[6].groups[2], "c");

  ASSERT_EQ(file.records[7].groups.size(), 2u);
  EXPECT_EQ(file.records[7].groups[0], " ");
  EXPECT_EQ(file.records[7].groups[1], "-");

  ASSERT_GE(file.records.size(), 9u);
  EXPECT_EQ(file.records[8].expectation, grxtest::Expectation::Refused);
  EXPECT_FALSE(file.records[8].has_subject);
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
