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
  // One input in four reads the UnicodeSets grammar, whose classes lower to
  // something quite different: an alternation of literal sequences rather
  // than a set. The engines have to agree about those too, and until this
  // line they were never asked.
  if ((selector & 0x18) == 0x18) {
    options |= GRX_OPT_UNICODE_SETS;
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

  // Every engine that can run this program, compared against the first.
  // Which engines those are is a property of the program, not a fixed pair:
  // the bit-state engine joined the set in WP-13 and its whole claim - that
  // skipping an (instruction, position) already tried changes no answer -
  // is checked here and nowhere else at this scale.
  GRX_Engine engines[3];
  size_t engine_count = 0;
  if (facts.is_regular) {
    engines[engine_count++] = GRX_ENGINE_PIKE;
  }
  if (!facts.has_backreference && !facts.has_lookaround
      && !facts.has_recursion) {
    engines[engine_count++] = GRX_ENGINE_BITSTATE;
  }
  engines[engine_count++] = GRX_ENGINE_BACKTRACK;
  if (engine_count < 2) {
    // Only one engine can run it, so the invariant says nothing.
    grx_regex_free(regex);
    return 0;
  }

  GRX_Match * first_match = nullptr;
  int first_matched = 0;
  size_t compared = 0;

  for (size_t i = 0; i < engine_count; i++) {
    GRX_Match * match = nullptr;
    if (grx_match_create(regex, nullptr, &match) != GRX_OK) {
      break;
    }

    int matched = 0;
    GRX_Result result = grx_regex_search(regex,
        reinterpret_cast<const char *>(subject), subject_size, 0, engines[i],
        &limits, match, &matched);

    // A limit reached by one engine and not another is expected and is not a
    // disagreement: the exponential engine running out of budget is what the
    // budget is for, and the bit-state engine refusing a bitmap that will
    // not fit is what max_match_memory is for.
    if (result != GRX_OK) {
      grx_match_destroy(match);
      continue;
    }

    if (!compared) {
      first_match = match;
      first_matched = matched;
      compared = 1;
      continue;
    }

    if (matched != first_matched) {
      disagreement("one matched and the other did not",
          reinterpret_cast<const char *>(data), pattern_size, subject,
          subject_size);
    }
    if (matched) {
      for (size_t group = 0; group < grx_match_count(match); group++) {
        GRX_Capture a;
        GRX_Capture b;
        grx_match_group(first_match, group, &a);
        grx_match_group(match, group, &b);
        if (a.start != b.start || a.end != b.end) {
          disagreement("a group landed in a different place",
              reinterpret_cast<const char *>(data), pattern_size, subject,
              subject_size);
        }
      }
    }
    grx_match_destroy(match);
  }

  grx_match_destroy(first_match);
  grx_regex_free(regex);
  return 0;
}
