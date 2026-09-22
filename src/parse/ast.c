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
 * The parsed pattern: building it, reading it, dumping it, freeing it.
 *
 * Everything here works on whatever grx_parse_pattern() produced, so it is
 * written once and does not change as the parser grows. The builders are the
 * only way a node reaches the tree, which is where the max_nodes cap and the
 * child-index bounds checks live.
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/pattern.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#include "../core/core_internal.h"
#include "parse_internal.h"

/**
 * How deep grx_pattern_dump() will walk before it stops.
 *
 * The dump recurses on the C stack, and a pattern reaches it only through a
 * parser that enforced max_nesting_depth - but a test may build a tree by
 * hand, and a debugging aid that can crash the program it is debugging is
 * worse than one that truncates.
 */
#define GRX_DUMP_MAX_DEPTH 256

const char * grx_node_kind_name(GRX_NodeKind kind) {
  switch (kind) {
    case GRX_NODE_EMPTY:
      return "empty";
    case GRX_NODE_LITERAL:
      return "literal";
    case GRX_NODE_CLASS:
      return "class";
    case GRX_NODE_ANY:
      return "any";
    case GRX_NODE_CONCAT:
      return "concat";
    case GRX_NODE_ALTERNATE:
      return "alternate";
    case GRX_NODE_REPEAT:
      return "repeat";
    case GRX_NODE_GROUP:
      return "group";
    case GRX_NODE_BACKREF:
      return "backref";
    case GRX_NODE_ANCHOR:
      return "anchor";
    case GRX_NODE_LOOKAROUND:
      return "lookaround";
    case GRX_NODE_CONDITIONAL:
      return "conditional";
    case GRX_NODE_RECURSE:
      return "recurse";
    case GRX_NODE_CONTROL:
      return "control";
    case GRX_NODE_OPTIONS:
      return "options";
    case GRX_NODE_CLASS_OP:
      return "class-op";
    case GRX_NODE_STRING_SET:
      return "string-set";
    case GRX_NODE_SCAN:
      return "scan";
    case GRX_NODE_KEEP:
      return "keep";
    case GRX_NODE_BRANCH_RESET:
      return "branch-reset";
    case GRX_NODE_GRAPHEME:
      return "grapheme";
    case GRX_NODE_COUNT:
    default:
      return "?";
  }
}

/** The name of an anchor, for the dump. */
static const char * anchor_name(uint32_t kind) {
  switch ((GRX_AnchorKind)kind) {
    case GRX_ANCHOR_CARET:
      return "caret";
    case GRX_ANCHOR_DOLLAR:
      return "dollar";
    case GRX_ANCHOR_START_SUBJECT:
      return "start-subject";
    case GRX_ANCHOR_END_SUBJECT:
      return "end-subject";
    case GRX_ANCHOR_END_BEFORE_NEWLINE:
      return "end-before-newline";
    case GRX_ANCHOR_WORD_BOUNDARY:
      return "word-boundary";
    case GRX_ANCHOR_NOT_WORD_BOUNDARY:
      return "not-word-boundary";
    case GRX_ANCHOR_SEARCH_START:
      return "search-start";
    case GRX_ANCHOR_START_BUFFER:
      return "start-buffer";
    case GRX_ANCHOR_END_BUFFER:
      return "end-buffer";
    case GRX_ANCHOR_WORD_START:
      return "word-start";
    case GRX_ANCHOR_WORD_END:
      return "word-end";
    case GRX_ANCHOR_GRAPHEME_BOUNDARY:
      return "grapheme-boundary";
    case GRX_ANCHOR_NOT_GRAPHEME_BOUNDARY:
      return "not-grapheme-boundary";
    case GRX_ANCHOR_WORD_SEG_BOUNDARY:
      return "word-seg-boundary";
    case GRX_ANCHOR_NOT_WORD_SEG_BOUNDARY:
      return "not-word-seg-boundary";
    case GRX_ANCHOR_SENTENCE_BOUNDARY:
      return "sentence-boundary";
    case GRX_ANCHOR_NOT_SENTENCE_BOUNDARY:
      return "not-sentence-boundary";
    case GRX_ANCHOR_LINE_BOUNDARY:
      return "line-boundary";
    case GRX_ANCHOR_NOT_LINE_BOUNDARY:
      return "not-line-boundary";
    case GRX_ANCHOR_COUNT:
    default:
      return "?";
  }
}

