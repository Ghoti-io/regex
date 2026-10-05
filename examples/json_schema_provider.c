/**
 * @file
 *
 * The adapter: this library plugged into `text`'s JSON Schema validator.
 *
 *     json_schema_provider                       # the built-in demonstration
 *     json_schema_provider schema.json data.json # a schema and an instance
 *     json_schema_provider --strict schema.json data.json
 *
 * `text` validates JSON Schema and has no regular-expression engine, so its
 * `pattern` and `patternProperties` keywords arrive through a vtable -
 * `GTEXT_JSON_Regex_Provider` - that a caller fills in. This file is that
 * vtable, implemented with this library, and it is about sixty lines. The
 * rest is a demonstration of what the seam then does.
 *
 * The three obligations `text`'s header states are each a decision made here,
 * and each is somewhere a validator can quietly get it wrong:
 *
 *   - **ECMA-262 with the `u` flag.** `GRX_SYNTAX_ECMASCRIPT | GRX_OPT_UTF`.
 *     A schema author writing `\d` means ECMA-262's `\d`, which is ASCII.
 *     Handing the pattern to a POSIX or Python engine validates different
 *     documents and reports no error while doing it.
 *   - **A search, not an anchored match.** `grx_regex_search()` with a start
 *     of 0, never `grx_regex_match()`. JSON Schema core section 6.4 says the
 *     expression need only match *somewhere*.
 *   - **Bytes and lengths.** Both strings are UTF-8 and neither is
 *     terminated; a JSON string may contain a NUL and a validator that
 *     copied through `strlen` would match the prefix and call it valid.
 *
 * The fourth decision is this library's own contribution, and it is the
 * reason the vtable's `search_fn` has three answers rather than two. A schema
 * is *input*: it arrives over the network as often as it arrives from a
 * developer. `--strict` refuses a pattern that needs the backtracking engine,
 * and without it `GRX_ERR_LIMIT` becomes the negative return, which `text`
 * turns into `GTEXT_JSON_E_LIMIT` - "this instance is neither valid nor
 * invalid" - rather than into a validation failure. Those are different
 * answers and a caller that conflates them has turned a denial-of-service
 * defence into a wrong result.
 *
 * Note which patterns `--strict` actually refuses. `(a+)+$` - the pattern
 * every article about catastrophic backtracking opens with - is **regular**,
 * and runs in linear time here; `(\w+)\1` is not. The policy is written
 * against `grx_regex_facts()`, not against a folklore list of scary-looking
 * patterns. examples/json_schema_pattern.c is the longer version of that
 * argument.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/cutil/file.h>
#include <ghoti.io/regex/regex.h>
#include <ghoti.io/text/json.h>
#include <stdio.h>
#include <string.h>

// ===========================================================================
// The adapter
// ===========================================================================

/**
 * What the three functions below share. One of these per validator, not one
 * per pattern: `limits` is the budget a hostile pattern is held to, and
 * `refuse_irregular` is this validator's policy on patterns that cannot have
 * the linear-time guarantee.
 */
typedef struct {
  GRX_Limits limits;       ///< The resource budget a pattern is compiled under.
  int refuse_irregular;    ///< Non-zero to refuse patterns without the linear-time guarantee.
} SchemaRegex;

static int schema_regex_compile(void * ctx, const char * pattern,
    size_t pattern_len, void ** out_regex, char * message,
    size_t message_capacity, size_t * out_offset) {
  SchemaRegex * self = (SchemaRegex *)ctx;

  GRX_Error error;
  grx_error_clear(&error);
  GRX_Regex * regex = NULL;
  // The dialect, spelled out. Not a default, and not inherited from whatever
  // the rest of the program happens to use.
  GRX_Result result = grx_regex_compile_with_allocator(pattern, pattern_len,
      GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, &self->limits, NULL, &error,
      &regex);
  if (result != GRX_OK) {
    // The library's own words and its own offset. `text` keeps both: the
    // message reaches the caller in context_snippet and the offset in the
    // error's `offset` field, so a schema author is told what to change and
    // where, rather than "invalid schema".
    snprintf(message, message_capacity, "%s", error.message);
    *out_offset = error.offset;
    return 1;
  }

  if (self->refuse_irregular) {
    GRX_Facts facts;
    grx_regex_facts(regex, &facts);
    if (!facts.is_regular) {
      snprintf(message, message_capacity,
          "this pattern needs the backtracking engine, whose worst case is "
          "exponential in the subject length");
      *out_offset = (size_t)-1;
      grx_regex_free(regex);
      return 1;
    }
  }

  *out_regex = regex;
  return 0;
}

