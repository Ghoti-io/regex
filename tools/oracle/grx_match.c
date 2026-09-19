/**
 * @file
 *
 * This library, as a matching oracle: reads pattern-and-subject pairs, prints
 * what matched.
 *
 * Each input line is `<flags>\t<pattern hex>\t<subject hex>`, hex for the
 * same reason the syntax oracle uses it: a pattern or a subject may contain a
 * newline, a NUL or a byte sequence that is not valid UTF-8, and the
 * transport should not need an escape of its own.
 *
 * Each output line is one of:
 *
 *   `match <engine> <start>:<end> ...`  one span per group, `-` for unset
 *   `nomatch`
 *   `unsupported`                        no engine here can run this program
 *   `compile <diag>`                     the pattern was rejected
 *   `error <code>`                       a limit, or a bad subject
 *
 * Offsets are byte offsets into the subject. A reference implementation that
 * counts UTF-16 code units has to convert; tools/oracle/node_match.mjs does.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/regex.h>
#include <stdio.h>
#include <string.h>

/** The longest pattern or subject a line may carry. */
#define MAX_BUFFER 65536

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

static size_t decode_hex(const char * hex, char * out, size_t capacity) {
  size_t length = 0;
  while (hex[0] && hex[1] && length < capacity) {
    int high = unhex(hex[0]);
    int low = unhex(hex[1]);
    if (high < 0 || low < 0) {
      break;
    }
    out[length++] = (char)((high << 4) | low);
    hex += 2;
  }
  return length;
}

static const char * engine_name(GRX_Engine engine) {
  switch (engine) {
    case GRX_ENGINE_PIKE:
      return "pike";
    case GRX_ENGINE_BACKTRACK:
      return "backtrack";
    default:
      return "?";
  }
}

int main(int argc, char ** argv) {
  GRX_Syntax syntax = GRX_SYNTAX_ECMASCRIPT;
  if (argc > 1 && grx_syntax_from_name(argv[1], &syntax) != GRX_OK) {
    fprintf(stderr, "unknown dialect: %s\n", argv[1]);
    return 2;
  }
  GRX_Engine engine = GRX_ENGINE_AUTO;
  if (argc > 2) {
    if (strcmp(argv[2], "pike") == 0) {
      engine = GRX_ENGINE_PIKE;
    }
    else if (strcmp(argv[2], "backtrack") == 0) {
      engine = GRX_ENGINE_BACKTRACK;
    }
  }

  static char line[3 * MAX_BUFFER];
  static char pattern[MAX_BUFFER];
  static char subject[MAX_BUFFER];

  while (fgets(line, (int)sizeof(line), stdin)) {
    char * first = strchr(line, '\t');
    if (!first) {
      continue;
    }
    *first = '\0';
    char * second = strchr(first + 1, '\t');
    if (!second) {
      continue;
    }
    *second = '\0';

    uint32_t options = 0;
    for (const char * f = line; *f; f++) {
      switch (*f) {
        case 'i': options |= GRX_OPT_CASELESS; break;
        case 'm': options |= GRX_OPT_MULTILINE; break;
        case 's': options |= GRX_OPT_DOTALL; break;
        case 'u': options |= GRX_OPT_UTF; break;
        case 'v': options |= GRX_OPT_UNICODE_SETS | GRX_OPT_UTF; break;
        default: break;
      }
    }

    size_t pattern_length
        = decode_hex(first + 1, pattern, sizeof(pattern));
    size_t subject_length
        = decode_hex(second + 1, subject, sizeof(subject));

    GRX_Error error;
    grx_error_clear(&error);
    GRX_Regex * regex = NULL;
    if (grx_regex_compile_with_allocator(pattern, pattern_length, syntax,
            options, NULL, NULL, &error, &regex)
        != GRX_OK) {
      printf("compile %d\n", (int)error.diag);
      continue;
    }

    GRX_Match * match = NULL;
    if (grx_match_create(regex, NULL, &match) != GRX_OK) {
      printf("error oom\n");
      grx_regex_free(regex);
      continue;
    }

    int matched = 0;
    GRX_Result result = grx_regex_search(
        regex, subject, subject_length, 0, engine, NULL, match, &matched);

    if (result == GRX_ERR_UNSUPPORTED) {
      printf("unsupported\n");
    }
    else if (result != GRX_OK) {
      printf("error %s\n", grx_result_string(result));
    }
    else if (!matched) {
      printf("nomatch\n");
    }
    else {
      printf("match %s", engine_name(grx_match_engine(match)));
      for (size_t i = 0; i < grx_match_count(match); i++) {
        GRX_Capture capture;
        grx_match_group(match, i, &capture);
        if (capture.start == GRX_NPOS) {
          printf(" -");
        }
        else {
          printf(" %zu:%zu", capture.start, capture.end);
        }
      }
      printf("\n");
    }

    grx_match_destroy(match);
    grx_regex_free(regex);
  }

  return 0;
}
