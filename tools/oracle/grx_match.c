/**
 * @file
 *
 * This library, as a matching oracle: reads pattern-and-subject pairs, prints
 * what matched.
 *
 * Each input line is `<flags>\t<pattern hex>\t<subject hex>`, optionally
 * followed by `\t<begin>,<end>,<search flags>` - the window and the
 * subject-side flags of GRX_SearchOptions. Hex for the same reason the syntax
 * oracle uses it: a pattern or a subject may contain a newline, a NUL or a
 * byte sequence that is not valid UTF-8, and the transport should not need an
 * escape of its own.
 *
 * The window field is optional so that every harness written before it
 * existed keeps working unchanged. `end` may be `-` for "all of it", and the
 * search flags are letters: `B` for NOTBOL, `E` for NOTEOL, `M` for NOTEMPTY
 * and `A` for NOTEMPTY_ATSTART.
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
#include <stdlib.h>
#include <string.h>

/** The longest pattern or subject a line may carry. */
/*
 * A pattern this driver will read, and a subject it will scan.
 *
 * Separate, and the subject far larger, because they are asked for
 * different things. They were one 64 KB constant that `decode_hex` silently
 * stopped at, which in a *differential* driver is the worst shape the bug
 * has: Node would have been asked about one pattern and this library about
 * a shorter one, and the gate would have reported agreement about two
 * different questions. Nothing in the current corpora comes near either
 * bound - the oracle's subjects are under twenty bytes - so this is a
 * latent failure being closed rather than an observed one being fixed.
 */
#define MAX_PATTERN 65536
#define MAX_SUBJECT (1 << 20)

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

/**
 * Decode a hex field, or refuse it.
 *
 * Returns the number of bytes written, or SIZE_MAX when the field holds
 * more than `capacity` of them.
 */
static size_t decode_hex(const char * hex, char * out, size_t capacity) {
  size_t length = 0;
  while (hex[0] && hex[1]) {
    if (length == capacity) {
      return (size_t)-1;
    }
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
    case GRX_ENGINE_BITSTATE:
      return "bitstate";
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
    else if (strcmp(argv[2], "bitstate") == 0) {
      engine = GRX_ENGINE_BITSTATE;
    }
  }

  static char line[2 * (MAX_PATTERN + MAX_SUBJECT) + 64];
  static char pattern[MAX_PATTERN];
  static char subject[MAX_SUBJECT];

  // The compiled regex is kept for as long as the rows keep asking for the
  // same one. Every harness that drives this tool groups its rows by pattern
  // - one pattern against many subjects - so a one-entry cache is the whole
  // of what is needed, and it is the difference between a run that takes a
  // second and one that takes an hour: `\p{RGI_Emoji}` compiles to an
  // alternation of nearly four thousand sequences, and compiling that once
  // per subject is what a differential over the emoji universe would do.
  static char cached_pattern[MAX_PATTERN];
  static size_t cached_length = 0;
  static uint32_t cached_options = 0;
  static int cached_valid = 0;
  GRX_Regex * regex = NULL;

  while (fgets(line, (int)sizeof(line), stdin)) {
    // A record too long for the buffer arrives without its newline, and its
    // tail would be read as the next record. Drain it and say so.
    if (!strchr(line, '\n') && !feof(stdin)) {
      int c;
      while ((c = fgetc(stdin)) != EOF && c != '\n') {
      }
      printf("toolong\n");
      fflush(stdout);
      continue;
    }

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

    // The window, when the harness asked for one. Split off before the
    // subject is decoded: decode_hex stops at the first byte that is not a
    // hex digit, so leaving the tab in place would silently hand the engine
    // a *shorter subject* and report agreement about a different question.
    char * third = strchr(second + 1, '\t');
    if (third) {
      *third = '\0';
    }

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
    if (pattern_length == (size_t)-1 || subject_length == (size_t)-1) {
      printf("toolong\n");
      fflush(stdout);
      continue;
    }

    if (!regex || !cached_valid || cached_options != options
        || cached_length != pattern_length
        || memcmp(cached_pattern, pattern, pattern_length) != 0) {
      grx_regex_free(regex);
      regex = NULL;

      GRX_Error error;
      grx_error_clear(&error);
      if (grx_regex_compile_with_allocator(pattern, pattern_length, syntax,
              options, NULL, NULL, &error, &regex)
          != GRX_OK) {
        cached_valid = 0;
        regex = NULL;
        printf("compile %d\n", (int)error.diag);
        continue;
      }
      memcpy(cached_pattern, pattern, pattern_length);
      cached_length = pattern_length;
      cached_options = options;
      cached_valid = 1;
    }

    GRX_SearchOptions search;
    grx_search_options_default(&search);
    search.engine = engine;
    if (third) {
      char * cursor = third + 1;
      search.begin = (size_t)strtoull(cursor, &cursor, 10);
      if (*cursor == ',') {
        cursor++;
      }
      if (*cursor == '-') {
        cursor++;
      }
      else {
        search.end = (size_t)strtoull(cursor, &cursor, 10);
      }
      if (*cursor == ',') {
        cursor++;
      }
      for (; *cursor; cursor++) {
        switch (*cursor) {
          case 'B': search.flags |= GRX_SEARCH_NOTBOL; break;
          case 'E': search.flags |= GRX_SEARCH_NOTEOL; break;
          case 'M': search.flags |= GRX_SEARCH_NOTEMPTY; break;
          case 'A': search.flags |= GRX_SEARCH_NOTEMPTY_ATSTART; break;
          default: break;
        }
      }
    }

    GRX_Match * match = NULL;
    if (grx_match_create(regex, NULL, &match) != GRX_OK) {
      printf("error oom\n");
      continue;
    }

    int matched = 0;
    GRX_Result result = grx_regex_search_ex(
        regex, subject, subject_length, &search, match, &matched);

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
  }

  grx_regex_free(regex);
  return 0;
}
