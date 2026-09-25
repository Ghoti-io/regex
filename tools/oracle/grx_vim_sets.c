/**
 * @file
 *
 * This library's four option-backed Vim sets, dumped for comparison to vim.
 *
 * `\i` is 'isident', `\k` is 'iskeyword', `\f` is 'isfname' and `\p` is
 * 'isprint'. All four are vim's data rather than the UCD's - vim decides
 * them from options whose defaults are vim's own - so, like the tables
 * tools/oracle/grx_vim_classes.c and grx_widths.c dump, nothing in
 * `make check-unicode-tables` can regenerate them.
 *
 * Run-length encoded, one run per line: `<set> <lo> <hi> <in>` with the
 * bounds in hexadecimal and `<in>` 1 or 0. The surrogate block is written
 * as -1 to match tools/unicode/vim_sets.vim, which cannot ask vim about it:
 * `nr2char()` cannot make a surrogate and a UTF-8 subject cannot hold one.
 *
 * Used by tools/check_vim_sets.py. The sets are asked of the *built library*
 * through the public API rather than read out of src/syntax/vim.c, so that
 * what is compared is what a caller would get.
 *
 * Why this exists: three of these four had no gate of any kind. The tables
 * were built by a one-off sweep, and src/syntax/vim.c's own header records
 * what that cost - three of the four were wrong when first written, and the
 * correction to `\k` then went two code points too far. A sweep that is not
 * a tool cannot be re-run when the reference moves, and on 2026-09-25 the
 * reference moved: vim 9.2 reclassifies 48 code points, 11 of them into
 * 'iskeyword'.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/regex.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/** The four patterns, each one atom, in the order they are dumped. */
static const struct {
  const char * name;
  const char * pattern;
} SETS[] = {
    {"ident", "\\i"},
    {"keyword", "\\k"},
    {"fname", "\\f"},
    {"print", "\\p"},
};

/** UTF-8 encode one code point. Returns the length, 0 for a surrogate. */
static size_t encode(uint32_t codepoint, char * out) {
  if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
    return 0;
  }
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

int main(void) {
  for (size_t index = 0; index < sizeof(SETS) / sizeof(SETS[0]); index++) {
    GRX_Regex * regex = NULL;
    if (grx_regex_compile(SETS[index].pattern, GRX_SYNTAX_VIM, GRX_OPT_NONE,
            &regex) != GRX_OK) {
      fprintf(stderr, "could not compile %s in the vim dialect\n",
          SETS[index].pattern);
      return 2;
    }
    long current = -2;
    uint32_t start = 0;
    for (uint32_t codepoint = 0; codepoint <= 0x110000; codepoint++) {
      long in;
      if (codepoint > 0x10FFFF) {
        in = -3;                /* forces the final run to be printed */
      }
      else if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
        in = -1;
      }
      else if (codepoint == 0) {
        /* nr2char(0, 1) is a zero-length string - vim cannot hold NUL in a
         * string - so there is nothing to ask it about. Written as -1 on
         * both sides, exactly as tools/oracle/grx_vim_classes.c does, so
         * that a disagreement about *which* code points are unaskable is
         * itself a disagreement rather than being absorbed. */
        in = -1;
      }
      else {
        char bytes[4];
        size_t length = encode(codepoint, bytes);
        int matched = 0;
        if (grx_regex_match(regex, bytes, length, 0, GRX_ENGINE_AUTO, NULL,
                NULL, &matched) != GRX_OK) {
          fprintf(stderr, "%s failed at U+%04X\n", SETS[index].name,
              codepoint);
          grx_regex_free(regex);
          return 2;
        }
        in = matched ? 1 : 0;
      }
      if (in != current) {
        if (codepoint > 0) {
          printf("%s %X %X %ld\n", SETS[index].name, start, codepoint - 1,
              current);
        }
        current = in;
        start = codepoint;
      }
    }
    grx_regex_free(regex);
  }
  return 0;
}
