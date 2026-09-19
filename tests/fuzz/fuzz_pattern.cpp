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
#include <cstdint>
#include <cstdio>

#include <ghoti.io/regex/regex.h>

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
    syntax = (selector & 0x07) == 0
        ? (GRX_Syntax)((selector >> 3) % (unsigned)GRX_SYNTAX_COUNT)
        : GRX_SYNTAX_ECMASCRIPT;
    if (selector & 0x40) {
      limits.max_nesting_depth = 8;
      limits.max_nodes = 64;
      limits.max_repeat_count = 16;
      limits.max_class_ranges = 8;
    }
    // The option combinations that change the *grammar*, not just the
    // match: ECMAScript reads a different language with `u` than without.
    switch ((selector >> 3) & 0x03) {
      case 1: options = GRX_OPT_UTF; break;
      case 2: options = GRX_OPT_CASELESS | GRX_OPT_UTF; break;
      case 3: options = GRX_OPT_CASELESS | GRX_OPT_MULTILINE | GRX_OPT_DOTALL;
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
