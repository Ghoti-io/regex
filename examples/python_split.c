/**
 * @file
 *
 * CPython's `re.split` and `re.sub` over a string, because Python's
 * splitting rule is a third one and not a variant of the other two.
 *
 *     python_split '\s*,\s*'                 < input
 *     python_split --maxsplit 1 ','          < input
 *     python_split --sub '(\w+)@(\w+)' '\g<2>/\g<1>'  < input
 *     python_split --show '(?P<user>\w+)@(?P<host>\w+)'  < input
 *
 * Four things are on show, and each is a place where a caller who assumed
 * ECMAScript or Perl would be wrong:
 *
 *   - **Splitting is the dialect's, not one rule for everybody.**
 *     `GRX_Syntax` selects it through `GRX_SyntaxProfile::split`, and the
 *     three values are genuinely three answers rather than two and a blend
 *     (documentation/dialects.md section 5.17). Over `""` with `x*`:
 *     Python gives two empty pieces, ECMAScript gives none, Perl gives
 *     none. Over `",a,"` with `,`: Python and ECMAScript give three pieces,
 *     Perl gives two, having dropped the trailing empty one.
 *   - **`maxsplit` counts splits, and the remainder is kept.** `--maxsplit
 *     1` over `"a,b,c"` gives `a` and `b,c` - two pieces, where
 *     ECMAScript's `split(",", 1)` gives one piece and throws the rest
 *     away. `--maxsplit 0` means *no limit* here and *no pieces* there. The
 *     same number means opposite things in the two libraries, which is why
 *     it is a per-dialect rule and not an argument the caller converts.
 *   - **Every match separates, including a zero-width one.** `re` gained
 *     that in 3.7 and it is the opposite of ECMAScript's rule, so `x*` over
 *     `"ab"` is four pieces here - `""`, `a`, `b`, `""` - and two there.
 *   - **The template is `\g<n>`, and a number has no fallback.** `\1` and
 *     `\g<1>` are a group; `\g<name>` is a named one; `\0` is the whole
 *     match. A reference to a group the pattern does not have is an
 *     *error*, where ECMAScript would quietly copy the text through
 *     (section 5.11).
 *   - **A name is `(?P<n>...)`.** `(?<n>...)` without the `P` is "unknown
 *     extension" here, and `\w` is Python's own word set - `str.isalnum()`
 *     plus `_`, which is neither Perl's nor ECMAScript's and differs from
 *     Perl's by 3,506 code points (section 5.9).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/cutil/file.h>
#include <ghoti.io/regex/regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** Print one piece, or `<unset>` for a group that did not participate. */
static void print_piece(const char * line, const GRX_Capture * piece) {
  if (piece->start == GRX_NPOS) {
    // A capturing group in the pattern appears between the pieces around
    // it, and one that did not participate is `None` in `re.split`. The
    // pieces are spans rather than copies, so this is the only way to say
    // it - there is no empty string to hand back that would mean the same.
    printf("  <unset>\n");
    return;
  }
  printf("  '%.*s'\n", (int)(piece->end - piece->start), line + piece->start);
}

/** Every match in one line, with its groups named where the pattern named. */
static int show_matches(const GRX_Regex * regex, const char * line,
    size_t length) {
  GRX_Match * match = NULL;
  if (grx_match_create(regex, NULL, &match) != GRX_OK) {
    return 0;
  }

  int found = 0;
  int matched = 0;
  while (grx_regex_search_next(regex, line, length, NULL, match, &matched)
          == GRX_OK
      && matched) {
    GRX_Capture whole;
    grx_match_span(match, &whole);
    printf("  %2zu-%-2zu %.*s", whole.start, whole.end,
        (int)(whole.end - whole.start), line + whole.start);

    for (size_t i = 1; i < grx_match_count(match); i++) {
      GRX_Capture group;
      if (grx_match_group(match, i, &group) != GRX_OK
          || group.start == GRX_NPOS) {
        printf("  %zu=-", i);
        continue;
      }
      // `grx_regex_capture_name()` answers for the `(?P<n>...)` spelling;
      // a group written `(...)` has no name and prints as its number.
      const char * name = grx_regex_capture_name(regex, i);
      if (name) {
        printf("  %s=%.*s", name, (int)(group.end - group.start),
            line + group.start);
      }
      else {
        printf("  %zu=%.*s", i, (int)(group.end - group.start),
            line + group.start);
      }
    }
    printf("\n");
    found = 1;
  }

  grx_match_destroy(match);
  return found;
}

