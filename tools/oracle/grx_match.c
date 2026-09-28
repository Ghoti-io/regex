/**
 * @file
 *
 * This library, as a matching oracle: reads pattern-and-subject pairs, prints
 * what matched.
 *
 * Each input line is `<flags>\t<pattern hex>\t<subject hex>`, optionally
 * followed by `\t<begin>,<end>,<search flags>` - the window and the
 * subject-side flags of GRX_SearchOptions. Hex for the same reason the syntax
 * oracle uses it: a pattern or a subject may contain a newline, a NUL or a
 * byte sequence that is not valid UTF-8, and the transport should not need an
 * escape of its own.
 *
 * The window field is optional so that every harness written before it
 * existed keeps working unchanged. `end` may be `-` for "all of it", and the
 * search flags are letters: `B` for NOTBOL, `E` for NOTEOL, `M` for NOTEMPTY
 * and `A` for NOTEMPTY_ATSTART.
 *
 * Each output line is one of:
 *
 *   `match <engine> <start>:<end> ...`  one span per group, `-` for unset
 *   `nomatch`
 *   `unsupported`                        no engine here can run this program
 *   `compile <diag>`                     the pattern was rejected
 *   `error <code>`                       a limit, or a bad subject
 *
 * Offsets are byte offsets into the subject. A reference implementation that
 * counts UTF-16 code units has to convert; tools/oracle/node_match.mjs does.
 *
 * With `all` as the third argument it runs the search-all loop instead - the
 * one grx_regex_search_next() writes for the caller - and answers
 *
 *   `all <count>` then one field per match, groups separated by commas
 *   `all-overflow`                       more matches than the cap below
 *
 * so that `a*` against `"baac"` is `all 4 0:0 1:3 3:3 4:4` rather than four
 * separate questions. The loop is the thing being asked about: which match
 * follows an empty one is the dialect's iteration rule
 * (documentation/dialects.md section 5.10), and it is a rule no single
 * search can be asked about.
 *
 * With `callout` instead it registers a GRX_CalloutFn and reports the
 * *trace* rather than the match:
 *
 *   `trace <outcome> <n>` then one field per callout, in the order they
 *   fired: `number/start/position/pattern_offset/capture_top/string/mark`,
 *   with `-` for an absent string or mark and both of those in hex.
 *
 * `capture_top` is derived here rather than carried in GRX_Callout - it is
 * one more than the highest group set, which is PCRE2's definition and
 * which every entry of `captures` being present already answers. It is in
 * the trace because pcre2's block has it, and a differential can only
 * compare what both sides say.
 *
 * The sequence is backtracking order, so the comparison is only meaningful
 * against a pcre2 compiled with PCRE2_NO_START_OPTIMIZE and
 * PCRE2_NO_AUTO_POSSESS: both of those change which paths are *taken*, and
 * neither is an optimisation this library has. tools/oracle/pcre2_match.c
 * sets them in this mode and in no other.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** The most callouts one row's trace will carry. */
#define MAX_CALLOUTS 4096

/** One row's trace, rebuilt for each row. */
typedef struct {
  char text[1 << 16];
  size_t used;
  size_t count;
  int overflow;
} Trace;

/** Append one callout to the trace. */
static GRX_Result trace_callout(
    const GRX_Callout * callout, int * out_fail, void * data) {
  (void)out_fail;
  Trace * trace = data;
  trace->count++;
  if (trace->count > MAX_CALLOUTS || trace->used + 256 >= sizeof(trace->text)) {
    trace->overflow = 1;
    // Stopping here would change the *match*, which is the one thing a
    // trace must not do: an overflowed row is reported as overflowed and
    // the search is left to finish.
    return GRX_OK;
  }

  // pcre2's capture_top: one more than the highest group set. Derived,
  // because GRX_Callout writes every entry and so has no such field.
  size_t top = 1;
  for (size_t i = 0; i < callout->count; i++) {
    if (callout->captures[i].start != GRX_NPOS) {
      top = i + 1;
    }
  }

  trace->used += (size_t)snprintf(trace->text + trace->used,
      sizeof(trace->text) - trace->used, " %u/%zu/%zu/%zu/%zu/",
      callout->number, callout->start, callout->position,
      callout->pattern_offset, top);
  if (!callout->string) {
    trace->used += (size_t)snprintf(trace->text + trace->used,
        sizeof(trace->text) - trace->used, "-");
  }
  for (size_t i = 0; i < callout->string_length; i++) {
    trace->used += (size_t)snprintf(trace->text + trace->used,
        sizeof(trace->text) - trace->used, "%02x",
        (unsigned char)callout->string[i]);
  }
  trace->used += (size_t)snprintf(trace->text + trace->used,
      sizeof(trace->text) - trace->used, "/");
  if (!callout->mark) {
    trace->used += (size_t)snprintf(trace->text + trace->used,
        sizeof(trace->text) - trace->used, "-");
  }
  for (const char * m = callout->mark; m && *m; m++) {
    trace->used += (size_t)snprintf(trace->text + trace->used,
        sizeof(trace->text) - trace->used, "%02x", (unsigned char)*m);
  }
  return GRX_OK;
}

