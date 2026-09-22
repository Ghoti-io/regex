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

/** The span of a node's children, concatenated. */
static Span walk_concat(Analysis * analysis, const GRX_IRNode * node) {
  Span span = {0, 0, 0, 0, 0, 0};

  for (uint32_t child = node->first_child; child != GRX_INDEX_NONE;) {
    const GRX_IRNode * child_node = grx_ir_node(analysis->ir, child);
    if (!child_node) {
      break;
    }
    Span part = walk(analysis, child);

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
    child = child_node->next_sibling;
  }

  return span;
}

/** The span of a node's children, as alternatives. */
static Span walk_alternate(Analysis * analysis, const GRX_IRNode * node) {
  Span span = {GRX_NPOS, 0, 1, 1, 0, 0};
  int any = 0;

  for (uint32_t child = node->first_child; child != GRX_INDEX_NONE;) {
    const GRX_IRNode * child_node = grx_ir_node(analysis->ir, child);
    if (!child_node) {
      break;
    }
    Span part = walk(analysis, child);
    any = 1;

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
    child = child_node->next_sibling;
  }

  if (!any) {
    return (Span) {0, 0, 0, 0, 0, 0};
  }
  return span;
}

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
  if (!group || analysis->resolving_count >= GRX_ANALYSIS_MAX_REFERENCES) {
    return unknown;
  }
  for (size_t i = 0; i < analysis->resolving_count; i++) {
    if (analysis->resolving[i] == group) {
      return unknown;
    }
  }

  uint32_t body = GRX_INDEX_NONE;
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

  analysis->resolving[analysis->resolving_count++] = group;
  Span span = walk(analysis, body);
  analysis->resolving_count--;

  span.min_length = 0;
  span.anchored_start = 0;
  span.anchored_end = 0;
  return span;
}

static Span walk(Analysis * analysis, uint32_t node_index) {
  Span span = {0, GRX_NPOS, 0, 0, 0, 0};

  const GRX_IRNode * node = grx_ir_node(analysis->ir, node_index);
  if (!node) {
    return (Span) {0, 0, 0, 0, 0, 0};
  }
  if (analysis->depth >= GRX_ANALYSIS_MAX_DEPTH) {
    // Deeper than the parser's own cap allows, so this is unreachable for a
    // pattern that came through it. A tree built by hand can still get here,
    // and the honest answer for a subtree that was not examined is "nothing
    // is known".
    analysis->is_regular = 0;
    return (Span) {0, GRX_NPOS, 0, 0, 0, 0};
  }
  analysis->depth++;

  switch (node->kind) {
    case GRX_IR_EMPTY:
      span = (Span) {0, 0, 0, 0, 0, 0};
      break;

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
      span = (Span) {low, high, 0, 0, 0, 0};
      break;
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

      span = (Span) {shortest[0], longest[0], 0, 0, 0, 0};
      break;
    }

    case GRX_IR_CONCAT:
      span = walk_concat(analysis, node);
      break;

    case GRX_IR_ALTERNATE:
      span = walk_alternate(analysis, node);
      break;

    case GRX_IR_REPEAT: {
      Span body = walk(analysis, node->first_child);
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
      break;
    }

    case GRX_IR_CAPTURE:
    case GRX_IR_ATOMIC:
    case GRX_IR_SCRIPT_RUN: {
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
      span = walk(analysis, node->first_child);
      break;
    }

    case GRX_IR_ASSERT:
      span = (Span) {0, 0, 0, 0, 0, 0};
      if (node->mode == GRX_ASSERT_START_SUBJECT
          || node->mode == GRX_ASSERT_SEARCH_START) {
        span.anchored_start = 1;
      }
      if (node->mode == GRX_ASSERT_END_SUBJECT) {
        span.anchored_end = 1;
      }
      break;

    case GRX_IR_LOOK: {
      analysis->has_lookaround = 1;
      analysis->is_regular = 0;
      Span body = walk(analysis, node->first_child);
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
          GRX_Result stored = grx_ir_look_span_set(
              analysis->writable, &offset, body.min_length, body.max_length);
          if (stored != GRX_OK) {
            if (analysis->failure == GRX_OK) {
              analysis->failure = stored;
            }
          }
          else {
            GRX_IRNode * writable
                = grx_ir_node(analysis->writable, node_index);
            if (writable) {
              writable->a = offset;
            }
          }
        }
      }
      span = (Span) {0, 0, 0, 0, 0, 0};
      break;
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
        span = (Span) {0, GRX_NPOS, 0, 0, 1, 0};
        break;
      }
      span = group_span(analysis, node->a);
      break;

    case GRX_IR_RECURSE:
      analysis->has_recursion = 1;
      analysis->is_regular = 0;
      // A call matches what its target matches, which is the same question a
      // backreference asks - and `(?0)`, the whole pattern, is the one that
      // has no answer.
      span = group_span(analysis, node->a);
      break;

    case GRX_IR_COND:
      analysis->is_regular = 0;
      span = walk_alternate(analysis, node);
      break;

    case GRX_IR_KEEP:
    case GRX_IR_VERB:
      analysis->is_regular = 0;
      span = (Span) {0, 0, 0, 0, 0, 0};
      span.accepts = node->mode == GRX_VERB_ACCEPT;
      break;

    case GRX_IR_SCAN:
      // Zero-width where it stands, whatever its body matches: the body runs
      // over a captured substring and consumes nothing here. The body is
      // walked all the same, so that what it contains still reaches the
      // facts - `(?<=ab(*scs:(1)cd))` is a fixed-length lookbehind of two,
      // and was refused as variable until this case existed.
      analysis->is_regular = 0;
      (void)walk(analysis, node->first_child);
      span = (Span) {0, 0, 0, 0, 0, 0};
      break;

    case GRX_IR_CALLOUT:
      // Zero-width, and regular. With no @ref GRX_CalloutFn registered a
      // callout changes nothing a match can observe, so a pattern that is
      // otherwise a plain automaton keeps its linear guarantee and the
      // Pike VM keeps running it. Registering one is what takes a search
      // off that engine, and that is decided per search in exec.c: a fact
      // about a program must not depend on how a caller later runs it.
      span = (Span) {0, 0, 0, 0, 0, 0};
      break;

    case GRX_IR_COUNT:
    default:
      analysis->is_regular = 0;
      span = (Span) {0, GRX_NPOS, 0, 0, 0, 0};
      break;
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
  if (span.accepts && span.min_length) {
    span.min_length = 0;
  }

  analysis->depth--;
  return span;
}

