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
 * Two mechanisms, because Unicode names come in two kinds. Most are
 * arbitrary strings and are in the generated table, sorted by name and
 * binary searched. The rest are *rules* - every CJK ideograph is
 * `CJK UNIFIED IDEOGRAPH-` and its code point in hex, every Hangul syllable
 * is `HANGUL SYLLABLE ` and its jamo spelling - and are computed here
 * instead, because storing them would add about 111,000 rows to a table of
 * 40,951.
 *
 * Matching is exact and case sensitive, which is Perl's rule and not an
 * approximation of it: `\N{latin small letter a}` is an error there, as are
 * `\N{LATIN-SMALL-LETTER-A}` and a name with two spaces where it should have
 * one. Each of those was tried against perl 5.40.1 before this file decided
 * to do nothing about them.
 */

#include <string.h>

#include "tables/tables_internal.h"
#include "unicode_internal.h"

/** The longest name in the table is 88 bytes; this is room and a margin. */
#define NAME_BUFFER 128

/**
 * The jamo spellings, in code-point order, from UAX #44 section 4.8.
 *
 * The empty strings are not padding. Index 11 of the leading consonants is
 * ieung, which is written but not pronounced and contributes no letters to
 * the name, and index 0 of the trailing consonants is "no trailing consonant
 * at all" - so `HANGUL SYLLABLE A` is a complete name with two of its three
 * parts empty.
 */
static const char * const kLeading[] = {
  "G", "GG", "N", "D", "DD", "R", "M", "B", "BB", "S", "SS", "",
  "J", "JJ", "C", "K", "T", "P", "H",
};
static const char * const kVowel[] = {
  "A", "AE", "YA", "YAE", "EO", "E", "YEO", "YE", "O", "WA", "WAE", "OE",
  "YO", "U", "WEO", "WE", "WI", "YU", "EU", "YI", "I",
};
static const char * const kTrailing[] = {
  "", "G", "GG", "GS", "N", "NJ", "NH", "D", "L", "LG", "LM", "LB", "LS",
  "LT", "LP", "LH", "M", "B", "BS", "S", "SS", "NG", "J", "C", "K", "T",
  "P", "H",
};

#define LEADING_COUNT ((size_t)(sizeof(kLeading) / sizeof(kLeading[0])))
#define VOWEL_COUNT ((size_t)(sizeof(kVowel) / sizeof(kVowel[0])))
#define TRAILING_COUNT ((size_t)(sizeof(kTrailing) / sizeof(kTrailing[0])))

/**
 * The longest entry of `table` that this text begins with.
 *
 * Longest rather than first, because the spellings are prefixes of one
 * another - `G` and `GG`, `S` and `SS`, `W` is not one but `WA` and `WAE`
 * are - and taking the first match would read "GGA" as G followed by a vowel
 * that does not exist.
 */
static int longest_jamo(const char * text, size_t length,
    const char * const * table, size_t count, size_t * out_index,
    size_t * out_length) {
  int found = 0;
  for (size_t i = 0; i < count; i++) {
    size_t piece = strlen(table[i]);
    if (piece > length || memcmp(text, table[i], piece) != 0) {
      continue;
    }
    if (!found || piece > *out_length) {
      found = 1;
      *out_index = i;
      *out_length = piece;
    }
  }
  return found;
}

/** `HANGUL SYLLABLE GAG` and kin, by UAX #44's composition. */
static int hangul_from_name(const char * text, size_t length,
    uint32_t first, uint32_t * out_codepoint) {
  size_t lead = 0, lead_length = 0;
  if (!longest_jamo(text, length, kLeading, LEADING_COUNT, &lead,
          &lead_length)) {
    return 0;
  }
  text += lead_length;
  length -= lead_length;

  size_t vowel = 0, vowel_length = 0;
  if (!longest_jamo(text, length, kVowel, VOWEL_COUNT, &vowel,
          &vowel_length)) {
    return 0;
  }
  text += vowel_length;
  length -= vowel_length;

  size_t trail = 0, trail_length = 0;
  if (!longest_jamo(text, length, kTrailing, TRAILING_COUNT, &trail,
          &trail_length)) {
    return 0;
  }
  // Anything left over is not part of a syllable, so the name is not one.
  if (length != trail_length) {
    return 0;
  }

  *out_codepoint = first
      + (uint32_t)((lead * VOWEL_COUNT + vowel) * TRAILING_COUNT + trail);
  return 1;
}

