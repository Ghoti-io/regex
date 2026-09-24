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
 * The POSIX and GNU front ends: basic and extended regular expressions.
 *
 * Four dialects out of one reader, because they differ in what they *have*
 * rather than in how anything is spelled. The two axes are already in the
 * tables: `escaped_specials` says whether `\(` groups and `(` is a literal,
 * and the feature bits say whether alternation, backreferences, `\w` and
 * `\<` exist at all. So a BRE and an ERE are the same grammar read with the
 * operators spelled differently, which is what POSIX means by calling one
 * "basic" and the other "extended".
 *
 * What the reference is. glibc's `regcomp` is the oracle these were written
 * against (tools/oracle/posix_match.c), and it is **GNU**, not POSIX: it
 * accepts `\|`, `\+`, `\?`, `\w`, `\b` and `\<` in a basic RE and `\w` and
 * `\b` in an extended one. POSIX leaves a backslash before an ordinary
 * character undefined and GNU defines it, so that is a conforming extension
 * rather than a disagreement - but it does mean the vectors imported from
 * glibc are `gnu-bre` and `gnu-ere`, and that the two POSIX rows here are
 * built from the standard rather than from a reference that can be asked.
 *
 * Three things a reader coming from Perl should know:
 *
 * - **There are no escapes inside a bracket expression.** `[\]]` is the
 *   class holding a backslash, followed by a literal `]`. POSIX says the
 *   backslash loses its meaning there, and glibc agrees.
 * - **`.` matches a newline**, and there is no option to say so because
 *   dot-all is the default. `REG_NEWLINE` is what turns it off.
 * - **A quantifier may be quantified.** `a**` is `(a*)*` and `a{2}{3}` is
 *   thirty-six characters' worth of `a`, both of which glibc matches.
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../parse/parse_internal.h"
#include "syntax_internal.h"

// --------------------------------------------------------------------------
// Which of the four
// --------------------------------------------------------------------------

/** The byte `ahead` past the position, or 0 at the end. */
static char byte_at(const GRX_Parser * parser, size_t ahead) {
  size_t at = parser->position + ahead;
  return at < parser->length ? parser->text[at] : '\0';
}

/** Whether this dialect spells its grouping operators with a backslash. */
static int is_basic(const GRX_Parser * parser) {
  return parser->spec.escaped_specials != 0;
}

/** Whether GNU's additions - `\w`, `\b`, `\<` - are part of the dialect. */
static int has_gnu_escapes(const GRX_Parser * parser) {
  return (parser->spec.features & GRX_FEATURE_WORD_BOUNDARY) != 0;
}

// --------------------------------------------------------------------------
// Bracket expressions
// --------------------------------------------------------------------------

/** The twelve classes POSIX names, and nothing else. */
static const char * const posix_class_names[] = {
  "alnum", "alpha", "blank", "cntrl", "digit", "graph", "lower", "print",
  "punct", "space", "upper", "xdigit", NULL,
};

/**
 * Whether `[` here opens `[:name:]`, `[.x.]` or `[=x=]`, and where it ends.
 *
 * `end` receives the offset, from the `[`, of the closing delimiter, so the
 * construct spans `end + 2` bytes.
 */
static int posix_construct_at(const GRX_Parser * parser, size_t * out_end) {
  char kind = byte_at(parser, 1);
  if (byte_at(parser, 0) != '[' || (kind != ':' && kind != '.' && kind != '=')) {
    return 0;
  }

  for (size_t scan = 2; parser->position + scan + 1 < parser->length; scan++) {
    if (byte_at(parser, scan) == kind && byte_at(parser, scan + 1) == ']') {
      *out_end = scan;
      return 1;
    }
  }
  return 0;
}

