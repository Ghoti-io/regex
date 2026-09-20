/**
 * @file
 *
 * Building, reading, disassembling and freeing a compiled program.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "compile_internal.h"

const char * grx_opcode_name(GRX_Opcode op) {
  static const char * const names[GRX_OP_COUNT] = {
    "match", "char", "class", "any", "any-nl", "split", "jmp", "save",
    "assert", "progress-set", "reset", "reset-stale", "progress-check",
    "backref", "look",
    "scan", "rewind", "atomic-begin", "atomic-end", "cond", "call", "ret",
    "keep", "verb",
  };
  return (unsigned)op < GRX_OP_COUNT ? names[op] : "?";
}

/** The name of an assertion, for the disassembly. */
static const char * assert_name(uint8_t kind) {
  static const char * const names[GRX_ASSERT_COUNT] = {
    "start-subject", "end-subject", "end-before-newline", "start-line",
    "start-line-interior", "end-line", "word-boundary",
    "not-word-boundary", "word-start", "word-end", "search-start",
    "grapheme-boundary", "not-grapheme-boundary",
    "word-seg-boundary", "not-word-seg-boundary",
    "sentence-boundary", "not-sentence-boundary",
    "line-boundary", "not-line-boundary", "look-length",
  };
  return kind < GRX_ASSERT_COUNT ? names[kind] : "?";
}

/** The name of a lookaround, for the disassembly. */
static const char * look_name(uint8_t kind) {
  static const char * const names[GRX_LOOK_COUNT] = {
    "ahead", "ahead-negative", "behind", "behind-negative",
    "ahead-non-atomic", "behind-non-atomic",
  };
  return kind < GRX_LOOK_COUNT ? names[kind] : "?";
}

/** The name of an empty-iteration rule, for the disassembly. */
static const char * empty_loop_name(uint8_t mode) {
  static const char * const names[GRX_EMPTY_LOOP_COUNT] = {
    "fail", "break", "allow",
  };
  return mode < GRX_EMPTY_LOOP_COUNT ? names[mode] : "?";
}

/** The name of an unset-backreference rule, for the disassembly. */
static const char * backref_unset_name(uint8_t mode) {
  static const char * const names[GRX_BACKREF_UNSET_COUNT] = {
    "fails", "empty",
  };
  return mode < GRX_BACKREF_UNSET_COUNT ? names[mode] : "?";
}

/** The name of a condition, for the disassembly. */
static const char * cond_name(uint8_t kind) {
  static const char * const names[GRX_COND_COUNT] = {
    "group-set", "recursion-any", "recursion-group", "assertion", "define",
  };
  return kind < GRX_COND_COUNT ? names[kind] : "?";
}

/** The name of a verb, for the disassembly. */
static const char * verb_name(uint8_t kind) {
  static const char * const names[GRX_VERB_COUNT] = {
    "ACCEPT", "FAIL", "COMMIT", "PRUNE", "SKIP", "THEN", "MARK",
  };
  return kind < GRX_VERB_COUNT ? names[kind] : "?";
}

/** The name of a match preference, for the disassembly header. */
static const char * preference_name(GRX_MatchPreference preference) {
  static const char * const names[GRX_PREFER_COUNT] = {
    "leftmost-first", "leftmost-longest",
  };
  return (unsigned)preference < GRX_PREFER_COUNT ? names[preference] : "?";
}

void grx_program_init(GRX_Program * program, const GRX_Allocator * allocator,
    const GRX_Limits * limits) {
  if (!program || !limits) {
    return;
  }

  grx_arena_init(&program->insts, allocator, sizeof(GRX_Inst),
      limits->max_program_size, GRX_DIAG_LIMIT_PROGRAM_SIZE);
  grx_class_table_init(&program->classes, allocator, limits->max_class_ranges);
  grx_arena_init(
      &program->scan_lists, allocator, sizeof(uint32_t), 0, GRX_DIAG_NONE);
  grx_arena_init(
      &program->look_spans, allocator, sizeof(size_t), 0, GRX_DIAG_NONE);
  program->flags = 0;
  program->register_count = 0;
  program->preference = GRX_PREFER_LEFTMOST_FIRST;
  program->iteration = GRX_ITERATE_RETRY_THEN_ADVANCE;
}

GRX_Result grx_program_add(
    GRX_Program * program, const GRX_Inst * inst, uint32_t * out_index) {
  if (!program || !inst) {
    return GRX_ERR_INVALID;
  }

  return grx_arena_append(&program->insts, inst, out_index);
}

GRX_Inst * grx_program_at(const GRX_Program * program, uint32_t index) {
  if (!program || index == GRX_INDEX_NONE) {
    return NULL;
  }

  return GRX_ARENA_AT(GRX_Inst, &program->insts, index);
}

