/**
 * @file
 *
 * Building, reading, dumping and freeing the intermediate representation.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdio.h>
#include <stdint.h>

#include "ir_internal.h"

/** As in the AST dump: a debugging aid must not overflow the stack. */
#define GRX_IR_DUMP_MAX_DEPTH 256

/** The name of an IR node kind, for the dump. */
static const char * ir_kind_name(GRX_IRKind kind) {
  static const char * const names[GRX_IR_COUNT] = {
    "empty", "char", "class", "any", "concat", "alternate", "repeat",
    "capture", "backref", "assert", "look", "atomic", "cond", "recurse",
    "keep", "verb",
  };
  return (unsigned)kind < GRX_IR_COUNT ? names[kind] : "?";
}

/** The name of an assertion, for the dump. */
static const char * assert_name(uint8_t kind) {
  static const char * const names[GRX_ASSERT_COUNT] = {
    "start-subject", "end-subject", "end-before-newline", "start-line",
    "end-line", "word-boundary", "not-word-boundary", "search-start",
  };
  return kind < GRX_ASSERT_COUNT ? names[kind] : "?";
}

/** The name of a lookaround, for the dump. */
static const char * look_name(uint8_t kind) {
  static const char * const names[GRX_LOOK_COUNT] = {
    "ahead", "ahead-negative", "behind", "behind-negative",
  };
  return kind < GRX_LOOK_COUNT ? names[kind] : "?";
}

/** The name of a repeat mode, for the dump. */
static const char * repeat_mode_name(uint8_t mode) {
  static const char * const names[GRX_REPEAT_MODE_COUNT] = {
    "greedy", "lazy", "possessive",
  };
  return mode < GRX_REPEAT_MODE_COUNT ? names[mode] : "?";
}

/** The name of an empty-iteration rule, for the dump. */
static const char * empty_loop_name(uint8_t mode) {
  static const char * const names[GRX_EMPTY_LOOP_COUNT] = {
    "fail", "break", "allow",
  };
  return mode < GRX_EMPTY_LOOP_COUNT ? names[mode] : "?";
}

/** The name of a capture-reset rule, for the dump. */
static const char * capture_reset_name(uint8_t mode) {
  static const char * const names[GRX_CAPTURE_RESET_COUNT] = {
    "keep", "each",
  };
  return mode < GRX_CAPTURE_RESET_COUNT ? names[mode] : "?";
}

/** The name of an unset-backreference rule, for the dump. */
static const char * backref_unset_name(uint8_t mode) {
  static const char * const names[GRX_BACKREF_UNSET_COUNT] = {
    "fails", "empty",
  };
  return mode < GRX_BACKREF_UNSET_COUNT ? names[mode] : "?";
}

/** The name of a condition, for the dump. */
static const char * cond_name(uint8_t kind) {
  static const char * const names[GRX_COND_COUNT] = {
    "group-set", "recursion-any", "recursion-group", "assertion", "define",
  };
  return kind < GRX_COND_COUNT ? names[kind] : "?";
}

/** The name of a verb, for the dump. */
static const char * verb_name(uint8_t kind) {
  static const char * const names[GRX_VERB_COUNT] = {
    "ACCEPT", "FAIL", "COMMIT", "PRUNE", "SKIP", "THEN",
  };
  return kind < GRX_VERB_COUNT ? names[kind] : "?";
}

/** The name of a match preference, for the dump. */
static const char * preference_name(GRX_MatchPreference preference) {
  static const char * const names[GRX_PREFER_COUNT] = {
    "leftmost-first", "leftmost-longest",
  };
  return (unsigned)preference < GRX_PREFER_COUNT ? names[preference] : "?";
}

