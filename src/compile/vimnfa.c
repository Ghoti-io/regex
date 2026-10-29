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
 * Build the `vim-nfa` automaton from a lowered pattern.
 *
 * The automaton is built the way Vim builds its own: a repeated atom is
 * copied rather than looped over, `X\+` is `X` followed by `X*`, a counted
 * repeat is its mandatory copies followed by optional ones that each lead to
 * the next copy, and an alternation of several arms is a left-leaning chain of
 * two-way splits. Each of those shapes is observable - through the order of
 * the thread list and through the estimate that decides whether an assertion
 * is postponed - so none of them is chosen for convenience.
 *
 * The automaton is built backwards, each node being given the state that
 * follows it, which is what lets a loop name its own entry before its body
 * exists.
 *
 * A pattern is accepted only when every node of it is one the executor
 * models. Anything else returns no automaton and the backtracking engine
 * runs the pattern; widening what is accepted is a matter of teaching the
 * executor one more construct and measuring it against the reference.
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <stdlib.h>
#include <string.h>

#include "../core/core_internal.h"
#include "../exec/vimnfa_internal.h"
#include "../ir/ir_internal.h"

/** Deepest IR nesting the builder follows before it declines the pattern. */
#define BUILD_DEPTH_LIMIT 64u
/** Most states a pattern may expand to before it is declined. */
#define BUILD_STATE_LIMIT 30000u

typedef struct Builder {
  const GRX_IR * ir;
  GRX_VimNfa * nfa;
  int declined; ///< A construct the executor does not model.
  int oom;
  uint32_t depth;
} Builder;

/**
 * The most bytes a look-behind body can span, as Vim estimates it: the sum
 * along the body, the larger arm of a split, unknown (-1, meaning "from the
 * start of the line") for a loop or a back-reference. INT32_MIN says the
 * estimate depends on something this automaton does not carry.
 */
static int32_t max_width(const GRX_VimNfa * nfa, uint32_t index, int depth) {
  if (index == GRX_VIMNFA_NONE || depth > 4) {
    return -1;
  }
  const GRX_VimNfaState * s = &nfa->states[index];
  switch (s->kind) {
    case GRX_VN_ENDINV:
    case GRX_VN_ENDPAT:
    case GRX_VN_MATCH:
      return 0;
    case GRX_VN_SPLIT: {
      int32_t x = max_width(nfa, s->out, depth + 1);
      int32_t y = max_width(nfa, s->out1, depth + 1);
      if (x == INT32_MIN || y == INT32_MIN) {
        return INT32_MIN;
      }
      return x < 0 || y < 0 ? -1 : (x > y ? x : y);
    }
    case GRX_VN_CHAR: {
      int32_t rest = max_width(nfa, s->out, depth);
      return rest == INT32_MIN ? INT32_MIN : (rest < 0 ? -1 : rest + 1);
    }
    case GRX_VN_SET:
    case GRX_VN_ANY:
      return INT32_MIN;
    case GRX_VN_BREF:
    case GRX_VN_PAT:
      return -1;
    case GRX_VN_INV:
      return max_width(nfa, nfa->states[s->out1].out, depth);
    default:
      return max_width(nfa, s->out, depth);
  }
}

static uint32_t build_node(Builder * b, uint32_t index, uint32_t next);
static uint32_t build_class(Builder * b, uint32_t class_index, int any, uint32_t next);
static uint32_t build_character(Builder * b, const GRX_IRNode * node, uint32_t next);

/** Add a state; the index, or GRX_VIMNFA_NONE after marking the builder. */
static uint32_t add_state(Builder * b, GRX_VimNfaKind kind, uint32_t out) {
  GRX_VimNfa * nfa = b->nfa;
  if (nfa->count >= BUILD_STATE_LIMIT) {
    b->declined = 1;
    return GRX_VIMNFA_NONE;
  }
  if (nfa->count == nfa->capacity) {
    uint32_t capacity = nfa->capacity ? nfa->capacity * 2u : 64u;
    GRX_VimNfaState * states = gcu_allocator_realloc(
        nfa->allocator, nfa->states, capacity * sizeof(*states));
    if (!states) {
      b->oom = 1;
      return GRX_VIMNFA_NONE;
    }
    nfa->states = states;
    nfa->capacity = capacity;
  }
  uint32_t index = nfa->count++;
  GRX_VimNfaState * state = &nfa->states[index];
  memset(state, 0, sizeof(*state));
  state->kind = (uint8_t)kind;
  state->out = out;
  state->out1 = GRX_VIMNFA_NONE;
  return index;
}

