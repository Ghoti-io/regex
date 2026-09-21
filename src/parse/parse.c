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
 * The pattern parser: dialect text to a node tree.
 *
 * One recursive-descent parser for every dialect. What varies between
 * dialects is factored three ways (documentation/design.md section 4): the
 * feature bits say which constructs exist, the profile says what they mean,
 * and the @ref GRX_Frontend hooks say how to *read* the ones a table cannot
 * describe. This file is what is left when those three are taken out - the
 * grammar every regular-expression syntax shares:
 *
 *     alternation := concatenation ( '|' concatenation )*
 *     concatenation := term*
 *     term := atom quantifier?
 *     atom := group | class | '.' | anchor | escape | literal
 *
 * The parser owns nesting depth, the unmatched-parenthesis diagnostic, the
 * repeat bounds and the node building, so that a front end is a table of
 * rules rather than a second parser. It owns no dialect knowledge at all: a
 * `switch (parser->syntax)` here would be the same defect that
 * `make check-layering` forbids below the IR, one level up.
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/pattern.h>
#include <stddef.h>
#include <string.h>

#include "../core/core_internal.h"
#include "../unicode/unicode_internal.h"
#include "parse_internal.h"

// --------------------------------------------------------------------------
// Reading the pattern text
// --------------------------------------------------------------------------

GRX_Result grx_parse_fail(
    GRX_Parser * parser, GRX_Diag diag, size_t offset, size_t length) {
  if (!parser) {
    return GRX_ERR_INVALID;
  }

  return grx_error_set(
      parser->error, grx_diag_result(diag), diag, offset, length);
}

int grx_parse_at_end(const GRX_Parser * parser) {
  return !parser || parser->position >= parser->length;
}

GRX_Result grx_parse_peek(
    const GRX_Parser * parser, uint32_t * out_codepoint, size_t * out_width) {
  if (!parser || !out_codepoint) {
    return GRX_ERR_INVALID;
  }
  if (parser->position >= parser->length) {
    return GRX_ERR_LIMIT;
  }

  // The pattern is read as UTF-8 whatever the dialect, because that is the
  // encoding the caller handed over. A dialect defined over UTF-16 code
  // units - ECMAScript without `u` - therefore sees an astral character as
  // one code point rather than two, which is a deviation and is recorded as
  // one in documentation/dialects.md section 6.
  size_t width = grx_unicode_utf8_decode(parser->text + parser->position,
      parser->length - parser->position, out_codepoint);
  if (!width) {
    return GRX_ERR_SYNTAX;
  }
  if (out_width) {
    *out_width = width;
  }

  return GRX_OK;
}

GRX_Result grx_parse_take(GRX_Parser * parser, uint32_t * out_codepoint) {
  size_t width = 0;
  GRX_Result result = grx_parse_peek(parser, out_codepoint, &width);
  if (result == GRX_ERR_SYNTAX) {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_UTF8_IN_PATTERN, parser->position, 1);
  }
  if (result != GRX_OK) {
    return result;
  }

  parser->position += width;
  return GRX_OK;
}

int grx_parse_eat(GRX_Parser * parser, char expected) {
  if (!parser || parser->position >= parser->length
      || parser->text[parser->position] != expected) {
    return 0;
  }

  parser->position++;
  return 1;
}

/**
 * Whether the parser is inside a quoted run, and closes one that has ended.
 *
 * The close is done on the way past rather than by whoever opened the run,
 * because the run's end is a *position* and every path that moves the
 * position would otherwise have to check for it. Once it is closed the `\E`
 * is still there, and the front end reads it as the ordinary escape it also
 * is when there was no `\Q` - which is what PCRE2 and Java both do with a
 * stray `\E`.
 */
/**
 * Whether this dialect spells its grouping operators with a backslash.
 *
 * A POSIX basic RE inverts the usual rule: `\(` opens a group and `(` is an
 * ordinary character, `\{` begins an interval and `{` is a brace, and GNU's
 * `\|` alternates where `|` is a pipe. The spec table has said so since the
 * table was written - `escaped_specials` on the two BRE rows - and nothing
 * read it, which is the same defect shape as a limit that is in the header
 * and enforced nowhere.
 *
 * Only the operators that *nest or join* invert. `*` is bare in a BRE as
 * well, which is why this is asked per operator rather than once.
 */
static int operators_are_escaped(const GRX_Parser * parser) {
  return parser->spec.escaped_specials != 0;
}

