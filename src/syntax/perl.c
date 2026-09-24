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
 * The Perl-family front end: how PCRE2 and Perl spell what they have.
 *
 * One file for two dialects, because they are one grammar with a short list
 * of differences (documentation/dialects.md section 9). Where they differ the
 * code asks flavour() rather than switching on GRX_Syntax at each use, so
 * that the list of differences is something a reader can count.
 *
 * The reference is pcre2pattern for PCRE2 10.46 and perlre for Perl 5.40, and
 * the oracle for every rule below is the installed pcre2test 10.46 - which
 * corrected several of them, each marked where it did.
 *
 * What this front end does *not* read is as much a decision as what it does.
 * A construct PCRE2 has and this library does not implement is
 * GRX_ERR_UNSUPPORTED with its own diagnostic, never a silent approximation:
 * `\X` is a grapheme cluster and not "any character", and accepting it as
 * something close would tell a caller their pattern means what it does not.
 * `(*script_run:...)` was on that list until it was built; what replaced it
 * is a node flag and a check on the text the body matched, because reading
 * it as an ordinary group is exactly the silent approximation this
 * paragraph refuses.
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/pattern.h>
#include <ghoti.io/regex/syntax.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../core/core_internal.h"
#include "../unicode/unicode_internal.h"
#include "../parse/parse_internal.h"
#include "../unicode/unicode_internal.h"

/** Which of the three dialects this parse is reading. */
typedef enum {
  FLAVOUR_PCRE = 0, ///< PCRE2 10.46.
  FLAVOUR_PERL,     ///< Perl 5.40.
  FLAVOUR_PYTHON    ///< CPython 3.13 `re`.
} Flavour;

/**
 * Which dialect this parse is reading.
 *
 * Switched over rather than tested, and the default aborts the
 * question rather than answering it. The earlier form was
 * `syntax == PERL ? PERL : PCRE`, which was correct while this file served
 * two dialects and became a trap the moment it served three: a dialect
 * routed here without a case would have been *parsed as PCRE2* and told its
 * caller the pattern was valid, which is the silent approximation the file's
 * own header refuses. GRX_SYNTAX_PYTHON was in the enum for the whole of
 * that time.
 */
static Flavour flavour(const GRX_Parser * parser) {
  switch (parser->syntax) {
    case GRX_SYNTAX_PERL:
      return FLAVOUR_PERL;
    case GRX_SYNTAX_PYTHON:
      return FLAVOUR_PYTHON;
    case GRX_SYNTAX_PCRE:
    default:
      return FLAVOUR_PCRE;
  }
}

/**
 * Whether a reference to this group is one Python would refuse.
 *
 * `re` requires the group to have *closed*: `(a\1)`, `((a)\1)`, `(a|\1)`
 * and `(?P<x>(?P<y>a)(?P=x))` are all "cannot refer to an open group", and
 * a reference to a group numbered later is "invalid group reference".
 * Perl and PCRE2 allow both - a forward reference simply fails to match
 * there - so this is the one rule that makes GRX_DIAG_FORWARD_BACKREFERENCE
 * reachable, a diagnostic that until now no dialect here produced.
 *
 * @param parser The parser.
 * @param group The capture number being referenced.
 * @return Non-zero when the reference must be refused.
 */
static int python_reference_is_premature(
    const GRX_Parser * parser, uint32_t group) {
  if (flavour(parser) != FLAVOUR_PYTHON) {
    return 0;
  }
  if (group > parser->groups_opened) {
    return 1;
  }
  for (const GRX_OpenGroup * open = parser->open_groups; open;
      open = open->outer) {
    if (open->number == group) {
      return 1;
    }
  }
  return 0;
}

/** The longest group name this front end will accept, in bytes. */
#define GRX_PCRE_NAME_MAX 128

/** The largest callout number PCRE2 takes: `(?C256)` is its error 138. */
#define GRX_PCRE_CALLOUT_MAX 255

static GRX_Result pcre_skip_ignorable(GRX_Parser * parser);

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

/** A name character: PCRE2's names are `[A-Za-z_][A-Za-z0-9_]*`. */
static int is_name_start(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int is_name_char(char c) { return is_name_start(c) || is_decimal(c); }

/** Whether `text` continues with `word` at the current position. */
static int looking_at(const GRX_Parser * parser, const char * word) {
  size_t length = strlen(word);
  if (parser->position + length > parser->length) {
    return 0;
  }

  return memcmp(parser->text + parser->position, word, length) == 0;
}

// --------------------------------------------------------------------------
// What an escape turned out to be
// --------------------------------------------------------------------------

/**
 * The escapes of this family, as a small vocabulary.
 *
 * One reader serves both contexts - an atom and a class item - because the
 * escapes are the same list in both and only a handful behave differently:
 * `\b` is a word boundary outside a class and a backspace inside one, and
 * the anchors, `\R`, `\X` and `\K` are errors inside one. Reading the list
 * twice would be two places for a new escape to be forgotten.
 */
typedef enum {
  ESC_LITERAL = 0, ///< One code point, in `codepoint`.
  ESC_SHORTHAND,   ///< `\d` and kin; `shorthand` and `negated`.
  ESC_PROPERTY,    ///< `\p{...}`; `name` and `negated`.
  ESC_BACKSPACE,   ///< `\b` inside a class.
  ESC_ANCHOR,      ///< `\b`, `\A`, `\z`, `\Z`, `\G`; `anchor`.
  ESC_BACKREF,     ///< `\1`, `\g{-1}`, `\k<name>`.
  ESC_SUBROUTINE,  ///< `\g<1>`, `\g'name'`.
  ESC_KEEP,        ///< `\K`.
  ESC_NEWLINE_SET, ///< `\R`.
  ESC_GRAPHEME,    ///< `\X`.
  ESC_NOT_NEWLINE  ///< `\N` with no `{`.
} EscapeKind;

/** What read_escape() found. */
typedef struct {
  EscapeKind kind;
  uint32_t codepoint;  ///< ESC_LITERAL.
  int shorthand;       ///< ESC_SHORTHAND: a GRX_ShorthandKind.
  int negated;         ///< ESC_SHORTHAND, ESC_PROPERTY.
  uint32_t name;       ///< ESC_PROPERTY, ESC_BACKREF, ESC_SUBROUTINE.
  uint32_t group;      ///< ESC_BACKREF, ESC_SUBROUTINE: a number, or 0.
  int relative;        ///< ESC_BACKREF, ESC_SUBROUTINE: `\g{-1}`.
  /**
   * ESC_SUBROUTINE: where the definition this call re-enters was written.
   *
   * GRX_NPOS when the number is all there is to go on. See
   * definition_offset(): one number can have several definitions and a
   * relative call means the nearest of them, not the first.
   */
  size_t definition;
  GRX_AnchorKind anchor; ///< ESC_ANCHOR.
  size_t offset;       ///< Where the backslash was.
  size_t length;       ///< Bytes the whole escape spans.
} Escape;

// --------------------------------------------------------------------------
// Numeric escapes
// --------------------------------------------------------------------------

/**
 * Read `\x`, the `x` already consumed.
 *
 * `\x{...}` is any number of hex digits; `\xHH` is *up to* two, so `\x` alone
 * is NUL and `\xg` is NUL followed by a literal `g`. That last rule is the
 * one that differs from ECMAScript, where the digits are required.
 */
static GRX_Result read_hex(GRX_Parser * parser, size_t start,
    uint32_t * out_value) {
  // Python has no braced form and no short one: `\xHH` takes *exactly* two
  // hex digits, and `\x{41}`, `\x4` and `\xg` are all "incomplete escape"
  // in `re`. Reading them the Perl family's way would accept three patterns
  // the reference refuses, and read `\x4` as U+0004 where `re` reads
  // nothing at all.
  if (flavour(parser) == FLAVOUR_PYTHON) {
    uint32_t value = 0;
    for (int digit = 0; digit < 2; digit++) {
      if (!is_hex(byte_at(parser, 0))) {
        return grx_parse_fail(
            parser, GRX_DIAG_INVALID_HEX_ESCAPE, start, digit + 2);
      }
      value = (value << 4) | hex_value(byte_at(parser, 0));
      parser->position++;
    }
    *out_value = value;
    return GRX_OK;
  }

  if (byte_at(parser, 0) == '{') {
    // Perl lets an underscore separate the digits, the way a numeric literal
    // does: `\x{_1_0000}` is U+10000. PCRE2 does not, and says "Malformed
    // \x{ escape" for the same pattern. Perl also reads `\x{_}` as zero,
    // which this still refuses: a digit is what the escape is for.
    int separators = flavour(parser) == FLAVOUR_PERL;
    size_t scan = 1;
    uint32_t value = 0;
    int digits = 0;
    for (;;) {
      if (separators && byte_at(parser, scan) == '_') {
        scan++;
        continue;
      }
      if (!is_hex(byte_at(parser, scan))) {
        break;
      }
      value = (value << 4) | hex_value(byte_at(parser, scan));
      if (value > GRX_CODEPOINT_MAX) {
        return grx_parse_fail(
            parser, GRX_DIAG_CODEPOINT_OUT_OF_RANGE, start, scan + 2);
      }
      digits++;
      scan++;
    }
    if (!digits || byte_at(parser, scan) != '}') {
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_HEX_ESCAPE, start, scan + 2);
    }
    parser->position += scan + 1;
    *out_value = value;
    return GRX_OK;
  }

  uint32_t value = 0;
  int digits = 0;
  for (; digits < 2 && is_hex(byte_at(parser, 0)); digits++) {
    value = (value << 4) | hex_value(byte_at(parser, 0));
    parser->position++;
  }
  if (!digits) {
    // pcre2test 10.46: "digits missing after \x". Older PCRE2 read `\x` with
    // no digits as NUL, and the corpus is what says which of the two this
    // library has to be.
    return grx_parse_fail(parser, GRX_DIAG_INVALID_HEX_ESCAPE, start, 2);
  }
  *out_value = value;
  return GRX_OK;
}

/** Read `\o{...}`, the `o` already consumed. */
static GRX_Result read_braced_octal(GRX_Parser * parser, size_t start,
    uint32_t * out_value) {
  if (byte_at(parser, 0) != '{') {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_OCTAL_ESCAPE, start, 2);
  }

  size_t scan = 1;
  uint64_t value = 0;
  int digits = 0;
  while (is_octal(byte_at(parser, scan))) {
    value = value * 8 + (uint64_t)(byte_at(parser, scan) - '0');
    if (value > GRX_CODEPOINT_MAX) {
      return grx_parse_fail(
          parser, GRX_DIAG_CODEPOINT_OUT_OF_RANGE, start, scan + 2);
    }
    digits++;
    scan++;
  }
  if (!digits || byte_at(parser, scan) != '}') {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_OCTAL_ESCAPE, start, scan + 2);
  }

  parser->position += scan + 1;
  *out_value = (uint32_t)value;
  return GRX_OK;
}

/** Read up to `allowed` octal digits from the current position. */
static uint32_t read_octal_digits(GRX_Parser * parser, size_t allowed) {
  uint32_t value = 0;
  for (size_t i = 0; i < allowed && is_octal(byte_at(parser, 0)); i++) {
    value = value * 8 + (uint32_t)(byte_at(parser, 0) - '0');
    parser->position++;
  }

  return value;
}

/**
 * Read `\cX`, the `c` already consumed.
 *
 * PCRE2 takes the character, upper-cases a lower-case letter, and XORs with
 * 0x40 - so `\cA` is 1, `\c{` is 0x3B, and only a value above 127 is an
 * error. That is wider than ECMAScript's letters-only rule, and `\c]` in the
 * corpus is why it is written out rather than borrowed.
 */
static GRX_Result read_control(GRX_Parser * parser, size_t start,
    uint32_t * out_value) {
  if (grx_parse_at_end(parser)) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_CONTROL_ESCAPE, start, 2);
  }

  unsigned char c = (unsigned char)byte_at(parser, 0);
  if (c > 127) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_CONTROL_ESCAPE, start, 3);
  }
  if (c >= 'a' && c <= 'z') {
    c = (unsigned char)(c - 'a' + 'A');
  }
  parser->position++;
  *out_value = (uint32_t)(c ^ 0x40u);
  return GRX_OK;
}

// --------------------------------------------------------------------------
// Names
// --------------------------------------------------------------------------

/**
 * Read a group name up to `terminator` and store it in the pattern.
 *
 * PCRE2's names are `[A-Za-z_][A-Za-z0-9_]*`. A name starting with a digit is
 * refused here rather than being read as a number, because `(?<1>a)` is an
 * error in PCRE2 and reading it as group 1 would accept a pattern the
 * reference rejects.
 */
static GRX_Result read_name(GRX_Parser * parser, char terminator,
    size_t start, uint32_t * out_offset) {
  // `\k{ name }` and `\g{ 1 }`: both references allow space around what is
  // inside the braces, and pcre2test accepts them.
  if (terminator == '}') {
    while (byte_at(parser, 0) == ' ' || byte_at(parser, 0) == '\t') {
      parser->position++;
    }
  }

  size_t first = parser->position;
  if (!is_name_start(byte_at(parser, 0))) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_NAME, start,
        parser->position - start + 1);
  }

  while (is_name_char(byte_at(parser, 0))) {
    parser->position++;
  }
  size_t length = parser->position - first;
  if (terminator == '}') {
    while (byte_at(parser, 0) == ' ' || byte_at(parser, 0) == '\t') {
      parser->position++;
    }
  }
  if (length > GRX_PCRE_NAME_MAX) {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_GROUP_NAME, start, parser->position - start);
  }
  if (!grx_parse_eat(parser, terminator)) {
    return grx_parse_fail(
        parser, GRX_DIAG_UNTERMINATED_NAME, start, parser->position - start);
  }

  GRX_Result result = grx_pattern_add_name(
      parser->pattern, parser->text + first, length, out_offset);
  if (result != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }

  return GRX_OK;
}

/**
 * The capture number of the group carrying this name, if one exists yet.
 *
 * A group's node is created when its `(` is read, before its body, so this
 * finds a group that is still *open* as well as one that has closed. That is
 * what the caller needs: "not found" means the name belongs to a group later
 * in the pattern, and found-but-open is the other half of Python's rule.
 *
 * @param parser The parser.
 * @param offset The name, as an offset into the pattern's name storage.
 * @param out_number Receives the capture number.
 * @return Non-zero when a group with that name has been reached.
 */
static int group_number_for_name(
    const GRX_Parser * parser, uint32_t offset, uint32_t * out_number) {
  const char * name = grx_pattern_name(parser->pattern, offset);
  if (!name) {
    return 0;
  }

  for (size_t i = 0; i < parser->pattern->nodes.count; i++) {
    const GRX_Node * node = grx_pattern_node(parser->pattern, (uint32_t)i);
    if (!node || node->kind != GRX_NODE_GROUP
        || !(node->flags & GRX_NODE_NAMED)
        || !(node->flags & GRX_NODE_CAPTURING)) {
      continue;
    }
    const char * existing = grx_pattern_name(parser->pattern, node->b);
    if (existing && strcmp(existing, name) == 0) {
      *out_number = node->a;
      return 1;
    }
  }

  return 0;
}

/** Whether a capturing group with this name has already been parsed. */
static int name_already_used(const GRX_Parser * parser, uint32_t offset) {
  const char * name = grx_pattern_name(parser->pattern, offset);
  if (!name) {
    return 0;
  }

  for (size_t i = 0; i < parser->pattern->nodes.count; i++) {
    const GRX_Node * node = grx_pattern_node(parser->pattern, (uint32_t)i);
    if (!node || node->kind != GRX_NODE_GROUP
        || !(node->flags & GRX_NODE_NAMED) || node->b == offset) {
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
// The escape table, shared by both contexts
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
    case 'h': return GRX_SHORTHAND_HSPACE;
    case 'H': *out_negated = 1; return GRX_SHORTHAND_HSPACE;
    case 'v': return GRX_SHORTHAND_VSPACE;
    case 'V': *out_negated = 1; return GRX_SHORTHAND_VSPACE;
    default: return -1;
  }
}

/**
 * Whether CPython's `re` gives this letter any meaning after a backslash.
 *
 * Python's escape alphabet is *closed* and much shorter than the Perl
 * family's, and `re` reports "bad escape \\q" for every letter outside it
 * rather than reading it as the literal. Probed one letter at a time, both
 * inside a class and outside, rather than read off the `re` documentation -
 * which lists what the module has and does not say what it refuses.
 *
 * Three letters are the reason this is a table and not a shorter test.
 * `\v` is the vertical-tab *character* here and the vertical-space
 * *shorthand* in PCRE2, so routing it to the shared shorthand table would
 * silently widen it to five code points. `\Z` is the end of the subject
 * here and the position before a final newline there - Python spells with
 * `\Z` what Perl spells with `\z`, and it has no `\z` at all. And `\N`
 * takes only `\N{NAME}`: `\N` alone is an error where Perl reads it as
 * "not a newline", and `\N{U+0041}` is an error where Perl reads a code
 * point.
 *
 * @param c The letter after the backslash.
 * @param in_class Whether this is inside a bracket expression.
 * @return Non-zero when `re` accepts it there.
 */
static int python_knows_escape(char c, int in_class) {
  switch (c) {
    // The anchors, which `re` refuses inside a class where PCRE2 refuses
    // them too - so this half agrees and is here only to be complete.
    case 'A':
    case 'B':
    case 'Z':
      return !in_class;
    // Characters, shorthands and the two constructs with an argument.
    case 'a': case 'b': case 'd': case 'f': case 'n': case 'r':
    case 's': case 't': case 'v': case 'w':
    case 'D': case 'S': case 'W':
    case 'N': case 'u': case 'x': case 'U':
      return 1;
    default:
      return 0;
  }
}

/** The code point a single-letter escape denotes, or 0 for "not one". */
static uint32_t simple_escape(char c) {
  switch (c) {
    case 'a': return 0x07;
    case 'e': return 0x1B;
    case 'f': return 0x0C;
    case 'n': return 0x0A;
    case 'r': return 0x0D;
    case 't': return 0x09;
    default: return 0;
  }
}

/**
 * Read `\p{...}` or `\P{...}`, the letter already consumed.
 *
 * Three spellings: `\p{Name}`, `\p{^Name}` (negated inside the braces, which
 * ECMAScript does not have) and the one-letter `\pL`. The name is stored as
 * written; resolving it against the Unicode tables is lowering's job.
 */
static GRX_Result read_property(GRX_Parser * parser, int negated, size_t start,
    Escape * out) {
  size_t first = parser->position;
  size_t length = 0;

  if (grx_parse_eat(parser, '{')) {
    if (grx_parse_eat(parser, '^')) {
      negated = !negated;
    }
    first = parser->position;
    while (!grx_parse_at_end(parser) && byte_at(parser, 0) != '}') {
      parser->position++;
    }
    if (!grx_parse_eat(parser, '}')) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_PROPERTY_SYNTAX, start,
          parser->position - start);
    }
    length = parser->position - first - 1;
  }
  else {
    // `\pL`: exactly one character names the property.
    if (grx_parse_at_end(parser)) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_PROPERTY_SYNTAX, start, 2);
    }
    parser->position++;
    length = 1;
  }

  if (!length) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_PROPERTY_SYNTAX, start,
        parser->position - start);
  }

  // Resolved here as well as at lowering, and the answer thrown away. That
  // is not a duplicate check: lowering reports GRX_DIAG_INTERNAL for a name
  // it cannot resolve, on the stated grounds that the parser has already
  // resolved it - and for this front end that was not true, so `\p{Nosuch}`
  // came back as an internal fault rather than as the unknown property it is.
  {
    const char * text = parser->text + first;
    const char * equals = memchr(text, '=', length);
    uint32_t property = 0;
    GRX_Result known = equals
        ? grx_unicode_property_lookup(text, (size_t)(equals - text),
              equals + 1, length - (size_t)(equals - text) - 1,
              parser->profile.property_match, &property)
        : grx_unicode_property_lookup(
              text, length, NULL, 0, parser->profile.property_match, &property);
    if (known != GRX_OK) {
      return grx_parse_fail(parser, GRX_DIAG_UNKNOWN_PROPERTY, start,
          parser->position - start);
    }
  }

  uint32_t offset = GRX_INDEX_NONE;
  GRX_Result result = grx_pattern_add_name(
      parser->pattern, parser->text + first, length, &offset);
  if (result != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }

  out->kind = ESC_PROPERTY;
  out->name = offset;
  out->negated = negated;
  return GRX_OK;
}

/**
 * The bound types `\b{...}` names, and the boundary each one asserts.
 *
 * From perlrebackslash, and checked against perl 5.40 rather than read off
 * it: `g` is an alias for `gcb` and was missing from this table until that
 * check, so `\b{g}` was refused where Perl compiles it. Uppercase and long
 * spellings are not accepted - `\b{WB}` is an error in Perl too.
 *
 * The negated spelling is the same table: `\B{wb}` is the entry's `negated`
 * anchor, which is how `\b` and `\B` are already told apart.
 */
