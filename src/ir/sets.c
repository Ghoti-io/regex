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
 * The sets a dialect names instead of writing out.
 *
 * `\d`, `\w`, `\s` and the line terminators. They live here, on lowering's
 * side of the line, because *which* set a dialect means is a dialect decision
 * and resolving it is what lowering is for. They are not in the generated
 * Unicode tables because they are not facts about the UCD: ECMA-262 says its
 * `\s` includes U+FEFF, which has not been White_Space since Unicode 4.0.1,
 * and that is a statement about ECMA-262.
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <string.h>

#include "../unicode/unicode_internal.h"
#include "lower_internal.h"

/** A set written out as ranges. */
typedef struct {
  const GRX_CharRange * ranges;
  size_t count;
} FixedSet;

static const GRX_CharRange ascii_digit[] = {{'0', '9'}};
static const GRX_CharRange ascii_word[]
    = {{'0', '9'}, {'A', 'Z'}, {'_', '_'}, {'a', 'z'}};
static const GRX_CharRange ascii_space[] = {{0x09, 0x0D}, {0x20, 0x20}};
static const GRX_CharRange ascii_hspace[] = {{0x09, 0x09}, {0x20, 0x20}};
static const GRX_CharRange ascii_vspace[] = {{0x0A, 0x0D}};

// pcre2pattern's own lists. `\h` is the horizontal whitespace, which is the
// space separators together with the tab and the no-break space; `\v` is the
// vertical, which adds U+0085 to the line and paragraph separators.
static const GRX_CharRange unicode_hspace[] = {
  {0x0009, 0x0009}, {0x0020, 0x0020}, {0x00A0, 0x00A0}, {0x1680, 0x1680},
  {0x2000, 0x200A}, {0x202F, 0x202F}, {0x205F, 0x205F}, {0x3000, 0x3000},
};

static const GRX_CharRange unicode_vspace[] = {
  {0x000A, 0x000D}, {0x0085, 0x0085}, {0x2028, 0x2029},
};

/**
 * ECMA-262's `\s`: WhiteSpace (table 34) united with LineTerminator.
 *
 * WhiteSpace is TAB, VT, FF, ZWNBSP and every `\p{Zs}`; LineTerminator is LF,
 * CR, U+2028 and U+2029. Written out rather than derived, because the union
 * is a specification constant and U+FEFF is in it despite having been removed
 * from White_Space in Unicode 4.0.1 - a derivation from the UCD would quietly
 * drop it.
 */
static const GRX_CharRange es_space[] = {
  {0x0009, 0x000D}, // TAB, LF, VT, FF, CR
  {0x0020, 0x0020}, // SPACE
  {0x00A0, 0x00A0}, // NO-BREAK SPACE
  {0x1680, 0x1680}, // OGHAM SPACE MARK
  {0x2000, 0x200A}, // EN QUAD .. HAIR SPACE
  {0x2028, 0x2029}, // LINE SEPARATOR, PARAGRAPH SEPARATOR
  {0x202F, 0x202F}, // NARROW NO-BREAK SPACE
  {0x205F, 0x205F}, // MEDIUM MATHEMATICAL SPACE
  {0x3000, 0x3000}, // IDEOGRAPHIC SPACE
  {0xFEFF, 0xFEFF}, // ZERO WIDTH NO-BREAK SPACE
};

static const GRX_CharRange newlines_lf[] = {{0x0A, 0x0A}};
static const GRX_CharRange newlines_es[]
    = {{0x000A, 0x000A}, {0x000D, 0x000D}, {0x2028, 0x2029}};
static const GRX_CharRange newlines_unicode[]
    = {{0x000A, 0x000D}, {0x0085, 0x0085}, {0x2028, 0x2029}};
/*
 * Vim's 'iskeyword' at its default, enumerated.
 *
 * It was `{'0','9'},{'A','Z'},{'_','_'},{'a','z'},{0xC0,0x10FFFF}` - a
 * sample, and wrong by 5,464 code points. U+00D7 and U+00F7 are not
 * keyword characters there and neither is U+2028, so `\>` held at the end
 * of "0<U+2028>..." here and one byte in inside vim. Every code point put
 * to vim one at a time; the surrogate block is unprobed and written
 * through, a UTF-8 subject having none.
 */