/** The name of a shorthand class escape, for the dump. */
static const char * shorthand_name(uint32_t kind) {
  static const char * const names[GRX_SHORTHAND_COUNT] = {
    "d", "D", "w", "W", "s", "S", "h", "H", "v", "V",
  };
  return kind < GRX_SHORTHAND_COUNT ? names[kind] : "?";
}

/** The name of a lookaround, for the dump. */
static const char * look_name(uint32_t kind) {
  static const char * const names[GRX_LOOK_COUNT] = {
    "ahead", "ahead-negative", "behind", "behind-negative",
    "ahead-non-atomic", "behind-non-atomic",
  };
  return kind < GRX_LOOK_COUNT ? names[kind] : "?";
}

/** The name of a repeat mode, for the dump. */
static const char * repeat_mode_name(uint32_t mode) {
  static const char * const names[GRX_REPEAT_MODE_COUNT] = {
    "greedy", "lazy", "possessive",
  };
  return mode < GRX_REPEAT_MODE_COUNT ? names[mode] : "?";
}

/** The name of a verb, for the dump. */
static const char * verb_name(uint32_t kind) {
  static const char * const names[GRX_VERB_COUNT] = {
    "ACCEPT", "FAIL", "COMMIT", "PRUNE", "SKIP", "THEN", "MARK",
  };
  return kind < GRX_VERB_COUNT ? names[kind] : "?";
}

/** The name of a conditional's condition, for the dump. */
static const char * cond_name(uint32_t kind) {
  static const char * const names[GRX_COND_COUNT] = {
    "group-set", "recursion-any", "recursion-group", "assertion", "define",
  };
  return kind < GRX_COND_COUNT ? names[kind] : "?";
}

/** The name of a class set operation, for the dump. */
static const char * class_op_name(uint32_t kind) {
  static const char * const names[GRX_CLASS_OP_COUNT] = {
    "union", "intersect", "subtract", "symdiff", "complement",
  };
  return kind < GRX_CLASS_OP_COUNT ? names[kind] : "?";
}

/**
 * Write one code point in a form that survives a terminal and a diff.
 *
 * Printable ASCII goes through as itself; everything else becomes \xHH or
 * \u{...}, so that a dump of a pattern containing a newline is still one
 * line per node.
 */
static void write_codepoint(FILE * out, uint32_t codepoint) {
  if (codepoint == '\\' || codepoint == '"') {
    fprintf(out, "\\%c", (char)codepoint);
  }
  else if (codepoint >= 0x20 && codepoint < 0x7F) {
    fputc((int)codepoint, out);
  }
  else if (codepoint < 0x100) {
    fprintf(out, "\\x%02X", codepoint);
  }
  else {
    fprintf(out, "\\u{%X}", codepoint);
  }
}

