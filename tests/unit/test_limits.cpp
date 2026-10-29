/**
 * @file
 *
 * The defaults, against the patterns that have to keep working.
 *
 * Every field of GRX_Limits is a promise, and a promise is only worth making
 * if somebody has checked what real patterns actually need. WP-14's method
 * is two corpora pulling in opposite directions: patterns that must compile
 * and run at the defaults, and pairs that must be refused by them. The
 * second half is tests/conformance/test_redos.cpp. This is the first.
 *
 * `tools/limits/measure.py` reports the whole distribution - median, p99,
 * maximum and the headroom over each default - and is what the numbers in
 * documentation/dialects.md section 7 came from. What is here is the part
 * that has to keep being true: not "the headroom is 7x" but "nothing in the
 * corpus is refused", which is the claim a future default would break.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

#include "test_helpers.h"

#include "../../src/exec/exec_internal.h"
#include "../conformance/rxt.h"

namespace {

/** Physical allocation sizes calibrate the discovery budget on each ABI. */
class ScratchAllocationMeter {
  union Header {
    size_t size;
    std::max_align_t alignment;
  };
  GRX_Allocator allocator_{};
  size_t live_ = 0;
  size_t baseline_ = 0;
  size_t peak_ = 0;
  bool measuring_ = false;
  struct StackAllocation {
    void * pointer;
    size_t id;
  };
  struct Event {
    size_t live, stack, size;
  };
  std::vector<StackAllocation> stacks_;
  std::vector<Event> events_;
  size_t next_stack_ = 0;
  size_t root_stack_id_ = 0;

  size_t stack_id(void * pointer) const {
    for (const auto & stack : stacks_)
      if (stack.pointer == pointer)
        return stack.id;
    return 0;
  }

  void * resize(void * ptr, size_t size, bool stack_candidate) {
    if (size > static_cast<size_t>(-1) - sizeof(Header)) {
      return nullptr;
    }
    Header * old = ptr ? static_cast<Header *>(ptr) - 1 : nullptr;
    const size_t prior = old ? old->size : 0;
    const size_t id = measuring_ && stack_candidate
        ? (ptr ? stack_id(ptr) : ++next_stack_)
        : 0;
    Header * block = static_cast<Header *>(
        std::realloc(old, sizeof(Header) + (size ? size : 1)));
    if (!block) {
      return nullptr;
    }
    block->size = size;
    live_ = live_ - prior + size;
    if (measuring_) {
      if (id) {
        if (!ptr)
          stacks_.push_back({block + 1, id});
        else
          for (auto & stack : stacks_)
            if (stack.id == id)
              stack.pointer = block + 1;
      }
      events_.push_back({live_, id, size});
      if (live_ > peak_) {
        peak_ = live_;
      }
    }
    return block + 1;
  }

public:
  ScratchAllocationMeter() {
    allocator_.ctx = this;
    allocator_.malloc_fn = [](void * ctx, size_t size) -> void * {
      return static_cast<ScratchAllocationMeter *>(ctx)->resize(
          nullptr, size, false);
    };
    allocator_.calloc_fn = [](void * ctx, size_t count, size_t size) -> void * {
      if (count && size > static_cast<size_t>(-1) / count) {
        return nullptr;
      }
      void * ptr = static_cast<ScratchAllocationMeter *>(ctx)->resize(
          nullptr, count * size, false);
      if (ptr) {
        std::memset(ptr, 0, count * size);
      }
      return ptr;
    };
    allocator_.realloc_fn = [](void * ctx, void * ptr, size_t size) -> void * {
      return static_cast<ScratchAllocationMeter *>(ctx)->resize(
          ptr, size, true);
    };
    allocator_.free_fn = [](void * ctx, void * ptr) {
      if (ptr) {
        auto * self = static_cast<ScratchAllocationMeter *>(ctx);
        Header * block = static_cast<Header *>(ptr) - 1;
        self->live_ -= block->size;
        if (self->measuring_) {
          const size_t id = self->stack_id(ptr);
          if (id) {
            // Query stacks retire before their caller. For these capture/
            // assertion programs the final realloc-backed buffer is the
            // ordinary root stack, also included in the aggregate scratch peak.
            self->root_stack_id_ = id;
            for (auto & stack : self->stacks_)
              if (stack.id == id)
                stack.pointer = nullptr;
          }
          self->events_.push_back({self->live_, id, 0});
        }
        std::free(block);
      }
    };
  }
  const GRX_Allocator * get() const {
    return &allocator_;
  }
  void begin() {
    baseline_ = peak_ = live_;
    next_stack_ = root_stack_id_ = 0;
    stacks_.clear();
    events_.clear();
    measuring_ = true;
  }
  size_t peak() const {
    return peak_ - baseline_;
  }
  size_t root_stack() const {
    size_t bytes = 0;
    for (const auto & event : events_)
      if (event.stack && event.stack == root_stack_id_)
        bytes = std::max(bytes, event.size);
    return bytes;
  }
  size_t charged_peak() const {
    // Every requested live scratch byte participates, including root frames.
    return peak();
  }
  size_t live() const {
    return live_;
  }
};

struct Row {
  std::string flags;
  std::string pattern;
  std::string source;
};

uint32_t options_for(const std::string & flags) {
  uint32_t options = 0;
  for (char f : flags) {
    switch (f) {
      case 'i':
        options |= GRX_OPT_CASELESS;
        break;
      case 'm':
        options |= GRX_OPT_MULTILINE;
        break;
      case 's':
        options |= GRX_OPT_DOTALL;
        break;
      case 'u':
        options |= GRX_OPT_UTF;
        break;
      case 'v':
        options |= GRX_OPT_UNICODE_SETS | GRX_OPT_UTF;
        break;
      default:
        break;
    }
  }
  return options;
}

/** `tools/limits/real_world.txt`: flags, a tab, the pattern. */
std::vector<Row> read_real_world() {
  std::vector<Row> rows;
  std::ifstream file(grxtest::repo("tools/limits/real_world.txt"));
  std::string line;
  while (std::getline(file, line)) {
    if (line.empty() || line[0] == '#') {
      continue;
    }
    size_t tab = line.find('\t');
    if (tab == std::string::npos) {
      continue;
    }
    rows.push_back({line.substr(0, tab), line.substr(tab + 1),
        "tools/limits/real_world.txt"});
  }
  return rows;
}

} // namespace

TEST(Limits, EveryRealWorldPatternCompilesAtTheDefaults) {
  // The patterns that appear in JSON Schemas and configuration files. A
  // default below what one of these needs is a default that would reject
  // working software, which is the failure this corpus exists to prevent.
  std::vector<Row> rows = read_real_world();
  ASSERT_GT(rows.size(), 20u)
      << "the corpus was not found or is too small to be measuring anything";

  GRX_Limits limits;
  grx_limits_default(&limits);

  for (const Row & row : rows) {
    GRX_Error error;
    grx_error_clear(&error);
    GRX_Regex * regex = nullptr;
    GRX_Result result = grx_regex_compile_with_allocator(row.pattern.data(),
        row.pattern.size(), GRX_SYNTAX_ECMASCRIPT, options_for(row.flags),
        &limits, nullptr, &error, &regex);
    EXPECT_EQ(result, GRX_OK)
        << "/" << row.pattern << "/" << row.flags << ": " << error.message;
    grx_regex_free(regex);
  }
}

