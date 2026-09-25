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
 * Resolving a character name, for Perl's `\N{NAME}`.
 *
 * The names themselves are the suite's, in ghoti.io-unicode: 40,951 stored
 * names, every alias, and the rules that compute the rest - every CJK
 * ideograph is `CJK UNIFIED IDEOGRAPH-` and its code point in hex, every
 * Hangul syllable is `HANGUL SYLLABLE ` and its jamo spelling, which is
 * about 111,000 names nobody stores. This file was a copy of all of that,
 * 243 lines over a 31,603-line table, until 2026-09-25.
 *
 * **What is still here is one rule, and it is a dialect rule rather than a
 * Unicode one: the match is exact.** Perl's `\N{...}` is case sensitive and
 * wants the separators the UCD uses - `\N{latin small letter a}` is an
 * error there, so are `\N{LATIN-SMALL-LETTER-A}` and a name with two spaces
 * where it should have one - and each of those was tried against perl
 * before this library decided to refuse them. The Unicode library matches
 * **loosely**, per UAX #44-LM2, which is the right default for a library
 * whose callers are not all perl.
 *
 * So the loose lookup finds the candidate and the spelling is then confirmed
 * against it: a name resolves here only if it is byte-for-byte the code
 * point's own name or one of its aliases. Two steps rather than one, and
 * the second is what makes the answer Perl's.
 *
 * Checked rather than reasoned: every stored name, every alias and a
 * lower-cased and a hyphenated mangle of every ninety-seventh of them -
 * 163,564 lookups - answer exactly what the table here answered, including
 * every refusal.
 */

#include <ghoti.io/unicode/name.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "unicode_internal.h"

/** The longest name in the UCD is 88 bytes; this is room and a margin. */
#define NAME_BUFFER 128

int grx_unicode_codepoint_from_name(
    const char * name, size_t length, uint32_t * out_codepoint) {
  if (!name || !length || !out_codepoint) {
    return 0;
  }

  uint32_t codepoint = 0;
  if (guni_codepoint_by_name(name, length, &codepoint) != GUNI_OK) {
    return 0;
  }

  // The loose match found *a* code point; Perl's rule is that the spelling
  // has to be one this code point actually has. Its own name first, which
  // is the case for all but a few hundred lookups.
  char buffer[NAME_BUFFER];
  size_t written = 0;
  if (guni_name(codepoint, buffer, sizeof buffer, &written) == GUNI_OK
      && written == length && memcmp(buffer, name, length) == 0) {
    *out_codepoint = codepoint;
    return 1;
  }

  // Then every alias, all five kinds of them. Perl accepts corrections and
  // figments as readily as abbreviations, so none is filtered out here.
  size_t aliases = guni_name_alias_count(codepoint);
  for (size_t index = 0; index < aliases; index++) {
    GUNI_NameAliasKind kind;
    if (guni_name_alias(codepoint, index, &kind, buffer, sizeof buffer,
            &written) != GUNI_OK) {
      continue;
    }
    if (written == length && memcmp(buffer, name, length) == 0) {
      *out_codepoint = codepoint;
      return 1;
    }
  }

  // The loose match reached a code point whose exact spellings this is not.
  return 0;
}
