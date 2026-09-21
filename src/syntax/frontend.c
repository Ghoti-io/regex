/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Regex.
 *
 * Ghoti.io Regex is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Regex is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

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
    case GRX_SYNTAX_POSIX_BRE:
      return &grx_frontend_posix_bre;
    case GRX_SYNTAX_POSIX_ERE:
      return &grx_frontend_posix_ere;
    case GRX_SYNTAX_GNU_BRE:
      return &grx_frontend_gnu_bre;
    case GRX_SYNTAX_GNU_ERE:
      return &grx_frontend_gnu_ere;
    default:
      // Everything in tiers 2 and beyond. Their packages are
      // documentation/plan.md WP-30 and later.
      return NULL;
  }
}