TEST(Limits, EveryConformanceVectorCompilesAtTheDefaults) {
  // The same claim over the vectors, which are a much larger and much
  // stranger corpus: they came from a generator aimed at the grammar's
  // corners rather than at what anybody would write.
  grxtest::VectorFile named;
  grxtest::VectorFile generated;
  std::string error;
  ASSERT_TRUE(grxtest::read_vector_file(
      grxtest::data("vectors/ecmascript/named.rxt"), &named, &error))
      << error;
  ASSERT_TRUE(grxtest::read_vector_file(
      grxtest::data("vectors/ecmascript/generated.rxt"), &generated, &error))
      << error;

  GRX_Limits limits;
  grx_limits_default(&limits);

  size_t compiled = 0;
  size_t refused_as_syntax = 0;
  for (const grxtest::VectorFile * file : {&named, &generated}) {
    for (const grxtest::Record & record : file->records) {
      GRX_Error compile_error;
      grx_error_clear(&compile_error);
      GRX_Regex * regex = nullptr;
      GRX_Result result = grx_regex_compile_with_allocator(
          record.pattern.data(), record.pattern.size(), record.syntax,
          record.options, &limits, nullptr, &compile_error, &regex);
      grx_regex_free(regex);

      if (result == GRX_OK) {
        compiled++;
        continue;
      }
      // A vector whose expectation is a syntax error is meant not to
      // compile. What must never happen is a *limit*.
      EXPECT_NE(result, GRX_ERR_LIMIT)
          << "/" << record.pattern << "/ hit " << compile_error.message;
      refused_as_syntax++;
    }
  }

  EXPECT_GT(compiled, 100u) << "almost nothing compiled; the corpus is wrong";
  printf("\nlimits: %zu vectors compiled at the defaults, %zu refused as "
         "syntax\n",
      compiled, refused_as_syntax);
}

TEST(Limits, NestingIsBoundedWellInsideTheSmallestStack) {
  // max_nesting_depth is the C-stack bound and the one limit design.md says
  // a caller should not lift casually. Measured rather than assumed: on the
  // 256 KB stack the fuzzers run under, a pattern nested 480 deep parses and
  // one nested 496 deep overflows - about 525 bytes of stack per level. The
  // default of 128 is therefore a factor of about four inside the smallest
  // stack this library claims to work on, and thirty inside the usual one.
  //
  // "Parses" was the whole of that measurement, and it was not the whole of
  // the cost. Matching a pattern whose nesting is *assertions* costs the
  // backtracker about the same per level again, so the 480 this comment
  // says the parser takes used to be a segmentation fault at match time;
  // tests/unit/test_stack.cpp measures that half and
  // GRX_BACKTRACK_MAX_C_DEPTH bounds it. The figures above still stand for
  // what they describe - they just never described both ends.
  //
  // What is checked here is the half that does not need a `ulimit`: that the
  // default is enforced exactly, one level either side of it.
  GRX_Limits limits;
  grx_limits_default(&limits);
  ASSERT_GT(limits.max_nesting_depth, 0u);

  const std::string at(limits.max_nesting_depth, '(');
  const std::string close(limits.max_nesting_depth, ')');
  GRX_Pattern * parsed = nullptr;
  GRX_Error error;
  grx_error_clear(&error);

  std::string ok = at + "a" + close;
  EXPECT_EQ(grx_pattern_parse_with_allocator(ok.data(), ok.size(),
                GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, &limits, nullptr, &error,
                &parsed),
      GRX_OK)
      << error.message;
  grx_pattern_free(parsed);
  parsed = nullptr;

  std::string over = at + "(a)" + close;
  EXPECT_EQ(grx_pattern_parse_with_allocator(over.data(), over.size(),
                GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, &limits, nullptr, &error,
                &parsed),
      GRX_ERR_LIMIT);
  EXPECT_EQ(error.diag, GRX_DIAG_LIMIT_NESTING_DEPTH);
  grx_pattern_free(parsed);
}

TEST(Limits, TheCostliestRealPatternIsWellInsideTheProgramLimit) {
  // `\p{RGI_Emoji}` is the largest thing ECMAScript can name: nearly four
  // thousand sequences, which lower to an alternation. It is the pattern
  // that decides max_nodes and max_program_size, and the headroom it leaves
  // is the headroom those two have.
  GRX_Limits limits;
  grx_limits_default(&limits);

  GRX_Regex * regex = nullptr;
  GRX_Error error;
  grx_error_clear(&error);
  const char * pattern = "^\\p{RGI_Emoji}+$";
  ASSERT_EQ(grx_regex_compile_with_allocator(pattern, strlen(pattern),
                GRX_SYNTAX_ECMASCRIPT,
                GRX_OPT_UTF | GRX_OPT_UNICODE_SETS, &limits, nullptr, &error,
                &regex),
      GRX_OK)
      << error.message;

  GRX_Facts facts;
  ASSERT_EQ(grx_regex_facts(regex, &facts), GRX_OK);
  EXPECT_LT(facts.program_size, limits.max_program_size);
  EXPECT_GT(facts.program_size, 10000u)
      << "it should be the costliest thing in the corpus; if it is not, the "
         "measurement in dialects.md section 7 is about a different pattern";
  printf("limits: /%s/v is %zu instructions against a cap of %zu\n", pattern,
      facts.program_size, limits.max_program_size);
  grx_regex_free(regex);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

namespace {

/**
 * One field of GRX_Limits, and an input that exceeds a deliberately tight
 * value of it.
 *
 * The offset rather than a pointer-to-member so the table stays a table: the
 * test sets one field of an otherwise default structure, which is how a
 * caller tunes one.
 */
struct Tunable {
  const char * name;
  size_t offset;
  size_t tight;      ///< A value the input below exceeds.
  const char * pattern;
  const char * subject;
  GRX_Engine engine; ///< The one that enforces it, where that matters.
  GRX_Diag diag;     ///< GRX_DIAG_NONE for the match-time limits.
};

#define LIMIT_FIELD(name) offsetof(GRX_Limits, name)

const Tunable kTunables[] = {
  {"max_pattern_length", LIMIT_FIELD(max_pattern_length), 4, "aaaaa", "aaaaa",
      GRX_ENGINE_AUTO, GRX_DIAG_LIMIT_PATTERN_LENGTH},
  {"max_nesting_depth", LIMIT_FIELD(max_nesting_depth), 2, "((((a))))", "a",
      GRX_ENGINE_AUTO, GRX_DIAG_LIMIT_NESTING_DEPTH},
  {"max_nodes", LIMIT_FIELD(max_nodes), 4, "abcdefgh", "abcdefgh",
      GRX_ENGINE_AUTO, GRX_DIAG_LIMIT_NODES},
  {"max_program_size", LIMIT_FIELD(max_program_size), 4, "abcdefgh",
      "abcdefgh", GRX_ENGINE_AUTO, GRX_DIAG_LIMIT_PROGRAM_SIZE},
  {"max_captures", LIMIT_FIELD(max_captures), 1, "(a)(b)", "ab",
      GRX_ENGINE_AUTO, GRX_DIAG_LIMIT_CAPTURES},
  {"max_repeat_count", LIMIT_FIELD(max_repeat_count), 4, "a{5}", "aaaaa",
      GRX_ENGINE_AUTO, GRX_DIAG_LIMIT_REPEAT_COUNT},
  {"max_class_ranges", LIMIT_FIELD(max_class_ranges), 1, "[a-cx-z]", "b",
      GRX_ENGINE_AUTO, GRX_DIAG_LIMIT_CLASS_RANGES},
  {"max_lookbehind_length", LIMIT_FIELD(max_lookbehind_length), 1,
      "(?<=abcd)x", "abcdx", GRX_ENGINE_BACKTRACK,
      GRX_DIAG_LIMIT_LOOKBEHIND_LENGTH},
  // The four match-time rows read GRX_DIAG_NONE until there was somewhere
  // for a match-time diagnostic to go; grx_match_error() is that place.
  {"max_steps", LIMIT_FIELD(max_steps), 1, "^a+$", "aaaaaaaa",
      GRX_ENGINE_AUTO, GRX_DIAG_LIMIT_STEPS},
  {"max_backtrack", LIMIT_FIELD(max_backtrack), 1, "^a+$", "aaaaaaaa",
      GRX_ENGINE_BACKTRACK, GRX_DIAG_LIMIT_BACKTRACK},
  {"max_match_memory", LIMIT_FIELD(max_match_memory), 1, "^a+$", "aaaaaaaa",
      GRX_ENGINE_AUTO, GRX_DIAG_LIMIT_MATCH_MEMORY},
  {"max_subject_length", LIMIT_FIELD(max_subject_length), 1, "^a+$",
      "aaaaaaaa", GRX_ENGINE_AUTO, GRX_DIAG_LIMIT_SUBJECT_LENGTH},
};

/** What one row does at one value of its field. */
struct Attempt {
  GRX_Result result;
  GRX_Diag diag;
  int matched;
};

Attempt run_with(const Tunable & row, size_t value) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  *(size_t *)((char *)&limits + row.offset) = value;

  Attempt attempt = {GRX_OK, GRX_DIAG_NONE, 0};
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  grx_error_clear(&error);
  attempt.result = grx_regex_compile_with_allocator(row.pattern,
      std::strlen(row.pattern), GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, &limits,
      nullptr, &error, &regex);
  if (attempt.result != GRX_OK) {
    attempt.diag = error.diag;
    return attempt;
  }

  GRX_Match * match = nullptr;
  grx_match_create(regex, nullptr, &match);
  attempt.result = grx_regex_search(regex, row.subject,
      std::strlen(row.subject), 0, row.engine, &limits, match,
      &attempt.matched);
  // A match-time failure has no offset into the pattern, so it travels on
  // the match rather than in a GRX_Error the caller passed in.
  const GRX_Error * match_error = grx_match_error(match);
  if (attempt.result != GRX_OK && match_error) {
    attempt.diag = match_error->diag;
  }
  grx_match_destroy(match);
  grx_regex_free(regex);
  return attempt;
}

} // namespace

