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
#include <utility>
#include <vector>

#include "test_helpers.h"

#include "../../src/compile/compile_internal.h"

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

TEST(Compile, OneAnalysisMemoServesTheWholeCompileNotOneQuestion) {
  // The fourth of this family and the last of it, and the one that says what
  // the other three were about.
  //
  // `grx_ir_can_match_empty()` and `grx_ir_span()` each build a fresh analysis,
  // and the analysis is where the group-span memo and the capture index live.
  // So the memos on the Codegen cut the *number* of questions to once per IR
  // node - 14,300 nodes to 338 questions on the artifact below - and every one
  // of those 338 built a cache, filled it, and threw it away. Codegen holds one
  // analysis memo for the compile now.
  //
  // pattern-pcre/slow-unit-2fcdef60, 24,901 bytes: 4,988 body walks and
  // 69,465,944 walk steps across those 338 questions, 1.84 s. It is three body
  // walks and 0.111 s now - 321 of its 338 questions are answered from the
  // cache, 61 of them from an entry a previous question stored.
  //
  // The witness is one group whose body is expensive to resolve and 6,000
  // repeats that each ask about it: `(?:\1x)*` is a repeat, so codegen asks
  // whether its body can match empty, and answering walks group 1. Old cost is
  // questions times body; new cost is body plus questions - so the fixed build
  // barely moves as the pattern grows and the broken one goes up with the
  // product. Measured 2.50 s against 0.014 s in release, a factor of 181.
  //
  // Half a second clears both populations in both builds: release 0.016 s
  // fixed against 2.65 s broken, and under AddressSanitizer 0.053 s against
  // 10.97 s. That is 9.4x under the bound and 5.3x over it.
  std::string body;
  for (int i = 0; i < 5000; i++) {
    body += "(?:a|b)";
  }
  std::string pattern = "(" + body + "\\1)";
  for (int i = 0; i < 6000; i++) {
    pattern += "(?:\\1" + std::string(1, (char)('a' + i % 26)) + ")*";
  }

  GRX_Limits limits;
  grx_limits_default(&limits);
  // Two caps lifted rather than the one its neighbours lift, and the second is
  // worth a word. Growing this pattern costs the fixed build nothing and the
  // broken one everything, so the separation is bought with size - and at
  // 83 KB it is past the 64 KB max_pattern_length ships with. Neither cap is
  // what is being measured: a pattern refused for its length is never analysed
  // at all.
  limits.max_program_size = 0;
  limits.max_pattern_length = 0;

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
      << pattern.size() << "-byte pattern, one expensive group and 6,000 "
      << "repeats that each ask about it, took " << seconds
      << " s to compile. codegen holds one GRX_IRMemo for the compile; a "
         "figure this large means every question is building its own analysis "
         "again";
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

namespace {

/** Whether `byte` is in a facts bitmap. */
bool first_byte_set(const GRX_Facts & facts, unsigned char byte) {
  return (facts.first_bytes[byte >> 3] & (1u << (byte & 7u))) != 0;
}

/** Every pattern the prefilter tests below agree to answer about. */
const char * const kPrefilterPatterns[] = {
  "xyzzy", "[b-z]+q", "aaaaaaaaab", "a.{2}q", "(a+)(b+)", "abc|def",
  "^foo", "a*b", "a?b", "(?i)abc", "[[:digit:]]x", "\\bword", "(?=a)b",
  "(?!a)b", "(?<=a)b", "\\Kabc", "(a)\\1", "(?:abc){2,}", "a{0,3}b",
  "(?>abc)d", "[^a]x", "\\d+", "(a|bb|ccc)d", "x|yy|zzz", "\\s+z",
  // `.` beside something that does constrain the first byte. Alone it makes
  // the set empty and the empty set is reported as unknown, so a bug that let
  // `.` contribute nothing instead of giving up would hide behind that - it
  // needs a branch that still contributes for the set to come back wrong.
  "a|.b", "ab|.q", "a|.", "(?s)x|.y", "[abc]|.",
  // The same shape for the opcodes the walk does not model at all: a branch
  // that gives up beside a branch that contributes. A backreference, a verb,
  // a subroutine call and a script run each have to take the whole set down
  // with them rather than quietly leaving their own starts out of it.
  "(*ACCEPT)|a", "(a)b|\\1c", "a|(?R)b", "(*sr:qz)|a", "(a)(?:\\1|b)c",
  // A conditional whose condition is an assertion: the LOOK that carries it
  // is laid out as two jumps rather than one continuation, and which of them
  // continues the match is the thing the walk would have to know. It refuses
  // the form instead, and these are what hold it to that.
  "(?(?=a)b|c)", "(?(?<=x)y|z)", "q(?(?=a)b|c)", "(?(?!a)b|c)",
  // A non-atomic lookbehind, which is inlined and so consumes backwards on
  // the main path. Its bytes are not at the start position and the set has to
  // refuse rather than name them.
  "(?<*ab)c", "(?<*a)b|q", "x(?<*y)z",
  // And a conditional on whether a group took part, which is the ordinary
  // COND: both arms can begin the match, so both have to be walked.
  "(a)?(?(1)b|z)", "(?(1)a|q)(x)", "(a)?(?(1)b|)",
  "(?:(?:a|b)c)+", "[\\x00-\\x08]q", "\\n+x", "[a-c]|[x-z]", "q$",
  "(?m)^ab", "a(?#comment)b", "\\Qa+b\\E", "[[:^alpha:]]z", "(?s).x",
  "", "a|", "(a*)", "x*", "(?:)", ".", "[^q]",
};

/**
 * The same, in UTF mode and over characters that are not one byte.
 *
 * A separate list because the byte set is built from code points there: a
 * range becomes a run of *leading* bytes, and the arithmetic that turns one
 * into the other is only reached with GRX_OPT_UTF. Without these the plain
 * list above leaves it untested - which is not a guess, it is what a mutation
 * that kept only the low end of every range survived.
 */
const char * const kPrefilterUtfPatterns[] = {
  "é", "[é-ü]", "[\\x{100}-\\x{200}]x", "\\p{Lu}", "(?i)k", "(?i)é",
  "[\\x{1F600}-\\x{1F64F}]", "\\x{4E00}+", "[a\\x{80}\\x{7FF}\\x{800}]",
  "[\\x{7F}-\\x{81}]", "[\\x{FFFF}-\\x{10000}]", "é|ü|漢",
  "[^\\x{100}]q", "\\w+\\x{E9}", "[\\x{10FFFF}]", "(?:漢字)+",
};

/**
 * How far the anchored sweep below advances from `at`.
 *
 * One character in UTF mode and one byte otherwise. A sweep that stepped bytes
 * would start a search inside a character, which is not a position a search
 * can begin at and which the library refuses rather than answers - so the
 * sweep would be comparing an error against a match and calling it a
 * disagreement about the prefilter.
 */
size_t utf_step(const std::string & subject, size_t at, bool utf) {
  if (!utf || at >= subject.size()) {
    return 1;
  }
  unsigned char lead = (unsigned char)subject[at];
  if (lead < 0x80u) { return 1; }
  if ((lead & 0xE0u) == 0xC0u) { return 2; }
  if ((lead & 0xF0u) == 0xE0u) { return 3; }
  if ((lead & 0xF8u) == 0xF0u) { return 4; }
  return 1;
}

/** Subjects for the list above: every length class and the edges between. */
std::vector<std::string> utf_subjects() {
  return {
    "", "a", "é", "ü", "漢", "字", "🙂", "aé", "éa", "éü", "漢字",
    "a\xC2\x80", "\xC2\x80", "\xDF\xBF", "\xE0\xA0\x80", "\xEF\xBF\xBF",
    "\xF0\x90\x80\x80", "\xF4\x8F\xBF\xBF", "K", "\xE2\x84\xAA",
    "aébü", "xé漢🙂z", "漢aé", "AÉÎ", "kK\xE2\x84\xAA",
  };
}

} // namespace

TEST(Compile, SkippingPositionsFindsTheSameMatchesAsTryingThemAll) {
  // prefilter.c's set lets both engines step over positions without running
  // anything there, so the whole of its correctness is that it never steps
  // over a position where a match begins. This checks that against the engines
  // rather than against itself, and without trusting the reported span.
  //
  // The reference is the same library with GRX_OPT_ANCHORED, tried at every
  // offset in turn: an anchored search never skips, so it is the brute force
  // the skipping search has to agree with. Leftmost says the unanchored answer
  // must be the anchored one at the first offset that has any, and the spans
  // must be the same span.
  //
  // Not the reported match start, which is what the first version of this
  // tested and got wrong: `\K` moves the *reported* start past where the
  // attempt began - `ab\K` reports an empty span two bytes along - so the byte
  // the set constrains is not the byte the caller is shown. Vim's `\zs` is the
  // same construct. The set is about where an attempt can begin, which is only
  // observable by comparing whole searches.
  std::vector<std::string> subjects;
  const std::string alphabet = "abqz";
  subjects.push_back("");
  for (size_t length = 1; length <= 4; length++) {
    size_t total = 1;
    for (size_t i = 0; i < length; i++) { total *= alphabet.size(); }
    for (size_t n = 0; n < total; n++) {
      std::string s;
      size_t at = n;
      for (size_t i = 0; i < length; i++) {
        s += alphabet[at % alphabet.size()];
        at /= alphabet.size();
      }
      subjects.push_back(s);
    }
  }
  subjects.push_back("ab\ncd");
  subjects.push_back("x\t y7");
  subjects.push_back(std::string("\x01\x05q", 3));
  subjects.push_back("ccc d");
  subjects.push_back("Foo FOO foo");
  subjects.push_back("a+b");

  size_t known = 0;
  size_t compared = 0;
  std::vector<std::pair<const char *, uint32_t>> work;
  for (const char * pattern : kPrefilterPatterns) {
    work.emplace_back(pattern, GRX_OPT_NONE);
  }
  for (const char * pattern : kPrefilterUtfPatterns) {
    work.emplace_back(pattern, GRX_OPT_UTF);
    work.emplace_back(pattern, GRX_OPT_UTF | GRX_OPT_CASELESS);
  }
  const std::vector<std::string> wide = utf_subjects();
  for (const auto & item : work) {
    const char * pattern = item.first;
    const uint32_t options = item.second;
    GRX_Regex * loose = nullptr;
    GRX_Regex * pinned = nullptr;
    if (grx_regex_compile_with_allocator(pattern, strlen(pattern),
            GRX_SYNTAX_PCRE, options, nullptr, nullptr, nullptr, &loose)
            != GRX_OK
        || grx_regex_compile_with_allocator(pattern, strlen(pattern),
               GRX_SYNTAX_PCRE, options | GRX_OPT_ANCHORED, nullptr, nullptr,
               nullptr, &pinned)
            != GRX_OK) {
      grx_regex_free(loose);
      grx_regex_free(pinned);
      continue;
    }
    GRX_Facts facts;
    ASSERT_EQ(grx_regex_facts(loose, &facts), GRX_OK);
    if (facts.first_bytes_known) {
      known++;
    }

    for (const std::string & subject :
        (options & GRX_OPT_UTF) ? wide : subjects) {
      // Where the brute force says the leftmost match is.
      size_t want_begin = GRX_NPOS;
      size_t want_end = GRX_NPOS;
      GRX_Result want_rc = GRX_OK;
      for (size_t at = 0; at <= subject.size();
          at += utf_step(subject, at, (options & GRX_OPT_UTF) != 0)) {
        GRX_Match * match = nullptr;
        ASSERT_EQ(grx_match_create(pinned, nullptr, &match), GRX_OK);
        int matched = 0;
        GRX_Result rc = grx_regex_search(pinned, subject.data(),
            subject.size(), at, GRX_ENGINE_AUTO, nullptr, match, &matched);
        if (rc == GRX_OK && matched) {
          GRX_Capture span {};
          grx_match_span(match, &span);
          want_begin = span.start;
          want_end = span.end;
        }
        grx_match_destroy(match);
        if (rc != GRX_OK) {
          want_rc = rc;
          break;
        }
        if (want_begin != GRX_NPOS) {
          break;
        }
      }

      GRX_Match * match = nullptr;
      ASSERT_EQ(grx_match_create(loose, nullptr, &match), GRX_OK);
      int matched = 0;
      GRX_Result rc = grx_regex_search(loose, subject.data(), subject.size(),
          0, GRX_ENGINE_AUTO, nullptr, match, &matched);
      size_t got_begin = GRX_NPOS;
      size_t got_end = GRX_NPOS;
      if (rc == GRX_OK && matched) {
        GRX_Capture span {};
        grx_match_span(match, &span);
        got_begin = span.start;
        got_end = span.end;
      }
      grx_match_destroy(match);

      // A pattern can spend max_steps rather than answer - `a|(?R)b` does,
      // recursion against an unanchored search being exactly that - and a
      // run that stopped at a limit has no leftmost match to compare. What
      // still has to hold is that the two searches agree on *whether* they
      // answered: a skip that turned a limit into a quiet non-match would be
      // this test passing for the wrong reason.
      EXPECT_EQ(rc != GRX_OK, want_rc != GRX_OK)
          << pattern << " on \"" << subject
          << "\": one of the two searches stopped at a limit and the other "
             "did not";
      if (rc != GRX_OK || want_rc != GRX_OK) {
        continue;
      }

      compared++;
      EXPECT_EQ(got_begin, want_begin)
          << pattern << " on \"" << subject
          << "\": the skipping search and the anchored sweep disagree about "
             "where the leftmost match begins";
      if (got_begin == want_begin) {
        EXPECT_EQ(got_end, want_end)
            << pattern << " on \"" << subject << "\" at " << got_begin
            << ": same start, different extent";
      }
    }
    grx_regex_free(loose);
    grx_regex_free(pinned);
  }

  // Denominators, because a sweep that reaches nothing passes. `known` is the
  // one that matters: every comparison above is vacuous for a pattern with no
  // set, since nothing is skipped for it.
  EXPECT_GE(known, 25u) << "only " << known << " patterns produced a set";
  EXPECT_GE(compared, 5000u) << "only " << compared << " searches compared";
}

TEST(Compile, AnEmptyMatchLeavesNoFirstByteSetUnlessKeepMovedTheStart) {
  // Two passes answer "can this match nothing" independently - the tree walk
  // that fills GRX_Facts::can_match_empty, and the program walk that gives up
  // the moment it can reach MATCH without consuming. They have to agree, and
  // where they do not is worth naming rather than smoothing over.
  //
  // They disagree on exactly one construct. `can_match_empty` is about the
  // span the caller is *shown*, and `\K` can empty that span while the attempt
  // still consumed input: `ab\K` matches only where "ab" is and reports the
  // empty span after it. The prefilter is about what the attempt consumes, so
  // it rightly keeps its set of {a}. Vim's `\zs` is the same construct.
  struct Row { const char * pattern; int empty; int known; };
  const Row rows[] = {
    {"abc", 0, 1},      // neither
    {"a*", 1, 0},       // empty without consuming: no set
    {"(?:)", 1, 0},     // and the plainest form of it
    {"a|", 1, 0},       // one branch of an alternation is enough
    {"\\Kabc", 1, 1},   // `\K` at the front changes nothing it can see
    {"ab\\K", 1, 1},    // and at the back empties the span, not the attempt
  };
  for (const Row & row : rows) {
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile(row.pattern, GRX_SYNTAX_PCRE, GRX_OPT_NONE,
                  &regex),
        GRX_OK)
        << row.pattern;
    GRX_Facts facts;
    ASSERT_EQ(grx_regex_facts(regex, &facts), GRX_OK);
    EXPECT_EQ(facts.can_match_empty != 0, row.empty != 0) << row.pattern;
    EXPECT_EQ(facts.first_bytes_known != 0, row.known != 0) << row.pattern;
    if (row.known) {
      EXPECT_TRUE(first_byte_set(facts, 'a')) << row.pattern;
      EXPECT_FALSE(first_byte_set(facts, 'z')) << row.pattern;
    }
    grx_regex_free(regex);
  }
}

