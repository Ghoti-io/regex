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
 * The backtracking engine: every construct, bounded by the limits.
 *
 * This is the engine that can run a backreference, a lookaround and an atomic
 * group, and it is the one whose worst case is exponential in the subject
 * length. `GRX_Limits::max_steps` and `max_backtrack` are not optional extras
 * here - they are the only thing standing between a pattern such as `(a+)+$`
 * and a process that never returns.
 *
 * The backtrack stack is explicit and never the C stack. A frame is one of
 * five things: an alternative to resume, a capture to undo, a progress
 * register to undo, an atomic barrier, or a lookaround's boundary. Undo
 * frames rather than snapshots, because a snapshot of the capture array at
 * every SPLIT is O(groups) per branch point and the writes that actually
 * need undoing are few.
 *
 * The one thing that *does* recurse on the C stack is a lookaround, and it
 * recurses exactly as deep as the pattern nests lookarounds - which the
 * parser has already capped with `max_nesting_depth`. A lookaround is atomic
 * by nature: it either holds at this position or it does not, and there is
 * nothing to backtrack into, so running it as a sub-match is not a shortcut.
 *
 * A lookbehind is the same sub-match with the body's instructions carrying
 * GRX_INST_REVERSE, so they step backwards through the subject. That is the
 * whole of what makes a lookbehind of any length work without a second
 * engine, and it is what the ECMAScript specification itself describes
 * (22.2.2.4, direction -1).
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../unicode/break_internal.h"
#include "../unicode/unicode_internal.h"
#include "exec_internal.h"

/** What a stack frame is. */
typedef enum {
  FRAME_RESUME = 0,   ///< An alternative not yet tried: `pc` and `position`.
  FRAME_CAPTURE,      ///< A capture slot to put back: `index` and `value`.
  FRAME_REGISTER,     ///< A progress register to put back.
  FRAME_ATOMIC,       ///< A barrier ATOMIC_END pops through and discards.
  /**
   * A RESUME that a `(*THEN)` may unwind to: one arm of an alternation.
   *
   * Behaves exactly as FRAME_RESUME in every other respect. It is a separate
   * kind because "the next alternative" is the one thing `(*THEN)` needs to
   * name, and a SPLIT pushed by a quantifier is not one: in `(a(*THEN)b)*`
   * the verb must leave the group, not take another turn of the loop.
   */
  FRAME_ALTERNATIVE,
  /** A call to unwind: `pc` is the call depth to go back to. */
  FRAME_CALL,
  /**
   * A control verb backtracking may return to: `pc` is the GRX_VerbKind.
   *
   * `(*PRUNE)` and its three relatives do nothing when they are reached and
   * everything when the path past them fails, so what they need is a place
   * on the undo stack rather than an action at the instruction.
   */
  FRAME_VERB,
  /**
   * A mark to unwind: `pc` is the mark-stack depth to go back to.
   *
   * The mark itself lives on a stack of its own rather than in this frame,
   * because two different readers want two different things from it - a
   * `(*SKIP:NAME)` wants the position of the most recent mark of one name,
   * and restoring on backtrack wants the previous mark whatever its name -
   * and one `uint32_t` cannot answer both without a scan.
   */
  FRAME_MARK,
  /**
   * A `(*SKIP:NAME)` to resume from: `pc` is the mark index it wants.
   *
   * Separate from FRAME_VERB because the position it resumes at is not its
   * own - it is wherever the named mark was set, which is only known when
   * backtracking arrives and the mark stack has been cut back to what is
   * still live.
   */
  FRAME_SKIP_MARK,
} FrameKind;

/**
 * What a control verb asked for, once the path it fired on has failed.
 *
 * `(*FAIL)` is not here: it fails the path and nothing more, which ordinary
 * backtracking already does. The other four each discard backtrack points
 * that are still on the stack, and they differ only in how far up the caller
 * the discarding goes - which is why the verb's answer has to travel out of
 * run() rather than being applied where it fires.
 */
typedef enum {
  VERB_NONE = 0,
  VERB_STOP_PRUNE,   ///< No more attempts from this starting position.
  VERB_STOP_SKIP,    ///< The same, and the next start is `skip_to`.
  VERB_STOP_COMMIT,  ///< No more attempts from any starting position.
  /**
   * A `(*THEN)` that found no alternative to go to.
   *
   * perlre: a `(*THEN)` outside any alternation behaves as `(*PRUNE)`, and
   * at the top level this is exactly that. It is a value of its own because
   * an assertion is a boundary it does not cross: pcre2test matches "abcd"
   * with `a(?=b(*THEN)x)c|a.+`, where a real `(*PRUNE)` in the same place
   * would have ended the attempt at offset zero.
   */
  VERB_STOP_THEN,
} VerbStop;

/** One mark that has been passed: which name, and where. */
typedef struct {
  uint32_t index;   ///< Index into the regex's mark-name table.
  size_t position;  ///< Where the `(*MARK:NAME)` was reached.
} MarkEntry;

/** One entry of the backtrack stack. */
typedef struct {
  uint8_t kind;      ///< A @ref FrameKind.
  uint32_t pc;       ///< RESUME: where to resume. Others: the slot index.
  size_t position;   ///< RESUME: the subject position. Others: the old value.
} Frame;

/** What one run of the engine carries. */
typedef struct {
  const GRX_ExecRequest * request;
  const GRX_Program * program;
  const GRX_Allocator * allocator;
  size_t captures;       ///< Capture slots: twice the group count plus two.
  size_t registers;      ///< Progress registers.
  /**
   * Where the shadow spans begin in `slots`, or 0 when there are none.
   *
   * GRX_PROGRAM_SHADOW_CAPTURES: the span each group had when it last
   * *closed*, which is what a backreference to a group that is still open
   * reads. Indices run parallel to the capture slots, so group N's pair is
   * at `shadow + N * 2`.
   */
  size_t shadow;
  size_t slot_count;     ///< Captures, registers and shadows together.
  size_t * slots;        ///< Captures, then registers.
  Frame * stack;
  size_t depth;          ///< Frames in use.
  size_t capacity;       ///< Frames allocated.
  size_t steps;          ///< Instructions executed, against max_steps.
  int utf;               ///< Whether a step is a code point or a byte.
  GRX_Result failure;    ///< Set when a limit stopped the run.

  /**
   * Leftmost-longest: keep searching past a match and report the longest.
   *
   * POSIX asks for the longest match at the leftmost start, and a
   * backtracker finds the one its alternation order reaches first. The only
   * way to know that no longer one exists is to look, so in this mode
   * GRX_OP_MATCH records what it found and then reports *failure*, which
   * sends the engine back into the search it would otherwise have stopped.
   * The run ends when the alternatives are exhausted rather than when one
   * succeeds, and `best_slots` is what is reported.
   *
   * That is exponential, and it is bounded the only way this engine bounds
   * anything: `max_steps`. The memo cannot rescue it. A visited bit means
   * "this state failed before and will fail again", which is exactly the
   * claim this mode breaks - a state that reported failure here may have
   * been the longest match - so `memo_after` stays unarmed, and a caller who
   * names the bit-state engine for a dialect that wants the longest match is
   * refused rather than quietly given the first one.
   *
   * Only a program that needs backtracking arrives here, which under these
   * dialects means one with a backreference; everything else is regular and
   * the Pike VM does the same job in linear time.
   */
  int longest;
  size_t best_end;       ///< End of the best match at this start, or NPOS.
  size_t * best_slots;   ///< Its captures; `slot_count` of them.

  /**
   * The visited set: one bit per (instruction, position), or NULL while the
   * run has no memo.
   *
   * Indexed pc * (length + 1) + position. It is *not* cleared between
   * starting positions, and that is the whole of what makes the search
   * linear rather than merely each attempt: a state that failed from an
   * earlier start will fail from a later one, because what it does next
   * depends on nothing else.
   *
   * The bit-state engine allocates this before the first step. The plain
   * backtracker allocates it partway through, when `memo_after` says the
   * run has already cost more than a memoised one ever could.
   */
  unsigned char * visited;
  size_t stride;         ///< Positions per instruction: length + 1.

  /**
   * The step count past which the plain backtracker starts memoising, or 0
   * for a run that will not.
   *
   * Perl's answer to `.X(.+)+X` is a cache it switches on once an attempt
   * has gone on long enough to look super-linear, and this is that: the
   * engine that has to be able to run anything keeps running the programs a
   * memo would be unsound for, and stops being exponential on the ones it
   * would not.
   *
   * The threshold is the number of (instruction, position) states, because
   * that is exactly the work a memoised run can do before it runs out of
   * states to visit. A run still under it has not yet repeated enough to pay
   * for the bitmap; a run past it has provably repeated itself, since it has
   * taken more steps than there are distinct states to take them from.
   */
  size_t memo_after;

  /**
   * The subroutine call stack.
   *
   * One entry per active call: where it returns to, which group it entered,
   * and the capture slots as they were when it started. PCRE2 restores those
   * on return - "any capturing parentheses set during the recursion are reset
   * to their previous values afterwards" - so the copy is not an optimisation
   * but the semantics.
   *
   * Parallel arrays rather than a struct with a pointer each, so that the
   * saved slots are one allocation that grows with the depth instead of one
   * malloc per call.
   */
  uint32_t * call_return;
  uint32_t * call_group;
  size_t * call_slots;
  size_t call_depth;
  size_t call_capacity;

  /**
   * Where a sub-match's own MATCH must land, or GRX_NPOS for anywhere.
   *
   * Set only while a forward lookbehind's body runs. That model picks a
   * start and matches the body left to right, so "the assertion holds" is
   * not "the body matched" but "the body matched and arrived exactly here" -
   * and the difference has to be inside run(), because a body that stops
   * short has to keep backtracking for a longer way to reach the same place.
   * `(?=foo)(?<=(|a|aa))` against "aafoo" is the case: the body prefers the
   * empty branch, and the assertion needs the one that gets to the `f`.
   *
   * `(*ACCEPT)` deliberately does not consult it. The verb ends the
   * assertion where it fires, which is the whole of what makes
   * `(?<=([cd](*ACCEPT)|x)gggg)blrph` match "cblrph" in Perl.
   */
  size_t look_end;

  /**
   * The stack height above which a capture undo is *not* applied, or
   * GRX_NPOS.
   *
   * Set while the body of a negative lookaround runs in a dialect that keeps
   * what the body captured (GRX_INST_KEEP_CAPTURES). Ordinary backtracking
   * puts a capture back as it unwinds, so by the time a failing body has run
   * out of paths its writes are already gone - which is the right answer
   * everywhere except here, where the assertion succeeding *because* the
   * body failed is supposed to leave them standing.
   *
   * Frames below the floor are the caller's and are undone normally, so
   * backtracking past the whole assertion still puts everything back.
   */
  size_t keep_floor;

  VerbStop verb_stop;    ///< What a control verb asked for, or VERB_NONE.
  size_t skip_to;        ///< Where `(*SKIP)` fired, for VERB_STOP_SKIP.

  /**
   * The marks passed on the path being explored, innermost last.
   *
   * `(*SKIP:NAME)` searches this from the top down for the name it wants, so
   * the entries have to be the *live* ones - a mark on a path that has since
   * been abandoned is not a place to resume from. FRAME_MARK is what makes
   * them live: it records the depth to cut back to.
   */
  MarkEntry * marks;
  size_t mark_depth;
  size_t mark_capacity;
  uint32_t mark;          ///< The last mark still on this path, or NONE.
  uint32_t nomatch_mark;  ///< The last mark reached at all, or NONE.

  /**
   * The stretch of subject that counts as the subject right now.
   *
   * [0, length) everywhere except inside `(*scs:(n)...)`, where pcre2pattern
   * says the captured substring "is treated as the whole subject" - so `^`
   * holds at its start, `$` and `\z` at its end, and nothing outside it can
   * be read. Every place that would have asked the request how long the
   * subject is asks these instead.
   */
  size_t window_start;
  size_t window_end;
} Backtrack;

