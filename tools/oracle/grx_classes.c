/**
 * @file
 *
 * This library's POSIX class and `\w` membership, dumped for comparison
 * against perl and pcre2.
 *
 * `[[:alpha:]]` and its thirteen siblings are *table* rules, and the figures
 * documentation/dialects.md section 5.9 quotes for how far perl's answer and
 * PCRE2's are apart - 1,694 code points for `alpha`, 1,513 for `\w`, 137,468
 * for `graph` - were taken by hand once, against perl 5.40.1, over an
 * intersection that release defined. The page says so and says "re-take the
 * figures before quoting them", which is a bare number's own admission that
 * nothing can check it. This is the instrument that re-takes them.
 *
 * One line per run, per class:
 *
 *     <class> <lo> <hi>
 *
 * in hexadecimal, `lo` and `hi` inclusive, naming the code points the class
 * *contains*. Runs rather than a bitmap because the sets are blocky and the
 * output is then small enough to read; `<class>` rather than one stream
 * because the caller asks for several at once and the container start-up is
 * most of the cost on the other side.
 *
 * The surrogate block is swept like any other code point. It cannot appear in
 * a UTF-8 subject, so no class contains it and all three sides agree by
 * construction; leaving it out would make the three outputs disagree about
 * their own length for a reason that is not a rule.
 *
 * Usage:  grx_classes <dialect> [class ...]
 *         grx_classes <dialect> gc <hex code point> ...
 *
 * With no class named it sweeps all fifteen. A dialect is a name
 * grx_syntax_from_name() knows; the options are that dialect's UTF and UCP,
 * because the question is what the dialect's *wide* classes hold and a
 * narrow answer would be the same for every dialect here.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/**
 * The fourteen POSIX names this library has, plus the `\w` shorthand.
 *
 * `assigned` is accepted as a name too and is not in this list: it is
 * `\P{Cn}`, which the comparison needs and which is not a class anybody
 * writes. A caller asks for it explicitly.
 */
static const char * const CLASSES[] = {
  "alnum", "alpha", "ascii", "blank", "cntrl", "digit", "graph", "lower",
  "print", "punct", "space", "upper", "word", "xdigit", "w",
};
static const size_t CLASS_COUNT = sizeof(CLASSES) / sizeof(*CLASSES);

/** The UTF-8 of one code point, and its length. */
static size_t encode(uint32_t codepoint, char * out) {
  if (codepoint < 0x80) {
    out[0] = (char)codepoint;
    return 1;
  }
  if (codepoint < 0x800) {
    out[0] = (char)(0xC0 | (codepoint >> 6));
    out[1] = (char)(0x80 | (codepoint & 0x3F));
    return 2;
  }
  if (codepoint < 0x10000) {
    out[0] = (char)(0xE0 | (codepoint >> 12));
    out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
    out[2] = (char)(0x80 | (codepoint & 0x3F));
    return 3;
  }
  out[0] = (char)(0xF0 | (codepoint >> 18));
  out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
  out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
  out[3] = (char)(0x80 | (codepoint & 0x3F));
  return 4;
}

