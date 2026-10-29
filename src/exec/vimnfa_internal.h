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
 * The thread-list automaton behind the `vim-nfa` profile.
 *
 * Vim's second regex engine does not backtrack. It advances a list of
 * threads one input position at a time, and the order of that list - not any
 * rule about repetition counts - decides which of several matches it reports.
 * The order is a consequence of three things: a state is kept at most once
 * per list unless its captures or its postponed assertion differ, a thread
 * that passes an assertion is replaced by what follows it *in front of* the
 * threads still waiting, and an assertion whose body is cheaper to test later
 * than the text that follows it is postponed until a match could end.
 * Emulating those consequences with a backtracker needs a rule per pattern
 * shape; running the automaton needs none.
 *
 * This file declares the automaton, which `compile/vimnfa.c` builds from the
 * lowered pattern and `exec/exec_vimnfa.c` runs. A pattern that uses a
 * construct the automaton does not model has no automaton, and the
 * backtracking engine runs it as before.
 */

#ifndef GHOTI_IO_GRX_SRC_EXEC_VIMNFA_INTERNAL_H
#define GHOTI_IO_GRX_SRC_EXEC_VIMNFA_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct GRX_IR;
struct GRX_ExecRequest;

/** One inclusive range of code points a class holds above ASCII. */
typedef struct GRX_VimNfaRange {
  uint32_t low;  ///< The first code point.
  uint32_t high; ///< The last.
} GRX_VimNfaRange;

/** The highest numbered group Vim keeps. */
#define GRX_VIMNFA_GROUPS 9u

/** No state. */
#define GRX_VIMNFA_NONE UINT32_MAX

/** @brief What one automaton state does. */
typedef enum {
  GRX_VN_CHAR,   ///< Consume one character equal to `ch`.
  GRX_VN_SET,    ///< Consume one character of a class: its bit is set in `set`.
  GRX_VN_ANY,    ///< Consume any character `set` accepts: Vim's `.`.
  GRX_VN_SPLIT,  ///< Try `out`, then `out1`. Never stored in a list.
  GRX_VN_MOPEN,  ///< Open group `group`. Stored, as a marker.
  GRX_VN_MCLOSE, ///< Close group `group`. Never stored.
  GRX_VN_NOPEN,  ///< Open a non-capturing group. Stored, as a marker.
  GRX_VN_NCLOSE, ///< Close it. Never stored.
  GRX_VN_BOL,    ///< The start of the subject.
  GRX_VN_EOL,    ///< The end of the subject.
  GRX_VN_EMPTY,  ///< Nothing. Never stored.
  GRX_VN_MATCH,  ///< The whole pattern matched.
  GRX_VN_INV,    ///< A look-ahead; `out` is its body, `out1` its end state.
  GRX_VN_ENDINV, ///< The end of an assertion body; `out` is what follows.
  GRX_VN_BREF,   ///< Match what group `group` holds; `out` is a SKIP.
  GRX_VN_SKIP,   ///< Consume the rest of a counted run of characters.
  GRX_VN_PAT,    ///< An atomic group; `out` is its body, `out1` its end state.
  GRX_VN_ENDPAT, ///< The end of an atomic body; `out` is a SKIP.
  GRX_VN_NEWL,   ///< A line break, consumed alone: the second arm of `\_x`.
  GRX_VN_ZSTART, ///< `\zs`. Stored, as a marker.
  GRX_VN_ZEND,   ///< `\ze`. Never stored.
} GRX_VimNfaKind;

/** @brief One state of the automaton. */
typedef struct GRX_VimNfaState {
  uint8_t kind;     ///< A @ref GRX_VimNfaKind.
  uint8_t neg;      ///< INV: a negative assertion.
  uint8_t post;     ///< INV: the postponed form, decided at compile time.
  uint8_t before;   ///< INV: a look-behind.
  uint8_t group;    ///< MOPEN, MCLOSE, BREF: the group number.
  uint32_t out;     ///< The state that follows.
  uint32_t out1;    ///< SPLIT: the second arm. INV: its end state.
  uint32_t ch;      ///< CHAR: the character.
  int32_t width;    ///< INV, look-behind: how far back it starts; <= 0 for any.
  uint8_t set[16];  ///< SET: one bit for each ASCII character it accepts.
  uint8_t invert;   ///< SET, ANY: the ranges name what it refuses.
  uint32_t ranges;  ///< SET, ANY: the first range above ASCII.
  uint32_t nranges; ///< SET, ANY: how many.
} GRX_VimNfaState;

/** @brief A built automaton. */
typedef struct GRX_VimNfa {
  const GRX_Allocator * allocator; ///< Where it came from.
  GRX_VimNfaState * states;        ///< All states.
  GRX_VimNfaRange * ranges;        ///< The ranges of every class above ASCII.
  uint32_t range_count;            ///< How many.
  uint32_t range_capacity;         ///< How many fit.
  uint32_t count;                  ///< How many there are.
  uint32_t capacity;               ///< How many fit.
  uint32_t start;                  ///< The first state of the pattern.
  uint32_t groups;                 ///< The highest group number used.
  int has_ze;                      ///< The pattern uses `\ze`.
  int has_behind;                  ///< The pattern has a look-behind.
} GRX_VimNfa;

/**
 * @brief Build the automaton for a lowered pattern, when it can be built.
 *
 * @param ir The lowered pattern.
 * @param allocator Where the automaton's storage comes from.
 * @param out_nfa Receives the automaton, or NULL when the pattern uses a
 *   construct the automaton does not model. That is not an error.
 * @return GRX_OK, or GRX_ERR_OOM.
 */
GRX_Result grx_vimnfa_build(const struct GRX_IR * ir,
    const GRX_Allocator * allocator, GRX_VimNfa ** out_nfa);

/**
 * @brief Whether the end of the match can be reached from a state without
 *   consuming input.
 *
 * @param nfa The automaton.
 * @param index The state.
 * @param depth How far the search has gone; callers pass 0.
 * @return Non-zero when it can.
 */
int grx_vimnfa_match_follows(
    const GRX_VimNfa * nfa, uint32_t index, int depth);

/**
 * @brief Release an automaton.
 *
 * @param nfa The automaton. NULL is ignored.
 */
void grx_vimnfa_free(GRX_VimNfa * nfa);

/**
 * @brief Whether the automaton may answer this search.
 *
 * The automaton works on whole-subject searches over text that has no NUL and
 * no composing character; the backtracking engine answers the rest.
 *
 * @param nfa The automaton.
 * @param request The search.
 * @return Non-zero when grx_exec_vimnfa() may be called.
 */
int grx_vimnfa_applies(
    const GRX_VimNfa * nfa, const struct GRX_ExecRequest * request);

/**
 * @brief Run the automaton.
 *
 * @param request The search; grx_vimnfa_applies() must hold for it.
 * @param out_matched Receives whether it matched.
 * @return GRX_OK, GRX_ERR_LIMIT or GRX_ERR_OOM.
 */
GRX_Result grx_exec_vimnfa(
    const struct GRX_ExecRequest * request, int * out_matched);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_EXEC_VIMNFA_INTERNAL_H