GRX_Result grx_analyze_ir(GRX_IR * ir, GRX_Facts * out_facts) {
  if (!ir || !out_facts) {
    return GRX_ERR_INVALID;
  }

  grx_facts_init(out_facts);

  Analysis analysis = {
    .ir = ir,
    .writable = ir,
    .failure = GRX_OK,
    .is_regular = 1,
    .has_backreference = 0,
    .has_lookaround = 0,
    .has_recursion = 0,
    .has_script_run = 0,
    .max_lookbehind = 0,
    .max_variable_lookbehind = 0,
    .depth = 0,
    .resolving = {0},
    .resolving_count = 0,
  };

  Span span = ir->root == GRX_INDEX_NONE ? (Span) {0, 0, 0, 0, 0, 0}
                                         : walk(&analysis, ir->root);
  if (analysis.failure != GRX_OK) {
    return analysis.failure;
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

  Analysis analysis = {
    .ir = ir,
    .writable = NULL,
    .failure = GRX_OK,
    .is_regular = 1,
    .has_backreference = 0,
    .has_lookaround = 0,
    .has_recursion = 0,
    .max_lookbehind = 0,
    .max_variable_lookbehind = 0,
    .depth = 0,
    .resolving = {0},
    .resolving_count = 0,
  };
  Span span = walk(&analysis, node_index);
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

  Analysis analysis = {
    .ir = ir,
    .writable = NULL,
    .failure = GRX_OK,
    .is_regular = 1,
    .has_backreference = 0,
    .has_lookaround = 0,
    .has_recursion = 0,
    .max_lookbehind = 0,
    .max_variable_lookbehind = 0,
    .depth = 0,
    .resolving = {0},
    .resolving_count = 0,
  };
  Span span = walk(&analysis, node_index);
  return span.min_length == 0;
}