/** Sweep one class and print its runs. */
static int sweep(GRX_Syntax syntax, const char * name) {
  char pattern[40];
  if (strcmp(name, "w") == 0) {
    /* `w` and not `\\w`: one spelling in the protocol, with no backslash for
     * a caller to quote. */
    snprintf(pattern, sizeof pattern, "\\w");
  }
  else if (strcmp(name, "assigned") == 0) {
    /* Not a POSIX class. It is here because the comparison has to be
     * restricted to what all three sides call assigned, and this library's
     * half of that restriction has to come from this library rather than be
     * assumed to be everything: unrestricted, the sweep measures which
     * Unicode release each side carries. */
    snprintf(pattern, sizeof pattern, "\\P{Cn}");
  }
  else {
    snprintf(pattern, sizeof pattern, "[[:%s:]]", name);
  }

  GRX_Regex * regex = NULL;
  GRX_Result result = grx_regex_compile(pattern, syntax,
      GRX_OPT_UTF | GRX_OPT_UCP, &regex);
  if (result != GRX_OK) {
    /* A dialect without the construct, which is an answer and not a failure:
     * `word` and `\w` exist everywhere here, and `[[:alpha:]]` does not exist
     * in ECMAScript. The caller sees the class missing from the output. */
    fprintf(stderr, "grx_classes: %s refused /%s/\n",
        grx_syntax_name(syntax), pattern);
    grx_regex_free(regex);
    return 0;
  }

  long start = -1;
  for (uint32_t codepoint = 0; codepoint <= 0x110000; codepoint++) {
    int member = 0;
    if (codepoint <= 0x10FFFF) {
      char bytes[4];
      size_t length = encode(codepoint, bytes);
      int matched = 0;
      if (grx_regex_match(regex, bytes, length, 0, GRX_ENGINE_AUTO, NULL,
              NULL, &matched) == GRX_OK) {
        member = matched;
      }
    }
    if (member && start < 0) {
      start = (long)codepoint;
    }
    else if (!member && start >= 0) {
      printf("%s %lX %lX\n", name, start, (long)codepoint - 1);
      start = -1;
    }
  }

  grx_regex_free(regex);
  return 1;
}

/** The thirty General_Category values, for the `gc` query below. */
static const char * const CATEGORIES[] = {
  "Lu", "Ll", "Lt", "Lm", "Lo", "Mn", "Mc", "Me", "Nd", "Nl", "No",
  "Pc", "Pd", "Ps", "Pe", "Pi", "Pf", "Po", "Sm", "Sc", "Sk", "So",
  "Zs", "Zl", "Zp", "Cc", "Cf", "Cs", "Co", "Cn",
};

/**
 * Print this library's General_Category for one code point.
 *
 * Here because a disagreement between a class here and the same class in a
 * reference is only a *rule* difference when both sides agree about the code
 * point's category; where they do not, the reference is carrying an older
 * UCD and the comparison is measuring the release. That is a second
 * measurement rather than a pattern match, which is what
 * tools/oracle/wide_class_diff.py needs in order to excuse a row without
 * excusing a family.
 */
static void print_category(GRX_Syntax syntax, uint32_t codepoint) {
  char bytes[4];
  size_t length = encode(codepoint, bytes);
  for (size_t i = 0; i < sizeof(CATEGORIES) / sizeof(*CATEGORIES); i++) {
    char pattern[16];
    snprintf(pattern, sizeof pattern, "\\p{%s}", CATEGORIES[i]);
    GRX_Regex * regex = NULL;
    if (grx_regex_compile(pattern, syntax, GRX_OPT_UTF | GRX_OPT_UCP, &regex)
        != GRX_OK) {
      grx_regex_free(regex);
      continue;
    }
    int matched = 0;
    if (grx_regex_match(regex, bytes, length, 0, GRX_ENGINE_AUTO, NULL, NULL,
            &matched) == GRX_OK && matched) {
      printf("gc %X %s\n", codepoint, CATEGORIES[i]);
      grx_regex_free(regex);
      return;
    }
    grx_regex_free(regex);
  }
  printf("gc %X ?\n", codepoint);
}

int main(int argc, char ** argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: grx_classes <dialect> [class ...]\n");
    return 2;
  }
  GRX_Syntax syntax;
  if (grx_syntax_from_name(argv[1], &syntax) != GRX_OK) {
    fprintf(stderr, "grx_classes: unknown dialect: %s\n", argv[1]);
    return 2;
  }

  if (argc > 2 && strcmp(argv[2], "gc") == 0) {
    for (int i = 3; i < argc; i++) {
      print_category(syntax, (uint32_t)strtoul(argv[i], NULL, 16));
    }
    return 0;
  }
  if (argc > 2) {
    for (int i = 2; i < argc; i++) {
      sweep(syntax, argv[i]);
    }
    return 0;
  }
  for (size_t i = 0; i < CLASS_COUNT; i++) {
    sweep(syntax, CLASSES[i]);
  }
  return 0;
}
