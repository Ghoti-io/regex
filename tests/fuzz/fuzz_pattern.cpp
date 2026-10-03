/**
 * @file
 *
 * libFuzzer harness for the pattern parser and compiler.
 *
 * The pattern is the untrusted input here, which is the opposite of most of
 * the suite: elsewhere the format is fixed and the bytes are hostile; here
 * the *program* is the bytes. A pattern must not crash the parser, and must
 * not make the matcher run past its limits.
 *
 * Build with: make fuzz-pattern
 * Run:        make fuzz-run-pattern FUZZ_TIME=300
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstddef>
#include <cstdlib>
#include <cstdint>
#include <cstdio>

#include <ghoti.io/regex/regex.h>

#include "fuzz_syntax.h"


extern "C" int LLVMFuzzerTestOneInput(const uint8_t * data, size_t size) {
  // The first byte selects the dialect and the limits, so that the capped
  // paths and every syntax are reachable rather than only the wide-open
  // defaults in one dialect. It also bounds the limits on one input in two,
  // because a capped parse takes a different path out.
  GRX_Limits limits;
  grx_limits_default(&limits);
  GRX_Syntax syntax = GRX_SYNTAX_ECMASCRIPT;
  uint32_t options = GRX_OPT_NONE;

  if (size) {
    uint8_t selector = data[0];
    data++;
    size--;

    // Weighted towards the dialects that have a front end, because a dialect
    // that does not is refused at the first call and the run is spent. One
    // input in eight still picks an arbitrary dialect, so the "named but not
    // built" path stays covered - that path is one line and it is the line
    // that keeps a caller from being told a PCRE pattern is valid.
    //
    // *Which* dialects have a front end changed and this did not: it sent
    // seven inputs in eight to ECMAScript for as long as ECMAScript was the
    // only one built, and went on doing it through WP-18 and WP-23. Seven
    // are built now - the count said six while the list held seven - and
    // they share those seven-eighths, so each gets about an eighth of the
    // campaign rather than a thirty-second of it.
    //
    // GRX_FUZZ_SYNTAX pins one by name, which is what work-packages.md §2's
    // fourth condition - "the pattern fuzzer has run eight hours clean with
    // the dialect selected" - needs in order to be a thing anyone can do.
    if (fuzz_syntax_is_pinned()) {
      syntax = fuzz_pick_syntax(0);
    }
    else if ((selector & 0x07) == 0) {
      syntax = (GRX_Syntax)((selector >> 3) % (unsigned)GRX_SYNTAX_COUNT);
    }
    else {
      syntax = fuzz_pick_syntax((uint32_t)selector >> 3);
    }
    if (selector & 0x40) {
      limits.max_nesting_depth = 8;
      limits.max_nodes = 64;
      limits.max_repeat_count = 16;
      limits.max_class_ranges = 8;
    }
    // The fields the set above leaves alone, so that every enforcement site
    // is reachable rather than the four that were written first.
    // max_lookbehind_length is here because it was reachable from nowhere at
    // all until it was enforced: a limit nothing exercises is a limit nobody
    // finds out is broken.
    //
    // Downward only, never to zero. "Zero means no limit" is checked in
    // tests/unit/test_limits.cpp and in the vectors, where the input is
    // chosen; here it is not, and `a{4294967295}` with max_program_size at
    // zero is an unbounded allocation that would be reported as a crash it
    // is not. The two match-time limits stay at their defaults for the same
    // reason - a fuzzer that can turn off max_steps is a fuzzer that hangs.
    if (selector & 0x80) {
      limits.max_pattern_length = 32;
      limits.max_captures = 4;
      limits.max_program_size = 128;
      limits.max_lookbehind_length = 4;
      limits.max_match_memory = 4096;
      limits.max_subject_length = 64;
    }
    // The option combinations that change the *grammar*, not just the
    // match: ECMAScript reads a different language with `u` than without,
    // and a third one with `v`, where `--` is an operator and a bare `-` is
    // a syntax error. `v` was missing here until a seven-minute run found a
    // defect that a 974,873-run soak had not - the lesson being that a run
    // count is not coverage, and that a grammar no option combination
    // reaches is a grammar no number of runs will test.
    switch ((selector >> 3) & 0x07) {
      case 1: options = GRX_OPT_UTF; break;
      case 2: options = GRX_OPT_CASELESS | GRX_OPT_UTF; break;
      case 3: options = GRX_OPT_CASELESS | GRX_OPT_MULTILINE | GRX_OPT_DOTALL;
        break;
      case 4: options = GRX_OPT_UTF | GRX_OPT_UNICODE_SETS; break;
      case 5:
        options = GRX_OPT_CASELESS | GRX_OPT_UTF | GRX_OPT_UNICODE_SETS;
        break;
      default: options = GRX_OPT_NONE; break;
    }
  }

  GRX_Error error;
  grx_error_clear(&error);

  GRX_Pattern * pattern = nullptr;
  GRX_Result result = grx_pattern_parse_with_allocator(
      reinterpret_cast<const char *>(data), size, syntax, options, &limits,
      nullptr, &error, &pattern);

  if (result == GRX_OK && pattern) {
    FILE * sink = fopen("/dev/null", "w");
    if (sink) {
      (void)grx_pattern_dump(pattern, sink);
      fclose(sink);
    }

    GRX_Regex * regex = nullptr;
    if (grx_regex_compile_pattern(pattern, &limits, nullptr, &error, &regex)
            == GRX_OK
        && regex) {
      // Run the compiled program against the pattern text itself: any subject
      // will do, and this one is already here and already arbitrary.
      GRX_Match * match = nullptr;
      if (grx_match_create(regex, nullptr, &match) == GRX_OK) {
        int matched = 0;
        (void)grx_regex_search(regex, reinterpret_cast<const char *>(data),
            size, 0, GRX_ENGINE_AUTO, &limits, match, &matched);
        grx_match_destroy(match);
      }
      grx_regex_free(regex);
    }
  }

  grx_pattern_free(pattern);
  return 0;
}
