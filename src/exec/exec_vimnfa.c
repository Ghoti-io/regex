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
 * The thread-list executor behind the `vim-nfa` profile.
 *
 * A thread is a state, the group boundaries it has seen, and possibly one
 * postponed assertion. The list for each input position is walked in order,
 * and the order is the whole of the semantics: a thread that matches is
 * recorded and everything behind it is dropped, a thread that survives puts
 * its successors on the next list in the order it met them, and a thread
 * that passes an assertion is replaced *in place* by what follows, so that
 * those successors are walked before anything still waiting.
 *
 * Three rules make the order what it is, and each is kept exactly:
 *
 * - **A state is kept once per list**, unless the new thread differs from the
 *   one already there in where its groups started or in the assertion it
 *   carries. Only the first of two equal threads survives, so a thread that
 *   arrives earlier shuts out one that arrives later. Open-group markers are
 *   states in their own right, and a marker that is shut out takes its whole
 *   continuation with it.
 * - **A postponed assertion travels with the thread** until the thread
 *   reaches a place where the match could end, and is evaluated there, from
 *   the position it was met at. A thread carries at most one; a second
 *   assertion on the same thread is evaluated at once.
 * - **Groups are carried as a count and an array**, and a group at or beyond
 *   the count is not there. A recursion that evaluates an assertion body is
 *   handed a scratch copy of the groups and returns through the same
 *   scratch, which is where a postponed assertion's captures are read back
 *   from when it is finally resolved.
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <stdlib.h>
#include <string.h>

#include "../compile/compile_internal.h"
#include "../unicode/unicode_internal.h"
#include "exec_internal.h"
#include "vimnfa_internal.h"

/** Most threads a list may hold before the run is stopped. */
#define LIST_LIMIT ((size_t)1 << 20)
/** How deep the epsilon closure may recurse. */
#define CLOSURE_DEPTH 5000u
/** How deeply assertion bodies may nest. */
#define BODY_DEPTH 16u

#define NONE GRX_VIMNFA_NONE

typedef struct Subs {
  int32_t st[10];
  int32_t en[10];
  int32_t n;
} Subs;

typedef struct Pim {
  uint32_t state; ///< The assertion, or NONE for no postponed assertion.
  int32_t end;    ///< Where it was met.
  int8_t res;     ///< -1 not yet evaluated, 0 failed, 1 held.
  int32_t nn;     ///< Groups its body left.
  int32_t gst[10];
  int32_t gen[10];
} Pim;

typedef struct Thread {
  uint32_t state;
  Subs subs;
  Pim pim;
  int32_t cnt; ///< SKIP: characters still to consume.
} Thread;

typedef struct List {
  Thread * t;
  size_t n;
  size_t cap;
  uint64_t id;
  int has_pim;
} List;

typedef struct Run {
  const GRX_VimNfa * nfa;
  const GRX_Allocator * allocator;
  const unsigned char * text;
  int32_t len;
  uint64_t * stamp;
  uint64_t listid;
  Subs m;          ///< The scratch every recursion shares.
  int32_t lastend; ///< Where the most recent match ended.
  int curc;        ///< The character at the position being walked; 0 at end.
  int bref;        ///< The pattern uses a back-reference.
  int anchored;    ///< Do not start a match anywhere but where asked.
  size_t steps;
  size_t max_steps;
  uint32_t body_depth;
  uint32_t closure_depth;
  size_t live;     ///< Scratch bytes held now.
  size_t cap;      ///< The cap on them; 0 for none.
  GRX_Result failure;
  GRX_Diag diag;
} Run;

/* ---- scratch accounting ---- */

/**
 * Take `size` bytes of scratch, or refuse.
 *
 * Every byte this engine holds goes through here so that the cap on live
 * scratch is a cap, not a hope: the check comes before the allocation, and a
 * refusal is the limit's own diagnostic.
 */
static void * scratch_alloc(Run * run, size_t size) {
  if (run->cap && (size > run->cap || run->live > run->cap - size)) {
    run->failure = GRX_ERR_LIMIT;
    run->diag = GRX_DIAG_LIMIT_MATCH_MEMORY;
    return NULL;
  }
  void * block = gcu_allocator_calloc(run->allocator, 1, size ? size : 1);
  if (!block) {
    run->failure = GRX_ERR_OOM;
    run->diag = GRX_DIAG_OUT_OF_MEMORY;
    return NULL;
  }
  run->live += size;
  return block;
}

