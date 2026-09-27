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
 * Analysis: what is knowable about a lowered pattern without running it.
 *
 * Computed once, at compile time, and the most important answer is the one a
 * caller wants *before* running anything: `is_regular` says whether the
 * linear-time engine can run this program, and so whether matching it against
 * hostile input is safe. A validator handed a pattern by an untrusted schema
 * can refuse what it cannot run in linear time rather than discovering the
 * cost at match time (documentation/design.md section 3.3).
 *
 * Lengths are in bytes throughout, computed per node from the widths the
 * characters it can match actually encode to, rather than as a character
 * count scaled by four at the end. The scaled version is a correct bound and
 * a useless one: it says `[a-z]{10}` may be forty bytes.
 *
 * Every fact here is conservative in the direction that cannot cause a wrong
 * answer. `min_length` may be too small and `max_length` too large; a program
 * may be reported as not regular when a cleverer analysis would see that it
 * is. The reverse of any of those would route a program to an engine that
 * mis-executes it, or let a caller skip a subject that would have matched.
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/compile.h>
#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <string.h>

#include "lower_internal.h"

/** What walking one subtree discovered. */
typedef struct {
  size_t min_length;  ///< Shortest match, in bytes.
  size_t max_length;  ///< Longest match, or GRX_NPOS when unbounded.
  int anchored_start; ///< Every match must begin where the search began.
  int anchored_end;   ///< Every match must end at the end of the subject.
  /**
   * The length could not be worked out from the tree at all.
   *
   * Not the same as unbounded. A backreference matches whatever its group
   * did, and that group usually has a length this walk can measure, which is
   * why pcre2test accepts `(a)(?<=\1)` and refuses `(a)(?<=\1+)`. The one
   * that cannot be measured is the reference whose target is being measured
   * already - `(a\2)(b\1)` - and a dialect that bounds its lookbehind
   * refuses that, because a bound nobody can compute is not a bound.
   */
  int unknown_length;
  /**
   * A reachable `(*ACCEPT)` inside this subtree.
   *
   * The verb ends the match where it fires, so everything after it in the
   * subtree is optional and the shortest match is nothing at all. Carried
   * rather than folded into `min_length` on the spot, because it is the
   * *enclosing* node whose minimum the verb collapses: `(?:a?(*ACCEPT))b`
   * matches the empty string, which is what pcre2test and perl both report,
   * and which reading the concatenation alone would deny.
   *
   * A lookaround stops it. pcre2pattern: a verb inside an assertion ends the
   * assertion, not the match it is part of - so the body's minimum is the
   * body's business and the assertion is still zero-width.
   */
  int accepts;
} Span;

/** The accumulating answer for the whole program. */
/** How many references deep the measuring of a group's length will go. */
#define GRX_ANALYSIS_MAX_REFERENCES 32

/**
 * How many group spans are remembered at a time.
 *
 * Direct-mapped by group number, because the alternative to remembering them
 * was measuring each one once per reference to it. `group_span()` walks a
 * group's body to answer "how long can what this captured be", and a body
 * containing references of its own walks those too - so with groups whose
 * bodies reference the group before them, the work doubles a level and a
 * 170-byte pattern took 6.7 seconds to compile. A 1,514-byte one found by the
 * fuzzer took 354 seconds and was then rejected as malformed; notes section
 * 14i has the curve.
 *
 * Direct-mapped rather than exact: a collision costs a recomputation and
 * nothing else, the table is a pure memo, and this keeps the whole thing on
 * the stack with no allocation to fail and no cleanup path to forget. What has
 * to be covered is the *chain* in play during one resolution, and
 * GRX_ANALYSIS_MAX_REFERENCES caps that at 32, so 64 slots hold a chain twice
 * over before two of its groups can collide.
 */
#define GRX_ANALYSIS_SPAN_CACHE 64

typedef struct {
  const GRX_IR * ir;
  /**
   * The same IR, when the walk is allowed to write its answers back.
   *
   * NULL for grx_ir_can_match_empty(), which asks about one subtree and must
   * not disturb what a full analysis recorded. Only the full pass fills in
   * the lookbehind spans, because only it is the pass compile.c runs before
   * codegen reads them.
   */
  GRX_IR * writable;
  GRX_Result failure;  ///< The first storage failure, or GRX_OK.
  int is_regular;
  int has_backreference;
  int has_lookaround;
  int has_recursion;
  int has_script_run;
  size_t max_lookbehind;
  size_t max_variable_lookbehind;
  /**
   * Non-zero while measuring a lookbehind body, so that a backreference
   * reports the width it *declares* rather than the width it may match.
   *
   * group_span() zeroes a reference's minimum because a reference to a
   * group that did not participate matches the empty string in ECMAScript
   * and fails in the Perl family - neither of which is "at least what the
   * group was". That is the right answer for the pattern's minimum match
   * length and the wrong one for "is this lookbehind one length", where
   * every reference would then read as variable.
   *
   * CPython settles it the same way: `(x)?(?<=\1a)` compiles there even
   * though group 1 may be unset, and `(x|yz)(?<=\1)` does not, because the
   * group itself is two widths. The question is about the group's spelling,
   * not about whether it took part.
   */
  size_t measuring_look_width;
  size_t depth;
  /**
   * The groups whose length is being measured, innermost last.
   *
   * A reference to one of these is a reference to something whose answer
   * depends on this answer, so it has none: `(a\2)(b\1)` is two groups each
   * of which is as long as the other.
   */
  uint32_t resolving[GRX_ANALYSIS_MAX_REFERENCES];
  size_t resolving_count;
  /**
   * Answers that depend on where the walk came from rather than on the IR.
   *
   * Bumped wherever a guard gives up: a reference to a group already being
   * resolved, a resolution deeper than GRX_ANALYSIS_MAX_REFERENCES, or a walk
   * deeper than GRX_ANALYSIS_MAX_DEPTH. Each returns "unknown" because of the
   * route taken to it, so a span computed with one of them underneath is not a
   * property of the group and must not be remembered. group_span() reads this
   * before and after walking a body and caches only when it did not move.
   */
  size_t context_hits;
  /** Group numbers in the cache, 0 for an empty slot. */
  uint32_t cache_group[GRX_ANALYSIS_SPAN_CACHE];
  /** Whether that entry was computed while measuring a lookbehind's width. */
  unsigned char cache_look[GRX_ANALYSIS_SPAN_CACHE];
  /** What walk() returned for that body, before group_span's own tidying. */
  Span cache_span[GRX_ANALYSIS_SPAN_CACHE];
  /**
   * The walk's own stack, one WalkFrame per node being measured.
   *
   * On the heap because a C stack runs out and this one grows. walk() used to
   * call itself - one 240-byte frame per level of nesting at -O2 - and
   * GRX_ANALYSIS_MAX_DEPTH stopped it at 512, which is 123 KB: half the 256 KB
   * stack testing.md section 12 runs every harness under, spent by the pass
   * that only measures. The cap was a frame count standing in for a byte
   * budget, the way GRX_BACKTRACK_MAX_C_DEPTH was before somebody measured the
   * matcher. It is unchanged in what it refuses; what changed is that reaching
   * it now costs bytes that grow rather than frames that run out.
   */
  GRX_Arena frames;
  /**
   * The span the frame that just finished came back with.
   *
   * A recursive walk returned its answer; a driver has to put it somewhere,
   * and one register is enough because a frame is resumed immediately after
   * its child pops. Read at the top of every resuming stage and nowhere else.
   */
  Span result;
} Analysis;

