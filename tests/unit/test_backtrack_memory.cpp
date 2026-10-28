/**
 * @file
 * Aggregate requested scratch bytes, moving allocations, refusal and reuse.
 *
 * Copyright 2026 by Corey Pennycuff
 */
#include <algorithm>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <memory>
#include <map>
#include <set>
#include <utility>
#include <string>
#include <vector>
#include "test_helpers.h"

namespace {
/** Count logical realloc replacement, excluding allocator-private copy space. */
class PayloadMeter {
  union Header { size_t bytes; std::max_align_t alignment; };
  GRX_Allocator allocator_ {};
  size_t live_ = 0, baseline_ = 0, peak_ = 0;
  size_t calls_ = 0, fail_ = 0, moves_ = 0, failed_ = 0;
  bool armed_ = false;
  struct Event { size_t ordinal, bytes, live; char kind; };
  std::vector<Event> events_;

  void * allocate(void * old, size_t bytes, char kind) {
    const size_t maximum = std::numeric_limits<size_t>::max();
    if (bytes > maximum - sizeof(Header)) return nullptr;
    if (armed_ && ++calls_ == fail_) { ++failed_; return nullptr; }
    Header * prior = old ? static_cast<Header *>(old) - 1 : nullptr;
    const size_t previous = prior ? prior->bytes : 0;
    if (bytes > maximum - (live_ - previous)) return nullptr;
    auto * block = static_cast<Header *>(std::malloc(sizeof(Header) + (bytes ? bytes : 1)));
    if (!block) return nullptr;
    block->bytes = bytes;
    if (prior) {
      std::memcpy(block + 1, old, std::min(previous, bytes));
      std::free(prior);
      if (armed_) ++moves_;
    }
    live_ = live_ - previous + bytes;
    if (armed_) {
      peak_ = std::max(peak_, live_ - baseline_);
      events_.push_back({calls_, bytes, live_ - baseline_, kind});
    }
    return block + 1;
  }
public:
  PayloadMeter() {
    allocator_.ctx = this;
    allocator_.malloc_fn = [](void * ctx, size_t bytes) -> void * {
      return static_cast<PayloadMeter *>(ctx)->allocate(nullptr, bytes, 'm');
    };
    allocator_.calloc_fn = [](void * ctx, size_t count, size_t width) -> void * {
      if (width && count > std::numeric_limits<size_t>::max() / width) return nullptr;
      const size_t bytes = count * width;
      void * data = static_cast<PayloadMeter *>(ctx)->allocate(nullptr, bytes, 'c');
      if (data) std::memset(data, 0, bytes);
      return data;
    };
    allocator_.realloc_fn = [](void * ctx, void * old, size_t bytes) -> void * {
      return static_cast<PayloadMeter *>(ctx)->allocate(old, bytes, 'r');
    };
    allocator_.free_fn = [](void * ctx, void * data) {
      if (!data) return;
      auto * self = static_cast<PayloadMeter *>(ctx);
      Header * block = static_cast<Header *>(data) - 1;
      self->live_ -= block->bytes;
      std::free(block);
    };
  }
  const GRX_Allocator * get() const { return &allocator_; }
  void begin(size_t fail = 0) {
    baseline_ = live_; peak_ = calls_ = moves_ = failed_ = 0;
    fail_ = fail; events_.clear(); armed_ = true;
  }
  size_t live() const { return live_; }
  size_t scratch() const { return live_ - baseline_; }
  size_t peak() const { return peak_; }
  size_t calls() const { return calls_; }
  size_t moves() const { return moves_; }
  size_t failed() const { return failed_; }
  size_t last_calloc_ordinal() const {
    size_t value = 0;
    for (const auto & event : events_) if (event.kind == 'c') value = event.ordinal;
    return value;
  }
  size_t last_calloc_bytes() const {
    size_t value = 0;
    for (const auto & event : events_) if (event.kind == 'c') value = event.bytes;
    return value;
  }
};
struct Case {
  GRX_Syntax syntax;
  std::string pattern, subject;
  GRX_Engine engine = GRX_ENGINE_BACKTRACK;
  bool callback = false;
  std::vector<std::string> captures {};
};
struct Program {
  std::unique_ptr<GRX_Regex, decltype(&grx_regex_free)> regex {nullptr, grx_regex_free};
  std::unique_ptr<GRX_Match, decltype(&grx_match_destroy)> match {nullptr, grx_match_destroy};
  bool create(const Case & row, PayloadMeter & meter) {
    GRX_Regex * r = nullptr;
    if (grx_regex_compile_with_allocator(row.pattern.data(), row.pattern.size(),
            row.syntax, 0, nullptr, meter.get(), nullptr, &r) != GRX_OK) return false;
    regex.reset(r);
    GRX_Match * m = nullptr;
    if (grx_match_create(r, meter.get(), &m) != GRX_OK) return false;
    match.reset(m); return true;
  }
  GRX_Result search(const Case & row, size_t cap, int & matched, size_t steps = 0,
      GRX_CalloutFn observer = nullptr, void * observer_data = nullptr) {
    GRX_Limits limits; grx_limits_default(&limits);
    limits.max_match_memory = cap;
    if (steps) limits.max_steps = steps;
    GRX_SearchOptions options; grx_search_options_default(&options);
    options.engine = row.engine; options.limits = &limits;
    if (row.callback) options.callout = [](const GRX_Callout *, int * fail, void *) -> GRX_Result { *fail = 0; return GRX_OK; };
    if (observer) { options.callout = observer; options.callout_data = observer_data; }
    return grx_regex_search_ex(regex.get(), row.subject.data(), row.subject.size(),
        &options, match.get(), &matched);
  }
  void destroy() { match.reset(); regex.reset(); }
};
void verify_match(const Case & row, GRX_Match * match) {
  GRX_Capture whole;
  ASSERT_EQ(grx_match_span(match, &whole), GRX_OK);
  EXPECT_EQ(whole.start, 0u); EXPECT_EQ(whole.end, row.subject.size());
  for (size_t i = 0; i < row.captures.size(); ++i) {
    GRX_Capture group;
    ASSERT_EQ(grx_match_group(match, i + 1, &group), GRX_OK);
    ASSERT_NE(group.start, GRX_NPOS); ASSERT_LE(group.start, group.end);
    ASSERT_LE(group.end, row.subject.size());
    EXPECT_EQ(row.subject.substr(group.start, group.end - group.start), row.captures[i]);
  }
  EXPECT_EQ(grx_match_error(match)->code, GRX_OK);
  EXPECT_EQ(grx_match_error(match)->diag, GRX_DIAG_NONE);
}
std::vector<Case> workloads() {
  return {
      {GRX_SYNTAX_ECMASCRIPT, "(?=(a))a", "a", GRX_ENGINE_BACKTRACK, false, {"a"}},
      {GRX_SYNTAX_VIM_BACKTRACK, R"(\(a\)\@=a)", "a", GRX_ENGINE_BACKTRACK, false, {"a"}},
      {GRX_SYNTAX_VIM_NFA, R"(\(a\)\@=a)", "a", GRX_ENGINE_BACKTRACK, false, {"a"}},
      {GRX_SYNTAX_POSIX_ERE, "^(a+)$", "aaa", GRX_ENGINE_BACKTRACK, true, {"aaa"}},
      {GRX_SYNTAX_PCRE, "^(a(?1)?b)$", std::string(12, 'a') + std::string(12, 'b'), GRX_ENGINE_BACKTRACK, false, {std::string(12, 'a') + std::string(12, 'b')}},
      {GRX_SYNTAX_PCRE, "^(?:(*MARK:A)a)+b$", std::string(96, 'a') + "b", GRX_ENGINE_BACKTRACK, false, {}},
      {GRX_SYNTAX_VIM_NFA, R"(^a*\%(a\)\@>\9b[@<=]\{0})", "aaab", GRX_ENGINE_BACKTRACK, false, {}},
      {GRX_SYNTAX_VIM_NFA, R"(\(a*\)\@=a*)", std::string(12, 'a'), GRX_ENGINE_BACKTRACK, false, {std::string(12, 'a')}},
      {GRX_SYNTAX_ECMASCRIPT, "^(a|b)*c$", "abababc", GRX_ENGINE_BITSTATE, false, {"b"}},
  };
}

TEST(BacktrackMemory, PeakIsBoundedBeforeAllocationAndRecoversOnSameMatch) {
  size_t refusals = 0;
  for (const auto & row : workloads()) {
    SCOPED_TRACE(row.pattern);
    PayloadMeter meter; Program program;
    ASSERT_TRUE(program.create(row, meter));
    meter.begin(); int matched = 0;
    ASSERT_EQ(program.search(row, 0, matched), GRX_OK); ASSERT_EQ(matched, 1);
    ASSERT_NO_FATAL_FAILURE(verify_match(row, program.match.get()));
    ASSERT_EQ(meter.scratch(), 0u); const size_t sufficient = meter.peak();
    ASSERT_GT(sufficient, 2u);
    for (size_t cap : {size_t(1), sufficient / 2, sufficient - 1, sufficient, sufficient + 1, std::numeric_limits<size_t>::max(), size_t(0)}) {
      SCOPED_TRACE(cap); meter.begin(); matched = 1;
      const GRX_Result result = program.search(row, cap, matched);
      if (cap) { EXPECT_LE(meter.peak(), cap); }
      EXPECT_EQ(meter.scratch(), 0u);
      if (result == GRX_ERR_LIMIT) {
        ++refusals; EXPECT_EQ(matched, 0);
        EXPECT_EQ(grx_match_error(program.match.get())->diag, GRX_DIAG_LIMIT_MATCH_MEMORY);
      } else {
        ASSERT_EQ(result, GRX_OK); ASSERT_EQ(matched, 1);
        ASSERT_NO_FATAL_FAILURE(verify_match(row, program.match.get()));
      }
      if (!cap || cap >= sufficient) { EXPECT_EQ(result, GRX_OK); }
      meter.begin(); matched = 0;
      ASSERT_EQ(program.search(row, sufficient, matched), GRX_OK); ASSERT_EQ(matched, 1);
      EXPECT_LE(meter.peak(), sufficient); EXPECT_EQ(meter.scratch(), 0u);
      ASSERT_NO_FATAL_FAILURE(verify_match(row, program.match.get()));
    }
    program.destroy(); EXPECT_EQ(meter.live(), 0u);
  }
  EXPECT_GT(refusals, 0u); RecordProperty("aggregate_refusals", std::to_string(refusals));
}

TEST(BacktrackMemory, MovingPartialAllocationFailuresRetainOwnershipAndRetry) {
  size_t faults = 0, oom = 0, moves = 0;
  for (const auto & row : workloads()) {
    SCOPED_TRACE(row.pattern);
    PayloadMeter meter; Program program; ASSERT_TRUE(program.create(row, meter));
    meter.begin(); int matched = 0;
    ASSERT_EQ(program.search(row, 0, matched), GRX_OK); ASSERT_EQ(matched, 1);
    const size_t count = meter.calls(); moves += meter.moves(); ASSERT_GT(count, 0u);
    for (size_t fault = 1; fault <= count; ++fault) {
      SCOPED_TRACE(fault); meter.begin(fault); matched = 1;
      const GRX_Result result = program.search(row, 0, matched);
      ASSERT_EQ(meter.failed(), 1u); ++faults; EXPECT_EQ(meter.scratch(), 0u);
      if (result == GRX_ERR_OOM) {
        ++oom; EXPECT_EQ(matched, 0);
        EXPECT_EQ(grx_match_error(program.match.get())->diag, GRX_DIAG_OUT_OF_MEMORY);
      } else { ASSERT_EQ(result, GRX_OK); ASSERT_EQ(matched, 1); verify_match(row, program.match.get()); }
      meter.begin(); matched = 0;
      ASSERT_EQ(program.search(row, 0, matched), GRX_OK); ASSERT_EQ(matched, 1);
      EXPECT_EQ(meter.scratch(), 0u); verify_match(row, program.match.get());
    }
    program.destroy(); EXPECT_EQ(meter.live(), 0u);
  }
  EXPECT_GT(oom, 0u); EXPECT_GT(moves, 0u);
  RecordProperty("allocation_faults", std::to_string(faults));
  RecordProperty("oom_results", std::to_string(oom));
  RecordProperty("moving_reallocations", std::to_string(moves));
}

TEST(BacktrackMemory, OptionalMemoCanDeclineBudgetAndAllocationFailure) {
  const Case row {GRX_SYNTAX_ECMASCRIPT, "^(?:a|aa)*$", std::string(18, 'a') + "b"};
  PayloadMeter meter; Program program; ASSERT_TRUE(program.create(row, meter));
  meter.begin(); int matched = 1;
  ASSERT_EQ(program.search(row, 0, matched), GRX_OK); ASSERT_EQ(matched, 0);
  const size_t peak = meter.peak(); const size_t memo_bytes = meter.last_calloc_bytes();
  const size_t memo_fault = meter.last_calloc_ordinal();
  ASSERT_GT(memo_bytes, 0u); ASSERT_GT(memo_fault, 1u); ASSERT_GT(peak, memo_bytes);
  EXPECT_EQ(meter.scratch(), 0u);
  meter.begin(memo_fault); matched = 1;
  ASSERT_EQ(program.search(row, 0, matched), GRX_OK); EXPECT_EQ(matched, 0);
  EXPECT_EQ(meter.failed(), 1u); EXPECT_EQ(meter.scratch(), 0u);
  EXPECT_EQ(grx_match_error(program.match.get())->diag, GRX_DIAG_NONE);
  const size_t required_peak = meter.peak();
  ASSERT_LT(required_peak, peak);
  meter.begin(); matched = 1;
  ASSERT_EQ(program.search(row, required_peak, matched), GRX_OK); EXPECT_EQ(matched, 0);
  EXPECT_LE(meter.peak(), required_peak); EXPECT_EQ(meter.scratch(), 0u);
  EXPECT_EQ(grx_match_error(program.match.get())->diag, GRX_DIAG_NONE);
  program.destroy(); EXPECT_EQ(meter.live(), 0u);
}

TEST(BacktrackMemory, DistinctCaptureVectorsAtOneAtomicPositionStayBounded) {
  struct Observed {
    std::map<size_t, std::set<std::pair<size_t, size_t>>> by_position;
    bool valid = true;
  } observed;
  GRX_CalloutFn observer = [](const GRX_Callout * callout, int * fail, void * data) -> GRX_Result {
    auto * seen = static_cast<Observed *>(data);
    *fail = 0;
    if (callout->number != 1 || callout->count < 2 ||
        callout->captures[1].start == GRX_NPOS ||
        callout->captures[1].end > callout->position) {
      seen->valid = false;
      return GRX_ERR_INVALID;
    }
    seen->by_position[callout->position].insert({callout->captures[1].start, callout->captures[1].end});
    return GRX_OK;
  };
  size_t max_vectors = 0, pressure_faults = 0;
  const std::string subject = std::string(31, 'a') + "b";
  for (const Case & row : {
      Case {GRX_SYNTAX_PCRE, R"(^(a*)a*(?C1)(?>a)\1b$)", subject, GRX_ENGINE_BACKTRACK, false, {std::string(15, 'a')}},
      Case {GRX_SYNTAX_VIM_BACKTRACK, R"(^\(a*\)a*\%(a\)\@>\1b$)", subject, GRX_ENGINE_BACKTRACK, false, {std::string(15, 'a')}},
      Case {GRX_SYNTAX_VIM_NFA, R"(^\(a*\)a*\%(a\)\@>\1b$)", subject, GRX_ENGINE_BACKTRACK, false, {std::string(15, 'a')}}}) {
    SCOPED_TRACE(row.syntax);
    PayloadMeter meter; Program program; ASSERT_TRUE(program.create(row, meter));
    const bool observing = row.syntax == GRX_SYNTAX_PCRE;
    meter.begin(); int matched = 0;
    ASSERT_EQ(program.search(row, 0, matched, 0, observing ? observer : nullptr, &observed), GRX_OK);
    ASSERT_EQ(matched, 1); verify_match(row, program.match.get());
    ASSERT_EQ(meter.scratch(), 0u); const size_t peak = meter.peak();
    const size_t allocation_count = meter.calls(); ASSERT_GT(peak, 1u);
    ASSERT_GT(allocation_count, 2u);
    if (observing) {
      ASSERT_TRUE(observed.valid);
      for (const auto & entry : observed.by_position) max_vectors = std::max(max_vectors, entry.second.size());
      EXPECT_GE(max_vectors, 17u);
    }
    for (size_t fault : {size_t(1), allocation_count / 2, allocation_count}) {
      SCOPED_TRACE(fault);
      meter.begin(fault); matched = 1;
      ASSERT_EQ(program.search(row, 0, matched, 0, observing ? observer : nullptr, &observed), GRX_ERR_OOM);
      EXPECT_EQ(matched, 0); EXPECT_EQ(meter.failed(), 1u);
      EXPECT_EQ(meter.scratch(), 0u); ++pressure_faults;
      EXPECT_EQ(grx_match_error(program.match.get())->diag, GRX_DIAG_OUT_OF_MEMORY);
      meter.begin(); matched = 0;
      ASSERT_EQ(program.search(row, peak, matched, 0, observing ? observer : nullptr, &observed), GRX_OK);
      ASSERT_EQ(matched, 1); EXPECT_LE(meter.peak(), peak);
      EXPECT_EQ(meter.scratch(), 0u); verify_match(row, program.match.get());
    }
    for (size_t cap : {peak / 2, peak - 1, peak}) {
      meter.begin(); matched = 1;
      const GRX_Result result = program.search(row, cap, matched, 0, observing ? observer : nullptr, &observed);
      EXPECT_LE(meter.peak(), cap); EXPECT_EQ(meter.scratch(), 0u);
      if (result == GRX_ERR_LIMIT) {
        EXPECT_EQ(matched, 0); EXPECT_EQ(grx_match_error(program.match.get())->diag, GRX_DIAG_LIMIT_MATCH_MEMORY);
      } else { ASSERT_EQ(result, GRX_OK); ASSERT_EQ(matched, 1); verify_match(row, program.match.get()); }
      if (cap == peak) { EXPECT_EQ(result, GRX_OK); }
      meter.begin(); matched = 0;
      ASSERT_EQ(program.search(row, 0, matched, 0, observing ? observer : nullptr, &observed), GRX_OK);
      EXPECT_EQ(matched, 1); EXPECT_EQ(meter.scratch(), 0u); verify_match(row, program.match.get());
    }
    program.destroy(); EXPECT_EQ(meter.live(), 0u);
  }
  RecordProperty("distinct_vectors_at_one_atomic_position", std::to_string(max_vectors));
  RecordProperty("capture_pressure_faults_and_retries", std::to_string(pressure_faults));
}

TEST(BacktrackMemory, RequiredFailureSurvivesCombinedBudgetAndAllocatorPressure) {
  size_t attempts = 0, refusals = 0, failures = 0;
  for (const auto & row : workloads()) {
    SCOPED_TRACE(row.pattern);
    PayloadMeter meter; Program program; ASSERT_TRUE(program.create(row, meter));
    meter.begin(); int matched = 0;
    ASSERT_EQ(program.search(row, 0, matched), GRX_OK); ASSERT_EQ(matched, 1);
    const size_t peak = meter.peak(), count = meter.calls();
    ASSERT_GT(peak, 0u); ASSERT_GT(count, 0u);
    for (size_t fraction = 1; fraction < 32; ++fraction) {
      const size_t cap = std::max(size_t(1), peak * fraction / 32);
      SCOPED_TRACE(cap);
      for (size_t fault = 1; fault <= count; ++fault) {
        SCOPED_TRACE(fault); meter.begin(fault); matched = 1;
        const GRX_Result result = program.search(row, cap, matched); ++attempts;
        EXPECT_LE(meter.peak(), cap); EXPECT_EQ(meter.scratch(), 0u);
        if (meter.failed()) {
          ++failures; ASSERT_EQ(result, GRX_ERR_OOM); EXPECT_EQ(matched, 0);
          EXPECT_EQ(meter.calls(), fault);
          EXPECT_EQ(grx_match_error(program.match.get())->diag, GRX_DIAG_OUT_OF_MEMORY);
        } else if (result == GRX_ERR_LIMIT) {
          ++refusals; EXPECT_EQ(matched, 0);
          EXPECT_EQ(grx_match_error(program.match.get())->diag, GRX_DIAG_LIMIT_MATCH_MEMORY);
        } else { ASSERT_EQ(result, GRX_OK); ASSERT_EQ(matched, 1); verify_match(row, program.match.get()); }
        meter.begin(); matched = 0;
        ASSERT_EQ(program.search(row, peak, matched), GRX_OK); ASSERT_EQ(matched, 1);
        EXPECT_LE(meter.peak(), peak); EXPECT_EQ(meter.scratch(), 0u);
        verify_match(row, program.match.get());
      }
    }
    program.destroy(); EXPECT_EQ(meter.live(), 0u);
  }
  EXPECT_GT(failures, 0u); EXPECT_GT(refusals, 0u);
  RecordProperty("combined_pressure_attempts_and_retries", std::to_string(attempts));
  RecordProperty("first_allocation_failures_preserved", std::to_string(failures));
  RecordProperty("budget_refusals_before_fault", std::to_string(refusals));
}

} // namespace
int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