/** Grow a block from `old_size` to `new_size`; the old block survives a refusal. */
static void * scratch_grow(
    Run * run, void * block, size_t old_size, size_t new_size) {
  // The replacement counts as the difference: the old block is released as
  // the new one is taken, and the cap is on what is live, not on the copy
  // space an allocator needs to move it.
  size_t others = run->live - old_size;
  if (run->cap && (new_size > run->cap || others > run->cap - new_size)) {
    run->failure = GRX_ERR_LIMIT;
    run->diag = GRX_DIAG_LIMIT_MATCH_MEMORY;
    return NULL;
  }
  void * grown = gcu_allocator_realloc(run->allocator, block, new_size);
  if (!grown) {
    run->failure = GRX_ERR_OOM;
    run->diag = GRX_DIAG_OUT_OF_MEMORY;
    return NULL;
  }
  run->live = run->live - old_size + new_size;
  return grown;
}

static void scratch_free(Run * run, void * block, size_t size) {
  if (block) {
    gcu_allocator_free(run->allocator, block);
    run->live -= size;
  }
}

/* ---- the group set ---- */

static void subs_init(Subs * s) {
  for (int k = 0; k < 10; k++) {
    s->st[k] = -1;
    s->en[k] = -1;
  }
  s->n = 0;
}

/** A thread's copy: the groups it has, and nothing in the slots beyond. */
static void subs_copy(Subs * to, const Subs * from) {
  subs_init(to);
  int32_t k = from->n > 1 ? from->n : 1;
  for (int32_t i = 0; i < k; i++) {
    to->st[i] = from->st[i];
    to->en[i] = from->en[i];
  }
  to->n = from->n;
}

/** The scratch's copy: the slots beyond the count keep what they held. */
static void subs_assign(Subs * to, const Subs * from) {
  int32_t k = from->n > 1 ? from->n : 1;
  for (int32_t i = 0; i < k; i++) {
    to->st[i] = from->st[i];
    to->en[i] = from->en[i];
  }
  to->n = from->n;
}

static int32_t max32(int32_t a, int32_t b) {
  return a > b ? a : b;
}

/* ---- the thread list ---- */

static int list_push(Run * run, List * list, const Thread * thread) {
  if (list->n == list->cap) {
    if (list->cap >= LIST_LIMIT) {
      run->failure = GRX_ERR_LIMIT;
      run->diag = GRX_DIAG_LIMIT_MATCH_MEMORY;
      return 0;
    }
    size_t cap = list->cap ? list->cap * 2u : 32u;
    Thread * t = scratch_grow(
        run, list->t, list->cap * sizeof(*t), cap * sizeof(*t));
    if (!t) {
      return 0;
    }
    list->t = t;
    list->cap = cap;
  }
  list->t[list->n++] = *thread;
  return 1;
}

static int stored(GRX_VimNfaKind kind) {
  switch (kind) {
    case GRX_VN_CHAR:
    case GRX_VN_SET:
    case GRX_VN_ANY:
    case GRX_VN_BOL:
    case GRX_VN_EOL:
    case GRX_VN_MATCH:
    case GRX_VN_INV:
    case GRX_VN_MOPEN:
    case GRX_VN_NOPEN:
    case GRX_VN_ENDINV:
    case GRX_VN_BREF:
    case GRX_VN_SKIP:
    case GRX_VN_PAT:
    case GRX_VN_ENDPAT:
    case GRX_VN_ZSTART:
      return 1;
    default:
      return 0;
  }
}

static int pim_equal(const Pim * a, const Pim * b) {
  if (a->state == NONE || b->state == NONE) {
    return a->state == b->state;
  }
  return a->state == b->state && a->end == b->end;
}

/** Whether two threads of one state have started their groups alike. */
static int key_equal(const Run * run, const Subs * a, const Subs * b) {
  int32_t n = max32(max32(a->n, b->n), 1);
  for (int32_t k = 0; k < n; k++) {
    if (a->st[k] != b->st[k]) {
      return 0;
    }
    if (run->bref && a->en[k] != b->en[k]) {
      return 0;
    }
  }
  return 1;
}

