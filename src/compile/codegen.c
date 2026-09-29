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
 * Code generation: the intermediate representation to instructions.
 *
 * A Thompson construction with the loop guards the dialects need. Nothing
 * here knows what dialect produced the IR - it reads modes and class indices
 * and emits opcodes, and `make check-layering` would catch it if it did
 * otherwise.
 *
 * Two things are worth knowing before reading:
 *
 * **Counted repetition is expanded.** `a{3,5}` becomes five copies of `a`
 * with three of them optional, bounded by `max_program_size`. That is what
 * RE2 does, and it is what keeps the Pike VM free of counters - a counter
 * would be per-thread state that the lockstep simulation cannot merge. The
 * price is a documented deviation: ECMA-262's grammar admits a repeat count
 * of 2^53-1 and this library refuses one that would not fit the program.
 *
 * **A lookbehind body is emitted backwards.** Its instructions carry
 * GRX_INST_REVERSE and step back through the subject, and its concatenations
 * are emitted last-child-first, because a body running right to left meets
 * its parts in the opposite order. That is the whole of what makes a
 * lookbehind of arbitrary length work without a second engine, and it is the
 * approach the ECMAScript specification itself describes (22.2.2.4,
 * direction -1).
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/core_internal.h"
#include "../ir/lower_internal.h"
#include "compile_internal.h"

/** How many distinct groups one program may call as subroutines. */
#define GRX_CODEGEN_MAX_CALLED 256

/** grx_ir_span()'s answer about one IR node; see Codegen::node_span. */
typedef struct {
  size_t min;    ///< Shortest the subtree can match.
  size_t max;    ///< Longest it can match, or GRX_NPOS.
  uint8_t state; ///< 0 not asked, 1 answered, 2 asked and there is no answer.
} SpanMemo;

/** What one code-generation run carries. */
typedef struct {
  const GRX_IR * ir;         ///< What is being compiled.
  GRX_Program * program;     ///< What is being built.
  const GRX_Limits * limits; ///< Caps to apply.
  GRX_Error * error;         ///< Where a failure is reported.
  uint32_t registers;        ///< Progress registers handed out so far.
  int no_memo;               ///< An opcode the bit-state memo cannot survive.
  int simulated;             ///< A loop the walk ends rather than a guard.
  int has_keep_end;          ///< A `\ze` claimed the end; see the epilogue.
  /**
   * What is being emitted is the tail of a forward lookbehind body.
   *
   * Two claims at once, and the guard on an alternative needs both. Inside
   * such a body, "how far is it to the end of this assertion" has an answer;
   * at its *tail*, that distance is what the alternative alone has to span.
   * An alternative with something after it inside the body does not have to
   * span the distance on its own, and a guard that says otherwise throws
   * away a branch that would have matched - which is what
   * `(?<=([cd](*ACCEPT)|x)gggg)blrph` caught, where the `x` branch spans one
   * byte of the five the assertion needs and `gggg` spans the rest.
   *
   * Narrowed on the way down rather than raised: entering a body sets it,
   * and anything that can be followed by more of the same body clears it.
   */
  int forward_tail;
  /**
   * Generate the current subtree in the direction opposite to its flags.
   *
   * A subroutine block's instructions step the way its *definition* does,
   * and a group defined inside a reverse lookbehind can be called from a
   * forward context - `(*naplb:(a))(?1)`. One block cannot serve both, so
   * a block is generated per (group, definition, direction) and this says
   * which of the two is being laid out.
   *
   * It is an XOR on the way down rather than a rewrite of the flags,
   * because the flags are shared: the same IR nodes are read again for the
   * other copy. And it is *cleared* at every lookaround body, which is the
   * whole reason a flag rewrite would not do - a lookbehind written inside
   * the called group looks behind whatever encloses the call, so its body
   * keeps the direction lowering gave it while everything around it turns
   * over. Lookarounds are the only construct that sets a direction
   * absolutely (see lower.c's lookaround), so they are the only place this
   * is cleared.
   */
  int flip;
  /**
   * The pattern can read a capture back: it has a conditional or a
   * backreference somewhere.
   *
   * What decides between the two forms of GRX_CAPTURE_RESET_AFTER_EACH's
   * clearing. Clearing late needs a register, and a register is history the
   * bit-state memo's (pc, position) key does not carry - so it is only worth
   * having where the difference can be seen, and the difference can only be
   * seen by something that reads a capture back. A pattern with neither gets
   * the early form, which reports exactly the same spans for one instruction
   * less and keeps the program memoizable.
   *
   * Anything that can read a capture back already makes a program
   * unmemoizable on its own account, so the register costs nothing here.
   */
  int captures_are_read;
  /**
   * Where each called group's subroutine block starts, and which groups need
   * one.
   *
   * A recursion re-enters a group's code, and the group's code as it stands
   * in the main program runs on into whatever follows it - so a call cannot
   * jump there. Each called group gets a second copy, emitted after the main
   * program and ending in RET, and every CALL is patched to it once the copy
   * exists. Two copies of a group's instructions is the cost; sharing one
   * would mean the main program's path through the group had to end in a RET
   * it must not execute.
   */
  uint32_t called[GRX_CODEGEN_MAX_CALLED];   ///< Group numbers, in order.
  /**
   * Which definition of each, as the byte offset it was written at.
   *
   * A group number is not a program: a `(?|...)` gives one number a
   * definition per branch, and a call names one of them. So the key here is
   * the pair, and two calls to different definitions of the same number get
   * a block each rather than sharing the first one's.
   */
  uint32_t called_definition[GRX_CODEGEN_MAX_CALLED];
  /**
   * And which direction each block runs in, as the third of the key.
   *
   * A group defined inside a reverse lookbehind and called from outside it
   * needs a forward copy; called from inside another one, it needs the
   * backwards copy as well. Both are the same IR subtree laid out twice,
   * which is why this is a key and not a flag on the group.
   */
  uint32_t called_reverse[GRX_CODEGEN_MAX_CALLED];
  uint32_t entry[GRX_CODEGEN_MAX_CALLED];    ///< Their block starts.
  size_t called_count;
  /**
   * One entry per CALL instruction waiting for its block's address.
   *
   * An arena rather than an array, and capped by max_program_size rather
   * than by a constant of its own. There is one of these per CALL and a CALL
   * is an instruction, so the program's own cap already bounds them - and a
   * fixed array here made `(a)(?2){0,1999}?(b)` refuse with "program is
   * larger than max_program_size" over a program of four thousand
   * instructions and a cap of two hundred thousand. A limit that names
   * something the caller cannot change is worse than no limit.
   */
  GRX_Arena fixups;
  /**
   * The generator's own stack, one GenFrame per IR level being laid out.
   *
   * On the heap because a C stack runs out and this one grows; see GenFrame.
   * Uncapped for the same reason the fixups above are capped by
   * max_program_size rather than by a constant of their own - the depth here
   * is the tree's, which max_nodes and max_nesting_depth already bound, and a
   * second limit naming something the caller cannot change would refuse
   * patterns those two accept.
   */
  GRX_Arena frames;
  /** A subtree walk's stack, for capture_span(); uint32_t node indices. */
  GRX_Arena walk;
  /**
   * grx_ir_can_match_empty()'s answer per IR node: 0 unknown, 1 yes, 2 no.
   *
   * The question is a pure function of the subtree - the call builds a fresh
   * analysis with an empty resolving set and no lookbehind in progress - so an
   * answer is good for the whole compile. It was being asked afresh for every
   * repeat, and a counted repeat asks it again for every copy it expands, which
   * is how a 2,465-byte pattern spent 15.9 seconds being refused: 2,036 IR
   * nodes, 92 repeats, 125 backreferences, and each question re-resolving every
   * one of those references from scratch.
   *
   * The memo in src/ir/analyze.c (`cache_span`) does not help, and that is the
   * lesson worth keeping: it lives on the Analysis struct, and the Analysis is
   * built per call. It made one analysis cheap and left the number of analyses
   * alone - so the gate written with it measured a single compile of a single
   * pattern, which is the one case that was ever fixed.
   */
  GRX_Arena node_empty;
  /**
   * grx_ir_span()'s answer per IR node.
   *
   * The same story as node_empty above, one call site along. A branch's length
   * is a pure function of its subtree, and gen_length_guard() asks it of every
   * alternation branch inside a forward lookbehind body - which sits inside the
   * same expansion loops, so a branch under sixteen nested `{1,2}` repeats was
   * measured once per copy. Sixteen levels over a chain of forty
   * backreferences: 1.9 s asking per copy, 0.02 s asking per node.
   *
   * It was recorded rather than fixed when node_empty went in, because it
   * appeared in none of the stack samples that found that one. The shape is
   * identical, which is the whole reason to keep both memos in view: a pure
   * function called from inside the expansion is answered per node, and what
   * decides that is the memo's lifetime, not its existence.
   */
  GRX_Arena node_span;
  /**
   * One analysis memo for every question the two arenas above miss on.
   *
   * They bound how many questions are asked - once per IR node. This bounds
   * what each one costs: `grx_ir_can_match_empty()` and `grx_ir_span()` each
   * build a fresh analysis, and the analysis is where the group-span memo and
   * the capture index live, so 338 questions about one pattern built and threw
   * away 338 caches. NULL when it could not be allocated, and then the
   * one-shot forms are called, which is what happened before this existed.
   */
  GRX_IRMemo * memo;
} Codegen;

/** One CALL waiting to be pointed at the block for its target. */
typedef struct {
  uint32_t call;       ///< The CALL instruction's index.
  uint32_t group;      ///< The group it re-enters.
  uint32_t definition; ///< Which definition of it; see call_target().
  uint32_t reverse;    ///< Non-zero when the call is inside a reverse body.
} Fixup;

/**
 * One node being laid out, and how far through laying it out we are.
 *
 * This pass used to walk the IR by calling itself - about four C frames to a
 * level, with no bound of any kind - and a fuzz campaign found it as a stack
 * overflow inside grx_regex_compile() at 106 levels of nesting, with nothing
 * returned and nothing logged. A depth cap cannot be the fix:
 * tests/unit/test_stack.cpp requires 1920 levels to *compile*, which is
 * design.md section 9 invariant 6 read through max_nesting_depth, so a
 * constant would have to be at least 1920 to keep that contract and at most
 * 105 to stop the crash. There is no such number. The walk keeps its stack on
 * the heap instead, where depth costs bytes that grow rather than frames that
 * run out.
 *
 * So every generator below is resumable: it is entered with a stage number,
 * emits up to its next child, and says which child to lay out next. Its
 * locals live here rather than in a C frame, and the union is one member per
 * kind named for what that kind keeps - a row of numbered slots is how this
 * sort of rewrite goes wrong.
 */
typedef struct {
  uint32_t node;  ///< The IR node this frame lays out.
  uint16_t stage; ///< How far it has got; each generator numbers its own.
  /**
   * `forward_tail` and `flip` as they were when this frame started.
   *
   * The recursive form read these into locals, changed them for the duration
   * of a child, and put them back when the child returned. The driver does
   * exactly that - it restores both before resuming a frame - and a stage
   * that wants its child to see something else sets it again on the way out.
   * Restoring in one place is what stops a sibling being generated under the
   * previous child's direction, which is two dialects' lookbehind semantics.
   */
  int8_t saved_tail;
  int8_t saved_flip;
  union {
    /** CAPTURE's closing slot, ATOMIC's begin, SCRIPT_RUN's register. */
    struct {
      uint32_t slot;
    } one;
    /** CONCAT: where in the child list, and how long it is when reversed. */
    struct {
      uint32_t child;
      uint32_t next;
      uint32_t count;
    } list;
    /** ALTERNATE: the shared trampoline, and the branch being emitted. */
    struct {
      uint32_t child;
      uint32_t stub;
      uint32_t split;
    } alternate;
    /** REPEAT, by expansion and as a loop; see gen_repeat_step(). */
    struct {
      uint32_t body;
      uint32_t i;
      uint32_t stub;
      uint32_t reg;
      uint32_t late_reg;
      uint32_t pair_reg;
      uint32_t optional;
      uint32_t top;
      uint32_t split;
      uint32_t body_start;
      uint32_t enter;
      uint8_t lazy;
      uint8_t guard;
      uint8_t late;
      uint8_t empty_first;
      uint8_t clears_tail;
      uint8_t simulate;
    } repeat;
    /** COND and its assertion form: the jumps waiting for their targets. */
    struct {
      uint32_t test;
      uint32_t held;
      uint32_t missed;
      uint32_t skip;
      uint32_t yes;
      uint32_t no;
    } cond;
    /** LOOK and SCAN: the instruction to patch, and a rewind register. */
    struct {
      uint32_t mark;
      uint32_t reg;
    } look;
  } u;
} GenFrame;

