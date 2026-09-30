/**
 * @file
 *
 * The two linear dialects, where the refusal *is* the guarantee.
 *
 *     linear_guarantee                       the built-in demonstration
 *     linear_guarantee '<pattern>' '<subject>'
 *     linear_guarantee --rust '<pattern>' '<subject>'
 *     linear_guarantee --replace '<pattern>' '<subject>' '<template>'
 *     linear_guarantee --split '<pattern>' '<subject>'
 *
 * Without `--rust` the pattern is read as `re2`, which is Go's `regexp` and
 * the reference this library was measured against for that row.
 *
 * These two dialects are the only ones here that are defined by what they
 * *lack*. A backreference, a lookaround, an atomic group and a possessive
 * quantifier are all refused at compile time - not because they are hard,
 * but because a dialect that accepts them cannot promise linear time, and
 * linear time is the whole of what these two are for. So the check a caller
 * elsewhere has to do at match time, on every pattern, moves to compile
 * time and happens once.
 *
 * What the built-in demonstration shows:
 *
 *   - **The same pattern, taken and refused.** `(a|aa)+\1b` compiles under
 *     `ecmascript`, and against forty bytes of "a" it spends the whole
 *     step budget and returns GRX_ERR_LIMIT; asked for GRX_ENGINE_PIKE by
 *     name it returns GRX_ERR_UNSUPPORTED, because a lockstep simulation
 *     has nowhere to keep what group 1 captured. Under `re2` and `rust` it
 *     never compiles: there is no program to bound and no engine to choose.
 *   - **`GRX_Facts::is_regular` is 1 for every pattern these two accept.**
 *     Which is the invariant, and the reason to pick one of these dialects:
 *     a caller that must not hang can name GRX_ENGINE_PIKE unconditionally
 *     and know the call cannot come back GRX_ERR_UNSUPPORTED. Under a
 *     dialect with the full feature set the same caller has to read
 *     `is_regular` for each pattern and decide what to do when it is 0.
 *   - **Where the two part is the template, not the pattern.** They agree
 *     on every one of the 5,040 rows of the matching battery in
 *     documentation/dialects.md section 5.5, and four template spellings
 *     mean different things: `$01`, `$00`, `${&}` and `${}`. The sigil is
 *     PCRE2's and almost nothing else is - a bare reference is *one word
 *     run, classified after it is read*, so `$1x` is the name "1x" and
 *     substitutes nothing where PCRE2 gives group 1 and a literal "x".
 *   - **And the split rule.** Both drop what the groups captured, which no
 *     other dialect here does. They part on an empty match at either end of
 *     the subject: `a*` over "baac" is two pieces in Go and four in the
 *     crate (section 5.16).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/regex.h>
#include <stdio.h>
#include <string.h>

/** Enough "a" to make a backtracker's cost visible and not enough to wait. */
#define BLOWUP_LENGTH 40

/** The engine a match actually ran on, for reporting. */
static const char * engine_name(GRX_Engine engine) {
  switch (engine) {
    case GRX_ENGINE_PIKE: return "pike";
    case GRX_ENGINE_BACKTRACK: return "backtrack";
    case GRX_ENGINE_BITSTATE: return "bitstate";
    case GRX_ENGINE_DFA: return "dfa";
    default: return "auto";
  }
}

/** The name of a result code, since three of them are outcomes here. */
static const char * result_name(GRX_Result result) {
  switch (result) {
    case GRX_OK: return "GRX_OK";
    case GRX_ERR_LIMIT: return "GRX_ERR_LIMIT";
    case GRX_ERR_UNSUPPORTED: return "GRX_ERR_UNSUPPORTED";
    default: return "an error";
  }
}

/** Compile, or explain why not and return NULL. */
static GRX_Regex * compile(const char * pattern, GRX_Syntax syntax) {
  GRX_Regex * regex = NULL;
  GRX_Error error;
  grx_error_clear(&error);
  if (grx_regex_compile_with_allocator(pattern, strlen(pattern), syntax, 0,
          NULL, NULL, &error, &regex)
      != GRX_OK) {
    // The message already carries the offset, so naming it again would
    // print it twice. Which text the offset is into is what this program
    // has to say, because `--replace` has a template as well as a pattern.
    //
    // On stdout, unlike the other examples here, because in the
    // demonstration a refusal is the content and not the failure. A caller
    // that passed one pattern learns of it from the exit status.
    printf("  %-10s refused: %s (in the pattern)\n", grx_syntax_name(syntax),
        error.message);
    grx_regex_free(regex);
    return NULL;
  }
  return regex;
}

/**
 * Search on a named engine, reporting the code as well as the span.
 *
 * Returns 0 when the search ran. A refusal is an outcome the demonstration
 * is *for* and a failure when a caller asked for one pattern, so it reaches
 * the exit status through main() rather than being decided here.
 */
