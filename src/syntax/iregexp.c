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
 * The I-Regexp front end: RFC 9485, and a *checking* implementation of it.
 *
 * RFC 9485 section 3.1 names two kinds of implementation. A non-checking one
 * maps the pattern onto another dialect by the rewrites in its section 5 and
 * lets that dialect's engine decide what is legal; a checking one refuses
 * everything the ABNF in Figure 1 does not generate, and the RFC RECOMMENDS
 * it. This is a checking one, which is the only kind worth adding to a
 * library that already has ECMAScript: a caller who wanted the mapping could
 * have compiled the text as ECMAScript themselves, and what they could not
 * do is find out that their pattern is not interoperable.
 *
 * **The mapping in section 5.3 is not a translation, and that is the point.**
 * It says to rewrite an unescaped `.` to `[^\n\r]` and wrap the result in
 * `^(?:` and `)$`. Both steps are wrong on raw I-Regexp text:
 *
 * - `^` and `$` are ordinary characters here (they are NormalChars), so the
 *   I-Regexp `^a$` matches the three characters `^`, `a`, `$`. Wrapped and
 *   read as ECMAScript it becomes `^(?:^a$)$`, which matches `a`.
 * - `{` and `}` are *not* NormalChars, so a bare `{` is not a literal brace:
 *   it is a syntax error, and `\{` is how a brace is written. ECMAScript's
 *   `u` mode refuses a bare `{` too, but for the opposite reason - there it
 *   is a metacharacter that was not escaped, here it is a character the
 *   grammar does not admit at all.
 * - `\d`, `\w`, `\s`, `\i`, `\c` and every other multi-character escape is
 *   absent, along with class subtraction, Unicode blocks, lookaround,
 *   backreferences, non-capturing groups, lazy quantifiers, inline flags and
 *   `\u` escapes. Each is legal ECMAScript, so a pattern compiled as
 *   ECMAScript would be accepted and a caller told their non-interoperable
 *   pattern is fine.
 *
 * What the dialect *is*, in one paragraph: alternation, concatenation,
 * capturing groups, the four quantifiers with no lazy or possessive form,
 * `.`, the 17 single-character escapes, `\p{...}` and `\P{...}` over the 36
 * general-category names of Figure 1, and bracket expressions of characters,
 * ranges and those two escapes. Nothing else. There is no flag anywhere in
 * the syntax, so there is no case-insensitive pattern and no `(?i)`.
 *
 * **Anchoring is not in here.** RFC 9485 section 4 gives the dialect XSD's
 * Boolean semantics, where the whole string must match, and JSONPath (RFC
 * 9535 section 2.4.6 and 2.4.7) wants *both* that and a substring search
 * from the same pattern. So the anchoring is the operation rather than the
 * grammar: GRX_OPT_ANCHORED and GRX_OPT_ANCHORED_END are what a caller sets
 * to get `match()`, and neither of them is set here. A front end that
 * wrapped the pattern would make `search()` unaskable.
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../parse/parse_internal.h"
#include "../unicode/unicode_internal.h"
#include "syntax_internal.h"

/** The byte `ahead` past the position, or 0 at the end. */
static char byte_at(const GRX_Parser * parser, size_t ahead) {
  size_t at = parser->position + ahead;
  return at < parser->length ? parser->text[at] : '\0';
}

// --------------------------------------------------------------------------
// Escapes
// --------------------------------------------------------------------------

/**
 * The code point `\c` denotes, or 0 when `c` is not a SingleCharEsc.
 *
 * Figure 1's production, character for character:
 *
 *     SingleCharEsc = "\" ( %x28-2B / "-" / "." / "?" / %x5B-5E
 *                         / %s"n" / %s"r" / %s"t" / %x7B-7D )
 *
 * Fourteen characters that stand for themselves - `( ) * + - . ? [ \ ] ^ {
 * | }` - and three letters. The absences are what a reader coming from any
 * other dialect needs to see: there is no `\d`, no `\s`, no `\w`, no `\b`,
 * no `\A`, no `\x41`, no `A`, no `\0`, and no `\$` either. A dollar
 * sign is an ordinary character, so escaping it is not merely unnecessary
 * but ill-formed.
 *
 * `%s` in the ABNF is RFC 7405's case-*sensitive* literal, so `\N`, `\R`
 * and `\T` are not these three.
 */
