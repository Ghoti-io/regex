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
 * Resolving `\p{...}`: the spelling here, the code points in the suite.
 *
 * Two layers, and the split between them is the whole design of this file.
 *
 * **The spelling is this library's**, because the dialects disagree about
 * it. ECMAScript requires the exact canonical name or a listed alias and
 * rejects everything else as a SyntaxError; Perl and PCRE2 ignore case,
 * underscores, hyphens and spaces; `\p{L&}` is Cased_Letter in both and a
 * key no loose table could hold; `\p{nv=-1/2}` is a different set from
 * `\p{nv=1/2}` because a numeric value is compared by arithmetic and the
 * hyphen loose matching drops is a sign. None of that is a property of the
 * UCD - it is a property of the four grammars - so it stays, as two binary
 * searches over tables the generator emitted with each rule already applied.
 *
 * **The code points are ghoti.io-unicode's.** A resolved record says which
 * question to put to it and nothing more: a property value, a
 * General_Category mask, the complement of a value, or a numeric value over
 * the domain a property encloses. This library holds no ranges at all, so a
 * `\p{Greek}` and a `guni_set_contains()` in one program cannot answer
 * differently - not because a gate compares them, but because there is only
 * one table.
 *
 * What the records still carry is `total`, the generator's own count of what
 * the UCD files say. That is deliberate: it is the one figure here derived
 * without asking the library that answers, so the test summing a property's
 * ranges against it compares two readings of the UCD rather than comparing
 * the generator with itself, which is what it did while both came from the
 * same run.
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <ghoti.io/unicode/char.h>
#include <ghoti.io/unicode/set.h>
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

    // `\p{blk=...}` and `\p{Block=...}`, which only perl has. pcre2test
    // 10.46 answers error 147 to both, so PCRE2 must keep refusing them
    // even though it shares this resolver and the loose tables with perl.
    if (kind == GRX_UPROP_BLOCK && match != GRX_PROPERTY_LOOSE_PERL) {
      return GRX_ERR_SYNTAX;
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

  // Blocks, and everything below here is perl's alone - measured against
  // perl 5.44.0 and pcre2test 10.46, which answers error 147 to every one
  // of these spellings.
  //
  // **A bare block name resolves, and it resolves last.** `\p{BasicLatin}`
  // and `\p{GreekExtended}` both match in perl; `\p{Greek}` is *not* the
  // block, because the script claims the name first - probed with a
  // minimal pair, U+0374 being in the Greek and Coptic block with
  // Script=Common, and U+1F00 being Script=Greek in the Greek Extended
  // block. `\p{Greek}` takes U+0374 and U+1F00 both, which is neither the
  // block nor plain Script but Script_Extensions, and `\p{InGreek}` takes
  // U+0374 and not U+1F00, which is the block. So the order below is the
  // measurement: binary, category, script, script extensions, then block.
  if (match == GRX_PROPERTY_LOOSE_PERL
      && find_name(names, name_count, name, name_length, GRX_UPROP_BLOCK,
          out_property)) {
    return GRX_OK;
  }

  // The `In` and `Is` prefixes, which differ from each other and are not
  // the same question:
  //
  // - `In` is the block prefix and nothing else. `\p{InGreek}` is the
  //   block; there is no non-block reading to try first.
  // - `Is` is *not* a block prefix. It re-runs the ordinary chain -
  //   `\p{IsGreek}` matches U+1F00, so it is the script-extensions reading
  //   and not the block - and only falls through to a block when nothing
  //   else claims the name, which is how `\p{IsGreekAndCoptic}` works.
  //
  // Both are tried only after every unprefixed reading has failed, so a
  // property whose own name begins with "In" or "Is" is unaffected.
  if (match == GRX_PROPERTY_LOOSE_PERL && name_length > 2) {
    const char * rest = name + 2;
    size_t rest_length = name_length - 2;
    if (name[0] == 'i' && name[1] == 'n'
        && find_name(names, name_count, rest, rest_length, GRX_UPROP_BLOCK,
            out_property)) {
      return GRX_OK;
    }
    if (name[0] == 'i' && name[1] == 's') {
      static const int is_kinds[] = {GRX_UPROP_BINARY, GRX_UPROP_GC,
          GRX_UPROP_SCRIPT, GRX_UPROP_SCX, GRX_UPROP_BLOCK};
      for (size_t i = 0; i < sizeof is_kinds / sizeof *is_kinds; i++) {
        if (find_name(names, name_count, rest, rest_length, is_kinds[i],
                out_property)) {
          return GRX_OK;
        }
      }
    }
  }

  return GRX_ERR_SYNTAX;
}

