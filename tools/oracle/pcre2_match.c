/**
 * @file
 *
 * pcre2 as a matching oracle, in the shape the other drivers here use.
 *
 * Reads `<flags>\t<pattern hex>\t<subject hex>` lines and writes one answer
 * per line:
 *
 *   match <start>:<end> ...   one span per group, `-` for a group that is unset
 *   nomatch
 *   compile                   the pattern was refused
 *   skip <reason>             the driver declines to answer
 *
 * Hex for the same reason tools/oracle/grx_match.c uses it: a pattern or a
 * subject may contain a newline, a NUL, or bytes that are not valid UTF-8,
 * and the transport should not need an escape of its own.
 *
 * **Why this exists when pcre2test does.** pcre2test reports the matched
 * *text*, not offsets, and omits a trailing group that did not participate
 * rather than naming it - so "which span did group two get" is not a question
 * it answers, and that is most of what a match comparison is. This asks
 * pcre2_match() and reads the ovector, which is the same question every other
 * oracle here is asked.
 *
 * **What it is linked against.** The pcre2 already on the machine, through
 * the public header of the release pinned in tools/corpus/VERSIONS. Debian
 * ships libpcre2-8.so.0 without the -dev package's pcre2.h, so the header is
 * fetched with the corpus and the four version macros its configure template
 * leaves open are substituted by the Makefile. The pin, the installed
 * library and the imported testinput files are therefore one version -
 * 10.46 - and a mismatch would be a build error rather than a quiet
 * disagreement across the whole corpus.
 *
 * Copyright 2026 by Corey Pennycuff
 */
#define PCRE2_CODE_UNIT_WIDTH 8

#include <pcre2.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** The longest pattern and subject the driver will carry. */
#define MAX_PATTERN 4096
#define MAX_SUBJECT 4096

/** Decode hex into `out`, returning the byte count or (size_t)-1. */
static size_t decode_hex(const char * text, char * out, size_t capacity) {
  size_t length = strlen(text);
  if (length % 2 != 0 || length / 2 > capacity) {
    return (size_t)-1;
  }
  for (size_t i = 0; i < length; i += 2) {
    int high = -1;
    int low = -1;
    for (int j = 0; j < 2; j++) {
      char c = text[i + j];
      int value = -1;
      if (c >= '0' && c <= '9') { value = c - '0'; }
      else if (c >= 'a' && c <= 'f') { value = c - 'a' + 10; }
      else if (c >= 'A' && c <= 'F') { value = c - 'A' + 10; }
      else { return (size_t)-1; }
      if (j == 0) { high = value; } else { low = value; }
    }
    out[i / 2] = (char)((high << 4) | low);
  }
  return length / 2;
}

/**
 * The flag letters, mapped to compile options.
 *
 * The same letters the other drivers take, so that one generator can feed
 * all of them. `u` is PCRE2_UTF: pcre2 is byte-oriented by default where
 * this library's pcre row is not, so the caller passes it to ask the same
 * question. PCRE2_UCP goes with it, because `\w` meaning "word character"
 * rather than "[A-Za-z0-9_]" is part of what UTF mode means elsewhere.
 */
static uint32_t options_for(const char * flags) {
  uint32_t options = 0;
  for (const char * f = flags; *f; f++) {
    switch (*f) {
      case 'i': options |= PCRE2_CASELESS; break;
      case 'm': options |= PCRE2_MULTILINE; break;
      case 's': options |= PCRE2_DOTALL; break;
      case 'x': options |= PCRE2_EXTENDED; break;
      case 'u': options |= PCRE2_UTF | PCRE2_UCP; break;
      default: break;
    }
  }
  return options;
}

int main(void) {
  static char line[2 * (MAX_PATTERN + MAX_SUBJECT) + 64];
  static char pattern[MAX_PATTERN];
  static char subject[MAX_SUBJECT];

  fprintf(stderr, "pcre2 %d.%d\n", PCRE2_MAJOR, PCRE2_MINOR);

  while (fgets(line, sizeof line, stdin)) {
    size_t length = strlen(line);
    while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
      line[--length] = '\0';
    }
    if (!length) {
      continue;
    }

    char * first = strchr(line, '\t');
    if (!first) { continue; }
    *first = '\0';
    char * second = strchr(first + 1, '\t');
    if (!second) { continue; }
    *second = '\0';

    uint32_t options = options_for(line);
    size_t pattern_length = decode_hex(first + 1, pattern, sizeof pattern);
    size_t subject_length = decode_hex(second + 1, subject, sizeof subject);
    if (pattern_length == (size_t)-1 || subject_length == (size_t)-1) {
      printf("toolong\n");
      fflush(stdout);
      continue;
    }

    int errorcode = 0;
    PCRE2_SIZE erroroffset = 0;
    pcre2_code * code = pcre2_compile((PCRE2_SPTR)pattern, pattern_length,
        options, &errorcode, &erroroffset, NULL);
    if (!code) {
      printf("compile\n");
      fflush(stdout);
      continue;
    }

    // How many groups the *pattern* has, which is not how many the match
    // filled in: pcre2_match() returns one past the highest pair it set, so
    // a trailing group that did not participate is simply not counted. Every
    // other driver here names every group, so this asks the pattern.
    uint32_t captures = 0;
    pcre2_pattern_info(code, PCRE2_INFO_CAPTURECOUNT, &captures);

    pcre2_match_data * data
        = pcre2_match_data_create_from_pattern(code, NULL);
    if (!data) {
      printf("skip nomemory\n");
      fflush(stdout);
      pcre2_code_free(code);
      continue;
    }

    int rc = pcre2_match(code, (PCRE2_SPTR)subject, subject_length, 0, 0,
        data, NULL);
    if (rc == PCRE2_ERROR_NOMATCH) {
      printf("nomatch\n");
    }
    else if (rc < 0) {
      // A bad UTF subject, a recursion limit, a match limit: all of them are
      // "pcre2 declined to answer", and none of them is an opinion about the
      // pattern that this library can be held to.
      printf("skip error%d\n", rc);
    }
    else {
      PCRE2_SIZE * ovector = pcre2_get_ovector_pointer(data);
      printf("match");
      for (uint32_t group = 0; group <= captures; group++) {
        int set = (int)group < rc
            && ovector[2 * group] != PCRE2_UNSET
            && ovector[2 * group + 1] != PCRE2_UNSET;
        if (set) {
          printf(" %zu:%zu", (size_t)ovector[2 * group],
              (size_t)ovector[2 * group + 1]);
        }
        else {
          printf(" -");
        }
      }
      printf("\n");
    }
    fflush(stdout);

    pcre2_match_data_free(data);
    pcre2_code_free(code);
  }
  return 0;
}