typedef struct {
  const char * name;
  GRX_AnchorKind anchor;
  GRX_AnchorKind negated;
} BoundType;

static const BoundType bound_types[] = {
  {"gcb", GRX_ANCHOR_GRAPHEME_BOUNDARY, GRX_ANCHOR_NOT_GRAPHEME_BOUNDARY},
  {"g", GRX_ANCHOR_GRAPHEME_BOUNDARY, GRX_ANCHOR_NOT_GRAPHEME_BOUNDARY},
  {"wb", GRX_ANCHOR_WORD_SEG_BOUNDARY, GRX_ANCHOR_NOT_WORD_SEG_BOUNDARY},
  {"sb", GRX_ANCHOR_SENTENCE_BOUNDARY, GRX_ANCHOR_NOT_SENTENCE_BOUNDARY},
  {"lb", GRX_ANCHOR_LINE_BOUNDARY, GRX_ANCHOR_NOT_LINE_BOUNDARY},
  {NULL, GRX_ANCHOR_COUNT, GRX_ANCHOR_COUNT},
};

/**
 * Read Perl's `\b{...}`, the `b` or `B` already consumed.
 *
 * Perl's, and only Perl's: pcre2test compiles `/\b{wb}/` as a word boundary
 * followed by four ordinary characters, which is what this library did for
 * both until it was asked. Three of the four are UAX #29's break algorithms
 * and the fourth is UAX #14's; src/unicode/break.c has them.
 *
 * `out_handled` is cleared when this is not the `\b{...}` form at all, so
 * that the caller carries on with the ordinary boundary. A sentinel result
 * cannot say that: grx_parse_fail() returns GRX_ERR_SYNTAX too, and reading
 * "not a bound" out of "an unknown bound type" let `\b{nosuch}` compile.
 */
static GRX_Result read_bound_type(GRX_Parser * parser, size_t start,
    int negated, int * out_handled, GRX_AnchorKind * out_anchor) {
  *out_handled = 0;
  if (flavour(parser) != FLAVOUR_PERL || byte_at(parser, 0) != '{') {
    return GRX_OK;
  }

  size_t scan = 1;
  while (byte_at(parser, scan) == ' ' || byte_at(parser, scan) == '\t') {
    scan++;
  }
  size_t first = scan;
  while (byte_at(parser, scan) && byte_at(parser, scan) != '}'
      && byte_at(parser, scan) != ' ' && byte_at(parser, scan) != '\t') {
    scan++;
  }
  size_t length = scan - first;
  while (byte_at(parser, scan) == ' ' || byte_at(parser, scan) == '\t') {
    scan++;
  }
  if (byte_at(parser, scan) != '}') {
    // Not a bound at all - `\b` followed by a brace that is something else.
    return GRX_OK;
  }

  *out_handled = 1;
  const char * name = parser->text + parser->position + first;
  parser->position += scan + 1;
  if (!length) {
    // perl: "Empty \b{} in regex".
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_ESCAPE, start, parser->position - start);
  }
  for (size_t i = 0; bound_types[i].name; i++) {
    if (strlen(bound_types[i].name) == length
        && memcmp(bound_types[i].name, name, length) == 0) {
      *out_anchor = negated ? bound_types[i].negated : bound_types[i].anchor;
      return GRX_OK;
    }
  }

  // perl: "'nosuch' is an unknown bound type in regex". `\b{3}` is one of
  // these and not a quantifier, which is where Perl and PCRE2 part.
  return grx_parse_fail(
      parser, GRX_DIAG_INVALID_ESCAPE, start, parser->position - start);
}

/**
 * Whether the `{` at an offset from here could be a quantifier's.
 *
 * Both references allow spaces inside the braces - `\N{ 3 }` is three of
 * anything - so the shape is digits, commas and blanks, with at least one
 * digit, closed by a brace. What it cannot be is a name.
 */
static int brace_is_repeat(const GRX_Parser * parser, size_t at) {
  size_t scan = at + 1;
  int digits = 0;
  while (is_decimal(byte_at(parser, scan)) || byte_at(parser, scan) == ','
      || byte_at(parser, scan) == ' ' || byte_at(parser, scan) == '\t') {
    digits += is_decimal(byte_at(parser, scan));
    scan++;
  }

  return digits && byte_at(parser, scan) == '}';
}

/**
 * Read `\N`, the `N` already consumed.
 *
 * `\N` alone is "any character that is not a newline". `\N{U+hhhh}` is a code
 * point, and PCRE2 accepts it only in UTF mode. `\N{name}` is a Perl
 * character-name lookup that PCRE2 does not implement and neither does this.
 */
static GRX_Result read_named_codepoint(GRX_Parser * parser, int in_class,
    size_t start, Escape * out) {
  if (byte_at(parser, 0) != '{') {
    // Python has no bare `\N`. `re` answers "missing {" for it, inside a
    // class and outside alike: `\N{NAME}` is the whole construct there,
    // where Perl also reads a lone `\N` as "not a newline".
    if (flavour(parser) == FLAVOUR_PYTHON) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_ESCAPE, start, 2);
    }
    if (in_class) {
      // "not a newline" is not a set operation a class can express, and
      // pcre2test refuses `[\N]`. Checked before the look-ahead below,
      // because a class does not ignore what a pattern ignores.
      return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_ITEM, start, 2);
    }

    // A comment or an extended-mode space may stand between the `N` and a
    // brace, and both references still read `\N {3}` as a quantified `\N`.
    // They part over a brace that is *not* a quantifier: perl refuses
    // `/abc\N {SPACE}/x` with "Missing braces on \N{}", while pcre2test
    // compiles `/\N {U+41}/x,utf` as `\N` followed by six literals. Only
    // Perl's half is checked here, and the skip is undone: what it passed
    // over is ignorable, but only the ordinary path may consume it.
    if (flavour(parser) == FLAVOUR_PERL) {
      size_t saved_position = parser->position;
      size_t saved_quote = parser->quote_end;
      (void)pcre_skip_ignorable(parser);
      int detached_name = byte_at(parser, 0) == '{'
          && !brace_is_repeat(parser, 0);
      parser->position = saved_position;
      parser->quote_end = saved_quote;
      if (detached_name) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_ESCAPE, start, 2);
      }
    }

    out->kind = ESC_NOT_NEWLINE;
    return GRX_OK;
  }

  // `\N{ U+0100 }`: both references allow space inside the braces. `scan`
  // counts from the `{`, so scan 1 is the first character inside it.
  size_t scan = 1;
  while (byte_at(parser, scan) == ' ' || byte_at(parser, scan) == '\t') {
    scan++;
  }

  // `\N{U+0041}` is a code point in both Perl-family dialects and an
  // "undefined character name" in `re`, which looks the brace contents up in
  // the Unicode name table and nowhere else. Refused rather than read,
  // because reading it would accept a spelling the reference rejects.
  if (flavour(parser) == FLAVOUR_PYTHON && byte_at(parser, scan) == 'U'
      && byte_at(parser, scan + 1) == '+') {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_ESCAPE, start, scan + 2);
  }

  if (byte_at(parser, scan) == 'U' && byte_at(parser, scan + 1) == '+') {
    scan += 2;
    uint64_t value = 0;
    int digits = 0;
    while (is_hex(byte_at(parser, scan))) {
      value = (value << 4) | hex_value(byte_at(parser, scan));
      if (value > GRX_CODEPOINT_MAX) {
        return grx_parse_fail(
            parser, GRX_DIAG_CODEPOINT_OUT_OF_RANGE, start, scan + 2);
      }
      digits++;
      scan++;
    }
    while (byte_at(parser, scan) == ' ' || byte_at(parser, scan) == '\t') {
      scan++;
    }
    if (!digits || byte_at(parser, scan) != '}') {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_ESCAPE, start, scan + 2);
    }
    if (!(parser->options & GRX_OPT_UTF)) {
      // pcre2test: "\N{U+dddd} is supported only in Unicode (UTF) mode".
      return grx_parse_fail(parser, GRX_DIAG_INVALID_ESCAPE, start, scan + 2);
    }
    parser->position += scan + 1;
    out->kind = ESC_LITERAL;
    out->codepoint = (uint32_t)value;
    return GRX_OK;
  }

  // `\N{2,3}` is `\N` quantified, and the `{` is the quantifier's. Only a
  // brace that cannot be a quantifier is the `\N{name}` form. See below.
  if (brace_is_repeat(parser, 0)) {
    if (in_class) {
      // `[\N{4}]` is a class item that quantifies nothing; pcre2test reads
      // the brace as a name it does not support and refuses the pattern.
      return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_ITEM, start, 2);
    }
    out->kind = ESC_NOT_NEWLINE;
    return GRX_OK;
  }

  // `\N{name}`. Perl has the construct and PCRE2 does not - pcre2test
  // answers error 137, "PCRE2 does not support \F, \L, \l, \N{name}, \U,
  // or \u" - so this is one spelling with two answers rather than one gap.
  // Python has it as well, and has *only* it: `\N{U+0041}` is refused above
  // and a bare `\N` earlier still, so for that dialect this is the whole of
  // what `\N` means.
  if (flavour(parser) == FLAVOUR_PCRE) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_ESCAPE, start, 3);
  }

  // The name runs to the closing brace. `scan` counts from the `{` and the
  // leading spaces were skipped before the `U+` test, so this is already the
  // first character of the name.
  size_t name_start = scan;
  while (byte_at(parser, scan) && byte_at(parser, scan) != '}') {
    scan++;
  }
  if (byte_at(parser, scan) != '}') {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_ESCAPE, start, scan + 2);
  }
  // Trailing space inside the braces, which Perl also ignores - `\N{ SPACE }`
  // is U+0020 there. The leading half was skipped above, so the two ends are
  // handled in the same place and for the same reason.
  size_t name_end = scan;
  while (name_end > name_start
      && (byte_at(parser, name_end - 1) == ' '
          || byte_at(parser, name_end - 1) == '\t')) {
    name_end--;
  }

  uint32_t named = 0;
  if (!grx_unicode_codepoint_from_name(
          parser->text + parser->position + name_start,
          name_end - name_start, &named)) {
    // A name nobody has is a *syntax* error rather than an unsupported
    // construct, because Perl rejects it too: `/abc\N{def}/` does not
    // compile there, and `re_tests` carries four rows that turn on it.
    // GRX_ERR_UNSUPPORTED would promise that the dialect accepts this.
    return grx_parse_fail(
        parser, GRX_DIAG_UNKNOWN_CHARACTER_NAME, start, scan + 2);
  }

  parser->position += scan + 1;
  out->kind = ESC_LITERAL;
  out->codepoint = named;
  return GRX_OK;
}

/**
 * Resolve a reference written relative to where it stands.
 *
 * `\g{-1}` is the most recently opened group and `\g{+1}` is the next one to
 * be opened, so both are the running count plus an offset. Resolved here
 * rather than at lowering because the parser is what knows the count; the
 * node keeps GRX_NODE_RELATIVE so that a dump can still say how it was
 * written.
 */
static GRX_Result resolve_relative(GRX_Parser * parser, int sign,
    uint64_t magnitude, size_t start, uint32_t * out_group) {
  uint64_t base = (uint64_t)parser->groups_opened;

  // Computed only once the subtraction is known not to wrap: `\g{-9}` in a
  // pattern with two groups is an error, not group 4294967289.
  if (!magnitude || (sign < 0 && magnitude > base)) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_BACKREFERENCE, start,
        parser->position - start);
  }

  uint64_t target = sign < 0 ? base + 1 - magnitude : base + magnitude;
  if (!target || target > (uint64_t)parser->group_count) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_BACKREFERENCE, start,
        parser->position - start);
  }

  *out_group = (uint32_t)target;
  return GRX_OK;
}

/**
 * Where the group currently numbered `group` was written.
 *
 * A `(?|...)` gives one number several definitions, and a subroutine call
 * written inside one means the definition in its own branch:
 * `(?|(?<a>a)(?-1)|(?<b>b)(?-1))` calls `a` from the first branch and `b`
 * from the second, though both calls resolve to the same number. So the
 * number alone cannot say what a call re-enters, and this says which
 * definition is the current one where the call stands.
 *
 * Read out of the tree rather than counted a second time: the nodes are
 * appended in the order they are opened, so the last GROUP node carrying
 * this number is the one most recently opened, which is what "relative to
 * where it stands" means.
 *
 * @return The byte offset of its opening parenthesis, or GRX_NPOS when
 *   nothing has opened that number yet - a forward `(?+1)`, which has no
 *   definition to point at and falls back to the number.
 */
static size_t definition_offset(GRX_Parser * parser, uint32_t group) {
  if (!group) {
    return GRX_NPOS;
  }

  for (size_t i = parser->pattern->nodes.count; i > 0; i--) {
    const GRX_Node * node
        = grx_pattern_node(parser->pattern, (uint32_t)(i - 1));
    if (node && node->kind == GRX_NODE_GROUP
        && (node->flags & GRX_NODE_CAPTURING) && node->a == group) {
      return node->offset < GRX_INDEX_NONE ? node->offset : GRX_NPOS;
    }
  }

  return GRX_NPOS;
}

/**
 * Read a `\g` reference, the `g` already consumed.
 *
 * Five spellings and two meanings. `\g1`, `\g{1}`, `\g{-1}` and `\g{name}`
 * are backreferences; `\g<1>`, `\g<name>`, `\g'1'` and `\g'name'` are
 * subroutine calls, which is a different construct with the same sigil.
 */
static GRX_Result read_g_reference(GRX_Parser * parser, size_t start,
    Escape * out) {
  char open = byte_at(parser, 0);
  int subroutine = open == '<' || open == '\'';
  char terminator = open == '<' ? '>' : (open == '\'' ? '\'' : '}');

  // `\g<1>` and `\g'name'` are PCRE2's spelling of a subroutine call and
  // are not perl's: perl answers "Unterminated \g... pattern in regex" for
  // all four of `\g<1>`, `\g<name>`, `\g'1'` and `\g'name'`, its `\g`
  // taking only `\g1`, `\g-1` and `\g{...}`. Perl has subroutine calls -
  // `(?1)` and `(?&name)` - so what is refused here is the spelling and not
  // the construct.
  if (subroutine && flavour(parser) != FLAVOUR_PCRE) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_BACKREFERENCE, start, 2);
  }

  if (open == '{' || subroutine) {
    parser->position++;
    if (open == '{') {
      while (byte_at(parser, 0) == ' ' || byte_at(parser, 0) == '\t') {
        parser->position++;
      }
    }
  }
  else {
    terminator = '\0';
  }

  int sign = 0;
  if (byte_at(parser, 0) == '-') {
    sign = -1;
    parser->position++;
  }
  else if (byte_at(parser, 0) == '+') {
    sign = 1;
    parser->position++;
  }

  if (is_decimal(byte_at(parser, 0))) {
    uint64_t value = 0;
    while (is_decimal(byte_at(parser, 0)) && value < 0x7FFFFFFFu) {
      value = value * 10 + (uint64_t)(byte_at(parser, 0) - '0');
      parser->position++;
    }
    if (terminator == '}') {
      while (byte_at(parser, 0) == ' ' || byte_at(parser, 0) == '\t') {
        parser->position++;
      }
    }
    if (terminator && !grx_parse_eat(parser, terminator)) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_BACKREFERENCE, start,
          parser->position - start);
    }

    out->kind = subroutine ? ESC_SUBROUTINE : ESC_BACKREF;
    out->name = GRX_INDEX_NONE;
    out->relative = sign != 0;
    if (sign) {
      GRX_Result result
          = resolve_relative(parser, sign, value, start, &out->group);
      if (result == GRX_OK && subroutine) {
        out->definition = definition_offset(parser, out->group);
      }
      return result;
    }
    if (!value) {
      // `\g0` is an error; `\g<0>` is a call to the whole pattern, which is
      // what `(?R)` also spells.
      if (!subroutine) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_BACKREFERENCE, start,
            parser->position - start);
      }
      out->group = 0;
      return GRX_OK;
    }
    if (value > (uint64_t)parser->group_count) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_BACKREFERENCE, start,
          parser->position - start);
    }
    out->group = (uint32_t)value;
    return GRX_OK;
  }

  if (!terminator || sign) {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_BACKREFERENCE, start, parser->position - start);
  }

  uint32_t offset = GRX_INDEX_NONE;
  GRX_Result result = read_name(parser, terminator, start, &offset);
  if (result != GRX_OK) {
    return result;
  }

  out->kind = subroutine ? ESC_SUBROUTINE : ESC_BACKREF;
  out->group = 0;
  out->name = offset;
  out->relative = 0;
  return GRX_OK;
}

/** Read `\k<name>`, `\k'name'` or `\k{name}`, the `k` already consumed. */
static GRX_Result read_k_reference(GRX_Parser * parser, size_t start,
    Escape * out) {
  char open = byte_at(parser, 0);
  char terminator = open == '<' ? '>' : (open == '\'' ? '\'' : '}');
  if (open != '<' && open != '\'' && open != '{') {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_BACKREFERENCE, start, 2);
  }
  parser->position++;

  uint32_t offset = GRX_INDEX_NONE;
  GRX_Result result = read_name(parser, terminator, start, &offset);
  if (result != GRX_OK) {
    return result;
  }

  out->kind = ESC_BACKREF;
  out->group = 0;
  out->name = offset;
  out->relative = 0;
  return GRX_OK;
}

/**
 * Read a numeric escape that may be a backreference or may be octal.
 *
 * pcre2pattern: a sequence of digits after `\` is a backreference when the
 * value is below 10, or when at least that many capturing parentheses have
 * been opened. Otherwise up to three octal digits are read. The whole number
 * decides, not the first digit, which is why `\10` in a pattern with twelve
 * groups is group 10 and in a pattern with two is a tab.
 */
static GRX_Result read_numeric_escape(GRX_Parser * parser, size_t start,
    Escape * out) {
  size_t digits = 0;
  uint64_t value = 0;
  while (is_decimal(byte_at(parser, digits)) && value < 0x7FFFFFFFu) {
    value = value * 10 + (uint64_t)(byte_at(parser, digits) - '0');
    digits++;
  }

  if (value < 10 || value <= parser->groups_opened) {
    if (value > parser->group_count) {
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_BACKREFERENCE, start, digits + 1);
    }
    if (python_reference_is_premature(parser, (uint32_t)value)) {
      return grx_parse_fail(
          parser, GRX_DIAG_FORWARD_BACKREFERENCE, start, digits + 1);
    }
    parser->position += digits;
    out->kind = ESC_BACKREF;
    out->group = (uint32_t)value;
    out->name = GRX_INDEX_NONE;
    out->relative = 0;
    return GRX_OK;
  }

  if (!is_octal(byte_at(parser, 0))) {
    // `\8` and `\81` are neither a reference nor octal, and pcre2test calls
    // both "reference to non-existent subpattern" rather than reading the
    // digits as literals.
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_BACKREFERENCE, start, digits + 1);
  }

  out->kind = ESC_LITERAL;
  out->codepoint = read_octal_digits(parser, 3);
  return GRX_OK;
}

/**
 * Read one escape, the backslash already consumed.
 *
 * `in_class` selects the handful of rules that differ inside a bracket
 * expression rather than a second copy of the table.
 */
