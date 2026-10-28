/**
 * @file
 *
 * This library's `grx_regex_replace()` as an oracle driver, so that a
 * dialect's replacement-template grammar can be compared with the tool that
 * defines it.
 *
 * Reads `<flags>\t<pattern hex>\t<subject hex>\t<template hex>` lines. For
 * Java comparisons it also accepts a leading mode: `first`, `all`,
 * `first-literal`, or `all-literal`. The original four-field protocol and
 * responses remain unchanged for older differential drivers.
 * Writes one answer per line:
 *
 *   ok <result hex>   the subject with every match replaced
 *   compile           the pattern was refused
 *   template          the template was refused
 *   template D O L    five-field mode only: diagnostic, byte offset, length
 *   error <code>      a limit, or a bad subject
 *
 * Hex for the reason grx_match.c uses it: a pattern, a subject or a template
 * may contain a newline or a NUL, and the transport should not need an
 * escape of its own.
 *
 * The flag letters are the dialect's own, read by grx_options_parse(); the
 * dialect itself is argv[1]. Replacement is global, because `s///g` is what
 * the sed side of the comparison does.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/regex.h>

#include "grx_flags.h"
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
    fprintf(stderr, "grx_replace: unknown dialect %s\n", argv[1]);
    return 2;
  }

  char * line = malloc(MAX_LINE);
  char * pattern = malloc(MAX_LINE / 2);
  char * subject = malloc(MAX_LINE / 2);
  char * replacement = malloc(MAX_LINE / 2);
  if (!line || !pattern || !subject || !replacement) {
    fprintf(stderr, "grx_replace: out of memory\n");
    return 2;
  }

  while (fgets(line, MAX_LINE, stdin)) {
    size_t length = strlen(line);
    while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
      line[--length] = '\0';
    }

    // Four fields, so three tabs, and any of the last three may be empty.
    char * fields[5] = {line, NULL, NULL, NULL, NULL};
    size_t count = 1;
    for (size_t i = 0; i < length && count < 5; i++) {
      if (line[i] == '\t') {
        line[i] = '\0';
        fields[count++] = line + i + 1;
      }
    }
    if (count != 4 && count != 5) {
      printf("error malformed\n");
      fflush(stdout);
      continue;
    }

    uint32_t replace_flags = GRX_REPLACE_GLOBAL;
    size_t base = 0;
    if (count == 5) {
      if (!strcmp(fields[0], "first")) {
        replace_flags = 0;
      }
      else if (!strcmp(fields[0], "all")) {
        replace_flags = GRX_REPLACE_GLOBAL;
      }
      else if (!strcmp(fields[0], "first-literal")) {
        replace_flags = GRX_REPLACE_LITERAL;
      }
      else if (!strcmp(fields[0], "all-literal")) {
        replace_flags = GRX_REPLACE_GLOBAL | GRX_REPLACE_LITERAL;
      }
      else {
        printf("error mode\n");
        fflush(stdout);
        continue;
      }
      base = 1;
    }
    size_t pattern_length = unhex(fields[base + 1], strlen(fields[base + 1]), pattern);
    size_t subject_length = unhex(fields[base + 2], strlen(fields[base + 2]), subject);
    size_t replacement_length
        = unhex(fields[base + 3], strlen(fields[base + 3]), replacement);
    if (pattern_length == (size_t)-1 || subject_length == (size_t)-1
        || replacement_length == (size_t)-1) {
      printf("error malformed hex\n");
      fflush(stdout);
      continue;
    }

    uint32_t options = 0;
    // `W` first: see grx_flags.h. No dialect's alphabet has the letter, so
    // grx_options_parse() would refuse the whole flag string over it, and
    // this driver's "error flags" would read as the dialect rejecting a
    // request it had never been asked.
    char letters[64];
    if (!grx_oracle_take_wtf8(fields[base], letters, sizeof letters, &options)) {
      printf("error flags\n");
      fflush(stdout);
      continue;
    }
    // Into a second variable and then ORed: grx_options_parse() *assigns*
    // what the letters mean, so passing `&options` here would overwrite the
    // GRX_OPT_WTF8 the lift above had just put in it. That cost 482 rows of
    // the split differential, all of them reported as this library refusing
    // a subject the reference answered - which is what its surrogate
    // denominator exists to say out loud.
    uint32_t parsed = 0;
    if (grx_options_parse(syntax, letters, &parsed, NULL) != GRX_OK) {
      printf("error flags\n");
      fflush(stdout);
      continue;
    }
    options |= parsed;

    GRX_Regex * regex = NULL;
    GRX_Error error;
    grx_error_clear(&error);
    GRX_Result compile_result = grx_regex_compile_with_allocator(pattern,
        pattern_length, syntax, options, NULL, NULL, &error, &regex);
    if (compile_result != GRX_OK) {
      /* Only syntax errors are refusals; resource and unsupported results
       * keep the error protocol instead of becoming accepted refusals. */
      if (compile_result == GRX_ERR_SYNTAX) {
        printf("compile %d\n", (int)error.diag);
      }
      else if (compile_result == GRX_ERR_UNSUPPORTED &&
          error.diag == GRX_DIAG_PREVIOUS_SUBSTITUTE_UNAVAILABLE) {
        printf("unavailable %d\n", (int)error.diag);
      }
      else {
        printf("error %d\n", (int)compile_result);
      }
      fflush(stdout);
      grx_regex_free(regex);
      continue;
    }

    GRX_Text out;
    memset(&out, 0, sizeof(out));
    grx_error_clear(&error);
    GRX_Result result = grx_regex_replace(regex, subject, subject_length,
        replacement, replacement_length, replace_flags, NULL, NULL, &error,
        &out);
    if (result == GRX_OK) {
      printf("ok ");
      print_hex(out.data, out.length);
      printf("\n");
      grx_text_free(&out);
    }
    else if (result == GRX_ERR_SYNTAX) {
      if (count == 5) {
        printf("template %d %zu %zu\n", (int)error.diag, error.offset,
            error.length);
      }
      else {
        printf("template\n");
      }
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
  free(replacement);
  return 0;
}
