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
 * The one-pass engine: documentation/design.md section 3.5.5.
 *
 * It answers the question the lazy DFA leaves open. The DFA finds where a
 * match is and cannot say which group is which, so a search that wants
 * groups runs the Pike VM over the span the DFA found - and on a long match
 * that is the whole remaining cost, because the Pike VM pays a closure for
 * every byte of it whether or not anything is in doubt. `(a+)(b+)` over four
 * kilobytes of `a` is the case: the DFA settles the extent in one pass and
 * then thirteen thread-steps a byte go on the division of it, which a
 * backtracker reads off in one.
 *
 * So: for the programs where nothing *is* in doubt, read it off in one.
 *
 * A program is **one-pass** when, from every set of instructions the machine
 * can be in, each input byte leads to at most one instruction that accepts
 * it. Then the subject alone determines the whole path - every branch taken,
 * every GRX_OP_SAVE crossed - and the division of the match is not chosen
 * but forced. `(a+)(b+)` is one-pass: after an `a`, the byte says whether
 * the first group continues or the second begins. `(a+)(a+)` is not: after
 * an `a`, both do, and which one gets the byte is exactly the question the
 * Pike VM exists to answer.
 *
 * That property is what lets this sit under a leftmost-longest dialect at
 * all. documentation/dialects.md section 5.1 splits the four of them by how
 * the *groups* divide a match - POSIX BRE/ERE compare candidate divisions,
 * GNU BRE/ERE take the first path - and refuses the bit-state engine for the
 * same axis. Here there is no split to fall on: one path exists, so the
 * first path and the best path are the same path, and the two rules cannot
 * disagree about which one it is.
 *
 * The machine is a table. Each state is one program counter - the entry to a
 * closure, which is enough of a key precisely because the closure is
 * deterministic - and carries, for each of 256 bytes, the program counter to
 * go to and the list of capture slots to write on the way. Building it walks
 * the zero-width graph once per state and refuses the moment two
 * instructions want the same byte, or two paths to one instruction disagree
 * about what to save.
 *
 * What it will not run is deliberately narrow, and grx_onepass_eligible() is
 * the list: no assertion, because a table row cannot ask where it is; no
 * empty-width loop, because the walk would not terminate; nothing the lazy
 * DFA refuses either. A refusal costs one table build and the search falls
 * to the Pike VM as it did before.
 */

#include "exec_internal.h"
#include "../compile/compile_internal.h"
#include <string.h>

/* One state is 256 transitions plus 256 action indices. Both are 16-bit, so
 * a row is a kilobyte and the cap below is a quarter of a megabyte - the
 * point being that this is built eagerly and in full, unlike the DFA's
 * cache, so the bound has to be one a caller can afford to have been paid
 * for a program that then never uses it. */
#define ONEPASS_MAX_STATES 256
/* Program counters are held as uint16_t in the table. */
#define ONEPASS_MAX_INSTS 65534u
#define PC_NONE 0xFFFFu
/* Slots written on one transition. A longer list is a program with more
 * nesting than this is worth building for. */
#define ONEPASS_MAX_ACTION 64
/* Instructions one state's walk may visit, and the whole build. A
 * diamond-shaped program can be walked exponentially many ways, all of which
 * agree, so without a bound the build pays for that instead of refusing.
 *
 * Two bounds and not one, because they hold different things. The per-state
 * one also bounds the recursion: the cycle check keeps each instruction on
 * the stack at most once, so the depth is bounded by the node count, and a
 * program with tens of thousands of zero-width instructions would otherwise
 * put that many frames on the C stack. The total one bounds the build's
 * cost, which the per-state bound alone does not - it is reset for each of
 * up to 256 states. */
#define ONEPASS_MAX_VISITS 4096
#define ONEPASS_MAX_TOTAL_VISITS 262144

typedef struct {
  /* A program counter while the table is being built, and the row that
   * counter's closure became once it is: the run loop reads one array per
   * byte rather than chasing a counter through a second one, and the two
   * numbers are never both needed at once. PC_NONE in either meaning is
   * "no transition"; a row index never reaches it, because there are at
   * most ONEPASS_MAX_STATES rows. */
  uint16_t next[256];
  uint16_t act[256];    /* Index into `acts`; 0 is the empty action. */
  uint16_t accept_act;
  uint8_t accepts;
} Row;

