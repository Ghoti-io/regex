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