static int schema_regex_search(
    void * ctx, void * regex, const char * subject, size_t subject_len) {
  SchemaRegex * self = (SchemaRegex *)ctx;

  int matched = 0;
  // A search from offset 0, and only the yes-or-no is consulted. Reporting
  // *where* it matched would be answering a question JSON Schema does not
  // ask; anchoring would answer a different one wrongly.
  GRX_Result result = grx_regex_search(regex, subject, subject_len, 0,
      GRX_ENGINE_AUTO, &self->limits, NULL, &matched);
  if (result != GRX_OK) {
    // Every failure is the third answer, not "no match". A budget spent is
    // not evidence that the instance is invalid.
    return -1;
  }
  return matched ? 1 : 0;
}

static void schema_regex_free(void * ctx, void * regex) {
  (void)ctx;
  grx_regex_free((GRX_Regex *)regex);
}

/**
 * Fill in the vtable. The provider and everything reachable from its `ctx`
 * must outlive every schema compiled with it: the compiled patterns are
 * released when the schema is freed.
 */
static GTEXT_JSON_Regex_Provider schema_regex_provider(SchemaRegex * self) {
  GTEXT_JSON_Regex_Provider provider;
  provider.ctx = self;
  provider.compile_fn = schema_regex_compile;
  provider.search_fn = schema_regex_search;
  provider.free_fn = schema_regex_free;
  return provider;
}

// ===========================================================================
// The demonstration
// ===========================================================================

/**
 * Read a whole file; the caller frees with gcu_file_free(). Returns NULL and
 * complains on error.
 *
 * cutil's, rather than a loop of its own. The loop that used to be here was
 * one of two copies in this repository and both got the same thing wrong:
 * `fread` returning 0 was read as end-of-file without asking `ferror()`, so a
 * read that failed halfway through produced a short buffer that looked like a
 * complete file - and a *truncated schema* validates differently rather than
 * failing to load. gcu_file_read() answers GCU_FILE_ERR_IO for that case.
 */
static char * read_file(const char * path, size_t * out_len) {
  void * data = NULL;
  GCU_File_Result result =
      gcu_file_read(path, GCU_FILE_UNLIMITED, NULL, &data, out_len);
  if (result != GCU_FILE_OK) {
    fprintf(stderr, "cannot read %s: %s\n", path,
        gcu_file_result_string(result));
    return NULL;
  }
  return (char *)data;
}

/** Report a compile failure the way a schema's author needs to read it. */
static void report_schema_error(const GTEXT_JSON_Error * error) {
  fprintf(stderr, "schema refused: %s\n",
      error->message ? error->message : "(no message)");
  if (error->context_snippet) {
    fprintf(stderr, "  %s", error->context_snippet);
    // (size_t)-1 is "no position", which a refusal about the whole pattern -
    // the --strict one below - genuinely has. Printing "at byte 0" for it
    // would point at a character that is not the problem.
    if (error->code == GTEXT_JSON_E_INVALID && error->offset != (size_t)-1) {
      fprintf(stderr, " (at byte %zu of the pattern)", error->offset);
    }
    fputc('\n', stderr);
  }
}

/**
 * A schema that uses both keywords, and the instances that show what each of
 * them does. Run when no files are given.
 */