static GRX_Result read_escape(GRX_Parser * parser, int in_class, Escape * out) {
  size_t start = parser->position - 1;
  *out = (Escape) {
    .kind = ESC_LITERAL,
    .codepoint = 0,
    .shorthand = 0,
    .negated = 0,
    .name = GRX_INDEX_NONE,
    .group = 0,
    .relative = 0,
    .definition = GRX_NPOS,
    .anchor = GRX_ANCHOR_CARET,
    .offset = start,
    .length = 0,
  };

  if (grx_parse_at_end(parser)) {
    return grx_parse_fail(parser, GRX_DIAG_TRAILING_BACKSLASH, start, 1);
  }

  char c = byte_at(parser, 0);

  // Python's alphabet is closed, so the refusal comes before every table
  // below rather than after them. A letter `re` has no meaning for is an
  // error there and a *literal* in neither dialect, so falling through to
  // the identity-escape tail would accept `\q` as "q" - which is what the
  // ECMAScript front end does in its own dialect and what `re` does not do
  // in this one.
  if (flavour(parser) == FLAVOUR_PYTHON
      && ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'))
      && !python_knows_escape(c, in_class)) {
    parser->position++;
    return grx_parse_fail(parser,
        in_class ? GRX_DIAG_INVALID_CLASS_ITEM : GRX_DIAG_INVALID_ESCAPE,
        start, 2);
  }

  // `\v` is the vertical tab here and the vertical-space shorthand in the
  // Perl family - one character against five. Taken before shorthand_for()
  // rather than inside it, because the shared table is the Perl family's
  // and this is not a value it has.
  if (flavour(parser) == FLAVOUR_PYTHON && c == 'v') {
    parser->position++;
    out->codepoint = 0x0B;
    out->length = parser->position - start;
    return GRX_OK;
  }

  int negated = 0;
  int shorthand = shorthand_for(c, &negated);
  if (shorthand >= 0) {
    parser->position++;
    out->kind = ESC_SHORTHAND;
    out->shorthand = shorthand;
    out->negated = negated;
    out->length = parser->position - start;
    return GRX_OK;
  }

  uint32_t simple = simple_escape(c);
  if (simple) {
    parser->position++;
    out->codepoint = simple;
    out->length = parser->position - start;
    return GRX_OK;
  }

  switch (c) {
    case 'b':
      parser->position++;
      if (!in_class) {
        int handled = 0;
        GRX_AnchorKind bound_anchor = GRX_ANCHOR_WORD_BOUNDARY;
        GRX_Result bound
            = read_bound_type(parser, start, 0, &handled, &bound_anchor);
        if (handled) {
          if (bound == GRX_OK) {
            out->kind = ESC_ANCHOR;
            out->anchor = bound_anchor;
            out->length = parser->position - start;
          }
          return bound;
        }
      }
      // The one escape whose meaning depends on where it is: a word boundary
      // outside a class and the backspace character inside one. Both
      // dialects, and both references say so in the same sentence.
      out->kind = in_class ? ESC_BACKSPACE : ESC_ANCHOR;
      out->codepoint = 0x08;
      out->anchor = GRX_ANCHOR_WORD_BOUNDARY;
      out->length = parser->position - start;
      return GRX_OK;

    case 'B':
    case 'A':
    case 'Z':
    case 'z':
    case 'G':
      if (in_class) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_ITEM, start, 2);
      }
      parser->position++;
      if (c == 'B') {
        int handled = 0;
        GRX_AnchorKind bound_anchor = GRX_ANCHOR_NOT_WORD_BOUNDARY;
        GRX_Result bound
            = read_bound_type(parser, start, 1, &handled, &bound_anchor);
        if (handled) {
          if (bound == GRX_OK) {
            out->kind = ESC_ANCHOR;
            out->anchor = bound_anchor;
            out->length = parser->position - start;
          }
          return bound;
        }
      }
      out->kind = ESC_ANCHOR;
      // Python spells with `\Z` what Perl spells with `\z`, and has no `\z`
      // at all: `a\Z` does not match "a\n" in `re` and does in perl. One
      // letter with two meanings rather than a letter one dialect lacks, so
      // it is a remapping here and not a refusal in python_knows_escape().
      out->anchor = c == 'B'   ? GRX_ANCHOR_NOT_WORD_BOUNDARY
          : c == 'A'           ? GRX_ANCHOR_START_SUBJECT
          : c == 'Z'           ? (flavour(parser) == FLAVOUR_PYTHON
                                     ? GRX_ANCHOR_END_SUBJECT
                                     : GRX_ANCHOR_END_BEFORE_NEWLINE)
          : c == 'z'           ? GRX_ANCHOR_END_SUBJECT
                               : GRX_ANCHOR_SEARCH_START;
      out->length = parser->position - start;
      return GRX_OK;

    case 'K':
    case 'R':
    case 'X':
      if (in_class) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_ITEM, start, 2);
      }
      parser->position++;
      out->kind = c == 'K' ? ESC_KEEP
          : c == 'R'       ? ESC_NEWLINE_SET
                           : ESC_GRAPHEME;
      out->length = parser->position - start;
      return GRX_OK;

    case 'N': {
      // `\N{U+hhhh}` is a code point and so is a class item; every other
      // spelling of `\N` is "not a newline" and is not. read_named_codepoint
      // is what knows which of the two this is.
      parser->position++;
      GRX_Result result = read_named_codepoint(parser, in_class, start, out);
      out->length = parser->position - start;
      return result;
    }

    case 'C':
      // One code unit. Meaningless here: this library's subject is code
      // points, and a construct that can land in the middle of a character
      // has no honest approximation.
      parser->position++;
      return grx_parse_fail(
          parser, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, start, 2);

    case 'p':
    case 'P': {
      parser->position++;
      GRX_Result result = read_property(parser, c == 'P', start, out);
      out->length = parser->position - start;
      return result;
    }

    case 'x': {
      parser->position++;
      GRX_Result result = read_hex(parser, start, &out->codepoint);
      out->length = parser->position - start;
      return result;
    }

    case 'u':
    case 'U': {
      // Python's fixed-width code-point escapes, four digits and eight.
      // Neither reference in the Perl family has them: pcre2test answers
      // error 137 for `\u` and `\U` unless PCRE2_ALT_BSUX is set, which is
      // a mode this library does not offer, and perl answers the same.
      // python_knows_escape() is what stops the other two dialects reaching
      // this case, so the guard here is only for a reader.
      if (flavour(parser) != FLAVOUR_PYTHON) {
        break;
      }
      int width = c == 'u' ? 4 : 8;
      parser->position++;
      uint32_t value = 0;
      for (int digit = 0; digit < width; digit++) {
        if (!is_hex(byte_at(parser, 0))) {
          return grx_parse_fail(
              parser, GRX_DIAG_INVALID_HEX_ESCAPE, start, digit + 2);
        }
        value = (value << 4) | hex_value(byte_at(parser, 0));
        parser->position++;
      }
      if (value > GRX_CODEPOINT_MAX) {
        return grx_parse_fail(
            parser, GRX_DIAG_CODEPOINT_OUT_OF_RANGE, start, width + 2);
      }
      out->codepoint = value;
      out->length = parser->position - start;
      return GRX_OK;
    }

    case 'o': {
      parser->position++;
      GRX_Result result = read_braced_octal(parser, start, &out->codepoint);
      out->length = parser->position - start;
      return result;
    }

    case 'c': {
      parser->position++;
      GRX_Result result = read_control(parser, start, &out->codepoint);
      out->length = parser->position - start;
      return result;
    }

    case '0':
      parser->position++;
      out->codepoint = read_octal_digits(parser, 2);
      out->length = parser->position - start;
      return GRX_OK;

    case 'g': {
      if (in_class) {
        // There are no references inside a bracket expression, and pcre2test
        // accepts `[\ga]` and `[\g<a>]` as classes containing the letter.
        parser->position++;
        out->codepoint = (uint32_t)c;
        out->length = parser->position - start;
        return GRX_OK;
      }
      parser->position++;
      GRX_Result result = read_g_reference(parser, start, out);
      out->length = parser->position - start;
      return result;
    }

    case 'k': {
      if (in_class) {
        parser->position++;
        out->codepoint = (uint32_t)c;
        out->length = parser->position - start;
        return GRX_OK;
      }
      parser->position++;
      GRX_Result result = read_k_reference(parser, start, out);
      out->length = parser->position - start;
      return result;
    }

    default:
      break;
  }

  if (c >= '1' && c <= '9') {
    if (in_class) {
      // Inside a class there are no backreferences, so a digit escape is
      // octal from the first digit: `[\1]` is U+0001.
      out->codepoint = read_octal_digits(parser, 3);
      out->length = parser->position - start;
      return GRX_OK;
    }
    GRX_Result result = read_numeric_escape(parser, start, out);
    out->length = parser->position - start;
    return result;
  }

  // An identity escape. Both dialects allow a backslash before any
  // non-alphanumeric character and reject it before a letter or digit that
  // has no meaning, which is what keeps `\j` from silently being "j".
  uint32_t codepoint = 0;
  GRX_Result result = grx_parse_take(parser, &codepoint);
  if (result != GRX_OK) {
    return result;
  }
  if ((codepoint >= 'a' && codepoint <= 'z')
      || (codepoint >= 'A' && codepoint <= 'Z')
      || (codepoint >= '0' && codepoint <= '9')) {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_ESCAPE, start, parser->position - start);
  }

  out->codepoint = codepoint;
  out->length = parser->position - start;
  return GRX_OK;
}

// --------------------------------------------------------------------------
// Turning an escape into a node or a class item
// --------------------------------------------------------------------------

/** Fill a class item from an escape that denotes one. */
static GRX_Result escape_as_item(GRX_Parser * parser, const Escape * escape,
    GRX_ClassItem * out) {
  *out = (GRX_ClassItem) {
    .kind = GRX_CLASS_ITEM_SINGLE,
    .flags = escape->negated ? GRX_CLASS_ITEM_NEGATED : 0,
    .lo = 0,
    .hi = 0,
    .a = 0,
    .offset = escape->offset,
    .length = escape->length,
  };

  switch (escape->kind) {
    case ESC_LITERAL:
    case ESC_BACKSPACE:
      out->lo = escape->kind == ESC_BACKSPACE ? 0x08 : escape->codepoint;
      return GRX_OK;

    case ESC_SHORTHAND:
      out->kind = GRX_CLASS_ITEM_SHORTHAND;
      out->a = (uint32_t)escape->shorthand;
      return GRX_OK;

    case ESC_PROPERTY:
      out->kind = GRX_CLASS_ITEM_PROPERTY;
      out->a = escape->name;
      return GRX_OK;

    default:
      // Everything else - the anchors, `\K`, `\R`, `\X`, `\Q` - has already
      // been refused by read_escape() in class context, so arriving here is
      // this file disagreeing with itself.
      return grx_parse_fail(parser, GRX_DIAG_INTERNAL, escape->offset, 1);
  }
}

/** Append a node holding one class item. */
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

/** Append a node of a kind with no payload, reporting failures as diagnostics. */
static GRX_Result plain_node(GRX_Parser * parser, GRX_NodeKind kind,
    size_t offset, size_t length, uint32_t * out_node) {
  GRX_Result result
      = grx_pattern_add_node(parser->pattern, kind, offset, length, out_node);
  if (result != GRX_OK) {
    return grx_parse_fail(parser,
        result == GRX_ERR_LIMIT ? GRX_DIAG_LIMIT_NODES : GRX_DIAG_OUT_OF_MEMORY,
        offset, length);
  }

  return GRX_OK;
}

/**
 * The code point runs `\R` stands for.
 *
 * pcre2pattern: `\R` is `(?>\r\n|\n|\x0b|\f|\r|\x85)`, and with the default
 * BSR_UNICODE also U+2028 and U+2029. CR LF is a *sequence*, which is why
 * this is a set of strings and not a character class - and why it is atomic:
 * `\R` never gives back the LF to let `\n` match it again.
 */
static GRX_Result newline_set_node(GRX_Parser * parser, size_t start,
    uint32_t * out_node) {
  static const uint32_t crlf[2] = {0x0D, 0x0A};
  static const uint32_t unicode[]
      = {0x0A, 0x0B, 0x0C, 0x0D, 0x85, 0x2028, 0x2029};
  // `(*BSR_ANYCRLF)` cuts it to the three ASCII line endings. pcre2test:
  // `(*BSR_ANYCRLF)\R` does not match a vertical tab, and `\R` does.
  static const uint32_t anycrlf[] = {0x0A, 0x0D};

  const uint32_t * singles = parser->bsr_anycrlf ? anycrlf : unicode;
  size_t single_count = parser->bsr_anycrlf
      ? sizeof(anycrlf) / sizeof(*anycrlf)
      : sizeof(unicode) / sizeof(*unicode);

  GRX_Result result = plain_node(
      parser, GRX_NODE_STRING_SET, start, parser->position - start, out_node);
  if (result != GRX_OK) {
    return result;
  }

  uint32_t first = GRX_INDEX_NONE;
  result = grx_pattern_add_string(parser->pattern, crlf, 2, &first);
  for (size_t i = 0; result == GRX_OK && i < single_count; i++) {
    uint32_t ignored = GRX_INDEX_NONE;
    result = grx_pattern_add_string(parser->pattern, &singles[i], 1, &ignored);
  }
  if (result != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }

  GRX_Node * node = grx_pattern_node(parser->pattern, *out_node);
  node->a = first;
  node->b = 1 + (uint32_t)single_count;
  node->flags |= GRX_NODE_ATOMIC;
  return GRX_OK;
}

static GRX_Result pcre_atom_escape(GRX_Parser * parser, uint32_t * out_node) {
  size_t start = parser->position - 1;
  Escape escape;
  GRX_Result result = read_escape(parser, 0, &escape);
  if (result != GRX_OK) {
    return result;
  }

  switch (escape.kind) {
    case ESC_LITERAL:
      return grx_parse_literal_node(
          parser, escape.codepoint, start, escape.length, out_node);

    case ESC_SHORTHAND:
      return grx_parse_shorthand_node(parser,
          (GRX_ShorthandKind)escape.shorthand, escape.negated, start,
          escape.length, out_node);

    case ESC_PROPERTY: {
      GRX_ClassItem item;
      result = escape_as_item(parser, &escape, &item);
      if (result != GRX_OK) {
        return result;
      }
      return item_as_node(parser, &item, out_node);
    }

    case ESC_ANCHOR:
      result = plain_node(
          parser, GRX_NODE_ANCHOR, start, escape.length, out_node);
      if (result == GRX_OK) {
        grx_pattern_node(parser->pattern, *out_node)->a
            = (uint32_t)escape.anchor;
      }
      return result;

    case ESC_BACKREF:
      result = plain_node(
          parser, GRX_NODE_BACKREF, start, escape.length, out_node);
      if (result == GRX_OK) {
        GRX_Node * node = grx_pattern_node(parser->pattern, *out_node);
        node->a = escape.group;
        node->b = escape.name;
        if (escape.name != GRX_INDEX_NONE) {
          node->flags |= GRX_NODE_NAMED;
        }
        if (escape.relative) {
          node->flags |= GRX_NODE_RELATIVE;
        }
      }
      return result;

    case ESC_SUBROUTINE:
      result = plain_node(
          parser, GRX_NODE_RECURSE, start, escape.length, out_node);
      if (result == GRX_OK) {
        GRX_Node * node = grx_pattern_node(parser->pattern, *out_node);
        node->a = escape.group;
        // `b` is the name where there is one and the definition's own offset
        // otherwise, which is the same split read_recursion() writes and the
        // NAMED flag is what tells them apart.
        node->b = escape.name;
        if (escape.name != GRX_INDEX_NONE) {
          node->flags |= GRX_NODE_NAMED;
        }
        else if (escape.definition != GRX_NPOS) {
          node->b = (uint32_t)escape.definition;
        }
        if (escape.relative) {
          node->flags |= GRX_NODE_RELATIVE;
        }
      }
      return result;

    case ESC_KEEP:
      if (parser->in_lookaround) {
        // pcre2test: "\K is not allowed in lookarounds". What it would mean
        // there has no answer - a lookaround consumes nothing, so there is
        // no reported start for `\K` to move.
        return grx_parse_fail(
            parser, GRX_DIAG_INVALID_LOOKAROUND, start, escape.length);
      }
      return plain_node(parser, GRX_NODE_KEEP, start, escape.length, out_node);

    case ESC_NEWLINE_SET:
      return newline_set_node(parser, start, out_node);

    case ESC_GRAPHEME:
      // One extended grapheme cluster. What that *is* is lowering's
      // business; the parser only records that this is what was written.
      return plain_node(
          parser, GRX_NODE_GRAPHEME, start, escape.length, out_node);

    case ESC_NOT_NEWLINE:
      // An ANY node that is not allowed to be dot-all, rather than a class:
      // what `\N` excludes is the dialect's newline *set*, which is the one
      // thing GRX_IR_ANY already carries and a class item would have to
      // resolve a second time.
      result = plain_node(parser, GRX_NODE_ANY, start, escape.length, out_node);
      if (result == GRX_OK) {
        grx_pattern_node(parser->pattern, *out_node)->flags
            |= GRX_NODE_NEGATED;
      }
      return result;

    case ESC_BACKSPACE:
    default:
      return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 1);
  }
}

static GRX_Result pcre_class_escape(GRX_Parser * parser, GRX_ClassItem * out) {
  Escape escape;
  GRX_Result result = read_escape(parser, 1, &escape);
  if (result != GRX_OK) {
    return result;
  }

  return escape_as_item(parser, &escape, out);
}

// --------------------------------------------------------------------------
// Character classes
// --------------------------------------------------------------------------

/** The POSIX class names both dialects accept, in `[[:name:]]`. */
static const char * const posix_class_names[] = {
  "alnum", "alpha", "ascii", "blank", "cntrl", "digit", "graph", "lower",
  "print", "punct", "space", "upper", "word", "xdigit", NULL
};

/**
 * Whether `[` at the current position begins a POSIX bracket construct, and
 * where its terminator is.
 *
 * This is pcre2_compile.c's check_posix_syntax(), transcribed, and it is
 * transcribed rather than approximated because every rule in it decides a
 * corpus case. Scanning starts after the `[` and its `:`, `.` or `=`:
 *
 *   - a `\` before `]` or `\` skips both, so `[abc[:x\]pqr:]]` still finds
 *     its `:]` and reports an unknown class name rather than a stray `]`;
 *   - a `[` followed by the same terminator, or any `]`, means this was not
 *     a POSIX construct after all;
 *   - the terminator followed by `]` ends it.
 *
 * What is inside is *not* checked here: `[[:1234:]]` is POSIX syntax with a
 * name nobody has, which is an error, and `[[:]` is not POSIX syntax at all,
 * which is a class of two characters.
 */
static int posix_construct_at(const GRX_Parser * parser, size_t * out_end) {
  char terminator = byte_at(parser, 1);
  if (terminator != ':' && terminator != '.' && terminator != '=') {
    return 0;
  }

  for (size_t scan = 2;; scan++) {
    char c = byte_at(parser, scan);
    if (!c && parser->position + scan >= parser->length) {
      return 0;
    }
    if (c == '\\' && (byte_at(parser, scan + 1) == ']'
            || byte_at(parser, scan + 1) == '\\')) {
      scan++;
      continue;
    }
    if ((c == '[' && byte_at(parser, scan + 1) == terminator) || c == ']') {
      return 0;
    }
    if (c == terminator && byte_at(parser, scan + 1) == ']') {
      *out_end = scan;
      return 1;
    }
  }
}

/**
 * Read `[:name:]`, the position on the `[`.
 *
 * `out_matched` is cleared when this was not a POSIX construct, so that the
 * caller reads the `[` as the ordinary character it then is. The sentinel is
 * an output rather than a result code because GRX_ERR_SYNTAX is what an
 * *unknown class name* returns, and the two were the same value for one
 * revision of this file - which made `[[:foo:]]` a class of six characters.
 */
static GRX_Result read_posix_class(
    GRX_Parser * parser, GRX_ClassItem * out, int * out_matched) {
  size_t start = parser->position;
  size_t end = 0;
  *out_matched = 0;

  if (!posix_construct_at(parser, &end)) {
    return GRX_OK;
  }
  *out_matched = 1;

  char terminator = byte_at(parser, 1);
  if (terminator != ':') {
    // `[[.a.]]` and `[[=a=]]`: collating elements and equivalence classes.
    // Both need a locale's collation table, which this library does not have
    // and will not invent - but that is not why they are refused here.
    // Neither reference has them either: pcre2test says "POSIX collating
    // elements are not supported" (error 113) and perl says the syntax "is
    // reserved for future extensions". So this is the dialect not having the
    // construct rather than this library not having built it yet, and the
    // difference is visible to a caller: one is a syntax error in a pattern
    // that will never be valid, the other is a promise.
    return grx_parse_fail(
        parser, GRX_DIAG_NOT_IN_DIALECT, start, end + 2);
  }

  size_t scan = 2;
  int negated = 0;
  if (byte_at(parser, scan) == '^') {
    negated = 1;
    scan++;
  }

  const char * name = parser->text + parser->position + scan;
  size_t length = end - scan;
  int known = 0;
  for (size_t i = 0; posix_class_names[i]; i++) {
    if (strlen(posix_class_names[i]) == length
        && memcmp(posix_class_names[i], name, length) == 0) {
      known = 1;
      break;
    }
  }
  if (!known) {
    return grx_parse_fail(
        parser, GRX_DIAG_UNKNOWN_POSIX_CLASS, start, end + 2);
  }

  uint32_t offset = GRX_INDEX_NONE;
  GRX_Result result
      = grx_pattern_add_name(parser->pattern, name, length, &offset);
  if (result != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }

  parser->position += end + 2;
  *out = (GRX_ClassItem) {
    .kind = GRX_CLASS_ITEM_POSIX,
    .flags = negated ? GRX_CLASS_ITEM_NEGATED : 0,
    .lo = 0,
    .hi = 0,
    .a = offset,
    .offset = start,
    .length = parser->position - start,
  };
  return GRX_OK;
}

/** Whether a class item is one code point, and so may end a range. */
static int is_single(const GRX_ClassItem * item) {
  return item->kind == GRX_CLASS_ITEM_SINGLE;
}

/** Skip what `xx` mode ignores inside a bracket expression. */
static void skip_class_space(GRX_Parser * parser) {
  if (!(parser->options & GRX_OPT_EXTENDED_MORE)) {
    return;
  }

  while (byte_at(parser, 0) == ' ' || byte_at(parser, 0) == '\t') {
    parser->position++;
  }
}

/**
 * Read one class atom: an escape, a POSIX class, or a single character.
 *
 * `out_is_item` is cleared when the atom was consumed but produced nothing -
 * a `\Q` run's boundary or a stray `\E` - so that the caller reads the next
 * one rather than adding an item that is not there.
 */