/**
 * Read `[:alpha:]`, `[.x.]` or `[=x=]` inside a bracket expression.
 *
 * A collating element and an equivalence class are *not* refused here, which
 * is where this differs from the Perl front end. Perl and PCRE2 have neither
 * construct - pcre2test raises error 113 and perl calls the syntax reserved
 * - so a pattern using one is invalid for those dialects. POSIX has both,
 * and in the C locale, which is the only locale here, both collapse to the
 * one character they name: there are no multi-character collating elements
 * and no equivalence classes. glibc answers the same way, matching `a` for
 * `[[.a.]]` and `[[=a=]]` and refusing `[[.ab.]]`. So a single character is
 * read as itself and anything longer is refused as unbuilt, which is the
 * honest split: one is what the locale says, the other is collation this
 * library does not have.
 */
static GRX_Result read_posix_construct(
    GRX_Parser * parser, GRX_ClassItem * out, int * out_matched) {
  size_t start = parser->position;
  size_t end = 0;
  *out_matched = 0;

  if (!posix_construct_at(parser, &end)) {
    return GRX_OK;
  }
  *out_matched = 1;

  char kind = byte_at(parser, 1);
  if (kind != ':') {
    if (end != 3) {
      // More than one character between the delimiters. Refused as a syntax
      // error rather than as something unbuilt, because glibc refuses it
      // too: in the C locale - the only locale here - there are no
      // multi-character collating elements and no equivalence classes, so
      // `[[.ch.]]` names a collating element that does not exist rather
      // than one this library has not implemented.
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_CLASS_ITEM, start, end + 2);
    }
    uint32_t only = (unsigned char)byte_at(parser, 2);
    parser->position += end + 2;
    *out = (GRX_ClassItem) {
      .kind = GRX_CLASS_ITEM_SINGLE,
      .flags = 0,
      .lo = only,
      .hi = only,
      .a = 0,
      .offset = start,
      .length = parser->position - start,
    };
    return GRX_OK;
  }

  const char * name = parser->text + parser->position + 2;
  size_t length = end - 2;
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
  if (grx_pattern_add_name(parser->pattern, name, length, &offset) != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }

  parser->position += end + 2;
  *out = (GRX_ClassItem) {
    .kind = GRX_CLASS_ITEM_POSIX,
    .flags = 0,
    .lo = 0,
    .hi = 0,
    .a = offset,
    .offset = start,
    .length = parser->position - start,
  };
  return GRX_OK;
}

/**
 * Read a bracket expression, the `[` already consumed.
 *
 * POSIX's rules, which are not Perl's:
 *
 * - `^` immediately after the `[` negates, and is an ordinary character
 *   anywhere else.
 * - `]` immediately after the `[` or the `^` is an ordinary character, which
 *   is how a class holding `]` is written - there being no escape to write
 *   it with.
 * - `-` first or last is an ordinary character.
 * - A backslash is an ordinary character. This is the rule that surprises
 *   people: `[\]]` is a class holding a backslash, followed by a literal
 *   `]`, and glibc reads it that way too.
 */
