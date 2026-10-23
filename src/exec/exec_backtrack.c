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
 * Nested assertion bodies recurse on the C stack under the engine's
 * non-liftable native-depth cap. Deferred continuations stay in the interpreter
 * loop, so flat sequences and repeats do not increase native depth. The parser
 * separately caps structural nesting with `max_nesting_depth`. A lookaround is
 * atomic
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
  FRAME_RESUME = 0, ///< An alternative not yet tried: `pc` and `position`.
  FRAME_ASSERTION_CAPTURE, ///< An assertion-result write; atomic entry owns it.
  FRAME_CAPTURE,           ///< A capture slot to put back: `index` and `value`.
  FRAME_COUNTER,  ///< Decrement a lexical counted-loop visit on unwind.
  FRAME_REGISTER, ///< A progress register to put back.
  FRAME_ATOMIC,   ///< A barrier ATOMIC_END pops through and discards.
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
  FRAME_PENDING, ///< One heap-owned assertion activation to leave on rollback.
  FRAME_PENDING_STATE,   ///< An activation state to restore before its marker.
  FRAME_PRIORITY_BRANCH, ///< Nonpreferred branch, retained for frontier order.
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
  uint8_t kind;      ///< A `FrameKind`.
  uint32_t pc;       ///< RESUME: where to resume. Others: the slot index.
  union {
    size_t position; ///< RESUME: subject position. Others: the old value.
    struct PendingLook * pending; ///< Assertion activation undo payload.
  };
} Frame;

/** An assertion whose body observes a completed continuation. */
typedef struct PendingLook {
  struct PendingLook * previous;
  struct PendingLook * active_previous;
  struct PendingLook * active_next;
  struct PendingLook * result_previous;
  size_t sequence; ///< Encounter order within this body run.
  int evaluated;   ///< A completed frontier can be revisited after rollback.
  int linked;      ///< On the unevaluated activation list.
  size_t repeat_activation; ///< Prior activations of this logical assertion.
  size_t repeat_context;    ///< Lexical repeat ownership boundaries.
  int repeat_later;         ///< A repeated assertion has an earlier activation.
  size_t state; ///< Per-activation frontier state; a loop may reuse the opcode.
  uint32_t pc;
  size_t position;
  size_t floor;    ///< Undo writes after entry belong to the continuation.
  size_t owner;    ///< Backward assertion generation at continuation entry.
  size_t * base;   ///< Atomic context owned before this continuation starts.
  size_t * entry;  ///< Captures as the assertion was first encountered.
  size_t * result; ///< Captures owned by a completed forward assertion.
} PendingLook;

/**
 * Body evaluation and continuation reporting have independent capture owners.
 * caller[] has five capture-width regions: reporting live spans, reporting
 * shadow spans, fixed BODY input, per-group BODY-result ownership, and fixed
 * BODY shadow input. Ownership pairs hold flags (BODY wrote/current prefix)
 * and the entry executed-write generation, respectively.
 */
enum {
  ATOMIC_CAPTURE_BEFORE_START = 4, ///< Closed before the reported-start marker.
  ATOMIC_CAPTURE_MARKED_REPORT = 8 ///< A competing closure owns its report.
};

typedef struct BehindFrontier {
  struct BehindFrontier * previous;
  uint32_t body;
  size_t words;
  size_t origin;
  struct ScalarPriority * priorities;
  unsigned char exposed[];
} BehindFrontier;

typedef struct ScalarPriority {
  struct ScalarPriority * next;
  size_t target;
  size_t origin;
  size_t count;
  size_t words;
  unsigned char path[];
} ScalarPriority;

typedef struct ScalarScope {
  GRX_ExecRequest request;
  const GRX_ExecRequest * outer;
  unsigned char * frontiers;
  size_t words;
  uint32_t pc;
  uint32_t stop;
  size_t origin;
  int atomic;
  size_t old_defer;
  size_t old_report;
  size_t floor;
  ScalarPriority * priorities;
  BehindFrontier * activation;
} ScalarScope;

typedef struct AtomicContext {
  struct AtomicContext * previous;
  ScalarScope * frontier;
  int merged;
  int overlap; ///< A preceding accepting BODY covers this input position.
  int incoming;
  int reporting; ///< An immediate caller reporting snapshot survives this BODY.
  size_t input_owner;
  size_t caller[];
} AtomicContext;

/** One atomic arrival, ordered by input position and then path priority. */
typedef struct AtomicArrival {
  struct AtomicArrival * next;
  size_t start;
  size_t end;
  size_t priority;
  size_t origin;
  size_t prior_end; ///< Furthest accepting exit from a strictly earlier input.
  int displaced;
  size_t input_owner;
  /** Closed incoming capture identity, excluding search origin. */
  size_t context[];
} AtomicArrival;

typedef struct AtomicIncoming {
  struct AtomicIncoming * next;
  size_t position;
  size_t priority;
  size_t origin;
  size_t context[];
} AtomicIncoming;

typedef struct AtomicSchedule {
  struct AtomicSchedule * next;
  AtomicArrival * arrivals;
  AtomicIncoming * incoming;
  BehindFrontier * frontiers;
  uint32_t pc;
  size_t attempt;
  int merged;
  size_t first_entry;
  size_t future_bound;
} AtomicSchedule;

/** Undoable atomic-base state; NPOS means no committed base. */
enum {
  ATOMIC_BASE_REQUIRED = 1, ///< Required consuming BODY owns backward captures.
  ATOMIC_BASE_CONTEXT =
      2,                 ///< Nullable result or temporary inside-BODY context.
  ATOMIC_BASE_READS = 4, ///< BODY reads numbered captures.
  ATOMIC_BASE_SHIFTED =
      8, ///< Later incoming closure selected for continuation.
};

/** Immediate caller reporting state; NPOS means no snapshot. */
enum {
  ATOMIC_REPORT_VALID = 1,
  ATOMIC_REPORT_COMPLETE =
      2, ///< Surviving numbered BODY writes own unset fields.
};

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
  BehindFrontier * behind_frontiers;
  int scalar_prepass;
  const size_t * capture_context; ///< Captures inherited by this assertion run.
  size_t atomic_result_slots; ///< Numbered captures owned by a prior accepting
                              ///< result.
  size_t atomic_result_present; ///< ATOMIC_REPORT_* bits, or NPOS.
  size_t atomic_base; ///< A mirrored slot prefix, holding the last atomic base.
  size_t atomic_input_owner_slot; ///< Incoming activation of a committed atomic
                                  ///< input.
  size_t capture_owner_depth;     ///< Nested capture-context scopes.
  size_t active_capture_owner;    ///< Generation of this assertion context.
  /**
   * Seven capture-width regions: 0 incoming prior-attempt context; 1 outgoing
   * context; 2 immediate committed BODY result; 3 delayed caller baseline;
   * 4 per-pair flags (even: prefix recorded, odd: immediate result owns field);
   * 5 persistent inherited input seed; 6 seed closing-frontier positions.
   * Group zero's unused start slot in regions 0/1/3 tags marker-end ownership;
   * marker-free BODY exits preserve the tagged outgoing end context.
   * Regions 2 and 4 reset at each search origin; 5 and 6 last one public
   * search.
   */
  size_t * atomic_history;
  /** Two instruction-indexed regions: staged root attempt and committed
   * final-boundary activation. Query copies borrow this immutable prefix;
   * only the owning user run may stage or commit entries. */
  size_t * ahead_arrivals;

  size_t frontier_position; ///< Current subject assertion frontier.
  size_t alternative_epoch; ///< Explicit alternatives already abandoned.
  size_t branch_epoch; ///< New alternatives distinguish current capture writes.
  size_t capture_branch_slots; ///< Branch that produced each closed capture.
  /** Per pair: even is open/closed; odd is undoable executed-write generation.
   * Only numbered SAVE/RESET instructions advance provenance. Installing an
   * inherited assertion context does not produce a numbered capture write.
   */
  size_t capture_open_slots;
  size_t capture_sequence; ///< Monotonic source of executed-write generations.
  size_t forward_result_slots; ///< Actual spans produced by forward assertions.
  size_t reference_end_slot;   ///< Undoable physical end owned by a reference.
  size_t forwarded_slots;      ///< Per-capture outer-owner generation, or NPOS.
  size_t
      behind_owner_slot; ///< Undoable generation of completed backward owners.
  size_t atomic_base_present; ///< ATOMIC_BASE_* bits, or NPOS.
  size_t
      forward_scope_slot;  ///< Undoable forward-subrun provenance in this body.
  size_t end_origin_slot;  ///< Whether the provisional end came through a
                           ///< forward subrun.
  size_t pending_end_slot; ///< Extra undoable state for provisional end markers.
  PendingLook *
      ready; ///< Only unevaluated activations; completed scopes retire.
  PendingLook *
      results; ///< Capturing forward results, newest completion first.
  AtomicContext * atomic_context; ///< Innermost live atomic body context.
  AtomicSchedule *
      atomic_schedules; ///< Accepting-frontier ownership in this body.
  const AtomicSchedule *
      discovery_schedules; ///< Read-only competing prior atomics.
  AtomicArrival * discovered_arrivals;
  AtomicIncoming * discovered_incoming;
  size_t discovery_priority;
  size_t discovery_first_start;
  int discovery_multiple;
  int atomic_entry_merged;
  int atomic_entry_overlap;
  uint32_t discovery_atomic;
  size_t discovery_depth;
  int discovery_prefix_only;
  size_t discovery_bound;
  uint32_t body_pc;
  size_t body_position;
  const size_t * body_entry;
  const size_t * body_history;
  size_t pending_sequence;
  size_t
      frontier_count; ///< Pending frontier work; delayed-only loops skip scans.
  PendingLook * pending; ///< Assertions belonging to the current body run.
  size_t
      scratch_bytes; ///< Live slots and assertion snapshots charged together.
  size_t slot_count;     ///< Captures, registers and shadows together.
  size_t * slots;        ///< Captures, then registers.
  Frame * stack;
  size_t depth;          ///< Frames in use.
  size_t capacity;       ///< Frames allocated.
  size_t
      stack_depth_base;  ///< Caller frames still live during an isolated pass.
  int stack_is_scratch;  ///< Isolated candidate stacks share the assertion
                         ///< budget.
  size_t steps;          ///< Instructions executed, against max_steps.
  int utf;               ///< Whether a step is a code point or a byte.
  int wtf8;              ///< Whether an unpaired surrogate decodes.
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
  size_t behind_depth; ///< Additional forward-lookbehind candidate frames.

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
   * The spans handed to a `GRX_CalloutFn`, or NULL when none is
   * registered.
   *
   * Allocated once for the run rather than per callout: a callout in a loop
   * fires as often as the loop turns, and a malloc for each would make
   * observing a match cost more than running it.
   */
  GRX_Capture * callout_captures;
} Backtrack;

#define GRX_PENDING_WORDS                                                      \
  ((sizeof(PendingLook) + sizeof(size_t) - 1) / sizeof(size_t))

static int scalar_scope_enter(Backtrack * bt, uint32_t pc, size_t position,
    uint32_t scope, uint32_t stop, int atomic, ScalarScope ** out_scope);
static void scalar_scope_leave(Backtrack * bt, ScalarScope * scope);
static int scalar_priority_save(Backtrack * bt, size_t target);
static int scalar_reports_cluster(Backtrack * bt, size_t position);
static int pending_step(Backtrack * bt);
static void scalar_activation_free(Backtrack * bt, BehindFrontier * activation);
static void pending_destroy(Backtrack * bt, PendingLook * pending);
static void atomic_context_leave(Backtrack * bt);
static void pending_restore(
    Backtrack * bt, PendingLook * pending, size_t state);

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
  if (limits->max_backtrack &&
      (bt->stack_depth_base >= limits->max_backtrack ||
          bt->depth >= limits->max_backtrack - bt->stack_depth_base)) {
    bt->failure = GRX_ERR_LIMIT;
    bt->failure_diag = GRX_DIAG_LIMIT_BACKTRACK;
    return 0;
  }

  if (bt->depth == bt->capacity) {
    size_t capacity = bt->capacity ? bt->capacity * 2 : GRX_BACKTRACK_MIN_STACK;
    if (limits->max_backtrack && capacity > limits->max_backtrack) {
      capacity = limits->max_backtrack;
    }
    size_t extra = (capacity - bt->capacity) * sizeof(Frame);
    if (bt->stack_is_scratch &&
        (extra > SIZE_MAX - bt->scratch_bytes ||
            (limits->max_match_memory &&
                (bt->scratch_bytes > limits->max_match_memory ||
                    extra > limits->max_match_memory - bt->scratch_bytes)))) {
      bt->failure = GRX_ERR_LIMIT;
      bt->failure_diag = GRX_DIAG_LIMIT_MATCH_MEMORY;
      return 0;
    }
    Frame * grown = gcu_allocator_realloc(
        bt->allocator, bt->stack, capacity * sizeof(Frame));
    if (!grown) {
      bt->failure = GRX_ERR_OOM;
      return 0;
    }
    bt->stack = grown;
    if (bt->stack_is_scratch) {
      bt->scratch_bytes += extra;
    }
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

/** Exhausted identity generations fail rather than aliasing an older owner. */
static int advance_generation(Backtrack * bt, size_t * generation) {
  if (*generation == GRX_NPOS - 1) {
    bt->failure = GRX_ERR_LIMIT;
    bt->failure_diag = GRX_DIAG_LIMIT_STEPS;
    return 0;
  }
  ++*generation;
  return 1;
}

/** Keep explicit numbered writes distinct from implicit context installation.
 */
static int capture_write(Backtrack * bt, size_t group) {
  if (!bt->capture_open_slots) {
    return 1;
  }
  if (bt->capture_sequence == GRX_NPOS - 1) {
    bt->failure = GRX_ERR_LIMIT;
    bt->failure_diag = GRX_DIAG_LIMIT_STEPS;
    return 0;
  }
  if (bt->slots[bt->capture_branch_slots + group + 1] != GRX_NPOS) {
    if (!save_slot(bt, bt->capture_branch_slots + group + 1)) {
      return 0;
    }
    bt->slots[bt->capture_branch_slots + group + 1] = GRX_NPOS;
  }
  if (!save_slot(bt, bt->capture_open_slots + group + 1)) {
    return 0;
  }
  bt->slots[bt->capture_open_slots + group + 1] = ++bt->capture_sequence;
  return 1;
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
        if (!advance_generation(bt, &bt->branch_epoch) ||
            (frame.kind == FRAME_ALTERNATIVE &&
                !advance_generation(bt, &bt->alternative_epoch))) {
          return 0;
        }
        if ((bt->program->flags & GRX_PROGRAM_SCALAR_FRONTIER) &&
            !push(bt, FRAME_PRIORITY_BRANCH, frame.pc, frame.position))
          return 0;
        *out_pc = frame.pc;
        *out_position = frame.position;
        return 1;
      case FRAME_ASSERTION_CAPTURE:
      case FRAME_CAPTURE:
        bt->slots[frame.pc] = frame.position;
        break;
      // The counter is lexical, not a saved old value. A reentered inner
      // repeat can reset it before an earlier visit unwinds below zero.
      // Magnitude/sign preserve that lifetime on both 32- and 64-bit ABIs.
      case FRAME_COUNTER:
        if (bt->slots[frame.pc + 1]) {
          bt->slots[frame.pc]++;
        }
        else if (bt->slots[frame.pc]) {
          bt->slots[frame.pc]--;
        }
        else {
          bt->slots[frame.pc + 1] = 1;
          bt->slots[frame.pc] = 1;
        }
        break;
      case FRAME_REGISTER:
        bt->slots[frame.pc] = frame.position;
        break;
      case FRAME_PENDING:
        bt->pending = frame.pending->previous;
        pending_destroy(bt, frame.pending);
        break;
      case FRAME_PENDING_STATE:
        pending_restore(bt, frame.pending, frame.pc == 2 ? GRX_NPOS : frame.pc);
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
            if (inner.kind == FRAME_CAPTURE ||
                inner.kind == FRAME_ASSERTION_CAPTURE ||
                inner.kind == FRAME_REGISTER) {
              bt->slots[inner.pc] = inner.position;
              continue;
            }
            if (inner.kind == FRAME_PENDING) {
              bt->pending = inner.pending->previous;
              pending_destroy(bt, inner.pending);
              continue;
            }
            if (inner.kind == FRAME_PENDING_STATE) {
              pending_restore(
                  bt, inner.pending, inner.pc == 2 ? GRX_NPOS : inner.pc);
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
              if (!advance_generation(bt, &bt->branch_epoch)) {
                return 0;
              }
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
        if (bt->atomic_base) {
          atomic_context_leave(bt);
        }
        break;
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
      bt->window_end - position, bt->wtf8, out_codepoint);
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
      bt->request->subject, position, bt->wtf8, out_codepoint);
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
          text, bt->window_start, position, bt->utf, bt->wtf8, &base);
      return inst->mode == GRX_ASSERT_WORD_CLASS_START
          ? grx_vim_word_start(has_base, base, has_after, after)
          : grx_vim_word_end(has_base, base, has_after, after);
    }

    // Vim's composing clusters: the end of a cluster, and the start of one.
    // `inst->x` is the class of composing characters in both.
    case GRX_ASSERT_NOT_COMPOSING:
      return !(has_after && in_class(bt, inst->x, after));

    case GRX_ASSERT_SCALAR_FRONTIER:
    case GRX_ASSERT_NO_SCALAR_FRONTIER: {
      size_t previous_width = 0;
      uint32_t previous = 0;
      int active = bt->request->scalar_frontiers &&
          read_backward(bt, position, &previous, &previous_width) &&
          bt->request->scalar_frontiers[position - previous_width];
      return inst->mode == GRX_ASSERT_SCALAR_FRONTIER ? active : !active;
    }

    case GRX_ASSERT_CLUSTER_BOUNDARY:
      if (inst->policy & GRX_POLICY_ASSERT_INITIAL_BOUNDARY) {
        uint32_t base = 0;
        return position != bt->attempt_start || !has_after ||
            !in_class(bt, inst->x, after) ||
            !grx_cluster_base_before(
                text, bt->window_start, position, bt->utf, bt->wtf8, &base) ||
            grx_display_cell_width(base, 0) == 0;
      }
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
      int boundary = grx_exec_break_at(bt->program,
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
  if (bt->pending_end_slot) {
    bt->slots[bt->pending_end_slot] = before[bt->pending_end_slot];
    bt->slots[bt->forward_scope_slot] = before[bt->forward_scope_slot];
    bt->slots[bt->end_origin_slot] = before[bt->end_origin_slot];
    bt->slots[bt->behind_owner_slot] = before[bt->behind_owner_slot];
    memcpy(bt->slots + bt->forwarded_slots, before + bt->forwarded_slots,
        bt->captures * sizeof(size_t));
    memcpy(bt->slots + bt->forward_result_slots,
        before + bt->forward_result_slots, bt->captures * sizeof(size_t));
    memcpy(bt->slots + bt->capture_open_slots, before + bt->capture_open_slots,
        bt->captures * sizeof(size_t));
    memcpy(bt->slots + bt->capture_branch_slots,
        before + bt->capture_branch_slots, bt->captures * sizeof(size_t));
  }
  if (bt->atomic_result_slots) {
    memcpy(bt->slots + bt->atomic_result_slots,
        before + bt->atomic_result_slots, bt->captures * sizeof(size_t));
    bt->slots[bt->atomic_result_present] = before[bt->atomic_result_present];
    bt->slots[bt->atomic_input_owner_slot] =
        before[bt->atomic_input_owner_slot];
  }
  if (bt->atomic_base) {
    memcpy(bt->slots + bt->atomic_base, before + bt->atomic_base,
        bt->atomic_base * sizeof(size_t));
    bt->slots[bt->atomic_base_present] = before[bt->atomic_base_present];
  }
  if (bt->shadow) {
    memcpy(bt->slots + bt->shadow, before + bt->shadow,
        (bt->slot_count - bt->shadow) * sizeof(size_t));
  }
}

/** Forward BODY results retain scalar tags but own reference completion.
 * Atomic executed-write provenance is separate from ahead reporting ownership. */