TEST(Compile, TheEngineSkipsWhatThePrefilterRulesOut) {
  // That the set exists is one thing and that a search uses it is another.
  // Without the skip, a literal that is not in the subject costs a closure at
  // every byte: measured over a megabyte, 27 ns per byte before and 0.9 after,
  // which is thirty-one times and is also the difference between this library
  // and everything it is compared against in notes/regex/TODO.md section 14x.
  //
  // A megabyte at the unskipped rate is 28 ms per search and 2.8 s for the
  // hundred here; skipped it is under 0.1 s. The bound is one second, which
  // clears both populations in both builds - under AddressSanitizer the
  // skipped figure is 0.35 s and the unskipped some twelve.
  std::string subject(1024 * 1024, 'a');
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile("xyzzy", GRX_SYNTAX_PCRE, GRX_OPT_NONE, &regex),
      GRX_OK);
  GRX_Facts facts;
  ASSERT_EQ(grx_regex_facts(regex, &facts), GRX_OK);
  ASSERT_TRUE(facts.first_bytes_known);

  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
  auto start = std::chrono::steady_clock::now();
  for (int i = 0; i < 100; i++) {
    int matched = 1;
    ASSERT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                  GRX_ENGINE_AUTO, nullptr, match, &matched),
        GRX_OK);
    ASSERT_FALSE(matched);
  }
  auto elapsed = std::chrono::steady_clock::now() - start;
  grx_match_destroy(match);
  grx_regex_free(regex);

  auto seconds
      = std::chrono::duration_cast<std::chrono::duration<double>>(elapsed)
            .count();
  EXPECT_LT(seconds, 1.0)
      << "100 failing searches of a 1 MB subject for a five-byte literal took "
      << seconds
      << " s. Both engines skip positions whose byte is not in the program's "
         "first-byte set; a figure this large means the skip is not happening";
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