/**
 * Every enforced limit refuses when it is tight, and caps nothing at zero.
 *
 * core.h says "zero means no limit for every field", and that sentence had
 * never been checked. It was also not quite true: `max_lookbehind_length`
 * and `max_recursion_depth` were in the structure and in the documentation
 * while nothing read either of them, so setting one to 1 was as unbounded as
 * setting it to 0. A limit a caller can set and cannot feel is worse than no
 * limit at all, because it is a defence they think they have.
 *
 * Each row also checks that the refusal names its *own* field. A caller who
 * has to raise one has to be told which, and a compile that reported
 * max_nodes when max_program_size was the binding limit would send them to
 * the wrong knob.
 */
TEST(Limits, EveryEnforcedLimitRefusesWhenTightAndCapsNothingAtZero) {
  for (const Tunable & row : kTunables) {
    Attempt tight = run_with(row, row.tight);
    EXPECT_EQ(tight.result, GRX_ERR_LIMIT)
        << row.name << " did not refuse at " << row.tight;
    if (row.diag != GRX_DIAG_NONE) {
      EXPECT_EQ(tight.diag, row.diag)
          << row.name << " reported a different field's diagnostic";
    }

    Attempt lifted = run_with(row, 0);
    EXPECT_EQ(lifted.result, GRX_OK) << row.name << " still capped at zero";
    EXPECT_TRUE(lifted.matched) << row.name << " did not match at zero";
  }
}

/**
 * A call is refused by max_program_size or not at all.
 *
 * Every CALL a program holds needs one entry in a list codegen keeps while
 * it patches them, and that list was a fixed array of a thousand and
 * twenty-four. `(a)(?2){0,1999}?(b)` - a pcre2test row, and one pcre2 itself
 * compiles into a single copy with a counter - has one CALL per expanded
 * iteration, so it overflowed the array and was refused with "program is
 * larger than max_program_size" over a program of some four thousand
 * instructions against a cap of two hundred thousand.
 *
 * Raising max_program_size did nothing, because max_program_size was not the
 * limit; nothing a caller could set was. A refusal that names a knob which
 * does not turn is worse than no limit, which is the sentence the test above
 * exists for, so this is the same rule applied to a limit that was hiding.
 */
TEST(Limits, ARepeatedSubroutineCallIsBoundedByTheProgramAndNothingElse) {
  GRX_Limits limits;
  grx_limits_default(&limits);

  GRX_Regex * regex = nullptr;
  GRX_Error error;
  grx_error_clear(&error);
  const char * pattern = "(a)(?2){0,1999}?(b)";
  ASSERT_EQ(grx_regex_compile_with_allocator(pattern, strlen(pattern),
                GRX_SYNTAX_PCRE, 0, &limits, nullptr, &error, &regex),
      GRX_OK)
      << error.message;

  GRX_Facts facts;
  grx_regex_facts(regex, &facts);
  EXPECT_LT(facts.program_size, limits.max_program_size);
  grx_regex_free(regex);

  // And the cap still binds: set it below what the pattern needs and the
  // refusal is the one the caller can act on.
  limits.max_program_size = 64;
  regex = nullptr;
  grx_error_clear(&error);
  EXPECT_EQ(grx_regex_compile_with_allocator(pattern, strlen(pattern),
                GRX_SYNTAX_PCRE, 0, &limits, nullptr, &error, &regex),
      GRX_ERR_LIMIT);
  EXPECT_EQ(error.diag, GRX_DIAG_LIMIT_PROGRAM_SIZE);
  grx_regex_free(regex);
}

/**
 * grx_limits_unlimited() caps nothing, field by field.
 *
 * Compared against the structure rather than against behaviour, because the
 * behaviour is the test above. What this pins is that a field added to
 * GRX_Limits later is also zeroed here - the failure mode being a new limit
 * that "unlimited" quietly still applies.
 */
TEST(Limits, UnlimitedIsEveryFieldZero) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  grx_limits_unlimited(&limits);

  const size_t * fields = (const size_t *)&limits;
  for (size_t i = 0; i < sizeof(GRX_Limits) / sizeof(size_t); i++) {
    EXPECT_EQ(fields[i], (size_t)0) << "field " << i << " of GRX_Limits";
  }

  // And it is a real function, not a memset the caller could have written:
  // NULL is ignored rather than crashing, like every other _default().
  grx_limits_unlimited(nullptr);
}

/**
 * An unbounded lookbehind is GRX_NPOS, not zero.
 *
 * `(?<=a+)x` used to report max_lookbehind == 0 - the same answer as a
 * pattern with no lookbehind at all - because the analysis skipped a body
 * whose length it could not bound. That is exactly backwards: the body it
 * could not bound is the one that may read the whole subject. It also made
 * max_lookbehind_length unenforceable, since the one pattern a caller would
 * want the limit to catch was the one reporting that it needed nothing.
 */
TEST(Limits, AnUnboundedLookbehindIsNotZero) {
  struct Row {
    const char * pattern;
    size_t expected;
  };
  const Row rows[] = {
    {"x", 0},
    {"(?<=abcd)x", 4},
    {"(?<=a|bcd)x", 3},
    {"(?<=a{3,7})x", 7},
    {"(?<!abc)x", 3},
    {"(?<=a+)x", GRX_NPOS},
    {"(?<=a*)x", GRX_NPOS},
    {"(?<=(?:ab)+)x", GRX_NPOS},
  };

  for (const Row & row : rows) {
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile(row.pattern, GRX_SYNTAX_ECMASCRIPT,
                  GRX_OPT_UTF, &regex),
        GRX_OK)
        << row.pattern;
    GRX_Facts facts;
    ASSERT_EQ(grx_regex_facts(regex, &facts), GRX_OK);
    EXPECT_EQ(facts.max_lookbehind, row.expected) << row.pattern;
    grx_regex_free(regex);
  }
}

/**
 * The default does not bound a lookbehind, because ECMAScript does not.
 *
 * Now that the field is enforced, a non-zero default would refuse `(?<=a+)x`
 * - valid ECMAScript, and a pattern the conformance corpus contains. The
 * limit is a caller's policy on top of the dialect; the default is to have
 * no policy.
 */