static GRX_Result read_class_atom(GRX_Parser * parser, GRX_ClassItem * out,
    int * out_is_item, size_t * quote_end) {
  size_t start = parser->position;
  *out_is_item = 1;

  if (parser->position < *quote_end) {
    uint32_t quoted = 0;
    GRX_Result result = grx_parse_take(parser, &quoted);
    if (result != GRX_OK) {
      return result;
    }
    *out = (GRX_ClassItem) {
      .kind = GRX_CLASS_ITEM_SINGLE,
      .flags = 0,
      .lo = quoted,
      .hi = 0,
      .a = 0,
      .offset = start,
      .length = parser->position - start,
    };
    return GRX_OK;
  }

  if (byte_at(parser, 0) == '[') {
    int matched = 0;
    GRX_Result result = read_posix_class(parser, out, &matched);
    if (matched || result != GRX_OK) {
      return result;
    }
  }

  if (grx_parse_eat(parser, '\\')) {
    if (grx_parse_at_end(parser)) {
      return grx_parse_fail(parser, GRX_DIAG_TRAILING_BACKSLASH, start, 1);
    }
    if ((parser->spec.features & GRX_FEATURE_QUOTING)
        && byte_at(parser, 0) == 'Q') {
      parser->position++;
      size_t end = parser->length;
      for (size_t i = parser->position; i + 1 < parser->length; i++) {
        if (parser->text[i] == '\\' && parser->text[i + 1] == 'E') {
          end = i;
          break;
        }
      }
      *quote_end = end;
      *out_is_item = 0;
      return GRX_OK;
    }
    if ((parser->spec.features & GRX_FEATURE_QUOTING)
        && byte_at(parser, 0) == 'E') {
      parser->position++;
      *quote_end = 0;
      *out_is_item = 0;
      return GRX_OK;
    }
    return pcre_class_escape(parser, out);
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

/**
 * Consume what a bracket expression ignores between two items.
 *
 * `\Q` and `\E` are lexically invisible here in the same way they are
 * outside a class: `[\Qa\E-z]` is the range a to z, not three characters, so
 * the `\E` between the low end and the `-` has to disappear before the
 * range is decided rather than after.
 */
static void skip_class_ignorable(GRX_Parser * parser, size_t * quote_end) {
  for (;;) {
    if (parser->position < *quote_end) {
      return;
    }
    skip_class_space(parser);
    if (!(parser->spec.features & GRX_FEATURE_QUOTING)) {
      return;
    }
    if (byte_at(parser, 0) == '\\' && byte_at(parser, 1) == 'Q') {
      parser->position += 2;
      size_t end = parser->length;
      for (size_t i = parser->position; i + 1 < parser->length; i++) {
        if (parser->text[i] == '\\' && parser->text[i + 1] == 'E') {
          end = i;
          break;
        }
      }
      *quote_end = end;
      if (end > parser->position) {
        return;
      }
      continue;
    }
    if (byte_at(parser, 0) == '\\' && byte_at(parser, 1) == 'E') {
      parser->position += 2;
      *quote_end = 0;
      continue;
    }
    return;
  }
}

static GRX_Result pcre_char_class(GRX_Parser * parser, uint32_t * out_node) {
  size_t start = parser->position - 1; // The `[`.
  size_t quote_end = 0;

  GRX_Result result = grx_parse_class_node(parser, start, out_node);
  if (result != GRX_OK) {
    return result;
  }

  // `[\E^]` is a negated class: the `\E` vanishes and the `^` is then the
  // first thing in the brackets. pcre2test reports that pattern as an
  // unterminated class, because the `]` after the caret is a member.
  skip_class_ignorable(parser, &quote_end);
  if (parser->position >= quote_end && grx_parse_eat(parser, '^')) {
    grx_pattern_node(parser->pattern, *out_node)->flags |= GRX_NODE_NEGATED;
  }

  // A `]` in the first position is a literal, which is the rule that makes
  // `[]` unterminated rather than empty: the `]` is a member and the class
  // runs on looking for another.
  int first = 1;

  for (;;) {
    skip_class_ignorable(parser, &quote_end);
    if (grx_parse_at_end(parser)) {
      return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_BRACKET, start, 1);
    }
    if (byte_at(parser, 0) == ']' && !first && parser->position >= quote_end) {
      break;
    }

    GRX_ClassItem low;
    int is_item = 0;
    result = read_class_atom(parser, &low, &is_item, &quote_end);
    if (result != GRX_OK) {
      return result;
    }
    if (!is_item) {
      continue;
    }
    first = 0;

    // A `-` before anything but `]` begins a range. Inside a quoted run it
    // does not: `[\Qa-z\E]` is three characters.
    size_t after_low = parser->position;
    if (parser->position >= quote_end) {
      skip_class_ignorable(parser, &quote_end);
    }
    if (parser->position >= quote_end && byte_at(parser, 0) == '-'
        && parser->position + 1 < parser->length) {
      size_t dash = parser->position;
      parser->position++;
      skip_class_ignorable(parser, &quote_end);
      if (parser->position >= quote_end
          && (byte_at(parser, 0) == ']' || grx_parse_at_end(parser))) {
        // `[a-]`, and `[[:digit:]-   ]` under `xx` where the spaces are not
        // there: the `-` is the last member, not the start of a range.
        parser->position = after_low;
        result = grx_parse_class_add(parser, *out_node, &low);
        if (result != GRX_OK) {
          return result;
        }
        continue;
      }

      GRX_ClassItem high;
      int high_is_item = 0;
      result = read_class_atom(parser, &high, &high_is_item, &quote_end);
      if (result != GRX_OK) {
        return result;
      }
      if (!high_is_item || !is_single(&low) || !is_single(&high)) {
        // pcre2test: "invalid range in character class". Both ends must be
        // single characters, which is where PCRE2 and Perl differ - Perl
        // warns "False [] range" and takes the `-` as a literal, so the
        // three become members of their own. `high_is_item` is the other
        // question and is not this one: nothing was read, so there is no
        // third member to add and no endpoint to be false about.
        if (!high_is_item || !parser->spec.false_range_is_union) {
          return grx_parse_fail(parser, GRX_DIAG_CLASS_ESCAPE_IN_RANGE,
              low.offset, parser->position - low.offset);
        }
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
    else {
      parser->position = after_low;
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
// Option letters
// --------------------------------------------------------------------------

/** The option a letter sets, or 0 when the letter is not one. */
static uint32_t option_for_letter(char c) {
  switch (c) {
    case 'i': return GRX_OPT_CASELESS;
    case 'm': return GRX_OPT_MULTILINE;
    case 's': return GRX_OPT_DOTALL;
    case 'x': return GRX_OPT_EXTENDED;
    case 'n': return GRX_OPT_NO_CAPTURE;
    default: return 0;
  }
}

/**
 * The option a letter sets in one flavour only.
 *
 * Both of these are PCRE2's. Perl answers "Sequence (?J...) not recognized
 * in regex" for `(?J)`, `(?J:...)`, `(?-J)` and `(?iJ)` alike, and the same
 * for every position of `U`. Perl does allow two groups to share a name
 * inside `(?|...)`, which is a rule about branch reset rather than a flag a
 * pattern may set; what it has no spelling for at all is inverting the
 * greediness of every quantifier.
 */
static uint32_t flavour_option_for_letter(const GRX_Parser * parser, char c) {
  if (flavour(parser) != FLAVOUR_PCRE) {
    return 0;
  }
  switch (c) {
    case 'J': return GRX_OPT_DUPLICATE_NAMES;
    case 'U': return GRX_OPT_UNGREEDY;
    default: return 0;
  }
}

/**
 * Perl's charset modifiers, gathered as the letters are read.
 *
 * `a`, `d`, `l` and `u` each choose a character-set semantics, so a pattern
 * may name one: perl reports `(?al:a)` as 'Regexp modifier "a" may appear a
 * maximum of twice'. `a` is the one letter that may appear twice and mean a
 * third thing - `/aa` also stops caseless matching folding across the ASCII
 * boundary - and the two need not be adjacent: `(?aia:s)` is `/aa` with an
 * `i` in the middle.
 */
typedef struct {
  char letter;  ///< The charset letter seen, or 0.
  int count;    ///< How many times, for the `a` that may repeat.
} CharsetChoice;

/**
 * Take one charset letter, or the `p` that keeps them company.
 *
 * What each asks for:
 *
 *   `u`, `d`  the dialect's own semantics, which for Perl is Unicode
 *   `a`       the shorthands and the POSIX classes are ASCII
 *   `aa`      the same, and no fold orbit crosses U+0080
 *   `l`       the locale's semantics, which here is the C locale's - this
 *             library has no other, and the C locale's answer to "which
 *             characters are word characters" is the ASCII one
 */
static GRX_Result read_charset_letter(
    GRX_Parser * parser, char c, int clearing, CharsetChoice * choice) {
  size_t at = parser->position;
  parser->position++;

  if (c == 'p') {
    return GRX_OK;
  }
  if (clearing) {
    // perl: a charset modifier cannot be unset, only replaced.
    return grx_parse_fail(parser, GRX_DIAG_UNKNOWN_FLAG, at, 1);
  }
  if (choice->letter && choice->letter != c) {
    return grx_parse_fail(parser, GRX_DIAG_CONFLICTING_FLAGS, at, 1);
  }
  if (choice->letter == c && c != 'a') {
    return grx_parse_fail(parser, GRX_DIAG_DUPLICATE_FLAG, at, 1);
  }

  choice->letter = c;
  if (++choice->count > 2) {
    // perl: "may appear a maximum of twice".
    return grx_parse_fail(parser, GRX_DIAG_DUPLICATE_FLAG, at, 1);
  }

  return GRX_OK;
}

/** The option bits a gathered charset choice asks for. */
static uint32_t charset_options(const CharsetChoice * choice) {
  uint32_t options = 0;
  if (choice->letter == 'a' || choice->letter == 'l') {
    options = GRX_OPT_ASCII_CLASSES;
  }
  if (choice->letter == 'a' && choice->count == 2) {
    options |= GRX_OPT_ASCII_FOLD_SEPARATE;
  }

  return options;
}

/**
 * Read the letters of an inline option setting, up to `:` or `)`.
 *
 * `(?i-m:...)`, `(?^i...)` and `(?xx)` are all this grammar. `^` means
 * "start from the dialect's defaults", which both references spell as
 * unsetting `imnsx` before applying what follows; `xx` is extended mode
 * widened to bracket expressions, so it sets a second bit as well as the
 * first.
 */
static GRX_Result read_option_letters(GRX_Parser * parser, size_t start,
    uint32_t * out_set, uint32_t * out_clear) {
  uint32_t set = 0;
  uint32_t clear = 0;
  int clearing = 0;
  CharsetChoice charset_choice = {0, 0};

  int reset = grx_parse_eat(parser, '^');
  if (reset) {
    clear = GRX_OPT_CASELESS | GRX_OPT_MULTILINE | GRX_OPT_DOTALL
        | GRX_OPT_EXTENDED | GRX_OPT_EXTENDED_MORE | GRX_OPT_NO_CAPTURE;
  }

  for (;;) {
    char c = byte_at(parser, 0);
    if (c == ':' || c == ')') {
      break;
    }
    if (c == '-') {
      // pcre2test: "invalid hyphen in option setting". `^` has already
      // cleared everything, so a second way to clear would say nothing.
      if (clearing || reset) {
        return grx_parse_fail(parser, GRX_DIAG_UNKNOWN_FLAG,
            parser->position, 1);
      }
      clearing = 1;
      parser->position++;
      continue;
    }

    if (flavour(parser) == FLAVOUR_PERL && (c == 'p' || c == 'a' || c == 'd'
            || c == 'l' || c == 'u')) {
      GRX_Result charset = read_charset_letter(parser, c, clearing, &charset_choice);
      if (charset != GRX_OK) {
        return charset;
      }
      continue;
    }

    // Python's `a` and `u`, which are one choice written two ways rather
    // than two flags: `(?a)` is ASCII and `(?u)` is Unicode, `re` refuses
    // `(?au)` as "flags 'a', 'u' and 'L' are incompatible", and it refuses
    // `(?-a:x)` as "cannot turn off flags 'a', 'u' and 'L'". `L` is refused
    // outright below: it is a locale flag and `re` itself rejects it for a
    // `str` pattern, which is the only kind of pattern this library has.
    //
    // One bit, GRX_OPT_ASCII_CLASSES, because Python's ASCII mode narrows
    // exactly two things at once - `\w` stops reaching "é" *and* `k` stops
    // folding to U+212A - where Perl's `/a` and `/aa` split that pair. The
    // profile's ascii_classes_fold_ascii is the second half. What it does
    // *not* touch is the subject: `(?a).` still matches "é" whole and
    // `(?a)\N{BULLET}` still compiles, so this is not UTF being turned off.
    if (flavour(parser) == FLAVOUR_PYTHON && (c == 'a' || c == 'u')) {
      if (clearing) {
        return grx_parse_fail(parser, GRX_DIAG_UNKNOWN_FLAG,
            parser->position, 1);
      }
      if (charset_choice.letter && charset_choice.letter != c) {
        return grx_parse_fail(parser, GRX_DIAG_CONFLICTING_FLAGS,
            parser->position, 1);
      }
      charset_choice.letter = c;
      parser->position++;
      if (c == 'a') {
        set |= GRX_OPT_ASCII_CLASSES;
        clear &= ~(uint32_t)GRX_OPT_ASCII_CLASSES;
      }
      else {
        // `u` is the dialect's default, so it clears the narrowing rather
        // than setting a widening bit. Spelling it as "set UTF" was wrong
        // in the other direction and cost `.` its decoding.
        clear |= GRX_OPT_ASCII_CLASSES;
        set &= ~(uint32_t)GRX_OPT_ASCII_CLASSES;
      }
      continue;
    }

    uint32_t option = option_for_letter(c);
    // `(?n)` is PCRE2's and perl's, and "unknown extension ?n" in `re`.
    // option_for_letter() is the shared table, so the letter has to be
    // taken back here rather than left out of it.
    if (flavour(parser) == FLAVOUR_PYTHON && c == 'n') {
      option = 0;
    }
    if (!option) {
      option = flavour_option_for_letter(parser, c);
    }
    if (!option) {
      return grx_parse_fail(parser, GRX_DIAG_UNKNOWN_FLAG, parser->position, 1);
    }

    parser->position++;
    if (option == GRX_OPT_EXTENDED && byte_at(parser, 0) == 'x') {
      parser->position++;
      option |= GRX_OPT_EXTENDED_MORE;
    }
    if (clearing) {
      clear |= option;
      if (option & GRX_OPT_EXTENDED) {
        clear |= GRX_OPT_EXTENDED_MORE;
      }
    }
    else {
      set |= option;
    }
  }

  if (grx_parse_at_end(parser)) {
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 1);
  }

  if (charset_choice.letter) {
    // Applied after the loop rather than as each letter arrives, because
    // `(?aia:s)` is `/aa` with an `i` in the middle: what the second `a`
    // means is not known until the string ends. Every one of the four is
    // exclusive, so the choice sets what it wants and clears what the other
    // three would have - `(?a:(?u:\w))` has to widen again, and a letter
    // that only ever set bits could not.
    uint32_t wanted = charset_options(&charset_choice);
    uint32_t both = GRX_OPT_ASCII_CLASSES | GRX_OPT_ASCII_FOLD_SEPARATE;
    set = (set & ~both) | wanted;
    clear = (clear & ~both) | (both & ~wanted);
  }

  if (reset) {
    // `^` is a reset applied *before* the letters, not a clear competing
    // with them: `(?^i:a)` is "defaults, then caseless" and matches "A" in
    // both references. The caller applies these as
    // `(options | set) & ~clear`, so leaving `i` in both made the clear win
    // and `(?^i:...)` silently mean `(?^:...)` - the one form of this
    // construct that this library got wrong, found by generating patterns
    // rather than by reading the grammar.
    //
    // Safe to do here rather than at the application site because the two
    // cannot both be explicit: a `-` after `^` is refused above, as it is by
    // pcre2 ("invalid hyphen in option setting") and by perl ("sequence
    // (?^i-...) not recognized"). So when `reset` is set, `clear` is the
    // reset mask and nothing else.
    clear &= ~set;
  }

  *out_set = set;
  *out_clear = clear;
  return GRX_OK;
}

// --------------------------------------------------------------------------
// The `(*...)` constructs: verbs, lookaround spellings and directives
// --------------------------------------------------------------------------

/** One `(*NAME)` the parser knows. */
typedef struct {
  const char * name;
  int verb;            ///< A GRX_VerbKind.
  int takes_argument;  ///< Whether `(*NAME:arg)` is allowed.
  int needs_argument;  ///< Whether `(*NAME)` without one is an error.
} VerbRow;

// Every row takes an argument. pcre2test 10.46 accepts `(*ACCEPT:X)` and
// `(*COMMIT:X)` as readily as `(*MARK:X)`, which the first draft of this
// table did not: it had the three verbs perlre describes as argument-less
// refusing one, and three corpus records said otherwise.
static const VerbRow verb_table[] = {
  {"ACCEPT", GRX_VERB_ACCEPT, 1, 0},
  {"FAIL", GRX_VERB_FAIL, 1, 0},
  {"F", GRX_VERB_FAIL, 1, 0},
  {"COMMIT", GRX_VERB_COMMIT, 1, 0},
  {"PRUNE", GRX_VERB_PRUNE, 1, 0},
  {"SKIP", GRX_VERB_SKIP, 1, 0},
  {"THEN", GRX_VERB_THEN, 1, 0},
  // The one verb whose argument is the whole point: pcre2test reports
  // `(*MARK)`, `(*MARK:)` and `(*:)` alike as "(*MARK) must have an
  // argument".
  {"MARK", GRX_VERB_MARK, 1, 1},
  {NULL, 0, 0, 0},
};

/** One `(*name:` that is another spelling of a group this library has. */
typedef struct {
  const char * name;
  GRX_NodeKind kind;
  uint32_t a;
  int pcre_only; ///< Perl has no such construct, whatever it is spelled.
} AltGroupRow;

// The four non-atomic rows are PCRE2's alone: perl answers "Unknown
// '(*...)' construct 'napla'" for each of them, and "Sequence (?*...) not
// recognized" for the short spellings read elsewhere. Everything above them
// is in both - `(*atomic:`, `(*pla:` and the rest all compile in perl
// 5.40.1, probed rather than assumed, which is also what corrected section
// 3's feature table: it gave NON_ATOMIC_LOOKAROUND to Perl as well.
static const AltGroupRow alt_group_table[] = {
  {"atomic", GRX_NODE_GROUP, 0, 0},
  {"pla", GRX_NODE_LOOKAROUND, GRX_LOOK_AHEAD_POSITIVE, 0},
  {"positive_lookahead", GRX_NODE_LOOKAROUND, GRX_LOOK_AHEAD_POSITIVE, 0},
  {"nla", GRX_NODE_LOOKAROUND, GRX_LOOK_AHEAD_NEGATIVE, 0},
  {"negative_lookahead", GRX_NODE_LOOKAROUND, GRX_LOOK_AHEAD_NEGATIVE, 0},
  {"plb", GRX_NODE_LOOKAROUND, GRX_LOOK_BEHIND_POSITIVE, 0},
  {"positive_lookbehind", GRX_NODE_LOOKAROUND, GRX_LOOK_BEHIND_POSITIVE, 0},
  {"nlb", GRX_NODE_LOOKAROUND, GRX_LOOK_BEHIND_NEGATIVE, 0},
  {"negative_lookbehind", GRX_NODE_LOOKAROUND, GRX_LOOK_BEHIND_NEGATIVE, 0},
  {"napla", GRX_NODE_LOOKAROUND, GRX_LOOK_AHEAD_NON_ATOMIC, 1},
  {"non_atomic_positive_lookahead", GRX_NODE_LOOKAROUND,
      GRX_LOOK_AHEAD_NON_ATOMIC, 1},
  {"naplb", GRX_NODE_LOOKAROUND, GRX_LOOK_BEHIND_NON_ATOMIC, 1},
  {"non_atomic_positive_lookbehind", GRX_NODE_LOOKAROUND,
      GRX_LOOK_BEHIND_NON_ATOMIC, 1},
  {NULL, GRX_NODE_GROUP, 0, 0},
};

/** Whether this parse may read that row at all. */
static int alt_group_available(const GRX_Parser * parser, size_t row) {
  return !alt_group_table[row].pcre_only || flavour(parser) == FLAVOUR_PCRE;
}

/** The four spellings of a script run, and whether each is atomic. */
static const struct {
  const char * name;
  int atomic;
} script_run_names[] = {
  {"script_run", 0},
  {"sr", 0},
  {"atomic_script_run", 1},
  {"asr", 1},
  {NULL, 0},
};

/** The two spellings of PCRE2's scan substring. */
static const char * const scan_substring_names[] = {
  "scs", "scan_substring", NULL
};

/** Whether everything before `start` is leading `(*...)` directives. */
static int only_directives_before(const GRX_Parser * parser, size_t start) {
  size_t i = 0;
  while (i < start) {
    if (parser->text[i] != '(' || i + 1 >= start || parser->text[i + 1] != '*') {
      return 0;
    }
    size_t close = i + 2;
    while (close < parser->length && parser->text[close] != ')') {
      close++;
    }
    if (close >= parser->length) {
      return 0;
    }
    i = close + 1;
  }

  return i == start;
}

/**
 * Apply a leading `(*NAME)` or `(*NAME=value)` directive.
 *
 * Returns GRX_ERR_SYNTAX when the name is not a directive, so that the
 * caller can try the verb and lookaround tables instead.
 */
static GRX_Result apply_directive(GRX_Parser * parser, const char * name,
    size_t length, size_t start) {
  // Every name below is PCRE2's own. Perl has none of them: `(*UTF)`,
  // `(*UCP)`, `(*NO_JIT)`, `(*CRLF)`, `(*LIMIT_MATCH=5)` and the rest are
  // each "Unknown verb pattern" there, all nineteen probed against perl
  // 5.40.1 rather than read off a manual page. Accepting one under the Perl
  // dialect is this library inventing a construct the reference refuses -
  // and inventing it *silently*, since none of them changes a match.
  if (flavour(parser) != FLAVOUR_PCRE) {
    return GRX_ERR_SYNTAX;
  }

  static const struct {
    const char * name;
    uint32_t option;
  } option_directives[] = {
    {"UTF", GRX_OPT_UTF},
    {"UCP", GRX_OPT_UCP},
    {NULL, 0},
  };
  // Directives that say something about how the *caller* will search, or
  // about an optimisation. Neither changes which subjects a pattern
  // matches, so both are accepted and have no effect here - and saying so
  // is not the same as ignoring a construct that does change the answer.
  //
  // `(*LF)` is in this list and the other five newline conventions are
  // not, which is the whole of the distinction: LF is the convention this
  // library and PCRE2 both already use, so naming it asks for what is
  // already true. `(*CR)`, `(*CRLF)`, `(*ANYCRLF)`, `(*ANY)` and `(*NUL)`
  // each change which subjects a pattern matches and are refused below.
  static const char * const inert_directives[] = {
    "NO_AUTO_POSSESS", "NO_START_OPT", "NO_DOTSTAR_ANCHOR", "NO_JIT",
    "NOTEMPTY", "NOTEMPTY_ATSTART", "LF", NULL
  };

  // The newline convention decides what `.` refuses and where `^` and `$`
  // hold. Measured against pcre2test 10.46 rather than read off
  // pcre2pattern:
  //
  //   convention  `.` refuses                  line terminator
  //   ----------  ---------------------------  ------------------------
  //   (*CR)       CR                           CR
  //   (*LF)       LF (the default here)        LF
  //   (*CRLF)     *nothing*                    the two-character CR LF
  //   (*ANYCRLF)  CR, LF                       CR, LF, or CR LF
  //   (*ANY)      LF VT FF CR NEL LS PS        those, and CR LF
  //   (*NUL)      NUL                          NUL
  //
  // `(*LF)` is in the inert list above rather than here, because naming
  // the default asks for what is already true. The other five are written
  // onto the pattern and spent by lowering; the CR LF pair travels as a
  // flag rather than as a set member, since no code-point set can say
  // "these two characters are one terminator".
  static const struct {
    const char * name;
    GRX_NewlineSet newlines;
  } newline_conventions[] = {
    {"CR", GRX_NEWLINES_CR},
    {"CRLF", GRX_NEWLINES_CRLF},
    {"ANYCRLF", GRX_NEWLINES_ANYCRLF},
    {"ANY", GRX_NEWLINES_ANY},
    {"NUL", GRX_NEWLINES_NUL},
    {NULL, GRX_NEWLINES_LF},
  };

  for (size_t i = 0; option_directives[i].name; i++) {
    if (strlen(option_directives[i].name) == length
        && memcmp(option_directives[i].name, name, length) == 0) {
      if (!only_directives_before(parser, start)) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
            parser->position - start);
      }
      // Both: the parse needs it now, and lowering reads the *pattern's*
      // copy rather than the caller's argument. Only a leading directive may
      // write that copy - an inline `(?i)` must not, or `ab(?i)cd` would
      // fold the "ab" as well, which is what happened when the two were
      // written back at the end of the parse instead of here.
      parser->options |= option_directives[i].option;
      parser->pattern->options |= option_directives[i].option;
      return GRX_OK;
    }
  }

  for (size_t i = 0; inert_directives[i]; i++) {
    if (strlen(inert_directives[i]) == length
        && memcmp(inert_directives[i], name, length) == 0) {
      if (!only_directives_before(parser, start)) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
            parser->position - start);
      }
      return GRX_OK;
    }
  }

  // `(*BSR_ANYCRLF)` and `(*BSR_UNICODE)`: what `\R` matches. This one is
  // built, because `\R` is an alternation the *parser* writes and a
  // directive that may only lead the pattern is therefore always read
  // before the `\R` it governs. `(*BSR_ANYCRLF)\R` does not match a
  // vertical tab in pcre2test and plain `\R` does.
  static const struct {
    const char * name;
    int anycrlf;
  } bsr_directives[] = {
    {"BSR_ANYCRLF", 1},
    {"BSR_UNICODE", 0},
    {NULL, 0},
  };
  for (size_t i = 0; bsr_directives[i].name; i++) {
    if (strlen(bsr_directives[i].name) == length
        && memcmp(bsr_directives[i].name, name, length) == 0) {
      if (!only_directives_before(parser, start)) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
            parser->position - start);
      }
      parser->bsr_anycrlf = bsr_directives[i].anycrlf;
      return GRX_OK;
    }
  }

  for (size_t i = 0; newline_conventions[i].name; i++) {
    if (strlen(newline_conventions[i].name) != length
        || memcmp(newline_conventions[i].name, name, length) != 0) {
      continue;
    }
    if (!only_directives_before(parser, start)) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
          parser->position - start);
    }
    // The last one written wins, which is what pcre2test does with two of
    // them and is the same rule the limit directives follow.
    parser->pattern->newlines = newline_conventions[i].newlines;
    return GRX_OK;
  }

  // `(*LIMIT_MATCH=d)` and kin. PCRE2 lets a pattern lower a limit and never
  // raise one, and that rule is the whole reason a pattern may set a limit
  // at all: a caller's cap is a policy and a pattern may not overrule it.
  //
  // Which GRX_Limits field each lands on, and why the units are not
  // pcre2's, is documentation/dialects.md section 6. The request is written
  // onto the *pattern*; narrowing the caller's limits by it is
  // grx_pattern_limits_apply(), at the one place a search resolves them.
  static const struct {
    const char * name;
    int field;     ///< 0 max_steps, 1 max_backtrack, 2 max_match_memory.
    size_t scale;  ///< Units of the directive's number, in ours.
  } limit_names[] = {
    {"LIMIT_MATCH", 0, 1},
    {"LIMIT_DEPTH", 1, 1},
    {"LIMIT_HEAP", 2, 1024}, // pcre2 counts kibibytes; max_match_memory bytes.
    {NULL, 0, 0},
  };
  for (size_t i = 0; limit_names[i].name; i++) {
    size_t name_length = strlen(limit_names[i].name);
    if (length <= name_length
        || memcmp(limit_names[i].name, name, name_length) != 0
        || name[name_length] != '=') {
      continue;
    }
    if (!only_directives_before(parser, start)) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
          parser->position - start);
    }
    if (length == name_length + 1) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
          parser->position - start);
    }
    // A number too large to hold is malformed rather than clamped, which is
    // what pcre2test answers too - `(*LIMIT_MATCH=4294967294)` is error 160,
    // "(*VERB) not recognized or malformed", because its counter is 32 bits
    // wide. The ceiling here is this library's own and is far higher; what
    // is shared is that an unrepresentable request is refused rather than
    // quietly turned into some other number. One below GRX_NPOS, because
    // GRX_NPOS is how GRX_PatternLimits spells "the pattern did not ask".
    size_t value = 0;
    for (size_t j = name_length + 1; j < length; j++) {
      if (!is_decimal(name[j])) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
            parser->position - start);
      }
      size_t digit = (size_t)(name[j] - '0');
      if (value > ((GRX_NPOS - 1) - digit) / 10) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
            parser->position - start);
      }
      value = value * 10 + digit;
    }
    if (limit_names[i].scale != 1) {
      if (value > (GRX_NPOS - 1) / limit_names[i].scale) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
            parser->position - start);
      }
      value *= limit_names[i].scale;
    }
    // The last one written wins rather than the smallest, which is
    // pcre2test's answer both ways round: `(*LIMIT_MATCH=1)` followed by
    // `(*LIMIT_MATCH=10)` matches "abc" and the other order does not. The
    // "may only lower" rule is about the *caller's* limit, not about an
    // earlier directive.
    GRX_PatternLimits * asked = &parser->pattern->limits;
    size_t * target = limit_names[i].field == 0 ? &asked->max_steps
        : limit_names[i].field == 1              ? &asked->max_backtrack
                                                 : &asked->max_match_memory;
    *target = value;
    return GRX_OK;
  }

  return GRX_ERR_SYNTAX;
}

