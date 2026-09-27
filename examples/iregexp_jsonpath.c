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
 * JSONPath's `match()` and `search()` over an I-Regexp, which is what RFC
 * 9535 sections 2.4.6 and 2.4.7 specify and RFC 9485 defines the pattern for.
 *
 *     iregexp_jsonpath '[a-z]+' hello HELLO 'hello there'
 *     iregexp_jsonpath '^a$' '^a$' a
 *     iregexp_jsonpath '\d'
 *
 * Four things are on show, and each is a place where a caller who reached for
 * ECMAScript would be wrong:
 *
 *   - **`match` and `search` are one compiled pattern and two options.**
 *     `match` is the whole string, which is `GRX_OPT_ANCHORED` and
 *     `GRX_OPT_ANCHORED_END` together; `search` is any substring, which is
 *     neither. The dialect anchors nothing itself, deliberately: a front end
 *     that wrapped the pattern could answer one of the two questions and not
 *     the other.
 *   - **A pattern outside RFC 9485 is refused, with an offset.** `\d`, `\w`,
 *     `(?:a)`, `a*?`, `[a-z-[aeiou]]` and a bare `{` are all legal
 *     ECMAScript and none is an I-Regexp. That refusal is the whole value of
 *     a checking implementation (RFC 9485 section 3.1): a caller learns their
 *     pattern is not interoperable instead of learning nothing.
 *   - **A refusal and a limit are different answers.** JSONPath turns an
 *     invalid regexp into `false`. A `GRX_ERR_LIMIT` decided nothing, so it
 *     must not become one - `a{20,200000}` is the pattern that shows it, and
 *     this program prints the two outcomes differently.
 *   - **`^` and `$` are ordinary characters.** `^a$` matches the three
 *     characters and not `a`, which is why RFC 9485's own ECMAScript mapping
 *     cannot be applied to the raw text.
 */

#include <stdio.h>
#include <string.h>

#include <ghoti.io/regex/regex.h>

/** Compile one pattern with the two option settings the two questions need. */
static int compile_one(
    const char * pattern, uint32_t options, GRX_Regex ** out_regex) {
  GRX_Error error;
  grx_error_clear(&error);
  GRX_Result result = grx_regex_compile_with_allocator(pattern,
      strlen(pattern), GRX_SYNTAX_IREGEXP, options, NULL, NULL, &error,
      out_regex);
  if (result == GRX_OK) {
    return 0;
  }

  // The two failures a caller has to keep apart. RFC 9535 says a function
  // whose regexp argument is invalid yields a false logical result; a budget
  // that ran out yielded no result at all, and answering `false` for it would
  // report "this string does not match" about a question nobody answered.
  if (result == GRX_ERR_SYNTAX) {
    printf("not an I-Regexp: %s\n", error.message);
    printf("  JSONPath: match() and search() are both false\n");
  }
  else {
    printf("undecided (%s): %s\n", grx_result_string(result), error.message);
    printf("  JSONPath: neither true nor false; report the limit\n");
  }
  return 1;
}

int main(int argc, char ** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: %s <i-regexp> [subject ...]\n", argv[0]);
    return 2;
  }
  const char * pattern = argv[1];

  // One compile per question, from the same text. The whole-string one is
  // what RFC 9485 section 4's Boolean semantics asks for; the substring one
  // is JSONPath's second function and has no counterpart in the RFC at all.
  GRX_Regex * whole = NULL;
  GRX_Regex * anywhere = NULL;
  if (compile_one(pattern, GRX_OPT_ANCHORED | GRX_OPT_ANCHORED_END, &whole)) {
    return 1;
  }
  if (compile_one(pattern, GRX_OPT_NONE, &anywhere)) {
    grx_regex_free(whole);
    return 1;
  }

  // `is_regular` is read rather than asserted: every I-Regexp is regular,
  // there being no backreference and no lookaround in the grammar, and a
  // program that printed the claim instead of the fact would go on printing
  // it if that ever stopped being true.
  GRX_Facts facts;
  int regular = grx_regex_facts(anywhere, &facts) == GRX_OK && facts.is_regular;
  printf("i-regexp: %s\n", pattern);
  printf("  %zu instruction(s), %zu capture(s), regular: %s\n",
      grx_regex_program_size(anywhere), grx_regex_capture_count(anywhere),
      regular ? "yes" : "no");

  if (argc == 2) {
    printf("  (no subjects given)\n");
  }
  for (int i = 2; i < argc; i++) {
    const char * subject = argv[i];
    size_t length = strlen(subject);
    int matched = 0;
    int found = 0;

    // Neither call asks for spans: JSONPath wants a Boolean, and a NULL match
    // object is how this library is told that. The engines skip the capture
    // bookkeeping when nothing will read it.
    GRX_Result a = grx_regex_search(whole, subject, length, 0,
        GRX_ENGINE_AUTO, NULL, NULL, &matched);
    GRX_Result b = grx_regex_search(anywhere, subject, length, 0,
        GRX_ENGINE_AUTO, NULL, NULL, &found);
    if (a != GRX_OK || b != GRX_OK) {
      printf("  %-20s undecided: %s\n", subject,
          grx_result_string(a != GRX_OK ? a : b));
      continue;
    }
    printf("  %-20s match=%-5s search=%s\n", subject,
        matched ? "true" : "false", found ? "true" : "false");
  }

  grx_regex_free(anywhere);
  grx_regex_free(whole);
  return 0;
}