/** Write the operands that belong to this instruction's opcode. */
static void dump_operands(
    FILE * out, const GRX_Program * program, const GRX_Inst * inst) {
  switch ((GRX_Opcode)inst->op) {
    case GRX_OP_CHAR:
      if (inst->x >= 0x20 && inst->x < 0x7F) {
        fprintf(out, "'%c'", (char)inst->x);
      }
      else {
        fprintf(out, "U+%04X", inst->x);
      }
      break;
    case GRX_OP_CLASS:
      fprintf(out, "#%u", inst->x);
      break;
    case GRX_OP_ANY:
      fprintf(out, "except #%u", inst->x);
      break;
    case GRX_OP_SPLIT:
      fprintf(out, "%u, %u", inst->x, inst->y);
      break;
    case GRX_OP_JMP:
      fprintf(out, "%u", inst->x);
      break;
    case GRX_OP_SAVE:
      // Slot to group and end, which is what a reader actually wants.
      fprintf(out, "%u  (group %u %s)", inst->x, inst->x / 2,
          (inst->x % 2) ? "end" : "start");
      break;
    case GRX_OP_ASSERT:
      fputs(assert_name(inst->mode), out);
      if (inst->mode == GRX_ASSERT_LOOK_LENGTH) {
        // `x` is a length span here, not a class.
        size_t min = 0;
        size_t max = 0;
        if (grx_program_look_span(program, inst->x, &min, &max)) {
          fprintf(out, " %zu..%zu", min, max);
        }
      }
      else if (inst->x != GRX_INDEX_NONE) {
        fprintf(out, " set=#%u", inst->x);
      }
      break;
    case GRX_OP_PROGRESS_SET:
      fprintf(out, "r%u", inst->x);
      break;
    case GRX_OP_RESET:
      fprintf(out, "slots %u..%u", inst->x, inst->y ? inst->y - 1 : 0);
      break;
    case GRX_OP_RESET_STALE:
      fprintf(out, "r%u, slot %u  (group %u)", inst->x, inst->y, inst->y / 2);
      break;
    case GRX_OP_PROGRESS_CHECK:
      fprintf(out, "r%u, %u  (%s)", inst->x, inst->y,
          empty_loop_name(inst->mode));
      break;
    case GRX_OP_BACKREF:
      fprintf(out, "#%u  (unset %s)", inst->x,
          backref_unset_name(inst->mode));
      break;
    case GRX_OP_LOOK: {
      // The body is the next instruction, always, so what is worth printing
      // is the thing that is not derivable: whether this one runs forwards
      // from a candidate start, and between which lengths.
      size_t min = 0;
      size_t max = 0;
      fprintf(out, "%s next=%u", look_name(inst->mode), inst->y);
      if (grx_program_look_span(program, inst->x, &min, &max)) {
        fprintf(out, " forward=%zu..%zu", min, max);
      }
      break;
    }
    case GRX_OP_ATOMIC_BEGIN:
      fprintf(out, "end=%u", inst->x);
      break;
    case GRX_OP_COND:
      fprintf(out, "%s #%u else=%u", cond_name(inst->mode), inst->x, inst->y);
      break;
    case GRX_OP_CALL:
      fprintf(out, "target=%u next=%u", inst->x, inst->y);
      break;
    case GRX_OP_VERB:
      fputs(verb_name(inst->mode), out);
      break;
    default:
      break;
  }
}

/** The name of an iteration rule, for the dump header. */
static const char * iteration_name(GRX_IterationRule iteration) {
  static const char * const names[GRX_ITERATE_COUNT] = {
    "retry-then-advance", "advance-one", "advance-skip-abutting"};
  return (unsigned)iteration < GRX_ITERATE_COUNT ? names[iteration] : "?";
}

GRX_Result grx_program_dump(const GRX_Program * program, FILE * out) {
  if (!program || !out) {
    return GRX_ERR_INVALID;
  }

  fprintf(out,
      "program: flags=0x%08x prefer=%s iterate=%s insts=%zu classes=%zu "
      "registers=%u\n",
      program->flags, preference_name(program->preference),
      iteration_name(program->iteration), program->insts.count,
      grx_class_table_count(&program->classes), program->register_count);

  for (size_t i = 0; i < program->insts.count; i++) {
    const GRX_Inst * inst
        = GRX_ARENA_AT(const GRX_Inst, &program->insts, i);
    if (!inst) {
      break;
    }

    fprintf(out, "  %4zu  %-14s ", i, grx_opcode_name((GRX_Opcode)inst->op));
    dump_operands(out, program, inst);
    if (inst->flags & GRX_INST_REVERSE) {
      fputs("  reverse", out);
    }
    fputc('\n', out);
  }

  return GRX_OK;
}

int grx_program_look_span(const GRX_Program * program, uint32_t offset,
    size_t * out_min, size_t * out_max) {
  if (!program || !out_min || !out_max || offset == GRX_INDEX_NONE
      || offset + 1 >= program->look_spans.count) {
    return 0;
  }

  const size_t * pair
      = GRX_ARENA_AT(const size_t, &program->look_spans, offset);
  if (!pair) {
    return 0;
  }
  *out_min = pair[0];
  *out_max = pair[1];
  return 1;
}

const uint32_t * grx_program_scan_list(
    const GRX_Program * program, uint32_t offset, size_t * out_count) {
  if (!program || !out_count || offset >= program->scan_lists.count) {
    return NULL;
  }

  const uint32_t * count
      = GRX_ARENA_AT(const uint32_t, &program->scan_lists, offset);
  if (!count) {
    return NULL;
  }
  *out_count = *count;
  return GRX_ARENA_AT(const uint32_t, &program->scan_lists, offset + 1);
}

void grx_program_clear(GRX_Program * program) {
  if (!program) {
    return;
  }

  grx_arena_clear(&program->insts);
  grx_class_table_clear(&program->classes);
  grx_arena_clear(&program->scan_lists);
  grx_arena_clear(&program->look_spans);
  program->register_count = 0;
}
