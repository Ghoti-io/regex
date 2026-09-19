/**
 * @file
 *
 * Lowering: the syntax tree to the intermediate representation.
 *
 * Every dialect decision is made here and then discarded. What the IR carries
 * forward is a vocabulary of explicit modes and canonical classes, and an
 * engine reading it cannot tell which dialect produced it - which is the
 * invariant the whole design rests on (documentation/design.md section 3).
 *
 * The one rule in this file that is easy to get backwards, and that decides
 * whether `[^a]` under `/i` matches "A": **fold before you negate.** A
 * caseless class is the closure of its positive content under the dialect's
 * folding, and *then* complemented. Doing it the other way round makes
 * `/[^a]/i` match "A", which is wrong in every dialect that has both
 * features. The same rule applies one level down, to a negated shorthand:
 * `\W` is the complement of the *folded* word set, which is exactly why
 * ECMA-262's `\w` gains U+017F and U+212A under `iu` while `\W` does not
 * lose them twice.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <string.h>

#include "../core/core_internal.h"
#include "../unicode/unicode_internal.h"
#include "lower_internal.h"

/** Everything one lowering run needs to carry down the tree. */
typedef struct {
  const GRX_Pattern * pattern; ///< What is being lowered.
  GRX_IR * ir;                 ///< What is being built.
  GRX_Profile profile;         ///< The dialect's semantics.
  GRX_SyntaxSpec spec;         ///< Its features.
  const GRX_Limits * limits;   ///< Caps to apply.
  GRX_Error * error;           ///< Where a failure is reported.
  uint32_t options;            ///< The options the pattern was parsed with.
  GRX_FoldKind fold;           ///< The folding a caseless match uses, or NONE.
  GRX_ShorthandSet shorthands; ///< Which sets `\d`, `\w`, `\s` name.
  int reverse;                 ///< Non-zero inside a lookbehind body.
  uint32_t newline_class;      ///< The line-terminator set, or GRX_INDEX_NONE.
  uint32_t word_class;         ///< The word set for `\b`, or GRX_INDEX_NONE.
} Lowering;

static GRX_Result lower_node(
    Lowering * low, uint32_t node_index, uint32_t * out_node);

/** Report a failure at a node's span. */
static GRX_Result fail(
    Lowering * low, GRX_Diag diag, const GRX_Node * node) {
  return grx_error_set(low->error, grx_diag_result(diag), diag,
      node ? node->offset : GRX_NPOS, node ? node->length : 0);
}

/** Turn a storage failure into the diagnostic that names the cap it hit. */
static GRX_Result storage_failed(
    Lowering * low, GRX_Result result, const GRX_Node * node) {
  if (result == GRX_OK) {
    return GRX_OK;
  }

  GRX_Diag diag = GRX_DIAG_OUT_OF_MEMORY;
  if (result == GRX_ERR_LIMIT) {
    diag = GRX_DIAG_LIMIT_NODES;
  }
  return fail(low, diag, node);
}

/** Append an IR node of a kind, with the original text's span. */
static GRX_Result add(Lowering * low, GRX_IRKind kind, const GRX_Node * node,
    uint32_t * out_node) {
  GRX_Result result = grx_ir_add_node(low->ir, kind,
      node ? node->offset : 0, node ? node->length : 0, out_node);
  if (result != GRX_OK) {
    return storage_failed(low, result, node);
  }

  if (low->reverse) {
    grx_ir_node(low->ir, *out_node)->flags |= GRX_IR_REVERSE;
  }
  return GRX_OK;
}

/** Attach a child, turning an index failure into an internal diagnostic. */
static GRX_Result attach(Lowering * low, uint32_t parent, uint32_t child) {
  if (grx_ir_add_child(low->ir, parent, child) != GRX_OK) {
    return fail(low, GRX_DIAG_INTERNAL, NULL);
  }

  return GRX_OK;
}

/**
 * Install a class in the IR's table and return its index.
 *
 * The class is canonicalised first - its negation applied, so that nothing
 * below this point consults a flag - and deduplicated, so a pattern naming
 * the same set six times costs one entry.
 */