/** Whether `op`, spelled the way this dialect spells it, stands here. */
static int at_operator(const GRX_Parser * parser, char op) {
  size_t at = parser->position;
  if (operators_are_escaped(parser)) {
    return at + 1 < parser->length && parser->text[at] == '\\'
        && parser->text[at + 1] == op;
  }
  return at < parser->length && parser->text[at] == op;
}

/** How many bytes this dialect's spelling of an operator takes. */
static size_t operator_width(const GRX_Parser * parser) {
  return operators_are_escaped(parser) ? 2u : 1u;
}

/** Consume `op` if it stands here, and say whether it did. */
static int eat_operator(GRX_Parser * parser, char op) {
  if (!at_operator(parser, op)) {
    return 0;
  }
  parser->position += operator_width(parser);
  return 1;
}

static int in_quote(GRX_Parser * parser) {
  if (parser->quote_end == GRX_NPOS) {
    return 0;
  }
  if (parser->position < parser->quote_end) {
    return 1;
  }

  parser->quote_end = GRX_NPOS;
  return 0;
}

/** Let the dialect consume what stands between atoms and means nothing. */
static GRX_Result skip_ignorable(GRX_Parser * parser) {
  if (!parser->frontend->skip_ignorable || in_quote(parser)) {
    return GRX_OK;
  }

  return parser->frontend->skip_ignorable(parser);
}

// --------------------------------------------------------------------------
// Building nodes
// --------------------------------------------------------------------------

GRX_Result grx_parse_literal_node(GRX_Parser * parser, uint32_t codepoint,
    size_t offset, size_t length, uint32_t * out_node) {
  if (!parser || !out_node) {
    return GRX_ERR_INVALID;
  }

  uint32_t first = (uint32_t)parser->pattern->literals.count;
  GRX_Result result
      = grx_arena_append(&parser->pattern->literals, &codepoint, NULL);
  if (result != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, offset, length);
  }

  result = grx_pattern_add_node(
      parser->pattern, GRX_NODE_LITERAL, offset, length, out_node);
  if (result != GRX_OK) {
    return grx_parse_fail(parser,
        result == GRX_ERR_LIMIT ? GRX_DIAG_LIMIT_NODES : GRX_DIAG_OUT_OF_MEMORY,
        offset, length);
  }

  GRX_Node * node = grx_pattern_node(parser->pattern, *out_node);
  node->a = first;
  node->b = 1;
  return GRX_OK;
}

GRX_Result grx_parse_class_node(
    GRX_Parser * parser, size_t offset, uint32_t * out_node) {
  if (!parser || !out_node) {
    return GRX_ERR_INVALID;
  }

  GRX_Result result
      = grx_pattern_add_node(parser->pattern, GRX_NODE_CLASS, offset, 0,
          out_node);
  if (result != GRX_OK) {
    return grx_parse_fail(parser,
        result == GRX_ERR_LIMIT ? GRX_DIAG_LIMIT_NODES : GRX_DIAG_OUT_OF_MEMORY,
        offset, 1);
  }

  GRX_Node * node = grx_pattern_node(parser->pattern, *out_node);
  node->a = (uint32_t)parser->pattern->class_items.count;
  node->b = 0;
  return GRX_OK;
}

GRX_Result grx_parse_class_add(
    GRX_Parser * parser, uint32_t node_index, const GRX_ClassItem * item) {
  if (!parser || !item) {
    return GRX_ERR_INVALID;
  }

  GRX_Node * node = grx_pattern_node(parser->pattern, node_index);
  if (!node || node->kind != GRX_NODE_CLASS) {
    return GRX_ERR_INVALID;
  }
  // A class node names a span of the item table, so its items must be
  // contiguous. Anything else means a second class was opened between two of
  // this one's items, and the span would silently include the other's.
  if (node->a + node->b != parser->pattern->class_items.count) {
    return GRX_ERR_INTERNAL;
  }

  GRX_Result result
      = grx_arena_append(&parser->pattern->class_items, item, NULL);
  if (result != GRX_OK) {
    return grx_parse_fail(parser,
        result == GRX_ERR_LIMIT ? GRX_DIAG_LIMIT_CLASS_RANGES
                                : GRX_DIAG_OUT_OF_MEMORY,
        item->offset, item->length);
  }

  // Re-read: the append may have moved the node arena? No - items and nodes
  // are separate arenas, so `node` is still valid. It is re-read anyway,
  // because relying on that is relying on a fact about a different arena.
  node = grx_pattern_node(parser->pattern, node_index);
  node->b++;
  return GRX_OK;
}

