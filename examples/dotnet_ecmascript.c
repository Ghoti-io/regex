/**
 * @file
 *
 * .NET Regex syntax: class subtraction, a repeated group name, and the four
 * option bits that spell RegexOptions.ECMAScript.
 *
 *     make examples PREFIX=<installed-prefix>
 *     build/linux/release/apps/examples/dotnet_ecmascript
 *
 * RegexOptions.ECMAScript has no inline spelling on .NET - it is a
 * constructor argument, so there is no `(?E)` to parse and
 * grx_options_parse() cannot reach it. Here it is what it is on .NET: four
 * caller-set bits, which narrow the shorthands to ASCII and change how a
 * numeric escape is read, and narrow nothing else.
 *
 * Every expected answer below is a row measured against the pinned .NET
 * oracle and committed in tests/data/probe/dotnet.json, not a reading of
 * this library.
 *
 * The program checks its own results and exits nonzero on any failure.
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/regex.h>
#include <stdio.h>
#include <string.h>

/* RegexOptions.ECMAScript, measured: exactly these four bits and no others.
 * The ASCII three narrow \d, \s and \w; the fourth reads a numeric escape as
 * the longest digit prefix naming an *already-open* group, so that a number
 * naming none is an octal literal rather than a forward reference. */
#define DOTNET_ECMASCRIPT (GRX_OPT_ASCII_DIGIT | GRX_OPT_ASCII_SPACE \
    | GRX_OPT_ASCII_WORD | GRX_OPT_PREFIX_BACKREFERENCE)

static int fail(const char * operation, GRX_Result result,
    const GRX_Error * error) {
  fprintf(stderr, "dotnet_ecmascript: %s failed: %s%s%s\n", operation,
      grx_result_string(result), error ? ": " : "",
      error ? error->message : "");
  return 1;
}

/* Compile for .NET with the given options and report the first match's
 * whole-match span. Returns 0 on success, 1 on an error worth exiting over. */
static int first_match(const char * pattern, uint32_t options,
    const char * subject, int * out_matched, GRX_Capture * out_span) {
  GRX_Error error;
  GRX_Regex * regex = NULL;
  GRX_Match * match = NULL;
  GRX_Result result;
  int status = 1;

  grx_error_clear(&error);
  result = grx_regex_compile_with_allocator(pattern, strlen(pattern),
      GRX_SYNTAX_DOTNET, options, NULL, NULL, &error, &regex);
  if (result != GRX_OK) {
    return fail("compile", result, &error);
  }
  result = grx_match_create(regex, NULL, &match);
  if (result != GRX_OK) {
    status = fail("create match", result, NULL);
    goto done;
  }
  result = grx_regex_search(regex, subject, strlen(subject), 0,
      GRX_ENGINE_AUTO, NULL, match, out_matched);
  if (result != GRX_OK) {
    status = fail("search", result, NULL);
    goto done;
  }
  if (*out_matched) {
    result = grx_match_group(match, 0, out_span);
    if (result != GRX_OK) {
      status = fail("whole-match span", result, NULL);
      goto done;
    }
  }
  status = 0;

done:
  grx_match_destroy(match);
  grx_regex_free(regex);
  return status;
}

