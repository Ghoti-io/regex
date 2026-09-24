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
 * Lowering: the point at which the dialect stops existing.
 *
 * Above this line a pattern is what its text said, in the shape its dialect
 * spells it. Below it a pattern is what it means, and every dialect-dependent
 * decision has been made and spent: caseless folded literals into classes,
 * multiline chose between two assertion kinds, `\d` became a set of code
 * points, a repeat acquired the dialect's empty-iteration rule as a mode.
 * The IR that comes out mentions no dialect and no option but UTF, and
 * `make check-layering` enforces that nothing below it consults one
 * (documentation/design.md section 3).
 */

#ifndef GHOTI_IO_GRX_SRC_IR_LOWER_INTERNAL_H
#define GHOTI_IO_GRX_SRC_IR_LOWER_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/compile.h>
#include <ghoti.io/regex/core.h>
#include <stddef.h>

#include "../charclass/charclass_internal.h"
#include "../parse/parse_internal.h"
#include "../syntax/syntax_internal.h"
#include "ir_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Which named set a dialect's rule asks for.
 *
 * The sets a dialect names rather than writes out. They are built here
 * rather than generated into the Unicode tables because they are
 * *specification* constants - ECMA-262 says which four code points end a
 * line - and not facts about the UCD.
 */
typedef enum {
  GRX_SET_ASCII_DIGIT = 0,  ///< `[0-9]`.
  GRX_SET_ASCII_WORD,       ///< `[0-9A-Za-z_]`.
  GRX_SET_ASCII_SPACE,      ///< `[ \t\n\v\f\r]`.
  GRX_SET_ES_SPACE,         ///< ECMA-262 WhiteSpace plus LineTerminator.
  GRX_SET_UNICODE_DIGIT,    ///< `\p{Nd}`.
  /**
   * UTS #18 Annex C: `\p{alpha}\p{M}\p{Nd}\p{Pc}` plus the join controls.
   *
   * Spelled out rather than summarised, because a summary is where this one
   * drifted twice: this comment read `\p{L}\p{N}\p{M}\p{Pc}` until
   * 2026-09-24, and it was wrong in both halves at once - `\p{alpha}` is
   * wider than `\p{L}` by `Nl` and Other_Alphabetic, and `\p{Nd}` is
   * narrower than `\p{N}` by `Nl` and `No`. The code had one of the two
   * right. See GRX_WordSet: this is one of three, and the other two are
   * *not* reachable by narrowing it.
   */
  GRX_SET_UNICODE_WORD,
  /** `\p{L}\p{N}\p{Mn}\p{Pc}`: GRX_WORD_CATEGORIES, which is PCRE2's. */
  GRX_SET_CATEGORIES_WORD,
  /** `\p{L}\p{N}` and `_`: GRX_WORD_ALNUM, which is CPython `re`'s. */
  GRX_SET_ALNUM_WORD,
  GRX_SET_UNICODE_SPACE,    ///< `\p{White_Space}`.
  /**
   * The Perl family's `\h` and `\v`, in every mode it has.
   *
   * pcre2pattern lists both sets in full, and they are not the ASCII ones
   * widened by a property: `\h` is the space separators *plus* the tab and
   * U+00A0, and `\v` is the line separators plus U+0085. Written out for
   * that reason - a set nobody can derive has to be a set somebody wrote
   * down.
   *
   * "In every mode" is the part that was wrong until 2026-09-24. There were
   * ASCII spellings of both beside these, chosen whenever the dialect's
   * shorthands were not Unicode - so `(?a)\h` and, in the PCRE2 dialect,
   * `\h` without UCP, both refused U+00A0 where pcre2test and perl take it.
   * Neither reference narrows these two for anything. See grx_shorthand_set.
   */
  GRX_SET_UNICODE_HSPACE,
  GRX_SET_UNICODE_VSPACE,
  GRX_SET_NEWLINES_LF,      ///< `[\n]`.
  GRX_SET_NEWLINES_ES,      ///< LF, CR, U+2028 and U+2029.
  GRX_SET_NEWLINES_UNICODE, ///< The `\R` set, less the CR LF pair.
  GRX_SET_NEWLINES_CR,      ///< `[\r]`: PCRE2's `(*CR)`.
  GRX_SET_NEWLINES_CRLF,    ///< `[\r\n]`: PCRE2's `(*ANYCRLF)`.
  GRX_SET_NEWLINES_NUL,     ///< `[\0]`: PCRE2's `(*NUL)`.
  GRX_SET_COUNT             ///< Closes the enum; not a set.
} GRX_NamedSet;

/**
 * @brief Fill a class with a named set's code points.
 *
 * Adds to whatever the class already holds, so that building `[\\d\\s]` is
 * two calls.
 *
 * @param cls The class. NULL is GRX_ERR_INVALID.
 * @param set Which set.
 * @param limits Caps to apply. NULL applies none.
 * @return GRX_OK, GRX_ERR_INVALID, GRX_ERR_LIMIT, or GRX_ERR_OOM.
 */
GRX_Result grx_named_set(
    GRX_CharClass * cls, GRX_NamedSet set, const GRX_Limits * limits);

