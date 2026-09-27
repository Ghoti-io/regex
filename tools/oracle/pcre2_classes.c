/**
 * @file
 *
 * pcre2's POSIX class and `\w` membership, in the shape grx_classes.c prints.
 *
 * Compiled inside the pinned image at run time, like tools/oracle/
 * pcre2_match.c and for the same reason: what the image holds is a pcre2 and
 * a compiler, and what it builds links only pcre2.
 *
 * One line per run, per class: `<class> <lo> <hi>` in hexadecimal, inclusive.
 *
 * `utf` and `ucp` on every pattern. Without `ucp` a POSIX class is ASCII in
 * pcre2 and the comparison would be about the option rather than about the
 * table; the figures dialects.md section 5.9 quotes are the *wide* sets.
 *
 * Usage:  pcre2_classes [class ...]
 *         pcre2_classes gc <hex code point> ...
 *
 * Copyright 2026 by Corey Pennycuff
 */

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char * const CLASSES[] = {
  "alnum", "alpha", "ascii", "blank", "cntrl", "digit", "graph", "lower",
  "print", "punct", "space", "upper", "word", "xdigit", "w",
};
static const size_t CLASS_COUNT = sizeof(CLASSES) / sizeof(*CLASSES);

static size_t encode(unsigned codepoint, char * out) {
  if (codepoint < 0x80) { out[0] = (char)codepoint; return 1; }
  if (codepoint < 0x800) {
    out[0] = (char)(0xC0 | (codepoint >> 6));
    out[1] = (char)(0x80 | (codepoint & 0x3F));
    return 2;
  }
  if (codepoint < 0x10000) {
    out[0] = (char)(0xE0 | (codepoint >> 12));
    out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out[2] = (char)(0x80 | (codepoint & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (codepoint >> 18));
  out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
  out[3] = (char)(0x80 | (codepoint & 0x3F));
  return 4;
}

static int sweep(const char * name) {
  char pattern[32];
  if (strcmp(name, "w") == 0) {
    snprintf(pattern, sizeof pattern, "^\\w$");
  }
  else {
    snprintf(pattern, sizeof pattern, "^[[:%s:]]$", name);
  }

  int error = 0;
  PCRE2_SIZE where = 0;
  pcre2_code * code = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
      PCRE2_UTF | PCRE2_UCP, &error, &where, NULL);
  if (!code) {
    PCRE2_UCHAR text[256];
    pcre2_get_error_message(error, text, sizeof text);
    fprintf(stderr, "pcre2_classes: /%s/ refused: %s\n", pattern,
        (const char *)text);
    return 0;
  }
  pcre2_match_data * data = pcre2_match_data_create_from_pattern(code, NULL);

  long start = -1;
  for (unsigned codepoint = 0; codepoint <= 0x110000; codepoint++) {
    int member = 0;
    if (codepoint <= 0x10FFFF) {
      char bytes[4];
      size_t length = encode(codepoint, bytes);
      member = pcre2_match(code, (PCRE2_SPTR)bytes, length, 0, 0, data, NULL)
          >= 0;
    }
    if (member && start < 0) {
      start = (long)codepoint;
    }
    else if (!member && start >= 0) {
      printf("%s %lX %lX\n", name, start, (long)codepoint - 1);
      start = -1;
    }
  }

  pcre2_match_data_free(data);
  pcre2_code_free(code);
  return 1;
}

static const char * const CATEGORIES[] = {
  "Lu", "Ll", "Lt", "Lm", "Lo", "Mn", "Mc", "Me", "Nd", "Nl", "No",
  "Pc", "Pd", "Ps", "Pe", "Pi", "Pf", "Po", "Sm", "Sc", "Sk", "So",
  "Zs", "Zl", "Zp", "Cc", "Cf", "Cs", "Co", "Cn",
};

/**
 * Print pcre2's General_Category for one code point.
 *
 * A class difference between pcre2 and this library is a difference of *rule*
 * only where the two agree about the category; where they do not, pcre2 is
 * carrying an older UCD. wide_class_diff.py asks both sides for this before it
 * excuses a row, so the excuse rests on a measurement rather than on a guess
 * about which release moved.
 */
static void print_category(unsigned codepoint) {
  char bytes[4];
  size_t length = encode(codepoint, bytes);
  for (size_t i = 0; i < sizeof(CATEGORIES) / sizeof(*CATEGORIES); i++) {
    char pattern[32];
    snprintf(pattern, sizeof pattern, "^\\p{%s}$", CATEGORIES[i]);
    int error; PCRE2_SIZE where;
    pcre2_code * code = pcre2_compile((PCRE2_SPTR)pattern,
        PCRE2_ZERO_TERMINATED, PCRE2_UTF | PCRE2_UCP, &error, &where, NULL);
    if (!code) { continue; }
    pcre2_match_data * data
        = pcre2_match_data_create_from_pattern(code, NULL);
    int matched
        = pcre2_match(code, (PCRE2_SPTR)bytes, length, 0, 0, data, NULL) >= 0;
    pcre2_match_data_free(data);
    pcre2_code_free(code);
    if (matched) {
      printf("gc %X %s\n", codepoint, CATEGORIES[i]);
      return;
    }
  }
  printf("gc %X ?\n", codepoint);
}

int main(int argc, char ** argv) {
  if (argc > 1 && strcmp(argv[1], "gc") == 0) {
    for (int i = 2; i < argc; i++) {
      print_category((unsigned)strtoul(argv[i], NULL, 16));
    }
    return 0;
  }
  if (argc > 1) {
    for (int i = 1; i < argc; i++) {
      sweep(argv[i]);
    }
    return 0;
  }
  for (size_t i = 0; i < CLASS_COUNT; i++) {
    sweep(CLASSES[i]);
  }
  return 0;
}
