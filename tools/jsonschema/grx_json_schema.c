/**
 * @file
 *
 * Run JSON-Schema-Test-Suite files through `text`, with this library as the
 * regular-expression provider.
 *
 *     grx_json_schema <file.json> [<file.json> ...]
 *
 * This is the other half of WP-11. examples/json_schema_provider.c shows the
 * adapter; this asks whether the adapter is *right*, against the corpus every
 * other JSON Schema implementation is measured with, rather than against
 * cases written by the person who wrote the code.
 *
 * `make check-json-schema-suite` drives it, and is gated on
 * `GRX_JSON_SCHEMA_SUITE` naming a checkout of
 * https://github.com/json-schema-org/JSON-Schema-Test-Suite - the suite is
 * not vendored here, because a vendored copy is a snapshot that stops being
 * the thing everyone else is measured against the moment it is taken.
 *
 * Each file is an array of groups; each group has a schema and a list of
 * instances with the answer expected for each. A group whose schema uses a
 * keyword this engine does not implement is reported as skipped rather than
 * as a failure: `text` refuses such a schema on purpose, and counting that as
 * a wrong answer would hide the wrong answers that matter.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/regex.h>
#include <ghoti.io/text/json.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ===========================================================================
// The adapter, as examples/json_schema_provider.c explains it
// ===========================================================================

typedef struct {
  GRX_Limits limits;
} SchemaRegex;

static int schema_regex_compile(void * ctx, const char * pattern,
    size_t pattern_len, void ** out_regex, char * message,
    size_t message_capacity, size_t * out_offset) {
  SchemaRegex * self = (SchemaRegex *)ctx;
  GRX_Error error;
  grx_error_clear(&error);
  GRX_Regex * regex = NULL;
  GRX_Result result = grx_regex_compile_with_allocator(pattern, pattern_len,
      GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, &self->limits, NULL, &error,
      &regex);
  if (result != GRX_OK) {
    snprintf(message, message_capacity, "%s", error.message);
    *out_offset = error.offset;
    return 1;
  }
  *out_regex = regex;
  return 0;
}

static int schema_regex_search(
    void * ctx, void * regex, const char * subject, size_t subject_len) {
  SchemaRegex * self = (SchemaRegex *)ctx;
  int matched = 0;
  GRX_Result result = grx_regex_search(regex, subject, subject_len, 0,
      GRX_ENGINE_AUTO, &self->limits, NULL, &matched);
  if (result != GRX_OK) {
    return -1;
  }
  return matched ? 1 : 0;
}

static void schema_regex_free(void * ctx, void * regex) {
  (void)ctx;
  grx_regex_free((GRX_Regex *)regex);
}

// ===========================================================================
// The runner
// ===========================================================================

static size_t passed = 0;
static size_t failed = 0;
static size_t skipped_groups = 0;

static char * read_file(const char * path, size_t * out_len) {
  FILE * file = fopen(path, "rb");
  if (!file) {
    return NULL;
  }
  size_t capacity = 65536;
  size_t length = 0;
  char * data = (char *)malloc(capacity);
  if (!data) {
    fclose(file);
    return NULL;
  }
  for (;;) {
    if (length == capacity) {
      char * grown = (char *)realloc(data, capacity * 2);
      if (!grown) {
        free(data);
        fclose(file);
        return NULL;
      }
      data = grown;
      capacity *= 2;
    }
    size_t got = fread(data + length, 1, capacity - length, file);
    length += got;
    if (got == 0) {
      break;
    }
  }
  fclose(file);
  *out_len = length;
  return data;
}

/** A group or case description, or a placeholder; the value is borrowed. */
static const char * description_of(const GTEXT_JSON_Value * value) {
  const GTEXT_JSON_Value * field =
      gtext_json_object_get(value, "description", 11);
  if (!field) {
    return "(no description)";
  }
  const char * text = NULL;
  size_t length = 0;
  if (gtext_json_get_string(field, &text, &length) != GTEXT_JSON_OK) {
    return "(no description)";
  }
  return text;
}

