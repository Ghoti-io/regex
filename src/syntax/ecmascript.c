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
 * The ECMAScript front end: how ECMA-262 spells what it has.
 *
 * Two of the three modes of documentation/dialects.md section 8 are here.
 * **Unicode mode** (the `u` flag) is ECMA-262 22.2.1 with the
 * `[+UnicodeMode]` productions, and it is the mode JSON Schema uses
 * (design.md section 1.1). **Legacy mode** is the same grammar as modified
 * by Annex B.1.2, which is what every browser and Node actually run for a
 * pattern without `u` - so it is the default here, not a compatibility
 * switch. UnicodeSets mode (`v`) is plan.md WP-12 and is refused with its
 * own diagnostic rather than silently read as `u`.
 *
 * Almost every rule below is a rule Annex B relaxes and Unicode mode does
 * not, which is why nearly every function here asks `unicode_mode()` once and
 * then reads as one grammar or the other. Where the two agree, the code does
 * not ask.
 *
 * The behaviour of each rule was checked against Node 22, which is the oracle
 * the conformance vectors come from (testing.md section 2). Three of the
 * checks corrected what this library would otherwise have implemented, and
 * each is marked below.
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/pattern.h>
#include <ghoti.io/regex/syntax.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../core/core_internal.h"
#include "../parse/parse_internal.h"
#include "../unicode/unicode_internal.h"

/** Whether the `u` flag is in force, and so which grammar applies. */
static int unicode_mode(const GRX_Parser * parser) {
  return (parser->options & GRX_OPT_UTF) != 0;
}

/**
 * The `v` flag: UnicodeSets mode.
 *
 * `v` implies `u` - grx_options_parse() sets both - so every test of
 * unicode_mode() is still true here. What this one selects is the *class*
 * grammar, which is a different language: `[a-z]--[aeiou]` is a subtraction
 * in `v` and a class of eight characters in `u`.
 */
static int unicode_sets_mode(const GRX_Parser * parser) {
  return (parser->options & GRX_OPT_UNICODE_SETS) != 0;
}

/** The byte at an offset from the current position, or 0 past the end. */
static char byte_at(const GRX_Parser * parser, size_t ahead) {
  size_t index = parser->position + ahead;
  return index < parser->length ? parser->text[index] : '\0';
}

static int is_decimal(char c) { return c >= '0' && c <= '9'; }
static int is_octal(char c) { return c >= '0' && c <= '7'; }
static int is_hex(char c) {
  return is_decimal(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static uint32_t hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return (uint32_t)(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return (uint32_t)(c - 'a' + 10);
  }
  return (uint32_t)(c - 'A' + 10);
}

/** Whether a code point may be a literal written without a backslash. */
/**
 * `ClassSetReservedPunctuator`: the characters `\` may quote in a `v` class.
 *
 * Every one of them is also half of a reserved double punctuator, which is
 * why they are quotable: `[&&]` is an operator and `[\&\&]` is two
 * ampersands, and without the escape there would be no way to write the
 * second.
 */
static int is_reserved_punctuator(uint32_t codepoint) {
  switch (codepoint) {
    case '&': case '-': case '!': case '#': case '%': case ',':
    case ':': case ';': case '<': case '=': case '>': case '@':
    case '`': case '~':
      return 1;
    default:
      return 0;
  }
}

/**
 * `ClassSetSyntaxCharacter`: what may not appear unescaped in a `v` class.
 *
 * A shorter list than the pattern-level one and a different list: `$`, `*`,
 * `+`, `?` and `.` are ordinary characters inside a set, and `-` is not.
 */
static int is_class_set_syntax_character(uint32_t codepoint) {
  switch (codepoint) {
    case '(': case ')': case '[': case ']': case '{': case '}':
    case '/': case '-': case '\\': case '|':
      return 1;
    default:
      return 0;
  }
}

/**
 * `ClassSetReservedDoublePunctuator`: a pair that must be written escaped.
 *
 * Reserved rather than forbidden: `&&` and `--` already mean something, and
 * the other twelve are held back so that a future edition can give them a
 * meaning without breaking a pattern that used them as two characters. A
 * pattern that wants two of them today writes `\!\!`.
 */
static int is_reserved_double(char c) {
  switch (c) {
    case '&': case '!': case '#': case '$': case '%': case '*':
    case '+': case ',': case '.': case ':': case ';': case '<':
    case '=': case '>': case '?': case '@': case '^': case '`':
    case '~':
      return 1;
    default:
      return 0;
  }
}

static int is_syntax_character(uint32_t codepoint) {
  switch (codepoint) {
    case '^': case '$': case '\\': case '.': case '*': case '+':
    case '?': case '(': case ')': case '[': case ']': case '{':
    case '}': case '|':
      return 1;
    default:
      return 0;
  }
}

/** Whether a code point has a Unicode property, by its canonical name. */
static int has_property(const char * name, uint32_t codepoint) {
  uint32_t property = 0;
  if (grx_unicode_property_lookup(name, strlen(name), NULL, 0,
          GRX_PROPERTY_STRICT, &property) != GRX_OK) {
    return 0;
  }

  size_t count = 0;
  const GRX_CharRange * ranges
      = grx_unicode_property_ranges(property, &count);
  return grx_range_contains(ranges, count, codepoint);
}

// --------------------------------------------------------------------------
// Numeric escapes
// --------------------------------------------------------------------------

/** Read exactly `digits` hex digits. Returns 0 without consuming if absent. */
static int read_fixed_hex(
    GRX_Parser * parser, size_t digits, uint32_t * out_value) {
  for (size_t i = 0; i < digits; i++) {
    if (!is_hex(byte_at(parser, i))) {
      return 0;
    }
  }

  uint32_t value = 0;
  for (size_t i = 0; i < digits; i++) {
    value = (value << 4) | hex_value(parser->text[parser->position + i]);
  }
  parser->position += digits;
  *out_value = value;
  return 1;
}

/**
 * Read `\xHH`, the `x` already consumed.
 *
 * Exactly two digits or nothing. In Unicode mode "nothing" is a syntax error;
 * Annex B makes it the identity escape `x`, so `/\xZ/` matches "xZ".
 */
static GRX_Result read_hex_escape(
    GRX_Parser * parser, size_t start, uint32_t * out_node) {
  uint32_t value = 0;
  if (read_fixed_hex(parser, 2, &value)) {
    return grx_parse_literal_node(
        parser, value, start, parser->position - start, out_node);
  }
  if (unicode_mode(parser)) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_HEX_ESCAPE, start,
        parser->position - start);
  }

  return grx_parse_literal_node(parser, 'x', start, 2, out_node);
}

/**
 * Read the code point of a `\u` escape, the `u` already consumed.
 *
 * Three forms: `\u{H...}` in Unicode mode only, a lone `\uHHHH`, and a
 * surrogate *pair* written as two `\u` escapes, which Unicode mode joins into
 * one code point. Returns 0 without consuming anything when the escape is
 * malformed, which is what Annex B needs to fall back to the identity escape.
 */
/**
 * Read what follows `\u`, with the Unicode escape grammar switched on or off.
 *
 * `unicode_syntax` is the grammar's [+UnicodeMode] parameter rather than the
 * `u` flag, and the two are not the same thing. Almost everywhere the flag
 * decides it - `\u{1F600}` is one code point with `u` and the letter `u`
 * repeated without it. Inside a group name it does not:
 * RegExpIdentifierStart's production is
 * `\ RegExpUnicodeEscapeSequence[+UnicodeMode]`, with the parameter set
 * unconditionally, so `(?<\u{1d5b0}x>y)` is a valid name in a pattern with no
 * flags at all. Passing the flag there rejected fifty-five of test262's
 * named-group cases.
 */
