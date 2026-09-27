/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Regex.
 *
 * Ghoti.io Regex is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Regex is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * This library as an I-Regexp Boolean: does the whole subject match?
 *
 * Each input line is `<pattern hex>\t<subject hex>`, and each output line is
 * one word:
 *
 *   `true`           the whole subject matches
 *   `false`          it does not
 *   `err <diag>`     the pattern is not an I-Regexp
 *   `limit <diag>`   a cap stopped it, so nothing was decided
 *   `error <code>`   the search itself failed
 *
 * **Why this is not `grx_match`.** The question here is XSD's, which RFC 9485
 * section 4 adopts: the *whole* string matches, or it does not. That is
 * `GRX_OPT_ANCHORED | GRX_OPT_ANCHORED_END`, and grx_match.c takes its options
 * from a flag string in the dialect's own alphabet - which for this dialect is
 * empty, there being no flag anywhere in RFC 9485's grammar. So the two option
 * bits have no letter to arrive as, and a driver that knows what question it is
 * asking is better than a letter invented for one.
 *
 * `limit` is a separate answer from `err` for the reason the whole dialect
 * exists: a caller mapping this onto JSONPath turns an invalid regexp into a
 * false logical result (RFC 9535 section 2.4.6) and must not do that with a
 * budget that ran out. The parse caps are lifted, as grx_syntax.c lifts them,
 * so that a comparison is not confounded by a policy the reference has no
 * counterpart for; the match-time caps stay, because a driver that can be
 * made to hang is one nobody runs twice.
 */

#include <ghoti.io/regex/regex.h>
#include <stdio.h>
#include <string.h>

/** The longest pattern or subject a line may carry. */
#define MAX_TEXT 65536

static int unhex_digit(int c) {
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

/** Decode hex into `out`, returning the length, or -1 if it does not fit. */
static long unhex(const char * hex, char * out, size_t capacity) {
  size_t length = 0;
  while (hex[0] && hex[0] != '\t' && hex[0] != '\n' && hex[1]) {
    if (length == capacity) {
      return -1;
    }
    int high = unhex_digit(hex[0]);
    int low = unhex_digit(hex[1]);
    if (high < 0 || low < 0) {
      break;
    }
    out[length++] = (char)((high << 4) | low);
    hex += 2;
  }
  return (long)length;
}

int main(void) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_repeat_count = 0;
  limits.max_nodes = 0;
  limits.max_nesting_depth = 0;
  limits.max_program_size = 0;

  static char line[2 * MAX_TEXT + 64];
  static char pattern[MAX_TEXT];
  static char subject[MAX_TEXT];

  while (fgets(line, (int)sizeof(line), stdin)) {
    if (!strchr(line, '\n') && !feof(stdin)) {
      int c;
      while ((c = fgetc(stdin)) != EOF && c != '\n') {
      }
      printf("toolong\n");
      fflush(stdout);
      continue;
    }

    char * tab = strchr(line, '\t');
    long pattern_length = unhex(line, pattern, sizeof(pattern));
    long subject_length = tab ? unhex(tab + 1, subject, sizeof(subject)) : 0;
    if (pattern_length < 0 || subject_length < 0) {
      printf("toolong\n");
      fflush(stdout);
      continue;
    }

    GRX_Error error;
    grx_error_clear(&error);
    GRX_Regex * regex = NULL;
    GRX_Result compiled = grx_regex_compile_with_allocator(pattern,
        (size_t)pattern_length, GRX_SYNTAX_IREGEXP,
        GRX_OPT_ANCHORED | GRX_OPT_ANCHORED_END, &limits, NULL, &error,
        &regex);
    if (compiled == GRX_ERR_LIMIT) {
      printf("limit %d\n", (int)error.diag);
    }
    else if (compiled != GRX_OK) {
      printf("err %d\n", (int)error.diag);
    }
    else {
      int matched = 0;
      // No match object: the answer is a Boolean and nothing reads a span, so
      // the engines skip the capture bookkeeping - which is also what a
      // JSONPath caller does.
      GRX_Result searched = grx_regex_search(regex, subject,
          (size_t)subject_length, 0, GRX_ENGINE_AUTO, &limits, NULL, &matched);
      if (searched != GRX_OK) {
        printf("error %d\n", (int)searched);
      }
      else {
        printf("%s\n", matched ? "true" : "false");
      }
    }
    grx_regex_free(regex);
    fflush(stdout);
  }

  return 0;
}