struct GRX_OnePass {
  const GRX_Allocator * allocator;
  const GRX_Program * program;
  Row * rows;
  size_t row_count;
  /* pc -> row index, or PC_NONE. */
  uint16_t * row_of;
  /* Action lists, back to back; `at[i]` is where action i starts and
   * `at[i + 1]` where it ends, so action 0 is empty by construction. */
  uint16_t * slots;
  size_t slot_count;
  size_t slot_capacity;
  uint32_t * at;
  size_t act_count;
  size_t act_capacity;
  /* Build-time only. */
  uint8_t * on_stack;
  size_t visits;
  size_t total_visits;
};

/* ------------------------------------------------------------------ */
/* Eligibility.                                                         */
/* ------------------------------------------------------------------ */

static int allowed_opcode(const GRX_Inst * inst) {
  switch ((GRX_Opcode)inst->op) {
    case GRX_OP_MATCH:
    case GRX_OP_SPLIT:
    case GRX_OP_JMP:
      return 1;
    case GRX_OP_SAVE:
      /* Vim's `\ze`: a save that writes only where the slot is still unset
       * is a decision about what has already been written, which a
       * transition's action list has no way to express. Refused rather than
       * written as an unconditional store. */
      return !(inst->flags & GRX_INST_SAVE_IF_UNSET)
          && !(inst->flags & GRX_INST_REVERSE);
    case GRX_OP_CHAR:
    case GRX_OP_CLASS:
    case GRX_OP_ANY:
    case GRX_OP_ANY_NL:
      /* Reversed, or refusing a position rather than a byte: neither is a
       * function of the byte alone, and a table row is. */
      return !(inst->flags & GRX_INST_NEWLINE_CRLF)
          && !(inst->flags & GRX_INST_REVERSE);
    default:
      /* Everything else - assertions, the progress guards, atomic groups,
       * resets, callouts, backreferences, lookaround - either reads the
       * position or carries state a table cannot hold. Refused rather than
       * approximated: an approximation here changes which group gets a byte,
       * which is the one thing this engine exists to decide. */
      return 0;
  }
}

