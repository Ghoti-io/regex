/**
 * @file
 *
 * What the engines cost in C stack, measured rather than reviewed.
 *
 * design.md section 9 invariant 6 says no engine's C stack depth depends on
 * the subject or the program, and until this file it was the one invariant
 * whose enforcement was the word "Reviewed". It was also half wrong. The
 * subject half holds - every engine is a loop over a program with its own
 * heap stack, and a subject a thousand times longer costs the same frame.
 * The program half does not: run() in the backtracker calls itself for an
 * assertion body, a lookbehind's forward pass and a sub-match condition, so
 * one level of nested assertion is one C frame, and the cost is linear in
 * how deeply the *program* nests them.
 *
 * That was worth finding. `max_nesting_depth` bounded it at the defaults by
 * accident - 128 levels is about 77 KB - but the field is the caller's to
 * raise, and raising it to the 480 the parser can take asked the matcher for
 * 241 KB, which on the 256 KB stack testing.md section 12 runs every harness
 * under was a segmentation fault with no diagnostic. It is now
 * GRX_BACKTRACK_MAX_C_DEPTH and a GRX_ERR_LIMIT.
 *
 * **How the measuring works.** The call runs on a thread whose stack this
 * file owns, so it can paint the memory below the frame with a sentinel,
 * make the call, and scan back up for the first byte the call disturbed.
 * That is the high-water mark, in bytes, and it is exact - unlike counting
 * frames, which needs the engine's cooperation and would measure the
 * instrumentation as much as the engine.
 *
 * The numbers are ranges, not equalities: a frame's size moves with the
 * compiler and the optimisation level (a level cost 515 bytes at -O1 and 600
 * in this library's own -O0 objects), so a test that pinned a byte count
 * would fail on somebody else's compiler and teach nothing. What is pinned
 * is the shape: flat in the subject, flat in the program's size, linear in
 * assertion nesting, and capped.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <pthread.h>

#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "test_helpers.h"

#include "../../src/compile/compile_internal.h"
#include "../../src/ir/ir_internal.h"
#include "../../src/ir/lower_internal.h"
#include "../../src/parse/parse_internal.h"

namespace {

/**
 * Bytes painted below the measuring frame.
 *
 * Comfortably over the cap the engine now enforces - about 94 KB - so that a
 * measurement that saturates this is a failure to report rather than a
 * number to believe. saturated() is what checks it.
 */
constexpr size_t kPaintBytes = 1u << 20;

/** The sentinel. Any value works; this one is visible in a hex dump. */
constexpr unsigned char kPaint = 0x5A;

/**
 * Stack for the measuring thread. Must exceed kPaintBytes by enough for the
 * engine's own use on top of it.
 */
constexpr size_t kThreadStack = 8u << 20;

/**
 * How much two measurements of the same call shape may differ.
 *
 * Not zero, and that is not a weakening. One frame is not one size: a loop
 * that takes a different branch spends a different number of its own
 * temporaries, so the same non-recursive engine measures 1119 bytes against
 * an eight-byte subject and 1727 against a sixty-four byte one - the shorter
 * one being the *larger*, which is the tell that this is layout and not
 * growth. What matters is that the difference is a constant and not a rate.
 * The sweeps below vary their input by a factor of 8192, so a stack that
 * grew by even one byte per byte of subject would land 64 KB apart; 4 KB
 * separates "jitter inside a frame" from anything that could be called a
 * dependence.
 */
constexpr size_t kFrameJitter = 4096;

/**
 * Bytes painted below a codegen measurement, and the stack it runs on.
 *
 * Larger than the matcher's, because the recursion this measures was the
 * unbounded one: before the explicit stack in gen(), a level cost about four
 * frames and the deepest pattern these tests compile is nested 1920 levels,
 * so a megabyte of paint saturated and reported a lower bound instead of a
 * figure. Six leaves room to *see* a regression rather than to hit the end of
 * the ruler, which is what saturated() distinguishes.
 */
constexpr size_t kCodegenPaintBytes = 6u << 20;

/** Stack for the codegen measuring thread; must exceed the paint. */
constexpr size_t kCodegenThreadStack = 32u << 20;

/**
 * What the compile path is allowed, of the 256 KB testing.md section 12 runs
 * every harness under.
 *
 * Half, for the same reason the matcher's cap test uses half: a harness calls
 * into the library rather than being called by it, and a pass that spent the
 * whole stack would leave the caller none.
 *
 * Read this bound with the sweep below rather than on its own. Codegen's own
 * cost is about 4.5 KB flat, twenty-five times inside this, and the figure
 * that approaches it is the analysis pass codegen re-enters -
 * grx_ir_can_match_empty() on a repeat body, GRX_ANALYSIS_MAX_DEPTH levels of
 * walk() at some 246 bytes a level, which is 126 KB and would pass this by
 * 641 bytes. So what is asserted against it is codegen's own term and the
 * default limits, never that sum: a bound sized to admit a figure it barely
 * clears is a tolerance rather than a gate, and the analysis pass wants the
 * treatment the matcher got instead - see notes/regex/TODO.md section 14o.
 */