/** How deep the walk will go before it gives up rather than overflowing. */
#define GRX_ANALYSIS_MAX_DEPTH 512

/** Add two lengths, saturating at "unbounded". */
static size_t add_length(size_t a, size_t b) {
  if (a == GRX_NPOS || b == GRX_NPOS) {
    return GRX_NPOS;
  }
  if (a > GRX_NPOS - b) {
    return GRX_NPOS;
  }
  return a + b;
}

/** Multiply a length by a repeat count, saturating at "unbounded". */
static size_t scale_length(size_t length, uint32_t count) {
  // Zero repetitions of anything, and any number of repetitions of nothing,
  // are both nothing - including zero repetitions of an unbounded body.
  if (!count || !length) {
    return 0;
  }
  if (length == GRX_NPOS || length > GRX_NPOS / count) {
    return GRX_NPOS;
  }
  return length * count;
}

/** The UTF-8 length of one code point, in bytes. */
static size_t encoded_width(uint32_t codepoint) {
  if (codepoint < 0x80u) {
    return 1;
  }
  if (codepoint < 0x800u) {
    return 2;
  }
  if (codepoint < 0x10000u) {
    return 3;
  }
  return 4;
}

/**
 * The narrowest and widest a consuming node's one character can be.
 *
 * A class of ASCII letters is one byte; a class of emoji is four; `.` is one
 * to four. Computing it from the class rather than assuming four keeps
 * `max_length` useful: a caller sizing a buffer from it wants the real bound,
 * and "four times the code point count" is a bound that is right and almost
 * always far too large.
 */
static void codepoint_widths(const GRX_IR * ir, const GRX_IRNode * node,
    size_t * out_low, size_t * out_high) {
  *out_low = 1;
  *out_high = 4;

  if (node->kind == GRX_IR_CHAR) {
    *out_low = encoded_width(node->a);
    *out_high = *out_low;
    return;
  }
  if (node->kind != GRX_IR_CLASS) {
    return; // ANY: any code point at all.
  }

  size_t count = 0;
  const GRX_CharRange * ranges
      = grx_class_table_get(&ir->classes, node->a, &count);
  if (!ranges || !count) {
    return;
  }

  // The ranges are sorted, so the first range's low is the smallest code
  // point and the last range's high is the largest.
  *out_low = encoded_width(ranges[0].low);
  *out_high = encoded_width(ranges[count - 1].high);
}

static Span walk(Analysis * analysis, uint32_t node_index);

/**
 * Fold one more alternative into a running span.
 *
 * Written out because a conditional's branches are alternatives too and
 * are not all of its children, so the loop below is not the only caller.
 */
static Span combine_alternative(Span span, Span part) {
  if (part.min_length < span.min_length) {
    span.min_length = part.min_length;
  }
  if (span.max_length != GRX_NPOS
      && (part.max_length == GRX_NPOS || part.max_length > span.max_length)) {
    span.max_length = part.max_length;
  }
  // A property holds for the alternation only if it holds for every branch:
  // one branch that can start anywhere means the whole thing can.
  span.anchored_start = span.anchored_start && part.anchored_start;
  span.anchored_end = span.anchored_end && part.anchored_end;
  span.unknown_length = span.unknown_length || part.unknown_length;
  span.accepts = span.accepts || part.accepts;
  return span;
}

/**
 * One node being measured, and how far through measuring it we are.
 *
 * walk() is a loop over a stack of these rather than a function that calls
 * itself, so a node's locals live here. What used to be a `Span part` in
 * walk_concat()'s frame and a `Span body` in three of walk()'s own arms is
 * `span` plus Analysis::result; what used to be the `child` of two loops is
 * `child` and `next`.
 *
 * `next` is read at the same moment the recursion read it - from the child's
 * node before that child is measured, not after - because that is what the two
 * loops did and a walk may write a lookbehind span onto a node it passes.
 */