/** Whether a state is in a list already, with groups that start alike. */
static int state_in_list(
    const Run * run, const List * list, uint32_t index, const Subs * subs) {
  if (index == NONE || run->stamp[index] != list->id) {
    return 0;
  }
  if (!run->bref) {
    return 1;
  }
  for (size_t k = 0; k < list->n; k++) {
    if (list->t[k].state == index && key_equal(run, subs, &list->t[k].subs)) {
      return 1;
    }
  }
  return 0;
}

/**
 * Add a state, and what it leads to, to a list.
 *
 * `here` is the index of the thread being replaced when the state is being
 * added to the list that is being walked, and -1 for the list that follows.
 * `cnt` belongs to the state added first, which is the only one that can be
 * a run of skipped characters.
 */
static int addstate(Run * run, List * list, uint32_t index, Subs * subs,
    const Pim * pim, int32_t pos, int32_t off, ptrdiff_t here, int32_t cnt) {
  if (index == NONE) {
    return 1;
  }
  if (++run->closure_depth > CLOSURE_DEPTH) {
    run->closure_depth--;
    run->failure = GRX_ERR_LIMIT;
    run->diag = GRX_DIAG_LIMIT_STEPS;
    return 0;
  }
  const GRX_VimNfaState * s = &run->nfa->states[index];
  int ok = 1;

  if (s->kind == GRX_VN_SPLIT) {
    ok = addstate(run, list, s->out, subs, pim, pos, off, here, 0)
        && addstate(run, list, s->out1, subs, pim, pos, off, here, 0);
    run->closure_depth--;
    return ok;
  }

  int keep = stored((GRX_VimNfaKind)s->kind);
  if (keep && s->kind != GRX_VN_SKIP && run->stamp[index] == list->id) {
    if (!run->bref && !pim && !list->has_pim && s->kind != GRX_VN_MATCH) {
      if (here < 0) {
        run->closure_depth--;
        return 1;
      }
      for (ptrdiff_t k = 0; k < here; k++) {
        if (list->t[k].state == index) {
          run->closure_depth--;
          return 1;
        }
      }
    }
    for (size_t k = 0; k < list->n; k++) {
      const Thread * th = &list->t[k];
      if (th->state != index) {
        continue;
      }
      Pim none = {.state = NONE};
      if (!pim_equal(&th->pim, pim ? pim : &none)) {
        continue;
      }
      // The thread being replaced is still in the list, and the groups being
      // walked are its own: Vim hands the add a pointer into the list, so a
      // state the walk comes back to is compared with the thread it came from
      // and found equal to it. That is what stops a loop whose body can match
      // nothing from taking a second, empty, iteration after an assertion.
      if ((ptrdiff_t)k == here || key_equal(run, subs, &th->subs)) {
        run->closure_depth--;
        return 1;
      }
    }
  }
  if (s->kind == GRX_VN_BOL && pos > 0 && run->curc != 0) {
    run->closure_depth--;
    return 1;
  }
  if (keep) {
    Thread th;
    th.state = index;
    subs_copy(&th.subs, subs);
    if (pim) {
      th.pim = *pim;
    }
    else {
      memset(&th.pim, 0, sizeof(th.pim));
      th.pim.state = NONE;
    }
    th.cnt = cnt;
    // A run of skipped characters is stamped like any other state - the
    // check for an atomic group looks for it - though it is never refused
    // for being there already.
    run->stamp[index] = list->id;
    if (!list_push(run, list, &th)) {
      run->closure_depth--;
      return 0;
    }
    if (pim) {
      list->has_pim = 1;
    }
  }

  switch ((GRX_VimNfaKind)s->kind) {
    case GRX_VN_MOPEN: {
      int g = s->group;
      int32_t saved_start = subs->st[g];
      int32_t saved_n = subs->n;
      for (int32_t q = subs->n; q < g; q++) {
        subs->st[q] = -1;
        subs->en[q] = -1;
      }
      subs->st[g] = pos + off;
      subs->n = max32(subs->n, g + 1);
      ok = addstate(run, list, s->out, subs, pim, pos, off, here, 0);
      subs->st[g] = saved_start;
      subs->n = saved_n;
      break;
    }
    case GRX_VN_ZSTART: {
      int32_t saved_start = subs->st[0];
      subs->st[0] = pos + off;
      ok = addstate(run, list, s->out, subs, pim, pos, off, here, 0);
      subs->st[0] = saved_start;
      break;
    }
    case GRX_VN_ZEND: {
      int32_t saved_end = subs->en[0];
      subs->en[0] = pos + off;
      ok = addstate(run, list, s->out, subs, pim, pos, off, here, 0);
      subs->en[0] = saved_end;
      break;
    }
    case GRX_VN_MCLOSE: {
      int g = s->group;
      int32_t saved_end = subs->en[g];
      int32_t saved_n = subs->n;
      if (g != 0 || !run->nfa->has_ze || subs->en[0] < 0) {
        subs->en[g] = pos + off;
      }
      subs->n = max32(subs->n, g + 1);
      ok = addstate(run, list, s->out, subs, pim, pos, off, here, 0);
      subs->en[g] = saved_end;
      subs->n = saved_n;
      break;
    }
    case GRX_VN_NOPEN:
    case GRX_VN_NCLOSE:
    case GRX_VN_EMPTY:
      ok = addstate(run, list, s->out, subs, pim, pos, off, here, 0);
      break;
    default:
      break;
  }
  run->closure_depth--;
  return ok;
}