TEST(Limits, TheDefaultDoesNotBoundAnEcmascriptLookbehind) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  EXPECT_EQ(limits.max_lookbehind_length, (size_t)0);

  GRX_Regex * regex = nullptr;
  EXPECT_EQ(grx_regex_compile_with_allocator("(?<=a+)x", 8,
                GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, &limits, nullptr, nullptr,
                &regex),
      GRX_OK);
  grx_regex_free(regex);

  // A caller who *does* set one catches the unbounded body, which is the
  // whole reason to set it.
  limits.max_lookbehind_length = 255;
  regex = nullptr;
  GRX_Error error;
  grx_error_clear(&error);
  EXPECT_EQ(grx_regex_compile_with_allocator("(?<=a+)x", 8,
                GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, &limits, nullptr, &error,
                &regex),
      GRX_ERR_LIMIT);
  EXPECT_EQ(error.diag, GRX_DIAG_LIMIT_LOOKBEHIND_LENGTH);
  EXPECT_EQ(regex, nullptr);

  // And a bounded one under the cap still compiles, so the limit is a bound
  // rather than a ban on lookbehind.
  regex = nullptr;
  EXPECT_EQ(grx_regex_compile_with_allocator("(?<=abcd)x", 10,
                GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, &limits, nullptr, nullptr,
                &regex),
      GRX_OK);
  grx_regex_free(regex);
}

/**
 * max_recursion_depth is reserved, and this is what says so.
 *
 * It has a default of 256 and nothing reads it, because no dialect this
 * library implements has recursion or a subroutine call - those arrive with
 * Perl and PCRE2 in WP-18. Rather than leave that as a comment, this pins
 * it: if any dialect ever compiles one of these into a program that reports
 * has_recursion, the limit has to be enforced at the same time, and this
 * test fails until it is.
 */
TEST(Limits, RecursionDepthIsEnforcedNowThatADialectHasRecursion) {
  // This test used to assert the opposite: that no dialect compiled any of
  // these into recursion, so that `max_recursion_depth` was a documented
  // field nothing read. PCRE2's front end (work-packages.md WP-18) is what it
  // was waiting for, and the assertion is now the one it was standing in for -
  // that the limit refuses a recursion deeper than it allows, rather than
  // being spent on a pattern that never recurses.
  const char * recursive[] = {"(a(?R)?b)", "(a(?1)?b)"};

  for (const char * pattern : recursive) {
    GRX_Limits limits;
    grx_limits_default(&limits);
    limits.max_recursion_depth = 2;

    GRX_Regex * regex = nullptr;
    GRX_Error error;
    ASSERT_EQ(grx_regex_compile_with_allocator(pattern, std::strlen(pattern),
                  GRX_SYNTAX_PCRE, 0, &limits, nullptr, &error, &regex),
        GRX_OK)
        << pattern << ": " << error.message;

    GRX_Facts facts;
    ASSERT_EQ(grx_regex_facts(regex, &facts), GRX_OK);
    EXPECT_TRUE(facts.has_recursion) << pattern;

    int matched = 0;
    // Two levels are inside the cap and four are not.
    EXPECT_EQ(grx_regex_search(regex, "aabb", 4, 0, GRX_ENGINE_BACKTRACK,
                  &limits, nullptr, &matched),
        GRX_OK)
        << pattern;
    EXPECT_TRUE(matched) << pattern;

    EXPECT_EQ(grx_regex_search(regex, "aaaabbbb", 8, 0, GRX_ENGINE_BACKTRACK,
                  &limits, nullptr, &matched),
        GRX_ERR_LIMIT)
        << pattern << ": a recursion deeper than max_recursion_depth has to "
                      "be refused, not run";

    // Zero is no limit, as it is for every other field.
    limits.max_recursion_depth = 0;
    EXPECT_EQ(grx_regex_search(regex, "aaaabbbb", 8, 0, GRX_ENGINE_BACKTRACK,
                  &limits, nullptr, &matched),
        GRX_OK)
        << pattern;
    EXPECT_TRUE(matched) << pattern;

    grx_regex_free(regex);
  }
}

// --------------------------------------------------------------------------
// The limits a pattern asks for: `(*LIMIT_MATCH=d)` and kin
// --------------------------------------------------------------------------

namespace {

/** Compile under PCRE2 with the defaults, search, report what happened. */
struct Asked {
  GRX_Result compile_result;
  GRX_Diag compile_diag;
  GRX_Result result;
  GRX_Diag diag;
  int matched;
};

Asked ask(const std::string & pattern, const std::string & subject,
    size_t caller_steps = 0) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  if (caller_steps) {
    limits.max_steps = caller_steps;
  }

  Asked asked = {GRX_OK, GRX_DIAG_NONE, GRX_OK, GRX_DIAG_NONE, 0};
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  grx_error_clear(&error);
  asked.compile_result = grx_regex_compile_with_allocator(pattern.c_str(),
      pattern.size(), GRX_SYNTAX_PCRE, 0, &limits, nullptr, &error, &regex);
  if (asked.compile_result != GRX_OK) {
    asked.compile_diag = error.diag;
    return asked;
  }

  GRX_Match * match = nullptr;
  grx_match_create(regex, nullptr, &match);
  asked.result = grx_regex_search(regex, subject.data(), subject.size(), 0,
      GRX_ENGINE_AUTO, &limits, match, &asked.matched);
  const GRX_Error * match_error = grx_match_error(match);
  asked.diag = match_error ? match_error->diag : GRX_DIAG_NONE;
  grx_match_destroy(match);
  grx_regex_free(regex);
  return asked;
}

} // namespace

TEST(Limits, APatternsOwnLimitIsAppliedAndNotMerelyParsed) {
  // All three directives, because each lands on a different GRX_Limits
  // field and a table with one row checked is a table with two holes. Each
  // pair is the same pattern with and without the directive, so that what
  // is being measured is the directive and not the pattern.
  //
  // Every expectation here was read off pcre2test 10.46 first.

  // `(*LIMIT_MATCH=d)` -> max_steps.
  EXPECT_EQ(ask("abc", "abc").matched, 1);
  Asked steps = ask("(*LIMIT_MATCH=1)abc", "abc");
  EXPECT_EQ(steps.result, GRX_ERR_LIMIT);
  EXPECT_EQ(steps.diag, GRX_DIAG_LIMIT_STEPS);

  // `(*LIMIT_DEPTH=d)` -> max_backtrack, on a program that backtracks.
  EXPECT_EQ(ask("(\\w+)\\1", "aaaaaaaaaaaaaaaaaaaab").matched, 1);
  Asked depth = ask("(*LIMIT_DEPTH=2)(\\w+)\\1", "aaaaaaaaaaaaaaaaaaaab");
  EXPECT_EQ(depth.result, GRX_ERR_LIMIT);
  EXPECT_EQ(depth.diag, GRX_DIAG_LIMIT_BACKTRACK);

  // `(*LIMIT_HEAP=d)` -> max_match_memory, and the number is *kibibytes*
  // where max_match_memory counts bytes. A subject long enough that the
  // thread lists cost more than one kibibyte is what makes the scale
  // visible: with the directive read as bytes this row would pass anyway.
  //
  // `[cq]` rather than `c`, and the reason is the prefilter rather than the
  // heap: a bare `c` is a literal every match contains, so a subject of `a`
  // with no `c` in it is answered by src/compile/prefilter.c's required
  // literal before any thread list is allocated, and a row about the size of
  // that allocation would never reach it. A class is a choice of code points
  // and leaves nothing to require.
  std::string big(4000, 'a');
  EXPECT_EQ(ask("(a|b)*[cq]", big).result, GRX_OK);
  Asked heap = ask("(*LIMIT_HEAP=1)(a|b)*[cq]", big);
  EXPECT_EQ(heap.result, GRX_ERR_LIMIT);
  EXPECT_EQ(heap.diag, GRX_DIAG_LIMIT_MATCH_MEMORY);
}