/** As in the AST dump: printable ASCII as itself, everything else escaped. */
static void write_codepoint(FILE * out, uint32_t codepoint) {
  if (codepoint == '\\' || codepoint == '\'') {
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

GRX_Result grx_ir_create(const GRX_Allocator * allocator,
    const GRX_Limits * limits, GRX_IR ** out_ir) {
  if (!out_ir || !limits) {
    return GRX_ERR_INVALID;
  }
  *out_ir = NULL;

  if (!allocator) {
    allocator = grx_allocator_default();
  }

  GRX_IR * ir = gcu_allocator_calloc(allocator, 1, sizeof(GRX_IR));
  if (!ir) {
    return GRX_ERR_OOM;
  }

  ir->allocator = allocator;
  ir->root = GRX_INDEX_NONE;
  ir->flags = 0;
  ir->preference = GRX_PREFER_LEFTMOST_FIRST;
  ir->iteration = GRX_ITERATE_RETRY_THEN_ADVANCE;
  ir->capture_count = 0;

  // The IR's node count is capped by max_nodes like the AST's: lowering can
  // expand a node into several - a string set becomes an alternation - and
  // the cap has to hold on what it produces, not only on what it read.
  grx_arena_init(&ir->nodes, allocator, sizeof(GRX_IRNode), limits->max_nodes,
      GRX_DIAG_LIMIT_NODES);
  grx_class_table_init(&ir->classes, allocator, limits->max_class_ranges);
  grx_arena_init(&ir->names, allocator, sizeof(char), 0, GRX_DIAG_NONE);

  *out_ir = ir;
  return GRX_OK;
}

GRX_Result grx_ir_add_node(GRX_IR * ir, GRX_IRKind kind, size_t offset,
    size_t length, uint32_t * out_index) {
  if (!ir) {
    return GRX_ERR_INVALID;
  }

  GRX_IRNode node = {
    .kind = kind,
    .flags = 0,
    .first_child = GRX_INDEX_NONE,
    .last_child = GRX_INDEX_NONE,
    .next_sibling = GRX_INDEX_NONE,
    .a = 0,
    .b = 0,
    .min = 0,
    .max = 0,
    .mode = 0,
    .empty_loop = GRX_EMPTY_LOOP_FAIL,
    .capture_reset = GRX_CAPTURE_KEEP_LAST_SET,
    .backref_unset = GRX_BACKREF_UNSET_FAILS,
    .offset = offset,
    .length = length,
  };

  return grx_arena_append(&ir->nodes, &node, out_index);
}

GRX_Result grx_ir_add_child(GRX_IR * ir, uint32_t parent, uint32_t child) {
  GRX_IRNode * parent_node = grx_ir_node(ir, parent);
  GRX_IRNode * child_node = grx_ir_node(ir, child);
  if (!parent_node || !child_node || parent == child) {
    return GRX_ERR_INVALID;
  }
  if (child_node->next_sibling != GRX_INDEX_NONE) {
    return GRX_ERR_INVALID;
  }

  if (parent_node->last_child == GRX_INDEX_NONE) {
    parent_node->first_child = child;
  }
  else {
    GRX_IRNode * last = grx_ir_node(ir, parent_node->last_child);
    if (!last) {
      return GRX_ERR_INTERNAL;
    }
    last->next_sibling = child;
  }
  parent_node->last_child = child;

  return GRX_OK;
}

GRX_IRNode * grx_ir_node(const GRX_IR * ir, uint32_t index) {
  if (!ir || index == GRX_INDEX_NONE) {
    return NULL;
  }

  return GRX_ARENA_AT(GRX_IRNode, &ir->nodes, index);
}

GRX_Result grx_ir_add_name(
    GRX_IR * ir, const char * name, size_t length, uint32_t * out_offset) {
  if (!ir || (!name && length)) {
    return GRX_ERR_INVALID;
  }

  uint32_t start = (uint32_t)ir->names.count;
  for (size_t i = 0; i < length; i++) {
    GRX_Result result = grx_arena_append(&ir->names, &name[i], NULL);
    if (result != GRX_OK) {
      return result;
    }
  }

  char terminator = '\0';
  GRX_Result result = grx_arena_append(&ir->names, &terminator, NULL);
  if (result != GRX_OK) {
    return result;
  }

  if (out_offset) {
    *out_offset = start;
  }
  return GRX_OK;
}

const char * grx_ir_name(const GRX_IR * ir, uint32_t offset) {
  if (!ir || offset == GRX_INDEX_NONE) {
    return NULL;
  }

  return GRX_ARENA_AT(const char, &ir->names, offset);
}

/** Write the payload that belongs to this node's kind. */
static void dump_payload(FILE * out, const GRX_IR * ir, const GRX_IRNode * node) {
  switch (node->kind) {
    case GRX_IR_CHAR:
      fputs(" '", out);
      write_codepoint(out, node->a);
      fputc('\'', out);
      break;
    case GRX_IR_CLASS:
      fprintf(out, " #%u", node->a);
      break;
    case GRX_IR_ANY:
      if (node->a == GRX_INDEX_NONE) {
        fputs(" all", out);
      }
      else {
        fprintf(out, " except #%u", node->a);
      }
      break;
    case GRX_IR_REPEAT:
      if (node->max == GRX_REPEAT_INF) {
        fprintf(out, " {%u,}", node->min);
      }
      else {
        fprintf(out, " {%u,%u}", node->min, node->max);
      }
      fprintf(out, " %s empty=%s reset=%s", repeat_mode_name(node->mode),
          empty_loop_name(node->empty_loop),
          capture_reset_name(node->capture_reset));
      break;
    case GRX_IR_CAPTURE:
      fprintf(out, " #%u", node->a);
      if (node->b != GRX_INDEX_NONE) {
        fprintf(out, " name=%s", grx_ir_name(ir, node->b));
      }
      break;
    case GRX_IR_BACKREF:
      fprintf(out, " #%u unset=%s", node->a,
          backref_unset_name(node->backref_unset));
      if (node->flags & GRX_IR_CASELESS) {
        fputs(" caseless", out);
      }
      break;
    case GRX_IR_ASSERT:
      fprintf(out, " %s", assert_name(node->mode));
      if (node->a != GRX_INDEX_NONE) {
        fprintf(out, " set=#%u", node->a);
      }
      break;
    case GRX_IR_LOOK:
      fprintf(out, " %s", look_name(node->mode));
      break;
    case GRX_IR_COND:
      fprintf(out, " %s", cond_name(node->mode));
      if (node->mode == GRX_COND_GROUP_SET
          || node->mode == GRX_COND_RECURSION_GROUP) {
        fprintf(out, " #%u", node->a);
      }
      break;
    case GRX_IR_RECURSE:
      fprintf(out, " #%u", node->a);
      break;
    case GRX_IR_VERB:
      fprintf(out, " %s", verb_name(node->mode));
      break;
    default:
      break;
  }

  if (node->flags & GRX_IR_REVERSE) {
    fputs(" reverse", out);
  }
}

/** Write one node and, indented beneath it, its children. */
static void dump_node(
    FILE * out, const GRX_IR * ir, uint32_t index, unsigned depth) {
  const GRX_IRNode * node = grx_ir_node(ir, index);
  if (!node) {
    return;
  }

  for (unsigned i = 0; i < depth; i++) {
    fputs("  ", out);
  }

  fprintf(out, "%s", ir_kind_name(node->kind));
  dump_payload(out, ir, node);
  fprintf(out, " @%zu+%zu\n", node->offset, node->length);

  if (depth + 1 >= GRX_IR_DUMP_MAX_DEPTH) {
    if (node->first_child != GRX_INDEX_NONE) {
      for (unsigned i = 0; i <= depth; i++) {
        fputs("  ", out);
      }
      fputs("... (depth limit)\n", out);
    }
    return;
  }

  for (uint32_t child = node->first_child; child != GRX_INDEX_NONE;) {
    const GRX_IRNode * child_node = grx_ir_node(ir, child);
    if (!child_node) {
      break;
    }
    dump_node(out, ir, child, depth + 1);
    child = child_node->next_sibling;
  }
}

/** The name of an iteration rule, for the dump header. */
static const char * iteration_name(GRX_IterationRule iteration) {
  static const char * const names[GRX_ITERATE_COUNT] = {
    "retry-then-advance", "advance-one", "advance-skip-abutting"};
  return (unsigned)iteration < GRX_ITERATE_COUNT ? names[iteration] : "?";
}

GRX_Result grx_ir_dump(const GRX_IR * ir, FILE * out) {
  if (!ir || !out) {
    return GRX_ERR_INVALID;
  }

  fprintf(out,
      "ir: flags=0x%08x prefer=%s iterate=%s nodes=%zu captures=%zu "
      "classes=%zu\n",
      ir->flags, preference_name(ir->preference),
      iteration_name(ir->iteration), ir->nodes.count, ir->capture_count,
      grx_class_table_count(&ir->classes));

  if (ir->root != GRX_INDEX_NONE) {
    dump_node(out, ir, ir->root, 1);
  }

  return GRX_OK;
}

void grx_ir_free(GRX_IR * ir) {
  if (!ir) {
    return;
  }

  const GRX_Allocator * allocator = ir->allocator;
  if (!allocator) {
    allocator = grx_allocator_default();
  }

  grx_arena_clear(&ir->nodes);
  grx_class_table_clear(&ir->classes);
  grx_arena_clear(&ir->names);
  gcu_allocator_free(allocator, ir);
}
