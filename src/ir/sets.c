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
    [GRX_SET_ASCII_SPACE] = {ascii_space, sizeof(ascii_space) / sizeof(*ascii_space)},
    [GRX_SET_ES_SPACE] = {es_space, sizeof(es_space) / sizeof(*es_space)},
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
      // UTS #18 Annex C's word character: alphabetic characters, marks,
      // decimal numbers, connector punctuation, and the two join controls
      // that hold a word together without being part of it.
      //
      // `Alphabetic` and not `L`, which is what this read until
      // 2026-09-24. Annex C spells it `\p{alpha}`, and the two are not the
      // same set: `Alphabetic` is `L` plus `Nl` plus Other_Alphabetic, so
      // `L` dropped every letter-number - 236 code points, the Roman
      // numerals and the Suzhou and Counting Rod numerals among them. `\w`
      // and `[[:word:]]` both refused U+2160 here where perl and pcre2test
      // take it, and `\b` refused a boundary beside one, being defined from
      // this set. Swept a code point at a time against both references over
      // all 1,112,064; `Nl` was the whole of the difference.
      GRX_Result result = add_property(cls, "Alphabetic", limits);
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
    case GRX_SET_CATEGORIES_WORD: {
      // PCRE2's `\w` under `PCRE2_UCP`, measured: `\p{L}`, `\p{N}`,
      // `\p{Mn}` and `\p{Pc}`, and nothing else. Zero difference from
      // pcre2test 10.46 over the 286,719 code points perl, pcre2 and this
      // library all call assigned.
      //
      // Note what it is not. It is not GRX_SET_UNICODE_WORD widened: it
      // *adds* `No` and *drops* `Mc`, `Me`, the alphabetic `So` and the
      // join controls. Neither set contains the other, so there is no
      // narrowing bit that could reach one from the other - which is why
      // GRX_WordSet is an enum and this is a set in its own right.
      GRX_Result result = add_property(cls, "L", limits);
      if (result == GRX_OK) {
        result = add_property(cls, "N", limits);
      }
      if (result == GRX_OK) {
        result = add_property(cls, "Mn", limits);
      }
      if (result == GRX_OK) {
        result = add_property(cls, "Pc", limits);
      }
      return result;
    }
    case GRX_SET_ALNUM_WORD: {
      // CPython `re`'s `\w` for a `str` pattern: `SRE_UNI_IS_WORD` is
      // `Py_UNICODE_ISALNUM(ch) || ch == '_'`, and `isalnum` is `L*` plus
      // the three numeric predicates, which together are `\p{N}`. Zero
      // difference from `re` over the same 286,719.
      //
      // The underscore on its own and not `\p{Pc}`: `re` refuses U+203F
      // UNDERTIE, which the other two word sets take.
      GRX_Result result = add_property(cls, "L", limits);
      if (result == GRX_OK) {
        result = add_property(cls, "N", limits);
      }
      if (result == GRX_OK) {
        result = grx_charclass_add_range(cls, '_', '_', limits);
      }
      return result;
    }
    default:
      return GRX_ERR_INTERNAL;
  }
}

/** The named set a Unicode-width `\w` denotes under each GRX_WordSet. */
static GRX_NamedSet unicode_word_set(GRX_WordSet word_set) {
  switch (word_set) {
    case GRX_WORD_CATEGORIES:
      return GRX_SET_CATEGORIES_WORD;
    case GRX_WORD_ALNUM:
      return GRX_SET_ALNUM_WORD;
    case GRX_WORD_UTS18:
    case GRX_WORD_COUNT:
      break;
  }
  return GRX_SET_UNICODE_WORD;
}

GRX_Result grx_shorthand_set(GRX_CharClass * cls, GRX_ShorthandSet shorthands,
    GRX_WordSet word_set, int mongolian_space, GRX_ShorthandKind kind,
    const GRX_Limits * limits) {
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
      return grx_named_set(cls,
          unicode ? unicode_word_set(word_set) : GRX_SET_ASCII_WORD, limits);

    case GRX_SHORTHAND_SPACE:
    case GRX_SHORTHAND_NOT_SPACE: {
      if (shorthands == GRX_SHORTHANDS_ECMASCRIPT) {
        return grx_named_set(cls, GRX_SET_ES_SPACE, limits);
      }
      GRX_Result result = grx_named_set(cls,
          unicode ? GRX_SET_UNICODE_SPACE : GRX_SET_ASCII_SPACE, limits);
      // U+180E, at the Unicode width only - `(?aS)\s` refuses it in
      // pcre2test. See GRX_Profile::mongolian_separator_is_space.
      if (result == GRX_OK && unicode && mongolian_space) {
        result = grx_charclass_add_range(cls, 0x180E, 0x180E, limits);
      }
      return result;
    }

    // `\h` and `\v` do not move with `shorthands`, which is the one thing
    // that made them look like the other three. They are fixed sets in both
    // references and in every mode: pcre2test matches byte 0xA0 with `\h`
    // and byte 0x85 with `\v` in 8-bit mode with no UTF and no UCP, matches
    // U+00A0 and U+2028 under `utf` alone, and goes on matching both under
    // `(?a)`; perl agrees, with the subject upgraded so that the rule being
    // read is Unicode's (perl's own below-U+0100 downgrade would otherwise
    // answer for ASCII). So neither UCP nor `/a` is the deciding question,
    // and there is no narrower set to choose - a non-UTF subject is bytes,
    // and the members above 0xFF simply match nothing there, which is the
    // same truncation pcre2 does for an 8-bit pattern.
    //
    // This read `unicode ? UNICODE : ASCII` until 2026-09-24, which was
    // wrong three ways at once: `(?a)\h` refused U+00A0 in perl and pcre
    // both, and plain `\h` refused it under `utf` without `ucp` in pcre,
    // where pcre2test takes it. Only those two dialects have the letters -
    // ECMAScript's `\v` is the vertical-tab character and python's is too -
    // so the ASCII sets these chose had no reader left and are gone.
    case GRX_SHORTHAND_HSPACE:
    case GRX_SHORTHAND_NOT_HSPACE: {
      GRX_Result result = grx_named_set(cls, GRX_SET_UNICODE_HSPACE, limits);
      // The one member the two references do not share, and it is added at
      // every width because `\h` has only one: `(?a)\h` matches U+180E in
      // pcre2test where `(?a)\s` does not.
      if (result == GRX_OK && mongolian_space) {
        result = grx_charclass_add_range(cls, 0x180E, 0x180E, limits);
      }
      return result;
    }

    case GRX_SHORTHAND_VSPACE:
    case GRX_SHORTHAND_NOT_VSPACE:
      return grx_named_set(cls, GRX_SET_UNICODE_VSPACE, limits);

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