/** Read a `(*...)` construct, the `(` consumed and the `*` next. */
static GRX_Result read_scan_body(GRX_Parser * parser, uint32_t node);

/**
 * Whether `(*name:` at `offset` names one of the lookarounds.
 *
 * A conditional's condition has to be an assertion: pcre2test takes
 * `(?(*pla:a)b)` and refuses `(?(*atomic:a)b)` and `(?(*script_run:a)b)`
 * with "atomic assertion expected after (?( or (?(?C)". Reading rather than
 * consuming, because the caller has not decided what it is looking at yet.
 */
static int star_names_assertion(const GRX_Parser * parser, size_t offset) {
  size_t first = offset;
  size_t scan = offset;
  while (scan < parser->length && parser->text[scan] != ')'
      && parser->text[scan] != ':') {
    scan++;
  }
  if (scan >= parser->length || parser->text[scan] != ':') {
    return 0;
  }

  size_t length = scan - first;
  const char * name = parser->text + first;
  for (size_t i = 0; alt_group_table[i].name; i++) {
    if (alt_group_table[i].kind == GRX_NODE_LOOKAROUND
        && alt_group_available(parser, i)
        && strlen(alt_group_table[i].name) == length
        && memcmp(alt_group_table[i].name, name, length) == 0) {
      return 1;
    }
  }
  return 0;
}

static GRX_Result read_star_construct(GRX_Parser * parser, size_t start,
    GRX_GroupOpen * out) {
  // Every `(*...)` construct is PCRE2's or perl's: the control verbs, the
  // directives, the script runs, the scan-substring forms and the
  // alternative group spellings. `re` has none of them - `(*FAIL)` is
  // "nothing to repeat at position 2", which is `re` reading the `*` as a
  // quantifier applied to the `(` that opened nothing - so the whole family
  // is refused here rather than one table at a time.
  if (flavour(parser) == FLAVOUR_PYTHON) {
    return grx_parse_fail(parser, GRX_DIAG_NOTHING_TO_REPEAT, start + 1, 1);
  }
  parser->position++; // The `*`.
  size_t first = parser->position;
  while (!grx_parse_at_end(parser) && byte_at(parser, 0) != ')'
      && byte_at(parser, 0) != ':') {
    parser->position++;
  }
  if (grx_parse_at_end(parser)) {
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 1);
  }

  const char * name = parser->text + first;
  size_t length = parser->position - first;
  int has_argument = byte_at(parser, 0) == ':';

  // `(*:name)` is `(*MARK:name)` written short.
  if (!length && has_argument) {
    name = "MARK";
    length = 4;
  }

  for (size_t i = 0; script_run_names[i].name; i++) {
    if (strlen(script_run_names[i].name) != length
        || memcmp(script_run_names[i].name, name, length) != 0) {
      continue;
    }
    if (!has_argument) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
          parser->position - start + 1);
    }
    parser->position++; // The `:`.
    out->kind = GRX_NODE_GROUP;
    // The atomic group goes *inside* the script run, which is what
    // pcre2pattern says `(*asr:...)` means: `(*sr:(?>...))`. Both flags on
    // one node, and lowering nests them in that order - outside, it would
    // not stop backtracking into the run.
    out->flags = GRX_NODE_SCRIPT_RUN
        | (script_run_names[i].atomic ? GRX_NODE_ATOMIC : 0u);
    out->has_body = 1;
    return GRX_OK;
  }

  for (size_t i = 0; scan_substring_names[i]; i++) {
    if (strlen(scan_substring_names[i]) != length
        || memcmp(scan_substring_names[i], name, length) != 0) {
      continue;
    }
    if (!has_argument) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
          parser->position - start + 1);
    }
    parser->position++; // The `:`.
    out->kind = GRX_NODE_SCAN;
    out->a = GRX_INDEX_NONE;
    out->has_body = 1;
    out->read_body = read_scan_body;
    return GRX_OK;
  }

  for (size_t i = 0; alt_group_table[i].name; i++) {
    if (strlen(alt_group_table[i].name) != length
        || memcmp(alt_group_table[i].name, name, length) != 0) {
      continue;
    }
    if (!alt_group_available(parser, i)) {
      break;
    }
    if (!has_argument) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
          parser->position - start + 1);
    }
    parser->position++; // The `:`.
    out->kind = alt_group_table[i].kind;
    out->a = alt_group_table[i].a;
    out->flags = alt_group_table[i].kind == GRX_NODE_GROUP
        ? GRX_NODE_ATOMIC
        : 0;
    out->has_body = 1;
    return GRX_OK;
  }

  for (size_t i = 0; verb_table[i].name; i++) {
    if (strlen(verb_table[i].name) != length
        || memcmp(verb_table[i].name, name, length) != 0) {
      continue;
    }

    uint32_t argument = GRX_INDEX_NONE;
    size_t argument_length = 0;
    if (has_argument) {
      if (!verb_table[i].takes_argument) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
            parser->position - start + 1);
      }
      parser->position++;
      size_t argument_first = parser->position;
      while (!grx_parse_at_end(parser) && byte_at(parser, 0) != ')') {
        parser->position++;
      }
      argument_length = parser->position - argument_first;
      GRX_Result result = grx_pattern_add_name(
          parser->pattern, parser->text + argument_first, argument_length,
          &argument);
      if (result != GRX_OK) {
        return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
      }
    }
    if (!grx_parse_eat(parser, ')')) {
      return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 1);
    }

    if (verb_table[i].needs_argument && !argument_length) {
      // An empty name is no name: `(*MARK:)` and `(*:)` are the same error
      // as `(*MARK)`, because a mark nothing can refer to marks nothing.
      return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
          parser->position - start);
    }

    out->kind = GRX_NODE_CONTROL;
    out->a = (uint32_t)verb_table[i].verb;
    out->b = argument;
    out->has_body = 0;
    return GRX_OK;
  }

  if (!has_argument) {
    GRX_Result result = apply_directive(parser, name, length, start);
    if (result != GRX_ERR_SYNTAX) {
      if (result != GRX_OK) {
        return result;
      }
      parser->position++; // The `)`.
      out->kind = GRX_NODE_EMPTY;
      out->has_body = 0;
      return GRX_OK;
    }
  }

  return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
      parser->position - start + 1);
}

// --------------------------------------------------------------------------
// Extended character classes: `(?[ ... ])`
// --------------------------------------------------------------------------

/**
 * Skip what an extended class ignores.
 *
 * Space and tab - a literal newline inside `(?[...])` is error 216 in
 * pcre2test, which is the same set `xx` ignores inside an ordinary bracket
 * expression, so the two rules agree. Plus a `\E` with no run open and an
 * empty `\Q\E`, both of which pcre2test lets stand between an operand and
 * an operator: `(?[\n \Q\E])` compiles. A `\Q` run with anything in it is
 * not ignorable and is left for read_extended_term() to refuse.
 */
/**
 * Whether a code point is Pattern_White_Space, which Perl's `(?[ ])` skips.
 *
 * Read off the UCD rather than listed here, because it is a *property* and
 * a list would be a copy of one. All eleven were probed against perl
 * 5.40.1 - U+0009 to U+000D, U+0020, U+0085, U+200E, U+200F, U+2028 and
 * U+2029 are ignored inside `(?[ ])` there, and U+00A0 is not, which is
 * exactly the property and not "whitespace".
 */
static int pattern_white_space(uint32_t code) {
  static const char name[] = "Pattern_White_Space";
  uint32_t property = 0;
  if (grx_unicode_property_lookup(name, sizeof name - 1, NULL, 0,
          GRX_PROPERTY_STRICT, &property)
      != GRX_OK) {
    return 0;
  }
  size_t count = 0;
  const GRX_CharRange * ranges = grx_unicode_property_ranges(property, &count);
  for (size_t i = 0; ranges && i < count; i++) {
    if (code >= ranges[i].low && code <= ranges[i].high) {
      return 1;
    }
  }
  return 0;
}

/**
 * Perl's extra ignorables: all of Pattern_White_Space, and `#` comments.
 *
 * The one place the two dialects' `(?[ ])` grammars differ, and the whole
 * of it. Everything else - the operands, the operators, their precedence,
 * that neither nests textually - was compared over 13,440 generated rows
 * and agrees. pcre2test refuses a literal newline inside `(?[ ])` with
 * error 216 and refuses `#` entirely; perl ignores both, which is what
 * makes a multi-line extended class a thing people write there.
 *
 * A comment runs to the next **line feed** and nothing else: probed, and
 * CR, VT and U+2028 all leave it open, so `(?[ [a] # c\r ])` is an error
 * in perl where `(?[ [a] # c\n ])` is not. A `#` with no line feed after
 * it is left where it is, so the expression reader refuses it - which is
 * perl's answer too.
 *
 * @return Non-zero when something was skipped.
 */
static int skip_perl_ignorable(GRX_Parser * parser) {
  uint32_t code = 0;
  size_t width = 0;
  if (grx_parse_peek(parser, &code, &width) == GRX_OK
      && pattern_white_space(code)) {
    parser->position += width;
    return 1;
  }
  if (byte_at(parser, 0) != '#') {
    return 0;
  }
  for (size_t scan = parser->position + 1; scan < parser->length; scan++) {
    if (parser->text[scan] == '\n') {
      parser->position = scan + 1;
      return 1;
    }
  }
  return 0;
}

static void skip_extended_ignorable(GRX_Parser * parser) {
  int perl = flavour(parser) == FLAVOUR_PERL;
  for (;;) {
    if (byte_at(parser, 0) == ' ' || byte_at(parser, 0) == '\t') {
      parser->position++;
      continue;
    }
    if (perl && skip_perl_ignorable(parser)) {
      continue;
    }
    if ((parser->spec.features & GRX_FEATURE_QUOTING)
        && byte_at(parser, 0) == '\\' && byte_at(parser, 1) == 'E') {
      parser->position += 2;
      continue;
    }
    if ((parser->spec.features & GRX_FEATURE_QUOTING)
        && byte_at(parser, 0) == '\\' && byte_at(parser, 1) == 'Q'
        && byte_at(parser, 2) == '\\' && byte_at(parser, 3) == 'E') {
      parser->position += 4;
      continue;
    }
    return;
  }
}

/**
 * How deep `(?[...])` may nest, the `(?[` itself counting as the first.
 *
 * A bracket expression counts as a level too, which is how the corpus
 * settles the number: `(?[` with fourteen nested parentheses compiles, and
 * the same pattern with one `[\n]` at the bottom does not. Transcribed
 * rather than chosen, the way the 65535 repeat bound is, because a cap this
 * library picked for itself would refuse patterns the reference accepts.
 */
#define GRX_PCRE_ECLASS_NEST_MAX 15

static GRX_Result read_extended_expression(
    GRX_Parser * parser, unsigned depth, uint32_t * out_node);

/**
 * Build a class-operation node over one operand already read.
 *
 * The node takes the span from `start` so that a diagnostic underlines the
 * operator and both of its operands rather than the second alone.
 */
static GRX_Result class_op_node(GRX_Parser * parser, GRX_ClassOpKind op,
    size_t start, uint32_t first, uint32_t * out_node) {
  GRX_Result result = plain_node(
      parser, GRX_NODE_CLASS_OP, start, parser->position - start, out_node);
  if (result != GRX_OK) {
    return result;
  }
  grx_pattern_node(parser->pattern, *out_node)->a = (uint32_t)op;
  if (grx_pattern_add_child(parser->pattern, *out_node, first) != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 0);
  }

  return GRX_OK;
}

/**
 * Wrap a term in a complement, if an odd number of `!` asked for one.
 *
 * An even number asks for none: complementing a set twice over a fixed
 * universe gives the set back, so `!![a]` is `[a]`. Collapsing them here
 * rather than building one node per `!` keeps a pattern of a thousand from
 * becoming a tree a thousand deep, which lowering would have to walk.
 */
