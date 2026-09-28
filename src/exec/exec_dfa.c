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
 * A lazy DFA over the compiled program: documentation/design.md section 3.5.5.
 *
 * The other two engines simulate the program. This one runs a *different*
 * automaton that accepts the same language: the program is lifted into a
 * byte-labelled NFA once, and the subset construction over that is built a
 * state at a time as the subject asks for them, into a cache that is thrown
 * away and rebuilt when it fills. A byte then costs one table lookup instead
 * of a closure, which is the whole point - and the price is that a DFA state
 * is a *set* of program counters, so it can say where a match is and cannot
 * say which thread found it.
 *
 * That is why this is not a third answer to the same question. It reports
 * the extent of a match and never its groups; a search that needs groups
 * runs the Pike VM over the span this found, which is the span and not the
 * subject. And because the extent is all it reports, it is offered only
 * where the extent is all that is being decided by priority - a program
 * whose preference is leftmost-longest, where the answer is a property of
 * the language and not of the order the arms were written in.
 *
 * What it can run at all is deliberately narrow; grx_dfa_eligible() is the
 * whole list. The two that matter: the lift is byte-level, so a program in
 * UTF mode is refused because a character there is one to four bytes and a
 * class matches a code point rather than a byte; and every opcode has to be
 * zero-width or consume exactly one byte, which rules out backreferences,
 * lookaround, recursion, script runs and callouts.
 */

#include "exec_internal.h"
#include "../compile/compile_internal.h"
#include <string.h>

enum { KIND_EPSILON = 0, KIND_CONSUME = 1, KIND_ACCEPT = 2 };

typedef struct {
  uint8_t kind;
  uint8_t nsucc;
  uint32_t succ[2];
  uint8_t bytes[32];  /* KIND_CONSUME only. */
} Node;

/* A DFA state is the epsilon-closure of a set of NFA nodes, reduced to the
 * nodes that can do something next: the consuming ones and the accepting
 * one. Two closures with the same such set behave identically from here on,
 * which is what makes the set the right key. */
typedef struct {
  uint32_t first;   /* Offset into the pool. */
  uint32_t count;
} State;

/* Macros rather than an enum: these do not fit in an `int`, which is what an
 * enumerator is before C23, and the build is -std=c17 -pedantic-errors. */
#define STATE_NONE 0xFFFFFFFFu
#define STATE_UNKNOWN 0xFFFFFFFEu
/* Set in a transition when the state it lands on accepts, so the byte loop
 * reads one array instead of two. Below STATE_UNKNOWN, so the same unsigned
 * compare still separates a filled transition from an empty one. */
#define STATE_ACCEPT 0x40000000u
#define STATE_INDEX 0x3FFFFFFFu

/* The transitions live in one flat array rather than inside the state,
 * because the inner loop reads exactly one of them and nothing else: a
 * 1 KB state made every step a multiply by the struct's size and put the
 * accepting flag a kilobyte away from the row that had just been read. */
typedef struct {
  State * states;
  uint32_t * trans;   /* count * 256 */
  uint8_t * flag;     /* count */
  size_t count;
  size_t capacity;
  uint32_t * pool;
  size_t pool_count;
  size_t pool_capacity;
  uint32_t * table;   /* Open-addressed; indexes into `states`. */
  size_t mask;
} Cache;

struct GRX_Dfa {
  const GRX_Allocator * allocator;
  const GRX_Program * program;
  Node * nodes;
  size_t node_count;
  /* Two tables, because they are two different automata. The anchored one
   * starts at the entry and never returns to it; the unanchored one adds the
   * entry back at every position. A transition cached by one is wrong for
   * the other, and sharing a table gave /a/ a match of "aa" - the entry
   * injected into a state the anchored run then reused. */
  Cache forward;
  Cache unanchored;
  size_t max_states;
  size_t flushes;
  uint32_t anchored_start;
  /* Scratch for building one closure. */
  uint32_t * work;
  uint8_t * seen;
  uint32_t * build;
  size_t build_count;
  int build_flag;
};

/* ------------------------------------------------------------------ */