// --------------------------------------------------------------------------
// The code points, which are ghoti.io-unicode's
// --------------------------------------------------------------------------

/**
 * The domain a Numeric_Value is looked for in.
 *
 * `guni_numeric_value()` answers for one code point, and `\p{nv=1/2}` needs
 * the set - the shape mismatch that kept this property's ranges here through
 * the first pass of the migration. It is closed without an API addition the
 * same way the domain of a simple case mapping was: **a property is the
 * enumeration and the function is the predicate.** Numeric_Type is None for
 * every code point that has no numeric value and for no code point that has
 * one, exhaustively checked in both directions, so its three other values
 * enclose the whole domain - 262 ranges and 2,023 code points to walk rather
 * than 1,114,112.
 *
 * The three together are 262 ranges in UCD 17.0.0, and they are gathered
 * into one list and sorted so that the filter below runs in code point
 * order - which is what makes its output sorted and coalesced without a
 * pass of its own, and is the whole reason the domain is materialised
 * rather than walked a value at a time.
 */
#define GRX_NUMERIC_DOMAIN_MAX 512

/**
 * The General_Category *groups*, which are not values.
 *
 * `\p{L}` is five categories. The Unicode library spells a group as a mask
 * rather than giving it a value, because a value would have put a fake
 * member in its General_Category enum - so the eight groups are the one kind
 * of property here that is not reached by name.
 *
 * @return The mask, or 0 for a General_Category value that is a real one.
 */
static uint32_t gc_mask_for(const char * name) {
  if (!strcmp(name, "Other")) { return GUNI_GC_MASK_C; }
  if (!strcmp(name, "Letter")) { return GUNI_GC_MASK_L; }
  if (!strcmp(name, "Cased_Letter")) { return GUNI_GC_MASK_LC; }
  if (!strcmp(name, "Mark")) { return GUNI_GC_MASK_M; }
  if (!strcmp(name, "Number")) { return GUNI_GC_MASK_N; }
  if (!strcmp(name, "Punctuation")) { return GUNI_GC_MASK_P; }
  if (!strcmp(name, "Symbol")) { return GUNI_GC_MASK_S; }
  if (!strcmp(name, "Separator")) { return GUNI_GC_MASK_Z; }
  return 0;
}

/**
 * The UCD properties this library reads as binary ones, and what they exclude.
 *
 * `\p{Indic_Conjunct_Break}` asks a yes/no question of an enumerated
 * property, and the set it means is every code point whose InCB is not None.
 * That is not a Unicode-library value, so the record cannot be reached the
 * way the other 447 are; it is the complement of one.
 *
 * A table of one, rather than an `if`, because this is a *shape* the UCD
 * produces whenever a file lists an enumerated property's non-default runs
 * and a dialect spells the name alone - and because a second one appearing
 * should be a row here rather than another branch. check-unicode-agreement
 * names any record that resolves to nothing, so a second one cannot arrive
 * unnoticed.
 */
static const struct {
  const char * name;
  const char * excluded;
} binary_readings[] = {
  {"Indic_Conjunct_Break", "None"},
};

/** How one record's code points are reached. */
typedef enum {
  ROUTE_SET,     ///< One property value's set.
  ROUTE_NOT,     ///< Everything one property value leaves out.
  ROUTE_MASK,    ///< A General_Category group, as a mask.
  ROUTE_NUMERIC  ///< One Numeric_Value over the Numeric_Type domain.
} Route;

