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
 * The two guaranteed-linear dialects: RE2 (as Go's `regexp` spells it) and
 * the Rust `regex` crate.
 *
 * **Why a file of their own rather than two more flavours in perl.c.** These
 * two are Perl-family *by subtraction*: the grammar is recognisably the same
 * and what defines each dialect is the list of constructs it does not have.
 * perl.c reads a construct and then asks `flavour()` whether this dialect
 * keeps it, which is accept-by-default - a construct nobody remembered to
 * ask about is accepted. For a dialect whose whole character is an absence
 * that is the wrong default, and the failure is silent in the direction that
 * matters: this library would tell a caller their pattern is valid for an
 * engine that rejects it, which documentation/design.md section 4 names as
 * the thing a dialect table exists to stop.
 *
 * So this reader is deny-by-default, the way src/syntax/iregexp.c is. Every
 * construct either appears below or is GRX_DIAG_NOT_IN_DIALECT, and what is
 * *in* each dialect was probed against its reference rather than read off a
 * manual page - `tools/oracle/go_match.go` and `tools/oracle/rust_match.rs`,
 * both pinned in `tools/oracle/containers/IMAGES`, and
 * `make check-oracle-linear` is the differential that keeps this file and
 * those two references from drifting apart.
 *
 * **The guarantee is the point of both dialects, and it is a parse-time
 * guarantee here.** No backreference and no lookaround means no construct
 * that needs a backtracker, so a pattern this front end accepts is one the
 * linear engines can run. That is a property of the dialect table rather
 * than of the engine selection, which is why refusing them here is not
 * merely conformance: it is the promise the dialect makes.
 *
 * **The two are close and are not the same, and the differences are not
 * where a reader would guess.** Each was measured, and each is marked
 * below with the probe that settled it:
 *
 *   `\Q...\E`          RE2 has it; `regex` does not
 *   octal escapes      RE2 has them; `regex` has none at all
 *   `\u`, `\U`         `regex` has both; RE2 has neither
 *   extended mode      `regex` has `(?x)`; RE2 has no `x` flag
 *   class set ops      `regex` has `&&`, `--` and `~~`; RE2 has none
 *   `a**`              `regex` accepts it; RE2 refuses it
 *   duplicate names    RE2 accepts them; `regex` refuses them
 *   `\p{Script=Greek}` `regex` has the qualified form; RE2 has bare names
 *   `\p{^L}`           RE2 has it; `regex` spells that `\P{L}` only
 *   `\b{start}`        `regex` has it; in RE2 `{start}` is literal text
 *   `{` bare           a literal in RE2; an error in `regex`
 *   `\w`, `\d`, `\s`   ASCII in RE2; Unicode in `regex`
 *
 * The last is the one with teeth, and it is a profile cell rather than a
 * syntax one: `\w` matches "é" in the crate and does not in Go. A row that
 * had been read off "both are RE2-shaped" would have been wrong about every
 * subject above U+007F.
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../parse/parse_internal.h"
#include "../unicode/unicode_internal.h"
#include "syntax_internal.h"

/** Which of the two dialects this file is reading. */
typedef enum {
  FLAVOUR_RE2 = 0, ///< Go 1.25 `regexp`, which is RE2's syntax.
  FLAVOUR_RUST     ///< The `regex` crate 1.13.
} Flavour;

/** The dialect being read, as this file's own two-valued question. */
static Flavour flavour(const GRX_Parser * parser) {
  return parser->syntax == GRX_SYNTAX_RUST ? FLAVOUR_RUST : FLAVOUR_RE2;
}

/**
 * The longest property or group name either reference will take.
 *
 * Its own constant rather than perl.c's `GRX_PCRE_NAME_MAX`, which is that
 * file's and names PCRE2's limit. Neither reference here states one; what
 * this bounds is the stack buffer read_property() normalises into, and a
 * name longer than this resolves to no property in any case.
 */
#define RE2_NAME_MAX 128

/** The byte `ahead` past the position, or 0 at the end. */
static char byte_at(const GRX_Parser * parser, size_t ahead) {
  size_t at = parser->position + ahead;
  return at < parser->length ? parser->text[at] : '\0';
}

static int is_decimal(char c) {
  return c >= '0' && c <= '9';
}

static int is_octal(char c) {
  return c >= '0' && c <= '7';
}

static int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

static int is_word_byte(char c) {
  return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
      || (c >= '0' && c <= '9') || c == '_';
}

/**
 * Whether the crate's `(?-u)` is in force, which refuses several atoms.
 *
 * `regex::Regex` matches `&str`, so every match is a run of whole
 * characters. Without `u` a negated set is a set of *bytes*, and a byte set
 * can hold half a character - so the crate refuses the construct rather
 * than producing a match that would slice one. `(?-u)\D`, `(?-u)\W`,
 * `(?-u)\S`, `(?-u)[^a]` and `(?-u).` are each "pattern can match invalid
 * UTF-8" there, and `(?-u)\w` and `(?-u)[a]` are fine. All probed.
 *
 * It is not a fact about the dialect's *grammar* - the same crate accepts
 * every one of them through `regex::bytes::Regex` - which is why it is a
 * condition here and not a feature bit.
 */
static int byte_mode(const GRX_Parser * parser) {
  return flavour(parser) == FLAVOUR_RUST
      && (parser->options & GRX_OPT_ASCII_CLASSES);
}

// --------------------------------------------------------------------------
// Escapes
// --------------------------------------------------------------------------

/**
 * The code point a one-letter escape denotes, or 0 for "not one of these".
 *
 * The C alphabet, and both references have exactly it. **`\v` is the
 * vertical tab and not the vertical-whitespace class**, which is the
 * dialects' agreement with Python's `re` and their disagreement with Perl
 * and PCRE2: `\v` there is a class of six code points. A reader that took
 * Perl's meaning would make `\v` match U+2028 in a dialect where it matches
 * one byte.
 *
 * `\e` is deliberately absent from both. Perl and PCRE2 have it for ESC;
 * `regexp` answers "invalid escape sequence" and so does the crate, both
 * probed.
 */
static uint32_t simple_escape(char c) {
  switch (c) {
    case 'a': return 0x07;
    case 'f': return 0x0C;
    case 'n': return 0x0A;
    case 'r': return 0x0D;
    case 't': return 0x09;
    case 'v': return 0x0B;
    default: return 0;
  }
}

/**
 * The shorthand a letter names, or GRX_SHORTHAND_COUNT for none.
 *
 * The negation is the item's flag rather than one of the enum's `NOT_`
 * members, which is what the rest of this library does with it: lowering
 * reads GRX_CLASS_ITEM_NEGATED and does not read `GRX_SHORTHAND_NOT_SPACE`,
 * so a reader that returned the `NOT_` kind would build a class matching
 * nothing and say so nowhere. It did, and `\S\S` matched no subject at all
 * until linear_diff.py asked.
 *
 * No `\h`, `\H`, `\v` or `\V`: `\v` is the vertical tab here (see
 * simple_escape()) and the other three are errors in both references.
 */
static GRX_ShorthandKind shorthand_for(char c, int * out_negated) {
  *out_negated = 0;
  switch (c) {
    case 'D': *out_negated = 1; /* fall through */
    case 'd': return GRX_SHORTHAND_DIGIT;
    case 'W': *out_negated = 1; /* fall through */
    case 'w': return GRX_SHORTHAND_WORD;
    case 'S': *out_negated = 1; /* fall through */
    case 's': return GRX_SHORTHAND_SPACE;
    default: return GRX_SHORTHAND_COUNT;
  }
}

/**
 * Read the hex digits of `\xHH`, `\x{...}`, `\uHHHH` or `\UHHHHHHHH`.
 *
 * `exact` is how many digits the unbraced form takes; a braced form is read
 * when `braced` is set and the next byte is `{`. Both references refuse a
 * short unbraced form outright - `\x4` is an error in each, probed - rather
 * than reading one digit the way PCRE2 does, so the count is a requirement
 * and not a maximum.
 */
static GRX_Result read_hex_escape(GRX_Parser * parser, size_t start,
    int exact, int braced, uint32_t * out_value) {
  if (braced && byte_at(parser, 0) == '{') {
    parser->position++;
    uint64_t value = 0;
    size_t digits = 0;
    while (hex_value(byte_at(parser, 0)) >= 0) {
      value = value * 16 + (uint64_t)hex_value(byte_at(parser, 0));
      if (value > 0x10FFFF) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_ESCAPE, start,
            parser->position + 1 - start);
      }
      parser->position++;
      digits++;
    }
    if (!digits || !grx_parse_eat(parser, '}')) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_ESCAPE, start,
          parser->position - start);
    }
    *out_value = (uint32_t)value;
    return GRX_OK;
  }

  uint32_t value = 0;
  for (int i = 0; i < exact; i++) {
    int digit = hex_value(byte_at(parser, 0));
    if (digit < 0) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_ESCAPE, start,
          parser->position + 1 - start);
    }
    value = value * 16 + (uint32_t)digit;
    parser->position++;
  }
  if (value > 0x10FFFF) {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_ESCAPE, start, parser->position - start);
  }
  *out_value = value;
  return GRX_OK;
}

