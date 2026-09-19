/**
 * @file
 *
 * What one pattern costs, measured rather than guessed.
 *
 * Every field of GRX_Limits is a promise about a quantity, and a default is
 * only defensible if somebody has looked at what real patterns actually use.
 * This driver answers that for one pattern at a time: for each compile-time
 * limit it binary-searches the smallest value at which the pattern still
 * compiles, which is exactly how much of that resource the pattern needs.
 *
 * Binary search rather than instrumentation, because the alternative is to
 * add a counter to every phase and export it - a second way of computing
 * each number, with a second chance of being wrong about it. The limits
 * themselves are already enforced in one place each; asking them is asking
 * the thing that will do the refusing.
 *
 * Protocol, so that it composes with the oracle harnesses:
 *
 *     stdin:   <flags>\t<pattern as hex>\n
 *     stdout:  <pattern_length> <nesting> <nodes> <captures> <repeat>
 *              <class_ranges> <program> <lookbehind>\n
 *
 * A pattern that does not compile at any limit prints `- - - - - - - -`.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/regex.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_BUFFER 65536

/** Which field of GRX_Limits a probe is varying. */
typedef enum {
  FIELD_NESTING = 0,
  FIELD_NODES,
  FIELD_CAPTURES,
  FIELD_REPEAT,
  FIELD_CLASS_RANGES,
  FIELD_PROGRAM,
  FIELD_LOOKBEHIND,
  FIELD_COUNT
} Field;

static const char * const kFieldNames[FIELD_COUNT] = {
  "max_nesting_depth", "max_nodes", "max_captures", "max_repeat_count",
  "max_class_ranges", "max_program_size", "max_lookbehind_length",
};

/** Everything unlimited, so that one field at a time is the only bound. */
static void unlimited(GRX_Limits * limits) {
  memset(limits, 0, sizeof(*limits));
}

static void set_field(GRX_Limits * limits, Field field, size_t value) {
  switch (field) {
    case FIELD_NESTING: limits->max_nesting_depth = value; break;
    case FIELD_NODES: limits->max_nodes = value; break;
    case FIELD_CAPTURES: limits->max_captures = value; break;
    case FIELD_REPEAT: limits->max_repeat_count = value; break;
    case FIELD_CLASS_RANGES: limits->max_class_ranges = value; break;
    case FIELD_PROGRAM: limits->max_program_size = value; break;
    case FIELD_LOOKBEHIND: limits->max_lookbehind_length = value; break;
    case FIELD_COUNT: break;
  }
}

/** Whether the pattern compiles with this one field set to `value`. */
static int compiles(const char * pattern, size_t length, uint32_t options,
    Field field, size_t value) {
  GRX_Limits limits;
  unlimited(&limits);
  set_field(&limits, field, value);

  GRX_Regex * regex = NULL;
  GRX_Result result = grx_regex_compile_with_allocator(pattern, length,
      GRX_SYNTAX_ECMASCRIPT, options, &limits, NULL, NULL, &regex);
  grx_regex_free(regex);
  return result == GRX_OK;
}

/**
 * The smallest value of `field` at which the pattern compiles.
 *
 * Doubling first and then bisecting, because the answer is usually small and
 * the ceiling is not known: a pattern with a big `{n,m}` can need a program
 * a hundred thousand instructions long, and starting the search there would
 * cost seventeen compiles for every pattern that needs three.
 */
static size_t needed(const char * pattern, size_t length, uint32_t options,
    Field field, size_t ceiling) {
  size_t high = 1;
  while (high < ceiling && !compiles(pattern, length, options, field, high)) {
    high *= 2;
  }
  if (!compiles(pattern, length, options, field, high)) {
    return 0; // Does not compile at any value this is willing to try.
  }

  size_t low = high / 2 + 1;
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    if (compiles(pattern, length, options, field, middle)) {
      high = middle;
    }
    else {
      low = middle + 1;
    }
  }
  return low;
}

static size_t decode_hex(const char * hex, char * out, size_t capacity) {
  size_t length = 0;
  while (hex[0] && hex[1] && length < capacity) {
    char byte[3] = {hex[0], hex[1], '\0'};
    out[length++] = (char)strtol(byte, NULL, 16);
    hex += 2;
  }
  return length;
}

