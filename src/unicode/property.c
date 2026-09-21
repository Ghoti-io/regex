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
 * Resolving `\p{...}` to a set of code points.
 *
 * Two spelling rules over one set of tables. ECMAScript requires the exact
 * canonical name or a listed alias and rejects everything else as a
 * SyntaxError; Perl and PCRE2 ignore case, underscores, hyphens and spaces.
 * Both are binary searches, the second over a table the generator emitted
 * with the loose spelling already applied, so that this file never has to
 * normalise a table entry at run time - only the caller's text.
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tables/tables_internal.h"
#include "unicode_internal.h"

/**
 * The longest property or value spelling the tables hold.
 *
 * Loose matching has to normalise the caller's text before comparing, and
 * doing that in a fixed buffer means a name longer than any table entry is
 * rejected without allocating - which is correct, because a name longer than
 * every entry cannot match one.
 */
#define GRX_PROPERTY_NAME_MAX 96

/**
 * Compare a table entry against a caller's name, which is not terminated.
 *
 * `strncmp` is the obvious implementation and is wrong, because a pattern is
 * *bytes*: `\p{L\0\0\0}` gives a name whose first byte is `L` and whose
 * remaining thirty-seven are NUL. `strncmp` stops at the NUL, reports the
 * entry "L" as equal, and the length comparison that followed then read
 * `entry[37]` - thirty-six bytes past the end of a two-byte string. Found by
 * the pattern fuzzer as a global-buffer-overflow; the name table is in
 * `.rodata`, so what it read was whichever property name happened to be
 * stored after it.
 *
 * So: compare the bytes both have, then the lengths. That is strcmp's
 * ordering, which is what the table is sorted by, extended to a name that
 * may contain a NUL.
 */
static int name_compare(
    const char * entry, const char * name, size_t name_length) {
  size_t entry_length = strlen(entry);
  size_t shared = entry_length < name_length ? entry_length : name_length;
  int order = shared ? memcmp(entry, name, shared) : 0;
  if (order != 0) {
    return order;
  }
  if (entry_length == name_length) {
    return 0;
  }
  return entry_length < name_length ? -1 : 1;
}

/**
 * UAX #44 section 5.9.2: fold case and drop `_`, `-` and space.
 *
 * @return The normalised length, or 0 when the name does not fit, which no
 *   table entry does either.
 */
static size_t loosen(const char * name, size_t name_length, char * buffer) {
  size_t written = 0;
  for (size_t i = 0; i < name_length; i++) {
    char c = name[i];
    if (c == '_' || c == '-' || c == ' ') {
      continue;
    }
    if (written + 1 >= GRX_PROPERTY_NAME_MAX) {
      return 0;
    }
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c - 'A' + 'a');
    }
    buffer[written++] = c;
  }
  buffer[written] = '\0';
  return written;
}

/**
 * Find an entry with this spelling and this kind.
 *
 * The table is sorted by name and then by kind, because one spelling names
 * two different sets: `Greek` is a Script value and a Script_Extensions
 * value, and they are not the same code points.
 */
static int find_name(const GRX_UnicodeName * table, size_t count,
    const char * name, size_t name_length, int kind, uint32_t * out_property) {
  size_t low = 0;
  size_t high = count;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    int order = name_compare(table[mid].name, name, name_length);
    if (order == 0) {
      order = (int)table[mid].kind - kind;
    }
    if (order > 0) {
      high = mid;
    }
    else if (order < 0) {
      low = mid + 1;
    }
    else {
      *out_property = table[mid].property;
      return 1;
    }
  }

  return 0;
}

/**
 * The general-category spellings the loose table cannot hold.
 *
 * `\p{L&}` is Cased_Letter in both references, and `\p{L_}` is Cased_Letter
 * in Perl. Neither can be a row: `L&` loosens to "l&", which is a key
 * nothing else would ever produce, and `L_` loosens to "l", which is already
 * `L`'s - and `L` is what PCRE2 means by it. So the two are resolved from
 * the caller's text, before loosening gets to it.
 *
 * @return The loose spelling to look up instead, or NULL for anything else.
 */
static const char * cased_letter_alias(
    const char * name, size_t name_length, GRX_PropertyMatch match) {
  if (name_length != 2 || (name[0] != 'L' && name[0] != 'l')) {
    return NULL;
  }
  if (name[1] == '&') {
    return "lc";
  }

  return name[1] == '_' && match == GRX_PROPERTY_LOOSE_PERL ? "lc" : NULL;
}