namespace {

/**
 * Patterns for the two literal facts.
 *
 * kPrefilterPatterns is about the *first* byte and is full of shapes that
 * refuse to have one; these are shapes with a literal run somewhere in them,
 * which is the axis the byte set cannot reach at all. `aaaaaaaaab` is the
 * case documentation/design.md names: every position passes a first-byte test
 * there and only one passes a string test.
 */
const char * const kLiteralPatterns[] = {
  "abc", "abc*", "a*bc", ".*foo", "foo.*", "[0-9]+-[0-9]+", "cat|car",
  "(abc)+", "^hello world", "a{3}b", "xyzzy", "aaaaaaaaab", "colou?r",
  "(a|b)zebra(c|d)", "[abc]def", "ab(cd)ef", "(a)(b)(c)", "q(?:abc)+z",
  "\\d+apple", "x*apple", "(?:ab)+cd", "ab\\Kcd", "ab\\Kcd|q",
  "(?=x)abc", "(?i)abc", "\\bword\\b", "a(?:bc)d", "foo(?!bar)",
  "(?<=x)abc", "(?>abc)d", "a(?#x)bc", "\\Qa+b\\E", "z(a)\\1z",
  "(?:a|a)bc", "a+b+c", "[^q]xyz", "\\A\\Qliteral\\E\\z", "(a*)bcd(e*)",
  "abc$", "(?m)abc$", "a\\nb", "a\\x00b", "(?:x|y)hello(?:x|y)",
  // One instruction that *consumes* in the middle of a literal run. The
  // required literal steps over the instructions between two characters, and
  // without one of these in the population it could step over a consuming one
  // and claim "ab" of a pattern every match of which is three characters.
  "a[0-9]b", "ab.cd", "q[a-z]z", "xy\\wz", "ab[^q]cd", "a.b.c",
  // A branch in the middle of what looks like a literal run. The prefix walk
  // stops at a branch and the required literal will not cross one, and these
  // are what say so: every one of them has a match that omits a character a
  // walk following the preferred arm would have claimed.
  "ab?c", "abc?d", "a(?:b)?c", "ax*y", "ab|abc", "a(?:b|)c", "ab{0,1}c",
  // Added 2026-09-28 for the offset window. Each separates the start of a
  // match from the literal every match contains by a different mechanism, so
  // that the distance is fixed, a range, or unbounded, rather than only the
  // zero the rest of this list mostly gives.
  "a.{3}q", "a.{1,3}q", "a{0,4}q", "a?q", "(abc)?q", "a+q", "a*q",
  "x(ab|cd)y", "(foo|bar)baz", "abcdef.*z", "a.b.c", "(a|b)zebra(c|d)",
  "", "a", ".", "a|", "(?R)x", "(*ACCEPT)abc", "\\1abc", "abc(?R)",
};

/** The same, for UTF mode, where a character is more than one byte. */
const char * const kLiteralUtfPatterns[] = {
  "é", "café", ".*café", "漢字", "x*漢字", "(?:漢)+字", "🙂ok",
  "(?i)é", "[a-z]+é", "é|ü", "a\\x{1F600}b",
};

/** Subjects that actually contain the literals above, and ones that do not. */
std::vector<std::string> literal_subjects() {
  return {
    "", "a", "abc", "abcabc", "xabc", "abcx", "aaaaaaaaab", "aaaaaaaaa",
    "aaaaaaaaabaaaaaaaaab", "foo", "xfoo", "fooy", "barfoobaz", "cat", "car",
    "hello world", "say hello world now", "aaab", "xyzzy", "colour", "color",
    "azebrac", "bzebrad", "zebra", "adef", "abcdef", "12-34", "1-2",
    "q abc abc z", "qabcabcz", "5apple", "apple", "xxapple", "word",
    "a word here", "abcd", "cd", "abKcd", "literal", "a\nb",
    std::string("a\x00" "b", 3), "xhelloy", "xhellox", "AbC", "ABC",
    "a+b", "zaaz", "abcq",
  };
}

std::vector<std::string> literal_utf_subjects() {
  return {
    "", "é", "café", "un café noir", "xxcafé", "漢字", "x漢字",
    "xx漢字yy", "漢漢字", "🙂ok", "z🙂okz", "ü", "aé", "É", "a😀b",
  };
}

/** Every match of `pattern` in `subject`, found by anchoring at each offset. */
std::vector<std::pair<size_t, size_t>> every_match(
    GRX_Regex * pinned, const std::string & subject, bool utf, bool * ok) {
  std::vector<std::pair<size_t, size_t>> out;
  *ok = true;
  for (size_t at = 0; at <= subject.size(); at += utf_step(subject, at, utf)) {
    GRX_Match * match = nullptr;
    if (grx_match_create(pinned, nullptr, &match) != GRX_OK) {
      *ok = false;
      return out;
    }
    int matched = 0;
    GRX_Result rc = grx_regex_search(pinned, subject.data(), subject.size(),
        at, GRX_ENGINE_AUTO, nullptr, match, &matched);
    if (rc == GRX_OK && matched) {
      GRX_Capture span {};
      grx_match_span(match, &span);
      out.emplace_back(span.start, span.end);
    }
    grx_match_destroy(match);
    if (rc != GRX_OK) {
      *ok = false;
      return out;
    }
  }
  return out;
}

} // namespace