static int search_on(const char * pattern, GRX_Syntax syntax,
    const char * subject, size_t length, GRX_Engine engine) {
  GRX_Regex * regex = compile(pattern, syntax);
  if (!regex) {
    return 1;
  }

  GRX_Facts facts;
  grx_regex_facts(regex, &facts);

  GRX_Match * match = NULL;
  if (grx_match_create(regex, NULL, &match) != GRX_OK) {
    grx_regex_free(regex);
    return 1;
  }

  int matched = 0;
  GRX_Result result = grx_regex_search(regex, subject, length, 0, engine,
      NULL, match, &matched);
  printf("  %-10s is_regular=%d  %-8s -> %s", grx_syntax_name(syntax),
      facts.is_regular, engine_name(engine), result_name(result));
  if (result == GRX_ERR_LIMIT) {
    // The number is the cap rather than a measurement of this pattern: the
    // search stopped because it reached GRX_Limits::max_steps, and what it
    // would have cost to finish is the thing nobody wants to find out.
    printf(" after %zu steps", grx_match_steps(match));
  }
  else if (result == GRX_OK) {
    if (matched) {
      GRX_Capture whole;
      grx_match_span(match, &whole);
      printf(" %zu-%zu on %s", whole.start, whole.end,
          engine_name(grx_match_engine(match)));
    }
    else {
      printf(" no match");
    }
  }
  printf("\n");

  grx_match_destroy(match);
  grx_regex_free(regex);
  return result == GRX_OK ? 0 : 1;
}

/** Substitute globally and print the result, or the template's error. */
static int replace_with(const char * pattern, GRX_Syntax syntax,
    const char * subject, size_t length, const char * spelling) {
  GRX_Regex * regex = compile(pattern, syntax);
  if (!regex) {
    return 1;
  }

  GRX_Text out;
  memset(&out, 0, sizeof(out));
  GRX_Error error;
  grx_error_clear(&error);
  if (grx_regex_replace(regex, subject, length, spelling, strlen(spelling),
          GRX_REPLACE_GLOBAL, NULL, NULL, &error, &out)
      != GRX_OK) {
    // An offset in this error is an offset into the *template*. Neither of
    // these two dialects has a template error to report - a reference to a
    // group that does not exist is empty in both - so this arm is here for
    // the malformed input a caller can still pass.
    printf("  %-10s %s (in the template)\n", grx_syntax_name(syntax),
        error.message);
    grx_regex_free(regex);
    return 1;
  }
  printf("  %-10s '%.*s'\n", grx_syntax_name(syntax), (int)out.length,
      out.data);
  grx_text_free(&out);
  grx_regex_free(regex);
  return 0;
}

/** Split and print the pieces, with the count first. */
static int split_by(const char * pattern, GRX_Syntax syntax,
    const char * subject, size_t length) {
  GRX_Regex * regex = compile(pattern, syntax);
  if (!regex) {
    return 1;
  }

  GRX_Split split;
  memset(&split, 0, sizeof(split));
  GRX_Error error;
  grx_error_clear(&error);
  if (grx_regex_split(regex, subject, length, GRX_NPOS, NULL, NULL, &error,
          &split)
      != GRX_OK) {
    printf("  %-10s %s\n", grx_syntax_name(syntax), error.message);
    grx_regex_free(regex);
    return 1;
  }

  // The pattern is in the row and not only in the heading, because the last
  // block below asks two different patterns of the same dialect.
  printf("  %-10s %-6s %zu:", grx_syntax_name(syntax), pattern, split.count);
  for (size_t i = 0; i < split.count; i++) {
    // Neither dialect emits a capture, so no piece here is ever unset -
    // but the field exists for the dialects that do, and a caller writing
    // against the API rather than against one dialect has to read it.
    if (split.pieces[i].start == GRX_NPOS) {
      printf(" <unset>");
      continue;
    }
    printf(" '%.*s'",
        (int)(split.pieces[i].end - split.pieces[i].start),
        subject + split.pieces[i].start);
  }
  printf("\n");

  grx_split_free(&split);
  grx_regex_free(regex);
  return 0;
}

/** The pattern the two dialects refuse, and what accepting it costs. */
static void show_the_refusal(void) {
  char subject[BLOWUP_LENGTH + 1];
  memset(subject, 'a', BLOWUP_LENGTH);
  subject[BLOWUP_LENGTH] = '\0';

  printf("(a|aa)+\\1b against %d bytes of \"a\", which never matches:\n",
      BLOWUP_LENGTH);
  // The backtracker is asked for by name rather than left to
  // GRX_ENGINE_AUTO, because AUTO would pick it anyway here and saying so
  // is the point: with a backreference in the pattern there is no other
  // engine to pick.
  search_on("(a|aa)+\\1b", GRX_SYNTAX_ECMASCRIPT, subject, BLOWUP_LENGTH,
      GRX_ENGINE_BACKTRACK);
  search_on("(a|aa)+\\1b", GRX_SYNTAX_ECMASCRIPT, subject, BLOWUP_LENGTH,
      GRX_ENGINE_PIKE);
  search_on("(a|aa)+\\1b", GRX_SYNTAX_RE2, subject, BLOWUP_LENGTH,
      GRX_ENGINE_PIKE);
  search_on("(a|aa)+\\1b", GRX_SYNTAX_RUST, subject, BLOWUP_LENGTH,
      GRX_ENGINE_PIKE);

  printf("\nthe same shape without the backreference, which these two take:\n");
  search_on("(a|aa)+b", GRX_SYNTAX_RE2, subject, BLOWUP_LENGTH,
      GRX_ENGINE_PIKE);
  search_on("(a|aa)+b", GRX_SYNTAX_RUST, subject, BLOWUP_LENGTH,
      GRX_ENGINE_PIKE);

  printf("\nand a lookahead, which is refused for the same reason:\n");
  search_on("(?=a)ab", GRX_SYNTAX_ECMASCRIPT, "ab", 2, GRX_ENGINE_PIKE);
  search_on("(?=a)ab", GRX_SYNTAX_RE2, "ab", 2, GRX_ENGINE_PIKE);
  search_on("(?=a)ab", GRX_SYNTAX_RUST, "ab", 2, GRX_ENGINE_PIKE);
}