/**
 * Whether (pc, position) has been tried already; records it if not.
 *
 * Only ever reached on the way *into* a state. A state that succeeded made
 * the whole run return, so a bit that is set is a state that failed, and a
 * state that failed once fails always.
 *
 * The bitmap is not cleared between starting positions, which is what makes
 * the whole *search* linear rather than each attempt. That is sound for the
 * same reason - but it takes one more step to see it for the one thing in
 * this engine that is not a function of (pc, position): the empty-match
 * rule, which reads the attempt's own start through `slots[0]`.
 *
 * It holds because the starts only increase. Suppose an attempt from `s`
 * reaches MATCH at `p` and is refused for being empty; then `s == p`, and
 * every later attempt starts after `p` and so cannot produce a match that
 * *ends* at `p`. The refused state is therefore never reached again, and
 * memoising it cannot hide an acceptance. If instead `s < p` the match is
 * not empty, is accepted, and the run returns before anything is memoised.
 */
static int already_tried(Backtrack * bt, uint32_t pc, size_t position) {
  if (!bt->visited) {
    return 0;
  }
  size_t index = (size_t)pc * bt->stride + position;
  size_t byte = index >> 3;
  unsigned char bit = (unsigned char)(1u << (index & 7u));
  if (bt->visited[byte] & bit) {
    return 1;
  }
  bt->visited[byte] |= bit;
  return 0;
}

/**
 * Switch the memo on partway through a run, if it can be afforded.
 *
 * Called once, the first time the step count passes `memo_after`. Failing to
 * allocate is not an error: the bitmap is an optimisation here rather than a
 * promise, and a run that cannot have one carries on to whatever limit it was
 * going to reach anyway. `memo_after` is cleared either way, so the attempt
 * is not repeated at every step from here on.
 *
 * Starting late costs nothing but the pruning that was not done: every bit
 * the map holds is still a state that was entered and failed, which is the
 * only thing already_tried() reads it for.
 */
static void enable_memo(Backtrack * bt) {
  size_t bytes = grx_exec_bitmap_bytes(bt->request->regex, bt->request->length);
  bt->memo_after = 0;
  if (bytes == GRX_NPOS) {
    return;
  }
  const GRX_Limits * limits = bt->request->limits;
  if (limits->max_match_memory && bytes > limits->max_match_memory) {
    return;
  }
  bt->visited = gcu_allocator_calloc(bt->allocator, bytes, 1);
}

/** Make room for `wanted` active calls, charged against max_match_memory. */
static int call_reserve(Backtrack * bt, size_t wanted) {
  if (wanted <= bt->call_capacity) {
    return 1;
  }

  size_t capacity = bt->call_capacity ? bt->call_capacity * 2 : 8;
  while (capacity < wanted) {
    capacity *= 2;
  }

  size_t bytes = capacity * (2 * sizeof(uint32_t) + bt->captures * sizeof(size_t));
  const GRX_Limits * limits = bt->request->limits;
  if (limits->max_match_memory && bytes > limits->max_match_memory) {
    bt->failure = GRX_ERR_LIMIT;
    return 0;
  }

  uint32_t * call_return = gcu_allocator_realloc(
      bt->allocator, bt->call_return, capacity * sizeof(uint32_t));
  if (!call_return) {
    bt->failure = GRX_ERR_OOM;
    return 0;
  }
  bt->call_return = call_return;

  uint32_t * call_group = gcu_allocator_realloc(
      bt->allocator, bt->call_group, capacity * sizeof(uint32_t));
  if (!call_group) {
    bt->failure = GRX_ERR_OOM;
    return 0;
  }
  bt->call_group = call_group;

  size_t * call_slots = gcu_allocator_realloc(bt->allocator, bt->call_slots,
      capacity * bt->slot_count * sizeof(size_t));
  if (!call_slots) {
    bt->failure = GRX_ERR_OOM;
    return 0;
  }
  bt->call_slots = call_slots;

  bt->call_capacity = capacity;
  return 1;
}

/** The smallest stack the engine allocates, so a simple match grows nothing. */
#define GRX_BACKTRACK_MIN_STACK 64

// --------------------------------------------------------------------------
// The stack
// --------------------------------------------------------------------------

static int push(Backtrack * bt, FrameKind kind, uint32_t pc, size_t position) {
  const GRX_Limits * limits = bt->request->limits;
  if (limits->max_backtrack && bt->depth + 1 > limits->max_backtrack) {
    bt->failure = GRX_ERR_LIMIT;
    return 0;
  }

  if (bt->depth == bt->capacity) {
    size_t capacity = bt->capacity ? bt->capacity * 2 : GRX_BACKTRACK_MIN_STACK;
    if (limits->max_backtrack && capacity > limits->max_backtrack) {
      capacity = limits->max_backtrack;
    }
    Frame * grown = gcu_allocator_realloc(
        bt->allocator, bt->stack, capacity * sizeof(Frame));
    if (!grown) {
      bt->failure = GRX_ERR_OOM;
      return 0;
    }
    bt->stack = grown;
    bt->capacity = capacity;
  }

  bt->stack[bt->depth].kind = (uint8_t)kind;
  bt->stack[bt->depth].pc = pc;
  bt->stack[bt->depth].position = position;
  bt->depth++;
  return 1;
}

/**
 * Decide what a verb that fired inside an assertion's body does to the rest.
 *
 * pcre2test settles all four: `a(?=b(*PRUNE)x).+|(.+)` against "abcd"
 * matches at offset one and `a(?=b(*COMMIT)x).+|(.+)` does not match at all,
 * so a `(*PRUNE)`, `(*COMMIT)` or `(*SKIP)` inside a *positive* assertion
 * whose body failed ends the attempt outside it too. A negative assertion
 * discards it - the assertion succeeded, so nothing failed - and a
 * `(*THEN)` with no alternative is confined to the assertion whatever its
 * sign.
 *
 * @return Non-zero when the caller must abandon this run as well.
 */
static int verb_escapes_assertion(Backtrack * bt, int negative) {
  if (bt->verb_stop == VERB_NONE) {
    return 0;
  }
  if (negative || bt->verb_stop == VERB_STOP_THEN) {
    bt->verb_stop = VERB_NONE;
    return 0;
  }

  return 1;
}

