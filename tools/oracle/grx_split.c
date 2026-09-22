/**
 * @file
 *
 * This library's `grx_regex_split()` as an oracle driver, so that the one
 * documented-but-ungenerated surface of the substitution API can be compared
 * with the tool that defines its rule.
 *
 * Reads `<flags>\t<pattern hex>\t<subject hex>\t<limit>` lines and writes one
 * answer per line:
 *
 *   ok <count> <piece hex>|<piece hex>|...
 *                                    the pieces, `-` for a group that did
 *                                    not participate
 *   compile                          the pattern was refused
 *   error <code>                     a limit, or a bad subject
 *
 * `limit` is a decimal count, or `-` for GRX_NPOS. Hex for the reason
 * grx_match.c uses it: a subject or a piece may contain a newline or a NUL.
 *
 * The count is printed as well as the pieces because `limit` 0 yields *no*
 * pieces and a one-element list holding the empty string prints identically
 * without it - and those are the two answers the limit rule exists to tell
 * apart.
 *
 * The pieces are spans and not strings, so an unset one - what ECMAScript
 * reports as `undefined` - is a different answer from an empty one, and the
 * `-` spelling keeps the two apart on the wire. A driver that printed both
 * as the empty field would have made the whole `(a)|(b)` family agree by
 * construction.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** The longest line the driver will read. */
#define MAX_LINE (1 << 20)

/** One hex digit to its value, or -1. */
static int hex_digit(int c) {
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
 * Decode a hex field into `out`.
 *
 * @return The byte count, or (size_t)-1 when the field is not hex.
 */
static size_t unhex(const char * text, size_t length, char * out) {
  if (length % 2) {
    return (size_t)-1;
  }
  for (size_t i = 0; i < length; i += 2) {
    int high = hex_digit((unsigned char)text[i]);
    int low = hex_digit((unsigned char)text[i + 1]);
    if (high < 0 || low < 0) {
      return (size_t)-1;
    }
    out[i / 2] = (char)((high << 4) | low);
  }
  return length / 2;
}

/** Write a byte run as hex. */
static void print_hex(const char * data, size_t length) {
  static const char digits[] = "0123456789abcdef";
  for (size_t i = 0; i < length; i++) {
    unsigned char byte = (unsigned char)data[i];
    putchar(digits[byte >> 4]);
    putchar(digits[byte & 0xf]);
  }
}

int main(int argc, char ** argv) {
  GRX_Syntax syntax = GRX_SYNTAX_ECMASCRIPT;
  if (argc > 1 && grx_syntax_from_name(argv[1], &syntax) != GRX_OK) {
    fprintf(stderr, "grx_split: unknown dialect %s\n", argv[1]);
    return 2;
  }
  GRX_Engine engine = GRX_ENGINE_AUTO;
  if (argc > 2 && strcmp(argv[2], "auto") != 0) {
    if (strcmp(argv[2], "pike") == 0) {
      engine = GRX_ENGINE_PIKE;
    }
    else if (strcmp(argv[2], "backtrack") == 0) {
      engine = GRX_ENGINE_BACKTRACK;
    }
    else if (strcmp(argv[2], "bitstate") == 0) {
      engine = GRX_ENGINE_BITSTATE;
    }
    else {
      fprintf(stderr, "grx_split: unknown engine %s\n", argv[2]);
      return 2;
    }
  }

  char * line = malloc(MAX_LINE);
  char * pattern = malloc(MAX_LINE / 2);
  char * subject = malloc(MAX_LINE / 2);
  if (!line || !pattern || !subject) {
    fprintf(stderr, "grx_split: out of memory\n");
    return 2;
  }

  while (fgets(line, MAX_LINE, stdin)) {
    size_t length = strlen(line);
    while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
      line[--length] = '\0';
    }

    char * fields[4] = {line, NULL, NULL, NULL};
    size_t count = 1;
    for (size_t i = 0; i < length && count < 4; i++) {
      if (line[i] == '\t') {
        line[i] = '\0';
        fields[count++] = line + i + 1;
      }
    }
    if (count != 4) {
      printf("error malformed\n");
      fflush(stdout);
      continue;
    }

    size_t pattern_length = unhex(fields[1], strlen(fields[1]), pattern);
    size_t subject_length = unhex(fields[2], strlen(fields[2]), subject);
    if (pattern_length == (size_t)-1 || subject_length == (size_t)-1) {
      printf("error malformed hex\n");
      fflush(stdout);
      continue;
    }

    size_t limit = GRX_NPOS;
    if (strcmp(fields[3], "-") != 0) {
      limit = (size_t)strtoull(fields[3], NULL, 10);
    }

    uint32_t options = 0;
    if (grx_options_parse(syntax, fields[0], &options, NULL) != GRX_OK) {
      printf("error flags\n");
      fflush(stdout);
      continue;
    }

    GRX_Regex * regex = NULL;
    GRX_Error error;
    grx_error_clear(&error);
    if (grx_regex_compile_with_allocator(pattern, pattern_length, syntax,
            options, NULL, NULL, &error, &regex)
        != GRX_OK) {
      printf("compile\n");
      fflush(stdout);
      grx_regex_free(regex);
      continue;
    }

    GRX_SearchOptions search;
    grx_search_options_default(&search);
    search.engine = engine;

    GRX_Split split;
    memset(&split, 0, sizeof(split));
    grx_error_clear(&error);
    GRX_Result result = grx_regex_split(regex, subject, subject_length, limit,
        &search, NULL, &error, &split);
    if (result == GRX_OK) {
      printf("ok %zu ", split.count);
      for (size_t i = 0; i < split.count; i++) {
        if (i) {
          putchar('|');
        }
        if (split.pieces[i].start == GRX_NPOS) {
          putchar('-');
        }
        else {
          print_hex(subject + split.pieces[i].start,
              split.pieces[i].end - split.pieces[i].start);
        }
      }
      printf("\n");
      grx_split_free(&split);
    }
    else {
      printf("error %d\n", (int)result);
    }
    fflush(stdout);
    grx_regex_free(regex);
  }

  free(line);
  free(pattern);
  free(subject);
  return 0;
}