static int read_unicode_escape_with(
    GRX_Parser * parser, int unicode_syntax, uint32_t * out_value) {
  size_t start = parser->position;

  if (unicode_syntax && byte_at(parser, 0) == '{') {
    size_t scan = 1;
    uint32_t value = 0;
    if (!is_hex(byte_at(parser, scan))) {
      return 0;
    }
    while (is_hex(byte_at(parser, scan))) {
      value = (value << 4) | hex_value(byte_at(parser, scan));
      if (value > GRX_CODEPOINT_MAX) {
        parser->position = start;
        return 0;
      }
      scan++;
    }
    if (byte_at(parser, scan) != '}') {
      return 0;
    }
    parser->position += scan + 1;
    *out_value = value;
    return 1;
  }

  uint32_t lead = 0;
  if (!read_fixed_hex(parser, 4, &lead)) {
    return 0;
  }

  // A high surrogate followed by `\u` and a low surrogate is one code point
  // in Unicode mode, because the source is UTF-16 and that pair is how an
  // astral character is written in it.
  if (unicode_syntax && lead >= 0xD800u && lead <= 0xDBFFu
      && byte_at(parser, 0) == '\\' && byte_at(parser, 1) == 'u') {
    size_t after_lead = parser->position;
    parser->position += 2;
    uint32_t trail = 0;
    if (read_fixed_hex(parser, 4, &trail) && trail >= 0xDC00u
        && trail <= 0xDFFFu) {
      *out_value = 0x10000u + ((lead - 0xD800u) << 10) + (trail - 0xDC00u);
      return 1;
    }
    parser->position = after_lead;
  }

  *out_value = lead;
  return 1;
}

/** The common case: the grammar parameter follows the `u` flag. */
static int read_unicode_escape(GRX_Parser * parser, uint32_t * out_value) {
  return read_unicode_escape_with(parser, unicode_mode(parser), out_value);
}

/**
 * Read `\cX`, the `c` already consumed.
 *
 * `in_class` widens what may follow: Annex B lets a digit or `_` follow `\c`
 * inside a class, so `[\c1]` is U+0011. Outside one it must be a letter, and
 * `\c` followed by anything else is not an escape at all - the atom is the
 * single character `\`, and the `c` is read afterwards as an ordinary
 * literal. Checked against Node: `/\c/` matches the two characters "\c".
 */
static int read_control_escape(
    GRX_Parser * parser, int in_class, uint32_t * out_value) {
  char c = byte_at(parser, 0);
  int letter = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
  int extra = in_class && !unicode_mode(parser)
      && (is_decimal(c) || c == '_');
  if (!letter && !extra) {
    return 0;
  }

  parser->position++;
  *out_value = (uint32_t)c % 32u;
  return 1;
}

/**
 * Read a legacy octal escape, the backslash consumed and `parser->position`
 * on the first digit.
 *
 * ECMA-262 Annex B's LegacyOctalEscapeSequence: three digits when the first
 * is 0 to 3, two otherwise, so the largest is `\377`. That is why `/\400/`
 * is accepted and means `\40` followed by a literal `0`.
 */
static uint32_t read_legacy_octal(GRX_Parser * parser) {
  uint32_t value = (uint32_t)(byte_at(parser, 0) - '0');
  size_t digits = 1;
  size_t allowed = byte_at(parser, 0) <= '3' ? 3 : 2;

  while (digits < allowed && is_octal(byte_at(parser, digits))) {
    value = value * 8 + (uint32_t)(byte_at(parser, digits) - '0');
    digits++;
  }

  parser->position += digits;
  return value;
}

// --------------------------------------------------------------------------
// Group names
// --------------------------------------------------------------------------

/**
 * Read a RegExpIdentifierName into `buffer`, up to but not including the
 * terminator.
 *
 * ECMA-262 22.2.1: an IdentifierName, with `\u` escapes allowed inside it, so
 * `(?<a>x)` names the group `a`. Unicode escapes are resolved here, so
 * that two spellings of one name are one name.
 */
static GRX_Result read_group_name(GRX_Parser * parser, char terminator,
    char * buffer, size_t capacity, size_t * out_length) {
  size_t start = parser->position;
  size_t written = 0;
  int first = 1;

  for (;;) {
    if (grx_parse_at_end(parser)) {
      return grx_parse_fail(
          parser, GRX_DIAG_UNTERMINATED_NAME, start, parser->position - start);
    }
    if (byte_at(parser, 0) == terminator) {
      break;
    }

    uint32_t codepoint = 0;
    if (byte_at(parser, 0) == '\\' && byte_at(parser, 1) == 'u') {
      parser->position += 2;
      // Always the Unicode grammar here, whatever the flags say.
      if (!read_unicode_escape_with(parser, 1, &codepoint)) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_UNICODE_ESCAPE,
            parser->position, 1);
      }
    }
    else {
      GRX_Result result = grx_parse_take(parser, &codepoint);
      if (result != GRX_OK) {
        return result;
      }
    }

    int ok = codepoint == '$' || codepoint == '_'
        || has_property(first ? "ID_Start" : "ID_Continue", codepoint)
        || (!first && (codepoint == 0x200Cu || codepoint == 0x200Du));
    if (!ok) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_NAME, start,
          parser->position - start);
    }
    first = 0;

    char encoded[4];
    size_t width = grx_unicode_utf8_encode(codepoint, encoded);
    if (!width || written + width >= capacity) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_NAME, start,
          parser->position - start);
    }
    memcpy(buffer + written, encoded, width);
    written += width;
  }

  if (!written) {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_GROUP_NAME, start, parser->position - start);
  }

  parser->position++; // The terminator.
  buffer[written] = '\0';
  *out_length = written;
  return GRX_OK;
}

/** The longest group name this front end will accept, in bytes. */
#define GRX_ES_NAME_MAX 256

/** Whether a capturing group with this name has already been parsed. */
static int name_already_used(const GRX_Parser * parser, const char * name) {
  for (size_t i = 0; i < parser->pattern->nodes.count; i++) {
    const GRX_Node * node = grx_pattern_node(parser->pattern, (uint32_t)i);
    if (!node || node->kind != GRX_NODE_GROUP
        || !(node->flags & GRX_NODE_NAMED)) {
      continue;
    }
    const char * existing = grx_pattern_name(parser->pattern, node->b);
    if (existing && strcmp(existing, name) == 0) {
      return 1;
    }
  }

  return 0;
}

// --------------------------------------------------------------------------
// Escapes outside a character class
// --------------------------------------------------------------------------

/** The shorthand a letter names, or -1. */
static int shorthand_for(char c, int * out_negated) {
  *out_negated = 0;
  switch (c) {
    case 'd': return GRX_SHORTHAND_DIGIT;
    case 'D': *out_negated = 1; return GRX_SHORTHAND_DIGIT;
    case 'w': return GRX_SHORTHAND_WORD;
    case 'W': *out_negated = 1; return GRX_SHORTHAND_WORD;
    case 's': return GRX_SHORTHAND_SPACE;
    case 'S': *out_negated = 1; return GRX_SHORTHAND_SPACE;
    default: return -1;
  }
}

/** The code point a single-letter control escape denotes, or 0. */
static uint32_t control_letter(char c) {
  switch (c) {
    case 'f': return 0x0C;
    case 'n': return 0x0A;
    case 'r': return 0x0D;
    case 't': return 0x09;
    case 'v': return 0x0B;
    default: return 0;
  }
}

/**
 * Read `\p{...}` or `\P{...}`, the letter already consumed, into an item.
 *
 * The name is stored in the pattern's name table exactly as written, with the
 * `=` kept when there is one: resolving it to a set is lowering's job, and
 * doing it here would put the Unicode tables in the parser.
 */