GRX_Result grx_pattern_create(const GRX_Allocator * allocator,
    GRX_Syntax syntax, uint32_t options, const GRX_Limits * limits,
    GRX_Pattern ** out_pattern) {
  if (!out_pattern || !limits) {
    return GRX_ERR_INVALID;
  }
  *out_pattern = NULL;

  if (!allocator) {
    allocator = grx_allocator_default();
  }

  GRX_Pattern * pattern
      = gcu_allocator_calloc(allocator, 1, sizeof(GRX_Pattern));
  if (!pattern) {
    return GRX_ERR_OOM;
  }

  pattern->allocator = allocator;
  pattern->syntax = syntax;
  pattern->options = options;
  pattern->root = GRX_INDEX_NONE;
  pattern->capture_count = 0;
  grx_pattern_limits_init(&pattern->limits);

  // Each arena carries the limit it is subject to and the diagnostic that
  // limit reports, so that every append is checked without the parser
  // remembering to check it. The name and string tables are bounded by the
  // pattern length, which was already checked at entry, so they have no cap
  // of their own.
  grx_arena_init(&pattern->nodes, allocator, sizeof(GRX_Node),
      limits->max_nodes, GRX_DIAG_LIMIT_NODES);
  grx_arena_init(&pattern->literals, allocator, sizeof(uint32_t), 0,
      GRX_DIAG_NONE);
  grx_arena_init(&pattern->class_items, allocator, sizeof(GRX_ClassItem),
      limits->max_class_ranges, GRX_DIAG_LIMIT_CLASS_RANGES);
  grx_arena_init(&pattern->names, allocator, sizeof(char), 0, GRX_DIAG_NONE);
  grx_arena_init(&pattern->strings, allocator, sizeof(uint32_t), 0,
      GRX_DIAG_NONE);

  *out_pattern = pattern;
  return GRX_OK;
}

GRX_Result grx_pattern_add_node(GRX_Pattern * pattern, GRX_NodeKind kind,
    size_t offset, size_t length, uint32_t * out_index) {
  if (!pattern) {
    return GRX_ERR_INVALID;
  }

  GRX_Node node = {
    .kind = kind,
    .flags = 0,
    .first_child = GRX_INDEX_NONE,
    .last_child = GRX_INDEX_NONE,
    .next_sibling = GRX_INDEX_NONE,
    .a = 0,
    .b = 0,
    .min = 0,
    .max = 0,
    .offset = offset,
    .length = length,
  };

  return grx_arena_append(&pattern->nodes, &node, out_index);
}

GRX_Result grx_pattern_add_child(
    GRX_Pattern * pattern, uint32_t parent, uint32_t child) {
  GRX_Node * parent_node = grx_pattern_node(pattern, parent);
  GRX_Node * child_node = grx_pattern_node(pattern, child);
  if (!parent_node || !child_node || parent == child) {
    return GRX_ERR_INVALID;
  }
  // A node belongs to one parent. Appending one that already has a sibling
  // would splice a second list into the first and produce a shape no walker
  // could free or dump.
  if (child_node->next_sibling != GRX_INDEX_NONE) {
    return GRX_ERR_INVALID;
  }

  if (parent_node->last_child == GRX_INDEX_NONE) {
    parent_node->first_child = child;
  }
  else {
    GRX_Node * last = grx_pattern_node(pattern, parent_node->last_child);
    if (!last) {
      return GRX_ERR_INTERNAL;
    }
    last->next_sibling = child;
  }
  parent_node->last_child = child;

  return GRX_OK;
}

GRX_Node * grx_pattern_node(const GRX_Pattern * pattern, uint32_t index) {
  if (!pattern || index == GRX_INDEX_NONE) {
    return NULL;
  }

  return GRX_ARENA_AT(GRX_Node, &pattern->nodes, index);
}

GRX_Result grx_pattern_add_name(GRX_Pattern * pattern, const char * name,
    size_t length, uint32_t * out_offset) {
  if (!pattern || (!name && length)) {
    return GRX_ERR_INVALID;
  }

  uint32_t start = (uint32_t)pattern->names.count;
  for (size_t i = 0; i < length; i++) {
    GRX_Result result = grx_arena_append(&pattern->names, &name[i], NULL);
    if (result != GRX_OK) {
      return result;
    }
  }

  char terminator = '\0';
  GRX_Result result = grx_arena_append(&pattern->names, &terminator, NULL);
  if (result != GRX_OK) {
    return result;
  }

  if (out_offset) {
    *out_offset = start;
  }
  return GRX_OK;
}