/** Find the kind a property *name* selects: `gc`, `sc`, `scx` and their aliases. */
static int find_prop_kind(const GRX_UnicodeName * table, size_t count,
    const char * name, size_t name_length, int * out_kind) {
  for (size_t i = 0; i < count; i++) {
    if (name_compare(table[i].name, name, name_length) == 0) {
      *out_kind = (int)table[i].kind;
      return 1;
    }
  }

  return 0;
}

// --------------------------------------------------------------------------
// Numeric_Value
// --------------------------------------------------------------------------

/**
 * Multiply two non-negative values, refusing to wrap.
 *
 * Everything here stays non-negative until the sign is applied at the very
 * end, which is what makes one unsigned-shaped check enough.
 */
static int mul_checked(int64_t a, int64_t b, int64_t * out) {
  if (b != 0 && a > INT64_MAX / b) {
    return 0;
  }
  *out = a * b;
  return 1;
}

/** Append one decimal digit, refusing to wrap. */
static int push_digit(int64_t * value, int digit) {
  if (!mul_checked(*value, 10, value) || *value > INT64_MAX - digit) {
    return 0;
  }
  *value += digit;
  return 1;
}

/** Euclid, over a non-negative `a` and a positive `b`. */
static int64_t greatest_common_divisor(int64_t a, int64_t b) {
  while (b != 0) {
    int64_t remainder = a % b;
    a = b;
    b = remainder;
  }
  return a;
}

/**
 * The text of a `\p{nv=...}` value as a reduced rational.
 *
 * UAX #44 section 5.9.2 gives numeric property values a matching rule of
 * their own. Case, whitespace and `_` are ignored as they are everywhere
 * else, but the comparison is by "numeric equivalencies" rather than by
 * spelling: PropertyAliases.txt states it as "01.00" being equivalent to
 * "1", and it is why `2/4`, `0.5` and `+1/2` are one value here. The hyphen
 * that loose matching drops from a *name* is a sign in a number, so it
 * survives - `nv=-1/2` is U+0F33 alone and `nv=1/2` is twenty other code
 * points. That is the whole reason these values are not in the spelling
 * tables.
 *
 * The grammar, after whitespace and `_` are dropped: an optional sign, then
 * either `digits/digits` or `digits` with an optional `.digits` and an
 * optional `e` exponent. A fraction's two parts are integers - `1.5/2` is
 * not a value in Perl either.
 *
 * @return 1 on success, or 0 for text that is not a number and for one too
 *   large to hold. Both are reported the same way because they have the
 *   same consequence: no property answers to it.
 */