/** What a stage asks the driver to do next. */
typedef enum {
  GEN_POP,     ///< This node is laid out.
  GEN_DESCEND, ///< Lay out the child named, then resume this frame.
  GEN_AGAIN    ///< Call this frame again; its stage has moved.
} GenAction;

/** Report a failure at a node's span. */
static GRX_Result fail(
    Codegen * codegen, GRX_Diag diag, const GRX_IRNode * node) {
  return grx_error_set(codegen->error, grx_diag_result(diag), diag,
      node ? node->offset : GRX_NPOS, node ? node->length : 0);
}

/** Whether a node runs right to left, in the copy being generated. */
static int reversed(const Codegen * codegen, const GRX_IRNode * node) {
  int flagged = (node->flags & GRX_IR_REVERSE) != 0;
  return codegen->flip ? !flagged : flagged;
}

/**
 * Append one instruction and return its index.
 *
 * Every emission goes through here, so the program-size cap is enforced in
 * one place - which matters because the repeat expansion below can ask for a
 * great many instructions from a very short pattern.
 */
static GRX_Result emit(Codegen * codegen, GRX_Opcode op, uint8_t mode,
    uint32_t x, uint32_t y, const GRX_IRNode * node, uint32_t * out_index) {
  switch (op) {
    case GRX_OP_KEEP:
    case GRX_OP_KEEP_END:
    case GRX_OP_VERB:
    case GRX_OP_SCAN:
    case GRX_OP_REWIND:
    case GRX_OP_COND:
    case GRX_OP_CALL:
    case GRX_OP_RET:
    case GRX_OP_ATOMIC_BEGIN:
    case GRX_OP_ATOMIC_END:
    // The memo skips an (instruction, position) pair already tried, which
    // assumes two arrivals there have the same future. A script run's
    // answer depends on where the *run* began, and two arrivals at its end
    // instruction at the same position can have begun in different places.
    // This is the same unsoundness the atomic group had, found once
    // already, and it is on this list for the same reason.
    case GRX_OP_SCRIPT_RUN:
      codegen->no_memo = 1;
      break;
    default:
      break;
  }

  GRX_Inst inst = {
    .op = (uint8_t)op,
    .mode = mode,
    .flags = (uint8_t)(
        ((node && reversed(codegen, node)) ? GRX_INST_REVERSE : 0u)
        | ((node && (node->flags & GRX_IR_LINE_ANCHOR))
            ? GRX_INST_LINE_ANCHOR : 0u)
        | ((node && (node->flags & GRX_IR_NEWLINE_CRLF))
            ? GRX_INST_NEWLINE_CRLF : 0u)),
    .reserved = 0,
    .x = x,
    .y = y,
  };

  GRX_Result result = grx_program_add(codegen->program, &inst, out_index);
  if (result == GRX_ERR_LIMIT) {
    return fail(codegen, GRX_DIAG_LIMIT_PROGRAM_SIZE, node);
  }
  if (result != GRX_OK) {
    return fail(codegen, GRX_DIAG_OUT_OF_MEMORY, node);
  }

  return GRX_OK;
}

/** The index the next instruction will get. */
static uint32_t here(const Codegen * codegen) {
  return (uint32_t)codegen->program->insts.count;
}

/** Fill in a jump target left blank when the instruction was emitted. */
static void patch_x(Codegen * codegen, uint32_t index, uint32_t target) {
  GRX_Inst * inst = grx_program_at(codegen->program, index);
  if (inst) {
    inst->x = target;
  }
}

static void patch_y(Codegen * codegen, uint32_t index, uint32_t target) {
  GRX_Inst * inst = grx_program_at(codegen->program, index);
  if (inst) {
    inst->y = target;
  }
}

/**
 * The span of capture slots a subtree writes, or an empty span.
 *
 * Group numbers within a subtree are contiguous, because a group is numbered
 * by where its opening parenthesis is and a subtree is a contiguous stretch
 * of the pattern. So "every capture inside this repeat" is two numbers, which
 * is what lets the reset be one instruction rather than a list.
 */
static GRX_Result capture_span(Codegen * codegen, uint32_t node_index,
    uint32_t * out_low, uint32_t * out_high) {
  // Its own explicit stack, for the reason GenFrame gives: this is a second
  // walk of the same tree, reached from inside the first one, and a frame per
  // level here would put back a per-level cost the generator has just taken
  // out. Children are pushed and popped in no particular order, which a
  // minimum and a maximum do not care about.
  size_t base = codegen->walk.count;
  GRX_Result result = grx_arena_append(&codegen->walk, &node_index, NULL);

  while (result == GRX_OK && codegen->walk.count > base) {
    const uint32_t * top
        = GRX_ARENA_AT(const uint32_t, &codegen->walk, codegen->walk.count - 1);
    if (!top) {
      result = GRX_ERR_INTERNAL;
      break;
    }
    uint32_t index = *top;
    codegen->walk.count--;

    const GRX_IRNode * node = grx_ir_node(codegen->ir, index);
    if (!node) {
      break; // As the recursive form did: abandon the walk, report no error.
    }

    if (node->kind == GRX_IR_CAPTURE) {
      if (node->a < *out_low) {
        *out_low = node->a;
      }
      if (node->a + 1 > *out_high) {
        *out_high = node->a + 1;
      }
    }

    for (uint32_t child = node->first_child; child != GRX_INDEX_NONE;) {
      const GRX_IRNode * child_node = grx_ir_node(codegen->ir, child);
      if (!child_node) {
        break;
      }
      result = grx_arena_append(&codegen->walk, &child, NULL);
      if (result != GRX_OK) {
        break;
      }
      child = child_node->next_sibling;
    }
  }

  codegen->walk.count = base;
  return result;
}

/**
 * Emit the capture reset for one iteration of a repeat, if the dialect wants
 * one and the body has any captures to clear.
 */
static GRX_Result emit_capture_reset(
    Codegen * codegen, const GRX_IRNode * node, uint32_t body) {
  if (node->capture_reset == GRX_CAPTURE_KEEP_LAST_SET
      || (node->capture_reset == GRX_CAPTURE_RESET_AFTER_EACH
          && codegen->captures_are_read)) {
    // KEEP_LAST_SET clears nothing at all; the late form clears at the end
    // of the iteration instead, in emit_capture_reset_late().
    return GRX_OK;
  }

  uint32_t low = GRX_INDEX_NONE;
  uint32_t high = 0;
  GRX_Result spanned = capture_span(codegen, body, &low, &high);
  if (spanned != GRX_OK) {
    return fail(codegen, GRX_DIAG_OUT_OF_MEMORY, node);
  }
  if (low == GRX_INDEX_NONE || high <= low) {
    return GRX_OK;
  }

  return emit(codegen, GRX_OP_RESET, 0, low * 2, high * 2, node, NULL);
}

/**
 * Whether this repeat clears its captures at the end of an iteration.
 *
 * The late form of the dialect's rule, and only where the difference can be
 * seen: see Codegen::captures_are_read.
 */
static int resets_captures_late(Codegen * codegen, const GRX_IRNode * node) {
  return node->capture_reset == GRX_CAPTURE_RESET_AFTER_EACH
      && codegen->captures_are_read;
}

/**
 * Emit the clearing an iteration does on its way out.
 *
 * One instruction per capture in the body: a group whose start is before
 * `reg`'s position was set by an earlier iteration, and this one is ending
 * without setting it. `reg` is the register a PROGRESS_SET filled at the
 * head of the same iteration.
 */
static GRX_Result emit_capture_reset_late(
    Codegen * codegen, const GRX_IRNode * node, uint32_t body, uint32_t reg) {
  if (!resets_captures_late(codegen, node)) {
    return GRX_OK;
  }

  uint32_t low = GRX_INDEX_NONE;
  uint32_t high = 0;
  GRX_Result spanned = capture_span(codegen, body, &low, &high);
  if (spanned != GRX_OK) {
    return fail(codegen, GRX_DIAG_OUT_OF_MEMORY, node);
  }
  if (low == GRX_INDEX_NONE || high <= low) {
    return GRX_OK;
  }

  for (uint32_t group = low; group < high; group++) {
    GRX_Result result
        = emit(codegen, GRX_OP_RESET_STALE, 0, reg, group * 2, node, NULL);
    if (result != GRX_OK) {
      return result;
    }
  }
  return GRX_OK;
}

/**
 * Whether a subtree can match the empty string, asked at most once per node.
 *
 * A cache miss falls through to the real answer and a cache that could not be
 * built falls through every time, so this is a memo and never a second opinion.
 */
static int can_match_empty(Codegen * codegen, uint32_t node_index) {
  unsigned char * slot = node_index == GRX_INDEX_NONE
      ? NULL
      : GRX_ARENA_AT(unsigned char, &codegen->node_empty, node_index);
  if (slot && *slot) {
    return *slot == 1;
  }
  int answer = codegen->memo
      ? grx_ir_memo_can_match_empty(codegen->memo, node_index)
      : grx_ir_can_match_empty(codegen->ir, node_index);
  if (slot) {
    *slot = answer ? 1u : 2u;
  }
  return answer;
}

/**
 * Whether this repeat is one the simulation shapes rather than guards.
 *
 * GRX_EMPTY_LOOP_SIMULATE is a claim about the program, so it is asked here
 * and only here, and it is asked about the *unbounded* form: a counted
 * repeat is copies with no back edge, so it has no state to arrive at twice
 * and needs neither the guard nor the shape. The body still has to be able
 * to match empty, because a body that cannot is already emitted as the
 * reference emits it and changing it would be churn with a differential
 * attached.
 */
static int simulated_loop(Codegen * codegen, const GRX_IRNode * node,
    uint32_t body) {
  return node->empty_loop == GRX_EMPTY_LOOP_SIMULATE
      && node->max == GRX_REPEAT_INF && can_match_empty(codegen, body);
}

/**
 * How long a subtree can be, asked at most once per node.
 *
 * "There is no answer" is an answer worth remembering too: a branch whose
 * length nobody can compute is the expensive case to re-ask, because the walk
 * that fails to compute it is the walk that covered the whole subtree.
 */
static int span_of(Codegen * codegen, uint32_t node_index, size_t * out_min,
    size_t * out_max) {
  SpanMemo * slot = node_index == GRX_INDEX_NONE
      ? NULL
      : GRX_ARENA_AT(SpanMemo, &codegen->node_span, node_index);
  if (slot && slot->state) {
    if (slot->state != 1) {
      return 0;
    }
    *out_min = slot->min;
    *out_max = slot->max;
    return 1;
  }

  int answer = codegen->memo
      ? grx_ir_memo_span(codegen->memo, node_index, out_min, out_max)
      : grx_ir_span(codegen->ir, node_index, out_min, out_max);
  if (slot) {
    slot->state = answer ? 1u : 2u;
    slot->min = answer ? *out_min : 0;
    slot->max = answer ? *out_max : 0;
  }
  return answer;
}