static GRX_Result px_char_class(GRX_Parser * parser, uint32_t * out_node) {
  size_t start = parser->position - 1; // The `[`.
  int negated = 0;

  if (byte_at(parser, 0) == '^') {
    negated = 1;
    parser->position++;
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

    // `]` closes, except as the very first item, where it is the character.
    if (byte_at(parser, 0) == ']' && !first) {
      parser->position++;
      break;
    }
    first = 0;

    GRX_ClassItem item = {GRX_CLASS_ITEM_SINGLE, 0, 0, 0, 0, 0, 0};
    size_t item_start = parser->position;
    int matched = 0;
    result = read_posix_construct(parser, &item, &matched);
    if (result != GRX_OK) {
      return result;
    }
    if (!matched) {
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

    // A range, if a `-` follows and something other than the closing `]`
    // follows that. `[a-]` is `a` and `-`, not an unfinished range.
    if (item.kind == GRX_CLASS_ITEM_SINGLE && byte_at(parser, 0) == '-'
        && byte_at(parser, 1) != ']' && parser->position + 1 < parser->length) {
      parser->position++;
      GRX_ClassItem high_item = {GRX_CLASS_ITEM_SINGLE, 0, 0, 0, 0, 0, 0};
      int high_matched = 0;
      result = read_posix_construct(parser, &high_item, &high_matched);
      if (result != GRX_OK) {
        return result;
      }
      uint32_t high = 0;
      if (high_matched) {
        // `[[.a.]-z]` is a range whose low end was written as a collating
        // element, which glibc accepts. A class as an endpoint is not a
        // range at all.
        if (high_item.kind != GRX_CLASS_ITEM_SINGLE) {
          return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_RANGE,
              item_start, parser->position - item_start);
        }
        high = high_item.lo;
      }
      else {
        result = grx_parse_take(parser, &high);
        if (result != GRX_OK) {
          return result;
        }
      }
      if (high < item.lo) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_RANGE, item_start,
            parser->position - item_start);
      }
      item.kind = GRX_CLASS_ITEM_RANGE;
      item.hi = high;
      item.length = parser->position - item_start;
    }

    // `[1-3-5]` is an error in glibc, and `[a-]` is not: a `-` may follow a
    // *single* character to make a range, and may stand last as itself, but
    // a range cannot be one end of another.
    if (item.kind == GRX_CLASS_ITEM_RANGE && byte_at(parser, 0) == '-'
        && byte_at(parser, 1) != ']') {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_RANGE, item_start,
          parser->position + 1 - item_start);
    }

    result = grx_parse_class_add(parser, *out_node, &item);
    if (result != GRX_OK) {
      return result;
    }
  }

  grx_pattern_node(parser->pattern, *out_node)->length
      = parser->position - start;
  return GRX_OK;
}

/**
 * There are no escapes inside a bracket expression.
 *
 * Never called: px_char_class reads every item itself and never hands a
 * backslash to the shared reader, because in POSIX a backslash there is an
 * ordinary character. The hook is present because the vtable requires it,
 * and says so rather than being NULL, so that a future reader who wires it
 * up finds the rule instead of a crash.
 */
static GRX_Result px_class_escape(
    GRX_Parser * parser, GRX_ClassItem * out_item) {
  (void)out_item;
  return grx_parse_fail(
      parser, GRX_DIAG_INVALID_CLASS_ITEM, parser->position, 1);
}

// --------------------------------------------------------------------------
// Atoms
// --------------------------------------------------------------------------

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

/**
 * Whether the group this number names has already closed.
 *
 * POSIX: a backreference refers to a *completed* subexpression, so
 * `a\(b\1\)c` is invalid - the group is still open where the reference
 * stands - while `a\(b\)\1c` is fine. The prescan has already counted
 * every group, so the count alone cannot tell the two apart.
 *
 * A group's node is created when its `(` is read and its `length` is set
 * when its `)` is, so a length of zero is exactly "still open". Linear in
 * the nodes built so far, which is a walk of a few dozen entries for the
 * patterns anybody writes.
 */
static int group_has_closed(const GRX_Parser * parser, uint32_t group) {
  for (uint32_t index = 0;; index++) {
    const GRX_Node * node = grx_pattern_node(parser->pattern, index);
    if (!node) {
      return 0;
    }
    if (node->kind == GRX_NODE_GROUP && (node->flags & GRX_NODE_CAPTURING)
        && node->a == group) {
      return node->length != 0;
    }
  }
}

/**
 * Read an escape outside a bracket expression, the `\` already consumed.
 *
 * The grouping operators never arrive here: the shared parser asks
 * at_operator() before it dispatches on the backslash, so `\(` in a BRE has
 * already been read as a group opener by the time this is reached.
 *
 * Everything this does not recognise is the character itself. POSIX calls a
 * backslash before an ordinary character undefined, and both references
 * define it the same way - glibc matches "y" for `\y` - so this is the
 * defined behaviour of the dialects as shipped rather than a guess.
 */