static GRX_Result read_property(GRX_Parser * parser, int negated,
    size_t start, GRX_ClassItem * out_item) {
  if (!grx_parse_eat(parser, '{')) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_PROPERTY_SYNTAX, start,
        parser->position - start);
  }

  size_t name_start = parser->position;
  while (!grx_parse_at_end(parser) && byte_at(parser, 0) != '}') {
    parser->position++;
  }
  if (grx_parse_at_end(parser)) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_PROPERTY_SYNTAX, start,
        parser->position - start);
  }

  size_t name_length = parser->position - name_start;
  parser->position++; // The `}`.
  if (!name_length) {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_PROPERTY_SYNTAX, start, parser->position - start);
  }

  // Resolved now, not at lowering, so that `\p{Nosuch}` is rejected with an
  // offset into the pattern - which is what a caller showing a user where
  // their pattern went wrong needs, and what ECMA-262 requires: an unknown
  // property is an early SyntaxError, not a match-time failure.
  const char * name = parser->text + name_start;
  const char * equals = memchr(name, '=', name_length);
  uint32_t property = 0;
  GRX_Result resolved = equals
      ? grx_unicode_property_lookup(name, (size_t)(equals - name), equals + 1,
            name_length - (size_t)(equals - name) - 1,
            parser->profile.property_match, &property)
      : grx_unicode_property_lookup(name, name_length, NULL, 0,
            parser->profile.property_match, &property);
  if (resolved != GRX_OK) {
    return grx_parse_fail(
        parser, GRX_DIAG_UNKNOWN_PROPERTY, start, parser->position - start);
  }

  uint32_t offset = GRX_INDEX_NONE;
  GRX_Result result
      = grx_pattern_add_name(parser->pattern, name, name_length, &offset);
  if (result != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }

  *out_item = (GRX_ClassItem) {
    .kind = GRX_CLASS_ITEM_PROPERTY,
    .flags = negated ? GRX_CLASS_ITEM_NEGATED : 0u,
    .lo = 0,
    .hi = 0,
    .a = offset,
    .offset = start,
    .length = parser->position - start,
  };
  return GRX_OK;
}

/** Wrap one class item in a class node of its own. */
/**
 * A `\p{...}` naming a property of *strings*, if that is what it names.
 *
 * Defined with the rest of the UnicodeSets grammar below; declared here
 * because an atom may be one too, not only a class operand.
 *
 * @return GRX_OK when it was one, GRX_ERR_SYNTAX when the name belongs to
 *   the ordinary property tables instead - in which case nothing was
 *   consumed - or a failure.
 */
/**
 * One operand of a class-set expression.
 *
 * `is_character` is not a convenience: `ClassSetRange` is two
 * *ClassSetCharacters* with a dash between them, so `[\d-z]` and `[\q{a}-z]`
 * are syntax errors where `[a-z]` is a range. Carrying the distinction on the
 * operand is what makes that one comparison rather than a second parse.
 *
 * `may_contain_strings` is ECMA-262's MayContainStrings, and exists for one
 * rule: a negated class may not contain strings. It propagates differently
 * through each operator - any operand for a union, every operand for an
 * intersection, only the first for a subtraction - which is why it is
 * computed alongside the parse rather than asked of the tree afterwards.
 */
struct SetOperand {
  GRX_ClassItem item;
  int is_character;
  int may_contain_strings;
};

typedef struct SetOperand SetOperand;
static GRX_Result string_property_atom(
    GRX_Parser * parser, int negated, size_t start, SetOperand * out);

static GRX_Result item_as_node(GRX_Parser * parser, const GRX_ClassItem * item,
    uint32_t * out_node) {
  GRX_Result result = grx_parse_class_node(parser, item->offset, out_node);
  if (result != GRX_OK) {
    return result;
  }
  result = grx_parse_class_add(parser, *out_node, item);
  if (result != GRX_OK) {
    return result;
  }

  grx_pattern_node(parser->pattern, *out_node)->length = item->length;
  return GRX_OK;
}

/** Append an anchor node. */
static GRX_Result anchor_node(GRX_Parser * parser, GRX_AnchorKind anchor,
    size_t start, uint32_t * out_node) {
  GRX_Result result = grx_pattern_add_node(parser->pattern, GRX_NODE_ANCHOR,
      start, parser->position - start, out_node);
  if (result != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }

  grx_pattern_node(parser->pattern, *out_node)->a = (uint32_t)anchor;
  return GRX_OK;
}

/** Append a backreference node, by number or by name. */
static GRX_Result backref_node(GRX_Parser * parser, uint32_t group,
    uint32_t name_offset, size_t start, uint32_t * out_node) {
  GRX_Result result = grx_pattern_add_node(parser->pattern, GRX_NODE_BACKREF,
      start, parser->position - start, out_node);
  if (result != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }

  GRX_Node * node = grx_pattern_node(parser->pattern, *out_node);
  node->a = group;
  node->b = name_offset;
  if (name_offset != GRX_INDEX_NONE) {
    node->flags |= GRX_NODE_NAMED;
  }
  return GRX_OK;
}

static GRX_Result es_atom_escape(GRX_Parser * parser, uint32_t * out_node) {
  size_t start = parser->position - 1; // The backslash.
  char c = byte_at(parser, 0);

  if (c == 'b' || c == 'B') {
    parser->position++;
    return anchor_node(parser,
        c == 'b' ? GRX_ANCHOR_WORD_BOUNDARY : GRX_ANCHOR_NOT_WORD_BOUNDARY,
        start, out_node);
  }

  int negated = 0;
  int shorthand = shorthand_for(c, &negated);
  if (shorthand >= 0) {
    parser->position++;
    return grx_parse_shorthand_node(parser, (GRX_ShorthandKind)shorthand,
        negated, start, parser->position - start, out_node);
  }

  if (c == 'p' || c == 'P') {
    if (unicode_mode(parser)) {
      // A property of strings is an atom in `v` mode as well as a class
      // operand: `\p{RGI_Emoji}` on its own matches a flag sequence, which
      // is not something a class of code points could express. Offered to
      // the string tables first for the same reason it is inside a class -
      // what a name means is decided by which table holds it.
      if (unicode_sets_mode(parser)) {
        SetOperand operand;
        GRX_Result result
            = string_property_atom(parser, c == 'P', start, &operand);
        if (result != GRX_ERR_SYNTAX) {
          return result == GRX_OK
              ? item_as_node(parser, &operand.item, out_node)
              : result;
        }
      }
      parser->position++;
      GRX_ClassItem item;
      GRX_Result result = read_property(parser, c == 'P', start, &item);
      if (result != GRX_OK) {
        return result;
      }
      return item_as_node(parser, &item, out_node);
    }
    // Annex B: property escapes do not exist, so `\p` is the identity
    // escape for `p`.
  }

  uint32_t control = control_letter(c);
  if (control) {
    parser->position++;
    return grx_parse_literal_node(parser, control, start, 2, out_node);
  }

  if (c == 'c') {
    parser->position++;
    uint32_t value = 0;
    if (read_control_escape(parser, 0, &value)) {
      return grx_parse_literal_node(
          parser, value, start, parser->position - start, out_node);
    }
    if (unicode_mode(parser)) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_CONTROL_ESCAPE, start, 2);
    }
    // Annex B ExtendedAtom: `\` followed by `c` with no letter after it is
    // the single character `\`. The `c` is left for the next atom, which is
    // why `/\c/` matches the two characters "\c".
    parser->position = start + 1;
    return grx_parse_literal_node(parser, '\\', start, 1, out_node);
  }

  if (c == 'x') {
    parser->position++;
    return read_hex_escape(parser, start, out_node);
  }

  if (c == 'u') {
    parser->position++;
    uint32_t value = 0;
    if (read_unicode_escape(parser, &value)) {
      return grx_parse_literal_node(
          parser, value, start, parser->position - start, out_node);
    }
    if (unicode_mode(parser)) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_UNICODE_ESCAPE, start, 2);
    }
    return grx_parse_literal_node(parser, 'u', start, 2, out_node);
  }

  if (c == 'k') {
    // `\k` is a named reference only when the pattern names a group
    // somewhere, which the prescan has already established - including when
    // the group comes *after* the reference.
    if (unicode_mode(parser) || parser->named_groups) {
      parser->position++;
      if (!grx_parse_eat(parser, '<')) {
        return grx_parse_fail(
            parser, GRX_DIAG_INVALID_BACKREFERENCE, start, 2);
      }
      char name[GRX_ES_NAME_MAX];
      size_t name_length = 0;
      GRX_Result result = read_group_name(
          parser, '>', name, sizeof(name), &name_length);
      if (result != GRX_OK) {
        return result;
      }
      uint32_t offset = GRX_INDEX_NONE;
      result = grx_pattern_add_name(
          parser->pattern, name, name_length, &offset);
      if (result != GRX_OK) {
        return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
      }
      return backref_node(parser, 0, offset, start, out_node);
    }
    // Otherwise Annex B's identity escape, so `/\k/` matches "k".
  }

  if (c == '0') {
    parser->position++;
    if (unicode_mode(parser)) {
      if (is_decimal(byte_at(parser, 0))) {
        return grx_parse_fail(
            parser, GRX_DIAG_INVALID_OCTAL_ESCAPE, start, 3);
      }
      return grx_parse_literal_node(parser, 0, start, 2, out_node);
    }
    if (is_octal(byte_at(parser, 0))) {
      parser->position = start + 1;
      uint32_t value = read_legacy_octal(parser);
      return grx_parse_literal_node(
          parser, value, start, parser->position - start, out_node);
    }
    return grx_parse_literal_node(parser, 0, start, 2, out_node);
  }

  if (c >= '1' && c <= '9') {
    // Read the whole number first: the choice between a backreference and an
    // octal escape is made on its value, not on its first digit.
    size_t digits = 0;
    uint64_t value = 0;
    while (is_decimal(byte_at(parser, digits)) && value < 0xFFFFFFFFu) {
      value = value * 10 + (uint64_t)(byte_at(parser, digits) - '0');
      digits++;
    }

    if (value <= parser->group_count) {
      parser->position += digits;
      return backref_node(
          parser, (uint32_t)value, GRX_INDEX_NONE, start, out_node);
    }
    if (unicode_mode(parser)) {
      // In Unicode mode a decimal escape must name a group that exists.
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_BACKREFERENCE, start, digits + 1);
    }
    if (c <= '7') {
      uint32_t octal = read_legacy_octal(parser);
      return grx_parse_literal_node(
          parser, octal, start, parser->position - start, out_node);
    }
    // `\8` and `\9` are not octal, so Annex B makes them identity escapes.
  }

  // Identity escapes. In Unicode mode only the syntax characters and `/`
  // may be escaped; Annex B allows any character that is not `c`, which the
  // `\c` branch above has already taken.
  uint32_t codepoint = 0;
  size_t before = parser->position;
  GRX_Result result = grx_parse_take(parser, &codepoint);
  if (result != GRX_OK) {
    return result;
  }
  if (unicode_mode(parser) && !is_syntax_character(codepoint)
      && codepoint != '/') {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_ESCAPE, start, parser->position - start);
  }

  (void)before;
  return grx_parse_literal_node(
      parser, codepoint, start, parser->position - start, out_node);
}