static int allowed_opcode(const GRX_Inst * inst) {
  switch ((GRX_Opcode)inst->op) {
    case GRX_OP_MATCH:
    case GRX_OP_SPLIT:
    case GRX_OP_JMP:
    case GRX_OP_SAVE:
    case GRX_OP_PROGRESS_SET:
    case GRX_OP_PROGRESS_CHECK:
    case GRX_OP_RESET:
    case GRX_OP_RESET_STALE:
    case GRX_OP_ATOMIC_BEGIN:
    case GRX_OP_ATOMIC_END:
      return 1;
    case GRX_OP_CHAR:
    case GRX_OP_CLASS:
    case GRX_OP_ANY:
    case GRX_OP_ANY_NL:
      /* `.` under (*CRLF) refuses a position rather than a byte, which is a
       * property of where it stands and not of what it reads. A DFA state
       * has no position, so such a program is not lifted. */
      return !(inst->flags & GRX_INST_NEWLINE_CRLF)
          && !(inst->flags & GRX_INST_REVERSE);
    default:
      return 0;
  }
}

int grx_dfa_eligible(const GRX_Program * program) {
  if (!program || !program->insts.count) {
    return 0;
  }
  /* One instruction per byte is the whole premise of a byte-level lift: a
   * CHAR under UTF-8 carries a code point that is one to four bytes, and a
   * class matches a code point rather than a byte. */
  if (program->flags & GRX_PROGRAM_UTF) {
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
/* Lifting.                                                             */
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

static int lift(GRX_Dfa * dfa) {
  const GRX_Program * program = dfa->program;
  size_t count = program->insts.count;
  dfa->nodes = gcu_allocator_calloc(dfa->allocator, count, sizeof *dfa->nodes);
  if (!dfa->nodes) {
    return 0;
  }
  dfa->node_count = count;
  for (size_t i = 0; i < count; i++) {
    const GRX_Inst * inst
        = GRX_ARENA_AT(const GRX_Inst, &program->insts, (uint32_t)i);
    Node * node = &dfa->nodes[i];
    switch ((GRX_Opcode)inst->op) {
      case GRX_OP_MATCH:
        node->kind = KIND_ACCEPT;
        break;
      case GRX_OP_CHAR:
      case GRX_OP_CLASS:
      case GRX_OP_ANY:
      case GRX_OP_ANY_NL:
        node->kind = KIND_CONSUME;
        byte_set_from(program, inst, node->bytes);
        if (i + 1 >= count) {
          return 0;
        }
        node->succ[node->nsucc++] = (uint32_t)(i + 1);
        break;
      case GRX_OP_JMP:
        node->kind = KIND_EPSILON;
        node->succ[node->nsucc++] = inst->x;
        break;
      case GRX_OP_SPLIT:
        node->kind = KIND_EPSILON;
        node->succ[node->nsucc++] = inst->x;
        node->succ[node->nsucc++] = inst->y;
        break;
      case GRX_OP_PROGRESS_CHECK:
        /* Both ways out, because the rule it enforces is about which
         * *division* an iteration may produce and about the thread list
         * terminating, not about which strings match: an iteration it
         * refuses consumed nothing, so every byte-path through it exists
         * without it too. Over-approximating here can only keep states the
         * NFA would have dropped, never invent a byte a match can read -
         * and the differential against the Pike VM is what says so. */
        node->kind = KIND_EPSILON;
        if (i + 1 >= count) {
          return 0;
        }
        node->succ[node->nsucc++] = (uint32_t)(i + 1);
        node->succ[node->nsucc++] = inst->y;
        break;
      default:
        /* Everything else the eligibility check allows is zero-width with
         * one way onwards. */
        node->kind = KIND_EPSILON;
        if (i + 1 >= count) {
          return 0;
        }
        node->succ[node->nsucc++] = (uint32_t)(i + 1);
        break;
    }
    for (uint8_t s = 0; s < node->nsucc; s++) {
      if (node->succ[s] >= count) {
        return 0;
      }
    }
  }
  return 1;
}

/* ------------------------------------------------------------------ */
/* Closures.                                                            */
/*                                                                      */
/* A state is the set of nodes that can do something next - consume a   */
/* byte, or accept - so a closure walks the zero-width edges and stops  */
/* at those. Two closures that reduce to the same set behave the same   */
/* from here on, which is what makes the set the key and the whole      */
/* construction finite.                                                 */
/* ------------------------------------------------------------------ */

static void build_reset(GRX_Dfa * dfa) {
  for (size_t i = 0; i < dfa->build_count; i++) {
    dfa->seen[dfa->build[i]] = 0;
  }
  /* The walk marks nodes it passed through as well as ones it kept, so the
   * marks are cleared wholesale rather than from the kept list. */
  memset(dfa->seen, 0, dfa->node_count);
  dfa->build_count = 0;
  dfa->build_flag = 0;
}

static void close_forward(GRX_Dfa * dfa, const uint32_t * from, size_t count) {
  size_t depth = 0;
  for (size_t i = 0; i < count; i++) {
    if (!dfa->seen[from[i]]) {
      dfa->seen[from[i]] = 1;
      dfa->work[depth++] = from[i];
    }
  }
  while (depth) {
    uint32_t pc = dfa->work[--depth];
    const Node * node = &dfa->nodes[pc];
    if (node->kind == KIND_ACCEPT) {
      dfa->build_flag = 1;
      dfa->build[dfa->build_count++] = pc;
      continue;
    }
    if (node->kind == KIND_CONSUME) {
      dfa->build[dfa->build_count++] = pc;
      continue;
    }
    for (uint8_t s = 0; s < node->nsucc; s++) {
      uint32_t to = node->succ[s];
      if (!dfa->seen[to]) {
        dfa->seen[to] = 1;
        dfa->work[depth++] = to;
      }
    }
  }
}

/* ------------------------------------------------------------------ */
/* The state cache.                                                     */
/* ------------------------------------------------------------------ */

/* Insertion sort, because a core holds a handful of program counters and a
 * comparison callback per element would cost more than the ordering does. */
static void sort_core(uint32_t * core, size_t count) {
  for (size_t i = 1; i < count; i++) {
    uint32_t value = core[i];
    size_t j = i;
    while (j && core[j - 1] > value) {
      core[j] = core[j - 1];
      j--;
    }
    core[j] = value;
  }
}

static uint32_t hash_core(const uint32_t * core, size_t count, int flag) {
  uint32_t h = 2166136261u ^ (uint32_t)flag;
  for (size_t i = 0; i < count; i++) {
    h = (h ^ core[i]) * 16777619u;
  }
  return h ? h : 1u;
}

static int cache_init(
    const GRX_Allocator * allocator, Cache * cache, size_t slots) {
  memset(cache, 0, sizeof *cache);
  cache->mask = slots - 1;
  cache->table = gcu_allocator_malloc(allocator, slots * sizeof *cache->table);
  if (!cache->table) {
    return 0;
  }
  for (size_t i = 0; i < slots; i++) {
    cache->table[i] = STATE_NONE;
  }
  return 1;
}

static void cache_clear(Cache * cache) {
  cache->count = 0;
  cache->pool_count = 0;
  for (size_t i = 0; i <= cache->mask; i++) {
    cache->table[i] = STATE_NONE;
  }
}

static void cache_free(const GRX_Allocator * allocator, Cache * cache) {
  gcu_allocator_free(allocator, cache->states);
  gcu_allocator_free(allocator, cache->trans);
  gcu_allocator_free(allocator, cache->flag);
  gcu_allocator_free(allocator, cache->pool);
  gcu_allocator_free(allocator, cache->table);
}

/* Intern the closure currently in `build` and return its state index, or
 * STATE_NONE when the cache is full and the caller must flush. */
static uint32_t intern(GRX_Dfa * dfa, Cache * cache) {
  sort_core(dfa->build, dfa->build_count);
  uint32_t h = hash_core(dfa->build, dfa->build_count, dfa->build_flag);
  size_t slot = h & cache->mask;
  while (cache->table[slot] != STATE_NONE) {
    State * state = &cache->states[cache->table[slot]];
    if (state->count == dfa->build_count
        && cache->flag[cache->table[slot]] == dfa->build_flag
        && memcmp(&cache->pool[state->first], dfa->build,
               dfa->build_count * sizeof *dfa->build)
            == 0) {
      return cache->table[slot];
    }
    slot = (slot + 1) & cache->mask;
  }
  if (cache->count >= dfa->max_states) {
    return STATE_NONE;
  }
  if (cache->count == cache->capacity) {
    size_t want = cache->capacity ? cache->capacity * 2 : 64;
    State * grown = gcu_allocator_realloc(
        dfa->allocator, cache->states, want * sizeof *grown);
    if (!grown) {
      return STATE_NONE;
    }
    cache->states = grown;
    uint32_t * rows = gcu_allocator_realloc(
        dfa->allocator, cache->trans, want * 256 * sizeof *rows);
    if (!rows) {
      return STATE_NONE;
    }
    cache->trans = rows;
    uint8_t * flags = gcu_allocator_realloc(dfa->allocator, cache->flag, want);
    if (!flags) {
      return STATE_NONE;
    }
    cache->flag = flags;
    cache->capacity = want;
  }
  if (cache->pool_count + dfa->build_count > cache->pool_capacity) {
    size_t want = cache->pool_capacity ? cache->pool_capacity * 2 : 1024;
    while (want < cache->pool_count + dfa->build_count) {
      want *= 2;
    }
    uint32_t * grown = gcu_allocator_realloc(
        dfa->allocator, cache->pool, want * sizeof *grown);
    if (!grown) {
      return STATE_NONE;
    }
    cache->pool = grown;
    cache->pool_capacity = want;
  }
  uint32_t index = (uint32_t)cache->count++;
  State * state = &cache->states[index];
  state->first = (uint32_t)cache->pool_count;
  state->count = (uint32_t)dfa->build_count;
  cache->flag[index] = (uint8_t)dfa->build_flag;
  memcpy(&cache->pool[cache->pool_count], dfa->build,
      dfa->build_count * sizeof *dfa->build);
  cache->pool_count += dfa->build_count;
  uint32_t * row = &cache->trans[(size_t)index * 256];
  for (size_t b = 0; b < 256; b++) {
    row[b] = STATE_UNKNOWN;
  }
  cache->table[slot] = index;
  return index;
}

/* ------------------------------------------------------------------ */
/* Running.                                                             */
/* ------------------------------------------------------------------ */

static uint32_t state_for(GRX_Dfa * dfa, Cache * cache, const uint32_t * from,
    size_t count) {
  build_reset(dfa);
  close_forward(dfa, from, count);
  return intern(dfa, cache);
}

/*
 * The transition, filled on demand. Split in two on purpose: the hit is four
 * instructions and wants to be inside the caller's loop, and the miss builds
 * a closure and interns it and must not be. Left as one function it was not
 * inlined at -O2 and the byte loop cost 36 instructions where it now costs
 * about eight.
 *
 * `trans` holds a real index or STATE_UNKNOWN, and STATE_UNKNOWN and
 * STATE_NONE are the top two values, so one unsigned compare separates a hit
 * from everything else.
 */
static uint32_t fill(GRX_Dfa * dfa, Cache * cache, uint32_t index,
    unsigned byte, int inject_entry) {
  const State * state = &cache->states[index];
  size_t depth = 0;
  for (uint32_t i = 0; i < state->count; i++) {
    uint32_t pc = cache->pool[state->first + i];
    const Node * node = &dfa->nodes[pc];
    if (node->kind == KIND_CONSUME
        && (node->bytes[byte >> 3] & (uint8_t)(1u << (byte & 7u)))) {
      dfa->work[depth++] = node->succ[0];
    }
  }
  /* An unanchored run is the anchored one with the entry point added back at
   * every position, which is the same thing as the `.*?` prefix a compiler
   * would otherwise have to emit. */
  if (inject_entry) {
    dfa->work[depth++] = 0;
  }
  uint32_t next = state_for(dfa, cache, dfa->work, depth);
  if (next != STATE_NONE) {
    cache->trans[(size_t)index * 256 + byte]
        = next | (cache->flag[next] ? STATE_ACCEPT : 0u);
  }
  return next;
}

/* Is there any match in [from, length)? 1, 0, or -1 for "gave up". */
int grx_dfa_exists(GRX_Dfa * dfa, const char * subject, size_t length,
    size_t from) {
  Cache * cache = &dfa->unanchored;
  for (int attempt = 0; attempt < 2; attempt++) {
    uint32_t entry = 0;
    uint32_t index = state_for(dfa, cache, &entry, 1);
    if (index == STATE_NONE) {
      break;
    }
    if (cache->flag[index]) {
      return 1;
    }
    size_t at = from;
    int full = 0;
    while (at < length) {
      /* Re-read on every re-entry: interning a state can reallocate both. */
      const uint32_t * trans = cache->trans;
      while (at < length) {
        uint32_t next
            = trans[(size_t)index * 256 + (unsigned char)subject[at]];
        if (next >= STATE_UNKNOWN) {
          break;
        }
        at++;
        if (next & STATE_ACCEPT) {
          return 1;
        }
        index = next;
      }
      if (at >= length) {
        break;
      }
      uint32_t next
          = fill(dfa, cache, index, (unsigned char)subject[at], 1);
      if (next == STATE_NONE) {
        full = 1;
        break;
      }
      index = next;
      at++;
      if (cache->flag[index]) {
        return 1;
      }
    }
    if (!full) {
      return 0;
    }
    /* Lazy means the cache is a cache: when it fills, throw it away and go
     * again. A second fill on the same subject is a program whose state set
     * is genuinely too large, and that is what the caller falls back for. */
    cache_clear(cache);
    dfa->flushes++;
  }
  return -1;
}

/* The longest match anchored at `at`, or SIZE_MAX for none. `budget` is
 * charged one per byte read and stops the run when it runs out. */
static size_t longest_from(GRX_Dfa * dfa, const char * subject, size_t length,
    size_t at, size_t * budget, int * gave_up) {
  Cache * cache = &dfa->forward;
  /* Built once, not once per start position: the anchored scan asks for it
   * at every candidate, and rebuilding a closure and interning it there was
   * most of what a short run cost. */
  if (dfa->anchored_start == STATE_NONE) {
    uint32_t entry = 0;
    dfa->anchored_start = state_for(dfa, cache, &entry, 1);
  }
  uint32_t index = dfa->anchored_start;
  if (index == STATE_NONE) {
    *gave_up = 1;
    return SIZE_MAX;
  }
  size_t best = cache->flag[index] ? at : SIZE_MAX;
  size_t pos = at;
  while (pos < length) {
    const uint32_t * trans = cache->trans;
    const State * states = cache->states;
    while (pos < length && *budget) {
      uint32_t next
          = trans[(size_t)index * 256 + (unsigned char)subject[pos]];
      if (next >= STATE_UNKNOWN) {
        break;
      }
      (*budget)--;
      pos++;
      if (next & STATE_ACCEPT) {
        index = next & STATE_INDEX;
        best = pos;
        continue;
      }
      index = next;
      if (!states[index].count) {
        return best;   /* Nothing alive: no longer match can start here. */
      }
    }
    if (pos >= length) {
      break;
    }
    if (!*budget) {
      *gave_up = 1;
      return SIZE_MAX;
    }
    (*budget)--;
    uint32_t next = fill(dfa, cache, index, (unsigned char)subject[pos], 0);
    if (next == STATE_NONE) {
      *gave_up = 1;
      return SIZE_MAX;
    }
    index = next;
    pos++;
    if (cache->flag[index]) {
      best = pos;
    }
    else if (!cache->states[index].count) {
      return best;
    }
  }
  return best;
}

int grx_dfa_search_skipping(GRX_Dfa * dfa, const char * subject, size_t length,
    size_t from, size_t budget, size_t * out_steps, size_t * out_begin,
    size_t * out_end) {
  /* The precheck exec.c runs before it dispatches to any engine at all. The
   * first sweep of this left it out and read as a 12,000x regression on
   * `(a+)(b+)` over a subject with no `b` - a search the library answers
   * without an engine, measured against a prototype that ran one. A harness
   * that models the engine but not what stands in front of it is comparing
   * against a library that does not exist. */
  if (dfa->program->required_literal_length
      && !grx_exec_find(subject + from, length - from,
             dfa->program->required_literal,
             dfa->program->required_literal_length)) {
    return 0;
  }
  size_t left = budget;
  size_t at = from;
  for (;;) {
    at = grx_exec_skip_to_first_byte(dfa->program, subject, length, at);
    if (at > length) {
      if (out_steps) {
        *out_steps = budget - left;
      }
      return 0;
    }
    int gave_up = 0;
    size_t end = longest_from(dfa, subject, length, at, &left, &gave_up);
    if (gave_up) {
      /* The anchored scan is exact but quadratic, and the budget is what
       * stops it being slow. Before handing the search back, ask the one
       * question that is linear by construction: the unanchored automaton
       * carries every start at once, so one pass says whether any match
       * exists at all. When it says no - which is the case that made the
       * anchored scan expensive in the first place, since nothing it tried
       * ever accepted - that is the whole answer. */
      int any = grx_dfa_exists(dfa, subject, length, at);
      if (out_steps) {
        *out_steps = budget - left + (length - at);
      }
      if (any == 0) {
        return 0;
      }
      return -1;
    }
    if (end != SIZE_MAX) {
      if (out_steps) {
        *out_steps = budget - left;
      }
      *out_begin = at;
      *out_end = end;
      return 1;
    }
    if (at == length) {
      if (out_steps) {
        *out_steps = budget - left;
      }
      return 0;
    }
    at++;
  }
}

int grx_dfa_search(GRX_Dfa * dfa, const char * subject, size_t length,
    size_t from, size_t * out_begin, size_t * out_end) {
  /* One linear pass settles the common case outright, and it is the only
   * part of this that is linear by construction. */
  int any = grx_dfa_exists(dfa, subject, length, from);
  if (any <= 0) {
    return any;
  }
  /* Then the leftmost start, by asking each position in turn. Every answer
   * this gives is exactly what the anchored automaton says, so it is the
   * POSIX leftmost-longest match by definition rather than by an argument
   * about earliest ends - but it is quadratic in the worst case, so it runs
   * on a budget and hands back -1 rather than a slow answer.
   */
  size_t budget = length * 2 + 64;
  for (size_t at = from; at <= length; at++) {
    int gave_up = 0;
    size_t end = longest_from(dfa, subject, length, at, &budget, &gave_up);
    if (gave_up) {
      /* The anchored scan is exact but quadratic, and the budget is what
       * stops it being slow. Before handing the search back, ask the one
       * question that is linear by construction: the unanchored automaton
       * carries every start at once, so one pass says whether any match
       * exists at all. When it says no - which is the case that made the
       * anchored scan expensive in the first place, since nothing it tried
       * ever accepted - that is the whole answer. */
      int any = grx_dfa_exists(dfa, subject, length, at);
      if (any == 0) {
        return 0;
      }
      return -1;
    }
    if (end != SIZE_MAX) {
      *out_begin = at;
      *out_end = end;
      return 1;
    }
  }
  return 0;
}

/* ------------------------------------------------------------------ */

GRX_Dfa * grx_dfa_create(const GRX_Allocator * allocator,
    const GRX_Program * program, size_t max_states) {
  if (!grx_dfa_eligible(program)) {
    return NULL;
  }
  if (!allocator) {
    allocator = grx_allocator_default();
  }
  GRX_Dfa * dfa = gcu_allocator_calloc(allocator, 1, sizeof *dfa);
  if (!dfa) {
    return NULL;
  }
  dfa->allocator = allocator;
  dfa->program = program;
  dfa->max_states = max_states ? max_states : 1024;
  dfa->anchored_start = STATE_NONE;
  if (!lift(dfa)) {
    grx_dfa_free(dfa);
    return NULL;
  }
  dfa->work = gcu_allocator_malloc(
      allocator, (dfa->node_count + 1) * sizeof *dfa->work);
  dfa->build = gcu_allocator_malloc(
      allocator, (dfa->node_count + 1) * sizeof *dfa->build);
  dfa->seen = gcu_allocator_malloc(allocator, dfa->node_count + 1);
  if (!dfa->work || !dfa->build || !dfa->seen
      || !cache_init(allocator, &dfa->forward, 1024)
      || !cache_init(allocator, &dfa->unanchored, 1024)) {
    grx_dfa_free(dfa);
    return NULL;
  }
  memset(dfa->seen, 0, dfa->node_count + 1);
  return dfa;
}

void grx_dfa_free(GRX_Dfa * dfa) {
  if (!dfa) {
    return;
  }
  const GRX_Allocator * allocator = dfa->allocator;
  gcu_allocator_free(allocator, dfa->nodes);
  gcu_allocator_free(allocator, dfa->work);
  gcu_allocator_free(allocator, dfa->build);
  gcu_allocator_free(allocator, dfa->seen);
  cache_free(allocator, &dfa->forward);
  cache_free(allocator, &dfa->unanchored);
  gcu_allocator_free(allocator, dfa);
}

size_t grx_dfa_states(const GRX_Dfa * dfa) {
  return dfa ? dfa->forward.count + dfa->unanchored.count : 0;
}

size_t grx_dfa_flushes(const GRX_Dfa * dfa) {
  return dfa ? dfa->flushes : 0;
}