constexpr size_t kHarnessBudget = 128u * 1024u;

/**
 * Whether the high-water mark can be taken in this build.
 *
 * Not under AddressSanitizer. `detect_stack_use_after_return` is on by
 * default and moves a frame's locals onto a heap "fake stack", so the memory
 * below the measuring frame is no longer the memory the call will use -
 * painting it reports nothing, and painting it *through* ASan's redzones is
 * a stack-buffer-underflow abort rather than a measurement.
 *
 * The call still runs under ASan; only the ruler is put away. Everything
 * these tests say about a return code, a verdict or a crash is checked in
 * both builds, and the sanitizer build is the one where an engine that ran
 * off its own stack would be caught by something better than a byte count.
 */
#if defined(__SANITIZE_ADDRESS__)
#define GRX_STACK_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define GRX_STACK_ASAN 1
#else
#define GRX_STACK_ASAN 0
#endif
#else
#define GRX_STACK_ASAN 0
#endif

/**
 * Whether Valgrind is underneath, without needing its headers.
 *
 * Memcheck is the other tool that cannot watch this: writing below the stack
 * pointer on purpose is what the measurement *is*, and memcheck reports it
 * as an invalid write, which `make test-valgrind` runs with
 * --error-exitcode=1 and would fail on. RUNNING_ON_VALGRIND would be the
 * proper way to ask and needs valgrind/valgrind.h, which is not a build
 * dependency this library has; the preload library's name in the environment
 * is the usual substitute and costs nothing.
 *
 * Skipping also keeps `make test-valgrind` quick. The sweeps paint a
 * megabyte per measurement and there are some fifty of them, which memcheck
 * turns into half a minute of shadow-memory traffic that proves nothing it
 * has not already checked in the ordinary build.
 */
bool under_valgrind() {
  const char * preload = std::getenv("LD_PRELOAD");
  return preload != nullptr && std::strstr(preload, "vgpreload") != nullptr;
}

/** Whether the high-water mark can be taken in this run. */
bool stack_measurable() { return !GRX_STACK_ASAN && !under_valgrind(); }

/** A measurement, or the reason there is not one. */
struct Measurement {
  size_t bytes = 0;      ///< High-water mark below the measuring frame.
  GRX_Result result = GRX_OK; ///< What the search returned.
  int matched = 0;       ///< Whether it matched.
  bool saturated = false; ///< The paint ran out; `bytes` is a lower bound.
  bool measured = false; ///< Whether `bytes` means anything.
};

/** Skip the rest of a test that has nothing to measure with. */
#define SKIP_UNLESS_MEASURABLE()                                               \
  do {                                                                         \
    if (!stack_measurable()) {                                                 \
      GTEST_SKIP() << "the stack high-water mark cannot be read under "        \
                      "AddressSanitizer or Valgrind; see stack_measurable()";  \
    }                                                                          \
  } while (0)

struct Job {
  const GRX_Regex * regex;
  const char * subject;
  size_t length;
  GRX_Engine engine;
  const GRX_Limits * limits;
  Measurement out;
};

/**
 * The address of a frame, through a volatile so the compiler keeps it.
 *
 * noinline because an inlined version would report the caller's frame, and
 * the whole measurement is relative to this address.
 */
char * __attribute__((noinline)) frame_address() {
  char anchor;
  char * volatile address = &anchor;
  return address;
}

void * measure_on_thread(void * argument) {
  Job * job = static_cast<Job *>(argument);
  char * top = frame_address();
  char * low = top - kPaintBytes;
  const bool measurable = stack_measurable();
  if (measurable) {
    std::memset(low, kPaint, kPaintBytes - 256);
  }

  GRX_Match * match = nullptr;
  if (grx_match_create(job->regex, nullptr, &match) != GRX_OK) {
    job->out.result = GRX_ERR_OOM;
    return nullptr;
  }

  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.engine = job->engine;
  options.limits = job->limits;

  job->out.result = grx_regex_search_ex(
      job->regex, job->subject, job->length, &options, match, &job->out.matched);

  grx_match_destroy(match);

  if (!measurable) {
    return nullptr;
  }
  size_t i = 0;
  while (i < kPaintBytes - 256 && low[i] == static_cast<char>(kPaint)) {
    i++;
  }
  job->out.saturated = (i == 0);
  job->out.bytes = static_cast<size_t>(top - (low + i));
  job->out.measured = true;
  return nullptr;
}