// --------------------------------------------------------------------------
// Escapes inside a character class
// --------------------------------------------------------------------------

static GRX_Result es_class_escape(GRX_Parser * parser, GRX_ClassItem * out) {
  size_t start = parser->position - 1;
  char c = byte_at(parser, 0);

  *out = (GRX_ClassItem) {
    .kind = GRX_CLASS_ITEM_SINGLE,
    .flags = 0,
    .lo = 0,
    .hi = 0,
    .a = 0,
    .offset = start,
    .length = 0,
  };

  // `\b` inside a class is a backspace, not a word boundary: there is no
  // zero-width assertion to be had inside a set of characters.
  if (c == 'b') {
    parser->position++;
    out->lo = 0x08;
    out->length = parser->position - start;
    return GRX_OK;
  }

  int negated = 0;
  int shorthand = shorthand_for(c, &negated);
  if (shorthand >= 0) {
    parser->position++;
    out->kind = GRX_CLASS_ITEM_SHORTHAND;
    out->flags = negated ? GRX_CLASS_ITEM_NEGATED : 0u;
    out->a = (uint32_t)shorthand;
    out->length = parser->position - start;
    return GRX_OK;
  }

  if ((c == 'p' || c == 'P') && unicode_mode(parser)) {
    parser->position++;
    return read_property(parser, c == 'P', start, out);
  }

  uint32_t control = control_letter(c);
  if (control) {
    parser->position++;
    out->lo = control;
    out->length = parser->position - start;
    return GRX_OK;
  }

  if (c == 'c') {
    parser->position++;
    uint32_t value = 0;
    if (read_control_escape(parser, 1, &value)) {
      out->lo = value;
      out->length = parser->position - start;
      return GRX_OK;
    }
    if (unicode_mode(parser)) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_CONTROL_ESCAPE, start, 2);
    }
    // Annex B ClassAtomNoDash: the item is the backslash itself.
    parser->position = start + 1;
    out->lo = '\\';
    out->length = 1;
    return GRX_OK;
  }

  if (c == 'x') {
    parser->position++;
    uint32_t value = 0;
    if (read_fixed_hex(parser, 2, &value)) {
      out->lo = value;
      out->length = parser->position - start;
      return GRX_OK;
    }
    if (unicode_mode(parser)) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_HEX_ESCAPE, start, 2);
    }
    out->lo = 'x';
    out->length = 2;
    return GRX_OK;
  }

  if (c == 'u') {
    parser->position++;
    uint32_t value = 0;
    if (read_unicode_escape(parser, &value)) {
      out->lo = value;
      out->length = parser->position - start;
      return GRX_OK;
    }
    if (unicode_mode(parser)) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_UNICODE_ESCAPE, start, 2);
    }
    out->lo = 'u';
    out->length = 2;
    return GRX_OK;
  }

  if (is_octal(c) && !unicode_mode(parser)) {
    uint32_t value = read_legacy_octal(parser);
    out->lo = value;
    out->length = parser->position - start;
    return GRX_OK;
  }
  if (c == '0' && unicode_mode(parser)) {
    parser->position++;
    if (is_decimal(byte_at(parser, 0))) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_OCTAL_ESCAPE, start, 3);
    }
    out->lo = 0;
    out->length = 2;
    return GRX_OK;
  }

  uint32_t codepoint = 0;
  GRX_Result result = grx_parse_take(parser, &codepoint);
  if (result != GRX_OK) {
    return result;
  }
  // `\-` is the one identity escape Unicode mode adds inside a class, so
  // that a literal dash can be written where it would otherwise be a range.
  // UnicodeSets mode adds thirteen more: `ClassSetReservedPunctuator`, which
  // exists because those characters may not appear doubled and a writer
  // needs a way to say one of them without worrying which.
  if (unicode_mode(parser) && !is_syntax_character(codepoint)
      && codepoint != '/' && codepoint != '-'
      && !(unicode_sets_mode(parser) && is_reserved_punctuator(codepoint))) {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_ESCAPE, start, parser->position - start);
  }
  // Annex B's SourceCharacterIdentityEscape[+N] excludes `k` when the
  // pattern names a group anywhere, and that exclusion reaches *inside* a
  // class - where a named reference cannot appear at all. So `[\k]` is a
  // literal `k` in a pattern with no named group and a syntax error in
  // `[\k](?<n>)`. Found by differential fuzzing against Node; reasoning
  // about the grammar alone had missed it, because the rule is attached to
  // a production parameter rather than to the class.
  if (codepoint == 'k' && parser->named_groups) {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_ESCAPE, start, parser->position - start);
  }

  out->lo = codepoint;
  out->length = parser->position - start;
  return GRX_OK;
}

// --------------------------------------------------------------------------
// Character classes
// --------------------------------------------------------------------------

