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
 * Is this sequence of code points a script run?
 *
 * The rule is UTS #39 section 5.1's, which PCRE2 and Perl both follow, and
 * it exists to answer a security question rather than a typographical one:
 * "paypal.com" written with a Cyrillic `а` looks identical and is not the
 * same string. A script run is a sequence that *could* have been written in
 * one script.
 *
 * "One script" is not "one Script property value", which is why this is a
 * file and not a class. Punctuation and the ASCII digits are Common and go
 * with anything; a diacritic is Inherited and takes the script of what it
 * modifies; and Han is written alongside Hiragana and Katakana in Japanese,
 * alongside Hangul in Korean, and alongside Bopomofo in Taiwanese Mandarin -
 * but a string mixing Hangul and Bopomofo and Han is none of those three.
 *
 * The last of those is the reason the table carries *augmented* sets. Adding
 * Han to Hiragana's set and Hiragana to Han's would make a Hangul, Bopomofo
 * and Han string a run, because each pair would intersect. UTS #39 instead
 * invents three scripts - Japanese, Korean, HanBopomofo - that the
 * participating characters all name, so the three-way case falls out of an
 * ordinary intersection with no rule of its own. tools/unicode/gen_tables.py
 * applies that per character, so what is left here is one intersection and
 * one arithmetic check on digits.
 *
 * The check is offered incrementally as well as over an array, because the
 * rule is **not** decomposable: {A,B}, {B,C} and {C,A} intersect pairwise
 * and not at all together, so a caller with a long span cannot check it in
 * windows. A span is a single pass with three words of state.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>

#include "unicode_internal.h"
#include "tables/tables_internal.h"

/** The set index for a code point, by binary search over the ranges. */
static size_t script_set_of(uint32_t code) {
  size_t low = 0;
  size_t high = grx_unicode_script_range_count;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    const GRX_UnicodeBreakRange * row = &grx_unicode_script_ranges[mid];
    if (code < row->low) {
      high = mid;
    }
    else if (code > row->high) {
      low = mid + 1;
    }
    else {
      return (size_t)row->value;
    }
  }
  // The table covers U+0000 to U+10FFFF with no hole - the generator refuses
  // to write one that does not - so this is reachable only for a code point
  // outside Unicode, which cannot occur in a decoded subject. Unknown is the
  // conservative answer: it ends any run of two or more.
  return grx_unicode_script_set_unknown;
}

/**
 * The block of ten this decimal digit belongs to, or GRX_NPOS.
 *
 * `grx_unicode_digit_zeros` holds each block's first code point, sorted, and
 * every Nd code point is inside exactly one of them - which the generator
 * asserts rather than assumes, because a UCD that stopped being arranged
 * that way would otherwise produce a table that mis-groups digits silently.
 */
static uint32_t digit_block_of(uint32_t code) {
  size_t low = 0;
  size_t high = grx_unicode_digit_zero_count;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    uint32_t zero = grx_unicode_digit_zeros[mid];
    if (code < zero) {
      high = mid;
    }
    else if (code > zero + 9) {
      low = mid + 1;
    }
    else {
      return zero;
    }
  }
  return (uint32_t)GRX_NPOS;
}

void grx_unicode_script_run_begin(GRX_ScriptRunState * state) {
  if (!state) {
    return;
  }
  for (size_t word = 0; word < GRX_UNICODE_SCRIPT_WORDS; word++) {
    state->intersection[word] = ~(uint64_t)0;
  }
  state->digits = (uint32_t)GRX_NPOS;
  state->unknown = 0;
  state->count = 0;
}

int grx_unicode_script_run_add(GRX_ScriptRunState * state, uint32_t code) {
  if (!state) {
    return 0;
  }

  // The digit rule is independent of the script rule and applies even to a
  // Common character, which every ASCII digit is: `0` and the Arabic-Indic
  // `٠` both pass the script half and are two different sets of ten.
  uint32_t block = digit_block_of(code);
  if (block != (uint32_t)GRX_NPOS) {
    if (state->digits == (uint32_t)GRX_NPOS) {
      state->digits = block;
    }
    else if (state->digits != block) {
      return 0;
    }
  }

  size_t set = script_set_of(code);
  state->count++;

  // "A string that is less than two characters long is a script run. This
  // is the only case in which an Unknown character can be part of one."
  // pcre2unicode states it as a rule and it is not a consequence: a single
  // unassigned code point has the Unknown script, which intersects
  // nothing, so `(*sr:\x{E0000})` would fail without this.
  //
  // Remembered rather than answered on the spot, because the character that
  // makes the string long enough to matter may arrive later - and it may be
  // Common, which takes the early return below without looking at anything
  // that came before it.
  if (set == grx_unicode_script_set_unknown) {
    state->unknown = 1;
    return state->count < 2;
  }
  if (state->unknown) {
    return 0;
  }

  // Exactly Inherited is always accepted; exactly Common is accepted
  // subject to the digit rule, which was applied above. Neither narrows the
  // intersection, and *this is checked before the length rule rather than
  // after it*: a run beginning with a full stop constrains no script, and
  // letting the first character write its set into the intersection anyway
  // made `.a` fail where both references match.
  if (set == grx_unicode_script_set_common
      || set == grx_unicode_script_set_inherited) {
    return 1;
  }

  uint64_t remaining = 0;
  for (size_t word = 0; word < GRX_UNICODE_SCRIPT_WORDS; word++) {
    state->intersection[word] &= grx_unicode_script_sets[set][word];
    remaining |= state->intersection[word];
  }
  // Under two characters the answer is yes whatever the intersection says,
  // but the intersection is still written: it is what the *next* character
  // will be checked against.
  return state->count < 2 || remaining != 0;
}

int grx_unicode_script_run(const uint32_t * points, size_t count) {
  if (count < 2) {
    return 1;
  }
  if (!points) {
    return 0;
  }

  GRX_ScriptRunState state;
  grx_unicode_script_run_begin(&state);
  for (size_t i = 0; i < count; i++) {
    if (!grx_unicode_script_run_add(&state, points[i])) {
      return 0;
    }
  }
  return 1;
}
