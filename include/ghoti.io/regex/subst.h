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
 * Substitution and splitting: the two things callers do with a match that are
 * not "where is it".
 *
 * Both are dialect-dependent in ways that are easy to miss. A replacement
 * template is a second small language - `$1` in ECMAScript, `\1` in Python,
 * `&` in sed - with its own rules for what a reference to a group that does
 * not exist means (documentation/dialects.md section 5.11). Splitting differs
 * in whether capturing groups appear in the output at all, and in what an
 * empty match does to the piece boundaries (section 5.16). Neither is
 * something a caller should have to reimplement per dialect, and both are
 * things they will get subtly wrong if they do.
 *
 * Status: ECMAScript. Every expectation in the tests is Node 22's own output
 * for the same pattern, subject and template.
 */

#ifndef GHOTI_IO_GRX_SUBST_H
#define GHOTI_IO_GRX_SUBST_H

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/compile.h>
#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/exec.h>
#include <ghoti.io/regex/macros.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Text this library allocated and the caller owns.
 *
 * The allocator travels with the buffer so that grx_text_free() needs only
 * the one argument: a caller who passed a custom allocator to the call that
 * produced this cannot free it with the wrong one, because there is no way
 * to say which.
 *
 * `data` is NUL-terminated as a convenience for `printf`, but `length` is
 * authoritative - a subject may contain a NUL and a replacement may put one
 * back.
 */
typedef struct GRX_Text {
  char * data;                     ///< The bytes; NUL-terminated.
  size_t length;                   ///< Bytes, not counting the terminator.
  const GRX_Allocator * allocator; ///< Where they came from.
} GRX_Text;

/**
 * @brief Release a GRX_Text and zero it. NULL, or an already-zeroed one, is
 * ignored.
 *
 * @param text The text to free.
 */
GRX_API void grx_text_free(GRX_Text * text);

/**
 * @brief The pieces a subject was split into.
 *
 * Spans into the subject rather than copies: every piece is a substring of
 * the text the caller already has, and copying them would allocate once per
 * piece to hand back bytes that are already addressable. A piece whose
 * `start` is GRX_NPOS is a capturing group that did not participate - what
 * ECMAScript reports as `undefined` - which is why the pieces are
 * GRX_Capture and not pointers.
 */
typedef struct GRX_Split {
  GRX_Capture * pieces;            ///< Spans into the subject.
  size_t count;                    ///< How many.
  const GRX_Allocator * allocator; ///< Where the array came from.
} GRX_Split;

/**
 * @brief Release a GRX_Split and zero it. NULL, or an already-zeroed one, is
 * ignored.
 *
 * @param split The split to free.
 */
GRX_API void grx_split_free(GRX_Split * split);

/**
 * @brief Bits for grx_regex_replace().
 */
typedef enum {
  GRX_REPLACE_NONE = 0,             ///< Replace the first match only.
  GRX_REPLACE_GLOBAL = GRX_BIT(0),  ///< Replace every match. ECMAScript's `g`.
  GRX_REPLACE_LITERAL = GRX_BIT(1)  ///< The replacement is text, not a
                                    ///< template: no character in it is
                                    ///< special. What a caller substituting
                                    ///< user input needs.
} GRX_ReplaceFlag;

/**
 * @brief Replace what a regex matches with a template.
 *
 * The subject for this call is `subject[0, options->end)` - see
 * GRX_SearchOptions - and the result is that text with the replacements made.
 * Bytes at or past `end` are not part of the subject and do not appear in the
 * output; bytes before `options->begin` are copied through unchanged, because
 * `begin` says where a *match* may start and not where the text does.
 *
 * The template grammar is the dialect's (documentation/dialects.md section
 * 5.11). For ECMAScript: `$$` is a literal `$`, `$&` the whole match,
 * `` $` `` and `$'` the text before and after it, `$1`..`$99` a group by
 * number, and `$<name>` a group by name when the pattern has named groups.
 * Anything else beginning with `$` - including a reference to a group the
 * pattern does not have - is literal text, which is ECMAScript's rule and
 * not a fallback.
 *
 * @param regex The regex. NULL is invalid.
 * @param subject The bytes to search. May be NULL only when `length` is 0.
 * @param length Length of `subject` in bytes.
 * @param replacement The template, or literal text with GRX_REPLACE_LITERAL.
 *   May be NULL only when `replacement_length` is 0.
 * @param replacement_length Length of `replacement` in bytes.
 * @param flags GRX_ReplaceFlag bits.
 * @param options The window, search flags, engine and limits. NULL uses the
 *   defaults.
 * @param allocator Allocator for the result. NULL uses the default.
 * @param out_error Receives the failure position and diagnostic. Optional.
 *   An offset in it is an offset into `replacement`, not into the pattern.
 * @param out_text Receives the result. Freed with grx_text_free(). Written
 *   only on success.
 * @return GRX_OK, GRX_ERR_SYNTAX for a template this dialect rejects,
 *   GRX_ERR_UNSUPPORTED for a dialect whose template grammar is not built,
 *   GRX_ERR_LIMIT, GRX_ERR_INVALID, or GRX_ERR_OOM.
 */