/**
 * Replace the thread being walked by what follows an assertion.
 *
 * What is added goes ahead of everything not yet walked, which is the
 * property that reorders threads. Returns the index to resume from, or -2
 * on failure.
 */
static ptrdiff_t addstate_here(Run * run, List * list, uint32_t index,
    Subs * subs, const Pim * pim, int32_t pos, ptrdiff_t at, int32_t cnt) {
  size_t before = list->n;
  if (!addstate(run, list, index, subs, pim, pos, 0, at, cnt)) {
    return -2;
  }
  size_t added = list->n - before;
  if (added == 0 || (size_t)at == before - 1u) {
    return at;
  }
  Thread * block = scratch_alloc(run, added * sizeof(*block));
  if (!block) {
    return -2;
  }
  memcpy(block, &list->t[before], added * sizeof(*block));
  memmove(&list->t[(size_t)at + added], &list->t[(size_t)at + 1u],
      (before - (size_t)at - 1u) * sizeof(*block));
  memcpy(&list->t[at], block, added * sizeof(*block));
  list->n = before - 1u + added;
  scratch_free(run, block, added * sizeof(*block));
  return at - 1;
}

/* ---- assertions ---- */

static int exec_body(Run * run, uint32_t start, int32_t pos0, const Subs * seed,
    int mode, int32_t endpos, Subs * match_out, int32_t * end_out);

/** Hand the thread's groups to the scratch. */
static void preload(Run * run, const Subs * subs) {
  for (int k = 1; k < subs->n; k++) {
    run->m.st[k] = subs->st[k];
    run->m.en[k] = subs->en[k];
  }
  run->m.n = max32(run->m.n, subs->n);
}

/** Run an assertion's body from `pos`. Returns 1 held, 0 failed, -1 error. */
static int run_assertion(Run * run, uint32_t state, int32_t pos, Subs * got) {
  if (run->body_depth >= BODY_DEPTH) {
    run->failure = GRX_ERR_LIMIT;
    run->diag = GRX_DIAG_LIMIT_STEPS;
    return -1;
  }
  int saved = run->curc;
  Subs seed;
  subs_copy(&seed, &run->m);
  int32_t end = 0;
  const GRX_VimNfaState * assertion = &run->nfa->states[state];
  int state_before = assertion->before;
  uint32_t start_state = assertion->out;
  int32_t begin = pos;
  if (state_before) {
    begin = assertion->width <= 0 ? 0
        : (pos > assertion->width ? pos - assertion->width : 0);
    while (begin > 0 && (run->text[begin] & 0xC0u) == 0x80u) {
      begin--;
    }
  }
  run->body_depth++;
  int r = exec_body(run, start_state, begin, &seed, state_before ? 2 : 0,
      state_before ? pos : -1, got, &end);
  run->body_depth--;
  run->curc = saved;
  return r;
}

/**
 * Evaluate an assertion now. Returns 1 and fills `out` with the thread's
 * groups after it, 0 if it does not hold, -1 on failure.
 */