/** Generate a concatenation, in reading order or against it. */
static GRX_Result gen_concat_step(Codegen * codegen, GenFrame * frame,
    const GRX_IRNode * node, GenAction * action, uint32_t * out_child) {
  switch (frame->stage) {
    case 0:
      frame->u.list.child = node->first_child;
      frame->stage = reversed(codegen, node) ? 10 : 1;
      *action = GEN_AGAIN;
      return GRX_OK;

    // In reading order.
    case 1: {
      uint32_t child = frame->u.list.child;
      if (child == GRX_INDEX_NONE) {
        return GRX_OK;
      }
      const GRX_IRNode * child_node = grx_ir_node(codegen->ir, child);
      if (!child_node) {
        return fail(codegen, GRX_DIAG_INTERNAL, node);
      }
      frame->u.list.next = child_node->next_sibling;
      if (frame->u.list.next != GRX_INDEX_NONE) {
        codegen->forward_tail = 0;
      }
      frame->stage = 2;
      *action = GEN_DESCEND;
      *out_child = child;
      return GRX_OK;
    }

    case 2:
      frame->u.list.child = frame->u.list.next;
      frame->stage = 1;
      *action = GEN_AGAIN;
      return GRX_OK;

    // Backwards. The child list is singly linked, so the order is recovered
    // by counting and then walking to the nth child - which is quadratic in
    // the number of children and confined to lookbehind bodies, where the
    // count is small and the alternative is a second link field on every node
    // in every pattern.
    case 10: {
      uint32_t count = 0;
      for (uint32_t child = node->first_child; child != GRX_INDEX_NONE;) {
        const GRX_IRNode * child_node = grx_ir_node(codegen->ir, child);
        if (!child_node) {
          return fail(codegen, GRX_DIAG_INTERNAL, node);
        }
        count++;
        child = child_node->next_sibling;
      }
      frame->u.list.count = count;
      frame->stage = 11;
      *action = GEN_AGAIN;
      return GRX_OK;
    }

    // `count` counts down from here: it is the `i` of the loop this was.
    case 11: {
      uint32_t i = frame->u.list.count;
      if (!i) {
        return GRX_OK;
      }
      uint32_t child = node->first_child;
      for (uint32_t step = 1; step < i; step++) {
        const GRX_IRNode * child_node = grx_ir_node(codegen->ir, child);
        if (!child_node) {
          return fail(codegen, GRX_DIAG_INTERNAL, node);
        }
        child = child_node->next_sibling;
      }
      frame->stage = 12;
      *action = GEN_DESCEND;
      *out_child = child;
      return GRX_OK;
    }

    case 12:
      frame->u.list.count--;
      frame->stage = 11;
      *action = GEN_AGAIN;
      return GRX_OK;

    default:
      return fail(codegen, GRX_DIAG_INTERNAL, node);
  }
}

/**
 * Emit a "jump to somewhere decided later" that many instructions can share.
 *
 * Both the alternation and the bounded repeat need a crowd of branches to
 * land on one place that is not known until the construct ends. Giving each
 * of them its own patch means keeping a list of them, and a list means a
 * bound on how many there may be - which `a{0,70000}` exceeds while still
 * fitting `max_program_size`. One shared trampoline costs two instructions
 * and has no bound.
 *
 * Emits `jmp past`, then the stub itself, and leaves the caller positioned
 * after both. `out_stub` is patched once, at the end.
 */
static GRX_Result emit_trampoline(
    Codegen * codegen, const GRX_IRNode * node, uint32_t * out_stub) {
  uint32_t skip = GRX_INDEX_NONE;
  GRX_Result result = emit(codegen, GRX_OP_JMP, 0, 0, 0, node, &skip);
  if (result == GRX_OK) {
    result = emit(codegen, GRX_OP_JMP, 0, 0, 0, node, out_stub);
  }
  if (result != GRX_OK) {
    return result;
  }

  patch_x(codegen, skip, here(codegen));
  return GRX_OK;
}

/**
 * Generate an alternation.
 *
 * Each branch but the last gets a SPLIT preferring it, so the thread order
 * is the pattern's order - which is what makes the result leftmost-*first*.
 * Each branch but the last ends by jumping to the shared trampoline.
 */
/**
 * Emit "there are between min and max bytes left", for one alternative.
 *
 * At the tail of a forward lookbehind body only, where the distance to the
 * end of the assertion is exactly what this alternative has to span, and one
 * that cannot is one to skip rather than to try and undo. Perl prunes a
 * variable lookbehind the same way, and without this `(?<=(a|aa|aaa))b`
 * costs every branch from every candidate start instead of the one that
 * fits.
 *
 * Emitted only where it prunes: a branch that could be any length at all
 * rules nothing out, and a guard that never fires is two instructions and a
 * step per iteration for nothing.
 */
static GRX_Result gen_length_guard(
    Codegen * codegen, const GRX_IRNode * node, uint32_t branch) {
  size_t min = 0;
  size_t max = 0;
  if (!codegen->forward_tail
      || !span_of(codegen, branch, &min, &max)
      || (min == 0 && max == GRX_NPOS)) {
    return GRX_OK;
  }

  uint32_t offset = (uint32_t)codegen->program->look_spans.count;
  if (grx_arena_append(&codegen->program->look_spans, &min, NULL) != GRX_OK
      || grx_arena_append(&codegen->program->look_spans, &max, NULL)
          != GRX_OK) {
    return fail(codegen, GRX_DIAG_OUT_OF_MEMORY, node);
  }
  return emit(
      codegen, GRX_OP_ASSERT, GRX_ASSERT_LOOK_LENGTH, offset, 0, node, NULL);
}

static GRX_Result gen_alternate_step(Codegen * codegen, GenFrame * frame,
    const GRX_IRNode * node, GenAction * action, uint32_t * out_child) {
  switch (frame->stage) {
    case 0: {
      uint32_t child = node->first_child;
      if (child == GRX_INDEX_NONE) {
        return GRX_OK;
      }
      const GRX_IRNode * first = grx_ir_node(codegen->ir, child);
      if (first && first->next_sibling == GRX_INDEX_NONE) {
        frame->stage = 5; // One branch is not a choice.
        *action = GEN_DESCEND;
        *out_child = child;
        return GRX_OK;
      }

      uint32_t stub = GRX_INDEX_NONE;
      GRX_Result result = emit_trampoline(codegen, node, &stub);
      if (result != GRX_OK) {
        return result;
      }
      frame->u.alternate.stub = stub;
      frame->u.alternate.child = child;
      frame->stage = 1;
      *action = GEN_AGAIN;
      return GRX_OK;
    }

    case 1: {
      uint32_t child = frame->u.alternate.child;
      const GRX_IRNode * child_node = grx_ir_node(codegen->ir, child);
      if (!child_node) {
        return fail(codegen, GRX_DIAG_INTERNAL, node);
      }

      if (child_node->next_sibling == GRX_INDEX_NONE) {
        GRX_Result result = gen_length_guard(codegen, node, child);
        if (result != GRX_OK) {
          return result;
        }
        frame->stage = 3;
        *action = GEN_DESCEND;
        *out_child = child;
        return GRX_OK;
      }

      uint32_t split = GRX_INDEX_NONE;
      GRX_Result result = emit(codegen, GRX_OP_SPLIT, 0, 0, 0, node, &split);
      if (result != GRX_OK) {
        return result;
      }
      // Marked, so that `(*THEN)` can tell this SPLIT from a quantifier's.
      GRX_Inst * marked = grx_program_at(codegen->program, split);
      if (marked) {
        marked->flags |= GRX_INST_ALTERNATION;
      }
      patch_x(codegen, split, here(codegen));

      result = gen_length_guard(codegen, node, child);
      if (result != GRX_OK) {
        return result;
      }
      frame->u.alternate.split = split;
      frame->stage = 2;
      *action = GEN_DESCEND;
      *out_child = child;
      return GRX_OK;
    }

    case 2: {
      const GRX_IRNode * child_node
          = grx_ir_node(codegen->ir, frame->u.alternate.child);
      if (!child_node) {
        return fail(codegen, GRX_DIAG_INTERNAL, node);
      }
      GRX_Result result = emit(
          codegen, GRX_OP_JMP, 0, frame->u.alternate.stub, 0, node, NULL);
      if (result != GRX_OK) {
        return result;
      }
      patch_y(codegen, frame->u.alternate.split, here(codegen));
      frame->u.alternate.child = child_node->next_sibling;
      frame->stage = 1;
      *action = GEN_AGAIN;
      return GRX_OK;
    }

    case 3:
      patch_x(codegen, frame->u.alternate.stub, here(codegen));
      return GRX_OK;

    case 5:
      return GRX_OK;

    default:
      return fail(codegen, GRX_DIAG_INTERNAL, node);
  }
}

/**
 * Generate a full-fold run: the folded string as a graph of class matches.
 *
 * The node holds positions 0..`b` of the folded string and the edges over
 * them (GRX_IR_FOLD_RUN). One block of instructions per position, in the
 * order they are walked, and every jump between them is forward - so the
 * blocks are emitted in one pass and the jumps patched afterwards.
 *
 * A block with several edges leaving it is a SPLIT chain, the way an
 * alternation is. The arms are dead ends rather than real alternatives,
 * because the classes leaving a position are disjoint and at most one of
 * them can match - but saying so with a SPLIT costs nothing and needs no
 * instruction that means "and nothing else can follow".
 *
 * Reversed, the walk runs from the last position to the first and a block
 * holds the edges arriving at its position rather than the ones leaving it.
 * The class instructions carry GRX_INST_REVERSE from the node, so what
 * changes here is only which block comes after which.
 */
static GRX_Result gen_fold_run(Codegen * codegen, const GRX_IRNode * node) {
  size_t states = (size_t)node->b + 1;
  size_t edge_count = grx_ir_fold_run_count(codegen->ir, node->a);
  if (!edge_count) {
    return fail(codegen, GRX_DIAG_INTERNAL, node);
  }

  const GRX_Allocator * allocator = codegen->program->insts.allocator;
  // Where each position's block starts, and the first edge leaving each
  // position. Both are indexed by position and both are scaffolding, so
  // they are arenas with the program's allocator rather than anything the
  // program keeps.
  GRX_Arena block;
  GRX_Arena first_edge;
  GRX_Arena fixups;
  grx_arena_init(&block, allocator, sizeof(uint32_t), 0, GRX_DIAG_NONE);
  grx_arena_init(&first_edge, allocator, sizeof(uint32_t), 0, GRX_DIAG_NONE);
  grx_arena_init(&fixups, allocator, sizeof(uint32_t), 0, GRX_DIAG_NONE);

  GRX_Result result = GRX_OK;
  for (size_t i = 0; i < states && result == GRX_OK; i++) {
    uint32_t none = GRX_INDEX_NONE;
    uint32_t end = (uint32_t)edge_count;
    result = grx_arena_append(&block, &none, NULL);
    if (result == GRX_OK) {
      result = grx_arena_append(&first_edge, &end, NULL);
    }
  }

  // The edges arrive ordered by the position they leave, so one pass fills
  // the index and a later lookup is a walk from it rather than a search.
  for (size_t i = edge_count; i > 0 && result == GRX_OK; i--) {
    GRX_IRFoldEdge edge;
    if (!grx_ir_fold_run_edge(codegen->ir, node->a, i - 1, &edge)
        || edge.from >= states) {
      result = GRX_ERR_INTERNAL;
      break;
    }
    uint32_t * slot = GRX_ARENA_AT(uint32_t, &first_edge, edge.from);
    *slot = (uint32_t)(i - 1);
  }

  int reverse = reversed(codegen, node);
  for (size_t step = 0; step < node->b && result == GRX_OK; step++) {
    // Forwards the walk leaves position `step`; backwards it arrives at
    // position `b - step`, and the edges it may take are the ones that end
    // there.
    size_t at = reverse ? node->b - step : step;
    uint32_t * start = GRX_ARENA_AT(uint32_t, &block, at);
    *start = here(codegen);

    // The edges this block chooses between, gathered before any of them is
    // emitted: a reversed block's come from up to three earlier positions.
    size_t taken[GRX_FULL_FOLD_MAX];
    size_t count = 0;
    if (!reverse) {
      const uint32_t * from = GRX_ARENA_AT(const uint32_t, &first_edge, at);
      for (size_t i = *from; i < edge_count && count < GRX_FULL_FOLD_MAX; i++) {
        GRX_IRFoldEdge edge;
        if (!grx_ir_fold_run_edge(codegen->ir, node->a, i, &edge)
            || edge.from != at) {
          break;
        }
        taken[count++] = i;
      }
    }
    else {
      for (size_t span = 1; span <= GRX_FULL_FOLD_MAX && span <= at; span++) {
        const uint32_t * from
            = GRX_ARENA_AT(const uint32_t, &first_edge, at - span);
        for (size_t i = *from; i < edge_count; i++) {
          GRX_IRFoldEdge edge;
          if (!grx_ir_fold_run_edge(codegen->ir, node->a, i, &edge)
              || edge.from != at - span) {
            break;
          }
          if (edge.to == at && count < GRX_FULL_FOLD_MAX) {
            taken[count++] = i;
          }
        }
      }
    }
    if (!count) {
      result = GRX_ERR_INTERNAL;
      break;
    }

    for (size_t i = 0; i < count && result == GRX_OK; i++) {
      GRX_IRFoldEdge edge;
      if (!grx_ir_fold_run_edge(codegen->ir, node->a, taken[i], &edge)) {
        result = GRX_ERR_INTERNAL;
        break;
      }

      uint32_t split = GRX_INDEX_NONE;
      if (i + 1 < count) {
        result = emit(codegen, GRX_OP_SPLIT, 0, 0, 0, node, &split);
        if (result != GRX_OK) {
          break;
        }
        patch_x(codegen, split, here(codegen));
      }

      result = emit(
          codegen, GRX_OP_CLASS, 0, edge.class_index, 0, node, NULL);
      uint32_t jump = GRX_INDEX_NONE;
      if (result == GRX_OK) {
        result = emit(codegen, GRX_OP_JMP, 0, 0, 0, node, &jump);
      }
      if (result == GRX_OK) {
        uint32_t target = reverse ? edge.from : edge.to;
        result = grx_arena_append(&fixups, &jump, NULL);
        if (result == GRX_OK) {
          result = grx_arena_append(&fixups, &target, NULL);
        }
      }
      if (result != GRX_OK) {
        break;
      }

      if (split != GRX_INDEX_NONE) {
        patch_y(codegen, split, here(codegen));
      }
    }
  }

  if (result == GRX_OK) {
    // The position the walk ends at is where the run's instructions stop,
    // and the only block that is not emitted: there is nothing to consume
    // there.
    uint32_t * done = GRX_ARENA_AT(uint32_t, &block, reverse ? 0 : node->b);
    *done = here(codegen);

    for (size_t i = 0; i + 1 < fixups.count; i += 2) {
      const uint32_t * jump = GRX_ARENA_AT(const uint32_t, &fixups, i);
      const uint32_t * target = GRX_ARENA_AT(const uint32_t, &fixups, i + 1);
      const uint32_t * to = GRX_ARENA_AT(const uint32_t, &block, *target);
      if (!jump || !target || !to || *to == GRX_INDEX_NONE) {
        result = GRX_ERR_INTERNAL;
        break;
      }
      patch_x(codegen, *jump, *to);
    }
  }

  grx_arena_clear(&block);
  grx_arena_clear(&first_edge);
  grx_arena_clear(&fixups);
  if (result == GRX_ERR_INTERNAL) {
    return fail(codegen, GRX_DIAG_INTERNAL, node);
  }
  if (result != GRX_OK) {
    return fail(codegen, GRX_DIAG_OUT_OF_MEMORY, node);
  }
  return GRX_OK;
}