/**
 * Read an octal escape, the leading digit not yet consumed.
 *
 * RE2's alone: the crate has no octal at all, and `\0` and `\101` are both
 * "backreferences are not supported" there - which is the crate telling a
 * caller what it thinks a digit after a backslash means.
 *
 * One to three octal digits, so `\0` is NUL and `\101` is "A". `\8` and
 * `\9` are errors rather than the digits they look like, which is what
 * separates this from a backreference the dialect has not got.
 */
static GRX_Result read_octal_escape(
    GRX_Parser * parser, size_t start, uint32_t * out_value) {
  // **A leading `0` stands alone and a leading 1 to 7 does not.** `\0` is
  // NUL, `\10` and `\101` are octal, and `\1` on its own is an error -
  // `regexp` reads a single non-zero digit as a backreference and then
  // refuses it, which is the one place its octal syntax is shaped by a
  // construct it has not got. A reader that took any octal digit as a
  // leading one would make `\1` the SOH character and tell a caller their
  // backreference compiled.
  if (byte_at(parser, 0) != '0' && !is_octal(byte_at(parser, 1))) {
    parser->position++;
    return grx_parse_fail(
        parser, GRX_DIAG_NOT_IN_DIALECT, start, parser->position - start);
  }
  uint32_t value = 0;
  size_t digits = 0;
  while (digits < 3 && is_octal(byte_at(parser, 0))) {
    value = value * 8 + (uint32_t)(byte_at(parser, 0) - '0');
    parser->position++;
    digits++;
  }
  if (!digits) {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_ESCAPE, start, parser->position + 1 - start);
  }
  *out_value = value;
  return GRX_OK;
}

/**
 * Read `\p{...}`, `\P{...}`, `\pL` or `\PL` into a class item.
 *
 * Four spellings between the two dialects and they are not the same four:
 *
 * - The one-letter form `\pL` is in both.
 * - `\p{^L}` is RE2's negation *inside* the braces and is an error in the
 *   crate, which spells it `\P{L}` only. Probed both ways round.
 * - `\p{Script=Greek}` and `\p{gc=L}` are the crate's, and every qualified
 *   form is an error in RE2, which takes bare names alone.
 *
 * The name is resolved through this library's tables at parse time, which
 * is what makes an unknown name a compile error rather than a class that
 * matches nothing. Both references do the same: `\p{Foo}` is an error in
 * each.
 *
 * **The property match mode is the profile's**, which for both of these is
 * GRX_PROPERTY_LOOSE rather than Perl's looser reading: neither reference
 * takes an `In` or `Is` prefix, and `\p{InGreek}` is an error in each -
 * probed, and the reason a `GRX_PROPERTY_LOOSE_PERL` row here would accept
 * two spellings the references refuse.
 */
static GRX_Result read_property(GRX_Parser * parser, int negated,
    size_t start, GRX_ClassItem * out_item) {
  // The name reaches the item as *one* string, `Script=Greek` and all, and
  // lowering splits it on `=` again. That is the shape perl.c's
  // read_property() already uses and the reason this one does not hand the
  // value across separately: two readers writing the item two ways would be
  // one rule in two places.
  //
  // Which is also why the crate's `:` separator is rewritten here. `regex`
  // takes `\p{sc:Greek}` as well as `\p{sc=Greek}` - probed, both accepted -
  // and lowering knows only `=`. Normalising at the one place the spelling
  // is read keeps the second spelling from reaching anything that would have
  // to learn it.
  char normalised[RE2_NAME_MAX + 1];
  const char * name = NULL;
  size_t length = 0;

  if (byte_at(parser, 0) != '{') {
    // `\pL`: exactly one character names the property, in both references.
    // One byte rather than one code point, no property this form can spell
    // being outside ASCII.
    if (grx_parse_at_end(parser)) {
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_PROPERTY_SYNTAX, start, 2);
    }
    name = parser->text + parser->position;
    length = 1;
    parser->position++;
  }
  else {
    parser->position++;
    if (byte_at(parser, 0) == '^') {
      // RE2's in-brace negation, and the crate has not got it: `\p{^L}` is
      // "unrecognized escape sequence" there, where `regexp` reads it as
      // `\P{L}`. It composes with `\P` the way two negations do, which is
      // what the exclusive-or below spells: `\P{^L}` is `\p{L}` in `regexp`.
      if (flavour(parser) == FLAVOUR_RUST) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_PROPERTY_SYNTAX, start,
            parser->position + 1 - start);
      }
      parser->position++;
      negated = !negated;
    }
    size_t first = parser->position;
    while (!grx_parse_at_end(parser) && byte_at(parser, 0) != '}') {
      parser->position++;
    }
    if (!grx_parse_eat(parser, '}')) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_PROPERTY_SYNTAX, start,
          parser->position - start);
    }
    name = parser->text + first;
    length = parser->position - first - 1;
  }

  if (!length || length > RE2_NAME_MAX) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_PROPERTY_SYNTAX, start,
        parser->position - start);
  }

  // The crate's `(?-u)` takes `\p{...}` away with it, which is a rule the
  // other dialects here have no equivalent of: Perl's `/a` and Python's
  // `(?a)` narrow what the shorthands *mean* and leave the property escape
  // alone. `(?-u)\p{L}` is "Unicode-aware ... not allowed" in the crate -
  // it has switched off the machinery the escape needs rather than changed
  // an answer - so this is GRX_DIAG_NOT_IN_DIALECT for that mode rather
  // than an unknown property.
  if (flavour(parser) == FLAVOUR_RUST
      && (parser->options & GRX_OPT_ASCII_CLASSES)) {
    return grx_parse_fail(
        parser, GRX_DIAG_NOT_IN_DIALECT, start, parser->position - start);
  }

  int qualified = 0;
  for (size_t i = 0; i < length; i++) {
    char c = name[i];
    if (c == '=' || c == ':') {
      qualified = 1;
    }
    normalised[i] = c == ':' ? '=' : c;
  }
  normalised[length] = '\0';

  // A qualified name is the crate's alone. `regexp` takes bare names -
  // `\p{Greek}`, `\p{Letter}`, `\p{Nd}` - and answers "invalid character
  // class range" or an unknown-name error for every `name=value` spelling,
  // probed across `Script=`, `sc=` and `gc=`.
  if (qualified && flavour(parser) == FLAVOUR_RE2) {
    return grx_parse_fail(
        parser, GRX_DIAG_UNKNOWN_PROPERTY, start, parser->position - start);
  }

  // Resolved here as well as at lowering, and the answer thrown away, for
  // the reason perl.c's copy of this gives: lowering reports
  // GRX_DIAG_INTERNAL for a name it cannot resolve, on the ground that the
  // parser resolved it already.
  {
    const char * equals = memchr(normalised, '=', length);
    uint32_t property = 0;
    GRX_Result known = equals
        ? grx_unicode_property_lookup(normalised,
              (size_t)(equals - normalised), equals + 1,
              length - (size_t)(equals - normalised) - 1,
              parser->profile.property_match, &property)
        : grx_unicode_property_lookup(normalised, length, NULL, 0,
              parser->profile.property_match, &property);
    if (known != GRX_OK) {
      return grx_parse_fail(
          parser, GRX_DIAG_UNKNOWN_PROPERTY, start, parser->position - start);
    }
  }

  uint32_t offset = GRX_INDEX_NONE;
  if (grx_pattern_add_name(parser->pattern, normalised, length, &offset)
      != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }

  *out_item = (GRX_ClassItem) {
    .kind = GRX_CLASS_ITEM_PROPERTY,
    .flags = negated ? (uint32_t)GRX_CLASS_ITEM_NEGATED : 0u,
    .lo = 0,
    .hi = 0,
    .a = offset,
    .offset = start,
    .length = parser->position - start,
  };
  return GRX_OK;
}

/** Build a zero-width assertion node. */
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

