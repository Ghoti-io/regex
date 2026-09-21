/**
 * @file
 *
 * grep and sed, in about a hundred lines, to show what the POSIX and GNU
 * dialects are actually for.
 *
 *     posix_stream match  gnu-ere  '^[a-z]+'        < input
 *     posix_stream match  posix-bre '\(ab*\)[ab]*\1' < input
 *     posix_stream subst  gnu-ere  '([a-z]+)@([a-z.]+)' '\2 knows \1' < input
 *
 * The first argument is what to do, the second is the dialect - one of
 * `posix-bre`, `posix-ere`, `gnu-bre`, `gnu-ere` - and the rest are the
 * pattern and, for `subst`, the replacement template.
 *
 * Three things are on show here, and each is a place where a caller who
 * assumed the Perl family would be wrong:
 *
 *   - **The match is the longest, not the first.** `a|ab` against "ab"
 *     reports two characters here and one in every other dialect this
 *     library has. That is POSIX's rule and it is the profile's
 *     `GRX_PREFER_LEFTMOST_LONGEST`, which the Pike VM and the backtracker
 *     both implement - see documentation/dialects.md section 5.1.
 *   - **The template is sed's, not `$1`'s.** `&` is the whole match, `\1` is
 *     a group, and `\&` is a literal ampersand. POSIX defines no replacement
 *     syntax at all, so sed's `s` command is the reference.
 *   - **A basic RE spells its operators with backslashes.** `\(`, `\|` and
 *     `\{` are the operators and the bare characters are literals, which is
 *     the `escaped_specials` axis of the spec table.
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
  // search_next() carries the previous match in and the next one out, which
  // is the only record of where the loop had got to - including the rule for
  // advancing past an empty match, which a caller writing its own loop gets
  // wrong.
  while (grx_regex_search_next(regex, line, length, NULL, match, &matched)
          == GRX_OK
      && matched) {
    GRX_Capture whole;
    grx_match_span(match, &whole);
    printf("  %2zu-%-2zu %.*s", whole.start, whole.end,
        (int)(whole.end - whole.start), line + whole.start);

    size_t groups = grx_match_count(match);
    for (size_t i = 1; i < groups; i++) {
      GRX_Capture capture;
      grx_match_group(match, i, &capture);
      if (capture.start == GRX_NPOS) {
        printf("   \\%zu=-", i);
      }
      else {
        printf("   \\%zu=%.*s", i, (int)(capture.end - capture.start),
            line + capture.start);
      }
    }
    printf("\n");
    found = 1;
  }

  grx_match_destroy(match);
  return found;
}

int main(int argc, char ** argv) {
  if (argc < 4) {
    fprintf(stderr,
        "usage: %s match|subst <dialect> <pattern> [<replacement>]\n"
        "       dialects: posix-bre posix-ere gnu-bre gnu-ere\n",
        argv[0]);
    return 2;
  }

  const int substituting = strcmp(argv[1], "subst") == 0;
  if (!substituting && strcmp(argv[1], "match") != 0) {
    fprintf(stderr, "%s: expected `match` or `subst`\n", argv[0]);
    return 2;
  }
  if (substituting && argc < 5) {
    fprintf(stderr, "%s: subst needs a replacement template\n", argv[0]);
    return 2;
  }

  GRX_Syntax syntax;
  if (grx_syntax_from_name(argv[2], &syntax) != GRX_OK) {
    fprintf(stderr, "%s: unknown dialect %s\n", argv[0], argv[2]);
    return 2;
  }

  GRX_Regex * regex = NULL;
  GRX_Error error;
  grx_error_clear(&error);
  if (grx_regex_compile_with_allocator(argv[3], strlen(argv[3]), syntax, 0,
          NULL, NULL, &error, &regex)
      != GRX_OK) {
    // The diagnostic names the construct and the offset, which for a basic
    // RE is usually "that operator wanted a backslash".
    fprintf(stderr, "%s: %s at byte %zu of the pattern\n", argv[0],
        error.message, error.offset);
    grx_regex_free(regex);
    return 1;
  }

  // Whole lines rather than a streaming reader, because the interesting part
  // is the dialect and not the I/O. cutil's reader so that a failed read is
  // an error and not a short file.
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
    size_t line_length = newline ? (size_t)(newline - (text + at))
                                 : length - at;
    const char * line = text + at;

    if (substituting) {
      GRX_Text out;
      memset(&out, 0, sizeof(out));
      grx_error_clear(&error);
      if (grx_regex_replace(regex, line, line_length, argv[4],
              strlen(argv[4]), GRX_REPLACE_GLOBAL, NULL, NULL, &error, &out)
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
    else {
      printf("%.*s\n", (int)line_length, line);
      if (show_matches(regex, line, line_length)) {
        status = 0;
      }
    }

    at += line_length + (newline ? 1 : 0);
  }

  gcu_file_free(NULL, data);
  grx_regex_free(regex);
  return status;
}