/** Run one search on a fresh thread and report what it cost. */
Measurement measure(const GRX_Regex * regex, const std::string & subject,
    GRX_Engine engine, const GRX_Limits * limits = nullptr) {
  Job job{regex, subject.data(), subject.size(), engine, limits, {}};
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, kThreadStack);
  pthread_t thread;
  int started = pthread_create(&thread, &attributes, measure_on_thread, &job);
  pthread_attr_destroy(&attributes);
  if (started != 0) {
    job.out.result = GRX_ERR_INTERNAL;
    return job.out;
  }
  pthread_join(thread, nullptr);
  return job.out;
}

/**
 * A pattern taken as far as codegen, and no further.
 *
 * The parser recurses over nesting too - the file comment above says 480
 * levels is about all a 256 KB stack takes - so a figure covering a whole
 * compile could not say which pass moved. These three stages run on the
 * caller's own thread and the painted thread runs the fourth by itself,
 * which is what makes the number below codegen's.
 */
struct Lowered {
  GRX_Pattern * pattern = nullptr;
  GRX_IR * ir = nullptr;
  GRX_Facts facts{};
  GRX_Result result = GRX_OK;
};

Lowered lower(const std::string & text, GRX_Syntax syntax,
    const GRX_Limits & limits) {
  Lowered out;
  const GRX_Allocator * allocator = grx_allocator_default();
  out.result = grx_parse_pattern(text.data(), text.size(), syntax,
      GRX_OPT_NONE, &limits, allocator, nullptr, &out.pattern);
  if (out.result != GRX_OK) {
    return out;
  }
  out.result
      = grx_lower_pattern(out.pattern, &limits, allocator, nullptr, &out.ir);
  if (out.result != GRX_OK) {
    return out;
  }
  grx_facts_init(&out.facts);
  out.result = grx_analyze_ir(out.ir, &out.facts);
  return out;
}

void release(Lowered & held) {
  grx_ir_free(held.ir);
  grx_pattern_free(held.pattern);
  held.ir = nullptr;
  held.pattern = nullptr;
}

struct CodegenJob {
  const GRX_IR * ir;
  const GRX_Limits * limits;
  GRX_Program * program;
  Measurement out;
};

void * codegen_on_thread(void * argument) {
  CodegenJob * job = static_cast<CodegenJob *>(argument);
  char * top = frame_address();
  char * low = top - kCodegenPaintBytes;
  const bool measurable = stack_measurable();
  if (measurable) {
    std::memset(low, kPaint, kCodegenPaintBytes - 256);
  }

  job->out.result
      = grx_codegen_program(job->ir, job->limits, nullptr, job->program);

  if (!measurable) {
    return nullptr;
  }
  size_t i = 0;
  while (i < kCodegenPaintBytes - 256
      && low[i] == static_cast<char>(kPaint)) {
    i++;
  }
  job->out.saturated = (i == 0);
  job->out.bytes = static_cast<size_t>(top - (low + i));
  job->out.measured = true;
  return nullptr;
}

/**
 * What grx_ir_can_match_empty() costs on the same IR, measured the same way.
 *
 * Codegen asks it about every repeat body, so its recursion is inside every
 * codegen measurement of a pattern that repeats anything. Subtracting it is
 * what leaves codegen's own figure, and a test that did not subtract it would
 * report the analysis cap's size as though codegen had spent it.
 */
void * analysis_on_thread(void * argument) {
  CodegenJob * job = static_cast<CodegenJob *>(argument);
  char * top = frame_address();
  char * low = top - kCodegenPaintBytes;
  const bool measurable = stack_measurable();
  if (measurable) {
    std::memset(low, kPaint, kCodegenPaintBytes - 256);
  }

  (void)grx_ir_can_match_empty(job->ir, job->ir->root);

  if (!measurable) {
    return nullptr;
  }
  size_t i = 0;
  while (i < kCodegenPaintBytes - 256
      && low[i] == static_cast<char>(kPaint)) {
    i++;
  }
  job->out.saturated = (i == 0);
  job->out.bytes = static_cast<size_t>(top - (low + i));
  job->out.measured = true;
  return nullptr;
}

/** Generate one program on a fresh thread and report what codegen cost. */
Measurement measure_codegen(const GRX_IR * ir, const GRX_Limits * limits,
    void * (*what)(void *) = codegen_on_thread) {
  GRX_Program program;
  grx_program_init(&program, grx_allocator_default(), limits);
  CodegenJob job{ir, limits, &program, {}};
  pthread_attr_t attributes;
  pthread_attr_init(&attributes);
  pthread_attr_setstacksize(&attributes, kCodegenThreadStack);
  pthread_t thread;
  int started = pthread_create(&thread, &attributes, what, &job);
  pthread_attr_destroy(&attributes);
  if (started != 0) {
    job.out.result = GRX_ERR_INTERNAL;
  }
  else {
    pthread_join(thread, nullptr);
  }
  grx_program_clear(&program);
  return job.out;
}

