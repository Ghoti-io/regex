/**
 * @file
 *
 * Java Pattern syntax and Matcher-style replacement and splitting.
 *
 *     make examples PREFIX=<installed-prefix>
 *     build/linux/release/apps/examples/java_named
 *
 * The program checks its own results and exits nonzero on any failure.
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/regex.h>
#include <stdio.h>
#include <string.h>

static int fail(const char * operation, GRX_Result result,
    const GRX_Error * error) {
  fprintf(stderr, "java_named: %s failed: %s%s%s\n", operation,
      grx_result_string(result), error ? ": " : "",
      error ? error->message : "");
  return 1;
}

int main(void) {
  static const char * const pattern = "(?<key>\\w+)=(?<value>\\d+)";
  static const char * const subject = "caf\xC3\xA9=42";
  GRX_Error error;
  GRX_Regex * regex = NULL;
  GRX_Match * match = NULL;
  GRX_Text replaced = {0};
  GRX_Split split = {0};
  uint32_t options = 0;
  int matched = 0;
  int status = 1;
  GRX_Result result;

  grx_error_clear(&error);
  result = grx_options_parse(GRX_SYNTAX_JAVA, "U", &options, &error);
  if (result != GRX_OK) {
    return fail("parse Java U flag", result, &error);
  }
  result = grx_regex_compile_with_allocator(pattern, strlen(pattern),
      GRX_SYNTAX_JAVA, options, NULL, NULL, &error, &regex);
  if (result != GRX_OK) {
    return fail("compile", result, &error);
  }
  result = grx_match_create(regex, NULL, &match);
  if (result != GRX_OK) {
    status = fail("create match", result, NULL);
    goto done;
  }
  result = grx_regex_search(regex, subject, strlen(subject), 0,
      GRX_ENGINE_AUTO, NULL, match, &matched);
  if (result != GRX_OK) {
    status = fail("search", result, NULL);
    goto done;
  }
  GRX_Capture key, value;
  if (!matched || grx_match_group_named(match, "key", &key) != GRX_OK
      || grx_match_group_named(match, "value", &value) != GRX_OK
      || key.start != 0 || key.end != 5 || value.start != 6
      || value.end != 8) {
    fprintf(stderr, "java_named: unexpected named captures or byte spans\n");
    goto done;
  }

  result = grx_regex_replace(regex, subject, strlen(subject),
      "${value}:${key}", strlen("${value}:${key}"), GRX_REPLACE_NONE,
      NULL, NULL, &error, &replaced);
  if (result != GRX_OK) {
    status = fail("replace", result, &error);
    goto done;
  }
  if (replaced.length != strlen("42:caf\xC3\xA9")
      || memcmp(replaced.data, "42:caf\xC3\xA9", replaced.length) != 0) {
    fprintf(stderr, "java_named: unexpected replacement\n");
    goto done;
  }

  /* Java's negative split limit retains trailing empty fields. */
  GRX_Regex * delimiter = NULL;
  result = grx_regex_compile_with_allocator("=", 1, GRX_SYNTAX_JAVA,
      0, NULL, NULL, &error, &delimiter);
  if (result != GRX_OK) {
    status = fail("compile delimiter", result, &error);
    goto done;
  }
  result = grx_regex_split_java(delimiter, "a=", 2, -1, NULL, NULL,
      &error, &split);
  grx_regex_free(delimiter);
  if (result != GRX_OK) {
    status = fail("split", result, &error);
    goto done;
  }
  if (split.count != 2 || split.pieces[0].start != 0
      || split.pieces[0].end != 1 || split.pieces[1].start != 2
      || split.pieces[1].end != 2) {
    fprintf(stderr, "java_named: unexpected negative-limit split\n");
    goto done;
  }

  printf("Java U: key=caf\xC3\xA9 [bytes 0..5], value=42 [bytes 6..8]\n");
  printf("replacement: %s\n", replaced.data);
  printf("split -1: ['a', '']\n");
  status = 0;

done:
  grx_split_free(&split);
  grx_text_free(&replaced);
  grx_match_destroy(match);
  grx_regex_free(regex);
  return status;
}