GRX_API GRX_Result grx_regex_replace(const GRX_Regex * regex,
    const char * subject, size_t length, const char * replacement,
    size_t replacement_length, uint32_t flags,
    const GRX_SearchOptions * options, const GRX_Allocator * allocator,
    GRX_Error * out_error, GRX_Text * out_text);

/**
 * @brief Divide a subject at every match.
 *
 * **The rule is the regex's dialect's**, and there are three of them - three
 * answers rather than two and a blend (documentation/dialects.md section
 * 5.16). A caller who assumed one would be wrong about the other two:
 *
 * - **ECMAScript** (22.2.6.14), described below, and what every dialect
 *   without a splitting library of its own is given.
 * - **Perl**, which drops the trailing empty *elements* unless `limit` is
 *   positive: `,` over `",a,"` is two pieces there and three under
 *   ECMAScript's rule.
 * - **Python**, whose `limit` counts **splits** rather than pieces and keeps
 *   the remainder as the last one - `,` over `"a,b,c"` with `limit` 1 is
 *   `a` and `b,c`, where ECMAScript's is `a` alone - and which yields two
 *   empty pieces for an empty subject the pattern matches, where the other
 *   two yield none.
 *
 * This paragraph said "ECMAScript's rule, for every dialect" until
 * 2026-09-26, and it had been wrong since Python's splitting rule landed.
 * The sentence carried its own justification - that a dialect's own split
 * "disagrees as a matter of its library rather than of its grammar" - which
 * is why nobody re-read it when the third rule arrived. A stale claim in a
 * public header is the one a caller reads first.
 *
 * ECMAScript's rule is more particular than it looks:
 *
 * - Capturing groups appear in the output between the pieces around them, so
 *   `(\d)` splitting `"a1b"` yields `a`, `1`, `b`.
 * - An empty match at the position a piece starts does not end that piece,
 *   which is what stops a pattern like `x*` splitting `"abc"` into seven
 *   pieces instead of three.
 * - An empty subject yields one empty piece, unless the pattern matches the
 *   empty string, in which case it yields none.
 * - A match at the very start or end yields the empty piece beside it:
 *   `b` splitting `"b"` yields two empty pieces, not none.
 *
 * @param regex The regex. NULL is invalid.
 * @param subject The bytes to split. May be NULL only when `length` is 0.
 * @param length Length of `subject` in bytes.
 * @param limit What the dialect's rule counts, and GRX_NPOS for no limit:
 *   the most *pieces* to produce under ECMAScript's rule and Perl's, where
 *   zero yields none - ECMAScript's `split(re, 0)` - and the most *splits*
 *   to make under Python's, where the remainder is kept as the last piece.
 * @param options The window, search flags, engine and limits. NULL uses the
 *   defaults.
 * @param allocator Allocator for the result. NULL uses the default.
 * @param out_error Receives the failure diagnostic. Optional.
 * @param out_split Receives the pieces. Freed with grx_split_free(). Written
 *   only on success.
 * @return GRX_OK, GRX_ERR_LIMIT, GRX_ERR_INVALID, or GRX_ERR_OOM.
 */
GRX_API GRX_Result grx_regex_split(const GRX_Regex * regex,
    const char * subject, size_t length, size_t limit,
    const GRX_SearchOptions * options, const GRX_Allocator * allocator,
    GRX_Error * out_error, GRX_Split * out_split);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SUBST_H
