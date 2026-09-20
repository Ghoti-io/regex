/**
 * @file
 *
 * glibc's POSIX `regex.h` as a matching oracle, in the shape the other
 * drivers here use.
 *
 * Reads `<flags>\t<pattern hex>\t<subject hex>` lines and writes one answer
 * per line:
 *
 *   match <start>:<end> ...   one span per group, `-` for a group that is unset
 *   nomatch
 *   compile                   the pattern was refused
 *   skip <reason>             the POSIX API cannot be asked this question
 *
 * Hex for the same reason tools/oracle/grx_match.c uses it: a pattern or a
 * subject may contain a newline, a NUL, or bytes that are not valid UTF-8,
 * and the transport should not need an escape of its own.
 *
 * The flag letters are Spencer's own, because his `tests` file is the corpus
 * this exists to import:
 *
 *   b   the pattern is a BRE; without it, an ERE (REG_EXTENDED)
 *   i   REG_ICASE
 *   n   REG_NEWLINE
 *   ^   REG_NOTBOL
 *   $   REG_NOTEOL
 *
 * Offsets are byte offsets, which is what POSIX offsets already are: this
 * driver never calls `setlocale`, so it runs in the C locale, where a
 * character is a byte. That is not a simplification, it is the semantics
 * documentation/dialects.md section 6 says this library implements - "no
 * locale; POSIX classes and case folding are C-locale ASCII".
 *
 * A subject may contain NUL, which a NUL-terminated API cannot express, so
 * the match is asked through `REG_STARTEND` - a glibc extension that takes
 * the extent in `pmatch[0]` instead. A *pattern* containing NUL has no such
 * escape, and is declined rather than truncated.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <gnu/libc-version.h>
#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef REG_STARTEND
#error "this oracle needs REG_STARTEND, which glibc provides"
#endif

/** The longest line the driver will read: two hex blobs and the flags. */
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
 * Decode a hex field in place into `out`.
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

/** Turn the flag letters into the two POSIX flag words. */
static void read_flags(const char * flags, size_t length, int * out_compile,
    int * out_exec) {
  // REG_EXTENDED unless `b` says otherwise: an ERE is the common case and a
  // BRE is the marked one, which is how Spencer's file spells it too.
  int compile_flags = REG_EXTENDED;
  int exec_flags = 0;
  for (size_t i = 0; i < length; i++) {
    switch (flags[i]) {
      case 'b': compile_flags &= ~REG_EXTENDED; break;
      case 'i': compile_flags |= REG_ICASE; break;
      case 'n': compile_flags |= REG_NEWLINE; break;
      case '^': exec_flags |= REG_NOTBOL; break;
      case '$': exec_flags |= REG_NOTEOL; break;
      default: break;
    }
  }
  *out_compile = compile_flags;
  *out_exec = exec_flags;
}

/** Answer one request, having decoded it. */
static void answer(const char * flags, size_t flags_length,
    const char * pattern, size_t pattern_length, const char * subject,
    size_t subject_length) {
  // regcomp takes a NUL-terminated pattern, and there is no REG_STARTEND for
  // the pattern side. A pattern carrying a NUL is declined rather than
  // silently truncated at it, which would answer a different question.
  if (memchr(pattern, '\0', pattern_length)) {
    printf("skip NUL in pattern\n");
    return;
  }

  char * terminated = malloc(pattern_length + 1);
  if (!terminated) {
    printf("skip out of memory\n");
    return;
  }
  memcpy(terminated, pattern, pattern_length);
  terminated[pattern_length] = '\0';

  int compile_flags = 0;
  int exec_flags = 0;
  read_flags(flags, flags_length, &compile_flags, &exec_flags);

  regex_t compiled;
  if (regcomp(&compiled, terminated, compile_flags) != 0) {
    free(terminated);
    printf("compile\n");
    return;
  }
  free(terminated);

  size_t group_count = compiled.re_nsub + 1;
  regmatch_t * spans = calloc(group_count, sizeof(regmatch_t));
  if (!spans) {
    regfree(&compiled);
    printf("skip out of memory\n");
    return;
  }

  // REG_STARTEND passes the extent in pmatch[0] rather than by a NUL, which
  // is the only way to ask about a subject that contains one. The offsets
  // that come back are still relative to the start of the buffer.
  spans[0].rm_so = 0;
  spans[0].rm_eo = (regoff_t)subject_length;
  int result = regexec(
      &compiled, subject, group_count, spans, exec_flags | REG_STARTEND);

  if (result != 0) {
    printf("nomatch\n");
  }
  else {
    printf("match");
    for (size_t i = 0; i < group_count; i++) {
      if (spans[i].rm_so < 0 || spans[i].rm_eo < 0) {
        printf(" -");
      }
      else {
        printf(" %lld:%lld", (long long)spans[i].rm_so,
            (long long)spans[i].rm_eo);
      }
    }
    printf("\n");
  }

  free(spans);
  regfree(&compiled);
}

int main(void) {
  // Announced on stderr the way the other drivers announce theirs, so that a
  // corpus header can record which implementation answered.
  fprintf(stderr, "glibc %s\n", gnu_get_libc_version());

  char * line = malloc(MAX_LINE);
  char * pattern = malloc(MAX_LINE / 2);
  char * subject = malloc(MAX_LINE / 2);
  if (!line || !pattern || !subject) {
    fprintf(stderr, "posix_match: out of memory\n");
    return 2;
  }

  while (fgets(line, MAX_LINE, stdin)) {
    size_t length = strlen(line);
    while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
      line[--length] = '\0';
    }

    // `<flags>\t<pattern>\t<subject>`, and the subject may be empty, so the
    // split is on the first two tabs rather than on every tab.
    char * first = memchr(line, '\t', length);
    if (!first) {
      printf("skip malformed request\n");
      fflush(stdout);
      continue;
    }
    size_t flags_length = (size_t)(first - line);
    char * rest = first + 1;
    size_t rest_length = length - flags_length - 1;
    char * second = memchr(rest, '\t', rest_length);
    if (!second) {
      printf("skip malformed request\n");
      fflush(stdout);
      continue;
    }

    size_t pattern_hex = (size_t)(second - rest);
    size_t subject_hex = rest_length - pattern_hex - 1;
    size_t pattern_length = unhex(rest, pattern_hex, pattern);
    size_t subject_length = unhex(second + 1, subject_hex, subject);
    if (pattern_length == (size_t)-1 || subject_length == (size_t)-1) {
      printf("skip malformed hex\n");
      fflush(stdout);
      continue;
    }

    answer(line, flags_length, pattern, pattern_length, subject,
        subject_length);
    // Flushed per line because the caller writes a request and waits for its
    // answer; a buffered driver deadlocks against a pipe.
    fflush(stdout);
  }

  free(line);
  free(pattern);
  free(subject);
  return 0;
}