static GRX_Result px_atom_escape(GRX_Parser * parser, uint32_t * out_node) {
  size_t start = parser->position - 1; // The backslash.
  char c = byte_at(parser, 0);

  if (has_gnu_escapes(parser)) {
    GRX_ShorthandKind shorthand = GRX_SHORTHAND_WORD;
    int is_shorthand = 1;
    int negated = 0;
    switch (c) {
      case 'w': shorthand = GRX_SHORTHAND_WORD; break;
      case 'W': shorthand = GRX_SHORTHAND_WORD; negated = 1; break;
      case 's': shorthand = GRX_SHORTHAND_SPACE; break;
      case 'S': shorthand = GRX_SHORTHAND_SPACE; negated = 1; break;
      default: is_shorthand = 0; break;
    }
    if (is_shorthand) {
      parser->position++;
      return grx_parse_shorthand_node(
          parser, shorthand, negated, start, parser->position - start,
          out_node);
    }

    GRX_AnchorKind anchor = GRX_ANCHOR_WORD_BOUNDARY;
    int is_anchor = 1;
    switch (c) {
      case 'b': anchor = GRX_ANCHOR_WORD_BOUNDARY; break;
      case 'B': anchor = GRX_ANCHOR_NOT_WORD_BOUNDARY; break;
      case '<': anchor = GRX_ANCHOR_WORD_START; break;
      case '>': anchor = GRX_ANCHOR_WORD_END; break;
      case '`': anchor = GRX_ANCHOR_START_BUFFER; break;
      case '\'': anchor = GRX_ANCHOR_END_BUFFER; break;
      default: is_anchor = 0; break;
    }
    if (is_anchor) {
      parser->position++;
      return anchor_node(parser, anchor, start, out_node);
    }
  }

  // `\{` with no atom before it. The shared parser only asks for an interval
  // after an atom, so reaching it here means there is none - which glibc
  // refuses rather than reading as a literal brace.
  if (is_basic(parser) && c == '{'
      && (parser->spec.features & GRX_FEATURE_BOUNDED_REPEAT)) {
    return grx_parse_fail(parser, GRX_DIAG_NOTHING_TO_REPEAT, start, 2);
  }

  // `\1` to `\9`. POSIX has backreferences in a BRE and not in an ERE; GNU
  // has them in both. Where the dialect has none, the digit is the
  // character, which is what POSIX's "undefined" leaves room for and what
  // makes `\1` in a POSIX ERE a literal 1 rather than an error.
  if (c >= '1' && c <= '9'
      && (parser->spec.features & GRX_FEATURE_BACKREFERENCE)) {
    uint32_t group = (uint32_t)(c - '0');
    parser->position++;
    if (group > parser->group_count || !group_has_closed(parser, group)) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_BACKREFERENCE, start,
          parser->position - start);
    }
    GRX_Result result = grx_pattern_add_node(parser->pattern,
        GRX_NODE_BACKREF, start, parser->position - start, out_node);
    if (result != GRX_OK) {
      return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
    }
    GRX_Node * node = grx_pattern_node(parser->pattern, *out_node);
    node->a = group;
    node->b = GRX_INDEX_NONE;
    return GRX_OK;
  }

  uint32_t literal = 0;
  GRX_Result result = grx_parse_take(parser, &literal);
  if (result != GRX_OK) {
    return result;
  }
  return grx_parse_literal_node(
      parser, literal, start, parser->position - start, out_node);
}

/**
 * Read a group opener, the `(` - or a BRE's `\(` - already consumed.
 *
 * There is nothing to read. POSIX has one kind of group and it captures;
 * `(?` is a literal question mark after an open parenthesis, which is why
 * this hook is four lines and ECMAScript's is four hundred.
 */
