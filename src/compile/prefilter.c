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
 * Which bytes a match can begin with, so that a search can skip the rest.
 *
 * An unanchored search asks the engine to start at every position in turn.
 * That is what makes the time linear in the subject *and* what makes it slow:
 * `xyzzy` against a megabyte of `a` ran the thread set a million times to
 * discover a million times that `a` is not `x`. Every other engine skips -
 * glibc and Python with a memchr, V8 with generated code - and measured on
 * the same failing search this library was 27 ns per subject byte where Node
 * was 0.014 and glibc 0.47, a factor of nineteen hundred against the best of
 * them (notes/regex/TODO.md section 14x).
 *
 * Nothing about a linear-time engine requires that. What it needs is the set
 * of bytes a match can start with, and a position whose byte is not in it can
 * be skipped without running anything.
 *
 * **The claim this file makes, and the only one that matters:** if the set is
 * *known*, then every byte at which a match can begin is in it. A superset is
 * correct and merely slower; a subset loses matches. So every instruction the
 * walk does not understand gives up on the whole set rather than contributing
 * nothing to it, and an assertion is walked *past* rather than interpreted -
 * ignoring a condition can only add starts, never remove one.
 *
 * **Why the program and not the tree.** The walk is over compiled
 * instructions, which is the one form every engine agrees on: the Pike VM,
 * the bit-state memo and the backtracker all execute this and nothing else,
 * so one answer serves all three and cannot drift from what they run.
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../charclass/charclass_internal.h"
#include "../core/core_internal.h"
#include "../unicode/unicode_internal.h"
#include "compile_internal.h"

/** Set one byte in a 256-bit set. */
static void byte_set_add(unsigned char * set, unsigned int byte) {
  set[byte >> 3] |= (unsigned char)(1u << (byte & 7u));
}

/** Set an inclusive run of bytes. */
static void byte_set_add_range(
    unsigned char * set, unsigned int low, unsigned int high) {
  for (unsigned int byte = low; byte <= high && byte < 256u; byte++) {
    byte_set_add(set, byte);
  }
}

/**
 * The first byte of `codepoint`'s UTF-8 encoding.
 *
 * Monotonic in the code point within each length class, which is what lets a
 * range be reduced to a run of leading bytes instead of being enumerated.
 */
static unsigned int utf8_lead(uint32_t codepoint) {
  if (codepoint < 0x80u) {
    return codepoint;
  }
  if (codepoint < 0x800u) {
    return 0xC0u | (codepoint >> 6);
  }
  if (codepoint < 0x10000u) {
    return 0xE0u | (codepoint >> 12);
  }
  return 0xF0u | (codepoint >> 18);
}

/** Every byte a code point in [low, high] can begin with. */
static void add_leading_bytes(
    unsigned char * set, uint32_t low, uint32_t high, int utf) {
  if (high < low) {
    return;
  }
  if (!utf) {
    // Without UTF the engines step bytes, and a code point above 0xFF is
    // something no byte can be - such a range contributes nothing, which is
    // not the same as the set being unknown.
    if (low > 0xFFu) {
      return;
    }
    byte_set_add_range(set, low, high > 0xFFu ? 0xFFu : high);
    return;
  }

  // Split at the encoding-length boundaries, because `utf8_lead` is only
  // monotonic inside one length class: U+07FF leads with 0xDF and U+0800 with
  // 0xE0, but U+FFFF leads with 0xEF and U+10000 with 0xF0.
  static const uint32_t bounds[] = {0x80u, 0x800u, 0x10000u, 0x110000u};
  uint32_t at = low;
  for (size_t i = 0; i < sizeof bounds / sizeof *bounds && at <= high; i++) {
    if (at >= bounds[i]) {
      continue;
    }
    uint32_t piece_end = high < bounds[i] - 1u ? high : bounds[i] - 1u;
    byte_set_add_range(set, utf8_lead(at), utf8_lead(piece_end));
    at = piece_end + 1u;
  }
}

/** Push a program counter if it has not been walked yet. */
static int push_pc(uint32_t * stack, size_t * depth, size_t capacity,
    unsigned char * seen, uint32_t pc, size_t count) {
  if (pc == GRX_INDEX_NONE || pc >= count) {
    return 0;
  }
  if (seen[pc >> 3] & (unsigned char)(1u << (pc & 7u))) {
    return 1;
  }
  seen[pc >> 3] |= (unsigned char)(1u << (pc & 7u));
  if (*depth >= capacity) {
    return 0;
  }
  stack[(*depth)++] = pc;
  return 1;
}