static uint32_t single_char_escape(char c) {
  switch (c) {
    case '(': case ')': case '*': case '+':
    case '-': case '.': case '?':
    case '[': case '\\': case ']': case '^':
    case '{': case '|': case '}':
      return (uint32_t)(unsigned char)c;
    case 'n': return 0x0A;
    case 'r': return 0x0D;
    case 't': return 0x09;
    default: return 0;
  }
}

/**
 * The 36 category names Figure 1 admits, and no others.
 *
 * `IsCategory` is written as seven productions with an optional second
 * letter each, which is this list. Two things about it are load-bearing:
 *
 * - **`Cs` is absent.** `Others = %s"C" [ ( %s"c" / %s"f" / %s"n" / %s"o" ) ]`
 *   has no `s`, so a surrogate category cannot be named - which is of a
 *   piece with NormalChar skipping the surrogate code points. `Cn` is
 *   there, so `\p{Cn}` (unassigned) is legal.
 * - **Nothing else is a charProp.** `charProp = IsCategory` and no more, so
 *   `\p{Letter}`, `\p{gc=Lu}`, `\p{Script=Latin}` and `\p{IsBasicLatin}`
 *   are all refused. The last is the one the RFC's own section 5.1 uses as
 *   an example of legacy ASCII and cannot itself spell - erratum 8505
 *   against that section says as much.
 *
 * Case-sensitive, `%s` again: `\p{lu}` and `\p{LU}` are not `\p{Lu}`.
 */
static const char * const category_names[] = {
  "C", "Cc", "Cf", "Cn", "Co",
  "L", "Ll", "Lm", "Lo", "Lt", "Lu",
  "M", "Mc", "Me", "Mn",
  "N", "Nd", "Nl", "No",
  "P", "Pc", "Pd", "Pe", "Pf", "Pi", "Po", "Ps",
  "S", "Sc", "Sk", "Sm", "So",
  "Z", "Zl", "Zp", "Zs",
  NULL,
};

/** Whether `name` is one of Figure 1's categories, exactly as written. */
static int is_category(const char * name, size_t length) {
  for (size_t i = 0; category_names[i]; i++) {
    if (strlen(category_names[i]) == length
        && memcmp(category_names[i], name, length) == 0) {
      return 1;
    }
  }
  return 0;
}

/**
 * Read `\p{...}` or `\P{...}` into a class item, the letter already read.
 *
 * The name is checked against Figure 1's list *and* resolved through the
 * Unicode tables. Both, deliberately: the list is what makes this a checking
 * implementation, and the resolution is what makes a name in the list that
 * the tables cannot answer a build-time impossibility rather than a
 * match-time surprise. The two can only disagree if this library's tables
 * lose a general category, which is a defect in them.
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

  const char * name = parser->text + name_start;
  size_t name_length = parser->position - name_start;
  parser->position++; // The `}`.

  if (!is_category(name, name_length)) {
    return grx_parse_fail(
        parser, GRX_DIAG_UNKNOWN_PROPERTY, start, parser->position - start);
  }

  uint32_t property = 0;
  if (grx_unicode_property_lookup(name, name_length, NULL, 0,
          parser->profile.property_match, &property)
      != GRX_OK) {
    return grx_parse_fail(
        parser, GRX_DIAG_UNKNOWN_PROPERTY, start, parser->position - start);
  }

  uint32_t offset = GRX_INDEX_NONE;
  if (grx_pattern_add_name(parser->pattern, name, name_length, &offset)
      != GRX_OK) {
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

/**
 * Read an escape outside a bracket expression, the `\` already consumed.
 *
 * Three outcomes and no fourth: a single-character escape is the character
 * it names, `\p`/`\P` is a one-item class, and everything else is
 * GRX_DIAG_INVALID_ESCAPE. There is no "unknown escape is the character
 * itself" rule here, which is the rule most dialects have and the reason a
 * pattern written for one of them passes through them silently.
 */