/** What codegen cost for one pattern, or a test failure saying why not. */
Measurement codegen_cost(const std::string & pattern, GRX_Syntax syntax,
    const GRX_Limits & limits) {
  Lowered held = lower(pattern, syntax, limits);
  if (held.result != GRX_OK) {
    ADD_FAILURE() << "lowering " << pattern.substr(0, 40) << "... returned "
                  << held.result
                  << "; this test is about codegen and needs an IR to give it";
    release(held);
    return {};
  }
  Measurement m = measure_codegen(held.ir, &limits);
  release(held);
  return m;
}

/** Limits that let a pattern nest as deeply as these tests ask. */
GRX_Limits deep_limits() {
  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_nesting_depth = 4096;
  limits.max_nodes = 0;
  limits.max_program_size = 0;
  limits.max_pattern_length = 0;
  return limits;
}

/** Compile, or fail the test saying which pattern and why. */
GRX_Regex * compile_or_fail(const std::string & pattern, GRX_Syntax syntax,
    const GRX_Limits * limits = nullptr) {
  GRX_Regex * regex = nullptr;
  GRX_Result result = grx_regex_compile_with_allocator(pattern.data(),
      pattern.size(), syntax, GRX_OPT_NONE, limits, nullptr, nullptr, &regex);
  if (result != GRX_OK) {
    ADD_FAILURE() << "compiling " << pattern.substr(0, 40) << "... returned "
                  << result;
    return nullptr;
  }
  return regex;
}

std::string repeated(const std::string & unit, size_t times) {
  std::string out;
  out.reserve(unit.size() * times);
  for (size_t i = 0; i < times; i++) {
    out += unit;
  }
  return out;
}

std::string nested(const std::string & open, size_t depth,
    const std::string & core, const std::string & close) {
  return repeated(open, depth) + core + repeated(close, depth);
}

/**
 * The subject lengths the flat-in-the-subject tests sweep.
 *
 * Four orders of magnitude. If any engine held a frame per byte, per code
 * point or per match attempt, 65536 would cost visibly more than 8.
 */
const std::vector<size_t> kSubjectLengths = {8, 64, 512, 4096, 65536};

/**
 * A pattern, and the engines that can run it.
 *
 * Only the backtracker takes an assertion or a backreference; the Pike VM
 * and the bit-state engine answer GRX_ERR_UNSUPPORTED and are not being
 * tested when they do, so the rows say which engines to ask rather than
 * treating a refusal as a pass.
 */
struct Shape {
  const char * name;
  const char * pattern;
  GRX_Syntax syntax;
  std::vector<GRX_Engine> engines;
};

const std::vector<Shape> & shapes() {
  static const std::vector<Shape> rows = {
      {"a literal and a star", "a*b", GRX_SYNTAX_ECMASCRIPT,
          {GRX_ENGINE_PIKE, GRX_ENGINE_BACKTRACK, GRX_ENGINE_BITSTATE}},
      {"alternation that has to be retried", "(a|aa|aaa)+b",
          GRX_SYNTAX_ECMASCRIPT,
          {GRX_ENGINE_PIKE, GRX_ENGINE_BACKTRACK, GRX_ENGINE_BITSTATE}},
      {"a lookahead", "(?=a*b)a*b", GRX_SYNTAX_ECMASCRIPT,
          {GRX_ENGINE_BACKTRACK}},
      {"a fixed-length lookbehind", "(?<=a)a*b", GRX_SYNTAX_ECMASCRIPT,
          {GRX_ENGINE_BACKTRACK}},
      // The one that could have grown with the subject and does not:
      // look_behind_forward() tries every start the body could have begun
      // at, and tries them in a loop rather than by recursing.
      {"a variable-length lookbehind", "(?<=a+)b", GRX_SYNTAX_ECMASCRIPT,
          {GRX_ENGINE_BACKTRACK}},
      {"a backreference", "(a)\\1*b", GRX_SYNTAX_ECMASCRIPT,
          {GRX_ENGINE_BACKTRACK}},
      // Leftmost-longest keeps searching after a match instead of stopping,
      // which is the mode most likely to have grown a frame per attempt.
      {"leftmost-longest", "a*b", GRX_SYNTAX_POSIX_ERE,
          {GRX_ENGINE_PIKE, GRX_ENGINE_BACKTRACK}},
  };
  return rows;
}

const char * engine_name(GRX_Engine engine) {
  switch (engine) {
    case GRX_ENGINE_PIKE: return "pike";
    case GRX_ENGINE_BACKTRACK: return "backtrack";
    case GRX_ENGINE_BITSTATE: return "bitstate";
    default: return "auto";
  }
}

} // namespace

