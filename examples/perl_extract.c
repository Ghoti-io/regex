/**
 * @file
 *
 * The Perl family: named groups, extended mode, lookaround, and the one
 * place PCRE2 and Perl spell the same idea differently.
 *
 *     perl_extract                        the built-in demonstration
 *     perl_extract '(?<k>\w+)=(?<v>\S+)' 'a=1 b=2'
 *     perl_extract --perl '<pattern>' '<subject>'
 *
 * Without `--perl` the pattern is read as PCRE2, which is the default
 * because pcre2test is the reference this library was measured against.
 *
 * What the built-in demonstration shows, and why each is here:
 *
 *   - **Named groups**, `(?<name>...)`, read back by name rather than by a
 *     number that changes whenever somebody adds a group.
 *   - **Extended mode**, where whitespace and `#` comments in the pattern
 *     are ignored, which is the only way a pattern this long stays readable.
 *   - **A lookahead**, `(?=...)`, which asserts without consuming - so the
 *     text it checks is still there for the next part of the pattern to
 *     match. Neither POSIX dialect has one.
 *   - **The template difference.** A named group is `${name}` in a PCRE2
 *     template and `$+{name}` in a Perl one, because Perl's templates are
 *     interpolated strings and `%+` is where a Perl program finds its named
 *     captures. Same pattern, same match, two spellings for writing it out.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/regex.h>
#include <stdio.h>
#include <string.h>

/** Compile, or explain why not and return NULL. */
static GRX_Regex * compile(const char * pattern, GRX_Syntax syntax,
    uint32_t options) {
  GRX_Regex * regex = NULL;
  GRX_Error error;
  grx_error_clear(&error);
  if (grx_regex_compile_with_allocator(pattern, strlen(pattern), syntax,
          options, NULL, NULL, &error, &regex)
      != GRX_OK) {
    fprintf(stderr, "%s refused: %s (byte %zu)\n", grx_syntax_name(syntax),
        error.message, error.offset);
    grx_regex_free(regex);
    return NULL;
  }
  return regex;
}

/** Print every match, naming the groups the pattern named. */
static void report(const GRX_Regex * regex, const char * subject) {
  size_t length = strlen(subject);
  GRX_Match * match = NULL;
  if (grx_match_create(regex, NULL, &match) != GRX_OK) {
    return;
  }

  int matched = 0;
  size_t index = 0;
  while (grx_regex_search_next(regex, subject, length, NULL, match, &matched)
          == GRX_OK
      && matched) {
    GRX_Capture whole;
    grx_match_span(match, &whole);
    printf("  match %zu: %.*s\n", ++index,
        (int)(whole.end - whole.start), subject + whole.start);

    size_t groups = grx_match_count(match);
    for (size_t i = 1; i < groups; i++) {
      GRX_Capture capture;
      grx_match_group(match, i, &capture);
      // A group's name, when it has one. A pattern may mix named and
      // numbered groups, and the numbering counts both.
      const char * name = grx_regex_capture_name(regex, i);
      if (capture.start == GRX_NPOS) {
        printf("      %-8s (unset)\n", name ? name : "");
      }
      else {
        printf("      %-8s %.*s\n", name ? name : "",
            (int)(capture.end - capture.start), subject + capture.start);
      }
    }
  }
  if (!index) {
    printf("  no match\n");
  }
  grx_match_destroy(match);
}

/** Substitute, and say what came out. */
static void rewrite(const GRX_Regex * regex, const char * subject,
    const char * tmpl) {
  GRX_Text out;
  memset(&out, 0, sizeof(out));
  GRX_Error error;
  grx_error_clear(&error);
  if (grx_regex_replace(regex, subject, strlen(subject), tmpl, strlen(tmpl),
          GRX_REPLACE_GLOBAL, NULL, NULL, &error, &out)
      == GRX_OK) {
    printf("  %-18s -> %.*s\n", tmpl, (int)out.length, out.data);
    grx_text_free(&out);
  }
  else {
    printf("  %-18s -> refused: %s\n", tmpl, error.message);
  }
}

/** The demonstration, when no pattern was given. */
static int demonstrate(void) {
  // Extended mode: the whitespace and the comments are not part of the
  // pattern. The lookahead asserts that a unit follows the number without
  // consuming it, so `(?<unit>...)` can then match the very same text.
  static const char * const pattern =
      "(?<value> \\d+ )      # the number\n"
      "(?= [kMG]B )          # ...but only when a unit follows\n"
      "(?<unit> [kMG] ) B    # and here it is, read again";

  printf("pattern, in extended mode:\n%s\n\n", pattern);

  static const char * const subject = "12kB then 7 then 340MB";
  printf("subject: %s\n\n", subject);

  GRX_Regex * pcre = compile(pattern, GRX_SYNTAX_PCRE, GRX_OPT_EXTENDED);
  if (!pcre) {
    return 1;
  }
  printf("as PCRE2:\n");
  report(pcre, subject);

  printf("\ntemplates, which is where the two dialects part:\n");
  rewrite(pcre, subject, "${value} ${unit}");
  grx_regex_free(pcre);

  GRX_Regex * perl = compile(pattern, GRX_SYNTAX_PERL, GRX_OPT_EXTENDED);
  if (!perl) {
    return 1;
  }
  rewrite(perl, subject, "$+{value} $+{unit}");
  // PCRE2's spelling, put to Perl, to show that this is a real difference
  // and not a convention. `${name}` is not a named-capture spelling in a
  // Perl template at all - `${...}` there is a scalar variable - so it is
  // not an unresolvable reference, it is ordinary text, and it comes out
  // exactly as written.
  rewrite(perl, subject, "${value}");
  grx_regex_free(perl);
  return 0;
}

int main(int argc, char ** argv) {
  int argi = 1;
  GRX_Syntax syntax = GRX_SYNTAX_PCRE;
  if (argi < argc && strcmp(argv[argi], "--perl") == 0) {
    syntax = GRX_SYNTAX_PERL;
    argi++;
  }

  if (argi >= argc) {
    return demonstrate();
  }
  if (argi + 1 >= argc) {
    fprintf(stderr, "usage: %s [--perl] <pattern> <subject>\n", argv[0]);
    return 2;
  }

  GRX_Regex * regex = compile(argv[argi], syntax, 0);
  if (!regex) {
    return 1;
  }
  report(regex, argv[argi + 1]);
  grx_regex_free(regex);
  return 0;
}