GRX_Result grx_parse_shorthand_node(GRX_Parser * parser,
    GRX_ShorthandKind shorthand, int negated, size_t offset, size_t length,
    uint32_t * out_node) {
  GRX_Result result = grx_parse_class_node(parser, offset, out_node);
  if (result != GRX_OK) {
    return result;
  }

  GRX_ClassItem item = {
    .kind = GRX_CLASS_ITEM_SHORTHAND,
    .flags = negated ? GRX_CLASS_ITEM_NEGATED : 0,
    .lo = 0,
    .hi = 0,
    .a = (uint32_t)shorthand,
    .offset = offset,
    .length = length,
  };
  result = grx_parse_class_add(parser, *out_node, &item);
  if (result != GRX_OK) {
    return result;
  }

  grx_pattern_node(parser->pattern, *out_node)->length = length;
  return GRX_OK;
}

/** Append a node of a kind with no payload, reporting a failure as a diagnostic. */
static GRX_Result add_node(GRX_Parser * parser, GRX_NodeKind kind,
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

/** Attach a child, turning an index failure into an internal diagnostic. */
static GRX_Result add_child(
    GRX_Parser * parser, uint32_t parent, uint32_t child) {
  GRX_Result result = grx_pattern_add_child(parser->pattern, parent, child);
  if (result != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_INTERNAL, GRX_NPOS, 0);
  }

  return GRX_OK;
}

// --------------------------------------------------------------------------
// The prescan
// --------------------------------------------------------------------------

/**
 * Count capturing groups and notice named ones, before parsing begins.
 *
 * Two dialects' escapes cannot be read without this. `\1` in ECMAScript
 * without `u` is a backreference when the pattern has a group 1 and a legacy
 * octal escape otherwise; `\k` is a named reference only when the pattern
 * names a group *somewhere*, which may be after the reference. ECMA-262
 * 22.2.1 does the same two-pass reading, for the same reason.
 *
 * This is a lexical scan, not a parse: it only needs to know which `(` are
 * capturing, so it tracks escapes and bracket nesting and nothing else. It
 * cannot fail - a malformed pattern is the real parse's business, and a
 * prescan that reported errors would report them in the wrong order.
 */
static void prescan(GRX_Parser * parser) {
  int in_class = 0;
  int first_in_class = 0;
  // In a basic RE the group opener *is* a backslash sequence, so the skip
  // below would step straight over every group there is. `a\(b*\)c\1d`
  // counted no groups at all, which made its `\1` a reference to a group
  // that does not exist.
  const int escaped = parser->spec.escaped_specials != 0;

  for (size_t i = 0; i < parser->length; i++) {
    char c = parser->text[i];
    if (c == '\\') {
      if (escaped && !in_class && i + 1 < parser->length
          && parser->text[i + 1] == '(') {
        parser->group_count++;
      }
      i++; // Skip whatever it escapes, including `[`, `]` and `(`.
      continue;
    }
    if (in_class) {
      if (c == ']' && !(first_in_class && !parser->spec.allow_empty_class)) {
        in_class = 0;
      }
      first_in_class = 0;
      continue;
    }
    if (c == '[') {
      in_class = 1;
      first_in_class = 1;
      // `[^]` puts the caret before the first member, so the `]` that may be
      // a literal is the one after it.
      if (i + 1 < parser->length && parser->text[i + 1] == '^') {
        i++;
      }
      continue;
    }
    // A bare `(` is an ordinary character where the operators are escaped.
    if (c != '(' || escaped) {
      continue;
    }

    if (i + 1 < parser->length && parser->text[i + 1] == '*') {
      // `(*ACCEPT)` and the leading directives. Not a group of any kind, and
      // counting one here would make `\1` in `(*UTF)\1` a backreference to a
      // group that does not exist.
      continue;
    }
    if (i + 1 >= parser->length || parser->text[i + 1] != '?') {
      parser->group_count++;
      continue;
    }
    // `(?<name>` captures and names; `(?<=` and `(?<!` are lookbehind and do
    // neither, and telling them apart is the whole reason this looks past
    // the `<`.
    if (i + 3 < parser->length && parser->text[i + 2] == '<'
        && parser->text[i + 3] != '=' && parser->text[i + 3] != '!') {
      parser->group_count++;
      parser->named_groups = 1;
    }
    // `(?P<name>` and `(?'name'`, for the dialects that spell it that way.
    else if (i + 4 < parser->length && parser->text[i + 2] == 'P'
        && parser->text[i + 3] == '<') {
      parser->group_count++;
      parser->named_groups = 1;
    }
    else if (i + 3 < parser->length && parser->text[i + 2] == '\'') {
      parser->group_count++;
      parser->named_groups = 1;
    }
  }
}