/**
 * Read the crate's `\b{...}`, the `b` already consumed and a `{` standing.
 *
 * The crate's alone, and the difference from Perl's `\b{...}` is not which
 * names each takes but whether the construct exists: in RE2 a `{` after
 * `\b` is the start of ordinary text, so `\b{start}` there is a word
 * boundary followed by the seven characters `{start}` - probed, and the
 * reason this is reached only under FLAVOUR_RUST.
 *
 * `start` and `end` are GNU's `\<` and `\>`, which this library already
 * has. `start-half` and `end-half` are the crate's own weaker pair - a
 * boundary that asks only about the side it names - and nothing here
 * asserts that, so they are refused as a construct rather than accepted as
 * an approximation of one.
 */
static GRX_Result read_bound_type(
    GRX_Parser * parser, size_t start, uint32_t * out_node) {
  size_t first = parser->position + 1;
  size_t at = first;
  while (at < parser->length && parser->text[at] != '}') {
    at++;
  }
  if (at >= parser->length) {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_ESCAPE, start, parser->length - start);
  }
  size_t length = at - first;
  const char * name = parser->text + first;
  parser->position = at + 1;

  if (length == 5 && memcmp(name, "start", 5) == 0) {
    return anchor_node(parser, GRX_ANCHOR_WORD_START, start, out_node);
  }
  if (length == 3 && memcmp(name, "end", 3) == 0) {
    return anchor_node(parser, GRX_ANCHOR_WORD_END, start, out_node);
  }
  if ((length == 10 && memcmp(name, "start-half", 10) == 0)
      || (length == 8 && memcmp(name, "end-half", 8) == 0)) {
    return grx_parse_fail(parser, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, start,
        parser->position - start);
  }

  // **A `{` after `\b` that names no bound type is a quantifier**, not an
  // error: `\b{1,2}` compiles in the crate and repeats the assertion, where
  // `\b{}` and `\b{g}` are errors. So the name is tried first and the
  // position rewound when it is not one, which leaves the `{` for the
  // shared parser to read as a repeat - and leaves the two error cases as
  // errors, because neither is a repeat either.
  parser->position = first - 1;
  return anchor_node(parser, GRX_ANCHOR_WORD_BOUNDARY, start, out_node);
}

/**
 * Read an escape outside a bracket expression, the `\` already consumed.
 *
 * Deny-by-default: what is not named here is GRX_DIAG_INVALID_ESCAPE, which
 * is what both references answer for an unknown letter. **There is no
 * identity-escape rule for letters**, which is the trap a reader coming from
 * Perl walks into - `\y` is "y" there and an error in each of these - and
 * there *is* one for punctuation: `\-`, `\%`, `\#` and `\ ` are all accepted
 * by both, probed.
 */
static GRX_Result re2_atom_escape(GRX_Parser * parser, uint32_t * out_node) {
  size_t start = parser->position - 1; // The backslash.
  if (grx_parse_at_end(parser)) {
    return grx_parse_fail(parser, GRX_DIAG_TRAILING_BACKSLASH, start, 1);
  }
  char c = byte_at(parser, 0);

  uint32_t simple = simple_escape(c);
  if (simple) {
    parser->position++;
    return grx_parse_literal_node(
        parser, simple, start, parser->position - start, out_node);
  }

  int negated = 0;
  GRX_ShorthandKind shorthand = shorthand_for(c, &negated);
  if (shorthand != GRX_SHORTHAND_COUNT) {
    parser->position++;
    if (negated && byte_mode(parser)) {
      return grx_parse_fail(
          parser, GRX_DIAG_NOT_IN_DIALECT, start, parser->position - start);
    }
    return grx_parse_shorthand_node(
        parser, shorthand, negated, start, parser->position - start, out_node);
  }

  if (c == 'p' || c == 'P') {
    parser->position++;
    GRX_ClassItem item = {GRX_CLASS_ITEM_SINGLE, 0, 0, 0, 0, 0, 0};
    GRX_Result result = read_property(parser, c == 'P', start, &item);
    if (result != GRX_OK) {
      return result;
    }
    result = grx_parse_class_node(parser, start, out_node);
    if (result != GRX_OK) {
      return result;
    }
    result = grx_parse_class_add(parser, *out_node, &item);
    if (result != GRX_OK) {
      return result;
    }
    grx_pattern_node(parser->pattern, *out_node)->length
        = parser->position - start;
    return GRX_OK;
  }

  if (c == 'b' || c == 'B') {
    parser->position++;
    if (c == 'b' && flavour(parser) == FLAVOUR_RUST
        && byte_at(parser, 0) == '{') {
      return read_bound_type(parser, start, out_node);
    }
    return anchor_node(parser,
        c == 'b' ? GRX_ANCHOR_WORD_BOUNDARY : GRX_ANCHOR_NOT_WORD_BOUNDARY,
        start, out_node);
  }

  // `\A` and `\z`, and **no `\Z`**. Perl, PCRE2 and Python all have the
  // third; `regexp` answers "invalid escape sequence" for it and so does
  // the crate, both probed. A front end that accepted it would be offering
  // an end-of-subject assertion that behaves differently from the one the
  // dialect has, under a spelling neither reference compiles.
  if (c == 'A') {
    parser->position++;
    return anchor_node(parser, GRX_ANCHOR_START_SUBJECT, start, out_node);
  }
  if (c == 'z') {
    parser->position++;
    return anchor_node(parser, GRX_ANCHOR_END_SUBJECT, start, out_node);
  }

  if (c == 'x') {
    parser->position++;
    uint32_t value = 0;
    GRX_Result result = read_hex_escape(parser, start, 2, 1, &value);
    if (result != GRX_OK) {
      return result;
    }
    return grx_parse_literal_node(
        parser, value, start, parser->position - start, out_node);
  }

  // `\uHHHH`, `\u{...}`, `\UHHHHHHHH` and `\U{...}` are the crate's, all
  // four probed; `regexp` has none of them and reads `\u` as an unknown
  // escape.
  if (flavour(parser) == FLAVOUR_RUST && (c == 'u' || c == 'U')) {
    parser->position++;
    uint32_t value = 0;
    GRX_Result result
        = read_hex_escape(parser, start, c == 'u' ? 4 : 8, 1, &value);
    if (result != GRX_OK) {
      return result;
    }
    return grx_parse_literal_node(
        parser, value, start, parser->position - start, out_node);
  }

  // Octal is RE2's and the crate has none: `\0` and `\101` are both
  // "backreferences are not supported" there, which is the crate saying
  // what it thinks a digit after a backslash is.
  if (flavour(parser) == FLAVOUR_RE2 && is_octal(c)) {
    uint32_t value = 0;
    GRX_Result result = read_octal_escape(parser, start, &value);
    if (result != GRX_OK) {
      return result;
    }
    return grx_parse_literal_node(
        parser, value, start, parser->position - start, out_node);
  }

  // A digit that is not octal, in either dialect. `\8`, `\9` and `\1` are
  // refused with the backreference diagnostic rather than the escape one,
  // because a caller who wrote `\1` has written a backreference and telling
  // them the escape is unknown would send them looking for the wrong thing.
  // That this dialect has no backreferences at all is the whole of why.
  if (is_decimal(c)) {
    parser->position++;
    return grx_parse_fail(
        parser, GRX_DIAG_NOT_IN_DIALECT, start, parser->position - start);
  }

  // `\Q...\E` is RE2's and the crate has neither letter. The run itself is
  // opened in re2_skip_ignorable(), which is where the parser looks for
  // one, so neither letter reaches here under RE2 - and under the crate
  // both are unknown escapes, which is what it answers.
  if ((c == 'Q' || c == 'E') && flavour(parser) == FLAVOUR_RUST) {
    parser->position++;
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_ESCAPE, start, parser->position - start);
  }

  // The identity escapes. Both references take a backslash before any
  // non-word ASCII byte and refuse it before a letter or digit, which is
  // the rule rather than a list: `\-`, `\.`, `\/`, `\#`, `\$`, `\^` and a
  // backslash before a space are all accepted, `\y` is not.
  if (!is_word_byte(c) && (unsigned char)c < 0x80) {
    parser->position++;
    return grx_parse_literal_node(parser, (uint32_t)(unsigned char)c, start,
        parser->position - start, out_node);
  }

  parser->position++;
  return grx_parse_fail(
      parser, GRX_DIAG_INVALID_ESCAPE, start, parser->position - start);
}