/** A record, turned into the question to ask ghoti.io-unicode. */
typedef struct {
  Route route;
  GUNI_Property property;  ///< ROUTE_SET and ROUTE_NOT.
  uint32_t value;          ///< ROUTE_SET and ROUTE_NOT.
  uint32_t mask;           ///< ROUTE_MASK.
  int64_t numerator;       ///< ROUTE_NUMERIC; carries the sign.
  int64_t denominator;     ///< ROUTE_NUMERIC; always positive.
} Resolved;

/**
 * Turn a property index into the question its code points answer.
 *
 * @return GRX_OK; GRX_ERR_INVALID for an index that names no record; and
 *   GRX_ERR_INTERNAL when the Unicode library does not know a name this
 *   library's own table holds, which is a build with two UCD releases in it
 *   rather than anything a pattern did.
 */
static GRX_Result resolve(uint32_t property, Resolved * out) {
  if (property >= grx_unicode_property_count) {
    return GRX_ERR_INVALID;
  }
  const GRX_UnicodeProperty * record = &grx_unicode_properties[property];
  memset(out, 0, sizeof *out);

  if (record->kind == GRX_UPROP_NV) {
    // The record's identity *is* its rational, and the numeric table is the
    // only place it is written down. 144 entries, walked once per pattern.
    for (size_t i = 0; i < grx_unicode_numeric_value_count; i++) {
      if (grx_unicode_numeric_values[i].property == property) {
        out->route = ROUTE_NUMERIC;
        out->numerator = grx_unicode_numeric_values[i].numerator;
        out->denominator = grx_unicode_numeric_values[i].denominator;
        return GRX_OK;
      }
    }
    return GRX_ERR_INTERNAL;
  }

  if (record->kind == GRX_UPROP_GC) {
    uint32_t mask = gc_mask_for(record->name);
    if (mask) {
      out->route = ROUTE_MASK;
      out->mask = mask;
      return GRX_OK;
    }
  }

  const char * name = record->name;
  const char * value = "Y";
  switch (record->kind) {
    case GRX_UPROP_GC:     name = "General_Category";  value = record->name; break;
    case GRX_UPROP_SCRIPT: name = "Script";            value = record->name; break;
    case GRX_UPROP_SCX:    name = "Script_Extensions"; value = record->name; break;
    case GRX_UPROP_BLOCK:  name = "Block";            value = record->name; break;
    default: break;
  }

  if (guni_property_by_name(name, strlen(name), &out->property) != GUNI_OK) {
    return GRX_ERR_INTERNAL;
  }

  for (size_t i = 0; i < sizeof binary_readings / sizeof *binary_readings;
      i++) {
    if (!strcmp(record->name, binary_readings[i].name)) {
      const char * excluded = binary_readings[i].excluded;
      if (guni_value_by_name(out->property, excluded, strlen(excluded),
              &out->value) != GUNI_OK) {
        return GRX_ERR_INTERNAL;
      }
      out->route = ROUTE_NOT;
      return GRX_OK;
    }
  }

  if (guni_value_by_name(out->property, value, strlen(value), &out->value)
      != GUNI_OK) {
    return GRX_ERR_INTERNAL;
  }
  out->route = ROUTE_SET;
  return GRX_OK;
}

/**
 * Complement a sorted, disjoint, non-adjacent range list in place.
 *
 * Safe in place because each input yields at most one output and only when
 * there is a gap before it, so the write index never overtakes the read
 * index - and the range being read is copied out before it is written over.
 * That is also why the loop needs no bounds check: `written` stays at or
 * below the input index throughout, and the input already fitted.
 *
 * The tail is the one output with no input, and the one that can be one past
 * a buffer that exactly fitted the input - so it is counted whether or not
 * it is written, and `cap` is what decides which.
 */