int main(int argc, char ** argv) {
  size_t limit = GRX_NPOS;
  int substituting = 0;
  int showing = 0;
  int at = 1;

  while (at < argc && argv[at][0] == '-' && argv[at][1] == '-') {
    if (strcmp(argv[at], "--sub") == 0) {
      substituting = 1;
      at++;
    }
    else if (strcmp(argv[at], "--show") == 0) {
      showing = 1;
      at++;
    }
    else if (strcmp(argv[at], "--maxsplit") == 0 && at + 1 < argc) {
      // Python's `maxsplit`, counting *splits*. Passed straight through as
      // the library's `limit`, because the library reads it the dialect's
      // way rather than making the caller convert it.
      limit = (size_t)strtoul(argv[at + 1], NULL, 10);
      at += 2;
    }
    else {
      fprintf(stderr, "%s: unknown option %s\n", argv[0], argv[at]);
      return 2;
    }
  }

  if (at >= argc || (substituting && at + 1 >= argc)) {
    fprintf(stderr,
        "usage: %s [--maxsplit N] <pattern>\n"
        "       %s --sub <pattern> <template>\n"
        "       %s --show <pattern>\n",
        argv[0], argv[0], argv[0]);
    return 2;
  }

  GRX_Regex * regex = NULL;
  GRX_Error error;
  grx_error_clear(&error);
  if (grx_regex_compile_with_allocator(argv[at], strlen(argv[at]),
          GRX_SYNTAX_PYTHON, 0, NULL, NULL, &error, &regex)
      != GRX_OK) {
    // This dialect's escape alphabet is *closed*, unlike the rest of the
    // Perl family: `\p{L}`, `\Q`, `\K` and `(?<n>...)` are refused here and
    // accepted there, so the diagnostic is often "that construct belongs to
    // another dialect" rather than "that is malformed".
    // The message already carries the offset - grx_error_set() composes
    // "<what> at offset <n>" - so naming it again printed "at offset 0 at
    // byte 0" in four of these programs. Which *text* the offset is into is
    // the half the message does not say, and the half that matters here:
    // this program has a pattern and a template.
    fprintf(stderr, "%s: %s (in the pattern)\n", argv[0], error.message);
    grx_regex_free(regex);
    return 1;
  }

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
  size_t cursor = 0;
  while (cursor < length) {
    const char * newline = memchr(text + cursor, '\n', length - cursor);
    size_t line_length
        = newline ? (size_t)(newline - (text + cursor)) : length - cursor;
    const char * line = text + cursor;

    if (showing) {
      printf("%.*s\n", (int)line_length, line);
      if (show_matches(regex, line, line_length)) {
        status = 0;
      }
    }
    else if (substituting) {
      GRX_Text out;
      memset(&out, 0, sizeof(out));
      grx_error_clear(&error);
      if (grx_regex_replace(regex, line, line_length, argv[at + 1],
              strlen(argv[at + 1]), GRX_REPLACE_GLOBAL, NULL, NULL, &error,
              &out)
          != GRX_OK) {
        // An offset in the error is an offset into the *template*, not into
        // the pattern - and `\g<9>` against a pattern with two groups is a
        // template error here where ECMAScript's `$9` is literal text.
        fprintf(stderr, "%s: %s (in the template)\n", argv[0],
            error.message);
        status = 1;
        break;
      }
      printf("%.*s\n", (int)out.length, out.data);
      grx_text_free(&out);
      status = 0;
    }
    else {
      GRX_Split split;
      memset(&split, 0, sizeof(split));
      grx_error_clear(&error);
      if (grx_regex_split(regex, line, line_length, limit, NULL, NULL, &error,
              &split)
          != GRX_OK) {
        fprintf(stderr, "%s: %s\n", argv[0], error.message);
        status = 1;
        break;
      }
      printf("%.*s -> %zu\n", (int)line_length, line, split.count);
      for (size_t i = 0; i < split.count; i++) {
        print_piece(line, &split.pieces[i]);
      }
      grx_split_free(&split);
      status = 0;
    }

    cursor += line_length + (newline ? 1 : 0);
  }

  gcu_file_free(NULL, data);
  grx_regex_free(regex);
  return status;
}