static GRX_Result ire_atom_escape(GRX_Parser * parser, uint32_t * out_node) {
  size_t start = parser->position - 1; // The backslash.
  char c = byte_at(parser, 0);

  uint32_t single = single_char_escape(c);
  if (single) {
    parser->position++;
    return grx_parse_literal_node(
        parser, single, start, parser->position - start, out_node);
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

  return grx_parse_fail(parser, GRX_DIAG_INVALID_ESCAPE, start,
      parser->position + 1 - start);
}

/**
 * Read an escape inside a bracket expression, the `\` already consumed.
 *
 * The same alphabet as outside it, which is what `CCchar` says: a CCchar is
 * either a character or a SingleCharEsc, and a CCE1 may also be a
 * charClassEsc. So `[\n\-\]]` is three members and `[\d]` is an error in
 * exactly the way `\d` is.
 */
static GRX_Result ire_class_escape(
    GRX_Parser * parser, GRX_ClassItem * out_item) {
  size_t start = parser->position - 1; // The backslash.
  char c = byte_at(parser, 0);

  uint32_t single = single_char_escape(c);
  if (single) {
    parser->position++;
    *out_item = (GRX_ClassItem) {
      .kind = GRX_CLASS_ITEM_SINGLE,
      .flags = 0,
      .lo = single,
      .hi = single,
      .a = 0,
      .offset = start,
      .length = parser->position - start,
    };
    return GRX_OK;
  }

  if (c == 'p' || c == 'P') {
    parser->position++;
    return read_property(parser, c == 'P', start, out_item);
  }

  return grx_parse_fail(parser, GRX_DIAG_INVALID_ESCAPE, start,
      parser->position + 1 - start);
}

// --------------------------------------------------------------------------
// Bracket expressions
// --------------------------------------------------------------------------

/**
 * Read a bracket expression, the `[` already consumed.
 *
 *     charClassExpr = "[" [ "^" ] ( "-" / CCE1 ) *CCE1 [ "-" ] "]"
 *     CCE1 = ( CCchar [ "-" CCchar ] ) / charClassEsc
 *     CCchar = ( %x00-2C / %x2E-5A / %x5E-D7FF / %xE000-10FFFF )
 *              / SingleCharEsc
 *
 * Four rules fall out of those three lines, and each of them refuses
 * something ECMAScript accepts:
 *
 * - **At least one item.** `[]` is not an empty class and not a class
 *   holding `]`; it is a syntax error, because a `]` cannot be a CCchar
 *   (%x2E-5A stops at `Z`) and the production requires an item.
 * - **`[^]` is refused by name.** The RFC adds it as a restriction on top
 *   of the grammar: by the grammar alone it would be a positive class whose
 *   one member is `^`, and the RFC does not want the spelling to mean that.
 * - **A bare `[` is not a member.** So the class subtraction of XSD and of
 *   .NET, `[a-z-[aeiou]]`, fails at the inner bracket - which is how a
 *   subtraction becomes an error rather than a class of `a`-`z`, `-`, `[`
 *   and the rest.
 * - **A bare `-` is a member only first or last, and is never a range
 *   endpoint.** CCchar covers %x00-2C and %x2E-5A, which skips %x2D: the
 *   only `-` the grammar admits are the two optional ones in charClassExpr,
 *   immediately after the `[` or `[^` and immediately before the `]`. So
 *   `[-a]` and `[a-]` are members, `[--a]` and `[a-b-c]` are errors, and a
 *   `-` that is to be the low end of a range has to be written `\-`, as in
 *   `[\--a]`. A reader that let a bare leading `-` open a range would accept
 *   `[--a]`, which every other dialect here reads as the range `-` to `a`
 *   and this one does not admit at all.
 * - **A range's endpoints are CCchars**, so `[\p{L}-z]` and `[a-\p{L}]` are
 *   not ranges and are not unions either: they are errors.
 */
static GRX_Result ire_char_class(GRX_Parser * parser, uint32_t * out_node) {
  size_t start = parser->position - 1; // The `[`.
  int negated = grx_parse_eat(parser, '^');

  if (byte_at(parser, 0) == ']') {
    // `[]` and `[^]`, which are two refusals rather than one. An empty class
    // is a class with nothing in it; `[^]` has something in it by the
    // grammar and is refused by the restriction under Figure 1, so saying
    // "empty" about it would be saying something false.
    parser->position++;
    return negated
        ? grx_parse_fail(
              parser, GRX_DIAG_NOT_IN_DIALECT, start, parser->position - start)
        : grx_parse_fail(
              parser, GRX_DIAG_EMPTY_CLASS, start, parser->position - start);
  }

  GRX_Result result = grx_parse_class_node(parser, start, out_node);
  if (result != GRX_OK) {
    return result;
  }
  if (negated) {
    grx_pattern_node(parser->pattern, *out_node)->flags |= GRX_NODE_NEGATED;
  }

  int first = 1;
  for (;;) {
    if (grx_parse_at_end(parser)) {
      return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_BRACKET, start,
          parser->position - start);
    }
    if (byte_at(parser, 0) == ']') {
      parser->position++;
      break;
    }

    size_t item_start = parser->position;
    GRX_ClassItem item = {GRX_CLASS_ITEM_SINGLE, 0, 0, 0, 0, 0, 0};

    // A bare `-` standing where the grammar has no `-`: not the optional one
    // after the `[`, not the optional one before the `]`. `[a-b-c]` is where
    // this bites, and it is legal in every other dialect in this library.
    int bare_dash = byte_at(parser, 0) == '-';
    if (bare_dash && !first && byte_at(parser, 1) != ']') {
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_CLASS_ITEM, item_start, 1);
    }

    if (byte_at(parser, 0) == '\\') {
      parser->position++;
      if (grx_parse_at_end(parser)) {
        return grx_parse_fail(parser, GRX_DIAG_TRAILING_BACKSLASH, item_start,
            parser->position - item_start);
      }
      result = ire_class_escape(parser, &item);
      if (result != GRX_OK) {
        return result;
      }
    }
    else if (byte_at(parser, 0) == '[') {
      // The subtraction case, and any other nested bracket. INVALID_CLASS_ITEM
      // rather than a set-operation diagnostic: this dialect has no set
      // operations for the `[` to be part of, so what is wrong with it is
      // that it is not a member.
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_CLASS_ITEM, item_start, 1);
    }
    else {
      uint32_t low = 0;
      result = grx_parse_take(parser, &low);
      if (result != GRX_OK) {
        return result;
      }
      item = (GRX_ClassItem) {
        .kind = GRX_CLASS_ITEM_SINGLE,
        .flags = 0,
        .lo = low,
        .hi = low,
        .a = 0,
        .offset = item_start,
        .length = parser->position - item_start,
      };
    }

    // A range, when a `-` follows a single character and something other
    // than the closing `]` follows the `-`. `[a-]` is `a` and `-`.
    if (item.kind == GRX_CLASS_ITEM_SINGLE && !bare_dash
        && byte_at(parser, 0) == '-' && byte_at(parser, 1) != ']'
        && parser->position + 1 < parser->length) {
      size_t dash = parser->position;
      parser->position++;
      GRX_ClassItem high_item = {GRX_CLASS_ITEM_SINGLE, 0, 0, 0, 0, 0, 0};
      if (byte_at(parser, 0) == '\\') {
        parser->position++;
        if (grx_parse_at_end(parser)) {
          return grx_parse_fail(parser, GRX_DIAG_TRAILING_BACKSLASH,
              item_start, parser->position - item_start);
        }
        result = ire_class_escape(parser, &high_item);
        if (result != GRX_OK) {
          return result;
        }
        if (high_item.kind != GRX_CLASS_ITEM_SINGLE) {
          // `[a-\p{L}]`. A range is CCchar to CCchar and a charClassEsc is
          // not a CCchar, so the endpoint is what is wrong rather than the
          // order of the two ends.
          return grx_parse_fail(parser, GRX_DIAG_CLASS_ESCAPE_IN_RANGE, dash,
              parser->position - dash);
        }
      }
      else if (byte_at(parser, 0) == '[') {
        return grx_parse_fail(
            parser, GRX_DIAG_INVALID_CLASS_ITEM, parser->position, 1);
      }
      else {
        uint32_t high = 0;
        result = grx_parse_take(parser, &high);
        if (result != GRX_OK) {
          return result;
        }
        high_item.lo = high;
      }
      if (high_item.lo < item.lo) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_RANGE, item_start,
            parser->position - item_start);
      }
      item.kind = GRX_CLASS_ITEM_RANGE;
      item.hi = high_item.lo;
      item.length = parser->position - item_start;
    }
    else if (item.kind == GRX_CLASS_ITEM_PROPERTY && byte_at(parser, 0) == '-'
        && byte_at(parser, 1) != ']') {
      // `[\p{L}-z]`, the other side of the same rule.
      return grx_parse_fail(
          parser, GRX_DIAG_CLASS_ESCAPE_IN_RANGE, item_start,
          parser->position + 1 - item_start);
    }

    result = grx_parse_class_add(parser, *out_node, &item);
    if (result != GRX_OK) {
      return result;
    }
    first = 0;
  }

  grx_pattern_node(parser->pattern, *out_node)->length
      = parser->position - start;
  return GRX_OK;
}