static GRX_Result px_group_open(GRX_Parser * parser, GRX_GroupOpen * out_open) {
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
 * Read an interval, the `{` - or a BRE's `\{` - already consumed.
 *
 * `{m}`, `{m,}` and `{m,n}`, closed by `}` or a BRE's `\}`. Anything else is
 * a syntax error rather than a literal brace: glibc refuses `a{` and `a{1`,
 * which is the one place POSIX's "undefined" is not resolved the permissive
 * way. A bare `{` in a BRE never reaches here at all - the shared parser
 * only asks for an interval when it sees the dialect's spelling of one - so
 * `a{2}` in a BRE is four literal characters, which is what glibc matches.
 */
static GRX_Result px_brace_quantifier(
    GRX_Parser * parser, GRX_Quantifier * out_quantifier) {
  size_t start = parser->position - (is_basic(parser) ? 2u : 1u);
  size_t digits = 0;
  uint32_t min = 0;

  while (byte_at(parser, 0) >= '0' && byte_at(parser, 0) <= '9') {
    if (min > (GRX_REPEAT_INF - 9) / 10) {
      return grx_parse_fail(parser, GRX_DIAG_LIMIT_REPEAT_COUNT, start,
          parser->position - start);
    }
    min = min * 10 + (uint32_t)(byte_at(parser, 0) - '0');
    parser->position++;
    digits++;
  }
  if (!digits) {
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_BRACE, start,
        parser->position - start);
  }

  uint32_t max = min;
  if (byte_at(parser, 0) == ',') {
    parser->position++;
    max = GRX_REPEAT_INF;
    size_t high_digits = 0;
    uint32_t high = 0;
    while (byte_at(parser, 0) >= '0' && byte_at(parser, 0) <= '9') {
      if (high > (GRX_REPEAT_INF - 9) / 10) {
        return grx_parse_fail(parser, GRX_DIAG_LIMIT_REPEAT_COUNT, start,
            parser->position - start);
      }
      high = high * 10 + (uint32_t)(byte_at(parser, 0) - '0');
      parser->position++;
      high_digits++;
    }
    if (high_digits) {
      max = high;
    }
  }

  // The close is spelled the way the dialect spells its operators: `}` in an
  // ERE, `\}` in a BRE.
  if (is_basic(parser)) {
    if (byte_at(parser, 0) != '\\' || byte_at(parser, 1) != '}') {
      return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_BRACE, start,
          parser->position - start);
    }
    parser->position += 2;
  }
  else {
    if (byte_at(parser, 0) != '}') {
      return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_BRACE, start,
          parser->position - start);
    }
    parser->position++;
  }

  out_quantifier->min = min;
  out_quantifier->max = max;
  out_quantifier->is_quantifier = 1;
  return GRX_OK;
}

/**
 * Whether `^` at this offset is an anchor, in a basic RE.
 *
 * POSIX: `^` is an anchor when it is the first character of the whole RE or
 * of a subexpression, and an ordinary character everywhere else. GNU adds
 * the alternative, so it is an anchor after `\|` too - glibc matches "b"
 * with `a\|^b`. Both openers are two bytes, which is what makes this a
 * look-back of exactly two.
 */
static int caret_is_anchor(const GRX_Parser * parser, size_t offset) {
  if (offset == 0) {
    return 1;
  }
  if (offset < 2 || parser->text[offset - 2] != '\\') {
    return 0;
  }
  char opener = parser->text[offset - 1];
  return opener == '(' || opener == '|';
}

/**
 * Whether `$` ending at this offset is an anchor, in a basic RE.
 *
 * The mirror of caret_is_anchor(): last in the whole RE, last in a
 * subexpression, or last in an alternative.
 */
static int dollar_is_anchor(const GRX_Parser * parser, size_t end) {
  if (end >= parser->length) {
    return 1;
  }
  if (end + 1 >= parser->length || parser->text[end] != '\\') {
    return 0;
  }
  char closer = parser->text[end + 1];
  return closer == ')' || closer == '|';
}