// --------------------------------------------------------------------------
// The grammar
// --------------------------------------------------------------------------

// Forward declaration: parse_quantifier() and the alternation both use it.

/**
 * Read `*`, `+`, `?` or `{m,n}` and the lazy or possessive suffix, and wrap
 * `atom` in a repeat node.
 *
 * `applied` is set when a quantifier was found, so that the caller can reject
 * a second one: `a**` is an error in every dialect but GNU's and Ruby's, and
 * the way to say so is to notice the second rather than to loop.
 */
static GRX_Result parse_quantifier(
    GRX_Parser * parser, uint32_t * node, int * applied) {
  *applied = 0;
  if (in_quote(parser)) {
    // `\Qa*\E` is two literals. The run is still open, so the `*` is not a
    // quantifier and must not be read as one.
    return GRX_OK;
  }
  GRX_Result skipped = skip_ignorable(parser);
  if (skipped != GRX_OK) {
    return skipped;
  }
  if (grx_parse_at_end(parser)) {
    return GRX_OK;
  }

  size_t start = parser->position;
  uint32_t min = 0;
  uint32_t max = GRX_REPEAT_INF;

  // Asked before anything is consumed: a dialect may say the character here
  // is not a quantifier at all, and then it must be left for the next atom.
  if (parser->frontend->quantifier_applies
      && !parser->frontend->quantifier_applies(parser, *node)) {
    return GRX_OK;
  }

  // `*` is bare in every dialect, a BRE included. `+`, `?` and `{` are the
  // ones a BRE spells with a backslash, which is why each is asked for
  // rather than switched on the character here.
  if (parser->text[parser->position] == '*') {
    parser->position++;
  }
  else if (at_operator(parser, '+')) {
    min = 1;
    parser->position += operator_width(parser);
  }
  else if (at_operator(parser, '?')) {
    max = 1;
    parser->position += operator_width(parser);
  }
  else if (at_operator(parser, '{')) {
    if (!(parser->spec.features & GRX_FEATURE_BOUNDED_REPEAT)) {
      return GRX_OK;
    }
    parser->position += operator_width(parser);
    GRX_Quantifier bounds = {0, GRX_REPEAT_INF, 0};
    GRX_Result result
        = parser->frontend->brace_quantifier(parser, &bounds);
    if (result != GRX_OK) {
      return result;
    }
    if (!bounds.is_quantifier) {
      // The dialect says this `{` is an ordinary character. Rewind so the
      // caller reads it as one.
      parser->position = start;
      return GRX_OK;
    }
    if (bounds.min > bounds.max && !parser->spec.allow_impossible_repeat) {
      return grx_parse_fail(parser, GRX_DIAG_QUANTIFIER_OUT_OF_ORDER, start,
          parser->position - start);
    }
    if (parser->limits->max_repeat_count
        && (bounds.max != GRX_REPEAT_INF
            && bounds.max > parser->limits->max_repeat_count)) {
      return grx_parse_fail(parser, GRX_DIAG_LIMIT_REPEAT_COUNT, start,
          parser->position - start);
    }
    min = bounds.min;
    max = bounds.max;
  }
  else {
    return GRX_OK;
  }

  // Whether this atom may be repeated at all is the dialect's call, not
  // this file's: `(?=a)*` is legal ECMAScript without `u` and a syntax error
  // with it, and no shared rule can say both.
  GRX_Result allowed = parser->frontend->check_quantifier_target(
      parser, *node, start, parser->position - start);
  if (allowed != GRX_OK) {
    return allowed;
  }

  // The suffix is part of the quantifier, and extended mode lets it be
  // written apart from the rest: `a + +` is `a++`, and is in the corpus.
  GRX_Result skipped_suffix = skip_ignorable(parser);
  if (skipped_suffix != GRX_OK) {
    return skipped_suffix;
  }

  // The lazy suffix is only read by a dialect that has one. Where there is
  // none, the `?` is left where it is: POSIX reads `a*?` as `?` applied to
  // `a*` - glibc matches "aa" with it - and consuming it here to report a
  // double quantifier would refuse a pattern the reference accepts, and
  // would report it at the wrong place for the dialects that do refuse it.
  // parse_term() is where a second quantifier is judged.
  GRX_RepeatMode mode = GRX_REPEAT_GREEDY;
  if ((parser->spec.features & GRX_FEATURE_NON_GREEDY)
      && grx_parse_eat(parser, '?')) {
    mode = GRX_REPEAT_LAZY;
  }
  else if ((parser->spec.features & GRX_FEATURE_POSSESSIVE)
      && grx_parse_eat(parser, '+')) {
    mode = GRX_REPEAT_POSSESSIVE;
  }

  // Ungreedy inverts the two orderings but leaves a possessive alone, which
  // is what every dialect offering the option does.
  if ((parser->options & GRX_OPT_UNGREEDY) && mode != GRX_REPEAT_POSSESSIVE) {
    mode = mode == GRX_REPEAT_GREEDY ? GRX_REPEAT_LAZY : GRX_REPEAT_GREEDY;
  }

  uint32_t repeat = GRX_INDEX_NONE;
  const GRX_Node * atom = grx_pattern_node(parser->pattern, *node);
  size_t atom_offset = atom ? atom->offset : start;
  GRX_Result result = add_node(parser, GRX_NODE_REPEAT, atom_offset,
      parser->position - atom_offset, &repeat);
  if (result != GRX_OK) {
    return result;
  }

  GRX_Node * repeat_node = grx_pattern_node(parser->pattern, repeat);
  repeat_node->a = (uint32_t)mode;
  repeat_node->min = min;
  repeat_node->max = max;

  result = add_child(parser, repeat, *node);
  if (result != GRX_OK) {
    return result;
  }

  *node = repeat;
  *applied = 1;
  return GRX_OK;
}