typedef struct {
  uint32_t node;  ///< The IR node this frame measures.
  uint32_t child; ///< Which child a list is up to, or GRX_INDEX_NONE.
  uint32_t next;  ///< That child's sibling, taken before it was measured.
  uint16_t stage; ///< How far it has got; the WALK_* constants below.
  uint8_t seen;   ///< Alternatives folded so far, saturating at two.
  uint8_t behind; ///< LOOK: whether it raised `measuring_look_width`.
  Span span;      ///< The answer being accumulated.
} WalkFrame;

/**
 * The stages a frame passes through.
 *
 * One per point where the recursion used to return into the middle of
 * something. A kind with no child at all is measured entirely in WALK_ENTER
 * and never reaches another stage, which is most of them.
 */
#define WALK_ENTER 0        ///< The dispatch: what kind is this node.
#define WALK_CONCAT_HEAD 1  ///< About to measure one part of a concatenation.
#define WALK_CONCAT_PART 2  ///< A part came back; add it on.
#define WALK_ALT_HEAD 3     ///< About to measure one alternative.
#define WALK_ALT_PART 4     ///< An alternative came back; fold it in.
#define WALK_REPEAT_BODY 5  ///< A repeat's body came back; scale it.
#define WALK_GROUP_BODY 6   ///< A capture, atomic group or script run's body.
#define WALK_LOOK_BODY 7    ///< A lookaround's body came back; record it.
#define WALK_SCAN_BODY 8    ///< A scan's body came back; discard it.
#define WALK_COND_TEST 9    ///< An assertion condition came back; discard it.
#define WALK_COND_HEAD 10   ///< About to measure one branch of a conditional.
#define WALK_COND_PART 11   ///< A branch came back; fold it in.

/** What a stage asks the driver to do next. */
typedef enum {
  WALK_POP,     ///< This node is measured; `span` is the answer.
  WALK_DESCEND, ///< Measure the child named, then resume this frame.
  WALK_AGAIN,   ///< Call this frame again; its stage has moved.
  /**
   * Measure the group named, then take that as the answer and pop.
   *
   * group_span() rather than a descent, because what it does is not a walk of
   * a child: it memoises, it guards against a group that is already being
   * measured, and its own recursion is bounded by
   * GRX_ANALYSIS_MAX_REFERENCES at thirty-two rather than by the tree.
   *
   * It is asked for by the driver rather than called from walk_step() because
   * it re-enters walk(), which pushes frames and may move the arena - so a
   * WalkFrame pointer does not survive it. Writing through one anyway is what
   * the first draft of this did, and ASan called it a heap-use-after-free
   * while the release build was lucky enough to pass.
   */
  WALK_GROUP
} WalkAction;

/**
 * How long the text a group captured can be.
 *
 * What a backreference or a subroutine call matches is a property of the
 * group it names, not of the subject, and pcre2test measures it: `(a)(?<=\1)`
 * compiles and `(a)(?<=\1+)` does not, which is the difference between a
 * bound of one and no bound at all. A group already being measured has no
 * answer this can give - `(a\2)(b\1)` is two groups each as long as the
 * other - and neither has `(?0)`, which is the whole pattern.
 *
 * The minimum comes back as zero whatever the group's is: a reference to a
 * group that did not participate matches the empty string in ECMAScript and
 * fails in the Perl family, and neither is "at least what the group was".
 */
static Span group_span(Analysis * analysis, uint32_t group) {
  Span unknown = {0, GRX_NPOS, 0, 0, 1, 0};
  if (!group) {
    return unknown;
  }
  if (analysis->resolving_count >= GRX_ANALYSIS_MAX_REFERENCES) {
    analysis->context_hits++;
    return unknown;
  }
  for (size_t i = 0; i < analysis->resolving_count; i++) {
    if (analysis->resolving[i] == group) {
      analysis->context_hits++;
      return unknown;
    }
  }

  // The memo. A hit skips the search below and the walk under it, which is the
  // whole difference between a group measured once and a group measured once
  // per reference to it.
  //
  // `measuring_look_width` is part of the key rather than of the entry,
  // because a body that contains references of its own has *their* minimums
  // zeroed or kept by the flag as it stood when they were measured. The flag
  // is not read by walk() itself - only here - so the two settings are two
  // different answers for the same group and cannot share a slot.
  unsigned char look = analysis->measuring_look_width ? 1u : 0u;
  size_t slot = group % GRX_ANALYSIS_SPAN_CACHE;
  uint32_t body = GRX_INDEX_NONE;
  Span span;
  if (analysis->cache_group[slot] == group && analysis->cache_look[slot] == look) {
    span = analysis->cache_span[slot];
  }
  else {
    size_t found = 0;
    int branch_reset = 0;
    for (size_t i = 0; i < analysis->ir->nodes.count; i++) {
      const GRX_IRNode * node = grx_ir_node(analysis->ir, (uint32_t)i);
      if (node && node->kind == GRX_IR_CAPTURE && node->a == group) {
        if (!found) {
          body = node->first_child;
        }
        branch_reset = branch_reset || (node->flags & GRX_IR_BRANCH_RESET);
        found++;
      }
    }
    // A group written inside `(?|...)` shares its number with the other
    // branches', so which of them a reference means is not decided until the
    // match runs and there is no length here to give. pcre2test refuses
    // `(?|([ab]))...(?<=\1)z` even with one branch, and takes the same
    // lookbehind over a group written outside one.
    if (body == GRX_INDEX_NONE || found > 1 || branch_reset) {
      return unknown;
    }

    size_t before = analysis->context_hits;
    analysis->resolving[analysis->resolving_count++] = group;
    span = walk(analysis, body);
    analysis->resolving_count--;

    // Remembered only if nothing underneath gave up because of where the walk
    // came from. A reference to a group already on `resolving` returns
    // "unknown" about *this* route, not about the group, and a walk that hit
    // GRX_ANALYSIS_MAX_DEPTH says only that it stopped - caching either would
    // answer a later query with a result that was never about it.
    if (analysis->context_hits == before) {
      analysis->cache_group[slot] = group;
      analysis->cache_look[slot] = look;
      analysis->cache_span[slot] = span;
    }
  }

  if (!analysis->measuring_look_width) {
    span.min_length = 0;
  }
  span.anchored_start = 0;
  span.anchored_end = 0;
  return span;
}

