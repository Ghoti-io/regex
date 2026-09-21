/**
 * @file
 *
 * This library's `grx_regex_replace()` as an oracle driver, so that a
 * dialect's replacement-template grammar can be compared with the tool that
 * defines it.
 *
 * Reads `<flags>\t<pattern hex>\t<subject hex>\t<template hex>` lines and
 * writes one answer per line:
 *
 *   ok <result hex>   the subject with every match replaced
 *   compile           the pattern was refused
 *   template          the template was refused
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
    size_t replacement_length
        = unhex(fields[3], strlen(fields[3]), replacement);
    if (pattern_length == (size_t)-1 || subject_length == (size_t)-1
        || replacement_length == (size_t)-1) {
      printf("error malformed hex\n");
      fflush(stdout);
      continue;
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

    GRX_Text out;
    memset(&out, 0, sizeof(out));
    grx_error_clear(&error);
    GRX_Result result = grx_regex_replace(regex, subject, subject_length,
        replacement, replacement_length, GRX_REPLACE_GLOBAL, NULL, NULL, &error,
        &out);
    if (result == GRX_OK) {
      printf("ok ");
      print_hex(out.data, out.length);
      printf("\n");
      grx_text_free(&out);
    }
    else if (result == GRX_ERR_SYNTAX) {
      printf("template\n");
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