TEST(Compile, ALiteralPrefixIsOneEveryMatchReallyBeginsWith) {
  // The one claim the fact makes. Checked against the engines rather than
  // against the walk that produced it: every match the pattern has anywhere
  // in any subject, found by anchoring at each offset in turn, has to begin
  // with those bytes.
  //
  // `\K` is why the claim is about the *reported* span and not about where
  // the attempt began. `ab\Kcd` consumes "ab" first and reports a span
  // starting at `c`, so a prefix taken from the attempt would be a prefix of
  // no match at all - which is why prefilter.c refuses the fact outright for
  // a program containing one, and why `ab\Kcd` is in the list above.
  size_t offered = 0;
  size_t checked = 0;
  std::vector<std::pair<const char *, uint32_t>> work;
  for (const char * pattern : kLiteralPatterns) {
    work.emplace_back(pattern, GRX_OPT_NONE);
  }
  for (const char * pattern : kLiteralUtfPatterns) {
    work.emplace_back(pattern, GRX_OPT_UTF);
  }

  for (const auto & item : work) {
    const bool utf = (item.second & GRX_OPT_UTF) != 0;
    GRX_Regex * loose = nullptr;
    GRX_Regex * pinned = nullptr;
    if (grx_regex_compile_with_allocator(item.first, strlen(item.first),
            GRX_SYNTAX_PCRE, item.second, nullptr, nullptr, nullptr, &loose)
            != GRX_OK
        || grx_regex_compile_with_allocator(item.first, strlen(item.first),
               GRX_SYNTAX_PCRE, item.second | GRX_OPT_ANCHORED, nullptr,
               nullptr, nullptr, &pinned) != GRX_OK) {
      grx_regex_free(loose);
      grx_regex_free(pinned);
      continue;
    }
    GRX_Facts facts;
    ASSERT_EQ(grx_regex_facts(loose, &facts), GRX_OK);
    if (!facts.literal_prefix) {
      EXPECT_EQ(facts.literal_prefix_length, 0u) << item.first;
      grx_regex_free(loose);
      grx_regex_free(pinned);
      continue;
    }
    offered++;
    EXPECT_GT(facts.literal_prefix_length, 0u) << item.first;
    std::string prefix(facts.literal_prefix, facts.literal_prefix_length);

    for (const std::string & subject :
        utf ? literal_utf_subjects() : literal_subjects()) {
      bool ok = false;
      auto spans = every_match(pinned, subject, utf, &ok);
      if (!ok) {
        continue;
      }
      for (const auto & span : spans) {
        checked++;
        ASSERT_LE(span.second, subject.size());
        std::string text = subject.substr(span.first, span.second - span.first);
        EXPECT_EQ(text.compare(0, prefix.size(), prefix), 0)
            << "pattern " << item.first << " claims every match starts with \""
            << prefix << "\", but \"" << text << "\" at " << span.first
            << " in \"" << subject << "\" does not";
      }
    }
    grx_regex_free(loose);
    grx_regex_free(pinned);
  }

  // A sweep that offered nothing would pass without having asked anything.
  EXPECT_GT(offered, 20u) << "hardly any pattern here has a literal prefix, "
                             "so this gate is checking almost nothing";
  EXPECT_GT(checked, 200u) << "the subjects do not match these patterns, so "
                              "no match was ever examined";
}