/**
 * Generate a repetition: the mandatory copies, then a loop or an expansion.
 *
 * A counted repetition is laid out by expansion:
 *
 *     <body> * min
 *     then, for each optional copy:  split body, trampoline
 *
 * The optional copies chain rather than nest, with every exit landing on one
 * trampoline, so `a{0,3}` can stop after any number of them and the number of
 * them is bounded only by `max_program_size`.
 *
 * An unbounded one is a loop instead - stages 10 and 11, which is what
 * gen_star() was:
 *
 *     top:   split body, exit        (arms swapped when lazy)
 *     body:  progress_set r
 *            <body>
 *            progress_check r, mode -> exit
 *            jmp top
 *     exit:
 *
 * The two progress instructions are what make the dialect's empty-iteration
 * rule an engine mode rather than a rewrite. Without them, `(a*)*` against
 * "b" loops forever in a backtracker and reports the wrong capture in a Pike
 * VM; with them, ECMAScript's "an iteration that consumed nothing fails" and
 * Perl's "it succeeds and the loop stops" are one instruction with two modes.
 *
 * The three functions this was - gen_repeat(), gen_repeat_body() and
 * gen_star() - are one frame here because they were never three levels of
 * anything: each called the next once, at its end, so they cost three C
 * frames per IR level to express one decision. Which is a third of the four
 * frames a level cost, and the reason a level now costs none.
 */
