/**
 * @file
 *
 * libFuzzer harness for the equivalence invariant: two engines, one program,
 * one answer.
 *
 * documentation/design.md section 3.5.4. Any pattern and subject that both
 * engines can run must give the same `matched`, the same group 0, and the
 * same spans for every group. This harness aborts when they do not.
 *
 * It is the strongest cheap test the library has, and it needs no oracle
 * installed. The Pike VM merges threads in lockstep; the backtracker walks
 * one path at a time with an explicit undo stack. They share the instruction
 * set and nothing else, so a disagreement is a defect in one of them, and a
 * mistake they could both make would have to be a mistake in the program -
 * which the reference-implementation harnesses are there for.
 *
 * Both the pattern and the subject come from the fuzzer's bytes, split at the
 * first NUL. That makes a mutation reachable on either side from one corpus
 * entry, which matters because the interesting disagreements need a pattern
 * and a subject that suit each other.
 *
 * Build with: make fuzz-crossengine
 * Run:        make fuzz-run-crossengine FUZZ_TIME=300
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include <ghoti.io/regex/regex.h>

namespace {

/** Report a disagreement in full, then stop. */
void disagreement(const char * what, const char * pattern,
    size_t pattern_size, const uint8_t * subject, size_t subject_size) {
  std::fprintf(stderr, "engines disagree: %s\npattern: ", what);
  for (size_t i = 0; i < pattern_size; i++) {
    std::fprintf(stderr, "%02X", (unsigned char)pattern[i]);
  }
  std::fprintf(stderr, "\nsubject: ");
  for (size_t i = 0; i < subject_size; i++) {
    std::fprintf(stderr, "%02X", subject[i]);
  }
  std::fprintf(stderr, "\n");
  __builtin_trap();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  uint8_t selector = 0;
  if (size) {
    selector = data[0];
    data++;
    size--;
  }

  // The pattern is everything up to the first NUL; the subject is the rest.
  const uint8_t * separator
      = static_cast<const uint8_t *>(std::memchr(data, '\0', size));
  size_t pattern_size = separator ? (size_t)(separator - data) : size;
  const uint8_t * subject = separator ? separator + 1 : data + size;
  size_t subject_size = separator ? size - pattern_size - 1 : 0;

  uint32_t options = GRX_OPT_UTF;
  if (selector & 0x01) {
    options |= GRX_OPT_CASELESS;
  }
  if (selector & 0x02) {
    options |= GRX_OPT_MULTILINE;
  }
  if (selector & 0x04) {
    options |= GRX_OPT_DOTALL;
  }

  GRX_Limits limits;
  grx_limits_default(&limits);
  // Both engines get the same budget, because a limit reached by one and not
  // the other is not a disagreement about the answer - it is a disagreement
  // about the cost, which is the whole reason there are two of them.
  limits.max_steps = 2000000;
  limits.max_backtrack = 100000;

  GRX_Regex * regex = nullptr;
  if (grx_regex_compile_with_allocator(
          reinterpret_cast<const char *>(data), pattern_size,
          GRX_SYNTAX_ECMASCRIPT, options, &limits, nullptr, nullptr, &regex)
          != GRX_OK
      || !regex) {
    return 0;
  }

  GRX_Facts facts;
  grx_regex_facts(regex, &facts);
  if (!facts.is_regular) {
    // Only one engine can run it, so the invariant says nothing.
    grx_regex_free(regex);
    return 0;
  }

  GRX_Match * pike = nullptr;
  GRX_Match * backtrack = nullptr;
  if (grx_match_create(regex, nullptr, &pike) != GRX_OK
      || grx_match_create(regex, nullptr, &backtrack) != GRX_OK) {
    grx_match_destroy(pike);
    grx_match_destroy(backtrack);
    grx_regex_free(regex);
    return 0;
  }

  int pike_matched = 0;
  int backtrack_matched = 0;
  GRX_Result pike_result = grx_regex_search(regex,
      reinterpret_cast<const char *>(subject), subject_size, 0,
      GRX_ENGINE_PIKE, &limits, pike, &pike_matched);
  GRX_Result backtrack_result = grx_regex_search(regex,
      reinterpret_cast<const char *>(subject), subject_size, 0,
      GRX_ENGINE_BACKTRACK, &limits, backtrack, &backtrack_matched);

  // A limit reached by the backtracker and not the Pike VM is expected and is
  // not a disagreement: the exponential engine running out of budget is what
  // the budget is for.
  if (pike_result == GRX_OK && backtrack_result == GRX_OK) {
    if (pike_matched != backtrack_matched) {
      disagreement("one matched and the other did not",
          reinterpret_cast<const char *>(data), pattern_size, subject,
          subject_size);
    }
    if (pike_matched) {
      for (size_t i = 0; i < grx_match_count(pike); i++) {
        GRX_Capture first;
        GRX_Capture second;
        grx_match_group(pike, i, &first);
        grx_match_group(backtrack, i, &second);
        if (first.start != second.start || first.end != second.end) {
          disagreement("a group landed in a different place",
              reinterpret_cast<const char *>(data), pattern_size, subject,
              subject_size);
        }
      }
    }
  }

  grx_match_destroy(pike);
  grx_match_destroy(backtrack);
  grx_regex_free(regex);
  return 0;
}