/** The four template spellings the two dialects read differently. */
static void show_the_templates(void) {
  static const char * const forms[] = {"$1", "$1x", "$01", "$00", "${00}",
      "${&}", "${}", "$0", "$&"};

  printf("(a)(b) over \"ab\", nine templates:\n");
  for (size_t i = 0; i < sizeof(forms) / sizeof(forms[0]); i++) {
    printf("  %s\n", forms[i]);
    replace_with("(a)(b)", GRX_SYNTAX_RE2, "ab", 2, forms[i]);
    replace_with("(a)(b)", GRX_SYNTAX_RUST, "ab", 2, forms[i]);
  }
}

/** The split rows where the two part, and the one where they do not. */
static void show_the_split(void) {
  printf("a* over \"baac\" - an empty match at either end:\n");
  split_by("a*", GRX_SYNTAX_RE2, "baac", 4);
  split_by("a*", GRX_SYNTAX_RUST, "baac", 4);

  printf("\n(,) over \"a,b\" - the capture is dropped by both:\n");
  split_by("(,)", GRX_SYNTAX_RE2, "a,b", 3);
  split_by("(,)", GRX_SYNTAX_RUST, "a,b", 3);

  printf("\nx* over \"\" - the empty subject:\n");
  split_by("x*", GRX_SYNTAX_RE2, "", 0);
  split_by("x*", GRX_SYNTAX_RUST, "", 0);

  // Go answers this from the length of the pattern's own source text, so
  // the empty pattern and `(?:)` differ here although they compile to the
  // same program. It is the only cell on the page that no property of a
  // compiled program can decide.
  printf("\n\"\" and (?:) over \"\" - one program, and only Go parts them:\n");
  split_by("", GRX_SYNTAX_RE2, "", 0);
  split_by("(?:)", GRX_SYNTAX_RE2, "", 0);
  split_by("", GRX_SYNTAX_RUST, "", 0);
  split_by("(?:)", GRX_SYNTAX_RUST, "", 0);
}

int main(int argc, char ** argv) {
  GRX_Syntax syntax = GRX_SYNTAX_RE2;
  int replacing = 0;
  int splitting = 0;
  int at = 1;

  while (at < argc && argv[at][0] == '-' && argv[at][1] == '-') {
    if (strcmp(argv[at], "--rust") == 0) {
      syntax = GRX_SYNTAX_RUST;
    }
    else if (strcmp(argv[at], "--replace") == 0) {
      replacing = 1;
    }
    else if (strcmp(argv[at], "--split") == 0) {
      splitting = 1;
    }
    else {
      fprintf(stderr, "%s: unknown option %s\n", argv[0], argv[at]);
      return 2;
    }
    at++;
  }

  if (at >= argc) {
    printf("=== the refusal is the guarantee ===\n\n");
    show_the_refusal();
    printf("\n=== four template spellings, two readings ===\n\n");
    show_the_templates();
    printf("\n=== two split rules ===\n\n");
    show_the_split();
    return 0;
  }

  if (at + 1 >= argc || (replacing && at + 2 >= argc)) {
    fprintf(stderr,
        "usage: %s [--rust] <pattern> <subject>\n"
        "       %s [--rust] --replace <pattern> <subject> <template>\n"
        "       %s [--rust] --split <pattern> <subject>\n"
        "       %s        (with no arguments, the demonstration)\n",
        argv[0], argv[0], argv[0], argv[0]);
    return 2;
  }

  const char * pattern = argv[at];
  const char * subject = argv[at + 1];
  size_t length = strlen(subject);

  if (replacing) {
    return replace_with(pattern, syntax, subject, length, argv[at + 2]);
  }
  if (splitting) {
    return split_by(pattern, syntax, subject, length);
  }
  {
    // GRX_ENGINE_PIKE by name, not GRX_ENGINE_AUTO, because that is the
    // whole claim: for a pattern one of these dialects accepted, asking
    // for the engine with the linear-time bound cannot be refused.
    return search_on(pattern, syntax, subject, length, GRX_ENGINE_PIKE);
  }
}