GRX_Result grx_pattern_add_string(GRX_Pattern * pattern,
    const uint32_t * points, size_t count, uint32_t * out_index) {
  if (!pattern || !out_index || (!points && count)) {
    return GRX_ERR_INVALID;
  }

  uint32_t index = (uint32_t)pattern->strings.count;
  uint32_t length = (uint32_t)count;
  GRX_Result result = grx_arena_append(&pattern->strings, &length, NULL);
  for (size_t i = 0; result == GRX_OK && i < count; i++) {
    result = grx_arena_append(&pattern->strings, &points[i], NULL);
  }
  if (result != GRX_OK) {
    return result;
  }

  *out_index = index;
  return GRX_OK;
}

GRX_Result grx_pattern_string_begin(
    GRX_Pattern * pattern, uint32_t * out_index) {
  if (!pattern || !out_index) {
    return GRX_ERR_INVALID;
  }
  uint32_t index = (uint32_t)pattern->strings.count;
  uint32_t zero = 0;
  GRX_Result result = grx_arena_append(&pattern->strings, &zero, NULL);
  if (result != GRX_OK) {
    return result;
  }
  *out_index = index;
  return GRX_OK;
}

GRX_Result grx_pattern_string_push(
    GRX_Pattern * pattern, uint32_t index, uint32_t codepoint) {
  if (!pattern) {
    return GRX_ERR_INVALID;
  }
  GRX_Result result = grx_arena_append(&pattern->strings, &codepoint, NULL);
  if (result != GRX_OK) {
    return result;
  }
  uint32_t * length = GRX_ARENA_AT(uint32_t, &pattern->strings, index);
  if (!length) {
    return GRX_ERR_INVALID;
  }
  (*length)++;
  return GRX_OK;
}

const uint32_t * grx_pattern_string(const GRX_Pattern * pattern,
    uint32_t index, size_t * out_count, uint32_t * out_next) {
  if (!pattern || !out_count) {
    return NULL;
  }
  const uint32_t * length
      = GRX_ARENA_AT(const uint32_t, &pattern->strings, index);
  if (!length) {
    return NULL;
  }
  if ((size_t)index + 1 + *length > pattern->strings.count) {
    return NULL;
  }

  *out_count = *length;
  if (out_next) {
    *out_next = index + 1 + *length;
  }
  return length + 1;
}

const char * grx_pattern_name(const GRX_Pattern * pattern, uint32_t offset) {
  if (!pattern || offset == GRX_INDEX_NONE) {
    return NULL;
  }

  return GRX_ARENA_AT(const char, &pattern->names, offset);
}

GRX_Result grx_pattern_parse(const char * pattern, GRX_Syntax syntax,
    uint32_t options, GRX_Pattern ** out_pattern) {
  if (!pattern) {
    return GRX_ERR_INVALID;
  }

  return grx_pattern_parse_with_allocator(pattern, strlen(pattern), syntax,
      options, NULL, NULL, NULL, out_pattern);
}

GRX_Result grx_pattern_parse_with_allocator(const char * pattern,
    size_t length, GRX_Syntax syntax, uint32_t options,
    const GRX_Limits * limits, const GRX_Allocator * allocator,
    GRX_Error * out_error, GRX_Pattern ** out_pattern) {
  if (!out_pattern) {
    return GRX_ERR_INVALID;
  }
  *out_pattern = NULL;
  grx_error_clear(out_error);

  GRX_Limits defaults;
  if (!limits) {
    grx_limits_default(&defaults);
    limits = &defaults;
  }
  if (!allocator) {
    allocator = grx_allocator_default();
  }

  return grx_parse_pattern(pattern, length, syntax, options, limits,
      allocator, out_error, out_pattern);
}

GRX_Syntax grx_pattern_syntax(const GRX_Pattern * pattern) {
  return pattern ? pattern->syntax : GRX_SYNTAX_COUNT;
}

size_t grx_pattern_capture_count(const GRX_Pattern * pattern) {
  return pattern ? pattern->capture_count : 0;
}

size_t grx_pattern_node_count(const GRX_Pattern * pattern) {
  return pattern ? pattern->nodes.count : 0;
}

