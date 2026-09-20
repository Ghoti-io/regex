/**
 * @file
 *
 * Which front end reads which dialect.
 *
 * One function, and it is here rather than in any one dialect's file for the
 * reason that matters as the second front end lands: the table of what is
 * built is not ECMAScript's business. It lived in src/syntax/ecmascript.c
 * while ECMAScript was the only entry, and that was fine exactly as long as
 * adding a dialect meant editing the file of the dialect it is not.
 *
 * A dialect with no entry here is *named but not built*, and
 * grx_parse_pattern() reports GRX_DIAG_DIALECT_NOT_IMPLEMENTED for it. That
 * is the whole point of the NULL: a library that read PCRE as ECMAScript
 * would tell a caller their pattern is valid for an engine that rejects it
 * (documentation/design.md section 4), and the difference between the two
 * dialects is not a rounding error - `(?i)` is a flag in one and a syntax
 * error in the other.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/syntax.h>

#include "../parse/parse_internal.h"

const GRX_Frontend * grx_frontend_for(GRX_Syntax syntax) {
  switch (syntax) {
    case GRX_SYNTAX_ECMASCRIPT:
      return &grx_frontend_ecmascript;
    case GRX_SYNTAX_PCRE:
      return &grx_frontend_pcre;
    case GRX_SYNTAX_PERL:
      return &grx_frontend_perl;
    default:
      // POSIX, GNU and everything in tiers 2 and beyond. Their packages are
      // documentation/plan.md WP-23 and later.
      return NULL;
  }
}