static GRX_Result intern(Lowering * low, GRX_CharClass * cls,
    const GRX_Node * node, uint32_t * out_index) {
  GRX_Result result = grx_charclass_canonicalize(cls, low->limits);
  if (result == GRX_OK) {
    result = grx_class_table_add_class(&low->ir->classes, cls, out_index);
  }
  if (result != GRX_OK) {
    return storage_failed(low,
        result == GRX_ERR_LIMIT ? GRX_ERR_LIMIT : result, node);
  }

  return GRX_OK;
}

// --------------------------------------------------------------------------
// Classes
// --------------------------------------------------------------------------

/** Fill `out` with the set one class item denotes, negation not yet applied. */
static GRX_Result item_base_set(Lowering * low, const GRX_ClassItem * item,
    const GRX_Node * node, GRX_CharClass * out) {
  switch (item->kind) {
    case GRX_CLASS_ITEM_SINGLE:
      return grx_charclass_add_range(out, item->lo, item->lo, low->limits);

    case GRX_CLASS_ITEM_RANGE:
      return grx_charclass_add_range(out, item->lo, item->hi, low->limits);

    case GRX_CLASS_ITEM_SHORTHAND: {
      GRX_Result result = grx_shorthand_set(out, low->shorthands,
          (GRX_ShorthandKind)item->a, low->limits);
      if (result == GRX_ERR_UNSUPPORTED) {
        return fail(low, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, node);
      }
      return result;
    }

    case GRX_CLASS_ITEM_PROPERTY: {
      const char * name = grx_pattern_name(low->pattern, item->a);
      if (!name) {
        return fail(low, GRX_DIAG_INTERNAL, node);
      }
      const char * equals = strchr(name, '=');
      uint32_t property = 0;
      GRX_Result result = equals
          ? grx_unicode_property_lookup(name, (size_t)(equals - name),
                equals + 1, strlen(equals + 1), low->profile.property_match,
                &property)
          : grx_unicode_property_lookup(name, strlen(name), NULL, 0,
                low->profile.property_match, &property);
      if (result != GRX_OK) {
        // The parser resolved this once already, so reaching here means the
        // two resolvers disagree - an internal fault, not a user's error.
        return fail(low, GRX_DIAG_INTERNAL, node);
      }
      size_t count = 0;
      const GRX_CharRange * ranges
          = grx_unicode_property_ranges(property, &count);
      return grx_charclass_add_ranges(out, ranges, count, low->limits);
    }

    case GRX_CLASS_ITEM_POSIX:
    case GRX_CLASS_ITEM_NESTED:
    case GRX_CLASS_ITEM_STRING:
    case GRX_CLASS_ITEM_COUNT:
    default:
      return fail(low, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, node);
  }
}

/**
 * Evaluate one class node into a set of code points.
 *
 * The order is the whole of the rule: each item's base set, then that item's
 * own negation applied *after* folding it, then the union of the items, then
 * the folding of the union, then the class's own negation. Every one of those
 * steps is observable - `[^a]` under `/i`, `\W` under `/iu`, `[a\W]` under
 * `/iu` each distinguish this order from a wrong one.
 */
static GRX_Result evaluate_class(
    Lowering * low, const GRX_Node * node, GRX_CharClass * out) {
  for (uint32_t i = 0; i < node->b; i++) {
    const GRX_ClassItem * item = GRX_ARENA_AT(
        const GRX_ClassItem, &low->pattern->class_items, node->a + i);
    if (!item) {
      return fail(low, GRX_DIAG_INTERNAL, node);
    }

    GRX_CharClass piece;
    grx_charclass_init(&piece, out->allocator);
    GRX_Result result = item_base_set(low, item, node, &piece);

    if (result == GRX_OK && (item->flags & GRX_CLASS_ITEM_NEGATED)) {
      // `\W` is the complement of the *folded* word set, not the fold of the
      // complement. The difference is U+017F under `/iu`: it is in `\w`
      // because folding put it there, and so must not also be in `\W`.
      result = grx_charclass_fold_closure(&piece, low->fold, low->limits);
      if (result == GRX_OK) {
        result = grx_charclass_complement(&piece, low->limits);
      }
    }
    if (result == GRX_OK) {
      result = grx_charclass_union(out, &piece, low->limits);
    }
    grx_charclass_clear(&piece);

    if (result != GRX_OK) {
      return storage_failed(low, result, node);
    }
  }

  GRX_Result result = grx_charclass_fold_closure(out, low->fold, low->limits);
  if (result != GRX_OK) {
    return storage_failed(low, result, node);
  }
  if (node->flags & GRX_NODE_NEGATED) {
    out->negated = 1;
  }

  return GRX_OK;
}