static GRX_Result complement_wrap(GRX_Parser * parser, unsigned complements,
    size_t start, uint32_t * out_node) {
  if (!(complements & 1)) {
    return GRX_OK;
  }

  uint32_t operand = *out_node;
  return class_op_node(
      parser, GRX_CLASS_OP_COMPLEMENT, start, operand, out_node);
}

/**
 * Read one operand of an extended class.
 *
 * Five things can stand here, and a bare character is not one of them:
 * pcre2test reports `(?[a])` as "unexpected character in (?[...]) extended
 * character class", because an operand has to be a *set* and `a` is a
 * character. `[a]` is how that set is written.
 */
static GRX_Result read_extended_term(
    GRX_Parser * parser, unsigned depth, uint32_t * out_node) {
  skip_extended_ignorable(parser);
  size_t start = parser->position;

  // Unary complement, and it binds tighter than every binary operator:
  // `! [a] & [ab]` is `(!a) & ab` and matches "b". Counted rather than
  // recursed on, and *not* against the nesting cap: pcre2test compiles a
  // hundred of them, so a reader that recursed once per `!` would be a
  // reader a pattern can overflow.
  unsigned complements = 0;
  for (;;) {
    skip_extended_ignorable(parser);
    if (byte_at(parser, 0) != '!') {
      break;
    }
    parser->position++;
    complements++;
  }

  if (grx_parse_at_end(parser)) {
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_BRACKET, start, 1);
  }

  char c = byte_at(parser, 0);

  if (c == '(') {
    parser->position++;
    if (depth + 1 > GRX_PCRE_ECLASS_NEST_MAX) {
      return grx_parse_fail(
          parser, GRX_DIAG_CLASS_NESTING_TOO_DEEP, start, 1);
    }
    GRX_Result result = read_extended_expression(parser, depth + 1, out_node);
    if (result != GRX_OK) {
      return result;
    }
    skip_extended_ignorable(parser);
    if (!grx_parse_eat(parser, ')')) {
      return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 1);
    }
    return complement_wrap(parser, complements, start, out_node);
  }

  if (c == '[') {
    if (depth + 1 > GRX_PCRE_ECLASS_NEST_MAX) {
      return grx_parse_fail(
          parser, GRX_DIAG_CLASS_NESTING_TOO_DEEP, start, 1);
    }
    GRX_ClassItem item;
    int matched = 0;
    GRX_Result result = read_posix_class(parser, &item, &matched);
    if (result != GRX_OK) {
      return result;
    }
    if (matched) {
      // `[:alpha:]` stands alone here, where inside brackets it would need a
      // second pair around it.
      result = item_as_node(parser, &item, out_node);
      if (result != GRX_OK) {
        return result;
      }
      return complement_wrap(parser, complements, start, out_node);
    }

    // An ordinary bracket expression, read by the ordinary reader - with the
    // whitespace rule `xx` gives it, because `(?[ [ a ] ])` does not match a
    // space in pcre2test. The flag is restored afterwards: what is inside
    // the brackets is the only place it applies.
    uint32_t outer = parser->options;
    parser->options |= GRX_OPT_EXTENDED_MORE;
    parser->position++;
    result = pcre_char_class(parser, out_node);
    parser->options = outer;
    if (result != GRX_OK) {
      return result;
    }
    return complement_wrap(parser, complements, start, out_node);
  }

  if (c == '\\') {
    if (byte_at(parser, 1) == 'Q') {
      // The empty run is ignorable and never arrives here. A run with
      // anything in it is a sequence of characters, and a sequence is not a
      // set: pcre2test refuses `(?[ \Qab\E ])`.
      return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_ITEM, start, 2);
    }

    parser->position++;
    GRX_ClassItem item;
    GRX_Result result = pcre_class_escape(parser, &item);
    if (result != GRX_OK) {
      return result;
    }
    result = item_as_node(parser, &item, out_node);
    if (result != GRX_OK) {
      return result;
    }
    return complement_wrap(parser, complements, start, out_node);
  }

  return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_ITEM, start, 1);
}

/**
 * The operator a character spells, or zero when it is not one.
 *
 * `|` and `+` are both union, which is PCRE2's spelling and not a synonym
 * this library invented.
 */
static int extended_operator(char c, GRX_ClassOpKind * out_op) {
  switch (c) {
    case '|': case '+': *out_op = GRX_CLASS_OP_UNION; return 1;
    case '&': *out_op = GRX_CLASS_OP_INTERSECT; return 1;
    case '-': *out_op = GRX_CLASS_OP_SUBTRACT; return 1;
    case '^': *out_op = GRX_CLASS_OP_SYMDIFF; return 1;
    default: return 0;
  }
}

/**
 * Read a run of terms joined by `&`, which binds tighter than the rest.
 *
 * `[a] | [b] & [b]` matches "a" and "b" in pcre2test, so the intersection
 * happens first. Everything else is one level below this and left to
 * read_extended_expression().
 */
static GRX_Result read_extended_intersection(
    GRX_Parser * parser, unsigned depth, uint32_t * out_node) {
  size_t start = parser->position;
  GRX_Result result = read_extended_term(parser, depth, out_node);
  if (result != GRX_OK) {
    return result;
  }

  for (;;) {
    skip_extended_ignorable(parser);
    if (byte_at(parser, 0) != '&') {
      return GRX_OK;
    }
    parser->position++;

    uint32_t right = GRX_INDEX_NONE;
    result = read_extended_term(parser, depth, &right);
    if (result != GRX_OK) {
      return result;
    }

    uint32_t combined = GRX_INDEX_NONE;
    result = class_op_node(
        parser, GRX_CLASS_OP_INTERSECT, start, *out_node, &combined);
    if (result != GRX_OK) {
      return result;
    }
    if (grx_pattern_add_child(parser->pattern, combined, right) != GRX_OK) {
      return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 0);
    }
    *out_node = combined;
  }
}

/**
 * Read a whole extended-class expression.
 *
 * `|`, `+`, `-` and `^` are one precedence level and associate to the left:
 * `[abc] - [a] | [a]` is `((abc - a) | a)` and `[a] | [abc] - [a]` is
 * `((a | abc) - a)`, which is why the two answer differently for "a".
 */
static GRX_Result read_extended_expression(
    GRX_Parser * parser, unsigned depth, uint32_t * out_node) {
  size_t start = parser->position;

  skip_extended_ignorable(parser);
  GRX_ClassOpKind leading = GRX_CLASS_OP_UNION;
  if (extended_operator(byte_at(parser, 0), &leading)) {
    // pcre2test: "unexpected operator in extended character class (no
    // preceding operand)".
    return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_SET_OP,
        parser->position, 1);
  }

  GRX_Result result = read_extended_intersection(parser, depth, out_node);
  if (result != GRX_OK) {
    return result;
  }

  for (;;) {
    skip_extended_ignorable(parser);
    GRX_ClassOpKind op = GRX_CLASS_OP_UNION;
    if (!extended_operator(byte_at(parser, 0), &op)) {
      if (byte_at(parser, 0) == ']' || byte_at(parser, 0) == ')'
          || grx_parse_at_end(parser)) {
        return GRX_OK;
      }
      // Two operands with nothing between them. pcre2test: "unexpected
      // expression in extended character class (no preceding operator)".
      return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_SET_OP,
          parser->position, 1);
    }
    parser->position++;

    uint32_t right = GRX_INDEX_NONE;
    result = read_extended_intersection(parser, depth, &right);
    if (result != GRX_OK) {
      return result;
    }

    uint32_t combined = GRX_INDEX_NONE;
    result = class_op_node(parser, op, start, *out_node, &combined);
    if (result != GRX_OK) {
      return result;
    }
    if (grx_pattern_add_child(parser->pattern, combined, right) != GRX_OK) {
      return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 0);
    }
    *out_node = combined;
  }
}

/**
 * Read the body of `(?[...])`, the `(?[` already consumed.
 *
 * Reached through GRX_GroupOpen::read_body, so the parser owns the closing
 * `)` and the nesting depth; this owns everything up to and including the
 * `]`. The node it attaches to is a union of one operand, which is the
 * expression - a wrapper rather than the expression itself, because the
 * parser creates the node before the operator is known.
 */
static GRX_Result read_extended_class_body(
    GRX_Parser * parser, uint32_t node) {
  size_t start = parser->position;

  skip_extended_ignorable(parser);
  if (byte_at(parser, 0) == ']') {
    // pcre2test: "empty expression in extended character class".
    return grx_parse_fail(parser, GRX_DIAG_EMPTY_CLASS, start, 1);
  }

  uint32_t expression = GRX_INDEX_NONE;
  GRX_Result result = read_extended_expression(parser, 1, &expression);
  if (result != GRX_OK) {
    return result;
  }

  skip_extended_ignorable(parser);
  if (!grx_parse_eat(parser, ']')) {
    return grx_parse_fail(
        parser, GRX_DIAG_UNMATCHED_OPEN_BRACKET, start - 1, 1);
  }
  if (grx_pattern_add_child(parser->pattern, node, expression) != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 0);
  }

  return GRX_OK;
}

// --------------------------------------------------------------------------
// Conditionals and branch resets: bodies that are not one alternation
// --------------------------------------------------------------------------

static GRX_Result pcre_group_open(GRX_Parser * parser, GRX_GroupOpen * out);
static GRX_Result pcre_skip_ignorable(GRX_Parser * parser);

/**
 * Read a complete `(...)` group as one node.
 *
 * Only a conditional's assertion needs this: its condition is a group that
 * stands where an atom cannot, so the shared grammar never reaches it. The
 * depth accounting is repeated here because the parser's own copy runs in
 * parse_atom(), which this path does not go through.
 */
static GRX_Result read_group_atom(
    GRX_Parser * parser, uint32_t conditional, uint32_t * out_node) {
  size_t start = parser->position;
  GRX_GroupOpen open = {
    .kind = GRX_NODE_GROUP,
    .b = GRX_INDEX_NONE,
    .has_body = 1,
  };

  // `(?(?C9)(?=a)b|c)` puts callouts before the condition, and `(?(?#x)(?=a)`
  // puts a comment there.
  //
  // The comment is skipped, which is exact. The callouts are *kept*, as the
  // conditional's leading children, and lowering hoists them out in front
  // of it - `(?(?C9)(?=a)b|c)` becomes `(?C9)(?(?=a)b|c)`, which is where
  // pcre2test prints callout 9 too. They were dropped here until this
  // library reported a trace pcre2test did not, 150 rows in 25,600 of
  // `callout_diff.py`; a conditional's children are the condition and the
  // branches positionally, and what makes this work is that the hoisting
  // happens before the IR is built, so nothing below the AST sees a fourth
  // kind of child.
  int callouts = 0;
  for (;;) {
    GRX_Result skipped = pcre_skip_ignorable(parser);
    if (skipped != GRX_OK) {
      return skipped;
    }
    start = parser->position;
    if (!grx_parse_eat(parser, '(')) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_CONDITION, start, 1);
    }
    GRX_Result result = pcre_group_open(parser, &open);
    if (result != GRX_OK) {
      return result;
    }
    if (!open.has_body && open.kind == GRX_NODE_CALLOUT) {
      if (callouts) {
        // pcre2 takes one and refuses two: `(?(?C1)(?C2)(?=a)b|c)` is
        // "assertion expected after (?( or (?(?C)". Measured; this was any
        // number here, which accepted a pattern the reference does not.
        return grx_parse_fail(parser, GRX_DIAG_INVALID_CONDITION, start,
            parser->position - start);
      }
      callouts++;
      uint32_t reported = GRX_INDEX_NONE;
      result = grx_pattern_add_node(parser->pattern, GRX_NODE_CALLOUT, start,
          parser->position - start, &reported);
      if (result != GRX_OK) {
        return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
      }
      GRX_Node * callout = grx_pattern_node(parser->pattern, reported);
      callout->a = open.a;
      callout->b = open.b;
      callout->min = open.min;
      callout->max = open.max;
      if (grx_pattern_add_child(parser->pattern, conditional, reported)
          != GRX_OK) {
        return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
      }
      open = (GRX_GroupOpen) {
        .kind = GRX_NODE_GROUP, .b = GRX_INDEX_NONE, .has_body = 1,
      };
      continue;
    }
    if (!open.has_body && open.kind == GRX_NODE_EMPTY) {
      open = (GRX_GroupOpen) {
        .kind = GRX_NODE_GROUP, .b = GRX_INDEX_NONE, .has_body = 1,
      };
      continue;
    }
    break;
  }

  if (open.kind != GRX_NODE_LOOKAROUND) {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_CONDITION, start, parser->position - start);
  }

  GRX_Result result;

  result = grx_pattern_add_node(
      parser->pattern, open.kind, start, 0, out_node);
  if (result != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }
  GRX_Node * node = grx_pattern_node(parser->pattern, *out_node);
  node->flags = open.flags;
  node->a = open.a;
  node->b = open.b;

  if (parser->limits->max_nesting_depth
      && parser->depth + 1 > parser->limits->max_nesting_depth) {
    return grx_parse_fail(parser, GRX_DIAG_LIMIT_NESTING_DEPTH, start, 1);
  }
  parser->depth++;
  int outer_lookbehind = parser->in_lookbehind;
  int outer_lookaround = parser->in_lookaround;
  parser->in_lookaround = 1;
  if (open.a == GRX_LOOK_BEHIND_POSITIVE || open.a == GRX_LOOK_BEHIND_NEGATIVE) {
    parser->in_lookbehind = 1;
  }

  uint32_t body = GRX_INDEX_NONE;
  result = grx_parse_alternation(parser, &body);
  parser->depth--;
  parser->in_lookbehind = outer_lookbehind;
  parser->in_lookaround = outer_lookaround;
  if (result != GRX_OK) {
    return result;
  }
  if (grx_pattern_add_child(parser->pattern, *out_node, body) != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 0);
  }
  if (!grx_parse_eat(parser, ')')) {
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 1);
  }

  grx_pattern_node(parser->pattern, *out_node)->length
      = parser->position - start;
  return GRX_OK;
}

/**
 * Read a conditional's body: the condition where there is one, then at most
 * two branches.
 *
 * "At most two" is the rule this exists for. `(?(1)a|b|c)` is an error in
 * both references, and reading the body as an alternation would make it a
 * conditional whose else-branch is `b|c` - a pattern that matches strings
 * neither reference would match.
 */
static GRX_Result read_conditional_body(GRX_Parser * parser, uint32_t node) {
  const GRX_Node * conditional = grx_pattern_node(parser->pattern, node);
  size_t start = conditional->offset;
  int assertion = conditional->a == (uint32_t)GRX_COND_ASSERTION;
  int define = conditional->a == (uint32_t)GRX_COND_DEFINE;

  if (assertion) {
    uint32_t condition = GRX_INDEX_NONE;
    GRX_Result result = read_group_atom(parser, node, &condition);
    if (result != GRX_OK) {
      return result;
    }
    const GRX_Node * test = grx_pattern_node(parser->pattern, condition);
    if (test && test->kind == GRX_NODE_LOOKAROUND
        && (test->a == (uint32_t)GRX_LOOK_AHEAD_NON_ATOMIC
            || test->a == (uint32_t)GRX_LOOK_BEHIND_NON_ATOMIC)) {
      // pcre2test refuses `(?(*napla:xx)bc)`. A condition is asked once and
      // answered once; there is nowhere for a second way through it to be
      // tried from, so the non-atomic kind has nothing to offer here.
      return grx_parse_fail(parser, GRX_DIAG_INVALID_CONDITION, start,
          parser->position - start);
    }
    if (grx_pattern_add_child(parser->pattern, node, condition) != GRX_OK) {
      return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 0);
    }
  }

  uint32_t branch = GRX_INDEX_NONE;
  GRX_Result result = grx_parse_concatenation(parser, &branch);
  if (result != GRX_OK) {
    return result;
  }
  if (grx_pattern_add_child(parser->pattern, node, branch) != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 0);
  }

  if (!grx_parse_eat(parser, '|')) {
    return GRX_OK;
  }
  if (define) {
    // `(?(DEFINE)...)` never runs, so a second branch is unreachable text.
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_CONDITION, start, parser->position - start);
  }

  uint32_t otherwise = GRX_INDEX_NONE;
  result = grx_parse_concatenation(parser, &otherwise);
  if (result != GRX_OK) {
    return result;
  }
  if (grx_pattern_add_child(parser->pattern, node, otherwise) != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 0);
  }
  grx_pattern_node(parser->pattern, node)->flags |= GRX_NODE_HAS_ELSE;

  if (!grx_parse_at_end(parser) && byte_at(parser, 0) == '|') {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_CONDITION, start, parser->position - start);
  }

  return GRX_OK;
}

/**
 * Read a branch reset's body: every branch numbers its groups from the same
 * base.
 *
 * `(?|(a)|(b)|(c))` has one capturing group, not three, and the count the
 * pattern ends with is the largest any branch reached - which is why the
 * running total is saved, reset per branch, and raised rather than summed.
 */
/**
 * Refuse two groups of the same number with two different names.
 *
 * pcre2test: "different names for subpatterns of the same number are not
 * allowed". Inside `(?|...)` the branches share their numbering, so
 * `(?|(?<a>A)|(?<b>B))` asks for one group to be called two things -
 * `(?|(?<a>A)|(?<a>B))` is fine, and so is a branch that leaves it unnamed.
 *
 * Checked over the nodes the branch reset created rather than as they are
 * created, because a name is only a conflict once a later branch has
 * produced the same number, and the parser reaches that at the `|`.
 *
 * PCRE2's rule alone. Perl compiles `(?|(?<a>a)|(?<b>b))` and lets each name
 * mean the branch it was written in, which is a different model of what a
 * name is and not one this library has.
 */
static GRX_Result check_branch_reset_names(GRX_Parser * parser, size_t first) {
  if (flavour(parser) != FLAVOUR_PCRE) {
    return GRX_OK;
  }

  for (size_t i = first; i < parser->pattern->nodes.count; i++) {
    const GRX_Node * outer = grx_pattern_node(parser->pattern, (uint32_t)i);
    if (!outer || outer->kind != GRX_NODE_GROUP
        || !(outer->flags & GRX_NODE_NAMED)) {
      continue;
    }
    const char * name = grx_pattern_name(parser->pattern, outer->b);
    for (size_t j = first; j < i; j++) {
      const GRX_Node * earlier
          = grx_pattern_node(parser->pattern, (uint32_t)j);
      if (!earlier || earlier->kind != GRX_NODE_GROUP
          || !(earlier->flags & GRX_NODE_NAMED) || earlier->a != outer->a) {
        continue;
      }
      const char * other = grx_pattern_name(parser->pattern, earlier->b);
      if (name && other && strcmp(name, other) != 0) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_NAME,
            outer->offset, outer->length);
      }
    }
  }

  return GRX_OK;
}

static GRX_Result read_branch_reset_body(GRX_Parser * parser, uint32_t node) {
  size_t base = parser->groups_opened;
  size_t highest = base;
  size_t first_node = parser->pattern->nodes.count;

  // `(?|(?'a'x)|(?'a'y))` names one group twice because it *is* one group,
  // and pcre2test accepts it without `(?J)`. Set rather than checked around,
  // because parse_atom() restores the options at the closing `)` - so the
  // relaxation ends exactly where the branch reset does.
  parser->options |= GRX_OPT_DUPLICATE_NAMES;

  for (;;) {
    parser->groups_opened = base;
    uint32_t branch = GRX_INDEX_NONE;
    GRX_Result result = grx_parse_concatenation(parser, &branch);
    if (result != GRX_OK) {
      return result;
    }
    if (grx_pattern_add_child(parser->pattern, node, branch) != GRX_OK) {
      return grx_parse_fail(parser, GRX_DIAG_INTERNAL,
          grx_pattern_node(parser->pattern, node)->offset, 0);
    }
    if (parser->groups_opened > highest) {
      highest = parser->groups_opened;
    }
    if (!grx_parse_eat(parser, '|')) {
      break;
    }
  }

  parser->groups_opened = highest;
  return check_branch_reset_names(parser, first_node);
}

/**
 * Read one entry of a scan-substring group list.
 *
 * `1`, `-1`, `+1`, `<name>` and `'name'`, which are the same five spellings
 * a subroutine call takes. A number is resolved here, relative or not,
 * because the parser is what knows how many groups have been opened; a name
 * is not, because `(*scs:(<x>)a)(?<x>a)` names a group written later.
 *
 * Each entry reaches the pattern as two code points: a kind and a value.
 */