/** Read one class atom: an escape, or a single character. */
static GRX_Result read_class_atom(GRX_Parser * parser, GRX_ClassItem * out) {
  size_t start = parser->position;
  if (grx_parse_eat(parser, '\\')) {
    if (grx_parse_at_end(parser)) {
      return grx_parse_fail(parser, GRX_DIAG_TRAILING_BACKSLASH, start, 1);
    }
    return es_class_escape(parser, out);
  }

  uint32_t codepoint = 0;
  GRX_Result result = grx_parse_take(parser, &codepoint);
  if (result != GRX_OK) {
    return result;
  }

  *out = (GRX_ClassItem) {
    .kind = GRX_CLASS_ITEM_SINGLE,
    .flags = 0,
    .lo = codepoint,
    .hi = 0,
    .a = 0,
    .offset = start,
    .length = parser->position - start,
  };
  return GRX_OK;
}

// --------------------------------------------------------------------------
// UnicodeSets mode: the `v` class grammar
// --------------------------------------------------------------------------


static GRX_Result class_set_contents(GRX_Parser * parser, size_t start,
    uint32_t * out_node, int * out_strings);

/** Wrap an operand in a node, so that an operator's children are nodes. */
static GRX_Result operand_node(
    GRX_Parser * parser, const SetOperand * operand, uint32_t * out_node) {
  // A nested class is already a node, and an unnegated one can be used as it
  // stands rather than wrapped in a class of one item.
  if (operand->item.kind == GRX_CLASS_ITEM_NESTED
      && !(operand->item.flags & GRX_CLASS_ITEM_NEGATED)) {
    *out_node = operand->item.a;
    return GRX_OK;
  }

  GRX_Result result
      = grx_parse_class_node(parser, operand->item.offset, out_node);
  if (result != GRX_OK) {
    return result;
  }
  return grx_parse_class_add(parser, *out_node, &operand->item);
}

/**
 * `\q{abc|de|}`: a disjunction of literal strings.
 *
 * Every alternative is a member of the set, including the empty one, and a
 * member may be any number of code points. An alternative of exactly one is
 * an ordinary character as far as everything downstream is concerned, which
 * is why `[^\q{a}]` is legal and `[^\q{ab}]` is not.
 */
static GRX_Result class_string_disjunction(
    GRX_Parser * parser, size_t start, SetOperand * out) {
  if (!grx_parse_eat(parser, '{')) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_ITEM, start,
        parser->position - start);
  }

  uint32_t node = GRX_INDEX_NONE;
  GRX_Result result = grx_pattern_add_node(parser->pattern,
      GRX_NODE_STRING_SET, start, 0, &node);
  if (result != GRX_OK) {
    return result;
  }

  uint32_t first = GRX_INDEX_NONE;
  uint32_t count = 0;
  int strings = 0;

  for (;;) {
    uint32_t index = GRX_INDEX_NONE;
    result = grx_pattern_string_begin(parser->pattern, &index);
    if (result != GRX_OK) {
      return result;
    }
    if (first == GRX_INDEX_NONE) {
      first = index;
    }
    count++;

    size_t length = 0;
    while (!grx_parse_at_end(parser) && byte_at(parser, 0) != '|'
        && byte_at(parser, 0) != '}') {
      GRX_ClassItem character;
      result = read_class_atom(parser, &character);
      if (result != GRX_OK) {
        return result;
      }
      if (character.kind != GRX_CLASS_ITEM_SINGLE) {
        // `\q{\d}` and kin: the contents are *characters*, not sets.
        return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_ITEM,
            character.offset, character.length);
      }
      result = grx_pattern_string_push(parser->pattern, index, character.lo);
      if (result != GRX_OK) {
        return result;
      }
      length++;
    }

    if (length != 1) {
      strings = 1;
    }
    if (grx_parse_eat(parser, '|')) {
      continue;
    }
    break;
  }

  if (!grx_parse_eat(parser, '}')) {
    return grx_parse_fail(
        parser, GRX_DIAG_UNMATCHED_OPEN_BRACE, start, parser->position - start);
  }

  GRX_Node * set = grx_pattern_node(parser->pattern, node);
  if (!set) {
    return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 0);
  }
  set->a = first;
  set->b = count;
  set->length = parser->position - start;

  *out = (SetOperand) {
    .item = {
      .kind = GRX_CLASS_ITEM_NESTED,
      .flags = 0,
      .lo = 0,
      .hi = 0,
      .a = node,
      .offset = start,
      .length = parser->position - start,
    },
    .is_character = 0,
    .may_contain_strings = strings,
  };
  return GRX_OK;
}

/**
 * `\p{RGI_Emoji}` and its six siblings, when the name is a property of
 * strings rather than of code points.
 *
 * @return GRX_OK when it was one, GRX_ERR_SYNTAX when the name is not a
 *   property of strings - in which case nothing was consumed beyond what the
 *   caller had already read, and the ordinary property path should run.
 */
static GRX_Result read_string_property(GRX_Parser * parser, int negated,
    size_t start, size_t name_start, size_t name_length, SetOperand * out) {
  uint32_t set = 0;
  if (grx_unicode_string_set_lookup(
          parser->text + name_start, name_length, &set)
      != GRX_OK) {
    return GRX_ERR_SYNTAX;
  }
  if (negated) {
    // `\P{RGI_Emoji}` has no meaning: the complement of a set of strings is
    // not a set of strings, and ECMA-262 makes it an early error rather than
    // inventing one.
    return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_ITEM, start,
        parser->position - start);
  }

  uint32_t offset = GRX_INDEX_NONE;
  GRX_Result result = grx_pattern_add_name(
      parser->pattern, parser->text + name_start, name_length, &offset);
  if (result != GRX_OK) {
    return result;
  }

  *out = (SetOperand) {
    .item = {
      .kind = GRX_CLASS_ITEM_STRING_PROPERTY,
      .flags = 0,
      .lo = 0,
      .hi = 0,
      .a = offset,
      .offset = start,
      .length = parser->position - start,
    },
    .is_character = 0,
    .may_contain_strings = 1,
  };
  return GRX_OK;
}

/**
 * Read `\p{name}` and offer the name to the string tables.
 *
 * The `\p` has not been consumed on entry; on GRX_ERR_SYNTAX the position is
 * put back so the caller can read it as an ordinary property.
 */
static GRX_Result string_property_atom(
    GRX_Parser * parser, int negated, size_t start, SetOperand * out) {
  size_t at = parser->position;
  parser->position++; // The `p` or `P`.
  if (byte_at(parser, 0) == '{') {
    parser->position++;
    size_t name_start = parser->position;
    while (!grx_parse_at_end(parser) && byte_at(parser, 0) != '}') {
      parser->position++;
    }
    if (byte_at(parser, 0) == '}') {
      size_t name_length = parser->position - name_start;
      parser->position++;
      GRX_Result result = read_string_property(
          parser, negated, start, name_start, name_length, out);
      if (result != GRX_ERR_SYNTAX) {
        return result;
      }
    }
  }
  parser->position = at;
  return GRX_ERR_SYNTAX;
}