static int failed(const Builder * b) {
  return b->declined || b->oom;
}

/** Copy a node's children, in order, into a fresh array. */
static uint32_t * children_of(
    Builder * b, const GRX_IRNode * node, size_t * out_count) {
  size_t count = 0;
  for (uint32_t c = node->first_child; c != GRX_INDEX_NONE;
      c = grx_ir_node(b->ir, c)->next_sibling) {
    count++;
  }
  *out_count = count;
  uint32_t * list = gcu_allocator_calloc(
      b->nfa->allocator, count ? count : 1, sizeof(*list));
  if (!list) {
    b->oom = 1;
    return NULL;
  }
  size_t at = 0;
  for (uint32_t c = node->first_child; c != GRX_INDEX_NONE;
      c = grx_ir_node(b->ir, c)->next_sibling) {
    list[at++] = c;
  }
  return list;
}

/**
 * A one-character collection, `[a]`, lowers to the character followed by a
 * check that no mark comes after it. To Vim it is a collection and not a
 * literal, and the two differ in how likely the text after them is to fail,
 * so it is built as a class of one.
 */
static int is_single_collection(const Builder * b, const GRX_IRNode * node) {
  uint32_t first = node->first_child;
  if (first == GRX_INDEX_NONE) {
    return 0;
  }
  const GRX_IRNode * c = grx_ir_node(b->ir, first);
  if (c->kind != GRX_IR_CHAR || c->next_sibling == GRX_INDEX_NONE) {
    return 0;
  }
  const GRX_IRNode * guard = grx_ir_node(b->ir, c->next_sibling);
  return guard->kind == GRX_IR_ASSERT
      && guard->mode == GRX_ASSERT_NOT_COMPOSING
      && guard->next_sibling == GRX_INDEX_NONE;
}

/** The `index`th child of a node, or NULL. */
static const GRX_IRNode * child_at(
    const Builder * b, const GRX_IRNode * node, unsigned index) {
  uint32_t c = node->first_child;
  while (c != GRX_INDEX_NONE && index--) {
    c = grx_ir_node(b->ir, c)->next_sibling;
  }
  return c == GRX_INDEX_NONE ? NULL : grx_ir_node(b->ir, c);
}

static unsigned child_count(const Builder * b, const GRX_IRNode * node) {
  unsigned n = 0;
  for (uint32_t c = node->first_child; c != GRX_INDEX_NONE;
      c = grx_ir_node(b->ir, c)->next_sibling) {
    n++;
  }
  return n;
}

/**
 * A class, a collection or `.` is lowered as the character followed by a choice
 * about the composing marks after it: skip them and require that none follow,
 * or require a boundary. On a subject without marks neither choice consumes
 * anything, and Vim sees one state of the character's kind, so that is what is
 * built. Returns the node that holds the character, or NULL for any other
 * shape.
 */
static const GRX_IRNode * cluster_consumer(
    const Builder * b, const GRX_IRNode * node) {
  if (node->kind != GRX_IR_CONCAT || child_count(b, node) != 2) {
    return NULL;
  }
  const GRX_IRNode * first = child_at(b, node, 0);
  const GRX_IRNode * tail = child_at(b, node, 1);
  if (first->kind == GRX_IR_CONCAT && child_count(b, first) == 2) {
    const GRX_IRNode * guard = child_at(b, first, 0);
    const GRX_IRNode * body = child_at(b, first, 1);
    if (guard->kind != GRX_IR_ASSERT || (body->kind != GRX_IR_CLASS
            && body->kind != GRX_IR_ANY)) {
      return NULL;
    }
    first = body;
  }
  if ((first->kind != GRX_IR_CLASS && first->kind != GRX_IR_ANY)
      || tail->kind != GRX_IR_ALTERNATE || child_count(b, tail) != 2) {
    return NULL;
  }
  const GRX_IRNode * skip = child_at(b, tail, 0);
  const GRX_IRNode * stop = child_at(b, tail, 1);
  if (skip->kind != GRX_IR_CONCAT || child_count(b, skip) != 3
      || stop->kind != GRX_IR_CONCAT || child_count(b, stop) != 2) {
    return NULL;
  }
  const GRX_IRNode * loop = child_at(b, skip, 1);
  const GRX_IRNode * end = child_at(b, skip, 2);
  if (loop->kind != GRX_IR_REPEAT || end->kind != GRX_IR_ASSERT
      || end->mode != GRX_ASSERT_NOT_COMPOSING) {
    return NULL;
  }
  return first;
}