TEST(Compile, ARequiredLiteralIsOneEveryMatchReallyContains) {
  size_t offered = 0;
  size_t checked = 0;
  std::vector<std::pair<const char *, uint32_t>> work;
  for (const char * pattern : kLiteralPatterns) {
    work.emplace_back(pattern, GRX_OPT_NONE);
  }
  for (const char * pattern : kLiteralUtfPatterns) {
    work.emplace_back(pattern, GRX_OPT_UTF);
  }

  for (const auto & item : work) {
    const bool utf = (item.second & GRX_OPT_UTF) != 0;
    GRX_Regex * loose = nullptr;
    GRX_Regex * pinned = nullptr;
    if (grx_regex_compile_with_allocator(item.first, strlen(item.first),
            GRX_SYNTAX_PCRE, item.second, nullptr, nullptr, nullptr, &loose)
            != GRX_OK
        || grx_regex_compile_with_allocator(item.first, strlen(item.first),
               GRX_SYNTAX_PCRE, item.second | GRX_OPT_ANCHORED, nullptr,
               nullptr, nullptr, &pinned) != GRX_OK) {
      grx_regex_free(loose);
      grx_regex_free(pinned);
      continue;
    }
    GRX_Facts facts;
    ASSERT_EQ(grx_regex_facts(loose, &facts), GRX_OK);
    if (!facts.required_literal) {
      EXPECT_EQ(facts.required_literal_length, 0u) << item.first;
      grx_regex_free(loose);
      grx_regex_free(pinned);
      continue;
    }
    offered++;
    std::string needle(facts.required_literal, facts.required_literal_length);
    EXPECT_GT(needle.size(), 0u) << item.first;

    for (const std::string & subject :
        utf ? literal_utf_subjects() : literal_subjects()) {
      bool ok = false;
      auto spans = every_match(pinned, subject, utf, &ok);
      if (!ok) {
        continue;
      }
      for (const auto & span : spans) {
        checked++;
        std::string text = subject.substr(span.first, span.second - span.first);
        EXPECT_NE(text.find(needle), std::string::npos)
            << "pattern " << item.first << " claims every match contains \""
            << needle << "\", but \"" << text << "\" at " << span.first
            << " in \"" << subject << "\" does not";
      }
    }
    grx_regex_free(loose);
    grx_regex_free(pinned);
  }

  EXPECT_GT(offered, 20u) << "hardly any pattern here has a required literal";
  EXPECT_GT(checked, 200u) << "no match was ever examined";
}