/**
 * Invariant 6, the half that holds: a longer subject is not a deeper stack.
 *
 * The engines keep their own stacks on the heap and step a program in a
 * loop, so the C frame is the same whether the subject is eight bytes or
 * sixty-four kilobytes. This sweeps four orders of magnitude and requires
 * the high-water mark to be *identical*, not merely bounded - an engine that
 * grew by one frame per thousand bytes would still look bounded at any one
 * size, and identical is what "does not depend on" means.
 *
 * A limit reached on the longer subjects is not a failure of this test: the
 * backtracker refuses `(a|aa|aaa)+b` against 65536 bytes long before it runs
 * out of stack, and what is being measured is the frame, not the verdict.
 * Rows that end in GRX_ERR_LIMIT are simply not compared.
 */
TEST(Stack, DepthDoesNotDependOnTheSubject) {
  SKIP_UNLESS_MEASURABLE();
  for (const Shape & shape : shapes()) {
    GRX_Regex * regex = compile_or_fail(shape.pattern, shape.syntax);
    ASSERT_NE(regex, nullptr) << shape.name;

    for (GRX_Engine engine : shape.engines) {
      size_t smallest = 0;
      size_t largest = 0;
      size_t measured = 0;
      for (size_t length : kSubjectLengths) {
        const std::string subject(length, 'a');
        Measurement m = measure(regex, subject, engine);
        ASSERT_FALSE(m.saturated)
            << shape.name << " on " << engine_name(engine) << ": the "
            << kPaintBytes << " bytes painted were not enough";
        if (m.result == GRX_ERR_LIMIT) {
          continue; // A verdict about steps, not about the frame.
        }
        ASSERT_EQ(m.result, GRX_OK)
            << shape.name << " on " << engine_name(engine) << " at " << length;
        measured++;
        if (smallest == 0 || m.bytes < smallest) {
          smallest = m.bytes;
        }
        if (m.bytes > largest) {
          largest = m.bytes;
        }
      }
      ASSERT_GT(measured, 1u)
          << shape.name << " on " << engine_name(engine)
          << ": fewer than two lengths ran, so nothing was compared";
      EXPECT_LT(largest - smallest, kFrameJitter)
          << shape.name << " on " << engine_name(engine) << ": between "
          << smallest << " and " << largest
          << " bytes of stack across subjects from " << kSubjectLengths.front()
          << " to " << kSubjectLengths.back()
          << " bytes. The engines step a program in a loop with their own "
             "stack on the heap; a spread this wide means one of them "
             "recursed over the subject.";
    }
    grx_regex_free(regex);
  }
}

/**
 * Invariant 6, the other half that holds: a bigger program is not a deeper
 * stack, as long as it is bigger and not deeper.
 *
 * `(?:a|b)` two hundred and fifty-six times over is a program two orders of
 * magnitude larger than one copy, and flat: every branch is at the same
 * nesting level. The frame does not move. Nesting is the axis that matters,
 * and it has its own test.
 */
TEST(Stack, DepthDoesNotDependOnTheSizeOfTheProgram) {
  SKIP_UNLESS_MEASURABLE();
  const std::vector<size_t> counts = {1, 4, 16, 64, 256};
  for (GRX_Engine engine : {GRX_ENGINE_PIKE, GRX_ENGINE_BACKTRACK}) {
    size_t smallest = 0;
    size_t largest = 0;
    for (size_t count : counts) {
      const std::string pattern = repeated("(?:a|b)", count);
      const std::string subject(count, 'a');
      GRX_Regex * regex = compile_or_fail(pattern, GRX_SYNTAX_ECMASCRIPT);
      ASSERT_NE(regex, nullptr);
      Measurement m = measure(regex, subject, engine);
      grx_regex_free(regex);
      ASSERT_FALSE(m.saturated);
      ASSERT_EQ(m.result, GRX_OK) << engine_name(engine) << " at " << count;
      if (smallest == 0 || m.bytes < smallest) {
        smallest = m.bytes;
      }
      if (m.bytes > largest) {
        largest = m.bytes;
      }
    }
    EXPECT_LT(largest - smallest, kFrameJitter)
        << engine_name(engine) << ": between " << smallest << " and "
        << largest << " bytes of stack for programs from " << counts.front()
        << " to " << counts.back() << " branches";
  }
}

/**
 * Nesting that is not an assertion does not recurse either.
 *
 * `(?:` ... `)` a hundred and twenty-eight deep is as deep as the default
 * `max_nesting_depth` allows, and costs the engine nothing: a group is a
 * bracket in the parser and disappears in lowering, so by the time a program
 * exists there is no group left to recurse over. This is here to say that
 * the next test is about assertions specifically, and not about depth.
 */