static int eval_direct(Run * run, uint32_t state, int32_t pos,
    const Subs * subs, Subs * out) {
  const GRX_VimNfaState * s = &run->nfa->states[state];
  int32_t saved_n = run->m.n;
  preload(run, subs);
  Subs got;
  int r = run_assertion(run, state, pos, &got);
  if (r < 0) {
    return -1;
  }
  int ok = (r != 0) != (s->neg != 0);
  if (!ok) {
    run->m.n = saved_n;
    return 0;
  }
  if (r && !s->neg) {
    subs_assign(&run->m, &got);
  }
  subs_copy(out, subs);
  for (int k = 1; k < 10; k++) {
    out->st[k] = run->m.st[k];
    out->en[k] = run->m.en[k];
  }
  out->n = max32(out->n, run->m.n);
  if (run->nfa->has_ze && run->m.en[0] >= 0) {
    out->en[0] = run->m.en[0];
  }
  run->m.n = saved_n;
  return 1;
}

/** Resolve a postponed assertion against a thread. Same return as above. */
static int resolve_pim(Run * run, Pim * pim, const Subs * subs, Subs * out) {
  const GRX_VimNfaState * s = &run->nfa->states[pim->state];
  if (pim->res < 0) {
    Subs got;
    int r = run_assertion(run, pim->state, pim->end, &got);
    if (r < 0) {
      return -1;
    }
    int ok = (r != 0) != (s->neg != 0);
    if (r && !s->neg) {
      subs_assign(&run->m, &got);
    }
    pim->res = (int8_t)ok;
    for (int k = 0; k < 10; k++) {
      pim->gst[k] = run->m.st[k];
      pim->gen[k] = run->m.en[k];
    }
    pim->nn = run->m.n;
  }
  if (!pim->res) {
    return 0;
  }
  subs_copy(out, subs);
  for (int k = 1; k < pim->nn; k++) {
    out->st[k] = pim->gst[k];
    out->en[k] = pim->gen[k];
  }
  out->n = max32(out->n, pim->nn);
  return 1;
}

/* ---- the walk ---- */

/**
 * Decode the character at `pos`: its code point, and how many bytes it takes.
 * The text is valid UTF-8, which grx_vimnfa_applies() checked.
 */
static int32_t decode_at(const Run * run, int32_t pos, int * out_cp) {
  const unsigned char * t = run->text + pos;
  int32_t left = run->len - pos;
  if (left <= 0) {
    *out_cp = 0;
    return 0;
  }
  if (t[0] < 0x80u) {
    *out_cp = t[0];
    return 1;
  }
  if (t[0] < 0xE0u && left >= 2) {
    *out_cp = (int)(((t[0] & 0x1Fu) << 6) | (t[1] & 0x3Fu));
    return 2;
  }
  if (t[0] < 0xF0u && left >= 3) {
    *out_cp = (int)(((t[0] & 0x0Fu) << 12) | ((t[1] & 0x3Fu) << 6)
        | (t[2] & 0x3Fu));
    return 3;
  }
  if (left >= 4) {
    *out_cp = (int)(((t[0] & 0x07u) << 18) | ((t[1] & 0x3Fu) << 12)
        | ((t[2] & 0x3Fu) << 6) | (t[3] & 0x3Fu));
    return 4;
  }
  *out_cp = t[0];
  return 1;
}

/** Whether a class state accepts a code point. */
static int member_of(
    const GRX_VimNfa * nfa, const GRX_VimNfaState * s, int cp) {
  if (cp < 128) {
    return (s->set[cp >> 3] >> (cp & 7)) & 1;
  }
  int found = 0;
  for (uint32_t k = 0; k < s->nranges && !found; k++) {
    const GRX_VimNfaRange * r = &nfa->ranges[s->ranges + k];
    found = (uint32_t)cp >= r->low && (uint32_t)cp <= r->high;
  }
  return s->invert ? !found : found;
}


static int charge(Run * run) {
  run->steps++;
  if (run->max_steps && run->steps > run->max_steps) {
    run->failure = GRX_ERR_LIMIT;
    run->diag = GRX_DIAG_LIMIT_STEPS;
    return 0;
  }
  return 1;
}