/** Read one atom: a group, a class, a metacharacter, an escape or a literal. */
static GRX_Result parse_atom(GRX_Parser * parser, uint32_t * out_node) {
  size_t start = parser->position;

  if (in_quote(parser)) {
    // Inside `\Q...\E` every character is a literal, including the ones
    // that are operators everywhere else. Not through literal_atom(): that
    // hook decides whether a character with no operator meaning is a literal
    // at all, and here the question does not arise.
    uint32_t quoted = 0;
    GRX_Result result = grx_parse_take(parser, &quoted);
    if (result != GRX_OK) {
      return result;
    }
    return grx_parse_literal_node(
        parser, quoted, start, parser->position - start, out_node);
  }

  char c = parser->text[parser->position];

  // Asked before the `\\` dispatch below, because in a BRE the group opener
  // *is* a backslash sequence and must not be handed to atom_escape.
  if (at_operator(parser, '(')) {
    parser->position += operator_width(parser);
    // Saved before the hook runs, not after: a hook that reads `(?i:` sets
    // the options as part of reading it, and a save taken afterwards would
    // restore the value it had just written. `^a(?i:b)c$` matching "aBC" is
    // what that looked like.
    uint32_t outer_options = parser->options;
    GRX_GroupOpen open = {GRX_NODE_GROUP, 0, 0, GRX_INDEX_NONE, 1, NULL};
    GRX_Result result = parser->frontend->group_open(parser, &open);
    if (result != GRX_OK) {
      return result;
    }

    result = add_node(parser, open.kind, start, 0, out_node);
    if (result != GRX_OK) {
      return result;
    }
    GRX_Node * node = grx_pattern_node(parser->pattern, *out_node);
    node->flags = open.flags;
    node->a = open.a;
    node->b = open.b;

    if (open.has_body) {
      if (parser->limits->max_nesting_depth
          && parser->depth + 1 > parser->limits->max_nesting_depth) {
        return grx_parse_fail(parser, GRX_DIAG_LIMIT_NESTING_DEPTH, start, 1);
      }
      parser->depth++;
      parser->group_depth++;
      int outer_lookbehind = parser->in_lookbehind;
      int outer_lookaround = parser->in_lookaround;
      if (open.kind == GRX_NODE_LOOKAROUND) {
        parser->in_lookaround = 1;
        if (open.a == GRX_LOOK_BEHIND_POSITIVE
            || open.a == GRX_LOOK_BEHIND_NEGATIVE
            || open.a == GRX_LOOK_BEHIND_NON_ATOMIC) {
          parser->in_lookbehind = 1;
        }
      }

      // The options a group sets are the group's own: `(a(?i)b)c` matches
      // "aBc" and not "abC". Restored here rather than by whoever sets them,
      // because it is this scope that ends at the `)`.
      if (open.read_body) {
        result = open.read_body(parser, *out_node);
      }
      else {
        uint32_t body = GRX_INDEX_NONE;
        result = grx_parse_alternation(parser, &body);
        if (result == GRX_OK) {
          result = add_child(parser, *out_node, body);
        }
      }
      parser->depth--;
      parser->group_depth--;
      parser->in_lookbehind = outer_lookbehind;
      parser->in_lookaround = outer_lookaround;
      parser->options = outer_options;
      if (result != GRX_OK) {
        return result;
      }

      if (!eat_operator(parser, ')')) {
        return grx_parse_fail(
            parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 1);
      }
    }

    grx_pattern_node(parser->pattern, *out_node)->length
        = parser->position - start;
    return GRX_OK;
  }

  if (c == '[') {
    parser->position++;
    return parser->frontend->char_class(parser, out_node);
  }

  if (c == '\\') {
    parser->position++;
    if (grx_parse_at_end(parser)) {
      return grx_parse_fail(parser, GRX_DIAG_TRAILING_BACKSLASH, start, 1);
    }
    return parser->frontend->atom_escape(parser, out_node);
  }

  if (c == '.') {
    parser->position++;
    return add_node(parser, GRX_NODE_ANY, start, 1, out_node);
  }

  // Where the operators are escaped, none of the three groups below is an
  // operator: a basic RE's `^` is an anchor only at the start of the RE or
  // of a subexpression, its `*` is a character wherever no atom precedes it,
  // and its `)` is just a right parenthesis. All three go to literal_atom(),
  // whose whole purpose is that "this character has no operator meaning
  // here" is a dialect's call - and the front end turns the two anchors back
  // into anchors where they are ones.
  if (!operators_are_escaped(parser)) {
    if (c == '^' || c == '$') {
      parser->position++;
      GRX_Result result
          = add_node(parser, GRX_NODE_ANCHOR, start, 1, out_node);
      if (result != GRX_OK) {
        return result;
      }
      grx_pattern_node(parser->pattern, *out_node)->a
          = c == '^' ? GRX_ANCHOR_CARET : GRX_ANCHOR_DOLLAR;
      return GRX_OK;
    }

    if (c == '*' || c == '+' || c == '?') {
      // A quantifier with nothing before it. Reported here rather than in
      // parse_quantifier() because at this point there is provably no atom -
      // the caller only calls this when it is about to read one.
      return grx_parse_fail(parser, GRX_DIAG_NOTHING_TO_REPEAT, start, 1);
    }

    if (c == ')' && !parser->spec.unmatched_close_is_literal) {
      return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_CLOSE_PAREN, start, 1);
    }
  }

  uint32_t codepoint = 0;
  GRX_Result result = grx_parse_take(parser, &codepoint);
  if (result != GRX_OK) {
    return result;
  }

  // Through the hook, not straight to a literal node: whether a character
  // with no operator meaning is a literal at all is a dialect rule.
  return parser->frontend->literal_atom(
      parser, codepoint, start, parser->position - start, out_node);
}

