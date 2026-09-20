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
 *
 * Copyright 2026 by Corey Pennycuff
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
    case GRX_NEWLINES_NONE:
      return GRX_OK; // An empty set: nothing ends a line.
    case GRX_NEWLINES_COUNT:
    default:
      return GRX_ERR_INVALID;
  }
}