TEST(Stack, NestedGroupsDoNotRecurse) {
  SKIP_UNLESS_MEASURABLE();
  const std::vector<size_t> depths = {1, 8, 64, 128};
  size_t smallest = 0;
  size_t largest = 0;
  for (size_t depth : depths) {
    const std::string pattern = nested("(?:", depth, "a", ")");
    GRX_Regex * regex = compile_or_fail(pattern, GRX_SYNTAX_ECMASCRIPT);
    ASSERT_NE(regex, nullptr);
    Measurement m = measure(regex, "a", GRX_ENGINE_BACKTRACK);
    grx_regex_free(regex);
    ASSERT_EQ(m.result, GRX_OK) << "depth " << depth;
    if (smallest == 0 || m.bytes < smallest) {
      smallest = m.bytes;
    }
    if (m.bytes > largest) {
      largest = m.bytes;
    }
  }
  EXPECT_LT(largest - smallest, kFrameJitter)
      << "between " << smallest << " and " << largest
      << " bytes of stack for groups nested " << depths.front() << " to "
      << depths.back() << " deep";
}

/**
 * The half of invariant 6 that does not hold, stated as what is true.
 *
 * A nested assertion is a C frame in the backtracker, so the stack grows
 * linearly with assertion nesting. This measures the slope and requires it
 * to be linear and small: doubling the nesting roughly doubles the cost, and
 * a level costs somewhere between 64 bytes and 4 KB. The bounds are wide
 * because a frame's size is the compiler's business - the point is that the
 * relationship is a line through a per-level cost, not that the cost is any
 * particular number.
 *
 * If a future change makes the engine iterative here, this test fails by
 * finding the cost flat. That is the right way for it to fail: the invariant
 * would then hold as design.md originally stated it, and both this test and
 * section 9 should be rewritten to say so.
 */
TEST(Stack, NestedAssertionsAreWhatGrowsTheStack) {
  SKIP_UNLESS_MEASURABLE();
  const std::vector<size_t> depths = {8, 16, 32, 64, 128};
  std::vector<size_t> costs;
  size_t base = 0;

  for (size_t depth : depths) {
    const std::string pattern = nested("(?=", depth, "a", ")");
    GRX_Regex * regex = compile_or_fail(pattern, GRX_SYNTAX_ECMASCRIPT);
    ASSERT_NE(regex, nullptr) << "depth " << depth;
    Measurement m = measure(regex, "a", GRX_ENGINE_BACKTRACK);
    grx_regex_free(regex);
    ASSERT_FALSE(m.saturated) << "depth " << depth;
    ASSERT_EQ(m.result, GRX_OK) << "depth " << depth
        << ": the default limits have to be able to run what they compile";
    ASSERT_EQ(m.matched, 1) << "depth " << depth;
    costs.push_back(m.bytes);
    if (base == 0) {
      base = m.bytes;
    }
  }

  // Strictly increasing: every added level costs something.
  for (size_t i = 1; i < costs.size(); i++) {
    EXPECT_GT(costs[i], costs[i - 1])
        << "nesting " << depths[i] << " cost no more than " << depths[i - 1]
        << "; if the engine stopped recursing here, say so in design.md "
           "section 9 and in this file rather than deleting the test";
  }

  // And the slope is a per-level cost inside sane bounds.
  const size_t span_bytes = costs.back() - costs.front();
  const size_t span_levels = depths.back() - depths.front();
  const double per_level = static_cast<double>(span_bytes) / span_levels;
  EXPECT_GT(per_level, 64.0) << "a level costing less than a frame suggests "
                                "the measurement, not the engine";
  EXPECT_LT(per_level, 4096.0)
      << "a level costs " << per_level
      << " bytes, which puts GRX_BACKTRACK_MAX_C_DEPTH over its stack budget";
}

/**
 * Nesting past what the C stack can hold is refused, not fatal.
 *
 * `max_nesting_depth` is the caller's to raise and the parser will take 480
 * levels on a 256 KB stack. The matcher will not: at roughly 600 bytes a
 * level that is 241 KB, and before GRX_BACKTRACK_MAX_C_DEPTH existed it was
 * a segmentation fault inside grx_regex_search_ex() with nothing returned
 * and nothing logged.
 *
 * Two things are required here. The call comes back, with GRX_ERR_LIMIT -
 * which is what design.md section 5 says a bound does. And the stack it used
 * getting there is bounded: the cap is on the engine's recursion, so the
 * high-water mark must not move when the pattern nests twice as deep again.
 * A test that only checked the return code would pass against a cap applied
 * after the damage.
 */
