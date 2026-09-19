/**
 * @file
 *
 * The JSON Schema profile: how a validator should use this library.
 *
 *     json_schema_pattern '^[a-z]+$' 'hello'
 *     json_schema_pattern '(a+)+$' 'aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa'
 *     json_schema_pattern --strict '(\w+)\1' 'abcabc'
 *
 * JSON Schema's `pattern` and `patternProperties` keywords are ECMA-262
 * regular expressions with the `u` flag, matched *unanchored*
 * (documentation/dialects.md section 8.8). That is three decisions, and this
 * example makes all three explicitly, because each of them is somewhere a
 * validator can quietly get it wrong:
 *
 *   - `GRX_SYNTAX_ECMASCRIPT | GRX_OPT_UTF`, not "some regex library's
 *     default dialect". A schema author writing `\d` means ECMA-262's `\d`.
 *   - `grx_regex_search()`, never `grx_regex_match()`. JSON Schema §6.4 says
 *     the pattern need only match *somewhere* in the string, so a validator
 *     that anchors rejects documents the specification accepts.
 *   - `matched` is the only output consulted. Where it matched is not part
 *     of the answer.
 *
 * The fourth decision is the one this example exists to show. A schema is
 * *input*: it arrives over the network as often as it arrives from a
 * developer. So before running anything, ask `grx_regex_facts()` whether the
 * pattern is regular.
 *
 * `(a+)+$` is the pattern every article about catastrophic backtracking opens
 * with, and this library reports it as **regular** - because it is. It has no
 * backreference and no lookaround, so the lockstep engine runs it in time
 * linear in the subject however it is written, and thirty `a`s cost thirty
 * steps rather than a billion. That is the answer a validator wants: not "be
 * careful with this one" but "this one cannot hurt you".
 *
 * `(\w+)\1` is a pattern that genuinely is not regular. A backreference
 * needs to know what a group matched, which a lockstep simulation cannot
 * carry, so it runs on the backtracking engine and its worst case really is
 * exponential - bounded, but only by `max_steps`. A validator has a policy
 * decision to make about those, and `--strict` here is one answer: refuse
 * them. JSON Schema core section 6.4 recommends the regular subset for
 * exactly this reason.
 *
 * The fifth decision is the one `grx_pattern_lint()` answers, and it is a
 * different question from all of the above: not "is this safe to run here"
 * but "does this mean the same thing to every other validator". `\d+` is
 * regular, is safe, and is outside section 6.4's list - because `\d` is
 * ASCII in ECMAScript and Unicode-aware in Python and .NET, so a schema
 * using it validates different documents depending on who reads it. That is
 * worth telling a schema's author, and it is not worth refusing.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/regex.h>
#include <stdio.h>
#include <string.h>

/** Print where a schema's pattern went wrong, the way a validator would. */
static void report_compile_failure(
    const char * pattern, const GRX_Error * error) {
  fprintf(stderr, "the schema's pattern is not a valid ECMAScript regular "
                  "expression:\n  %s\n",
      pattern);
  if (error->offset != GRX_NPOS) {
    fprintf(stderr, "  ");
    for (size_t i = 0; i < error->offset; i++) {
      fputc(' ', stderr);
    }
    size_t length = error->length ? error->length : 1;
    for (size_t i = 0; i < length; i++) {
      fputc('^', stderr);
    }
    fputc('\n', stderr);
  }
  fprintf(stderr, "  %s\n", error->message);
}