// --------------------------------------------------------------------------
// Groups, quantifiers, and the characters that are not operators
// --------------------------------------------------------------------------

/**
 * Read a group opener, the `(` already consumed.
 *
 *     atom = NormalChar / charClass / ( "(" i-regexp ")" )
 *
 * One kind of group, and what follows the `(` is a whole i-regexp. So `(?`
 * is not a group form at all: the `?` would have to begin a piece, and a
 * quantifier is not an atom. Refused as GRX_DIAG_NOT_IN_DIALECT rather than
 * as "nothing to repeat", because a caller who wrote `(?:a)` or `(?=a)` has
 * written a construct this dialect does not have, and telling them the `?`
 * has nothing to repeat would send them looking for the wrong thing.
 *
 * The group captures, which the RFC neither requires nor forbids - it has
 * only a Boolean answer to give, so a capture slot is invisible to it. A
 * slot is what lets grx_match_group() report where a group landed for a
 * caller who wants more than the Boolean, and GRX_OPT_NO_CAPTURE is how a
 * caller who does not says so.
 */
static GRX_Result ire_group_open(
    GRX_Parser * parser, GRX_GroupOpen * out_open) {
  if (byte_at(parser, 0) == '?') {
    return grx_parse_fail(
        parser, GRX_DIAG_NOT_IN_DIALECT, parser->position - 1, 2);
  }

  out_open->kind = GRX_NODE_GROUP;
  out_open->b = GRX_INDEX_NONE;
  out_open->has_body = 1;
  if (parser->options & GRX_OPT_NO_CAPTURE) {
    return GRX_OK;
  }
  parser->groups_opened++;
  out_open->flags = GRX_NODE_CAPTURING;
  out_open->a = (uint32_t)parser->groups_opened;
  return GRX_OK;
}

