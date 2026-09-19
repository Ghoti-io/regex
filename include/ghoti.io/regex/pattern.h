/**
 * @file
 *
 * Pattern parsing: text in one dialect to a syntax tree.
 *
 * Parsing is a separate step from compiling so that the two failure modes stay
 * apart. "This is not valid PCRE" is a statement about the text, and a caller
 * that is linting, translating between dialects, or reporting a position to a
 * user wants it without having built a program it will not run.
 *
 * Status: stub. grx_pattern_parse() returns GRX_ERR_UNSUPPORTED until the
 * parser is written; see documentation/dialects.md.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRX_PATTERN_H
#define GHOTI_IO_GRX_PATTERN_H

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/macros.h>
#include <ghoti.io/regex/syntax.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A parsed pattern.
 *
 * Owned by the caller; released with grx_pattern_free(). Holds no reference to
 * the text it was parsed from, so the pattern buffer may be freed immediately
 * after the call returns.
 */
typedef struct GRX_Pattern GRX_Pattern;

/**
 * @brief The kind of one node in a parsed pattern.
 *
 * Dialect differences are resolved away by this point: `\(` in POSIX BRE and
 * `(` in ERE both arrive here as GRX_NODE_GROUP. What survives is what the
 * pattern means, which is what the compiler and any dialect translator work
 * from.
 */
typedef enum {
  GRX_NODE_EMPTY = 0,   ///< Matches the empty string.
  GRX_NODE_LITERAL,     ///< One code point, or a run of them.
  GRX_NODE_CLASS,       ///< A character class, `[...]` or `\d`.
  GRX_NODE_ANY,         ///< `.`, subject to GRX_OPT_DOTALL.
  GRX_NODE_CONCAT,      ///< A sequence.
  GRX_NODE_ALTERNATE,   ///< `a|b`.
  GRX_NODE_REPEAT,      ///< `a*`, `a+?`, `a{2,5}`.
  GRX_NODE_GROUP,       ///< `(a)`, capturing or not.
  GRX_NODE_BACKREF,     ///< `\1`, `\k<name>`.
  GRX_NODE_ANCHOR,      ///< `^`, `$`, `\A`, `\b`.
  GRX_NODE_LOOKAROUND,  ///< `(?=a)`, `(?<!a)`.
  GRX_NODE_CONDITIONAL, ///< `(?(1)a|b)`.
  GRX_NODE_RECURSE,     ///< `(?R)`, `(?1)`, `(?&name)`.
  GRX_NODE_CONTROL,     ///< `(*SKIP)` and the other backtracking verbs.
  GRX_NODE_COUNT        ///< Closes the enum; not a node kind.
} GRX_NodeKind;

/**
 * @brief Parse a NUL-terminated pattern using the default limits and
 * allocator.
 *
 * @param pattern The pattern text. NULL is invalid.
 * @param syntax The dialect to read it in.
 * @param options @ref GRX_Option bits, or GRX_OPT_NONE.
 * @param out_pattern Receives the parsed pattern on success.
 * @return GRX_OK, GRX_ERR_SYNTAX, GRX_ERR_UNSUPPORTED, GRX_ERR_LIMIT,
 *   GRX_ERR_INVALID, or GRX_ERR_OOM.
 */
GRX_API GRX_Result grx_pattern_parse(const char * pattern, GRX_Syntax syntax,
    uint32_t options, GRX_Pattern ** out_pattern);

/**
 * @brief Parse a pattern of a given length, with explicit limits, allocator
 * and error reporting.
 *
 * The length is explicit so that a pattern containing a NUL - which several
 * dialects allow - is not silently truncated.
 *
 * @param pattern The pattern text. May be NULL only when `length` is 0.
 * @param length Length of `pattern` in bytes.
 * @param syntax The dialect to read it in.
 * @param options @ref GRX_Option bits, or GRX_OPT_NONE.
 * @param limits Caps to apply. NULL uses grx_limits_default().
 * @param allocator Allocator for the pattern. NULL uses the default.
 * @param out_error Receives the failure position and message. Optional.
 * @param out_pattern Receives the parsed pattern on success.
 * @return GRX_OK, GRX_ERR_SYNTAX, GRX_ERR_UNSUPPORTED, GRX_ERR_LIMIT,
 *   GRX_ERR_INVALID, or GRX_ERR_OOM.
 */
GRX_API GRX_Result grx_pattern_parse_with_allocator(const char * pattern,
    size_t length, GRX_Syntax syntax, uint32_t options,
    const GRX_Limits * limits, const GRX_Allocator * allocator,
    GRX_Error * out_error, GRX_Pattern ** out_pattern);

/**
 * @brief The dialect a pattern was parsed in.
 *
 * @param pattern The pattern. NULL returns GRX_SYNTAX_COUNT.
 * @return The dialect.
 */
GRX_API GRX_Syntax grx_pattern_syntax(const GRX_Pattern * pattern);

/**
 * @brief The number of capturing groups in a pattern.
 *
 * Group 0, the whole match, is not counted.
 *
 * @param pattern The pattern. NULL returns 0.
 * @return The number of capturing groups.
 */
GRX_API size_t grx_pattern_capture_count(const GRX_Pattern * pattern);

/**
 * @brief The number of nodes in a parsed pattern.
 *
 * @param pattern The pattern. NULL returns 0.
 * @return The node count.
 */
GRX_API size_t grx_pattern_node_count(const GRX_Pattern * pattern);

/**
 * @brief Write a human-readable form of the syntax tree.
 *
 * For debugging and for tests; the format is not stable across versions.
 *
 * @param pattern The pattern.
 * @param out Destination.
 * @return GRX_OK, or GRX_ERR_INVALID for a NULL argument.
 */
GRX_API GRX_Result grx_pattern_dump(const GRX_Pattern * pattern, FILE * out);

/**
 * @brief Release a parsed pattern. NULL is ignored.
 *
 * @param pattern The pattern.
 */
GRX_API void grx_pattern_free(GRX_Pattern * pattern);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_PATTERN_H