/**
 * Record a mark, and the point backtracking should forget it at.
 *
 * Two pushes, and the order matters: the frame records the depth *before*
 * the entry goes on, so unwinding to it removes exactly this mark.
 */
static int push_mark(Backtrack * bt, uint32_t index, size_t position) {
  if (!push(bt, FRAME_MARK, (uint32_t)bt->mark_depth, position)) {
    return 0;
  }

  const GRX_Limits * limits = bt->request->limits;
  if (bt->mark_depth == bt->mark_capacity) {
    size_t capacity
        = bt->mark_capacity ? bt->mark_capacity * 2 : GRX_BACKTRACK_MIN_STACK;
    if (limits->max_backtrack && capacity > limits->max_backtrack) {
      capacity = limits->max_backtrack;
    }
    if (bt->mark_depth == capacity) {
      bt->failure = GRX_ERR_LIMIT;
      return 0;
    }
    MarkEntry * grown = gcu_allocator_realloc(
        bt->allocator, bt->marks, capacity * sizeof(MarkEntry));
    if (!grown) {
      bt->failure = GRX_ERR_OOM;
      return 0;
    }
    bt->marks = grown;
    bt->mark_capacity = capacity;
  }

  bt->marks[bt->mark_depth].index = index;
  bt->marks[bt->mark_depth].position = position;
  bt->mark_depth++;
  bt->mark = index;
  bt->nomatch_mark = index;
  return 1;
}

/** Where the most recent live `(*MARK:NAME)` of one name was reached. */
static int find_mark(const Backtrack * bt, uint32_t index, size_t * out_position) {
  for (size_t i = bt->mark_depth; i > 0; i--) {
    if (bt->marks[i - 1].index == index) {
      *out_position = bt->marks[i - 1].position;
      return 1;
    }
  }

  return 0;
}

/** Record a slot's old value so that backtracking past this point restores it. */
static int save_slot(Backtrack * bt, size_t index) {
  if (index >= bt->slot_count) {
    return 1;
  }
  // A shadow span is a capture as far as undoing goes: it is what a
  // backreference reads, so a path that is abandoned must not leave it
  // behind. A register is the loop's own bookkeeping.
  FrameKind kind = (index >= bt->captures && index < bt->captures
      + bt->registers) ? FRAME_REGISTER : FRAME_CAPTURE;
  return push(bt, kind, (uint32_t)index, bt->slots[index]);
}

/**
 * Fix a group's shadow span, if this SAVE is the one that closes it.
 *
 * Which of a group's two SAVEs closes it depends on which way the
 * instructions run. Forward the odd slot is written second; inside a reverse
 * lookbehind body the group writes its *end* first, because that is the
 * boundary its body reaches first, and the even slot is the one that closes
 * it. Keying on "odd" alone got that wrong, and `(?<=([abc]+)).\1` against
 * "aaa" is where the ECMAScript corpus said so.
 */
static int shadow_close(Backtrack * bt, size_t slot, int reverse) {
  if (!bt->shadow || slot >= bt->captures) {
    return 1;
  }
  int closes = reverse ? !(slot & 1u) : (int)(slot & 1u);
  if (!closes) {
    return 1;
  }

  size_t live = slot & ~(size_t)1u;
  size_t index = bt->shadow + live;
  if (!save_slot(bt, index) || !save_slot(bt, index + 1)) {
    return 0;
  }
  bt->slots[index] = bt->slots[live];
  bt->slots[index + 1] = bt->slots[live + 1];
  return 1;
}

/** Clear one group's shadow span alongside its live one. */
static int shadow_clear(Backtrack * bt, size_t slot) {
  if (!bt->shadow || slot >= bt->captures) {
    return 1;
  }
  size_t index = bt->shadow + slot;
  if (!save_slot(bt, index)) {
    return 0;
  }
  bt->slots[index] = GRX_NPOS;
  return 1;
}

/**
 * Unwind to the most recent alternative, undoing everything on the way.
 *
 * Returns 0 when the stack is empty, which means every alternative from this
 * starting position has been tried and failed.
 */
static int backtrack(Backtrack * bt, uint32_t * out_pc, size_t * out_position,
    size_t floor) {
  while (bt->depth > floor) {
    Frame frame = bt->stack[--bt->depth];
    switch ((FrameKind)frame.kind) {
      case FRAME_RESUME:
      case FRAME_ALTERNATIVE:
        *out_pc = frame.pc;
        *out_position = frame.position;
        return 1;
      case FRAME_CAPTURE:
        if (bt->keep_floor != GRX_NPOS && bt->depth >= bt->keep_floor) {
          // Inside a negative lookaround body whose writes are meant to
          // survive it. A register is still put back: it is the loop's own
          // bookkeeping, not something a caller can read.
          break;
        }
        bt->slots[frame.pc] = frame.position;
        break;
      case FRAME_REGISTER:
        bt->slots[frame.pc] = frame.position;
        break;
      case FRAME_CALL:
        bt->call_depth = frame.pc;
        break;
      case FRAME_MARK:
        // The mark is no longer on the path. `nomatch_mark` is deliberately
        // not restored: what a failed match reports is the last mark it
        // reached, not the last one it was still standing on.
        bt->mark_depth = frame.pc;
        bt->mark = bt->mark_depth
            ? bt->marks[bt->mark_depth - 1].index
            : GRX_INDEX_NONE;
        break;
      case FRAME_VERB:
        // Backtracking has returned to a control verb. What it asks for
        // depends on which one, and all four discard what is still on the
        // stack: `(*THEN)` down to the next alternative of the enclosing
        // group, the other three down to the floor.
        if (frame.pc == (uint32_t)GRX_VERB_THEN) {
          while (bt->depth > floor) {
            Frame inner = bt->stack[--bt->depth];
            if (inner.kind == FRAME_CAPTURE || inner.kind == FRAME_REGISTER) {
              bt->slots[inner.pc] = inner.position;
              continue;
            }
            if (inner.kind == FRAME_CALL) {
              bt->call_depth = inner.pc;
              continue;
            }
            if (inner.kind == FRAME_MARK) {
              bt->mark_depth = inner.pc;
              bt->mark = bt->mark_depth
                  ? bt->marks[bt->mark_depth - 1].index
                  : GRX_INDEX_NONE;
              continue;
            }
            if (inner.kind == FRAME_ALTERNATIVE) {
              *out_pc = inner.pc;
              *out_position = inner.position;
              return 1;
            }
          }
          // No alternative left. perlre: a `(*THEN)` outside any alternation
          // behaves as `(*PRUNE)` - but only as far as the nearest assertion,
          // which is why this is its own stop and not that one.
          bt->verb_stop = VERB_STOP_THEN;
          return 0;
        }
        bt->verb_stop = frame.pc == (uint32_t)GRX_VERB_COMMIT
            ? VERB_STOP_COMMIT
            : frame.pc == (uint32_t)GRX_VERB_SKIP ? VERB_STOP_SKIP
                                                  : VERB_STOP_PRUNE;
        bt->skip_to = frame.position;
        bt->depth = floor;
        return 0;
      case FRAME_SKIP_MARK: {
        size_t where = 0;
        if (!find_mark(bt, frame.pc, &where)) {
          // pcre2pattern: a `(*SKIP:NAME)` with no `(*MARK:NAME)` set is
          // ignored. Not treated as a bare `(*SKIP)`, which would resume at
          // the verb's own position and report a different match.
          break;
        }
        bt->verb_stop = VERB_STOP_SKIP;
        bt->skip_to = where;
        bt->depth = floor;
        return 0;
      }
      case FRAME_ATOMIC:
      default:
        break;
    }
  }

  return 0;
}

// --------------------------------------------------------------------------
// Reading the subject
// --------------------------------------------------------------------------

static int read_forward(const Backtrack * bt, size_t position,
    uint32_t * out_codepoint, size_t * out_width) {
  if (position >= bt->window_end) {
    return 0;
  }
  if (!bt->utf) {
    *out_codepoint = (unsigned char)bt->request->subject[position];
    *out_width = 1;
    return 1;
  }

  size_t width = grx_unicode_utf8_decode(bt->request->subject + position,
      bt->window_end - position, out_codepoint);
  if (!width) {
    return 0;
  }
  *out_width = width;
  return 1;
}

static int read_backward(const Backtrack * bt, size_t position,
    uint32_t * out_codepoint, size_t * out_width) {
  // A window begins on a character boundary - it is a capture span - so
  // scanning back from inside it never walks past its start.
  if (position <= bt->window_start) {
    return 0;
  }
  if (!bt->utf) {
    *out_codepoint = (unsigned char)bt->request->subject[position - 1];
    *out_width = 1;
    return 1;
  }

  size_t width = grx_unicode_utf8_decode_prev(
      bt->request->subject, position, out_codepoint);
  if (!width) {
    return 0;
  }
  *out_width = width;
  return 1;
}