static void run_group(const char * file, const GTEXT_JSON_Value * group,
    const GTEXT_JSON_Regex_Provider * provider) {
  const GTEXT_JSON_Value * schema_doc =
      gtext_json_object_get(group, "schema", 6);
  const GTEXT_JSON_Value * tests = gtext_json_object_get(group, "tests", 5);
  if (!schema_doc || !tests || gtext_json_typeof(tests) != GTEXT_JSON_ARRAY) {
    return;
  }

  GTEXT_JSON_Schema_Options options = gtext_json_schema_options_default();
  options.regex = provider;
  GTEXT_JSON_Error error;
  memset(&error, 0, sizeof(error));
  GTEXT_JSON_Schema * schema =
      gtext_json_schema_compile_with_options(schema_doc, &options, &error);
  if (!schema) {
    /* A keyword `text` does not implement is a skip; anything else - a
     * pattern this library refused, say - is a failure, because the suite's
     * schemas are all valid. */
    if (error.code == GTEXT_JSON_E_SCHEMA_UNSUPPORTED) {
      skipped_groups++;
      printf("SKIP %s: %s (unsupported keyword: %s)\n", file,
          description_of(group),
          error.context_snippet ? error.context_snippet : "?");
    }
    else {
      failed++;
      printf("FAIL %s: %s\n  schema did not compile: %s%s%s\n", file,
          description_of(group), error.message ? error.message : "?",
          error.context_snippet ? " - " : "",
          error.context_snippet ? error.context_snippet : "");
    }
    gtext_json_error_free(&error);
    return;
  }

  size_t count = gtext_json_array_size(tests);
  for (size_t i = 0; i < count; i++) {
    const GTEXT_JSON_Value * test = gtext_json_array_get(tests, i);
    const GTEXT_JSON_Value * data = gtext_json_object_get(test, "data", 4);
    const GTEXT_JSON_Value * valid = gtext_json_object_get(test, "valid", 5);
    if (!data || !valid) {
      continue;
    }
    bool expected = false;
    if (gtext_json_get_bool(valid, &expected) != GTEXT_JSON_OK) {
      continue;
    }

    memset(&error, 0, sizeof(error));
    GTEXT_JSON_Status status = gtext_json_schema_validate(schema, data, &error);
    int actual = (status == GTEXT_JSON_OK);
    /* GTEXT_JSON_E_LIMIT is neither answer, and the suite has no case that
     * should reach it; if one does, it is a failure and worth seeing. */
    if (status != GTEXT_JSON_OK && status != GTEXT_JSON_E_SCHEMA) {
      failed++;
      printf("FAIL %s: %s / %s\n  undecided: %s\n", file,
          description_of(group), description_of(test),
          error.message ? error.message : "?");
    }
    else if (actual != (expected ? 1 : 0)) {
      failed++;
      printf("FAIL %s: %s / %s\n  expected %s, got %s%s%s\n", file,
          description_of(group), description_of(test),
          expected ? "valid" : "invalid", actual ? "valid" : "invalid",
          actual ? "" : ": ", actual ? "" : (error.message ? error.message : ""));
    }
    else {
      passed++;
    }
    gtext_json_error_free(&error);
  }

  gtext_json_schema_free(schema);
}

int main(int argc, char ** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: grx_json_schema <file.json> [<file.json> ...]\n");
    return 2;
  }

  SchemaRegex self;
  grx_limits_default(&self.limits);
  GTEXT_JSON_Regex_Provider provider;
  provider.ctx = &self;
  provider.compile_fn = schema_regex_compile;
  provider.search_fn = schema_regex_search;
  provider.free_fn = schema_regex_free;

  GTEXT_JSON_Parse_Options parse_options = gtext_json_parse_options_default();

  for (int i = 1; i < argc; i++) {
    size_t length = 0;
    char * text = read_file(argv[i], &length);
    if (!text) {
      fprintf(stderr, "cannot read %s\n", argv[i]);
      return 2;
    }
    GTEXT_JSON_Error error;
    memset(&error, 0, sizeof(error));
    GTEXT_JSON_Value * doc =
        gtext_json_parse(text, length, &parse_options, &error);
    if (!doc || gtext_json_typeof(doc) != GTEXT_JSON_ARRAY) {
      fprintf(stderr, "%s is not a JSON-Schema-Test-Suite file\n", argv[i]);
      gtext_json_free(doc);
      free(text);
      return 2;
    }

    /* Only the basename is printed, so that a failure line does not carry
     * whatever directory the suite happens to be checked out into. */
    const char * name = strrchr(argv[i], '/');
    name = name ? name + 1 : argv[i];

    size_t groups = gtext_json_array_size(doc);
    for (size_t g = 0; g < groups; g++) {
      run_group(name, gtext_json_array_get(doc, g), &provider);
    }

    gtext_json_free(doc);
    free(text);
  }

  printf("\n%zu passed, %zu failed, %zu groups skipped\n", passed, failed,
      skipped_groups);
  return failed ? 1 : 0;
}
