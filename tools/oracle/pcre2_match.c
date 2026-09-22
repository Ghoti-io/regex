/**
 * @file
 *
 * pcre2 as a matching and replacing oracle, in the shape the other drivers
 * here use.
 *
 * With no argument it reads `<flags>\t<pattern hex>\t<subject hex>` lines,
 * optionally followed by `\t<begin>,<end>,<match flags>`, and writes one
 * answer per line:
 *
 *   match <start>:<end> ...   one span per group, `-` for a group that is unset
 *   nomatch
 *   compile                   the pattern was refused
 *   skip <reason>             the driver declines to answer
 *
 * With `replace` it reads a fourth field, the template, and writes:
 *
 *   ok <count> <result hex>   the subject with every match replaced, and
 *                             how many replacements that was
 *   compile                   the pattern was refused
 *   template                  the template was refused
 *   skip <reason>             the driver declines to answer
 *
 * The two shapes are grx_match.c's and grx_replace.c's respectively, so one
 * generator can drive either side of a comparison without knowing which
 * implementation is answering.
 *
 * The optional window field is grx_match.c's too, and it maps onto pcre2
 * exactly: `begin` is pcre2_match()'s `startoffset`, `end` is the `length` it
 * is given, and the letters are PCRE2_NOTBOL, PCRE2_NOTEOL, PCRE2_NOTEMPTY
 * and PCRE2_NOTEMPTY_ATSTART. That correspondence is the reason a window
 * differential is possible at all: PCRE2 is the one reference here whose
 * offsets are bytes and whose `^` means the start of the *subject* rather
 * than the start of the search, which is what
 * GRX_SearchOptions::begin also means.
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

/**
 * Replace every match, and print the result.
 *
 * PCRE2_SUBSTITUTE_GLOBAL because grx_replace is given GRX_REPLACE_GLOBAL,
 * and PCRE2_SUBSTITUTE_UNSET_EMPTY is deliberately *not* set: without it a
 * reference to a group that did not participate is an error, which is what
 * pcre2's own default is and therefore what the dialect this library calls
 * `pcre` has to be measured against.
 *
 * The output buffer is grown once on PCRE2_ERROR_NOMEMORY rather than sized
 * by a guess, because a template may multiply the subject - `$&$&$&` on a
 * subject of a thousand matches is not a length anybody can predict.
 */
static void do_replace(pcre2_code * code, const char * subject,
    size_t subject_length, const char * template, size_t template_length) {
  static PCRE2_UCHAR out[4 * (MAX_SUBJECT + MAX_PATTERN)];
  PCRE2_SIZE length = sizeof out / sizeof out[0];

  int rc = pcre2_substitute(code, (PCRE2_SPTR)subject, subject_length, 0,
      PCRE2_SUBSTITUTE_GLOBAL, NULL, NULL, (PCRE2_SPTR)template,
      template_length, out, &length);

  if (rc == PCRE2_ERROR_NOMEMORY) {
    printf("skip toolong\n");
    return;
  }
  if (rc < 0) {
    // Every remaining negative is pcre2 refusing the *template*: a bad
    // `$` form, a reference to a group the pattern has not got, an
    // unterminated `${`. A match-time failure cannot reach here, because
    // "no match" is a successful substitution of nothing.
    printf("template\n");
    return;
  }

  // The substitution *count*, which pcre2_substitute() returns and nothing
  // else here reports. It is what says whether the template was ever
  // reached: pcre2 parses a template lazily, so a malformed one on a subject
  // with no match comes back as the subject unchanged rather than as an
  // error, and a comparison that could not tell "no match" from "matched and
  // produced the same text" would have to guess which.
  printf("ok %d ", rc);
  for (PCRE2_SIZE i = 0; i < length; i++) {
    printf("%02x", (unsigned)out[i]);
  }
  printf("\n");
}

int main(int argc, char ** argv) {
  static char line[3 * (MAX_PATTERN + MAX_SUBJECT) + 64];
  static char pattern[MAX_PATTERN];
  static char subject[MAX_SUBJECT];
  static char template[MAX_PATTERN];

  int replacing = argc > 1 && strcmp(argv[1], "replace") == 0;

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

    char * third = strchr(second + 1, '\t');
    if (third) {
      *third = '\0';
    }
    if (replacing && !third) { continue; }

    uint32_t options = options_for(line);
    size_t pattern_length = decode_hex(first + 1, pattern, sizeof pattern);
    size_t subject_length = decode_hex(second + 1, subject, sizeof subject);
    size_t template_length = 0;
    if (replacing) {
      template_length = decode_hex(third + 1, template, sizeof template);
    }
    if (pattern_length == (size_t)-1 || subject_length == (size_t)-1
        || template_length == (size_t)-1) {
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

    if (replacing) {
      do_replace(code, subject, subject_length, template, template_length);
      fflush(stdout);
      pcre2_code_free(code);
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

    size_t begin = 0;
    size_t end = subject_length;
    uint32_t match_options = 0;
    if (third) {
      char * cursor = third + 1;
      begin = (size_t)strtoull(cursor, &cursor, 10);
      if (*cursor == ',') { cursor++; }
      if (*cursor == '-') { cursor++; }
      else { end = (size_t)strtoull(cursor, &cursor, 10); }
      if (*cursor == ',') { cursor++; }
      for (; *cursor; cursor++) {
        switch (*cursor) {
          case 'B': match_options |= PCRE2_NOTBOL; break;
          case 'E': match_options |= PCRE2_NOTEOL; break;
          case 'M': match_options |= PCRE2_NOTEMPTY; break;
          case 'A': match_options |= PCRE2_NOTEMPTY_ATSTART; break;
          default: break;
        }
      }
    }

    int rc = pcre2_match(code, (PCRE2_SPTR)subject, end, begin,
        match_options, data, NULL);
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