TEST(Compile, TheRequiredLiteralSitsWhereTheOffsetWindowSaysItDoes) {
  // The claim the *skip* rests on, and it is a stronger one than the sweep
  // above makes. Knowing every match contains the literal only says the
  // subject is worth searching; knowing it sits `[min, max]` bytes from the
  // start of the match is what lets a position be ruled out - and a window
  // that is too narrow drops a start that would have matched, which is the
  // one way this optimisation can be wrong rather than merely weak.
  //
  // Checked against the engines: for every match the pattern has anywhere in
  // every subject, some occurrence of the literal inside it must fall in the
  // window. Not *every* occurrence - the match is entitled to contain others.
  size_t offered = 0;
  size_t bounded = 0;
  size_t checked = 0;
  std::vector<std::pair<const char *, uint32_t>> work;
  for (const char * pattern : kLiteralPatterns) {
    work.emplace_back(pattern, GRX_OPT_NONE);
  }
  for (const char * pattern : kLiteralUtfPatterns) {
    work.emplace_back(pattern, GRX_OPT_UTF);
  }

  for (const auto & item : work) {
    const bool utf = (item.second & GRX_OPT_UTF) != 0;
    GRX_Regex * loose = nullptr;
    GRX_Regex * pinned = nullptr;
    if (grx_regex_compile_with_allocator(item.first, strlen(item.first),
            GRX_SYNTAX_PCRE, item.second, nullptr, nullptr, nullptr, &loose)
            != GRX_OK
        || grx_regex_compile_with_allocator(item.first, strlen(item.first),
               GRX_SYNTAX_PCRE, item.second | GRX_OPT_ANCHORED, nullptr,
               nullptr, nullptr, &pinned) != GRX_OK) {
      grx_regex_free(loose);
      grx_regex_free(pinned);
      continue;
    }
    const GRX_Program * program = &loose->program;
    if (!program->required_literal_length) {
      grx_regex_free(loose);
      grx_regex_free(pinned);
      continue;
    }
    offered++;
    std::string needle(
        program->required_literal, program->required_literal_length);
    const size_t low = program->required_offset_min;
    const size_t high = program->required_offset_max;
    if (high != GRX_NPOS) {
      bounded++;
      EXPECT_LE(low, high) << item.first;
    }

    for (const std::string & subject :
        utf ? literal_utf_subjects() : literal_subjects()) {
      bool ok = false;
      auto spans = every_match(pinned, subject, utf, &ok);
      if (!ok || high == GRX_NPOS) {
        continue;
      }
      for (const auto & span : spans) {
        std::string text = subject.substr(span.first, span.second - span.first);
        bool inside = false;
        for (size_t at = text.find(needle); at != std::string::npos;
            at = text.find(needle, at + 1)) {
          if (at >= low && at <= high) {
            inside = true;
            break;
          }
        }
        checked++;
        EXPECT_TRUE(inside)
            << "pattern " << item.first << " puts \"" << needle
            << "\" at [" << low << ", " << high << "] from the start of a "
            << "match, but \"" << text << "\" at " << span.first << " in \""
            << subject << "\" has no occurrence there";
      }
    }
    grx_regex_free(loose);
    grx_regex_free(pinned);
  }

  EXPECT_GT(offered, 20u) << "hardly any pattern here has a required literal";
  EXPECT_GT(bounded, 10u)
      << "every window is unbounded, so this gate never tested one";
  EXPECT_GT(checked, 100u) << "no match was ever examined";
}