/** `CJK UNIFIED IDEOGRAPH-4E00` and kin: the code point, written in hex. */
static int hex_from_name(const char * text, size_t length,
    uint32_t * out_codepoint) {
  if (!length || length > 6) {
    return 0;
  }
  uint32_t value = 0;
  for (size_t i = 0; i < length; i++) {
    char c = text[i];
    uint32_t digit;
    if (c >= '0' && c <= '9') {
      digit = (uint32_t)(c - '0');
    }
    // Upper case only: the names are upper case throughout, and accepting
    // `4e00` here would accept a spelling Perl rejects.
    else if (c >= 'A' && c <= 'F') {
      digit = (uint32_t)(c - 'A') + 10;
    }
    else {
      return 0;
    }
    value = (value << 4) | digit;
  }
  *out_codepoint = value;
  return 1;
}

/** The computed families, tried before the table. */
static int algorithmic_name(const char * name, size_t length,
    uint32_t * out_codepoint) {
  for (size_t i = 0; i < grx_unicode_name_range_count; i++) {
    const GRX_UnicodeNameRange * range = &grx_unicode_name_ranges[i];
    size_t prefix = strlen(range->prefix);
    if (length <= prefix || memcmp(name, range->prefix, prefix) != 0) {
      continue;
    }
    uint32_t codepoint = 0;
    int ok = range->hangul
        ? hangul_from_name(name + prefix, length - prefix, range->first,
              &codepoint)
        : hex_from_name(name + prefix, length - prefix, &codepoint);
    // A name of the right shape for a range it does not fall in is not a
    // name: `CJK UNIFIED IDEOGRAPH-0041` parses as hex and is not an
    // ideograph, and perl refuses it.
    if (ok && codepoint >= range->first && codepoint <= range->last) {
      *out_codepoint = codepoint;
      return 1;
    }
  }
  return 0;
}

/** One table row, written out. Returns its length. */
static size_t decode_name(size_t index, char * buffer) {
  uint32_t begin = grx_unicode_name_offset[index];
  uint32_t end = grx_unicode_name_offset[index + 1];
  size_t written = 0;
  for (uint32_t i = begin; i < end && written < NAME_BUFFER; i++) {
    uint16_t token = grx_unicode_name_tokens[i];
    if (i != begin) {
      buffer[written++] = (token & 0x8000u) ? '-' : ' ';
    }
    const char * word
        = grx_unicode_name_words
        + grx_unicode_name_word_offset[token & 0x7FFFu];
    while (*word && written < NAME_BUFFER) {
      buffer[written++] = *word++;
    }
  }
  return written;
}

int grx_unicode_codepoint_from_name(
    const char * name, size_t length, uint32_t * out_codepoint) {
  if (!name || !out_codepoint || !length || length >= NAME_BUFFER) {
    return 0;
  }

  if (algorithmic_name(name, length, out_codepoint)) {
    return 1;
  }

  size_t low = 0;
  size_t high = grx_unicode_name_count;
  char buffer[NAME_BUFFER];
  while (low < high) {
    size_t middle = low + (high - low) / 2;
    size_t written = decode_name(middle, buffer);
    size_t shortest = written < length ? written : length;
    int order = memcmp(buffer, name, shortest);
    if (order == 0) {
      // A prefix sorts before what it is a prefix of, which is the order the
      // generator sorted by.
      order = written < length ? -1 : (written > length ? 1 : 0);
    }
    if (order == 0) {
      *out_codepoint = grx_unicode_name_codepoint[middle];
      return 1;
    }
    if (order < 0) {
      low = middle + 1;
    }
    else {
      high = middle;
    }
  }
  return 0;
}