int main(int argc, char ** argv) {
  int strict = 0;
  int argi = 1;
  if (argi < argc && strcmp(argv[argi], "--strict") == 0) {
    strict = 1;
    argi++;
  }

  if (argc - argi < 2) {
    fprintf(stderr,
        "usage: json_schema_pattern [--strict] <pattern> <instance>\n");
    return 2;
  }

  const char * pattern = argv[argi];
  const char * instance = argv[argi + 1];

  // JSON Schema's dialect, spelled out. GRX_OPT_UTF is the `u` flag, and it
  // is not optional here: without it `\w` and a caseless match follow
  // ECMA-262's Annex B rules rather than its Unicode ones.
  GRX_Error error;
  grx_error_clear(&error);
  GRX_Pattern * parsed = NULL;
  GRX_Result result = grx_pattern_parse_with_allocator(pattern,
      strlen(pattern), GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, NULL, NULL,
      &error, &parsed);
  if (result != GRX_OK) {
    report_compile_failure(pattern, &error);
    return 1;
  }

  // Whether the pattern stays inside the token list section 6.4 recommends.
  // This is a *portability* answer and not a safety one: a pattern outside
  // the list runs here and means something else somewhere else, which is a
  // warning a validator can pass on to the schema's author. It is asked of
  // the parsed pattern rather than the compiled one, because by the time a
  // pattern is compiled `\d` and `[0-9]` are the same class.
  GRX_LintReport lint;
  grx_pattern_lint(parsed, &lint);

  GRX_Regex * regex = NULL;
  result = grx_regex_compile_pattern(parsed, NULL, NULL, &error, &regex);
  grx_pattern_free(parsed);
  if (result != GRX_OK) {
    report_compile_failure(pattern, &error);
    return 1;
  }

  // What compiling it discovered. `is_regular` is the one that decides
  // whether this pattern is safe to run against input a stranger chose.
  GRX_Facts facts;
  grx_regex_facts(regex, &facts);

  printf("pattern:   %s\n", pattern);
  printf("regular:   %s\n", facts.is_regular ? "yes" : "no");
  printf("engine:    %s\n",
      facts.is_regular ? "lockstep, linear in the subject"
                       : "backtracking, exponential worst case");
  if (facts.min_length) {
    printf("minimum:   %zu bytes\n", facts.min_length);
  }

  printf("portable:  %s\n",
      lint.finding == GRX_LINT_NONE ? "yes, inside JSON Schema 6.4's subset"
                                    : grx_lint_string(lint.finding));
  if (lint.finding != GRX_LINT_NONE && lint.offset != GRX_NPOS) {
    printf("           ");
    for (size_t i = 0; i < lint.offset; i++) {
      putchar(' ');
    }
    putchar('^');
    for (size_t i = 1; i < lint.length; i++) {
      putchar('~');
    }
    putchar('\n');
  }

  if (!facts.is_regular && strict) {
    fprintf(stderr,
        "\nrefused: this pattern needs the backtracking engine, whose worst\n"
        "case is exponential in the subject length. A schema from an\n"
        "untrusted source should not be given one. JSON Schema core section\n"
        "6.4 recommends the regular subset for exactly this reason.\n");
    grx_regex_free(regex);
    return 1;
  }

  // The limits are what bound a pattern that is *not* regular. They are the
  // defaults here; a validator handling untrusted schemas would lower
  // max_steps until the worst case fits its own budget, and would treat
  // GRX_ERR_LIMIT as "this schema is too expensive" rather than as a
  // validation failure - the two are different answers.
  int matched = 0;
  result = grx_regex_search(regex, instance, strlen(instance), 0,
      GRX_ENGINE_AUTO, NULL, NULL, &matched);

  if (result == GRX_ERR_LIMIT) {
    fprintf(stderr, "\nthe match exceeded its budget; the instance is "
                    "neither valid nor invalid\n");
    grx_regex_free(regex);
    return 1;
  }
  if (result != GRX_OK) {
    fprintf(stderr, "\nmatch failed: %s\n", grx_result_string(result));
    grx_regex_free(regex);
    return 1;
  }

  // Unanchored, and only the yes-or-no is used. A validator that reported
  // *where* it matched would be answering a question JSON Schema does not
  // ask.
  printf("instance:  %s\n", instance);
  printf("valid:     %s\n", matched ? "yes" : "no");

  grx_regex_free(regex);
  return matched ? 0 : 1;
}