/**
 * Whether the caller said the start of the subject is not a start of line.
 *
 * A flag about the *caller's* subject, so it says nothing about the start of
 * a scan-substring window: inside one, `^` holds because the substring is
 * the subject there, and PCRE2_NOTBOL was about a different string.
 */
static int at_subject_start_suppressed(const Backtrack * bt) {
  return bt->request->not_bol && !bt->window_start;
}

/** The same for the end, and PCRE2_NOTEOL. */
static int at_subject_end_suppressed(const Backtrack * bt) {
  return bt->request->not_eol && bt->window_end == bt->request->length;
}

static int in_class(const Backtrack * bt, uint32_t index, uint32_t codepoint) {
  if (index == GRX_INDEX_NONE) {
    return 0;
  }
  return grx_class_table_contains(&bt->program->classes, index, codepoint);
}

/** As the Pike VM's: a kind and a class index, and no dialect in sight. */
static int assertion_holds(
    const Backtrack * bt, const GRX_Inst * inst, size_t position) {
  const GRX_ExecRequest * request = bt->request;
  uint32_t before = 0;
  uint32_t after = 0;
  size_t width = 0;
  int has_before = read_backward(bt, position, &before, &width);
  int has_after = read_forward(bt, position, &after, &width);

  // NOTBOL and NOTEOL suppress only the end-of-subject halves; see the same
  // switch in exec_pike.c, which this one has to agree with exactly.
  switch ((GRX_AssertKind)inst->mode) {
    case GRX_ASSERT_START_SUBJECT:
      return position == bt->window_start && !at_subject_start_suppressed(bt);
    case GRX_ASSERT_END_SUBJECT:
      return position == bt->window_end && !at_subject_end_suppressed(bt);
    case GRX_ASSERT_END_BEFORE_NEWLINE:
      if (at_subject_end_suppressed(bt)) {
        return 0;
      }
      if (position == bt->window_end) {
        return 1;
      }
      return has_after && in_class(bt, inst->x, after)
          && position + width == bt->window_end;
    case GRX_ASSERT_START_LINE:
      return (position == bt->window_start && !at_subject_start_suppressed(bt))
          || (has_before && in_class(bt, inst->x, before));

    case GRX_ASSERT_START_LINE_INTERIOR:
      // The same, less the position after a newline that ends the
      // subject: there is no line there to be at the start of.
      return (position == bt->window_start && !at_subject_start_suppressed(bt))
          || (position != bt->window_end && has_before
              && in_class(bt, inst->x, before));
    case GRX_ASSERT_END_LINE:
      return (position == bt->window_end && !at_subject_end_suppressed(bt))
          || (has_after && in_class(bt, inst->x, after));
    case GRX_ASSERT_WORD_BOUNDARY:
    case GRX_ASSERT_NOT_WORD_BOUNDARY:
    case GRX_ASSERT_WORD_START:
    case GRX_ASSERT_WORD_END: {
      int word_before = has_before && in_class(bt, inst->x, before);
      int word_after = has_after && in_class(bt, inst->x, after);
      if (inst->mode == GRX_ASSERT_WORD_START) {
        return !word_before && word_after;
      }
      if (inst->mode == GRX_ASSERT_WORD_END) {
        return word_before && !word_after;
      }
      int boundary = word_before != word_after;
      return inst->mode == GRX_ASSERT_WORD_BOUNDARY ? boundary : !boundary;
    }

    case GRX_ASSERT_LOOK_LENGTH: {
      // How far is it to the end of this assertion, and can the alternative
      // that follows span it? Outside a forward lookbehind body there is no
      // end to measure to, and the guard holds rather than guessing - it is
      // an optimisation, and one that is not applicable is not one that
      // fails.
      size_t min = 0;
      size_t max = 0;
      if (bt->look_end == GRX_NPOS || position > bt->look_end
          || !grx_program_look_span(bt->program, inst->x, &min, &max)) {
        return 1;
      }
      size_t remaining = bt->look_end - position;
      return remaining >= min && remaining <= max;
    }

    // The four segmentation boundaries. Each reads the subject itself, so
    // the instruction carries no class and the window is not consulted: a
    // boundary is a fact about the text, and `(*scs:` narrowing what may be
    // *matched* does not move where a sentence ends.
    case GRX_ASSERT_GRAPHEME_BOUNDARY:
    case GRX_ASSERT_NOT_GRAPHEME_BOUNDARY:
    case GRX_ASSERT_WORD_SEG_BOUNDARY:
    case GRX_ASSERT_NOT_WORD_SEG_BOUNDARY:
    case GRX_ASSERT_SENTENCE_BOUNDARY:
    case GRX_ASSERT_NOT_SENTENCE_BOUNDARY:
    case GRX_ASSERT_LINE_BOUNDARY:
    case GRX_ASSERT_NOT_LINE_BOUNDARY: {
      static const GRX_BreakKind kinds[] = {
        GRX_BREAK_GRAPHEME, GRX_BREAK_GRAPHEME,
        GRX_BREAK_WORD, GRX_BREAK_WORD,
        GRX_BREAK_SENTENCE, GRX_BREAK_SENTENCE,
        GRX_BREAK_LINE, GRX_BREAK_LINE,
      };
      size_t which = (size_t)inst->mode - GRX_ASSERT_GRAPHEME_BOUNDARY;
      int boundary = grx_unicode_break_at(
          kinds[which], request->subject, request->length, position);
      // The odd members of the run are the `\B{...}` spellings.
      return (which % 2) ? !boundary : boundary;
    }
    case GRX_ASSERT_SEARCH_START:
      return position == request->start;
    case GRX_ASSERT_COUNT:
    default:
      return 0;
  }
}

/**
 * Whether the text a group matched appears again at `position`.
 *
 * Under a caseless match the comparison is on folded code points rather than
 * on bytes, because the two runs may be the same characters in different
 * cases and so different lengths. This is the one place folding survives
 * lowering, and it has to: what a backreference matches is not known until
 * the match runs.
 */
static int backref_matches(const Backtrack * bt, const GRX_Inst * inst,
    size_t position, int reverse, size_t * out_width) {
  size_t start_slot = (size_t)inst->x * 2;
  size_t end_slot = start_slot + 1;
  *out_width = 0;

  if (end_slot >= bt->captures) {
    return inst->mode == GRX_BACKREF_UNSET_EMPTY;
  }
  if (bt->shadow) {
    // The span the group last closed with, which is the live one unless the
    // group is open right now. `^(a\1?){4}$` is the case that needs it.
    start_slot += bt->shadow;
    end_slot += bt->shadow;
  }

  if (bt->slots[start_slot] == GRX_NPOS
      || bt->slots[end_slot] == GRX_NPOS) {
    // The group did not participate. Which of two things that means is the
    // dialect's answer, carried here as a mode: ECMAScript matches the empty
    // string, everything else fails.
    return inst->mode == GRX_BACKREF_UNSET_EMPTY;
  }

  size_t from = bt->slots[start_slot];
  size_t to = bt->slots[end_slot];
  if (to < from) {
    return 0;
  }

  if (!inst->y) {
    size_t length = to - from;
    // Backwards, the run being compared is the one that *ends* at `position`,
    // so the comparison starts `length` bytes earlier and the width is given
    // back to the caller to subtract rather than to add.
    size_t at = position;
    if (reverse) {
      if (length > position) {
        return 0;
      }
      at = position - length;
    }
    else if (position + length > bt->window_end) {
      return 0;
    }
    if (memcmp(bt->request->subject + from, bt->request->subject + at, length)
        != 0) {
      return 0;
    }
    *out_width = length;
    return 1;
  }

  // The caseless comparison walks both runs a character at a time, in the
  // direction the body runs. A folded comparison cannot be done on bytes,
  // because two runs that fold alike may have different lengths.
  if (reverse) {
    size_t source = to;
    size_t target = position;
    while (source > from) {
      uint32_t wanted = 0;
      uint32_t found = 0;
      size_t source_width = 0;
      size_t target_width = 0;
      if (!read_backward(bt, source, &wanted, &source_width)
          || !read_backward(bt, target, &found, &target_width)) {
        return 0;
      }
      if (grx_unicode_fold_simple(wanted) != grx_unicode_fold_simple(found)) {
        return 0;
      }
      source -= source_width;
      target -= target_width;
    }
    *out_width = position - target;
    return 1;
  }

  size_t source = from;
  size_t target = position;
  while (source < to) {
    uint32_t wanted = 0;
    uint32_t found = 0;
    size_t source_width = 0;
    size_t target_width = 0;
    if (!read_forward(bt, source, &wanted, &source_width)
        || !read_forward(bt, target, &found, &target_width)) {
      return 0;
    }
    if (grx_unicode_fold_simple(wanted) != grx_unicode_fold_simple(found)) {
      return 0;
    }
    source += source_width;
    target += target_width;
  }

  *out_width = target - position;
  return 1;
}

// --------------------------------------------------------------------------
// The machine
// --------------------------------------------------------------------------