static GRX_Result complement_in_place(
    GUNI_Range * ranges, size_t count, size_t cap, size_t * out_count) {
  uint32_t next = 0;
  size_t written = 0;
  for (size_t i = 0; i < count; i++) {
    uint32_t low = ranges[i].first;
    uint32_t high = ranges[i].last;
    if (low > next) {
      ranges[written].first = next;
      ranges[written].last = low - 1;
      written++;
    }
    next = high + 1;
  }
  // The tail. Not reached by anything shipped - InCB is None at U+10FFFF, so
  // the excluded set covers the top of the code space and there is nothing
  // above it - and kept because this is a complement rather than a special
  // case of one. Put in a position to work rather than assumed: pointing
  // `binary_readings` at InCB's Linker instead of its None makes the set end
  // U+11F43-U+10FFFF with this arm and U+11A9A-U+11F41 without it.
  if (next <= GRX_CODEPOINT_MAX) {
    if (written < cap) {
      ranges[written].first = next;
      ranges[written].last = GRX_CODEPOINT_MAX;
    }
    written++;
  }
  *out_count = written;
  return written <= cap ? GRX_OK : GRX_ERR_LIMIT;
}

/** The code points whose Numeric_Value is exactly this rational. */
static GRX_Result numeric_ranges(const Resolved * resolved, GUNI_Range * out,
    size_t cap, size_t * out_count) {
  GUNI_Property type;
  uint32_t none = 0;
  if (guni_property_by_name("Numeric_Type", 12, &type) != GUNI_OK
      || guni_value_by_name(type, "None", 4, &none) != GUNI_OK) {
    return GRX_ERR_INTERNAL;
  }

  // Every Numeric_Type but None, gathered and put in code point order. The
  // values are disjoint, so ordering by `first` orders them completely.
  GUNI_Range domain[GRX_NUMERIC_DOMAIN_MAX];
  size_t domain_count = 0;
  uint32_t values = guni_property_value_count(type);
  for (uint32_t value = 0; value < values; value++) {
    if (value == none) {
      continue;
    }
    size_t count = 0;
    if (guni_set_ranges(type, value, domain + domain_count,
            GRX_NUMERIC_DOMAIN_MAX - domain_count, &count) != GUNI_OK) {
      return GRX_ERR_INTERNAL;
    }
    domain_count += count;
  }
  for (size_t i = 1; i < domain_count; i++) {
    GUNI_Range key = domain[i];
    size_t j = i;
    while (j && domain[j - 1].first > key.first) {
      domain[j] = domain[j - 1];
      j--;
    }
    domain[j] = key;
  }

  // One pass in code point order, so a run of members is one range and two
  // runs that meet across a domain boundary are one range too. `written`
  // counts what the set needs whether or not there was room for it, which is
  // what lets a caller ask with a cap of 0 and be told the answer.
  size_t written = 0;
  uint32_t low = 0;
  uint32_t high = 0;
  int open = 0;
  for (size_t i = 0; i < domain_count; i++) {
    for (uint32_t code = domain[i].first; code <= domain[i].last; code++) {
      int64_t numerator = 0;
      uint32_t denominator = 0;
      if (!guni_numeric_value(code, &numerator, &denominator)
          || numerator != resolved->numerator
          || (int64_t)denominator != resolved->denominator) {
        continue;
      }
      if (open && code == high + 1) {
        high = code;
        continue;
      }
      if (open) {
        if (written < cap) {
          out[written].first = low;
          out[written].last = high;
        }
        written++;
      }
      low = code;
      high = code;
      open = 1;
    }
  }
  if (open) {
    if (written < cap) {
      out[written].first = low;
      out[written].last = high;
    }
    written++;
  }

  *out_count = written;
  return written <= cap ? GRX_OK : GRX_ERR_LIMIT;
}