static const GRX_CharRange vim_keyword[]
    = {
    {0x30, 0x39}, {0x41, 0x5A}, {0x5F, 0x5F}, {0x61, 0x7A}, {0xB5, 0xB5},
    {0xC0, 0xD6}, {0xD8, 0xF6}, {0xF8, 0x37D}, {0x37F, 0x386},
    {0x388, 0x559}, {0x560, 0x588}, {0x58A, 0x5BD}, {0x5BF, 0x5BF},
    {0x5C1, 0x5C2}, {0x5C4, 0x5F2}, {0x5F5, 0x60B}, {0x60D, 0x61A},
    {0x61C, 0x61E}, {0x620, 0x669}, {0x66E, 0x6D3}, {0x6D5, 0x6FF},
    {0x70E, 0x963}, {0x966, 0x96F}, {0x971, 0xDF3}, {0xDF5, 0xE4E},
    {0xE50, 0xE59}, {0xE5C, 0xF03}, {0xF13, 0xF39}, {0xF3E, 0xF84},
    {0xF86, 0x1049}, {0x1050, 0x10FA}, {0x10FC, 0x1360}, {0x1369, 0x166C},
    {0x166F, 0x167F}, {0x1681, 0x169A}, {0x169D, 0x16EA}, {0x16EE, 0x1734},
    {0x1737, 0x17D3}, {0x17DD, 0x17FF}, {0x180B, 0x1FFF}, {0x203C, 0x203C},
    {0x2049, 0x2049}, {0x2122, 0x2122}, {0x2139, 0x2139}, {0x2194, 0x2199},
    {0x21A9, 0x21AA}, {0x231A, 0x231B}, {0x2328, 0x2328}, {0x23CF, 0x23CF},
    {0x23E9, 0x23F3}, {0x23F8, 0x23FA}, {0x24C2, 0x24C2}, {0x25AA, 0x25AB},
    {0x25B6, 0x25B6}, {0x25C0, 0x25C0}, {0x25FB, 0x25FE}, {0x2600, 0x2604},
    {0x260E, 0x260E}, {0x2611, 0x2611}, {0x2614, 0x2615}, {0x2618, 0x2618},
    {0x261D, 0x261D}, {0x2620, 0x2620}, {0x2622, 0x2623}, {0x2626, 0x2626},
    {0x262A, 0x262A}, {0x262E, 0x262F}, {0x2638, 0x263A}, {0x2640, 0x2640},
    {0x2642, 0x2642}, {0x2648, 0x2653}, {0x265F, 0x2660}, {0x2663, 0x2663},
    {0x2665, 0x2666}, {0x2668, 0x2668}, {0x267B, 0x267B}, {0x267E, 0x267F},
    {0x2692, 0x2697}, {0x2699, 0x2699}, {0x269B, 0x269C}, {0x26A0, 0x26A1},
    {0x26A7, 0x26A7}, {0x26AA, 0x26AB}, {0x26B0, 0x26B1}, {0x26BD, 0x26BE},
    {0x26C4, 0x26C5}, {0x26C8, 0x26C8}, {0x26CE, 0x26CF}, {0x26D1, 0x26D1},
    {0x26D3, 0x26D4}, {0x26E9, 0x26EA}, {0x26F0, 0x26F5}, {0x26F7, 0x26FA},
    {0x26FD, 0x26FD}, {0x2702, 0x2702}, {0x2705, 0x2705}, {0x2708, 0x270D},
    {0x270F, 0x270F}, {0x2712, 0x2712}, {0x2714, 0x2714}, {0x2716, 0x2716},
    {0x271D, 0x271D}, {0x2721, 0x2721}, {0x2728, 0x2728}, {0x2733, 0x2734},
    {0x2744, 0x2744}, {0x2747, 0x2747}, {0x274C, 0x274C}, {0x274E, 0x274E},
    {0x2753, 0x2755}, {0x2757, 0x2757}, {0x2763, 0x2764}, {0x2795, 0x2797},
    {0x27A1, 0x27A1}, {0x27B0, 0x27B0}, {0x27BF, 0x27BF}, {0x2800, 0x28FF},
    {0x2934, 0x2935}, {0x2999, 0x29D7}, {0x29DC, 0x29FB}, {0x29FE, 0x2DFF},
    {0x2E80, 0x2FFF}, {0x3021, 0xFD3D}, {0xFD40, 0xFE2F}, {0xFE6C, 0xFEFF},
    {0xFF10, 0xFF19}, {0xFF21, 0xFF3A}, {0xFF41, 0xFF5A}, {0xFF66, 0x1CFFF},
    {0x1D250, 0x1D3FF}, {0x1D800, 0x1EFFF}, {0x1F004, 0x1F004},
    {0x1F0CF, 0x1F0CF}, {0x1F170, 0x1F171}, {0x1F17E, 0x1F17F},
    {0x1F18E, 0x1F18E}, {0x1F191, 0x1F19A}, {0x1F1E6, 0x1F1FF},
    {0x1F201, 0x1F202}, {0x1F21A, 0x1F21A}, {0x1F22F, 0x1F22F},
    {0x1F232, 0x1F23A}, {0x1F250, 0x1F251}, {0x1F300, 0x1F321},
    {0x1F324, 0x1F393}, {0x1F396, 0x1F397}, {0x1F399, 0x1F39B},
    {0x1F39E, 0x1F3F0}, {0x1F3F3, 0x1F3F5}, {0x1F3F7, 0x1F4FD},
    {0x1F4FF, 0x1F53D}, {0x1F549, 0x1F54E}, {0x1F550, 0x1F567},
    {0x1F56F, 0x1F570}, {0x1F573, 0x1F57A}, {0x1F587, 0x1F587},
    {0x1F58A, 0x1F58D}, {0x1F590, 0x1F590}, {0x1F595, 0x1F596},
    {0x1F5A4, 0x1F5A5}, {0x1F5A8, 0x1F5A8}, {0x1F5B1, 0x1F5B2},
    {0x1F5BC, 0x1F5BC}, {0x1F5C2, 0x1F5C4}, {0x1F5D1, 0x1F5D3},
    {0x1F5DC, 0x1F5DE}, {0x1F5E1, 0x1F5E1}, {0x1F5E3, 0x1F5E3},
    {0x1F5E8, 0x1F5E8}, {0x1F5EF, 0x1F5EF}, {0x1F5F3, 0x1F5F3},
    {0x1F5FA, 0x1F64F}, {0x1F680, 0x1F6C5}, {0x1F6CB, 0x1F6D2},
    {0x1F6D5, 0x1F6D7}, {0x1F6DC, 0x1F6E5}, {0x1F6E9, 0x1F6E9},
    {0x1F6EB, 0x1F6EC}, {0x1F6F0, 0x1F6F0}, {0x1F6F3, 0x1F6FC},
    {0x1F7E0, 0x1F7EB}, {0x1F7F0, 0x1F7F0}, {0x1F90C, 0x1F93A},
    {0x1F93C, 0x1F945}, {0x1F947, 0x10FFFF}};
