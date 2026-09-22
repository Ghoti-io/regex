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
 * Private helpers shared by every phase: building an error, and the
 * diagnostic catalogue behind it.
 */

#ifndef GHOTI_IO_GRX_SRC_CORE_CORE_INTERNAL_H
#define GHOTI_IO_GRX_SRC_CORE_CORE_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Fill in a caller's error structure from a diagnostic.
 *
 * The one place a GRX_Error is built, so that every failure in the library
 * arrives with the same shape: a result code, the diagnostic that identifies
 * it, the span it applies to, and a message assembled from the catalogue.
 * A phase that formats its own message by hand is a phase whose failures a
 * test has to match on English.
 *
 * The message is the diagnostic's text, with " at offset N" appended when
 * `offset` is not GRX_NPOS. It is truncated rather than overrun if it does
 * not fit.
 *
 * @param error Structure to fill. NULL is ignored, which is what makes the
 *   optional `out_error` parameter of the public API cost nothing at each
 *   call site.
 * @param code The result code to report.
 * @param diag The diagnostic identifying the specific failure.
 * @param offset Byte offset into the pattern, or GRX_NPOS for no position.
 * @param length Bytes spanned by the offending construct, or 0 if unknown.
 * @return `code`, so that a caller can `return grx_error_set(...)`.
 */
GRX_Result grx_error_set(GRX_Error * error, GRX_Result code, GRX_Diag diag,
    size_t offset, size_t length);

/**
 * @brief The result code a diagnostic implies.
 *
 * Each diagnostic belongs to exactly one result code - a limit diagnostic is
 * always GRX_ERR_LIMIT, a lexical one always GRX_ERR_SYNTAX - so a caller
 * that has chosen the diagnostic has already chosen the code. This returns
 * it, so the two cannot drift apart at a call site.
 *
 * @param diag The diagnostic.
 * @return The result code, or GRX_ERR_INTERNAL for a value out of range.
 */
GRX_Result grx_diag_result(GRX_Diag diag);

/**
 * @brief The limits a *pattern* asked for, as against the ones a caller set.
 *
 * PCRE2's `(*LIMIT_MATCH=d)`, `(*LIMIT_DEPTH=d)` and `(*LIMIT_HEAP=d)`.
 * Each field is GRX_NPOS when the pattern did not ask, and a *request*
 * rather than a limit: a pattern may lower what the caller allowed and may
 * never raise it, so the two are resolved by taking the smaller.
 *
 * Zero is a request, not an absence - `(*LIMIT_MATCH=0)` fails every match
 * in pcre2test with "match limit exceeded" - which is why GRX_NPOS and not
 * 0 is what "unasked" looks like here. GRX_Limits reads 0 as *no* limit, so
 * the two encodings disagree about zero and disagree backwards.
 *
 * It lives beside GRX_Error rather than in the parser's header because the
 * parser writes it, the compiled regex carries it and the executor spends
 * it, and a type three phases share belongs to none of them.
 * documentation/dialects.md section 6 has which GRX_Limits field each
 * directive lands on and why the units are not the reference's.
 */
typedef struct GRX_PatternLimits {
  size_t max_steps;         ///< From `(*LIMIT_MATCH=d)`.
  size_t max_backtrack;     ///< From `(*LIMIT_DEPTH=d)`.
  size_t max_match_memory;  ///< From `(*LIMIT_HEAP=d)`, kibibytes to bytes.
} GRX_PatternLimits;

/**
 * @brief Set every field to "the pattern did not ask".
 *
 * @param limits Structure to populate. NULL is ignored.
 */
void grx_pattern_limits_init(GRX_PatternLimits * limits);

/**
 * @brief Narrow a caller's limits by whatever the pattern asked for.
 *
 * @param requested What the pattern asked for. NULL is GRX_ERR_INVALID.
 * @param target The caller's limits, narrowed in place.
 * @param out_diag Receives which limit refused, on GRX_ERR_LIMIT. Optional.
 * @return GRX_OK, GRX_ERR_INVALID, or GRX_ERR_LIMIT when the pattern asked
 *   for a budget of zero, which no search can be run inside.
 */
GRX_Result grx_pattern_limits_apply(const GRX_PatternLimits * requested,
    GRX_Limits * target, GRX_Diag * out_diag);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_CORE_CORE_INTERNAL_H