/**
 * Advance a set of threads over the subject.
 *
 * `top` is the whole-pattern run: it ends at MATCH and, unless anchored,
 * starts a new thread at every position until a match has been found. A body
 * run ends at the end of the assertion and starts nothing.
 *
 * Returns 1 and fills `match_out` and `end_out` when a match was recorded,
 * 0 when there was none, -1 on failure.
 */
static int exec_body(Run * run, uint32_t start, int32_t pos0, const Subs * seed,
    int mode, int32_t endpos, Subs * match_out, int32_t * end_out) {
  const GRX_VimNfa * nfa = run->nfa;
  List lists[2] = {{0}, {0}};
  int flag = 0;
  int matched = 0;
  int status = 0;
  List * cur = &lists[0];
  cur->id = ++run->listid;
  int32_t pos = pos0;
  {
    int first_cp = 0;
    decode_at(run, pos, &first_cp);
    run->curc = first_cp;
  }
  Subs first;
  subs_copy(&first, seed);
  if (!addstate(run, cur, start, &first, NULL, pos, 0, -1, 0)) {
    status = -1;
    goto done;
  }

  for (;;) {
    int curc = 0;
    int32_t clen = decode_at(run, pos, &curc);
    run->curc = curc;
    List * nxt = &lists[flag ^ 1];
    nxt->n = 0;
    nxt->has_pim = 0;
    nxt->id = ++run->listid;
    if (cur->n == 0) {
      break;
    }
    ptrdiff_t i = 0;
    int hit = 0;
    while ((size_t)i < cur->n) {
      if (!charge(run)) {
        status = -1;
        goto done;
      }
      Thread th = cur->t[i];
      const GRX_VimNfaState * s = &nfa->states[th.state];
      uint32_t add = NONE;
      int here = 0;
      int32_t skipcnt = 0;
      int has_skip = 0;
      Subs addsub;
      subs_copy(&addsub, &th.subs);
      Pim pim = th.pim;

      switch ((GRX_VimNfaKind)s->kind) {
        case GRX_VN_MATCH:
        case GRX_VN_ENDINV:
        case GRX_VN_ENDPAT:
          if (endpos >= 0 && pos != endpos) {
            break;
          }
          *match_out = th.subs;
          *end_out = pos;
          run->lastend = pos;
          matched = 1;
          if (nxt->n == 0) {
            clen = 0;
          }
          hit = 1;
          break;
        case GRX_VN_INV:
          if (th.pim.state != NONE || !s->post) {
            Subs out;
            int r = eval_direct(run, th.state, pos, &th.subs, &out);
            run->curc = curc;
            if (r < 0) {
              status = -1;
              goto done;
            }
            if (r) {
              add = nfa->states[s->out1].out;
              here = 1;
              addsub = out;
              if (add == NONE) {
                add = NONE;
              }
            }
          }
          else {
            Pim fresh;
            memset(&fresh, 0, sizeof(fresh));
            fresh.state = th.state;
            fresh.end = pos;
            fresh.res = -1;
            i = addstate_here(run, cur, nfa->states[s->out1].out, &th.subs,
                &fresh, pos, i, 0);
            if (i == -2) {
              status = -1;
              goto done;
            }
          }
          break;
        case GRX_VN_PAT: {
          uint32_t end_state = s->out1;
          uint32_t skip_state = nfa->states[end_state].out;
          uint32_t follow = nfa->states[skip_state].out;
          if (state_in_list(run, nxt, skip_state, &th.subs)
              || state_in_list(run, nxt, follow, &th.subs)
              || state_in_list(run, cur, follow, &th.subs)) {
            break;
          }
          if (run->body_depth >= BODY_DEPTH) {
            run->failure = GRX_ERR_LIMIT;
            run->diag = GRX_DIAG_LIMIT_STEPS;
            status = -1;
            goto done;
          }
          preload(run, &th.subs);
          Subs seed;
          subs_copy(&seed, &run->m);
          Subs got;
          int32_t body_end = 0;
          run->body_depth++;
          int r = exec_body(run, s->out, pos, &seed, 0, -1, &got, &body_end);
          run->body_depth--;
          run->curc = curc;
          if (r < 0) {
            status = -1;
            goto done;
          }
          if (r) {
            subs_assign(&run->m, &got);
            for (int k = 1; k < 10; k++) {
              addsub.st[k] = got.st[k];
              addsub.en[k] = got.en[k];
            }
            addsub.n = max32(addsub.n, got.n);
            int32_t length = body_end - pos;
            if (length == 0) {
              add = follow;
              here = 1;
            }
            else if (length <= clen) {
              add = follow;
            }
            else {
              add = skip_state;
              skipcnt = length - clen;
              has_skip = 1;
            }
          }
          break;
        }
        case GRX_VN_BOL:
          if (pos == 0) {
            add = s->out;
            here = 1;
          }
          break;
        case GRX_VN_EOL:
          if (curc == 0) {
            add = s->out;
            here = 1;
          }
          break;
        case GRX_VN_CHAR:
          if ((int)s->ch == curc) {
            add = s->out;
          }
          break;
        case GRX_VN_SET:
        case GRX_VN_ANY:
          if (curc != 0 && member_of(nfa, s, curc)) {
            add = s->out;
          }
          break;
        case GRX_VN_BREF: {
          int g = s->group;
          int32_t a = th.subs.st[g];
          int32_t b = th.subs.en[g];
          int32_t len = 0;
          if (th.subs.n <= g || a < 0 || b < 0) {
            len = 0;
          }
          else {
            len = b - a;
            if (len < 0) {
              len = a == pos ? len : INT32_MIN;
            }
            else if (pos + len > run->len
                || memcmp(run->text + a, run->text + pos, (size_t)len) != 0) {
              len = INT32_MIN;
            }
          }
          if (len != INT32_MIN) {
            uint32_t follow = nfa->states[s->out].out;
            if (len == 0) {
              add = follow;
              here = 1;
            }
            else if (len <= clen) {
              add = follow;
            }
            else {
              add = s->out;
              skipcnt = len - clen;
              has_skip = 1;
            }
          }
          break;
        }
        case GRX_VN_SKIP: {
          int32_t left = th.cnt - clen;
          if (left <= 0) {
            add = s->out;
          }
          else {
            add = th.state;
            skipcnt = left;
            has_skip = 1;
          }
          break;
        }
        default:
          break;
      }
      if (hit) {
        break;
      }

      if (add != NONE) {
        Pim * live = pim.state != NONE ? &pim : NULL;
        int dead = 0;
        if (live && (clen == 0 || grx_vimnfa_match_follows(nfa, add, 0))) {
          Subs out;
          int r = resolve_pim(run, live, &addsub, &out);
          run->curc = curc;
          if (r < 0) {
            status = -1;
            goto done;
          }
          if (r) {
            addsub = out;
            live = NULL;
          }
          else {
            dead = 1;
          }
        }
        if (!dead) {
          int ok;
          if (here) {
            i = addstate_here(run, cur, add, &addsub, live, pos, i, 0);
            ok = i != -2;
          }
          else {
            ok = addstate(run, nxt, add, &addsub, live, pos, clen, -1,
                has_skip ? skipcnt : 0);
          }
          if (!ok) {
            status = -1;
            goto done;
          }
        }
      }
      i++;
    }

    if (!hit && !matched && clen != 0
        && ((mode == 1 && !run->anchored)
            || (mode == 2 && pos + clen <= endpos))) {
      // The new thread starts from the scratch, groups and all: whatever
      // an earlier assertion left in it is what the thread begins with.
      Subs again;
      subs_copy(&again, &run->m);
      if (mode == 1) {
        again.st[0] = pos + clen;
      }
      again.n = max32(again.n, 1);
      if (!addstate(run, nxt, start, &again, NULL, pos, clen, -1, 0)) {
        status = -1;
        goto done;
      }
    }
    if (clen == 0) {
      break;
    }
    pos += clen;
    cur = nxt;
    flag ^= 1;
  }
  status = matched;

done:
  scratch_free(run, lists[0].t, lists[0].cap * sizeof(Thread));
  scratch_free(run, lists[1].t, lists[1].cap * sizeof(Thread));
  return status;
}