/**
 * Run the program from `pc` at `position` until it matches or runs out of
 * alternatives.
 *
 * `floor` is the stack height this run may not unwind below, so that a
 * lookaround's sub-match cannot backtrack into its caller's alternatives.
 * The caller unwinds to the floor itself afterwards, which is what makes a
 * lookaround atomic.
 *
 * `toplevel` distinguishes the whole-pattern run from a lookaround body.
 * Both end in a MATCH - a lookaround body is compiled as a sub-program with
 * a success state of its own - but only the outer one is a match the caller
 * asked for, and so only the outer one is subject to the request's rule
 * about empty matches. Applying it to a body would make `(?=)` fail under
 * NOTEMPTY, which is a rule about the *result*, not about an assertion.
 */
static int run(Backtrack * bt, uint32_t pc, size_t position, size_t floor,
    int toplevel, size_t * out_end);

/**
 * Leave the innermost subroutine call, and say where to carry on.
 *
 * Two instructions reach here: the RET a call's block ends in, and an
 * `(*ACCEPT)` inside the call, which pcre2pattern says returns from the
 * subroutine rather than ending the match. What the call captured is not
 * kept - pcre2pattern again: the values are reset to what they were before
 * it - and the restoring is slot by slot through the undo stack, so that
 * backtracking past the whole call still puts back what *it* found.
 *
 * @param bt The run.
 * @param in_out_pc Receives the instruction to continue at.
 * @param position Where the call is returning from.
 * @return Non-zero on success; zero with `bt->failure` set otherwise.
 */
static int return_from_call(
    Backtrack * bt, uint32_t * in_out_pc, size_t position) {
  if (!bt->call_depth) {
    bt->failure = GRX_ERR_INTERNAL;
    return 0;
  }

  size_t depth = bt->call_depth - 1;
  const size_t * saved = bt->call_slots + depth * bt->slot_count;
  for (size_t i = 0; i < bt->slot_count; i++) {
    if (i >= bt->captures && i < bt->captures + bt->registers) {
      // A progress register is the loop's own bookkeeping and belongs to
      // whoever is running the loop, not to the call that interrupted it.
      continue;
    }
    if (bt->slots[i] == saved[i]) {
      continue;
    }
    size_t restored = saved[i];
    if (!save_slot(bt, i)) {
      return 0;
    }
    bt->slots[i] = restored;
  }
  if (!push(bt, FRAME_CALL, (uint32_t)bt->call_depth, position)) {
    return 0;
  }
  *in_out_pc = bt->call_return[depth];
  bt->call_depth = depth;
  return 1;
}

/**
 * Run a lookbehind's body forwards, from every start its length allows.
 *
 * The second of the two lookbehind models (documentation/design.md section
 * 3.5.2), and the one Perl and PCRE2 use. The body is an ordinary left to
 * right sub-program; what the assertion adds is the choice of where it
 * begins and the requirement that it arrive exactly here.
 *
 * Furthest back first, which is the order the reference tries and the one
 * that settles `(?=foo)(?<=(a??))` against "afoo": the body is lazy and
 * would rather match nothing, but the *assertion* prefers the longest, so
 * Perl reports group one as "a" and running the body in reverse reports it
 * empty.
 *
 * The loop is bounded by `max - min`, not by how far back `max` reaches: a
 * body of one length has one candidate however long it is. That bound is
 * the dialect's - 255 bytes of variation for PCRE2 and Perl, checked at
 * compile time - and it is what keeps an assertion from costing the subject.
 *
 * A start that falls inside a character is not filtered out. It does not
 * need to be: UTF-8 decoding refuses a continuation byte in lead position,
 * so such a start matches nothing it could have reached this position with,
 * and skipping the arithmetic is what keeps the loop off the subject.
 *
 * @param bt The run.
 * @param body The body's first instruction.
 * @param position Where the assertion stands, and where the body must end.
 * @param min The body's shortest match, in bytes.
 * @param max Its longest.
 * @param before The captures as they were, restored before each candidate.
 * @param out_end Receives where the body ended.
 * @return Non-zero when some start matched.
 */
static int look_behind_forward(Backtrack * bt, uint32_t body, size_t position,
    size_t min, size_t max, const size_t * before, size_t * out_end) {
  size_t available = position - bt->window_start;
  size_t reach = max < available ? max : available;
  if (reach < min) {
    // Not enough subject behind this position for the shortest body.
    return 0;
  }

  // The memo is keyed on (instruction, position) and means "this state leads
  // nowhere". Inside this body it would mean "leads nowhere *ending here*",
  // which is a different claim at every position the assertion is tried
  // from - so the body runs without one. The outer program's bits are
  // untouched and still stand.
  unsigned char * outer_visited = bt->visited;
  size_t outer_memo_after = bt->memo_after;
  size_t outer_look_end = bt->look_end;
  bt->visited = NULL;
  bt->memo_after = 0;
  bt->look_end = position;

  size_t floor = bt->depth;
  int matched = 0;
  for (size_t back = reach;; back--) {
    memcpy(bt->slots, before, bt->captures * sizeof(size_t));
    bt->depth = floor;
    matched = run(bt, body, position - back, floor, 0, out_end);
    bt->depth = floor;
    if (matched || bt->failure != GRX_OK) {
      break;
    }
    // A `(*COMMIT)`, `(*PRUNE)` or `(*SKIP)` that fired in the body has
    // already said there are to be no more attempts, and another candidate
    // start is another attempt. The caller decides how far out the refusal
    // travels; this only stops adding to it.
    if (bt->verb_stop != VERB_NONE) {
      break;
    }
    if (back == min) {
      break;
    }
  }

  bt->visited = outer_visited;
  bt->memo_after = outer_memo_after;
  bt->look_end = outer_look_end;
  if (!matched) {
    memcpy(bt->slots, before, bt->captures * sizeof(size_t));
  }
  return matched;
}