/** Write the items of a class node, in the form the dump documents. */
static void dump_class_items(
    FILE * out, const GRX_Pattern * pattern, const GRX_Node * node) {
  fputc('[', out);
  if (node->flags & GRX_NODE_NEGATED) {
    fputc('^', out);
  }

  for (uint32_t i = 0; i < node->b; i++) {
    const GRX_ClassItem * item
        = GRX_ARENA_AT(const GRX_ClassItem, &pattern->class_items, node->a + i);
    if (!item) {
      fputs("<bad>", out);
      break;
    }
    if (i) {
      fputc(' ', out);
    }

    int negated = (item->flags & GRX_CLASS_ITEM_NEGATED) != 0;
    switch (item->kind) {
      case GRX_CLASS_ITEM_SINGLE:
        write_codepoint(out, item->lo);
        break;
      case GRX_CLASS_ITEM_RANGE:
        write_codepoint(out, item->lo);
        fputc('-', out);
        write_codepoint(out, item->hi);
        break;
      case GRX_CLASS_ITEM_SHORTHAND:
        fprintf(out, "\\%s", shorthand_name(item->a));
        break;
      case GRX_CLASS_ITEM_POSIX:
        fprintf(out, "[:%s%s:]", negated ? "^" : "",
            grx_pattern_name(pattern, item->a));
        break;
      case GRX_CLASS_ITEM_PROPERTY:
        fprintf(out, "\\%c{%s}", negated ? 'P' : 'p',
            grx_pattern_name(pattern, item->a));
        break;
      case GRX_CLASS_ITEM_NESTED:
        fprintf(out, "nested=%u", item->a);
        break;
      case GRX_CLASS_ITEM_STRING:
        fprintf(out, "string=%u", item->a);
        break;
      case GRX_CLASS_ITEM_STRING_PROPERTY:
        fprintf(out, "\\p{%s}", grx_pattern_name(pattern, item->a));
        break;
      case GRX_CLASS_ITEM_COUNT:
      default:
        fputc('?', out);
        break;
    }
  }

  fputc(']', out);
}

/** Write the payload that belongs to this node's kind. */
static void dump_payload(
    FILE * out, const GRX_Pattern * pattern, const GRX_Node * node) {
  switch (node->kind) {
    case GRX_NODE_LITERAL: {
      fputs(" \"", out);
      for (uint32_t i = 0; i < node->b; i++) {
        const uint32_t * codepoint
            = GRX_ARENA_AT(const uint32_t, &pattern->literals, node->a + i);
        if (!codepoint) {
          fputs("<bad>", out);
          break;
        }
        write_codepoint(out, *codepoint);
      }
      fputc('"', out);
      break;
    }
    case GRX_NODE_CLASS:
      fputc(' ', out);
      dump_class_items(out, pattern, node);
      break;
    case GRX_NODE_REPEAT:
      if (node->max == GRX_REPEAT_INF) {
        fprintf(out, " {%u,} %s", node->min, repeat_mode_name(node->a));
      }
      else {
        fprintf(out, " {%u,%u} %s", node->min, node->max,
            repeat_mode_name(node->a));
      }
      break;
    case GRX_NODE_GROUP:
      if (node->flags & GRX_NODE_CAPTURING) {
        fprintf(out, " #%u", node->a);
      }
      else {
        fputs(" non-capturing", out);
      }
      if (node->flags & GRX_NODE_NAMED) {
        fprintf(out, " name=%s", grx_pattern_name(pattern, node->b));
      }
      if (node->flags & GRX_NODE_ATOMIC) {
        fputs(" atomic", out);
      }
      break;
    case GRX_NODE_BACKREF:
      if (node->flags & GRX_NODE_NAMED) {
        fprintf(out, " name=%s", grx_pattern_name(pattern, node->b));
      }
      else {
        fprintf(out, " #%u", node->a);
      }
      if (node->flags & GRX_NODE_RELATIVE) {
        fputs(" relative", out);
      }
      break;
    case GRX_NODE_ANCHOR:
      fprintf(out, " %s", anchor_name(node->a));
      break;
    case GRX_NODE_LOOKAROUND:
      fprintf(out, " %s", look_name(node->a));
      break;
    case GRX_NODE_CONDITIONAL:
      fprintf(out, " %s", cond_name(node->a));
      if (node->a == GRX_COND_GROUP_SET
          || node->a == GRX_COND_RECURSION_GROUP) {
        fprintf(out, " #%u", node->b);
      }
      break;
    case GRX_NODE_RECURSE:
      if (node->flags & GRX_NODE_NAMED) {
        fprintf(out, " name=%s", grx_pattern_name(pattern, node->b));
      }
      else {
        fprintf(out, " #%u", node->a);
        if (node->b != GRX_INDEX_NONE) {
          // Which definition of that number, by where it was written. Only a
          // `(?|...)` gives a number more than one, so this is absent from
          // most dumps and is the whole story where it is not.
          fprintf(out, " at=%u", node->b);
        }
      }
      break;
    case GRX_NODE_CONTROL:
      fprintf(out, " %s", verb_name(node->a));
      break;
    case GRX_NODE_OPTIONS:
      fprintf(out, " +0x%08x -0x%08x%s", node->a, node->b,
          (node->flags & GRX_NODE_SCOPED) ? " scoped" : "");
      break;
    case GRX_NODE_CLASS_OP:
      fprintf(out, " %s", class_op_name(node->a));
      break;
    case GRX_NODE_STRING_SET:
      fprintf(out, " strings=%u", node->b);
      break;
    default:
      break;
  }
}