static GRX_Result gen_repeat_step(Codegen * codegen, GenFrame * frame,
    const GRX_IRNode * node, GenAction * action, uint32_t * out_child) {
  switch (frame->stage) {
    case 0: {
      uint32_t body = node->first_child;
      if (body == GRX_INDEX_NONE) {
        return fail(codegen, GRX_DIAG_INTERNAL, node);
      }
      frame->u.repeat.body = body;
      frame->u.repeat.lazy = (node->mode == GRX_REPEAT_LAZY) ? 1u : 0u;
      // One iteration can be followed by another, so nothing inside a repeat
      // is the tail of the body - except a repeat of exactly one, which is
      // not a loop at all. `(?<=(a|aa){2})` is the case: the first `a|aa` has
      // a second one after it, and a guard there would ask one iteration to
      // span both. Re-applied before every descent below rather than set
      // once, because the driver puts `forward_tail` back between children.
      frame->u.repeat.clears_tail
          = (node->min != 1 || node->max != 1) ? 1u : 0u;
      // The mandatory copies need the register too: `((?(2)x|y)(a)){2}` is
      // the same question as the unbounded form, asked twice.
      frame->u.repeat.late = resets_captures_late(codegen, node) ? 1u : 0u;
      frame->u.repeat.late_reg
          = frame->u.repeat.late ? codegen->registers++ : 0;
      // The simulation's shape puts the loop's one copy of the body at the
      // foot of the mandatory run rather than after it, so stage 1 emits one
      // copy fewer and stage 12 emits the one it held back. `(a*)+` is a
      // body and a split either way; what moves is where the split is.
      frame->u.repeat.simulate
          = simulated_loop(codegen, node, body) ? 1u : 0u;
      if (frame->u.repeat.simulate) {
        codegen->simulated = 1;
      }
      frame->u.repeat.enter = GRX_INDEX_NONE;

      /*
       * GRX_EMPTY_LOOP_BREAK_FIRST needs a second number: the position the
       * whole repeat began at, as against the position this iteration began
       * at.
       *
       * "The whole repeat" and not "the optional tail", which is the
       * distinction `(b+|(c)*)+` against "b" turns on. Its mandatory copy
       * consumes the `b`, and if the tail counted as the loop then its first
       * iteration would be the loop's first and would take the empty
       * alternative - reporting group 1 as 1-1 where glibc and musl both say
       * 0-1. So the register is set here, before the mandatory copies run,
       * and the empty iteration is allowed only while the repeat as a whole
       * has consumed nothing.
       *
       * The pair is allocated together and used as `reg` and `reg - 1`,
       * because GRX_Inst has two operands and the check has spent both.
       * Allocating them here is what makes them adjacent: a nested loop
       * inside a mandatory copy would otherwise take a number between them.
       */
      frame->u.repeat.empty_first
          = (node->empty_loop == GRX_EMPTY_LOOP_BREAK_FIRST
                && can_match_empty(codegen, body))
          ? 1u
          : 0u;
      frame->u.repeat.pair_reg = 0;
      frame->u.repeat.i = 0;
      frame->stage = 1;
      *action = GEN_AGAIN;
      if (frame->u.repeat.empty_first) {
        codegen->registers++;             // the entry register, at reg - 1
        frame->u.repeat.pair_reg = codegen->registers++;
        return emit(codegen, GRX_OP_PROGRESS_SET, 0,
            frame->u.repeat.pair_reg - 1, 0, node, NULL);
      }
      return GRX_OK;
    }

    // One mandatory copy per iteration of this stage pair.
    case 1: {
      uint32_t mandatory = frame->u.repeat.simulate && node->min
          ? node->min - 1u
          : node->min;
      if (frame->u.repeat.i >= mandatory) {
        frame->stage = 3;
        *action = GEN_AGAIN;
        return GRX_OK;
      }
      GRX_Result result = GRX_OK;
      if (frame->u.repeat.late) {
        result = emit(codegen, GRX_OP_PROGRESS_SET, 0,
            frame->u.repeat.late_reg, 0, node, NULL);
      }
      if (result == GRX_OK) {
        result = emit_capture_reset(codegen, node, frame->u.repeat.body);
      }
      if (result != GRX_OK) {
        return result;
      }
      if (frame->u.repeat.clears_tail) {
        codegen->forward_tail = 0;
      }
      frame->stage = 2;
      *action = GEN_DESCEND;
      *out_child = frame->u.repeat.body;
      return GRX_OK;
    }

    case 2: {
      GRX_Result result = emit_capture_reset_late(
          codegen, node, frame->u.repeat.body, frame->u.repeat.late_reg);
      if (result != GRX_OK) {
        return result;
      }
      frame->u.repeat.i++;
      frame->stage = 1;
      *action = GEN_AGAIN;
      return GRX_OK;
    }

    // The mandatory copies are done. Either a loop, or the optional copies.
    case 3: {
      if (node->max == GRX_REPEAT_INF) {
        frame->stage = frame->u.repeat.simulate ? 12 : 10;
        *action = GEN_AGAIN;
        return GRX_OK;
      }

      uint32_t optional = node->max - node->min;
      if (!optional) {
        return GRX_OK;
      }

      uint32_t stub = GRX_INDEX_NONE;
      GRX_Result result = emit_trampoline(codegen, node, &stub);
      if (result != GRX_OK) {
        return result;
      }
      frame->u.repeat.stub = stub;
      frame->u.repeat.optional = optional;

      // One register for the whole expansion. Each copy sets it immediately
      // before its body and checks it immediately after, and a thread meets
      // those in that order, so the copies cannot tread on each other. A body
      // that cannot match empty needs none of it; see stage 10.
      //
      // A counted repeat is copies with no back edge, so GRX_EMPTY_LOOP_
      // SIMULATE needs nothing here: there is no state to reach twice, and
      // the register it would cost is what would make the program
      // unmemoizable. The mode's two references agree with this across every
      // `{n,m}` in tools/oracle/linear_diff.py's shape battery.
      frame->u.repeat.guard
          = (node->empty_loop != GRX_EMPTY_LOOP_SIMULATE
                && can_match_empty(codegen, frame->u.repeat.body))
          ? 1u
          : 0u;
      frame->u.repeat.reg = frame->u.repeat.empty_first
          ? frame->u.repeat.pair_reg
          : frame->u.repeat.guard ? codegen->registers++
                                  : frame->u.repeat.late_reg;
      frame->u.repeat.i = 0;
      frame->stage = 4;
      *action = GEN_AGAIN;
      return GRX_OK;
    }

    case 4: {
      if (frame->u.repeat.i >= frame->u.repeat.optional) {
        frame->stage = 6;
        *action = GEN_AGAIN;
        return GRX_OK;
      }

      uint32_t split = GRX_INDEX_NONE;
      GRX_Result result = emit(codegen, GRX_OP_SPLIT, 0, 0, 0, node, &split);
      if (result != GRX_OK) {
        return result;
      }
      if (frame->u.repeat.lazy) {
        patch_x(codegen, split, frame->u.repeat.stub);
        patch_y(codegen, split, here(codegen));
      }
      else {
        patch_x(codegen, split, here(codegen));
        patch_y(codegen, split, frame->u.repeat.stub);
      }

      // The minimum is satisfied by the time an optional copy runs, so the
      // dialect's empty-iteration rule applies to it exactly as it applies to
      // an unbounded loop. ECMA-262 says so by passing min = 0 into the
      // remaining RepeatMatcher; `(a|){1,2}` against "ab" is what tells the
      // two apart, and the guard is why group 1 comes out as "a" rather than
      // as the empty string the second iteration would have set it to.
      result = GRX_OK;
      if (frame->u.repeat.guard || frame->u.repeat.late) {
        result = emit(codegen, GRX_OP_PROGRESS_SET, 0, frame->u.repeat.reg, 0,
            node, NULL);
      }
      if (result == GRX_OK) {
        result = emit_capture_reset(codegen, node, frame->u.repeat.body);
      }
      if (result != GRX_OK) {
        return result;
      }
      if (frame->u.repeat.clears_tail) {
        codegen->forward_tail = 0;
      }
      frame->stage = 5;
      *action = GEN_DESCEND;
      *out_child = frame->u.repeat.body;
      return GRX_OK;
    }

    case 5: {
      GRX_Result result = emit_capture_reset_late(
          codegen, node, frame->u.repeat.body, frame->u.repeat.reg);
      if (result == GRX_OK && frame->u.repeat.guard) {
        result = emit(codegen, GRX_OP_PROGRESS_CHECK, node->empty_loop,
            frame->u.repeat.reg, frame->u.repeat.stub, node, NULL);
      }
      if (result != GRX_OK) {
        return result;
      }
      frame->u.repeat.i++;
      frame->stage = 4;
      *action = GEN_AGAIN;
      return GRX_OK;
    }

    case 6:
      patch_x(codegen, frame->u.repeat.stub, here(codegen));
      return GRX_OK;

    // The unbounded form: one copy of the body, inside a loop.
    case 10: {
      // A body that cannot match the empty string cannot stall, so it needs
      // no guard. Emitting one anyway would be two dead instructions per loop
      // and - the reason this is a decision rather than a tidy-up - would
      // make the program unmemoizable: a progress register is history the
      // bit-state engine's (pc, position) key does not capture.
      //
      // A body that *can* and whose mode is GRX_EMPTY_LOOP_SIMULATE never
      // arrives here at all; stage 3 sent it to stage 12.
      frame->u.repeat.guard
          = can_match_empty(codegen, frame->u.repeat.body) ? 1u
                                                                     : 0u;
      // The late capture reset wants the same number the guard wants - where
      // this iteration began - so one register serves both, and a loop that
      // needs only the reset still gets one. `empty_first` hands its own
      // pair's second register down instead, having had to allocate the pair
      // before the mandatory copies; see stage 0.
      frame->u.repeat.reg = frame->u.repeat.empty_first
          ? frame->u.repeat.pair_reg
          : (frame->u.repeat.guard || frame->u.repeat.late)
              ? codegen->registers++
              : 0;

      frame->u.repeat.top = here(codegen);
      uint32_t split = GRX_INDEX_NONE;
      GRX_Result result = emit(codegen, GRX_OP_SPLIT, 0, 0, 0, node, &split);
      if (result != GRX_OK) {
        return result;
      }
      frame->u.repeat.split = split;
      frame->u.repeat.body_start = here(codegen);

      result = GRX_OK;
      if (frame->u.repeat.guard || frame->u.repeat.late) {
        result = emit(codegen, GRX_OP_PROGRESS_SET, 0, frame->u.repeat.reg, 0,
            node, NULL);
      }
      if (result == GRX_OK) {
        result = emit_capture_reset(codegen, node, frame->u.repeat.body);
      }
      if (result != GRX_OK) {
        return result;
      }
      if (frame->u.repeat.clears_tail) {
        codegen->forward_tail = 0;
      }
      frame->stage = 11;
      *action = GEN_DESCEND;
      *out_child = frame->u.repeat.body;
      return GRX_OK;
    }

    case 11: {
      // Before the check, because the check is what leaves the loop: an
      // iteration that ends by exiting has still ended.
      GRX_Result result = emit_capture_reset_late(
          codegen, node, frame->u.repeat.body, frame->u.repeat.reg);
      uint32_t check = GRX_INDEX_NONE;
      if (result == GRX_OK && frame->u.repeat.guard) {
        result = emit(codegen, GRX_OP_PROGRESS_CHECK, node->empty_loop,
            frame->u.repeat.reg, 0, node, &check);
      }
      if (result == GRX_OK) {
        result = emit(
            codegen, GRX_OP_JMP, 0, frame->u.repeat.top, 0, node, NULL);
      }
      if (result != GRX_OK) {
        return result;
      }

      uint32_t exit_target = here(codegen);
      if (frame->u.repeat.guard) {
        patch_y(codegen, check, exit_target);
      }
      if (frame->u.repeat.lazy) {
        patch_x(codegen, frame->u.repeat.split, exit_target);
        patch_y(codegen, frame->u.repeat.split, frame->u.repeat.body_start);
      }
      else {
        patch_x(codegen, frame->u.repeat.split, frame->u.repeat.body_start);
        patch_y(codegen, frame->u.repeat.split, exit_target);
      }
      return GRX_OK;
    }

    /*
     * The simulation's form: `(e+)?`, one copy of the body with the split at
     * its foot.
     *
     *     enter: split body, exit          (only when min is 0)
     *     body:  <body>
     *            split body, exit          (arms swapped when lazy)
     *     exit:
     *
     * Against stage 10's `split; body; jmp split`, which is the same language
     * and a different program. The difference is what a walk that has already
     * been to the split does with an iteration that consumed nothing: there
     * it arrives at the split a second time, finds it occupied and dies, so
     * the captures that iteration wrote go with it; here it leaves by the
     * split's other arm and they stand. `(a*)*` against "b" is the shortest
     * case - group 1 is 0-0 in both references and unset under the other
     * shape - and `(a*?)+b` against "aab" is the one that pays for the copy
     * stage 1 held back: with two copies of the body the fresh iteration and
     * the continuing one sit at different program counters, both survive, and
     * the wrong one is preferred.
     *
     * No progress register, which is the other half of the mode and is why
     * this is not stage 10 with a flag: a register would make those two
     * threads distinct again, and it is also the one thing that stops the
     * bit-state engine from running the program at all.
     */
    case 12: {
      GRX_Result result = GRX_OK;
      if (node->min == 0) {
        result = emit(codegen, GRX_OP_SPLIT, 0, 0, 0, node, &frame->u.repeat.enter);
        if (result != GRX_OK) {
          return result;
        }
      }
      frame->u.repeat.body_start = here(codegen);
      if (frame->u.repeat.late) {
        result = emit(codegen, GRX_OP_PROGRESS_SET, 0,
            frame->u.repeat.late_reg, 0, node, NULL);
      }
      if (result == GRX_OK) {
        result = emit_capture_reset(codegen, node, frame->u.repeat.body);
      }
      if (result != GRX_OK) {
        return result;
      }
      if (frame->u.repeat.clears_tail) {
        codegen->forward_tail = 0;
      }
      frame->stage = 13;
      *action = GEN_DESCEND;
      *out_child = frame->u.repeat.body;
      return GRX_OK;
    }

    case 13: {
      GRX_Result result = emit_capture_reset_late(
          codegen, node, frame->u.repeat.body, frame->u.repeat.late_reg);
      uint32_t split = GRX_INDEX_NONE;
      if (result == GRX_OK) {
        result = emit(codegen, GRX_OP_SPLIT, 0, 0, 0, node, &split);
      }
      if (result != GRX_OK) {
        return result;
      }

      uint32_t exit_target = here(codegen);
      // Both splits prefer the same arm, because both are this repeat's
      // greediness asked twice: the `?` about whether to run the body at all
      // and the `+` about whether to run it again.
      if (frame->u.repeat.lazy) {
        patch_x(codegen, split, exit_target);
        patch_y(codegen, split, frame->u.repeat.body_start);
      }
      else {
        patch_x(codegen, split, frame->u.repeat.body_start);
        patch_y(codegen, split, exit_target);
      }
      if (frame->u.repeat.enter != GRX_INDEX_NONE) {
        if (frame->u.repeat.lazy) {
          patch_x(codegen, frame->u.repeat.enter, exit_target);
          patch_y(codegen, frame->u.repeat.enter, frame->u.repeat.body_start);
        }
        else {
          patch_x(codegen, frame->u.repeat.enter, frame->u.repeat.body_start);
          patch_y(codegen, frame->u.repeat.enter, exit_target);
        }
      }
      return GRX_OK;
    }

    default:
      return fail(codegen, GRX_DIAG_INTERNAL, node);
  }
}

/** Generate a lookaround: a sub-program run without consuming input. */
/**
 * Lay out a conditional whose condition is an assertion.
 *
 *     LOOK  x=span  y=t    mode = which assertion, flags |= COND_ELSE
 *     <body>
 *     MATCH
 *   t:                     the condition held
 *     JMP yes
 *   t+1:                   it did not
 *     JMP no
 *   yes:
 *     <then>
 *     JMP end
 *   no:
 *     <else>
 *   end:
 *
 * The two jumps are the price of the single run: a LOOK has `x` for its
 * length span and `y` for one continuation, and this needs two. They sit
 * adjacently so that the relation is local - the flag's own documentation
 * is the only place that has to state it - and they keep the blocks in
 * source order, which an else-branch inlined at `y + 1` would not.
 *
 * Two instructions, against a whole second copy of the assertion's body
 * under the rewrite this replaces.
 */
static GRX_Result gen_cond_assertion_step(Codegen * codegen,
    GenFrame * frame, const GRX_IRNode * node, GenAction * action,
    uint32_t * out_child) {
  switch (frame->stage) {
    case 20: {
      uint32_t condition = node->first_child;
      const GRX_IRNode * look = grx_ir_node(codegen->ir, condition);
      if (!look || look->kind != GRX_IR_LOOK) {
        return fail(codegen, GRX_DIAG_INTERNAL, node);
      }
      frame->u.cond.yes = look->next_sibling;

      // Descending on the condition emits the LOOK, its body and the body's
      // MATCH, and patches the LOOK's `y` to here - so on the way back,
      // `here()` is the first of the two jumps. Going through the ordinary
      // generator rather than by hand is what keeps a conditional's assertion
      // the same assertion as any other: the span, the capture rules and the
      // two lookbehind models are gen_look_step()'s, not a second copy of
      // them.
      frame->stage = 21;
      *action = GEN_DESCEND;
      *out_child = condition;
      return GRX_OK;
    }

    case 21: {
      uint32_t held = GRX_INDEX_NONE;
      GRX_Result result = emit(codegen, GRX_OP_JMP, 0, 0, 0, node, &held);
      if (result != GRX_OK) {
        return result;
      }
      uint32_t missed = GRX_INDEX_NONE;
      result = emit(codegen, GRX_OP_JMP, 0, 0, 0, node, &missed);
      if (result != GRX_OK) {
        return result;
      }
      frame->u.cond.held = held;
      frame->u.cond.missed = missed;
      frame->u.cond.no = GRX_INDEX_NONE;

      patch_x(codegen, held, here(codegen));
      frame->stage = 22;
      if (frame->u.cond.yes != GRX_INDEX_NONE) {
        const GRX_IRNode * taken
            = grx_ir_node(codegen->ir, frame->u.cond.yes);
        frame->u.cond.no = taken ? taken->next_sibling : GRX_INDEX_NONE;
        *action = GEN_DESCEND;
        *out_child = frame->u.cond.yes;
        return GRX_OK;
      }
      *action = GEN_AGAIN;
      return GRX_OK;
    }

    case 22: {
      uint32_t skip = GRX_INDEX_NONE;
      GRX_Result result = emit(codegen, GRX_OP_JMP, 0, 0, 0, node, &skip);
      if (result != GRX_OK) {
        return result;
      }
      frame->u.cond.skip = skip;

      patch_x(codegen, frame->u.cond.missed, here(codegen));
      frame->stage = 23;
      if (frame->u.cond.no != GRX_INDEX_NONE) {
        *action = GEN_DESCEND;
        *out_child = frame->u.cond.no;
        return GRX_OK;
      }
      *action = GEN_AGAIN;
      return GRX_OK;
    }

    case 23:
      patch_x(codegen, frame->u.cond.skip, here(codegen));
      return GRX_OK;

    default:
      return fail(codegen, GRX_DIAG_INTERNAL, node);
  }
}

/**
 * Lay out a conditional: the test, the true branch, the false branch.
 *
 *     COND  x=group  y=false     mode = what is being tested
 *     <yes>
 *     JMP end
 *   false:
 *     <no>
 *   end:
 *
 * The JMP is what makes it a conditional rather than an alternation: once the
 * test has chosen a branch, the other one is not reachable by backtracking.
 *
 * An assertion condition has no such cheap test - it has to run a
 * sub-program - and is laid out by gen_cond_assertion() instead.
 */