/* ---- entry points ---- */

int grx_vimnfa_applies(
    const GRX_VimNfa * nfa, const GRX_ExecRequest * request) {
  if (!nfa || !request || request->not_bol || request->not_eol
      || request->empty_rule != GRX_EMPTY_OK || request->callout
      || request->search_start != request->start
      || request->length >= (size_t)INT32_MAX / 4u
      || request->start > request->length) {
    return 0;
  }
  const unsigned char * text = (const unsigned char *)request->subject;
  size_t i = 0;
  while (i < request->length) {
    unsigned char c = text[i];
    if (c == 0) {
      return 0;
    }
    if (c < 0x80u) {
      i++;
      continue;
    }
    // A multi-byte character must be complete and must not be a composing
    // one: the automaton steps a character at a time and does not model the
    // marks that Vim attaches to the character before them.
    size_t bytes = c >= 0xF0u ? 4u : c >= 0xE0u ? 3u : c >= 0xC0u ? 2u : 0u;
    if (bytes == 0 || i + bytes > request->length) {
      return 0;
    }
    uint32_t cp = bytes == 2 ? (c & 0x1Fu) : bytes == 3 ? (c & 0x0Fu)
                                                        : (c & 0x07u);
    for (size_t k = 1; k < bytes; k++) {
      if ((text[i + k] & 0xC0u) != 0x80u) {
        return 0;
      }
      cp = (cp << 6) | (text[i + k] & 0x3Fu);
    }
    if (grx_display_cell_width(cp, 0) == 0) {
      return 0;
    }
    i += bytes;
  }
  return 1;
}