TEST(Limits, APatternMayLowerACallersLimitAndMayNotRaiseIt) {
  // The rule that makes the directive safe to honour at all: a caller's cap
  // is a policy, and a pattern that arrived from outside must not be able
  // to lift it. Both directions, because "the smaller wins" is only a rule
  // if it holds when the pattern is the larger of the two.
  EXPECT_EQ(ask("abc", "abc", 1000000).matched, 1);

  // The caller is the tighter of the two.
  Asked caller_wins = ask("(*LIMIT_MATCH=1000000)abc", "abc", 1);
  EXPECT_EQ(caller_wins.result, GRX_ERR_LIMIT);
  EXPECT_EQ(caller_wins.diag, GRX_DIAG_LIMIT_STEPS);

  // The pattern is the tighter of the two.
  Asked pattern_wins = ask("(*LIMIT_MATCH=1)abc", "abc", 1000000);
  EXPECT_EQ(pattern_wins.result, GRX_ERR_LIMIT);
  EXPECT_EQ(pattern_wins.diag, GRX_DIAG_LIMIT_STEPS);

  // Neither is: the pattern asks for less than the default and more than
  // this pattern needs, and the search runs.
  EXPECT_EQ(ask("(*LIMIT_MATCH=1000)abc", "abc").matched, 1);

  // A *later* directive replaces an earlier one rather than joining it.
  // pcre2test answers both orders that way: `=1` then `=1000000` matches
  // "abc" there and `=1000000` then `=1` does not.
  EXPECT_EQ(ask("(*LIMIT_MATCH=1)(*LIMIT_MATCH=1000000)abc", "abc").matched, 1);
  EXPECT_EQ(ask("(*LIMIT_MATCH=1000000)(*LIMIT_MATCH=1)abc", "abc").result,
      GRX_ERR_LIMIT);
}

TEST(Limits, APatternAskingForNothingIsRefusedRatherThanUnbounded) {
  // Zero is where the directive's encoding and GRX_Limits' disagree, and
  // they disagree backwards: GRX_Limits reads 0 as "no limit at all", and
  // `(*LIMIT_MATCH=0)` is a budget of nothing. pcre2test answers "match
  // limit exceeded" for `(*LIMIT_MATCH=0)abc` against "abc", so writing the
  // request straight through would turn the tightest limit a pattern can
  // ask for into the loosest there is - and silently.
  for (const char * pattern : {"(*LIMIT_MATCH=0)abc", "(*LIMIT_DEPTH=0)abc",
           "(*LIMIT_HEAP=0)abc"}) {
    Asked asked = ask(pattern, "abc");
    EXPECT_EQ(asked.compile_result, GRX_OK) << pattern;
    EXPECT_EQ(asked.result, GRX_ERR_LIMIT) << pattern;
    EXPECT_EQ(asked.matched, 0) << pattern;
  }
}

TEST(Limits, ALimitDirectiveTooLargeToHoldIsMalformedRatherThanClamped) {
  // pcre2test answers error 160, "(*VERB) not recognized or malformed", for
  // `(*LIMIT_MATCH=4294967294)`, because its counter is 32 bits wide. The
  // ceiling here is this library's own and far higher; what is shared is
  // that an unrepresentable request is refused rather than quietly turned
  // into some other number.
  for (const char * pattern : {
           "(*LIMIT_MATCH=99999999999999999999999999)abc",
           "(*LIMIT_HEAP=99999999999999999999999999)abc",
           "(*LIMIT_MATCH=)abc",
           "(*LIMIT_MATCH=1x)abc",
       }) {
    Asked asked = ask(pattern, "abc");
    EXPECT_EQ(asked.compile_result, GRX_ERR_SYNTAX) << pattern;
    EXPECT_EQ(asked.compile_diag, GRX_DIAG_INVALID_GROUP_SYNTAX) << pattern;
  }

  // A value this library *can* hold is accepted, so the row above is a
  // ceiling and not a refusal of every large number.
  EXPECT_EQ(ask("(*LIMIT_MATCH=4294967296)abc", "abc").matched, 1);
}


TEST(Limits, SequentialVimAssertionsDoNotSpendNativeRecursionDepth) {
  for (GRX_Syntax syntax : {GRX_SYNTAX_VIM_BACKTRACK, GRX_SYNTAX_VIM_NFA}) {
    std::string pattern = syntax == GRX_SYNTAX_VIM_BACKTRACK ? "a" : "";
    for (size_t i = 0; i < 200; i++) {
      pattern += syntax == GRX_SYNTAX_VIM_BACKTRACK ? R"(\%(a\)\@<=)"
                                                    : R"(\%(\%(a\)\@=a\)\@=)";
    }
    pattern += "a";
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile(pattern.c_str(), syntax, 0, &regex), GRX_OK);
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
    int matched = 0;
    EXPECT_EQ(grx_regex_search(regex, "aa", 2, 0, GRX_ENGINE_BACKTRACK, nullptr,
                  match, &matched),
        GRX_OK);
    EXPECT_EQ(matched, 1);
    grx_match_destroy(match);
    grx_regex_free(regex);
  }
}

TEST(Limits, NestedVimAssertionBodiesRetainTheNativeStackSafetyBound) {
  for (GRX_Syntax syntax : {GRX_SYNTAX_VIM_BACKTRACK, GRX_SYNTAX_VIM_NFA}) {
    std::string pattern = "a";
    for (size_t i = 0; i < 200; i++) {
      pattern = R"(\%()" + pattern + R"(\)\@=)";
    }
    pattern += "a";
    GRX_Limits limits;
    grx_limits_default(&limits);
    limits.max_nesting_depth = 480;
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile_with_allocator(pattern.data(), pattern.size(),
                  syntax, 0, &limits, nullptr, nullptr, &regex),
        GRX_OK);
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
    int matched = 1;
    EXPECT_EQ(grx_regex_search(regex, "aa", 2, 0, GRX_ENGINE_BACKTRACK, &limits,
                  match, &matched),
        GRX_ERR_LIMIT);
    EXPECT_EQ(matched, 0);
    EXPECT_EQ(grx_match_error(match)->diag, GRX_DIAG_LIMIT_RECURSION_DEPTH);
    grx_match_destroy(match);
    grx_regex_free(regex);
  }
}

TEST(Limits, DefaultAcceptedNestedAssertionsCanDiscoverAtomicBodies) {
  for (bool outside : {false, true}) {
    std::string pattern = outside ? "a" : R"(\%(a\)\@>)";
    for (size_t depth = 0; depth < 127; depth++) {
      pattern = R"(\%()" + pattern + R"(\)\@=)";
    }
    if (outside) {
      pattern = R"(\%()" + pattern + R"(\)\@>)";
    }
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile(pattern.c_str(), GRX_SYNTAX_VIM_NFA, 0,
                  &regex), GRX_OK);
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
    int matched = 0;
    EXPECT_EQ(grx_regex_search(regex, "a", 1, 0, GRX_ENGINE_BACKTRACK,
                  nullptr, match, &matched), GRX_OK);
    ASSERT_TRUE(matched);
    GRX_Capture whole;
    ASSERT_EQ(grx_match_group(match, 0, &whole), GRX_OK);
    EXPECT_EQ(whole.start, 0u);
    EXPECT_EQ(whole.end, 0u);
    grx_match_destroy(match);
    grx_regex_free(regex);
  }
}

TEST(Limits, FlatVimAssertionLoopsMatchWithinThePublicBudgets) {
  for (GRX_Syntax syntax : {GRX_SYNTAX_VIM_BACKTRACK, GRX_SYNTAX_VIM_NFA}) {
    for (const char * pattern : {R"(\%(\(a\)\@=a\)*)", R"(\%(a\(a\)\@<=\)*)"}) {
      GRX_Regex * regex = nullptr;
      ASSERT_EQ(grx_regex_compile(pattern, syntax, 0, &regex), GRX_OK);
      GRX_Match * match = nullptr;
      ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
      for (size_t length : {300u, 2000u}) {
        std::string subject(length, 'a');
        int matched = 0;
        EXPECT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                      GRX_ENGINE_BACKTRACK, nullptr, match, &matched),
            GRX_OK)
            << grx_syntax_name(syntax) << " " << pattern << " length "
            << length;
        ASSERT_EQ(matched, 1);
        GRX_Capture whole;
        ASSERT_EQ(grx_match_group(match, 0, &whole), GRX_OK);
        EXPECT_EQ(whole.start, 0u);
        EXPECT_EQ(whole.end, subject.size());
      }
      grx_match_destroy(match);
      grx_regex_free(regex);
    }
  }
}