/**
 * Read a range quantifier, the `{` already consumed.
 *
 *     range-quantifier = "{" QuantExact [ "," [ QuantExact ] ] "}"
 *     QuantExact = 1*%x30-39
 *
 * `{m}`, `{m,}` and `{m,n}`, and nothing else. The two refusals worth
 * naming:
 *
 * - `{,3}` has no QuantExact before the comma, so it is not a quantifier -
 *   and a `{` that is not a quantifier is not a literal brace either, the
 *   way it would be in Perl or in ECMAScript without `u`. It is an error.
 * - A leading zero is fine. `{007}` is 1*%x30-39 and means seven.
 */
static GRX_Result ire_brace_quantifier(
    GRX_Parser * parser, GRX_Quantifier * out_quantifier) {
  size_t start = parser->position - 1; // The `{`.
  size_t digits = 0;
  uint32_t min = 0;

  while (byte_at(parser, 0) >= '0' && byte_at(parser, 0) <= '9') {
    if (min > (GRX_REPEAT_INF - 9) / 10) {
      return grx_parse_fail(parser, GRX_DIAG_REPEAT_COUNT_TOO_LARGE, start,
          parser->position + 1 - start);
    }
    min = min * 10 + (uint32_t)(byte_at(parser, 0) - '0');
    parser->position++;
    digits++;
  }
  if (!digits) {
    // No QuantExact, so this `{` is not the start of a quantifier - and the
    // grammar has no other place for it, NormalChar stopping at %x7A. The
    // brace has to be written `\{`, which is what this diagnostic says.
    return grx_parse_fail(
        parser, GRX_DIAG_UNESCAPED_METACHARACTER, start, 1);
  }

  uint32_t max = min;
  if (byte_at(parser, 0) == ',') {
    parser->position++;
    size_t high_digits = 0;
    uint32_t high = 0;
    while (byte_at(parser, 0) >= '0' && byte_at(parser, 0) <= '9') {
      if (high > (GRX_REPEAT_INF - 9) / 10) {
        return grx_parse_fail(parser, GRX_DIAG_REPEAT_COUNT_TOO_LARGE, start,
            parser->position + 1 - start);
      }
      high = high * 10 + (uint32_t)(byte_at(parser, 0) - '0');
      parser->position++;
      high_digits++;
    }
    max = high_digits ? high : GRX_REPEAT_INF;
  }

  if (byte_at(parser, 0) != '}') {
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_BRACE, start,
        parser->position - start);
  }
  parser->position++;

  out_quantifier->min = min;
  out_quantifier->max = max;
  out_quantifier->is_quantifier = 1;
  return GRX_OK;
}

