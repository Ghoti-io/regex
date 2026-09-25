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
 * UAX #29 and UAX #14 boundaries, asked of ghoti.io-unicode.
 *
 * This file was 1,265 lines of rule engine over 2,922 lines of generated
 * table until 2026-09-25 - the twelve grapheme rules, the twenty-odd word
 * rules, the sentence rules and the thirty line-break ones, each written
 * once here and once in `unicode`, from the same UCD, with nothing checking
 * that the two agreed. Now there is one of them. See break_internal.h for
 * what the caller is promised; this is the adapter that keeps that promise.
 *
 * **The tailoring is STRICT and that is not a choice this file makes.**
 * UAX #14 rule LB1 resolves the classes a character alone does not settle,
 * and `CJ` goes to `NS` under CSS's `strict` and `normal` and to `ID` under
 * `loose`. STRICT is what this library answered before the move - it is the
 * Standard's own worked example - so naming it here changes nothing and
 * says which of the four a reader is looking at. A dialect that wanted
 * another would pass it; none does, and `\b{lb}` has no syntax for one.
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/unicode/break.h>
#include <stddef.h>

#include "break_internal.h"

int grx_unicode_break_at(GRX_BreakKind kind, const char * subject,
    size_t length, size_t position) {
  // The empty subject has no boundary of any kind. Neither standard says so
  // in those words - both would break at offset 0 - and it is what perl
  // answers, which is why break_internal.h states it as this library's
  // contract. The unicode library documents the same answer for the same
  // reason, so this is belt and braces rather than a correction; it is here
  // because a contract that depends on a dependency's wording is one that
  // can change under you without a compile error.
  if (!length) {
    return 0;
  }

  GUNI_BreakOptions options = {
    .kind = GUNI_BREAK_GRAPHEME,
    .tailoring = GUNI_LINE_BREAK_STRICT,
    .provider = NULL,
    .writing_system = GUNI_WRITING_SYSTEM_NEUTRAL,
  };
  switch (kind) {
    case GRX_BREAK_GRAPHEME: options.kind = GUNI_BREAK_GRAPHEME; break;
    case GRX_BREAK_WORD:     options.kind = GUNI_BREAK_WORD;     break;
    case GRX_BREAK_SENTENCE: options.kind = GUNI_BREAK_SENTENCE; break;
    case GRX_BREAK_LINE:     options.kind = GUNI_BREAK_LINE;     break;
    case GRX_BREAK_COUNT:
    default:
      // Not a segmentation. Refusing is the answer that cannot invent a
      // boundary, which is what every other unreachable default here does.
      return 0;
  }

  return guni_break_at(&options, subject, length, position) ? 1 : 0;
}