int grx_onepass_eligible(const GRX_Program * program) {
  if (!program || !program->insts.count) {
    return 0;
  }
  if (program->flags & GRX_PROGRAM_UTF) {
    return 0;
  }
  if (program->insts.count > ONEPASS_MAX_INSTS) {
    return 0;
  }
  for (size_t i = 0; i < program->insts.count; i++) {
    const GRX_Inst * inst
        = GRX_ARENA_AT(const GRX_Inst, &program->insts, (uint32_t)i);
    if (!inst || !allowed_opcode(inst)) {
      return 0;
    }
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* Building.                                                            */
/* ------------------------------------------------------------------ */

static void byte_set_from(const GRX_Program * program, const GRX_Inst * inst,
    uint8_t * out) {
  memset(out, 0, 32);
  switch ((GRX_Opcode)inst->op) {
    case GRX_OP_CHAR:
      if (inst->x < 256u) {
        out[inst->x >> 3] = (uint8_t)(1u << (inst->x & 7u));
      }
      return;
    case GRX_OP_CLASS:
      for (unsigned b = 0; b < 256u; b++) {
        if (inst->x != GRX_INDEX_NONE
            && grx_class_table_contains(&program->classes, inst->x, b)) {
          out[b >> 3] |= (uint8_t)(1u << (b & 7u));
        }
      }
      return;
    case GRX_OP_ANY:
      for (unsigned b = 0; b < 256u; b++) {
        if (inst->x == GRX_INDEX_NONE
            || !grx_class_table_contains(&program->classes, inst->x, b)) {
          out[b >> 3] |= (uint8_t)(1u << (b & 7u));
        }
      }
      return;
    case GRX_OP_ANY_NL:
      memset(out, 0xFF, 32);
      return;
    default:
      return;
  }
}

/* Intern one action list, so that two transitions carrying the same writes
 * carry the same index and the conflict test below is an integer compare. */
static int intern_action(GRX_OnePass * op, const uint16_t * list, size_t count,
    uint16_t * out) {
  for (size_t i = 0; i < op->act_count; i++) {
    size_t begin = op->at[i];
    size_t end = op->at[i + 1];
    if (end - begin == count
        && (!count
            || !memcmp(op->slots + begin, list, count * sizeof *list))) {
      *out = (uint16_t)i;
      return 1;
    }
  }
  if (op->act_count + 1 >= 0xFFFFu) {
    return 0;
  }
  if (op->slot_count + count > op->slot_capacity) {
    size_t want = op->slot_capacity ? op->slot_capacity * 2 : 64;
    while (want < op->slot_count + count) {
      want *= 2;
    }
    uint16_t * grown = gcu_allocator_realloc(
        op->allocator, op->slots, want * sizeof *grown);
    if (!grown) {
      return 0;
    }
    op->slots = grown;
    op->slot_capacity = want;
  }
  if (op->act_count + 2 > op->act_capacity) {
    size_t want = op->act_capacity ? op->act_capacity * 2 : 32;
    uint32_t * grown
        = gcu_allocator_realloc(op->allocator, op->at, want * sizeof *grown);
    if (!grown) {
      return 0;
    }
    op->at = grown;
    op->act_capacity = want;
  }
  memcpy(op->slots + op->slot_count, list, count * sizeof *list);
  op->slot_count += count;
  *out = (uint16_t)op->act_count;
  op->act_count++;
  op->at[op->act_count] = (uint32_t)op->slot_count;
  return 1;
}

/* Make a row for `pc`, or find the one it already has. Zero on failure, and
 * that includes the state cap: the table is built in full, so running past
 * the cap is a refusal and not a flush. */
static int row_for(GRX_OnePass * op, uint32_t pc, uint16_t * out) {
  if (op->row_of[pc] != PC_NONE) {
    *out = op->row_of[pc];
    return 1;
  }
  if (op->row_count >= ONEPASS_MAX_STATES) {
    return 0;
  }
  uint16_t index = (uint16_t)op->row_count++;
  Row * row = &op->rows[index];
  memset(row->next, 0xFF, sizeof row->next);
  memset(row->act, 0, sizeof row->act);
  row->accepts = 0;
  row->accept_act = 0;
  op->row_of[pc] = index;
  *out = index;
  return 1;
}

/*
 * Walk the zero-width graph from `pc`, filling `row`.
 *
 * `list`/`count` is the run of capture slots crossed to get here, which is
 * what the transition this walk is about to write will carry. Returns zero
 * the moment the program turns out not to be one-pass - two instructions
 * wanting one byte, or two ways to one instruction that disagree about what
 * to save - and that is a refusal for the whole program rather than for this
 * state, because a table with a hole in it is not a table.
 */
static int walk(GRX_OnePass * op, uint32_t pc, uint16_t row_index,
    uint16_t * list, size_t count, uint32_t * pending, size_t * pending_count) {
  /* Before `on_stack[pc]`, which is sized by the program: a jump target is
   * read out of the instruction and this is the only thing standing between
   * a malformed one and an out-of-bounds read. The compiler does not emit
   * one; that is a reason to check cheaply, not a reason not to. */
  if (pc >= op->program->insts.count) {
    return 0;
  }
  if (++op->visits > ONEPASS_MAX_VISITS
      || ++op->total_visits > ONEPASS_MAX_TOTAL_VISITS) {
    return 0;
  }
  if (op->on_stack[pc]) {
    /* A zero-width cycle: an empty-width loop, which this does not try to
     * unroll. The Pike VM's progress guard is what handles those, and it is
     * an opcode this engine already refuses. */
    return 0;
  }
  op->on_stack[pc] = 1;
  int ok = 1;
  const GRX_Inst * inst = GRX_ARENA_AT(const GRX_Inst, &op->program->insts, pc);
  if (!inst) {
    op->on_stack[pc] = 0;
    return 0;
  }
  Row * row = &op->rows[row_index];
  switch ((GRX_Opcode)inst->op) {
    case GRX_OP_MATCH: {
      uint16_t action = 0;
      if (!intern_action(op, list, count, &action)) {
        ok = 0;
        break;
      }
      /* Re-read: interning can reallocate nothing here, but the row pointer
       * is held across a call either way and the DFA's transition array
       * taught this file that lesson once already. */
      row = &op->rows[row_index];
      if (row->accepts && row->accept_act != action) {
        ok = 0;
        break;
      }
      row->accepts = 1;
      row->accept_act = action;
      break;
    }
    case GRX_OP_JMP:
      ok = walk(op, inst->x, row_index, list, count, pending, pending_count);
      break;
    case GRX_OP_SPLIT:
      ok = walk(op, inst->x, row_index, list, count, pending, pending_count)
          && walk(op, inst->y, row_index, list, count, pending, pending_count);
      break;
    case GRX_OP_SAVE:
      if (count >= ONEPASS_MAX_ACTION) {
        ok = 0;
        break;
      }
      if (inst->x > 0xFFFFu) {
        ok = 0;
        break;
      }
      list[count] = (uint16_t)inst->x;
      ok = walk(
          op, pc + 1, row_index, list, count + 1, pending, pending_count);
      break;
    case GRX_OP_CHAR:
    case GRX_OP_CLASS:
    case GRX_OP_ANY:
    case GRX_OP_ANY_NL: {
      uint16_t action = 0;
      if (!intern_action(op, list, count, &action)) {
        ok = 0;
        break;
      }
      row = &op->rows[row_index];
      uint8_t bytes[32];
      byte_set_from(op->program, inst, bytes);
      uint16_t to = (uint16_t)(pc + 1);
      int fresh = 0;
      for (unsigned b = 0; b < 256u; b++) {
        if (!(bytes[b >> 3] & (1u << (b & 7u)))) {
          continue;
        }
        if (row->next[b] != PC_NONE) {
          /* Two instructions in one closure want this byte. If they agree
           * about everything - the same successor, the same writes - the
           * program is still one-pass and this is a diamond rejoining;
           * otherwise it is the ambiguity this engine cannot resolve. */
          if (row->next[b] != to || row->act[b] != action) {
            ok = 0;
            break;
          }
          continue;
        }
        row->next[b] = to;
        row->act[b] = action;
        fresh = 1;
      }
      if (ok && fresh) {
        if (pc + 1 >= op->program->insts.count) {
          ok = 0;
          break;
        }
        pending[(*pending_count)++] = pc + 1;
      }
      break;
    }
    default:
      ok = 0;
      break;
  }
  op->on_stack[pc] = 0;
  return ok;
}

GRX_OnePass * grx_onepass_create(
    const GRX_Allocator * allocator, const GRX_Program * program) {
  if (!grx_onepass_eligible(program)) {
    return NULL;
  }
  GRX_OnePass * op = gcu_allocator_calloc(allocator, 1, sizeof *op);
  if (!op) {
    return NULL;
  }
  op->allocator = allocator;
  op->program = program;
  op->rows = gcu_allocator_calloc(
      allocator, ONEPASS_MAX_STATES, sizeof *op->rows);
  op->row_of = gcu_allocator_calloc(
      allocator, program->insts.count, sizeof *op->row_of);
  op->on_stack = gcu_allocator_calloc(
      allocator, program->insts.count, sizeof *op->on_stack);
  uint16_t * list
      = gcu_allocator_calloc(allocator, ONEPASS_MAX_ACTION, sizeof *list);
  /* One entry per instruction is the most a single state's walk can queue,
   * because it queues a successor the first time a byte reaches it. */
  uint32_t * pending = gcu_allocator_calloc(
      allocator, program->insts.count + 1, sizeof *pending);
  uint32_t * queue = gcu_allocator_calloc(
      allocator, ONEPASS_MAX_STATES, sizeof *queue);
  if (!op->rows || !op->row_of || !op->on_stack || !list || !pending
      || !queue) {
    goto fail;
  }
  memset(op->row_of, 0xFF, program->insts.count * sizeof *op->row_of);
  /* Action 0 is the empty list, and `at` needs its first two bounds before
   * anything is interned. */
  op->at = gcu_allocator_calloc(allocator, 32, sizeof *op->at);
  if (!op->at) {
    goto fail;
  }
  op->act_capacity = 32;
  op->at[0] = 0;
  op->at[1] = 0;
  op->act_count = 1;

  uint16_t start = 0;
  if (!row_for(op, 0, &start) || start != 0) {
    goto fail;
  }
  size_t head = 0;
  size_t tail = 0;
  queue[tail++] = 0;
  while (head < tail) {
    uint32_t pc = queue[head++];
    uint16_t index = op->row_of[pc];
    size_t pending_count = 0;
    op->visits = 0;
    if (!walk(op, pc, index, list, 0, pending, &pending_count)) {
      goto fail;
    }
    for (size_t i = 0; i < pending_count; i++) {
      if (op->row_of[pending[i]] != PC_NONE) {
        continue;
      }
      uint16_t made = 0;
      if (!row_for(op, pending[i], &made)) {
        goto fail;
      }
      if (tail >= ONEPASS_MAX_STATES) {
        goto fail;
      }
      queue[tail++] = pending[i];
    }
  }
  /* Program counters become row indices, once and here, so that the byte
   * loop below is one indexed read and not two. */
  for (size_t i = 0; i < op->row_count; i++) {
    for (unsigned b = 0; b < 256u; b++) {
      uint16_t to = op->rows[i].next[b];
      if (to != PC_NONE) {
        op->rows[i].next[b] = op->row_of[to];
      }
    }
  }
  gcu_allocator_free(allocator, list);
  gcu_allocator_free(allocator, pending);
  gcu_allocator_free(allocator, queue);
  gcu_allocator_free(allocator, op->on_stack);
  op->on_stack = NULL;
  return op;

fail:
  gcu_allocator_free(allocator, list);
  gcu_allocator_free(allocator, pending);
  gcu_allocator_free(allocator, queue);
  grx_onepass_free(op);
  return NULL;
}

void grx_onepass_free(GRX_OnePass * op) {
  if (!op) {
    return;
  }
  const GRX_Allocator * allocator = op->allocator;
  gcu_allocator_free(allocator, op->rows);
  gcu_allocator_free(allocator, op->row_of);
  gcu_allocator_free(allocator, op->on_stack);
  gcu_allocator_free(allocator, op->slots);
  gcu_allocator_free(allocator, op->at);
  gcu_allocator_free(allocator, op);
}

size_t grx_onepass_states(const GRX_OnePass * op) {
  return op ? op->row_count : 0;
}

/* ------------------------------------------------------------------ */
/* Running.                                                             */
/* ------------------------------------------------------------------ */

static void apply(const GRX_OnePass * op, uint16_t action, size_t position,
    GRX_Capture * captures, size_t count) {
  size_t begin = op->at[action];
  size_t end = op->at[action + 1];
  for (size_t i = begin; i < end; i++) {
    size_t slot = op->slots[i];
    size_t group = slot >> 1;
    if (group >= count) {
      continue;
    }
    if (slot & 1u) {
      captures[group].end = position;
    }
    else {
      captures[group].start = position;
    }
  }
}

int grx_onepass_run(const GRX_OnePass * op, const char * subject, size_t begin,
    size_t finish, GRX_Capture * captures, size_t count) {
  if (!op || finish < begin) {
    return -1;
  }
  uint16_t state = 0;
  for (size_t pos = begin; pos < finish; pos++) {
    const Row * row = &op->rows[state];
    unsigned byte = (unsigned char)subject[pos];
    uint16_t to = row->next[byte];
    if (to == PC_NONE) {
      /* The span came from the DFA, so this cannot happen for a correct
       * table - and if it ever does, the answer is the Pike VM's rather than
       * a half-filled set of groups. */
      return -1;
    }
    if (row->act[byte]) {
      apply(op, row->act[byte], pos, captures, count);
    }
    state = to;
  }
  const Row * row = &op->rows[state];
  if (!row->accepts) {
    return -1;
  }
  apply(op, row->accept_act, finish, captures, count);
  return 1;
}