static GRX_Result read_scan_entry(GRX_Parser * parser, uint32_t run) {
  size_t start = parser->position;
  char open = byte_at(parser, 0);

  if (open == '<' || open == '\'') {
    char close = open == '<' ? '>' : '\'';
    parser->position++;
    size_t first = parser->position;
    while (!grx_parse_at_end(parser) && byte_at(parser, 0) != close) {
      parser->position++;
    }
    if (grx_parse_at_end(parser) || parser->position == first) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_NAME, start,
          parser->position - start);
    }
    uint32_t offset = GRX_INDEX_NONE;
    GRX_Result result = grx_pattern_add_name(parser->pattern,
        parser->text + first, parser->position - first, &offset);
    if (result != GRX_OK) {
      return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
    }
    parser->position++; // The closing delimiter.
    if (grx_pattern_string_push(parser->pattern, run, GRX_SCAN_ENTRY_NAME)
            != GRX_OK
        || grx_pattern_string_push(parser->pattern, run, offset) != GRX_OK) {
      return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
    }
    return GRX_OK;
  }

  int sign = 0;
  if (open == '+' || open == '-') {
    sign = open == '-' ? -1 : 1;
    parser->position++;
  }
  if (!is_decimal(byte_at(parser, 0))) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start, 1);
  }

  uint64_t value = 0;
  while (is_decimal(byte_at(parser, 0))) {
    value = value * 10 + (uint64_t)(byte_at(parser, 0) - '0');
    if (value > 0xFFFFu) {
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_BACKREFERENCE, start, parser->position - start);
    }
    parser->position++;
  }

  uint32_t group = 0;
  if (sign) {
    GRX_Result result = resolve_relative(parser, sign, value, start, &group);
    if (result != GRX_OK) {
      return result;
    }
  }
  else {
    // pcre2test refuses `(*scs:(0)a)`: group 0 is the whole match, which is
    // not a captured substring and is not finished being one when the scan
    // would run.
    if (!value || value > (uint64_t)parser->group_count) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_BACKREFERENCE, start,
          parser->position - start);
    }
    group = (uint32_t)value;
  }

  if (grx_pattern_string_push(parser->pattern, run, GRX_SCAN_ENTRY_GROUP)
          != GRX_OK
      || grx_pattern_string_push(parser->pattern, run, group) != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }

  return GRX_OK;
}

/**
 * Read the body of `(*scs:...)`, the `(*scs:` already consumed.
 *
 * The group list first, then an ordinary alternation. The list is what makes
 * this construct different from a lookahead: the body does not run here, it
 * runs over the text one of those groups captured, anchored at its start and
 * with the substring standing in for the whole subject.
 */
static GRX_Result read_scan_body(GRX_Parser * parser, uint32_t node) {
  size_t start = parser->position;

  if (!grx_parse_eat(parser, '(')) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start, 1);
  }

  uint32_t run = GRX_INDEX_NONE;
  if (grx_pattern_string_begin(parser->pattern, &run) != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }

  for (;;) {
    GRX_Result result = read_scan_entry(parser, run);
    if (result != GRX_OK) {
      return result;
    }
    if (!grx_parse_eat(parser, ',')) {
      break;
    }
  }

  if (!grx_parse_eat(parser, ')')) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start, 1);
  }
  grx_pattern_node(parser->pattern, node)->a = run;

  uint32_t body = GRX_INDEX_NONE;
  GRX_Result result = grx_parse_alternation(parser, &body);
  if (result != GRX_OK) {
    return result;
  }
  if (grx_pattern_add_child(parser->pattern, node, body) != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 0);
  }

  return GRX_OK;
}

// --------------------------------------------------------------------------
// Groups
// --------------------------------------------------------------------------

/** Read the condition of `(?(...)`, the `(?(` already consumed. */
static GRX_Result read_condition(GRX_Parser * parser, size_t start,
    GRX_GroupOpen * out) {
  char c = byte_at(parser, 0);

  if (looking_at(parser, "DEFINE)")) {
    parser->position += 6;
    out->a = (uint32_t)GRX_COND_DEFINE;
    return GRX_OK;
  }

  if (looking_at(parser, "VERSION") && flavour(parser) == FLAVOUR_PCRE) {
    // `(?(VERSION>=10.46)...)`. Answered here: this library is not PCRE2 and
    // reports the version it emulates, which documentation/dialects.md
    // section 9 names.
    //
    // PCRE2's alone. Perl answers "Unknown switch condition (?(...))" for
    // every spelling of it, its own version being asked about with `$]`
    // outside the pattern. Falling through rather than failing here lets
    // the name branch read `VERSION...` as a group name, which is what
    // perl's own error is about.
    parser->position += 7;
    int at_least = grx_parse_eat(parser, '>');
    if (!grx_parse_eat(parser, '=')) {
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_CONDITION, start, parser->position - start);
    }
    unsigned major = 0;
    unsigned minor = 0;
    int digits = 0;
    while (is_decimal(byte_at(parser, 0))) {
      major = major * 10 + (unsigned)(byte_at(parser, 0) - '0');
      parser->position++;
      digits++;
    }
    if (grx_parse_eat(parser, '.')) {
      while (is_decimal(byte_at(parser, 0))) {
        minor = minor * 10 + (unsigned)(byte_at(parser, 0) - '0');
        parser->position++;
        digits++;
      }
    }
    if (!digits || minor > 99 || byte_at(parser, 0) != ')') {
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_CONDITION, start, parser->position - start);
    }
    unsigned asked = major * 100 + minor;
    unsigned have = 10 * 100 + 46;
    out->a = (uint32_t)GRX_COND_STATIC;
    out->b = at_least ? (asked <= have) : (asked == have);
    return GRX_OK;
  }

  if (c == 'R') {
    parser->position++;
    if (byte_at(parser, 0) == ')') {
      out->a = (uint32_t)GRX_COND_RECURSION_ANY;
      return GRX_OK;
    }
    if (byte_at(parser, 0) == '&') {
      parser->position++;
      uint32_t offset = GRX_INDEX_NONE;
      GRX_Result result = read_name(parser, ')', start, &offset);
      if (result != GRX_OK) {
        return result;
      }
      parser->position--; // read_name() ate the `)`; the caller wants it.
      out->a = (uint32_t)GRX_COND_RECURSION_GROUP;
      out->b = offset;
      out->flags |= GRX_NODE_NAMED;
      return GRX_OK;
    }
    if (is_decimal(byte_at(parser, 0))) {
      uint64_t value = 0;
      while (is_decimal(byte_at(parser, 0)) && value < 0x7FFFFFFFu) {
        value = value * 10 + (uint64_t)(byte_at(parser, 0) - '0');
        parser->position++;
      }
      if (byte_at(parser, 0) != ')'
          || (value > (uint64_t)parser->group_count
              && !parser->spec.condition_group_may_be_absent)) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_CONDITION, start,
            parser->position - start);
      }
      out->a = (uint32_t)GRX_COND_RECURSION_GROUP;
      out->b = (uint32_t)value;
      return GRX_OK;
    }
    // `(?(R` followed by anything else is a group named `R...`, which the
    // name branch below reads. Rewind so that it sees the `R`.
    parser->position--;
  }

  out->a = (uint32_t)GRX_COND_GROUP_SET;

  if (c == '<' || c == '\'') {
    parser->position++;
    uint32_t offset = GRX_INDEX_NONE;
    GRX_Result result
        = read_name(parser, c == '<' ? '>' : '\'', start, &offset);
    if (result != GRX_OK) {
      return result;
    }
    out->b = offset;
    out->flags |= GRX_NODE_NAMED;
    return GRX_OK;
  }

  if (c == '+' || c == '-' || is_decimal(c)) {
    int sign = c == '+' ? 1 : (c == '-' ? -1 : 0);
    if (sign) {
      parser->position++;
    }
    if (!is_decimal(byte_at(parser, 0))) {
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_CONDITION, start, parser->position - start);
    }
    uint64_t value = 0;
    while (is_decimal(byte_at(parser, 0)) && value < 0x7FFFFFFFu) {
      value = value * 10 + (uint64_t)(byte_at(parser, 0) - '0');
      parser->position++;
    }
    if (byte_at(parser, 0) != ')') {
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_CONDITION, start, parser->position - start);
    }
    if (sign) {
      uint32_t group = 0;
      GRX_Result result
          = resolve_relative(parser, sign, value, start, &group);
      if (result != GRX_OK) {
        return result;
      }
      out->b = group;
      out->flags |= GRX_NODE_RELATIVE;
      return GRX_OK;
    }
    if (!value
        || (value > (uint64_t)parser->group_count
            && !parser->spec.condition_group_may_be_absent)) {
      // `(?(0)` is an error in perl as well as in pcre2test, which is why
      // the zero is tested whatever the dialect says.
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_CONDITION, start, parser->position - start);
    }
    out->b = (uint32_t)value;
    return GRX_OK;
  }

  // A bare name: `(?(name)...)`.
  uint32_t offset = GRX_INDEX_NONE;
  GRX_Result result = read_name(parser, ')', start, &offset);
  if (result != GRX_OK) {
    return result;
  }
  parser->position--;
  out->b = offset;
  out->flags |= GRX_NODE_NAMED;
  return GRX_OK;
}

/** Read a recursion or subroutine call spelled `(?...)`, and its `)`. */
static GRX_Result read_recursion(GRX_Parser * parser, size_t start,
    GRX_GroupOpen * out) {
  out->kind = GRX_NODE_RECURSE;
  out->has_body = 0;
  out->a = 0;
  out->b = GRX_INDEX_NONE;

  char c = byte_at(parser, 0);
  if (c == 'R') {
    parser->position++;
  }
  else if (c == '&') {
    parser->position++;
    uint32_t offset = GRX_INDEX_NONE;
    GRX_Result result = read_name(parser, ')', start, &offset);
    if (result != GRX_OK) {
      return result;
    }
    out->b = offset;
    out->flags |= GRX_NODE_NAMED;
    return GRX_OK;
  }
  else if (c == 'P' && byte_at(parser, 1) == '>') {
    parser->position += 2;
    uint32_t offset = GRX_INDEX_NONE;
    GRX_Result result = read_name(parser, ')', start, &offset);
    if (result != GRX_OK) {
      return result;
    }
    out->b = offset;
    out->flags |= GRX_NODE_NAMED;
    return GRX_OK;
  }
  else {
    int sign = c == '+' ? 1 : (c == '-' ? -1 : 0);
    if (sign) {
      parser->position++;
    }
    uint64_t value = 0;
    int digits = 0;
    while (is_decimal(byte_at(parser, 0)) && value < 0x7FFFFFFFu) {
      value = value * 10 + (uint64_t)(byte_at(parser, 0) - '0');
      parser->position++;
      digits++;
    }
    if (!digits) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_RECURSION, start,
          parser->position - start);
    }
    if (sign) {
      GRX_Result result = resolve_relative(parser, sign, value, start, &out->a);
      if (result != GRX_OK) {
        return result;
      }
      out->flags |= GRX_NODE_RELATIVE;
      size_t where = definition_offset(parser, out->a);
      if (where != GRX_NPOS) {
        out->b = (uint32_t)where;
      }
    }
    else {
      if (value > (uint64_t)parser->group_count) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_RECURSION, start,
            parser->position - start);
      }
      out->a = (uint32_t)value;
    }
  }

  if (!grx_parse_eat(parser, ')')) {
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 1);
  }

  return GRX_OK;
}

static GRX_Result pcre_group_open(GRX_Parser * parser, GRX_GroupOpen * out) {
  size_t start = parser->position - 1; // The `(`.

  *out = (GRX_GroupOpen) {
    .kind = GRX_NODE_GROUP,
    .flags = 0,
    .a = 0,
    .b = GRX_INDEX_NONE,
    .has_body = 1,
    .read_body = NULL,
  };

  if (byte_at(parser, 0) == '*') {
    return read_star_construct(parser, start, out);
  }

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

  // The `(?...` constructs Python does not have. Each was probed against
  // CPython 3.13 rather than read off the `re` documentation, which lists
  // what the module has and is silent about what it refuses:
  //
  //   `(?<name>` and `(?'name'`  "unknown extension" - `(?P<name>` only
  //   `(?&name)` `(?P>name)`     no subroutine call
  //   `(?R)` `(?1)` `(?-1)`      no recursion
  //   `(?|`                      no branch reset
  //
  // `(?<=` and `(?<!` are read above this, so the `<` refused here is only
  // ever the start of a name. Refusing the *spelling* is the point: Python
  // has named groups, so accepting `(?<n>a)` would not be adding a
  // construct - it would be accepting a pattern `re` rejects and telling
  // the caller it is valid Python.
  if (flavour(parser) == FLAVOUR_PYTHON
      && ((c == '<' && byte_at(parser, 1) != '=' && byte_at(parser, 1) != '!')
          || c == '\'' || c == '|' || c == '&' || c == 'R'
          || is_decimal(c) || (c == '-' && is_decimal(byte_at(parser, 1)))
          || (c == 'P' && byte_at(parser, 1) == '>'))) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start, 2);
  }

  if (c == ':') {
    parser->position++;
    return GRX_OK;
  }

  if (c == '>') {
    parser->position++;
    out->flags = GRX_NODE_ATOMIC;
    return GRX_OK;
  }

  if (c == '[' && (parser->spec.features & GRX_FEATURE_CLASS_SET_OPS)) {
    // An extended class is an atom, not a group, and the node the parser is
    // about to build is a union of the one expression inside it - the
    // operator is not known until the expression has been read, and the node
    // exists before that.
    parser->position++;
    out->kind = GRX_NODE_CLASS_OP;
    out->a = (uint32_t)GRX_CLASS_OP_UNION;
    out->b = 0;
    out->read_body = read_extended_class_body;
    return GRX_OK;
  }

  if (c == '|') {
    parser->position++;
    out->kind = GRX_NODE_BRANCH_RESET;
    out->read_body = read_branch_reset_body;
    return GRX_OK;
  }

  if (c == '=' || c == '!') {
    parser->position++;
    out->kind = GRX_NODE_LOOKAROUND;
    out->a = c == '=' ? GRX_LOOK_AHEAD_POSITIVE : GRX_LOOK_AHEAD_NEGATIVE;
    return GRX_OK;
  }

  if ((c == '*' || (c == '<' && byte_at(parser, 1) == '*'))
      && flavour(parser) == FLAVOUR_PCRE) {
    // `(?*` and `(?<*` are `(*napla:` and `(*naplb:` written short. Before
    // the named-group branch, which would otherwise read `(?<*` as a name
    // beginning with an asterisk.
    //
    // PCRE2's, with the long spellings: perl has no non-atomic lookaround
    // at all and answers "Sequence (?*...) not recognized in regex".
    out->kind = GRX_NODE_LOOKAROUND;
    out->a = c == '*' ? GRX_LOOK_AHEAD_NON_ATOMIC
                      : GRX_LOOK_BEHIND_NON_ATOMIC;
    parser->position += c == '*' ? 1 : 2;
    return GRX_OK;
  }

  if (c == '<' && (byte_at(parser, 1) == '=' || byte_at(parser, 1) == '!')) {
    int positive = byte_at(parser, 1) == '=';
    parser->position += 2;
    out->kind = GRX_NODE_LOOKAROUND;
    out->a = positive ? GRX_LOOK_BEHIND_POSITIVE : GRX_LOOK_BEHIND_NEGATIVE;
    return GRX_OK;
  }

  if (c == '(') {
    out->kind = GRX_NODE_CONDITIONAL;
    out->read_body = read_conditional_body;

    // `(?(?=a)b|c)` shares one parenthesis between the conditional and the
    // assertion: the `(` this hook is looking at opens both. It is left
    // where it is when the condition is an assertion, so that
    // read_conditional_body() reads the whole `(?=a)` as the group it is.
    if (byte_at(parser, 1) == '*'
        && !star_names_assertion(parser, parser->position + 2)) {
      // `(*script_run:` and `(*atomic:` are groups, not assertions, and a
      // condition has to be one. Refused here rather than where the
      // construct itself is read, so that the answer is "that is not a
      // condition" rather than anything about the construct - which is
      // what pcre2test says, and it says it whether or not the build has
      // script runs.
      return grx_parse_fail(parser, GRX_DIAG_INVALID_CONDITION, start,
          parser->position - start + 1);
    }
    if (byte_at(parser, 1) == '?' || byte_at(parser, 1) == '*') {
      out->a = (uint32_t)GRX_COND_ASSERTION;
      return GRX_OK;
    }

    parser->position++;
    GRX_Result result = read_condition(parser, start, out);
    if (result != GRX_OK) {
      return result;
    }
    if (!grx_parse_eat(parser, ')')) {
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_CONDITION, start, parser->position - start);
    }
    return GRX_OK;
  }

  if (c == 'R' || c == '&' || c == '+' || is_decimal(c)
      || (c == 'P' && byte_at(parser, 1) == '>')
      || (c == '-' && is_decimal(byte_at(parser, 1)))) {
    return read_recursion(parser, start, out);
  }

  if (c == 'P' && byte_at(parser, 1) == '=') {
    parser->position += 2;
    uint32_t offset = GRX_INDEX_NONE;
    GRX_Result result = read_name(parser, ')', start, &offset);
    if (result != GRX_OK) {
      return result;
    }
    // `(?P=n)` before `(?P<n>...)` is "unknown group name" in `re`, and
    // `(?P<x>(?P<y>a)(?P=x))` is "cannot refer to an open group". The same
    // rule the numeric form has, asked of the number the name resolves to -
    // a name whose group has not been reached yet resolves to nothing,
    // which is the "later" half.
    if (flavour(parser) == FLAVOUR_PYTHON) {
      uint32_t number = 0;
      if (!group_number_for_name(parser, offset, &number)
          || python_reference_is_premature(parser, number)) {
        return grx_parse_fail(parser, GRX_DIAG_FORWARD_BACKREFERENCE, start,
            parser->position - start);
      }
    }
    out->kind = GRX_NODE_BACKREF;
    out->flags = GRX_NODE_NAMED;
    out->a = 0;
    out->b = offset;
    out->has_body = 0;
    return GRX_OK;
  }

  if (c == '<' || c == '\'' || (c == 'P' && byte_at(parser, 1) == '<')) {
    char terminator = '>';
    if (c == 'P') {
      parser->position += 2;
    }
    else {
      terminator = c == '<' ? '>' : '\'';
      parser->position++;
    }

    uint32_t offset = GRX_INDEX_NONE;
    GRX_Result result = read_name(parser, terminator, start, &offset);
    if (result != GRX_OK) {
      return result;
    }
    if (!(parser->options & GRX_OPT_DUPLICATE_NAMES)
        && name_already_used(parser, offset)) {
      return grx_parse_fail(parser, GRX_DIAG_DUPLICATE_GROUP_NAME, start,
          parser->position - start);
    }

    parser->groups_opened++;
    out->flags = GRX_NODE_CAPTURING | GRX_NODE_NAMED;
    out->a = (uint32_t)parser->groups_opened;
    out->b = offset;
    return GRX_OK;
  }

  if (c == 'C') {
    // A callout. It reports a position to a @ref GRX_CalloutFn the caller
    // registered on the search, and changes no answer when there is none -
    // which is PCRE2's rule for a callout with no function registered too.
    //
    // PCRE2's, and only PCRE2's: perl answers "Sequence (?C...) not
    // recognized in regex" for every spelling of it, `(?C)` included.
    if (flavour(parser) != FLAVOUR_PCRE) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start, 3);
    }
    parser->position++;
    uint32_t number = 0;
    uint32_t text = GRX_INDEX_NONE;
    size_t text_length = 0;
    size_t body_offset = 0;
    if (byte_at(parser, 0) == '`' || byte_at(parser, 0) == '\''
        || byte_at(parser, 0) == '"' || byte_at(parser, 0) == '^'
        || byte_at(parser, 0) == '%' || byte_at(parser, 0) == '#'
        || byte_at(parser, 0) == '$' || byte_at(parser, 0) == '{') {
      char opener = byte_at(parser, 0);
      char closer = opener == '{' ? '}' : opener;
      parser->position++;
      // The body's offset in the pattern, which is what a caller gets as
      // GRX_Callout::string_offset. Taken from where the delimiter actually
      // was rather than computed from the spelling later.
      body_offset = parser->position;
      size_t body = parser->position;
      // pcre2pattern: "If the ending delimiter is needed within the string,
      // it must be doubled." So the delimiter's own character is the escape,
      // and the collapsed string is shorter than the text it came from -
      // which is why `(?C"a""b")` reports offset 4, length 3 and `a"b`, and
      // why `(?C"a"")` is *unterminated* rather than the string `a"`.
      size_t raw = 0;
      size_t collapsed = 0;
      int terminated = 0;
      while (!grx_parse_at_end(parser)) {
        if (byte_at(parser, 0) == closer) {
          if (byte_at(parser, 1) != closer) {
            terminated = 1;
            break;
          }
          parser->position += 2;
          raw += 2;
          collapsed++;
          continue;
        }
        parser->position++;
        raw++;
        collapsed++;
      }
      if (!terminated) {
        return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 1);
      }
      parser->position++;

      text_length = collapsed;
      if (raw == collapsed) {
        GRX_Result added = grx_pattern_add_name(
            parser->pattern, parser->text + body, collapsed, &text);
        if (added != GRX_OK) {
          return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 1);
        }
      }
      else {
        // A doubled delimiter, so the stored string is not a span of the
        // pattern and has to be built. Allocated rather than kept on the
        // stack because a callout string has no length this library bounds
        // below max_pattern_length.
        const GRX_Allocator * allocator = parser->pattern->allocator;
        char * buffer = gcu_allocator_malloc(allocator, collapsed);
        if (!buffer) {
          return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 1);
        }
        size_t at = body;
        size_t out_at = 0;
        while (out_at < collapsed) {
          buffer[out_at++] = parser->text[at];
          at += parser->text[at] == closer ? 2 : 1;
        }
        GRX_Result added
            = grx_pattern_add_name(parser->pattern, buffer, collapsed, &text);
        gcu_allocator_free(allocator, buffer);
        if (added != GRX_OK) {
          return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 1);
        }
      }
    }
    else {
      // The number identifies the callout to the caller's function and is
      // one byte wide in PCRE2: `(?C256)` is error 138 there, "number after
      // (?C is greater than 255". The digits are where a pattern written
      // for PCRE2 finds that out.
      size_t digits = 0;
      int overflowed = 0;
      while (is_decimal(byte_at(parser, 0))) {
        if (digits > (GRX_PCRE_CALLOUT_MAX - (size_t)(byte_at(parser, 0)
                         - '0'))
                / 10) {
          overflowed = 1;
        }
        else {
          digits = digits * 10 + (size_t)(byte_at(parser, 0) - '0');
        }
        parser->position++;
      }
      if (overflowed || digits > GRX_PCRE_CALLOUT_MAX) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
            parser->position - start);
      }
      number = (uint32_t)digits;
    }
    if (!grx_parse_eat(parser, ')')) {
      return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 1);
    }
    out->kind = GRX_NODE_CALLOUT;
    out->has_body = 0;
    out->a = number;
    out->b = text;
    out->min = (uint32_t)text_length;
    out->max = (uint32_t)body_offset;
    return GRX_OK;
  }

  if (c == '{' || (c == '?' && byte_at(parser, 1) == '{')) {
    // Perl's embedded code, `(?{...})` and `(??{...})`. Refused with its own
    // diagnostic rather than GRX_DIAG_INVALID_GROUP_SYNTAX: the construct is
    // real and this library will never have it, which is not the same as a
    // malformed group.
    //
    // PCRE2 is the other way round. It has no embedded code at all, so `(?{`
    // is not a construct it declines to run - it is a `{` after `(?`, which
    // pcre2test reports as "unrecognized character after (? or (?-". Saying
    // "not implemented yet" there would promise a construct the dialect does
    // not have.
    if (flavour(parser) == FLAVOUR_PCRE) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start, 3);
    }
    return grx_parse_fail(
        parser, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, start, 3);
  }

  // What is left is an option setting: `(?i)`, `(?i:...)`, `(?^x)`, `(?-i:)`.
  uint32_t set = 0;
  uint32_t clear = 0;
  GRX_Result result = read_option_letters(parser, start, &set, &clear);
  if (result != GRX_OK) {
    return result;
  }

  out->kind = GRX_NODE_OPTIONS;
  out->a = set;
  out->b = clear;
  parser->options = (parser->options | set) & ~clear;

  if (grx_parse_eat(parser, ':')) {
    out->flags = GRX_NODE_SCOPED;
    out->has_body = 1;
    return GRX_OK;
  }

  // An unscoped setting: `(?i)` rather than `(?i:...)`. Python allows it
  // only in a run at the very start of the whole pattern, so `(?i)(?m)ab`
  // compiles, `(?#c)(?i)a` compiles because a comment is not a term, and
  // `a(?i)b`, `((?i)a)`, `(?:(?i)a)` and `(?i)(?:a)(?m)b` are all "global
  // flags not at the start of the expression". Two conditions, because
  // being first is not enough: `((?i)a)` has read no term either, and what
  // disqualifies it is the group around it.
  if (flavour(parser) == FLAVOUR_PYTHON
      && (!parser->only_global_flags_so_far || parser->group_depth > 0)) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
        parser->position - start);
  }

  // `(?i)` applies to the rest of the enclosing group, which parse_atom()
  // bounds by saving and restoring the options across a body. There is no
  // body here and no `)` for the parser to eat, so both are this hook's.
  if (!grx_parse_eat(parser, ')')) {
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 1);
  }
  out->has_body = 0;
  return GRX_OK;
}

