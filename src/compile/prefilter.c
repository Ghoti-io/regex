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
