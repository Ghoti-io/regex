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
#include <ghoti.io/unicode/script.h>
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
  /** Which limit, so the caller is told more than "a limit". */
  GRX_Diag failure_diag;

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
  /**
   * Which division of that match the groups get: documentation/dialects.md
   * section 5.1. GRX_SUBMATCH_FIRST_PATH keeps the first path to reach the
   * greatest end, which is this engine's own alternation order;
   * GRX_SUBMATCH_POSIX compares the two and keeps the one POSIX asks for.
   */
  GRX_SubmatchRule submatch;
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
   * How deep run() has called itself, against GRX_BACKTRACK_MAX_C_DEPTH.
   *
   * Not `depth`, which counts backtrack frames on the heap, and not
   * `call_depth`, which counts `(?R)` and subroutine calls whatever engine
   * machinery they use. This is the C stack itself.
   */
  size_t run_depth;

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
   * Inside a KEEP body: the last value each slot was given by an iteration
   * that *finished*, or NULL outside one.
   *
   * The obvious implementation - stop undoing captures while the body runs,
   * so whatever it wrote is still there when it fails - was wrong twice over.
   * A repeat clears its group at the top of every iteration (GRX_OP_RESET,
   * the capture-reset axis), so a body failing part way through an iteration
   * fails with that clear applied and no write yet to replace it: not undoing
   * it keeps the *clear* rather than the value, and which of the two a
   * pattern gets depends on where in the iteration the failure fell, which is
   * a rule stated in terms of this file's own lowering. Worse, un-restored
   * slots are incoherent *during* the search and not merely at the end, so a
   * group could be read with its start from one abandoned path and its end
   * from another - `^(a*?)(?!(a{6}|a{5})*$)` against 31 a's reported a group
   * as 28-27, ending before it began.
   *
   * So captures are undone normally, exactly as everywhere else, and this is
   * the whole of the rule instead.
   *
   * So the value a group reports is the last one an iteration finished
   * writing, recorded here when a group's closing SAVE runs. A group that
   * never completed one inside the body has no entry and keeps whatever it
   * held before. GRX_NPOS is the "nothing recorded" mark, and it cannot
   * collide with a real one: a closing SAVE always writes a real offset.
   *
   * Saved and replaced around a nested lookaround, so an inner body's
   * completions cannot be read as an outer body's.
   */
  size_t * keep_last;

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

  /**
   * Where the current attempt began, for GRX_Callout::start.
   *
   * Not `slots[0]`, which is the *reported* start and moves under `\K`, and
   * not `request->start`, which is where the whole search began and does
   * not move at all. PCRE2's callout block calls this `start_match` and
   * means the bumpalong position, which is the one thing neither of the
   * other two says.
   */
  size_t attempt_start;

  /**
   * The spans handed to a @ref GRX_CalloutFn, or NULL when none is
   * registered.
   *
   * Allocated once for the run rather than per callout: a callout in a loop
   * fires as often as the loop turns, and a malloc for each would make
   * observing a match cost more than running it.
   */
  GRX_Capture * callout_captures;
} Backtrack;

/**
 * Whether (pc, position) has been tried already; records it if not.
 *
 * Only ever reached on the way *into* a state. A state that succeeded made
 * the whole run return, so a bit that is set is a state that failed, and a
 * state that failed once fails always.
 *
 * With one exception, and it is deliberate: a program carrying
 * GRX_PROGRAM_SIMULATED_LOOP has an epsilon cycle with no guard in it, so a
 * set bit there can also be a state still on the stack. Skipping *that* one
 * is not an optimisation and does change the answer - it is the answer, and
 * it is the same one the Pike VM's thread list gives. See
 * GRX_EMPTY_LOOP_SIMULATE.
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
    bt->failure_diag = GRX_DIAG_LIMIT_MATCH_MEMORY;
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
    bt->failure_diag = GRX_DIAG_LIMIT_BACKTRACK;
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
      bt->failure_diag = GRX_DIAG_LIMIT_BACKTRACK;
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
 * Whether the subject from `from` to `to` is a script run.
 *
 * One pass, because the rule cannot be decomposed: a sequence can fail
 * while every adjacent pair passes, so a window-at-a-time check over a long
 * span would answer a different question. `GUNI_ScriptRun` is what makes
 * one pass enough - a few words carried across the span rather than the
 * span carried in a buffer.
 *
 * The code points examined are charged to `max_steps`. A script run inside
 * a loop is re-checked on every backtrack into it, so a span the engine
 * walks is work the engine did, and a caller who bounded the match has
 * bounded this too. Without it `(*sr:.*)x` against a large subject would
 * spend time no limit could see.
 *
 * Non-UTF mode reads bytes, which is what every other consuming
 * instruction does there.
 */