/**
 * Read an escape inside a bracket expression, the `\` already consumed.
 *
 * The same alphabet minus the assertions, which are three refusals and not
 * one class of refusal: `[\b]` is an error in both references - it is not
 * the backspace character it would be in ECMAScript - and so are `[\A]` and
 * `[\z]`. `\Q` is refused inside a class in RE2 as well, probed: `[\Q]\E]`
 * does not compile there.
 */
static GRX_Result re2_class_escape(
    GRX_Parser * parser, GRX_ClassItem * out_item) {
  size_t start = parser->position - 1; // The backslash.
  if (grx_parse_at_end(parser)) {
    return grx_parse_fail(parser, GRX_DIAG_TRAILING_BACKSLASH, start, 1);
  }
  char c = byte_at(parser, 0);

  uint32_t simple = simple_escape(c);
  if (simple) {
    parser->position++;
    *out_item = (GRX_ClassItem) {
      .kind = GRX_CLASS_ITEM_SINGLE, .flags = 0, .lo = simple, .hi = simple,
      .a = 0, .offset = start, .length = parser->position - start,
    };
    return GRX_OK;
  }

  int negated = 0;
  GRX_ShorthandKind shorthand = shorthand_for(c, &negated);
  if (shorthand != GRX_SHORTHAND_COUNT) {
    parser->position++;
    // A negated set inside a class is a negated set: `(?-u)[\D]` and
    // `(?-u)[[:^alpha:]]` are refused by the crate exactly as `(?-u)\D` is.
    // See byte_mode().
    if (negated && byte_mode(parser)) {
      return grx_parse_fail(
          parser, GRX_DIAG_NOT_IN_DIALECT, start, parser->position - start);
    }
    *out_item = (GRX_ClassItem) {
      .kind = GRX_CLASS_ITEM_SHORTHAND,
      .flags = negated ? (uint32_t)GRX_CLASS_ITEM_NEGATED : 0u,
      .lo = 0, .hi = 0,
      .a = (uint32_t)shorthand, .offset = start,
      .length = parser->position - start,
    };
    return GRX_OK;
  }

  if (c == 'p' || c == 'P') {
    parser->position++;
    return read_property(parser, c == 'P', start, out_item);
  }

  if (c == 'x') {
    parser->position++;
    uint32_t value = 0;
    GRX_Result result = read_hex_escape(parser, start, 2, 1, &value);
    if (result != GRX_OK) {
      return result;
    }
    *out_item = (GRX_ClassItem) {
      .kind = GRX_CLASS_ITEM_SINGLE, .flags = 0, .lo = value, .hi = value,
      .a = 0, .offset = start, .length = parser->position - start,
    };
    return GRX_OK;
  }

  if (flavour(parser) == FLAVOUR_RUST && (c == 'u' || c == 'U')) {
    parser->position++;
    uint32_t value = 0;
    GRX_Result result
        = read_hex_escape(parser, start, c == 'u' ? 4 : 8, 1, &value);
    if (result != GRX_OK) {
      return result;
    }
    *out_item = (GRX_ClassItem) {
      .kind = GRX_CLASS_ITEM_SINGLE, .flags = 0, .lo = value, .hi = value,
      .a = 0, .offset = start, .length = parser->position - start,
    };
    return GRX_OK;
  }

  if (flavour(parser) == FLAVOUR_RE2 && is_octal(c)) {
    uint32_t value = 0;
    GRX_Result result = read_octal_escape(parser, start, &value);
    if (result != GRX_OK) {
      return result;
    }
    *out_item = (GRX_ClassItem) {
      .kind = GRX_CLASS_ITEM_SINGLE, .flags = 0, .lo = value, .hi = value,
      .a = 0, .offset = start, .length = parser->position - start,
    };
    return GRX_OK;
  }

  if (!is_word_byte(c) && (unsigned char)c < 0x80) {
    parser->position++;
    *out_item = (GRX_ClassItem) {
      .kind = GRX_CLASS_ITEM_SINGLE, .flags = 0,
      .lo = (uint32_t)(unsigned char)c, .hi = (uint32_t)(unsigned char)c,
      .a = 0, .offset = start, .length = parser->position - start,
    };
    return GRX_OK;
  }

  parser->position++;
  return grx_parse_fail(
      parser, GRX_DIAG_INVALID_ESCAPE, start, parser->position - start);
}

// --------------------------------------------------------------------------
// Bracket expressions
// --------------------------------------------------------------------------

/**
 * The fourteen POSIX class names, and both references have exactly these.
 *
 * `word` is in the list, which POSIX itself has not got: both dialects
 * inherited it from Perl, and both answer `[[:word:]]` with the word set.
 * `[[:^alpha:]]` is the negated spelling and is in both too, probed.
 *
 * **They are ASCII in both dialects**, which is not a syntax fact and is
 * why it is not decided here: `[[:alpha:]]` does not match "é" in either
 * reference, and GRX_Profile::posix_wide_general_category on each row is
 * what says so.
 */
static const char * const posix_class_names[] = {
  "alnum", "alpha", "ascii", "blank", "cntrl", "digit", "graph", "lower",
  "print", "punct", "space", "upper", "word", "xdigit", NULL,
};

/**
 * Read `[:name:]` standing at the position, or report that none does.
 *
 * Returns GRX_ERR_SYNTAX with no diagnostic set when the `[` is not the
 * start of one, so that the caller can fall back to reading a literal `[`,
 * which is what RE2 does with it.
 */
static GRX_Result read_posix_class(
    GRX_Parser * parser, GRX_ClassItem * out_item) {
  size_t start = parser->position;
  size_t at = start + 2;
  int negated = 0;
  if (at < parser->length && parser->text[at] == '^') {
    negated = 1;
    at++;
  }
  size_t first = at;
  while (at + 1 < parser->length
      && !(parser->text[at] == ':' && parser->text[at + 1] == ']')) {
    at++;
  }
  const char * name = parser->text + first;
  size_t length = at - first;
  int known = 0;
  for (size_t i = 0; posix_class_names[i]; i++) {
    if (strlen(posix_class_names[i]) == length
        && memcmp(posix_class_names[i], name, length) == 0) {
      known = 1;
      break;
    }
  }
  if (!known) {
    // **The two references part company here**, probed rather than assumed -
    // this comment first said "an error in both" and was wrong about one of
    // them. `[[:nosuch:]]` is "invalid character class range" in `regexp`
    // and compiles in the crate, which reads `[:nosuch:]` as ordinary
    // members once the name is not one it knows. Returning GRX_ERR_SYNTAX
    // with no diagnostic is how the caller is told to do that.
    if (flavour(parser) == FLAVOUR_RUST) {
      return GRX_ERR_SYNTAX;
    }
    parser->position = at + 2;
    return grx_parse_fail(
        parser, GRX_DIAG_UNKNOWN_POSIX_CLASS, start, parser->position - start);
  }

  if (negated && byte_mode(parser)) {
    parser->position = at + 2;
    return grx_parse_fail(
        parser, GRX_DIAG_NOT_IN_DIALECT, start, parser->position - start);
  }

  uint32_t offset = GRX_INDEX_NONE;
  if (grx_pattern_add_name(parser->pattern, name, length, &offset) != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }
  parser->position = at + 2;
  *out_item = (GRX_ClassItem) {
    .kind = GRX_CLASS_ITEM_POSIX,
    .flags = negated ? (uint32_t)GRX_CLASS_ITEM_NEGATED : 0u,
    .lo = 0, .hi = 0, .a = offset, .offset = start,
    .length = parser->position - start,
  };
  return GRX_OK;
}

/**
 * Whether a POSIX class stands here, without consuming anything.
 *
 * Needed because the crate's fall-back is not "the `[` is a member" but
 * "the `[` opens a nested class", and the two readings of `[[:x:]]` have to
 * be chosen before either is taken. Asking read_posix_class() would either
 * consume or report a diagnostic, neither of which a look-ahead may do.
 */
static int at_posix_class(const GRX_Parser * parser) {
  if (byte_at(parser, 0) != '[' || byte_at(parser, 1) != ':') {
    return 0;
  }
  size_t at = parser->position + 2;
  if (at < parser->length && parser->text[at] == '^') {
    at++;
  }
  size_t first = at;
  while (at + 1 < parser->length
      && !(parser->text[at] == ':' && parser->text[at + 1] == ']')) {
    at++;
  }
  if (at + 1 >= parser->length) {
    return 0;
  }
  size_t length = at - first;
  for (size_t i = 0; posix_class_names[i]; i++) {
    if (strlen(posix_class_names[i]) == length
        && memcmp(posix_class_names[i], parser->text + first, length) == 0) {
      return 1;
    }
  }
  // Under RE2 an unknown name is still a POSIX class - a malformed one,
  // which read_posix_class() reports. Only the crate falls back.
  return flavour(parser) != FLAVOUR_RUST;
}

