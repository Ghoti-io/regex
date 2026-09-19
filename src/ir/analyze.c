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
 *
 * Copyright 2026 by Corey Pennycuff
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
} Span;

/** The accumulating answer for the whole program. */
typedef struct {
  const GRX_IR * ir;
  int is_regular;
  int has_backreference;
  int has_lookaround;
  int has_recursion;
  size_t max_lookbehind;
  size_t depth;
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
  Span span = {0, 0, 0, 0};

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
    child = child_node->next_sibling;
  }

  return span;
}

/** The span of a node's children, as alternatives. */
static Span walk_alternate(Analysis * analysis, const GRX_IRNode * node) {
  Span span = {GRX_NPOS, 0, 1, 1};
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
    child = child_node->next_sibling;
  }

  if (!any) {
    return (Span) {0, 0, 0, 0};
  }
  return span;
}

static Span walk(Analysis * analysis, uint32_t node_index) {
  Span span = {0, GRX_NPOS, 0, 0};

  const GRX_IRNode * node = grx_ir_node(analysis->ir, node_index);
  if (!node) {
    return (Span) {0, 0, 0, 0};
  }
  if (analysis->depth >= GRX_ANALYSIS_MAX_DEPTH) {
    // Deeper than the parser's own cap allows, so this is unreachable for a
    // pattern that came through it. A tree built by hand can still get here,
    // and the honest answer for a subtree that was not examined is "nothing
    // is known".
    analysis->is_regular = 0;
    return (Span) {0, GRX_NPOS, 0, 0};
  }
  analysis->depth++;

  switch (node->kind) {
    case GRX_IR_EMPTY:
      span = (Span) {0, 0, 0, 0};
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
      span = (Span) {low, high, 0, 0};
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
      if (node->mode == GRX_REPEAT_POSSESSIVE) {
        analysis->is_regular = 0;
      }
      break;
    }

    case GRX_IR_CAPTURE:
    case GRX_IR_ATOMIC: {
      if (node->kind == GRX_IR_ATOMIC) {
        analysis->is_regular = 0;
      }
      span = walk(analysis, node->first_child);
      break;
    }

    case GRX_IR_ASSERT:
      span = (Span) {0, 0, 0, 0};
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
      if ((node->mode == GRX_LOOK_BEHIND_POSITIVE
              || node->mode == GRX_LOOK_BEHIND_NEGATIVE)
          && body.max_length != GRX_NPOS
          && body.max_length > analysis->max_lookbehind) {
        analysis->max_lookbehind = body.max_length;
      }
      span = (Span) {0, 0, 0, 0};
      break;
    }

    case GRX_IR_BACKREF:
      analysis->has_backreference = 1;
      analysis->is_regular = 0;
      // A backreference matches whatever its group did, which is anything
      // from nothing to the whole subject.
      span = (Span) {0, GRX_NPOS, 0, 0};
      break;

    case GRX_IR_RECURSE:
      analysis->has_recursion = 1;
      analysis->is_regular = 0;
      span = (Span) {0, GRX_NPOS, 0, 0};
      break;

    case GRX_IR_COND:
      analysis->is_regular = 0;
      span = walk_alternate(analysis, node);
      break;

    case GRX_IR_KEEP:
    case GRX_IR_VERB:
      analysis->is_regular = 0;
      span = (Span) {0, 0, 0, 0};
      break;

    case GRX_IR_COUNT:
    default:
      analysis->is_regular = 0;
      span = (Span) {0, GRX_NPOS, 0, 0};
      break;
  }

  analysis->depth--;
  return span;
}

GRX_Result grx_analyze_ir(const GRX_IR * ir, GRX_Facts * out_facts) {
  if (!ir || !out_facts) {
    return GRX_ERR_INVALID;
  }

  grx_facts_init(out_facts);

  Analysis analysis = {
    .ir = ir,
    .is_regular = 1,
    .has_backreference = 0,
    .has_lookaround = 0,
    .has_recursion = 0,
    .max_lookbehind = 0,
    .depth = 0,
  };

  Span span = ir->root == GRX_INDEX_NONE ? (Span) {0, 0, 0, 0}
                                         : walk(&analysis, ir->root);

  out_facts->is_regular = analysis.is_regular;
  out_facts->has_backreference = analysis.has_backreference;
  out_facts->has_lookaround = analysis.has_lookaround;
  out_facts->has_recursion = analysis.has_recursion;
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
    .is_regular = 1,
    .has_backreference = 0,
    .has_lookaround = 0,
    .has_recursion = 0,
    .max_lookbehind = 0,
    .depth = 0,
  };
  Span span = walk(&analysis, node_index);
  return span.min_length == 0;
}