static const GRX_CharRange newlines_cr[] = {{0x0D, 0x0D}};
static const GRX_CharRange newlines_crlf[] = {{0x0A, 0x0A}, {0x0D, 0x0D}};
static const GRX_CharRange newlines_nul[] = {{0x00, 0x00}};

/** Add every code point of a Unicode property, by its canonical name. */
static GRX_Result add_property(
    GRX_CharClass * cls, const char * name, const GRX_Limits * limits) {
  uint32_t property = 0;
  GRX_Result result = grx_unicode_property_lookup(name, strlen(name), NULL, 0,
      GRX_PROPERTY_STRICT, &property);
  if (result != GRX_OK) {
    return GRX_ERR_INTERNAL; // A name this file wrote; it must resolve.
  }

  size_t count = 0;
  const GRX_CharRange * ranges
      = grx_unicode_property_ranges(property, &count);
  return grx_charclass_add_ranges(cls, ranges, count, limits);
}

GRX_Result grx_named_set(
    GRX_CharClass * cls, GRX_NamedSet set, const GRX_Limits * limits) {
  if (!cls) {
    return GRX_ERR_INVALID;
  }

  static const FixedSet fixed[GRX_SET_COUNT] = {
    [GRX_SET_ASCII_DIGIT] = {ascii_digit, sizeof(ascii_digit) / sizeof(*ascii_digit)},
    [GRX_SET_ASCII_WORD] = {ascii_word, sizeof(ascii_word) / sizeof(*ascii_word)},
    [GRX_SET_VIM_KEYWORD]
        = {vim_keyword, sizeof(vim_keyword) / sizeof(*vim_keyword)},
    [GRX_SET_ASCII_SPACE] = {ascii_space, sizeof(ascii_space) / sizeof(*ascii_space)},
    [GRX_SET_ES_SPACE] = {es_space, sizeof(es_space) / sizeof(*es_space)},
    [GRX_SET_ASCII_HSPACE] = {ascii_hspace, sizeof(ascii_hspace) / sizeof(*ascii_hspace)},
    [GRX_SET_ASCII_VSPACE] = {ascii_vspace, sizeof(ascii_vspace) / sizeof(*ascii_vspace)},
    [GRX_SET_UNICODE_HSPACE]
        = {unicode_hspace, sizeof(unicode_hspace) / sizeof(*unicode_hspace)},
    [GRX_SET_UNICODE_VSPACE]
        = {unicode_vspace, sizeof(unicode_vspace) / sizeof(*unicode_vspace)},
    [GRX_SET_NEWLINES_LF] = {newlines_lf, sizeof(newlines_lf) / sizeof(*newlines_lf)},
    [GRX_SET_NEWLINES_ES] = {newlines_es, sizeof(newlines_es) / sizeof(*newlines_es)},
    [GRX_SET_NEWLINES_UNICODE]
        = {newlines_unicode, sizeof(newlines_unicode) / sizeof(*newlines_unicode)},
    [GRX_SET_NEWLINES_CR] = {newlines_cr, sizeof(newlines_cr) / sizeof(*newlines_cr)},
    [GRX_SET_NEWLINES_CRLF]
        = {newlines_crlf, sizeof(newlines_crlf) / sizeof(*newlines_crlf)},
    [GRX_SET_NEWLINES_NUL]
        = {newlines_nul, sizeof(newlines_nul) / sizeof(*newlines_nul)},
  };

  if ((unsigned)set >= (unsigned)GRX_SET_COUNT) {
    return GRX_ERR_INVALID;
  }
  if (fixed[set].ranges) {
    return grx_charclass_add_ranges(
        cls, fixed[set].ranges, fixed[set].count, limits);
  }

  switch (set) {
    case GRX_SET_UNICODE_DIGIT:
      return add_property(cls, "Nd", limits);
    case GRX_SET_UNICODE_SPACE:
      return add_property(cls, "White_Space", limits);
    case GRX_SET_UNICODE_WORD: {
      // UTS #18 Annex C's word character: letters, marks, decimal numbers,
      // connector punctuation, and the two join controls that hold a word
      // together without being part of it.
      GRX_Result result = add_property(cls, "L", limits);
      if (result == GRX_OK) {
        result = add_property(cls, "M", limits);
      }
      if (result == GRX_OK) {
        result = add_property(cls, "Nd", limits);
      }
      if (result == GRX_OK) {
        result = add_property(cls, "Pc", limits);
      }
      if (result == GRX_OK) {
        result = grx_charclass_add_range(cls, 0x200C, 0x200D, limits);
      }
      return result;
    }
    default:
      return GRX_ERR_INTERNAL;
  }
}