/** Whether a class item is one code point, and so may end a range. */
static int is_single(const GRX_ClassItem * item) {
  return item->kind == GRX_CLASS_ITEM_SINGLE;
}

/** Whether the two bytes here are one of the crate's set operators. */
static int class_operator(const GRX_Parser * parser, GRX_ClassOpKind * out_op) {
  char c = byte_at(parser, 0);
  if (byte_at(parser, 1) != c) {
    return 0;
  }
  switch (c) {
    case '&': *out_op = GRX_CLASS_OP_INTERSECT; return 1;
    case '-': *out_op = GRX_CLASS_OP_SUBTRACT; return 1;
    case '~': *out_op = GRX_CLASS_OP_SYMDIFF; return 1;
    case '|': *out_op = GRX_CLASS_OP_UNION; return 1;
    default: return 0;
  }
}

static GRX_Result read_class_expression(
    GRX_Parser * parser, size_t start, uint32_t * out_node);
static GRX_Result read_class_operand(
    GRX_Parser * parser, size_t start, uint32_t * out_node);

/**
 * One operand: a run of members, or a nested `[...]` under the crate.
 *
 * A run ends at the `]` that closes the class, or at a set operator - which
 * is what makes `[ab~~bc]` the symmetric difference of two two-member sets
 * rather than one six-member one. Probed: it matches "a" and not "b".
 */
static GRX_Result read_class_operand(
    GRX_Parser * parser, size_t start, uint32_t * out_node) {
  int rust = flavour(parser) == FLAVOUR_RUST;

  GRX_Result result = grx_parse_class_node(parser, start, out_node);
  if (result != GRX_OK) {
    return result;
  }

  int first = 1;
  for (;;) {
    GRX_ClassOpKind op = GRX_CLASS_OP_UNION;
    if (grx_parse_at_end(parser)) {
      return grx_parse_fail(
          parser, GRX_DIAG_UNMATCHED_OPEN_BRACKET, start, parser->position - start);
    }
    if (byte_at(parser, 0) == ']') {
      break;
    }
    if (rust && class_operator(parser, &op)) {
      break;
    }
    if (rust && byte_at(parser, 0) == '[' && !at_posix_class(parser)) {
      // A nested class standing beside members, as in `[a[b-c]]`: the run
      // ends here and read_class_union() joins the two. It cannot be a
      // member: a class node names a *span* of the item table, so opening
      // a second class node in the middle of a run interleaves the two
      // nodes' items and grx_parse_class_add() answers GRX_ERR_INTERNAL.
      break;
    }

    size_t item_start = parser->position;
    GRX_ClassItem item = {GRX_CLASS_ITEM_SINGLE, 0, 0, 0, 0, 0, 0};

    // **Asked before it is read, and not by reading it and looking at the
    // code.** `grx_parse_fail()` returns GRX_ERR_SYNTAX, which is also how
    // read_posix_class() used to say "no POSIX class stands here" - so a
    // *malformed* one under RE2 reported its diagnostic and was then read
    // as literal members anyway. `[[:word(?>a):]]` alone happened to fail
    // for a second reason and `\d\p{Greek}[[:word(?>a):]]*[\x41-\x5A]`
    // did not, which is why a two-atom pattern found it and a one-atom one
    // did not.
    if (at_posix_class(parser)) {
      result = read_posix_class(parser, &item);
      if (result != GRX_OK) {
        return result;
      }
      result = grx_parse_class_add(parser, *out_node, &item);
      if (result != GRX_OK) {
        return result;
      }
      first = 0;
      continue;
    }

    if (byte_at(parser, 0) == '\\') {
      parser->position++;
      result = re2_class_escape(parser, &item);
      if (result != GRX_OK) {
        return result;
      }
    }
    else {
      uint32_t low = 0;
      result = grx_parse_take(parser, &low);
      if (result != GRX_OK) {
        return result;
      }
      item = (GRX_ClassItem) {
        .kind = GRX_CLASS_ITEM_SINGLE, .flags = 0, .lo = low, .hi = low,
        .a = 0, .offset = item_start, .length = parser->position - item_start,
      };
    }

    // A range, when a `-` follows a single member and the `-` is not the
    // one closing the class or the head of a `--` operator.
    if (is_single(&item) && byte_at(parser, 0) == '-'
        && byte_at(parser, 1) != ']'
        && !(rust && byte_at(parser, 1) == '-')
        && parser->position + 1 < parser->length) {
      size_t dash = parser->position;
      parser->position++;
      GRX_ClassItem high = {GRX_CLASS_ITEM_SINGLE, 0, 0, 0, 0, 0, 0};
      if (byte_at(parser, 0) == '\\') {
        parser->position++;
        result = re2_class_escape(parser, &high);
        if (result != GRX_OK) {
          return result;
        }
        if (!is_single(&high)) {
          // `[a-\d]`. An error in both references, which is the opposite of
          // Perl's "False [] range" union and the reason
          // GRX_SyntaxSpec::false_range_is_union is clear on both rows.
          return grx_parse_fail(parser, GRX_DIAG_CLASS_ESCAPE_IN_RANGE, dash,
              parser->position - dash);
        }
      }
      else {
        uint32_t value = 0;
        result = grx_parse_take(parser, &value);
        if (result != GRX_OK) {
          return result;
        }
        high.lo = value;
      }
      if (high.lo < item.lo) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_RANGE, item_start,
            parser->position - item_start);
      }
      item.kind = GRX_CLASS_ITEM_RANGE;
      item.hi = high.lo;
      item.length = parser->position - item_start;
    }
    else if (rust
        && (item.kind == GRX_CLASS_ITEM_SHORTHAND
            || item.kind == GRX_CLASS_ITEM_PROPERTY)
        && byte_at(parser, 0) == '-' && byte_at(parser, 1) != ']'
        && byte_at(parser, 1) != '-') {
      // `[\d-x]`. RE2 reads the `-` as a member and the crate refuses the
      // whole thing, which is the one place the two disagree about a class
      // that is not a set operation - probed both ways.
      //
      // **A shorthand or a property, and not every item that is not one
      // code point.** `[[a-c]-x]` and `[[:alpha:]-x]` both compile in the
      // crate, so a nested class and a POSIX class leave the `-` an
      // ordinary member; it is the escape alone that does not.
      return grx_parse_fail(parser, GRX_DIAG_CLASS_ESCAPE_IN_RANGE,
          item_start, parser->position + 1 - item_start);
    }

    result = grx_parse_class_add(parser, *out_node, &item);
    if (result != GRX_OK) {
      return result;
    }
    first = 0;
  }

  (void)first;
  grx_pattern_node(parser->pattern, *out_node)->length
      = parser->position - start;
  return GRX_OK;
}

/**
 * One operand: a union of member runs and nested classes.
 *
 * **Union binds tighter than the four operators**, which is the level this
 * function is. `[a-z--[aeiou]{]` is the case that shows it: the crate reads
 * the right-hand side as `[aeiou]` *and* `{`, so the `{` is subtracted and
 * the class does not match it. A reader that joined the `{` at the operator
 * level would compute `(a-z -- [aeiou]) | {` instead and match it - which
 * is what this one did until the differential asked.
 *
 * A union of one part is that part, so the common case builds no extra
 * node.
 */
static GRX_Result read_class_union(
    GRX_Parser * parser, size_t start, uint32_t * out_node) {
  int rust = flavour(parser) == FLAVOUR_RUST;
  GRX_Result result;

  if (rust && byte_at(parser, 0) == '[' && !at_posix_class(parser)) {
    size_t nested = parser->position;
    parser->position++;
    result = read_class_expression(parser, nested, out_node);
  }
  else {
    result = read_class_operand(parser, start, out_node);
  }
  if (result != GRX_OK) {
    return result;
  }

  GRX_ClassOpKind ignored = GRX_CLASS_OP_UNION;
  while (rust && !grx_parse_at_end(parser) && byte_at(parser, 0) != ']'
      && !class_operator(parser, &ignored)) {
    uint32_t right = GRX_INDEX_NONE;
    if (byte_at(parser, 0) == '[' && !at_posix_class(parser)) {
      size_t nested = parser->position;
      parser->position++;
      result = read_class_expression(parser, nested, &right);
    }
    else {
      result = read_class_operand(parser, parser->position, &right);
    }
    if (result != GRX_OK) {
      return result;
    }
    uint32_t combined = GRX_INDEX_NONE;
    if (grx_pattern_add_node(parser->pattern, GRX_NODE_CLASS_OP, start,
            parser->position - start, &combined)
            != GRX_OK
        || grx_pattern_add_child(parser->pattern, combined, *out_node) != GRX_OK
        || grx_pattern_add_child(parser->pattern, combined, right) != GRX_OK) {
      return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
    }
    grx_pattern_node(parser->pattern, combined)->a
        = (uint32_t)GRX_CLASS_OP_UNION;
    *out_node = combined;
  }
  return GRX_OK;
}