static int backref_complete(Backtrack * bt, const GRX_Inst * inst, size_t width,
    int reverse, size_t * position) {
  uint32_t group = (inst->flags & GRX_INST_AMBIGUOUS_REF)
      ? ambiguous_group(bt, inst)
      : inst->x;
  size_t slot = (size_t)group * 2;
  if (slot + 1 >= bt->captures)
    return 1;
  int ahead = bt->forward_result_slots &&
      bt->slots[bt->forward_result_slots + slot] != GRX_NPOS &&
      bt->slots[bt->forward_result_slots + slot + 1] != GRX_NPOS;
  int atomic = bt->atomic_history &&
      bt->atomic_history[4 * bt->captures + slot + 1] == 1;
  if (bt->reference_end_slot) {
    if (!save_slot(bt, bt->reference_end_slot))
      return 0;
    bt->slots[bt->reference_end_slot] =
        width && (ahead || atomic) ? GRX_NPOS : *position;
  }
  if (!width || reverse || !(inst->policy & GRX_POLICY_COMPLETE_COMPOSING))
    return 1;
  if (!ahead && !atomic)
    return 1;
  int scalar = bt->request->scalar_frontiers &&
      bt->request->scalar_frontiers[*position - width];
  size_t end = *position;
  uint32_t point = 0;
  size_t mark_width = 0;
  while (read_forward(bt, end, &point, &mark_width) &&
      grx_display_cell_width(point, 0) == 0) {
    if (!pending_step(bt))
      return 0;
    end += mark_width;
  }
  if (!scalar) {
    *position = end;
  }
  else if ((inst->policy & GRX_POLICY_REPORT_COMPOSING) &&
      scalar_reports_cluster(bt, *position)) {
    size_t report = bt->captures +
        ((bt->program->flags & GRX_PROGRAM_DEFER_COMPOSING) ? 1u : 0u);
    if (!save_slot(bt, report))
      return 0;
    bt->slots[report] = end;
  }
  return bt->failure == GRX_OK;
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
 * harness gets in the soak (testing.md section 12). The assertion-context
 * machinery costs about 864 bytes per nested run in the pinned GCC 16
 * build, and forward lookbehind adds a candidate-search frame. The budget
 * charges seven units per run and one per active candidate-search frame,
 * with 148 * 7 units available. Thus 128 default nested forward lookbehinds
 * plus the top-level run fit, while raised nesting stops before the test's
 * 128 KB allowance. Measurements peak at 130,655 bytes for forward behind,
 * 130,623 for mixed ahead/behind, and 129,567 for ahead alone.
 * tests/unit/test_stack.cpp measures both bounds because frame sizes change
 * with compiler and optimisation settings.
 *
 * That the limits field bounds this at all is why nothing had hit it. The
 * field is not enough on its own, being the caller's to raise: raising it to
 * the 480 the *parser* can take asks the matcher for 241 KB, which on a
 * 256 KB stack was a segmentation fault rather than a refusal. This is the
 * floor underneath it, and it is deliberately not a GRX_Limits field - a
 * bound whose whole job is to be un-liftable should not come with a lever.
 */
#define GRX_BACKTRACK_MAX_C_DEPTH 148
#define GRX_BACKTRACK_RUN_WEIGHT 7

/** Allocate capture state under the aggregate assertion scratch budget. */
static void * capture_alloc(Backtrack * bt, size_t count) {
  size_t limit = bt->request->limits->max_match_memory;
  if (count > SIZE_MAX / sizeof(size_t) ||
      count * sizeof(size_t) > SIZE_MAX - bt->scratch_bytes ||
      (limit &&
          (bt->scratch_bytes > limit ||
              count * sizeof(size_t) > limit - bt->scratch_bytes))) {
    bt->failure = GRX_ERR_LIMIT;
    bt->failure_diag = GRX_DIAG_LIMIT_MATCH_MEMORY;
    return NULL;
  }
  size_t * data = gcu_allocator_malloc(bt->allocator, count * sizeof(size_t));
  if (!data) {
    bt->failure = GRX_ERR_OOM;
    return NULL;
  }
  bt->scratch_bytes += count * sizeof(size_t);
  return data;
}

static void capture_free(Backtrack * bt, void * data, size_t count) {
  if (data) {
    bt->scratch_bytes -= count * sizeof(size_t);
    gcu_allocator_free(bt->allocator, data);
  }
}

static void atomic_incoming_clear(Backtrack * bt, AtomicIncoming ** list) {
  while (*list) {
    AtomicIncoming * entry = *list;
    *list = entry->next;
    capture_free(bt, entry, sizeof(*entry) / sizeof(size_t) + bt->captures);
  }
}

static void atomic_context_leave(Backtrack * bt) {
  AtomicContext * context = bt->atomic_context;
  if (!context) {
    return;
  }
  if (context->frontier)
    scalar_scope_leave(bt, context->frontier);
  bt->atomic_context = context->previous;
  capture_free(
      bt, context, sizeof(*context) / sizeof(size_t) + 5 * bt->captures);
}

/** Pending records are execution work even when their result is cached. */
static int pending_step(Backtrack * bt) {
  bt->steps++;
  if (bt->request->limits->max_steps &&
      bt->steps > bt->request->limits->max_steps) {
    bt->failure = GRX_ERR_LIMIT;
    bt->failure_diag = GRX_DIAG_LIMIT_STEPS;
    return 0;
  }
  return 1;
}

static void pending_unlink(Backtrack * bt, PendingLook * pending) {
  if (grx_program_at(bt->program, pending->pc)->policy &
      GRX_POLICY_LOOK_FRONTIER) {
    bt->frontier_count--;
  }
  if (pending->active_next) {
    pending->active_next->active_previous = pending->active_previous;
  }
  else {
    bt->ready = pending->active_previous;
  }
  if (pending->active_previous) {
    pending->active_previous->active_next = pending->active_next;
  }
  pending->linked = 0;
}

static void pending_restore(
    Backtrack * bt, PendingLook * pending, size_t state) {
  if (pending->state == 1 && state != 1) {
    if (pending->active_next) {
      pending->active_next->active_previous = pending;
    }
    else {
      bt->ready = pending;
    }
    if (pending->active_previous) {
      pending->active_previous->active_next = pending;
    }
    pending->linked = 1;
    if (grx_program_at(bt->program, pending->pc)->policy &
        GRX_POLICY_LOOK_FRONTIER) {
      bt->frontier_count++;
    }
    if (pending->result) {
      bt->results = pending->result_previous;
    }
  }
  pending->state = state;
}

static void pending_destroy(Backtrack * bt, PendingLook * pending) {
  if (pending->linked) {
    pending_unlink(bt, pending);
  }
  capture_free(bt, pending->entry, bt->captures);
  capture_free(bt, pending->result, bt->captures);
  capture_free(bt, pending->base, bt->atomic_base);
  capture_free(bt, pending, GRX_PENDING_WORDS);
}

static int pending_set_state(
    Backtrack * bt, PendingLook * pending, size_t state) {
  if (!push(bt, FRAME_PENDING_STATE,
          pending->state == GRX_NPOS ? 2 : (uint32_t)pending->state, 0)) {
    return 0;
  }
  bt->stack[bt->depth - 1].pending = pending;
  if (state == 1 && pending->state != 1) {
    pending_unlink(bt, pending);
    if (pending->result) {
      pending->result_previous = bt->results;
      bt->results = pending;
    }
  }
  pending->state = state;
  return 1;
}

/** One body run; deferred continuations stay in its interpreter loop. */
static int run_body(Backtrack * bt, uint32_t pc, size_t position, size_t floor,
    int toplevel, size_t * out_end);

static int run(Backtrack * bt, uint32_t pc, size_t position, size_t floor,
    int toplevel, size_t * out_end) {
  if (bt->run_depth >= GRX_BACKTRACK_MAX_C_DEPTH ||
      bt->run_depth * GRX_BACKTRACK_RUN_WEIGHT + bt->behind_depth >=
          GRX_BACKTRACK_MAX_C_DEPTH * GRX_BACKTRACK_RUN_WEIGHT) {
    bt->failure = GRX_ERR_LIMIT;
    bt->failure_diag = GRX_DIAG_LIMIT_RECURSION_DEPTH;
    return 0;
  }
  typedef struct {
    const size_t * outer_entry;
    const size_t * outer_history;
    AtomicContext * outer_atomic_context;
    PendingLook * outer;
    PendingLook * outer_ready;
    PendingLook * outer_results;
    size_t outer_sequence;
    size_t outer_frontier_count;
    size_t outer_branch;
    size_t outer_alternative;
    size_t outer_frontier_position;
    AtomicSchedule * outer_schedules;
    uint32_t outer_pc;
    size_t outer_position;
    size_t origin_count;
    size_t * origin;
    int scalar_prepass;
  } RunContext;
  const size_t saved_words =
      (sizeof(RunContext) + sizeof(size_t) - 1) / sizeof(size_t);
  RunContext * saved = capture_alloc(bt, saved_words);
  if (!saved)
    return 0;
  saved->scalar_prepass = bt->scalar_prepass;
  bt->scalar_prepass = 0;
  saved->origin_count = 0;
  saved->origin = NULL;
  if (bt->program->flags & GRX_PROGRAM_ATOMIC_FRONTIER) {
    saved->origin_count =
        bt->slot_count + (bt->atomic_history ? 7 * bt->captures : 0);
    saved->origin = capture_alloc(bt, saved->origin_count);
    if (!saved->origin) {
      capture_free(bt, saved, saved_words);
      return 0;
    }
    memcpy(saved->origin, bt->slots, bt->slot_count * sizeof(size_t));
    if (bt->atomic_history) {
      memcpy(saved->origin + bt->slot_count, bt->atomic_history,
          7 * bt->captures * sizeof(size_t));
    }
  }
  saved->outer_entry = bt->body_entry;
  saved->outer_history = bt->body_history;
  bt->body_entry = saved->origin;
  bt->body_history = saved->origin && bt->atomic_history
      ? saved->origin + bt->slot_count
      : NULL;
  saved->outer_atomic_context = bt->atomic_context;
  bt->atomic_context = NULL;
  saved->outer = bt->pending;
  saved->outer_ready = bt->ready;
  saved->outer_results = bt->results;
  saved->outer_sequence = bt->pending_sequence;
  saved->outer_frontier_count = bt->frontier_count;
  saved->outer_branch = bt->branch_epoch;
  saved->outer_alternative = bt->alternative_epoch;
  saved->outer_frontier_position = bt->frontier_position;
  saved->outer_schedules = bt->atomic_schedules;
  saved->outer_pc = bt->body_pc;
  saved->outer_position = bt->body_position;
  bt->atomic_schedules = toplevel ? saved->outer_schedules : NULL;
  bt->body_pc = pc;
  bt->body_position = position;
  bt->branch_epoch = 0;
  bt->pending = NULL;
  bt->ready = NULL;
  bt->results = NULL;
  bt->pending_sequence = 0;
  bt->frontier_count = 0;
  bt->run_depth++;
  int matched = 0;
  // An atomic incoming-path pass resumes the current BODY, rather than
  // invoking a new assertion scope. Retain that BODY's discovery request;
  // creating another scope here would recursively rediscover this prefix.
  if (toplevel || bt->run_depth == bt->discovery_depth ||
      !(bt->program->flags & GRX_PROGRAM_SCALAR_FRONTIER) ||
      scalar_scope_enter(bt, pc, position, grx_program_scope(bt->program, pc),
          GRX_INDEX_NONE, 0, NULL)) {
    if (!saved->scalar_prepass)
      matched = run_body(bt, pc, position, floor, toplevel, out_end);
  }
  bt->run_depth--;
  while (bt->pending) {
    PendingLook * finished = bt->pending;
    bt->pending = finished->previous;
    pending_destroy(bt, finished);
  }
  while (bt->atomic_context) {
    atomic_context_leave(bt);
  }
  if (bt->request->scalar_local == 2 &&
      bt->request->scalar_depth == bt->run_depth + 1 &&
      !((ScalarScope *)bt->request->scalar_owner)->atomic) {
    scalar_scope_leave(bt, bt->request->scalar_owner);
  }
  bt->atomic_context = saved->outer_atomic_context;
  bt->pending = saved->outer;
  bt->ready = saved->outer_ready;
  bt->results = saved->outer_results;
  bt->pending_sequence = saved->outer_sequence;
  bt->frontier_count = saved->outer_frontier_count;
  bt->branch_epoch = saved->outer_branch;
  bt->alternative_epoch = saved->outer_alternative;
  bt->frontier_position = saved->outer_frontier_position;
  while (!toplevel && bt->atomic_schedules) {
    AtomicSchedule * schedule = bt->atomic_schedules;
    bt->atomic_schedules = schedule->next;
    while (schedule->arrivals) {
      AtomicArrival * arrival = schedule->arrivals;
      schedule->arrivals = arrival->next;
      capture_free(bt, arrival,
          (sizeof(*arrival) + sizeof(size_t) - 1) / sizeof(size_t) +
              bt->captures);
    }
    atomic_incoming_clear(bt, &schedule->incoming);
    scalar_activation_free(bt, schedule->frontiers);
    capture_free(bt, schedule,
        (sizeof(*schedule) + sizeof(size_t) - 1) / sizeof(size_t));
  }
  if (!toplevel) {
    bt->atomic_schedules = saved->outer_schedules;
  }
  bt->body_pc = saved->outer_pc;
  bt->body_position = saved->outer_position;
  bt->body_entry = saved->outer_entry;
  bt->body_history = saved->outer_history;
  capture_free(bt, saved->origin, saved->origin_count);
  capture_free(bt, saved, saved_words);
  return matched && bt->failure == GRX_OK;
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
 * Furthest back first for Perl/PCRE2, which is the order those references
 * try and the one
 * that settles `(?=foo)(?<=(a??))` against "afoo": the body is lazy and
 * would rather match nothing, but the *assertion* prefers the longest, so
 * Perl reports group one as "a" and running the body in reverse reports it
 * empty.
 *
 * Java tries nearest starts first; its unbounded loop is charged to the
 * ordinary execution step limit. Perl/PCRE2 cap variation at 255 bytes.
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
static void scalar_activation_free(
    Backtrack * bt, BehindFrontier * activation) {
  while (activation) {
    BehindFrontier * previous = activation->previous;
    while (activation->priorities) {
      ScalarPriority * priority = activation->priorities;
      activation->priorities = priority->next;
      capture_free(bt, priority, priority->words);
    }
    capture_free(bt, activation, activation->words);
    activation = previous;
  }
}

static int look_behind_forward(Backtrack * bt, uint32_t body, size_t position,
    size_t min, size_t max, int nearest, int round_bound, int persist,
    const size_t * before, int fresh, const size_t * context,
    size_t * out_end) {
  size_t available = position - bt->window_start;
  size_t reach = max < available ? max : available;
  if (round_bound && (bt->program->flags & GRX_PROGRAM_UTF)) {
    while (reach < available
        && (bt->request->subject[position - reach] & 0xc0u) == 0x80u) {
      reach++;
    }
  }
  int clusters = (bt->program->flags & GRX_PROGRAM_COMPOSING_CLUSTERS) != 0;
  if (round_bound && clusters && reach < available) {
    size_t at = position - reach;
    uint32_t point = 0;
    while (at > bt->window_start) {
      if (!pending_step(bt)) {
        return 0;
      }
      if (!grx_unicode_utf8_decode(bt->request->subject + at,
              bt->window_end - at, bt->wtf8, &point) ||
          grx_display_cell_width(point, 0) != 0) {
        break;
      }
      size_t width = grx_unicode_utf8_decode_prev(
          bt->request->subject, at, bt->wtf8, &point);
      if (!width) {
        break;
      }
      at -= width;
    }
    reach = position - at;
  }
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
  BehindFrontier * frontier = NULL;
  if (bt->program->flags & GRX_PROGRAM_SCALAR_FRONTIER) {
    uint32_t scope = grx_program_scope(bt->program, body);
    int competition = 0;
    for (size_t i = 0; i < bt->program->insts.count && !competition; i++) {
      if (!pending_step(bt))
        break;
      const GRX_Inst * inst = grx_program_at(bt->program, (uint32_t)i);
      competition = grx_program_scope(bt->program, (uint32_t)i) == scope &&
          grx_exec_scalar_cluster_consumer(inst);
    }
    if (competition && bt->failure == GRX_OK) {
      size_t words = (sizeof(*frontier) + sizeof(size_t) - 1) / sizeof(size_t) +
          bt->request->length / sizeof(size_t) + 1;
      frontier = capture_alloc(bt, words);
      if (frontier) {
        memset(frontier, 0, words * sizeof(size_t));
        frontier->previous = bt->behind_frontiers;
        frontier->body = body;
        frontier->words = words;
        bt->behind_frontiers = frontier;
      }
    }
  }

  size_t floor = bt->depth;
  int matched = 0;
  bt->behind_depth++;
  for (int discovering = frontier ? 1 : 0; discovering >= 0; discovering--) {
    for (size_t back = nearest ? min : reach;;
        back = nearest ? back + 1 : back - 1) {
      if (bt->failure != GRX_OK)
        break;
      if (clusters && position - back > bt->window_start &&
          position - back < bt->window_end) {
        uint32_t point = 0;
        if (!pending_step(bt)) {
          break;
        }
        size_t at = position - back;
        int continuation = (bt->request->subject[at] & 0xc0u) == 0x80u;
        int composing = !continuation &&
            grx_unicode_utf8_decode(bt->request->subject + at,
                bt->window_end - at, bt->wtf8, &point) &&
            grx_display_cell_width(point, 0) == 0;
        if (continuation ||
            (composing && !(frontier && (frontier->exposed[at] & 1u)))) {
          if (back == (nearest ? reach : min)) {
            break;
          }
          continue;
        }
      }
      if (!persist) {
        restore_spans(bt, before);
      }
      if (fresh) {
        for (size_t i = 2; i < bt->captures; i++) {
          bt->slots[i] = context ? context[i] : GRX_NPOS;
          if (bt->shadow) {
            bt->slots[bt->shadow + i] =
                context ? context[bt->shadow + i] : GRX_NPOS;
          }
        }
      }
      if (fresh) {
        // A forward lookbehind preserves the openings owned by its enclosing
        // capture frames, while completed incoming captures use the fresh base.
        for (size_t i = 2; i + 1 < bt->captures; i += 2) {
          if (before[i] != GRX_NPOS && before[i + 1] == GRX_NPOS) {
            bt->slots[i] = before[i];
          }
        }
      }
      if (bt->pending_end_slot) {
        bt->slots[bt->pending_end_slot] = GRX_NPOS;
      }
      bt->depth = floor;
      if (bt->atomic_base) {
        bt->slots[bt->atomic_base_present] = GRX_NPOS;
      }
      if (bt->atomic_result_slots) {
        bt->slots[bt->atomic_result_present] = GRX_NPOS;
      }
      if (bt->forward_scope_slot) {
        bt->slots[bt->forward_scope_slot] = GRX_NPOS;
      }
      if (discovering)
        bt->scalar_prepass = 1;
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
      if (back == (nearest ? reach : min)) {
        break;
      }
    }
  }

  if (frontier) {
    bt->behind_frontiers = frontier->previous;
    frontier->previous = NULL;
    scalar_activation_free(bt, frontier);
  }

  bt->behind_depth--;
  bt->visited = outer_visited;
  bt->memo_after = outer_memo_after;
  bt->look_end = outer_look_end;
  if (!matched && !persist) {
    restore_spans(bt, before);
  }
  return matched;
}

/** Discard only outer captures forwarded through a newer backward owner. */
static int apply_capture_owner(Backtrack * bt, size_t owner) {
  if (!bt->forwarded_slots) {
    return 1;
  }
  size_t encoded = owner == GRX_NPOS ? 0 : owner + 1;
  for (size_t i = 2; i + 1 < bt->captures; i += 2) {
    size_t source = bt->slots[bt->forwarded_slots + i];
    // A commit displaces owners established before it. A later assertion
    // can forward a new enclosing capture through its own backward owner;
    // an earlier atomic base cannot discard that newer result.
    if (source == GRX_NPOS || source >= encoded) {
      continue;
    }
    for (size_t slot = i; slot <= i + 1; slot++) {
      if (bt->slots[slot] != GRX_NPOS) {
        if (!save_slot(bt, slot) || !shadow_clear(bt, slot)) {
          return 0;
        }
        bt->slots[slot] = GRX_NPOS;
      }
    }
  }
  return 1;
}

static int apply_atomic_owner(Backtrack * bt) {
  return !bt->atomic_base ||
      (bt->slots[bt->atomic_base_present] == GRX_NPOS ||
          !(bt->slots[bt->atomic_base_present] & ATOMIC_BASE_REQUIRED)) ||
      apply_capture_owner(
          bt, bt->slots[bt->atomic_base + bt->behind_owner_slot]);
}

/** Successful root arrivals retain evaluation activation, not capture values.
 * Discovery copies may observe this immutable prefix, but never extend it. */
static int ahead_arrival(Backtrack * bt, uint32_t pc, int record) {
  if (bt->run_depth != 1)
    return 0;
  if (record && !bt->request->scalar_query && !bt->discovery_depth) {
    if (!bt->ahead_arrivals) {
      size_t count = bt->program->insts.count;
      if (count > SIZE_MAX / 2) {
        bt->failure = GRX_ERR_LIMIT;
        bt->failure_diag = GRX_DIAG_LIMIT_MATCH_MEMORY;
        return -1;
      }
      bt->ahead_arrivals = capture_alloc(bt, 2 * count);
      if (!bt->ahead_arrivals)
        return -1;
      for (size_t i = 0; i < 2 * count; i++) {
        if (!pending_step(bt))
          return -1;
        bt->ahead_arrivals[i] = GRX_NPOS;
      }
    }
    if (bt->ahead_arrivals[bt->program->insts.count + pc] != 1)
      bt->ahead_arrivals[pc] = bt->attempt_start;
  }
  return bt->ahead_arrivals &&
      bt->ahead_arrivals[bt->program->insts.count + pc] == 1 &&
      bt->ahead_arrivals[pc] != bt->attempt_start;
}

/** Evaluate one assertion, leaving continuation and undo ownership to its
 * caller. */
static int evaluate_look(Backtrack * bt, uint32_t pc, size_t position,
    uint32_t * out_next, int retain_end, const size_t * entry_base,
    int scheduled, int owned_ahead, size_t entry_owner, const size_t * entry,
    size_t repeat_context) {
  const GRX_Inst * inst = grx_program_at(bt->program, pc);
  int fresh = (inst->policy & GRX_POLICY_LOOK_FRESH) != 0 ||
      (owned_ahead && (inst->flags & GRX_INST_LOOK_REQUIRED) &&
          (inst->mode == GRX_LOOK_AHEAD_POSITIVE ||
              inst->mode == GRX_LOOK_AHEAD_NEGATIVE ||
              inst->mode == GRX_LOOK_BEHIND_POSITIVE ||
              inst->mode == GRX_LOOK_BEHIND_NEGATIVE));
  int root_arrival = inst->mode == GRX_LOOK_AHEAD_POSITIVE && fresh &&
      (inst->policy & GRX_POLICY_LOOK_HAS_CAPTURES) && bt->run_depth == 1;
  if (root_arrival && ahead_arrival(bt, pc, 0))
    fresh = 0;
  int negative = inst->mode == GRX_LOOK_AHEAD_NEGATIVE ||
      inst->mode == GRX_LOOK_BEHIND_NEGATIVE;
  // A conditional's condition: it chooses a branch rather than
  // failing, and `y` and `y + 1` are the two jumps that say which.
  int choosing = (inst->flags & GRX_INST_COND_ELSE) != 0;

  // The body runs as its own sub-match above this point on the stack,
  // and everything it pushed is discarded afterwards: a lookaround
  // either holds here or does not, and there is nothing to backtrack
  // into. The capture writes it made are turned into undo frames on
  // the caller's stack so that backtracking past the whole lookaround
  // still puts them back.
  size_t before_count = bt->slot_count +
      (bt->atomic_history && bt->atomic_base ? bt->atomic_base : 0);
  size_t * before = capture_alloc(bt, before_count);
  if (!before) {
    return 0;
  }
  snapshot_spans(bt, before);
  size_t outer_owner = bt->active_capture_owner;
  if (bt->behind_owner_slot && (scheduled || !bt->capture_owner_depth)) {
    bt->active_capture_owner =
        scheduled ? entry_owner : before[bt->behind_owner_slot];
  }
  bt->capture_owner_depth++;
  size_t outer_pending =
      bt->pending_end_slot ? bt->slots[bt->pending_end_slot] : GRX_NPOS;
  if (bt->pending_end_slot) {
    bt->slots[bt->pending_end_slot] = GRX_NPOS;
  }

  const size_t * outer_context = bt->capture_context;
  int atomic_continuation = bt->atomic_base &&
      before[bt->atomic_base_present] != GRX_NPOS &&
      (before[bt->atomic_base_present] & ATOMIC_BASE_SHIFTED) &&
      ((before[bt->atomic_base_present] & ATOMIC_BASE_READS) ||
          (inst->policy & GRX_POLICY_LOOK_READS_CAPTURES)) &&
      (inst->mode == GRX_LOOK_BEHIND_POSITIVE ||
          inst->mode == GRX_LOOK_BEHIND_NEGATIVE);
  if (entry_base) {
    bt->capture_context = entry_base;
  }
  else if (!scheduled && bt->atomic_base &&
      before[bt->atomic_base_present] != GRX_NPOS) {
    bt->capture_context = before + bt->atomic_base;
    if (atomic_continuation) {
      fresh = 1;
    }
  }
  if (bt->atomic_base && before[bt->atomic_base_present] != GRX_NPOS &&
      (before[bt->atomic_base_present] & ATOMIC_BASE_SHIFTED) &&
      !atomic_continuation &&
      (inst->mode == GRX_LOOK_BEHIND_POSITIVE ||
          inst->mode == GRX_LOOK_BEHIND_NEGATIVE)) {
    // An unobserved later input does not replace the caller's reporting owner.
    bt->capture_context = before;
  }
  if (bt->atomic_history && bt->atomic_base && bt->capture_context &&
      (inst->mode == GRX_LOOK_BEHIND_POSITIVE ||
          inst->mode == GRX_LOOK_BEHIND_NEGATIVE)) {
    // Captures from competing incoming paths become visible to a postponed
    // body only after their closing frontier has been crossed.
    size_t * context = before + bt->slot_count;
    memcpy(context, fresh ? bt->capture_context : before,
        bt->atomic_base * sizeof(size_t));
    int inherited = fresh;
    for (size_t group = 2; group + 1 < bt->captures; group += 2) {
      size_t source = bt->captures + group;
      const size_t * inherited_context = bt->atomic_history;
      int complete = before[bt->atomic_result_present] != GRX_NPOS &&
          (before[bt->atomic_result_present] & ATOMIC_REPORT_COMPLETE);
      if (complete && bt->atomic_history[source] == GRX_NPOS) {
        if (before[bt->forward_result_slots + group] != GRX_NPOS) {
          inherited_context = before;
          source = bt->forward_result_slots + group;
        }
        else if (bt->alternative_epoch) {
          source = 5 * bt->captures + group;
        }
      }
      if (context[group] == GRX_NPOS && inherited_context[source] != GRX_NPOS &&
          (inherited_context[source + 1] < bt->frontier_position ||
              bt->alternative_epoch || inherited_context == before ||
              (before[bt->atomic_base_present] != GRX_NPOS &&
                  (before[bt->atomic_base_present] & ATOMIC_BASE_READS)))) {
        inherited = 1;
        if (!complete || before[bt->forward_result_slots + group] != GRX_NPOS) {
          context[group] = inherited_context[source];
          context[group + 1] = inherited_context[source + 1];
        }
        if (bt->shadow) {
          context[bt->shadow + group] = inherited_context[source];
          context[bt->shadow + group + 1] = inherited_context[source + 1];
        }
      }
    }
    if (inherited) {
      fresh = 1;
      bt->capture_context = context;
    }
  }
  if (fresh) {
    for (size_t i = 2; i < bt->captures; i++) {
      bt->slots[i] = bt->capture_context ? bt->capture_context[i] : GRX_NPOS;
      if (bt->shadow) {
        bt->slots[bt->shadow + i] = bt->capture_context
            ? bt->capture_context[bt->shadow + i]
            : GRX_NPOS;
      }
    }
  }
  if (!fresh) {
    bt->capture_context = before;
  }
  size_t outer_scope =
      bt->forward_scope_slot ? bt->slots[bt->forward_scope_slot] : GRX_NPOS;
  if (bt->forward_scope_slot) {
    bt->slots[bt->forward_scope_slot] = GRX_NPOS;
  }
  if (bt->atomic_result_slots) {
    bt->slots[bt->atomic_result_present] = GRX_NPOS;
  }
  if (bt->atomic_base) {
    bt->slots[bt->atomic_base_present] = GRX_NPOS;
  }
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
    keep_last = capture_alloc(bt, bt->captures);
    if (!keep_last) {
      bt->capture_context = outer_context;
      if (bt->atomic_base) {
        bt->slots[bt->atomic_base_present] = before[bt->atomic_base_present];
      }
      if (bt->forward_scope_slot) {
        bt->slots[bt->forward_scope_slot] = outer_scope;
      }
      bt->active_capture_owner = outer_owner;
      bt->capture_owner_depth--;
      capture_free(bt, before, before_count);
      return 0;
    }
    for (size_t i = 0; i < bt->captures; i++) {
      keep_last[i] = GRX_NPOS;
    }
    bt->keep_last = keep_last;
  }
  if ((inst->mode == GRX_LOOK_BEHIND_POSITIVE ||
          inst->mode == GRX_LOOK_BEHIND_NEGATIVE) &&
      grx_program_look_span(bt->program, inst->x, &min, &max)) {
    body_matched = look_behind_forward(bt, pc + 1, position, min, max,
        (inst->flags & GRX_INST_LOOK_NEAREST) != 0,
        (inst->flags & GRX_INST_LOOK_ROUND_BOUND) != 0,
        (inst->flags & GRX_INST_LOOK_PERSIST_CAPTURES) != 0, before, fresh != 0,
        bt->capture_context, &end);
  }
  else {
    size_t outer_look_end = bt->look_end;
    bt->look_end = GRX_NPOS;
    body_matched = run(bt, pc + 1, position, body_floor, 0, &end);
    bt->look_end = outer_look_end;
  }
  bt->keep_last = outer_keep_last;
  bt->capture_context = outer_context;
  // A zero-width BODY cannot replace its caller's physical reference end.
  if (bt->reference_end_slot)
    bt->slots[bt->reference_end_slot] = before[bt->reference_end_slot];
  if (bt->atomic_result_slots) {
    memcpy(bt->slots + bt->atomic_result_slots,
        before + bt->atomic_result_slots, bt->captures * sizeof(size_t));
    bt->slots[bt->atomic_result_present] = before[bt->atomic_result_present];
  }
  if (bt->atomic_base) {
    memcpy(bt->slots + bt->atomic_base, before + bt->atomic_base,
        bt->atomic_base * sizeof(size_t));
    bt->slots[bt->atomic_base_present] = before[bt->atomic_base_present];
  }
  int body_forward =
      bt->forward_scope_slot && bt->slots[bt->forward_scope_slot] != GRX_NPOS;
  if (bt->behind_owner_slot) {
    bt->slots[bt->behind_owner_slot] = before[bt->behind_owner_slot];
  }
  if (bt->forward_scope_slot) {
    bt->slots[bt->forward_scope_slot] = outer_scope;
  }
  bt->depth = body_floor;
  if (bt->pending_end_slot) {
    bt->slots[bt->pending_end_slot] = outer_pending;
  }
  if (bt->failure != GRX_OK) {
    capture_free(bt, keep_last, bt->captures);
    bt->active_capture_owner = outer_owner;
    bt->capture_owner_depth--;
    capture_free(bt, before, before_count);
    return 0;
  }
  if (verb_escapes_assertion(bt, negative)) {
    restore_spans(bt, before);
    capture_free(bt, keep_last, bt->captures);
    bt->active_capture_owner = outer_owner;
    bt->capture_owner_depth--;
    capture_free(bt, before, before_count);
    return 0;
  }

  if (!choosing && body_matched == negative) {
    // A positive lookaround whose body failed, or a negative one whose
    // body succeeded. Either way the construct fails and the captures
    // go back to what they were.
    if (!(inst->flags & GRX_INST_LOOK_PERSIST_CAPTURES) ||
        inst->mode == GRX_LOOK_BEHIND_NEGATIVE) {
      restore_spans(bt, before);
    }
    capture_free(bt, keep_last, bt->captures);
    bt->active_capture_owner = outer_owner;
    bt->capture_owner_depth--;
    capture_free(bt, before, before_count);
    return 0;
  }

  if (body_matched && root_arrival && ahead_arrival(bt, pc, 1) < 0) {
    capture_free(bt, keep_last, bt->captures);
    bt->active_capture_owner = outer_owner;
    bt->capture_owner_depth--;
    capture_free(bt, before, before_count);
    return 0;
  }

  if (body_matched && !negative && fresh &&
      !(inst->policy & GRX_POLICY_LOOK_HAS_CAPTURES)) {
    for (size_t i = 2; i < bt->captures; i++) {
      const size_t * result_base =
          atomic_continuation ? before + bt->atomic_base : before;
      bt->slots[i] = result_base[i];
      if (bt->shadow) {
        bt->slots[bt->shadow + i] = result_base[bt->shadow + i];
      }
    }
  }

  if (body_matched && !negative &&
      (bt->program->flags & GRX_PROGRAM_LOOK_CONTEXT) && !fresh) {
    for (size_t i = 2; i + 1 < bt->captures; i += 2) {
      if (bt->slots[i] == GRX_NPOS && bt->slots[i + 1] == GRX_NPOS) {
        bt->slots[i] = before[i];
        bt->slots[i + 1] = before[i + 1];
        if (bt->shadow) {
          bt->slots[bt->shadow + i] = before[bt->shadow + i];
          bt->slots[bt->shadow + i + 1] = before[bt->shadow + i + 1];
        }
      }
    }
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
  if (!body_matched &&
      !(inst->flags &
          (GRX_INST_KEEP_CAPTURES | GRX_INST_LOOK_PERSIST_CAPTURES))) {
    // ECMA-262 22.2.2.4: a negative lookaround leaves the captures as
    // they were, whatever its body touched on the way to failing.
    // Perl does not - see GRX_INST_KEEP_CAPTURES - and there the
    // writes fall through to the same bookkeeping a positive
    // lookaround's do.
    restore_spans(bt, before);
    if (negative && atomic_continuation) {
      for (size_t group = 2; group < bt->captures; group++) {
        size_t desired = before[bt->atomic_base + group];
        if (bt->slots[group] != desired) {
          if (!save_slot(bt, group)) {
            capture_free(bt, keep_last, bt->captures);
            bt->active_capture_owner = outer_owner;
            bt->capture_owner_depth--;
            capture_free(bt, before, before_count);
            return 0;
          }
          bt->stack[bt->depth - 1].kind = FRAME_ASSERTION_CAPTURE;
          bt->slots[group] = desired;
        }
        if (bt->shadow) {
          size_t slot = bt->shadow + group;
          desired = before[bt->atomic_base + slot];
          if (bt->slots[slot] != desired) {
            if (!save_slot(bt, slot)) {
              capture_free(bt, keep_last, bt->captures);
              bt->active_capture_owner = outer_owner;
              bt->capture_owner_depth--;
              capture_free(bt, before, before_count);
              return 0;
            }
            bt->slots[slot] = desired;
          }
        }
      }
    }
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
        if (inst->flags & GRX_INST_LOOK_PERSIST_CAPTURES) {
          continue;
        }
        if (bt->slots[i] == before[i]) {
          continue;
        }
        size_t old = before[i];
        size_t current = bt->slots[i];
        bt->slots[i] = old;
        if (!save_slot(bt, i)) {
          capture_free(bt, keep_last, bt->captures);
          bt->active_capture_owner = outer_owner;
          bt->capture_owner_depth--;
          capture_free(bt, before, before_count);
          return 0;
        }
        if (i < bt->captures &&
            (bt->program->flags & GRX_PROGRAM_LOOK_CONTEXT)) {
          bt->stack[bt->depth - 1].kind = FRAME_ASSERTION_CAPTURE;
        }
        bt->slots[i] = current;
      }
    }
  }
  if (bt->forwarded_slots) {
    for (size_t i = 2; i < bt->captures; i++) {
      size_t slot = bt->capture_open_slots + i;
      size_t value = bt->slots[slot];
      if (value != before[slot]) {
        bt->slots[slot] = before[slot];
        if (!save_slot(bt, slot)) {
          capture_free(bt, keep_last, bt->captures);
          bt->active_capture_owner = outer_owner;
          bt->capture_owner_depth--;
          capture_free(bt, before, before_count);
          return 0;
        }
        bt->slots[slot] = value;
      }
    }
    if (body_matched && !negative) {
      for (uint32_t body = pc + 1; body < inst->y; body++) {
        if (!pending_step(bt)) {
          capture_free(bt, keep_last, bt->captures);
          bt->active_capture_owner = outer_owner;
          bt->capture_owner_depth--;
          capture_free(bt, before, before_count);
          return 0;
        }
        const GRX_Inst * part = grx_program_at(bt->program, body);
        if (part->op == GRX_OP_SAVE && part->x >= 2 && part->x < bt->captures) {
          if (inst->mode == GRX_LOOK_AHEAD_POSITIVE) {
            bt->slots[bt->forward_result_slots + part->x] = bt->slots[part->x];
          }
          bt->slots[bt->capture_branch_slots + (part->x & ~(size_t)1)] =
              bt->branch_epoch;
        }
      }
    }
    for (size_t i = 2; i < bt->captures; i++) {
      size_t slot = bt->capture_branch_slots + i;
      size_t value = bt->slots[slot];
      if (value != before[slot]) {
        bt->slots[slot] = before[slot];
        if (!save_slot(bt, slot)) {
          capture_free(bt, keep_last, bt->captures);
          bt->active_capture_owner = outer_owner;
          bt->capture_owner_depth--;
          capture_free(bt, before, before_count);
          return 0;
        }
        bt->slots[slot] = value;
      }
    }
    for (size_t i = 2; i < bt->captures; i++) {
      size_t slot = bt->forward_result_slots + i;
      size_t value = bt->slots[slot];
      if (value != before[slot]) {
        bt->slots[slot] = before[slot];
        if (!save_slot(bt, slot)) {
          capture_free(bt, keep_last, bt->captures);
          bt->active_capture_owner = outer_owner;
          bt->capture_owner_depth--;
          capture_free(bt, before, before_count);
          return 0;
        }
        bt->slots[slot] = value;
      }
    }
    if (inst->mode == GRX_LOOK_AHEAD_POSITIVE ||
        inst->mode == GRX_LOOK_AHEAD_NEGATIVE) {
      const size_t * opened = entry ? entry : before;
      size_t owner = bt->active_capture_owner;
      for (size_t i = 2; i + 1 < bt->captures; i += 2) {
        if (opened[i] != GRX_NPOS && opened[i + 1] == GRX_NPOS) {
          bt->slots[bt->forwarded_slots + i] =
              owner == GRX_NPOS ? 0 : owner + 1;
        }
      }
    }
    for (size_t i = 2; i < bt->captures; i++) {
      size_t slot = bt->forwarded_slots + i;
      size_t value = bt->slots[slot];
      if (value != before[slot]) {
        bt->slots[slot] = before[slot];
        if (!save_slot(bt, slot)) {
          capture_free(bt, keep_last, bt->captures);
          bt->active_capture_owner = outer_owner;
          bt->capture_owner_depth--;
          capture_free(bt, before, before_count);
          return 0;
        }
        bt->slots[slot] = value;
      }
    }
  }
  if (!apply_atomic_owner(bt)) {
    capture_free(bt, keep_last, bt->captures);
    bt->active_capture_owner = outer_owner;
    bt->capture_owner_depth--;
    capture_free(bt, before, before_count);
    return 0;
  }
  if ((inst->flags & GRX_INST_LOOK_PROVISIONAL_END) && bt->pending_end_slot &&
      body_matched && !negative && bt->slots[1] != before[1]) {
    if (!save_slot(bt, bt->pending_end_slot)) {
      capture_free(bt, keep_last, bt->captures);
      bt->active_capture_owner = outer_owner;
      bt->capture_owner_depth--;
      capture_free(bt, before, before_count);
      return 0;
    }
    bt->slots[bt->pending_end_slot] = 1;
    size_t repeat_state = 0;
    size_t context = 0;
    size_t prior_body_end = GRX_NPOS;
    size_t repeat_global = 0;
    if (grx_program_look_repeat(bt->program, inst, &repeat_state, &context,
            &repeat_global, NULL)) {
      size_t marker = bt->captures + repeat_global + 1;
      prior_body_end = bt->slots[marker];
      if (!save_slot(bt, marker)) {
        capture_free(bt, keep_last, bt->captures);
        bt->active_capture_owner = outer_owner;
        bt->capture_owner_depth--;
        capture_free(bt, before, before_count);
        return 0;
      }
      bt->slots[marker] = bt->slots[1];
    }
    int last_ahead = (repeat_context & GRX_LOOK_REPEAT_LAST) &&
        (inst->mode == GRX_LOOK_AHEAD_POSITIVE);
    int restore_repeat = last_ahead &&
        (repeat_context & GRX_LOOK_REPEAT_NULLABLE) &&
        (repeat_context & GRX_LOOK_REPEAT_LITERAL_FRONTIER);
    int expire_repeat = last_ahead &&
        !(repeat_context & GRX_LOOK_REPEAT_NULLABLE) &&
        (repeat_context & GRX_LOOK_REPEAT_EXIT_NULLABLE);
    if (((inst->policy & GRX_POLICY_LOOK_REPLACE_END) && !retain_end) ||
        restore_repeat || expire_repeat) {
      if (!save_slot(bt, 1)) {
        capture_free(bt, keep_last, bt->captures);
        bt->active_capture_owner = outer_owner;
        bt->capture_owner_depth--;
        capture_free(bt, before, before_count);
        return 0;
      }
      int prior_repeat = (inst->mode == GRX_LOOK_BEHIND_POSITIVE) &&
          (((repeat_context & GRX_LOOK_REPEAT_LAST) &&
               (repeat_context & GRX_LOOK_REPEAT_EXIT_REPEAT)) ||
              (repeat_context & GRX_LOOK_REPEAT_COPY_POLICY)) &&
          prior_body_end != GRX_NPOS;
      bt->slots[1] = expire_repeat
          ? GRX_NPOS
          : (prior_repeat ? prior_body_end : before[1]);
      bt->slots[bt->pending_end_slot] =
          expire_repeat ? GRX_NPOS : outer_pending;
    }
  }
  if (bt->forward_scope_slot) {
    size_t origin = bt->slots[bt->end_origin_slot];
    if (body_matched && !negative && bt->slots[1] != GRX_NPOS) {
      if (body_forward) {
        origin = 1;
      }
      else if (bt->slots[1] != before[1]) {
        origin = 0;
      }
    }
    bt->slots[bt->end_origin_slot] = before[bt->end_origin_slot];
    if (!save_slot(bt, bt->end_origin_slot)) {
      capture_free(bt, keep_last, bt->captures);
      bt->active_capture_owner = outer_owner;
      bt->capture_owner_depth--;
      capture_free(bt, before, before_count);
      return 0;
    }
    bt->slots[bt->end_origin_slot] = origin;
    if (inst->mode == GRX_LOOK_AHEAD_POSITIVE ||
        inst->mode == GRX_LOOK_AHEAD_NEGATIVE) {
      if (!save_slot(bt, bt->forward_scope_slot)) {
        capture_free(bt, keep_last, bt->captures);
        bt->active_capture_owner = outer_owner;
        bt->capture_owner_depth--;
        capture_free(bt, before, before_count);
        return 0;
      }
      bt->slots[bt->forward_scope_slot] = 1;
    }
  }
  capture_free(bt, keep_last, bt->captures);
  bt->active_capture_owner = outer_owner;
  bt->capture_owner_depth--;
  capture_free(bt, before, before_count);

  // `y` when the condition held and `y + 1` when it did not; both are
  // jumps, so the branches stay in source order. For everything else
  // `y` is simply where the outer program resumes.
  *out_next = choosing && body_matched == negative ? inst->y + 1 : inst->y;
  return 1;
}