/** Read one term: an atom and whatever quantifiers the dialect allows on it. */
static GRX_Result parse_term(GRX_Parser * parser, uint32_t * out_node) {
  GRX_Result result = parse_atom(parser, out_node);
  if (result != GRX_OK) {
    return result;
  }

  int applied = 0;
  result = parse_quantifier(parser, out_node, &applied);
  if (result != GRX_OK) {
    return result;
  }
  if (!applied) {
    return GRX_OK;
  }

  // A second quantifier on the same atom. `a**` is an error everywhere but
  // GNU's syntaxes, which is a feature bit rather than a special case here.
  size_t second = parser->position;
  int again = 0;
  result = parse_quantifier(parser, out_node, &again);
  if (result != GRX_OK) {
    return result;
  }
  if (!again) {
    return GRX_OK;
  }
  if (!parser->spec.allow_double_quantifier) {
    return grx_parse_fail(parser, GRX_DIAG_DOUBLE_QUANTIFIER, second,
        parser->position - second);
  }

  // A dialect that allows a second allows a third: glibc reads `a***` and
  // `a{1}{1}{1}` as repeats of repeats, and stopping at two would refuse a
  // pattern the reference accepts. Each turn consumes at least one byte -
  // parse_quantifier only reports one applied when it read one - so the
  // loop is bounded by the pattern.
  for (;;) {
    int more = 0;
    result = parse_quantifier(parser, out_node, &more);
    if (result != GRX_OK) {
      return result;
    }
    if (!more) {
      break;
    }
  }

  return GRX_OK;
}

