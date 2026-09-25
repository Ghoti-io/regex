/**
 * @file
 *
 * This library's terminal cell widths, dumped for comparison against vim.
 *
 * Run-length encoded, one run per line: `<lo> <hi> <cells>` in hexadecimal
 * except the width. The surrogate block is written through as -1, matching
 * tools/unicode/vim_widths.vim, because neither side can be asked about it.
 *
 * `first` is 0 for every code point here, which is the question vim can be
 * asked cleanly. `strdisplaywidth()` of a *lone* combining character is
 * vim's escape rendering - `<180b>` is six columns - so the dumper on the
 * other side measures the contribution after a base instead, and that is
 * what `first == 0` means. For everything that is not a combining character
 * the two are identical.
 *
 * The `first == 1` reading is therefore not compared here. What a leading
 * combining character does to `\\%v` is checked by
 * tools/oracle/vim_diff.py's COMPOSING_SUBJECTS block against vim itself,
 * which is the construct-level question rather than the table's.
 *
 * Used by tools/check_vim_widths.py, which is the only thing that can
 * reproduce src/unicode/display.c's table: it is vim's data rather than the
 * UCD's, so `make check-unicode-tables` cannot regenerate it and the file
 * header's provenance sentence had nothing behind it until this existed.
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
    long width;
    if (codepoint > 0x10FFFF) {
      width = -2;               /* forces the final run to be printed */
    }
    else if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
      width = -1;
    }
    else {
      width = (long)grx_display_cell_width(codepoint, 0);
    }
    if (width != current) {
      if (codepoint > 0) {
        printf("%X %X %ld\n", start, codepoint - 1, current);
      }
      current = width;
      start = codepoint;
    }
  }
  return 0;
}