GRX_Result grx_shorthand_set(GRX_CharClass * cls, GRX_ShorthandSet shorthands,
    GRX_ShorthandKind kind, const GRX_Limits * limits) {
  if (!cls) {
    return GRX_ERR_INVALID;
  }

  int unicode = shorthands == GRX_SHORTHANDS_UNICODE;

  switch (kind) {
    case GRX_SHORTHAND_DIGIT:
    case GRX_SHORTHAND_NOT_DIGIT:
      return grx_named_set(cls,
          unicode ? GRX_SET_UNICODE_DIGIT : GRX_SET_ASCII_DIGIT, limits);

    case GRX_SHORTHAND_WORD:
    case GRX_SHORTHAND_NOT_WORD:
      // ECMAScript's `\w` is ASCII in both its modes. The two extra members
      // its Unicode mode gains under `i` - U+017F and U+212A - are not a
      // different set: they are what closing this set under simple folding
      // produces, and the caller closes it (ECMA-262 22.2.2.9.3, whose
      // definition is exactly that closure).
      if (shorthands == GRX_SHORTHANDS_VIM_KEYWORD) {
        // Vim, where the *word* this asks about is 'iskeyword' and not
        // `\w`. Only `\<` and `\>` reach here for that dialect: its
        // front end builds `\w`, `\d` and the rest from its own measured
        // tables, so this is the boundary set and nothing else.
        return grx_named_set(cls, GRX_SET_VIM_KEYWORD, limits);
      }
      return grx_named_set(cls,
          unicode ? GRX_SET_UNICODE_WORD : GRX_SET_ASCII_WORD, limits);

    case GRX_SHORTHAND_SPACE:
    case GRX_SHORTHAND_NOT_SPACE:
      if (shorthands == GRX_SHORTHANDS_ECMASCRIPT) {
        return grx_named_set(cls, GRX_SET_ES_SPACE, limits);
      }
      return grx_named_set(cls,
          unicode ? GRX_SET_UNICODE_SPACE : GRX_SET_ASCII_SPACE, limits);

    case GRX_SHORTHAND_HSPACE:
    case GRX_SHORTHAND_NOT_HSPACE:
      return grx_named_set(cls,
          unicode ? GRX_SET_UNICODE_HSPACE : GRX_SET_ASCII_HSPACE, limits);

    case GRX_SHORTHAND_VSPACE:
    case GRX_SHORTHAND_NOT_VSPACE:
      return grx_named_set(cls,
          unicode ? GRX_SET_UNICODE_VSPACE : GRX_SET_ASCII_VSPACE, limits);

    case GRX_SHORTHAND_COUNT:
    default:
      return GRX_ERR_UNSUPPORTED;
  }
}