static int parse_rational(const char * text, size_t length,
    int64_t * out_numerator, int64_t * out_denominator) {
  char digits[GRX_PROPERTY_NAME_MAX];
  size_t count = 0;

  for (size_t i = 0; i < length; i++) {
    char c = text[i];
    if (c == '_' || c == ' ' || c == '\t' || c == '\n' || c == '\r') {
      continue;
    }
    if (count + 1 >= GRX_PROPERTY_NAME_MAX) {
      return 0;
    }
    digits[count++] = c;
  }
  digits[count] = '\0';

  size_t at = 0;
  int negative = 0;
  if (at < count && (digits[at] == '+' || digits[at] == '-')) {
    negative = digits[at] == '-';
    at++;
  }

  int64_t numerator = 0;
  int64_t denominator = 1;
  size_t first = at;
  while (at < count && digits[at] >= '0' && digits[at] <= '9') {
    if (!push_digit(&numerator, digits[at] - '0')) {
      return 0;
    }
    at++;
  }
  if (at == first) {
    return 0;
  }

  if (at < count && digits[at] == '/') {
    at++;
    size_t start = at;
    denominator = 0;
    while (at < count && digits[at] >= '0' && digits[at] <= '9') {
      if (!push_digit(&denominator, digits[at] - '0')) {
        return 0;
      }
      at++;
    }
    if (at == start || at != count || denominator == 0) {
      return 0;
    }
  }
  else {
    // A decimal point and an exponent both just move the value along a power
    // of ten, so they are collected as one shift and applied once.
    int scale = 0;
    int exponent = 0;
    int exponent_negative = 0;
    if (at < count && digits[at] == '.') {
      at++;
      while (at < count && digits[at] >= '0' && digits[at] <= '9') {
        if (!push_digit(&numerator, digits[at] - '0')) {
          return 0;
        }
        scale++;
        at++;
      }
    }
    if (at < count && (digits[at] == 'e' || digits[at] == 'E')) {
      at++;
      if (at < count && (digits[at] == '+' || digits[at] == '-')) {
        exponent_negative = digits[at] == '-';
        at++;
      }
      size_t start = at;
      while (at < count && digits[at] >= '0' && digits[at] <= '9') {
        // Bounded rather than accumulated: an exponent past a few hundred
        // overflows whatever it is applied to, and the bound keeps the loop
        // below from running a caller's own digit count.
        if (exponent < 4096) {
          exponent = exponent * 10 + (digits[at] - '0');
        }
        at++;
      }
      if (at == start) {
        return 0;
      }
    }
    if (at != count) {
      return 0;
    }

    int shift = exponent_negative ? -exponent - scale : exponent - scale;
    for (int i = 0; i < shift; i++) {
      if (!mul_checked(numerator, 10, &numerator)) {
        return 0;
      }
    }
    for (int i = 0; i < -shift; i++) {
      if (!mul_checked(denominator, 10, &denominator)) {
        return 0;
      }
    }
  }

  // Reducing is what makes equality an integer comparison later. gcd(0, d)
  // is d, so every spelling of zero - `0`, `-0`, `0.00` - lands on 0/1.
  int64_t divisor = greatest_common_divisor(numerator, denominator);
  numerator /= divisor;
  denominator /= divisor;
  *out_numerator = negative ? -numerator : numerator;
  *out_denominator = denominator;
  return 1;
}

/** Find the property a numeric value names, by value and not by spelling. */
static int find_numeric(
    const char * value, size_t value_length, uint32_t * out_property) {
  int64_t numerator = 0;
  int64_t denominator = 0;
  if (!parse_rational(value, value_length, &numerator, &denominator)) {
    return 0;
  }

  // Sorted by numerator and then denominator, both reduced on each side, so
  // this compares integers and never multiplies.
  size_t low = 0;
  size_t high = grx_unicode_numeric_value_count;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    const GRX_UnicodeNumeric * entry = &grx_unicode_numeric_values[mid];
    int64_t theirs = entry->numerator;
    int64_t mine = numerator;
    if (theirs == mine) {
      theirs = entry->denominator;
      mine = denominator;
    }
    if (theirs == mine) {
      *out_property = entry->property;
      return 1;
    }
    if (theirs > mine) {
      high = mid;
    }
    else {
      low = mid + 1;
    }
  }

  return 0;
}