// --------------------------------------------------------------------------
// Quantifiers, literals and what extended mode drops
// --------------------------------------------------------------------------

/**
 * Read `{m,n}`, the brace already consumed.
 *
 * A `{` that does not begin a valid quantifier is an ordinary character in
 * both references, so `a{` matches "a{" and `x{,}` matches "x{,}". `{,n}`
 * *is* a quantifier - Perl 5.34 and PCRE2 10.43 made it `{0,n}`, and before
 * that it was a literal, which is the kind of change a corpus catches and a
 * reading of the documentation does not.
 */
/** PCRE2's own cap on a `{}` bound; `z{65536}` is "number too big". */
#define GRX_PCRE_REPEAT_MAX 65535

static GRX_Result check_repeat_bound(
    GRX_Parser * parser, uint64_t min, uint64_t max) {
  if (min > GRX_PCRE_REPEAT_MAX || max > GRX_PCRE_REPEAT_MAX) {
    // The dialect's cap, not a caller's: pcre2test reports `/z{65536}/` as
    // "number too big in {} quantifier", and no option raises it. A limit is
    // what GRX_Limits sets.
    return grx_parse_fail(parser, GRX_DIAG_REPEAT_COUNT_TOO_LARGE,
        parser->position, 0);
  }

  return GRX_OK;
}

/** Step past the space a quantifier may hold: `{ 3 , 4 }` is `{3,4}`. */
static void skip_quantifier_space(const GRX_Parser * parser, size_t * scan) {
  while (byte_at(parser, *scan) == ' ' || byte_at(parser, *scan) == '\t') {
    (*scan)++;
  }
}

static GRX_Result pcre_brace_quantifier(
    GRX_Parser * parser, GRX_Quantifier * out) {
  size_t scan = 0;
  uint64_t min = 0;
  int min_digits = 0;

  *out = (GRX_Quantifier) {0, GRX_REPEAT_INF, 0, GRX_REPEAT_GREEDY};

  skip_quantifier_space(parser, &scan);
  while (is_decimal(byte_at(parser, scan)) && min < 0xFFFFFFFFu) {
    min = min * 10 + (uint64_t)(byte_at(parser, scan) - '0');
    scan++;
    min_digits++;
  }
  skip_quantifier_space(parser, &scan);

  if (byte_at(parser, scan) == '}') {
    if (!min_digits) {
      return GRX_OK;
    }
    parser->position += scan + 1;
    out->min = (uint32_t)min;
    out->max = (uint32_t)min;
    out->is_quantifier = 1;
    return check_repeat_bound(parser, min, min);
  }

  if (byte_at(parser, scan) != ',') {
    return GRX_OK;
  }
  scan++;
  skip_quantifier_space(parser, &scan);

  uint64_t max = 0;
  int max_digits = 0;
  while (is_decimal(byte_at(parser, scan)) && max < 0xFFFFFFFFu) {
    max = max * 10 + (uint64_t)(byte_at(parser, scan) - '0');
    scan++;
    max_digits++;
  }
  skip_quantifier_space(parser, &scan);
  if (byte_at(parser, scan) != '}' || (!min_digits && !max_digits)) {
    return GRX_OK;
  }

  parser->position += scan + 1;
  out->min = (uint32_t)min;
  out->max = max_digits ? (uint32_t)max : GRX_REPEAT_INF;
  out->is_quantifier = 1;
  return check_repeat_bound(parser, min, max_digits ? max : 0);
}

static GRX_Result pcre_literal_atom(GRX_Parser * parser, uint32_t codepoint,
    size_t offset, size_t length, uint32_t * out_node) {
  // `{` is where "a literal" and "an operator" are decided by what follows.
  // `a{` is two characters because `{` begins no quantifier; `{4,5}abc` is an
  // error because it begins one and there is nothing before it to repeat.
  // The parser cannot tell the two apart - it only calls this hook once the
  // character has no operator meaning *at the start of an atom*.
  if (codepoint == '{') {
    size_t resume = parser->position;
    GRX_Quantifier bounds = {0, GRX_REPEAT_INF, 0, GRX_REPEAT_GREEDY};
    GRX_Result probe = pcre_brace_quantifier(parser, &bounds);
    parser->position = resume;
    if (probe == GRX_OK && bounds.is_quantifier) {
      return grx_parse_fail(parser, GRX_DIAG_NOTHING_TO_REPEAT, offset, 1);
    }
  }

  return grx_parse_literal_node(parser, codepoint, offset, length, out_node);
}

/** Whether a code point is whitespace for the purpose of extended mode. */
static int is_extended_space(const GRX_Parser * parser, uint32_t codepoint) {
  switch (codepoint) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20:
      return 1;
    default:
      break;
  }
  if (!(parser->options & GRX_OPT_UTF)) {
    return 0;
  }

  // pcre2pattern: in UTF mode the wider set applies. These are the code
  // points with the White_Space property above U+007F.
  switch (codepoint) {
    case 0x85: case 0xA0: case 0x1680: case 0x2028: case 0x2029:
    case 0x202F: case 0x205F: case 0x3000:
      return 1;
    default:
      return codepoint >= 0x2000 && codepoint <= 0x200A;
  }
}

/**
 * Find where a `\Q` run ends: the next `\E`, or the end of the pattern.
 */
static size_t quote_run_end(const GRX_Parser * parser) {
  for (size_t i = parser->position; i + 1 < parser->length; i++) {
    if (parser->text[i] == '\\' && parser->text[i + 1] == 'E') {
      return i;
    }
  }

  return parser->length;
}

/**
 * Consume what stands between atoms and is not one.
 *
 * Two things: `\Q...\E`, always, and what extended mode ignores. `\Q` is
 * here rather than in the escape reader because an empty run is not an atom
 * at all - `a\Q\E*` is `a*` in both references, and a front end that gave
 * `\Q\E` a node of its own would make the `*` repeat that node instead.
 */
static GRX_Result pcre_skip_ignorable(GRX_Parser * parser) {
  for (;;) {
    if (looking_at(parser, "(?#")) {
      // A comment group is lexically invisible, not an empty atom:
      // pcre2test compiles `a(?#x)*` as `a*` and refuses `(?#x)*` for having
      // nothing to repeat. A node here would make the first of those a
      // repeat of the comment. A callout is *not* in this list - `a(?C1)*`
      // is an error, so a callout is an atom that may not be repeated.
      size_t start = parser->position;
      parser->position += 3;
      while (!grx_parse_at_end(parser) && byte_at(parser, 0) != ')') {
        parser->position++;
      }
      if (!grx_parse_eat(parser, ')')) {
        return grx_parse_fail(parser, GRX_DIAG_UNTERMINATED_COMMENT, start,
            parser->position - start);
      }
      continue;
    }
    // Only where the dialect has the construct. The bit was in the table
    // from the start and nothing read it, because the two dialects that
    // reached this code both had it - the shape a feature flag is for, and
    // the shape that hides until a third dialect arrives. Python has no
    // `\Q`, so leaving this ungated made `\Qa\E` a quoted run there and
    // `\Q` on its own a silent nothing, where `re` calls both "bad escape".
    if ((parser->spec.features & GRX_FEATURE_QUOTING)
        && byte_at(parser, 0) == '\\' && byte_at(parser, 1) == 'Q') {
      parser->position += 2;
      parser->quote_end = quote_run_end(parser);
      if (parser->quote_end > parser->position) {
        return GRX_OK;
      }
      parser->quote_end = GRX_NPOS;
      continue;
    }
    if ((parser->spec.features & GRX_FEATURE_QUOTING)
        && byte_at(parser, 0) == '\\' && byte_at(parser, 1) == 'E') {
      // A `\E` with no run open. Harmless in both references.
      parser->position += 2;
      continue;
    }
    if (!(parser->options & GRX_OPT_EXTENDED)) {
      return GRX_OK;
    }
    break;
  }

  for (;;) {
    if (grx_parse_at_end(parser)) {
      return GRX_OK;
    }

    uint32_t codepoint = 0;
    size_t width = 0;
    if (grx_parse_peek(parser, &codepoint, &width) != GRX_OK) {
      return GRX_OK; // Malformed UTF-8; the atom reader reports it.
    }

    if (is_extended_space(parser, codepoint)) {
      parser->position += width;
      continue;
    }
    if (codepoint != '#') {
      if (((parser->spec.features & GRX_FEATURE_QUOTING)
              && codepoint == '\\'
              && (byte_at(parser, 1) == 'Q' || byte_at(parser, 1) == 'E'))
          || looking_at(parser, "(?#")) {
        // Whitespace, then something else that is not an atom either: start
        // over, so that `(?x) \Qa b\E` keeps the space inside the run and
        // drops the one before it, and `a (?#x) (?#y) {3}` is `a{3}`.
        return pcre_skip_ignorable(parser);
      }
      return GRX_OK;
    }

    // A comment runs to the next line break, or to the end of the pattern.
    parser->position += width;
    while (!grx_parse_at_end(parser)) {
      if (grx_parse_peek(parser, &codepoint, &width) != GRX_OK) {
        return GRX_OK;
      }
      parser->position += width;
      if (codepoint == 0x0A || codepoint == 0x0D
          || ((parser->options & GRX_OPT_UTF)
              && (codepoint == 0x85 || codepoint == 0x2028
                  || codepoint == 0x2029))) {
        break;
      }
    }
  }
}

/**
 * Refuse a quantifier the dialect does not allow on this atom.
 *
 * Both references report "nothing to repeat" for a quantifier applied to
 * something with no extent: an assertion, a verb, an option setting or a
 * comment. `\K` is in the list for the same reason - it moves the reported
 * start and consumes nothing.
 */
static GRX_Result pcre_check_quantifier_target(
    GRX_Parser * parser, uint32_t node, size_t offset, size_t length) {
  const GRX_Node * atom = grx_pattern_node(parser->pattern, node);
  if (!atom) {
    return GRX_OK;
  }

  switch (atom->kind) {
    case GRX_NODE_KEEP:
      // `\K*` compiles in perl and is error 109 in pcre2test. It is the
      // only atom the two dialects disagree about repeating, and repeating
      // it changes nothing: `\K` moves the reported start to here, and
      // moving it here again leaves it here.
      if (flavour(parser) == FLAVOUR_PERL) {
        return GRX_OK;
      }
      return grx_parse_fail(
          parser, GRX_DIAG_NOTHING_TO_REPEAT, offset, length);

    case GRX_NODE_ANCHOR:
      // `a$?` and `^*` compile in perl and are error 109 in pcre2test. The
      // corpus row that asked is `m?^xy\?$?`, which reads as the match
      // operator in Perl's own harness and as a pattern ending in a
      // quantified `$` everywhere the importer looks at it.
      if (flavour(parser) == FLAVOUR_PERL) {
        return GRX_OK;
      }
      return grx_parse_fail(
          parser, GRX_DIAG_NOTHING_TO_REPEAT, offset, length);

    // A lookaround *is* quantifiable here, which surprised this file's first
    // draft: `/(?=a)*/` compiles in pcre2test 10.46, where the same pattern
    // is a syntax error in ECMAScript's Unicode mode. So is `(*ACCEPT)*`.
    // Both were refused until the corpus said otherwise.
    case GRX_NODE_EMPTY:
      return grx_parse_fail(
          parser, GRX_DIAG_NOTHING_TO_REPEAT, offset, length);

    case GRX_NODE_CALLOUT:
      // `a(?C1)*` is error 109 in pcre2test, which is the whole reason a
      // callout is a node rather than something pcre_skip_ignorable() eats:
      // a comment is lexically invisible and `a(?#x)*` repeats the `a`,
      // where a callout is an atom that may not be repeated. Only PCRE2
      // reaches this - perl refuses `(?C...)` outright.
      return grx_parse_fail(
          parser, GRX_DIAG_NOTHING_TO_REPEAT, offset, length);

    case GRX_NODE_CONTROL:
      // perl quantifies every verb, as it quantifies every other zero-width
      // thing: `(*FAIL)*`, `(*PRUNE)*`, `(*MARK:x)*` and the rest all
      // compile in 5.40.1, and `a(*FAIL)*b` matches "ab" with the repeat
      // taken zero times. The third case in this function with that shape,
      // after `\K` and the anchors, and the one a generated pattern found
      // rather than a corpus record.
      if (flavour(parser) == FLAVOUR_PERL) {
        return GRX_OK;
      }
      // pcre2 allows it on `(*ACCEPT)` alone, which is not an inconsistency:
      // `(*ACCEPT)` is the one verb that ends the match where it stands, so
      // a quantifier on it is unreachable rather than meaningless.
      // pcre2test reports error 109 for the other six, `(*MARK:x)*` and
      // `(*:x)*` among them. Only FAIL was refused here until a corpus
      // record asked about a repeated mark.
      if (atom->a != (uint32_t)GRX_VERB_ACCEPT) {
        return grx_parse_fail(
            parser, GRX_DIAG_NOTHING_TO_REPEAT, offset, length);
      }
      return GRX_OK;

    case GRX_NODE_OPTIONS:
      // `(?i:a)*` is a group and may be repeated; `(?i)*` is not.
      if (!(atom->flags & GRX_NODE_SCOPED)) {
        return grx_parse_fail(
            parser, GRX_DIAG_NOTHING_TO_REPEAT, offset, length);
      }
      return GRX_OK;

    default:
      return GRX_OK;
  }
}

// --------------------------------------------------------------------------
// What only the finished pattern can show
// --------------------------------------------------------------------------

/** Whether the pattern has a capturing group with this name. */
static int group_named(const GRX_Parser * parser, const char * name) {
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

/**
 * Check the references the pattern makes against the groups it has.
 *
 * Done afterwards rather than at each reference because a name may be
 * defined later: `(?&later)(?<later>a)` is a valid subroutine call, and a
 * check at the reference would have to guess.
 */
static GRX_Result pcre_validate(GRX_Parser * parser) {
  for (size_t i = 0; i < parser->pattern->nodes.count; i++) {
    const GRX_Node * node = grx_pattern_node(parser->pattern, (uint32_t)i);
    if (!node) {
      continue;
    }

    int names_group = (node->kind == GRX_NODE_BACKREF
                          || node->kind == GRX_NODE_RECURSE
                          || node->kind == GRX_NODE_CONDITIONAL)
        && (node->flags & GRX_NODE_NAMED);
    if (!names_group) {
      continue;
    }

    uint32_t offset = node->kind == GRX_NODE_CONDITIONAL ? node->b
        : node->kind == GRX_NODE_BACKREF                 ? node->b
                                                         : node->b;
    const char * name = grx_pattern_name(parser->pattern, offset);
    if (!name || !group_named(parser, name)) {
      return grx_parse_fail(parser, GRX_DIAG_UNKNOWN_GROUP_NAME, node->offset,
          node->length);
    }
  }

  return GRX_OK;
}

// --------------------------------------------------------------------------
// The tables
// --------------------------------------------------------------------------

const GRX_Frontend grx_frontend_pcre = {
  .name = "pcre",
  .atom_escape = pcre_atom_escape,
  .class_escape = pcre_class_escape,
  .char_class = pcre_char_class,
  .group_open = pcre_group_open,
  .brace_quantifier = pcre_brace_quantifier,
  .literal_atom = pcre_literal_atom,
  .check_quantifier_target = pcre_check_quantifier_target,
  .skip_ignorable = pcre_skip_ignorable,
  .validate = pcre_validate,
};

// Perl shares every hook. The differences between the two are inside them,
// selected by flavour(), and there are few enough to list: `(?^...)` and the
// charset modifiers, `\N{U+...}`, and what a class escape at the end of a
// range does. A second table with the same entries is still worth having,
// because `grx_frontend_for()` returning one front end for two dialects
// would make "which dialect is this" a question with no answer at the point
// a diagnostic is written.
// Python shares every hook too, and for the same reason PCRE2 and perl do:
// `re` is a Perl-family grammar with a list of differences short enough to
// count, all of them selected by flavour(). What is *not* shared is the
// escape alphabet - python_knows_escape() closes it, where the other two
// dialects read an unknown letter as an identity escape - and that one
// difference is why the list of refusals in this file is longer for Python
// than for either of the others.
const GRX_Frontend grx_frontend_python = {
  .name = "python",
  .atom_escape = pcre_atom_escape,
  .class_escape = pcre_class_escape,
  .char_class = pcre_char_class,
  .group_open = pcre_group_open,
  .brace_quantifier = pcre_brace_quantifier,
  .literal_atom = pcre_literal_atom,
  .check_quantifier_target = pcre_check_quantifier_target,
  .skip_ignorable = pcre_skip_ignorable,
  .validate = pcre_validate,
};

const GRX_Frontend grx_frontend_perl = {
  .name = "perl",
  .atom_escape = pcre_atom_escape,
  .class_escape = pcre_class_escape,
  .char_class = pcre_char_class,
  .group_open = pcre_group_open,
  .brace_quantifier = pcre_brace_quantifier,
  .literal_atom = pcre_literal_atom,
  .check_quantifier_target = pcre_check_quantifier_target,
  .skip_ignorable = pcre_skip_ignorable,
  .validate = pcre_validate,
};