/**
 * How many steps one search costs, at no limit at all.
 *
 * The other half of what max_steps has to be: a cap has to be above what a
 * legitimate match costs as well as below what a pathological one does, and
 * the first of those is a function of the program's size and the subject's
 * length rather than of anything a corpus of patterns alone can show.
 */
static void report_steps(const char * pattern, size_t pattern_length,
    uint32_t options, const char * subject, size_t subject_length) {
  GRX_Limits limits;
  unlimited(&limits);

  GRX_Regex * regex = NULL;
  if (grx_regex_compile_with_allocator(pattern, pattern_length,
          GRX_SYNTAX_ECMASCRIPT, options, &limits, NULL, NULL, &regex)
      != GRX_OK) {
    printf("- - -\n");
    return;
  }

  GRX_Facts facts;
  grx_regex_facts(regex, &facts);

  GRX_Match * match = NULL;
  if (grx_match_create(regex, NULL, &match) != GRX_OK) {
    grx_regex_free(regex);
    printf("- - -\n");
    return;
  }

  int matched = 0;
  GRX_Result result = grx_regex_search(regex, subject, subject_length, 0,
      GRX_ENGINE_AUTO, &limits, match, &matched);
  printf("%zu %zu %s\n", facts.program_size,
      result == GRX_OK ? grx_match_steps(match) : (size_t)0,
      result == GRX_OK ? (matched ? "match" : "nomatch")
                       : grx_result_string(result));

  grx_match_destroy(match);
  grx_regex_free(regex);
}

/** The library's own defaults, so a report never transcribes them. */
static void report_defaults(void) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  printf("max_pattern_length %zu\n", limits.max_pattern_length);
  printf("max_nesting_depth %zu\n", limits.max_nesting_depth);
  printf("max_nodes %zu\n", limits.max_nodes);
  printf("max_captures %zu\n", limits.max_captures);
  printf("max_repeat_count %zu\n", limits.max_repeat_count);
  printf("max_class_ranges %zu\n", limits.max_class_ranges);
  printf("max_program_size %zu\n", limits.max_program_size);
  printf("max_lookbehind_length %zu\n", limits.max_lookbehind_length);
  printf("max_recursion_depth %zu\n", limits.max_recursion_depth);
  printf("max_steps %zu\n", limits.max_steps);
  printf("max_backtrack %zu\n", limits.max_backtrack);
  printf("max_match_memory %zu\n", limits.max_match_memory);
  printf("max_subject_length %zu\n", limits.max_subject_length);
}

int main(int argc, char ** argv) {
  if (argc > 1 && strcmp(argv[1], "--fields") == 0) {
    for (int i = 0; i < FIELD_COUNT; i++) {
      printf("%s%s", i ? " " : "", kFieldNames[i]);
    }
    printf("\n");
    return 0;
  }
  if (argc > 1 && strcmp(argv[1], "--defaults") == 0) {
    report_defaults();
    return 0;
  }
  int steps_mode = argc > 1 && strcmp(argv[1], "--steps") == 0;

  static char line[3 * MAX_BUFFER];
  static char pattern[MAX_BUFFER];
  static char subject[MAX_BUFFER];

  while (fgets(line, (int)sizeof(line), stdin)) {
    char * tab = strchr(line, '\t');
    if (!tab) {
      continue;
    }
    *tab = '\0';

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

    if (steps_mode) {
      char * second = strchr(tab + 1, '\t');
      if (!second) {
        continue;
      }
      *second = '\0';
      size_t pattern_length
          = decode_hex(tab + 1, pattern, sizeof(pattern));
      size_t subject_length
          = decode_hex(second + 1, subject, sizeof(subject));
      report_steps(pattern, pattern_length, options, subject, subject_length);
      fflush(stdout);
      continue;
    }

    size_t length = decode_hex(tab + 1, pattern, sizeof(pattern));
    if (!compiles(pattern, length, options, FIELD_NODES, 0)) {
      printf("- - - - - - - -\n");
      continue;
    }

    printf("%zu", length);
    for (int i = 0; i < FIELD_COUNT; i++) {
      // The ceiling is generous but finite: a pattern that needs more than
      // this is one whose cost this tool should report rather than spend a
      // minute bisecting.
      printf(" %zu",
          needed(pattern, length, options, (Field)i, (size_t)1 << 24));
    }
    printf("\n");
    fflush(stdout);
  }

  return 0;
}