static int run(Backtrack * bt, uint32_t pc, size_t position, size_t floor,
    int toplevel, size_t * out_end) {
  const GRX_Limits * limits = bt->request->limits;
  // The call depth this run started at. `(*ACCEPT)` ends the innermost thing
  // it is inside, and a sub-match's own body is one of those: a call entered
  // *before* this run began belongs to the caller, so a verb here must not
  // return from it.
  size_t call_floor = bt->call_depth;

  for (;;) {
    bt->steps++;
    if (limits->max_steps && bt->steps > limits->max_steps) {
      bt->failure = GRX_ERR_LIMIT;
      return 0;
    }

    if (bt->memo_after && bt->steps > bt->memo_after) {
      enable_memo(bt);
    }

    if (already_tried(bt, pc, position)) {
      if (!backtrack(bt, &pc, &position, floor)) {
        return 0;
      }
      continue;
    }

    const GRX_Inst * inst
        = GRX_ARENA_AT(const GRX_Inst, &bt->program->insts, pc);
    if (!inst) {
      bt->failure = GRX_ERR_INTERNAL;
      return 0;
    }

    int reverse = (inst->flags & GRX_INST_REVERSE) != 0;
    uint32_t codepoint = 0;
    size_t width = 0;
    int consumed = 0;
    int ok = 1;

    switch ((GRX_Opcode)inst->op) {
      case GRX_OP_CHAR:
      case GRX_OP_CLASS:
      case GRX_OP_ANY:
      case GRX_OP_ANY_NL: {
        int have = reverse
            ? read_backward(bt, position, &codepoint, &width)
            : read_forward(bt, position, &codepoint, &width);
        if (!have) {
          ok = 0;
          break;
        }
        switch ((GRX_Opcode)inst->op) {
          case GRX_OP_CHAR:
            ok = codepoint == inst->x;
            break;
          case GRX_OP_CLASS:
            ok = in_class(bt, inst->x, codepoint);
            break;
          case GRX_OP_ANY:
            ok = !in_class(bt, inst->x, codepoint);
            break;
          default:
            ok = 1;
            break;
        }
        consumed = ok;
        break;
      }

      case GRX_OP_SPLIT:
        // The other continuation is an alternative to come back to. The
        // preferred one is taken now, and that is what makes the result
        // leftmost-first.
        if (!push(bt,
                (inst->flags & GRX_INST_ALTERNATION) ? FRAME_ALTERNATIVE
                                                     : FRAME_RESUME,
                inst->y, position)) {
          return 0;
        }
        pc = inst->x;
        continue;

      case GRX_OP_JMP:
        pc = inst->x;
        continue;

      case GRX_OP_SAVE:
        if (!save_slot(bt, inst->x)) {
          return 0;
        }
        if (inst->x < bt->captures) {
          bt->slots[inst->x] = position;
        }
        if (!shadow_close(bt, inst->x, reverse)) {
          return 0;
        }
        pc++;
        continue;

      case GRX_OP_RESET:
        for (uint32_t slot = inst->x;
            slot < inst->y && slot < bt->captures; slot++) {
          if (!shadow_clear(bt, slot)) {
            return 0;
          }
          if (!save_slot(bt, slot)) {
            return 0;
          }
          bt->slots[slot] = GRX_NPOS;
        }
        pc++;
        continue;

      case GRX_OP_ASSERT:
        ok = assertion_holds(bt, inst, position);
        break;

      case GRX_OP_PROGRESS_SET: {
        size_t index = bt->captures + inst->x;
        if (!save_slot(bt, index)) {
          return 0;
        }
        if (index < bt->captures + bt->registers) {
          bt->slots[index] = position;
        }
        pc++;
        continue;
      }

      case GRX_OP_REWIND: {
        // The tail of a non-atomic lookaround: the body ran as ordinary
        // instructions, and this is what makes it consume nothing. No undo
        // frame - the position is the caller's own local, restored by
        // backtracking like any other.
        size_t index = bt->captures + inst->x;
        if (index < bt->captures + bt->registers
            && bt->slots[index] != GRX_NPOS) {
          position = bt->slots[index];
        }
        pc++;
        continue;
      }

      case GRX_OP_RESET_STALE: {
        // This iteration is ending. A group whose start is before where the
        // iteration began was set by an earlier one, and the dialect's rule
        // is that this iteration takes it away on the way out. Through the
        // undo stack, so backtracking into the loop puts it back.
        size_t index = bt->captures + inst->x;
        if (index >= bt->captures + bt->registers) {
          bt->failure = GRX_ERR_INTERNAL;
          return 0;
        }
        size_t began = bt->slots[index];
        size_t first = inst->y;
        if (first + 1 < bt->captures && bt->slots[first] != GRX_NPOS
            && bt->slots[first] < began) {
          if (!save_slot(bt, first) || !save_slot(bt, first + 1)
              || !shadow_clear(bt, first)
              || !shadow_clear(bt, first + 1)) {
            return 0;
          }
          bt->slots[first] = GRX_NPOS;
          bt->slots[first + 1] = GRX_NPOS;
        }
        pc++;
        continue;
      }

      case GRX_OP_PROGRESS_CHECK: {
        size_t index = bt->captures + inst->x;
        int stalled = index < bt->captures + bt->registers
            && bt->slots[index] == position;
        if (!stalled) {
          pc++;
          continue;
        }
        switch ((GRX_EmptyLoopMode)inst->mode) {
          case GRX_EMPTY_LOOP_FAIL:
            ok = 0;
            break;
          case GRX_EMPTY_LOOP_BREAK:
            pc = inst->y;
            continue;
          case GRX_EMPTY_LOOP_BREAK_FIRST: {
            // See exec_pike.c: BREAK while the loop stands where it was
            // entered, FAIL once it has moved. The entry register is the
            // one after `x`.
            size_t entry = bt->captures + inst->x - 1;
            if (entry < bt->captures + bt->registers
                && bt->slots[entry] == position) {
              pc = inst->y;
              continue;
            }
            ok = 0;
            break;
          }
          case GRX_EMPTY_LOOP_ALLOW:
          case GRX_EMPTY_LOOP_COUNT:
          default:
            pc++;
            continue;
        }
        break;
      }

      case GRX_OP_BACKREF:
        // A lookbehind runs its body backwards, and this instruction used to
        // ignore that: it compared forward from `position` and advanced
        // forward, inside a body that was walking the other way. The visible
        // result was captures with an end before their start - `(.)(?<=(\1\1))`
        // against "aaa" reported group 2 as 3-1 - and lookbehinds that matched
        // when they must not. Found by test262's named-group and backreference
        // cases.
        ok = backref_matches(bt, inst, position, reverse, &width);
        if (ok) {
          position = reverse ? position - width : position + width;
        }
        pc++;
        if (ok) {
          continue;
        }
        break;

      case GRX_OP_ATOMIC_BEGIN:
        if (!push(bt, FRAME_ATOMIC, pc, position)) {
          return 0;
        }
        pc++;
        continue;

      case GRX_OP_ATOMIC_END: {
        // Everything the region pushed is discarded, so nothing can
        // backtrack into it. The capture and register frames are kept - the
        // region's writes stand, and must still be undone if the *whole*
        // group is backtracked past.
        size_t write = floor;
        size_t read = floor;
        size_t barrier = bt->depth;
        for (size_t i = bt->depth; i > floor; i--) {
          if (bt->stack[i - 1].kind == FRAME_ATOMIC) {
            barrier = i - 1;
            break;
          }
        }
        for (read = barrier + 1, write = barrier; read < bt->depth; read++) {
          // Both resume kinds. FRAME_ALTERNATIVE is a resume point that
          // `(*THEN)` can name, and for one revision this line kept them -
          // which left `\R` able to give back the LF of a CR LF pair, the
          // one thing pcre2pattern says its `(?>` is there to prevent.
          if (bt->stack[read].kind != FRAME_RESUME
              && bt->stack[read].kind != FRAME_ALTERNATIVE) {
            bt->stack[write++] = bt->stack[read];
          }
        }
        bt->depth = write;
        pc++;
        continue;
      }

      case GRX_OP_LOOK: {
        int negative = inst->mode == GRX_LOOK_AHEAD_NEGATIVE
            || inst->mode == GRX_LOOK_BEHIND_NEGATIVE;

        // The body runs as its own sub-match above this point on the stack,
        // and everything it pushed is discarded afterwards: a lookaround
        // either holds here or does not, and there is nothing to backtrack
        // into. The capture writes it made are turned into undo frames on
        // the caller's stack so that backtracking past the whole lookaround
        // still puts them back.
        size_t * before = gcu_allocator_malloc(
            bt->allocator, bt->captures * sizeof(size_t));
        if (!before) {
          bt->failure = GRX_ERR_OOM;
          return 0;
        }
        memcpy(before, bt->slots, bt->captures * sizeof(size_t));

        size_t body_floor = bt->depth;
        size_t end = 0;
        size_t min = 0;
        size_t max = 0;
        int body_matched;
        size_t outer_keep = bt->keep_floor;
        if (negative && (inst->flags & GRX_INST_KEEP_CAPTURES)) {
          bt->keep_floor = body_floor;
        }
        if (grx_program_look_span(bt->program, inst->x, &min, &max)) {
          body_matched = look_behind_forward(
              bt, pc + 1, position, min, max, before, &end);
        }
        else {
          size_t outer_look_end = bt->look_end;
          bt->look_end = GRX_NPOS;
          body_matched = run(bt, pc + 1, position, body_floor, 0, &end);
          bt->look_end = outer_look_end;
        }
        bt->keep_floor = outer_keep;
        bt->depth = body_floor;
        if (bt->failure != GRX_OK) {
          gcu_allocator_free(bt->allocator, before);
          return 0;
        }
        if (verb_escapes_assertion(bt, negative)) {
          memcpy(bt->slots, before, bt->captures * sizeof(size_t));
          gcu_allocator_free(bt->allocator, before);
          return 0;
        }

        if (body_matched == negative) {
          // A positive lookaround whose body failed, or a negative one whose
          // body succeeded. Either way the construct fails and the captures
          // go back to what they were.
          memcpy(bt->slots, before, bt->captures * sizeof(size_t));
          gcu_allocator_free(bt->allocator, before);
          ok = 0;
          break;
        }

        if (negative && !(inst->flags & GRX_INST_KEEP_CAPTURES)) {
          // ECMA-262 22.2.2.4: a negative lookaround leaves the captures as
          // they were, whatever its body touched on the way to failing.
          // Perl does not - see GRX_INST_KEEP_CAPTURES - and there the
          // writes fall through to the same bookkeeping a positive
          // lookaround's do.
          memcpy(bt->slots, before, bt->captures * sizeof(size_t));
        }
        else {
          for (size_t i = 0; i < bt->captures; i++) {
            if (bt->slots[i] == before[i]) {
              continue;
            }
            size_t old = before[i];
            size_t current = bt->slots[i];
            bt->slots[i] = old;
            if (!save_slot(bt, i)) {
              gcu_allocator_free(bt->allocator, before);
              return 0;
            }
            bt->slots[i] = current;
          }
        }
        gcu_allocator_free(bt->allocator, before);

        pc = inst->y;
        continue;
      }

      case GRX_OP_SCAN: {
        // `(*scs:(n)body)`. The body runs over what one group captured, with
        // that substring standing in for the whole subject: anchored at its
        // start, unable to read past its end, and zero-width where it
        // stands. Which group: the first of the listed ones that is set,
        // which is how pcre2test answers `(?:(x)|y)(b)(*scs:(1,2)b)` - group
        // one is unset, so the scan runs over group two.
        size_t count = 0;
        const uint32_t * groups
            = grx_program_scan_list(bt->program, inst->x, &count);
        if (!groups) {
          bt->failure = GRX_ERR_INVALID;
          return 0;
        }

        size_t from = GRX_NPOS;
        size_t to = GRX_NPOS;
        for (size_t i = 0; i < count; i++) {
          size_t first = (size_t)groups[i] * 2;
          if (first + 1 < bt->captures && bt->slots[first] != GRX_NPOS
              && bt->slots[first + 1] != GRX_NPOS) {
            from = bt->slots[first];
            to = bt->slots[first + 1];
            break;
          }
        }
        if (from == GRX_NPOS) {
          // No listed group has captured anything, so there is no substring
          // to scan and the assertion cannot hold.
          ok = 0;
          break;
        }

        size_t * before = gcu_allocator_malloc(
            bt->allocator, bt->captures * sizeof(size_t));
        if (!before) {
          bt->failure = GRX_ERR_OOM;
          return 0;
        }
        memcpy(before, bt->slots, bt->captures * sizeof(size_t));

        size_t outer_start = bt->window_start;
        size_t outer_end = bt->window_end;
        bt->window_start = from;
        bt->window_end = to;
        size_t body_floor = bt->depth;
        size_t end = 0;
        size_t outer_look_end = bt->look_end;
        bt->look_end = GRX_NPOS;
        int body_matched = run(bt, pc + 1, from, body_floor, 0, &end);
        bt->look_end = outer_look_end;
        bt->depth = body_floor;
        bt->window_start = outer_start;
        bt->window_end = outer_end;
        if (bt->failure != GRX_OK) {
          gcu_allocator_free(bt->allocator, before);
          return 0;
        }
        if (verb_escapes_assertion(bt, 0)) {
          memcpy(bt->slots, before, bt->captures * sizeof(size_t));
          gcu_allocator_free(bt->allocator, before);
          return 0;
        }

        if (!body_matched) {
          memcpy(bt->slots, before, bt->captures * sizeof(size_t));
          gcu_allocator_free(bt->allocator, before);
          ok = 0;
          break;
        }

        // The captures the body set stand, and are undone if the whole
        // construct is backtracked past - the same bookkeeping a positive
        // lookaround does, for the same reason.
        for (size_t i = 0; i < bt->captures; i++) {
          if (bt->slots[i] == before[i]) {
            continue;
          }
          size_t old = before[i];
          size_t current = bt->slots[i];
          bt->slots[i] = old;
          if (!save_slot(bt, i)) {
            gcu_allocator_free(bt->allocator, before);
            return 0;
          }
          bt->slots[i] = current;
        }
        gcu_allocator_free(bt->allocator, before);

        pc = inst->y;
        continue;
      }

      case GRX_OP_MATCH:
        if (toplevel
            && !grx_exec_accepts(bt->request, bt->slots[0], position)) {
          // Not a match the caller will take. Fall through to backtracking
          // so that a longer alternative from the same position still has
          // its chance.
          ok = 0;
          break;
        }
        if (!toplevel && bt->look_end != GRX_NPOS
            && position != bt->look_end) {
          // A forward lookbehind body that ran out somewhere other than the
          // assertion. Not the body failing - a shorter way of matching it
          // than this candidate start needs - so keep backtracking.
          ok = 0;
          break;
        }
        if (toplevel && bt->longest) {
          // Strictly longer only: the first path to reach a given end is the
          // one whose captures the documented approximation reports, and a
          // later path of the same length must not displace it.
          if (bt->best_end == GRX_NPOS || position > bt->best_end) {
            bt->best_end = position;
            for (size_t i = 0; i < bt->slot_count; i++) {
              bt->best_slots[i] = bt->slots[i];
            }
            if (position >= bt->window_end) {
              // Nothing can be longer than everything. Worth the branch:
              // without it `\(a*\)*\1` against twenty characters walks the
              // whole tree to prove there is no longer match than the one
              // that already reached the end, and hits max_steps doing it.
              // With it the answer comes back at once, and both references
              // answer this shape at once too.
              *out_end = position;
              return 1;
            }
          }
          ok = 0;
          break;
        }
        *out_end = position;
        return 1;

      case GRX_OP_KEEP:
        // `\K` moves the reported start of the match to here. Through the
        // undo stack, because backtracking past it has to put the old start
        // back - `(a\Kb|ac)` against "ac" must report from the `a`.
        if (!save_slot(bt, 0)) {
          return 0;
        }
        bt->slots[0] = position;
        pc++;
        continue;

      case GRX_OP_VERB:
        switch ((GRX_VerbKind)inst->mode) {
          case GRX_VERB_ACCEPT:
            if (bt->call_depth > call_floor) {
              // pcre2pattern: "in a recursion or subroutine call, (*ACCEPT)
              // causes only that subroutine to return". Perl agrees -
              // `(?(DEFINE)(?<a>x(*ACCEPT)y))(?&a)z` matches "xz" in both,
              // where ending the whole match here would stop at the `x`. The
              // open groups inside the call are closed first, because the
              // verb is still an accept as far as they are concerned, and
              // then the call returns the way RET does.
              for (size_t i = 0; i + 1 < bt->captures; i += 2) {
                if (bt->slots[i] != GRX_NPOS
                    && bt->slots[i + 1] == GRX_NPOS) {
                  if (!save_slot(bt, i + 1)) {
                    return 0;
                  }
                  bt->slots[i + 1] = position;
                }
              }
              if (!return_from_call(bt, &pc, position)) {
                return 0;
              }
              continue;
            }

            // The match ends here, with whatever the groups hold. The
            // closing SAVE of group 0 is skipped over, so it is written by
            // hand: an accepted match has an end whether or not the program
            // reached its own.
            if (toplevel
                && !grx_exec_accepts(bt->request, bt->slots[0], position)) {
              ok = 0;
              break;
            }
            // pcre2pattern: the match succeeds here, and "any capturing
            // parentheses that are open are closed". Without this
            // `(A(A|B(*ACCEPT)|C)D)(E)` against "AB" reports no groups at
            // all, where both references report two.
            for (size_t i = 0; i + 1 < bt->captures; i += 2) {
              if (bt->slots[i] != GRX_NPOS && bt->slots[i + 1] == GRX_NPOS) {
                bt->slots[i + 1] = position;
              }
            }
            bt->slots[1] = position;
            *out_end = position;
            return 1;

          case GRX_VERB_FAIL:
            ok = 0;
            break;

          case GRX_VERB_MARK:
            // The one verb that acts on the way through. Everything it does
            // is leave a name behind, and the frame is what takes it away
            // again when this path is abandoned.
            if (!push_mark(bt, inst->x, position)) {
              return 0;
            }
            pc++;
            continue;

          case GRX_VERB_SKIP:
            // A named SKIP resumes where its mark was set, not where it
            // stands, so it needs a frame of its own. An unnamed one is one
            // of the four below.
            if (inst->x != GRX_INDEX_NONE) {
              if (!push(bt, FRAME_SKIP_MARK, inst->x, position)) {
                return 0;
              }
              pc++;
              continue;
            }
            if (!push(bt, FRAME_VERB, (uint32_t)inst->mode, position)) {
              return 0;
            }
            pc++;
            continue;

          case GRX_VERB_THEN:
          case GRX_VERB_PRUNE:
          case GRX_VERB_COMMIT:
          default:
            // None of the four does anything when it is reached. They act
            // when backtracking *returns* to them, which is what makes
            // `a(*PRUNE)b` match "xab": the verb is passed through, `b`
            // succeeds, and nothing ever comes back. A verb that failed its
            // own path on the way through would refuse every subject.
            if (!push(bt, FRAME_VERB, (uint32_t)inst->mode, position)) {
              return 0;
            }
            pc++;
            continue;
        }
        break;

      case GRX_OP_COND: {
        int holds = 0;
        switch ((GRX_CondKind)inst->mode) {
          case GRX_COND_RECURSION_ANY:
            holds = bt->call_depth > 0;
            break;
          case GRX_COND_RECURSION_GROUP:
            holds = bt->call_depth > 0
                && bt->call_group[bt->call_depth - 1] == inst->x;
            break;
          case GRX_COND_GROUP_SET:
          default: {
            size_t start_slot = (size_t)inst->x * 2;
            size_t end_slot = start_slot + 1;
            holds = end_slot < bt->captures
                && bt->slots[start_slot] != GRX_NPOS
                && bt->slots[end_slot] != GRX_NPOS;
            break;
          }
        }
        pc = holds ? pc + 1 : inst->y;
        continue;
      }

      case GRX_OP_CALL: {
        const GRX_Limits * call_limits = bt->request->limits;
        if (call_limits->max_recursion_depth
            && bt->call_depth + 1 > call_limits->max_recursion_depth) {
          bt->failure = GRX_ERR_LIMIT;
          return 0;
        }
        if (!call_reserve(bt, bt->call_depth + 1)) {
          return 0;
        }
        // The depth to come back to, recorded before the call so that
        // backtracking out of a call that failed leaves the stack as it was.
        if (!push(bt, FRAME_CALL, (uint32_t)bt->call_depth, position)) {
          return 0;
        }
        // The return is to the instruction after the CALL, which is the
        // ATOMIC_END that closes it; `y` is free to carry the group number,
        // which `(?(R1))` needs and nothing else records.
        bt->call_return[bt->call_depth] = pc + 1;
        bt->call_group[bt->call_depth] = inst->y;
        memcpy(bt->call_slots + bt->call_depth * bt->slot_count, bt->slots,
            bt->slot_count * sizeof(size_t));
        bt->call_depth++;
        pc = inst->x;
        continue;
      }

      case GRX_OP_RET:
        if (!return_from_call(bt, &pc, position)) {
          return 0;
        }
        continue;

      default:
        bt->failure = GRX_ERR_INTERNAL;
        return 0;
    }

    if (ok) {
      if (consumed) {
        position = reverse ? position - width : position + width;
      }
      pc++;
      continue;
    }

    if (!backtrack(bt, &pc, &position, floor)) {
      return 0;
    }
  }
}