/** Whether a pending record needs this frontier, before allocating scratch. */
static int pending_ready(
    Backtrack * bt, const PendingLook * next, int frontier, uint32_t demand) {
  if (!pending_step(bt)) {
    return 0;
  }
  const GRX_Inst * inst = grx_program_at(bt->program, next->pc);
  if (!(inst->policy & GRX_POLICY_LOOK_FRONTIER)) {
    return !frontier && next->state != 1;
  }
  size_t value = next->state;
  if (demand && !next->evaluated && inst->mode == GRX_LOOK_AHEAD_POSITIVE &&
      (inst->flags & GRX_INST_LOOK_BODY_NULLABLE))
    return 0;
  int repeated_demand = 0;
  if (demand && value == 0 && (inst->policy & GRX_POLICY_LOOK_REPEATED)) {
    size_t channel = 0;
    size_t context = 0;
    grx_program_look_repeat(bt->program, inst, &channel, &context, NULL, NULL);
    size_t count = bt->slots[bt->captures + channel];
    repeated_demand = count != GRX_NPOS && count > next->repeat_activation + 1;
  }
  // A state-0 activation that has completed before owns a frontier in this
  // continuation even after rollback. A backreference can demand reevaluation
  // there. An activation not yet evaluated must first pass its consuming atom.
  return value != 1 &&
      !((inst->policy & GRX_POLICY_LOOK_AFTER_ATOM) &&
          ((demand &&
               ((!next->evaluated && !repeated_demand) || value == GRX_NPOS)) ||
              (frontier && value == GRX_NPOS)));
}

/** A completed forward assertion owns only captures its body writes. */
static int restore_ahead_results(Backtrack * bt, const PendingLook * pending) {
  if (!pending) {
    return 1;
  }
  for (const PendingLook * next = bt->results; next;
      next = next->result_previous) {
    if (!pending_step(bt)) {
      return 0;
    }
    if (next->sequence > pending->sequence) {
      continue;
    }
    for (size_t i = 2; i < bt->captures; i++) {
      if (next->result[i] != GRX_NPOS && bt->slots[i] == GRX_NPOS) {
        if (!save_slot(bt, i)) {
          return 0;
        }
        bt->slots[i] = next->result[i];
      }
    }
  }
  return 1;
}

static int has_ahead_result(Backtrack * bt, const PendingLook * pending) {
  if (!pending) {
    return 0;
  }
  for (const PendingLook * next = bt->results; next;
      next = next->result_previous) {
    if (!pending_step(bt)) {
      return 0;
    }
    if (next->sequence <= pending->sequence) {
      return 1;
    }
  }
  return 0;
}

/** Check postponed assertions at the next consuming frontier or accepting exit.
 */
