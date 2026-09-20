/**
 * @file
 *
 * musl's POSIX `regex.h` as a matching oracle, in the shape the other
 * drivers here use.
 *
 * The protocol is `tools/oracle/posix_match.c`'s exactly, so that one
 * harness can drive either and the answers can be compared line for line:
 * `<flags>\t<pattern hex>\t<subject hex>` in, and one of
 *
 *   match <start>:<end> ...   one span per group, `-` for a group that is unset
 *   nomatch
 *   compile                   the pattern was refused
 *   skip <reason>             this oracle cannot be asked this question
 *
 * out. The flag letters are Spencer's, again as in `posix_match.c`: `b` for
 * a BRE, `i` for `REG_ICASE`, `n` for `REG_NEWLINE`, `^` for `REG_NOTBOL`,
 * `$` for `REG_NOTEOL`.
 *
 * *Why* a second one of these is `tools/corpus/VERSIONS`'s subject: glibc's
 * `regcomp` is GNU and so answers what GNU does, which leaves the two
 * `posix-*` rows of the spec table with nothing to be measured against.
 * musl's regex descends from Ville Laurikari's TRE rather than from glibc,
 * which makes it a second reading rather than a second spelling of the
 * first.
 *
 * It is musl's regex *hosted on glibc* - two translation units compiled
 * against glibc's headers and linked against glibc's `mbtowc` and
 * `iswctype` - and it therefore declines two kinds of question outright
 * rather than answering them wrongly:
 *
 *   - A NUL anywhere. musl has no `REG_STARTEND`, so a subject containing
 *     NUL cannot be expressed through its API at all; a pattern containing
 *     one cannot be expressed through anybody's. `posix_match.c` can answer
 *     the subject half of that and this driver cannot, which is a real
 *     difference in reach between the two oracles and not a bug in either.
 *   - Any byte >= 0x80. In the C locale glibc's `mbtowc` refuses such a
 *     byte where musl's would decode it as UTF-8, so an answer here would
 *     be a fact about the host libc rather than about musl's regex.
 *
 * Both are declined rather than skipped silently, so a harness counts them.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef REG_STARTEND
#error "this is glibc's regex.h; musl's regex needs -I tools/oracle/musl-include"
#endif

#ifndef GRX_MUSL_REF
#define GRX_MUSL_REF "unknown"
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

/** Whether every byte is one this hosted build can speak for. */
static int is_ascii(const char * text, size_t length) {
  for (size_t i = 0; i < length; i++) {
    if ((unsigned char)text[i] >= 0x80) {
      return 0;
    }
  }
  return 1;
}

/** Turn the flag letters into the two POSIX flag words. */
static void read_flags(const char * flags, size_t length, int * out_compile,
    int * out_exec) {
  // REG_EXTENDED unless `b` says otherwise, which is how Spencer's file
  // spells it and how posix_match.c reads it.
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
  // Without REG_STARTEND a subject is what lies before its first NUL, which
  // is a different question from the one asked.
  if (memchr(pattern, '\0', pattern_length)) {
    printf("skip NUL in pattern\n");
    return;
  }
  if (memchr(subject, '\0', subject_length)) {
    printf("skip NUL in subject\n");
    return;
  }
  if (!is_ascii(pattern, pattern_length)
      || !is_ascii(subject, subject_length)) {
    printf("skip non-ASCII byte\n");
    return;
  }

  char * terminated_pattern = malloc(pattern_length + 1);
  char * terminated_subject = malloc(subject_length + 1);
  if (!terminated_pattern || !terminated_subject) {
    free(terminated_pattern);
    free(terminated_subject);
    printf("skip out of memory\n");
    return;
  }
  memcpy(terminated_pattern, pattern, pattern_length);
  terminated_pattern[pattern_length] = '\0';
  memcpy(terminated_subject, subject, subject_length);
  terminated_subject[subject_length] = '\0';

  int compile_flags = 0;
  int exec_flags = 0;
  read_flags(flags, flags_length, &compile_flags, &exec_flags);

  regex_t compiled;
  if (musl_regcomp(&compiled, terminated_pattern, compile_flags) != 0) {
    free(terminated_pattern);
    free(terminated_subject);
    printf("compile\n");
    return;
  }
  free(terminated_pattern);

  size_t group_count = compiled.re_nsub + 1;
  regmatch_t * spans = calloc(group_count, sizeof(regmatch_t));
  if (!spans) {
    musl_regfree(&compiled);
    free(terminated_subject);
    printf("skip out of memory\n");
    return;
  }

  int result = musl_regexec(
      &compiled, terminated_subject, group_count, spans, exec_flags);

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
  free(terminated_subject);
  musl_regfree(&compiled);
}

int main(void) {
  // The ref comes from tools/corpus/VERSIONS by way of the Makefile, so that
  // a corpus header can record which implementation answered and at which
  // version - there is no musl on this machine to ask at run time.
  fprintf(stderr, "musl %s (hosted on glibc; ASCII, no NUL)\n", GRX_MUSL_REF);

  char * line = malloc(MAX_LINE);
  char * pattern = malloc(MAX_LINE / 2);
  char * subject = malloc(MAX_LINE / 2);
  if (!line || !pattern || !subject) {
    fprintf(stderr, "musl_match: out of memory\n");
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