static int span_is_script_run(Backtrack * bt, size_t from, size_t to) {
  GUNI_ScriptRun state;
  guni_script_run_begin(&state);

  int ok = 1;
  size_t position = from;
  while (position < to) {
    uint32_t codepoint = 0;
    size_t width = 0;
    if (!read_forward(bt, position, &codepoint, &width)) {
      // The span was matched by this same engine, so it decodes. A failure
      // here would be a defect rather than an input, and refusing is the
      // answer that cannot invent a match.
      ok = 0;
      break;
    }
    position += width;
    bt->steps++;
    if (!guni_script_run_add(&state, codepoint)) {
      // Charged in full before stopping, so that the cost of finding out is
      // counted whether the answer is yes or no.
      ok = 0;
      break;
    }
  }
  return ok;
}

/**
 * Report a `(?C...)` to the caller's function.
 *
 * The one place this engine calls out of the library while a match is in
 * flight, so everything it hands over is a copy or a borrow of something
 * already immutable: the block lives on this frame, the spans live in a
 * buffer the run owns, and neither outlives the call.
 *
 * @return Non-zero to carry on. Zero with `bt->failure` set when the
 *   function asked to stop; `*out_fail` is set instead when it asked only
 *   for this path to fail.
 */
static int fire_callout(Backtrack * bt, const GRX_Inst * inst,
    size_t position, int * out_fail) {
  const GRX_ProgramCallout * record
      = grx_program_callout(bt->program, inst->x);
  if (!record || !bt->callout_captures) {
    bt->failure = GRX_ERR_INTERNAL;
    bt->failure_diag = GRX_DIAG_INTERNAL;
    return 0;
  }

  size_t groups = bt->captures / 2;
  for (size_t i = 0; i < groups; i++) {
    // A group that is *open* - its start written and its end not - has
    // captured nothing yet, and is reported as unset. `{start, GRX_NPOS}`
    // would be a span whose end a caller could subtract, and the pair is
    // the one shape GRX_Capture has no way to mark as half-made.
    //
    // It is also pcre2's answer: `((?C)a)(b)` reports capture_top 1 there,
    // not 2, and this library said 2 until tools/oracle/callout_diff.py
    // was run for the first time.
    int complete = bt->slots[2 * i] != GRX_NPOS
        && bt->slots[2 * i + 1] != GRX_NPOS;
    bt->callout_captures[i] = complete
        ? (GRX_Capture) {bt->slots[2 * i], bt->slots[2 * i + 1]}
        : (GRX_Capture) {GRX_NPOS, GRX_NPOS};
  }

  // `nomatch_mark`, not `mark`. pcre2api defines the callout block's as
  // "the most recently passed (*MARK), (*PRUNE), or (*THEN) item in the
  // match" - the running value, which a branch being abandoned does not
  // take back and a failed attempt does not clear. `bt->mark` is the
  // narrower "still standing on this path", which is the right answer to a
  // different question and the one grx_match_mark() reports after a match.
  //
  // Measured, not assumed: pcre2test on `(?C1)x(*MARK:m)y` against "xaby"
  // reports the mark at every attempt after the first, where a callout
  // standing *before* the `(*MARK:m)` can be on no path that passed it.
  const GRX_Regex * regex = bt->request->regex;
  uint32_t seen = bt->nomatch_mark;
  const char * mark = seen != GRX_INDEX_NONE && regex->mark_names
          && seen < regex->mark_count
      ? regex->mark_names[seen]
      : NULL;

  GRX_Callout block = {
    .number = record->number,
    .string = grx_program_callout_string(bt->program, record->string),
    .string_length = record->string_length,
    .string_offset = record->string_offset,
    .pattern_offset = record->pattern_offset,
    .subject = bt->request->subject,
    // The search's subject, not `window_end`: inside a `(*scs:(n)...)` the
    // engine treats a captured substring as the whole subject, and a
    // callout's job is to say where the match is in the text the *caller*
    // passed. `position` is an offset into that same buffer either way.
    .subject_length = bt->request->length,
    .start = bt->attempt_start,
    .position = position,
    .count = groups,
    .captures = bt->callout_captures,
    .mark = mark,
  };

  *out_fail = 0;
  GRX_Result result
      = bt->request->callout(&block, out_fail, bt->request->callout_data);
  if (result == GRX_OK) {
    return 1;
  }

  // A function that returned something outside the enum is out of contract,
  // and passing it through would make a public entry point return a value
  // that is not a GRX_Result. GRX_DIAG_CALLOUT_STOPPED's own row says what
  // that becomes.
  bt->failure = (unsigned)result < (unsigned)GRX_RESULT_COUNT
      ? result
      : grx_diag_result(GRX_DIAG_CALLOUT_STOPPED);
  bt->failure_diag = GRX_DIAG_CALLOUT_STOPPED;
  return 0;
}