GRX_Result grx_exec_vimnfa(const GRX_ExecRequest * request, int * out_matched) {
  if (!request || !out_matched) {
    return GRX_ERR_INVALID;
  }
  *out_matched = 0;
  const GRX_Regex * regex = request->regex;
  const GRX_VimNfa * nfa = regex->vimnfa;
  if (!nfa) {
    return GRX_ERR_INVALID;
  }

  Run run;
  memset(&run, 0, sizeof(run));
  run.nfa = nfa;
  run.allocator = regex->allocator ? regex->allocator : grx_allocator_default();
  run.text = (const unsigned char *)request->subject;
  run.len = (int32_t)request->length;
  run.anchored = request->anchored;
  run.max_steps = request->limits->max_steps;
  run.failure = GRX_OK;
  run.diag = GRX_DIAG_NONE;
  subs_init(&run.m);
  run.m.st[0] = (int32_t)request->start;
  run.m.n = 1;
  for (uint32_t i = 0; i < nfa->count; i++) {
    if (nfa->states[i].kind == GRX_VN_BREF) {
      run.bref = 1;
    }
  }
  run.cap = request->limits->max_match_memory;
  size_t stamp_bytes = (nfa->count ? nfa->count : 1u) * sizeof(*run.stamp);
  run.stamp = scratch_alloc(&run, stamp_bytes);
  if (!run.stamp) {
    if (request->out_diag) {
      *request->out_diag = run.diag;
    }
    return run.failure;
  }

  Subs seed;
  subs_init(&seed);
  seed.st[0] = (int32_t)request->start;
  seed.n = 1;
  Subs result;
  int32_t end = 0;
  int status
      = exec_body(&run, nfa->start, (int32_t)request->start, &seed, 1, -1,
          &result, &end);

  if (status > 0 && request->match) {
    GRX_Match * match = request->match;
    for (size_t k = 0; k < match->count; k++) {
      match->captures[k] = (GRX_Capture) {GRX_NPOS, GRX_NPOS};
    }
    int32_t s0 = result.st[0] < 0 ? 0 : result.st[0];
    int32_t e0 = result.en[0] >= 0 ? result.en[0] : end;
    if (e0 < s0) {
      e0 = s0;
    }
    if (match->count > 0) {
      match->captures[0] = (GRX_Capture) {(size_t)s0, (size_t)e0};
    }
    for (int k = 1; k < 10 && (size_t)k < match->count; k++) {
      if (result.st[k] >= 0 && result.en[k] >= 0) {
        int32_t e = result.en[k] < result.st[k] ? result.st[k] : result.en[k];
        match->captures[k] = (GRX_Capture) {(size_t)result.st[k], (size_t)e};
      }
    }
  }
  if (status > 0) {
    *out_matched = 1;
  }
  if (request->out_steps) {
    *request->out_steps = run.steps;
  }
  if (request->out_diag) {
    *request->out_diag = run.diag;
  }
  scratch_free(&run, run.stamp, stamp_bytes);
  return status < 0 ? run.failure : GRX_OK;
}