// --------------------------------------------------------------------------
// The shared classes: the newline set and the word set
// --------------------------------------------------------------------------

/**
 * The line-terminator set, interned once and reused.
 *
 * `.`, `^`, `$` and `\Z` all name it, and a pattern using all four should
 * cost one class rather than four. The lazy build is here rather than at
 * setup because a pattern that names none of them should cost none.
 */
static GRX_Result newline_class(Lowering * low, uint32_t * out_index) {
  if (low->newline_class != GRX_INDEX_NONE) {
    *out_index = low->newline_class;
    return GRX_OK;
  }

  GRX_CharClass cls;
  grx_charclass_init(&cls, low->ir->allocator);
  GRX_Result result
      = grx_newline_set(&cls, low->profile.newlines, low->limits);
  if (result == GRX_OK) {
    result = intern(low, &cls, NULL, &low->newline_class);
  }
  grx_charclass_clear(&cls);
  if (result != GRX_OK) {
    return result;
  }

  *out_index = low->newline_class;
  return GRX_OK;
}

/**
 * The word set `\b` is defined from, interned once and reused.
 *
 * Folded, because `\b` is defined in terms of `\w` and `\w` under a caseless
 * Unicode mode includes U+017F and U+212A. An engine testing a boundary then
 * does one binary search and needs to know nothing about case.
 */
static GRX_Result word_class(Lowering * low, uint32_t * out_index) {
  if (low->word_class != GRX_INDEX_NONE) {
    *out_index = low->word_class;
    return GRX_OK;
  }

  GRX_CharClass cls;
  grx_charclass_init(&cls, low->ir->allocator);
  GRX_Result result = grx_shorthand_set(
      &cls, low->shorthands, GRX_SHORTHAND_WORD, low->limits);
  if (result == GRX_OK) {
    result = grx_charclass_fold_closure(&cls, low->fold, low->limits);
  }
  if (result == GRX_OK) {
    result = intern(low, &cls, NULL, &low->word_class);
  }
  grx_charclass_clear(&cls);
  if (result != GRX_OK) {
    return result;
  }

  *out_index = low->word_class;
  return GRX_OK;
}

// --------------------------------------------------------------------------
// Node kinds
// --------------------------------------------------------------------------

/**
 * Lower one code point.
 *
 * Under a caseless match it becomes the class of everything that folds with
 * it, which is what lets every engine treat a caseless match as an ordinary
 * class match and know nothing about case.
 */
static GRX_Result lower_codepoint(Lowering * low, uint32_t codepoint,
    const GRX_Node * node, uint32_t * out_node) {
  uint32_t orbit[GRX_FOLD_ORBIT_MAX];
  size_t members = grx_unicode_orbit(low->fold, codepoint, orbit);

  if (members <= 1) {
    GRX_Result result = add(low, GRX_IR_CHAR, node, out_node);
    if (result != GRX_OK) {
      return result;
    }
    grx_ir_node(low->ir, *out_node)->a = codepoint;
    return GRX_OK;
  }

  GRX_CharClass cls;
  grx_charclass_init(&cls, low->ir->allocator);
  GRX_Result result = GRX_OK;
  for (size_t i = 0; i < members && result == GRX_OK; i++) {
    result = grx_charclass_add_range(&cls, orbit[i], orbit[i], low->limits);
  }
  uint32_t class_index = GRX_INDEX_NONE;
  if (result == GRX_OK) {
    result = intern(low, &cls, node, &class_index);
  }
  grx_charclass_clear(&cls);
  if (result != GRX_OK) {
    return storage_failed(low, result, node);
  }

  result = add(low, GRX_IR_CLASS, node, out_node);
  if (result != GRX_OK) {
    return result;
  }
  grx_ir_node(low->ir, *out_node)->a = class_index;
  return GRX_OK;
}