TEST(Stack, NestingBeyondWhatTheStackHoldsIsALimitAndNotACrash) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_nesting_depth = 4096;
  limits.max_nodes = 0;
  limits.max_program_size = 0;
  limits.max_pattern_length = 0;

  size_t reference = 0;
  for (size_t depth : {480u, 960u, 1920u}) {
    const std::string pattern = nested("(?=", depth, "a", ")");
    GRX_Regex * regex = compile_or_fail(pattern, GRX_SYNTAX_ECMASCRIPT, &limits);
    ASSERT_NE(regex, nullptr) << "depth " << depth
        << ": raising max_nesting_depth has to be enough to compile it, or "
           "this test is measuring the parser";

    Measurement m = measure(regex, "a", GRX_ENGINE_BACKTRACK, &limits);
    grx_regex_free(regex);

    EXPECT_EQ(m.result, GRX_ERR_LIMIT)
        << "depth " << depth
        << ": deeper than the engine can recurse has to come back as a "
           "limit";
    ASSERT_FALSE(m.saturated) << "depth " << depth;
    if (!m.measured) {
      continue;
    }
    if (reference == 0) {
      reference = m.bytes;
      continue;
    }
    const size_t difference
        = m.bytes > reference ? m.bytes - reference : reference - m.bytes;
    EXPECT_LT(difference, kFrameJitter)
        << "depth " << depth << " used " << m.bytes
        << " bytes of stack where 480 used " << reference
        << "; the cap has to stop the recursion, not report it afterwards";
  }

  // And the cap itself has to fit the smallest stack the library claims:
  // testing.md section 12 runs every harness under 256 KB.
  if (!stack_measurable()) {
    return;
  }
  EXPECT_LT(reference, 128u * 1024u)
      << "the engine's own recursion peaks at " << reference
      << " bytes, which is over half the 256 KB stack the harnesses run "
         "under and leaves the caller too little";
}

/**
 * What the defaults can compile, the defaults can run.
 *
 * The bound above must never be the tighter of the two, or a pattern would
 * compile at the default limits and then fail to match - which is the
 * failure mode a first draft of GRX_BACKTRACK_MAX_C_DEPTH had, at exactly
 * the depth the defaults allow.
 */
TEST(Stack, TheDeepestAssertionTheDefaultsCompileStillMatches) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  ASSERT_GT(limits.max_nesting_depth, 0u);

  const std::string pattern
      = nested("(?=", limits.max_nesting_depth, "a", ")");
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile(pattern.c_str(), GRX_SYNTAX_ECMASCRIPT,
                GRX_OPT_NONE, &regex),
      GRX_OK)
      << "the defaults have to compile a pattern nested exactly "
      << limits.max_nesting_depth << " deep";

  Measurement m = measure(regex, "a", GRX_ENGINE_BACKTRACK);
  grx_regex_free(regex);

  EXPECT_EQ(m.result, GRX_OK)
      << "a pattern nested to max_nesting_depth compiled and then would not "
         "run: GRX_BACKTRACK_MAX_C_DEPTH is below what the defaults allow";
  EXPECT_EQ(m.matched, 1);
}

/**
 * Codegen does not recurse over nesting.
 *
 * The pass that lays an IR out as instructions used to call itself for every
 * child, about four frames to a level with no bound of any kind, and a fuzz
 * campaign found it at 106 levels: a stack overflow inside
 * grx_regex_compile() with nothing returned and nothing logged, which is the
 * same failure the matcher had before GRX_BACKTRACK_MAX_C_DEPTH and was
 * invisible here because this file pointed its ruler only at a *match*.
 *
 * A depth cap cannot be the answer: NestingBeyondWhatTheStackHoldsIsALimit
 * below requires 1920 levels to compile, and the crash was at 106, so no
 * constant sits between them. gen() keeps its stack on the heap instead, and
 * what that buys is measured here rather than reviewed - the high-water mark
 * must not move across a sweep that varies the nesting by a factor of 240.
 *
 * A lookahead is the shape to sweep because it reaches nothing else: codegen
 * asks the analysis pass about a *repeat* body, and that recursion has a cap
 * of its own with its own test below. Here the only thing that could grow is
 * codegen.
 */
TEST(Stack, GeneratingCodeDoesNotRecurseOverNesting) {
  SKIP_UNLESS_MEASURABLE();
  const GRX_Limits limits = deep_limits();
  const std::vector<size_t> depths = {8, 64, 128, 480, 960, 1920};

  size_t smallest = 0;
  size_t largest = 0;
  for (size_t depth : depths) {
    const std::string pattern = nested("(?=", depth, "a", ")");
    Measurement m = codegen_cost(pattern, GRX_SYNTAX_ECMASCRIPT, limits);
    ASSERT_TRUE(m.measured) << "depth " << depth;
    ASSERT_FALSE(m.saturated) << "depth " << depth << ": the "
                              << kCodegenPaintBytes
                              << " bytes painted were not enough";
    ASSERT_EQ(m.result, GRX_OK) << "depth " << depth;
    if (smallest == 0 || m.bytes < smallest) {
      smallest = m.bytes;
    }
    if (m.bytes > largest) {
      largest = m.bytes;
    }
  }

  EXPECT_LT(largest - smallest, kFrameJitter)
      << "codegen used between " << smallest << " and " << largest
      << " bytes of stack for patterns nested " << depths.front() << " to "
      << depths.back() << " deep. gen() keeps its own stack on the heap; a "
         "spread this wide means it recursed over the tree again";
}