/**
 * Put one node's frame on the walk's stack, or answer for it without one.
 *
 * The two refusals the recursion made on the way in, in the order it made
 * them: a node index that resolves to nothing is measured as zero-width, and a
 * node deeper than GRX_ANALYSIS_MAX_DEPTH is not measured at all. Both write
 * the answer to Analysis::result and push nothing, which is what a caller
 * resuming from a descent reads either way.
 *
 * The third refusal is new and is the price of the heap: an arena that cannot
 * grow answers the way the depth cap does - nothing is known - and records
 * GRX_ERR_OOM, so the full pass reports it and the two query entry points
 * degrade conservatively rather than lying.
 */
/**
 * Prepare an Analysis, and the stack its walk will use.
 *
 * Written out because there were three of these, spelled as three
 * near-identical designated initialisers that had already drifted - two of
 * them left `has_script_run` and `measuring_look_width` to the zero fill
 * while the third named them. A pass whose defaults are stated in three
 * places has three chances to disagree about what conservative means.
 */
static void analysis_init(
    Analysis * analysis, const GRX_IR * ir, GRX_IR * writable) {
  memset(analysis, 0, sizeof *analysis);
  analysis->ir = ir;
  analysis->writable = writable;
  analysis->failure = GRX_OK;
  analysis->is_regular = 1;
  grx_arena_init(
      &analysis->frames, ir->allocator, sizeof(WalkFrame), 0, GRX_DIAG_NONE);
}

/** Release what analysis_init() took. */
static void analysis_done(Analysis * analysis) {
  grx_arena_clear(&analysis->frames);
}

static int walk_push(Analysis * analysis, uint32_t node_index) {
  const GRX_IRNode * node = grx_ir_node(analysis->ir, node_index);
  if (!node) {
    analysis->result = (Span) {0, 0, 0, 0, 0, 0};
    return 0;
  }
  if (analysis->depth >= GRX_ANALYSIS_MAX_DEPTH) {
    // Deeper than the parser's own cap allows, so this is unreachable for a
    // pattern that came through it. A tree built by hand can still get here,
    // and the honest answer for a subtree that was not examined is "nothing
    // is known".
    analysis->is_regular = 0;
    // Context-dependent: this says the walk stopped, not what the subtree
    // is, so group_span() must not remember a span computed above it.
    analysis->context_hits++;
    analysis->result = (Span) {0, GRX_NPOS, 0, 0, 0, 0};
    return 0;
  }

  WalkFrame frame = {.node = node_index, .child = GRX_INDEX_NONE,
    .next = GRX_INDEX_NONE, .stage = WALK_ENTER, .seen = 0, .behind = 0,
    .span = {0, GRX_NPOS, 0, 0, 0, 0}};
  if (grx_arena_append(&analysis->frames, &frame, NULL) != GRX_OK) {
    if (analysis->failure == GRX_OK) {
      analysis->failure = GRX_ERR_OOM;
    }
    analysis->is_regular = 0;
    analysis->context_hits++;
    analysis->result = (Span) {0, GRX_NPOS, 0, 0, 0, 0};
    return 0;
  }
  analysis->depth++;
  return 1;
}

/**
 * Take one node's measurement one step further, and say what to do next.
 *
 * The switch walk() was. Every arm that had no child finishes in WALK_ENTER,
 * which is why most of them are unchanged; the six that did recurse hand the
 * child back to the driver and pick up where they left off.
 *
 * **`frame` must not outlive a push.** Nothing called from here may reach
 * walk(), because a push can move the arena and every arm writes through this
 * pointer. That is why measuring a group is WALK_GROUP rather than a call to
 * group_span() - see WalkAction.
 */
