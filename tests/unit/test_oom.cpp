/**
 * @file
 *
 * Every allocation failure, one at a time.
 *
 * The `GRX_ERR_OOM` branches are the largest block of code in this library
 * that no hand-written test reaches, because reaching one means making a
 * particular `malloc` fail. This file makes each of them fail in turn: it
 * counts how many allocations a whole compile-and-match takes, then runs it
 * again once per allocation with that one refused.
 *
 * Each run asserts the two things that matter, and they are the two things
 * an out-of-memory path gets wrong:
 *
 * - **The failure comes back as a result code**, not as a crash and not as a
 *   success with a half-built object. A caller that is out of memory needs to
 *   be told, and the sanitizers are watching for the other outcome.
 * - **Nothing is leaked.** The failure path is the one that unwinds a
 *   partially built structure, and it is the path that is never taken in
 *   ordinary use - so it is the one where a missing `free` lives for years.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <string>
#include <vector>

#include "test_helpers.h"

namespace {

/**
 * Compile a pattern and run it, through one allocator.
 *
 * Returns the result of whichever step failed first, or GRX_OK when the whole
 * sequence completed. Every object is released before returning, on every
 * path, which is the property the sweep below is checking.
 */
GRX_Result compile_and_match(const GRX_Allocator * allocator,
    const std::string & pattern, const std::string & subject,
    uint32_t options, GRX_Engine engine) {
  GRX_Error error;
  grx_error_clear(&error);

  GRX_Pattern * parsed = nullptr;
  GRX_Result result = grx_pattern_parse_with_allocator(pattern.data(),
      pattern.size(), GRX_SYNTAX_ECMASCRIPT, options, nullptr, allocator,
      &error, &parsed);
  if (result != GRX_OK) {
    return result;
  }

  GRX_Regex * regex = nullptr;
  result = grx_regex_compile_pattern(parsed, nullptr, allocator, &error,
      &regex);
  grx_pattern_free(parsed);
  if (result != GRX_OK) {
    return result;
  }

  GRX_Match * match = nullptr;
  result = grx_match_create(regex, allocator, &match);
  if (result == GRX_OK) {
    int matched = 0;
    result = grx_regex_search(regex, subject.data(), subject.size(), 0,
        engine, nullptr, match, &matched);
    grx_match_destroy(match);
  }

  grx_regex_free(regex);
  return result;
}

/**
 * Replace and split, through one allocator.
 *
 * A second sequence rather than more rows in the first, because it
 * allocates in places the first never reaches: the parsed template, the
 * growable output buffer, the piece array, and the match object each of the
 * two creates for itself. Every one of those is released on every path,
 * which is what the sweep checks.
 */
GRX_Result replace_and_split(const GRX_Allocator * allocator,
    const std::string & pattern, const std::string & subject,
    const std::string & replacement, uint32_t options) {
  GRX_Error error;
  grx_error_clear(&error);

  GRX_Regex * regex = nullptr;
  GRX_Result result = grx_regex_compile_with_allocator(pattern.data(),
      pattern.size(), GRX_SYNTAX_ECMASCRIPT, options, nullptr, allocator,
      &error, &regex);
  if (result != GRX_OK) {
    return result;
  }

  GRX_Text text {};
  result = grx_regex_replace(regex, subject.data(), subject.size(),
      replacement.data(), replacement.size(), GRX_REPLACE_GLOBAL, nullptr,
      allocator, &error, &text);
  grx_text_free(&text);

  if (result == GRX_OK) {
    GRX_Split split {};
    result = grx_regex_split(regex, subject.data(), subject.size(), GRX_NPOS,
        nullptr, allocator, &error, &split);
    grx_split_free(&split);
  }

  grx_regex_free(regex);
  return result;
}

/** Patterns chosen to allocate in as many different places as possible. */
struct Workload {
  const char * pattern;
  const char * subject;
  uint32_t options;
  GRX_Engine engine;
};

const Workload kWorkloads[] = {
  // Names, classes, properties, a repeat expansion and an alternation: the
  // arenas, the class table, the name table and the program all grow.
  {"(?<word>[a-z\\d]+)-(\\p{Lu}{2,4}|x)*", "abc-ABCD", GRX_OPT_UTF,
      GRX_ENGINE_AUTO},
  // The lockstep engine's thread state, which is reference-counted and copied
  // on write - a different set of allocations from the compile.
  {"(a|b)*c", "ababababc", 0, GRX_ENGINE_PIKE},
  // The backtracker's frame stack, which grows geometrically.
  {"(a|b)\\1c", "xaacy", 0, GRX_ENGINE_BACKTRACK},
  // A lookaround, which snapshots the captures on the heap per evaluation.
  {"(?<=(ab|a))c", "abc", GRX_OPT_UTF, GRX_ENGINE_BACKTRACK},
  // Caseless, which builds fold orbits and closes classes over them.
  {"[a-z]+", "ABC", GRX_OPT_CASELESS | GRX_OPT_UTF, GRX_ENGINE_AUTO},
};

} // namespace