static GRX_Result copy_group_list(Codegen * codegen, const GRX_IRNode * node,
    uint32_t ir_list, uint32_t * out_offset);

static GRX_Result gen_cond_step(Codegen * codegen, GenFrame * frame,
    const GRX_IRNode * node, GenAction * action, uint32_t * out_child) {
  if (frame->stage == 0 && node->mode == GRX_COND_ASSERTION) {
    frame->stage = 20;
  }
  if (frame->stage >= 20) {
    return gen_cond_assertion_step(codegen, frame, node, action, out_child);
  }

  switch (frame->stage) {
    case 0: {
      // `x` is the group, unless the name it was written with belongs to
      // several - then it is a list of them and GRX_INST_AMBIGUOUS_REF says
      // so, exactly as a backreference carries one, because the question is
      // the same: which of the groups with that name is set when this runs.
      uint32_t operand = node->a;
      if (node->flags & GRX_IR_AMBIGUOUS_REF) {
        GRX_Result listed = copy_group_list(codegen, node, node->b, &operand);
        if (listed != GRX_OK) {
          return listed;
        }
      }

      uint32_t test = GRX_INDEX_NONE;
      GRX_Result result
          = emit(codegen, GRX_OP_COND, node->mode, operand, 0, node, &test);
      if (result != GRX_OK) {
        return result;
      }
      if (node->flags & GRX_IR_AMBIGUOUS_REF) {
        GRX_Inst * inst
            = GRX_ARENA_AT(GRX_Inst, &codegen->program->insts, test);
        if (!inst) {
          return fail(codegen, GRX_DIAG_INTERNAL, node);
        }
        inst->flags |= GRX_INST_AMBIGUOUS_REF;
      }
      frame->u.cond.test = test;
      frame->u.cond.no = GRX_INDEX_NONE;

      frame->stage = 1;
      uint32_t yes = node->first_child;
      if (yes != GRX_INDEX_NONE) {
        const GRX_IRNode * taken = grx_ir_node(codegen->ir, yes);
        frame->u.cond.no = taken ? taken->next_sibling : GRX_INDEX_NONE;
        *action = GEN_DESCEND;
        *out_child = yes;
        return GRX_OK;
      }
      *action = GEN_AGAIN;
      return GRX_OK;
    }

    case 1: {
      uint32_t skip = GRX_INDEX_NONE;
      GRX_Result result = emit(codegen, GRX_OP_JMP, 0, 0, 0, node, &skip);
      if (result != GRX_OK) {
        return result;
      }
      frame->u.cond.skip = skip;

      patch_y(codegen, frame->u.cond.test, here(codegen));
      frame->stage = 2;
      if (frame->u.cond.no != GRX_INDEX_NONE) {
        *action = GEN_DESCEND;
        *out_child = frame->u.cond.no;
        return GRX_OK;
      }
      *action = GEN_AGAIN;
      return GRX_OK;
    }

    case 2:
      patch_x(codegen, frame->u.cond.skip, here(codegen));
      return GRX_OK;

    default:
      return fail(codegen, GRX_DIAG_INTERNAL, node);
  }
}

/**
 * Emit a call to a group, and remember to lay that group's block out later.
 *
 * Whether the call is atomic is not decided here: PCRE2's is and Perl's is
 * not, which makes it a dialect choice, and lowering has already spent it by
 * wrapping this node in GRX_IR_ATOMIC where it applies.
 */
static GRX_Result gen_call(Codegen * codegen, const GRX_IRNode * node) {
  uint32_t call = GRX_INDEX_NONE;
  GRX_Result result = emit(
      codegen, GRX_OP_CALL, 0, GRX_INDEX_NONE, node->a, node, &call);
  if (result != GRX_OK) {
    return result;
  }

  Fixup fixup = {.call = call, .group = node->a, .definition = node->b,
    .reverse = reversed(codegen, node) ? 1u : 0u};
  result = grx_arena_append(&codegen->fixups, &fixup, NULL);
  if (result != GRX_OK) {
    return fail(codegen,
        result == GRX_ERR_LIMIT ? GRX_DIAG_LIMIT_PROGRAM_SIZE
                                : GRX_DIAG_OUT_OF_MEMORY,
        node);
  }
  return GRX_OK;
}

/**
 * Copy a forward lookbehind's body length into the program.
 *
 * The number of candidate starts the engine will try is `max - min + 1`, and
 * lowering only chose this model for a dialect that caps that at 256
 * (documentation/design.md section 3.5.2). An unbounded span here would
 * uncap it, so the pattern is refused as the variable-length lookbehind it
 * is rather than compiled into a loop with nothing to stop it. compile.c's
 * own check catches every body whose *variation* is unmeasurable and every
 * one whose variation is merely too large, so what is left for this to catch
 * is the body whose two ends both saturate - a length no subject could
 * reach, and one no bounded dialect should accept for being nominally fixed.
 */
static GRX_Result gen_look_span(
    Codegen * codegen, const GRX_IRNode * node, uint32_t * out_offset) {
  size_t min = 0;
  size_t max = 0;
  if (!grx_ir_look_span(codegen->ir, node->a, &min, &max)) {
    return fail(codegen, GRX_DIAG_INTERNAL, node);
  }
  if (max == GRX_NPOS || min > max) {
    return fail(codegen, GRX_DIAG_VARIABLE_LOOKBEHIND, node);
  }

  uint32_t offset = (uint32_t)codegen->program->look_spans.count;
  if (grx_arena_append(&codegen->program->look_spans, &min, NULL) != GRX_OK
      || grx_arena_append(&codegen->program->look_spans, &max, NULL)
          != GRX_OK) {
    return fail(codegen, GRX_DIAG_OUT_OF_MEMORY, node);
  }
  *out_offset = offset;
  return GRX_OK;
}

static GRX_Result gen_look_step(Codegen * codegen, GenFrame * frame,
    const GRX_IRNode * node, GenAction * action, uint32_t * out_child) {
  if (frame->stage == 0
      && (node->mode == GRX_LOOK_AHEAD_NON_ATOMIC
          || node->mode == GRX_LOOK_BEHIND_NON_ATOMIC)) {
    frame->stage = 10;
  }

  switch (frame->stage) {
    case 0: {
      // `x` is the length span, not the body: the body is always the next
      // instruction, so writing that down twice only makes a second place for
      // it to disagree with itself. A lookahead and a reverse lookbehind have
      // no span and say so.
      uint32_t span = GRX_INDEX_NONE;
      if (node->flags & GRX_IR_LOOK_FORWARD) {
        GRX_Result measured = gen_look_span(codegen, node, &span);
        if (measured != GRX_OK) {
          return measured;
        }
      }

      uint32_t look = GRX_INDEX_NONE;
      GRX_Result result
          = emit(codegen, GRX_OP_LOOK, node->mode, span, 0, node, &look);
      if (result != GRX_OK) {
        return result;
      }
      if (node->flags & (GRX_IR_LOOK_KEEP_CAPTURES | GRX_IR_LOOK_CONDITION)) {
        GRX_Inst * marked = grx_program_at(codegen->program, look);
        if (marked) {
          marked->flags |= (node->flags & GRX_IR_LOOK_KEEP_CAPTURES)
              ? GRX_INST_KEEP_CAPTURES : 0u;
          marked->flags |= (node->flags & GRX_IR_LOOK_CONDITION)
              ? GRX_INST_COND_ELSE : 0u;
        }
      }
      frame->u.look.mark = look;

      // Set rather than raised: the distance a guard measures is *this*
      // assertion's, and a body nested in another one has its own end or none
      // at all. Everything that is not a forward lookbehind - a lookahead, a
      // reverse lookbehind, a scan - clears it, and the engine clears the
      // matching runtime field in the same places.
      codegen->forward_tail = (node->flags & GRX_IR_LOOK_FORWARD) ? 1 : 0;
      // A lookaround sets its body's direction absolutely, so a subroutine
      // block being laid out the other way round turns over everything except
      // this. See the `flip` field.
      codegen->flip = 0;
      frame->stage = 1;
      *action = GEN_DESCEND;
      *out_child = node->first_child;
      return GRX_OK;
    }

    case 1: {
      // The body ends in a MATCH so that the sub-program has a success state
      // of its own; the LOOK's `y` is where the outer program resumes.
      GRX_Result result = emit(codegen, GRX_OP_MATCH, 0, 0, 0, node, NULL);
      if (result != GRX_OK) {
        return result;
      }
      patch_y(codegen, frame->u.look.mark, here(codegen));
      return GRX_OK;
    }

    /*
     * The non-atomic form, which was gen_non_atomic_look().
     *
     * The whole difference is that there is no sub-match. The body's
     * instructions are the outer program's, so the choice points it leaves
     * stay on the backtrack stack and can be returned to - which is what
     * "non-atomic" means. A register records where the body started and a
     * REWIND puts the position back, so the construct still consumes nothing.
     *
     * A non-atomic lookbehind needs nothing else: its body carries
     * GRX_IR_REVERSE already, so its instructions walk backwards and the
     * REWIND undoes that the same way.
     */
    case 10: {
      uint32_t reg = codegen->registers++;
      GRX_Result result
          = emit(codegen, GRX_OP_PROGRESS_SET, 0, reg, 0, node, NULL);
      if (result != GRX_OK) {
        return result;
      }
      frame->u.look.reg = reg;

      // Inlined into the caller's instructions, so a length guard here would
      // be measuring the caller's distance against this body's alternatives.
      // It is not the same assertion; it gets no guards.
      codegen->forward_tail = 0;
      codegen->flip = 0; // As in stage 0: the body's own direction.
      frame->stage = 11;
      *action = GEN_DESCEND;
      *out_child = node->first_child;
      return GRX_OK;
    }

    case 11:
      return emit(codegen, GRX_OP_REWIND, 0, frame->u.look.reg, 0, node, NULL);

    default:
      return fail(codegen, GRX_DIAG_INTERNAL, node);
  }
}

/**
 * Emit `(*scs:(...)body)`: the group list, then the body as a sub-program.
 *
 * The list is copied into the program because a program outlives the IR. The
 * layout is a LOOK's - the body follows the instruction and ends in a MATCH
 * of its own - so `x` is free to hold the list's offset.
 */
/**
 * Copy one of the IR's group lists into the program, and say where it went.
 *
 * The program outlives the IR, so a list an instruction refers to has to be
 * carried over rather than pointed at. Two instructions use one: SCAN, whose
 * list is the groups `(*scs:(...))` may scan, and an ambiguous BACKREF,
 * whose list is every group sharing the name it was written with.
 */
static GRX_Result copy_group_list(Codegen * codegen, const GRX_IRNode * node,
    uint32_t ir_list, uint32_t * out_offset) {
  size_t count = 0;
  const uint32_t * groups = grx_ir_scan_list(codegen->ir, ir_list, &count);
  if (!groups || !count) {
    return fail(codegen, GRX_DIAG_INTERNAL, node);
  }

  uint32_t offset = (uint32_t)codegen->program->scan_lists.count;
  uint32_t total = (uint32_t)count;
  if (grx_arena_append(&codegen->program->scan_lists, &total, NULL) != GRX_OK) {
    return fail(codegen, GRX_DIAG_OUT_OF_MEMORY, node);
  }
  for (size_t i = 0; i < count; i++) {
    uint32_t group = groups[i];
    if (grx_arena_append(&codegen->program->scan_lists, &group, NULL)
        != GRX_OK) {
      return fail(codegen, GRX_DIAG_OUT_OF_MEMORY, node);
    }
  }
  *out_offset = offset;
  return GRX_OK;
}

/**
 * Emit a callout, copying its payload into the program's own tables.
 *
 * The record is per instruction rather than deduplicated. Two callouts that
 * are written the same way are still two places in the pattern, and
 * GRX_ProgramCallout::pattern_offset is what tells them apart - which is
 * the opposite of a mark, where two `(*MARK:A)` are deliberately one entry
 * because a `(*SKIP:A)` has to compare them.
 */