int grx_program_first_bytes(
    const GRX_Program * program, unsigned char * out_set) {
  if (!program || !out_set) {
    return 0;
  }
  memset(out_set, 0, GRX_FIRST_BYTES_SIZE);

  size_t count = program->insts.count;
  if (!count) {
    return 0;
  }
  int utf = (program->flags & GRX_PROGRAM_UTF) != 0;

  // One bit per instruction for "walked", one slot per instruction for the
  // stack. Both on the heap for the reason codegen's frames are: the depth
  // here is the program's and the program is bounded by the caller's limits,
  // not by anything this file could pick.
  const GRX_Allocator * allocator = program->insts.allocator;
  unsigned char * seen = gcu_allocator_calloc(allocator, (count + 7) / 8, 1);
  uint32_t * stack = gcu_allocator_calloc(allocator, count, sizeof *stack);
  if (!seen || !stack) {
    gcu_allocator_free(allocator, seen);
    gcu_allocator_free(allocator, stack);
    return 0;
  }

  int known = 1;
  size_t depth = 0;
  if (!push_pc(stack, &depth, count, seen, 0, count)) {
    known = 0;
  }

  while (known && depth) {
    uint32_t pc = stack[--depth];
    const GRX_Inst * inst = GRX_ARENA_AT(const GRX_Inst, &program->insts, pc);
    if (!inst) {
      known = 0;
      break;
    }

    // A non-atomic lookbehind is *inlined* rather than run as a sub-match -
    // that is what makes it non-atomic - so its body's instructions sit on the
    // main path and consume backwards. What they consume is not at the start
    // position: `(?<*ab)c` puts `char 'b' reverse` first, and a set built from
    // it would say {b} where a match begins with `c`. Nothing here can tell
    // how far back such an instruction reads, so the whole set goes.
    //
    // Found by Lower.WhichLookbehindModelIsTheDialectsAndNotTheEngines, which
    // had the case written down before this pass existed.
    if (inst->flags & GRX_INST_REVERSE) {
      known = 0;
      break;
    }

    switch (inst->op) {
      // Consuming. The walk stops here on this path: whatever this can begin
      // with is what a match starting here begins with.
      case GRX_OP_CHAR:
        add_leading_bytes(out_set, inst->x, inst->x, utf);
        break;

      case GRX_OP_CLASS: {
        size_t ranges = 0;
        const GRX_CharRange * range
            = grx_class_table_get(&program->classes, inst->x, &ranges);
        if (!range && ranges) {
          known = 0;
          break;
        }
        for (size_t i = 0; i < ranges; i++) {
          add_leading_bytes(out_set, range[i].low, range[i].high, utf);
        }
        break;
      }

      // `.` and its kin can begin with very nearly every byte, so a set that
      // held them would be a test that never skips and always costs. Saying
      // "unknown" is the same answer with none of the work.
      case GRX_OP_ANY:
      case GRX_OP_ANY_NL:
        known = 0;
        break;

      // A match that consumed nothing starts everywhere.
      case GRX_OP_MATCH:
        known = 0;
        break;

      // Two continuations.
      case GRX_OP_SPLIT:
        if (!push_pc(stack, &depth, count, seen, inst->x, count)
            || !push_pc(stack, &depth, count, seen, inst->y, count)) {
          known = 0;
        }
        break;

      case GRX_OP_JMP:
        if (!push_pc(stack, &depth, count, seen, inst->x, count)) {
          known = 0;
        }
        break;

      // Zero-width and one continuation: step over it. An assertion is not
      // interpreted - a start it would have refused stays in the set, which
      // is a superset and so still correct.
      case GRX_OP_SAVE:
      case GRX_OP_ASSERT:
      case GRX_OP_PROGRESS_SET:
      case GRX_OP_RESET:
      case GRX_OP_RESET_STALE:
      case GRX_OP_ATOMIC_BEGIN:
      case GRX_OP_ATOMIC_END:
      case GRX_OP_KEEP:
      case GRX_OP_KEEP_END:
      case GRX_OP_CALLOUT:
        if (!push_pc(stack, &depth, count, seen, pc + 1, count)) {
          known = 0;
        }
        break;

      // Zero-width with a second way out.
      case GRX_OP_PROGRESS_CHECK:
      case GRX_OP_COND:
        if (!push_pc(stack, &depth, count, seen, pc + 1, count)
            || !push_pc(stack, &depth, count, seen, inst->y, count)) {
          known = 0;
        }
        break;

      // A lookaround consumes nothing of the match itself, so the walk
      // continues after the body and the body is not entered. Skipping it can
      // only widen the set: a positive assertion's own first byte would have
      // narrowed the answer and a negative one says nothing about this
      // position at all. The two-jump form is not read here, because which of
      // the two continues the match is the thing this would have to know.
      case GRX_OP_LOOK:
        if ((inst->flags & GRX_INST_COND_ELSE)
            || !push_pc(stack, &depth, count, seen, inst->y, count)) {
          known = 0;
        }
        break;

      // A backreference can match the empty string, a subroutine call can
      // reach anything, and the rest consume in ways this does not model.
      // Every one of them gives up on the whole set rather than being left
      // out of it.
      default:
        known = 0;
        break;
    }
  }

  gcu_allocator_free(allocator, seen);
  gcu_allocator_free(allocator, stack);

  if (!known) {
    memset(out_set, 0, GRX_FIRST_BYTES_SIZE);
    return 0;
  }

  // An empty set means no byte can start a match, which is true only for a
  // program that cannot match at all. Reporting it as known would be correct
  // and would let a search skip the whole subject; reporting it as unknown is
  // what a pass this new should do with a case it has never seen.
  for (size_t i = 0; i < GRX_FIRST_BYTES_SIZE; i++) {
    if (out_set[i]) {
      return 1;
    }
  }
  return 0;
}