/** One `ClassSetCharacter`, or one escape that is a set rather than a char. */
static GRX_Result class_set_atom(GRX_Parser * parser, SetOperand * out) {
  size_t start = parser->position;

  if (grx_parse_eat(parser, '\\')) {
    if (grx_parse_at_end(parser)) {
      return grx_parse_fail(parser, GRX_DIAG_TRAILING_BACKSLASH, start, 1);
    }

    // `\q` is the one escape that exists only here.
    if (byte_at(parser, 0) == 'q') {
      parser->position++;
      return class_string_disjunction(parser, start, out);
    }

    // A property name is read once and offered to both tables, so that
    // `\p{RGI_Emoji}` and `\p{Lu}` are told apart by what they name rather
    // than by a list of names kept in the parser.
    if (byte_at(parser, 0) == 'p' || byte_at(parser, 0) == 'P') {
      GRX_Result result = string_property_atom(
          parser, byte_at(parser, 0) == 'P', start, out);
      if (result != GRX_ERR_SYNTAX) {
        return result;
      }
    }

    GRX_ClassItem item;
    GRX_Result result = es_class_escape(parser, &item);
    if (result != GRX_OK) {
      return result;
    }
    *out = (SetOperand) {
      .item = item,
      .is_character = item.kind == GRX_CLASS_ITEM_SINGLE,
      .may_contain_strings = 0,
    };
    return GRX_OK;
  }

  char c = byte_at(parser, 0);
  if (c && is_reserved_double(c) && byte_at(parser, 1) == c) {
    // `&&` and `--` are operators and are consumed by the caller; the other
    // twelve pairs are reserved, and a pattern that wants two of them writes
    // them escaped.
    return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_ITEM, start, 2);
  }

  uint32_t codepoint = 0;
  GRX_Result result = grx_parse_take(parser, &codepoint);
  if (result != GRX_OK) {
    return result;
  }
  if (is_class_set_syntax_character(codepoint)) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_ITEM, start,
        parser->position - start);
  }

  *out = (SetOperand) {
    .item = {
      .kind = GRX_CLASS_ITEM_SINGLE,
      .flags = 0,
      .lo = codepoint,
      .hi = 0,
      .a = 0,
      .offset = start,
      .length = parser->position - start,
    },
    .is_character = 1,
    .may_contain_strings = 0,
  };
  return GRX_OK;
}

/** One `ClassSetOperand`: a nested class, a string disjunction, or a char. */
static GRX_Result class_set_operand(GRX_Parser * parser, SetOperand * out) {
  size_t start = parser->position;

  if (grx_parse_eat(parser, '[')) {
    uint32_t node = GRX_INDEX_NONE;
    int strings = 0;
    GRX_Result result = class_set_contents(parser, start, &node, &strings);
    if (result != GRX_OK) {
      return result;
    }
    *out = (SetOperand) {
      .item = {
        .kind = GRX_CLASS_ITEM_NESTED,
        .flags = 0,
        .lo = 0,
        .hi = 0,
        .a = node,
        .offset = start,
        .length = parser->position - start,
      },
      .is_character = 0,
      .may_contain_strings = strings,
    };
    return GRX_OK;
  }

  return class_set_atom(parser, out);
}

/** Whether the next two bytes are this operator. */
static int at_operator(const GRX_Parser * parser, char c) {
  return byte_at(parser, 0) == c && byte_at(parser, 1) == c;
}

/**
 * `ClassIntersection` or `ClassSubtraction`: two or more operands joined by
 * one repeated operator.
 *
 * The operator may not change part-way and may not be mixed with union, so
 * `[a&&b--c]` and `[[a][b]&&[b]]` are both syntax errors. That is a rule
 * about *this* production rather than about precedence: ECMA-262 gives the
 * operators no precedence at all, precisely so that a reader never has to
 * work one out.
 */
static GRX_Result class_set_operator_list(GRX_Parser * parser, size_t start,
    char operator_char, const SetOperand * first, uint32_t * out_node,
    int * out_strings) {
  uint32_t node = GRX_INDEX_NONE;
  GRX_Result result = grx_pattern_add_node(
      parser->pattern, GRX_NODE_CLASS_OP, start, 0, &node);
  if (result != GRX_OK) {
    return result;
  }
  grx_pattern_node(parser->pattern, node)->a = operator_char == '&'
      ? (uint32_t)GRX_CLASS_OP_INTERSECT
      : (uint32_t)GRX_CLASS_OP_SUBTRACT;

  uint32_t child = GRX_INDEX_NONE;
  result = operand_node(parser, first, &child);
  if (result == GRX_OK) {
    result = grx_pattern_add_child(parser->pattern, node, child);
  }
  if (result != GRX_OK) {
    return result;
  }

  // Intersection keeps strings only where every operand has them;
  // subtraction keeps the left operand's, because subtracting cannot add.
  int strings = first->may_contain_strings;

  while (at_operator(parser, operator_char)) {
    parser->position += 2;
    SetOperand operand;
    result = class_set_operand(parser, &operand);
    if (result != GRX_OK) {
      return result;
    }
    if (operator_char == '&') {
      strings = strings && operand.may_contain_strings;
    }

    result = operand_node(parser, &operand, &child);
    if (result == GRX_OK) {
      result = grx_pattern_add_child(parser->pattern, node, child);
    }
    if (result != GRX_OK) {
      return result;
    }
  }

  if (byte_at(parser, 0) != ']') {
    // Something followed the last operand that was neither the operator nor
    // the close: a union mixed into an operator expression.
    return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_SET_OP,
        parser->position, 1);
  }

  grx_pattern_node(parser->pattern, node)->length = parser->position - start;
  *out_node = node;
  *out_strings = strings;
  return GRX_OK;
}

/**
 * The contents of one `[...]` in `v` mode, up to but not including the `]`.
 *
 * `start` is the offset of the `[`, which has already been consumed.
 */