/**
 * Turn a character with no operator meaning into a node.
 *
 * `^` and `$` are the whole of the difference between the two grammars here.
 * In an ERE they are anchors wherever they stand - glibc refuses `^*` and
 * matches nothing for `a^b`, which is an anchor in a place nothing can
 * satisfy. In a BRE they are anchors only where an anchor could be meant:
 * `^` at the start of the RE or of a subexpression or of an alternative,
 * `$` at the end of one. Everywhere else they are ordinary characters, which
 * is why `a^b` matches "a^b" in a BRE and nothing in an ERE.
 */
static GRX_Result px_literal_atom(GRX_Parser * parser, uint32_t codepoint,
    size_t offset, size_t length, uint32_t * out_node) {
  // A bare `{` reaching here is one with no atom before it, the shared
  // parser having asked for an interval only after an atom. glibc refuses
  // `{1}` and `{1` alike, which is the one place POSIX's "undefined" is not
  // resolved the permissive way.
  if (codepoint == '{' && !is_basic(parser)
      && (parser->spec.features & GRX_FEATURE_BOUNDED_REPEAT)) {
    return grx_parse_fail(parser, GRX_DIAG_NOTHING_TO_REPEAT, offset, length);
  }
  if (codepoint == '^' && (!is_basic(parser) || caret_is_anchor(parser, offset))) {
    return anchor_node(parser, GRX_ANCHOR_CARET, offset, out_node);
  }
  if (codepoint == '$'
      && (!is_basic(parser) || dollar_is_anchor(parser, offset + length))) {
    return anchor_node(parser, GRX_ANCHOR_DOLLAR, offset, out_node);
  }
  return grx_parse_literal_node(parser, codepoint, offset, length, out_node);
}

/**
 * Whether the quantifier here applies, or is a literal asterisk.
 *
 * POSIX: an `*` that is the first character of the RE or of a subexpression,
 * after an initial `^` if there is one, is an ordinary character. With no
 * atom before it the parser has already read it as one; this is the `^*`
 * case, where the anchor is the atom and the `*` would otherwise repeat it.
 * glibc matches "*a" with `^*a`, so there the `*` is a character.
 *
 * Only in a basic RE. An extended one refuses `^*` outright, which it
 * reaches through check_quantifier_target() below.
 *
 * Every anchor, not only `^`. POSIX states the rule for the start of an RE
 * and for a `*` after `^`, and glibc applies it after `\<`, `\>`, `\b` and
 * `\B` too - `\>*` against "a*" matches 1-2, and `\>**` against "a**"
 * matches 1-3, which is a literal asterisk and then a quantifier over it.
 * Reading it as a repeat of the anchor would make both of those nomatch.
 */
static int px_quantifier_applies(GRX_Parser * parser, uint32_t node) {
  if (!is_basic(parser) || parser->text[parser->position] != '*') {
    return 1;
  }
  const GRX_Node * atom = grx_pattern_node(parser->pattern, node);
  return !(atom && atom->kind == GRX_NODE_ANCHOR);
}

/**
 * Refuse a quantifier the dialect does not allow on this atom.
 *
 * An extended RE refuses to repeat an anchor: glibc answers `^*` with a
 * compile error rather than with a literal asterisk, which is the one place
 * the two grammars disagree about the same three characters.
 */