static WalkAction walk_step(
    Analysis * analysis, WalkFrame * frame, uint32_t * out_child) {
  const GRX_IRNode * node = grx_ir_node(analysis->ir, frame->node);
  if (!node) {
    frame->span = (Span) {0, 0, 0, 0, 0, 0};
    return WALK_POP;
  }

  switch (frame->stage) {
    case WALK_CONCAT_HEAD: {
      if (frame->child == GRX_INDEX_NONE) {
        return WALK_POP;
      }
      const GRX_IRNode * child_node = grx_ir_node(analysis->ir, frame->child);
      if (!child_node) {
        return WALK_POP; // The loop's `break`: what was gathered stands.
      }
      frame->next = child_node->next_sibling;
      frame->stage = WALK_CONCAT_PART;
      *out_child = frame->child;
      return WALK_DESCEND;
    }

    case WALK_CONCAT_PART: {
      Span part = analysis->result;
      Span span = frame->span;

      // Anchored at the start if any part *before the first one that can
      // consume* is anchored: `^a` is anchored and so is `\b^a`, because a
      // zero-width part in front of the `^` does not move where the match
      // begins. `a^` is not.
      if (!span.anchored_start && span.max_length == 0) {
        span.anchored_start = part.anchored_start;
      }
      // Anchored at the end if the last part is, or if an anchored part is
      // followed only by zero-width ones: `a$` and `a$\b` both are.
      span.anchored_end = part.anchored_end
          || (span.anchored_end && part.min_length == 0
              && part.max_length == 0);

      span.min_length = add_length(span.min_length, part.min_length);
      span.max_length = add_length(span.max_length, part.max_length);
      span.unknown_length = span.unknown_length || part.unknown_length;
      span.accepts = span.accepts || part.accepts;
      frame->span = span;

      frame->child = frame->next;
      frame->stage = WALK_CONCAT_HEAD;
      return WALK_AGAIN;
    }

    case WALK_ALT_HEAD: {
      const GRX_IRNode * child_node = frame->child == GRX_INDEX_NONE
          ? NULL
          : grx_ir_node(analysis->ir, frame->child);
      if (!child_node) {
        if (!frame->seen) {
          frame->span = (Span) {0, 0, 0, 0, 0, 0};
        }
        return WALK_POP;
      }
      frame->next = child_node->next_sibling;
      frame->stage = WALK_ALT_PART;
      *out_child = frame->child;
      return WALK_DESCEND;
    }

    case WALK_ALT_PART:
      frame->span = combine_alternative(frame->span, analysis->result);
      if (frame->seen < 2) {
        frame->seen++;
      }
      frame->child = frame->next;
      frame->stage = WALK_ALT_HEAD;
      return WALK_AGAIN;

    case WALK_REPEAT_BODY: {
      Span body = analysis->result;
      Span span = {0, GRX_NPOS, 0, 0, 0, 0};
      span.min_length = scale_length(body.min_length, node->min);
      span.max_length = node->max == GRX_REPEAT_INF
          ? (body.max_length ? GRX_NPOS : 0)
          : scale_length(body.max_length, node->max);
      // A repeat that must run at least once inherits its body's anchoring;
      // one that may run zero times does not, because zero iterations match
      // the empty string anywhere.
      span.anchored_start = node->min > 0 && body.anchored_start;
      span.anchored_end = node->min > 0 && body.anchored_end;
      span.unknown_length = body.unknown_length;
      span.accepts = body.accepts && node->max != 0;
      if (node->mode == GRX_REPEAT_POSSESSIVE) {
        analysis->is_regular = 0;
      }
      frame->span = span;
      return WALK_POP;
    }

    case WALK_GROUP_BODY:
      frame->span = analysis->result;
      return WALK_POP;

    case WALK_LOOK_BODY: {
      Span body = analysis->result;
      analysis->measuring_look_width -= frame->behind ? 1u : 0u;
      // GRX_NPOS is kept rather than skipped. An unbounded body - `(?<=a+)`
      // - used to leave this at zero, which reads as "no lookbehind" and is
      // the opposite of the truth: that is the one lookbehind that may need
      // the whole subject before the start. GRX_NPOS is what
      // GRX_Facts::max_length already means by unbounded, and because it is
      // SIZE_MAX no later body can lower it.
      if (node->mode == GRX_LOOK_BEHIND_POSITIVE
          || node->mode == GRX_LOOK_BEHIND_NEGATIVE
          || node->mode == GRX_LOOK_BEHIND_NON_ATOMIC) {
        if (body.max_length > analysis->max_lookbehind) {
          analysis->max_lookbehind = body.max_length;
        }
        // A body that matches more than one length is the one a dialect may
        // bound separately: PCRE2 takes `(?<=a{256})` and refuses
        // `(?<=\d{1,256})`, and the two differ only in this test. A body
        // whose length could not be worked out at all counts as exceeding
        // every finite bound, which is pcre2test's answer for
        // `(a\2)(b\1)(?<=\2)`: a bound nobody can compute is not one.
        if (body.unknown_length) {
          analysis->max_variable_lookbehind = GRX_NPOS;
        }
        else if (body.min_length != body.max_length
            && body.max_length > analysis->max_variable_lookbehind) {
          analysis->max_variable_lookbehind = body.max_length;
        }

        // What the forward model needs: the starts it may try. Recorded on
        // the node, because measuring a subtree is this pass and choosing a
        // model was lowering's, and codegen - which runs after both - is
        // where the two meet.
        if (analysis->writable && (node->flags & GRX_IR_LOOK_FORWARD)) {
          uint32_t offset = GRX_INDEX_NONE;
          // Vim's `\@123<=`: the match may start at most that many bytes
          // back, so the span the engine enumerates is the body's own
          // clipped to it. Both ends, because a body that cannot fit
          // inside the bound is one the assertion can never satisfy -
          // `\(ab\)\@1<=c` does not match "abc" in vim - and a minimum
          // above the maximum is not a span at all.
          size_t low_end = body.min_length;
          size_t high_end = body.max_length;
          if (node->b) {
            high_end = high_end > node->b ? node->b : high_end;
            low_end = low_end > high_end ? high_end : low_end;
          }
          GRX_Result stored = grx_ir_look_span_set(
              analysis->writable, &offset, low_end, high_end);
          if (stored != GRX_OK) {
            if (analysis->failure == GRX_OK) {
              analysis->failure = stored;
            }
          }
          else {
            GRX_IRNode * writable
                = grx_ir_node(analysis->writable, frame->node);
            if (writable) {
              writable->a = offset;
            }
          }
        }
      }
      frame->span = (Span) {0, 0, 0, 0, 0, 0};
      return WALK_POP;
    }

    case WALK_SCAN_BODY:
      frame->span = (Span) {0, 0, 0, 0, 0, 0};
      return WALK_POP;

    case WALK_COND_TEST:
      // The condition is discarded, being zero-width where it stands; it was
      // walked so that what it contains still reaches the facts.
      //
      // The branches, as alternatives. A conditional with no else-part can
      // take a zero-length path - `(?(?=a)b)` matches empty where `a` does
      // not follow - so the absent branch is an alternative of length zero
      // rather than no alternative at all. The old rewrite spelled that as
      // an explicit empty node and this says it directly.
      frame->span = (Span) {GRX_NPOS, 0, 1, 1, 0, 0};
      frame->seen = 0;
      frame->child = frame->next;
      frame->stage = WALK_COND_HEAD;
      return WALK_AGAIN;

    case WALK_COND_HEAD: {
      const GRX_IRNode * part = frame->child == GRX_INDEX_NONE
          ? NULL
          : grx_ir_node(analysis->ir, frame->child);
      if (!part) {
        if (frame->seen < 2) {
          frame->span = combine_alternative(
              frame->span, (Span) {0, 0, 1, 1, 0, 0});
        }
        if (!frame->seen) {
          frame->span = (Span) {0, 0, 0, 0, 0, 0};
        }
        return WALK_POP;
      }
      frame->next = part->next_sibling;
      frame->stage = WALK_COND_PART;
      *out_child = frame->child;
      return WALK_DESCEND;
    }

    case WALK_COND_PART:
      frame->span = combine_alternative(frame->span, analysis->result);
      if (frame->seen < 2) {
        frame->seen++;
      }
      frame->child = frame->next;
      frame->stage = WALK_COND_HEAD;
      return WALK_AGAIN;

    case WALK_ENTER:
    default:
      break;
  }

  switch (node->kind) {
    case GRX_IR_EMPTY:
      frame->span = (Span) {0, 0, 0, 0, 0, 0};
      return WALK_POP;

    case GRX_IR_CHAR:
    case GRX_IR_CLASS:
    case GRX_IR_ANY: {
      // In bytes, because that is what the facts promise and what a caller
      // sizing a buffer needs. Without UTF a step is a byte; with it, the
      // width of the *narrowest* and *widest* code point the node can match,
      // so `a` is one byte either way and `.` is one to four.
      size_t low = 1;
      size_t high = 1;
      if (analysis->ir->flags & GRX_PROGRAM_UTF) {
        codepoint_widths(analysis->ir, node, &low, &high);
      }
      frame->span = (Span) {low, high, 0, 0, 0, 0};
      return WALK_POP;
    }

    case GRX_IR_FOLD_RUN: {
      // The shortest and longest path through the graph, in bytes. Each edge
      // consumes one character from a class, so its cost is that class's
      // narrowest and widest encoding, and the answer is the usual shortest-
      // and longest-path walk done backwards from the end.
      //
      // No array for the per-position answers, because an edge spans at most
      // GRX_FULL_FOLD_MAX positions: a window of one more than that is
      // enough, and position `i` never overwrites one it still has to read.
      size_t shortest[GRX_FULL_FOLD_MAX + 1] = {0};
      size_t longest[GRX_FULL_FOLD_MAX + 1] = {0};
      size_t window = GRX_FULL_FOLD_MAX + 1;
      size_t n = node->b;
      shortest[n % window] = 0;
      longest[n % window] = 0;

      // The edges are ordered by the position they leave, so walking them
      // backwards visits the positions in the order this needs them.
      size_t cursor = grx_ir_fold_run_count(analysis->ir, node->a);
      for (size_t step = n; step > 0; step--) {
        size_t at = step - 1;
        size_t low = GRX_NPOS;
        size_t high = 0;
        while (cursor > 0) {
          GRX_IRFoldEdge edge;
          if (!grx_ir_fold_run_edge(
                  analysis->ir, node->a, cursor - 1, &edge)
              || edge.from != at) {
            break;
          }
          cursor--;

          size_t narrow = 1;
          size_t wide = 1;
          if (analysis->ir->flags & GRX_PROGRAM_UTF) {
            size_t count = 0;
            const GRX_CharRange * ranges = grx_class_table_get(
                &analysis->ir->classes, edge.class_index, &count);
            if (ranges && count) {
              narrow = encoded_width(ranges[0].low);
              wide = encoded_width(ranges[count - 1].high);
            }
          }
          size_t reached = shortest[edge.to % window] + narrow;
          if (reached < low) {
            low = reached;
          }
          reached = longest[edge.to % window] + wide;
          if (reached > high) {
            high = reached;
          }
        }
        shortest[at % window] = low == GRX_NPOS ? 0 : low;
        longest[at % window] = high;
      }

      frame->span = (Span) {shortest[0], longest[0], 0, 0, 0, 0};
      return WALK_POP;
    }

    case GRX_IR_CONCAT:
      frame->span = (Span) {0, 0, 0, 0, 0, 0};
      frame->child = node->first_child;
      frame->stage = WALK_CONCAT_HEAD;
      return WALK_AGAIN;

    case GRX_IR_ALTERNATE:
      frame->span = (Span) {GRX_NPOS, 0, 1, 1, 0, 0};
      frame->child = node->first_child;
      frame->stage = WALK_ALT_HEAD;
      return WALK_AGAIN;

    case GRX_IR_REPEAT:
      frame->stage = WALK_REPEAT_BODY;
      *out_child = node->first_child;
      return WALK_DESCEND;

    case GRX_IR_CAPTURE:
    case GRX_IR_ATOMIC:
    case GRX_IR_SCRIPT_RUN:
      if (node->kind != GRX_IR_CAPTURE) {
        // A script run is not regular for the same reason an atomic group
        // is not: it can refuse a path the automaton already took, and a
        // thread set that merged two paths at one instruction cannot tell
        // them apart afterwards. The check reads the *text* consumed, which
        // is exactly what a Pike VM does not keep.
        analysis->is_regular = 0;
      }
      if (node->kind == GRX_IR_SCRIPT_RUN) {
        analysis->has_script_run = 1;
      }
      frame->stage = WALK_GROUP_BODY;
      *out_child = node->first_child;
      return WALK_DESCEND;

    case GRX_IR_ASSERT:
      frame->span = (Span) {0, 0, 0, 0, 0, 0};
      if (node->mode == GRX_ASSERT_START_SUBJECT
          || node->mode == GRX_ASSERT_SEARCH_START) {
        frame->span.anchored_start = 1;
      }
      if (node->mode == GRX_ASSERT_END_SUBJECT) {
        frame->span.anchored_end = 1;
      }
      return WALK_POP;

    case GRX_IR_LOOK: {
      analysis->has_lookaround = 1;
      analysis->is_regular = 0;
      // Counted rather than set, because lookarounds nest and the inner one
      // must not clear the outer one's claim on the way back out.
      //
      // Only where the dialect's bound is *zero* - where the body must be
      // one length, which is what FIXED means and what keying on the bound
      // says without a second copy of the profile reaching this pass.
      // PCRE2 and perl allow a body to vary and cap how much by, and there
      // the zeroed minimum is load-bearing: pcre2test refuses
      // `(X{65535})(?<=\1{32770})`, and it is refused here because a
      // reference whose group may not participate reads as varying from
      // nothing up to the group's length, which exceeds the cap. Calling
      // that body fixed made this library accept a pattern the reference
      // rejects, and the conformance corpus said so.
      int behind_here = (node->mode == GRX_LOOK_BEHIND_POSITIVE
                            || node->mode == GRX_LOOK_BEHIND_NEGATIVE
                            || node->mode == GRX_LOOK_BEHIND_NON_ATOMIC)
          && analysis->ir->max_variable_lookbehind == 0;
      frame->behind = behind_here ? 1u : 0u;
      analysis->measuring_look_width += behind_here ? 1u : 0u;
      frame->stage = WALK_LOOK_BODY;
      *out_child = node->first_child;
      return WALK_DESCEND;
    }

    case GRX_IR_BACKREF:
      analysis->has_backreference = 1;
      analysis->is_regular = 0;
      // A backreference matches whatever its group did, so its length is
      // that group's - measured here rather than given up on, because that
      // is the difference between the lookbehind pcre2test accepts and the
      // one it refuses.
      if (node->flags & GRX_IR_AMBIGUOUS_REF) {
        // Unless the name it was written with belongs to several groups, in
        // which case it is several lengths and so not one.
        frame->span = (Span) {0, GRX_NPOS, 0, 0, 1, 0};
        return WALK_POP;
      }
      *out_child = node->a;
      return WALK_GROUP;

    case GRX_IR_RECURSE:
      analysis->has_recursion = 1;
      analysis->is_regular = 0;
      // A call matches what its target matches, which is the same question a
      // backreference asks - and `(?0)`, the whole pattern, is the one that
      // has no answer.
      *out_child = node->a;
      return WALK_GROUP;

    case GRX_IR_COND: {
      analysis->is_regular = 0;
      if (node->mode != GRX_COND_ASSERTION) {
        frame->span = (Span) {GRX_NPOS, 0, 1, 1, 0, 0};
        frame->child = node->first_child;
        frame->stage = WALK_ALT_HEAD;
        return WALK_AGAIN;
      }
      // Under GRX_COND_ASSERTION the first child is the *condition*, not a
      // branch. It is walked - so that what it contains still reaches the
      // facts, the way GRX_IR_SCAN's body does - and then discarded, being
      // zero-width where it stands. `(?<=x(?(?=(?<=ab))c|d))` is the shape
      // that says the walk matters: the lookbehind inside the condition
      // has to be measured or the outer one cannot be.
      uint32_t condition = node->first_child;
      const GRX_IRNode * first = grx_ir_node(analysis->ir, condition);
      if (!first) {
        frame->span = (Span) {0, 0, 0, 0, 0, 0};
        return WALK_POP;
      }
      frame->next = first->next_sibling;
      frame->stage = WALK_COND_TEST;
      *out_child = condition;
      return WALK_DESCEND;
    }

    case GRX_IR_KEEP:
    case GRX_IR_KEEP_END:
    case GRX_IR_VERB:
      analysis->is_regular = 0;
      frame->span = (Span) {0, 0, 0, 0, 0, 0};
      frame->span.accepts = node->mode == GRX_VERB_ACCEPT;
      return WALK_POP;

    case GRX_IR_SCAN:
      // Zero-width where it stands, whatever its body matches: the body runs
      // over a captured substring and consumes nothing here. The body is
      // walked all the same, so that what it contains still reaches the
      // facts - `(?<=ab(*scs:(1)cd))` is a fixed-length lookbehind of two,
      // and was refused as variable until this case existed.
      analysis->is_regular = 0;
      frame->stage = WALK_SCAN_BODY;
      *out_child = node->first_child;
      return WALK_DESCEND;

    case GRX_IR_CALLOUT:
      // Zero-width, and regular. With no @ref GRX_CalloutFn registered a
      // callout changes nothing a match can observe, so a pattern that is
      // otherwise a plain automaton keeps its linear guarantee and the
      // Pike VM keeps running it. Registering one is what takes a search
      // off that engine, and that is decided per search in exec.c: a fact
      // about a program must not depend on how a caller later runs it.
      frame->span = (Span) {0, 0, 0, 0, 0, 0};
      return WALK_POP;

    case GRX_IR_COUNT:
    default:
      analysis->is_regular = 0;
      frame->span = (Span) {0, GRX_NPOS, 0, 0, 0, 0};
      return WALK_POP;
  }
}