static uint32_t build_concat(Builder * b, const GRX_IRNode * node,
    uint32_t next) {
  const GRX_IRNode * character = cluster_consumer(b, node);
  if (character) {
    return build_character(b, character, next);
  }
  if (is_single_collection(b, node)) {
    const GRX_IRNode * c = grx_ir_node(b->ir, node->first_child);
    if (c->a == 0 || c->a >= 128u) {
      b->declined = 1;
      return GRX_VIMNFA_NONE;
    }
    uint32_t state = add_state(b, GRX_VN_SET, next);
    if (!failed(b)) {
      b->nfa->states[state].set[c->a >> 3]
          = (uint8_t)(1u << (c->a & 7u));
    }
    return failed(b) ? GRX_VIMNFA_NONE : state;
  }
  size_t count = 0;
  uint32_t * list = children_of(b, node, &count);
  if (!list) {
    return GRX_VIMNFA_NONE;
  }
  uint32_t current = next;
  for (size_t i = count; i > 0 && !failed(b); i--) {
    current = build_node(b, list[i - 1], current);
  }
  gcu_allocator_free(b->nfa->allocator, list);
  return failed(b) ? GRX_VIMNFA_NONE : current;
}

static uint32_t build_alternate(Builder * b, const GRX_IRNode * node,
    uint32_t next) {
  size_t count = 0;
  uint32_t * list = children_of(b, node, &count);
  if (!list) {
    return GRX_VIMNFA_NONE;
  }
  uint32_t current = GRX_VIMNFA_NONE;
  for (size_t i = 0; i < count && !failed(b); i++) {
    uint32_t arm = build_node(b, list[i], next);
    if (failed(b)) {
      break;
    }
    if (i == 0) {
      current = arm;
      continue;
    }
    uint32_t split = add_state(b, GRX_VN_SPLIT, current);
    if (failed(b)) {
      break;
    }
    b->nfa->states[split].out1 = arm;
    current = split;
  }
  gcu_allocator_free(b->nfa->allocator, list);
  return failed(b) ? GRX_VIMNFA_NONE : current;
}

/**
 * Whether `target` can be reached from a state without passing a state that
 * is stored in a thread list. A loop whose body can do that is a cycle of
 * states no marker breaks, which the executor would follow forever; Vim
 * guards it only with a depth limit, and the backtracking engine's rules for
 * an empty iteration are what answer such a pattern here.
 */
static int epsilon_reaches(
    const GRX_VimNfa * nfa, uint32_t from, uint32_t target, unsigned depth) {
  if (from == GRX_VIMNFA_NONE || depth > 256u) {
    return depth > 256u;
  }
  if (from == target) {
    return 1;
  }
  const GRX_VimNfaState * s = &nfa->states[from];
  switch (s->kind) {
    case GRX_VN_SPLIT:
      return epsilon_reaches(nfa, s->out, target, depth + 1u)
          || epsilon_reaches(nfa, s->out1, target, depth + 1u);
    case GRX_VN_MCLOSE:
    case GRX_VN_NCLOSE:
    case GRX_VN_EMPTY:
      return epsilon_reaches(nfa, s->out, target, depth + 1u);
    default:
      return 0;
  }
}