static GRX_Result class_set_contents(GRX_Parser * parser, size_t start,
    uint32_t * out_node, int * out_strings) {
  // A nested class is nesting, and the C stack this walks down is the same
  // one a group nests on, so it is bounded by the same limit.
  if (parser->limits->max_nesting_depth
      && parser->depth + 1 > parser->limits->max_nesting_depth) {
    return grx_parse_fail(
        parser, GRX_DIAG_LIMIT_NESTING_DEPTH, start, 1);
  }
  parser->depth++;

  GRX_Result result = GRX_OK;
  int negated = grx_parse_eat(parser, '^');
  int strings = 0;
  uint32_t node = GRX_INDEX_NONE;

  if (byte_at(parser, 0) == ']') {
    // `[]` is empty and `[^]` is everything, as in `u` mode.
    result = grx_parse_class_node(parser, start, &node);
    if (result != GRX_OK) {
      goto done;
    }
  }
  else {
    SetOperand first;
    result = class_set_operand(parser, &first);
    if (result != GRX_OK) {
      goto done;
    }

    if (at_operator(parser, '&') || at_operator(parser, '-')) {
      char operator_char = byte_at(parser, 0);
      result = class_set_operator_list(
          parser, start, operator_char, &first, &node, &strings);
      if (result != GRX_OK) {
        goto done;
      }
    }
    else {
      // A union. Its operands are a mixture of things that fit in a class
      // item - characters, ranges, `\d`, `\p{...}` - and things that are
      // whole nodes: a nested class, a string disjunction, a property of
      // strings. The two cannot share one node, because a class names a
      // *span* of the item table and a nested class parsed between two items
      // would land its own items in the middle of that span.
      //
      // So consecutive simple items are batched into one class node and a
      // node operand ends the batch. A union of nothing but simple items -
      // which is `[abc]` and `[a-z]`, the overwhelming majority - is one
      // class node and no operator at all.
      uint32_t single = GRX_INDEX_NONE;
      uint32_t op_node = GRX_INDEX_NONE;
      uint32_t pending = GRX_INDEX_NONE;
      size_t expected = 0;

      SetOperand operand = first;
      for (;;) {
        // A range, when both ends are characters and a lone `-` separates
        // them. `--` was ruled out above and is ruled out again here,
        // because a union may not turn into a subtraction part-way.
        if (operand.is_character && byte_at(parser, 0) == '-'
            && byte_at(parser, 1) != '-' && byte_at(parser, 1) != ']'
            && byte_at(parser, 1) != '\0') {
          size_t dash = parser->position;
          parser->position++;

          SetOperand high;
          result = class_set_operand(parser, &high);
          if (result != GRX_OK) {
            goto done;
          }
          if (!high.is_character) {
            result = grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_RANGE,
                dash, parser->position - dash);
            goto done;
          }
          if (high.item.lo < operand.item.lo) {
            result = grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_RANGE,
                operand.item.offset, parser->position - operand.item.offset);
            goto done;
          }
          operand.item.kind = GRX_CLASS_ITEM_RANGE;
          operand.item.hi = high.item.lo;
          operand.item.length = parser->position - operand.item.offset;
          operand.is_character = 0;
        }

        if (operand.may_contain_strings) {
          strings = 1;
        }

        uint32_t child = GRX_INDEX_NONE;
        if (operand.item.kind == GRX_CLASS_ITEM_NESTED) {
          child = operand.item.a;
          pending = GRX_INDEX_NONE;
        }
        else if (pending != GRX_INDEX_NONE
            && expected == parser->pattern->class_items.count) {
          result = grx_parse_class_add(parser, pending, &operand.item);
          if (result != GRX_OK) {
            goto done;
          }
          expected = parser->pattern->class_items.count;
        }
        else {
          result = grx_parse_class_node(
              parser, operand.item.offset, &pending);
          if (result == GRX_OK) {
            result = grx_parse_class_add(parser, pending, &operand.item);
          }
          if (result != GRX_OK) {
            goto done;
          }
          expected = parser->pattern->class_items.count;
          child = pending;
        }

        if (child != GRX_INDEX_NONE) {
          if (single == GRX_INDEX_NONE && op_node == GRX_INDEX_NONE) {
            single = child;
          }
          else {
            if (op_node == GRX_INDEX_NONE) {
              result = grx_pattern_add_node(
                  parser->pattern, GRX_NODE_CLASS_OP, start, 0, &op_node);
              if (result == GRX_OK) {
                grx_pattern_node(parser->pattern, op_node)->a
                    = (uint32_t)GRX_CLASS_OP_UNION;
                result = grx_pattern_add_child(
                    parser->pattern, op_node, single);
              }
              if (result != GRX_OK) {
                goto done;
              }
            }
            result = grx_pattern_add_child(parser->pattern, op_node, child);
            if (result != GRX_OK) {
              goto done;
            }
          }
        }

        if (byte_at(parser, 0) == ']' || grx_parse_at_end(parser)) {
          break;
        }
        if (at_operator(parser, '&') || at_operator(parser, '-')) {
          result = grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_SET_OP,
              parser->position, 2);
          goto done;
        }
        result = class_set_operand(parser, &operand);
        if (result != GRX_OK) {
          goto done;
        }
      }

      node = op_node != GRX_INDEX_NONE ? op_node : single;
    }
  }

  if (!grx_parse_eat(parser, ']')) {
    result = grx_parse_fail(
        parser, GRX_DIAG_UNMATCHED_OPEN_BRACKET, start, 1);
    goto done;
  }

  if (negated) {
    if (strings) {
      // ECMA-262 makes this an early error rather than deciding what the
      // complement of a set containing "abc" would be. There is no good
      // answer: the complement of a set of strings is not a set of strings.
      result = grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_ITEM, start,
          parser->position - start);
      goto done;
    }
    grx_pattern_node(parser->pattern, node)->flags |= GRX_NODE_NEGATED;
  }

  grx_pattern_node(parser->pattern, node)->length = parser->position - start;
  *out_node = node;
  *out_strings = strings;

done:
  parser->depth--;
  return result;
}

static GRX_Result es_char_class(GRX_Parser * parser, uint32_t * out_node) {
  size_t start = parser->position - 1; // The `[`.

  if (unicode_sets_mode(parser)) {
    int strings = 0;
    return class_set_contents(parser, start, out_node, &strings);
  }

  GRX_Result result = grx_parse_class_node(parser, start, out_node);
  if (result != GRX_OK) {
    return result;
  }
  if (grx_parse_eat(parser, '^')) {
    grx_pattern_node(parser->pattern, *out_node)->flags |= GRX_NODE_NEGATED;
  }

  // `[]` is the empty class and `[^]` is everything: ECMAScript is the only
  // tier-1 dialect where a leading `]` closes rather than being a literal.
  while (!grx_parse_at_end(parser) && byte_at(parser, 0) != ']') {
    GRX_ClassItem low;
    result = read_class_atom(parser, &low);
    if (result != GRX_OK) {
      return result;
    }

    // A `-` that is not the last character before `]` begins a range.
    if (byte_at(parser, 0) == '-' && byte_at(parser, 1) != ']'
        && byte_at(parser, 1) != '\0') {
      size_t dash = parser->position;
      parser->position++;

      GRX_ClassItem high;
      result = read_class_atom(parser, &high);
      if (result != GRX_OK) {
        return result;
      }

      if (low.kind != GRX_CLASS_ITEM_SINGLE
          || high.kind != GRX_CLASS_ITEM_SINGLE) {
        if (unicode_mode(parser)) {
          return grx_parse_fail(parser, GRX_DIAG_CLASS_ESCAPE_IN_RANGE,
              low.offset, parser->position - low.offset);
        }
        // Annex B NonemptyClassRangesNoDash: a class escape at either end
        // makes this a union of the two items and a literal `-`, not a
        // range, so `[\d-z]` is `\d`, `-` and `z`.
        GRX_ClassItem dash_item = {
          .kind = GRX_CLASS_ITEM_SINGLE,
          .flags = 0,
          .lo = '-',
          .hi = 0,
          .a = 0,
          .offset = dash,
          .length = 1,
        };
        if ((result = grx_parse_class_add(parser, *out_node, &low)) != GRX_OK
            || (result = grx_parse_class_add(parser, *out_node, &dash_item))
                != GRX_OK
            || (result = grx_parse_class_add(parser, *out_node, &high))
                != GRX_OK) {
          return result;
        }
        continue;
      }

      if (low.lo > high.lo) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_RANGE, low.offset,
            parser->position - low.offset);
      }

      low.kind = GRX_CLASS_ITEM_RANGE;
      low.hi = high.lo;
      low.length = parser->position - low.offset;
    }

    result = grx_parse_class_add(parser, *out_node, &low);
    if (result != GRX_OK) {
      return result;
    }
  }

  if (!grx_parse_eat(parser, ']')) {
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_BRACKET, start, 1);
  }

  grx_pattern_node(parser->pattern, *out_node)->length
      = parser->position - start;
  return GRX_OK;
}

// --------------------------------------------------------------------------
// Groups
// --------------------------------------------------------------------------

static GRX_Result es_group_open(GRX_Parser * parser, GRX_GroupOpen * out) {
  size_t start = parser->position - 1; // The `(`.

  *out = (GRX_GroupOpen) {
    .kind = GRX_NODE_GROUP,
    .flags = 0,
    .a = 0,
    .b = GRX_INDEX_NONE,
    .has_body = 1,
  };

  if (!grx_parse_eat(parser, '?')) {
    if (parser->options & GRX_OPT_NO_CAPTURE) {
      return GRX_OK;
    }
    parser->groups_opened++;
    out->flags = GRX_NODE_CAPTURING;
    out->a = (uint32_t)parser->groups_opened;
    return GRX_OK;
  }

  char c = byte_at(parser, 0);
  if (c == ':') {
    parser->position++;
    return GRX_OK;
  }
  if (c == '=' || c == '!') {
    parser->position++;
    out->kind = GRX_NODE_LOOKAROUND;
    out->a = c == '=' ? GRX_LOOK_AHEAD_POSITIVE : GRX_LOOK_AHEAD_NEGATIVE;
    return GRX_OK;
  }
  if (c == '<' && (byte_at(parser, 1) == '=' || byte_at(parser, 1) == '!')) {
    int positive = byte_at(parser, 1) == '=';
    parser->position += 2;
    out->kind = GRX_NODE_LOOKAROUND;
    out->a = positive ? GRX_LOOK_BEHIND_POSITIVE : GRX_LOOK_BEHIND_NEGATIVE;
    return GRX_OK;
  }
  if (c == '<') {
    parser->position++;
    char name[GRX_ES_NAME_MAX];
    size_t name_length = 0;
    GRX_Result result
        = read_group_name(parser, '>', name, sizeof(name), &name_length);
    if (result != GRX_OK) {
      return result;
    }
    // Node 22 rejects a duplicate name even across alternatives, so this
    // library does too: ES2025 allows it and the oracle does not yet, and
    // the oracle is what the conformance vectors come from.
    if (name_already_used(parser, name)) {
      return grx_parse_fail(parser, GRX_DIAG_DUPLICATE_GROUP_NAME, start,
          parser->position - start);
    }

    uint32_t offset = GRX_INDEX_NONE;
    result = grx_pattern_add_name(parser->pattern, name, name_length, &offset);
    if (result != GRX_OK) {
      return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
    }

    parser->groups_opened++;
    out->flags = GRX_NODE_CAPTURING | GRX_NODE_NAMED;
    out->a = (uint32_t)parser->groups_opened;
    out->b = offset;
    return GRX_OK;
  }

  // Everything else that a Perl-family dialect spells with `(?`: a comment
  // group, an atomic group, a conditional, a named group written `(?P<n>` or
  // `(?'n'`, and the ES2025 modifier `(?i:...)`. ECMAScript has none of
  // them - checked against Node 22, which rejects the modifiers as well,
  // resolving the **probe** in dialects.md section 8.5.
  return grx_parse_fail(
      parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start, parser->position - start);
}