/**
 * Whether anything in the program moves the reported start or end away from
 * where the attempt began and ended.
 *
 * `\K` and vim's `\ze` both do, and both invalidate the two literal facts
 * rather than shifting them: what the attempt consumes first is no longer
 * what the *match* begins with. `ab\Kcd` reports a span starting at `c`, so
 * "ab" is not a prefix of any match of it even though every attempt consumes
 * it. The byte set does not have this problem because it is explicitly about
 * where an attempt may begin, and these two are about the match.
 *
 * A whole-program scan rather than a check on the walked path, because the
 * instruction that moves the start can sit after the literal that the walk
 * would otherwise have believed.
 */
static int start_or_end_moves(const GRX_Program * program) {
  size_t count = program->insts.count;
  for (size_t i = 0; i < count; i++) {
    const GRX_Inst * inst = GRX_ARENA_AT(const GRX_Inst, &program->insts, i);
    if (!inst) {
      return 1;
    }
    if (inst->op == GRX_OP_KEEP || inst->op == GRX_OP_KEEP_END) {
      return 1;
    }
  }
  return 0;
}

/**
 * Append one code point's bytes, or report that it does not fit.
 *
 * @return Non-zero when the whole code point was written.
 */
static int literal_append(
    char * out, size_t cap, size_t * written, uint32_t codepoint, int utf) {
  char encoded[8];
  size_t width;
  if (utf) {
    width = grx_unicode_utf8_encode(codepoint, encoded);
    if (!width) {
      return 0;
    }
  }
  else {
    if (codepoint > 0xFFu) {
      return 0;
    }
    encoded[0] = (char)(unsigned char)codepoint;
    width = 1;
  }
  if (*written + width > cap) {
    return 0;
  }
  memcpy(out + *written, encoded, width);
  *written += width;
  return 1;
}

size_t grx_program_literal_prefix(
    const GRX_Program * program, char * out, size_t cap) {
  if (!program || !out || !cap) {
    return 0;
  }
  size_t count = program->insts.count;
  if (!count || start_or_end_moves(program)) {
    return 0;
  }
  int utf = (program->flags & GRX_PROGRAM_UTF) != 0;

  size_t written = 0;
  uint32_t pc = 0;
  // The step cap is the program's length: a path that visits more
  // instructions than the program holds has gone round a loop, and a loop
  // means a second way onwards, which this walk stops at anyway.
  for (size_t step = 0; step < count; step++) {
    const GRX_Inst * inst = GRX_ARENA_AT(const GRX_Inst, &program->insts, pc);
    if (!inst || (inst->flags & GRX_INST_REVERSE)) {
      break;
    }

    if (inst->op == GRX_OP_CHAR) {
      if (!literal_append(out, cap, &written, inst->x, utf)) {
        break;
      }
      pc++;
      continue;
    }

    switch (inst->op) {
      // Zero-width and one way onwards. An assertion is stepped over rather
      // than interpreted: it can refuse a match but cannot change what one
      // consumes, so what follows is still what a match begins with.
      case GRX_OP_SAVE:
      case GRX_OP_ASSERT:
      case GRX_OP_PROGRESS_SET:
      case GRX_OP_RESET:
      case GRX_OP_RESET_STALE:
      case GRX_OP_ATOMIC_BEGIN:
      case GRX_OP_ATOMIC_END:
      case GRX_OP_CALLOUT:
        pc++;
        continue;

      case GRX_OP_JMP:
        pc = inst->x;
        continue;

      // A lookaround consumes nothing of the match, so the walk resumes after
      // the body. The two-jump form is not read, for the reason the byte set
      // does not read it: which of the two continues the match is the thing
      // this would have to know.
      case GRX_OP_LOOK:
        if (!(inst->flags & GRX_INST_COND_ELSE)) {
          pc = inst->y;
          continue;
        }
        break;

      // Everything else either branches, consumes a choice of code points, or
      // is not modelled here. Each of them ends the prefix, and ending it is
      // always safe: what has been written is still a prefix of every match.
      default:
        break;
    }
    // Anything that reached here neither consumed a known code point nor
    // found one way onwards, so the prefix ends.
    break;
  }

  return written;
}