TEST(Limits, DeferredVimAssertionsHonorBudgetsAcrossLongFlatContinuations) {
  for (GRX_Syntax syntax : {GRX_SYNTAX_VIM_BACKTRACK, GRX_SYNTAX_VIM_NFA}) {
    std::string pattern = syntax == GRX_SYNTAX_VIM_BACKTRACK
        ? R"(a\%(a\)\@<=)"
        : R"(\%(\%(a\)\@=a\)\@=)";
    pattern += std::string(4000, 'a');
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile(pattern.c_str(), syntax, 0, &regex), GRX_OK);
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
    GRX_Limits limits;
    grx_limits_default(&limits);
    limits.max_steps = 32;
    std::string subject(4001, 'a');
    int matched = 1;
    EXPECT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                  GRX_ENGINE_BACKTRACK, &limits, match, &matched),
        GRX_ERR_LIMIT);
    EXPECT_EQ(matched, 0);
    EXPECT_EQ(grx_match_error(match)->diag, GRX_DIAG_LIMIT_STEPS);
    grx_match_destroy(match);
    grx_regex_free(regex);
  }
}

TEST(Limits, VimAssertionCaptureSnapshotsShareTheMatchMemoryBudget) {
  for (const char * pattern : {
           R"(\(a\)\@>a\(a\1\)\@<=.)",
           R"(\(a\)\%(z\)\@>x\|\%(ab\)\@>)",
           R"(\(\(a\)\@=a\)\@<=\(a\)\@<=\%(a\)\@>)",
       }) {
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(
        grx_regex_compile(pattern, GRX_SYNTAX_VIM_NFA, 0, &regex), GRX_OK);
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
    GRX_Limits limits;
    grx_limits_default(&limits);
    limits.max_match_memory = std::strstr(pattern, "z") ? 512 : 1024;
    int matched = 1;
    EXPECT_EQ(grx_regex_search(regex, "aab", 3, 0, GRX_ENGINE_BACKTRACK,
                  &limits, match, &matched),
        GRX_ERR_LIMIT)
        << pattern;
    EXPECT_EQ(matched, 0);
    EXPECT_EQ(grx_match_error(match)->diag, GRX_DIAG_LIMIT_MATCH_MEMORY);
    limits.max_match_memory = 1024 * 1024;
    EXPECT_EQ(grx_regex_search(regex, "aab", 3, 0, GRX_ENGINE_BACKTRACK,
                  &limits, match, &matched),
        GRX_OK)
        << pattern;
    EXPECT_EQ(matched, 1);
    grx_match_destroy(match);
    grx_regex_free(regex);
  }
}

TEST(Limits, AtomicCandidateFrontiersHonorBudgetsAndFlatSubjectLengths) {
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile(
                R"(a\+\(a\)\@>\(a\)\@<=)", GRX_SYNTAX_VIM_NFA, 0, &regex),
      GRX_OK);
  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
  GRX_Limits limits;
  grx_limits_default(&limits);
  int matched = 1;
  limits.max_steps = 16;
  EXPECT_EQ(grx_regex_search(regex, "aaab", 4, 0, GRX_ENGINE_BACKTRACK, &limits,
                match, &matched),
      GRX_ERR_LIMIT);
  EXPECT_FALSE(matched);
  EXPECT_EQ(grx_match_error(match)->diag, GRX_DIAG_LIMIT_STEPS);
  grx_limits_default(&limits);
  limits.max_match_memory = 1024;
  EXPECT_EQ(grx_regex_search(regex, "aaab", 4, 0, GRX_ENGINE_BACKTRACK, &limits,
                match, &matched),
      GRX_ERR_LIMIT);
  EXPECT_FALSE(matched);
  EXPECT_EQ(grx_match_error(match)->diag, GRX_DIAG_LIMIT_MATCH_MEMORY);
  for (size_t length : {300u, 2000u}) {
    std::string subject(length, 'a');
    subject += 'b';
    EXPECT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                  GRX_ENGINE_BACKTRACK, nullptr, match, &matched),
        GRX_OK);
    ASSERT_TRUE(matched);
    GRX_Capture whole;
    ASSERT_EQ(grx_match_group(match, 0, &whole), GRX_OK);
    EXPECT_EQ(whole.start, 0u);
    EXPECT_EQ(whole.end, length);
  }
  grx_match_destroy(match);
  grx_regex_free(regex);
}

TEST(Limits, AtomicDiscoveryStackChargeUsesAnAbiSizedBudget) {
  ScratchAllocationMeter meter;
  const char * pattern = R"(a\+\(a\)\@>\(a\)\@<=)";
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile_with_allocator(pattern, std::strlen(pattern),
                GRX_SYNTAX_VIM_NFA, 0, nullptr, meter.get(), nullptr, &regex),
      GRX_OK);
  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex, meter.get(), &match), GRX_OK);
  const size_t baseline = meter.live();
  meter.begin();
  int matched = 0;
  ASSERT_EQ(grx_regex_search(regex, "aaab", 4, 0, GRX_ENGINE_BACKTRACK, nullptr,
                match, &matched), GRX_OK);
  ASSERT_TRUE(matched);
  ASSERT_EQ(meter.live(), baseline);
  ASSERT_GT(meter.root_stack(), 0u);
  ASSERT_GT(meter.peak(), meter.root_stack() + 1);
  // The first realloc is the ordinary backtracking stack, whose legacy
  // allocation is outside aggregate scratch accounting. All remaining live
  // runtime buffers on this input belong to the charged assertion search.
  // Derive the cap from physical allocations, independently of the charge:
  // dropping discovery's stack charge must still make this search succeed.
  const size_t charged_peak = meter.charged_peak();
  std::fprintf(stderr, "Discovery physical peak %zu, root stack %zu, cap %zu\n",
      meter.peak(), meter.root_stack(), charged_peak - 1);
  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_match_memory = charged_peak - 1;
  EXPECT_EQ(grx_regex_search(regex, "aaab", 4, 0, GRX_ENGINE_BACKTRACK, &limits,
                match, &matched), GRX_ERR_LIMIT);
  EXPECT_FALSE(matched);
  EXPECT_EQ(grx_match_error(match)->diag, GRX_DIAG_LIMIT_MATCH_MEMORY);
  EXPECT_EQ(meter.live(), baseline);
  limits.max_match_memory = charged_peak;
  EXPECT_EQ(grx_regex_search(regex, "aaab", 4, 0, GRX_ENGINE_BACKTRACK, &limits,
                match, &matched), GRX_OK);
  EXPECT_TRUE(matched);
  EXPECT_EQ(meter.live(), baseline);
  grx_match_destroy(match);
  grx_regex_free(regex);
  EXPECT_EQ(meter.live(), 0u);
}

TEST(Limits, AtomicDiscoveryIncludesCallerFramesInBacktrackLimit) {
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile(R"(a\+\(a\)\@>\(a\)\@<=)",
                GRX_SYNTAX_VIM_NFA, 0, &regex), GRX_OK);
  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
  GRX_Limits limits;
  grx_limits_default(&limits);
  // A subject with a composing character is the backtracking engine's: the
  // thread-list automaton behind this profile does not model the marks Vim
  // attaches to the character before them, and what is measured here is the
  // backtracker's own frame accounting.
  const std::string subject = std::string(100, 'a') + "b\xcc\x81";
  // Each interpreter fits 200 frames independently; simultaneous caller and
  // discovery frames require 224. Zeroing the probe's caller-depth base makes
  // the 200-frame search incorrectly succeed (its independent peak is 151).
  for (size_t cap : {256u, 200u, 256u}) {
    limits.max_backtrack = cap;
    int matched = 1;
    GRX_Result result = grx_regex_search(regex, subject.data(), subject.size(), 0,
        GRX_ENGINE_BACKTRACK, &limits, match, &matched);
    EXPECT_EQ(result, cap == 200 ? GRX_ERR_LIMIT : GRX_OK);
    EXPECT_EQ(matched, cap == 200 ? 0 : 1);
    EXPECT_EQ(grx_match_error(match)->diag,
        cap == 200 ? GRX_DIAG_LIMIT_BACKTRACK : GRX_DIAG_NONE);
  }
  grx_match_destroy(match);
  grx_regex_free(regex);
}