/**
 * A whole bracket expression's contents, the `[` consumed, up to its `]`.
 *
 * One precedence level, left-associative, which is what the probe says
 * rather than what the crate's documentation implies: `[a-c--b&&a]` matches
 * "a" and not "c", which is `(a-c -- b) && a`. A reader that gave `&&` a
 * tighter binding would compute `a-c -- (b&&a)`, which is all three.
 *
 * Under RE2 there are no operators, so this reads exactly one operand.
 */
static GRX_Result read_class_expression(
    GRX_Parser * parser, size_t start, uint32_t * out_node) {
  int negated = grx_parse_eat(parser, '^');

  if (byte_at(parser, 0) == ']') {
    // `[]` and `[^]` are errors in both references - neither is an empty
    // class and neither is a class holding `]`, which is what ECMAScript
    // and Perl respectively make of them.
    parser->position++;
    return grx_parse_fail(
        parser, GRX_DIAG_EMPTY_CLASS, start, parser->position - start);
  }

  GRX_Result result = read_class_union(parser, start, out_node);
  if (result != GRX_OK) {
    return result;
  }

  // Operands until the `]`, joined by the explicit operators alone: union
  // is read one level down, in read_class_union(), because it binds
  // tighter. One precedence level here and left-associative, which
  // `[a-c--b&&a]` is the only case that shows.
  GRX_ClassOpKind op = GRX_CLASS_OP_UNION;
  while (flavour(parser) == FLAVOUR_RUST && class_operator(parser, &op)) {
    parser->position += 2;
    uint32_t right = GRX_INDEX_NONE;
    result = read_class_union(parser, parser->position, &right);
    if (result != GRX_OK) {
      return result;
    }
    uint32_t combined = GRX_INDEX_NONE;
    result = grx_pattern_add_node(parser->pattern, GRX_NODE_CLASS_OP, start,
        parser->position - start, &combined);
    if (result != GRX_OK) {
      return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
    }
    grx_pattern_node(parser->pattern, combined)->a = (uint32_t)op;
    if (grx_pattern_add_child(parser->pattern, combined, *out_node) != GRX_OK
        || grx_pattern_add_child(parser->pattern, combined, right) != GRX_OK) {
      return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
    }
    *out_node = combined;
  }

  if (!grx_parse_eat(parser, ']')) {
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_BRACKET, start,
        parser->position - start);
  }
  if (negated) {
    if (byte_mode(parser)) {
      return grx_parse_fail(
          parser, GRX_DIAG_NOT_IN_DIALECT, start, parser->position - start);
    }
    // **A set expression is complemented by an operation, not by a flag**,
    // which is what GRX_NODE_NEGATED is on a class node. `[^a--b]` matches
    // "b" in the crate - the `^` takes the complement of `a--b`, which is
    // `{a}` - and setting the flag on the CLASS_OP node did nothing at all,
    // so it matched "a" instead. The differential could not see it: nothing
    // in its vocabulary spelled a negated class with an operator in it.
    if (grx_pattern_node(parser->pattern, *out_node)->kind
        == GRX_NODE_CLASS_OP) {
      uint32_t complement = GRX_INDEX_NONE;
      if (grx_pattern_add_node(parser->pattern, GRX_NODE_CLASS_OP, start,
              parser->position - start, &complement)
              != GRX_OK
          || grx_pattern_add_child(parser->pattern, complement, *out_node)
              != GRX_OK) {
        return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
      }
      grx_pattern_node(parser->pattern, complement)->a
          = (uint32_t)GRX_CLASS_OP_COMPLEMENT;
      *out_node = complement;
    }
    else {
      grx_pattern_node(parser->pattern, *out_node)->flags |= GRX_NODE_NEGATED;
    }
  }
  grx_pattern_node(parser->pattern, *out_node)->length
      = parser->position - start;
  return GRX_OK;
}

/** Read a bracket expression, the `[` already consumed. */
static GRX_Result re2_char_class(GRX_Parser * parser, uint32_t * out_node) {
  return read_class_expression(parser, parser->position - 1, out_node);
}

// --------------------------------------------------------------------------
// Groups
// --------------------------------------------------------------------------

/**
 * The option a flag letter sets, or 0 for a letter this dialect has not got.
 *
 * Four letters in RE2 and seven in the crate, and the sets are not nested
 * the way "one is a subset of the other" would suggest - they are, but the
 * *meaning* of one of them is not shared. `u` is the crate's Unicode
 * switch, which is on by default and which `(?-u)` turns off: it is the
 * only flag here whose interesting spelling is the clearing one, and what
 * it clears is the width of `\w`, `\d`, `\s`, `.` and the POSIX classes.
 * GRX_OPT_ASCII_CLASSES is this library's name for that, so `u` is
 * *inverted* on the way in - set-`u` clears it and clear-`u` sets it - and
 * flag_option() reports the inversion rather than performing it, because
 * the caller is what knows which side of the `-` the letter was on.
 */
static uint32_t flag_option(const GRX_Parser * parser, char c, int * inverted) {
  *inverted = 0;
  switch (c) {
    case 'i': return GRX_OPT_CASELESS;
    case 'm': return GRX_OPT_MULTILINE;
    case 's': return GRX_OPT_DOTALL;
    case 'U': return GRX_OPT_UNGREEDY;
    default: break;
  }
  if (flavour(parser) != FLAVOUR_RUST) {
    return 0;
  }
  switch (c) {
    case 'x': return GRX_OPT_EXTENDED;
    case 'u': *inverted = 1; return GRX_OPT_ASCII_CLASSES;
    default: return 0;
  }
}

/**
 * Read the letters of `(?...)` or `(?...:`, the `?` already consumed.
 *
 * Both references take a `-` once and refuse an empty setting: `(?)` is an
 * error in each, and so is `(?-)`.
 */
static GRX_Result read_flags(GRX_Parser * parser, size_t start,
    uint32_t * out_set, uint32_t * out_clear) {
  int clearing = 0;
  size_t letters = 0;

  for (;;) {
    char c = byte_at(parser, 0);
    if (c == ':' || c == ')') {
      break;
    }
    if (c == '-') {
      if (clearing) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
            parser->position + 1 - start);
      }
      clearing = 1;
      parser->position++;
      continue;
    }
    // The crate's CRLF mode, which is a line-terminator set rather than an
    // option bit - so it lands where PCRE2's `(*CRLF)` lands, on the
    // pattern. GRX_NEWLINES_ANYCRLF is `\r`, `\n` or the pair, which is
    // what the crate documents `R` as and what it answers: `(?mR)^b`
    // matches after a CRLF and `(?R).` does not match a bare `\r`.
    //
    // It is the one flag here that cannot be *cleared* per group, because
    // the set belongs to the pattern; `(?-R)` is accepted and restores the
    // default for the same reason the last `(*CRLF)` wins in PCRE2.
    if (flavour(parser) == FLAVOUR_RUST && c == 'R') {
      parser->position++;
      letters++;
      parser->pattern->newlines
          = clearing ? GRX_NEWLINES_LF : GRX_NEWLINES_ANYCRLF;
      continue;
    }
    int inverted = 0;
    uint32_t option = flag_option(parser, c, &inverted);
    if (!option) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
          parser->position + 1 - start);
    }
    parser->position++;
    letters++;
    if (clearing != inverted) {
      *out_clear |= option;
      *out_set &= ~option;
    }
    else {
      *out_set |= option;
      *out_clear &= ~option;
    }
  }

  if (!letters) {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start, parser->position - start);
  }
  return GRX_OK;
}