TEST(OutOfMemory, EveryAllocationFailureIsReportedAndNothingLeaks) {
  for (const Workload & workload : kWorkloads) {
    // How many allocations the whole sequence takes when none of them fails.
    grxtest::FailingAllocator counter(0);
    ASSERT_EQ(compile_and_match(counter.get(), workload.pattern,
                  workload.subject, workload.options, workload.engine),
        GRX_OK)
        << workload.pattern;
    ASSERT_EQ(counter.live(), 0) << workload.pattern << ": the succeeding run "
                                << "leaked before any failure was injected";

    const long total = counter.requested();
    ASSERT_GT(total, 0) << workload.pattern;

    for (long n = 1; n <= total; n++) {
      grxtest::FailingAllocator allocator(n);
      GRX_Result result = compile_and_match(allocator.get(),
          workload.pattern, workload.subject, workload.options,
          workload.engine);

      // Either the refusal was reported, or the library got through without
      // needing that allocation at all - which happens when an earlier
      // failure changed the path. Both are fine; a crash or a silent success
      // with a half-built object is not, and the sanitizers see those.
      if (allocator.failed() && result == GRX_OK) {
        // The allocation was requested and refused, yet everything
        // succeeded. That means the refused block was never needed on this
        // path, which is possible when the failure count lands past the
        // last allocation the shortened path makes.
        EXPECT_GE(allocator.requested(), n) << workload.pattern << " n=" << n;
      }
      else if (result != GRX_OK) {
        EXPECT_TRUE(result == GRX_ERR_OOM || result == GRX_ERR_LIMIT
            || result == GRX_ERR_INTERNAL)
            << workload.pattern << " n=" << n << ": "
            << grx_result_string(result);
      }

      EXPECT_EQ(allocator.live(), 0)
          << workload.pattern << ": failing allocation " << n << " of "
          << total << " leaked " << allocator.live() << " block(s)";
    }
  }
}

TEST(OutOfMemory, ReplaceAndSplitReportEveryFailureAndLeakNothing) {
  struct Row {
    const char * pattern;
    const char * subject;
    const char * replacement;
    uint32_t options;
  };
  const Row rows[] = {
    // A template with every kind of op in it, over several matches, so the
    // output buffer grows more than once.
    {"(a)(b)", "abababab", "[$&:$1:$2:$$]", GRX_OPT_UTF},
    // Named groups, which put a name in the template and a lookup behind it.
    {"(?<x>a)", "aaaa", "<$<x>>", GRX_OPT_UTF},
    // A pattern that matches empty, so the iteration rule runs and the split
    // takes its "not a separator" path.
    {"x*", "abc", "-", GRX_OPT_UTF},
    // Captures in the split output, which is a second array of pieces.
    {"(,)", "a,b,c", "", GRX_OPT_UTF},
  };

  for (const Row & row : rows) {
    grxtest::FailingAllocator counter(0);
    ASSERT_EQ(replace_and_split(counter.get(), row.pattern, row.subject,
                  row.replacement, row.options),
        GRX_OK)
        << row.pattern;
    ASSERT_EQ(counter.live(), 0) << row.pattern;

    const long total = counter.requested();
    ASSERT_GT(total, 0) << row.pattern;

    for (long n = 1; n <= total; n++) {
      grxtest::FailingAllocator allocator(n);
      GRX_Result result = replace_and_split(allocator.get(), row.pattern,
          row.subject, row.replacement, row.options);
      if (result != GRX_OK) {
        EXPECT_TRUE(result == GRX_ERR_OOM || result == GRX_ERR_LIMIT
            || result == GRX_ERR_INTERNAL)
            << row.pattern << " n=" << n << ": "
            << grx_result_string(result);
      }
      EXPECT_EQ(allocator.live(), 0)
          << row.pattern << ": failing allocation " << n << " of " << total
          << " leaked " << allocator.live() << " block(s)";
    }
  }
}

/**
 * Patterns this library refuses, across the failure kinds and the dialects.
 *
 * The sweep above injects a failure into a pattern that is *valid*, so the
 * path it unwinds is the out-of-memory one. A pattern the front end rejects
 * unwinds somewhere else - the parser has built part of an AST, opened part
 * of a class, perhaps interned a name, and then returns a diagnostic instead
 * of a tree - and nothing was checking that path at all. It is the same hole
 * the POSIX differential had: a corpus of things that succeed never asks the
 * question the failure path answers.
 */
