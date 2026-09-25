/**
 * @file
 *
 * This library's vim character classes, dumped for comparison against vim.
 *
 * Run-length encoded, one run per line: `<lo> <hi> <class>` with the bounds
 * in hexadecimal. U+0000 and the surrogate block are written as -1 to match
 * tools/unicode/vim_classes.vim, which cannot ask vim about either.
 *
 * Used by tools/check_vim_classes.py. Like tools/oracle/grx_widths.c, this
 * exists because the table it dumps is vim's data rather than the UCD's, so
 * `make check-unicode-tables` cannot regenerate it and nothing else could.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/regex.h>
#include <stdio.h>

#include "../../src/unicode/unicode_internal.h"

int main(void) {
  long current = -2;
  uint32_t start = 0;
  for (uint32_t codepoint = 0; codepoint <= 0x110000; codepoint++) {
    long cls;
    if (codepoint > 0x10FFFF) {
      cls = -3;                 /* forces the final run to be printed */
    }
    else if (codepoint == 0 || (codepoint >= 0xD800 && codepoint <= 0xDFFF)) {
      cls = -1;
    }
    else {
      cls = (long)grx_vim_char_class(codepoint);
    }
    if (cls != current) {
      if (codepoint > 0) {
        printf("%X %X %ld\n", start, codepoint - 1, current);
      }
      current = cls;
      start = codepoint;
    }
  }
  return 0;
}