/**
 * Turn a character with no operator meaning into a node.
 *
 * Three characters arrive here that are not NormalChars and must not become
 * literals: `{`, `}` and `]`. NormalChar is
 *
 *     %x00-27 / "," / "-" / %x2F-3E / %x40-5A / %x5E-7A / %x7E-D7FF
 *     / %xE000-10FFFF
 *
 * which skips %x5B-5D (`[`, `\`, `]`) and %x7B-7D (`{`, `|`, `}`). Of those
 * six, `[`, `\` and `|` are operators and never reach here, and the other
 * three are refused: each has to be written with a backslash.
 *
 * `^` and `$` do reach here and are ordinary characters, which is the
 * difference from every other dialect in this library and the reason the
 * RFC's own ECMAScript mapping cannot be applied to raw text. They are in
 * NormalChar (%x00-27 covers `$`, %x5E-7A begins with `^`) and there is no
 * anchor in the grammar for them to be.
 */
static GRX_Result ire_literal_atom(GRX_Parser * parser, uint32_t codepoint,
    size_t offset, size_t length, uint32_t * out_node) {
  if (codepoint == '{' || codepoint == '}' || codepoint == ']') {
    return grx_parse_fail(
        parser, GRX_DIAG_UNESCAPED_METACHARACTER, offset, length);
  }
  return grx_parse_literal_node(parser, codepoint, offset, length, out_node);
}

/**
 * Whether this atom may carry this quantifier.
 *
 * Every atom may: `piece = atom [quantifier]` puts no condition on the atom,
 * and this dialect has no zero-width assertion for a quantifier to be applied
 * to - there is no `^`, no `$`, no `\b` and no lookaround. So there is
 * nothing to refuse here.
 *
 * A *second* quantifier is refused, and not here: `a**` and `a*?` are one
 * atom with two quantifiers, which is GRX_SyntaxSpec::allow_double_quantifier
 * left at zero and the shared parser's GRX_DIAG_DOUBLE_QUANTIFIER. Saying it
 * again in this hook would put one rule in two places.
 *
 * The hook is still here rather than NULL because the parser calls it
 * unconditionally - see the vtable's own documentation for which hooks are
 * mandatory. A front end that leaves this one out does not fall back to
 * anything; it crashes.
 */
static GRX_Result ire_check_quantifier_target(GRX_Parser * parser,
    uint32_t node, size_t offset, size_t length) {
  (void)parser;
  (void)node;
  (void)offset;
  (void)length;
  return GRX_OK;
}

/**
 * Whether `op` is spelled with a backslash here - or has no spelling at all.
 *
 * `^` and `$` are the second case. This dialect has no `^` or `$` operator,
 * so the shared parser must not build an anchor out of either, and the hook
 * that says "the bare character is not the operator" is this one. `\^` is a
 * legal escape for the literal caret and `\$` is not legal at all, so
 * neither answer here is quite "it is written with a backslash" - what is
 * true is that the bare character is a literal, which is what the parser
 * does with a non-zero answer and what the hook's own documentation names as
 * its effect.
 *
 * Every other operator - `( ) | . [ * + ? {` - is bare.
 */
static int ire_operator_is_escaped(const GRX_Parser * parser, char op) {
  (void)parser;
  return op == '^' || op == '$';
}

const GRX_Frontend grx_frontend_iregexp = {
  .name = "i-regexp",
  .atom_escape = ire_atom_escape,
  .class_escape = ire_class_escape,
  .char_class = ire_char_class,
  .group_open = ire_group_open,
  .brace_quantifier = ire_brace_quantifier,
  .literal_atom = ire_literal_atom,
  .check_quantifier_target = ire_check_quantifier_target,
  .operator_is_escaped = ire_operator_is_escaped,
};