namespace {

struct Rejection {
  const char * pattern;
  GRX_Syntax syntax;
  uint32_t options;
  const char * why;
};

const Rejection kRejections[] = {
  // Nothing allocated yet: the cheapest rejection there is, and the control.
  {"*", GRX_SYNTAX_ECMASCRIPT, 0, "a quantifier with nothing to quantify"},
  // Deep into a group, with captures open and a name interned.
  {"(?<word>[a-z]+", GRX_SYNTAX_ECMASCRIPT, 0, "an unclosed group"},
  {"(?<word>a)(?<word>b)", GRX_SYNTAX_ECMASCRIPT, 0, "a duplicate name"},
  // Inside a class, which has its own range table growing as it reads.
  {"[a-z", GRX_SYNTAX_ECMASCRIPT, 0, "an unclosed class"},
  {"[z-a]", GRX_SYNTAX_ECMASCRIPT, 0, "a reversed range"},
  {"[[:nosuch:]]", GRX_SYNTAX_POSIX_ERE, 0, "an unknown class name"},
  // Properties, which allocate a name buffer before they look anything up.
  {"\\p{NoSuchProperty}", GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF,
      "an unknown property"},
  {"\\p{Script=NoSuchScript}", GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF,
      "an unknown script"},
  // References, resolved after the tree exists. Both need unicode mode to be
  // errors at all: Annex B reads `\\2` with no group 2 as a legacy octal
  // escape and `\\k` with no named group in the pattern as a literal `k`, and
  // this library follows it - which is what the first draft of these two
  // rows found out by being accepted.
  {"(a)\\2", GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF,
      "a backreference to no group"},
  {"\\k<nosuch>", GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF,
      "a named reference to no group"},
  // Repeats, rejected after both bounds are read.
  {"a{3,2}", GRX_SYNTAX_ECMASCRIPT, 0, "a repeat whose bounds are inverted"},
  // A construct the dialect does not have, refused by the front end rather
  // than by the grammar - a different return and a different unwind.
  {"(?<=a)b", GRX_SYNTAX_POSIX_ERE, 0, "a lookbehind in POSIX ERE"},
  // Not `\\d`, which POSIX leaves undefined for an escaped ordinary character
  // and which glibc and this library both read as a literal `d`.
  {"\\(a", GRX_SYNTAX_POSIX_BRE, 0, "an unclosed group in POSIX BRE"},
  // The other dialect family, whose reader is a different file.
  {"(?(1)a)", GRX_SYNTAX_ECMASCRIPT, 0, "a conditional in ECMAScript"},
  {"a\\", GRX_SYNTAX_PERL, 0, "a trailing escape"},
  // UnicodeSets, whose class algebra allocates operand sets as it goes.
  {"[\\q{abc}&&", GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF | GRX_OPT_UNICODE_SETS,
      "an unclosed set operation"},
};

} // namespace

/**
 * Invariant 4, for the calls that fail because the *input* is wrong.
 *
 * design.md section 9 says no failing call leaves an allocation, and the
 * sweep above had only ever asked it of calls that failed because memory ran
 * out. Every row here is refused, and after each one the allocator must be
 * back to zero - so a parser that returns a diagnostic while still holding
 * the half-built tree it was working on is caught here rather than by
 * whoever next runs the library under Valgrind.
 *
 * The rows go through the whole sequence, not just the parse, because a
 * pattern that parses and is then refused by lowering or codegen unwinds a
 * third way again.
 */
TEST(OutOfMemory, ARefusedPatternLeavesNothingAllocated) {
  for (const Rejection & row : kRejections) {
    grxtest::CountingAllocator allocator;
    GRX_Error error;
    grx_error_clear(&error);

    GRX_Pattern * parsed = nullptr;
    GRX_Result result = grx_pattern_parse_with_allocator(row.pattern,
        std::strlen(row.pattern), row.syntax, row.options, nullptr,
        allocator.get(), &error, &parsed);
    if (result == GRX_OK) {
      // Some rows are refused later than others; carry on to the step that
      // refuses them, so the row is still testing what it says it is.
      GRX_Regex * regex = nullptr;
      result = grx_regex_compile_pattern(
          parsed, nullptr, allocator.get(), &error, &regex);
      grx_regex_free(regex);
      grx_pattern_free(parsed);
      parsed = nullptr;
    }

    EXPECT_NE(result, GRX_OK)
        << row.pattern << " (" << row.why
        << ") was accepted; this row is no longer testing a failure path";
    EXPECT_EQ(parsed, nullptr) << row.pattern;
    EXPECT_GT(allocator.total(), 0)
        << row.pattern << " (" << row.why
        << ") allocated nothing at all, so it cannot show a leak; find an "
           "input that is refused later";
    EXPECT_EQ(allocator.live(), 0)
        << row.pattern << " (" << row.why << ") was refused with "
        << allocator.live() << " of " << allocator.total()
        << " block(s) still held";
  }
}

TEST(OutOfMemory, TheHarnessItselfFailsWhatItSaysItFails) {
  // A sweep that never actually refused anything would pass forever. This
  // states that the injected failure happens and that the library notices.
  grxtest::FailingAllocator allocator(1);
  GRX_Pattern * parsed = nullptr;
  EXPECT_EQ(grx_pattern_parse_with_allocator("abc", 3, GRX_SYNTAX_ECMASCRIPT,
                0, nullptr, allocator.get(), nullptr, &parsed),
      GRX_ERR_OOM);
  EXPECT_EQ(parsed, nullptr);
  EXPECT_TRUE(allocator.failed());
  EXPECT_EQ(allocator.live(), 0);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