GRX_Result grx_parse_concatenation(
    GRX_Parser * parser, uint32_t * out_node) {
  if (!parser || !out_node) {
    return GRX_ERR_INVALID;
  }

  size_t start = parser->position;
  uint32_t concat = GRX_INDEX_NONE;
  uint32_t first = GRX_INDEX_NONE;
  size_t count = 0;

  for (;;) {
    GRX_Result skipped = skip_ignorable(parser);
    if (skipped != GRX_OK) {
      return skipped;
    }
    if (grx_parse_at_end(parser)) {
      break;
    }
    if (!in_quote(parser) && at_operator(parser, '|')) {
      break;
    }
    // A `)` ends a branch when there is a group for it to close. Where the
    // dialect makes an unmatched one an ordinary character, at the top level
    // it is not a terminator at all and the atom reader takes it.
    //
    // Only where the close is the bare character. A basic RE's `\)` is an
    // operator whatever stands around it - glibc refuses a lone `\)` and
    // matches a lone `)` - so the two questions are about two different
    // spellings and only the bare one is ever a literal.
    if (!in_quote(parser) && at_operator(parser, ')')
        && (parser->group_depth > 0 || operators_are_escaped(parser)
            || !parser->spec.unmatched_close_is_literal)) {
      break;
    }

    uint32_t term = GRX_INDEX_NONE;
    GRX_Result result = parse_term(parser, &term);
    if (result != GRX_OK) {
      return result;
    }

    count++;
    if (count == 1) {
      first = term;
      continue;
    }
    if (count == 2) {
      // The concat node is created only when there is a second term, so that
      // `a` is a literal rather than a concatenation of one thing - which
      // keeps the dump readable and the node count honest.
      result = add_node(parser, GRX_NODE_CONCAT, start, 0, &concat);
      if (result != GRX_OK) {
        return result;
      }
      result = add_child(parser, concat, first);
      if (result != GRX_OK) {
        return result;
      }
    }
    result = add_child(parser, concat, term);
    if (result != GRX_OK) {
      return result;
    }
  }

  if (!count) {
    return add_node(parser, GRX_NODE_EMPTY, start, 0, out_node);
  }
  if (count == 1) {
    *out_node = first;
    return GRX_OK;
  }

  grx_pattern_node(parser->pattern, concat)->length = parser->position - start;
  *out_node = concat;
  return GRX_OK;
}

GRX_Result grx_parse_alternation(GRX_Parser * parser, uint32_t * out_node) {
  if (!parser || !out_node) {
    return GRX_ERR_INVALID;
  }

  size_t start = parser->position;
  uint32_t first = GRX_INDEX_NONE;
  GRX_Result result = grx_parse_concatenation(parser, &first);
  if (result != GRX_OK) {
    return result;
  }

  if (grx_parse_at_end(parser) || !at_operator(parser, '|')
      || in_quote(parser)) {
    *out_node = first;
    return GRX_OK;
  }
  if (!(parser->spec.features & GRX_FEATURE_ALTERNATION)) {
    return grx_parse_fail(parser, GRX_DIAG_NOT_IN_DIALECT, parser->position, 1);
  }

  uint32_t alternate = GRX_INDEX_NONE;
  result = add_node(parser, GRX_NODE_ALTERNATE, start, 0, &alternate);
  if (result != GRX_OK) {
    return result;
  }
  result = add_child(parser, alternate, first);
  if (result != GRX_OK) {
    return result;
  }

  while (eat_operator(parser, '|')) {
    if (parser->limits->max_nesting_depth
        && parser->depth + 1 > parser->limits->max_nesting_depth) {
      return grx_parse_fail(
          parser, GRX_DIAG_LIMIT_NESTING_DEPTH, parser->position, 1);
    }
    parser->depth++;
    uint32_t branch = GRX_INDEX_NONE;
    result = grx_parse_concatenation(parser, &branch);
    parser->depth--;
    if (result != GRX_OK) {
      return result;
    }
    result = add_child(parser, alternate, branch);
    if (result != GRX_OK) {
      return result;
    }
  }

  grx_pattern_node(parser->pattern, alternate)->length
      = parser->position - start;
  *out_node = alternate;
  return GRX_OK;
}

// --------------------------------------------------------------------------
// The entry point
// --------------------------------------------------------------------------