TEST(Compile, AmongEqualLiteralsTheOneThePrefixAlreadyGivesIsTheLastChoice) {
  // By value, because the sweeps cannot see this at all: every answer below
  // and every answer this used to give are equally true facts about the
  // pattern, and the difference is only which one is worth having.
  //
  // `a.{20}q` is the case that prompted it. `a` and `q` both dominate every
  // match and both are one byte, the earlier used to win, and the answer was
  // then identical to the literal prefix - one fact written twice, and a
  // benchmark row three orders of magnitude off what the other was worth.
  struct Expected {
    const char * pattern;
    const char * required;
    size_t low;
    size_t high;   // GRX_NPOS for unbounded.
  };
  const Expected table[] = {
    // The tie-break doing its work: not the byte at offset 0.
    {"a.{20}q", "q", 21, 21},
    {"a.{3}q", "q", 4, 4},
    {"a.{1,3}q", "q", 2, 4},
    {"a?q", "q", 0, 1},
    {"(abc)?q", "q", 0, 3},
    {"x(ab|cd)y", "y", 3, 3},
    {"(a|b)zebra(c|d)", "zebra", 1, 1},
    {"(foo|bar)baz", "baz", 3, 3},
    {"(a+)(b+)", "b", 0, GRX_NPOS},
    // Length still decides first, so a long literal at offset 0 keeps the
    // answer even though a shorter one sits further in.
    {"abcdef.*z", "abcdef", 0, 0},
    {"xyzzy", "xyzzy", 0, 0},
    {"aaaaaaaaab", "aaaaaaaaab", 0, 0},
    {"colou?r", "colo", 0, 0},
    // Unbounded before the literal: the window is given up, the literal is
    // not.
    {".*foo", "foo", 0, GRX_NPOS},
    {"a*q", "q", 0, GRX_NPOS},
  };

  for (const Expected & want : table) {
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile_with_allocator(want.pattern,
                  strlen(want.pattern), GRX_SYNTAX_PCRE, GRX_OPT_NONE,
                  nullptr, nullptr, nullptr, &regex),
        GRX_OK)
        << want.pattern;
    const GRX_Program * program = &regex->program;
    EXPECT_EQ(std::string(program->required_literal,
                  program->required_literal_length),
        std::string(want.required))
        << want.pattern;
    EXPECT_EQ(program->required_offset_min, want.low) << want.pattern;
    EXPECT_EQ(program->required_offset_max, want.high) << want.pattern;
    grx_regex_free(regex);
  }
}

