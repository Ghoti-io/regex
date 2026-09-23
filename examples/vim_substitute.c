/**
 * @file
 *
 * Vim's `:s` over a string, to show what the four magic levels and the
 * replacement's case markers actually do.
 *
 *     vim_substitute '\(\w\+\)\s\+\(\w\+\)' '\2 \1'      < input
 *     vim_substitute '\v(\w+)\s+(\w+)'      '\u\2 \l\1'  < input
 *     vim_substitute '\Ma.c'                '[&]'        < input
 *     vim_substitute --show 'r\%[ead]'                   < input
 *
 * With `--show` the pattern is matched and every match printed with its
 * groups; otherwise the second argument is a replacement template and every
 * match in the line is replaced.
 *
 * Four things are on show, and each is a place where a caller who assumed
 * the Perl family would be wrong:
 *
 *   - **The grammar is chosen inside the pattern.** `\v`, `\m`, `\M` and
 *     `\V` decide which characters are operators, they take effect where
 *     they stand, and they are scoped to nothing: `\v(a\m)b` is "E54:
 *     Unmatched \(" in vim, because the `\m` makes the `)` an ordinary
 *     character before the group it would have closed is closed. See
 *     documentation/dialects.md section 3.
 *   - **`\zs` and `\ze` move the reported match**, without changing what
 *     has to be there: `r\zsead` matches "read" and reports "ead".
 *   - **The template changes case.** `\u` and `\l` take the next character,
 *     `\U` and `\L` run until `\E`, and a one-character marker suspends a
 *     run for exactly one character - `\Uab\lcd` is "ABcD". No other
 *     dialect here has a template piece that emits nothing and still
 *     changes the answer.
 *   - **The search loop is vim's.** After an empty match it advances a
 *     character rather than retrying without one, and a match reaching the
 *     end of the subject ends it: `b*` over "ab" gives "<>a<>" where node,
 *     perl and Python all give "<>a<><>".
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/cutil/file.h>
#include <ghoti.io/regex/regex.h>
#include <stdio.h>
#include <string.h>

/** Print every match in one line, with the groups it captured. */
static int show_matches(const GRX_Regex * regex, const char * line,
    size_t length) {
  GRX_Match * match = NULL;
  if (grx_match_create(regex, NULL, &match) != GRX_OK) {
    return 0;
  }

  int found = 0;
  int matched = 0;
  // grx_regex_search_next() carries the previous match in and the next one
  // out, which is where this dialect's own iteration rule lives: a caller
  // writing the loop by hand would have to know it.
  while (grx_regex_search_next(regex, line, length, NULL, match, &matched)
          == GRX_OK
      && matched) {
    GRX_Capture whole;
    grx_match_span(match, &whole);
    printf("  %2zu-%-2zu %.*s", whole.start, whole.end,
        (int)(whole.end - whole.start), line + whole.start);

    size_t groups = grx_match_count(match);
    for (size_t i = 1; i < groups; i++) {
      GRX_Capture group;
      if (grx_match_group(match, i, &group) != GRX_OK
          || group.start == GRX_NPOS) {
        printf("  \\%zu=-", i);
        continue;
      }
      printf("  \\%zu=%.*s", i, (int)(group.end - group.start),
          line + group.start);
    }
    printf("\n");
    found = 1;
  }

  grx_match_destroy(match);
  return found;
}

int main(int argc, char ** argv) {
  int showing = argc > 1 && strcmp(argv[1], "--show") == 0;
  int first = showing ? 2 : 1;
  if (argc < first + (showing ? 1 : 2)) {
    fprintf(stderr,
        "usage: %s <pattern> <replacement>\n"
        "       %s --show <pattern>\n",
        argv[0], argv[0]);
    return 2;
  }

  GRX_Regex * regex = NULL;
  GRX_Error error;
  grx_error_clear(&error);
  if (grx_regex_compile_with_allocator(argv[first], strlen(argv[first]),
          GRX_SYNTAX_VIM, 0, NULL, NULL, &error, &regex)
      != GRX_OK) {
    // The diagnostic names the construct and the offset. For this dialect
    // that is usually "the level in force here spells that operator the
    // other way".
    fprintf(stderr, "%s: %s at byte %zu of the pattern\n", argv[0],
        error.message, error.offset);
    grx_regex_free(regex);
    return 1;
  }

  // Whole lines rather than a streaming reader, because the interesting
  // part is the dialect and not the I/O. Note that a line break is an
  // ordinary character to this dialect - the subject is a string and not a
  // buffer - so splitting here is the caller's choice and not the
  // library's.
  void * data = NULL;
  size_t length = 0;
  if (gcu_file_read("/dev/stdin", GCU_FILE_UNLIMITED, NULL, &data, &length)
      != GCU_FILE_OK) {
    fprintf(stderr, "%s: cannot read standard input\n", argv[0]);
    grx_regex_free(regex);
    return 1;
  }

  int status = 1;
  const char * text = (const char *)data;
  size_t at = 0;
  while (at < length) {
    const char * newline = memchr(text + at, '\n', length - at);
    size_t line_length
        = newline ? (size_t)(newline - (text + at)) : length - at;
    const char * line = text + at;

    if (showing) {
      printf("%.*s\n", (int)line_length, line);
      if (show_matches(regex, line, line_length)) {
        status = 0;
      }
    }
    else {
      GRX_Text out;
      memset(&out, 0, sizeof(out));
      grx_error_clear(&error);
      if (grx_regex_replace(regex, line, line_length, argv[first + 1],
              strlen(argv[first + 1]), GRX_REPLACE_GLOBAL, NULL, NULL,
              &error, &out)
          == GRX_OK) {
        printf("%.*s\n", (int)out.length, out.data);
        grx_text_free(&out);
        status = 0;
      }
      else {
        fprintf(stderr, "%s: %s\n", argv[0], error.message);
        status = 1;
        break;
      }
    }

    at += line_length + (newline ? 1 : 0);
  }

  gcu_file_free(NULL, data);
  grx_regex_free(regex);
  return status;
}