static GRX_Result px_check_quantifier_target(GRX_Parser * parser,
    uint32_t node, size_t offset, size_t length) {
  const GRX_Node * atom = grx_pattern_node(parser->pattern, node);
  if (!atom) {
    return GRX_OK;
  }

  // Neither grammar repeats an anchor - any anchor, not just the two that
  // were listed here when this was written. In a basic RE a `*` after one
  // never arrives at all, px_quantifier_applies() having already said it is
  // a character, so what this refuses there is `^\{1\}` and `\<\{1\}`,
  // which glibc refuses too.
  //
  // It also refuses the basic RE's `\<\?` and `\<\+`, where glibc instead
  // compiles a pattern that cannot match anything: `\<\?` against "" is
  // nomatch there, and an optional assertion that declines to match the
  // empty string is not a rule, it is an artifact. musl matches it, so the
  // two references disagree and there is nothing to reproduce. Refusing says
  // what is true - the construct means nothing - and documentation/dialects.md
  // section 6 records the deviation.
  if (atom->kind == GRX_NODE_ANCHOR) {
    return grx_parse_fail(parser, GRX_DIAG_NOTHING_TO_REPEAT, offset, length);
  }

  // A second quantifier. An extended RE stacks them freely - `a**` and
  // `a{2}{3}` both match in glibc - and a basic one accepts only `\+` and
  // `\?` as the second, which is a rule found by asking rather than by
  // reading: `a*\?` and `a*\+` match, while `a**`, `a\+*`, `a*\{1\}`
  // and `a\{1\}\{1\}` are all refused.
  if (is_basic(parser) && atom->kind == GRX_NODE_REPEAT) {
    int escaped_suffix = length == 2 && parser->text[offset] == '\\'
        && (parser->text[offset + 1] == '+'
            || parser->text[offset + 1] == '?');
    if (!escaped_suffix) {
      return grx_parse_fail(
          parser, GRX_DIAG_DOUBLE_QUANTIFIER, offset, length);
    }
  }
  return GRX_OK;
}

/**
 * Whether this subtree opens capturing group `group`.
 *
 * Used only by the check below, which asks it of one branch of an
 * alternation at a time.
 */
static int px_defines_group(
    const GRX_Pattern * pattern, uint32_t index, uint32_t group) {
  const GRX_Node * node = grx_pattern_node(pattern, index);
  if (!node) {
    return 0;
  }
  if (node->kind == GRX_NODE_GROUP && (node->flags & GRX_NODE_CAPTURING)
      && node->a == group) {
    return 1;
  }
  for (uint32_t child = node->first_child; child != GRX_INDEX_NONE;) {
    const GRX_Node * part = grx_pattern_node(pattern, child);
    if (!part) {
      return 0;
    }
    if (px_defines_group(pattern, child, group)) {
      return 1;
    }
    child = part->next_sibling;
  }
  return 0;
}

/**
 * The first backreference in this subtree naming a group another branch of
 * `alternation` defines, or GRX_INDEX_NONE.
 */
static uint32_t px_reference_across(const GRX_Pattern * pattern,
    uint32_t index, uint32_t alternation, uint32_t own_branch) {
  const GRX_Node * node = grx_pattern_node(pattern, index);
  if (!node) {
    return GRX_INDEX_NONE;
  }
  if (node->kind == GRX_NODE_BACKREF) {
    const GRX_Node * alt = grx_pattern_node(pattern, alternation);
    for (uint32_t branch = alt ? alt->first_child : GRX_INDEX_NONE;
        branch != GRX_INDEX_NONE;) {
      const GRX_Node * part = grx_pattern_node(pattern, branch);
      if (!part) {
        break;
      }
      if (branch != own_branch
          && px_defines_group(pattern, branch, node->a)) {
        return index;
      }
      branch = part->next_sibling;
    }
  }
  for (uint32_t child = node->first_child; child != GRX_INDEX_NONE;) {
    const GRX_Node * part = grx_pattern_node(pattern, child);
    if (!part) {
      break;
    }
    uint32_t found
        = px_reference_across(pattern, child, alternation, own_branch);
    if (found != GRX_INDEX_NONE) {
      return found;
    }
    child = part->next_sibling;
  }
  return GRX_INDEX_NONE;
}