/** `X*`: a split whose preferred arm is the body, which returns to it. */
static uint32_t build_star(Builder * b, uint32_t body_node, uint32_t next) {
  uint32_t split = add_state(b, GRX_VN_SPLIT, GRX_VIMNFA_NONE);
  if (failed(b)) {
    return GRX_VIMNFA_NONE;
  }
  b->nfa->states[split].out1 = next;
  uint32_t body = build_node(b, body_node, split);
  if (failed(b)) {
    return GRX_VIMNFA_NONE;
  }
  if (epsilon_reaches(b->nfa, body, split, 0)) {
    b->declined = 1;
    return GRX_VIMNFA_NONE;
  }
  b->nfa->states[split].out = body;
  return split;
}

/** `X\=`: a split whose preferred arm is the body and whose other skips it. */
static uint32_t build_quest(Builder * b, uint32_t body_node, uint32_t next) {
  uint32_t body = build_node(b, body_node, next);
  if (failed(b)) {
    return GRX_VIMNFA_NONE;
  }
  uint32_t split = add_state(b, GRX_VN_SPLIT, body);
  if (failed(b)) {
    return GRX_VIMNFA_NONE;
  }
  b->nfa->states[split].out1 = next;
  return split;
}

static uint32_t build_repeat(Builder * b, const GRX_IRNode * node,
    uint32_t next) {
  if (node->mode != GRX_REPEAT_GREEDY || node->first_child == GRX_INDEX_NONE) {
    b->declined = 1;
    return GRX_VIMNFA_NONE;
  }
  uint32_t body = node->first_child;
  uint32_t min = node->min;
  uint32_t max = node->max;
  if (max == 0) {
    return add_state(b, GRX_VN_EMPTY, next);
  }
  if (min == 0 && max == GRX_REPEAT_INF) {
    return build_star(b, body, next);
  }
  // The copies, counted from the end: a bounded repeat is `min` mandatory
  // copies and `max - min` optional ones; an unbounded one is `min`
  // mandatory copies and a starred one.
  if (min > 200u || (max != GRX_REPEAT_INF && max > 500u)) {
    b->declined = 1;
    return GRX_VIMNFA_NONE;
  }
  uint32_t copies = max == GRX_REPEAT_INF ? min + 1u : max;
  uint32_t current = next;
  for (uint32_t k = copies; k > 0 && !failed(b); k--) {
    uint32_t index = k - 1u;
    if (index < min) {
      current = build_node(b, body, current);
    }
    else if (max == GRX_REPEAT_INF) {
      current = build_star(b, body, current);
    }
    else {
      current = build_quest(b, body, current);
    }
  }
  return failed(b) ? GRX_VIMNFA_NONE : current;
}

static uint32_t build_class(Builder * b, uint32_t class_index, int any,
    uint32_t next) {
  uint32_t state = add_state(b, any ? GRX_VN_ANY : GRX_VN_SET, next);
  if (failed(b)) {
    return GRX_VIMNFA_NONE;
  }
  GRX_VimNfaState * s = &b->nfa->states[state];
  for (uint32_t c = 1; c < 128u; c++) {
    int member = 0;
    if (class_index == GRX_INDEX_NONE) {
      member = any;
    }
    else {
      member = grx_class_table_contains(&b->ir->classes, class_index, c) != 0;
      if (any) {
        member = !member;
      }
    }
    if (member) {
      s->set[c >> 3] = (uint8_t)(s->set[c >> 3] | (1u << (c & 7u)));
    }
  }
  return state;
}

/**
 * One class, collection or `.`. Written `\_.` it is two states, the character
 * or a line break, which is what makes an atomic group in front of it enterable
 * where another has just ended: what follows the group is a choice, which no
 * list holds.
 */
static uint32_t build_character(
    Builder * b, const GRX_IRNode * node, uint32_t next) {
  uint32_t any = build_class(b, node->a, node->kind == GRX_IR_ANY, next);
  if (failed(b) || !(node->flags & GRX_IR_ANY_OR_NEWLINE)) {
    return any;
  }
  uint32_t newline = add_state(b, GRX_VN_SET, next);
  if (failed(b)) {
    return GRX_VIMNFA_NONE;
  }
  b->nfa->states[newline].set[1] = 0x04;
  uint32_t split = add_state(b, GRX_VN_SPLIT, any);
  if (!failed(b)) {
    b->nfa->states[split].out1 = newline;
  }
  return failed(b) ? GRX_VIMNFA_NONE : split;
}