GRX_Result grx_exec_backtrack(
    const GRX_ExecRequest * request, int * out_matched) {
  if (!request || !out_matched) {
    return GRX_ERR_INVALID;
  }
  *out_matched = 0;

  const GRX_Program * program = &request->regex->program;
  if (!program->insts.count) {
    return GRX_ERR_INVALID;
  }

  Backtrack bt = {
    .request = request,
    .program = program,
    .allocator = request->regex->allocator,
    .captures = 2 * (request->regex->capture_count + 1),
    .registers = program->register_count,
    .shadow = 0,
    .slot_count = 0,
    .slots = NULL,
    .stack = NULL,
    .depth = 0,
    .capacity = 0,
    .steps = 0,
    .utf = (program->flags & GRX_PROGRAM_UTF) != 0,
    .failure = GRX_OK,
    .longest = program->preference == GRX_PREFER_LEFTMOST_LONGEST,
    .best_end = GRX_NPOS,
    .best_slots = NULL,
    .visited = NULL,
    .stride = request->length + 1,
    .memo_after = 0,
    .call_return = NULL,
    .call_group = NULL,
    .call_slots = NULL,
    .call_depth = 0,
    .call_capacity = 0,
    .look_end = GRX_NPOS,
    .keep_floor = GRX_NPOS,
    .verb_stop = VERB_NONE,
    .skip_to = 0,
    .window_start = 0,
    .window_end = request->length,
    .marks = NULL,
    .mark_depth = 0,
    .mark_capacity = 0,
    .mark = GRX_INDEX_NONE,
    .nomatch_mark = GRX_INDEX_NONE,
  };
  if (!bt.allocator) {
    bt.allocator = grx_allocator_default();
  }

  // The shadow spans sit after the registers, and only for a program that
  // has a backreference to read them - see GRX_PROGRAM_SHADOW_CAPTURES.
  bt.slot_count = bt.captures + bt.registers;
  if (program->flags & GRX_PROGRAM_SHADOW_CAPTURES) {
    bt.shadow = bt.slot_count;
    bt.slot_count += bt.captures;
  }

  bt.slots = gcu_allocator_malloc(
      bt.allocator, bt.slot_count * sizeof(size_t));
  if (!bt.slots) {
    return GRX_ERR_OOM;
  }
  if (bt.longest) {
    bt.best_slots = gcu_allocator_malloc(
        bt.allocator, bt.slot_count * sizeof(size_t));
    if (!bt.best_slots) {
      gcu_allocator_free(bt.allocator, bt.slots);
      return GRX_ERR_OOM;
    }
  }

  if (request->memoize && bt.longest) {
    // The bit-state engine, named explicitly, for a dialect that wants the
    // longest match. Its bitmap records that a state failed so that the
    // state is never tried again, and this mode reports failure from a
    // *match* - so the bitmap would prune the very paths it exists to find.
    // Refused rather than run without it, for the reason GRX_ENGINE_PIKE is
    // refused a program it cannot run: a caller who named an engine asked
    // for that engine's guarantee, and quietly giving them a different
    // answer is worse than telling them it cannot be done.
    gcu_allocator_free(bt.allocator, bt.best_slots);
    gcu_allocator_free(bt.allocator, bt.slots);
    return GRX_ERR_UNSUPPORTED;
  }
  if (request->memoize) {
    size_t bytes = grx_exec_bitmap_bytes(request->regex, request->length);
    if (bytes == GRX_NPOS
        || (request->limits->max_match_memory
            && bytes > request->limits->max_match_memory)) {
      // Refused rather than run without the bitmap: a caller who named this
      // engine asked for the linear-time guarantee, and quietly giving them
      // the exponential one is the substitution GRX_ENGINE_PIKE already
      // refuses to make.
      gcu_allocator_free(bt.allocator, bt.slots);
      return GRX_ERR_LIMIT;
    }
    bt.visited = gcu_allocator_calloc(bt.allocator, bytes, 1);
    if (!bt.visited) {
      gcu_allocator_free(bt.allocator, bt.slots);
      return GRX_ERR_OOM;
    }
  }
  else if (!bt.longest && grx_exec_program_is_memoizable(request->regex)) {
    // The plain backtracker, on a program the memo would be sound for: arm
    // the late cache rather than allocating a bitmap a run that never needs
    // one would pay for. The threshold is the state count; anything past it
    // is repeated work. Programs the memo would be *unsound* for - a
    // backreference, a lookaround, a recursion, a progress register - get
    // nothing, and are the reason this engine still has an exponential worst
    // case to document.
    size_t instructions = program->insts.count;
    size_t positions = request->length + 1;
    if (instructions && positions <= GRX_NPOS / instructions) {
      bt.memo_after = instructions * positions;
    }
  }

  GRX_Result result = GRX_OK;
  size_t start = request->start;
  for (;;) {
    for (size_t i = 0; i < bt.slot_count; i++) {
      bt.slots[i] = GRX_NPOS;
    }
    bt.depth = 0;

    size_t end = 0;
    bt.best_end = GRX_NPOS;
    bt.call_depth = 0;
    bt.verb_stop = VERB_NONE;
    // The mark stack is the path's, and each attempt is a new path.
    // `nomatch_mark` is not reset: what a failed search reports is the last
    // mark it reached anywhere, which is pcre2_get_mark()'s answer too.
    bt.mark_depth = 0;
    bt.mark = GRX_INDEX_NONE;
    if (run(&bt, 0, start, 0, 1, &end)) {
      *out_matched = 1;
      break;
    }
    if (bt.failure != GRX_OK) {
      result = bt.failure;
      break;
    }
    if (bt.longest && bt.best_end != GRX_NPOS) {
      // Exhausted, and something matched along the way. The run reported
      // failure at every one of them on purpose; this is where the longest
      // is taken, and taking it here rather than inside run() is what keeps
      // "leftmost" intact - no later start is tried once this one matched.
      for (size_t i = 0; i < bt.slot_count; i++) {
        bt.slots[i] = bt.best_slots[i];
      }
      *out_matched = 1;
      break;
    }
    // A `(*THEN)` that reached the top with no alternative left is a
    // `(*PRUNE)` there, which is the ordinary "try the next position".
    if (bt.verb_stop == VERB_STOP_THEN) {
      bt.verb_stop = VERB_STOP_PRUNE;
    }
    if (bt.verb_stop == VERB_STOP_COMMIT) {
      // `(*COMMIT)`: no further starting position is tried at all. The four
      // control verbs differ only here, which is why the engine's inner loop
      // records what was asked for and this loop is what acts on it.
      break;
    }
    if (request->anchored || start >= request->length) {
      break;
    }
    if (bt.verb_stop == VERB_STOP_SKIP && bt.skip_to > start) {
      // `(*SKIP)`: the next attempt begins where the verb was reached, not
      // one character on. Never backwards, and never at the same place -
      // either would make the search loop forever.
      start = bt.skip_to;
      continue;
    }

    // Leftmost: try each starting position in turn, and take the first that
    // matches. Stepping by a character rather than a byte matters in UTF
    // mode - a match may not begin inside one.
    uint32_t codepoint = 0;
    size_t width = 0;
    if (!read_forward(&bt, start, &codepoint, &width)) {
      result = GRX_ERR_INVALID;
      break;
    }
    start += width;
  }

  if (result == GRX_OK && request->match) {
    // pcre2_get_mark(): the last mark still standing on the path that
    // matched, or - when nothing matched - the last one reached at all.
    request->match->mark = *out_matched ? bt.mark : bt.nomatch_mark;
  }

  if (result == GRX_OK && *out_matched && request->match) {
    GRX_Match * match = request->match;
    for (size_t i = 0; i < match->count; i++) {
      size_t first = 2 * i;
      size_t second = first + 1;
      if (second < bt.captures && bt.slots[first] != GRX_NPOS
          && bt.slots[second] != GRX_NPOS) {
        match->captures[i].start = bt.slots[first];
        match->captures[i].end = bt.slots[second];
      }
      else {
        match->captures[i] = (GRX_Capture) {GRX_NPOS, GRX_NPOS};
      }
    }
  }

  if (request->out_steps) {
    *request->out_steps = bt.steps;
  }
  gcu_allocator_free(bt.allocator, bt.visited);
  gcu_allocator_free(bt.allocator, bt.best_slots);
  gcu_allocator_free(bt.allocator, bt.slots);
  gcu_allocator_free(bt.allocator, bt.stack);
  gcu_allocator_free(bt.allocator, bt.call_return);
  gcu_allocator_free(bt.allocator, bt.call_group);
  gcu_allocator_free(bt.allocator, bt.call_slots);
  gcu_allocator_free(bt.allocator, bt.marks);
  return result;
}