/** The first reference that crosses an alternation, anywhere in the tree. */
static uint32_t px_crossing_reference(
    const GRX_Pattern * pattern, uint32_t index) {
  const GRX_Node * node = grx_pattern_node(pattern, index);
  if (!node) {
    return GRX_INDEX_NONE;
  }
  if (node->kind == GRX_NODE_ALTERNATE) {
    for (uint32_t branch = node->first_child; branch != GRX_INDEX_NONE;) {
      const GRX_Node * part = grx_pattern_node(pattern, branch);
      if (!part) {
        break;
      }
      uint32_t found
          = px_reference_across(pattern, branch, index, branch);
      if (found != GRX_INDEX_NONE) {
        return found;
      }
      branch = part->next_sibling;
    }
  }
  for (uint32_t child = node->first_child; child != GRX_INDEX_NONE;) {
    const GRX_Node * part = grx_pattern_node(pattern, child);
    if (!part) {
      break;
    }
    uint32_t found = px_crossing_reference(pattern, child);
    if (found != GRX_INDEX_NONE) {
      return found;
    }
    child = part->next_sibling;
  }
  return GRX_INDEX_NONE;
}

/**
 * Refuse a backreference to a group in another alternative.
 *
 * glibc's: `(a)|(b)\1` is "Invalid back reference" there, and so is
 * `\(a\)\|\(b\)\1` in the basic grammar, while `(a)\1|(b)` - the reference
 * and its group in the *same* branch - compiles, and so does
 * `((a)|(b))\3`, where the reference is outside the alternation that holds
 * the group. The rule is the crossing and not the alternation.
 *
 * It is the GNU rows' rule alone, because glibc is what defines them. The
 * two POSIX rows are decided by glibc and musl agreeing, and they do not
 * agree here - musl compiles all of these and matches - so the standard's
 * "undefined" is left undefined rather than settled from one side.
 */
static GRX_Result px_validate(GRX_Parser * parser) {
  uint32_t crossing
      = px_crossing_reference(parser->pattern, parser->pattern->root);
  if (crossing == GRX_INDEX_NONE) {
    return GRX_OK;
  }
  const GRX_Node * node = grx_pattern_node(parser->pattern, crossing);
  return grx_parse_fail(parser, GRX_DIAG_INVALID_BACKREFERENCE,
      node ? node->offset : 0, node ? node->length : 0);
}

const GRX_Frontend grx_frontend_posix_bre = {
  .name = "posix-bre",
  .atom_escape = px_atom_escape,
  .class_escape = px_class_escape,
  .char_class = px_char_class,
  .group_open = px_group_open,
  .brace_quantifier = px_brace_quantifier,
  .literal_atom = px_literal_atom,
  .check_quantifier_target = px_check_quantifier_target,
  .quantifier_applies = px_quantifier_applies,
};

const GRX_Frontend grx_frontend_posix_ere = {
  .name = "posix-ere",
  .atom_escape = px_atom_escape,
  .class_escape = px_class_escape,
  .char_class = px_char_class,
  .group_open = px_group_open,
  .brace_quantifier = px_brace_quantifier,
  .literal_atom = px_literal_atom,
  .check_quantifier_target = px_check_quantifier_target,
  .quantifier_applies = px_quantifier_applies,
};

const GRX_Frontend grx_frontend_gnu_bre = {
  .name = "gnu-bre",
  .validate = px_validate,
  .atom_escape = px_atom_escape,
  .class_escape = px_class_escape,
  .char_class = px_char_class,
  .group_open = px_group_open,
  .brace_quantifier = px_brace_quantifier,
  .literal_atom = px_literal_atom,
  .check_quantifier_target = px_check_quantifier_target,
  .quantifier_applies = px_quantifier_applies,
};

const GRX_Frontend grx_frontend_gnu_ere = {
  .name = "gnu-ere",
  .validate = px_validate,
  .atom_escape = px_atom_escape,
  .class_escape = px_class_escape,
  .char_class = px_char_class,
  .group_open = px_group_open,
  .brace_quantifier = px_brace_quantifier,
  .literal_atom = px_literal_atom,
  .check_quantifier_target = px_check_quantifier_target,
  .quantifier_applies = px_quantifier_applies,
};