GRX_Result grx_unicode_property_ranges(
    uint32_t property, GUNI_Range * out, size_t cap, size_t * out_count) {
  if (!out_count || (!out && cap)) {
    return GRX_ERR_INVALID;
  }
  *out_count = 0;

  Resolved resolved;
  GRX_Result result = resolve(property, &resolved);
  if (result != GRX_OK) {
    return result;
  }

  size_t count = 0;
  switch (resolved.route) {
    case ROUTE_MASK:
      if (guni_gc_mask_ranges(resolved.mask, out, cap, &count) != GUNI_OK) {
        *out_count = count;
        return GRX_ERR_LIMIT;
      }
      break;

    case ROUTE_SET:
      if (guni_set_ranges(resolved.property, resolved.value, out, cap, &count)
          != GUNI_OK) {
        *out_count = count;
        return GRX_ERR_LIMIT;
      }
      break;

    case ROUTE_NOT: {
      // The excluded value's own ranges land in the caller's buffer and are
      // turned inside out there. The complement of N ranges is N + 1 of
      // them, less one for each end of the code space the excluded set
      // already covers - which two membership questions settle exactly, so
      // that a caller asking with a cap of 0 is told the requirement rather
      // than a bound one too large.
      if (guni_set_ranges(resolved.property, resolved.value, out, cap, &count)
          != GUNI_OK) {
        *out_count = count + 1
            - (guni_set_contains(resolved.property, resolved.value, 0) ? 1 : 0)
            - (guni_set_contains(
                   resolved.property, resolved.value, GRX_CODEPOINT_MAX)
                    ? 1 : 0);
        return GRX_ERR_LIMIT;
      }
      result = complement_in_place(out, count, cap, &count);
      if (result != GRX_OK) {
        *out_count = count;
        return result;
      }

      break;
    }

    case ROUTE_NUMERIC:
      // `count` is what the set needs whether or not it fitted, so it is
      // reported either way - a refusal that says nothing about the
      // requirement leaves a caller asking with a cap of 0 no better off
      // than it started.
      result = numeric_ranges(&resolved, out, cap, &count);
      if (result != GRX_OK) {
        *out_count = count;
        return result;
      }
      break;

    default:
      return GRX_ERR_INTERNAL;
  }

  *out_count = count;
  return GRX_OK;
}

int grx_unicode_property_contains(uint32_t property, uint32_t codepoint) {
  Resolved resolved;
  if (resolve(property, &resolved) != GRX_OK) {
    return 0;
  }

  switch (resolved.route) {
    case ROUTE_MASK:
      return guni_gc_mask_contains(resolved.mask, codepoint) ? 1 : 0;
    case ROUTE_SET:
      return guni_set_contains(resolved.property, resolved.value, codepoint)
          ? 1 : 0;
    case ROUTE_NOT:
      return guni_set_contains(resolved.property, resolved.value, codepoint)
          ? 0 : 1;
    case ROUTE_NUMERIC: {
      // The one route with no membership test of its own: a Numeric_Value is
      // arithmetic, so the question is asked of the code point directly and
      // the domain never enters into it.
      int64_t numerator = 0;
      uint32_t denominator = 0;
      if (!guni_numeric_value(codepoint, &numerator, &denominator)) {
        return 0;
      }
      return numerator == resolved.numerator
          && (int64_t)denominator == resolved.denominator;
    }
    default:
      return 0;
  }
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

uint64_t grx_unicode_property_digest(uint32_t property) {
  if (property >= grx_unicode_property_count) {
    return 0;
  }

  return grx_unicode_properties[property].digest;
}

uint64_t grx_unicode_range_digest(const GUNI_Range * ranges, size_t count) {
  uint64_t value = 0xCBF29CE484222325ULL;
  for (size_t i = 0; i < count; i++) {
    const uint32_t words[2] = {ranges[i].first, ranges[i].last};
    for (size_t w = 0; w < 2; w++) {
      for (unsigned shift = 0; shift < 32; shift += 8) {
        value ^= (words[w] >> shift) & 0xFFu;
        value *= 0x100000001B3ULL;
      }
    }
  }
  return value;
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
