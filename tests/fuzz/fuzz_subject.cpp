/**
 * @file
 *
 * libFuzzer harness for the *subject*, against patterns that already work.
 *
 * The sibling harness fuzzes the pattern, which finds parser and compiler
 * defects. This one holds a small corpus of interesting patterns fixed and
 * fuzzes the bytes they are run against, which is where the engines' own
 * defects live: a class boundary, a UTF-8 sequence that ends mid-character, a
 * subject that is all one character for a pattern with a loop in it.
 *
 * Splitting the two matters because a fuzzer that varies both at once spends
 * almost all its time on patterns that do not compile, and so almost none on
 * the engines.
 *
 * Build with: make fuzz-subject
 * Run:        make fuzz-run-subject FUZZ_TIME=300
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdint>
#include <cstring>

#include <ghoti.io/regex/regex.h>

namespace {

/**
 * The patterns the subject is run against.
 *
 * Chosen for what they make an engine do rather than for what they mean: a
 * loop whose body can match empty, a class with a boundary at a UTF-8 length
 * change, a backreference, a lookbehind, an alternation whose branches
 * overlap. Each is a construct with a rule attached to it.
 */
const char * const kPatterns[] = {
  "a(b|c)*d",
  "(a*)*b",
  "(a*)+b",
  "((a)|b)+",
  "^(a+)(b+)$",
  "[a-c\\d\\s]+",
  "[^\\w]{2,5}",
  "\\b\\w+\\b",
  "(?:ab|a)(c|)",
  "a{2,4}?b",
  "(a|b)\\1",
  "(?<=ab)c",
  "(?<!x)y",
  "(?=(a+))a",
  "\\p{L}+\\p{Nd}*",
  ".*",
  ".+?x",
  "(?:)",
  "$",
  "^",
};

const size_t kPatternCount = sizeof(kPatterns) / sizeof(*kPatterns);

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  // The first byte chooses the pattern, the options and whether the limits
  // are tight, so that every pattern and both the capped and uncapped paths
  // are reachable from a corpus of subjects.
  uint8_t selector = 0;
  if (size) {
    selector = data[0];
    data++;
    size--;
  }

  // Both budgets are small, and deliberately. The default max_steps is ten
  // million, and this harness runs every input on both engines and both
  // entry points - so one adversarial subject against the backtracker costs
  // more than ten thousand ordinary ones. Measured: the defaults gave 83
  // executions a second, which is a fuzzer that explores almost nothing.
  // A fuzzer's job here is to reach many *shapes* of input; the limits' own
  // arithmetic is unit-tested.
  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_steps = (selector & 0x40) ? 2000 : 50000;
  limits.max_backtrack = (selector & 0x40) ? 200 : 5000;
  limits.max_match_memory = 256 * 1024;

  uint32_t options = GRX_OPT_UTF;
  if (selector & 0x20) {
    options |= GRX_OPT_CASELESS;
  }
  if (selector & 0x80) {
    options |= GRX_OPT_MULTILINE;
  }

  const char * pattern = kPatterns[(selector >> 1) % kPatternCount];

  GRX_Error error;
  grx_error_clear(&error);
  GRX_Regex * regex = nullptr;
  if (grx_regex_compile_with_allocator(pattern, std::strlen(pattern),
          GRX_SYNTAX_ECMASCRIPT, options, &limits, nullptr, &error, &regex)
          != GRX_OK
      || !regex) {
    return 0;
  }

  GRX_Match * match = nullptr;
  if (grx_match_create(regex, nullptr, &match) == GRX_OK) {
    // Both entry points and both engines, so that a defect reachable from one
    // of the four is reachable from this harness.
    for (int anchored = 0; anchored < 2; anchored++) {
      for (int which = 0; which < 2; which++) {
        GRX_Engine engine
            = which ? GRX_ENGINE_BACKTRACK : GRX_ENGINE_PIKE;
        int matched = 0;
        GRX_Result result = anchored
            ? grx_regex_match(regex, reinterpret_cast<const char *>(data),
                  size, 0, engine, &limits, match, &matched)
            : grx_regex_search(regex, reinterpret_cast<const char *>(data),
                  size, 0, engine, &limits, match, &matched);

        // A reported match must name a span inside the subject. An engine
        // that reported one outside it would be handing a caller an offset
        // to read from, which is the defect shape that matters most here.
        if (result == GRX_OK && matched) {
          for (size_t i = 0; i < grx_match_count(match); i++) {
            GRX_Capture capture;
            if (grx_match_group(match, i, &capture) != GRX_OK) {
              __builtin_trap();
            }
            if (capture.start == GRX_NPOS) {
              continue;
            }
            if (capture.start > capture.end || capture.end > size) {
              __builtin_trap();
            }
          }
        }
      }
    }
    grx_match_destroy(match);
  }

  grx_regex_free(regex);
  return 0;
}
