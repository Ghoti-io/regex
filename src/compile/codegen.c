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
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/core_internal.h"
#include "../ir/lower_internal.h"
#include "compile_internal.h"

/** What one code-generation run carries. */
/** How many distinct groups one program may call as subroutines. */
#define GRX_CODEGEN_MAX_CALLED 256

typedef struct {
  const GRX_IR * ir;         ///< What is being compiled.
  GRX_Program * program;     ///< What is being built.
  const GRX_Limits * limits; ///< Caps to apply.
  GRX_Error * error;         ///< Where a failure is reported.
  uint32_t registers;        ///< Progress registers handed out so far.
  int no_memo;               ///< An opcode the bit-state memo cannot survive.
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
  uint32_t entry[GRX_CODEGEN_MAX_CALLED];    ///< Their block starts.
  size_t called_count;
  uint32_t fixups[GRX_CODEGEN_MAX_CALLED * 4]; ///< CALL instruction indices.
  uint32_t fixup_group[GRX_CODEGEN_MAX_CALLED * 4];
  size_t fixup_count;
} Codegen;

static GRX_Result gen(Codegen * codegen, uint32_t node_index);

/** Report a failure at a node's span. */
static GRX_Result fail(
    Codegen * codegen, GRX_Diag diag, const GRX_IRNode * node) {
  return grx_error_set(codegen->error, grx_diag_result(diag), diag,
      node ? node->offset : GRX_NPOS, node ? node->length : 0);
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
    case GRX_OP_VERB:
    case GRX_OP_COND:
    case GRX_OP_CALL:
    case GRX_OP_RET:
    case GRX_OP_ATOMIC_BEGIN:
    case GRX_OP_ATOMIC_END:
      codegen->no_memo = 1;
      break;
    default:
      break;
  }

  GRX_Inst inst = {
    .op = (uint8_t)op,
    .mode = mode,
    .flags = (node && (node->flags & GRX_IR_REVERSE)) ? GRX_INST_REVERSE : 0u,
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
static void capture_span(const GRX_IR * ir, uint32_t node_index,
    uint32_t * out_low, uint32_t * out_high) {
  const GRX_IRNode * node = grx_ir_node(ir, node_index);
  if (!node) {
    return;
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
    const GRX_IRNode * child_node = grx_ir_node(ir, child);
    if (!child_node) {
      return;
    }
    capture_span(ir, child, out_low, out_high);
    child = child_node->next_sibling;
  }
}

/**
 * Emit the capture reset for one iteration of a repeat, if the dialect wants
 * one and the body has any captures to clear.
 */
static GRX_Result emit_capture_reset(
    Codegen * codegen, const GRX_IRNode * node, uint32_t body) {
  if (node->capture_reset != GRX_CAPTURE_RESET_EACH) {
    return GRX_OK;
  }

  uint32_t low = GRX_INDEX_NONE;
  uint32_t high = 0;
  capture_span(codegen->ir, body, &low, &high);
  if (low == GRX_INDEX_NONE || high <= low) {
    return GRX_OK;
  }

  return emit(codegen, GRX_OP_RESET, 0, low * 2, high * 2, node, NULL);
}

/** Whether a node runs right to left. */
static int reversed(const GRX_IRNode * node) {
  return (node->flags & GRX_IR_REVERSE) != 0;
}

/** Generate a concatenation, in reading order or against it. */
static GRX_Result gen_concat(Codegen * codegen, const GRX_IRNode * node) {
  if (!reversed(node)) {
    for (uint32_t child = node->first_child; child != GRX_INDEX_NONE;) {
      const GRX_IRNode * child_node = grx_ir_node(codegen->ir, child);
      if (!child_node) {
        return fail(codegen, GRX_DIAG_INTERNAL, node);
      }
      uint32_t next = child_node->next_sibling;
      GRX_Result result = gen(codegen, child);
      if (result != GRX_OK) {
        return result;
      }
      child = next;
    }
    return GRX_OK;
  }

  // Backwards. The child list is singly linked, so the order is recovered by
  // counting and then walking to the nth child - which is quadratic in the
  // number of children and confined to lookbehind bodies, where the count is
  // small and the alternative is a second link field on every node in every
  // pattern.
  uint32_t count = 0;
  for (uint32_t child = node->first_child; child != GRX_INDEX_NONE;) {
    const GRX_IRNode * child_node = grx_ir_node(codegen->ir, child);
    if (!child_node) {
      return fail(codegen, GRX_DIAG_INTERNAL, node);
    }
    count++;
    child = child_node->next_sibling;
  }

  for (uint32_t i = count; i > 0; i--) {
    uint32_t child = node->first_child;
    for (uint32_t step = 1; step < i; step++) {
      const GRX_IRNode * child_node = grx_ir_node(codegen->ir, child);
      if (!child_node) {
        return fail(codegen, GRX_DIAG_INTERNAL, node);
      }
      child = child_node->next_sibling;
    }
    GRX_Result result = gen(codegen, child);
    if (result != GRX_OK) {
      return result;
    }
  }

  return GRX_OK;
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
static GRX_Result gen_alternate(Codegen * codegen, const GRX_IRNode * node) {
  uint32_t child = node->first_child;
  if (child == GRX_INDEX_NONE) {
    return GRX_OK;
  }
  const GRX_IRNode * first = grx_ir_node(codegen->ir, child);
  if (first && first->next_sibling == GRX_INDEX_NONE) {
    return gen(codegen, child); // One branch is not a choice.
  }

  uint32_t stub = GRX_INDEX_NONE;
  GRX_Result result = emit_trampoline(codegen, node, &stub);
  if (result != GRX_OK) {
    return result;
  }

  for (;;) {
    const GRX_IRNode * child_node = grx_ir_node(codegen->ir, child);
    if (!child_node) {
      return fail(codegen, GRX_DIAG_INTERNAL, node);
    }
    uint32_t next = child_node->next_sibling;

    if (next == GRX_INDEX_NONE) {
      result = gen(codegen, child);
      if (result != GRX_OK) {
        return result;
      }
      break;
    }

    uint32_t split = GRX_INDEX_NONE;
    result = emit(codegen, GRX_OP_SPLIT, 0, 0, 0, node, &split);
    if (result != GRX_OK) {
      return result;
    }
    // Marked, so that `(*THEN)` can tell this SPLIT from a quantifier's.
    GRX_Inst * marked = grx_program_at(codegen->program, split);
    if (marked) {
      marked->flags |= GRX_INST_ALTERNATION;
    }
    patch_x(codegen, split, here(codegen));

    result = gen(codegen, child);
    if (result == GRX_OK) {
      result = emit(codegen, GRX_OP_JMP, 0, stub, 0, node, NULL);
    }
    if (result != GRX_OK) {
      return result;
    }

    patch_y(codegen, split, here(codegen));
    child = next;
  }

  patch_x(codegen, stub, here(codegen));
  return GRX_OK;
}

/**
 * Generate one unbounded repetition of a body, with its loop guard.
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
 */
static GRX_Result gen_star(
    Codegen * codegen, const GRX_IRNode * node, uint32_t body_index) {
  int lazy = node->mode == GRX_REPEAT_LAZY;

  // A body that cannot match the empty string cannot stall, so it needs no
  // guard. Emitting one anyway would be two dead instructions per loop and -
  // the reason this is a decision rather than a tidy-up - would make the
  // program unmemoizable: a progress register is history the bit-state
  // engine's (pc, position) key does not capture.
  int guard = grx_ir_can_match_empty(codegen->ir, body_index);
  uint32_t reg = guard ? codegen->registers++ : 0;

  uint32_t top = here(codegen);
  uint32_t split = GRX_INDEX_NONE;
  GRX_Result result = emit(codegen, GRX_OP_SPLIT, 0, 0, 0, node, &split);
  if (result != GRX_OK) {
    return result;
  }

  uint32_t body_start = here(codegen);
  result = GRX_OK;
  if (guard) {
    result = emit(codegen, GRX_OP_PROGRESS_SET, 0, reg, 0, node, NULL);
  }
  if (result == GRX_OK) {
    result = emit_capture_reset(codegen, node, body_index);
  }
  if (result == GRX_OK) {
    result = gen(codegen, body_index);
  }
  uint32_t check = GRX_INDEX_NONE;
  if (result == GRX_OK && guard) {
    result = emit(codegen, GRX_OP_PROGRESS_CHECK, node->empty_loop, reg, 0,
        node, &check);
  }
  if (result == GRX_OK) {
    result = emit(codegen, GRX_OP_JMP, 0, top, 0, node, NULL);
  }
  if (result != GRX_OK) {
    return result;
  }

  uint32_t exit_target = here(codegen);
  if (guard) {
    patch_y(codegen, check, exit_target);
  }
  if (lazy) {
    patch_x(codegen, split, exit_target);
    patch_y(codegen, split, body_start);
  }
  else {
    patch_x(codegen, split, body_start);
    patch_y(codegen, split, exit_target);
  }

  return GRX_OK;
}

/**
 * Generate a counted repetition by expansion.
 *
 *     <body> * min
 *     then, for each optional copy:  split body, trampoline
 *
 * The optional copies chain rather than nest, with every exit landing on one
 * trampoline, so `a{0,3}` can stop after any number of them and the number
 * of them is bounded only by `max_program_size`.
 */
static GRX_Result gen_repeat(Codegen * codegen, const GRX_IRNode * node) {
  uint32_t body = node->first_child;
  if (body == GRX_INDEX_NONE) {
    return fail(codegen, GRX_DIAG_INTERNAL, node);
  }

  int lazy = node->mode == GRX_REPEAT_LAZY;

  for (uint32_t i = 0; i < node->min; i++) {
    GRX_Result result = emit_capture_reset(codegen, node, body);
    if (result == GRX_OK) {
      result = gen(codegen, body);
    }
    if (result != GRX_OK) {
      return result;
    }
  }

  if (node->max == GRX_REPEAT_INF) {
    return gen_star(codegen, node, body);
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

  // One register for the whole expansion. Each copy sets it immediately
  // before its body and checks it immediately after, and a thread meets those
  // in that order, so the copies cannot tread on each other. A body that
  // cannot match empty needs none of it; see gen_star().
  int guard = grx_ir_can_match_empty(codegen->ir, body);
  uint32_t reg = guard ? codegen->registers++ : 0;

  for (uint32_t i = 0; i < optional; i++) {
    uint32_t split = GRX_INDEX_NONE;
    result = emit(codegen, GRX_OP_SPLIT, 0, 0, 0, node, &split);
    if (result != GRX_OK) {
      return result;
    }
    if (lazy) {
      patch_x(codegen, split, stub);
      patch_y(codegen, split, here(codegen));
    }
    else {
      patch_x(codegen, split, here(codegen));
      patch_y(codegen, split, stub);
    }

    // The minimum is satisfied by the time an optional copy runs, so the
    // dialect's empty-iteration rule applies to it exactly as it applies to
    // an unbounded loop. ECMA-262 says so by passing min = 0 into the
    // remaining RepeatMatcher; `(a|){1,2}` against "ab" is what tells the
    // two apart, and the guard is why group 1 comes out as "a" rather than
    // as the empty string the second iteration would have set it to.
    result = GRX_OK;
    if (guard) {
      result = emit(codegen, GRX_OP_PROGRESS_SET, 0, reg, 0, node, NULL);
    }
    if (result == GRX_OK) {
      result = emit_capture_reset(codegen, node, body);
    }
    if (result == GRX_OK) {
      result = gen(codegen, body);
    }
    if (result == GRX_OK && guard) {
      result = emit(codegen, GRX_OP_PROGRESS_CHECK, node->empty_loop, reg,
          stub, node, NULL);
    }
    if (result != GRX_OK) {
      return result;
    }
  }

  patch_x(codegen, stub, here(codegen));
  return GRX_OK;
}

/** Generate a lookaround: a sub-program run without consuming input. */
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
 */
static GRX_Result gen_cond(Codegen * codegen, const GRX_IRNode * node) {
  uint32_t test = GRX_INDEX_NONE;
  GRX_Result result
      = emit(codegen, GRX_OP_COND, node->mode, node->a, 0, node, &test);
  if (result != GRX_OK) {
    return result;
  }

  uint32_t yes = node->first_child;
  uint32_t no = GRX_INDEX_NONE;
  if (yes != GRX_INDEX_NONE) {
    const GRX_IRNode * taken = grx_ir_node(codegen->ir, yes);
    no = taken ? taken->next_sibling : GRX_INDEX_NONE;
    result = gen(codegen, yes);
    if (result != GRX_OK) {
      return result;
    }
  }

  uint32_t skip = GRX_INDEX_NONE;
  result = emit(codegen, GRX_OP_JMP, 0, 0, 0, node, &skip);
  if (result != GRX_OK) {
    return result;
  }

  patch_y(codegen, test, here(codegen));
  if (no != GRX_INDEX_NONE) {
    result = gen(codegen, no);
    if (result != GRX_OK) {
      return result;
    }
  }

  patch_x(codegen, skip, here(codegen));
  return GRX_OK;
}

/**
 * Emit a call to a group, and remember to lay that group's block out later.
 *
 * Whether the call is atomic is not decided here: PCRE2's is and Perl's is
 * not, which makes it a dialect choice, and lowering has already spent it by
 * wrapping this node in GRX_IR_ATOMIC where it applies.
 */
static GRX_Result gen_call(Codegen * codegen, const GRX_IRNode * node) {
  if (codegen->fixup_count
      >= sizeof(codegen->fixups) / sizeof(*codegen->fixups)) {
    return fail(codegen, GRX_DIAG_LIMIT_PROGRAM_SIZE, node);
  }

  uint32_t call = GRX_INDEX_NONE;
  GRX_Result result = emit(
      codegen, GRX_OP_CALL, 0, GRX_INDEX_NONE, node->a, node, &call);
  if (result != GRX_OK) {
    return result;
  }

  codegen->fixups[codegen->fixup_count] = call;
  codegen->fixup_group[codegen->fixup_count] = node->a;
  codegen->fixup_count++;
  return GRX_OK;
}

static GRX_Result gen_look(Codegen * codegen, const GRX_IRNode * node) {
  uint32_t look = GRX_INDEX_NONE;
  GRX_Result result
      = emit(codegen, GRX_OP_LOOK, node->mode, 0, 0, node, &look);
  if (result != GRX_OK) {
    return result;
  }

  patch_x(codegen, look, here(codegen));
  result = gen(codegen, node->first_child);
  if (result != GRX_OK) {
    return result;
  }

  // The body ends in a MATCH so that the sub-program has a success state of
  // its own; the LOOK's `y` is where the outer program resumes.
  uint32_t done = GRX_INDEX_NONE;
  result = emit(codegen, GRX_OP_MATCH, 0, 0, 0, node, &done);
  if (result != GRX_OK) {
    return result;
  }

  patch_y(codegen, look, here(codegen));
  return GRX_OK;
}

static GRX_Result gen(Codegen * codegen, uint32_t node_index) {
  const GRX_IRNode * node = grx_ir_node(codegen->ir, node_index);
  if (!node) {
    return fail(codegen, GRX_DIAG_INTERNAL, NULL);
  }

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
      return gen_concat(codegen, node);

    case GRX_IR_ALTERNATE:
      return gen_alternate(codegen, node);

    case GRX_IR_REPEAT:
      return gen_repeat(codegen, node);

    case GRX_IR_CAPTURE: {
      // The slots are twice the group number and one more, so group 0's
      // start is slot 0 and the whole match is slots 0 and 1. A reversed
      // capture writes its end first, because that is the boundary its body
      // reaches first.
      uint32_t first = node->a * 2 + (reversed(node) ? 1u : 0u);
      uint32_t second = node->a * 2 + (reversed(node) ? 0u : 1u);
      GRX_Result result
          = emit(codegen, GRX_OP_SAVE, 0, first, 0, node, NULL);
      if (result == GRX_OK) {
        result = gen(codegen, node->first_child);
      }
      if (result == GRX_OK) {
        result = emit(codegen, GRX_OP_SAVE, 0, second, 0, node, NULL);
      }
      return result;
    }

    case GRX_IR_ASSERT:
      return emit(codegen, GRX_OP_ASSERT, node->mode, node->a, 0, node, NULL);

    case GRX_IR_LOOK:
      return gen_look(codegen, node);

    case GRX_IR_ATOMIC: {
      uint32_t begin = GRX_INDEX_NONE;
      GRX_Result result
          = emit(codegen, GRX_OP_ATOMIC_BEGIN, 0, 0, 0, node, &begin);
      if (result == GRX_OK) {
        result = gen(codegen, node->first_child);
      }
      uint32_t end = GRX_INDEX_NONE;
      if (result == GRX_OK) {
        result = emit(codegen, GRX_OP_ATOMIC_END, 0, 0, 0, node, &end);
      }
      if (result == GRX_OK) {
        patch_x(codegen, begin, end);
      }
      return result;
    }

    case GRX_IR_BACKREF:
      return emit(codegen, GRX_OP_BACKREF, node->backref_unset, node->a,
          (node->flags & GRX_IR_CASELESS) ? 1u : 0u, node, NULL);

    case GRX_IR_KEEP:
      return emit(codegen, GRX_OP_KEEP, 0, 0, 0, node, NULL);

    case GRX_IR_VERB:
      return emit(codegen, GRX_OP_VERB, node->mode, node->a, 0, node, NULL);

    case GRX_IR_COND:
      return gen_cond(codegen, node);

    case GRX_IR_RECURSE:
      return gen_call(codegen, node);

    case GRX_IR_COUNT:
    default:
      return fail(codegen, GRX_DIAG_INTERNAL, node);
  }
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
 */
static uint32_t call_target(const GRX_IR * ir, uint32_t group) {
  if (!group) {
    return ir->root;
  }

  for (size_t i = 0; i < ir->nodes.count; i++) {
    const GRX_IRNode * node = grx_ir_node(ir, (uint32_t)i);
    if (node && node->kind == GRX_IR_CAPTURE && node->a == group) {
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
  for (size_t next = 0; next < codegen->fixup_count;) {
    uint32_t group = codegen->fixup_group[next];

    size_t known = codegen->called_count;
    for (size_t i = 0; i < codegen->called_count; i++) {
      if (codegen->called[i] == group) {
        known = i;
        break;
      }
    }

    if (known == codegen->called_count) {
      if (codegen->called_count
          >= sizeof(codegen->called) / sizeof(*codegen->called)) {
        return fail(codegen, GRX_DIAG_LIMIT_PROGRAM_SIZE, NULL);
      }
      uint32_t target = call_target(codegen->ir, group);
      if (target == GRX_INDEX_NONE) {
        return fail(codegen, GRX_DIAG_INVALID_RECURSION, NULL);
      }

      // Reserved before the block is generated, so that a call the block
      // makes to its own group finds the entry already recorded rather than
      // starting a second copy of it.
      codegen->called[codegen->called_count] = group;
      codegen->entry[codegen->called_count] = here(codegen);
      codegen->called_count++;

      GRX_Result result = gen(codegen, target);
      if (result == GRX_OK) {
        result = emit(codegen, GRX_OP_RET, 0, 0, 0, NULL, NULL);
      }
      if (result != GRX_OK) {
        return result;
      }
      // Generating the block may have appended calls of its own; they are
      // after `next` and the loop reaches them without rescanning.
      continue;
    }

    patch_x(codegen, codegen->fixups[next], codegen->entry[known]);
    next++;
  }

  return GRX_OK;
}

GRX_Result grx_codegen_program(const GRX_IR * ir, const GRX_Limits * limits,
    GRX_Error * out_error, GRX_Program * out_program) {
  if (!ir || !limits || !out_program) {
    return GRX_ERR_INVALID;
  }

  Codegen codegen = {
    .ir = ir,
    .program = out_program,
    .limits = limits,
    .error = out_error,
    .registers = 0,
    .no_memo = 0,
    .called = {0},
    .entry = {0},
    .called_count = 0,
    .fixups = {0},
    .fixup_group = {0},
    .fixup_count = 0,
  };

  GRX_Result result = copy_classes(&codegen, &ir->classes);
  if (result != GRX_OK) {
    return result;
  }

  // Group 0 is the whole match, so the program brackets everything in its
  // two slots. An engine therefore never special-cases the overall span: it
  // is capture 0 and comes out of the same array as the rest.
  result = emit(&codegen, GRX_OP_SAVE, 0, 0, 0, NULL, NULL);
  if (result == GRX_OK && ir->root != GRX_INDEX_NONE) {
    result = gen(&codegen, ir->root);
  }
  if (result == GRX_OK) {
    result = emit(&codegen, GRX_OP_SAVE, 0, 1, 0, NULL, NULL);
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
  if (result != GRX_OK) {
    return result;
  }

  out_program->flags = ir->flags;
  if (codegen.no_memo) {
    out_program->flags |= GRX_PROGRAM_NO_MEMO;
  }
  out_program->preference = ir->preference;
  out_program->iteration = ir->iteration;
  out_program->register_count = codegen.registers;
  return GRX_OK;
}
