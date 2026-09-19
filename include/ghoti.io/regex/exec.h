/**
 * @file
 *
 * Execution: running a compiled regex against a subject, and the match result.
 *
 * Two engines, because no single one covers the dialects:
 *
 * - The Pike VM runs the whole program in lockstep and so is linear in the
 *   subject length, but cannot express a backreference.
 * - The backtracking engine can, at the cost of an exponential worst case,
 *   which is what GRX_Limits::max_steps and max_backtrack exist to bound.
 *
 * GRX_ENGINE_AUTO picks the first that can run the program. A caller that
 * needs the linear-time guarantee asks for GRX_ENGINE_PIKE by name and gets
 * GRX_ERR_UNSUPPORTED for a pattern that cannot have it, rather than silently
 * getting the engine that can hang.
 *
 * Status: both engines are built. The constructs neither runs yet -
 * conditionals, recursion and the backtracking control verbs - compile and
 * are then refused, rather than being ignored: a plausible wrong answer is
 * worse than no answer.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRX_EXEC_H
#define GHOTI_IO_GRX_EXEC_H

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/compile.h>
#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/macros.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Which engine runs a match.
 */
typedef enum {
  GRX_ENGINE_AUTO = 0, ///< The fastest engine that can run this program.
  GRX_ENGINE_PIKE,     ///< Lockstep NFA simulation; linear time, no backrefs.
  GRX_ENGINE_BACKTRACK, ///< Backtracking; every feature, bounded by the limits.
  GRX_ENGINE_COUNT     ///< Closes the enum; not an engine.
} GRX_Engine;

/**
 * @brief The span one capturing group matched.
 *
 * Both offsets are byte offsets into the subject. A group that did not
 * participate in the match has both set to GRX_NPOS, which is distinct from a
 * group that matched the empty string (`start == end`, both valid).
 */
typedef struct GRX_Capture {
  size_t start; ///< First byte of the span, or GRX_NPOS.
  size_t end;   ///< One past the last byte, or GRX_NPOS.
} GRX_Capture;

/**
 * @brief The mutable state of a match: where the groups landed.
 *
 * Kept separate from GRX_Regex so that the compiled regex stays immutable and
 * shareable, and so that a caller matching in a loop allocates once.
 */
typedef struct GRX_Match GRX_Match;

/**
 * @brief Create a match object sized for a regex.
 *
 * @param regex The regex it will be used with. NULL is invalid.
 * @param allocator Allocator for the match. NULL uses the default.
 * @param out_match Receives the new match object on success.
 * @return GRX_OK, GRX_ERR_INVALID, or GRX_ERR_OOM.
 */
GRX_API GRX_Result grx_match_create(const GRX_Regex * regex,
    const GRX_Allocator * allocator, GRX_Match ** out_match);

/**
 * @brief Search a subject for the first match at or after an offset.
 *
 * @param regex The regex. NULL is invalid.
 * @param subject The bytes to search. May be NULL only when `length` is 0.
 * @param length Length of `subject` in bytes.
 * @param start Byte offset to start at; at most `length`.
 * @param engine Which engine to use. GRX_ENGINE_AUTO chooses.
 * @param limits Caps to apply. NULL uses grx_limits_default().
 * @param match Receives the capture spans. Optional; NULL tests for a match
 *   without recording where.
 * @param out_matched Receives non-zero when a match was found. Required; "no
 *   match" is an outcome, not an error, and so is not a result code.
 * @return GRX_OK, GRX_ERR_UNSUPPORTED, GRX_ERR_LIMIT, GRX_ERR_INVALID, or
 *   GRX_ERR_OOM.
 */
GRX_API GRX_Result grx_regex_search(const GRX_Regex * regex,
    const char * subject, size_t length, size_t start, GRX_Engine engine,
    const GRX_Limits * limits, GRX_Match * match, int * out_matched);

/**
 * @brief Match a subject against a regex anchored at an offset.
 *
 * As grx_regex_search(), except that the match must begin at `start`.
 *
 * @param regex The regex. NULL is invalid.
 * @param subject The bytes to match. May be NULL only when `length` is 0.
 * @param length Length of `subject` in bytes.
 * @param start Byte offset the match must begin at; at most `length`.
 * @param engine Which engine to use. GRX_ENGINE_AUTO chooses.
 * @param limits Caps to apply. NULL uses grx_limits_default().
 * @param match Receives the capture spans. Optional.
 * @param out_matched Receives non-zero when the regex matched. Required.
 * @return GRX_OK, GRX_ERR_UNSUPPORTED, GRX_ERR_LIMIT, GRX_ERR_INVALID, or
 *   GRX_ERR_OOM.
 */
GRX_API GRX_Result grx_regex_match(const GRX_Regex * regex,
    const char * subject, size_t length, size_t start, GRX_Engine engine,
    const GRX_Limits * limits, GRX_Match * match, int * out_matched);

/**
 * @brief Which engine actually ran the last match.
 *
 * Meaningful after a successful grx_regex_search() or grx_regex_match() with
 * GRX_ENGINE_AUTO, which is the only way to find out what AUTO chose.
 *
 * @param match The match object. NULL returns GRX_ENGINE_COUNT.
 * @return The engine used.
 */
GRX_API GRX_Engine grx_match_engine(const GRX_Match * match);

/**
 * @brief The number of capture spans a match object holds.
 *
 * This is the regex's capture count plus one, because group 0 is the whole
 * match.
 *
 * @param match The match object. NULL returns 0.
 * @return The number of groups, including group 0.
 */
GRX_API size_t grx_match_count(const GRX_Match * match);

/**
 * @brief The span one group matched.
 *
 * @param match The match object. NULL is invalid.
 * @param index Group index; 0 is the whole match.
 * @param out_capture Receives the span. A group that did not participate
 *   comes back as GRX_NPOS/GRX_NPOS rather than an error.
 * @return GRX_OK, or GRX_ERR_INVALID for an out-of-range index or a NULL
 *   argument.
 */
GRX_API GRX_Result grx_match_group(
    const GRX_Match * match, size_t index, GRX_Capture * out_capture);

/**
 * @brief The span one named group matched.
 *
 * @param match The match object. NULL is invalid.
 * @param name The group name. NULL is invalid.
 * @param out_capture Receives the span.
 * @return GRX_OK, or GRX_ERR_INVALID for an unknown name or a NULL argument.
 */
GRX_API GRX_Result grx_match_group_named(
    const GRX_Match * match, const char * name, GRX_Capture * out_capture);

/**
 * @brief Write a human-readable form of a match.
 *
 * For debugging and for tests; the format is not stable across versions.
 *
 * @param match The match object.
 * @param out Destination.
 * @return GRX_OK, or GRX_ERR_INVALID for a NULL argument.
 */
GRX_API GRX_Result grx_match_dump(const GRX_Match * match, FILE * out);

/**
 * @brief Destroy a match object. NULL is ignored.
 *
 * @param match The match object.
 */
GRX_API void grx_match_destroy(GRX_Match * match);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_EXEC_H