/**
 * The instructions one instruction can continue to.
 *
 * Modelled conservatively in one direction only: an edge this does not know
 * about would make a node look like a dominator when a match can reach the
 * end without it, and that is the direction that loses matches. So an opcode
 * whose control flow is not spelled out here refuses the whole analysis
 * rather than being given no edges.
 *
 * `LOOK` is refused rather than modelled, and the reason is worth stating
 * because it looks over-cautious: a lookaround's body **ends in a MATCH of
 * its own**. Treating that as the program's end would let a node inside an
 * assertion's body dominate "every MATCH" while consuming text no match
 * contains.
 *
 * @return Non-zero when the opcode is modelled; the count is written to
 *   `out_count` and the targets to `out`.
 */
static int successors_of(const GRX_Program * program, uint32_t pc,
    const GRX_Inst * inst, uint32_t * out, size_t * out_count) {
  size_t next = program->insts.count;
  *out_count = 0;
  switch (inst->op) {
    case GRX_OP_MATCH:
      return 1;

    case GRX_OP_CHAR:
    case GRX_OP_CLASS:
    case GRX_OP_ANY:
    case GRX_OP_ANY_NL:
    case GRX_OP_SAVE:
    case GRX_OP_ASSERT:
    case GRX_OP_PROGRESS_SET:
    case GRX_OP_RESET:
    case GRX_OP_RESET_STALE:
    case GRX_OP_ATOMIC_BEGIN:
    case GRX_OP_ATOMIC_END:
    case GRX_OP_CALLOUT:
      if ((size_t)pc + 1 >= next) {
        return 0;
      }
      out[(*out_count)++] = pc + 1;
      return 1;

    case GRX_OP_JMP:
      out[(*out_count)++] = inst->x;
      return 1;

    case GRX_OP_SPLIT:
      out[(*out_count)++] = inst->x;
      out[(*out_count)++] = inst->y;
      return 1;

    case GRX_OP_PROGRESS_CHECK:
      if ((size_t)pc + 1 >= next) {
        return 0;
      }
      out[(*out_count)++] = pc + 1;
      out[(*out_count)++] = inst->y;
      return 1;

    default:
      return 0;
  }
}

/** Working storage for the dominator pass, so one failure path frees it all. */
typedef struct {
  uint32_t * post;      ///< Postorder -> pc.
  uint32_t * number;    ///< pc -> reverse-postorder index, or GRX_INDEX_NONE.
  uint32_t * order;     ///< Reverse-postorder index -> pc.
  uint32_t * idom;      ///< RPO index -> RPO index of its immediate dominator.
  uint32_t * pred_at;   ///< CSR offsets, one per RPO index plus a terminator.
  uint32_t * pred;      ///< CSR predecessors, as RPO indices.
  uint32_t * stack;     ///< DFS stack.
  uint8_t * state;      ///< DFS colour, then reused as a marker.
  uint32_t * succ_seen; ///< How many successors of a node the DFS has taken.
  size_t nodes;         ///< Reachable instructions.
} Dominators;

static void dominators_free(const GRX_Allocator * allocator, Dominators * d) {
  gcu_allocator_free(allocator, d->post);
  gcu_allocator_free(allocator, d->number);
  gcu_allocator_free(allocator, d->order);
  gcu_allocator_free(allocator, d->idom);
  gcu_allocator_free(allocator, d->pred_at);
  gcu_allocator_free(allocator, d->pred);
  gcu_allocator_free(allocator, d->stack);
  gcu_allocator_free(allocator, d->state);
  gcu_allocator_free(allocator, d->succ_seen);
  memset(d, 0, sizeof *d);
}

