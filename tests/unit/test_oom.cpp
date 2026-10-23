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

GRX_Result replace_java(const GRX_Allocator * allocator) {
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  grx_error_clear(&error);
  GRX_Result result = grx_regex_compile_with_allocator("(?<x>a)", 7,
      GRX_SYNTAX_JAVA, 0, nullptr, allocator, &error, &regex);
  if (result != GRX_OK) {
    return result;
  }
  GRX_Text out {};
  result = grx_regex_replace(regex, "aaa", 3, "${x}-$0", 7,
      GRX_REPLACE_GLOBAL, nullptr, allocator, &error, &out);
  grx_text_free(&out);
  grx_regex_free(regex);
  return result;
}

GRX_Result split_java(const GRX_Allocator * allocator) {
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  grx_error_clear(&error);
  GRX_Result result = grx_regex_compile_with_allocator(",", 1,
      GRX_SYNTAX_JAVA, 0, nullptr, allocator, &error, &regex);
  if (result != GRX_OK) return result;
  GRX_Split split {};
  result = grx_regex_split_java(regex, "a,b,", 4, -1, nullptr, allocator,
      &error, &split);
  grx_split_free(&split);
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

TEST(OutOfMemory, JavaReplacementReportsEveryFailureAndLeaksNothing) {
  grxtest::FailingAllocator counter(0);
  ASSERT_EQ(replace_java(counter.get()), GRX_OK);
  ASSERT_EQ(counter.live(), 0);
  for (long n = 1; n <= counter.requested(); n++) {
    grxtest::FailingAllocator allocator(n);
    GRX_Result result = replace_java(allocator.get());
    if (result != GRX_OK) {
      EXPECT_TRUE(result == GRX_ERR_OOM || result == GRX_ERR_LIMIT
          || result == GRX_ERR_INTERNAL) << "n=" << n;
    }
    EXPECT_EQ(allocator.live(), 0) << "n=" << n;
  }
}

TEST(OutOfMemory, JavaSplitReportsEveryFailureAndLeaksNothing) {
  grxtest::FailingAllocator counter(0);
  ASSERT_EQ(split_java(counter.get()), GRX_OK);
  ASSERT_EQ(counter.live(), 0);
  for (long n = 1; n <= counter.requested(); n++) {
    grxtest::FailingAllocator allocator(n);
    GRX_Result result = split_java(allocator.get());
    if (result != GRX_OK) {
      EXPECT_TRUE(result == GRX_ERR_OOM || result == GRX_ERR_LIMIT
          || result == GRX_ERR_INTERNAL) << "n=" << n;
    }
    EXPECT_EQ(allocator.live(), 0) << "n=" << n;
  }
}

TEST(OutOfMemory, JavaNoMatchCopyReportsEveryFailureAndLeaksNothing) {
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile("a", GRX_SYNTAX_JAVA, 0, &regex), GRX_OK);
  auto run = [regex](const GRX_Allocator * allocator, GRX_Error * error) {
    GRX_Text out {};
    GRX_Result result = grx_regex_replace(regex, "bbbb", 4, "$", 1,
        GRX_REPLACE_GLOBAL, nullptr, allocator, error, &out);
    if (result == GRX_OK) {
      EXPECT_EQ(std::string(out.data, out.length), "bbbb");
    }
    else {
      EXPECT_EQ(out.data, nullptr);
    }
    grx_text_free(&out);
    return result;
  };
  grxtest::FailingAllocator counter(0);
  GRX_Error error;
  grx_error_clear(&error);
  ASSERT_EQ(run(counter.get(), &error), GRX_OK);
  EXPECT_EQ(error.diag, GRX_DIAG_NONE);
  ASSERT_EQ(counter.live(), 0);
  const long total = counter.requested();
  ASSERT_GT(total, 0);
  for (long n = 1; n <= total; n++) {
    grxtest::FailingAllocator allocator(n);
    grx_error_clear(&error);
    GRX_Result result = run(allocator.get(), &error);
    if (result != GRX_OK) {
      EXPECT_EQ(result, GRX_ERR_OOM) << "n=" << n;
      EXPECT_EQ(error.diag, GRX_DIAG_OUT_OF_MEMORY) << "n=" << n;
    }
    EXPECT_EQ(allocator.live(), 0) << "n=" << n;
  }
  grx_regex_free(regex);
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

TEST(OutOfMemory, DeferredVimAssertionSnapshotsUnwindEveryAllocationFailure) {
  struct Case {
    GRX_Syntax syntax;
    const char * pattern;
    const char * subject;
    size_t pressure = 0;
    int expected_match = 1;
  };
  const Case cases[] = {
      {GRX_SYNTAX_VIM_BACKTRACK, "[a-ź]", "b́"},
      {GRX_SYNTAX_VIM_NFA, "[a-ź]", "b́"},
      {GRX_SYNTAX_VIM_BACKTRACK, "[[:alpha:]́]", "á"},
      {GRX_SYNTAX_VIM_NFA, "[[:alpha:]́]", "á"},
      {GRX_SYNTAX_VIM_BACKTRACK, "\\([a-ź]\\)", "b́"},
      {GRX_SYNTAX_VIM_NFA, "\\([[:alpha:]́]\\)", "á"},
      {GRX_SYNTAX_VIM_BACKTRACK, "\\c\u0301\\?b", "a\u0301B"},
      {GRX_SYNTAX_VIM_BACKTRACK, "\\Z\u0301\\?a\u0302b", "a\u0301a\u0301b"},
      {GRX_SYNTAX_VIM_BACKTRACK, "\\c\\Za\u0302b", "a\u0301B", 0, 0},
      {GRX_SYNTAX_VIM_NFA, "\\c\\Za\u0302b", "A\u0301b", 0, 0},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\)\@>.\1)", "aaa\u0301", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\)\@=a\(\1\)\@=.)", "a\u0301a\u0301", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\)\@=a\(\1\)\@>x)", "aa\u0301x", 1024, 0},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\)\(\1\)\@>x)", "aa\u0301x", 1024, 0},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\)\@=\(aX\|\1\)\@=.)", "a\u0301", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\)\@=\(\1\|aX\)\@>x)", "a\u0301x", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\+\)\@=\(a\)\@=a)", "a\u0301aaa", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\)\@=a\1)", "aab", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\?\)\@=\1.)", "a\u0301", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\(\(a\zsb\)\@>\)\@>)", "ab", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\(x\)\%(\(\(ab\)\@>\)\@>\)\@=.)", "xab", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(^\(a\)\?a\?b\(\%(\1aX\|[ab].x\)\)\@>)",
          "abaa\u0301x", 1024, 0},
      {GRX_SYNTAX_VIM_NFA, R"(\(\([ab].\|abX\)\@=.\)\@=.)", "a\u0301", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\?.\)\@<=x)", "aa\u0301x", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\%(a\|[ab]\))", "a\u0301b", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(^\%([ab]..\|a\nX\))", "a\n\u0301b", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\(\_[^x]\).)", "\n\u0301b", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\([ab].\|abX\)\@=.)", "a\u0301", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(^\([ab]\)\(\%(\1aX\|[ab].x\)\)\@>)", "baa\u0301x",
          1024},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\|.\)\@<=x)", "a\u0301x", 1024},
      {GRX_SYNTAX_VIM_BACKTRACK, "\u0301\\?b", "a\u0301xb"},
      {GRX_SYNTAX_VIM_BACKTRACK,
          R"(\Za)"
          "\u0301"
          "\u0302b",
          "a\u0302\u0301b"},
      {GRX_SYNTAX_VIM_BACKTRACK, R"(\%(aa\|a\)\{2}aab)", "aaab"},
      {GRX_SYNTAX_VIM_BACKTRACK, R"(\%(\%(a\)\{1,2}\)\{2}b)", "aab", 0, 0},
      {GRX_SYNTAX_VIM_BACKTRACK, R"(\%(\%(a\)\+\)\{2}b)", "aab"},
      {GRX_SYNTAX_VIM_BACKTRACK, R"(a\%(a\)\@<=\%(a\|aa\)b)", "aaab"},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\)\@=\(\1b\)\@=..)", "ab"},
      {GRX_SYNTAX_VIM_NFA, R"(\(\(a\ze\)\@=a\)\@=\(a\)\@=ab)", "ab"},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\)\@<=\(a\)\1\l)", "aaab"},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\)\@>a\(a\1\)\@<=.)", "aab", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\(\(a\)\@=a\)\@<=\(a\)\@<=\%(a\)\@>)", "aab",
          1024},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\)\%(z\)\@>x\|\%(ab\)\@>)", "aab", 512},
      {GRX_SYNTAX_VIM_NFA, R"(\(\(a\)\%(z\)\@>x\)\|\%(ab\)\@>)", "aab", 640},
      {GRX_SYNTAX_VIM_NFA, R"(\%(\(a\)\@=a\)*ab)", "aaab", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(a\+\(a\)\@>\(a\)\@<=)", "aaab", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(a\+\(a*\)\@>\(a\(b\)\@=\)\@<=)", "aaab", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\%(aa\|a\)\(a\)\@>b)", "aaaab", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\)\?\%(\1a\)\@>b)", "aaab", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\)\@=\%(\(a\)\@=a\)\@>b)", "ab", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\%(a\@<=a\)\@>b)", "aab", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\%(a\|aa\)\%(a\ze\)\@>b)", "aaaaaab", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\)\?\%(a\ze\)\@>\%(a\)\@>b)", "aaaab", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\|b\)\%(a\1\)\@>\(ab\)\@<=X)", "babX", 1024},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\|b\)\%(a\1\)\@>\%(a\1\)\@<!X)", "babX", 1024},
      {GRX_SYNTAX_VIM_BACKTRACK, R"(a\%(a\)\@<=\%(a\|aa\)x)", "aaab", 0, 0},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\)\@=\(\1b\)\@=ab)", "ab", 0, 0},
      {GRX_SYNTAX_VIM_NFA, R"(a\+\(a\)\@>\(a\)\@<=x)", "aaab", 0, 0},
      {GRX_SYNTAX_VIM_NFA, R"(\%(\%(a\(a\ze\)\@<=\)\{0,2}\)\{0,3}b)", "aaab",
          1024},
      {GRX_SYNTAX_VIM_NFA, R"(\(a*\)\zs\(a\)\%(\1a\)\@>\%(a\)\@<=b)", "aaaab",
          1024},
  };
  size_t fault_runs = 0;
  size_t successful_fallbacks = 0;
  size_t recovered_searches = 0;
  size_t compared_spans = 0;
  for (const Case & row : cases) {
    SCOPED_TRACE(row.pattern);
    std::vector<GRX_Capture> baseline;
    auto run = [&](const GRX_Allocator * allocator, size_t budget = 0,
                   bool establish_baseline = false) {
      GRX_Regex * regex = nullptr;
      GRX_Result result = grx_regex_compile_with_allocator(row.pattern,
          std::strlen(row.pattern), row.syntax, 0, nullptr, allocator, nullptr,
          &regex);
      if (result != GRX_OK) {
        return result;
      }
      GRX_Match * match = nullptr;
      result = grx_match_create(regex, allocator, &match);
      if (result == GRX_OK) {
        int matched = 0;
        GRX_Limits limits;
        grx_limits_default(&limits);
        if (budget) {
          limits.max_match_memory = budget;
        }
        result = grx_regex_search(regex, row.subject, std::strlen(row.subject),
            0, GRX_ENGINE_BACKTRACK, &limits, match, &matched);
        if (result == GRX_ERR_LIMIT && budget) {
          EXPECT_EQ(grx_match_error(match)->diag, GRX_DIAG_LIMIT_MATCH_MEMORY);
        }
        const GRX_Result first_result = result;
        if (result == GRX_ERR_OOM) {
          // The one-shot refusal has been consumed. Recovery must reuse the
          // exact regex and Match whose search just failed.
          EXPECT_EQ(matched, 0);
          EXPECT_EQ(grx_match_error(match)->code, GRX_ERR_OOM);
          result = grx_regex_search(regex, row.subject, std::strlen(row.subject),
              0, GRX_ENGINE_BACKTRACK, &limits, match, &matched);
          EXPECT_EQ(result, GRX_OK);
          recovered_searches++;
          EXPECT_EQ(grx_match_error(match)->code, GRX_OK);
          EXPECT_EQ(grx_match_error(match)->diag, GRX_DIAG_NONE);
        }
        if (result == GRX_OK) {
          EXPECT_EQ(matched, row.expected_match) << row.pattern;
          size_t count = matched ? grx_regex_capture_count(regex) + 1 : 0;
          if (!establish_baseline) {
            EXPECT_EQ(count, baseline.size());
          }
          for (size_t i = 0; i < count; i++) {
            GRX_Capture group;
            EXPECT_EQ(grx_match_group(match, i, &group), GRX_OK);
            if (establish_baseline) {
              baseline.push_back(group);
            }
            else if (i < baseline.size()) {
              EXPECT_EQ(group.start, baseline[i].start) << "group " << i;
              EXPECT_EQ(group.end, baseline[i].end) << "group " << i;
              compared_spans++;
            }
            if (group.start != GRX_NPOS) {
              EXPECT_LE(group.start, group.end);
              EXPECT_LE(group.end, std::strlen(row.subject));
            }
          }
        }
        grx_match_destroy(match);
        if (first_result == GRX_ERR_OOM) {
          result = first_result;
        }
      }
      grx_regex_free(regex);
      return result;
    };
    grxtest::FailingAllocator counter(0);
    ASSERT_EQ(run(counter.get(), 0, true), GRX_OK) << row.pattern;
    ASSERT_EQ(counter.live(), 0);
    if (row.pressure) {
      grxtest::FailingAllocator limited(0);
      EXPECT_EQ(run(limited.get(), row.pressure), GRX_ERR_LIMIT);
      EXPECT_EQ(limited.live(), 0);
    }
    for (long n = 1; n <= counter.requested(); n++) {
      SCOPED_TRACE("allocation " + std::to_string(n));
      grxtest::FailingAllocator allocator(n);
      fault_runs++;
      GRX_Result result = run(allocator.get());
      if (result == GRX_OK && allocator.failed()) {
        successful_fallbacks++;
      }
      EXPECT_TRUE(result == GRX_OK || result == GRX_ERR_OOM)
          << row.pattern << " allocation " << n << ": "
          << grx_result_string(result);
      EXPECT_EQ(allocator.live(), 0) << row.pattern << " allocation " << n;
    }
  }
  std::fprintf(stderr,
      "Vim assertion OOM invariance: %zu fault runs, "
      "%zu successful fallbacks, %zu recovered searches, %zu capture spans "
      "compared\n",
      fault_runs, successful_fallbacks, recovered_searches, compared_spans);
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