static int check_pending_owned(
    Backtrack * bt, int frontier, uint32_t demand, size_t owner_floor) {
  if (frontier && !bt->frontier_count) {
    return 1;
  }
  int ready = 0;
  for (const PendingLook * next = bt->ready; next;
      next = next->active_previous) {
    if (next->floor >= owner_floor &&
        pending_ready(bt, next, frontier, demand)) {
      ready = 1;
      break;
    }
  }
  if (!ready) {
    return bt->failure == GRX_OK;
  }
  size_t * continuation = capture_alloc(bt, bt->slot_count);
  if (!continuation) {
    return 0;
  }
  for (const PendingLook * next = bt->ready; next;
      next = next->active_previous) {
    const GRX_Inst * inst = grx_program_at(bt->program, next->pc);
    int deferred = (inst->policy & GRX_POLICY_LOOK_FRONTIER) != 0;
    if (next->floor < owner_floor ||
        !pending_ready(bt, next, frontier, demand)) {
      continue;
    }
    if (demand) {
      int writes_group = 0;
      for (uint32_t pc = next->pc + 1; pc < inst->y; pc++) {
        bt->steps++;
        if (bt->request->limits->max_steps &&
            bt->steps > bt->request->limits->max_steps) {
          bt->failure = GRX_ERR_LIMIT;
          bt->failure_diag = GRX_DIAG_LIMIT_STEPS;
          capture_free(bt, continuation, bt->slot_count);
          return 0;
        }
        const GRX_Inst * part = grx_program_at(bt->program, pc);
        if (part->op == GRX_OP_SAVE && part->x / 2 == demand) {
          writes_group = 1;
          break;
        }
      }
      if (!writes_group) {
        continue;
      }
    }
    snapshot_spans(bt, continuation);
    size_t writes = bt->depth;
    int combining = 0;
    if (deferred && !demand) {
      for (const PendingLook * previous = next->active_previous; previous;
          previous = previous->active_previous) {
        if (previous->floor >= owner_floor &&
            pending_ready(bt, previous, frontier, 0)) {
          combining = 1;
          break;
        }
      }
    }
    uint32_t after = 0;
    if (!evaluate_look(bt, next->pc, next->position, &after,
            combining || next->repeat_later, next->base, 1,
            has_ahead_result(bt, next->previous), next->owner, next->entry,
            next->repeat_context)) {
      capture_free(bt, continuation, bt->slot_count);
      return 0;
    }
    ((PendingLook *)next)->evaluated = 1;
    if (next->result) {
      for (size_t i = 0; i < bt->captures; i++) {
        next->result[i] = GRX_NPOS;
      }
      for (uint32_t body = next->pc + 1; body < inst->y; body++) {
        bt->steps++;
        if (bt->request->limits->max_steps &&
            bt->steps > bt->request->limits->max_steps) {
          bt->failure = GRX_ERR_LIMIT;
          bt->failure_diag = GRX_DIAG_LIMIT_STEPS;
          capture_free(bt, continuation, bt->slot_count);
          return 0;
        }
        const GRX_Inst * part = grx_program_at(bt->program, body);
        if (part->op == GRX_OP_SAVE && part->x >= 2 && part->x < bt->captures) {
          next->result[part->x] = bt->slots[part->x];
        }
      }
    }
    if ((inst->mode == GRX_LOOK_BEHIND_POSITIVE ||
            inst->mode == GRX_LOOK_BEHIND_NEGATIVE) &&
        !restore_ahead_results(bt, next->previous)) {
      capture_free(bt, continuation, bt->slot_count);
      return 0;
    }
    if (deferred) {
      // A fresh assertion context replaces its incoming captures, but a
      // SAVE reached after assertion entry belongs to the continuation.
      // Read ownership from the existing undo frames rather than keeping
      // a cross-start capture cache or guessing from capture text.
      for (size_t i = next->floor; i < writes; i++) {
        if (!pending_step(bt)) {
          capture_free(bt, continuation, bt->slot_count);
          return 0;
        }
        if (bt->stack[i].kind != FRAME_CAPTURE &&
            bt->stack[i].kind != FRAME_ASSERTION_CAPTURE) {
          continue;
        }
        size_t slot = bt->stack[i].pc;
        if (slot < 2 || slot >= bt->captures) {
          continue;
        }
        if (bt->slots[slot] != continuation[slot]) {
          if (!save_slot(bt, slot)) {
            capture_free(bt, continuation, bt->slot_count);
            return 0;
          }
          bt->slots[slot] = continuation[slot];
        }
        if (bt->shadow) {
          bt->slots[bt->shadow + slot] = continuation[bt->shadow + slot];
        }
      }
    }
    if (!pending_set_state(bt, (PendingLook *)next, 1)) {
      capture_free(bt, continuation, bt->slot_count);
      return 0;
    }
  }
  capture_free(bt, continuation, bt->slot_count);
  return bt->failure == GRX_OK && apply_atomic_owner(bt);
}

static int check_pending(Backtrack * bt, int frontier, uint32_t demand) {
  return check_pending_owned(bt, frontier, demand, 0);
}

/** Keep the highest-priority result for one input arrival. */
/** Context discovery does not by itself make an entry a displacing frontier. */
static int atomic_owns_frontier(const Backtrack * bt, const GRX_Inst * inst) {
  return (inst->policy & GRX_POLICY_ATOMIC_FRONTIER) &&
      !(bt->forward_scope_slot && bt->slots[bt->forward_scope_slot] == 1 &&
          bt->slots[bt->pending_end_slot] != GRX_NPOS);
}

/** Completed BODY output owns a field, including an explicit empty field. */
static const size_t * atomic_inherited_context(
    const Backtrack * bt, size_t group, size_t position) {
  if (!bt->atomic_history) {
    return NULL;
  }
  if (bt->atomic_history[2 * bt->captures + group] != GRX_NPOS ||
      bt->atomic_history[4 * bt->captures + group + 1] == 1) {
    return bt->atomic_history + 2 * bt->captures;
  }
  size_t owner = bt->atomic_history[6 * bt->captures + group];
  return owner != GRX_NPOS && owner <= position
      ? bt->atomic_history + 5 * bt->captures
      : NULL;
}

/** Effective BODY input before entry materializes its undoable capture overlay.
 */
static size_t atomic_input_slot(const Backtrack * bt, size_t slot) {
  size_t group = slot & ~(size_t)1;
  if (bt->slots[bt->atomic_base_present] != GRX_NPOS &&
      bt->slots[bt->capture_branch_slots + group] != bt->branch_epoch) {
    return bt->slots[bt->atomic_base + slot];
  }
  if (bt->slots[bt->capture_open_slots + group] == 1 ||
      bt->slots[bt->capture_branch_slots + group] == bt->branch_epoch) {
    return bt->slots[slot];
  }
  const size_t * inherited =
      atomic_inherited_context(bt, group, bt->frontier_position);
  return inherited ? inherited[slot] : bt->slots[slot];
}

static int atomic_context_equal(
    Backtrack * bt, uint32_t pc, const size_t * a, const size_t * b) {
  const GRX_Inst * begin = grx_program_at(bt->program, pc);
  for (uint32_t body = pc + 1; body < begin->x; body++) {
    if (!pending_step(bt)) {
      return 0;
    }
    const GRX_Inst * part = grx_program_at(bt->program, body);
    if (part->op != GRX_OP_BACKREF) {
      continue;
    }
    size_t slot = 2 * (size_t)part->x;
    if (slot + 1 < bt->captures &&
        (a[slot] != (b ? b[slot] : atomic_input_slot(bt, slot)) ||
            a[slot + 1] !=
                (b ? b[slot + 1] : atomic_input_slot(bt, slot + 1)))) {
      return 0;
    }
  }
  return 1;
}

/** An absent reference has no span owner outside its incoming activation. */
static size_t atomic_input_owner(
    Backtrack * bt, uint32_t pc, const size_t * input) {
  const GRX_Inst * begin = grx_program_at(bt->program, pc);
  if (!(begin->policy & GRX_POLICY_ATOMIC_READS_CAPTURES)) {
    return bt->atomic_input_owner_slot ? bt->slots[bt->atomic_input_owner_slot]
                                       : GRX_NPOS;
  }
  for (uint32_t body = pc + 1; body < begin->x; body++) {
    if (!pending_step(bt)) {
      return 0;
    }
    const GRX_Inst * part = grx_program_at(bt->program, body);
    if (part->op == GRX_OP_BACKREF) {
      size_t slot = 2 * (size_t)part->x;
      if (slot + 1 < bt->captures) {
        size_t start = input ? input[slot] : atomic_input_slot(bt, slot);
        size_t end = input ? input[slot + 1] : atomic_input_slot(bt, slot + 1);
        if (start == GRX_NPOS) {
          return bt->attempt_start;
        }
        if (start == end) {
          return start;
        }
      }
    }
  }
  return bt->atomic_input_owner_slot ? bt->slots[bt->atomic_input_owner_slot]
                                     : GRX_NPOS;
}

static int atomic_needs_incoming_record(Backtrack * bt, uint32_t pc) {
  const GRX_Inst * begin = grx_program_at(bt->program, pc);
  if (begin->policy & GRX_POLICY_ATOMIC_READS_CAPTURES) {
    return 1;
  }
  for (size_t group = 2; group + 1 < bt->captures; group += 2) {
    int replaced = 0;
    for (uint32_t body = pc + 1; body < begin->x; body++) {
      if (!pending_step(bt)) {
        return 0;
      }
      const GRX_Inst * part = grx_program_at(bt->program, body);
      // Only a write on the linear mandatory prefix proves replacement.
      // Branches, calls and negative/conditional assertions can bypass it.
      if (part->op == GRX_OP_SPLIT || part->op == GRX_OP_CALL ||
          part->op == GRX_OP_VERB ||
          (part->op == GRX_OP_JMP && part->x != body + 1) ||
          (part->op == GRX_OP_LOOK &&
              (part->flags & GRX_INST_COND_ELSE ||
                  (part->mode != GRX_LOOK_AHEAD_POSITIVE &&
                      part->mode != GRX_LOOK_BEHIND_POSITIVE)))) {
        return 1;
      }
      if (part->op == GRX_OP_SAVE && (part->x & ~(size_t)1) == group) {
        replaced = 1;
        break;
      }
    }
    if (!replaced) {
      return 1;
    }
  }
  return 0;
}

/** Caller closure snapshots have full identity, independent of BODY reads. */
static int atomic_incoming(Backtrack * bt, size_t position) {
  AtomicIncoming ** at = &bt->discovered_incoming;
  while (*at && (*at)->position <= position) {
    if (!pending_step(bt)) {
      return 0;
    }
    if ((*at)->position == position &&
        memcmp((*at)->context, bt->slots, bt->captures * sizeof(size_t)) == 0) {
      return 1;
    }
    at = &(*at)->next;
  }
  if (!advance_generation(bt, &bt->discovery_priority)) {
    return 0;
  }
  AtomicIncoming * entry =
      capture_alloc(bt, sizeof(*entry) / sizeof(size_t) + bt->captures);
  if (!entry) {
    return 0;
  }
  *entry = (AtomicIncoming){.next = *at,
      .position = position,
      .priority = bt->discovery_priority - 1,
      .origin = bt->attempt_start};
  memcpy(entry->context, bt->slots, bt->captures * sizeof(size_t));
  *at = entry;
  return 1;
}

static int atomic_arrival(Backtrack * bt, size_t start, size_t end) {
  AtomicArrival ** at = &bt->discovered_arrivals;
  while (*at && (*at)->start < start) {
    if (!pending_step(bt)) {
      return 0;
    }
    at = &(*at)->next;
  }
  const size_t * context = bt->atomic_context
      ? bt->atomic_context->caller + 2 * bt->captures
      : bt->slots;
  size_t input_owner = bt->atomic_context
      ? bt->atomic_context->input_owner
      : atomic_input_owner(bt, bt->discovery_atomic, context);
  for (AtomicArrival * prior = *at; prior && prior->start == start;
      prior = prior->next) {
    if (!pending_step(bt)) {
      return 0;
    }
    if (prior->input_owner == input_owner &&
        atomic_context_equal(
            bt, bt->discovery_atomic, prior->context, context)) {
      if (end != GRX_NPOS && prior->origin == bt->attempt_start) {
        prior->end = end;
      }
      return 1;
    }
  }
  if (!advance_generation(bt, &bt->discovery_priority)) {
    return 0;
  }
  AtomicArrival * arrival = capture_alloc(bt,
      (sizeof(*arrival) + sizeof(size_t) - 1) / sizeof(size_t) + bt->captures);
  if (!arrival) {
    return 0;
  }
  *arrival = (AtomicArrival){.next = *at,
      .start = start,
      .end = end,
      .priority = bt->discovery_priority - 1,
      .origin = bt->attempt_start,
      .input_owner = input_owner};
  memcpy(arrival->context, context, bt->captures * sizeof(size_t));
  *at = arrival;
  return 1;
}

/** Discover BODY inputs or strictly earlier caller closures using the same VM.
 */
static int atomic_discover(Backtrack * bt, AtomicSchedule * schedule,
    uint32_t pc, int future, size_t bound) {
  // The probe owns every mutable buffer. Only the immutable request,
  // Program, allocator and inherited capture context are borrowed read-only.
  // Capture-write generations are copied by value: probe metadata never moves
  // into the caller's live slots, so its independent counter is not merged.
  Backtrack * probe =
      capture_alloc(bt, (sizeof(*probe) + sizeof(size_t) - 1) / sizeof(size_t));
  if (!probe) {
    return 0;
  }
  *probe = *bt;
  probe->behind_frontiers = NULL;
  probe->stack = NULL;
  probe->depth = 0;
  probe->capacity = 0;
  probe->stack_is_scratch = 1;
  probe->stack_depth_base = bt->stack_depth_base + bt->depth;
  probe->discovery_schedules = bt->atomic_schedules;
  probe->atomic_schedules = NULL;
  // Temporarily move this mutable chain into the isolated interpreter.
  // Other schedules are read-only; this schedule's target is not borrowed.
  probe->discovered_arrivals = future ? NULL : schedule->arrivals;
  if (!future) {
    schedule->arrivals = NULL;
  }
  probe->discovered_incoming = future ? schedule->incoming : NULL;
  if (future) {
    schedule->incoming = NULL;
  }
  probe->discovery_atomic = pc;
  probe->discovery_first_start = GRX_NPOS;
  probe->discovery_multiple = 0;
  probe->atomic_entry_merged = 0;
  probe->discovery_depth = bt->run_depth + 1;
  probe->discovery_prefix_only = future;
  probe->discovery_bound = bound;
  probe->visited = NULL;
  probe->memo_after = 0;
  probe->pending = NULL;
  probe->ready = NULL;
  probe->results = NULL;
  probe->keep_last = NULL;
  probe->atomic_history = NULL;
  probe->atomic_context = NULL;
  probe->best_slots = NULL;
  probe->call_return = NULL;
  probe->call_group = NULL;
  probe->call_slots = NULL;
  probe->call_depth = 0;
  probe->call_capacity = 0;
  probe->marks = NULL;
  probe->mark_depth = 0;
  probe->mark_capacity = 0;
  probe->callout_captures = NULL;
  probe->slots = capture_alloc(probe, bt->slot_count);
  if (probe->slots) {
    memcpy(probe->slots, bt->body_entry, bt->slot_count * sizeof(size_t));
    const size_t * history = future ? bt->atomic_history : bt->body_history;
    if (history) {
      probe->atomic_history = capture_alloc(probe, 7 * bt->captures);
      if (probe->atomic_history) {
        memcpy(
            probe->atomic_history, history, 7 * bt->captures * sizeof(size_t));
      }
    }
    if (probe->failure == GRX_OK) {
      size_t ignored = 0;
      if (!future) {
        (void)run(probe, bt->body_pc, bt->body_position, 0, 0, &ignored);
      }
      else {
        size_t origin = bt->attempt_start;
        while (origin < bound && probe->failure == GRX_OK) {
          uint32_t codepoint = 0;
          size_t width = 0;
          if (!pending_step(probe) ||
              !read_forward(probe, origin, &codepoint, &width)) {
            break;
          }
          origin += width;
          if (origin >= bound) {
            break;
          }
          for (size_t slot = 0; slot < probe->slot_count; slot++) {
            probe->slots[slot] = GRX_NPOS;
          }
          probe->attempt_start = origin;
          probe->depth = 0;
          probe->best_end = GRX_NPOS;
          probe->verb_stop = VERB_NONE;
          probe->mark_depth = 0;
          probe->mark = GRX_INDEX_NONE;
          (void)run(probe, bt->body_pc, origin, 0, 0, &ignored);
        }
      }
    }
  }
  // All arrays outside assertion scratch are empty for this policy's
  // programs, but are independently owned if an instruction grows them.
  capture_free(
      probe, probe->stack, probe->capacity * sizeof(Frame) / sizeof(size_t));
  gcu_allocator_free(bt->allocator, probe->call_return);
  gcu_allocator_free(bt->allocator, probe->call_group);
  gcu_allocator_free(bt->allocator, probe->call_slots);
  gcu_allocator_free(bt->allocator, probe->marks);
  gcu_allocator_free(bt->allocator, probe->visited);
  capture_free(probe, probe->slots, bt->slot_count);
  capture_free(probe, probe->atomic_history, 7 * bt->captures);
  bt->discovery_priority = probe->discovery_priority;
  bt->steps = probe->steps;
  bt->failure = probe->failure;
  bt->failure_diag = probe->failure_diag;
  bt->scratch_bytes = probe->scratch_bytes;
  if (!future) {
    schedule->arrivals = probe->discovered_arrivals;
    schedule->merged = probe->discovery_multiple;
    schedule->first_entry = probe->discovery_first_start;
  }
  schedule->incoming = probe->discovered_incoming;
  probe->discovered_incoming = NULL;
  capture_free(
      bt, probe, (sizeof(*probe) + sizeof(size_t) - 1) / sizeof(size_t));
  if (bt->failure != GRX_OK) {
    return 0;
  }
  return 1;
}

static int scalar_priority_frame(const Frame * frame) {
  return frame->kind == FRAME_RESUME || frame->kind == FRAME_ALTERNATIVE ||
      frame->kind == FRAME_PRIORITY_BRANCH;
}

/** Save the ordered branch path at the first reachable scalar frontier. */
static int scalar_priority_save(Backtrack * bt, size_t target) {
  if (!bt->request->scalar_priority_out)
    return 1;
  size_t count = 0;
  for (size_t i = bt->request->scalar_priority_floor; i < bt->depth; i++) {
    if (!pending_step(bt))
      return 0;
    count += scalar_priority_frame(&bt->stack[i]);
  }
  size_t bytes = (count + 7) / 8;
  size_t words =
      (sizeof(ScalarPriority) + bytes + sizeof(size_t) - 1) / sizeof(size_t);
  ScalarPriority * priority = capture_alloc(bt, words);
  if (!priority)
    return 0;
  memset(priority, 0, words * sizeof(size_t));
  priority->target = target;
  priority->origin = bt->request->scalar_local
      ? bt->request->scalar_priority_origin
      : bt->attempt_start;
  priority->count = count;
  priority->words = words;
  size_t bit = 0;
  for (size_t i = bt->request->scalar_priority_floor; i < bt->depth; i++) {
    if (!pending_step(bt)) {
      capture_free(bt, priority, words);
      return 0;
    }
    if (!scalar_priority_frame(&bt->stack[i]))
      continue;
    if (bt->stack[i].kind == FRAME_PRIORITY_BRANCH)
      priority->path[bit / 8] |= (unsigned char)(1u << (bit % 8));
    bit++;
  }
  *bt->request->scalar_priority_out = priority;
  return 1;
}

/** Compare discovery path priorities; an ended path lies between preferred
 * and fallback continuations. */
static int scalar_priority_before(
    Backtrack * bt, const ScalarPriority * a, const ScalarPriority * b) {
  if (a->origin != b->origin)
    return a->origin < b->origin;
  size_t count = a->count < b->count ? a->count : b->count;
  for (size_t bit = 0; bit < count; bit++) {
    if (!pending_step(bt))
      return 0;
    int av = (a->path[bit / 8] >> (bit % 8)) & 1;
    int bv = (b->path[bit / 8] >> (bit % 8)) & 1;
    if (av != bv)
      return av < bv;
  }
  if (a->count == b->count)
    return 0;
  if (a->count == count)
    return ((b->path[count / 8] >> (count % 8)) & 1) != 0;
  return ((a->path[count / 8] >> (count % 8)) & 1) == 0;
}

/** A higher-priority cluster arrival retains its logical cluster end. */
static int scalar_reports_cluster(Backtrack * bt, size_t position) {
  uint32_t point = 0;
  size_t width = 0;
  if (!read_backward(bt, position, &point, &width))
    return 1;
  size_t target = position - width;
  const ScalarPriority * scalar = NULL;
  for (const ScalarPriority * candidate = bt->request->scalar_priorities;
      candidate; candidate = candidate->next) {
    if (!pending_step(bt))
      return 0;
    if (candidate->target == target &&
        (!scalar || scalar_priority_before(bt, candidate, scalar)))
      scalar = candidate;
  }
  if (!scalar)
    return 1;
  size_t origin = bt->request->scalar_local
      ? bt->request->scalar_priority_origin
      : bt->attempt_start;
  if (origin != scalar->origin)
    return origin < scalar->origin;
  size_t bit = 0;
  for (size_t i = bt->request->scalar_priority_floor; i < bt->depth; i++) {
    if (!pending_step(bt))
      return 0;
    if (!scalar_priority_frame(&bt->stack[i]))
      continue;
    int actual = bt->stack[i].kind == FRAME_PRIORITY_BRANCH;
    if (bit == scalar->count)
      return !actual;
    int prior = (scalar->path[bit / 8] >> (bit % 8)) & 1;
    if (actual != prior)
      return actual < prior;
    bit++;
  }
  if (bit < scalar->count)
    return ((scalar->path[bit / 8] >> (bit % 8)) & 1) != 0;
  return 1;
}

/** Map one borrowed assertion record into an isolated activation list. */
static PendingLook * scalar_pending_map(Backtrack * bt,
    const PendingLook * source, PendingLook * target,
    const PendingLook * wanted) {
  while (source && target) {
    if (!pending_step(bt))
      return NULL;
    if (source == wanted)
      return target;
    source = source->previous;
    target = target->previous;
  }
  return NULL;
}

/** A prefix query borrows no mutable caller capture, assertion or atomic data.
 */