// --------------------------------------------------------------------------
// Quantifiers and literals
// --------------------------------------------------------------------------

/**
 * Read a `{m,n}` quantifier, the `{` already consumed.
 *
 * Annex B makes a `{` that does not begin a valid quantifier an ordinary
 * character, so `a{`, `a{1` and `a{,3}` are literal sequences; Unicode mode
 * makes each of them a syntax error.
 */
static GRX_Result es_brace_quantifier(
    GRX_Parser * parser, GRX_Quantifier * out) {
  size_t start = parser->position - 1;
  out->is_quantifier = 0;
  out->min = 0;
  out->max = GRX_REPEAT_INF;

  size_t scan = 0;
  uint64_t min = 0;
  size_t min_digits = 0;
  while (is_decimal(byte_at(parser, scan)) && min < 0xFFFFFFFFu) {
    min = min * 10 + (uint64_t)(byte_at(parser, scan) - '0');
    scan++;
    min_digits++;
  }

  uint64_t max = min;
  int unbounded = 0;
  if (min_digits && byte_at(parser, scan) == ',') {
    scan++;
    if (is_decimal(byte_at(parser, scan))) {
      max = 0;
      while (is_decimal(byte_at(parser, scan)) && max < 0xFFFFFFFFu) {
        max = max * 10 + (uint64_t)(byte_at(parser, scan) - '0');
        scan++;
      }
    }
    else {
      unbounded = 1;
    }
  }

  if (!min_digits || byte_at(parser, scan) != '}') {
    if (unicode_mode(parser)) {
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_QUANTIFIER, start, scan + 1);
    }
    return GRX_OK; // The caller rewinds and reads `{` as a literal.
  }

  parser->position += scan + 1;
  out->is_quantifier = 1;
  out->min = (uint32_t)min;
  out->max = unbounded ? GRX_REPEAT_INF : (uint32_t)max;
  return GRX_OK;
}

static GRX_Result es_literal_atom(GRX_Parser * parser, uint32_t codepoint,
    size_t offset, size_t length, uint32_t * out_node) {
  if (codepoint == '{' || codepoint == '}' || codepoint == ']') {
    if (unicode_mode(parser)) {
      return grx_parse_fail(
          parser, GRX_DIAG_UNESCAPED_METACHARACTER, offset, length);
    }
    if (codepoint == '{') {
      // Annex B keeps `{` as a literal only when it does not begin a valid
      // quantifier. `/{2,3}/` is a SyntaxError in Node even without `u`,
      // because the brace does begin one and there is nothing to repeat.
      size_t resume = parser->position;
      GRX_Quantifier probe = {0, GRX_REPEAT_INF, 0, GRX_REPEAT_GREEDY};
      GRX_Result result = es_brace_quantifier(parser, &probe);
      parser->position = resume;
      if (result != GRX_OK) {
        return result;
      }
      if (probe.is_quantifier) {
        return grx_parse_fail(
            parser, GRX_DIAG_NOTHING_TO_REPEAT, offset, length);
      }
    }
  }

  return grx_parse_literal_node(parser, codepoint, offset, length, out_node);
}

// --------------------------------------------------------------------------
// Whole-pattern validation
// --------------------------------------------------------------------------

/**
 * Check what only the finished pattern can show.
 *
 * One rule: every `\k<name>` must name a group the pattern has. It cannot be
 * checked where the reference is read, because ECMAScript allows the group to
 * come afterwards - `/\k<a>(?<a>x)/` is valid.
 */
static GRX_Result es_validate(GRX_Parser * parser) {
  for (size_t i = 0; i < parser->pattern->nodes.count; i++) {
    const GRX_Node * node = grx_pattern_node(parser->pattern, (uint32_t)i);
    if (!node || node->kind != GRX_NODE_BACKREF
        || !(node->flags & GRX_NODE_NAMED)) {
      continue;
    }

    const char * wanted = grx_pattern_name(parser->pattern, node->b);
    if (!wanted) {
      return grx_parse_fail(parser, GRX_DIAG_INTERNAL, node->offset,
          node->length);
    }

    int found = 0;
    for (size_t j = 0; j < parser->pattern->nodes.count && !found; j++) {
      const GRX_Node * group = grx_pattern_node(parser->pattern, (uint32_t)j);
      if (!group || group->kind != GRX_NODE_GROUP
          || !(group->flags & GRX_NODE_NAMED)) {
        continue;
      }
      const char * name = grx_pattern_name(parser->pattern, group->b);
      found = name && strcmp(name, wanted) == 0;
    }
    if (!found) {
      return grx_parse_fail(parser, GRX_DIAG_UNKNOWN_GROUP_NAME, node->offset,
          node->length);
    }
  }

  return GRX_OK;
}

/**
 * Refuse the quantifiers ECMAScript refuses.
 *
 * Annex B's ExtendedTerm lets a *lookahead* be quantified, so `(?=a)*` is
 * valid without `u` (and matches the empty string, since the assertion
 * consumes nothing). Unicode mode removes that relaxation, and a lookbehind
 * is never quantifiable in either mode. Checked against Node 22 in all four
 * combinations; an earlier draft of this file allowed all four, which is one
 * of the three things the oracle corrected.
 */
static GRX_Result es_check_quantifier_target(
    GRX_Parser * parser, uint32_t node_index, size_t offset, size_t length) {
  const GRX_Node * node = grx_pattern_node(parser->pattern, node_index);
  if (!node) {
    return GRX_OK;
  }

  if (node->kind == GRX_NODE_ANCHOR) {
    return grx_parse_fail(
        parser, GRX_DIAG_QUANTIFIED_ASSERTION, offset, length);
  }
  if (node->kind != GRX_NODE_LOOKAROUND) {
    return GRX_OK;
  }

  int behind = node->a == GRX_LOOK_BEHIND_POSITIVE
      || node->a == GRX_LOOK_BEHIND_NEGATIVE;
  if (behind || unicode_mode(parser)) {
    return grx_parse_fail(
        parser, GRX_DIAG_QUANTIFIED_ASSERTION, offset, length);
  }

  return GRX_OK;
}

const GRX_Frontend grx_frontend_ecmascript = {
  .name = "ecmascript",
  .atom_escape = es_atom_escape,
  .class_escape = es_class_escape,
  .char_class = es_char_class,
  .group_open = es_group_open,
  .brace_quantifier = es_brace_quantifier,
  .literal_atom = es_literal_atom,
  .check_quantifier_target = es_check_quantifier_target,
  .validate = es_validate,
};