/** Lower a literal run: one node per code point, concatenated. */
static GRX_Result lower_literal(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  if (node->b == 1) {
    const uint32_t * codepoint
        = GRX_ARENA_AT(const uint32_t, &low->pattern->literals, node->a);
    if (!codepoint) {
      return fail(low, GRX_DIAG_INTERNAL, node);
    }
    return lower_codepoint(low, *codepoint, node, out_node);
  }

  GRX_Result result = add(low, GRX_IR_CONCAT, node, out_node);
  if (result != GRX_OK) {
    return result;
  }

  for (uint32_t i = 0; i < node->b; i++) {
    const uint32_t * codepoint
        = GRX_ARENA_AT(const uint32_t, &low->pattern->literals, node->a + i);
    if (!codepoint) {
      return fail(low, GRX_DIAG_INTERNAL, node);
    }
    uint32_t child = GRX_INDEX_NONE;
    result = lower_codepoint(low, *codepoint, node, &child);
    if (result == GRX_OK) {
      result = attach(low, *out_node, child);
    }
    if (result != GRX_OK) {
      return result;
    }
  }

  return GRX_OK;
}

/** Lower `.`: everything but the line terminators, unless dot-all is on. */
static GRX_Result lower_any(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  uint32_t excluded = GRX_INDEX_NONE;
  if (!(low->options & GRX_OPT_DOTALL)) {
    GRX_Result result = newline_class(low, &excluded);
    if (result != GRX_OK) {
      return result;
    }
  }

  GRX_Result result = add(low, GRX_IR_ANY, node, out_node);
  if (result != GRX_OK) {
    return result;
  }
  grx_ir_node(low->ir, *out_node)->a = excluded;
  return GRX_OK;
}

/** Whether `^` and `$` are line anchors in this pattern. */
static int multiline(const Lowering * low) {
  return (low->options & GRX_OPT_MULTILINE) != 0
      || low->profile.multiline_by_default;
}

/**
 * Lower an anchor, which is where two of the three differences a feature bit
 * cannot express are resolved.
 *
 * `^` becomes the subject anchor or the line anchor; `$` becomes one of
 * three things depending on the dialect's rule and the multiline flag. Both
 * arrive at an engine as a kind and a class index.
 */
static GRX_Result lower_anchor(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  GRX_AssertKind kind;
  int needs_newlines = 0;
  int needs_word = 0;

  switch ((GRX_AnchorKind)node->a) {
    case GRX_ANCHOR_CARET:
      if (multiline(low)) {
        kind = GRX_ASSERT_START_LINE;
        needs_newlines = 1;
      }
      else {
        kind = GRX_ASSERT_START_SUBJECT;
      }
      break;

    case GRX_ANCHOR_DOLLAR:
      if (multiline(low) || low->profile.dollar == GRX_DOLLAR_ALWAYS_LINE) {
        kind = GRX_ASSERT_END_LINE;
        needs_newlines = 1;
      }
      else if (low->profile.dollar == GRX_DOLLAR_BEFORE_FINAL_NEWLINE) {
        kind = GRX_ASSERT_END_BEFORE_NEWLINE;
        needs_newlines = 1;
      }
      else {
        kind = GRX_ASSERT_END_SUBJECT;
      }
      break;

    case GRX_ANCHOR_START_SUBJECT:
    case GRX_ANCHOR_START_BUFFER:
      kind = GRX_ASSERT_START_SUBJECT;
      break;
    case GRX_ANCHOR_END_SUBJECT:
    case GRX_ANCHOR_END_BUFFER:
      kind = GRX_ASSERT_END_SUBJECT;
      break;
    case GRX_ANCHOR_END_BEFORE_NEWLINE:
      kind = GRX_ASSERT_END_BEFORE_NEWLINE;
      needs_newlines = 1;
      break;
    case GRX_ANCHOR_WORD_BOUNDARY:
      kind = GRX_ASSERT_WORD_BOUNDARY;
      needs_word = 1;
      break;
    case GRX_ANCHOR_NOT_WORD_BOUNDARY:
      kind = GRX_ASSERT_NOT_WORD_BOUNDARY;
      needs_word = 1;
      break;
    case GRX_ANCHOR_SEARCH_START:
      kind = GRX_ASSERT_SEARCH_START;
      break;

    case GRX_ANCHOR_WORD_START:
    case GRX_ANCHOR_WORD_END:
    case GRX_ANCHOR_COUNT:
    default:
      return fail(low, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, node);
  }

  uint32_t class_index = GRX_INDEX_NONE;
  GRX_Result result = GRX_OK;
  if (needs_newlines) {
    result = newline_class(low, &class_index);
  }
  else if (needs_word) {
    result = word_class(low, &class_index);
  }
  if (result != GRX_OK) {
    return result;
  }

  result = add(low, GRX_IR_ASSERT, node, out_node);
  if (result != GRX_OK) {
    return result;
  }
  GRX_IRNode * assertion = grx_ir_node(low->ir, *out_node);
  assertion->mode = (uint8_t)kind;
  assertion->a = class_index;
  return GRX_OK;
}