/**
 * Whether the caller said the start of the subject is not a start of line.
 *
 * A flag about the *caller's* subject, so it says nothing about the start of
 * a scan-substring window: inside one, `^` holds because the substring is
 * the subject there, and PCRE2_NOTBOL was about a different string.
 *
 * And a flag about a *line*, so it says nothing about `\A`, which shares a
 * kind with `^` here. PCRE2_NOTBOL states that it does not affect `\A`, and
 * glibc's REG_NOTBOL leaves GNU's `` \` `` alone; GRX_INST_LINE_ANCHOR is
 * which of the two spellings produced this instruction.
 */
static int at_subject_start_suppressed(
    const Backtrack * bt, const GRX_Inst * inst) {
  return bt->request->not_bol && !bt->window_start
      && (inst->flags & GRX_INST_LINE_ANCHOR);
}

/** The same for the end, and PCRE2_NOTEOL. */
static int at_subject_end_suppressed(
    const Backtrack * bt, const GRX_Inst * inst) {
  return bt->request->not_eol && bt->window_end == bt->request->length
      && (inst->flags & GRX_INST_LINE_ANCHOR);
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

  // `(*CRLF)`, `(*ANYCRLF)` and `(*ANY)`: a CR LF pair is one terminator.
  // The same three clauses as exec_pike.c's, which this switch has to
  // agree with exactly - the helpers are shared for that reason.
  int crlf = (inst->flags & GRX_INST_NEWLINE_CRLF) != 0;
  const char * text = request->subject;

  // NOTBOL and NOTEOL suppress only the end-of-subject halves; see the same
  // switch in exec_pike.c, which this one has to agree with exactly.
  switch ((GRX_AssertKind)inst->mode) {
    case GRX_ASSERT_START_SUBJECT:
      return position == bt->window_start && !at_subject_start_suppressed(bt, inst);
    case GRX_ASSERT_END_SUBJECT:
      return position == bt->window_end && !at_subject_end_suppressed(bt, inst);
    case GRX_ASSERT_END_BEFORE_NEWLINE:
      if (at_subject_end_suppressed(bt, inst)) {
        return 0;
      }
      if (position == bt->window_end) {
        return 1;
      }
      return (has_after && in_class(bt, inst->x, after)
                 && position + width == bt->window_end)
          || (crlf && grx_crlf_begins_at(text, bt->window_end, position)
              && position + 2 == bt->window_end);
    case GRX_ASSERT_START_LINE:
      return (position == bt->window_start && !at_subject_start_suppressed(bt, inst))
          || (has_before && in_class(bt, inst->x, before)
              && !(crlf
                  && grx_between_crlf(
                      text, bt->window_start, bt->window_end, position)))
          || (crlf && grx_crlf_ends_at(text, bt->window_start, position));

    case GRX_ASSERT_START_LINE_INTERIOR:
      // The same, less the position after a newline that ends the
      // subject: there is no line there to be at the start of.
      return (position == bt->window_start && !at_subject_start_suppressed(bt, inst))
          || (position != bt->window_end
              && ((has_before && in_class(bt, inst->x, before)
                      && !(crlf
                          && grx_between_crlf(text, bt->window_start,
                              bt->window_end, position)))
                  || (crlf
                      && grx_crlf_ends_at(text, bt->window_start, position))));
    case GRX_ASSERT_END_LINE:
      return (position == bt->window_end && !at_subject_end_suppressed(bt, inst))
          || (has_after && in_class(bt, inst->x, after))
          || (crlf && grx_crlf_begins_at(text, bt->window_end, position));
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

    // Vim's `\<` and `\>`, which ask a different question of the same two
    // characters: not "is one a word character and the other not" but "do
    // they belong to different classes", of which vim has nine. No class
    // index is carried - src/unicode/vim_class.c holds the table - and the
    // absent character at either end is a blank, which is what vim reads
    // there too.
    case GRX_ASSERT_WORD_CLASS_START:
    case GRX_ASSERT_WORD_CLASS_END: {
      // The character *before* is the base of the cluster it belongs to,
      // which is what vim asks: `\>` holds between U+65E5 U+0301 and "x"
      // there, and reading the mark instead would say both sides are
      // keyword characters and no boundary falls.
      uint32_t base = 0;
      int has_base = grx_cluster_base_before(
          text, bt->window_start, position, bt->utf, &base);
      return inst->mode == GRX_ASSERT_WORD_CLASS_START
          ? grx_vim_word_start(has_base, base, has_after, after)
          : grx_vim_word_end(has_base, base, has_after, after);
    }

    // Vim's composing clusters: the end of a cluster, and the start of one.
    // `inst->x` is the class of composing characters in both.
    case GRX_ASSERT_NOT_COMPOSING:
      return !(has_after && in_class(bt, inst->x, after));

    case GRX_ASSERT_CLUSTER_BOUNDARY:
      return !has_before || !(has_after && in_class(bt, inst->x, after));

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

    case GRX_ASSERT_BYTE_COLUMN:
      // Vim's `\%23c` and kin, as an inclusive range of offsets. The
      // subject's start is where the count begins, not the line's: over a
      // string vim has one line, and `\%1c` holds at offset 0 and nowhere
      // after a line break.
      return position >= inst->x && position <= inst->y;

    case GRX_ASSERT_SCREEN_COLUMN: {
      // Vim's `\%23v`. The column of every offset was computed once before
      // the search - see GRX_ExecRequest::columns - so this is a lookup and
      // not a walk. Zero means "no column of its own", which is an offset
      // inside a character or at a combining one, and no `\%Nv` holds
      // there whatever N is.
      if (!request->columns || position > request->length) {
        return 0;
      }
      uint32_t column = request->columns[position];
      return column != 0 && column >= inst->x && column <= inst->y;
    }

    case GRX_ASSERT_NEVER:
      // `\%V`, `\%#`, `\%23l`: compiled, and never true over a subject
      // that is not a buffer.
      return 0;

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
      return position == request->search_start;
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
/**
 * Which group an ambiguous backreference means, right now.
 *
 * A name may belong to several groups, and a reference written with it means
 * the first of them that is *set*. Both references agree and the rule is the
 * one grx_match_group_named() follows, so this is the same question asked at
 * match time: `(?(DEFINE)(?<n>a))(?<n>b)\k<n>` matches "abb" because the
 * DEFINE's group never ran, and `(?<n>a)(?<n>b)\k<n>` does not, because the
 * first group did run and captured "a".
 *
 * "Set" is read through the shadow spans where there are any, for the reason
 * backref_matches() reads them: a group that is open right now last closed
 * with something, and that is what a reference to it compares against.
 *
 * Returns the first set group, or the first in the list when none is set -
 * so that the caller's "the group did not participate" branch is reached
 * with a real group number and the dialect's answer to that question is
 * still the one given.
 */
static uint32_t ambiguous_group(const Backtrack * bt, const GRX_Inst * inst) {
  size_t count = 0;
  const uint32_t * groups
      = grx_program_scan_list(bt->program, inst->x, &count);
  if (!groups || !count) {
    return 0;
  }
  for (size_t i = 0; i < count; i++) {
    size_t start_slot = (size_t)groups[i] * 2;
    size_t end_slot = start_slot + 1;
    if (end_slot >= bt->captures) {
      continue;
    }
    if (bt->shadow) {
      start_slot += bt->shadow;
      end_slot += bt->shadow;
    }
    if (bt->slots[start_slot] != GRX_NPOS
        && bt->slots[end_slot] != GRX_NPOS) {
      return groups[i];
    }
  }
  return groups[0];
}

/**
 * Copy out the spans an assertion must be able to put back.
 *
 * The capture slots *and* the shadow spans, which is the half that was
 * missing. A shadow span is what a backreference reads
 * (backref_matches()), so an assertion that restored only the live captures
 * left its body's writes where a later reference could still find them:
 * `(?=(a))$|(a)\1` matched "aa" here and "a" in node and pcre2test,
 * because the `\1` of the second branch read what the *first* branch's
 * assertion had written on its way to failing. The live captures were right
 * throughout - group one was reported unset either way - so the only
 * visible symptom was the width of the match, which is why no conformance
 * vector caught it.
 *
 * Found by the Vim differential, and reachable from every dialect that has
 * both a lookaround and a backreference.
 *
 * Registers are copied and deliberately not restored: they are a repeat's
 * own bookkeeping, and the callers put back exactly what this pair names.
 */
static void snapshot_spans(const Backtrack * bt, size_t * out) {
  memcpy(out, bt->slots, bt->slot_count * sizeof(size_t));
}

/** Put back what snapshot_spans() took. */
static void restore_spans(Backtrack * bt, const size_t * before) {
  memcpy(bt->slots, before, bt->captures * sizeof(size_t));
  if (bt->shadow) {
    memcpy(bt->slots + bt->shadow, before + bt->shadow,
        (bt->slot_count - bt->shadow) * sizeof(size_t));
  }
}

static int backref_matches(const Backtrack * bt, const GRX_Inst * inst,
    size_t position, int reverse, size_t * out_width) {
  uint32_t group = (inst->flags & GRX_INST_AMBIGUOUS_REF)
      ? ambiguous_group(bt, inst)
      : inst->x;
  size_t start_slot = (size_t)group * 2;
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
 * The deepest run() will call itself, whatever the caller's limits say.
 *
 * run() recurses in C for three things, all of them assertions in the wide
 * sense: a lookahead body, a lookbehind's forward pass, and a sub-match
 * condition. One level of nested assertion is one C frame, so the engine's
 * stack use is linear in how deeply the *program* nests them - which is the
 * one place design.md section 9 invariant 6 does not hold, and now says so.
 *
 * The number is a stack budget rather than a taste, and it is bounded from
 * both sides.
 *
 * From below, by what has to keep working: assertion nesting can never
 * exceed parse nesting, so the default `max_nesting_depth` of 128 is the
 * deepest a pattern compiled at the defaults can be, and a pattern that
 * compiles has to be one that runs. The ceiling must clear 128, plus the
 * top-level frame.
 *
 * From above, by the smallest stack this library claims: the 256 KB every
 * harness gets in the soak (testing.md section 12). A level costs about 600
 * bytes in this library's own -O0 objects - tests/unit/test_stack.cpp
 * measures it rather than trusting the figure, because it moves with the
 * optimisation level, and 515 bytes at -O1 was what made a first draft of
 * this constant 240 and 40 KB over budget. 160 levels at 600 bytes is 96 KB,
 * two fifths of the smallest stack, leaving the caller's own frames the rest.
 *
 * That the limits field bounds this at all is why nothing had hit it. The
 * field is not enough on its own, being the caller's to raise: raising it to
 * the 480 the *parser* can take asks the matcher for 241 KB, which on a
 * 256 KB stack was a segmentation fault rather than a refusal. This is the
 * floor underneath it, and it is deliberately not a GRX_Limits field - a
 * bound whose whole job is to be un-liftable should not come with a lever.
 */
#define GRX_BACKTRACK_MAX_C_DEPTH 160

/** run(), less the C-stack bookkeeping. */
static int run_body(Backtrack * bt, uint32_t pc, size_t position, size_t floor,
    int toplevel, size_t * out_end);

static int run(Backtrack * bt, uint32_t pc, size_t position, size_t floor,
    int toplevel, size_t * out_end) {
  if (bt->run_depth >= GRX_BACKTRACK_MAX_C_DEPTH) {
    bt->failure = GRX_ERR_LIMIT;
    bt->failure_diag = GRX_DIAG_LIMIT_RECURSION_DEPTH;
    return 0;
  }
  bt->run_depth++;
  int matched = run_body(bt, pc, position, floor, toplevel, out_end);
  bt->run_depth--;
  return matched;
}

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
    restore_spans(bt, before);
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
    restore_spans(bt, before);
  }
  return matched;
}

static int run_body(Backtrack * bt, uint32_t pc, size_t position, size_t floor,
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
      bt->failure_diag = GRX_DIAG_LIMIT_STEPS;
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
            // `.` and `\N` refuse the position where a line terminator
            // *begins*, and under `(*CRLF)` that is a CR followed by an LF
            // while no single character ends a line at all. So `a..b`
            // refuses "a\r\nb" in pcre2test and `\r.b` accepts "\r\nb" -
            // the CR is refused and the LF is not, which is what "begins"
            // means and what a class could never say.
            if (ok && (inst->flags & GRX_INST_NEWLINE_CRLF)) {
              size_t at = reverse ? position - width : position;
              ok = !grx_crlf_begins_at(
                  bt->request->subject, bt->window_end, at);
            }
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
        // The program's closing save of group 0 carries
        // GRX_INST_SAVE_IF_UNSET when a `\ze` is somewhere in the pattern,
        // and then defers to whatever that marker left in the slot. Nothing
        // else in a program ever wears the flag, so this is the one
        // instruction it can change.
        if ((inst->flags & GRX_INST_SAVE_IF_UNSET) && inst->x < bt->captures
            && bt->slots[inst->x] != GRX_NPOS) {
          pc++;
          continue;
        }
        if (!save_slot(bt, inst->x)) {
          return 0;
        }
        if (inst->x < bt->captures) {
          bt->slots[inst->x] = position;
          // An odd slot is a group's end, so the group has just become whole.
          // Recorded rather than read off the slots at the end, because by
          // then a later iteration may have cleared it.
          if (bt->keep_last && (inst->x % 2) == 1
              && bt->slots[inst->x - 1] != GRX_NPOS) {
            bt->keep_last[inst->x - 1] = bt->slots[inst->x - 1];
            bt->keep_last[inst->x] = bt->slots[inst->x];
          }
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
          // Never emitted: a loop under GRX_EMPTY_LOOP_SIMULATE carries no
          // progress register, so there is no check to reach. Named here
          // rather than left to the default so that the enum's arms are
          // the enum's members.
          case GRX_EMPTY_LOOP_SIMULATE:
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
        // A conditional's condition: it chooses a branch rather than
        // failing, and `y` and `y + 1` are the two jumps that say which.
        int choosing = (inst->flags & GRX_INST_COND_ELSE) != 0;

        // The body runs as its own sub-match above this point on the stack,
        // and everything it pushed is discarded afterwards: a lookaround
        // either holds here or does not, and there is nothing to backtrack
        // into. The capture writes it made are turned into undo frames on
        // the caller's stack so that backtracking past the whole lookaround
        // still puts them back.
        size_t * before = gcu_allocator_malloc(
            bt->allocator, bt->slot_count * sizeof(size_t));
        if (!before) {
          bt->failure = GRX_ERR_OOM;
          return 0;
        }
        snapshot_spans(bt, before);

        size_t body_floor = bt->depth;
        size_t end = 0;
        size_t min = 0;
        size_t max = 0;
        int body_matched;
        size_t * outer_keep_last = bt->keep_last;
        size_t * keep_last = NULL;
        // Wanted whenever the *body* may fail and its writes are to
        // survive. For a plain assertion that is the negative one; for a
        // condition it is either sign, because the failure of the body is
        // the else branch and not the failure of the construct.
        if ((negative || choosing) && (inst->flags & GRX_INST_KEEP_CAPTURES)) {
          keep_last = gcu_allocator_malloc(
              bt->allocator, bt->captures * sizeof(size_t));
          if (!keep_last) {
            gcu_allocator_free(bt->allocator, before);
            bt->failure = GRX_ERR_OOM;
            return 0;
          }
          for (size_t i = 0; i < bt->captures; i++) {
            keep_last[i] = GRX_NPOS;
          }
          bt->keep_last = keep_last;
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
        bt->keep_last = outer_keep_last;
        bt->depth = body_floor;
        if (bt->failure != GRX_OK) {
          gcu_allocator_free(bt->allocator, keep_last);
          gcu_allocator_free(bt->allocator, before);
          return 0;
        }
        if (verb_escapes_assertion(bt, negative)) {
          restore_spans(bt, before);
          gcu_allocator_free(bt->allocator, keep_last);
          gcu_allocator_free(bt->allocator, before);
          return 0;
        }

        if (!choosing && body_matched == negative) {
          // A positive lookaround whose body failed, or a negative one whose
          // body succeeded. Either way the construct fails and the captures
          // go back to what they were.
          restore_spans(bt, before);
          gcu_allocator_free(bt->allocator, keep_last);
          gcu_allocator_free(bt->allocator, before);
          ok = 0;
          break;
        }

        // What the body wrote when the body *failed*: the dialect's
        // negative-lookaround rule decides, and `keep_last` below is the
        // whole of the keeping. For a plain assertion this is the same
        // test as `negative`, because a positive one that reaches here
        // matched; a condition reaches here either way, and both
        // references still answer by what the body did.
        // `^(?(?=(a)b)x|a)` against "ay" reports group one as "a" in perl
        // 5.40.1 and unset in pcre2test, which is section 5.17's split
        // arriving by another route.
        if (!body_matched && !(inst->flags & GRX_INST_KEEP_CAPTURES)) {
          // ECMA-262 22.2.2.4: a negative lookaround leaves the captures as
          // they were, whatever its body touched on the way to failing.
          // Perl does not - see GRX_INST_KEEP_CAPTURES - and there the
          // writes fall through to the same bookkeeping a positive
          // lookaround's do.
          restore_spans(bt, before);
        }
        else {
          // The body failed and its writes stand. What stands is the last
          // value each group *finished*, not the raw slot state: a group
          // whose final iteration was abandoned part way through holds that
          // iteration's clear, and reporting it would make the answer depend
          // on where in the iteration the body ran out of paths.
          // NULL here for a *positive* lookaround, which reaches this same
          // bookkeeping with nothing to correct.
          //
          // `!body_matched` because a condition reaches here on both
          // outcomes and this rule is about a body that ran out of paths.
          // A body that *matched* has its final values in the slots
          // already, and `keep_last` may hold an earlier iteration's -
          // `(?(?=((a)|b)+)x|y)` is the shape - so applying it there would
          // report a group the last iteration cleared. A plain negative
          // lookaround never reaches this with a matched body, so nothing
          // that worked before changes.
          if (keep_last && !body_matched) {
            for (size_t i = 1; i < bt->captures; i += 2) {
              if (keep_last[i] != GRX_NPOS) {
                bt->slots[i - 1] = keep_last[i - 1];
                bt->slots[i] = keep_last[i];
              }
            }
          }
          // The shadow spans as well as the live ones, for the reason
          // snapshot_spans() gives: they are what a backreference reads, so
          // a path abandoned later has to put them back too.
          for (size_t pass = 0; pass < 2; pass++) {
            if (pass == 1 && !bt->shadow) {
              break;
            }
            size_t first = pass == 0 ? 0 : bt->shadow;
            size_t last = pass == 0 ? bt->captures : bt->slot_count;
            for (size_t i = first; i < last; i++) {
              if (bt->slots[i] == before[i]) {
                continue;
              }
              size_t old = before[i];
              size_t current = bt->slots[i];
              bt->slots[i] = old;
              if (!save_slot(bt, i)) {
                gcu_allocator_free(bt->allocator, keep_last);
                gcu_allocator_free(bt->allocator, before);
                return 0;
              }
              bt->slots[i] = current;
            }
          }
        }
        gcu_allocator_free(bt->allocator, keep_last);
        gcu_allocator_free(bt->allocator, before);

        // `y` when the condition held and `y + 1` when it did not; both are
        // jumps, so the branches stay in source order. For everything else
        // `y` is simply where the outer program resumes.
        pc = choosing && body_matched == negative ? inst->y + 1 : inst->y;
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
            bt->allocator, bt->slot_count * sizeof(size_t));
        if (!before) {
          bt->failure = GRX_ERR_OOM;
          return 0;
        }
        snapshot_spans(bt, before);

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
          restore_spans(bt, before);
          gcu_allocator_free(bt->allocator, before);
          return 0;
        }

        if (!body_matched) {
          restore_spans(bt, before);
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
          int posix = bt->submatch == GRX_SUBMATCH_POSIX;
          int longer = bt->best_end == GRX_NPOS || position > bt->best_end;
          // Under GRX_SUBMATCH_FIRST_PATH, strictly longer only: the first
          // path to reach a given end is the one that dialect reports, and a
          // later path of the same length must not displace it. Under
          // GRX_SUBMATCH_POSIX a path of the *same* length is still a
          // candidate, because which of the two divides it correctly is the
          // question that rule exists to answer.
          if (longer
              || (posix && position == bt->best_end
                  && grx_exec_submatch_better(
                      bt->slots, bt->best_slots, bt->captures))) {
            bt->best_end = position;
            for (size_t i = 0; i < bt->slot_count; i++) {
              bt->best_slots[i] = bt->slots[i];
            }
            if (!posix && position >= bt->window_end) {
              // Nothing can be longer than everything. Worth the branch:
              // without it `\(a*\)*\1` against twenty characters walks the
              // whole tree to prove there is no longer match than the one
              // that already reached the end, and hits max_steps doing it.
              // With it the answer comes back at once, and both references
              // answer this shape at once too.
              //
              // Not available under GRX_SUBMATCH_POSIX, and this is what
              // that rule costs here: another path reaching the same end may
              // still divide it better, so reaching the end of the window is
              // no longer a reason to stop looking. `bt->submatch` is
              // therefore GRX_SUBMATCH_POSIX only for a program the Pike VM
              // could also run, where this engine is a cross-check of a
              // linear-time answer and the cost is a test's to pay. A
              // program that needs *this* engine - a backreference, under
              // these dialects - keeps the short-circuit: without it
              // `\(a*\)*\1` against twenty characters runs out of steps
              // where it used to answer at once, and there is no reference to
              // be exact against anyway, since musl refuses a basic RE with a
              // backreference outright.
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

      case GRX_OP_KEEP_END:
        // `\ze`, and the mirror of the case above: the reported *end* is
        // here, and the rest of the pattern still has to match. Through the
        // same undo stack, and for the same reason - vim answers
        // `\(a\zex\|ab\)c` against "abc" with 0-3, so a marker on a
        // branch that was abandoned leaves nothing behind.
        //
        // The last one reached on the winning path is the one that decides:
        // `\%(a\zeb\)\{2}` against "abab" is 0-3 there, which is the
        // second iteration's marker and not the first's. Writing the slot
        // each time is exactly that rule.
        if (1 < bt->captures) {
          if (!save_slot(bt, 1)) {
            return 0;
          }
          bt->slots[1] = position;
        }
        pc++;
        continue;

      case GRX_OP_SCRIPT_RUN: {
        // The register holds where the body began. Inside a lookbehind the
        // body ran backwards, so it holds the *later* offset and the span
        // is taken in whichever order the two come - the text is the same
        // text and a script run is not a directional property.
        size_t index = bt->captures + inst->x;
        size_t mark = index < bt->captures + bt->registers ? bt->slots[index]
                                                           : GRX_NPOS;
        if (mark == GRX_NPOS) {
          // No PROGRESS_SET reached this register, which codegen never
          // emits: the pair is written together or not at all.
          bt->failure_diag = GRX_DIAG_INTERNAL;
          return 0;
        }
        size_t from = mark < position ? mark : position;
        size_t to = mark < position ? position : mark;
        ok = span_is_script_run(bt, from, to);
        break;
      }

      case GRX_OP_CALLOUT: {
        if (!bt->request->callout) {
          // What PCRE2 does with no function registered: nothing. The
          // instruction is still emitted, because whether a callout has an
          // effect is a fact about the *search* and the program is shared
          // between searches that differ on it.
          pc++;
          continue;
        }
        int fail_path = 0;
        if (!fire_callout(bt, inst, position, &fail_path)) {
          return 0;
        }
        ok = !fail_path;
        break;
      }

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
            if (bt->slots[1] == GRX_NPOS) {
              bt->slots[1] = position;
            }
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
            // One name may belong to several groups, and then the question
            // is whether any of them is set: ambiguous_group() answers with
            // the first that is, or with the first of the list when none
            // is, so the test below gives the right answer either way.
            uint32_t group = (inst->flags & GRX_INST_AMBIGUOUS_REF)
                ? ambiguous_group(bt, inst)
                : inst->x;
            size_t start_slot = (size_t)group * 2;
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
          bt->failure_diag = GRX_DIAG_LIMIT_RECURSION_DEPTH;
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
    .failure_diag = GRX_DIAG_NONE,
    .longest = program->preference == GRX_PREFER_LEFTMOST_LONGEST,
    // Exact for a program the Pike VM could also run, and first-path for
    // one it could not: see the GRX_OP_MATCH case, which says what the
    // difference buys and what it would otherwise cost.
    .submatch = request->regex && request->regex->facts.is_regular
        ? program->submatch : GRX_SUBMATCH_FIRST_PATH,
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
    .keep_last = NULL,
    .verb_stop = VERB_NONE,
    .skip_to = 0,
    .window_start = 0,
    .window_end = request->length,
    .marks = NULL,
    .mark_depth = 0,
    .mark_capacity = 0,
    .mark = GRX_INDEX_NONE,
    .nomatch_mark = GRX_INDEX_NONE,
    .attempt_start = request->start,
    .callout_captures = NULL,
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
  if (request->callout) {
    bt.callout_captures = gcu_allocator_malloc(
        bt.allocator, (bt.captures / 2) * sizeof(GRX_Capture));
    if (!bt.callout_captures) {
      gcu_allocator_free(bt.allocator, bt.slots);
      return GRX_ERR_OOM;
    }
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
      if (request->out_diag) {
        *request->out_diag = GRX_DIAG_LIMIT_MATCH_MEMORY;
      }
      return GRX_ERR_LIMIT;
    }
    bt.visited = gcu_allocator_calloc(bt.allocator, bytes, 1);
    if (!bt.visited) {
      gcu_allocator_free(bt.allocator, bt.slots);
      return GRX_ERR_OOM;
    }
  }
  else if (!bt.longest && !request->callout
      && grx_exec_program_is_memoizable(request->regex)) {
    // `!request->callout` because the late memo skips a state already
    // tried, and a callout on that state is a side effect the caller can
    // see not happening. Sound for the *match*, which is what a visited bit
    // claims; wrong for the sequence of callouts, which is what the caller
    // asked to be told. Tested by CalloutFiresOnEveryArrivalNotOnlyTheFirst
    // in tests/unit/test_callout.cpp, which is a pattern this branch would
    // otherwise arm.
    //
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
    // Step over every position whose byte cannot begin a match, this one
    // included. Each would have failed on its first consumed byte, and
    // failing takes a descent through the program where this takes a byte
    // test.
    //
    // At the top rather than beside the advance below, so that the *first*
    // attempt is covered too. That is not only speed: a first attempt nothing
    // can skip is a first attempt that hides a wrong set from any test whose
    // subject matches at offset zero, which is how the first version of this
    // let a mutation of prefilter.c's give-up arm pass.
    if (!request->anchored) {
      start = grx_exec_skip_to_first_byte(
          program, request->subject, request->length, start);
    }
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
    bt.attempt_start = start;
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
    // pcre2api's CRLF rule, which it calls a compromise: an attempt that
    // failed at a CR LF sequence resumes *after* the LF, so no attempt
    // ever begins between the two - unless the pattern names CR or LF
    // itself, in which case the author plainly meant to reach that
    // position. pcre2's own example is `.+A`, which does not match
    // "\r\nA" and where `[\r\n]A` does.
    //
    // A caller's explicit start offset is not affected, because this is
    // the *advance* and the first attempt has already happened.
    if ((program->flags & GRX_PROGRAM_NEWLINE_CRLF)
        && !(program->flags & GRX_PROGRAM_HAS_CR_OR_LF)
        && grx_crlf_begins_at(request->subject, request->length, start)) {
      start += 2;
      continue;
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
    // A `\ze` can pin the end *before* a later `\zs` moves the start, and
    // then the span reported is the empty one at the start. vim's own
    // `matchend()` answers 3 for `ab\zec\zsd` against "abcd", so this is
    // its answer and not a repair of one: the two markers are independent
    // and the second one wins the ground they contest.
    //
    // GRX_OP_KEEP_END is the only instruction that can invert the pair, so
    // for every other program this is a comparison that never fires.
    if (bt.captures > 1 && bt.slots[0] != GRX_NPOS
        && bt.slots[1] != GRX_NPOS && bt.slots[1] < bt.slots[0]) {
      bt.slots[1] = bt.slots[0];
    }
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

  if (request->out_diag) {
    *request->out_diag = bt.failure_diag;
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
  gcu_allocator_free(bt.allocator, bt.callout_captures);
  return result;
}
