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
 * Building, reading, disassembling and freeing a compiled program.
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "compile_internal.h"

const char * grx_opcode_name(GRX_Opcode op) {
  static const char * const names[GRX_OP_COUNT] = {
    "match", "char", "class", "any", "any-nl", "split", "jmp", "save",
    "assert", "progress-set", "reset", "reset-stale", "progress-check",
    "backref", "look",
    "scan", "rewind", "atomic-begin", "atomic-end", "cond", "call", "ret",
    "keep", "keep-end", "verb", "script-run", "callout",
    "count-init", "count-step", "count-back",
  };
  // NULL-checked for the reason ir_kind_name() is: an opcode added without
  // a mnemonic is a zeroed slot, and a disassembler that crashes hides the
  // defect it was opened to find.
  return (unsigned)op < GRX_OP_COUNT && names[op] ? names[op] : "?";
}

/**
 * A group operand: one number, or the list of groups a duplicated name
 * stands for.
 *
 * GRX_INST_AMBIGUOUS_REF says `x` is where a list starts rather than a
 * group number, and a dump that printed it either way would be naming a
 * group the pattern may not have.
 */
static void dump_group_operand(
    FILE * out, const GRX_Program * program, const GRX_Inst * inst) {
  if (!(inst->flags & GRX_INST_AMBIGUOUS_REF)) {
    fprintf(out, "#%u", inst->x);
    return;
  }
  size_t count = 0;
  const uint32_t * groups = grx_program_scan_list(program, inst->x, &count);
  fputs("#", out);
  for (size_t i = 0; groups && i < count; i++) {
    fprintf(out, "%s%u", i ? "|" : "", groups[i]);
  }
  if (!groups || !count) {
    fprintf(out, "?");
  }
}