/** Write one node and, indented beneath it, its children. */
static void dump_node(FILE * out, const GRX_Pattern * pattern, uint32_t index,
    unsigned depth) {
  const GRX_Node * node = grx_pattern_node(pattern, index);
  if (!node) {
    return;
  }

  for (unsigned i = 0; i < depth; i++) {
    fputs("  ", out);
  }

  fprintf(out, "%s @%zu+%zu", grx_node_kind_name(node->kind), node->offset,
      node->length);
  dump_payload(out, pattern, node);
  fputc('\n', out);

  if (depth + 1 >= GRX_DUMP_MAX_DEPTH) {
    if (node->first_child != GRX_INDEX_NONE) {
      for (unsigned i = 0; i <= depth; i++) {
        fputs("  ", out);
      }
      fputs("... (depth limit)\n", out);
    }
    return;
  }

  for (uint32_t child = node->first_child; child != GRX_INDEX_NONE;) {
    const GRX_Node * child_node = grx_pattern_node(pattern, child);
    if (!child_node) {
      break;
    }
    dump_node(out, pattern, child, depth + 1);
    child = child_node->next_sibling;
  }
}

GRX_Result grx_pattern_dump(const GRX_Pattern * pattern, FILE * out) {
  if (!pattern || !out) {
    return GRX_ERR_INVALID;
  }

  fprintf(out, "pattern: syntax=%s options=0x%08x nodes=%zu captures=%zu\n",
      grx_syntax_name(pattern->syntax), pattern->options, pattern->nodes.count,
      pattern->capture_count);

  if (pattern->root != GRX_INDEX_NONE) {
    dump_node(out, pattern, pattern->root, 1);
  }

  return GRX_OK;
}

void grx_pattern_free(GRX_Pattern * pattern) {
  if (!pattern) {
    return;
  }

  const GRX_Allocator * allocator = pattern->allocator;
  if (!allocator) {
    allocator = grx_allocator_default();
  }

  grx_arena_clear(&pattern->nodes);
  grx_arena_clear(&pattern->literals);
  grx_arena_clear(&pattern->class_items);
  grx_arena_clear(&pattern->names);
  grx_arena_clear(&pattern->strings);
  gcu_allocator_free(allocator, pattern);
}