/** Lower a group: a capture, or nothing at all. */
static GRX_Result lower_group(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  uint32_t body = GRX_INDEX_NONE;
  GRX_Result result = GRX_OK;

  if (node->first_child != GRX_INDEX_NONE) {
    result = lower_node(low, node->first_child, &body);
  }
  else {
    result = add(low, GRX_IR_EMPTY, node, &body);
  }
  if (result != GRX_OK) {
    return result;
  }

  if (node->flags & GRX_NODE_ATOMIC) {
    result = add(low, GRX_IR_ATOMIC, node, out_node);
    if (result != GRX_OK) {
      return result;
    }
    return attach(low, *out_node, body);
  }

  if (!(node->flags & GRX_NODE_CAPTURING)) {
    // A non-capturing group has no representation: it grouped the text, and
    // the tree already says how the text grouped.
    *out_node = body;
    return GRX_OK;
  }

  result = add(low, GRX_IR_CAPTURE, node, out_node);
  if (result != GRX_OK) {
    return result;
  }

  GRX_IRNode * capture = grx_ir_node(low->ir, *out_node);
  capture->a = node->a;
  capture->b = GRX_INDEX_NONE;

  if (node->flags & GRX_NODE_NAMED) {
    const char * name = grx_pattern_name(low->pattern, node->b);
    if (!name) {
      return fail(low, GRX_DIAG_INTERNAL, node);
    }
    uint32_t offset = GRX_INDEX_NONE;
    result = grx_ir_add_name(low->ir, name, strlen(name), &offset);
    if (result != GRX_OK) {
      return storage_failed(low, result, node);
    }
    grx_ir_node(low->ir, *out_node)->b = offset;
  }

  return attach(low, *out_node, body);
}

/** Find the group a named backreference refers to. */
static GRX_Result resolve_name(
    Lowering * low, const char * name, uint32_t * out_group) {
  for (size_t i = 0; i < low->pattern->nodes.count; i++) {
    const GRX_Node * node = grx_pattern_node(low->pattern, (uint32_t)i);
    if (!node || node->kind != GRX_NODE_GROUP
        || !(node->flags & GRX_NODE_NAMED)) {
      continue;
    }
    const char * candidate = grx_pattern_name(low->pattern, node->b);
    if (candidate && strcmp(candidate, name) == 0) {
      *out_group = node->a;
      return GRX_OK;
    }
  }

  return GRX_ERR_SYNTAX;
}

/** Lower a backreference, resolving a name to a number and fixing the modes. */
static GRX_Result lower_backref(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  uint32_t group = node->a;
  if (node->flags & GRX_NODE_NAMED) {
    const char * name = grx_pattern_name(low->pattern, node->b);
    if (!name || resolve_name(low, name, &group) != GRX_OK) {
      return fail(low, GRX_DIAG_UNKNOWN_GROUP_NAME, node);
    }
  }

  GRX_Result result = add(low, GRX_IR_BACKREF, node, out_node);
  if (result != GRX_OK) {
    return result;
  }

  GRX_IRNode * backref = grx_ir_node(low->ir, *out_node);
  backref->a = group;
  backref->backref_unset = (uint8_t)low->profile.backref_unset;
  if (low->fold != GRX_FOLD_NONE) {
    // The engine compares folded code points rather than bytes. It is the one
    // place folding survives lowering, because what a backreference matches
    // is not known until the match runs.
    backref->flags |= GRX_IR_CASELESS;
  }
  return GRX_OK;
}