/** Cooper, Harvey and Kennedy's intersection, over reverse-postorder indices. */
static uint32_t dom_intersect(
    const uint32_t * idom, uint32_t left, uint32_t right) {
  while (left != right) {
    while (left > right) {
      left = idom[left];
    }
    while (right > left) {
      right = idom[right];
    }
  }
  return left;
}

/**
 * @brief How many bytes one instruction consumes, as a [min, max] range.
 *
 * Only the opcodes successors_of() models can reach this; everything else has
 * already refused the analysis. A class or a dot under UTF-8 is 1 to 4 bytes
 * because it is a code point and this does not look inside the class to see
 * whether it holds anything above U+007F - conservative, and conservative
 * here means a wider window, which is the safe direction.
 */
static void inst_width(
    const GRX_Inst * inst, int utf, size_t * out_min, size_t * out_max) {
  switch (inst->op) {
    case GRX_OP_CHAR: {
      if (!utf) {
        *out_min = *out_max = 1;
        return;
      }
      char encoded[8];
      size_t width = grx_unicode_utf8_encode(inst->x, encoded);
      *out_min = *out_max = width ? width : 1;
      return;
    }
    case GRX_OP_CLASS:
    case GRX_OP_ANY:
    case GRX_OP_ANY_NL:
      *out_min = 1;
      *out_max = utf ? 4 : 1;
      return;
    default:
      *out_min = *out_max = 0;
      return;
  }
}

