/**
 * @file
 *
 * Core types, result codes, diagnostics, and limits for the Ghoti.io Regex
 * library.
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
 *
 * Three of these carry a precise meaning here, and the difference matters to
 * a caller deciding what to tell a user (documentation/design.md section 6.4):
 * GRX_ERR_SYNTAX is "this dialect rejects this text", GRX_ERR_UNSUPPORTED is
 * "this dialect accepts it and this library does not yet", and
 * GRX_ERR_INVALID is a wrong argument - which includes a subject that is not
 * the encoding the caller declared.
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
 * @brief The specific thing that went wrong.
 *
 * A result code says which of nine kinds of failure occurred; a diagnostic
 * says which failure. It exists so that a test asserts
 * `GRX_DIAG_UNMATCHED_PAREN` rather than a message string, which means the
 * messages can be reworded, translated or made more precise without breaking
 * a single test - and so that a caller can react to one particular failure
 * without parsing English.
 *
 * The constants are grouped by the phase that raises them. Every limit has
 * its own diagnostic, because "a limit was exceeded" is not actionable and
 * "max_nesting_depth was exceeded" is.
 *
 * Adding one is: a constant here, before the COUNT that closes its group's
 * enum, and a row in the catalogue in src/core/diag.c. The two are checked
 * against each other by DiagnosticCatalogueIsComplete in
 * tests/unit/test_diag.cpp.
 */
typedef enum {
  GRX_DIAG_NONE = 0, ///< No diagnostic; the operation succeeded.

  // Lexical: the pattern text is malformed.
  GRX_DIAG_UNMATCHED_OPEN_PAREN,   ///< `(` with no `)`.
  GRX_DIAG_UNMATCHED_CLOSE_PAREN,  ///< `)` with no `(`.
  GRX_DIAG_UNMATCHED_OPEN_BRACKET, ///< `[` with no `]`.
  GRX_DIAG_UNMATCHED_OPEN_BRACE,   ///< `{` with no `}` where one is required.
  GRX_DIAG_TRAILING_BACKSLASH,     ///< The pattern ends in `\`.
  GRX_DIAG_UNTERMINATED_COMMENT,   ///< `(?#` with no `)`.
  GRX_DIAG_UNTERMINATED_QUOTE,     ///< `\Q` with no `\E` where one is needed.
  GRX_DIAG_UNTERMINATED_NAME,      ///< A group name with no closing delimiter.

  // Quantifiers.
  GRX_DIAG_NOTHING_TO_REPEAT,      ///< A quantifier with no preceding atom.
  GRX_DIAG_DOUBLE_QUANTIFIER,      ///< `a**`, where the dialect forbids it.
  GRX_DIAG_QUANTIFIER_OUT_OF_ORDER, ///< `{3,1}`: the minimum exceeds the max.
  GRX_DIAG_QUANTIFIED_ASSERTION,   ///< A quantifier on a zero-width assertion.

  // Escapes and code points.
  GRX_DIAG_INVALID_ESCAPE,         ///< An escape this dialect does not define.
  GRX_DIAG_INVALID_HEX_ESCAPE,     ///< `\x` without the digits it requires.
  GRX_DIAG_INVALID_OCTAL_ESCAPE,   ///< `\o` or `\0` malformed.
  GRX_DIAG_INVALID_UNICODE_ESCAPE, ///< `\u` malformed.
  GRX_DIAG_INVALID_CONTROL_ESCAPE, ///< `\c` without the letter it requires.
  GRX_DIAG_CODEPOINT_OUT_OF_RANGE, ///< Above U+10FFFF, or a surrogate.
  GRX_DIAG_INVALID_UTF8_IN_PATTERN, ///< The pattern is not valid UTF-8.

  // Character classes.
  GRX_DIAG_INVALID_CLASS_RANGE,    ///< `[z-a]`: the endpoints are reversed.
  GRX_DIAG_INVALID_CLASS_ITEM,     ///< An item this dialect does not allow.
  GRX_DIAG_EMPTY_CLASS,            ///< `[]` where the dialect requires an item.
  GRX_DIAG_CLASS_ESCAPE_IN_RANGE,  ///< `[\d-z]` where the dialect forbids it.
  GRX_DIAG_UNKNOWN_POSIX_CLASS,    ///< `[[:nosuch:]]`.
  GRX_DIAG_UNKNOWN_PROPERTY,       ///< `\p{Nosuch}`.
  GRX_DIAG_INVALID_PROPERTY_SYNTAX, ///< `\p` malformed.
  GRX_DIAG_INVALID_CLASS_SET_OP,   ///< A set operation mixed with union.

  // Groups, names and references.
  GRX_DIAG_INVALID_GROUP_NAME,     ///< A name the dialect's grammar rejects.
  GRX_DIAG_DUPLICATE_GROUP_NAME,   ///< A name used twice, where that is wrong.
  GRX_DIAG_UNKNOWN_GROUP_NAME,     ///< A reference to a name no group has.
  GRX_DIAG_INVALID_BACKREFERENCE,  ///< A reference to a group that cannot be.
  GRX_DIAG_FORWARD_BACKREFERENCE,  ///< A reference to a later group.
  GRX_DIAG_INVALID_GROUP_SYNTAX,   ///< `(?` followed by nothing meaningful.
  GRX_DIAG_INVALID_CONDITION,      ///< A conditional's condition is malformed.
  GRX_DIAG_INVALID_RECURSION,      ///< A recursion or subroutine target.

  // Flags and options.
  GRX_DIAG_UNKNOWN_FLAG,           ///< A letter this dialect's alphabet lacks.
  GRX_DIAG_DUPLICATE_FLAG,         ///< The same flag twice.
  GRX_DIAG_CONFLICTING_FLAGS,      ///< `u` and `v` together, for instance.
  GRX_DIAG_SEARCH_FLAG_IN_PATTERN, ///< `g` or `y`: a search mode, not a flag.

  // Lookaround.
  GRX_DIAG_VARIABLE_LOOKBEHIND,    ///< A lookbehind the dialect requires fixed.
  GRX_DIAG_INVALID_LOOKAROUND,     ///< A lookaround this dialect does not have.

  // Dialect: the construct exists, but not here or not yet.
  GRX_DIAG_NOT_IN_DIALECT,         ///< This dialect does not have it.
  GRX_DIAG_DIALECT_NOT_IMPLEMENTED, ///< The dialect is named but not built.
  GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, ///< This library does not have it yet.

  // Limits. One per GRX_Limits field, so the message can name the field a
  // caller has to raise.
  GRX_DIAG_LIMIT_PATTERN_LENGTH,   ///< max_pattern_length.
  GRX_DIAG_LIMIT_NESTING_DEPTH,    ///< max_nesting_depth.
  GRX_DIAG_LIMIT_NODES,            ///< max_nodes.
  GRX_DIAG_LIMIT_PROGRAM_SIZE,     ///< max_program_size.
  GRX_DIAG_LIMIT_CAPTURES,         ///< max_captures.
  GRX_DIAG_LIMIT_REPEAT_COUNT,     ///< max_repeat_count.
  GRX_DIAG_LIMIT_CLASS_RANGES,     ///< max_class_ranges.
  GRX_DIAG_LIMIT_LOOKBEHIND_LENGTH, ///< max_lookbehind_length.
  GRX_DIAG_LIMIT_RECURSION_DEPTH,  ///< max_recursion_depth.
  GRX_DIAG_LIMIT_SUBJECT_LENGTH,   ///< max_subject_length.
  GRX_DIAG_LIMIT_STEPS,            ///< max_steps.
  GRX_DIAG_LIMIT_BACKTRACK,        ///< max_backtrack.
  GRX_DIAG_LIMIT_MATCH_MEMORY,     ///< max_match_memory.

  // Everything else.
  GRX_DIAG_OUT_OF_MEMORY,          ///< The allocator returned NULL.
  GRX_DIAG_INVALID_ARGUMENT,       ///< A caller-supplied argument is wrong.
  GRX_DIAG_INVALID_SUBJECT_UTF8,   ///< The subject is not valid UTF-8.
  GRX_DIAG_INTERNAL,               ///< An invariant of this library failed.

  GRX_DIAG_COUNT ///< Closes the enum; not a diagnostic.
} GRX_Diag;