/**
 * @brief The set a dialect means by one shorthand escape.
 *
 * @param cls The class to fill. NULL is GRX_ERR_INVALID.
 * @param shorthands Which definitions the dialect uses.
 * @param word_set Which of the three Unicode word sets `\w` denotes. Read
 *   only when `shorthands` is GRX_SHORTHANDS_UNICODE and the shorthand is
 *   `\w` or `\W`; a parameter rather than a lookup inside, so that neither
 *   of the two call sites can take the width from the profile and forget to
 *   take this with it.
 * @param mongolian_space GRX_Profile::mongolian_separator_is_space. Reaches
 *   `\\h` at every width, because `\\h` is a fixed set, and `\\s` only at the
 *   Unicode one - which is what pcre2test does: `(?a)\\h` matches U+180E
 *   there and `(?a)\\s` does not.
 * @param kind Which shorthand was written; the negated spellings are
 *   resolved by the caller, which has to complement *after* folding.
 * @param limits Caps to apply. NULL applies none.
 * @return GRX_OK, GRX_ERR_UNSUPPORTED for a shorthand the definitions do not
 *   cover, or a failure code.
 */
GRX_Result grx_shorthand_set(GRX_CharClass * cls, GRX_ShorthandSet shorthands,
    GRX_WordSet word_set, int mongolian_space, GRX_ShorthandKind kind,
    const GRX_Limits * limits);

/**
 * @brief The line-terminator set a newline rule names.
 *
 * @param cls The class to fill. NULL is GRX_ERR_INVALID.
 * @param newlines Which rule.
 * @param limits Caps to apply. NULL applies none.
 * @return GRX_OK or a failure code.
 */
GRX_Result grx_newline_set(GRX_CharClass * cls, GRX_NewlineSet newlines,
    const GRX_Limits * limits);

/**
 * @brief Whether a CR LF pair is one line terminator in this convention.
 *
 * Separate from grx_newline_set() because it is not a fact about single
 * code points: `^` does not hold between the CR and the LF, and `$` before
 * a final CR LF holds where the set alone would say it does not.
 *
 * @param newlines The convention.
 * @return Non-zero for `(*CRLF)`, `(*ANYCRLF)` and `(*ANY)`.
 */
int grx_newline_has_crlf(GRX_NewlineSet newlines);

/**
 * @brief Lower a parsed pattern into the intermediate representation.
 *
 * @param pattern The parsed pattern. Never NULL here.
 * @param limits Caps to apply. Never NULL here.
 * @param allocator Allocator to use. Never NULL here.
 * @param out_error Receives the failure position and message. May be NULL.
 * @param out_ir Receives the lowered pattern on success.
 * @return GRX_OK or a failure code.
 */
GRX_Result grx_lower_pattern(const GRX_Pattern * pattern,
    const GRX_Limits * limits, const GRX_Allocator * allocator,
    GRX_Error * out_error, GRX_IR ** out_ir);

/**
 * @brief Compute what is knowable about a lowered pattern.
 *
 * documentation/design.md section 3.3. Run once, at compile time; the result
 * is what decides which engine may run the program and what a caller is told
 * by grx_regex_facts().
 *
 * Writes as well as reads: a lookbehind that lowering chose to run forwards
 * gets the body length it needs recorded on its node here, because measuring
 * a subtree is this pass and nothing else measures one.
 *
 * @param ir The lowered pattern. Never NULL here.
 * @param out_facts Receives the facts. Never NULL here.
 * @return GRX_OK or a failure code.
 */
GRX_Result grx_analyze_ir(GRX_IR * ir, GRX_Facts * out_facts);

/**
 * @brief How long a subtree's match can be, in bytes.
 *
 * The same walk grx_analyze_ir() uses, asked about one node - and asked for
 * the same reason grx_ir_can_match_empty() is: the answer is already written
 * down once, and codegen having its own would be a second place for it to be
 * wrong. What codegen wants it for is the length guard inside a forward
 * lookbehind body, which turns "try this alternative and see" into "this
 * alternative cannot span what is left".
 *
 * @param ir The lowered pattern.
 * @param node_index The subtree root.
 * @param out_min Receives the shortest match.
 * @param out_max Receives the longest, or GRX_NPOS when unbounded.
 * @return Non-zero when the length is knowable at all.
 */
int grx_ir_span(const GRX_IR * ir, uint32_t node_index, size_t * out_min,
    size_t * out_max);

/**
 * @brief Whether a subtree can match the empty string.
 *
 * The same walk grx_analyze_ir() uses, asked about one node. Codegen needs
 * it to decide whether a repeat needs its empty-iteration guard at all: a
 * body that cannot match empty cannot stall, so the guard - two instructions
 * and a progress register per loop - is dead weight, and its absence is what
 * makes a program memoizable by the bit-state engine.
 *
 * Asking analysis rather than deciding here is deliberate: "can this match
 * empty" is already written down once, and a second implementation in
 * codegen would be a second place for it to be wrong about `(?=x)` or an
 * unset backreference.
 *
 * @param ir The lowered pattern.
 * @param node_index The subtree root.
 * @return Non-zero when it can match empty, or when that cannot be decided.
 */
int grx_ir_can_match_empty(const GRX_IR * ir, uint32_t node_index);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_IR_LOWER_INTERNAL_H