static GRX_Result gen_callout(Codegen * codegen, const GRX_IRNode * node) {
  GRX_ProgramCallout record = {
    // Past this callout's own `)`, which is where the next item begins in
    // every case - including the ones where that item is a `)`, a `|` or
    // the end of the pattern.
    .pattern_offset = node->offset + node->length,
    .string_offset = node->max,
    .string_length = node->min,
    .number = node->a,
    .string = GRX_INDEX_NONE,
  };

  if (node->b != GRX_INDEX_NONE) {
    const char * text = grx_ir_name(codegen->ir, node->b);
    if (!text) {
      return fail(codegen, GRX_DIAG_INTERNAL, node);
    }
    record.string = (uint32_t)codegen->program->callout_strings.count;
    for (size_t i = 0; i < record.string_length; i++) {
      if (grx_arena_append(&codegen->program->callout_strings, &text[i], NULL)
          != GRX_OK) {
        return fail(codegen, GRX_DIAG_OUT_OF_MEMORY, node);
      }
    }
    char terminator = '\0';
    if (grx_arena_append(
            &codegen->program->callout_strings, &terminator, NULL)
        != GRX_OK) {
      return fail(codegen, GRX_DIAG_OUT_OF_MEMORY, node);
    }
  }

  uint32_t index = (uint32_t)codegen->program->callouts.count;
  if (grx_arena_append(&codegen->program->callouts, &record, NULL) != GRX_OK) {
    return fail(codegen, GRX_DIAG_OUT_OF_MEMORY, node);
  }
  return emit(codegen, GRX_OP_CALLOUT, 0, index, 0, node, NULL);
}

/**
 * Emit a backreference.
 *
 * `x` is the group, unless the name it was written with belongs to several -
 * then it is a list of them and GRX_INST_AMBIGUOUS_REF says so, because
 * which one the reference means is whichever is *set* when it runs.
 */
static GRX_Result gen_backref(Codegen * codegen, const GRX_IRNode * node) {
  uint32_t operand = node->a;
  if (node->flags & GRX_IR_AMBIGUOUS_REF) {
    GRX_Result listed
        = copy_group_list(codegen, node, node->b, &operand);
    if (listed != GRX_OK) {
      return listed;
    }
  }

  uint32_t index = GRX_INDEX_NONE;
  GRX_Result result = emit(codegen, GRX_OP_BACKREF, node->backref_unset,
      operand, (node->flags & GRX_IR_CASELESS) ? 1u : 0u, node, &index);
  if (result != GRX_OK) {
    return result;
  }
  if (node->flags & GRX_IR_AMBIGUOUS_REF) {
    GRX_Inst * inst
        = GRX_ARENA_AT(GRX_Inst, &codegen->program->insts, index);
    if (!inst) {
      return fail(codegen, GRX_DIAG_INTERNAL, node);
    }
    inst->flags |= GRX_INST_AMBIGUOUS_REF;
  }
  return GRX_OK;
}

static GRX_Result gen_scan_step(Codegen * codegen, GenFrame * frame,
    const GRX_IRNode * node, GenAction * action, uint32_t * out_child) {
  switch (frame->stage) {
    case 0: {
      uint32_t offset = 0;
      GRX_Result listed = copy_group_list(codegen, node, node->a, &offset);
      if (listed != GRX_OK) {
        return listed;
      }

      uint32_t scan = GRX_INDEX_NONE;
      GRX_Result result
          = emit(codegen, GRX_OP_SCAN, 0, offset, 0, node, &scan);
      if (result != GRX_OK) {
        return result;
      }
      frame->u.look.mark = scan;

      // A scan's body is a sub-match over a captured substring, with an end of
      // its own; a guard measuring the enclosing assertion's distance would be
      // measuring against the wrong subject entirely.
      codegen->forward_tail = 0;
      frame->stage = 1;
      *action = GEN_DESCEND;
      *out_child = node->first_child;
      return GRX_OK;
    }

    case 1: {
      GRX_Result result = emit(codegen, GRX_OP_MATCH, 0, 0, 0, node, NULL);
      if (result != GRX_OK) {
        return result;
      }
      patch_y(codegen, frame->u.look.mark, here(codegen));
      return GRX_OK;
    }

    default:
      return fail(codegen, GRX_DIAG_INTERNAL, node);
  }
}

/**
 * Take one node one step further, and say what to do next.
 *
 * The switch the recursive gen() was, with each arm either finishing the node
 * or naming the child to lay out before resuming. A leaf kind finishes in its
 * first stage and never descends, which is why most arms here are unchanged.
 */
static GRX_Result gen_step(Codegen * codegen, GenFrame * frame,
    const GRX_IRNode * node, GenAction * action, uint32_t * out_child) {
  switch (node->kind) {
    case GRX_IR_EMPTY:
      return GRX_OK;

    case GRX_IR_CHAR:
      return emit(codegen, GRX_OP_CHAR, 0, node->a, 0, node, NULL);

    case GRX_IR_CLASS:
      return emit(codegen, GRX_OP_CLASS, 0, node->a, 0, node, NULL);

    case GRX_IR_ANY:
      // "Any code point at all" is its own opcode rather than a class of
      // everything, because it is the common case and an engine can answer it
      // without a binary search.
      if (node->a == GRX_INDEX_NONE) {
        return emit(codegen, GRX_OP_ANY_NL, 0, 0, 0, node, NULL);
      }
      return emit(codegen, GRX_OP_ANY, 0, node->a, 0, node, NULL);

    case GRX_IR_CONCAT:
      return gen_concat_step(codegen, frame, node, action, out_child);

    case GRX_IR_ALTERNATE:
      return gen_alternate_step(codegen, frame, node, action, out_child);

    case GRX_IR_REPEAT:
      return gen_repeat_step(codegen, frame, node, action, out_child);

    case GRX_IR_CAPTURE: {
      // The slots are twice the group number and one more, so group 0's
      // start is slot 0 and the whole match is slots 0 and 1. A reversed
      // capture writes its end first, because that is the boundary its body
      // reaches first.
      if (frame->stage == 0) {
        uint32_t first = node->a * 2 + (reversed(codegen, node) ? 1u : 0u);
        GRX_Result result
            = emit(codegen, GRX_OP_SAVE, 0, first, 0, node, NULL);
        if (result != GRX_OK) {
          return result;
        }
        frame->u.one.slot
            = node->a * 2 + (reversed(codegen, node) ? 0u : 1u);
        frame->stage = 1;
        *action = GEN_DESCEND;
        *out_child = node->first_child;
        return GRX_OK;
      }
      return emit(
          codegen, GRX_OP_SAVE, 0, frame->u.one.slot, 0, node, NULL);
    }

    case GRX_IR_ASSERT:
      // `y` is only read by GRX_ASSERT_BYTE_COLUMN, which is the only
      // assertion carrying two numbers; every other kind leaves `b` zero.
      return emit(
          codegen, GRX_OP_ASSERT, node->mode, node->a, node->b, node, NULL);

    case GRX_IR_LOOK:
      return gen_look_step(codegen, frame, node, action, out_child);

    case GRX_IR_ATOMIC: {
      if (frame->stage == 0) {
        uint32_t begin = GRX_INDEX_NONE;
        GRX_Result result
            = emit(codegen, GRX_OP_ATOMIC_BEGIN, 0, 0, 0, node, &begin);
        if (result != GRX_OK) {
          return result;
        }
        frame->u.one.slot = begin;
        frame->stage = 1;
        *action = GEN_DESCEND;
        *out_child = node->first_child;
        return GRX_OK;
      }
      uint32_t end = GRX_INDEX_NONE;
      GRX_Result result
          = emit(codegen, GRX_OP_ATOMIC_END, 0, 0, 0, node, &end);
      if (result == GRX_OK) {
        patch_x(codegen, frame->u.one.slot, end);
      }
      return result;
    }

    case GRX_IR_BACKREF:
      return gen_backref(codegen, node);

    case GRX_IR_KEEP:
      return emit(codegen, GRX_OP_KEEP, 0, 0, 0, node, NULL);

    case GRX_IR_KEEP_END:
      codegen->has_keep_end = 1;
      return emit(codegen, GRX_OP_KEEP_END, 0, 0, 0, node, NULL);

    case GRX_IR_VERB:
      return emit(codegen, GRX_OP_VERB, node->mode, node->a, 0, node, NULL);

    case GRX_IR_SCAN:
      return gen_scan_step(codegen, frame, node, action, out_child);

    case GRX_IR_SCRIPT_RUN: {
      // A register to hold where the body began, and a check at the end
      // over what it consumed. GRX_OP_PROGRESS_SET is the write, because
      // "record the position" is exactly what it does and its undo frame
      // is what makes a backtrack out of the body put the mark back.
      if (frame->stage == 0) {
        uint32_t reg = codegen->registers++;
        GRX_Result result
            = emit(codegen, GRX_OP_PROGRESS_SET, 0, reg, 0, node, NULL);
        if (result != GRX_OK) {
          return result;
        }
        frame->u.one.slot = reg;
        frame->stage = 1;
        *action = GEN_DESCEND;
        *out_child = node->first_child;
        return GRX_OK;
      }
      return emit(
          codegen, GRX_OP_SCRIPT_RUN, 0, frame->u.one.slot, 0, node, NULL);
    }

    case GRX_IR_FOLD_RUN:
      return gen_fold_run(codegen, node);

    case GRX_IR_CALLOUT:
      return gen_callout(codegen, node);

    case GRX_IR_COND:
      return gen_cond_step(codegen, frame, node, action, out_child);

    case GRX_IR_RECURSE:
      return gen_call(codegen, node);

    case GRX_IR_COUNT:
    default:
      return fail(codegen, GRX_DIAG_INTERNAL, node);
  }
}

/** Put one node's frame on the generator's stack. */
static GRX_Result gen_push(Codegen * codegen, uint32_t node_index) {
  GenFrame frame = {.node = node_index, .stage = 0};
  if (grx_arena_append(&codegen->frames, &frame, NULL) != GRX_OK) {
    return fail(codegen, GRX_DIAG_OUT_OF_MEMORY, NULL);
  }
  return GRX_OK;
}

/**
 * Lay out a subtree: the driver over the frames the generators leave.
 *
 * Depth-first, exactly as the recursion was, with the C stack's job done by
 * codegen->frames. The one piece of bookkeeping that is the driver's rather
 * than a generator's is `forward_tail` and `flip`: they are saved when a frame
 * starts and put back whenever it is resumed, which is what the recursive form
 * did with a local at each of a dozen call sites. Doing it here means a new
 * generator cannot forget to.
 *
 * Re-entrant, because gen_subroutines() calls this while nothing else is on
 * the stack and a future caller might not be so lucky: the loop runs until the
 * stack is back to the depth it was handed, not until it is empty.
 */
static GRX_Result gen(Codegen * codegen, uint32_t node_index) {
  size_t base = codegen->frames.count;
  GRX_Result result = gen_push(codegen, node_index);

  while (result == GRX_OK && codegen->frames.count > base) {
    GenFrame * frame
        = GRX_ARENA_AT(GenFrame, &codegen->frames, codegen->frames.count - 1);
    if (!frame) {
      result = fail(codegen, GRX_DIAG_INTERNAL, NULL);
      break;
    }
    const GRX_IRNode * node = grx_ir_node(codegen->ir, frame->node);
    if (!node) {
      result = fail(codegen, GRX_DIAG_INTERNAL, NULL);
      break;
    }

    if (frame->stage == 0) {
      frame->saved_tail = (int8_t)codegen->forward_tail;
      frame->saved_flip = (int8_t)codegen->flip;
    }
    else {
      codegen->forward_tail = frame->saved_tail;
      codegen->flip = frame->saved_flip;
    }

    GenAction action = GEN_POP;
    uint32_t child = GRX_INDEX_NONE;
    result = gen_step(codegen, frame, node, &action, &child);
    if (result != GRX_OK) {
      break;
    }

    // `frame` is dead after a push: the arena may have moved. The loop
    // re-reads the top rather than keeping a pointer across one.
    if (action == GEN_DESCEND) {
      result = gen_push(codegen, child);
      continue;
    }
    if (action == GEN_AGAIN) {
      continue;
    }
    codegen->frames.count--;
  }

  // Unwound whether it finished or failed, so that a caller which carries on
  // after a failure - gen_subroutines() does not, but grx_codegen_program()
  // reads codegen->registers afterwards - is not looking at a part-walked
  // stack.
  codegen->frames.count = base;
  return result;
}

