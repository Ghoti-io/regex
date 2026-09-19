/**
 * @file
 *
 * The JSON Schema subset check.
 *
 * A pattern this reports is valid and runs. What it is not is *portable*:
 * JSON Schema core section 6.4 names seven kinds of token and says a schema
 * author should stay inside them, because validators disagree about the
 * rest. A validator that wants to warn its users needs to know which
 * construct took a pattern outside that list and where it was written, and
 * neither of those is something GRX_Facts can say - `is_regular` answers a
 * different and weaker question, and answers it with one bit.
 *
 * The walk is over the AST rather than the IR, deliberately. By the time a
 * pattern has been lowered, `\d` and `[0-9]` are the same class and `(?:a)`
 * and `a` are the same node - which is exactly the information this needs.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/pattern.h>
#include <stddef.h>

#include "parse_internal.h"

const char * grx_lint_string(GRX_Lint finding) {
  static const char * const names[GRX_LINT_COUNT] = {
    "inside the subset",
    "built without the u or v flag",
    "`.`",
    "a shorthand class",
    "a property escape",
    "an anchor other than ^ and $",
    "a group that is not plain grouping",
    "a backreference",
    "a lookaround",
    "a character class beyond characters and ranges",
    "a construct outside the subset",
  };
  return (unsigned)finding < GRX_LINT_COUNT ? names[finding] : "unknown";
}

/** The report, if this is the leftmost finding so far. */
static void record(GRX_LintReport * report, GRX_Lint finding, size_t offset,
    size_t length) {
  if (report->finding != GRX_LINT_NONE && report->offset <= offset) {
    return;
  }
  report->finding = finding;
  report->offset = offset;
  report->length = length;
}

/** Whether a class holds nothing but characters and ranges. */
static GRX_Lint class_finding(
    const GRX_Pattern * pattern, const GRX_Node * node, size_t * out_offset,
    size_t * out_length) {
  for (uint32_t i = 0; i < node->b; i++) {
    const GRX_ClassItem * item = GRX_ARENA_AT(
        const GRX_ClassItem, &pattern->class_items, node->a + i);
    if (!item) {
      continue;
    }
    switch (item->kind) {
      case GRX_CLASS_ITEM_SINGLE:
      case GRX_CLASS_ITEM_RANGE:
        continue;
      case GRX_CLASS_ITEM_SHORTHAND:
        *out_offset = item->offset;
        *out_length = item->length;
        return GRX_LINT_SHORTHAND;
      case GRX_CLASS_ITEM_PROPERTY:
      case GRX_CLASS_ITEM_STRING_PROPERTY:
        *out_offset = item->offset;
        *out_length = item->length;
        return GRX_LINT_PROPERTY;
      default:
        *out_offset = item->offset;
        *out_length = item->length;
        return GRX_LINT_CLASS;
    }
  }
  return GRX_LINT_NONE;
}

/** Walk one node and its children. */
static void walk(const GRX_Pattern * pattern, uint32_t index,
    GRX_LintReport * report) {
  const GRX_Node * node = grx_pattern_node(pattern, index);
  if (!node) {
    return;
  }

  switch (node->kind) {
    case GRX_NODE_EMPTY:
    case GRX_NODE_LITERAL:
    case GRX_NODE_CONCAT:
    case GRX_NODE_ALTERNATE:
    case GRX_NODE_REPEAT:
      // In the list: characters, alternation, and every quantifier - greedy
      // and lazy alike, which section 6.4 names explicitly.
      break;

    case GRX_NODE_ANY:
      record(report, GRX_LINT_DOT, node->offset, node->length);
      break;

    case GRX_NODE_CLASS: {
      // A class written as `\d` is a class node too, and is told apart from
      // `[0-9]` by spanning no brackets.
      size_t offset = node->offset;
      size_t length = node->length;
      GRX_Lint finding = class_finding(pattern, node, &offset, &length);
      if (finding != GRX_LINT_NONE) {
        record(report, finding, offset, length);
      }
      break;
    }

    case GRX_NODE_GROUP:
      // `(a)` is in the list; `(?:a)`, `(?<n>a)` and `(?>a)` are not. The
      // header says why the first of those is reported rather than waved
      // through.
      if (!(node->flags & GRX_NODE_CAPTURING)
          || (node->flags & (GRX_NODE_NAMED | GRX_NODE_ATOMIC))) {
        record(report, GRX_LINT_GROUP, node->offset, node->length);
      }
      break;

    case GRX_NODE_ANCHOR:
      if (node->a != (uint32_t)GRX_ANCHOR_CARET
          && node->a != (uint32_t)GRX_ANCHOR_DOLLAR) {
        record(report, GRX_LINT_ANCHOR, node->offset, node->length);
      }
      break;

    case GRX_NODE_BACKREF:
      record(report, GRX_LINT_BACKREFERENCE, node->offset, node->length);
      break;

    case GRX_NODE_LOOKAROUND:
      record(report, GRX_LINT_LOOKAROUND, node->offset, node->length);
      break;

    case GRX_NODE_CLASS_OP:
    case GRX_NODE_STRING_SET:
      record(report, GRX_LINT_CLASS, node->offset, node->length);
      break;

    default:
      record(report, GRX_LINT_CONSTRUCT, node->offset, node->length);
      break;
  }

  // Children are walked whatever this node was, because the leftmost
  // finding wins and a lookaround's body may begin before the next sibling.
  for (uint32_t child = node->first_child; child != GRX_INDEX_NONE;) {
    const GRX_Node * node_child = grx_pattern_node(pattern, child);
    if (!node_child) {
      break;
    }
    walk(pattern, child, report);
    child = node_child->next_sibling;
  }
}

GRX_Result grx_pattern_lint(
    const GRX_Pattern * pattern, GRX_LintReport * out_report) {
  if (!pattern || !out_report) {
    return GRX_ERR_INVALID;
  }

  *out_report = (GRX_LintReport) {GRX_LINT_NONE, GRX_NPOS, 0};

  if (pattern->root != GRX_INDEX_NONE) {
    walk(pattern, pattern->root, out_report);
  }

  // The flags are checked last and win only when nothing in the text did,
  // because a caller fixing one construct at a time wants the construct
  // first - and because the flag has no offset to underline.
  if (out_report->finding == GRX_LINT_NONE
      && !(pattern->options & (GRX_OPT_UTF | GRX_OPT_UNICODE_SETS))) {
    *out_report = (GRX_LintReport) {GRX_LINT_FLAGS, GRX_NPOS, 0};
  }

  return GRX_OK;
}