/**
 * @brief The English text for a diagnostic.
 *
 * The returned string is statically allocated and must not be freed. It is a
 * sentence fragment naming the problem ("unmatched opening parenthesis"), not
 * a full message: @ref GRX_Error::message is where the position and any
 * specifics are assembled.
 *
 * @param diag The diagnostic.
 * @return Its text, or "unknown diagnostic" for a value out of range. Never
 *   NULL.
 */
GRX_API const char * grx_diag_string(GRX_Diag diag);

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
 * GRX_OK / GRX_DIAG_NONE / GRX_NPOS / 0 / an empty string, so a caller may
 * reuse one across several compiles without clearing it.
 *
 * `offset` and `length` together delimit the offending construct, so that a
 * caller can underline it rather than pointing at a single byte. Every
 * GRX_ERR_SYNTAX, GRX_ERR_UNSUPPORTED and compile-time GRX_ERR_LIMIT carries
 * an offset; a match-time limit does not, because "ran out of steps" has no
 * position in the pattern, and reports its exhausted count through the match
 * object instead.
 */
typedef struct GRX_Error {
  GRX_Result code;   ///< The failing result code.
  GRX_Diag diag;     ///< Which specific failure, for a test or a caller.
  size_t offset;     ///< Byte offset into the pattern, or GRX_NPOS.
  size_t length;     ///< Bytes spanned by the construct, or 0 if unknown.
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
 *
 * Each field is enforced at one place and reported with its own diagnostic,
 * so that a caller who has to raise one is told which. See
 * documentation/design.md section 6.2 for the table of where each is checked.
 */
typedef struct GRX_Limits {
  size_t max_pattern_length; ///< Longest accepted pattern, in bytes.
  size_t max_nesting_depth;  ///< Cap on nested groups and alternations.
  size_t max_nodes;          ///< Cap on parsed syntax-tree nodes.
  size_t max_program_size;   ///< Cap on compiled instructions.
  size_t max_captures;       ///< Cap on capturing groups.
  size_t max_repeat_count;   ///< Largest accepted bound in `{m,n}`.
  size_t max_class_ranges;   ///< Cap on ranges in one character class.
  size_t max_lookbehind_length; ///< Cap on the length of a lookbehind body.
  size_t max_recursion_depth; ///< Cap on nested recursion and subroutine calls.
  size_t max_steps;          ///< Cap on engine steps for one match attempt.
  size_t max_backtrack;      ///< Cap on backtracking stack depth.
  size_t max_match_memory;   ///< Cap on a match object's scratch, in bytes.
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