/** Read a group name and its terminator, the opener already consumed. */
static GRX_Result read_group_name(GRX_Parser * parser, size_t start,
    char terminator, uint32_t * out_offset) {
  size_t first = parser->position;
  while (!grx_parse_at_end(parser) && byte_at(parser, 0) != terminator) {
    parser->position++;
  }
  size_t length = parser->position - first;
  if (!length || length > RE2_NAME_MAX) {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_GROUP_NAME, start, parser->position - start);
  }
  if (!grx_parse_eat(parser, terminator)) {
    return grx_parse_fail(
        parser, GRX_DIAG_UNTERMINATED_NAME, start, parser->position - start);
  }
  // **Both restrict the characters and they restrict them differently**,
  // which took four probes to separate: `(?P<1a>x)` compiles in `regexp`
  // and not in the crate, `(?P<\u00e9>x)` compiles in the crate and not in
  // `regexp`, and `(?P<a b>x)`, `(?P<a-b>x)` and `(?P<>x)` are refused by
  // both.
  //
  // So RE2's name is ASCII word bytes, any of them first; the crate's is
  // *Unicode* word characters and may not begin with a digit. A byte above
  // 0x7F is taken on trust here rather than resolved: what would decide it
  // is Unicode's Word property, and a name that names no group is refused
  // later anyway.
  for (size_t i = 0; i < length; i++) {
    unsigned char c = (unsigned char)parser->text[first + i];
    if (c >= 0x80) {
      if (flavour(parser) == FLAVOUR_RUST) {
        continue;
      }
      return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_NAME, start,
          parser->position - start);
    }
    if (!is_word_byte((char)c)
        || (i == 0 && flavour(parser) == FLAVOUR_RUST
            && is_decimal((char)c))) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_NAME, start,
          parser->position - start);
    }
  }
  if (grx_pattern_add_name(parser->pattern, parser->text + first, length,
          out_offset)
      != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }
  return GRX_OK;
}

/** Whether a group with this name has already been opened. */
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
    const char * other = grx_pattern_name(parser->pattern, node->b);
    if (other && strcmp(other, name) == 0) {
      return 1;
    }
  }
  return 0;
}

/**
 * Read a group opener, the `(` already consumed.
 *
 * Four forms and no fifth: a capturing group, `(?:`, a named group in the
 * two spellings each dialect takes, and an option setting. Everything else
 * is GRX_DIAG_NOT_IN_DIALECT, which is the deny-by-default this file exists
 * for - `(?=`, `(?!`, `(?<=`, `(?<!`, `(?>`, `(?#`, `(?(`, `(?R)`, `(?1)`,
 * `(?&n)`, `(?P=n)`, `(?P>n)`, `(?|`, `(?C` and every `(*VERB)` are all
 * refused, each of them probed against both references first.
 *
 * `(?=` and `(?<=` are worth naming among those because refusing them is
 * the dialect's promise rather than its pedantry: a pattern these two
 * accept is one the linear engines can run, and lookaround is one of the
 * two constructs that would break it.
 */
static GRX_Result re2_group_open(
    GRX_Parser * parser, GRX_GroupOpen * out_open) {
  size_t start = parser->position - 1; // The `(`.

  if (!grx_parse_eat(parser, '?')) {
    if (parser->options & GRX_OPT_NO_CAPTURE) {
      return GRX_OK;
    }
    parser->groups_opened++;
    out_open->flags = GRX_NODE_CAPTURING;
    out_open->a = (uint32_t)parser->groups_opened;
    return GRX_OK;
  }

  char c = byte_at(parser, 0);

  if (c == ':') {
    parser->position++;
    return GRX_OK;
  }

  // `(?P<n>`, and `(?<n>` which both references also take. `(?'n'` is an
  // error in each, so the third Perl spelling is not here.
  if ((c == '<' && byte_at(parser, 1) != '=' && byte_at(parser, 1) != '!')
      || (c == 'P' && byte_at(parser, 1) == '<')) {
    parser->position += c == 'P' ? 2 : 1;
    uint32_t offset = GRX_INDEX_NONE;
    GRX_Result result = read_group_name(parser, start, '>', &offset);
    if (result != GRX_OK) {
      return result;
    }
    // **RE2 allows the same name twice and the crate does not**, probed
    // both ways: `(?P<n>a)(?P<n>b)` compiles in `regexp` and is "duplicate
    // capture group name" in the crate. The option is what carries it, set
    // on the `re2` row's default_options the way perl's is.
    if (!(parser->options & GRX_OPT_DUPLICATE_NAMES)
        && name_already_used(parser, offset)) {
      return grx_parse_fail(parser, GRX_DIAG_DUPLICATE_GROUP_NAME, start,
          parser->position - start);
    }
    parser->groups_opened++;
    out_open->flags = GRX_NODE_CAPTURING | GRX_NODE_NAMED;
    out_open->a = (uint32_t)parser->groups_opened;
    out_open->b = offset;
    return GRX_OK;
  }

  // Everything that is a construct in the Perl family and is not one here.
  // Named rather than folded into the fall-through so that the diagnostic
  // says "this dialect has not got it" instead of "that is not a group",
  // which are different things to tell a caller.
  if (c == '=' || c == '!' || c == '>' || c == '#' || c == '(' || c == '|'
      || c == '&' || c == 'C' || c == '\'' || c == '{'
      // `(?R)` is recursion in the Perl family and RE2 has not got it -
      // but in the crate `R` is the CRLF *flag*, so the letter reaches
      // the flag reader there instead of being refused here.
      || (c == 'R' && flavour(parser) == FLAVOUR_RE2)
      // `<` reaching here is `(?<=` or `(?<!`, the named-group branch above
      // having taken every other `<`. Without it both fell through to the
      // flag reader and a lookbehind was reported as a bad flag letter.
      || c == '<'
      || is_decimal(c) || (c == '-' && is_decimal(byte_at(parser, 1)))
      || (c == 'P' && (byte_at(parser, 1) == '=' || byte_at(parser, 1) == '>'))
      || (c == '+' && is_decimal(byte_at(parser, 1)))) {
    return grx_parse_fail(parser, GRX_DIAG_NOT_IN_DIALECT, start,
        parser->position + 1 - start);
  }

  uint32_t set = 0;
  uint32_t clear = 0;
  GRX_Result result = read_flags(parser, start, &set, &clear);
  if (result != GRX_OK) {
    return result;
  }

  out_open->kind = GRX_NODE_OPTIONS;
  out_open->a = set;
  out_open->b = clear;
  parser->options = (parser->options | set) & ~clear;

  if (grx_parse_eat(parser, ':')) {
    out_open->flags = GRX_NODE_SCOPED;
    out_open->has_body = 1;
    return GRX_OK;
  }
  if (!grx_parse_eat(parser, ')')) {
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 1);
  }
  out_open->has_body = 0;
  return GRX_OK;
}

// --------------------------------------------------------------------------
// Quantifiers, literals, and the atoms a quantifier may not follow
// --------------------------------------------------------------------------

/**
 * The largest repeat RE2 will compile, and the crate has no such number.
 *
 * A thousand, measured rather than read: `a{1000}` compiles in `regexp` and
 * `a{1001}` does not. It is a property of that engine - it expands a
 * counted repeat into that many copies of the program - and it is the
 * dialect's for the same reason PCRE2's 65535 is PCRE2's.
 *
 * The crate expands one too and still takes `a{100000}`, probed: what
 * bounds it there is the compiled program's *size*, which is a limit on the
 * whole pattern rather than on one repeat, and this library spells that
 * kind of bound in GRX_Limits rather than in a dialect table.
 */
#define RE2_REPEAT_MAX 1000

/**
 * Read a `{` quantifier, the brace already consumed.
 *
 * **The two dialects disagree about a `{` that is not a quantifier**, and
 * it is the only place in this file where one of them has a literal the
 * other refuses. `a{,3}` and `a{2,` are five and four literal characters in
 * RE2 - probed: `a{,3}` matches the string "a{,3}" and not "aaa" - and the
 * crate answers "repetition quantifier expects a valid decimal" for the
 * first and "unclosed counted repetition" for the second.
 *
 * `{3,1}` is refused by both, which is the shared default and is why
 * GRX_SyntaxSpec::allow_impossible_repeat is clear on each row.
 */
