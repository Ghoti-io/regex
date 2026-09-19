/**
 * @file
 *
 * Core types, result codes, and limits for the Ghoti.io Regex library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRX_CORE_H
#define GHOTI_IO_GRX_CORE_H

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/macros.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Result code for regex library operations.
 *
 * The first nine are the suite's fixed vocabulary (CONVENTIONS.md section 5).
 * GRX_ERR_SYNTAX is this library's one addition, and it is there because a
 * malformed pattern is the failure a regex library exists to report: folding
 * it into GRX_ERR_FORMAT would make "this is not a pattern I can parse" and
 * "this dialect does not have that construct" indistinguishable to a caller
 * that wants to show the user where the pattern went wrong. It always arrives
 * with an offset into the pattern; see @ref GRX_Error.
 */
typedef enum {
  GRX_OK = 0,           ///< Operation succeeded.
  GRX_ERR_IO,           ///< I/O error (read/write/seek failed).
  GRX_ERR_FORMAT,       ///< Unrecognized or invalid format.
  GRX_ERR_UNSUPPORTED,  ///< Feature or construct not supported.
  GRX_ERR_LIMIT,        ///< Resource or size limit exceeded.
  GRX_ERR_CORRUPT,      ///< Corrupt or invalid data.
  GRX_ERR_OOM,          ///< Out of memory.
  GRX_ERR_INVALID,      ///< Invalid argument.
  GRX_ERR_INTERNAL,     ///< Internal library error.
  GRX_ERR_SYNTAX,       ///< The pattern is not valid in the chosen dialect.
  GRX_RESULT_COUNT
} GRX_Result;

/**
 * @brief Convert a result code to a human-readable string.
 *
 * The returned string is statically allocated and must not be freed.
 *
 * @param result The result code.
 * @return A description of the result code, never NULL.
 */
GRX_API const char * grx_result_string(GRX_Result result);

/**
 * @brief The offset value meaning "no position".
 *
 * Used for an unset capture group and for an error with no meaningful
 * position in the pattern.
 */
#define GRX_NPOS ((size_t)-1)

/**
 * @brief Where a compile failed, and why, in terms the caller can show a user.
 *
 * Filled in only on failure. On success every field is left as
 * GRX_OK / GRX_NPOS / an empty string, so a caller may reuse one across
 * several compiles without clearing it.
 */
typedef struct GRX_Error {
  GRX_Result code;   ///< The failing result code.
  size_t offset;     ///< Byte offset into the pattern, or GRX_NPOS.
  char message[192]; ///< NUL-terminated English description.
} GRX_Error;

/**
 * @brief Clear an error structure.
 *
 * @param error Structure to reset. NULL is ignored.
 */
GRX_API void grx_error_clear(GRX_Error * error);

/**
 * @brief Caps applied while parsing, compiling and matching, so that a hostile
 * pattern or subject cannot make the library allocate or run without bound.
 *
 * Zero means "no limit" for every field. Pass NULL to a compile or match
 * function to use grx_limits_default().
 *
 * The three that are not about memory are the ones that matter most here. A
 * regular expression is the one input where a *small* pattern can cost
 * unbounded time: `(a+)+$` against a few dozen `a`s is the standard
 * demonstration. max_steps and max_backtrack are what turn that from a hang
 * into GRX_ERR_LIMIT.
 */
typedef struct GRX_Limits {
  size_t max_pattern_length; ///< Longest accepted pattern, in bytes.
  size_t max_nesting_depth;  ///< Cap on nested groups and alternations.
  size_t max_nodes;          ///< Cap on parsed syntax-tree nodes.
  size_t max_program_size;   ///< Cap on compiled instructions.
  size_t max_captures;       ///< Cap on capturing groups.
  size_t max_repeat_count;   ///< Largest accepted bound in `{m,n}`.
  size_t max_class_ranges;   ///< Cap on ranges in one character class.
  size_t max_steps;          ///< Cap on engine steps for one match attempt.
  size_t max_backtrack;      ///< Cap on backtracking stack depth.
  size_t max_subject_length; ///< Longest accepted subject, in bytes.
} GRX_Limits;

/**
 * @brief Fill in the default limits.
 *
 * @param limits Structure to populate. NULL is ignored.
 */
GRX_API void grx_limits_default(GRX_Limits * limits);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_CORE_H
