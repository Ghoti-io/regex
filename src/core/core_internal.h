/**
 * @file
 *
 * Private helpers shared by every phase: building an error, and the
 * diagnostic catalogue behind it.
 *
 * Copyright 2026 by Corey Pennycuff
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

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_CORE_CORE_INTERNAL_H
