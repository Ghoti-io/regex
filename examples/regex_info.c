/**
 * @file
 *
 * Print what the library knows about a dialect, and try to compile a pattern
 * in it.
 *
 *     regex_info                 list every dialect
 *     regex_info pcre            what PCRE has
 *     regex_info pcre '(a|b)+'   compile that pattern as PCRE
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/regex.h>
#include <stdio.h>
#include <string.h>

/** One row of the feature table, for the listing below. */
typedef struct {
  GRX_Feature feature;
  const char * name;
} FeatureName;

static const FeatureName feature_names[] = {
  {GRX_FEATURE_ALTERNATION, "alternation"},
  {GRX_FEATURE_BOUNDED_REPEAT, "bounded-repeat"},
  {GRX_FEATURE_NON_GREEDY, "non-greedy"},
  {GRX_FEATURE_POSSESSIVE, "possessive"},
  {GRX_FEATURE_NON_CAPTURING, "non-capturing"},
  {GRX_FEATURE_NAMED_CAPTURE, "named-capture"},
  {GRX_FEATURE_BACKREFERENCE, "backreference"},
  {GRX_FEATURE_LOOKAHEAD, "lookahead"},
  {GRX_FEATURE_LOOKBEHIND, "lookbehind"},
  {GRX_FEATURE_ATOMIC_GROUP, "atomic-group"},
  {GRX_FEATURE_CONDITIONAL, "conditional"},
  {GRX_FEATURE_RECURSION, "recursion"},
  {GRX_FEATURE_INLINE_FLAGS, "inline-flags"},
  {GRX_FEATURE_COMMENT_GROUP, "comment-group"},
  {GRX_FEATURE_POSIX_CLASS, "posix-class"},
  {GRX_FEATURE_UNICODE_PROPERTY, "unicode-property"},
  {GRX_FEATURE_CLASS_SET_OPS, "class-set-ops"},
  {GRX_FEATURE_WORD_BOUNDARY, "word-boundary"},
  {GRX_FEATURE_ANCHOR_ESCAPES, "anchor-escapes"},
  {GRX_FEATURE_QUOTING, "quoting"},
  {GRX_FEATURE_HEX_ESCAPE, "hex-escape"},
  {GRX_FEATURE_OCTAL_ESCAPE, "octal-escape"},
  {GRX_FEATURE_CONTROL_ESCAPE, "control-escape"},
  {GRX_FEATURE_SUBROUTINE, "subroutine"},
  {GRX_FEATURE_BACKTRACK_CONTROL, "backtrack-control"},
};

static void list_dialects(void) {
  printf("Dialects (ghoti.io-regex %s):\n\n", grx_version_string());
  for (int i = 0; i < GRX_SYNTAX_COUNT; i++) {
    printf("  %s\n", grx_syntax_name((GRX_Syntax)i));
  }
}

static int describe(GRX_Syntax syntax) {
  GRX_SyntaxSpec spec;
  if (grx_syntax_spec(syntax, &spec) != GRX_OK) {
    return 1;
  }

  printf("%s:\n", grx_syntax_name(syntax));
  printf("  operators are %s\n",
      spec.escaped_specials ? "escaped (\\( groups)" : "bare (( groups)");

  printf("  features:");
  for (size_t i = 0; i < GRX_ARRAY_SIZE(feature_names); i++) {
    if (grx_syntax_has_feature(syntax, feature_names[i].feature)) {
      printf(" %s", feature_names[i].name);
    }
  }
  printf("\n");
  return 0;
}

static int compile(GRX_Syntax syntax, const char * pattern) {
  GRX_Error error;
  GRX_Regex * regex = NULL;
  GRX_Result result = grx_regex_compile_with_allocator(pattern,
      strlen(pattern), syntax, GRX_OPT_NONE, NULL, NULL, &error, &regex);

  if (result != GRX_OK) {
    fprintf(stderr, "%s: %s", pattern, grx_result_string(result));
    if (error.message[0]) {
      fprintf(stderr, ": %s", error.message);
    }
    if (error.offset != GRX_NPOS) {
      fprintf(stderr, " (at offset %zu)", error.offset);
    }
    fprintf(stderr, "\n");
    return 1;
  }

  printf("compiled: %zu instruction(s), %zu capture(s)\n",
      grx_regex_program_size(regex), grx_regex_capture_count(regex));
  grx_regex_dump(regex, stdout);
  grx_regex_free(regex);
  return 0;
}

int main(int argc, char ** argv) {
  if (argc < 2) {
    list_dialects();
    return 0;
  }

  GRX_Syntax syntax;
  if (grx_syntax_from_name(argv[1], &syntax) != GRX_OK) {
    fprintf(stderr, "unknown dialect: %s\n", argv[1]);
    list_dialects();
    return 1;
  }

  if (argc == 2) {
    return describe(syntax);
  }

  return compile(syntax, argv[2]);
}
