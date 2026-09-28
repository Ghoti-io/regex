/**
 * @file
 *
 * The compile entry points.
 *
 * The argument contract, which is the part that does not change as the
 * pipeline behind it fills in. What a compile *produces* is
 * tests/unit/test_lower.cpp; what it matches is checked against Node by
 * tools/oracle/match_diff.py.
 *
 * The dialect used here is deliberately one that is not built. Every call
 * below is about arguments rather than about patterns, and using a dialect
 * with a front end would make the tests depend on that front end's rules.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <chrono>
#include <cstdio>
#include <string>

#include "test_helpers.h"

TEST(Compile, NullArgumentsAreInvalid) {
  GRX_Regex * regex = nullptr;
  EXPECT_EQ(
      grx_regex_compile(nullptr, GRX_SYNTAX_PCRE, GRX_OPT_NONE, &regex),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_regex_compile("a", GRX_SYNTAX_PCRE, GRX_OPT_NONE, nullptr),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_regex_compile_with_allocator(nullptr, 4, GRX_SYNTAX_PCRE,
                GRX_OPT_NONE, nullptr, nullptr, nullptr, &regex),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_regex_compile_pattern(nullptr, nullptr, nullptr, nullptr,
                &regex),
      GRX_ERR_INVALID);
  EXPECT_EQ(regex, nullptr);
}

// A dialect this library names and has not built. Named once so that
// building the next one changes this line rather than four call sites: these
// said POSIX until WP-23 built it and Python until WP-30 did.
static const GRX_Syntax kUnbuiltDialect = GRX_SYNTAX_JAVA;

TEST(Compile, OutputIsNullOnFailure) {
  GRX_Regex * regex = (GRX_Regex *)0x1;
  EXPECT_NE(grx_regex_compile("a", kUnbuiltDialect, GRX_OPT_NONE, &regex),
      GRX_OK);
  EXPECT_EQ(regex, nullptr);
}

TEST(Compile, ReportsThePositionOfTheFailure) {
  // Every failing compile fills in the error structure when one is supplied.
  // A caller showing a user where their pattern went wrong needs that to be
  // unconditional, not a property of some error paths.
  GRX_Error error;
  GRX_Regex * regex = nullptr;
  GRX_Result result = grx_regex_compile_with_allocator("a", 1,
      kUnbuiltDialect, GRX_OPT_NONE, nullptr, nullptr, &error, &regex);

  ASSERT_NE(result, GRX_OK);
  EXPECT_EQ(error.code, result);
  EXPECT_NE(error.message[0], '\0');
}

TEST(Compile, ADialectThatIsNotBuiltSaysSo) {
  // design.md section 4: a dialect that accepts everything is a bug. A
  // pattern of an unbuilt dialect is refused rather than read with somebody
  // else's rules and pronounced valid. `a` is a valid pattern in every
  // dialect here, so the only thing that can refuse it is the absence of a
  // front end. This named POSIX until WP-23 built it and Python until WP-30
  // did, which is the shape of staleness the test below guards.
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  EXPECT_EQ(grx_regex_compile_with_allocator("a", 1, kUnbuiltDialect,
                GRX_OPT_NONE, nullptr, nullptr, &error, &regex),
      GRX_ERR_UNSUPPORTED);
  EXPECT_EQ(error.diag, GRX_DIAG_DIALECT_NOT_IMPLEMENTED);
  EXPECT_EQ(regex, nullptr);
}

TEST(Compile, ThePerlFamilyIsBuilt) {
  // The other half of the rule above, and the one that goes stale silently:
  // a dialect this library *does* read has to be reachable through the same
  // call, or "not implemented" is being reported for a front end that exists.
  for (GRX_Syntax syntax : {GRX_SYNTAX_PCRE, GRX_SYNTAX_PERL}) {
    GRX_Regex * regex = nullptr;
    GRX_Error error;
    EXPECT_EQ(grx_regex_compile_with_allocator("a(?i:b)c", 8, syntax,
                  GRX_OPT_NONE, nullptr, nullptr, &error, &regex),
        GRX_OK)
        << grx_syntax_name(syntax) << ": " << error.message;
    grx_regex_free(regex);
  }
}

TEST(Compile, TheCommonPathProducesARunnableProgram) {
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile("a(b|c)*d", GRX_SYNTAX_ECMASCRIPT, GRX_OPT_NONE,
                &regex),
      GRX_OK);
  ASSERT_NE(regex, nullptr);
  EXPECT_EQ(grx_regex_syntax(regex), GRX_SYNTAX_ECMASCRIPT);
  EXPECT_EQ(grx_regex_capture_count(regex), 1u);
  EXPECT_GT(grx_regex_program_size(regex), 0u);
  grx_regex_free(regex);
}

TEST(Compile, AnAlreadyParsedPatternCanBeCompiledMoreThanOnce) {
  // The pattern is read, not consumed: a caller linting a pattern and then
  // compiling it should not have to parse it twice.
  GRX_Pattern * parsed = nullptr;
  ASSERT_EQ(grx_pattern_parse("[a-z]+", GRX_SYNTAX_ECMASCRIPT, GRX_OPT_NONE,
                &parsed),
      GRX_OK);

  GRX_Regex * first = nullptr;
  GRX_Regex * second = nullptr;
  ASSERT_EQ(
      grx_regex_compile_pattern(parsed, nullptr, nullptr, nullptr, &first),
      GRX_OK);
  ASSERT_EQ(
      grx_regex_compile_pattern(parsed, nullptr, nullptr, nullptr, &second),
      GRX_OK);
  EXPECT_EQ(grx_regex_program_size(first), grx_regex_program_size(second));

  grx_regex_free(first);
  grx_regex_free(second);
  grx_pattern_free(parsed);
}

TEST(Compile, AGroupsLengthIsMeasuredOncePerGroupNotOncePerReference) {
  // `group_span()` in src/ir/analyze.c answers "how long can what this group
  // captured be", and `walk()` asks it for every reference. It had a cycle
  // guard and no memo, so a group referenced twice was measured twice - and
  // when the body doing the referencing was itself a group being measured, the
  // work doubled a level.
  //
  // The shape below is the cheapest witness: group 1 is a literal, and every
  // group after it is two references to the one before. Measured on the build
  // that had no memo, 24 groups took 6.68 seconds and 22 took 2.43 - a factor
  // of 2.1 a level - so the 40 groups here would have taken about nine hours.
  // A 1,514-byte pattern from the fuzzer took 354 seconds and was then
  // rejected as malformed, which is notes section 14i.
  //
  // A wall clock in a test is usually a bad idea and here it is the only
  // honest instrument: the defect is time, not allocation or output, and the
  // two implementations are nine hours apart. Five seconds is therefore not a
  // tolerance to argue about - it is thirty times what a memoised compile of
  // this pattern needs on a slow machine and a millionth of what the
  // unmemoised one needs.
  std::string pattern = "(a)";
  for (int group = 1; group < 40; group++) {
    pattern += "(\\" + std::to_string(group) + "\\" + std::to_string(group) + ")";
  }

  GRX_Regex * regex = nullptr;
  auto start = std::chrono::steady_clock::now();
  GRX_Result result = grx_regex_compile(
      pattern.c_str(), GRX_SYNTAX_ECMASCRIPT, GRX_OPT_NONE, &regex);
  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(result, GRX_OK);
  EXPECT_NE(regex, nullptr);
  grx_regex_free(regex);

  auto seconds
      = std::chrono::duration_cast<std::chrono::duration<double>>(elapsed)
            .count();
  EXPECT_LT(seconds, 5.0) << pattern.size() << "-byte pattern with 40 groups "
                             "took " << seconds << " s to compile";
}

TEST(Compile, AGroupThatReferencesItselfIsMeasuredOnceNotOncePerReference) {
  // The memo above is guarded, and the guard was wide enough to switch it off.
  //
  // group_span() must not remember a span that came out "unknown" because of
  // the route to it: a reference to a group already being resolved says
  // nothing about that group. The old rule for that was a counter - if
  // anything underneath gave up, do not store - and it cannot tell two unlike
  // cases apart. A group whose own body references *itself* gives up on every
  // route, because group_span() pushes the group before walking its body, so
  // that answer is a property of the group and is the same wherever it is
  // asked from. A group that gives up because an enclosing resolution put some
  // other group on the stack is the case the guard is for.
  //
  // With a self-reference in the body the counter moves every time, so nothing
  // was ever stored and every reference re-walked the whole body. The fix
  // compares against the resolving-stack index the group was pushed at.
  //
  // Found by replaying the soak's slow units: `pattern-pcre/slow-unit-2fcdef60`
  // is 24,901 bytes, took 20,384 body walks and **zero** cache hits -
  // 265,488,928 walk steps and 7.89 s. It is 1.95 s now, and the two artifacts
  // either side of it in notes/regex/TODO.md section 14u went 6.03 s to 1.09 s
  // and 0.32 s to 0.006 s.
  //
  // The witness here is one group whose body is expensive to walk and contains
  // a reference to itself, plus many references to it from outside. Old cost
  // is references times body; new cost is body plus references.
  //
  // Half a second sits between the two populations in both builds: release
  // 0.011 s fixed against 1.14 s broken, and under AddressSanitizer - where
  // `make test` also runs this - 0.024 s against 4.90 s. The margin under the
  // bound is 21x, which is the side a loaded machine can move.
  std::string body;
  for (int i = 0; i < 4000; i++) {
    body += "(?:a|b)";
  }
  std::string pattern = "(" + body + "\\1)";
  for (int i = 0; i < 4000; i++) {
    pattern += "\\1";
  }

  GRX_Limits limits;
  grx_limits_default(&limits);
  // The only cap lifted, for the same reason as the two tests below: the
  // program this expands to is large and the program-size cap is not the
  // subject. Everything else is the default, and the 36 KB pattern is inside
  // max_pattern_length and max_nodes as they ship.
  limits.max_program_size = 0;

  GRX_Regex * regex = nullptr;
  auto start = std::chrono::steady_clock::now();
  GRX_Result result = grx_regex_compile_with_allocator(pattern.data(),
      pattern.size(), GRX_SYNTAX_PCRE, GRX_OPT_NONE, &limits, nullptr, nullptr,
      &regex);
  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(result, GRX_OK);
  EXPECT_NE(regex, nullptr);
  grx_regex_free(regex);

  auto seconds
      = std::chrono::duration_cast<std::chrono::duration<double>>(elapsed)
            .count();
  EXPECT_LT(seconds, 0.5)
      << pattern.size() << "-byte pattern, one self-referencing group and "
      << "4,000 references to it, took " << seconds
      << " s to compile. group_span() remembers a span whose give-ups were all "
         "at or above the group's own place on the resolving stack; a figure "
         "this large means it is refusing to remember them again";
}

TEST(Compile, ASubtreeIsAskedAboutTheEmptyStringOncePerNodeNotOncePerCopy) {
  // The other half of the memo above, and the half it did not fix.
  //
  // `grx_ir_can_match_empty()` is a pure function of a subtree - the call
  // builds a fresh analysis with an empty resolving set - and codegen asks it
  // of every repeat body, to decide whether the loop needs a progress guard. A
  // counted repeat is laid out by expansion, so an inner repeat's body is
  // walked again for every copy of the outer one, and the question was asked
  // again with it. The memo in group_span() cannot help: it lives on the
  // Analysis struct and the Analysis is built per call, so it made one analysis
  // cheap and left the *number* of analyses alone.
  //
  // Which is why the test above did not catch this. It compiles one pattern
  // once, so it measures a single analysis - the only case that was ever fixed.
  // This one needs many analyses of the same subtree, and nesting `{1,2}`
  // sixteen deep over a chain of backreferences is the cheapest way to get
  // 65,536 copies of one body out of a 381-byte pattern.
  //
  // A fuzz artifact found it as 15.9 seconds spent *refusing* a 2,465-byte
  // pattern - 2,036 IR nodes, 92 repeats, 125 backreferences - which is
  // notes/regex/TODO.md section 14s. Measured here: 1.37 s before the per-node
  // cache and 0.017 s after, a factor of eighty.
  //
  // Half a second sits between those two populations in both builds, which is
  // the only property a bound like this needs. Release: 0.017 s fixed against
  // 1.37 s broken. Under AddressSanitizer, where `make test` also runs it:
  // 0.141 s fixed, and the broken build would be some eleven. The tightest
  // margin is therefore the sanitizer's 3.5x, which is why the work here is
  // sixteen levels and not twenty.
  std::string pattern = "(a)";
  for (int group = 1; group <= 40; group++) {
    pattern += "(\\" + std::to_string(group) + "x)";
  }
  std::string inner = "\\41";
  for (int level = 0; level < 16; level++) {
    inner = "(?:" + inner + "){1,2}";
  }
  pattern += inner;

  GRX_Limits limits;
  grx_limits_default(&limits);
  // The expansion is 393,377 instructions, and the program-size cap is not
  // what this test is about - a refused compile does the same walking.
  limits.max_program_size = 0;

  GRX_Regex * regex = nullptr;
  auto start = std::chrono::steady_clock::now();
  GRX_Result result = grx_regex_compile_with_allocator(pattern.data(),
      pattern.size(), GRX_SYNTAX_PCRE, GRX_OPT_NONE, &limits, nullptr, nullptr,
      &regex);
  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(result, GRX_OK);
  EXPECT_NE(regex, nullptr);
  grx_regex_free(regex);

  auto seconds
      = std::chrono::duration_cast<std::chrono::duration<double>>(elapsed)
            .count();
  EXPECT_LT(seconds, 0.5)
      << pattern.size() << "-byte pattern, 16 levels of {1,2} over 40 "
      << "backreferences, took " << seconds
      << " s to compile. codegen caches grx_ir_can_match_empty() per IR node; "
         "a figure this large means it is asking once per expanded copy again";
}

TEST(Compile, AnAlternationBranchIsMeasuredOncePerNodeNotOncePerCopy) {
  // The third of the same family, and the one that was recorded rather than
  // fixed when the second went in.
  //
  // `grx_ir_span()` is the other pure function codegen calls from inside the
  // expansion: gen_length_guard() asks how long each alternation branch is, to
  // emit the assertion that prunes a variable lookbehind. That call site sits
  // inside the same nested `{1,2}` repeats, so a branch was being measured once
  // per expanded copy of the body around it.
  //
  // It appeared in none of the stack samples that found the empty-string memo,
  // so it was left in notes/regex/TODO.md section 14s as "the same kind of pure
  // function inside the same loops" rather than fixed with it. The shape was
  // the whole argument and the measurement bore it out: 1.99 s asking per copy
  // against 0.019 s asking per node, a factor of a hundred.
  //
  // A forward lookbehind is what reaches the guard at all - `forward_tail` is
  // set entering one and cleared by any repeat that can iterate - so the
  // lookbehind goes *inside* the nesting, and the alternation inside that.
  //
  // The backreference chain is 120 long rather than the 40 above, because it is
  // the one axis that separates the two builds without costing the fixed one:
  // it sits outside the nesting, so it makes each measurement expensive and
  // adds nothing to the expansion. Lengthening it and dropping a level of
  // nesting buys the same 2 s on the broken build for half the time on the
  // fixed one.
  //
  // Measured against the half-second bound, which sits between the two
  // populations in both builds: release 0.017 s fixed against 2.03 s broken,
  // and under AddressSanitizer - where `make test` also runs this - 0.107 s
  // fixed against 8.66 s broken. The tightest margin is therefore 4.7x, on the
  // side that matters, which is a false failure on a loaded machine.
  std::string pattern = "(a)";
  for (int group = 1; group <= 120; group++) {
    pattern += "(\\" + std::to_string(group) + "x)";
  }
  std::string alternation;
  for (int branch = 0; branch < 6; branch++) {
    if (branch) {
      alternation += "|";
    }
    alternation += "\\121" + std::string((size_t)branch, 'y');
  }
  std::string inner = "(?<=" + alternation + ")";
  for (int level = 0; level < 14; level++) {
    inner = "(?:" + inner + "){1,2}";
  }
  pattern += inner;

  GRX_Limits limits;
  grx_limits_default(&limits);
  // Same as above: a refused compile walks exactly as much, and the cap is not
  // what is being measured.
  limits.max_program_size = 0;

  GRX_Regex * regex = nullptr;
  auto start = std::chrono::steady_clock::now();
  GRX_Result result = grx_regex_compile_with_allocator(pattern.data(),
      pattern.size(), GRX_SYNTAX_PCRE, GRX_OPT_NONE, &limits, nullptr, nullptr,
      &regex);
  auto elapsed = std::chrono::steady_clock::now() - start;
  EXPECT_EQ(result, GRX_OK);
  EXPECT_NE(regex, nullptr);
  grx_regex_free(regex);

  auto seconds
      = std::chrono::duration_cast<std::chrono::duration<double>>(elapsed)
            .count();
  EXPECT_LT(seconds, 0.5)
      << pattern.size() << "-byte pattern, 14 levels of {1,2} over a "
      << "six-branch lookbehind, took " << seconds
      << " s to compile. codegen caches grx_ir_span() per IR node; a figure "
         "this large means gen_length_guard() is measuring once per expanded "
         "copy again";
}

TEST(Compile, AllocatesNothingOnAFailedCompile) {
  grxtest::CountingAllocator allocator;

  GRX_Regex * regex = nullptr;
  (void)grx_regex_compile_with_allocator("a", 1, kUnbuiltDialect,
      GRX_OPT_NONE, nullptr, allocator.get(), nullptr, &regex);

  EXPECT_EQ(allocator.live(), 0);
}

TEST(Regex, AccessorsTolerateNull) {
  EXPECT_EQ(grx_regex_capture_count(nullptr), 0u);
  EXPECT_EQ(grx_regex_capture_name(nullptr, 1), nullptr);
  EXPECT_EQ(grx_regex_syntax(nullptr), GRX_SYNTAX_COUNT);
  EXPECT_EQ(grx_regex_program_size(nullptr), 0u);
  EXPECT_EQ(grx_regex_dump(nullptr, stderr), GRX_ERR_INVALID);
  grx_regex_free(nullptr);

  size_t index = 0;
  EXPECT_EQ(grx_regex_capture_index(nullptr, "name", &index), GRX_ERR_INVALID);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