/**
 * Copy the IR's class table into the program's.
 *
 * The program owns its classes because it outlives the IR: a compiled regex
 * is what a caller keeps, and the IR is freed as soon as the program exists.
 */
static GRX_Result copy_classes(
    Codegen * codegen, const GRX_ClassTable * source) {
  size_t count = grx_class_table_count(source);
  for (size_t i = 0; i < count; i++) {
    size_t ranges = 0;
    const GRX_CharRange * first
        = grx_class_table_get(source, (uint32_t)i, &ranges);
    uint32_t index = GRX_INDEX_NONE;
    GRX_Result result = grx_class_table_add(
        &codegen->program->classes, first, ranges, &index);
    if (result != GRX_OK) {
      return fail(codegen,
          result == GRX_ERR_LIMIT ? GRX_DIAG_LIMIT_CLASS_RANGES
                                  : GRX_DIAG_OUT_OF_MEMORY,
          NULL);
    }
    if (index != (uint32_t)i) {
      // Every instruction names a class by index, so the copy has to
      // preserve them. It does, because the table is appended to in order
      // and starts empty; this says so rather than assuming it.
      return fail(codegen, GRX_DIAG_INTERNAL, NULL);
    }
  }

  return GRX_OK;
}

/**
 * The IR node a call to group `group` re-enters.
 *
 * Group 0 is the whole pattern, which is what `(?R)` calls. Anything else is
 * the capture node with that number, found by a scan rather than by an index:
 * the numbers are the pattern's, not the arena's, and a table mapping one to
 * the other would be a second thing to keep in step with lowering.
 *
 * `definition` is the byte offset the wanted one was written at, or
 * GRX_INDEX_NONE for the first of them - which is what a call by number
 * means, and all a pattern without a `(?|...)` ever has. Where it is given,
 * a repeat that expanded the group into several copies still matches: the
 * copies share the offset they came from, and the first is as good as any.
 */
static uint32_t call_target(
    const GRX_IR * ir, uint32_t group, uint32_t definition) {
  if (!group) {
    return ir->root;
  }

  for (size_t i = 0; i < ir->nodes.count; i++) {
    const GRX_IRNode * node = grx_ir_node(ir, (uint32_t)i);
    if (node && node->kind == GRX_IR_CAPTURE && node->a == group
        && (definition == GRX_INDEX_NONE || node->offset == definition)) {
      return (uint32_t)i;
    }
  }

  return GRX_INDEX_NONE;
}

/**
 * Lay out one subroutine block per called group, and patch the calls.
 *
 * A block generated here may contain calls of its own - `(?R)` inside the
 * pattern it calls is the ordinary case - so the loop runs until no new
 * target appears rather than over a list fixed in advance.
 */
static GRX_Result gen_subroutines(Codegen * codegen) {
  for (size_t next = 0; next < codegen->fixups.count;) {
    const Fixup * fixup = GRX_ARENA_AT(Fixup, &codegen->fixups, next);
    if (!fixup) {
      return fail(codegen, GRX_DIAG_INTERNAL, NULL);
    }
    uint32_t group = fixup->group;
    uint32_t definition = fixup->definition;
    uint32_t call = fixup->call;

    size_t known = codegen->called_count;
    for (size_t i = 0; i < codegen->called_count; i++) {
      if (codegen->called[i] == group
          && codegen->called_definition[i] == definition
          && codegen->called_reverse[i] == fixup->reverse) {
        known = i;
        break;
      }
    }

    if (known == codegen->called_count) {
      if (codegen->called_count
          >= sizeof(codegen->called) / sizeof(*codegen->called)) {
        return fail(codegen, GRX_DIAG_LIMIT_PROGRAM_SIZE, NULL);
      }
      uint32_t target = call_target(codegen->ir, group, definition);
      if (target == GRX_INDEX_NONE) {
        return fail(codegen, GRX_DIAG_INVALID_RECURSION, NULL);
      }
      // A block's instructions step the way its *definition* does, and a
      // group written inside a lookbehind that runs backwards therefore has
      // a backwards definition. A call from outside it wants a forward one:
      // `(*naplb:(a))(?1)` against "aa" is 1-2 in pcre2test, and was 1-1
      // here until 2026-09-24, the call having matched nothing and left
      // group one holding a span outside the match. That is why the key
      // above is a triple - the direction is part of what a block *is*, and
      // the same group may need a copy each way.
      //
      // The other copy is the same subtree laid out with `flip` set, which
      // is an XOR on the way down and not a rewrite of the IR flags: a
      // lookbehind written *inside* the called group looks behind whatever
      // encloses the call, so its body has to keep the direction lowering
      // gave it while everything around it turns over. Rewriting the flags
      // would straighten that too, which is the reason this was refused
      // rather than answered for as long as it was.
      const GRX_IRNode * definition_node = grx_ir_node(codegen->ir, target);
      if (!definition_node) {
        return fail(codegen, GRX_DIAG_INTERNAL, NULL);
      }
      uint32_t defined_reverse
          = (definition_node->flags & GRX_IR_REVERSE) ? 1u : 0u;

      // Reserved before the block is generated, so that a call the block
      // makes to its own group finds the entry already recorded rather than
      // starting a second copy of it.
      codegen->called[codegen->called_count] = group;
      codegen->called_definition[codegen->called_count] = definition;
      codegen->called_reverse[codegen->called_count] = fixup->reverse;
      codegen->entry[codegen->called_count] = here(codegen);
      codegen->called_count++;

      int outer_flip = codegen->flip;
      codegen->flip = defined_reverse != fixup->reverse;
      GRX_Result result = gen(codegen, target);
      if (result == GRX_OK) {
        result = emit(codegen, GRX_OP_RET, 0, 0, 0, NULL, NULL);
      }
      codegen->flip = outer_flip;
      if (result != GRX_OK) {
        return result;
      }
      // Generating the block may have appended calls of its own; they are
      // after `next` and the loop reaches them without rescanning.
      continue;
    }

    patch_x(codegen, call, codegen->entry[known]);
    next++;
  }

  return GRX_OK;
}

GRX_Result grx_codegen_program(const GRX_IR * ir, const GRX_Limits * limits,
    GRX_Error * out_error, GRX_Program * out_program) {
  if (!ir || !limits || !out_program) {
    return GRX_ERR_INVALID;
  }

  // Whether anything in the pattern reads a capture back. One pass over the
  // flat node arena rather than a walk, because the question is about the
  // tree's contents and not its shape.
  int captures_are_read = 0;
  int has_backref = 0;
  for (size_t i = 0; i < ir->nodes.count; i++) {
    const GRX_IRNode * node = grx_ir_node(ir, (uint32_t)i);
    if (!node) {
      continue;
    }
    if (node->kind == GRX_IR_BACKREF) {
      has_backref = 1;
      captures_are_read = 1;
    }
    else if (node->kind == GRX_IR_COND) {
      captures_are_read = 1;
    }
  }

  Codegen codegen = {
    .ir = ir,
    .captures_are_read = captures_are_read,
    .program = out_program,
    .limits = limits,
    .error = out_error,
    .registers = 0,
    .no_memo = 0,
    .simulated = 0,
    .has_keep_end = 0,
    .called = {0},
    .called_definition = {0},
    .entry = {0},
    .called_count = 0,
    .fixups = {0},
    .frames = {0},
    .walk = {0},
    .node_empty = {0},
    .node_span = {0},
    .memo = NULL,
  };
  grx_arena_init(&codegen.fixups, out_program->insts.allocator, sizeof(Fixup),
      limits->max_program_size, GRX_DIAG_LIMIT_PROGRAM_SIZE);
  grx_arena_init(&codegen.frames, out_program->insts.allocator,
      sizeof(GenFrame), 0, GRX_DIAG_NONE);
  grx_arena_init(&codegen.walk, out_program->insts.allocator,
      sizeof(uint32_t), 0, GRX_DIAG_NONE);
  // One slot per node, filled with "unknown". A failure to allocate leaves the
  // arena short and can_match_empty() simply asks every time, which is what it
  // did before this existed.
  grx_arena_init(&codegen.node_empty, out_program->insts.allocator,
      sizeof(unsigned char), 0, GRX_DIAG_NONE);
  if (grx_arena_reserve(&codegen.node_empty, ir->nodes.count) == GRX_OK) {
    for (size_t i = 0; i < ir->nodes.count; i++) {
      if (grx_arena_append(&codegen.node_empty, NULL, NULL) != GRX_OK) {
        break;
      }
    }
  }
  // Same discipline as the two arenas above: a memo that could not be made
  // means asking the long way, not failing.
  codegen.memo = grx_ir_memo_create(ir);
  grx_arena_init(&codegen.node_span, out_program->insts.allocator,
      sizeof(SpanMemo), 0, GRX_DIAG_NONE);
  if (grx_arena_reserve(&codegen.node_span, ir->nodes.count) == GRX_OK) {
    for (size_t i = 0; i < ir->nodes.count; i++) {
      if (grx_arena_append(&codegen.node_span, NULL, NULL) != GRX_OK) {
        break;
      }
    }
  }

  GRX_Result result = copy_classes(&codegen, &ir->classes);

  // Group 0 is the whole match, so the program brackets everything in its
  // two slots. An engine therefore never special-cases the overall span: it
  // is capture 0 and comes out of the same array as the rest.
  if (result == GRX_OK) {
    result = emit(&codegen, GRX_OP_SAVE, 0, 0, 0, NULL, NULL);
  }
  if (result == GRX_OK && ir->root != GRX_INDEX_NONE) {
    result = gen(&codegen, ir->root);
  }
  if (result == GRX_OK) {
    // Where a `\ze` is in the program, the closing save of group 0 defers
    // to it: the marker writes slot 1 on its way past and this instruction
    // leaves a slot that is already set alone. The flag is only set when
    // there is a marker to defer to, so every other program keeps the
    // unconditional write it has always had.
    uint32_t closing = here(&codegen);
    result = emit(&codegen, GRX_OP_SAVE, 0, 1, 0, NULL, NULL);
    if (result == GRX_OK && codegen.has_keep_end) {
      GRX_Inst * inst = grx_program_at(codegen.program, closing);
      if (inst) {
        inst->flags |= GRX_INST_SAVE_IF_UNSET;
      }
    }
  }
  if (result == GRX_OK) {
    result = emit(&codegen, GRX_OP_MATCH, 0, 0, 0, NULL, NULL);
  }
  if (result == GRX_OK) {
    // After the MATCH, where nothing falls into them: a subroutine block is
    // reached by a CALL and left by its RET, and never by running off the
    // end of the instruction before it.
    result = gen_subroutines(&codegen);
  }

  // The fixups are scaffolding: every one of them has been spent patching a
  // CALL by now, and the program keeps nothing that points into them.
  grx_arena_clear(&codegen.fixups);
  grx_arena_clear(&codegen.frames);
  grx_arena_clear(&codegen.walk);
  grx_arena_clear(&codegen.node_empty);
  grx_arena_clear(&codegen.node_span);
  grx_ir_memo_destroy(codegen.memo);
  if (result != GRX_OK) {
    return result;
  }

  out_program->flags = ir->flags;
  if (has_backref) {
    // A reference can name a group that is still open, and then it wants the
    // span that group last closed with rather than the half-written one.
    out_program->flags |= GRX_PROGRAM_SHADOW_CAPTURES;
  }
  if (codegen.no_memo) {
    out_program->flags |= GRX_PROGRAM_NO_MEMO;
  }
  if (codegen.simulated) {
    // Not an optimisation any more: exec.c arms the bitmap for this program
    // whichever backtracking engine runs it. See GRX_PROGRAM_SIMULATED_LOOP.
    out_program->flags |= GRX_PROGRAM_SIMULATED_LOOP;
  }
  out_program->preference = ir->preference;
  out_program->submatch = ir->submatch;
  out_program->iteration = ir->iteration;
  out_program->search_start = ir->search_start;
  out_program->register_count = codegen.registers;
  return GRX_OK;
}
