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
 * Pattern parsing: text in one dialect to a syntax tree.
 *
 * Parsing is a separate step from compiling so that the two failure modes stay
 * apart. "This is not valid PCRE" is a statement about the text, and a caller
 * that is linting, translating between dialects, or reporting a position to a
 * user wants it without having built a program it will not run.
 *
 * Status: ECMAScript is implemented, in both the legacy and Unicode modes of
 * documentation/dialects.md section 8. Every other dialect is named and
 * reports GRX_ERR_UNSUPPORTED with GRX_DIAG_DIALECT_NOT_IMPLEMENTED, rather
 * than being read with a front end written for something else - a caller
 * uses this library to learn whether a pattern is valid *for that engine*.
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
  GRX_NODE_OPTIONS,     ///< `(?i)` bare, or `(?i:a)` scoped.
  GRX_NODE_CLASS_OP,    ///< A set operation between classes: `&&`, `--`.
  GRX_NODE_STRING_SET,  ///< A set of strings: `\q{ab|cd}`, and `\R`.
  /**
   * @brief `(*scs:(n)...)`: match the body against a captured substring.
   *
   * PCRE2 10.45's scan substring. Zero-width where it stands, like a
   * lookahead, but the body runs somewhere else entirely - over the text one
   * of the named groups captured, treated as though it were the whole
   * subject. `a` names the group list as written; the one child is the body.
   */
  GRX_NODE_SCAN,
  GRX_NODE_KEEP,        ///< `\K`, which resets the reported match start.
  GRX_NODE_BRANCH_RESET, ///< `(?|...)`, which renumbers per alternative.
  /**
   * `\X`: one extended grapheme cluster.
   *
   * A node rather than an expansion in the parser, because what a cluster is
   * belongs to the dialect's Unicode rules and not to its spelling. Lowering
   * turns it into "one character, then every following character that is not
   * a cluster boundary", which is the definition - so `\X` and `\b{gcb}`
   * are one algorithm and cannot come apart.
   */
  GRX_NODE_GRAPHEME,
  GRX_NODE_COUNT        ///< Closes the enum; not a node kind.
} GRX_NodeKind;

/**
 * @brief The name of a node kind, for a dump or a diagnostic.
 *
 * The returned string is statically allocated and must not be freed.
 *
 * @param kind The node kind.
 * @return A lowercase name such as "alternate", or "?" for a value out of
 *   range. Never NULL.
 */
GRX_API const char * grx_node_kind_name(GRX_NodeKind kind);

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
 * @brief What took a pattern outside the JSON Schema subset.
 *
 * Not a @ref GRX_Diag, and deliberately: every diagnostic in that catalogue
 * means something went wrong, and a pattern reported here has nothing wrong
 * with it. It is valid, this library runs it, and the only claim being made
 * is that a schema author following JSON Schema core section 6.4's advice
 * would not have written it.
 */
typedef enum {
  GRX_LINT_NONE = 0,       ///< The pattern is inside the subset.
  GRX_LINT_FLAGS,          ///< Parsed without `u` or `v`.
  GRX_LINT_DOT,            ///< `.`.
  GRX_LINT_SHORTHAND,      ///< `\d`, `\w`, `\s` and their complements.
  GRX_LINT_PROPERTY,       ///< `\p{...}` or `\P{...}`.
  GRX_LINT_ANCHOR,         ///< An anchor other than `^` and `$`.
  GRX_LINT_GROUP,          ///< A group that is not plain `(...)`.
  GRX_LINT_BACKREFERENCE,  ///< `\1` or `\k<name>`.
  GRX_LINT_LOOKAROUND,     ///< `(?=...)` and its three siblings.
  GRX_LINT_CLASS,          ///< A class beyond characters and ranges.
  GRX_LINT_CONSTRUCT,      ///< Something else outside the subset.
  GRX_LINT_COUNT           ///< Closes the enum; not a finding.
} GRX_Lint;

/** @brief What grx_pattern_lint() found, and where. */
typedef struct GRX_LintReport {
  GRX_Lint finding; ///< GRX_LINT_NONE when the pattern is in the subset.
  size_t offset;    ///< Byte offset in the pattern; GRX_NPOS for a flag.
  size_t length;    ///< Bytes the construct spans, or 0.
} GRX_LintReport;

/**
 * @brief The English name of a lint finding.
 *
 * @param finding The finding.
 * @return A static string. Never NULL; out of range gives "unknown".
 */
GRX_API const char * grx_lint_string(GRX_Lint finding);

/**
 * @brief Report the first construct that leaves the JSON Schema subset.
 *
 * JSON Schema core section 6.4 says a `pattern` SHOULD be an ECMAScript
 * regular expression built with `u`, and that "given the high disparity in
 * regular expression constructs support, schema authors SHOULD limit
 * themselves to the following regular expression tokens":
 *
 * - individual Unicode characters;
 * - simple and range character classes, `[abc]` and `[a-z]`;
 * - complemented character classes, `[^abc]` and `[^a-z]`;
 * - `+`, `*`, `?` and their lazy versions;
 * - `{x}`, `{x,y}`, `{x,}` and their lazy versions;
 * - `^` and `$`;
 * - simple grouping `(...)` and alternation `|`.
 *
 * That is the whole list, and this function reports anything else. Three of
 * its consequences are worth stating because they surprise people, and all
 * three are the list read as written rather than a judgment added to it:
 *
 * - **`.` is not in it.** Which is right: `.` is the construct schema
 *   engines disagree about most, over line terminators and over whether an
 *   astral character is one thing or two.
 * - **`\d`, `\w` and `\s` are not in it.** Also right: they are ASCII in
 *   ECMAScript and Unicode-aware in Python and .NET, so a schema using them
 *   means different things to different validators.
 * - **`(?:...)` is not in it.** "Simple grouping" is `(...)`. This one is
 *   harmless in practice, and it is reported anyway because the alternative
 *   is to start deciding which items on the list were meant loosely.
 *
 * A caller who disagrees with any of those has the finding and can ignore
 * it; a caller who wants only the strong signal - whether the pattern can
 * run in linear time - wants GRX_Facts::is_regular instead, which is a
 * different and weaker question.
 *
 * The *first* construct means the leftmost one in the pattern text, not the
 * first one a tree walk meets, because the offset is what a caller
 * underlines.
 *
 * @param pattern The parsed pattern. NULL is invalid.
 * @param out_report Receives the finding. Required; a pattern inside the
 *   subset reports GRX_LINT_NONE, which is an outcome and not an error.
 * @return GRX_OK, or GRX_ERR_INVALID for a NULL argument.
 */
GRX_API GRX_Result grx_pattern_lint(
    const GRX_Pattern * pattern, GRX_LintReport * out_report);

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