/**
 * Measure a subtree: the driver over the frames walk_step() leaves.
 *
 * Depth-first, exactly as the recursion was. Re-entrant, because group_span()
 * calls this from inside a walk and the depth cap is shared across the whole
 * nest: the loop runs until the stack is back to the depth it was handed, not
 * until it is empty.
 */
static Span walk(Analysis * analysis, uint32_t node_index) {
  size_t base = analysis->frames.count;
  if (!walk_push(analysis, node_index)) {
    return analysis->result;
  }

  while (analysis->frames.count > base) {
    WalkFrame * frame
        = GRX_ARENA_AT(WalkFrame, &analysis->frames, analysis->frames.count - 1);
    if (!frame) {
      break;
    }

    uint32_t child = GRX_INDEX_NONE;
    WalkAction action = walk_step(analysis, frame, &child);
    if (action == WALK_DESCEND) {
      // `frame` is dead after this: the arena may have moved. A push that
      // refuses has already written the answer to Analysis::result, which is
      // what the resumed stage reads, so both outcomes carry on the same way.
      (void)walk_push(analysis, child);
      continue;
    }
    if (action == WALK_AGAIN) {
      continue;
    }
    if (action == WALK_GROUP) {
      // Re-enters walk(), so the frame is refetched rather than written
      // through; see WalkAction::WALK_GROUP.
      Span measured = group_span(analysis, child);
      frame = GRX_ARENA_AT(
          WalkFrame, &analysis->frames, analysis->frames.count - 1);
      if (!frame) {
        break;
      }
      frame->span = measured;
    }

    // A subtree that can reach `(*ACCEPT)` can stop there, so nothing after
    // the verb is required and the shortest match is the empty string. Applied
    // here, once, rather than in each combining case: every node that contains
    // the verb inherits the claim, and every node that does not is untouched.
    //
    // In the direction the file's header requires: a minimum that is too small
    // can only make a caller try a subject it would have skipped, where one
    // that is too large makes it skip a subject that matches. `(?<=a(*ACCEPT))`
    // is the lookbehind this exists for - Perl calls it variable-length, and
    // the candidate-start model has to be told which starts to try.
    if (frame->span.accepts && frame->span.min_length) {
      frame->span.min_length = 0;
    }
    analysis->result = frame->span;
    analysis->depth--;
    analysis->frames.count--;
  }

  return analysis->result;
}