GRX_Result grx_newline_set(GRX_CharClass * cls, GRX_NewlineSet newlines,
    const GRX_Limits * limits) {
  if (!cls) {
    return GRX_ERR_INVALID;
  }

  switch (newlines) {
    case GRX_NEWLINES_LF:
      return grx_named_set(cls, GRX_SET_NEWLINES_LF, limits);
    case GRX_NEWLINES_ECMASCRIPT:
      return grx_named_set(cls, GRX_SET_NEWLINES_ES, limits);
    case GRX_NEWLINES_UNICODE:
      return grx_named_set(cls, GRX_SET_NEWLINES_UNICODE, limits);
    case GRX_NEWLINES_CR:
      return grx_named_set(cls, GRX_SET_NEWLINES_CR, limits);
    case GRX_NEWLINES_ANYCRLF:
      return grx_named_set(cls, GRX_SET_NEWLINES_CRLF, limits);
    case GRX_NEWLINES_ANY:
      return grx_named_set(cls, GRX_SET_NEWLINES_UNICODE, limits);
    case GRX_NEWLINES_NUL:
      return grx_named_set(cls, GRX_SET_NEWLINES_NUL, limits);
    case GRX_NEWLINES_CRLF:
      // Empty on purpose, and it is the case that shows why the CR LF pair
      // is a *flag* and not a member: under `(*CRLF)` no single character
      // ends a line, so `.` refuses nothing - pcre2test matches `a.b`
      // against both "a\nb" and "a\rb" there - while `^` and `$` still
      // hold around the pair.
      return GRX_OK;
    case GRX_NEWLINES_NONE:
      return GRX_OK; // An empty set: nothing ends a line.
    case GRX_NEWLINES_COUNT:
    default:
      return GRX_ERR_INVALID;
  }
}

int grx_newline_has_crlf(GRX_NewlineSet newlines) {
  // The three conventions in which a CR LF pair is one line terminator
  // rather than two. What it changes is where `^`, `$` and `\Z` hold, and
  // nothing about which single characters end a line - which is why it is
  // asked separately from grx_newline_set().
  return newlines == GRX_NEWLINES_CRLF || newlines == GRX_NEWLINES_ANYCRLF
      || newlines == GRX_NEWLINES_ANY;
}