static uint32_t build_node(Builder * b, uint32_t index, uint32_t next) {
  if (failed(b)) {
    return GRX_VIMNFA_NONE;
  }
  const GRX_IRNode * node = grx_ir_node(b->ir, index);
  if (!node || b->depth >= BUILD_DEPTH_LIMIT) {
    b->declined = 1;
    return GRX_VIMNFA_NONE;
  }
  b->depth++;
  uint32_t result = GRX_VIMNFA_NONE;
  switch (node->kind) {
    case GRX_IR_EMPTY:
      result = add_state(b, GRX_VN_EMPTY, next);
      break;
    case GRX_IR_CHAR:
      if (node->a == 0 || node->a >= 128u) {
        b->declined = 1;
        break;
      }
      result = add_state(b, GRX_VN_CHAR, next);
      if (!failed(b)) {
        b->nfa->states[result].ch = node->a;
      }
      break;
    case GRX_IR_CLASS:
      result = build_class(b, node->a, 0, next);
      break;
    case GRX_IR_ANY:
      result = build_character(b, node, next);
      break;
    case GRX_IR_CONCAT:
      if (node->flags & GRX_IR_NFA_GROUP) {
        uint32_t close = add_state(b, GRX_VN_NCLOSE, next);
        if (failed(b)) {
          break;
        }
        uint32_t body = build_concat(b, node, close);
        if (failed(b)) {
          break;
        }
        result = add_state(b, GRX_VN_NOPEN, body);
        break;
      }
      result = build_concat(b, node, next);
      break;
    case GRX_IR_ALTERNATE:
      if (node->flags & GRX_IR_OPTIONAL_MEMBER) {
        b->declined = 1;
        break;
      }
      result = build_alternate(b, node, next);
      break;
    case GRX_IR_REPEAT:
      result = build_repeat(b, node, next);
      break;
    case GRX_IR_CAPTURE: {
      if (node->a == 0 || node->a > GRX_VIMNFA_GROUPS
          || node->first_child == GRX_INDEX_NONE) {
        b->declined = 1;
        break;
      }
      uint32_t close = add_state(b, GRX_VN_MCLOSE, next);
      if (failed(b)) {
        break;
      }
      b->nfa->states[close].group = (uint8_t)node->a;
      uint32_t body = build_node(b, node->first_child, close);
      if (failed(b)) {
        break;
      }
      result = add_state(b, GRX_VN_MOPEN, body);
      if (!failed(b)) {
        b->nfa->states[result].group = (uint8_t)node->a;
        if (node->a > b->nfa->groups) {
          b->nfa->groups = node->a;
        }
      }
      break;
    }
    case GRX_IR_BACKREF: {
      if (node->a == 0 || node->a > GRX_VIMNFA_GROUPS
          || (node->flags & (GRX_IR_CASELESS | GRX_IR_BACKREF_OPEN))) {
        b->declined = 1;
        break;
      }
      uint32_t skip = add_state(b, GRX_VN_SKIP, next);
      if (failed(b)) {
        break;
      }
      result = add_state(b, GRX_VN_BREF, skip);
      if (!failed(b)) {
        b->nfa->states[result].group = (uint8_t)node->a;
        if (node->a > b->nfa->groups) {
          b->nfa->groups = node->a;
        }
      }
      break;
    }
    case GRX_IR_ASSERT:
      if (node->mode == GRX_ASSERT_START_SUBJECT) {
        result = add_state(b, GRX_VN_BOL, next);
      }
      else if (node->mode == GRX_ASSERT_END_SUBJECT) {
        result = add_state(b, GRX_VN_EOL, next);
      }
      else if (node->mode == GRX_ASSERT_CLUSTER_BOUNDARY
          || node->mode == GRX_ASSERT_NOT_COMPOSING) {
        // The automaton runs only on subjects without a composing
        // character, where every position is a cluster boundary and none
        // is in front of a mark.
        result = next;
      }
      else {
        b->declined = 1;
      }
      break;
    case GRX_IR_KEEP:
      result = add_state(b, GRX_VN_ZSTART, next);
      break;
    case GRX_IR_KEEP_END:
      if (node->flags & GRX_IR_END_CONTEXT_ONLY) {
        b->declined = 1;
        break;
      }
      b->nfa->has_ze = 1;
      result = add_state(b, GRX_VN_ZEND, next);
      break;
    case GRX_IR_ATOMIC: {
      if (node->first_child == GRX_INDEX_NONE) {
        b->declined = 1;
        break;
      }
      uint32_t skip = add_state(b, GRX_VN_SKIP, next);
      if (failed(b)) {
        break;
      }
      uint32_t end = add_state(b, GRX_VN_ENDPAT, skip);
      if (failed(b)) {
        break;
      }
      uint32_t body = build_node(b, node->first_child, end);
      if (failed(b)) {
        break;
      }
      result = add_state(b, GRX_VN_PAT, body);
      if (!failed(b)) {
        b->nfa->states[result].out1 = end;
      }
      break;
    }
    case GRX_IR_LOOK: {
      int behind = node->mode == GRX_LOOK_BEHIND_POSITIVE
          || node->mode == GRX_LOOK_BEHIND_NEGATIVE;
      int negative = node->mode == GRX_LOOK_AHEAD_NEGATIVE
          || node->mode == GRX_LOOK_BEHIND_NEGATIVE;
      if ((!behind && node->mode != GRX_LOOK_AHEAD_POSITIVE && !negative)
          || (node->flags & GRX_IR_LOOK_CONDITION)
          || node->first_child == GRX_INDEX_NONE) {
        b->declined = 1;
        break;
      }
      uint32_t end = add_state(b, GRX_VN_ENDINV, next);
      if (failed(b)) {
        break;
      }
      uint32_t body = build_node(b, node->first_child, end);
      if (failed(b)) {
        break;
      }
      result = add_state(b, GRX_VN_INV, body);
      if (!failed(b)) {
        GRX_VimNfaState * state = &b->nfa->states[result];
        state->out1 = end;
        state->neg = negative ? 1u : 0u;
        state->before = behind ? 1u : 0u;
        if (behind) {
          // An explicit bound is the width; without one it is estimated from
          // the body, and a body whose width cannot be told - a class or a
          // collection, which Vim sizes by what they could match in UTF-8 -
          // is not modelled.
          int32_t width = node->b > 0 ? (int32_t)node->b
                                      : max_width(b->nfa, body, 0);
          if (width == INT32_MIN) {
            b->declined = 1;
          }
          state->width = width;
        }
      }
      break;
    }
    default:
      b->declined = 1;
      break;
  }
  b->depth--;
  return failed(b) ? GRX_VIMNFA_NONE : result;
}