/** The longest pattern or subject a line may carry. */
/*
 * A pattern this driver will read, and a subject it will scan.
 *
 * Separate, and the subject far larger, because they are asked for
 * different things. They were one 64 KB constant that `decode_hex` silently
 * stopped at, which in a *differential* driver is the worst shape the bug
 * has: Node would have been asked about one pattern and this library about
 * a shorter one, and the gate would have reported agreement about two
 * different questions. Nothing in the current corpora comes near either
 * bound - the oracle's subjects are under twenty bytes - so this is a
 * latent failure being closed rather than an observed one being fixed.
 */
#define MAX_PATTERN 65536
#define MAX_SUBJECT (1 << 20)

/**
 * The most matches the find-all loop will report before giving up.
 *
 * A loop that does not terminate is a defect worth catching, and a driver
 * that hangs reports it as a harness that hangs. Every subject the harnesses
 * here generate is under a few dozen bytes, so any run that reaches this has
 * found something.
 */
#define MAX_MATCHES 100000

static int unhex(int c) {
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
 * Decode a hex field, or refuse it.
 *
 * Returns the number of bytes written, or SIZE_MAX when the field holds
 * more than `capacity` of them.
 */
static size_t decode_hex(const char * hex, char * out, size_t capacity) {
  size_t length = 0;
  while (hex[0] && hex[1]) {
    if (length == capacity) {
      return (size_t)-1;
    }
    int high = unhex(hex[0]);
    int low = unhex(hex[1]);
    if (high < 0 || low < 0) {
      break;
    }
    out[length++] = (char)((high << 4) | low);
    hex += 2;
  }
  return length;
}

static const char * engine_name(GRX_Engine engine) {
  switch (engine) {
    case GRX_ENGINE_PIKE:
      return "pike";
    case GRX_ENGINE_BACKTRACK:
      return "backtrack";
    case GRX_ENGINE_BITSTATE:
      return "bitstate";
    case GRX_ENGINE_DFA:
      return "dfa";
    default:
      return "?";
  }
}

/** One match's spans, in the shape both find-all drivers print. */
static void print_spans(const GRX_Match * match, char separator) {
  for (size_t i = 0; i < grx_match_count(match); i++) {
    if (i) {
      putchar(separator);
    }
    GRX_Capture capture;
    grx_match_group(match, i, &capture);
    if (capture.start == GRX_NPOS) {
      putchar('-');
    }
    else {
      printf("%zu:%zu", capture.start, capture.end);
    }
  }
}

int main(int argc, char ** argv) {
  GRX_Syntax syntax = GRX_SYNTAX_ECMASCRIPT;
  if (argc > 1 && grx_syntax_from_name(argv[1], &syntax) != GRX_OK) {
    fprintf(stderr, "unknown dialect: %s\n", argv[1]);
    return 2;
  }
  int find_all = argc > 3 && strcmp(argv[3], "all") == 0;
  int tracing = (argc > 2 && strcmp(argv[2], "callout") == 0)
      || (argc > 3 && strcmp(argv[3], "callout") == 0);
  GRX_Engine engine = GRX_ENGINE_AUTO;
  if (argc > 2) {
    if (strcmp(argv[2], "pike") == 0) {
      engine = GRX_ENGINE_PIKE;
    }
    else if (strcmp(argv[2], "backtrack") == 0) {
      engine = GRX_ENGINE_BACKTRACK;
    }
    else if (strcmp(argv[2], "dfa") == 0) {
      engine = GRX_ENGINE_DFA;
    }
    else if (strcmp(argv[2], "bitstate") == 0) {
      engine = GRX_ENGINE_BITSTATE;
    }
  }

  static char line[2 * (MAX_PATTERN + MAX_SUBJECT) + 64];
  static char pattern[MAX_PATTERN];
  static char subject[MAX_SUBJECT];

  // The compiled regex is kept for as long as the rows keep asking for the
  // same one. Every harness that drives this tool groups its rows by pattern
  // - one pattern against many subjects - so a one-entry cache is the whole
  // of what is needed, and it is the difference between a run that takes a
  // second and one that takes an hour: `\p{RGI_Emoji}` compiles to an
  // alternation of nearly four thousand sequences, and compiling that once
  // per subject is what a differential over the emoji universe would do.
  static char cached_pattern[MAX_PATTERN];
  static size_t cached_length = 0;
  static uint32_t cached_options = 0;
  static int cached_valid = 0;
  GRX_Regex * regex = NULL;

  while (fgets(line, (int)sizeof(line), stdin)) {
    // A record too long for the buffer arrives without its newline, and its
    // tail would be read as the next record. Drain it and say so.
    if (!strchr(line, '\n') && !feof(stdin)) {
      int c;
      while ((c = fgetc(stdin)) != EOF && c != '\n') {
      }
      printf("toolong\n");
      fflush(stdout);
      continue;
    }

    char * first = strchr(line, '\t');
    if (!first) {
      continue;
    }
    *first = '\0';
    char * second = strchr(first + 1, '\t');
    if (!second) {
      continue;
    }
    *second = '\0';

    // The window, when the harness asked for one. Split off before the
    // subject is decoded: decode_hex stops at the first byte that is not a
    // hex digit, so leaving the tab in place would silently hand the engine
    // a *shorter subject* and report agreement about a different question.
    char * third = strchr(second + 1, '\t');
    if (third) {
      *third = '\0';
    }

    uint32_t options = 0;
    for (const char * f = line; *f; f++) {
      switch (*f) {
        case 'i': options |= GRX_OPT_CASELESS; break;
        case 'm': options |= GRX_OPT_MULTILINE; break;
        case 's': options |= GRX_OPT_DOTALL; break;
        case 'u': options |= GRX_OPT_UTF; break;
        // Perl's `/a` and Python's `re.ASCII`. Here because a letter this
        // table does not know is *silently ignored*, so a generator that
        // ever put one in a flag set would have this driver answering a
        // different question from the reference - agreement and
        // disagreement both meaning nothing. No generator spells it:
        // perl_diff.py reaches every ASCII mode through the pattern -
        // `(?a)` in both dialects and PCRE2's `(?aD)`, `(?aS)`, `(?aW)`,
        // `(?aP)` and `(?aT)` besides - which is where a modifier's scope
        // can be asked about and a flag set's cannot. The five narrower
        // bits WP-46 added therefore have no letter here, on purpose.
        case 'a': options |= GRX_OPT_ASCII_CLASSES; break;
        // Separate from `u`, because PCRE2 separates them and the pair is
        // the axis this library got wrong: `\w`, `\d` and `\s` widen on
        // UCP and the case folding widens on UTF.
        case 'P': options |= GRX_OPT_UCP; break;
        case 'v': options |= GRX_OPT_UNICODE_SETS | GRX_OPT_UTF; break;
        // `n` is REG_NEWLINE, spelled the way posix_match.c and
        // musl_match.c spell it, so that a differential can hand the same
        // flag string to all three. It is *both* of this library's bits,
        // because regcomp has one flag for the two rules: `^` and `$`
        // become line anchors, and a newline stops being "any character".
        case 'n':
          options |= GRX_OPT_MULTILINE | GRX_OPT_NEWLINE_TERMINATES;
          break;
        default: break;
      }
    }

    size_t pattern_length
        = decode_hex(first + 1, pattern, sizeof(pattern));
    size_t subject_length
        = decode_hex(second + 1, subject, sizeof(subject));
    if (pattern_length == (size_t)-1 || subject_length == (size_t)-1) {
      printf("toolong\n");
      fflush(stdout);
      continue;
    }

    if (!regex || !cached_valid || cached_options != options
        || cached_length != pattern_length
        || memcmp(cached_pattern, pattern, pattern_length) != 0) {
      grx_regex_free(regex);
      regex = NULL;

      GRX_Error error;
      grx_error_clear(&error);
      if (grx_regex_compile_with_allocator(pattern, pattern_length, syntax,
              options, NULL, NULL, &error, &regex)
          != GRX_OK) {
        cached_valid = 0;
        regex = NULL;
        printf("compile %d\n", (int)error.diag);
        continue;
      }
      memcpy(cached_pattern, pattern, pattern_length);
      cached_length = pattern_length;
      cached_options = options;
      cached_valid = 1;
    }

    GRX_SearchOptions search;
    grx_search_options_default(&search);
    search.engine = engine;
    if (third) {
      char * cursor = third + 1;
      search.begin = (size_t)strtoull(cursor, &cursor, 10);
      if (*cursor == ',') {
        cursor++;
      }
      if (*cursor == '-') {
        cursor++;
      }
      else {
        search.end = (size_t)strtoull(cursor, &cursor, 10);
      }
      if (*cursor == ',') {
        cursor++;
      }
      for (; *cursor; cursor++) {
        switch (*cursor) {
          case 'B': search.flags |= GRX_SEARCH_NOTBOL; break;
          case 'E': search.flags |= GRX_SEARCH_NOTEOL; break;
          case 'M': search.flags |= GRX_SEARCH_NOTEMPTY; break;
          case 'A': search.flags |= GRX_SEARCH_NOTEMPTY_ATSTART; break;
          default: break;
        }
      }
    }

    static Trace trace;
    if (tracing) {
      trace.used = 0;
      trace.count = 0;
      trace.overflow = 0;
      trace.text[0] = '\0';
      search.callout = trace_callout;
      search.callout_data = &trace;
    }

    GRX_Match * match = NULL;
    if (grx_match_create(regex, NULL, &match) != GRX_OK) {
      printf("error oom\n");
      continue;
    }

    if (tracing) {
      int matched = 0;
      GRX_Result result = grx_regex_search_ex(
          regex, subject, subject_length, &search, match, &matched);
      if (trace.overflow) {
        printf("trace-overflow\n");
      }
      else if (result == GRX_ERR_UNSUPPORTED) {
        printf("unsupported\n");
      }
      else if (result != GRX_OK) {
        printf("error %s\n", grx_result_string(result));
      }
      else {
        printf("trace %s %zu%s\n", matched ? "match" : "nomatch",
            trace.count, trace.text);
      }
      fflush(stdout);
      grx_match_destroy(match);
      continue;
    }

    if (find_all) {
      // Written exactly as exec.h documents it, because the documented loop
      // is what is being compared: a harness that drove the engine some
      // other way would be measuring a loop no caller writes.
      int matched = 0;
      GRX_Result result = grx_regex_search_ex(
          regex, subject, subject_length, &search, match, &matched);
      size_t count = 0;
      // Collected into one buffer rather than printed as they come, because
      // the count belongs at the front and a limit reached half-way through
      // must not leave a partial answer on the wire.
      static char collected[1 << 16];
      size_t used = 0;
      int overflow = 0;
      // Terminated before the loop, not only written inside it: the buffer
      // is static, and a row with no matches at all would otherwise print
      // the *previous* row's matches beside its own count of zero.
      collected[0] = '\0';
      while (result == GRX_OK && matched) {
        if (count >= MAX_MATCHES || used + 64 >= sizeof(collected)) {
          overflow = 1;
          break;
        }
        used += (size_t)snprintf(collected + used, sizeof(collected) - used,
            "%s", count ? " " : "");
        for (size_t i = 0; i < grx_match_count(match); i++) {
          GRX_Capture capture;
          grx_match_group(match, i, &capture);
          if (used + 48 >= sizeof(collected)) {
            overflow = 1;
            break;
          }
          used += (size_t)snprintf(collected + used, sizeof(collected) - used,
              capture.start == GRX_NPOS ? "%s-" : "%s%zu:%zu",
              i ? "," : "", capture.start, capture.end);
        }
        if (overflow) {
          break;
        }
        count++;
        result = grx_regex_search_next(
            regex, subject, subject_length, &search, match, &matched);
      }
      if (overflow) {
        printf("all-overflow\n");
      }
      else if (result == GRX_ERR_UNSUPPORTED) {
        printf("unsupported\n");
      }
      else if (result != GRX_OK) {
        printf("error %s\n", grx_result_string(result));
      }
      else {
        printf("all %zu%s%s\n", count, count ? " " : "", collected);
      }
      fflush(stdout);
      grx_match_destroy(match);
      continue;
    }

    int matched = 0;
    GRX_Result result = grx_regex_search_ex(
        regex, subject, subject_length, &search, match, &matched);

    if (result == GRX_ERR_UNSUPPORTED) {
      printf("unsupported\n");
    }
    else if (result != GRX_OK) {
      printf("error %s\n", grx_result_string(result));
    }
    else if (!matched) {
      printf("nomatch\n");
    }
    else {
      printf("match %s ", engine_name(grx_match_engine(match)));
      print_spans(match, ' ');
      printf("\n");
    }

    grx_match_destroy(match);
  }

  grx_regex_free(regex);
  return 0;
}