/** Lower a repeat, attaching the dialect's two loop rules as modes. */
static GRX_Result lower_repeat(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  if (node->first_child == GRX_INDEX_NONE) {
    return fail(low, GRX_DIAG_INTERNAL, node);
  }

  uint32_t body = GRX_INDEX_NONE;
  GRX_Result result = lower_node(low, node->first_child, &body);
  if (result != GRX_OK) {
    return result;
  }

  result = add(low, GRX_IR_REPEAT, node, out_node);
  if (result != GRX_OK) {
    return result;
  }

  GRX_IRNode * repeat = grx_ir_node(low->ir, *out_node);
  repeat->min = node->min;
  repeat->max = node->max;
  repeat->mode = (uint8_t)node->a;
  repeat->empty_loop = (uint8_t)low->profile.empty_loop;
  repeat->capture_reset = (uint8_t)low->profile.capture_reset;
  return attach(low, *out_node, body);
}

/** Lower a lookaround, marking a lookbehind's body to run right to left. */
static GRX_Result lower_look(
    Lowering * low, const GRX_Node * node, uint32_t * out_node) {
  GRX_LookKind kind = (GRX_LookKind)node->a;
  int behind = kind == GRX_LOOK_BEHIND_POSITIVE
      || kind == GRX_LOOK_BEHIND_NEGATIVE;

  GRX_Result result = add(low, GRX_IR_LOOK, node, out_node);
  if (result != GRX_OK) {
    return result;
  }
  grx_ir_node(low->ir, *out_node)->mode = (uint8_t)kind;

  // Inside a lookbehind every node is marked reverse, and its instructions
  // step backwards. That is the whole of what makes a lookbehind of any
  // length work without a second engine (design.md section 3.5.2).
  int outer = low->reverse;
  if (behind) {
    low->reverse = 1;
  }

  uint32_t body = GRX_INDEX_NONE;
  if (node->first_child != GRX_INDEX_NONE) {
    result = lower_node(low, node->first_child, &body);
  }
  else {
    result = add(low, GRX_IR_EMPTY, node, &body);
  }
  low->reverse = outer;
  if (result != GRX_OK) {
    return result;
  }

  return attach(low, *out_node, body);
}

/** Lower a concatenation or an alternation: the same shape, a different kind. */
static GRX_Result lower_sequence(Lowering * low, const GRX_Node * node,
    GRX_IRKind kind, uint32_t * out_node) {
  GRX_Result result = add(low, kind, node, out_node);
  if (result != GRX_OK) {
    return result;
  }

  for (uint32_t child = node->first_child; child != GRX_INDEX_NONE;) {
    const GRX_Node * child_node = grx_pattern_node(low->pattern, child);
    if (!child_node) {
      return fail(low, GRX_DIAG_INTERNAL, node);
    }
    uint32_t next = child_node->next_sibling;

    uint32_t lowered = GRX_INDEX_NONE;
    result = lower_node(low, child, &lowered);
    if (result == GRX_OK) {
      result = attach(low, *out_node, lowered);
    }
    if (result != GRX_OK) {
      return result;
    }
    child = next;
  }

  return GRX_OK;
}