/* ---- the estimates that decide whether an assertion is postponed ---- */

/**
 * How likely the text that follows a state is to fail, as a score out of a
 * hundred: a literal character fails often, an assertion almost never.
 * Open and close markers add no information and are looked through. The
 * search gives up two levels past the fourth state and scores it low, which
 * is why a long tail can make an assertion look cheap to follow.
 */
static int failure_chance(const GRX_VimNfa * nfa, uint32_t index, int depth) {
  if (index == GRX_VIMNFA_NONE) {
    return 50;
  }
  if (depth > 4) {
    return 1;
  }
  const GRX_VimNfaState * s = &nfa->states[index];
  switch (s->kind) {
    case GRX_VN_SPLIT: {
      const GRX_VimNfaState * a = &nfa->states[s->out];
      const GRX_VimNfaState * c = &nfa->states[s->out1];
      if (a->kind == GRX_VN_SPLIT || c->kind == GRX_VN_SPLIT) {
        return 1;
      }
      int x = failure_chance(nfa, s->out, depth + 1);
      int y = failure_chance(nfa, s->out1, depth + 1);
      return x < y ? x : y;
    }
    case GRX_VN_ANY:
      return 1;
    case GRX_VN_MATCH:
      return 0;
    case GRX_VN_INV:
    case GRX_VN_PAT:
      return 5;
    case GRX_VN_BOL:
    case GRX_VN_EOL:
      return 99;
    case GRX_VN_BREF:
      return 94;
    case GRX_VN_CHAR:
      return 95;
    case GRX_VN_SET: {
      // A set holding only the line break is Vim's NEWL, which it rates as it
      // does the anchors.
      int only_newline = (s->set[1] == 0x04);
      for (int k = 0; k < 16 && only_newline; k++) {
        if (k != 1 && s->set[k]) {
          only_newline = 0;
        }
      }
      return only_newline ? 99 : 50;
    }
    case GRX_VN_MOPEN:
    case GRX_VN_NOPEN:
    case GRX_VN_MCLOSE:
    case GRX_VN_NCLOSE:
      return failure_chance(nfa, s->out, depth + 1);
    default:
      return 50;
  }
}