/** Build the whole pattern as one literal run, for GRX_OPT_LITERAL. */
static GRX_Result parse_as_literal(GRX_Parser * parser) {
  uint32_t node = GRX_INDEX_NONE;
  GRX_Result result = add_node(parser, GRX_NODE_LITERAL, 0, parser->length,
      &node);
  if (result != GRX_OK) {
    return result;
  }

  GRX_Node * literal = grx_pattern_node(parser->pattern, node);
  literal->a = (uint32_t)parser->pattern->literals.count;
  literal->b = 0;

  while (!grx_parse_at_end(parser)) {
    uint32_t codepoint = 0;
    result = grx_parse_take(parser, &codepoint);
    if (result != GRX_OK) {
      return result;
    }
    result = grx_arena_append(&parser->pattern->literals, &codepoint, NULL);
    if (result != GRX_OK) {
      return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, 0, 0);
    }
    grx_pattern_node(parser->pattern, node)->b++;
  }

  parser->pattern->root = node;
  return GRX_OK;
}

GRX_Result grx_parse_pattern(const char * pattern, size_t length,
    GRX_Syntax syntax, uint32_t options, const GRX_Limits * limits,
    const GRX_Allocator * allocator, GRX_Error * out_error,
    GRX_Pattern ** out_pattern) {
  if (!out_pattern || (!pattern && length) || !limits || !allocator) {
    return GRX_ERR_INVALID;
  }
  *out_pattern = NULL;

  if ((unsigned)syntax >= (unsigned)GRX_SYNTAX_COUNT) {
    return GRX_ERR_INVALID;
  }
  if (limits->max_pattern_length && length > limits->max_pattern_length) {
    return grx_error_set(out_error, GRX_ERR_LIMIT,
        GRX_DIAG_LIMIT_PATTERN_LENGTH, limits->max_pattern_length, 0);
  }

  const GRX_Frontend * frontend = grx_frontend_for(syntax);
  if (!frontend) {
    return grx_error_set(out_error, GRX_ERR_UNSUPPORTED,
        GRX_DIAG_DIALECT_NOT_IMPLEMENTED, GRX_NPOS, 0);
  }

  GRX_Parser parser = {
    .text = pattern,
    .length = length,
    .position = 0,
    .syntax = syntax,
    .frontend = frontend,
    .options = options,
    .limits = limits,
    .pattern = NULL,
    .error = out_error,
    .depth = 0,
    .group_count = 0,
    .group_depth = 0,
    .groups_opened = 0,
    .named_groups = 0,
    .in_lookbehind = 0,
    .in_lookaround = 0,
    .quote_end = GRX_NPOS,
  };
  GRX_Result result = grx_syntax_spec(syntax, &parser.spec);
  if (result != GRX_OK) {
    return result;
  }
  result = grx_syntax_profile(syntax, &parser.profile);
  if (result != GRX_OK) {
    return result;
  }
  // The dialect's own default options are the floor; the caller adds to them.
  parser.options |= parser.spec.default_options;

  result = grx_pattern_create(
      allocator, syntax, parser.options, limits, &parser.pattern);
  if (result != GRX_OK) {
    return grx_error_set(out_error, result, GRX_DIAG_OUT_OF_MEMORY, GRX_NPOS, 0);
  }

  prescan(&parser);
  if (limits->max_captures && parser.group_count > limits->max_captures) {
    grx_pattern_free(parser.pattern);
    return grx_error_set(out_error, GRX_ERR_LIMIT, GRX_DIAG_LIMIT_CAPTURES,
        limits->max_captures, 0);
  }

  uint32_t root = GRX_INDEX_NONE;
  if (parser.options & GRX_OPT_LITERAL) {
    result = parse_as_literal(&parser);
  }
  else {
    result = grx_parse_alternation(&parser, &root);
    if (result == GRX_OK && !grx_parse_at_end(&parser)) {
      // The only way out of the grammar with input left is an unbalanced
      // `)`: every other character is an atom.
      result = grx_parse_fail(&parser, GRX_DIAG_UNMATCHED_CLOSE_PAREN,
          parser.position, 1);
    }
    if (result == GRX_OK) {
      parser.pattern->root = root;
      if (frontend->validate) {
        result = frontend->validate(&parser);
      }
    }
  }

  if (result != GRX_OK) {
    grx_pattern_free(parser.pattern);
    return result;
  }

  parser.pattern->capture_count = parser.groups_opened;
  *out_pattern = parser.pattern;
  return GRX_OK;
}