/**
 * And codegen costs the same whatever the analysis it re-enters costs.
 *
 * The other recursion on this stack. gen_repeat_step() asks
 * grx_ir_can_match_empty() about its body, so a nested repeat puts walk() in
 * analyze.c underneath codegen and the two share one stack - which is what
 * made the campaign's crash hard to read, and why lowering
 * GRX_ANALYSIS_MAX_DEPTH to 32 left two of the three artifacts crashing.
 *
 * So the figure to pin is the difference, not the sum. Measuring the analysis
 * call on its own and subtracting it leaves codegen's own frames, and those
 * must not grow: measured here they are 4432 bytes from eight levels to
 * nineteen hundred, against a sum that runs from 9 KB to 130 KB.
 *
 * What the sum does say is where the next bound goes. The analysis pass is
 * 246 bytes a level and stops only at GRX_ANALYSIS_MAX_DEPTH, so at a raised
 * max_nesting_depth it wants 126 KB of the 256 KB testing.md section 12
 * allows - the same shape of problem GRX_BACKTRACK_MAX_C_DEPTH was written
 * for, a frame count standing in for a byte budget. That is recorded rather
 * than asserted here, because this file is about codegen and a bound that
 * cleared 126 KB by 641 bytes would be a tolerance, not a gate.
 */
TEST(Stack, GeneratingCodeCostsTheSameWhateverTheAnalysisItReentersCosts) {
  SKIP_UNLESS_MEASURABLE();
  const GRX_Limits limits = deep_limits();

  size_t smallest = 0;
  size_t largest = 0;
  for (size_t depth : {8u, 128u, 512u, 1024u, 1920u}) {
    // Unbounded rather than counted, because a counted repeat is laid out by
    // expansion and `(?:X){1,2}` nested 1920 deep is 2^1920 instructions -
    // which measures max_program_size, and with that raised measures nothing
    // at all before the heat death of the sun.
    const std::string pattern = nested("(?:", depth, "a", ")*");
    Lowered held = lower(pattern, GRX_SYNTAX_ECMASCRIPT, limits);
    ASSERT_EQ(held.result, GRX_OK) << "depth " << depth;

    Measurement both = measure_codegen(held.ir, &limits);
    Measurement analysis
        = measure_codegen(held.ir, &limits, analysis_on_thread);
    release(held);

    ASSERT_TRUE(both.measured && analysis.measured) << "depth " << depth;
    ASSERT_FALSE(both.saturated || analysis.saturated) << "depth " << depth;
    ASSERT_EQ(both.result, GRX_OK) << "depth " << depth;
    ASSERT_GT(both.bytes, analysis.bytes)
        << "depth " << depth
        << ": codegen measured less than the analysis call it makes, so one "
           "of the two measurements is not of what it says";

    const size_t own = both.bytes - analysis.bytes;
    if (smallest == 0 || own < smallest) {
      smallest = own;
    }
    if (own > largest) {
      largest = own;
    }
  }

  EXPECT_LT(largest - smallest, kFrameJitter)
      << "codegen's own frames took between " << smallest << " and " << largest
      << " bytes over a repeat nested 8 to 1920 deep; gen() keeps its stack "
         "on the heap, so this is flat or it is recursing again";
  EXPECT_LT(largest, kHarnessBudget)
      << "codegen's own frames took " << largest << " bytes";
}

/**
 * The deepest thing these tests compile, compiled where a harness would.
 *
 * NestingBeyondWhatTheStackHoldsIsALimitAndNotACrash compiles 1920 levels on
 * the test runner's own thread, which is eight megabytes and says nothing
 * about a harness. This runs the same generation on a painted thread and
 * requires the figure, not merely a return code: the crash the campaign
 * found came back as no code at all.
 */
TEST(Stack, TheDeepestPatternTheseTestsCompileGeneratesWithinBudget) {
  SKIP_UNLESS_MEASURABLE();
  const GRX_Limits limits = deep_limits();
  const std::string pattern = nested("(?=", 1920, "a", ")");
  Measurement m = codegen_cost(pattern, GRX_SYNTAX_ECMASCRIPT, limits);
  ASSERT_TRUE(m.measured);
  ASSERT_FALSE(m.saturated);
  EXPECT_EQ(m.result, GRX_OK);
  EXPECT_LT(m.bytes, kHarnessBudget)
      << "generating code for 1920 nested assertions took " << m.bytes
      << " bytes of stack";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