size_t grx_program_required_literal(const GRX_Program * program, char * out,
    size_t cap, size_t * out_offset_min, size_t * out_offset_max) {
  if (out_offset_min) {
    *out_offset_min = 0;
  }
  if (out_offset_max) {
    *out_offset_max = GRX_NPOS;
  }
  if (!program || !out || !cap) {
    return 0;
  }
  size_t count = program->insts.count;
  if (!count || start_or_end_moves(program)) {
    return 0;
  }
  int utf = (program->flags & GRX_PROGRAM_UTF) != 0;
  const GRX_Allocator * allocator = program->insts.allocator;

  Dominators d;
  memset(&d, 0, sizeof d);
  d.post = gcu_allocator_calloc(allocator, count, sizeof *d.post);
  d.number = gcu_allocator_calloc(allocator, count, sizeof *d.number);
  d.order = gcu_allocator_calloc(allocator, count, sizeof *d.order);
  d.idom = gcu_allocator_calloc(allocator, count, sizeof *d.idom);
  d.pred_at = gcu_allocator_calloc(allocator, count + 1, sizeof *d.pred_at);
  d.pred = gcu_allocator_calloc(allocator, 2 * count, sizeof *d.pred);
  d.stack = gcu_allocator_calloc(allocator, count, sizeof *d.stack);
  d.state = gcu_allocator_calloc(allocator, count, 1);
  d.succ_seen = gcu_allocator_calloc(allocator, count, sizeof *d.succ_seen);
  if (!d.post || !d.number || !d.order || !d.idom || !d.pred_at || !d.pred
      || !d.stack || !d.state || !d.succ_seen) {
    dominators_free(allocator, &d);
    return 0;
  }
  for (size_t i = 0; i < count; i++) {
    d.number[i] = GRX_INDEX_NONE;
  }

  // Depth-first from the entry, iteratively, recording a postorder. `state`
  // is 0 for untouched, 1 for on the stack and 2 for finished.
  int modelled = 1;
  size_t posted = 0;
  size_t depth = 0;
  d.stack[depth++] = 0;
  d.state[0] = 1;
  while (depth) {
    uint32_t pc = d.stack[depth - 1];
    const GRX_Inst * inst = GRX_ARENA_AT(const GRX_Inst, &program->insts, pc);
    uint32_t succ[2];
    size_t succ_count = 0;
    if (!inst || !successors_of(program, pc, inst, succ, &succ_count)) {
      modelled = 0;
      break;
    }
    if (d.succ_seen[pc] < succ_count) {
      uint32_t next = succ[d.succ_seen[pc]++];
      if (next >= count) {
        modelled = 0;
        break;
      }
      if (!d.state[next]) {
        d.state[next] = 1;
        d.stack[depth++] = next;
      }
      continue;
    }
    d.state[pc] = 2;
    d.post[posted++] = pc;
    depth--;
  }
  if (!modelled) {
    dominators_free(allocator, &d);
    return 0;
  }

  // Reverse postorder: the entry is 0, and a node's index is always larger
  // than its dominator's, which is what dom_intersect() walks by.
  d.nodes = posted;
  for (size_t i = 0; i < posted; i++) {
    uint32_t pc = d.post[posted - 1 - i];
    d.order[i] = pc;
    d.number[pc] = (uint32_t)i;
  }

  // Predecessors, as two passes over the same edges: count, then fill. One
  // predicate written twice is how a count-then-fill pair drifts, so the edge
  // is produced by successors_of() in both.
  for (size_t i = 0; i < posted; i++) {
    uint32_t pc = d.order[i];
    const GRX_Inst * inst = GRX_ARENA_AT(const GRX_Inst, &program->insts, pc);
    uint32_t succ[2];
    size_t succ_count = 0;
    successors_of(program, pc, inst, succ, &succ_count);
    for (size_t s = 0; s < succ_count; s++) {
      uint32_t to = d.number[succ[s]];
      if (to != GRX_INDEX_NONE) {
        d.pred_at[to + 1]++;
      }
    }
  }
  for (size_t i = 0; i < posted; i++) {
    d.pred_at[i + 1] += d.pred_at[i];
  }
  uint32_t * fill = d.succ_seen;   // Finished with; reused as a cursor.
  for (size_t i = 0; i < posted; i++) {
    fill[i] = d.pred_at[i];
  }
  for (size_t i = 0; i < posted; i++) {
    uint32_t pc = d.order[i];
    const GRX_Inst * inst = GRX_ARENA_AT(const GRX_Inst, &program->insts, pc);
    uint32_t succ[2];
    size_t succ_count = 0;
    successors_of(program, pc, inst, succ, &succ_count);
    for (size_t s = 0; s < succ_count; s++) {
      uint32_t to = d.number[succ[s]];
      if (to != GRX_INDEX_NONE) {
        d.pred[fill[to]++] = (uint32_t)i;
      }
    }
  }

  // Cooper, Harvey and Kennedy: iterate to a fixed point in reverse
  // postorder. GRX_INDEX_NONE stands for "no immediate dominator yet".
  for (size_t i = 0; i < posted; i++) {
    d.idom[i] = GRX_INDEX_NONE;
  }
  d.idom[0] = 0;
  int changed = 1;
  while (changed) {
    changed = 0;
    for (size_t i = 1; i < posted; i++) {
      uint32_t candidate = GRX_INDEX_NONE;
      for (uint32_t e = d.pred_at[i]; e < d.pred_at[i + 1]; e++) {
        uint32_t p = d.pred[e];
        if (d.idom[p] == GRX_INDEX_NONE) {
          continue;
        }
        candidate = candidate == GRX_INDEX_NONE
            ? p : dom_intersect(d.idom, p, candidate);
      }
      if (candidate != GRX_INDEX_NONE && d.idom[i] != candidate) {
        d.idom[i] = candidate;
        changed = 1;
      }
    }
  }

  // The instructions on every path from the entry to every MATCH. Built as
  // the intersection of the dominator chains, which for this shape is
  // cheaper and simpler than a bitset per node.
  uint8_t * common = d.state;    // Finished with; reused as the marker.
  memset(common, 0, count);
  int seen_match = 0;
  for (size_t i = 0; i < posted; i++) {
    const GRX_Inst * inst
        = GRX_ARENA_AT(const GRX_Inst, &program->insts, d.order[i]);
    if (!inst || inst->op != GRX_OP_MATCH) {
      continue;
    }
    // Mark this MATCH's chain, then keep only what every chain so far marked.
    for (uint32_t at = (uint32_t)i;; at = d.idom[at]) {
      common[at] = seen_match ? (uint8_t)(common[at] | 2u) : (uint8_t)1u;
      if (at == 0) {
        break;
      }
    }
    if (seen_match) {
      for (size_t n = 0; n < posted; n++) {
        common[n] = (uint8_t)((common[n] & 2u) ? 1u : 0u);
      }
    }
    seen_match = 1;
  }
  if (!seen_match) {
    dominators_free(allocator, &d);
    return 0;
  }

  // How far from the start of a match each instruction can be, in bytes.
  // `lo` is the least over every path from the entry and `hi` the greatest,
  // with `unbounded` standing for a maximum there is none of. This is what
  // turns an occurrence of the literal into a *window* of start positions
  // instead of only a reason to search the subject at all.
  //
  // Both are conservative towards a wider window, which is the direction that
  // keeps start positions: a `lo` too small moves the window's right edge
  // right, a `hi` too large moves its left edge left, and neither can drop a
  // position a true window would have held.
  size_t * lo = gcu_allocator_calloc(allocator, posted, sizeof *lo);
  size_t * hi = gcu_allocator_calloc(allocator, posted, sizeof *hi);
  uint8_t * unbounded = gcu_allocator_calloc(allocator, posted, 1);
  int offsets = lo && hi && unbounded;
  if (offsets) {
    for (size_t i = 0; i < posted; i++) {
      lo[i] = GRX_NPOS;
    }
    lo[0] = 0;
    // A retreating edge in reverse postorder is the only way back to a node
    // already passed, so its target is where a repeat re-enters. Everything
    // reachable from there can be any number of iterations from the start,
    // and the maximum is given up rather than counted: a repeat that consumes
    // nothing would converge, but telling those apart is not worth a pass and
    // being wrong costs only the window.
    for (size_t i = 0; i < posted; i++) {
      const GRX_Inst * inst
          = GRX_ARENA_AT(const GRX_Inst, &program->insts, d.order[i]);
      uint32_t succ[2];
      size_t succ_count = 0;
      if (!inst || !successors_of(program, d.order[i], inst, succ,
              &succ_count)) {
        continue;
      }
      for (size_t sx = 0; sx < succ_count; sx++) {
        uint32_t to = d.number[succ[sx]];
        if (to != GRX_INDEX_NONE && to <= i) {
          unbounded[to] = 1;
        }
      }
    }
    // Relax to a fixed point rather than in one pass, because a shortest
    // path to a node may run through an edge that retreats in this order and
    // one pass would then report a minimum that is too large - the one
    // direction that is not safe. Not settling means no offsets at all.
    int settled = 0;
    for (size_t pass = 0; !settled && pass <= posted + 1; pass++) {
      settled = 1;
      for (size_t i = 0; i < posted; i++) {
        if (lo[i] == GRX_NPOS) {
          continue;
        }
        const GRX_Inst * inst
            = GRX_ARENA_AT(const GRX_Inst, &program->insts, d.order[i]);
        uint32_t succ[2];
        size_t succ_count = 0;
        if (!inst || !successors_of(program, d.order[i], inst, succ,
                &succ_count)) {
          continue;
        }
        size_t wmin = 0;
        size_t wmax = 0;
        inst_width(inst, utf, &wmin, &wmax);
        for (size_t sx = 0; sx < succ_count; sx++) {
          uint32_t to = d.number[succ[sx]];
          if (to == GRX_INDEX_NONE) {
            continue;
          }
          if (lo[i] + wmin < lo[to]) {
            lo[to] = lo[i] + wmin;
            settled = 0;
          }
          if (unbounded[i] && !unbounded[to]) {
            unbounded[to] = 1;
            settled = 0;
          }
          if (!unbounded[to] && hi[i] + wmax > hi[to]) {
            hi[to] = hi[i] + wmax;
            settled = 0;
          }
        }
      }
    }
    if (!settled) {
      offsets = 0;
    }
  }

  // The longest run of CHARs that a match must consume adjacently: each is
  // the only way on from the one before, and the only way into the one after.
  // The head has to dominate every MATCH; the rest then follow from it.
  size_t best = 0;
  int best_rank = -1;
  size_t best_lo = 0;
  size_t best_hi = GRX_NPOS;
  char candidate[GRX_LITERAL_MAX];
  size_t limit = cap < GRX_LITERAL_MAX ? cap : GRX_LITERAL_MAX;
  for (size_t i = 0; i < posted; i++) {
    if (!common[i]) {
      continue;
    }
    const GRX_Inst * head
        = GRX_ARENA_AT(const GRX_Inst, &program->insts, d.order[i]);
    if (!head || head->op != GRX_OP_CHAR
        || (head->flags & GRX_INST_REVERSE)) {
      continue;
    }

    size_t written = 0;
    size_t at = i;
    for (size_t step = 0; step <= posted; step++) {
      const GRX_Inst * inst
          = GRX_ARENA_AT(const GRX_Inst, &program->insts, d.order[at]);
      if (!inst || inst->op != GRX_OP_CHAR
          || (inst->flags & GRX_INST_REVERSE)
          || !literal_append(candidate, limit, &written, inst->x, utf)) {
        break;
      }
      // On to the next instruction that consumes, stepping over the ones
      // that do not. A SAVE between two characters does not put anything
      // between them in the subject, and `ab(cd)ef` is three SAVEs' worth of
      // exactly that - without this the answer would stop at "ab".
      //
      // One way *out* is the whole of what each step needs, and the head
      // dominating every MATCH carries the rest: if an instruction is on
      // every path to MATCH and has exactly one successor, then every such
      // path continues through that successor, so it is on every path too.
      // Induction does the remaining steps.
      //
      // A second way *in* was also required here at first, on the reasoning
      // that another edge is another way to reach the next instruction
      // without consuming this one's character. That is true and it does not
      // matter: the claim is that every match *contains* these bytes, and it
      // is settled by the edge every match does take. The condition was
      // measured before it was removed - it is reached 470,686 times over the
      // corpus's 31,648 patterns and changes the answer for none of the
      // 13,631 that have one - so it was cost and lost length, not safety.
      size_t next = at;
      int chained = 1;
      for (size_t hop = 0; hop <= posted; hop++) {
        const GRX_Inst * here
            = GRX_ARENA_AT(const GRX_Inst, &program->insts, d.order[next]);
        uint32_t succ[2];
        size_t succ_count = 0;
        // `succ_count != 1` cannot currently fail on its own: every opcode
        // the whitelist below allows a step onto continues at `pc + 1` and
        // nowhere else, so a branch is refused there before it is reached
        // here. It is kept as the condition the argument actually rests on,
        // so that adding an opcode to that whitelist cannot quietly extend a
        // literal across a branch. No input separates the two today, and a
        // mutation of this line alone survives the gates for that reason.
        if (!here || !successors_of(program, d.order[next], here, succ,
                &succ_count) || succ_count != 1) {
          chained = 0;
          break;
        }
        // Reverse postorder rises along a chain of dominators, so requiring
        // it to rise is a cycle guard that costs nothing a real chain wants.
        uint32_t to = d.number[succ[0]];
        if (to == GRX_INDEX_NONE || to <= next) {
          chained = 0;
          break;
        }
        next = to;
        const GRX_Inst * landed
            = GRX_ARENA_AT(const GRX_Inst, &program->insts, d.order[next]);
        if (!landed) {
          chained = 0;
          break;
        }
        if (landed->op == GRX_OP_CHAR) {
          break;
        }
        // Only a zero-width instruction may be stepped over. Anything that
        // consumes puts text between the two characters, and anything not
        // listed here is not known to do neither.
        if (landed->op != GRX_OP_SAVE && landed->op != GRX_OP_ASSERT
            && landed->op != GRX_OP_PROGRESS_SET
            && landed->op != GRX_OP_RESET
            && landed->op != GRX_OP_RESET_STALE
            && landed->op != GRX_OP_ATOMIC_BEGIN
            && landed->op != GRX_OP_ATOMIC_END
            && landed->op != GRX_OP_CALLOUT) {
          chained = 0;
          break;
        }
      }
      if (!chained || next == at) {
        break;
      }
      at = next;
    }
    if (!written) {
      continue;
    }
    // Length first, because a longer literal is rarer under any subject
    // distribution with imperfect correlation between its bytes, and that
    // needs no corpus to assume. The rank below only ever settles a tie, so
    // no candidate can lose to a shorter one.
    //
    //   2  a bounded window somewhere other than the very start
    //   1  an unbounded offset, or no offsets at all: presence only
    //   0  always at offset 0 and no longer than the literal prefix
    //
    // Rank 0 is last because such a literal is exactly what the prefix skip
    // already tests at every position it tries; reporting it leaves the
    // engine with one fact written twice. `a.{20}q` is the case - `a` and `q`
    // both dominate, both are one byte - and it used to answer `a`.
    size_t here_lo = 0;
    size_t here_hi = GRX_NPOS;
    if (offsets && !unbounded[i] && lo[i] != GRX_NPOS) {
      here_lo = lo[i];
      here_hi = hi[i];
    }
    int rank = 1;
    if (here_hi == 0 && program->literal_prefix_length >= written) {
      rank = 0;
    }
    else if (here_hi != GRX_NPOS) {
      rank = 2;
    }
    if (written > best || (written == best && rank > best_rank)) {
      best = written;
      best_rank = rank;
      best_lo = here_lo;
      best_hi = here_hi;
      memcpy(out, candidate, written);
    }
  }

  dominators_free(allocator, &d);
  gcu_allocator_free(allocator, lo);
  gcu_allocator_free(allocator, hi);
  gcu_allocator_free(allocator, unbounded);
  if (best && out_offset_min) {
    *out_offset_min = best_lo;
  }
  if (best && out_offset_max) {
    *out_offset_max = best_hi;
  }
  return best;
}
