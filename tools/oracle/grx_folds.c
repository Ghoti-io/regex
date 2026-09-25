/**
 * @file
 *
 * This library's simple case-fold orbits, dumped for comparison.
 *
 * One line per code point that shares a fold orbit with any other:
 *
 *   <code point> <member> <member> ...
 *
 * all in hexadecimal, the members in ascending order. A code point whose
 * orbit is itself alone is not printed, because a partition is defined by
 * its non-singleton classes and printing 1.1 million singletons would be
 * slower than the comparison.
 *
 * The *simple* orbit. Code points whose only fold is multi-code-point -
 * U+00DF to "ss", U+0149 to U+02BC U+006E - are singletons here and are
 * lowered as fold runs instead (GRX_IR_FOLD_RUN); tools/oracle/fold_diff.py
 * excludes them by asking the reference which keys are multi-code-point,
 * rather than by listing them.
 *
 * Used by tools/oracle/fold_diff.py. What that gate adds over
 * check-oracle-properties is *structure*: the property comparison already
 * checks which code points fold at all (Changes_When_Casefolded), and
 * nothing checked which ones fold *together*.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/regex.h>
#include <stdio.h>

#include "../../src/unicode/unicode_internal.h"

int main(void) {
  for (uint32_t codepoint = 0; codepoint <= 0x10FFFF; codepoint++) {
    if (codepoint >= 0xD800 && codepoint <= 0xDFFF) {
      continue;
    }
    uint32_t members[GRX_FOLD_ORBIT_MAX];
    size_t count = grx_unicode_fold_orbit(codepoint, members);
    if (count <= 1) {
      continue;
    }
    printf("%X", codepoint);
    for (size_t i = 0; i < count; i++) {
      printf(" %X", members[i]);
    }
    printf("\n");
  }
  return 0;
}