int main(void) {
  /* U+0661 ARABIC-INDIC DIGIT ONE, two bytes in UTF-8. */
  static const char * const arabic_one = "\xD9\xA1";
  GRX_Capture span;
  int matched = 0;

  /* 1. \d takes a non-ASCII decimal digit by default and declines it under
   *    ECMAScript. This is the narrowing, and it is the whole of it for \d. */
  if (first_match("\\d", 0, arabic_one, &matched, &span)) {
    return 1;
  }
  if (!matched || span.start != 0 || span.end != 2) {
    fprintf(stderr, "dotnet_ecmascript: \\d should take U+0661 by default\n");
    return 1;
  }
  if (first_match("\\d", DOTNET_ECMASCRIPT, arabic_one, &matched, &span)) {
    return 1;
  }
  if (matched) {
    fprintf(stderr, "dotnet_ecmascript: \\d should decline U+0661 under E\n");
    return 1;
  }

  /* 2. The prefix-backreference rule. `\1(a)` names no group that is open
   *    yet, so under ECMAScript it is the octal literal U+0001 and matches;
   *    by default .NET reads the number against the total group count, makes
   *    it a forward reference, and the match fails. */
  if (first_match("\\1(a)", DOTNET_ECMASCRIPT, "\x01" "a", &matched, &span)) {
    return 1;
  }
  if (!matched || span.start != 0 || span.end != 2) {
    fprintf(stderr, "dotnet_ecmascript: \\1(a) under E should be U+0001\n");
    return 1;
  }
  if (first_match("\\1(a)", 0, "\x01" "a", &matched, &span)) {
    return 1;
  }
  if (matched) {
    fprintf(stderr, "dotnet_ecmascript: \\1(a) by default is a reference\n");
    return 1;
  }

  /* 3. Class subtraction is .NET's own syntax and ECMAScript narrows nothing
   *    about it: [a-z-[aeiou]] is the consonants under both. */
  if (first_match("[a-z-[aeiou]]", 0, "ab", &matched, &span)) {
    return 1;
  }
  if (!matched || span.start != 1 || span.end != 2) {
    fprintf(stderr, "dotnet_ecmascript: subtraction should skip the 'a'\n");
    return 1;
  }
  if (first_match("[a-z-[aeiou]]", DOTNET_ECMASCRIPT, "ab", &matched, &span)) {
    return 1;
  }
  if (!matched || span.start != 1 || span.end != 2) {
    fprintf(stderr, "dotnet_ecmascript: E should not narrow subtraction\n");
    return 1;
  }

  /* 4. A repeated group name is one group on .NET, not two sharing a name:
   *    the occurrences merge into a single slot, and the group holds
   *    whichever one last participated. */
  {
    GRX_Error error;
    GRX_Regex * regex = NULL;
    GRX_Match * match = NULL;
    GRX_Capture n;
    const char * name;
    GRX_Result result;
    int status = 1;

    grx_error_clear(&error);
    result = grx_regex_compile_with_allocator("(?<n>a)(?<n>b)",
        strlen("(?<n>a)(?<n>b)"), GRX_SYNTAX_DOTNET, 0, NULL, NULL,
        &error, &regex);
    if (result != GRX_OK) {
      return fail("compile repeated name", result, &error);
    }
    name = grx_regex_capture_name(regex, 1);
    if (grx_regex_capture_count(regex) != 1 || name == NULL
        || strcmp(name, "n") != 0) {
      fprintf(stderr, "dotnet_ecmascript: a repeated name is one group\n");
      grx_regex_free(regex);
      return 1;
    }
    result = grx_match_create(regex, NULL, &match);
    if (result != GRX_OK) {
      grx_regex_free(regex);
      return fail("create match", result, NULL);
    }
    result = grx_regex_search(regex, "ab", 2, 0, GRX_ENGINE_AUTO, NULL,
        match, &matched);
    if (result == GRX_OK && matched
        && grx_match_group_named(match, "n", &n) == GRX_OK
        && n.start == 1 && n.end == 2) {
      status = 0;
    }
    else {
      fprintf(stderr, "dotnet_ecmascript: group n should hold the 'b'\n");
    }
    grx_match_destroy(match);
    grx_regex_free(regex);
    if (status != 0) {
      return status;
    }
  }

  printf("default:    \\d takes U+0661; \\1(a) is a forward reference\n");
  printf("ECMAScript: \\d declines it;   \\1(a) is the literal U+0001\n");
  printf("unchanged:  [a-z-[aeiou]] over \"ab\" matches bytes 1..2\n");
  printf("captures:   (?<n>a)(?<n>b) has 1 group, named n, holding \"b\"\n");
  return 0;
}