TEST(Limits, AtomicCandidateLateFailureKeepsTheDefaultStepCeiling) {
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile(
                R"(\%(\%(\(a\)\@=a\)\@>\)*ab$)", GRX_SYNTAX_VIM_NFA, 0, &regex),
      GRX_OK);
  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
  // Both tails hold the `ab` the pattern requires, so the required-literal
  // prefilter cannot answer and the search has to try every start; the `$`
  // is what makes each of them fail late. Without the `ab` the search is
  // skipped and never reaches a ceiling.
  for (const char * tail : {"abx", "abxx"}) {
    std::string subject(600, 'a');
    subject += tail;
    // A composing mark, so that the backtracker and not the automaton runs it.
    subject += "\xcc\x81";
    int matched = 1;
    EXPECT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                  GRX_ENGINE_BACKTRACK, nullptr, match, &matched),
        GRX_ERR_LIMIT);
    EXPECT_FALSE(matched);
    EXPECT_EQ(grx_match_error(match)->diag, GRX_DIAG_LIMIT_STEPS);
    size_t default_steps = grx_match_steps(match);
    GRX_Limits limits;
    grx_limits_default(&limits);
    limits.max_steps = 30000000;
    EXPECT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                  GRX_ENGINE_BACKTRACK, &limits, match, &matched),
        GRX_OK);
    EXPECT_FALSE(matched);
    EXPECT_GT(grx_match_steps(match), default_steps);
    std::fprintf(stderr,
        "Atomic candidate failed-search pressure length %zu: "
        "default %zu steps, explicit 30000000 cap %zu steps\n",
        subject.size(), default_steps, grx_match_steps(match));
  }
  grx_match_destroy(match);
  grx_regex_free(regex);
}

TEST(Limits,
    DeferredAssertionLateFailureReportsStepsAndAcceptsAnExplicitBudget) {
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(
      grx_regex_compile(R"(\%(a\(a\)\@<=\)*ab$)", GRX_SYNTAX_VIM_NFA, 0,
          &regex),
      GRX_OK);
  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
  // `ab` is present so the required-literal prefilter cannot skip the search.
  // The trailing composing mark is what leaves the search to the backtracker;
  // the automaton does not model marks.
  std::string subject = std::string(600, 'a') + "abx\xcc\x81";
  int matched = 1;
  EXPECT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                GRX_ENGINE_BACKTRACK, nullptr, match, &matched),
      GRX_ERR_LIMIT);
  EXPECT_FALSE(matched);
  EXPECT_EQ(grx_match_error(match)->diag, GRX_DIAG_LIMIT_STEPS);
  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_steps = 20000000;
  EXPECT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                GRX_ENGINE_BACKTRACK, &limits, match, &matched),
      GRX_OK);
  EXPECT_FALSE(matched);
  grx_match_destroy(match);
  grx_regex_free(regex);
}

TEST(Limits,
    BacktrackLayoutRejectsUnrepresentableExpandedRangesBeforeAllocation) {
  grxtest::FailingAllocator allocator(0);
  GRX_Regex regex = {};
  regex.allocator = allocator.get();
  regex.program.insts.count = 1;
  GRX_Limits limits;
  grx_limits_unlimited(&limits);
  GRX_ExecRequest request = {};
  request.regex = &regex;
  request.limits = &limits;
  GRX_Diag diag = GRX_DIAG_NONE;
  size_t steps = 1;
  request.out_diag = &diag;
  request.out_steps = &steps;
  size_t max_words = SIZE_MAX / sizeof(size_t);
  size_t slot_limit = max_words < GRX_INDEX_NONE ? max_words : GRX_INDEX_NONE;
  for (int shape : {0, 1, 2}) {
    SCOPED_TRACE(shape);
    regex.capture_count = shape == 0 ? SIZE_MAX
        : shape == 2                 ? slot_limit / 20
                                     : 0;
    regex.program.register_count = shape == 1 ? UINT32_MAX : 0;
    regex.program.flags = shape == 2 ? GRX_PROGRAM_SHADOW_CAPTURES |
            GRX_PROGRAM_PROVISIONAL_LOOK_END | GRX_PROGRAM_ATOMIC_FRONTIER
                                     : 0;
    int matched = 1;
    EXPECT_EQ(grx_exec_backtrack(&request, &matched), GRX_ERR_LIMIT);
    EXPECT_EQ(matched, 0);
    EXPECT_EQ(diag, GRX_DIAG_LIMIT_MATCH_MEMORY);
    EXPECT_EQ(steps, 0u);
    EXPECT_EQ(allocator.requested(), 0);
    EXPECT_EQ(allocator.live(), 0);
  }
}

TEST(Limits, MirroredReferenceEndpointLayoutChecksTheExactBoundary) {
  const size_t words = SIZE_MAX / sizeof(size_t);
  const size_t slots = std::min(words, static_cast<size_t>(GRX_INDEX_NONE));
  const size_t mirrored = std::min((slots - 1) / 2, (words - 1) / 3);
  // Two group-zero slots, five two-slot regions and seven owner tags.
  const size_t registers = mirrored - 19;
  for (size_t extra : {size_t{0}, size_t{1}}) {
    SCOPED_TRACE(extra);
    grxtest::FailingAllocator allocator(1);
    GRX_Regex regex = {};
    regex.allocator = allocator.get();
    regex.program.insts.count = 1;
    regex.program.flags = GRX_PROGRAM_PROVISIONAL_LOOK_END;
    regex.program.register_count = static_cast<uint32_t>(registers + extra);
    GRX_Limits limits;
    grx_limits_unlimited(&limits);
    GRX_ExecRequest request = {};
    request.regex = &regex;
    request.limits = &limits;
    GRX_Diag diag = GRX_DIAG_NONE;
    request.out_diag = &diag;
    int matched = 1;
    EXPECT_EQ(grx_exec_backtrack(&request, &matched),
        extra ? GRX_ERR_LIMIT : GRX_ERR_OOM);
    EXPECT_FALSE(matched);
    EXPECT_EQ(allocator.requested(), extra ? 0 : 1);
    EXPECT_EQ(allocator.live(), 0);
    if (extra) {
      EXPECT_EQ(diag, GRX_DIAG_LIMIT_MATCH_MEMORY);
    }
  }
}