static int scalar_scope_discover(
    Backtrack * bt, ScalarScope * scope, size_t target, int * found) {
  const size_t words =
      (sizeof(Backtrack) + sizeof(size_t) - 1) / sizeof(size_t);
  Backtrack * probe = capture_alloc(bt, words);
  if (!probe)
    return 0;
  *probe = *bt;
  GRX_ExecRequest request = scope->request;
  request.scalar_query = 1;
  request.scalar_local = 1;
  request.scalar_depth = bt->run_depth;
  request.scalar_target = target;
  request.scalar_stop_pc = scope->stop;
  void * priority_result = NULL;
  request.scalar_priority_out = &priority_result;
  request.scalar_priority_floor = bt->depth;
  request.scalar_priority_origin = scope->origin;
  request.match = NULL;
  request.out_steps = NULL;
  request.out_diag = NULL;
  probe->request = &request;
  probe->behind_frontiers = NULL;
  probe->stack = NULL;
  probe->depth = 0;
  probe->capacity = 0;
  probe->stack_is_scratch = 1;
  probe->stack_depth_base = bt->stack_depth_base + bt->depth;
  probe->slots = NULL;
  probe->atomic_history = NULL;
  probe->atomic_context = NULL;
  probe->atomic_schedules = NULL;
  probe->discovery_schedules = bt->atomic_schedules;
  probe->discovery_depth = 0;
  probe->discovery_atomic = GRX_INDEX_NONE;
  probe->discovered_arrivals = NULL;
  probe->discovered_incoming = NULL;
  probe->visited = NULL;
  probe->memo_after = 0;
  probe->pending = NULL;
  probe->ready = NULL;
  probe->results = NULL;
  probe->keep_last = NULL;
  probe->best_slots = NULL;
  probe->call_return = NULL;
  probe->call_group = NULL;
  probe->call_slots = NULL;
  probe->call_depth = 0;
  probe->call_capacity = 0;
  probe->marks = NULL;
  probe->mark_depth = 0;
  probe->mark_capacity = 0;
  probe->callout_captures = NULL;
  size_t * entry = capture_alloc(probe, bt->slot_count);
  probe->slots = capture_alloc(probe, bt->slot_count);
  if (entry && probe->slots) {
    memcpy(entry, bt->slots, bt->slot_count * sizeof(size_t));
    memcpy(probe->slots, bt->slots, bt->slot_count * sizeof(size_t));
  }
  probe->body_entry = entry;
  probe->body_pc = scope->pc;
  probe->body_position = scope->origin;
  if (bt->atomic_history && probe->failure == GRX_OK) {
    probe->atomic_history = capture_alloc(probe, 7 * bt->captures);
    if (probe->atomic_history)
      memcpy(probe->atomic_history, bt->atomic_history,
          7 * bt->captures * sizeof(size_t));
  }
  probe->body_history = probe->atomic_history;
  AtomicContext ** context_tail = &probe->atomic_context;
  for (const AtomicContext * old = bt->atomic_context;
      old && probe->failure == GRX_OK; old = old->previous) {
    size_t count = sizeof(*old) / sizeof(size_t) + 5 * bt->captures;
    AtomicContext * copy = capture_alloc(probe, count);
    if (!copy)
      break;
    memcpy(copy, old, count * sizeof(size_t));
    copy->frontier = NULL;
    copy->previous = NULL;
    *context_tail = copy;
    context_tail = &copy->previous;
  }
  PendingLook ** pending_tail = &probe->pending;
  for (const PendingLook * old = bt->pending; old && probe->failure == GRX_OK;
      old = old->previous) {
    PendingLook * copy = capture_alloc(probe, GRX_PENDING_WORDS);
    if (!copy)
      break;
    *copy = *old;
    copy->previous = NULL;
    copy->active_previous = NULL;
    copy->active_next = NULL;
    copy->result_previous = NULL;
    copy->base = NULL;
    copy->entry = NULL;
    copy->result = NULL;
    *pending_tail = copy;
    pending_tail = &copy->previous;
    if (old->base) {
      copy->base = capture_alloc(probe, bt->atomic_base);
      if (copy->base)
        memcpy(copy->base, old->base, bt->atomic_base * sizeof(size_t));
    }
    if (old->entry) {
      copy->entry = capture_alloc(probe, bt->captures);
      if (copy->entry)
        memcpy(copy->entry, old->entry, bt->captures * sizeof(size_t));
    }
    if (old->result) {
      copy->result = capture_alloc(probe, bt->captures);
      if (copy->result)
        memcpy(copy->result, old->result, bt->captures * sizeof(size_t));
    }
  }
  const PendingLook * old = bt->pending;
  for (PendingLook * copy = probe->pending; copy && old;
      copy = copy->previous, old = old->previous) {
    copy->active_previous = scalar_pending_map(
        probe, bt->pending, probe->pending, old->active_previous);
    copy->active_next = scalar_pending_map(
        probe, bt->pending, probe->pending, old->active_next);
    copy->result_previous = scalar_pending_map(
        probe, bt->pending, probe->pending, old->result_previous);
  }
  probe->ready =
      scalar_pending_map(probe, bt->pending, probe->pending, bt->ready);
  probe->results =
      scalar_pending_map(probe, bt->pending, probe->pending, bt->results);
  if (bt->depth && probe->failure == GRX_OK) {
    probe->stack =
        capture_alloc(probe, bt->depth * sizeof(Frame) / sizeof(size_t));
    if (probe->stack) {
      memcpy(probe->stack, bt->stack, bt->depth * sizeof(Frame));
      probe->capacity = bt->depth;
      probe->depth = bt->depth;
      for (size_t i = 0; i < bt->depth; i++) {
        if (!pending_step(probe))
          break;
        if (probe->stack[i].kind == FRAME_PENDING ||
            probe->stack[i].kind == FRAME_PENDING_STATE) {
          probe->stack[i].pending = scalar_pending_map(
              probe, bt->pending, probe->pending, bt->stack[i].pending);
        }
      }
    }
  }
  *found = 0;
  if (probe->failure == GRX_OK) {
    size_t ignored = 0;
    *found =
        run_body(probe, scope->pc, scope->origin, probe->depth, 0, &ignored);
  }
  while (probe->pending) {
    PendingLook * pending = probe->pending;
    probe->pending = pending->previous;
    pending_destroy(probe, pending);
  }
  while (probe->atomic_context)
    atomic_context_leave(probe);
  while (probe->atomic_schedules) {
    AtomicSchedule * schedule = probe->atomic_schedules;
    probe->atomic_schedules = schedule->next;
    while (schedule->arrivals) {
      AtomicArrival * arrival = schedule->arrivals;
      schedule->arrivals = arrival->next;
      capture_free(probe, arrival,
          (sizeof(*arrival) + sizeof(size_t) - 1) / sizeof(size_t) +
              bt->captures);
    }
    atomic_incoming_clear(probe, &schedule->incoming);
    scalar_activation_free(probe, schedule->frontiers);
    capture_free(probe, schedule,
        (sizeof(*schedule) + sizeof(size_t) - 1) / sizeof(size_t));
  }
  capture_free(
      probe, probe->stack, probe->capacity * sizeof(Frame) / sizeof(size_t));
  capture_free(probe, entry, bt->slot_count);
  capture_free(probe, probe->slots, bt->slot_count);
  capture_free(probe, probe->atomic_history, 7 * bt->captures);
  gcu_allocator_free(bt->allocator, probe->call_return);
  gcu_allocator_free(bt->allocator, probe->call_group);
  gcu_allocator_free(bt->allocator, probe->call_slots);
  gcu_allocator_free(bt->allocator, probe->marks);
  gcu_allocator_free(bt->allocator, probe->visited);
  ScalarPriority * priority = priority_result;
  if (priority) {
    if (probe->failure == GRX_OK && *found) {
      priority->next = scope->priorities;
      scope->priorities = priority;
      scope->request.scalar_priorities = scope->priorities;
    }
    else
      capture_free(probe, priority, priority->words);
  }
  bt->steps = probe->steps;
  bt->failure = probe->failure;
  bt->failure_diag = probe->failure_diag;
  bt->scratch_bytes = probe->scratch_bytes;
  capture_free(bt, probe, words);
  return bt->failure == GRX_OK;
}

static void scalar_scope_leave(Backtrack * bt, ScalarScope * scope) {
  if (scope->frontiers && scope->activation) {
    for (size_t at = scope->origin;
        at < bt->window_end && bt->failure == GRX_OK; at++) {
      if (!pending_step(bt))
        break;
      if (!scope->frontiers[at])
        continue;
      scope->activation->exposed[at] |= 2u;
      uint32_t point = 0;
      size_t width = 0;
      if (!read_forward(bt, at, &point, &width))
        continue;
      size_t next = at + width;
      while (read_forward(bt, next, &point, &width) &&
          grx_display_cell_width(point, 0) == 0) {
        if (!pending_step(bt))
          break;
        scope->activation->exposed[next] |= 1u;
        next += width;
      }
    }
  }
  if (bt->program->flags & GRX_PROGRAM_DEFER_COMPOSING)
    bt->slots[bt->captures] = scope->old_defer;
  if (bt->program->flags & GRX_PROGRAM_REPORT_COMPOSING)
    bt->slots[bt->captures +
        ((bt->program->flags & GRX_PROGRAM_DEFER_COMPOSING) ? 1u : 0u)] =
        scope->old_report;
  bt->request = scope->outer;
  if (scope->activation) {
    while (scope->activation->priorities) {
      ScalarPriority * prior = scope->activation->priorities;
      scope->activation->priorities = prior->next;
      capture_free(bt, prior, prior->words);
    }
    while (scope->priorities) {
      ScalarPriority * candidate = scope->priorities;
      scope->priorities = candidate->next;
      ScalarPriority ** selected = &scope->activation->priorities;
      while (*selected && (*selected)->target != candidate->target) {
        if (!pending_step(bt))
          break;
        selected = &(*selected)->next;
      }
      if (*selected && !scalar_priority_before(bt, candidate, *selected)) {
        capture_free(bt, candidate, candidate->words);
      }
      else {
        ScalarPriority * replaced = *selected;
        candidate->next = replaced ? replaced->next : NULL;
        *selected = candidate;
        if (replaced)
          capture_free(bt, replaced, replaced->words);
      }
    }
  }
  while (scope->priorities) {
    ScalarPriority * priority = scope->priorities;
    scope->priorities = priority->next;
    capture_free(bt, priority, priority->words);
  }
  capture_free(bt, scope->frontiers, scope->words);
  capture_free(
      bt, scope, (sizeof(*scope) + sizeof(size_t) - 1) / sizeof(size_t));
}

/** Each body invocation owns a fresh query input and a fresh frontier table. */
static int scalar_scope_enter(Backtrack * bt, uint32_t pc, size_t position,
    uint32_t id, uint32_t stop, int atomic, ScalarScope ** out_scope) {
  ScalarScope * scope =
      capture_alloc(bt, (sizeof(*scope) + sizeof(size_t) - 1) / sizeof(size_t));
  if (!scope)
    return 0;
  *scope = (ScalarScope){.request = *bt->request,
      .outer = bt->request,
      .pc = pc,
      .stop = stop,
      .origin = position,
      .atomic = atomic,
      .floor = bt->depth};
  scope->request.scalar_scope = id;
  scope->request.scalar_frontiers = NULL;
  scope->request.scalar_query = 0;
  scope->request.scalar_depth = bt->run_depth;
  scope->request.scalar_local = 2;
  scope->request.scalar_owner = scope;
  scope->request.scalar_cutoff = NULL;
  scope->request.scalar_priorities = NULL;
  scope->request.scalar_priority_out = NULL;
  scope->request.scalar_priority_floor = bt->depth;
  scope->request.scalar_priority_origin = position;
  bt->request = &scope->request;
  if (bt->program->flags & GRX_PROGRAM_DEFER_COMPOSING) {
    scope->old_defer = bt->slots[bt->captures];
    bt->slots[bt->captures] = GRX_NPOS;
  }
  if (bt->program->flags & GRX_PROGRAM_REPORT_COMPOSING) {
    size_t slot = bt->captures +
        ((bt->program->flags & GRX_PROGRAM_DEFER_COMPOSING) ? 1u : 0u);
    scope->old_report = bt->slots[slot];
    bt->slots[slot] = GRX_NPOS;
  }
  if (out_scope)
    *out_scope = scope;
  int competition = 0;
  for (size_t i = 0; i < bt->program->insts.count; i++) {
    if (!pending_step(bt))
      break;
    const GRX_Inst * inst = grx_program_at(bt->program, (uint32_t)i);
    if (grx_program_scope(bt->program, (uint32_t)i) == id &&
        grx_exec_scalar_cluster_consumer(inst)) {
      competition = 1;
      break;
    }
  }
  if (bt->failure == GRX_OK && competition && atomic) {
    for (AtomicSchedule * schedule = bt->atomic_schedules; schedule;
        schedule = schedule->next) {
      if (!pending_step(bt))
        break;
      if (schedule->pc + 1 != pc || schedule->attempt != bt->attempt_start)
        continue;
      BehindFrontier * activation = schedule->frontiers;
      while (activation && activation->origin != position) {
        if (!pending_step(bt))
          break;
        activation = activation->previous;
      }
      if (!activation && bt->failure == GRX_OK) {
        size_t words =
            (sizeof(*activation) + sizeof(size_t) - 1) / sizeof(size_t) +
            bt->request->length / sizeof(size_t) + 1;
        activation = capture_alloc(bt, words);
        if (activation) {
          memset(activation, 0, words * sizeof(size_t));
          activation->previous = schedule->frontiers;
          activation->origin = position;
          activation->words = words;
          activation->body = pc;
          schedule->frontiers = activation;
        }
      }
      scope->activation = activation;
      break;
    }
  }
  if (!atomic && bt->behind_frontiers && bt->behind_frontiers->body == pc)
    scope->activation = bt->behind_frontiers;
  if (bt->failure == GRX_OK && competition) {
    scope->words = bt->request->length / sizeof(size_t) + 1;
    scope->frontiers = capture_alloc(bt, scope->words);
    if (scope->frontiers) {
      memset(scope->frontiers, 0, scope->words * sizeof(size_t));
      scope->request.scalar_frontiers = scope->frontiers;
      if (scope->activation) {
        for (size_t at = position; at < bt->window_end; at++) {
          if (!pending_step(bt))
            break;
          scope->frontiers[at] = (scope->activation->exposed[at] & 2u) != 0;
        }
        for (ScalarPriority * prior = scope->activation->priorities; prior;
            prior = prior->next) {
          if (!pending_step(bt))
            break;
          ScalarPriority * copy = capture_alloc(bt, prior->words);
          if (!copy)
            break;
          memcpy(copy, prior, prior->words * sizeof(size_t));
          copy->next = scope->priorities;
          scope->priorities = copy;
        }
        scope->request.scalar_priorities = scope->priorities;
      }
      for (size_t at = position;
          at < bt->window_end && bt->failure == GRX_OK;) {
        uint32_t point = 0;
        uint32_t next = 0;
        size_t width = 0;
        size_t next_width = 0;
        if (!pending_step(bt) || !read_forward(bt, at, &point, &width))
          break;
        int relevant = read_forward(bt, at + width, &next, &next_width) &&
            grx_display_cell_width(next, 0) == 0;
        int literal = 0;
        for (size_t i = 0; relevant && !literal && i < bt->program->insts.count;
            i++) {
          if (!pending_step(bt))
            break;
          const GRX_Inst * inst = grx_program_at(bt->program, (uint32_t)i);
          literal = grx_program_scope(bt->program, (uint32_t)i) == id &&
              (inst->policy & GRX_POLICY_SCALAR_LITERAL) &&
              ((inst->op == GRX_OP_CHAR && inst->x == point) ||
                  (inst->op == GRX_OP_CLASS && in_class(bt, inst->x, point)));
        }
        if (literal && bt->failure == GRX_OK) {
          int found = 0;
          if (!scalar_scope_discover(bt, scope, at, &found))
            break;
          scope->frontiers[at] |= (unsigned char)found;
        }
        at += width;
      }
    }
  }
  if (bt->failure != GRX_OK) {
    if (out_scope)
      *out_scope = NULL;
    scalar_scope_leave(bt, scope);
    return 0;
  }
  return 1;
}


/**
 * Enumerate incoming paths with this same instruction machine. The isolated
 * pass stops each path at its atomic exit, so it never tests or commits the
 * continuation. Earlier accepting exits own their input frontier and displace
 * a competing entry at that position. The ordinary pass retains path priority
 * among the remaining entries, including all continuation backtracking.
 */