static GRX_Result re2_brace_quantifier(
    GRX_Parser * parser, GRX_Quantifier * out_quantifier) {
  size_t start = parser->position - 1; // The `{`.
  size_t scan = parser->position;
  uint64_t min = 0;
  size_t digits = 0;

  while (scan < parser->length && is_decimal(parser->text[scan])) {
    min = min * 10 + (uint64_t)(parser->text[scan] - '0');
    if (min > 0xFFFFFFFFu) {
      min = 0xFFFFFFFFu;
    }
    scan++;
    digits++;
  }

  uint64_t max = min;
  int open_ended = 0;
  if (scan < parser->length && parser->text[scan] == ',') {
    scan++;
    size_t high_digits = 0;
    uint64_t high = 0;
    while (scan < parser->length && is_decimal(parser->text[scan])) {
      high = high * 10 + (uint64_t)(parser->text[scan] - '0');
      if (high > 0xFFFFFFFFu) {
        high = 0xFFFFFFFFu;
      }
      scan++;
      high_digits++;
    }
    if (high_digits) {
      max = high;
    }
    else {
      open_ended = 1;
    }
  }

  if (!digits || scan >= parser->length || parser->text[scan] != '}') {
    if (flavour(parser) == FLAVOUR_RE2) {
      // Not a quantifier, and the `{` is an ordinary character. The shared
      // parser reads it as an atom next, which is what is_quantifier says.
      out_quantifier->is_quantifier = 0;
      return GRX_OK;
    }
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_BRACE, start,
        (scan < parser->length ? scan : parser->length) - start);
  }
  parser->position = scan + 1;

  if (flavour(parser) == FLAVOUR_RE2
      && (min > RE2_REPEAT_MAX || (!open_ended && max > RE2_REPEAT_MAX))) {
    return grx_parse_fail(parser, GRX_DIAG_REPEAT_COUNT_TOO_LARGE, start,
        parser->position - start);
  }
  // `{3,1}` is refused by both references and is refused here by the shared
  // parser, which compares the two bounds against
  // GRX_SyntaxSpec::allow_impossible_repeat. Saying it again would put one
  // rule in two places.

  out_quantifier->min = (uint32_t)min;
  out_quantifier->max = open_ended ? GRX_REPEAT_INF : (uint32_t)max;
  out_quantifier->is_quantifier = 1;
  return GRX_OK;
}

/**
 * Turn a character with no operator meaning into a node.
 *
 * `]` and `}` are literals in both references, probed - so a stray closing
 * bracket or brace is a character rather than the error it is in
 * ECMAScript's `u` mode. A bare `{` is a literal in RE2 and an error in the
 * crate, which is the same split re2_brace_quantifier() has and is reached
 * from here only when the `{` began nothing quantifier-shaped at all.
 */
static GRX_Result re2_literal_atom(GRX_Parser * parser, uint32_t codepoint,
    size_t offset, size_t length, uint32_t * out_node) {
  if (codepoint == '{' && flavour(parser) == FLAVOUR_RUST) {
    return grx_parse_fail(
        parser, GRX_DIAG_UNESCAPED_METACHARACTER, offset, length);
  }
  return grx_parse_literal_node(parser, codepoint, offset, length, out_node);
}

/**
 * Whether this atom may carry this quantifier.
 *
 * **An anchor may**, which is the answer a reader would not guess and which
 * both references give: `^*` compiles in each, and matches the empty string
 * at position 0. Perl refuses `(?=a)*` and ECMAScript's `u` mode refuses
 * `^*`; these two do not, and a hook that borrowed either rule would refuse
 * a pattern the dialect compiles.
 *
 * A *second* quantifier is a different question and is not this hook's:
 * `a**` is one atom with two quantifiers, which
 * GRX_SyntaxSpec::allow_double_quantifier decides - clear for RE2, which
 * refuses it, and set for the crate, which reads it as `(a*)*`.
 */
static GRX_Result re2_check_quantifier_target(GRX_Parser * parser,
    uint32_t node, size_t offset, size_t length) {
  (void)parser;
  (void)node;
  (void)offset;
  (void)length;
  return GRX_OK;
}

/**
 * Whether a `\Q` run is open at `at`, counted from the start of the pattern.
 *
 * `regexp` refuses a `\E` that closes nothing - `a\Eb` and `\Qa\E\E` are
 * both "invalid escape sequence" - where perl accepts it with a warning. So
 * the two letters are not independent here and the reader has to know which
 * it is looking at.
 *
 * Counted rather than remembered because a front end holds no state across
 * hook calls, and the scan is bounded by the pattern rather than by the
 * subject. Inside a run a backslash is an ordinary character and only `\E`
 * ends it; outside one, a backslash takes the byte after it with it, so
 * `\\Q` is a literal backslash followed by `Q` and opens nothing.
 */
static int quote_run_open_at(const GRX_Parser * parser, size_t at) {
  int open = 0;
  size_t i = 0;
  while (i + 1 < at) {
    if (parser->text[i] != '\\') {
      i++;
      continue;
    }
    char next = parser->text[i + 1];
    if (!open && next == 'Q') {
      open = 1;
      i += 2;
      continue;
    }
    if (open && next == 'E') {
      open = 0;
      i += 2;
      continue;
    }
    i += open ? 1 : 2;
  }
  return open;
}

/**
 * Consume what stands between atoms and means nothing.
 *
 * Two unrelated things, one per dialect, and the hook is shared because the
 * parser calls it in one place.
 *
 * **`\Q...\E` is RE2's**, and it is here rather than in the escape reader
 * because a quoted run is not an escape: it is a stretch of the pattern in
 * which nothing is an operator, and `parser->quote_end` is what the shared
 * parser reads to know that. A run with nothing in it - `\Q\E` - opens
 * nothing - and a `\E` with no run open is an *error*, which is `regexp`'s
 * answer and not perl's: `a\Eb` and `\Qa\E\E` are both refused there.
 *
 * **Extended mode is the crate's**: `(?x)` ignores whitespace and
 * `#`-to-end-of-line comments. RE2 has no `x` flag at all, so the option
 * can never be set under it.
 */
static GRX_Result re2_skip_ignorable(GRX_Parser * parser) {
  for (;;) {
    // `.` is an operator, so it reaches no hook of this front end - and
    // this hook is called immediately before every atom, with the options
    // that are in force there. That makes it the one place a dialect can
    // refuse the dot itself. See byte_mode().
    if (byte_at(parser, 0) == '.' && byte_mode(parser)) {
      return grx_parse_fail(
          parser, GRX_DIAG_NOT_IN_DIALECT, parser->position, 1);
    }
    if (flavour(parser) == FLAVOUR_RE2 && byte_at(parser, 0) == '\\'
        && (byte_at(parser, 1) == 'Q' || byte_at(parser, 1) == 'E')) {
      int opening = byte_at(parser, 1) == 'Q';
      if (!opening && !quote_run_open_at(parser, parser->position)) {
        return grx_parse_fail(
            parser, GRX_DIAG_INVALID_ESCAPE, parser->position, 2);
      }
      parser->position += 2;
      if (!opening) {
        continue;
      }
      size_t end = parser->length;
      for (size_t i = parser->position; i + 1 < parser->length; i++) {
        if (parser->text[i] == '\\' && parser->text[i + 1] == 'E') {
          end = i;
          break;
        }
      }
      if (end > parser->position) {
        parser->quote_end = end;
        return GRX_OK;
      }
      continue;
    }
    if (!(parser->options & GRX_OPT_EXTENDED)) {
      return GRX_OK;
    }
    char c = byte_at(parser, 0);
    if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'
        || c == '\v') {
      parser->position++;
      continue;
    }
    if (c == '#') {
      while (!grx_parse_at_end(parser) && byte_at(parser, 0) != '\n') {
        parser->position++;
      }
      continue;
    }
    return GRX_OK;
  }
}

// --------------------------------------------------------------------------
// The tables
// --------------------------------------------------------------------------

// Two tables with the same entries, the differences being inside the hooks
// and selected by flavour(). The second table is still worth having for the
// reason perl.c's three are: one front end answering for two dialects would
// make "which dialect is this" a question with no answer at the point a
// diagnostic is written.

const GRX_Frontend grx_frontend_re2 = {
  .name = "re2",
  .atom_escape = re2_atom_escape,
  .class_escape = re2_class_escape,
  .char_class = re2_char_class,
  .group_open = re2_group_open,
  .brace_quantifier = re2_brace_quantifier,
  .literal_atom = re2_literal_atom,
  .check_quantifier_target = re2_check_quantifier_target,
  .skip_ignorable = re2_skip_ignorable,
};

const GRX_Frontend grx_frontend_rust = {
  .name = "rust",
  .atom_escape = re2_atom_escape,
  .class_escape = re2_class_escape,
  .char_class = re2_char_class,
  .group_open = re2_group_open,
  .brace_quantifier = re2_brace_quantifier,
  .literal_atom = re2_literal_atom,
  .check_quantifier_target = re2_check_quantifier_target,
  .skip_ignorable = re2_skip_ignorable,
};