TEST(Compile, WhichLiteralsTheseParticularPatternsHaveAndWhichHaveNone) {
  // By value, because the sweeps above can only catch a literal that is
  // *wrong*; one that is merely absent passes them silently, and absent is
  // what a broken walk returns. Each row says which of the two facts the
  // pattern has, so a change that quietly stops answering is a failure and
  // not an improvement in the skip rate.
  struct Row {
    const char * pattern;
    const char * prefix;    // nullptr for "there must be none"
    const char * required;
  };
  const Row rows[] = {
    {"abc", "abc", "abc"},
    {"aaaaaaaaab", "aaaaaaaaab", "aaaaaaaaab"},
    {".*foo", nullptr, "foo"},
    {"foo.*", "foo", "foo"},
    {"a*bc", nullptr, "bc"},
    {"ab(cd)ef", "abcdef", "abcdef"},
    {"\\d+apple", nullptr, "apple"},
    {"^hello world", "hello world", "hello world"},
    {"colou?r", "colo", "colo"},
    {"(abc)+", "abc", "abc"},
    // Both arms spell the same text and neither instruction is on every
    // path, so dominance cannot see it. Recorded as a known weakness rather
    // than left to look like an accident.
    {"cat|car", nullptr, nullptr},
    // `\K` moves the reported start, so nothing consumed before it is part of
    // any match and both facts are refused.
    {"ab\\Kcd", nullptr, nullptr},
    // A class is a choice of code points, so it ends a literal run.
    {"(?i)abc", nullptr, nullptr},
    {"[abc]def", nullptr, "def"},
    // A lookaround consumes nothing of the match, so the prefix walks past it
    // and the required literal refuses the program outright.
    {"(?=x)abc", "abc", nullptr},
    {"", nullptr, nullptr},
    {".", nullptr, nullptr},
  };
  for (const Row & row : rows) {
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile_with_allocator(row.pattern,
                  strlen(row.pattern), GRX_SYNTAX_PCRE, GRX_OPT_NONE, nullptr,
                  nullptr, nullptr, &regex),
        GRX_OK)
        << row.pattern;
    GRX_Facts facts;
    ASSERT_EQ(grx_regex_facts(regex, &facts), GRX_OK);

    if (row.prefix) {
      ASSERT_NE(facts.literal_prefix, nullptr) << row.pattern;
      EXPECT_EQ(
          std::string(facts.literal_prefix, facts.literal_prefix_length),
          std::string(row.prefix))
          << row.pattern;
    }
    else {
      EXPECT_EQ(facts.literal_prefix, nullptr)
          << row.pattern << " offered a prefix where none was expected";
    }
    if (row.required) {
      ASSERT_NE(facts.required_literal, nullptr) << row.pattern;
      EXPECT_EQ(
          std::string(facts.required_literal, facts.required_literal_length),
          std::string(row.required))
          << row.pattern;
    }
    else {
      EXPECT_EQ(facts.required_literal, nullptr)
          << row.pattern << " offered a required literal where none was "
                            "expected";
    }
    grx_regex_free(regex);
  }
}

TEST(Compile, ALiteralIsTruncatedRatherThanRefusedWhenItRunsLong) {
  // Truncation is sound for both facts - a prefix of a string every match
  // begins with is still one, and a substring of one every match contains is
  // still one - so a pattern longer than the cap answers with as much as
  // fits rather than with nothing.
  std::string pattern(200, 'q');
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile_with_allocator(pattern.data(), pattern.size(),
                GRX_SYNTAX_PCRE, GRX_OPT_NONE, nullptr, nullptr, nullptr,
                &regex),
      GRX_OK);
  GRX_Facts facts;
  ASSERT_EQ(grx_regex_facts(regex, &facts), GRX_OK);
  ASSERT_NE(facts.literal_prefix, nullptr);
  EXPECT_GT(facts.literal_prefix_length, 8u);
  EXPECT_LE(facts.literal_prefix_length, 64u);
  EXPECT_EQ(std::string(facts.literal_prefix, facts.literal_prefix_length),
      std::string(facts.literal_prefix_length, 'q'));
  grx_regex_free(regex);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