static int atomic_entry_allowed(Backtrack * bt, uint32_t pc, size_t position) {
  AtomicSchedule * schedule = bt->atomic_schedules;
  while (schedule && schedule->pc != pc) {
    if (!pending_step(bt)) {
      return 0;
    }
    schedule = schedule->next;
  }
  if (!schedule || schedule->attempt != bt->attempt_start) {
    if (!schedule) {
      schedule = capture_alloc(
          bt, (sizeof(*schedule) + sizeof(size_t) - 1) / sizeof(size_t));
      if (!schedule) {
        return 0;
      }
      *schedule = (AtomicSchedule){.next = bt->atomic_schedules, .pc = pc};
      bt->atomic_schedules = schedule;
    }
    schedule->attempt = bt->attempt_start;
    scalar_activation_free(bt, schedule->frontiers);
    schedule->frontiers = NULL;
    atomic_incoming_clear(bt, &schedule->incoming);
    schedule->future_bound = 0;
    if (!atomic_discover(bt, schedule, pc, 0, 0)) {
      return 0;
    }
    for (AtomicArrival * arrival = schedule->arrivals; arrival;
        arrival = arrival->next) {
      if (!pending_step(bt)) {
        return 0;
      }
      arrival->displaced = 0;
    }
    for (AtomicArrival * arrival = schedule->arrivals; arrival;
        arrival = arrival->next) {
      if (!pending_step(bt)) {
        return 0;
      }
      if (arrival->displaced || arrival->end == GRX_NPOS) {
        continue;
      }
      for (AtomicArrival * later = arrival->next;
          later && later->start <= arrival->end; later = later->next) {
        if (!pending_step(bt)) {
          return 0;
        }
        // A reference-bearing BODY can share its read input across starts,
        // but each search origin owns an independent successful caller.
        if ((grx_program_at(bt->program, pc)->policy &
                GRX_POLICY_ATOMIC_READS_CAPTURES) &&
            arrival->origin != later->origin) {
          continue;
        }
        if (arrival->input_owner != GRX_NPOS &&
            memcmp(arrival->context, later->context,
                bt->captures * sizeof(size_t)) != 0) {
          continue;
        }
        if (arrival->input_owner == later->input_owner &&
            atomic_context_equal(bt, pc, arrival->context, later->context) &&
            (later->start == arrival->end ||
                arrival->priority < later->priority)) {
          later->displaced = 1;
        }
      }
    }
    size_t prior_end = 0;
    size_t group_end = 0;
    size_t group_start = GRX_NPOS;
    for (AtomicArrival * arrival = schedule->arrivals; arrival;
        arrival = arrival->next) {
      if (!pending_step(bt)) {
        return 0;
      }
      if (arrival->start != group_start) {
        if (group_end > prior_end) {
          prior_end = group_end;
        }
        group_start = arrival->start;
        group_end = 0;
      }
      arrival->prior_end = prior_end;
      if (!arrival->displaced && arrival->end != GRX_NPOS &&
          arrival->end > group_end) {
        group_end = arrival->end;
      }
    }
  }
  bt->atomic_entry_merged = schedule->merged && position > schedule->first_entry;
  bt->atomic_entry_overlap = 0;
  size_t input_owner = atomic_input_owner(bt, pc, NULL);
  for (AtomicArrival * arrival = schedule->arrivals; arrival;
      arrival = arrival->next) {
    if (!pending_step(bt)) {
      return 0;
    }
    if (arrival->start == position && arrival->input_owner == input_owner &&
        atomic_context_equal(bt, pc, arrival->context, NULL)) {
      bt->atomic_entry_overlap = arrival->prior_end > position;
      return !arrival->displaced &&
          (arrival->end != GRX_NPOS || arrival->origin == bt->attempt_start);
    }
  }
  return 1;
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

    if (bt->discovery_prefix_only && bt->run_depth == bt->discovery_depth &&
        position >= bt->discovery_bound) {
      if (!backtrack(bt, &pc, &position, floor)) {
        return 0;
      }
      continue;
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

    bt->frontier_position = position;
    const GRX_Inst * inst =
        GRX_ARENA_AT(const GRX_Inst, &bt->program->insts, pc);
    if (!inst) {
      bt->failure = GRX_ERR_INTERNAL;
      return 0;
    }

    if (bt->request->scalar_query &&
        bt->run_depth == bt->request->scalar_depth) {
      size_t target = bt->request->scalar_target;
      if (inst->op == GRX_OP_ASSERT && inst->mode == GRX_ASSERT_LOOK_LENGTH) {
        pc++;
        continue;
      }
      if (bt->request->scalar_local && pc == bt->request->scalar_stop_pc) {
        if (!backtrack(bt, &pc, &position, floor))
          return 0;
        continue;
      }
      int consumer = inst->op == GRX_OP_CHAR || inst->op == GRX_OP_CLASS ||
          inst->op == GRX_OP_ANY || inst->op == GRX_OP_ANY_NL;
      if (position > target || (consumer && position == target)) {
        uint32_t point = 0;
        size_t width = 0;
        if (position == target &&
            grx_program_scope(bt->program, pc) == bt->request->scalar_scope &&
            (inst->policy & GRX_POLICY_SCALAR_LITERAL) &&
            read_forward(bt, position, &point, &width) &&
            ((inst->op == GRX_OP_CHAR && point == inst->x) ||
                (inst->op == GRX_OP_CLASS && in_class(bt, inst->x, point)))) {
          if (!scalar_priority_save(bt, position))
            return 0;
          *out_end = position;
          return 1;
        }
        if (!backtrack(bt, &pc, &position, floor))
          return 0;
        continue;
      }
    }

    if ((bt->program->flags & GRX_PROGRAM_DEFER_COMPOSING) &&
        bt->slots[bt->captures] != GRX_NPOS &&
        grx_exec_composing_frontier(inst)) {
      if (!save_slot(bt, bt->captures)) {
        return 0;
      }
      size_t saved_position = bt->slots[bt->captures];
      bt->slots[bt->captures] = GRX_NPOS;
      uint32_t mark = 0;
      size_t width = 0;
      size_t previous_width = 0;
      uint32_t previous = 0;
      int scalar = bt->request->scalar_frontiers &&
          read_backward(bt, saved_position, &previous, &previous_width) &&
          bt->request->scalar_frontiers[saved_position - previous_width];
      while (!scalar && read_forward(bt, position, &mark, &width) &&
          grx_display_cell_width(mark, 0) == 0) {
        if (!pending_step(bt)) {
          return 0;
        }
        position += width;
      }
    }
    int reverse = (inst->flags & GRX_INST_REVERSE) != 0;
    uint32_t codepoint = 0;
    size_t width = 0;
    int consumed = 0;
    int ok = 1;
    size_t reported_position = position;
    if (bt->program->flags & GRX_PROGRAM_REPORT_COMPOSING) {
      size_t index = bt->captures +
          ((bt->program->flags & GRX_PROGRAM_DEFER_COMPOSING) ? 1u : 0u);
      if (bt->slots[index] != GRX_NPOS && bt->slots[index] > reported_position)
        reported_position = bt->slots[index];
    }

    switch ((GRX_Opcode)inst->op) {
      case GRX_OP_CHAR:
      case GRX_OP_CLASS:
      case GRX_OP_ANY:
      case GRX_OP_ANY_NL: {
        if (!check_pending(bt, 1, 0)) {
          if (bt->failure != GRX_OK || bt->verb_stop != VERB_NONE) {
            return 0;
          }
          ok = 0;
          break;
        }
        int have = reverse ? read_backward(bt, position, &codepoint, &width)
                           : read_forward(bt, position, &codepoint, &width);
        if (!have) {
          ok = 0;
          break;
        }
        switch ((GRX_Opcode)inst->op) {
          case GRX_OP_CHAR:
            ok = codepoint == inst->x;
            break;
          case GRX_OP_CLASS: {
            uint32_t tested = codepoint;
            if (inst->policy & GRX_POLICY_CLASS_TAIL) {
              size_t at = position + width;
              uint32_t mark = 0;
              size_t mark_width = 0;
              while (read_forward(bt, at, &mark, &mark_width) &&
                  grx_display_cell_width(mark, 0) == 0) {
                if (!pending_step(bt)) {
                  return 0;
                }
                tested = mark;
                at += mark_width;
              }
            }
            ok = in_class(bt, inst->x, tested);
            break;
          }
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
              ok =
                  !grx_crlf_begins_at(bt->request->subject, bt->window_end, at);
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
        if (!advance_generation(bt, &bt->branch_epoch)) {
          return 0;
        }
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
        if ((inst->flags & GRX_INST_SAVE_IF_UNSET) && inst->x < bt->captures &&
            bt->slots[inst->x] != GRX_NPOS) {
          pc++;
          continue;
        }
        if (bt->forwarded_slots && inst->x >= 2 && inst->x < bt->captures &&
            !(inst->x & 1)) {
          if (!save_slot(bt, bt->forwarded_slots + inst->x)) {
            return 0;
          }
          bt->slots[bt->forwarded_slots + inst->x] = GRX_NPOS;
          if (!save_slot(bt, bt->forward_result_slots + inst->x) ||
              !save_slot(bt, bt->forward_result_slots + inst->x + 1)) {
            return 0;
          }
          bt->slots[bt->forward_result_slots + inst->x] = GRX_NPOS;
          bt->slots[bt->forward_result_slots + inst->x + 1] = GRX_NPOS;
        }
        if (bt->capture_open_slots && inst->x >= 2 && inst->x < bt->captures) {
          size_t group = inst->x & ~(size_t)1;
          if (!capture_write(bt, group)) {
            return 0;
          }
          if (!save_slot(bt, bt->capture_open_slots + group)) {
            return 0;
          }
          bt->slots[bt->capture_open_slots + group] =
              (inst->x & 1) ? GRX_NPOS : 1;
          if (!save_slot(bt, bt->capture_branch_slots + group)) {
            return 0;
          }
          bt->slots[bt->capture_branch_slots + group] =
              (inst->x & 1) ? bt->branch_epoch : GRX_NPOS;
        }
        if (!save_slot(bt, inst->x)) {
          return 0;
        }
        if (inst->x < bt->captures) {
          bt->slots[inst->x] = reported_position;
          // An odd slot is a group's end, so the group has just become whole.
          // Recorded rather than read off the slots at the end, because by
          // then a later iteration may have cleared it.
          if (bt->keep_last && (inst->x % 2) == 1 &&
              bt->slots[inst->x - 1] != GRX_NPOS) {
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
          if (slot >= 2 && !(slot & 1) && !capture_write(bt, slot)) {
            return 0;
          }
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

      case GRX_OP_ASSERT: {
        size_t previous_width = 0;
        uint32_t previous = 0;
        int scalar = bt->request->scalar_frontiers &&
            read_backward(bt, position, &previous, &previous_width) &&
            bt->request->scalar_frontiers[position - previous_width];
        ok = ((bt->program->flags & GRX_PROGRAM_DEFER_COMPOSING) &&
                 bt->slots[bt->captures] != GRX_NPOS && !scalar &&
                 inst->mode == GRX_ASSERT_CLUSTER_BOUNDARY) ||
            assertion_holds(bt, inst, position);
        if (!ok && (inst->policy & GRX_POLICY_ASSERT_FINAL_BOUNDARY) &&
            bt->run_depth == 1 && !bt->request->scalar_query &&
            !bt->discovery_depth && bt->ahead_arrivals) {
          for (size_t i = 0; i < bt->program->insts.count; i++) {
            if (!pending_step(bt))
              return 0;
            if (bt->ahead_arrivals[i] == bt->attempt_start)
              bt->ahead_arrivals[bt->program->insts.count + i] = 1;
          }
        }
        break;
      }

      case GRX_OP_COUNT_INIT:
      case GRX_OP_COUNT_STEP:
      case GRX_OP_COUNT_BACK: {
        if (inst->x >= bt->program->look_spans.count ||
            bt->program->look_spans.count - inst->x < 3) {
          bt->failure = GRX_ERR_INTERNAL;
          return 0;
        }
        const size_t * metadata =
            GRX_ARENA_AT(const size_t, &bt->program->look_spans, inst->x);
        if (bt->registers < 5 || metadata[2] > bt->registers - 5) {
          bt->failure = GRX_ERR_INTERNAL;
          return 0;
        }
        size_t state = bt->captures + metadata[2];
        // Count resets on each invocation. Counted backedge history is
        // search-origin local; an unbounded loop instead scopes that cache
        // to this invocation. Progress and BODY starts have ordinary undo.
        if (inst->op == GRX_OP_COUNT_INIT) {
          bt->slots[state] = 0;
          bt->slots[state + 1] = 0;
          if (metadata[1] == GRX_REPEAT_INF) {
            if (!save_slot(bt, state + 2)) {
              return 0;
            }
            bt->slots[state + 2] = GRX_NPOS;
          }
          if (!save_slot(bt, state + 3)) {
            return 0;
          }
          bt->slots[state + 3] = 0;
          pc++;
          continue;
        }
        // Only a BODY backedge tests the remembered end. Testing initial
        // entry would reject a later invocation before trying its BODY.
        if (inst->op == GRX_OP_COUNT_BACK) {
          const GRX_Inst * step = grx_program_at(bt->program, inst->y);
          if (bt->slots[state + 4] == position) {
            if ((metadata[0] == 0 && bt->slots[state + 3]) ||
                (metadata[0] > 1 && !bt->slots[state + 3])) {
              ok = 0;
              break;
            }
            pc = step->y;
            continue;
          }
          bt->slots[state + 3] = 1;
          if (bt->slots[state + 2] == position) {
            ok = 0;
            break;
          }
          bt->slots[state + 2] = position;
          pc = inst->y;
          continue;
        }
        if (bt->slots[state + 1]) {
          if (--bt->slots[state] == 0) {
            bt->slots[state + 1] = 0;
          }
        }
        else {
          bt->slots[state]++;
        }
        size_t count = bt->slots[state];
        int negative = bt->slots[state + 1] != 0;
        // Maximum exits and lazy optional exits own no decrement frame.
        // Mandatory/greedy BODY failures decrement before their next exit.
        if (!negative && metadata[1] != GRX_REPEAT_INF && count > metadata[1]) {
          pc = inst->y;
          continue;
        }
        if (!negative && count > metadata[0]) {
          if (!push(bt, FRAME_RESUME,
                  inst->mode == GRX_REPEAT_LAZY ? pc + 1 : inst->y, position)) {
            return 0;
          }
          if (inst->mode == GRX_REPEAT_LAZY) {
            pc = inst->y;
            continue;
          }
        }
        if (state > UINT32_MAX) {
          bt->failure = GRX_ERR_LIMIT;
          bt->failure_diag = GRX_DIAG_LIMIT_MATCH_MEMORY;
          return 0;
        }
        if (!push(bt, FRAME_COUNTER, (uint32_t)state, 0)) {
          return 0;
        }
        pc++;
        continue;
      }

      case GRX_OP_PROGRESS_SET: {
        size_t index = bt->captures + inst->x;
        if (!save_slot(bt, index)) {
          return 0;
        }
        if (index < bt->captures + bt->registers) {
          bt->slots[index] = position;
          if ((inst->policy & GRX_POLICY_REPORT_COMPOSING) &&
              scalar_reports_cluster(bt, position)) {
            uint32_t point = 0;
            size_t width = 0;
            while (read_forward(bt, bt->slots[index], &point, &width) &&
                grx_display_cell_width(point, 0) == 0) {
              if (!pending_step(bt))
                return 0;
              bt->slots[index] += width;
            }
          }
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
        if (!check_pending(bt, 1, inst->x)) {
          if (bt->failure != GRX_OK || bt->verb_stop != VERB_NONE) {
            return 0;
          }
          ok = 0;
          break;
        }
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
          if (!backref_complete(bt, inst, width, reverse, &position))
            return 0;
        }
        pc++;
        if (ok) {
          continue;
        }
        break;

      case GRX_OP_ATOMIC_BEGIN:
        if (bt->discovery_prefix_only && bt->run_depth == bt->discovery_depth &&
            pc == bt->discovery_atomic) {
          int closed = 0;
          for (size_t group = 2; group + 1 < bt->captures; group += 2) {
            if (bt->slots[group] != GRX_NPOS &&
                bt->slots[group + 1] != GRX_NPOS &&
                bt->slots[group] < bt->slots[group + 1] &&
                bt->slots[bt->capture_open_slots + group] != 1) {
              closed = 1;
              break;
            }
          }
          if (closed && !atomic_incoming(bt, position)) {
            return 0;
          }
          ok = 0;
          break;
        }
        if (bt->run_depth == bt->discovery_depth &&
            pc == bt->discovery_atomic) {
          if (bt->discovery_first_start == GRX_NPOS) {
            bt->discovery_first_start = position;
          }
          else if (bt->discovery_first_start != position) {
            bt->discovery_multiple = 1;
            if (position < bt->discovery_first_start) {
              bt->discovery_first_start = position;
            }
          }
        }
        if (bt->discovery_depth && pc != bt->discovery_atomic &&
            bt->run_depth == bt->discovery_depth &&
            atomic_owns_frontier(bt, inst)) {
          for (const AtomicSchedule * previous = bt->discovery_schedules;
              previous; previous = previous->next) {
            if (!pending_step(bt)) {
              return 0;
            }
            if (previous->pc != pc) {
              continue;
            }
            for (const AtomicArrival * arrival = previous->arrivals; arrival;
                arrival = arrival->next) {
              if (!pending_step(bt)) {
                return 0;
              }
              if (arrival->start == position && arrival->displaced &&
                  atomic_context_equal(bt, pc, arrival->context, NULL)) {
                ok = 0;
              }
            }
          }
          if (!ok) {
            break;
          }
        }
        if (!bt->discovery_depth && bt->atomic_base) {
          int allowed = atomic_entry_allowed(bt, pc, position);
          if (bt->failure != GRX_OK) {
            return 0;
          }
          if (!allowed && !bt->request->scalar_query &&
              atomic_owns_frontier(bt, inst)) {
            ok = 0;
            break;
          }
        }
        if (bt->atomic_base) {
          if (bt->slots[bt->atomic_base_present] != GRX_NPOS) {
            for (size_t group = 2; group + 1 < bt->captures; group += 2) {
              // An enclosing open capture owns its start until its closing
              // SAVE. A nested BODY's inherited closed-capture snapshot
              // cannot replace that live opening.
              if (bt->slots[bt->capture_open_slots + group] == 1 ||
                  bt->slots[bt->capture_branch_slots + group] ==
                      bt->branch_epoch) {
                continue;
              }
              for (size_t i = group; i <= group + 1; i++) {
                if (!save_slot(bt, i)) {
                  return 0;
                }
                bt->slots[i] = bt->slots[bt->atomic_base + i];
                if (bt->shadow) {
                  if (!save_slot(bt, bt->shadow + i)) {
                    return 0;
                  }
                  bt->slots[bt->shadow + i] =
                      bt->slots[bt->atomic_base + bt->shadow + i];
                }
              }
            }
          }
          AtomicContext * context = capture_alloc(
              bt, sizeof(*context) / sizeof(size_t) + 5 * bt->captures);
          if (!context) {
            return 0;
          }
          context->previous = bt->atomic_context;
          context->frontier = NULL;
          context->merged = bt->atomic_entry_merged;
          context->overlap = bt->atomic_entry_overlap;
          context->reporting = bt->slots[bt->atomic_result_present] == GRX_NPOS
              ? 0
              : (int)bt->slots[bt->atomic_result_present];
          bt->atomic_context = context;
          for (size_t i = 0; i < bt->captures; i++) {
            int inherited = bt->slots[bt->atomic_base_present] != GRX_NPOS &&
                bt->slots[i] == GRX_NPOS;
            context->caller[3 * bt->captures + i] = 0;
            context->caller[2 * bt->captures + i] = bt->slots[i];
            size_t group = i & ~(size_t)1;
            int current = i < 2 ||
                bt->slots[bt->capture_branch_slots + group] == bt->branch_epoch;
            context->caller[i] = context->reporting && !current
                ? bt->slots[bt->atomic_result_slots + i]
                : inherited ? bt->slots[bt->atomic_base + i]
                            : bt->slots[i];
            context->caller[bt->captures + i] = bt->shadow
                ? (inherited ? bt->slots[bt->atomic_base + bt->shadow + i]
                             : bt->slots[bt->shadow + i])
                : GRX_NPOS;
          }
          for (size_t group = 2; group + 1 < bt->captures; group += 2) {
            context->caller[3 * bt->captures + group] =
                bt->slots[bt->capture_branch_slots + group] ==
                        bt->branch_epoch &&
                    bt->slots[group] != GRX_NPOS
                ? 2
                : 0;
            if (bt->slots[bt->capture_branch_slots + group + 1] == 1) {
              context->caller[3 * bt->captures + group] |= ATOMIC_CAPTURE_BEFORE_START;
            }
            context->caller[3 * bt->captures + group + 1] =
                bt->slots[bt->capture_open_slots + group + 1];
          }
          if (!bt->atomic_history) {
            bt->atomic_history = capture_alloc(bt, 7 * bt->captures);
            if (!bt->atomic_history) {
              return 0;
            }
            for (size_t i = 0; i < 7 * bt->captures; i++) {
              bt->atomic_history[i] = GRX_NPOS;
            }
          }
          for (size_t group = 2; group + 1 < bt->captures; group += 2) {
            int closed = bt->slots[bt->capture_open_slots + group] != 1 &&
                bt->slots[group] != GRX_NPOS &&
                bt->slots[group + 1] != GRX_NPOS;
            int owned =
                bt->slots[bt->capture_branch_slots + group] == bt->branch_epoch;
            size_t * owner = bt->atomic_history + 6 * bt->captures + group;
            if (closed && owned &&
                bt->atomic_history[4 * bt->captures + group] != 1) {
              bt->atomic_history[4 * bt->captures + group] = 1;
              bt->atomic_history[bt->captures + group] = bt->slots[group];
              bt->atomic_history[bt->captures + group + 1] =
                  bt->slots[group + 1];
            }
            if (closed && owned && (*owner == GRX_NPOS || position < *owner)) {
              *owner = position;
              bt->atomic_history[5 * bt->captures + group] = bt->slots[group];
              bt->atomic_history[5 * bt->captures + group + 1] =
                  bt->slots[group + 1];
            }
            const size_t * inherited =
                atomic_inherited_context(bt, group, position);
            if (bt->slots[bt->atomic_base_present] == GRX_NPOS && !owned &&
                bt->slots[bt->capture_open_slots + group] != 1 && inherited) {
              for (size_t i = group; i <= group + 1; i++) {
                if (!save_slot(bt, i)) {
                  return 0;
                }
                bt->slots[i] = inherited[i];
                context->caller[i] = bt->slots[i];
                context->caller[bt->captures + i] = bt->slots[i];
              }
              if (!shadow_close(bt, group + 1, 0)) {
                return 0;
              }
            }
          }
        }
        // A completed deferred ahead owns the caller's reported captures;
        // a later capture-reading BODY starts from its fresh read context.
        // Keep the reporting owner in context->caller before projecting reads.
        if ((inst->policy & GRX_POLICY_ATOMIC_READS_CAPTURES) &&
            has_ahead_result(bt, bt->pending)) {
          for (size_t slot = 2; slot < bt->captures; slot++) {
            if (!save_slot(bt, slot) || !shadow_clear(bt, slot))
              return 0;
            bt->slots[slot] =
                bt->capture_context ? bt->capture_context[slot] : GRX_NPOS;
            if (bt->shadow)
              bt->slots[bt->shadow + slot] = bt->capture_context
                  ? bt->capture_context[bt->shadow + slot]
                  : GRX_NPOS;
          }
        }
        if (bt->atomic_context) {
          bt->atomic_context->incoming = atomic_needs_incoming_record(bt, pc);
          bt->atomic_context->input_owner =
              atomic_input_owner(bt, pc, bt->slots);
          memcpy(bt->atomic_context->caller + 2 * bt->captures, bt->slots,
              bt->captures * sizeof(size_t));
          if (bt->shadow) {
            memcpy(bt->atomic_context->caller + 4 * bt->captures,
                bt->slots + bt->shadow, bt->captures * sizeof(size_t));
          }
        }
        if (bt->run_depth == bt->discovery_depth &&
            pc == bt->discovery_atomic && bt->atomic_context &&
            bt->atomic_context->incoming) {
          if (!atomic_incoming(bt, position) ||
              !atomic_arrival(bt, position, GRX_NPOS)) {
            return 0;
          }
        }
        if (bt->atomic_result_slots) {
          if (!save_slot(bt, bt->atomic_result_present)) {
            return 0;
          }
          bt->slots[bt->atomic_result_present] = GRX_NPOS;
        }
        if (bt->forward_result_slots) {
          for (size_t i = 2; i < bt->captures; i++) {
            size_t slot = bt->atomic_base + bt->forward_result_slots + i;
            if (!save_slot(bt, slot)) {
              return 0;
            }
            bt->slots[slot] = bt->slots[bt->forward_result_slots + i];
          }
        }
        if (bt->atomic_base) {
          // Fresh assertion bodies inside an atomic region inherit the
          // incoming context. Its lifetime starts at entry, before any
          // nested assertion can replace a capture.
          for (size_t i = 0; i < bt->captures; i++) {
            if (!save_slot(bt, bt->atomic_base + i)) {
              return 0;
            }
            size_t group = i & ~(size_t)1;
            int current = i < 2 ||
                bt->slots[bt->capture_branch_slots + group] ==
                    bt->branch_epoch ||
                bt->slots[bt->atomic_base_present] != GRX_NPOS;
            bt->slots[bt->atomic_base + i] = current ? bt->slots[i] : GRX_NPOS;
            if (bt->shadow) {
              if (!save_slot(bt, bt->atomic_base + bt->shadow + i)) {
                return 0;
              }
              bt->slots[bt->atomic_base + bt->shadow + i] =
                  current ? bt->slots[bt->shadow + i] : GRX_NPOS;
            }
          }
          if (!save_slot(bt, bt->atomic_base_present)) {
            return 0;
          }
          bt->slots[bt->atomic_base_present] = ATOMIC_BASE_CONTEXT;
        }
        if (bt->end_origin_slot && bt->slots[bt->end_origin_slot] == 1 &&
            bt->slots[bt->pending_end_slot] != GRX_NPOS &&
            (inst->policy & GRX_POLICY_ATOMIC_REQUIRED)) {
          if (!save_slot(bt, 1) || !save_slot(bt, bt->pending_end_slot)) {
            return 0;
          }
          bt->slots[1] = GRX_NPOS;
          bt->slots[bt->pending_end_slot] = GRX_NPOS;
        }
        if ((bt->program->flags & GRX_PROGRAM_SCALAR_FRONTIER) &&
            bt->atomic_context) {
          uint32_t stop = pc + 1;
          while (stop < bt->program->insts.count) {
            if (!pending_step(bt))
              return 0;
            const GRX_Inst * end = grx_program_at(bt->program, stop);
            if (end->op == GRX_OP_ATOMIC_END &&
                grx_program_scope(bt->program, stop) ==
                    grx_program_scope(bt->program, pc))
              break;
            stop++;
          }
          if (!scalar_scope_enter(bt, pc + 1, position,
                  grx_program_scope(bt->program, pc), stop, 1,
                  &bt->atomic_context->frontier))
            return 0;
        }
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
        // An exit belongs to this atomic body only after all assertions
        // entered inside its barrier have completed. Outer pending work
        // belongs to the continuation and must not validate a discovery.
        if (!check_pending_owned(bt, 0, 0, barrier + 1)) {
          if (bt->failure != GRX_OK) {
            return 0;
          }
          ok = 0;
          break;
        }
        int discovering = bt->run_depth == bt->discovery_depth &&
            barrier < bt->depth &&
            bt->stack[barrier].pc == bt->discovery_atomic;
        size_t arrival_start =
            barrier < bt->depth ? bt->stack[barrier].position : position;
        int reads = barrier < bt->depth &&
            (grx_program_at(bt->program, bt->stack[barrier].pc)->policy &
                GRX_POLICY_ATOMIC_READS_CAPTURES);
        int required = barrier < bt->depth &&
            (grx_program_at(bt->program, bt->stack[barrier].pc)->policy &
                GRX_POLICY_ATOMIC_REQUIRED);
        int captured_result = 0;
        if (bt->atomic_context) {
          for (size_t group = 2; group + 1 < bt->captures; group += 2) {
            if (bt->slots[bt->capture_open_slots + group + 1] !=
                bt->atomic_context->caller[3 * bt->captures + group + 1]) {
              captured_result = 1;
              break;
            }
          }
        }
        int shifted_context = 0;
        if (bt->atomic_history && bt->atomic_context &&
            bt->atomic_context->incoming) {
          const size_t * incoming =
              bt->atomic_context->caller + 2 * bt->captures;
          const AtomicIncoming * frontier = NULL;
          const AtomicIncoming * future_frontier = NULL;
          const AtomicIncoming * entries =
              discovering ? bt->discovered_incoming : NULL;
          if (!discovering && barrier < bt->depth) {
            for (const AtomicSchedule * schedule = bt->atomic_schedules;
                schedule; schedule = schedule->next) {
              if (!pending_step(bt)) {
                return 0;
              }
              if (schedule->pc == bt->stack[barrier].pc) {
                const GRX_Inst * begin =
                    grx_program_at(bt->program, schedule->pc);
                if (bt->body_pc == 0 && !bt->request->anchored &&
                    (begin->policy & GRX_POLICY_ATOMIC_CONTEXT_CONTINUATION) &&
                    position > schedule->future_bound) {
                  if (!atomic_discover(bt, (AtomicSchedule *)schedule,
                          schedule->pc, 1, position)) {
                    return 0;
                  }
                  ((AtomicSchedule *)schedule)->future_bound = position;
                }
                entries = schedule->incoming;
                break;
              }
            }
          }
          for (const AtomicIncoming * entry = entries;
              entry && entry->position <= position; entry = entry->next) {
            if (!pending_step(bt)) {
              return 0;
            }
            if (entry->origin == bt->attempt_start &&
                entry->position == position &&
                (!frontier || entry->priority < frontier->priority)) {
              frontier = entry;
            }
            if (entry->origin > bt->attempt_start &&
                entry->position < position &&
                (!future_frontier ||
                    entry->position > future_frontier->position ||
                    (entry->position == future_frontier->position &&
                        (entry->origin < future_frontier->origin ||
                            (entry->origin == future_frontier->origin &&
                                entry->priority <
                                    future_frontier->priority))))) {
              future_frontier = entry;
            }
          }
          shifted_context =
              !frontier && future_frontier && !bt->atomic_context->merged;
          const AtomicIncoming * marked_prefix = NULL;
          for (const AtomicIncoming * entry = entries; entry &&
              entry->position < position; entry = entry->next) {
            if (!pending_step(bt)) {
              return 0;
            }
            if (entry->origin == bt->attempt_start &&
                (!marked_prefix || entry->position > marked_prefix->position ||
                    (entry->position == marked_prefix->position &&
                        entry->priority < marked_prefix->priority))) {
              marked_prefix = entry;
            }
          }
          const AtomicIncoming * report_prefix = reads ? frontier
              : !bt->atomic_context->overlap ? marked_prefix : NULL;
          if (report_prefix && barrier < bt->depth &&
              (grx_program_at(bt->program, bt->stack[barrier].pc)->policy &
                  GRX_POLICY_ATOMIC_CONTEXT_CONTINUATION)) {
            for (size_t group = 2; group + 1 < bt->captures; group += 2) {
              if (bt->atomic_context->caller[3 * bt->captures + group] &
                  ATOMIC_CAPTURE_BEFORE_START) {
                bt->atomic_context->caller[3 * bt->captures + group] |=
                    ATOMIC_CAPTURE_MARKED_REPORT;
                for (size_t i = group; i <= group + 1; i++) {
                  bt->atomic_context->caller[i] = report_prefix->context[i];
                  bt->atomic_context->caller[bt->captures + i] =
                      report_prefix->context[i];
                }
              }
            }
          }
          const size_t * outgoing = frontier ? frontier->context
              : shifted_context              ? future_frontier->context
                                             : incoming;
          for (size_t group = 2; group + 1 < bt->captures; group += 2) {
            size_t * owner = bt->atomic_history + 6 * bt->captures + group;
            const size_t * visible =
                captured_result && !frontier && !shifted_context ? NULL
                                                                 : outgoing;
            if (!frontier) {
              const AtomicIncoming * earlier = NULL;
              for (const AtomicIncoming * entry = entries;
                  entry && entry->position < arrival_start;
                  entry = entry->next) {
                if (!pending_step(bt)) {
                  return 0;
                }
                if (entry->origin == bt->attempt_start &&
                    entry->context[group] != GRX_NPOS &&
                    (!earlier || entry->priority < earlier->priority)) {
                  earlier = entry;
                }
              }
              if (earlier) {
                visible = earlier->context;
              }
            }
            int current_prefix =
                bt->atomic_context->caller[3 * bt->captures + group] & 2;
            if ((!visible || visible[group] == GRX_NPOS) &&
                *owner != GRX_NPOS &&
                (captured_result ? !current_prefix && *owner < position
                                 : *owner <= position)) {
              visible = bt->atomic_history + 5 * bt->captures;
            }
            bt->atomic_history[bt->captures + group] =
                visible ? visible[group] : GRX_NPOS;
            bt->atomic_history[bt->captures + group + 1] =
                visible ? visible[group + 1] : GRX_NPOS;
          }
        }
        if (!discovering && bt->atomic_history && barrier < bt->depth &&
            (grx_program_at(bt->program, bt->stack[barrier].pc)->policy &
                GRX_POLICY_ATOMIC_END_OWNER) &&
            bt->atomic_history[1] != GRX_NPOS &&
            (!(grx_program_at(bt->program, bt->stack[barrier].pc)->policy &
                 GRX_POLICY_ATOMIC_FRONTIER) ||
                bt->atomic_history[1] <= bt->attempt_start)) {
          if (!save_slot(bt, 1)) {
            return 0;
          }
          bt->slots[1] = bt->atomic_history[1];
        }
        if (bt->atomic_history) {
          int marker_owner = barrier < bt->depth &&
              (grx_program_at(bt->program, bt->stack[barrier].pc)->policy &
                  GRX_POLICY_ATOMIC_END_OWNER);
          // Group zero's unused start field tags the outgoing end context.
          // A later marker-free BODY may read this context, but its own
          // consuming exit does not replace the earlier marker owner's end.
          if (marker_owner || bt->atomic_history[bt->captures] != 1) {
            bt->atomic_history[bt->captures + 1] = bt->atomic_context &&
                    bt->atomic_context->merged &&
                    bt->atomic_history[1] != GRX_NPOS
                ? bt->atomic_history[1]
                : position;
          }
          if (marker_owner) {
            bt->atomic_history[bt->captures] = 1;
          }
          for (size_t group = 2; group + 1 < bt->captures; group += 2) {
            if (bt->slots[group] != GRX_NPOS &&
                bt->slots[group] ==
                    bt->slots[bt->atomic_base + bt->forward_result_slots +
                        group] &&
                bt->slots[group + 1] ==
                    bt->slots[bt->atomic_base + bt->forward_result_slots +
                        group + 1]) {
              bt->atomic_history[2 * bt->captures + group] = bt->slots[group];
              bt->atomic_history[2 * bt->captures + group + 1] =
                  bt->slots[group + 1];
              bt->atomic_history[4 * bt->captures + group + 1] = 1;
            }
          }
          for (size_t i = barrier + 1; i < bt->depth; i++) {
            const Frame * saved = &bt->stack[i];
            if (saved->kind != FRAME_CAPTURE || saved->pc < 2 ||
                saved->pc >= bt->captures) {
              continue;
            }
            size_t group = saved->pc & ~(size_t)1;
            if (bt->slots[bt->capture_open_slots + group] != 1 &&
                bt->slots[group] != GRX_NPOS &&
                bt->slots[group + 1] != GRX_NPOS &&
                bt->slots[group] <= bt->slots[group + 1]) {
              for (size_t part = group; part <= group + 1; part++) {
                bt->atomic_history[bt->captures + part] = bt->slots[part];
                bt->atomic_history[2 * bt->captures + part] = bt->slots[part];
              }
              bt->atomic_history[4 * bt->captures + group + 1] = 1;
            }
          }
        }
        if (bt->atomic_context) {
          AtomicContext * context = bt->atomic_context;
          for (size_t group = 2; group + 1 < bt->captures; group += 2) {
            int body_owned = bt->slots[bt->capture_open_slots + group + 1] !=
                context->caller[3 * bt->captures + group + 1];
            context->caller[3 * bt->captures + group] =
                (context->caller[3 * bt->captures + group] &
                    (2 | ATOMIC_CAPTURE_BEFORE_START |
                        ATOMIC_CAPTURE_MARKED_REPORT)) | body_owned;
            if (body_owned && bt->atomic_history) {
              bt->atomic_history[4 * bt->captures + group + 1] = 1;
            }
            if (body_owned && bt->atomic_history &&
                bt->slots[group] == GRX_NPOS) {
              for (size_t i = group; i <= group + 1; i++) {
                bt->atomic_history[bt->captures + i] = GRX_NPOS;
                bt->atomic_history[2 * bt->captures + i] = GRX_NPOS;
              }
            }
            if (!body_owned) {
              for (size_t i = group; i <= group + 1; i++) {
                // Keep the body result as the context read by later bodies,
                // while its caller retains ownership of reported captures.
                size_t body_value = bt->slots[i];
                bt->slots[i] = context->caller[i];
                context->caller[i] = body_value;
                if (bt->shadow) {
                  body_value = bt->slots[bt->shadow + i];
                  bt->slots[bt->shadow + i] = context->caller[bt->captures + i];
                  context->caller[bt->captures + i] = body_value;
                }
              }
            }
            else {
              context->caller[group] = bt->slots[group];
              context->caller[group + 1] = bt->slots[group + 1];
              if (bt->shadow) {
                context->caller[bt->captures + group] =
                    bt->slots[bt->shadow + group];
                context->caller[bt->captures + group + 1] =
                    bt->slots[bt->shadow + group + 1];
              }
            }
          }
        }
        for (read = barrier + 1, write = barrier; read < bt->depth; read++) {
          // Both resume kinds. FRAME_ALTERNATIVE is a resume point that
          // `(*THEN)` can name, and for one revision this line kept them -
          // which left `\R` able to give back the LF of a CR LF pair, the
          // one thing pcre2pattern says its `(?>` is there to prevent.
          if ((inst->flags & GRX_INST_LOOK_PERSIST_CAPTURES)
              && bt->stack[read].kind == FRAME_CAPTURE) {
            continue;
          }
          if (bt->stack[read].kind != FRAME_RESUME
              && bt->stack[read].kind != FRAME_ALTERNATIVE) {
            bt->stack[write] = bt->stack[read];
            if (bt->stack[write].kind == FRAME_PENDING) {
              bt->stack[write].pending->floor = write + 1;
            }
            write++;
          }
        }
        bt->depth = write;
        if (discovering) {
          if (!atomic_arrival(bt, arrival_start, position)) {
            return 0;
          }
          atomic_context_leave(bt);
          ok = 0;
          break;
        }
        if (bt->atomic_base) {
          if (required &&
              !apply_capture_owner(bt, bt->slots[bt->behind_owner_slot])) {
            return 0;
          }
          if (bt->atomic_context && bt->atomic_history) {
            AtomicContext * context = bt->atomic_context;
            for (size_t group = 2; group + 1 < bt->captures; group += 2) {
              if (!context->reporting &&
                  !(context->caller[3 * bt->captures + group] & 1) &&
                  (captured_result ||
                      bt->atomic_history[bt->captures + group] != GRX_NPOS)) {
                for (size_t i = group; i <= group + 1; i++) {
                  context->caller[i] = bt->atomic_history[bt->captures + i];
                  context->caller[bt->captures + i] =
                      bt->atomic_history[bt->captures + i];
                }
              }
            }
          }
          for (size_t i = 0; i < bt->captures; i++) {
            if (!save_slot(bt, bt->atomic_base + i)) {
              return 0;
            }
            size_t group = i & ~(size_t)1;
            int marked_prefix = bt->atomic_context && i >= 2 &&
                (bt->atomic_context->caller[3 * bt->captures + group] &
                    ATOMIC_CAPTURE_MARKED_REPORT);
            bt->slots[bt->atomic_base + i] = bt->atomic_context && !marked_prefix
                ? bt->atomic_context->caller[i]
                : bt->slots[i];
            if (bt->shadow) {
              if (!save_slot(bt, bt->atomic_base + bt->shadow + i)) {
                return 0;
              }
              bt->slots[bt->atomic_base + bt->shadow + i] = bt->atomic_context && !marked_prefix
                  ? bt->atomic_context->caller[bt->captures + i]
                  : bt->slots[bt->shadow + i];
            }
          }
          if (bt->atomic_context) {
            for (size_t i = 0; i < bt->captures; i++) {
              if (!save_slot(bt, bt->atomic_result_slots + i)) {
                return 0;
              }
              size_t group = i & ~(size_t)1;
              int owned = i < 2 || !captured_result ||
                  bt->atomic_context->caller[3 * bt->captures + group] ||
                  bt->slots[bt->forward_result_slots + group] != GRX_NPOS;
              size_t value = owned ? bt->slots[i] : GRX_NPOS;
              if (!captured_result && i >= 2 && bt->atomic_history) {
                size_t frontier = bt->atomic_history[6 * bt->captures + group];
                if (reads) {
                  value = bt->atomic_history[bt->captures + i];
                }
                else if (frontier != GRX_NPOS &&
                    (frontier < position || bt->alternative_epoch)) {
                  value = bt->atomic_history[5 * bt->captures + i];
                }
              }
              bt->slots[bt->atomic_result_slots + i] = value;
            }
            if (!save_slot(bt, bt->atomic_result_present)) {
              return 0;
            }
            bt->slots[bt->atomic_result_present] = captured_result
                ? ATOMIC_REPORT_VALID | ATOMIC_REPORT_COMPLETE
                : bt->atomic_context->reporting
                ? (size_t)bt->atomic_context->reporting
                : ATOMIC_REPORT_VALID;
          }
          if (!save_slot(bt, bt->atomic_base + bt->behind_owner_slot)) {
            return 0;
          }
          bt->slots[bt->atomic_base + bt->behind_owner_slot] =
              bt->slots[bt->behind_owner_slot];
          if (!save_slot(bt, bt->atomic_base_present)) {
            return 0;
          }
          bt->slots[bt->atomic_base_present] =
              (required ? ATOMIC_BASE_REQUIRED : ATOMIC_BASE_CONTEXT) |
              (reads ? ATOMIC_BASE_READS : 0) |
              (shifted_context ? ATOMIC_BASE_SHIFTED : 0);
          if (!save_slot(bt, bt->atomic_input_owner_slot)) {
            return 0;
          }
          bt->slots[bt->atomic_input_owner_slot] = reads &&
                  bt->atomic_context &&
                  bt->atomic_context->input_owner == GRX_NPOS
              ? bt->attempt_start
              : bt->atomic_context ? bt->atomic_context->input_owner
                                   : GRX_NPOS;
          atomic_context_leave(bt);
        }
        if (bt->forward_scope_slot) {
          if (!save_slot(bt, bt->forward_scope_slot)) {
            return 0;
          }
          bt->slots[bt->forward_scope_slot] = 1;
        }
        if ((inst->policy & GRX_POLICY_COMPLETE_COMPOSING) &&
            (!bt->reference_end_slot ||
                bt->slots[bt->reference_end_slot] != position)) {
          uint32_t point = 0;
          size_t width = 0;
          while (read_forward(bt, position, &point, &width) &&
              grx_display_cell_width(point, 0) == 0) {
            if (!pending_step(bt))
              return 0;
            position += width;
          }
        }
        pc++;
        continue;
      }

      case GRX_OP_LOOK: {
        int repeat_later = 0;
        size_t repeat_activation = 0;
        size_t repeat_context = 0;
        if (inst->policy & GRX_POLICY_LOOK_REPEATED) {
          size_t channel = 0;
          size_t global = 0;
          size_t copy = 0;
          grx_program_look_repeat(bt->program, inst, &channel, &repeat_context,
              &global, &copy);
          size_t global_slot = bt->captures + global;
          size_t total = bt->slots[global_slot] == GRX_NPOS
              ? 0 : bt->slots[global_slot];
          if (!save_slot(bt, global_slot) || !advance_generation(bt, &total)) {
            return 0;
          }
          bt->slots[global_slot] = total;
          int behind = inst->mode == GRX_LOOK_BEHIND_POSITIVE ||
              inst->mode == GRX_LOOK_BEHIND_NEGATIVE;
          size_t slot = bt->captures + channel;
          repeat_activation = bt->slots[slot] == GRX_NPOS ? 0 : bt->slots[slot];
          repeat_later = repeat_activation != 0;
          if (behind && (repeat_context & GRX_LOOK_REPEAT_BOUNDED)) {
            repeat_later = (repeat_context & GRX_LOOK_REPEAT_FIRST) ||
                ((repeat_context & GRX_LOOK_REPEAT_LAST) &&
                    !(repeat_context & GRX_LOOK_REPEAT_EXIT_REPEAT));
          }
          if (behind && (repeat_context & GRX_LOOK_REPEAT_COPY_POLICY)) {
            repeat_later = ((repeat_context & GRX_LOOK_REPEAT_COPY_KEEP_END) ||
                ((repeat_context & GRX_LOOK_REPEAT_COPY_DYNAMIC) &&
                    repeat_activation != 0) ||
                ((repeat_context & GRX_LOOK_REPEAT_COPY_COMPARE) &&
                    total - 1 >= copy)) &&
                !((repeat_context & GRX_LOOK_REPEAT_LAST) &&
                    (repeat_context & GRX_LOOK_REPEAT_EXIT_REPEAT));
          }
          if (!save_slot(bt, slot)) {
            return 0;
          }
          size_t next_activation = repeat_activation;
          if (!advance_generation(bt, &next_activation)) {
            return 0;
          }
          bt->slots[slot] = next_activation;
        }
        if (bt->behind_owner_slot &&
            (inst->mode == GRX_LOOK_BEHIND_POSITIVE ||
                inst->mode == GRX_LOOK_BEHIND_NEGATIVE)) {
          if (!save_slot(bt, bt->behind_owner_slot)) {
            return 0;
          }
          size_t owner = bt->slots[bt->behind_owner_slot];
          if (owner != GRX_NPOS && owner >= GRX_NPOS - 2) {
            bt->failure = GRX_ERR_LIMIT;
            bt->failure_diag = GRX_DIAG_LIMIT_STEPS;
            return 0;
          }
          bt->slots[bt->behind_owner_slot] = owner == GRX_NPOS ? 0 : owner + 1;
        }
        if (inst->policy &
            (GRX_POLICY_LOOK_DELAYED | GRX_POLICY_LOOK_FRONTIER)) {
          if (!advance_generation(bt, &bt->pending_sequence)) {
            return 0;
          }
          PendingLook * deferred =
              (PendingLook *)capture_alloc(bt, GRX_PENDING_WORDS);
          if (!deferred) {
            return 0;
          }
          *deferred = (PendingLook){
              .previous = bt->pending,
              .sequence = bt->pending_sequence - 1,
              .state = GRX_NPOS,
              .repeat_later = repeat_later,
              .repeat_context = repeat_context,
              .repeat_activation = repeat_activation,
              .pc = pc,
              .position = position,
              .owner = bt->capture_owner_depth
                  ? bt->active_capture_owner
                  : (bt->behind_owner_slot ? bt->slots[bt->behind_owner_slot]
                                           : GRX_NPOS),
          };
          if (bt->forwarded_slots) {
            deferred->entry = capture_alloc(bt, bt->captures);
            if (!deferred->entry) {
              pending_destroy(bt, deferred);
              return 0;
            }
            memcpy(deferred->entry, bt->slots, bt->captures * sizeof(size_t));
          }
          if (bt->atomic_base &&
              bt->slots[bt->atomic_base_present] != GRX_NPOS) {
            deferred->base = capture_alloc(bt, bt->atomic_base);
            if (!deferred->base) {
              pending_destroy(bt, deferred);
              return 0;
            }
            memcpy(deferred->base, bt->slots + bt->atomic_base,
                bt->atomic_base * sizeof(size_t));
          }
          if (inst->mode == GRX_LOOK_AHEAD_POSITIVE &&
              (inst->policy & GRX_POLICY_LOOK_FRONTIER) &&
              (inst->policy & GRX_POLICY_LOOK_HAS_CAPTURES)) {
            deferred->result = capture_alloc(bt, bt->captures);
            if (!deferred->result) {
              pending_destroy(bt, deferred);
              return 0;
            }
          }
          if (!push(bt, FRAME_PENDING, 0, 0)) {
            pending_destroy(bt, deferred);
            return 0;
          }
          bt->stack[bt->depth - 1].pending = deferred;
          deferred->floor = bt->depth;
          bt->pending = deferred;
          deferred->active_previous = bt->ready;
          if (bt->ready) {
            bt->ready->active_next = deferred;
          }
          bt->ready = deferred;
          deferred->linked = 1;
          if (inst->policy & GRX_POLICY_LOOK_FRONTIER) {
            bt->frontier_count++;
          }
          pc = inst->y;
          continue;
        }
        if (!evaluate_look(bt, pc, position, &pc, repeat_later, NULL, 0,
                has_ahead_result(bt, bt->pending), GRX_NPOS, NULL,
                repeat_context)) {
          if (bt->failure != GRX_OK || bt->verb_stop != VERB_NONE) {
            return 0;
          }
          ok = 0;
          break;
        }
        if ((inst->mode == GRX_LOOK_BEHIND_POSITIVE ||
                inst->mode == GRX_LOOK_BEHIND_NEGATIVE ||
                ((inst->mode == GRX_LOOK_AHEAD_POSITIVE ||
                     inst->mode == GRX_LOOK_AHEAD_NEGATIVE) &&
                    (inst->flags & GRX_INST_LOOK_REQUIRED) &&
                    has_ahead_result(bt, bt->pending))) &&
            !restore_ahead_results(bt, bt->pending)) {
          return 0;
        }
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
        const uint32_t * groups =
            grx_program_scan_list(bt->program, inst->x, &count);
        if (!groups) {
          bt->failure = GRX_ERR_INVALID;
          return 0;
        }

        size_t from = GRX_NPOS;
        size_t to = GRX_NPOS;
        for (size_t i = 0; i < count; i++) {
          size_t first = (size_t)groups[i] * 2;
          if (first + 1 < bt->captures && bt->slots[first] != GRX_NPOS &&
              bt->slots[first + 1] != GRX_NPOS) {
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

        size_t * before = capture_alloc(bt, bt->slot_count);
        if (!before) {
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
          capture_free(bt, before, bt->slot_count);
          return 0;
        }
        if (verb_escapes_assertion(bt, 0)) {
          restore_spans(bt, before);
          capture_free(bt, before, bt->slot_count);
          return 0;
        }

        if (!body_matched) {
          restore_spans(bt, before);
          capture_free(bt, before, bt->slot_count);
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
            capture_free(bt, before, bt->slot_count);
            return 0;
          }
          bt->slots[i] = current;
        }
        capture_free(bt, before, bt->slot_count);

        pc = inst->y;
        continue;
      }

      case GRX_OP_MATCH:
        if (bt->discovery_depth == bt->run_depth) {
          ok = 0;
          break;
        }
        if (!check_pending(bt, 0, 0)) {
          if (bt->failure != GRX_OK || bt->verb_stop != VERB_NONE) {
            return 0;
          }
          ok = 0;
          break;
        }
        if (bt->request->scalar_query &&
            bt->run_depth == bt->request->scalar_depth) {
          if (bt->request->scalar_cutoff &&
              position < *bt->request->scalar_cutoff)
            *bt->request->scalar_cutoff = position;
          ok = 0;
          break;
        }
        if (toplevel && bt->slots[1] == GRX_NPOS) {
          if (!save_slot(bt, 1)) {
            return 0;
          }
          bt->slots[1] = position;
        }
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
        bt->slots[0] = reported_position;
        if (bt->capture_branch_slots) {
          for (size_t group = 2; group + 1 < bt->captures; group += 2) {
            if (!save_slot(bt, bt->capture_branch_slots + group + 1)) {
              return 0;
            }
            bt->slots[bt->capture_branch_slots + group + 1] =
                bt->slots[group + 1] != GRX_NPOS &&
                    bt->slots[bt->capture_open_slots + group] != 1
                ? 1 : GRX_NPOS;
          }
        }
        pc++;
        continue;

      case GRX_OP_KEEP_END:
        if (inst->policy & GRX_POLICY_END_CONTEXT_ONLY) {
          pc++;
          continue;
        }
        // An explicit end commits the ordinary nullable caller closure.
        // An abandoned alternative and a complete numbered result retain
        // their own reporting ownership instead of reviving that closure.
        if (bt->atomic_base && !bt->alternative_epoch &&
            bt->slots[bt->atomic_base_present] == ATOMIC_BASE_CONTEXT &&
            bt->slots[bt->atomic_result_present] == ATOMIC_REPORT_VALID) {
          for (size_t group = 2; group + 1 < bt->captures; group += 2) {
            if (bt->slots[group] != GRX_NPOS ||
                bt->slots[bt->atomic_base + group] == GRX_NPOS) {
              continue;
            }
            for (size_t part = group; part <= group + 1; part++) {
              if (!save_slot(bt, part)) {
                return 0;
              }
              bt->slots[part] = bt->slots[bt->atomic_base + part];
              if (bt->shadow) {
                if (!save_slot(bt, bt->shadow + part)) {
                  return 0;
                }
                bt->slots[bt->shadow + part] =
                    bt->slots[bt->atomic_base + bt->shadow + part];
              }
            }
          }
        }
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
          bt->slots[1] = reported_position;
          // An explicit end supersedes an assertion's provisional end.
          // Undo both writes when this path is abandoned.
          if (bt->pending_end_slot
              && bt->slots[bt->pending_end_slot] != GRX_NPOS) {
            if (!save_slot(bt, bt->pending_end_slot)) {
              return 0;
            }
            bt->slots[bt->pending_end_slot] = GRX_NPOS;
          }
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

            if (inst->policy & GRX_POLICY_ACCEPT_OPEN) {
              size_t barrier = bt->depth;
              for (size_t i = bt->depth; i > floor; i--) {
                if (bt->stack[i - 1].kind == FRAME_ATOMIC) {
                  barrier = i - 1;
                  break;
                }
              }
              if (barrier < bt->depth) {
                size_t end = bt->depth;
                for (size_t i = barrier + 1; i < end; i++) {
                  const Frame * frame = &bt->stack[i];
                  size_t slot = frame->pc;
                  if (frame->kind == FRAME_CAPTURE && slot >= 2 &&
                      slot < bt->captures && !(slot & 1u) &&
                      bt->slots[slot + 1] == GRX_NPOS) {
                    if (!save_slot(bt, slot)) {
                      return 0;
                    }
                    bt->slots[slot] = GRX_NPOS;
                  }
                }
                pc = grx_program_at(bt->program, bt->stack[barrier].pc)->x;
                continue;
              }
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
                if (i && (inst->policy & GRX_POLICY_ACCEPT_OPEN)) {
                  bt->slots[i] = GRX_NPOS;
                }
                else {
                  bt->slots[i + 1] = position;
                }
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
        if (bt->reference_end_slot &&
            bt->slots[bt->reference_end_slot] != GRX_NPOS) {
          if (!save_slot(bt, bt->reference_end_slot))
            return 0;
          bt->slots[bt->reference_end_slot] = GRX_NPOS;
        }
        for (PendingLook * next = bt->frontier_count ? bt->ready : NULL; next;
            next = next->active_previous) {
          if (!pending_step(bt)) {
            return 0;
          }
          const GRX_Inst * look = grx_program_at(bt->program, next->pc);
          if (look->policy & GRX_POLICY_LOOK_AFTER_ATOM) {
            if (next->state == GRX_NPOS && !pending_set_state(bt, next, 0)) {
              return 0;
            }
          }
        }
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

/** Validate every expanded slot range before any offset arithmetic or
 * allocation. */
static int backtrack_layout_fits(
    const GRX_Program * program, size_t capture_count, size_t * out_captures) {
  size_t max_words = SIZE_MAX / sizeof(size_t);
  size_t slot_limit = max_words < GRX_INDEX_NONE ? max_words : GRX_INDEX_NONE;
  if (capture_count >= slot_limit / 2) {
    return 0;
  }
  size_t captures = 2 * (capture_count + 1);
  size_t slots = captures;
  if (program->register_count > slot_limit - slots) {
    return 0;
  }
  slots += program->register_count;
  if (program->flags & GRX_PROGRAM_SHADOW_CAPTURES) {
    if (captures > slot_limit - slots) {
      return 0;
    }
    slots += captures;
  }
  size_t mirrored = 0;
  if (program->flags & GRX_PROGRAM_PROVISIONAL_LOOK_END) {
    // Six assertion-owner tags plus the physical reference endpoint.
    if (slot_limit - slots < 7 || captures > (slot_limit - slots - 7) / 5) {
      return 0;
    }
    slots += 5 * captures + 7;
    mirrored = slots;
    if (slots > (slot_limit - 1) / 2) {
      return 0;
    }
    slots = 2 * slots + 1;
  }
  // Assertion snapshots include the mirrored prefix; discovery snapshots
  // additionally carry all seven capture-history regions. Both sums must
  // fit a size_t allocation even when compile-time limits are unlimited.
  if (mirrored > max_words - slots ||
      ((program->flags & GRX_PROGRAM_ATOMIC_FRONTIER) &&
          captures > (max_words - slots) / 7)) {
    return 0;
  }
  *out_captures = captures;
  return 1;
}

/**
 * Feasibility of the selected lexical run under its effective policies.
 *
 * Run selection uses the original encoded lengths and barriers. Folding or
 * removing marks before selection would choose a different required run.
 * The comparison itself is on scalars: folded partners may have different
 * UTF-8 lengths, and ignored marks may occur on either side of a base.
 */
static int candidate_run_compare(Backtrack * bt, const char * literal,
    size_t length, size_t position, int ignore_marks) {
  uint32_t options = bt->program->candidate_options;
  size_t start = position;
  size_t bases = 0;
  size_t total_bases = 0;
  if (ignore_marks) {
    for (size_t i = 0; i < length;) {
      if (!pending_step(bt)) {
        return 0;
      }
      uint32_t point = 0;
      size_t width = grx_unicode_utf8_decode(literal + i, length - i, 0, &point);
      if (!width) {
        return 0;
      }
      i += width;
      total_bases += grx_display_cell_width(point, 0) != 0;
    }
  }
  for (size_t i = 0; i < length;) {
    if (!pending_step(bt)) {
      return 0;
    }
    uint32_t wanted = 0;
    size_t width = grx_unicode_utf8_decode(literal + i, length - i, 0, &wanted);
    if (!width) {
      return 0;
    }
    i += width;
    // Caseless feasibility accepts the first complete written cluster;
    // the ignored-mark fallback still compares the entire selected run.
    if (!ignore_marks && (options & GRX_OPT_CASELESS) && bases &&
        grx_display_cell_width(wanted, 0) != 0) {
      return 1;
    }
    if (ignore_marks && grx_display_cell_width(wanted, 0) == 0) {
      continue;
    }
    uint32_t found = 0;
    do {
      if (!pending_step(bt) ||
          !read_forward(bt, position, &found, &width)) {
        return 0;
      }
      position += width;
    } while (ignore_marks && bases &&
        grx_display_cell_width(found, 0) == 0);
    if ((options & GRX_OPT_CASELESS) &&
        (!ignore_marks || (!bases && total_bases > 1))) {
      wanted = grx_unicode_fold_simple(wanted);
      found = grx_unicode_fold_simple(found);
    }
    if (wanted != found) {
      return 0;
    }
    bases++;
    if (ignore_marks) {
      uint32_t next = 0;
      while (position < bt->window_end) {
        if (!pending_step(bt) ||
            !read_forward(bt, position, &next, &width)) {
          return 0;
        }
        if (grx_display_cell_width(next, 0) != 0) {
          break;
        }
        position += width;
      }
      if (position - start >= length) {
        return 1;
      }
    }
  }
  return 1;
}

/** Exact folding precedes the old mark-ignoring, suffix-caseful fallback. */
static int candidate_run_at(Backtrack * bt, const char * literal,
    size_t length, size_t position) {
  if (candidate_run_compare(bt, literal, length, position, 0)) {
    return 1;
  }
  return bt->failure == GRX_OK &&
      (bt->program->candidate_options & GRX_OPT_IGNORE_COMBINING) &&
      candidate_run_compare(bt, literal, length, position, 1);
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

  size_t capture_slots = 0;
  if (!backtrack_layout_fits(
          program, request->regex->capture_count, &capture_slots)) {
    if (request->out_diag) {
      *request->out_diag = GRX_DIAG_LIMIT_MATCH_MEMORY;
    }
    if (request->out_steps) {
      *request->out_steps = 0;
    }
    return GRX_ERR_LIMIT;
  }

  Backtrack bt = {
      .request = request,
      .program = program,
      .stack_is_scratch = request->scalar_query,
      .allocator = request->regex->allocator,
      .captures = capture_slots,
      .registers = program->register_count,
      .shadow = 0,
      .capture_context = NULL,
      .active_capture_owner = GRX_NPOS,
      .atomic_base = 0,
      .atomic_base_present = 0,
      .forward_scope_slot = 0,
      .end_origin_slot = 0,
      .pending_end_slot = 0,
      .slot_count = 0,
      .slots = NULL,
      .stack = NULL,
      .depth = 0,
      .capacity = 0,
      .steps = 0,
      .utf = (program->flags & GRX_PROGRAM_UTF) != 0,
      .wtf8 = (program->flags & GRX_PROGRAM_WTF8) != 0,
      .failure = GRX_OK,
      .failure_diag = GRX_DIAG_NONE,
      .longest = program->preference == GRX_PREFER_LEFTMOST_LONGEST,
      // Exact for a program the Pike VM could also run, and first-path for
      // one it could not: see the GRX_OP_MATCH case, which says what the
      // difference buys and what it would otherwise cost.
      .submatch = request->regex && request->regex->facts.is_regular
          ? program->submatch
          : GRX_SUBMATCH_FIRST_PATH,
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

  if (program->candidate_literal_length) {
    size_t length = program->candidate_literal_length;
    const char * literal = GRX_ARENA_AT(
        const char, &program->callout_strings, program->candidate_literal);
    int found = 0;
    if (program->candidate_options) {
      for (size_t at = request->start; !found;) {
        found = candidate_run_at(&bt, literal, length, at);
        if (bt.failure != GRX_OK) {
          if (request->out_diag) {
            *request->out_diag = bt.failure_diag;
          }
          if (request->out_steps) {
            *request->out_steps = bt.steps;
          }
          return bt.failure;
        }
        uint32_t point = 0;
        size_t width = 0;
        if (!read_forward(&bt, at, &point, &width)) {
          break;
        }
        at += width;
      }
    }
    else if (length <= request->length - request->start) {
      size_t last = request->length - length;
      for (size_t at = request->start; at <= last && !found; at++) {
        found = 1;
        for (size_t i = 0; i < length; i++) {
          if (!pending_step(&bt)) {
            if (request->out_diag) {
              *request->out_diag = bt.failure_diag;
            }
            if (request->out_steps) {
              *request->out_steps = bt.steps;
            }
            return bt.failure;
          }
          if (request->subject[at + i] != literal[i]) {
            found = 0;
            break;
          }
        }
      }
    }
    if (!found) {
      if (request->out_steps) {
        *request->out_steps = bt.steps;
      }
      return GRX_OK;
    }
  }

  // The shadow spans sit after the registers, and only for a program that
  // has a backreference to read them - see GRX_PROGRAM_SHADOW_CAPTURES.
  bt.slot_count = bt.captures + bt.registers;
  if (program->flags & GRX_PROGRAM_SHADOW_CAPTURES) {
    bt.shadow = bt.slot_count;
    bt.slot_count += bt.captures;
  }

  if (program->flags & GRX_PROGRAM_PROVISIONAL_LOOK_END) {
    bt.pending_end_slot = bt.slot_count++;
    bt.forward_scope_slot = bt.slot_count++;
    bt.end_origin_slot = bt.slot_count++;
    bt.behind_owner_slot = bt.slot_count++;
    bt.forwarded_slots = bt.slot_count;
    bt.slot_count += bt.captures;
    bt.forward_result_slots = bt.slot_count;
    bt.slot_count += bt.captures;
    bt.reference_end_slot = bt.slot_count++;
    bt.capture_open_slots = bt.slot_count;
    bt.slot_count += bt.captures;
    bt.capture_branch_slots = bt.slot_count;
    bt.slot_count += bt.captures;
    bt.atomic_result_slots = bt.slot_count;
    bt.slot_count += bt.captures;
    bt.atomic_result_present = bt.slot_count++;
    bt.atomic_input_owner_slot = bt.slot_count++;
    bt.atomic_base = bt.slot_count;
    bt.slot_count += bt.atomic_base;
    bt.atomic_base_present = bt.slot_count++;
  }

  bt.slots = capture_alloc(&bt, bt.slot_count);
  if (!bt.slots) {
    if (request->out_diag) {
      *request->out_diag = bt.failure_diag;
    }
    return bt.failure;
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
  unsigned char * scalar_frontiers = NULL;
  ScalarPriority * scalar_priorities = NULL;
  size_t frontier_words = request->length / sizeof(size_t) + 1;
  GRX_ExecRequest active_request;
  if ((program->flags & GRX_PROGRAM_SCALAR_FRONTIER) &&
      !request->scalar_query) {
    scalar_frontiers = capture_alloc(&bt, frontier_words);
    if (!scalar_frontiers) {
      result = bt.failure;
    }
    else {
      memset(scalar_frontiers, 0, frontier_words * sizeof(size_t));
      active_request = *request;
      active_request.scalar_frontiers = scalar_frontiers;
      bt.request = &active_request;
      request = &active_request;
      for (size_t position = request->start; position < request->length;) {
        uint32_t point = 0;
        uint32_t next = 0;
        size_t width = 0;
        size_t next_width = 0;
        if (!pending_step(&bt) ||
            !read_forward(&bt, position, &point, &width)) {
          result = bt.failure;
          break;
        }
        int relevant =
            read_forward(&bt, position + width, &next, &next_width) &&
            grx_display_cell_width(next, 0) == 0;
        int literal = 0;
        for (size_t i = 0; relevant && !literal && i < program->insts.count;
            i++) {
          if (!pending_step(&bt)) {
            result = bt.failure;
            break;
          }
          const GRX_Inst * inst = grx_program_at(program, (uint32_t)i);
          literal = grx_program_scope(program, (uint32_t)i) == 0 &&
              (inst->policy & GRX_POLICY_SCALAR_LITERAL) &&
              ((inst->op == GRX_OP_CHAR && inst->x == point) ||
                  (inst->op == GRX_OP_CLASS && in_class(&bt, inst->x, point)));
        }
        if (result != GRX_OK)
          break;
        if (literal) {
          GRX_Limits remaining = *request->limits;
          if (remaining.max_steps) {
            if (bt.steps >= remaining.max_steps) {
              bt.failure_diag = GRX_DIAG_LIMIT_STEPS;
              result = GRX_ERR_LIMIT;
              break;
            }
            remaining.max_steps -= bt.steps;
          }
          if (remaining.max_match_memory) {
            if (bt.scratch_bytes >= remaining.max_match_memory) {
              bt.failure_diag = GRX_DIAG_LIMIT_MATCH_MEMORY;
              result = GRX_ERR_LIMIT;
              break;
            }
            remaining.max_match_memory -= bt.scratch_bytes;
          }
          GRX_ExecRequest probe = *request;
          size_t steps = 0;
          size_t cutoff = GRX_NPOS;
          void * priority_result = NULL;
          int found = 0;
          probe.scalar_query = 1;
          probe.scalar_depth = 1;
          probe.scalar_scope = 0;
          probe.scalar_priority_out = &priority_result;
          probe.scalar_priority_floor = 0;
          probe.scalar_target = position;
          probe.scalar_cutoff = &cutoff;
          probe.match = NULL;
          probe.memoize = 0;
          probe.limits = &remaining;
          probe.out_steps = &steps;
          probe.out_diag = &bt.failure_diag;
          result = grx_exec_backtrack(&probe, &found);
          bt.steps += steps;
          ScalarPriority * priority = priority_result;
          if (priority) {
            bt.scratch_bytes += priority->words * sizeof(size_t);
            priority->next = scalar_priorities;
            scalar_priorities = priority;
            active_request.scalar_priorities = scalar_priorities;
          }
          if (result != GRX_OK)
            break;
          scalar_frontiers[position] = (unsigned char)found;
        }
        position += width;
      }
    }
  }
  size_t start = request->start;
  for (; result == GRX_OK;) {
    if (request->scalar_query && request->scalar_cutoff &&
        start >= *request->scalar_cutoff)
      break;
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
    if (!request->anchored && !request->scalar_query &&
        !(program->flags &
            (GRX_PROGRAM_ATOMIC_FRONTIER | GRX_PROGRAM_CLUSTER_SEARCH))) {
      start = grx_exec_skip_to_first_byte(
          program, request->subject, request->length, start);
    }
    // Atomic BODY entry and the outer inherited baseline rotate separately:
    // one prior attempt for entry, two for the outer baseline. The immediate
    // successful atomic result lasts only this attempt. All four arrays are
    // search-call local, including when the caller reuses its Match object.
    if (bt.atomic_history) {
      memcpy(bt.atomic_history + 3 * bt.captures, bt.atomic_history,
          bt.captures * sizeof(size_t));
      memcpy(bt.atomic_history, bt.atomic_history + bt.captures,
          bt.captures * sizeof(size_t));
      for (size_t i = 2 * bt.captures; i < 3 * bt.captures; i++) {
        bt.atomic_history[i] = GRX_NPOS;
      }
      for (size_t i = 4 * bt.captures; i < 5 * bt.captures; i++) {
        bt.atomic_history[i] = 0;
      }
    }
    for (size_t i = 0; i < bt.slot_count; i++) {
      bt.slots[i] = GRX_NPOS;
    }
    bt.depth = 0;
    if (bt.atomic_history) {
      bt.slots[1] = bt.atomic_history[3 * bt.captures + 1];
      for (size_t i = 2; i < bt.captures; i++) {
        bt.slots[i] = bt.atomic_history[3 * bt.captures + i];
        if (bt.shadow) {
          bt.slots[bt.shadow + i] = bt.slots[i];
        }
      }
    }

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
    if (request->anchored || start >= request->length ||
        (request->scalar_query &&
            (start >= request->scalar_target ||
                (request->scalar_cutoff &&
                    start >= *request->scalar_cutoff)))) {
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
    size_t origin = start;
    start += width;
    int expose = request->scalar_frontiers && request->scalar_frontiers[origin];
    if (!expose && (program->flags & GRX_PROGRAM_CLUSTER_SEARCH) &&
        grx_display_cell_width(codepoint, 0) == 0) {
      for (size_t i = 0; !expose && i < program->insts.count; i++) {
        if (!pending_step(&bt)) {
          result = bt.failure;
          break;
        }
        const GRX_Inst * inst = grx_program_at(program, (uint32_t)i);
        expose = grx_program_scope(program, (uint32_t)i) == 0 &&
            (inst->policy & GRX_POLICY_SCALAR_LITERAL) &&
            ((inst->op == GRX_OP_CHAR && inst->x == codepoint) ||
                (inst->op == GRX_OP_CLASS &&
                    in_class(&bt, inst->x, codepoint)));
      }
    }
    if ((program->flags & GRX_PROGRAM_CLUSTER_SEARCH) && !expose) {
      while (read_forward(&bt, start, &codepoint, &width) &&
          grx_display_cell_width(codepoint, 0) == 0) {
        if (!pending_step(&bt)) {
          result = bt.failure;
          break;
        }
        start += width;
      }
    }
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
  while (bt.atomic_schedules) {
    AtomicSchedule * schedule = bt.atomic_schedules;
    bt.atomic_schedules = schedule->next;
    while (schedule->arrivals) {
      AtomicArrival * arrival = schedule->arrivals;
      schedule->arrivals = arrival->next;
      capture_free(&bt, arrival,
          (sizeof(*arrival) + sizeof(size_t) - 1) / sizeof(size_t) +
              bt.captures);
    }
    atomic_incoming_clear(&bt, &schedule->incoming);
    scalar_activation_free(&bt, schedule->frontiers);
    capture_free(&bt, schedule,
        (sizeof(*schedule) + sizeof(size_t) - 1) / sizeof(size_t));
  }
  while (scalar_priorities) {
    ScalarPriority * priority = scalar_priorities;
    scalar_priorities = priority->next;
    capture_free(&bt, priority, priority->words);
  }
  capture_free(&bt, scalar_frontiers, frontier_words);
  capture_free(&bt, bt.ahead_arrivals, 2 * program->insts.count);
  capture_free(&bt, bt.atomic_history, 7 * bt.captures);
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