GRX_Result grx_unicode_property_lookup(const char * name, size_t name_length,
    const char * value, size_t value_length, GRX_PropertyMatch match,
    uint32_t * out_property) {
  if (!name || !name_length || !out_property || (!value && value_length)) {
    return GRX_ERR_INVALID;
  }

  char name_buffer[GRX_PROPERTY_NAME_MAX];
  char value_buffer[GRX_PROPERTY_NAME_MAX];
  const GRX_UnicodeName * names = grx_unicode_strict_names;
  size_t name_count = grx_unicode_strict_name_count;
  const GRX_UnicodeName * props = grx_unicode_prop_names;
  size_t prop_count = grx_unicode_prop_name_count;

  if (match != GRX_PROPERTY_STRICT) {
    const char * alias
        = value ? NULL : cased_letter_alias(name, name_length, match);
    if (alias) {
      name = alias;
      name_length = strlen(alias);
    }
    else {
      name_length = loosen(name, name_length, name_buffer);
      if (!name_length) {
        return GRX_ERR_SYNTAX;
      }
      name = name_buffer;
    }
    names = grx_unicode_loose_names;
    name_count = grx_unicode_loose_name_count;
    props = grx_unicode_loose_prop_names;
    prop_count = grx_unicode_loose_prop_name_count;
  }

  if (value) {
    int kind = 0;
    if (!find_prop_kind(props, prop_count, name, name_length, &kind)) {
      return GRX_ERR_SYNTAX;
    }

    // Numeric_Value before the value is loosened, because loosening drops
    // the `-` that tells `nv=-1/2` from `nv=1/2`. Perl's alone: pcre2test
    // 10.46 and V8 both refuse `\p{nv=1}` as an unknown property, and the
    // generator keeps `nv` out of the strict table so that a lookup can
    // only arrive here under a loose spelling rule.
    if (kind == GRX_UPROP_NV) {
      if (match != GRX_PROPERTY_LOOSE_PERL) {
        return GRX_ERR_SYNTAX;
      }
      return find_numeric(value, value_length, out_property) ? GRX_OK
                                                             : GRX_ERR_SYNTAX;
    }

    if (match != GRX_PROPERTY_STRICT) {
      value_length = loosen(value, value_length, value_buffer);
      if (!value_length) {
        return GRX_ERR_SYNTAX;
      }
      value = value_buffer;
    }
    if (!find_name(names, name_count, value, value_length, kind,
            out_property)) {
      return GRX_ERR_SYNTAX;
    }
    return GRX_OK;
  }

  // The lone form. A binary property name first, then a General_Category
  // value: those are the two things ECMA-262 22.2.2.9.7 allows without a
  // property name, and the loose dialects accept the same two plus the
  // synthetic classes their own hooks add.
  if (find_name(names, name_count, name, name_length, GRX_UPROP_BINARY,
          out_property)) {
    return GRX_OK;
  }
  if (find_name(names, name_count, name, name_length, GRX_UPROP_GC,
          out_property)) {
    return GRX_OK;
  }

  // A lone *script* name, which the loose dialects also accept: `\p{Latin}`
  // is `\p{Script=Latin}` in Perl and PCRE2 and a syntax error in
  // ECMAScript, which is exactly the difference the two resolvers exist to
  // keep apart. Tried last, so that a name which is both a binary property
  // and a script still resolves the way it does in the strict form.
  if (match != GRX_PROPERTY_STRICT
      && find_name(names, name_count, name, name_length, GRX_UPROP_SCRIPT,
          out_property)) {
    return GRX_OK;
  }

  return GRX_ERR_SYNTAX;
}

const GRX_CharRange * grx_unicode_property_ranges(
    uint32_t property, size_t * out_count) {
  if (!out_count) {
    return NULL;
  }
  *out_count = 0;
  if (property >= grx_unicode_property_count) {
    return NULL;
  }

  *out_count = grx_unicode_properties[property].count;
  return &grx_unicode_ranges[grx_unicode_properties[property].first];
}

const char * grx_unicode_property_name(uint32_t property) {
  if (property >= grx_unicode_property_count) {
    return NULL;
  }

  return grx_unicode_properties[property].name;
}

size_t grx_unicode_property_total(uint32_t property) {
  if (property >= grx_unicode_property_count) {
    return 0;
  }

  return grx_unicode_properties[property].total;
}

// --------------------------------------------------------------------------
// Properties of strings
// --------------------------------------------------------------------------

GRX_Result grx_unicode_string_set_lookup(
    const char * name, size_t name_length, uint32_t * out_set) {
  if (!name || !out_set) {
    return GRX_ERR_INVALID;
  }

  // Seven entries, compared linearly. A binary search would need the table
  // sorted by name, and the table is sorted by *layout* so that RGI_Emoji
  // can be the whole of it; seven string comparisons is the cheaper of the
  // two things to give up.
  for (size_t i = 0; i < grx_unicode_string_set_count; i++) {
    const char * candidate = grx_unicode_string_sets[i].name;
    if (strlen(candidate) == name_length
        && memcmp(candidate, name, name_length) == 0) {
      *out_set = (uint32_t)i;
      return GRX_OK;
    }
  }
  return GRX_ERR_SYNTAX;
}

size_t grx_unicode_string_set_size(uint32_t set) {
  if (set >= grx_unicode_string_set_count) {
    return 0;
  }
  return grx_unicode_string_sets[set].count;
}

size_t grx_unicode_string_set_at(
    uint32_t set, size_t index, const uint32_t ** out_points) {
  if (!out_points || set >= grx_unicode_string_set_count
      || index >= grx_unicode_string_sets[set].count) {
    return 0;
  }
  const GRX_UnicodeString * sequence
      = &grx_unicode_strings[grx_unicode_string_sets[set].first + index];
  *out_points = &grx_unicode_string_points[sequence->first];
  return sequence->length;
}