int grx_vimnfa_match_follows(
    const GRX_VimNfa * nfa, uint32_t index, int depth) {
  if (index == GRX_VIMNFA_NONE || depth > 10) {
    return 0;
  }
  const GRX_VimNfaState * s = &nfa->states[index];
  switch (s->kind) {
    case GRX_VN_SPLIT:
      return grx_vimnfa_match_follows(nfa, s->out, depth + 1)
          || grx_vimnfa_match_follows(nfa, s->out1, depth + 1);
    case GRX_VN_MATCH:
    case GRX_VN_ENDINV:
    case GRX_VN_ENDPAT:
      return 1;
    case GRX_VN_INV:
      return grx_vimnfa_match_follows(nfa, nfa->states[s->out1].out, depth + 1);
    case GRX_VN_CHAR:
    case GRX_VN_SET:
    case GRX_VN_ANY:
      return 0;
    default:
      return grx_vimnfa_match_follows(nfa, s->out, depth + 1);
  }
}

/** Decide, for each assertion, whether it is postponed. */
static void decide_postponement(GRX_VimNfa * nfa) {
  for (uint32_t i = 0; i < nfa->count; i++) {
    GRX_VimNfaState * s = &nfa->states[i];
    if (s->kind != GRX_VN_INV) {
      continue;
    }
    uint32_t follow = nfa->states[s->out1].out;
    if (grx_vimnfa_match_follows(nfa, follow, 0)) {
      s->post = 0;
      continue;
    }
    int inside = failure_chance(nfa, s->out, 0);
    int after = failure_chance(nfa, follow, 0);
    if (s->before) {
      if (s->width <= 0 && after > 0) {
        s->post = 1;
      }
      else {
        s->post = after * 10 < inside ? 0u : 1u;
      }
      continue;
    }
    s->post = after < inside ? 0u : 1u;
  }
}

GRX_Result grx_vimnfa_build(const GRX_IR * ir, const GRX_Allocator * allocator,
    GRX_VimNfa ** out_nfa) {
  if (!ir || !allocator || !out_nfa) {
    return GRX_ERR_INVALID;
  }
  *out_nfa = NULL;
  if (ir->root == GRX_INDEX_NONE) {
    return GRX_OK;
  }

  GRX_VimNfa * nfa = gcu_allocator_calloc(allocator, 1, sizeof(*nfa));
  if (!nfa) {
    return GRX_ERR_OOM;
  }
  nfa->allocator = allocator;

  Builder b = {.ir = ir, .nfa = nfa};
  uint32_t match = add_state(&b, GRX_VN_MATCH, GRX_VIMNFA_NONE);
  // Vim closes the whole match's group before it reaches the end. That state
  // is not stored in a list, so it is also what an atomic group at the end of
  // the pattern finds after it: nothing a list could already hold.
  uint32_t close = failed(&b) ? GRX_VIMNFA_NONE
                              : add_state(&b, GRX_VN_MCLOSE, match);
  if (!failed(&b)) {
    nfa->states[close].group = 0;
  }
  uint32_t start = failed(&b) ? GRX_VIMNFA_NONE : build_node(&b, ir->root, close);
  if (b.oom) {
    grx_vimnfa_free(nfa);
    return GRX_ERR_OOM;
  }
  if (b.declined || start == GRX_VIMNFA_NONE) {
    grx_vimnfa_free(nfa);
    return GRX_OK;
  }
  nfa->start = start;
  decide_postponement(nfa);
  *out_nfa = nfa;
  return GRX_OK;
}

void grx_vimnfa_free(GRX_VimNfa * nfa) {
  if (!nfa) {
    return;
  }
  const GRX_Allocator * allocator = nfa->allocator;
  gcu_allocator_free(allocator, nfa->states);
  gcu_allocator_free(allocator, nfa);
}