static GRX_Result lower_node(
    Lowering * low, uint32_t node_index, uint32_t * out_node) {
  const GRX_Node * node = grx_pattern_node(low->pattern, node_index);
  if (!node) {
    return fail(low, GRX_DIAG_INTERNAL, NULL);
  }

  switch (node->kind) {
    case GRX_NODE_EMPTY:
      return add(low, GRX_IR_EMPTY, node, out_node);

    case GRX_NODE_LITERAL:
      return lower_literal(low, node, out_node);

    case GRX_NODE_CLASS: {
      GRX_CharClass cls;
      grx_charclass_init(&cls, low->ir->allocator);
      GRX_Result result = evaluate_class(low, node, &cls);
      uint32_t class_index = GRX_INDEX_NONE;
      if (result == GRX_OK) {
        result = intern(low, &cls, node, &class_index);
      }
      grx_charclass_clear(&cls);
      if (result != GRX_OK) {
        return result;
      }
      result = add(low, GRX_IR_CLASS, node, out_node);
      if (result != GRX_OK) {
        return result;
      }
      grx_ir_node(low->ir, *out_node)->a = class_index;
      return GRX_OK;
    }

    case GRX_NODE_ANY:
      return lower_any(low, node, out_node);

    case GRX_NODE_CONCAT:
      return lower_sequence(low, node, GRX_IR_CONCAT, out_node);

    case GRX_NODE_ALTERNATE:
      return lower_sequence(low, node, GRX_IR_ALTERNATE, out_node);

    case GRX_NODE_REPEAT:
      return lower_repeat(low, node, out_node);

    case GRX_NODE_GROUP:
      return lower_group(low, node, out_node);

    case GRX_NODE_BACKREF:
      return lower_backref(low, node, out_node);

    case GRX_NODE_ANCHOR:
      return lower_anchor(low, node, out_node);

    case GRX_NODE_LOOKAROUND:
      return lower_look(low, node, out_node);

    case GRX_NODE_KEEP:
      return add(low, GRX_IR_KEEP, node, out_node);

    // Everything the parsers of later phases will produce. Each has an IR
    // kind waiting for it; none has a front end that emits it yet, and a
    // pattern reaching here with one is a bug in whichever front end did.
    case GRX_NODE_CONDITIONAL:
    case GRX_NODE_RECURSE:
    case GRX_NODE_CONTROL:
    case GRX_NODE_OPTIONS:
    case GRX_NODE_CLASS_OP:
    case GRX_NODE_STRING_SET:
    case GRX_NODE_BRANCH_RESET:
    case GRX_NODE_COUNT:
    default:
      return fail(low, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, node);
  }
}

GRX_Result grx_lower_pattern(const GRX_Pattern * pattern,
    const GRX_Limits * limits, const GRX_Allocator * allocator,
    GRX_Error * out_error, GRX_IR ** out_ir) {
  if (!pattern || !limits || !allocator || !out_ir) {
    return GRX_ERR_INVALID;
  }
  *out_ir = NULL;

  Lowering low = {
    .pattern = pattern,
    .ir = NULL,
    .limits = limits,
    .error = out_error,
    .options = pattern->options,
    .reverse = 0,
    .newline_class = GRX_INDEX_NONE,
    .word_class = GRX_INDEX_NONE,
  };

  GRX_Result result = grx_syntax_spec(pattern->syntax, &low.spec);
  if (result == GRX_OK) {
    result = grx_syntax_profile(pattern->syntax, &low.profile);
  }
  if (result != GRX_OK) {
    return result;
  }

  // The two option-dependent choices the whole run reads. Both are made once
  // here rather than at each use, so that "which folding is this pattern
  // using" has one answer.
  int utf = (low.options & GRX_OPT_UTF) != 0
      || (low.options & GRX_OPT_UCP) != 0;
  low.shorthands
      = utf ? low.profile.shorthands_utf : low.profile.shorthands;
  low.fold = GRX_FOLD_NONE;
  if (low.options & GRX_OPT_CASELESS) {
    low.fold = utf ? low.profile.fold_utf : low.profile.fold;
  }

  result = grx_ir_create(allocator, limits, &low.ir);
  if (result != GRX_OK) {
    return grx_error_set(
        out_error, result, GRX_DIAG_OUT_OF_MEMORY, GRX_NPOS, 0);
  }

  low.ir->preference = low.profile.preference;
  low.ir->iteration = low.profile.iteration;
  low.ir->capture_count = pattern->capture_count;
  if ((low.options & GRX_OPT_UTF) || low.profile.subject_is_text) {
    low.ir->flags |= GRX_PROGRAM_UTF;
  }

  uint32_t root = GRX_INDEX_NONE;
  if (pattern->root == GRX_INDEX_NONE) {
    result = add(&low, GRX_IR_EMPTY, NULL, &root);
  }
  else {
    result = lower_node(&low, pattern->root, &root);
  }

  if (result != GRX_OK) {
    grx_ir_free(low.ir);
    return result;
  }

  low.ir->root = root;
  *out_ir = low.ir;
  return GRX_OK;
}