static int demonstrate(const GTEXT_JSON_Regex_Provider * provider) {
  static const char * const schema_text =
      "{"
      "  \"type\": \"object\","
      "  \"properties\": { \"name\": { \"type\": \"string\" } },"
      "  \"patternProperties\": {"
      "    \"^x-\": { \"type\": \"string\", \"pattern\": \"^[0-9]+$\" }"
      "  },"
      "  \"additionalProperties\": false"
      "}";

  struct {
    const char * instance;
    const char * why;
  } cases[] = {
      {"{\"name\":\"a\"}", "`properties` named it"},
      {"{\"x-id\":\"42\"}", "matched `^x-`, and its own pattern"},
      {"{\"x-id\":\"forty-two\"}", "matched `^x-`, and failed its pattern"},
      {"{\"other\":1}",
          "neither named nor matched, so `additionalProperties` saw it"},
  };

  GTEXT_JSON_Parse_Options parse_options = gtext_json_parse_options_default();
  GTEXT_JSON_Error error;
  memset(&error, 0, sizeof(error));

  GTEXT_JSON_Value * schema_doc = gtext_json_parse(
      schema_text, strlen(schema_text), &parse_options, &error);
  if (!schema_doc) {
    fprintf(stderr, "the demonstration schema does not parse\n");
    return 1;
  }

  GTEXT_JSON_Schema_Options options = gtext_json_schema_options_default();
  options.regex = provider;
  memset(&error, 0, sizeof(error));
  GTEXT_JSON_Schema * schema =
      gtext_json_schema_compile_with_options(schema_doc, &options, &error);
  if (!schema) {
    report_schema_error(&error);
    gtext_json_error_free(&error);
    gtext_json_free(schema_doc);
    return 1;
  }

  printf("schema:\n%s\n\n", schema_text);
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
    memset(&error, 0, sizeof(error));
    GTEXT_JSON_Value * instance = gtext_json_parse(cases[i].instance,
        strlen(cases[i].instance), &parse_options, &error);
    if (!instance) {
      continue;
    }
    memset(&error, 0, sizeof(error));
    GTEXT_JSON_Status status =
        gtext_json_schema_validate(schema, instance, &error);
    const char * verdict = status == GTEXT_JSON_OK      ? "valid"
        : status == GTEXT_JSON_E_SCHEMA                 ? "invalid"
                                                        : "undecided";
    printf("%-26s %-10s %s\n", cases[i].instance, verdict, cases[i].why);
    if (status != GTEXT_JSON_OK && error.message) {
      printf("%-26s %-10s %s\n", "", "", error.message);
    }
    gtext_json_error_free(&error);
    gtext_json_free(instance);
  }

  gtext_json_schema_free(schema);
  gtext_json_free(schema_doc);
  return 0;
}

int main(int argc, char ** argv) {
  SchemaRegex self;
  grx_limits_default(&self.limits);
  self.refuse_irregular = 0;

  int argi = 1;
  if (argi < argc && strcmp(argv[argi], "--strict") == 0) {
    self.refuse_irregular = 1;
    argi++;
  }

  GTEXT_JSON_Regex_Provider provider = schema_regex_provider(&self);

  if (argc - argi == 0) {
    return demonstrate(&provider);
  }
  if (argc - argi != 2) {
    fprintf(stderr,
        "usage: json_schema_provider [--strict] [<schema.json> "
        "<instance.json>]\n");
    return 2;
  }

  size_t schema_len = 0;
  size_t instance_len = 0;
  char * schema_text = read_file(argv[argi], &schema_len);
  if (!schema_text) {
    return 1;
  }
  char * instance_text = read_file(argv[argi + 1], &instance_len);
  if (!instance_text) {
    gcu_file_free(NULL, schema_text);
    return 1;
  }

  GTEXT_JSON_Parse_Options parse_options = gtext_json_parse_options_default();
  GTEXT_JSON_Error error;
  memset(&error, 0, sizeof(error));

  int exit_code = 1;
  GTEXT_JSON_Value * schema_doc =
      gtext_json_parse(schema_text, schema_len, &parse_options, &error);
  GTEXT_JSON_Value * instance = NULL;
  GTEXT_JSON_Schema * schema = NULL;

  if (!schema_doc) {
    fprintf(stderr, "%s does not parse: %s\n", argv[argi],
        error.message ? error.message : "");
    goto done;
  }
  memset(&error, 0, sizeof(error));
  instance = gtext_json_parse(instance_text, instance_len, &parse_options,
      &error);
  if (!instance) {
    fprintf(stderr, "%s does not parse: %s\n", argv[argi + 1],
        error.message ? error.message : "");
    goto done;
  }

  GTEXT_JSON_Schema_Options options = gtext_json_schema_options_default();
  options.regex = &provider;
  memset(&error, 0, sizeof(error));
  schema = gtext_json_schema_compile_with_options(schema_doc, &options,
      &error);
  if (!schema) {
    report_schema_error(&error);
    gtext_json_error_free(&error);
    goto done;
  }

  memset(&error, 0, sizeof(error));
  GTEXT_JSON_Status status =
      gtext_json_schema_validate(schema, instance, &error);
  if (status == GTEXT_JSON_OK) {
    printf("valid\n");
    exit_code = 0;
  }
  else if (status == GTEXT_JSON_E_SCHEMA) {
    printf("invalid: %s\n", error.message ? error.message : "");
  }
  else {
    // The third answer. Not a validation failure, and the difference matters
    // to whoever is deciding what to do about it.
    printf("undecided: %s\n", error.message ? error.message : "");
    exit_code = 2;
  }
  gtext_json_error_free(&error);

done:
  gtext_json_schema_free(schema);
  gtext_json_free(instance);
  gtext_json_free(schema_doc);
  gcu_file_free(NULL, schema_text);
  gcu_file_free(NULL, instance_text);
  return exit_code;
}
