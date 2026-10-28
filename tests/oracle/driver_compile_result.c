/**
 * @file
 *
 * Test the actual oracle driver with deterministic compile-entry failures.
 *
 * Copyright 2026 by Corey Pennycuff
 */
#include <ghoti.io/regex/regex.h>
#include <string.h>

/* Bind the real namespaced entry before overriding its public macro. */
static GRX_Result (*const grx_oracle_real_compile)(const char *, size_t,
    GRX_Syntax, uint32_t, const GRX_Limits *, const GRX_Allocator *,
    GRX_Error *, GRX_Regex **) = grx_regex_compile_with_allocator;

static GRX_Result grx_oracle_test_compile(const char * pattern, size_t length,
    GRX_Syntax syntax, uint32_t options, const GRX_Limits * limits,
    const GRX_Allocator * allocator, GRX_Error * error, GRX_Regex ** regex) {
  static const struct {
    const char * pattern;
    GRX_Result result;
    GRX_Diag diag;
  } faults[] = {
      {"__grx_test_compile_syntax__", GRX_ERR_SYNTAX,
          GRX_DIAG_UNMATCHED_OPEN_PAREN},
      {"__grx_test_compile_oom__", GRX_ERR_OOM, GRX_DIAG_OUT_OF_MEMORY},
      {"__grx_test_compile_limit__", GRX_ERR_LIMIT,
          GRX_DIAG_LIMIT_PROGRAM_SIZE},
      {"__grx_test_compile_unsupported__", GRX_ERR_UNSUPPORTED,
          GRX_DIAG_DIALECT_NOT_IMPLEMENTED},
      {"__grx_test_compile_unbuilt_construct__", GRX_ERR_UNSUPPORTED,
          GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED},
      {"__grx_test_compile_unavailable__", GRX_ERR_UNSUPPORTED,
          GRX_DIAG_PREVIOUS_SUBSTITUTE_UNAVAILABLE},
  };
  for (size_t i = 0; i < sizeof(faults) / sizeof(faults[0]); i++) {
    if (length == strlen(faults[i].pattern) &&
        memcmp(pattern, faults[i].pattern, length) == 0) {
      if (regex) {
        *regex = NULL;
      }
      if (error) {
        grx_error_clear(error);
        error->code = faults[i].result;
        error->diag = faults[i].diag;
      }
      return faults[i].result;
    }
  }
  return grx_oracle_real_compile(pattern, length, syntax, options, limits,
      allocator, error, regex);
}

#undef grx_regex_compile_with_allocator
#define grx_regex_compile_with_allocator grx_oracle_test_compile

#ifdef GRX_ORACLE_REPLACE_DRIVER
#include "../../tools/oracle/grx_replace.c"
#else
#include "../../tools/oracle/grx_match.c"
#endif