TEST(Limits, CombiningQueriesChargePhysicalScratchAndAggregateSteps) {
  struct Row {
    const char * pattern;
    const char * subject;
  };
  for (const Row & row : {Row{R"(\([ab].\|abX\)\@=.)", "a\u0301"},
           Row{R"(\([ab].\|abX\)\@>)", "a\u0301"},
           Row{R"(\(a\?.\)\@<=x)", "aa\u0301x"},
           Row{R"(\(a\+\)\@=\(a\)\@=a)", "a\u0301aaa"},
           Row{R"(\(a\)\@=a\1)", "aab"}, Row{R"(\(a\)\@>.\1)", "aaa\u0301"},
           Row{R"(\(a\)\@=a\(\1\)\@=.)", "a\u0301a\u0301"},
           Row{R"(\(a\)\@=\(aX\|\1\)\@=.)", "a\u0301"},
           Row{R"(\(a\)\@=\(\1\|aX\)\@>x)", "a\u0301x"}}) {
    const char * pattern = row.pattern;
    const char * subject = row.subject;
    SCOPED_TRACE(pattern);
    ScratchAllocationMeter meter;
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile_with_allocator(pattern, std::strlen(pattern),
                  GRX_SYNTAX_VIM_NFA, 0, nullptr, meter.get(), nullptr, &regex),
        GRX_OK);
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex, meter.get(), &match), GRX_OK);
    const size_t baseline = meter.live();
    meter.begin();
    int matched = 0;
    ASSERT_EQ(grx_regex_search(regex, subject, std::strlen(subject), 0,
                  GRX_ENGINE_BACKTRACK, nullptr, match, &matched),
        GRX_OK);
    ASSERT_TRUE(matched);
    const size_t steps = grx_match_steps(match);
    ASSERT_GT(steps, 1u);
    ASSERT_GT(meter.root_stack(), 0u);
    ASSERT_GT(meter.peak(), meter.root_stack() + 1);
    const size_t peak = meter.charged_peak();
    EXPECT_EQ(meter.live(), baseline);
    std::fprintf(stderr,
        "Combining query physical peak %zu, legacy root stack %zu, charged "
        "peak %zu, steps %zu\n",
        meter.peak(), meter.root_stack(), peak, steps);
    for (size_t budget : {size_t{1}, steps - 1, steps}) {
      GRX_Limits limits;
      grx_limits_default(&limits);
      limits.max_steps = budget;
      EXPECT_EQ(grx_regex_search(regex, subject, std::strlen(subject), 0,
                    GRX_ENGINE_BACKTRACK, &limits, match, &matched),
          budget < steps ? GRX_ERR_LIMIT : GRX_OK);
      EXPECT_EQ(matched, budget >= steps);
      if (budget < steps) {
        EXPECT_EQ(grx_match_error(match)->diag, GRX_DIAG_LIMIT_STEPS);
      }
      EXPECT_EQ(meter.live(), baseline);
    }
    for (size_t budget : {size_t{1}, peak - 1, peak}) {
      GRX_Limits limits;
      grx_limits_default(&limits);
      limits.max_match_memory = budget;
      EXPECT_EQ(grx_regex_search(regex, subject, std::strlen(subject), 0,
                    GRX_ENGINE_BACKTRACK, &limits, match, &matched),
          budget < peak ? GRX_ERR_LIMIT : GRX_OK);
      EXPECT_EQ(matched, budget >= peak);
      if (budget < peak) {
        EXPECT_EQ(grx_match_error(match)->diag, GRX_DIAG_LIMIT_MATCH_MEMORY);
      }
      EXPECT_EQ(meter.live(), baseline);
    }
    EXPECT_EQ(grx_regex_search(regex, subject, std::strlen(subject), 0,
                  GRX_ENGINE_BACKTRACK, nullptr, match, &matched),
        GRX_OK);
    EXPECT_TRUE(matched);
    grx_match_destroy(match);
    grx_regex_free(regex);
    EXPECT_EQ(meter.live(), 0u);
  }
}

TEST(Limits, CombiningFrontiersBoundLongMarkRunsAndRecoverAfterLimit) {
  for (const char * pattern :
      {R"(\%(a\|[ab]\))", R"(\([ab].\|abX\)\@=.)", R"(\(a\|.\)\@<=b)",
          R"(\(a\)\@=\1)"}) {
    SCOPED_TRACE(pattern);
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(
        grx_regex_compile(pattern, GRX_SYNTAX_VIM_NFA, 0, &regex), GRX_OK);
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
    for (size_t count : {64u, 2048u}) {
      std::string subject = "a";
      for (size_t i = 0; i < count; i++)
        subject += "\u0301";
      subject += 'b';
      GRX_Limits limits;
      grx_limits_default(&limits);
      limits.max_steps = 16;
      int matched = 0;
      EXPECT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                    GRX_ENGINE_BACKTRACK, &limits, match, &matched),
          GRX_ERR_LIMIT);
      EXPECT_FALSE(matched);
      EXPECT_EQ(grx_match_error(match)->diag, GRX_DIAG_LIMIT_STEPS);
      limits.max_steps = 200000000;
      ASSERT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                    GRX_ENGINE_BACKTRACK, &limits, match, &matched),
          GRX_OK);
      std::fprintf(stderr, "Combining long marks %zu, steps %zu: %s\n", count,
          grx_match_steps(match), pattern);
      ASSERT_TRUE(matched);
      GRX_Capture whole;
      ASSERT_EQ(grx_match_group(match, 0, &whole), GRX_OK);
      const bool ahead = std::strstr(pattern, "@=") != nullptr;
      EXPECT_EQ(whole.start, ahead ? 0u : subject.size() - 1);
      EXPECT_EQ(whole.end, ahead ? subject.size() - 1 : subject.size());
    }
    grx_match_destroy(match);
    grx_regex_free(regex);
  }
}

TEST(Limits, CharacterPolicyCandidateScansChargeEveryScalarAndRecover) {
  const char * patterns[] = {"\\c\u0301\\?b", "\\Z\u0301\\?a\u0302b",
      "\\c\\Z\u0301\\?a\u0302b"};
  for (const char * pattern : patterns) {
    SCOPED_TRACE(pattern);
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile(pattern, GRX_SYNTAX_VIM_BACKTRACK, 0, &regex),
        GRX_OK);
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
    for (size_t count : {64u, 2048u}) {
      std::string subject = "a";
      for (size_t i = 0; i < count; i++) {
        subject += "\u0301";
      }
      subject += "a\u0301b";
      int matched = 0;
      ASSERT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                    GRX_ENGINE_BACKTRACK, nullptr, match, &matched), GRX_OK);
      ASSERT_TRUE(matched);
      const size_t steps = grx_match_steps(match);
      ASSERT_GT(steps, count);
      GRX_Capture baseline;
      ASSERT_EQ(grx_match_group(match, 0, &baseline), GRX_OK);
      for (size_t budget : {size_t{1}, steps - 1, steps}) {
        GRX_Limits limits;
        grx_limits_default(&limits);
        limits.max_steps = budget;
        EXPECT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                      GRX_ENGINE_BACKTRACK, &limits, match, &matched),
            budget < steps ? GRX_ERR_LIMIT : GRX_OK);
        EXPECT_EQ(matched, budget >= steps);
        if (budget < steps) {
          EXPECT_EQ(grx_match_error(match)->diag, GRX_DIAG_LIMIT_STEPS);
        }
      }
      ASSERT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                    GRX_ENGINE_BACKTRACK, nullptr, match, &matched), GRX_OK);
      ASSERT_TRUE(matched);
      GRX_Capture recovered;
      ASSERT_EQ(grx_match_group(match, 0, &recovered), GRX_OK);
      EXPECT_EQ(recovered.start, baseline.start);
      EXPECT_EQ(recovered.end, baseline.end);
    }
    grx_match_destroy(match);
    grx_regex_free(regex);
  }
}

TEST(Limits, NonliteralMarkedCollectionsRecoverAfterLowBudgetsAndLongMarks) {
  for (GRX_Syntax syntax : {GRX_SYNTAX_VIM_BACKTRACK, GRX_SYNTAX_VIM_NFA}) {
    for (const char * pattern : {"[a-ź]", "[[:alpha:]́]"}) {
      SCOPED_TRACE(pattern);
      GRX_Regex * regex = nullptr;
      ASSERT_EQ(grx_regex_compile(pattern, syntax, 0, &regex), GRX_OK);
      GRX_Match * match = nullptr;
      ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
      for (size_t count : {64u, 2048u}) {
        std::string subject = "b";
        for (size_t i = 0; i < count; i++) subject += "́";
        subject += 'b';
        for (GRX_Engine engine : {GRX_ENGINE_AUTO, GRX_ENGINE_BACKTRACK,
                 GRX_ENGINE_PIKE, GRX_ENGINE_BITSTATE}) {
          if (syntax == GRX_SYNTAX_VIM_BACKTRACK &&
              (engine == GRX_ENGINE_PIKE || engine == GRX_ENGINE_BITSTATE))
            continue;
          GRX_Limits limits;
          grx_limits_default(&limits);
          limits.max_steps = 1;
          int matched = 0;
          EXPECT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                        engine, &limits, match, &matched), GRX_ERR_LIMIT);
          EXPECT_FALSE(matched);
          ASSERT_EQ(grx_regex_search(regex, subject.data(), subject.size(), 0,
                        engine, nullptr, match, &matched), GRX_OK);
          ASSERT_TRUE(matched);
          GRX_Capture whole;
          ASSERT_EQ(grx_match_group(match, 0, &whole), GRX_OK);
          EXPECT_EQ(whole.start, 0u);
          EXPECT_EQ(whole.end, syntax == GRX_SYNTAX_VIM_BACKTRACK
                  ? 1u : subject.size() - 1);
        }
      }
      grx_match_destroy(match);
      grx_regex_free(regex);
    }
  }
}