/** The name of an assertion, for the disassembly. */
static const char * assert_name(uint8_t kind) {
  static const char * const names[GRX_ASSERT_COUNT] = {
    "start-subject", "end-subject", "end-before-newline", "start-line",
    "start-line-interior", "end-line", "word-boundary",
    "not-word-boundary", "word-start", "word-end",
    "word-class-start", "word-class-end",
    "not-composing", "cluster-boundary", "search-start",
    "grapheme-boundary", "not-grapheme-boundary",
    "word-seg-boundary", "not-word-seg-boundary",
    "sentence-boundary", "not-sentence-boundary",
    "line-boundary", "not-line-boundary", "look-length",
    "byte-column", "never", "screen-column",
      "scalar-frontier",
      "no-scalar-frontier",
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
    "fail", "break", "allow", "break-first", "simulate",
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
    "static",
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

/** The name of a submatch rule, for the disassembly header. */
static const char * submatch_name(GRX_SubmatchRule submatch) {
  static const char * names[GRX_SUBMATCH_COUNT] = {"first-path", "posix"};
  return (unsigned)submatch < GRX_SUBMATCH_COUNT ? names[submatch] : "?";
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
  grx_arena_init(&program->scalar_scopes, allocator, sizeof(uint32_t),
      limits->max_program_size, GRX_DIAG_LIMIT_PROGRAM_SIZE);
  grx_arena_init(
      &program->scan_lists, allocator, sizeof(uint32_t), 0, GRX_DIAG_NONE);
  grx_arena_init(
      &program->look_spans, allocator, sizeof(size_t), 0, GRX_DIAG_NONE);
  grx_arena_init(&program->callouts, allocator, sizeof(GRX_ProgramCallout), 0,
      GRX_DIAG_NONE);
  grx_arena_init(
      &program->callout_strings, allocator, sizeof(char), 0, GRX_DIAG_NONE);
  program->flags = 0;
  program->candidate_literal = 0;
  program->candidate_literal_length = 0;
  program->candidate_options = 0;
  program->register_count = 0;
  // The prefilter fields are not filled until after codegen, and "not known"
  // has to be what they say until then: every one of them is read by the
  // engines, and a stale value here would skip a position a match begins at.
  program->first_bytes_known = 0;
  memset(program->first_bytes, 0, sizeof program->first_bytes);
  memset(&program->first_byte_ranges, 0, sizeof program->first_byte_ranges);
  program->literal_prefix_length = 0;
  program->required_literal_length = 0;
  program->required_offset_min = 0;
  program->required_offset_max = GRX_NPOS;
  program->preference = GRX_PREFER_LEFTMOST_FIRST;
  program->submatch = GRX_SUBMATCH_FIRST_PATH;
  program->iteration = GRX_ITERATE_RETRY_THEN_ADVANCE;
  program->search_start = GRX_SEARCH_START_ATTEMPT;
}

GRX_Result grx_program_add(
    GRX_Program * program, const GRX_Inst * inst, uint32_t * out_index) {
  if (!program || !inst) {
    return GRX_ERR_INVALID;
  }

  return grx_arena_append(&program->insts, inst, out_index);
}

uint32_t grx_program_scope(const GRX_Program * program, uint32_t index) {
  const uint32_t * scope = program && index < program->insts.count
      ? GRX_ARENA_AT(const uint32_t, &program->scalar_scopes, index)
      : NULL;
  return scope ? *scope : 0;
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
      if (inst->policy & GRX_POLICY_ASSERT_FINAL_BOUNDARY)
        fputs(" whole-boundary", out);
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
    case GRX_OP_COUNT_INIT:
    case GRX_OP_COUNT_STEP:
    case GRX_OP_COUNT_BACK:
      fprintf(out, "count=#%u, %u", inst->x, inst->y);
      if (inst->x < program->look_spans.count &&
          program->look_spans.count - inst->x >= 4) {
        const size_t * count =
            GRX_ARENA_AT(const size_t, &program->look_spans, inst->x);
        fprintf(out, " (r%zu, %zu..%zu)", count[2], count[0], count[1]);
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
      fprintf(
          out, "r%u, %u  (%s)", inst->x, inst->y, empty_loop_name(inst->mode));
      break;
    case GRX_OP_BACKREF:
      // `x` is a group number, unless the name it was written with belongs
      // to several - then it is where the *list* of them starts, and a
      // dump that printed it as "#5" would be naming a group the pattern
      // may not have. The same is true of a conditional below.
      dump_group_operand(out, program, inst);
      fprintf(out, "  (unset %s)", backref_unset_name(inst->mode));
      if (inst->policy & GRX_POLICY_COMPLETE_COMPOSING)
        fputs(" forward-composing", out);
      if (inst->policy & GRX_POLICY_REPORT_COMPOSING)
        fputs(" report-composing", out);
      break;
    case GRX_OP_LOOK: {
      size_t min = 0;
      size_t max = 0;
      int behind = inst->mode == GRX_LOOK_BEHIND_POSITIVE ||
          inst->mode == GRX_LOOK_BEHIND_NEGATIVE;
      fprintf(out, "%s next=%u", look_name(inst->mode), inst->y);
      if (behind && grx_program_look_span(program, inst->x, &min, &max)) {
        fprintf(out, " forward=%zu..%zu", min, max);
        if (inst->flags & GRX_INST_LOOK_NEAREST) {
          fputs(" nearest", out);
        }
      }
      size_t repeat_state = 0;
      size_t repeat_context = 0;
      int repeated = grx_program_look_repeat(
          program, inst, &repeat_state, &repeat_context, NULL, NULL);
      if (inst->policy & GRX_POLICY_LOOK_FRONTIER) {
        if (inst->policy & GRX_POLICY_LOOK_AFTER_ATOM) {
          const size_t * state = GRX_ARENA_AT(
              const size_t, &program->look_spans, (size_t)inst->x + 2);
          if (state) {
            fprintf(out, " state=r%zu after-atom", *state);
          }
        }
        else {
          fprintf(
              out, " state=r%zu", repeated ? repeat_state : (size_t)inst->x);
        }
        fputs(" frontier", out);
      }
      if (inst->policy & GRX_POLICY_LOOK_FRESH) {
        fputs(" fresh", out);
      }
      if (inst->policy & GRX_POLICY_LOOK_REPLACE_END) {
        fputs(" replace-end", out);
      }
      if (inst->policy & GRX_POLICY_LOOK_DELAYED) {
        fputs(" delayed", out);
      }
      if (inst->policy & GRX_POLICY_LOOK_HAS_CAPTURES) {
        fputs(" captures", out);
      }
      if (inst->policy & GRX_POLICY_LOOK_READS_CAPTURES) {
        fputs(" reads-captures", out);
      }
      if (inst->flags & GRX_INST_LOOK_REQUIRED) {
        fputs(" required-tail", out);
      }
      if ((inst->mode == GRX_LOOK_AHEAD_POSITIVE ||
              inst->mode == GRX_LOOK_AHEAD_NEGATIVE) &&
          (inst->flags & GRX_INST_LOOK_BODY_NULLABLE))
        fputs(" nullable-body", out);
      if (repeated) {
        size_t global = 0;
        size_t copy = 0;
        grx_program_look_repeat(
            program, inst, &repeat_state, &repeat_context, &global, &copy);
        fprintf(out, " repeat=r%zu global=r%zu marker=r%zu copy=%zu",
            repeat_state, global, global + 1, copy);
        if (repeat_context & GRX_LOOK_REPEAT_BOUNDED) {
          fputs(" bounded", out);
        }
        if (repeat_context & GRX_LOOK_REPEAT_FIRST) {
          fputs(" first", out);
        }
        if (repeat_context & GRX_LOOK_REPEAT_LAST) {
          fputs(" last", out);
        }
        if (repeat_context & GRX_LOOK_REPEAT_NULLABLE) {
          fputs(" nullable-body", out);
        }
        if (repeat_context & GRX_LOOK_REPEAT_EXIT_NULLABLE) {
          fputs(" nullable-exit", out);
        }
        if (repeat_context & GRX_LOOK_REPEAT_LITERAL_FRONTIER) {
          fputs(" literal-frontier", out);
        }
        if (repeat_context & GRX_LOOK_REPEAT_COPY_POLICY) {
          fputs((repeat_context & GRX_LOOK_REPEAT_COPY_DYNAMIC)
              ? " copy-activation-end"
              : (repeat_context & GRX_LOOK_REPEAT_COPY_KEEP_END)
              ? " copy-keeps-end" : " copy-expires-end", out);
        }
        if (repeat_context & GRX_LOOK_REPEAT_EXIT_REPEAT) {
          fputs(" following-repeat", out);
        }
      }
      break;
    }
    case GRX_OP_ATOMIC_BEGIN:
      fprintf(out, "end=%u", inst->x);
      if (inst->policy & GRX_POLICY_ATOMIC_REQUIRED) {
        fputs(" required", out);
      }
      if (inst->policy & GRX_POLICY_ATOMIC_FRONTIER) {
        fputs(" frontier-owner", out);
      }
      if (inst->policy & GRX_POLICY_ATOMIC_HAS_CAPTURES) {
        fputs(" captures", out);
      }
      if (inst->policy & GRX_POLICY_ATOMIC_READS_CAPTURES) {
        fputs(" reads-captures", out);
      }
      if (inst->policy & GRX_POLICY_ATOMIC_CONTEXT_CONTINUATION) {
        fputs(" continuation-context", out);
      }
      if (inst->policy & GRX_POLICY_ATOMIC_END_OWNER) {
        fputs(" end-context-owner", out);
      }
      break;
    case GRX_OP_ATOMIC_END:
      if (inst->policy & GRX_POLICY_COMPLETE_COMPOSING)
        fputs("complete-composing", out);
      break;
    case GRX_OP_KEEP_END:
      if (inst->policy & GRX_POLICY_END_CONTEXT_ONLY) {
        fputs("context-only", out);
      }
      break;
    case GRX_OP_COND:
      fprintf(out, "%s ", cond_name(inst->mode));
      if (inst->mode == GRX_COND_GROUP_SET) {
        dump_group_operand(out, program, inst);
      }
      else {
        fprintf(out, "#%u", inst->x);
      }
      fprintf(out, " else=%u", inst->y);
      break;
    case GRX_OP_CALL:
      fprintf(out, "target=%u next=%u", inst->x, inst->y);
      break;
    case GRX_OP_VERB:
      fputs(verb_name(inst->mode), out);
      break;
    case GRX_OP_SCRIPT_RUN:
      fprintf(out, "from=r%u", inst->x);
      break;
    case GRX_OP_CALLOUT: {
      const GRX_ProgramCallout * callout
          = grx_program_callout(program, inst->x);
      if (!callout) {
        fprintf(out, "#%u", inst->x);
      }
      else if (callout->string == GRX_INDEX_NONE) {
        fprintf(out, "number=%u at=%zu", callout->number,
            callout->pattern_offset);
      }
      else {
        const char * text
            = grx_program_callout_string(program, callout->string);
        fprintf(out, "string@%zu+%zu=%.*s at=%zu", callout->string_offset,
            callout->string_length, (int)callout->string_length,
            text ? text : "", callout->pattern_offset);
      }
      break;
    }
    default:
      break;
  }
}

/** The name of an iteration rule, for the dump header. */
static const char * iteration_name(GRX_IterationRule iteration) {
  static const char * const names[GRX_ITERATE_COUNT] = {
    "retry-then-advance", "advance-one", "advance-skip-abutting",
    "advance-one-stop-at-end"};
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
  if (program->preference == GRX_PREFER_LEFTMOST_LONGEST) {
    fprintf(out, "  submatch=%s\n", submatch_name(program->submatch));
  }

  if (program->candidate_literal_length) {
    fprintf(out, "  candidate-run bytes=%zu caseless=%u ignore-composing=%u\n",
        program->candidate_literal_length,
        !!(program->candidate_options & GRX_OPT_CASELESS),
        !!(program->candidate_options & GRX_OPT_IGNORE_COMBINING));
  }

  for (size_t i = 0; i < program->insts.count; i++) {
    const GRX_Inst * inst
        = GRX_ARENA_AT(const GRX_Inst, &program->insts, i);
    if (!inst) {
      break;
    }

    fprintf(out, "  %4zu  %-14s ", i, grx_opcode_name((GRX_Opcode)inst->op));
    dump_operands(out, program, inst);
    uint32_t scope = grx_program_scope(program, (uint32_t)i);
    if (scope)
      fprintf(out, " scope=%u", scope);
    if (inst->op == GRX_OP_SPLIT &&
        (inst->policy & GRX_POLICY_NULLABLE_BRANCH)) {
      fputs(" nullable", out);
    }
    if (inst->flags & GRX_INST_REVERSE) {
      fputs("  reverse", out);
    }
    if (inst->flags & GRX_INST_COND_ELSE) {
      // Without this the two jumps after the body read as dead weight.
      // With it, `next` names the first of the pair and the second is the
      // one below it.
      fputs("  condition", out);
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

int grx_program_look_repeat(const GRX_Program * program, const GRX_Inst * inst,
    size_t * out_state, size_t * out_context,
    size_t * out_global, size_t * out_copy) {
  if (!program || !inst || !out_state || !out_context ||
      inst->op != GRX_OP_LOOK || !(inst->policy & GRX_POLICY_LOOK_REPEATED)) {
    return 0;
  }
  size_t offset = inst->x;
  if (inst->mode == GRX_LOOK_BEHIND_POSITIVE ||
      inst->mode == GRX_LOOK_BEHIND_NEGATIVE) {
    offset += (inst->policy & GRX_POLICY_LOOK_AFTER_ATOM) ? 3 : 2;
  }
  if (offset >= program->look_spans.count ||
      program->look_spans.count - offset < 4) {
    return 0;
  }
  const size_t * pair =
      GRX_ARENA_AT(const size_t, &program->look_spans, offset);
  *out_state = pair[0];
  *out_context = pair[1];
  if (out_global) {
    *out_global = pair[2];
  }
  if (out_copy) {
    *out_copy = pair[3];
  }
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

const GRX_ProgramCallout * grx_program_callout(
    const GRX_Program * program, uint32_t index) {
  if (!program) {
    return NULL;
  }

  return GRX_ARENA_AT(const GRX_ProgramCallout, &program->callouts, index);
}

const char * grx_program_callout_string(
    const GRX_Program * program, uint32_t offset) {
  if (!program || offset == GRX_INDEX_NONE) {
    return NULL;
  }

  return GRX_ARENA_AT(const char, &program->callout_strings, offset);
}

void grx_program_clear(GRX_Program * program) {
  if (!program) {
    return;
  }

  grx_arena_clear(&program->insts);
  grx_class_table_clear(&program->classes);
  grx_arena_clear(&program->scan_lists);
  grx_arena_clear(&program->look_spans);
  grx_arena_clear(&program->scalar_scopes);
  grx_arena_clear(&program->callouts);
  grx_arena_clear(&program->callout_strings);
  program->register_count = 0;
}
