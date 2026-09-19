/**
 * @file
 *
 * This library, as a syntax oracle: reads patterns, prints verdicts.
 *
 * One process for a whole corpus, because the corpus is millions of patterns
 * and a process per pattern is a hundred times the cost of the parse. Each
 * input line is `<flags>\t<pattern as hex>`; the pattern is hex so that a
 * newline, a NUL or invalid UTF-8 can all be in a pattern without the
 * transport needing an escape of its own.
 *
 * Each output line is `ok`, `err <diag>` or `limit <diag>`. The three are
 * kept apart because a comparison against a reference implementation has to
 * tell "the reference rejects this too" from "this library has a cap the
 * reference does not" - the second is a recorded deviation, not a defect.
 *
 * Used by tools/oracle/syntax_diff.py. See documentation/testing.md.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/regex.h>
#include <stdio.h>
#include <string.h>

/** The longest pattern a line may carry. */
#define MAX_PATTERN 32768

static int unhex(int c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

/** Map a flag string in a dialect's own alphabet onto option bits. */
static uint32_t options_from_flags(const char * flags, GRX_Syntax syntax) {
  (void)syntax;
  uint32_t options = 0;
  for (const char * f = flags; *f; f++) {
    switch (*f) {
      case 'i': options |= GRX_OPT_CASELESS; break;
      case 'm': options |= GRX_OPT_MULTILINE; break;
      case 's': options |= GRX_OPT_DOTALL; break;
      case 'x': options |= GRX_OPT_EXTENDED; break;
      case 'u': options |= GRX_OPT_UTF; break;
      case 'v': options |= GRX_OPT_UNICODE_SETS | GRX_OPT_UTF; break;
      default: break;
    }
  }
  return options;
}

int main(int argc, char ** argv) {
  GRX_Syntax syntax = GRX_SYNTAX_ECMASCRIPT;
  if (argc > 1 && grx_syntax_from_name(argv[1], &syntax) != GRX_OK) {
    fprintf(stderr, "unknown dialect: %s\n", argv[1]);
    return 2;
  }

  // The caps are lifted: a syntax comparison must not be confounded by a
  // resource limit, because a pattern refused for its repeat count never
  // gets far enough to show whether its syntax was good. The deviation the
  // caps represent is measured separately (documentation/dialects.md
  // section 7).
  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_repeat_count = 0;
  limits.max_nodes = 0;
  limits.max_nesting_depth = 0;

  // A fixed buffer rather than getline(), which is POSIX and not C17: this
  // file is compiled with the same -pedantic-errors the library is. Two hex
  // digits per byte, plus the flags and the newline.
  static char line[2 * MAX_PATTERN + 64];
  char pattern[MAX_PATTERN];

  while (fgets(line, (int)sizeof(line), stdin)) {
    // A record too long for the buffer arrives without its newline, and its
    // tail would be read as the next record. Drain it and say so, rather
    // than parsing the prefix that fit: this driver's answer is compared
    // against what Node said about the *whole* pattern, and a shortened one
    // would make the two agree about different questions.
    if (!strchr(line, '\n') && !feof(stdin)) {
      int c;
      while ((c = fgetc(stdin)) != EOF && c != '\n') {
      }
      printf("toolong\n");
      fflush(stdout);
      continue;
    }

    char * tab = strchr(line, '\t');
    if (!tab) {
      continue;
    }
    *tab = '\0';

    uint32_t options = options_from_flags(line, syntax);
    const char * hex = tab + 1;
    size_t length = 0;
    int too_long = 0;
    while (hex[0] && hex[1]) {
      if (length == sizeof(pattern)) {
        too_long = 1;
        break;
      }
      int high = unhex(hex[0]);
      int low = unhex(hex[1]);
      if (high < 0 || low < 0) {
        break;
      }
      pattern[length++] = (char)((high << 4) | low);
      hex += 2;
    }
    if (too_long) {
      printf("toolong\n");
      fflush(stdout);
      continue;
    }

    GRX_Error error;
    grx_error_clear(&error);
    GRX_Pattern * parsed = NULL;
    GRX_Result result = grx_pattern_parse_with_allocator(pattern, length,
        syntax, options, &limits, NULL, &error, &parsed);

    if (result == GRX_OK) {
      printf("ok\n");
      grx_pattern_free(parsed);
    }
    else if (result == GRX_ERR_LIMIT) {
      printf("limit %d\n", (int)error.diag);
    }
    else {
      printf("err %d\n", (int)error.diag);
    }
  }

  return 0;
}