GRX_Result grx_analyze_ir(GRX_IR * ir, GRX_Facts * out_facts) {
  if (!ir || !out_facts) {
    return GRX_ERR_INVALID;
  }

  grx_facts_init(out_facts);

  Analysis analysis;
  analysis_init(&analysis, ir, ir);

  Span span = ir->root == GRX_INDEX_NONE ? (Span) {0, 0, 0, 0, 0, 0}
                                         : walk(&analysis, ir->root);
  GRX_Result failure = analysis.failure;
  analysis_done(&analysis);
  if (failure != GRX_OK) {
    return failure;
  }

  out_facts->is_regular = analysis.is_regular;
  out_facts->has_backreference = analysis.has_backreference;
  out_facts->has_lookaround = analysis.has_lookaround;
  out_facts->has_recursion = analysis.has_recursion;
  out_facts->has_script_run = analysis.has_script_run;
  out_facts->anchored_start = span.anchored_start;
  out_facts->anchored_end = span.anchored_end;
  out_facts->can_match_empty = span.min_length == 0;
  out_facts->capture_count = ir->capture_count;

  // The lengths are already in bytes: each consuming node contributed the
  // width of the narrowest and widest character it can match, rather than a
  // count of characters scaled at the end.
  out_facts->min_length = span.min_length;
  out_facts->max_length = span.max_length;
  out_facts->max_lookbehind = analysis.max_lookbehind;
  out_facts->max_variable_lookbehind = analysis.max_variable_lookbehind;

  // Duplicate names are a dialect question the front end has already
  // answered - it either rejected them or allowed them - so this only
  // reports what is there.
  out_facts->has_duplicate_names = 0;
  for (size_t i = 0; i < ir->nodes.count && !out_facts->has_duplicate_names;
      i++) {
    const GRX_IRNode * node = grx_ir_node(ir, (uint32_t)i);
    if (!node || node->kind != GRX_IR_CAPTURE
        || node->b == GRX_INDEX_NONE) {
      continue;
    }
    const char * name = grx_ir_name(ir, node->b);
    if (!name) {
      continue;
    }
    for (size_t j = i + 1; j < ir->nodes.count; j++) {
      const GRX_IRNode * other = grx_ir_node(ir, (uint32_t)j);
      if (!other || other->kind != GRX_IR_CAPTURE
          || other->b == GRX_INDEX_NONE) {
        continue;
      }
      const char * candidate = grx_ir_name(ir, other->b);
      if (candidate && strcmp(candidate, name) == 0) {
        out_facts->has_duplicate_names = 1;
        break;
      }
    }
  }

  return GRX_OK;
}

int grx_ir_span(const GRX_IR * ir, uint32_t node_index, size_t * out_min,
    size_t * out_max) {
  if (!ir || !out_min || !out_max || node_index == GRX_INDEX_NONE) {
    return 0;
  }

  Analysis analysis;
  analysis_init(&analysis, ir, NULL);
  Span span = walk(&analysis, node_index);
  analysis_done(&analysis);
  if (span.unknown_length) {
    // A length nobody can compute is not one to prune with. Saying so is the
    // difference between a guard that skips work and a guard that skips an
    // alternative that would have matched.
    return 0;
  }
  *out_min = span.min_length;
  *out_max = span.max_length;
  return 1;
}

int grx_ir_can_match_empty(const GRX_IR * ir, uint32_t node_index) {
  // Conservative when it cannot tell: "yes" is the answer that makes the
  // caller keep its empty-iteration guard, and a guard that was not needed
  // costs two instructions. Answering "no" wrongly would remove the thing
  // that stops `(a*)*` looping.
  if (!ir || node_index == GRX_INDEX_NONE) {
    return 1;
  }

  Analysis analysis;
  analysis_init(&analysis, ir, NULL);
  Span span = walk(&analysis, node_index);
  analysis_done(&analysis);
  return span.min_length == 0;
}
