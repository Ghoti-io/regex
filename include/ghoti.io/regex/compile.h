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
 * Compilation: a parsed pattern to a program the engines can run.
 *
 * GRX_Regex is the type a consumer holds on to. It is immutable once built and
 * carries no match state, so one compiled regex may be used from several
 * threads at once; the mutable part lives in @ref GRX_Match.
 *
 * Status: working for the dialects with a front end, which today is
 * ECMAScript. A dialect that is named and not built reports
 * GRX_ERR_UNSUPPORTED with GRX_DIAG_DIALECT_NOT_IMPLEMENTED.
 */

#ifndef GHOTI_IO_GRX_COMPILE_H
#define GHOTI_IO_GRX_COMPILE_H

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/macros.h>
#include <ghoti.io/regex/pattern.h>
#include <ghoti.io/regex/syntax.h>
#include <stdint.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A compiled regular expression.
 *
 * Owned by the caller; released with grx_regex_free(). Immutable, and so safe
 * to share between threads.
 */
typedef struct GRX_Regex GRX_Regex;

/**
 * @brief Compile a NUL-terminated pattern using the default limits and
 * allocator.
 *
 * The common case: parse and compile in one call, discarding the intermediate
 * syntax tree.
 *
 * @param pattern The pattern text. NULL is invalid.
 * @param syntax The dialect to read it in.
 * @param options @ref GRX_Option bits, or GRX_OPT_NONE.
 * @param out_regex Receives the compiled regex on success.
 * @return GRX_OK, GRX_ERR_SYNTAX, GRX_ERR_UNSUPPORTED, GRX_ERR_LIMIT,
 *   GRX_ERR_INVALID, or GRX_ERR_OOM.
 */
GRX_API GRX_Result grx_regex_compile(const char * pattern, GRX_Syntax syntax,
    uint32_t options, GRX_Regex ** out_regex);

/**
 * @brief Compile a pattern of a given length, with explicit limits, allocator
 * and error reporting.
 *
 * @param pattern The pattern text. May be NULL only when `length` is 0.
 * @param length Length of `pattern` in bytes.
 * @param syntax The dialect to read it in.
 * @param options @ref GRX_Option bits, or GRX_OPT_NONE.
 * @param limits Caps to apply. NULL uses grx_limits_default().
 * @param allocator Allocator for the regex. NULL uses the default.
 * @param out_error Receives the failure position and message. Optional.
 * @param out_regex Receives the compiled regex on success.
 * @return GRX_OK, GRX_ERR_SYNTAX, GRX_ERR_UNSUPPORTED, GRX_ERR_LIMIT,
 *   GRX_ERR_INVALID, or GRX_ERR_OOM.
 */
GRX_API GRX_Result grx_regex_compile_with_allocator(const char * pattern,
    size_t length, GRX_Syntax syntax, uint32_t options,
    const GRX_Limits * limits, const GRX_Allocator * allocator,
    GRX_Error * out_error, GRX_Regex ** out_regex);

/**
 * @brief Compile an already-parsed pattern.
 *
 * The pattern is read, not consumed: the caller still owns it afterwards and
 * may compile it more than once.
 *
 * @param pattern The parsed pattern. NULL is invalid.
 * @param limits Caps to apply. NULL uses grx_limits_default().
 * @param allocator Allocator for the regex. NULL uses the default.
 * @param out_error Receives the failure position and message. Optional.
 * @param out_regex Receives the compiled regex on success.
 * @return GRX_OK, GRX_ERR_UNSUPPORTED, GRX_ERR_LIMIT, GRX_ERR_INVALID, or
 *   GRX_ERR_OOM.
 */
GRX_API GRX_Result grx_regex_compile_pattern(const GRX_Pattern * pattern,
    const GRX_Limits * limits, const GRX_Allocator * allocator,
    GRX_Error * out_error, GRX_Regex ** out_regex);

/**
 * @brief What compiling a pattern discovered about it.
 *
 * Computed once, at compile time, and exposed because the most important of
 * them is one a caller wants *before* deciding to run anything: `is_regular`
 * says whether the linear-time engine can run this program, and so whether
 * matching it against hostile input is safe. A validator handed a pattern by
 * an untrusted schema can refuse what it cannot run in linear time, rather
 * than discovering the cost at match time.
 *
 * The prefilter fields are populated by a later phase
 * (documentation/design.md section 3.5.5). Until then `literal_prefix` and
 * `required_literal` are NULL and `first_bytes_known` is 0, which is what a
 * consumer must check rather than assuming an empty prefix means "no prefix
 * exists".
 *
 * Pointers in this structure are owned by the @ref GRX_Regex it was read
 * from and are valid until that regex is freed.
 */
typedef struct GRX_Facts {
  int is_regular;        ///< No construct that needs a backtracking engine.
  int anchored_start;    ///< Every match must begin where the search began.
  int anchored_end;      ///< Every match must end at the end of the subject.
  int can_match_empty;   ///< The empty string is a possible match.
  int has_backreference; ///< The program contains a backreference.
  int has_lookaround;    ///< The program contains a lookahead or lookbehind.
  int has_recursion;     ///< The program recurses or calls a subroutine.
  int has_duplicate_names; ///< Two capturing groups share a name.
  size_t min_length;     ///< Shortest possible match, in bytes.
  size_t max_length;     ///< Longest possible match, or GRX_NPOS if unbounded.
  size_t max_lookbehind; ///< Bytes a lookbehind may need before the start,
                         ///< or GRX_NPOS if unbounded.
  /**
   * The same, counting only lookbehinds whose body is not one fixed length.
   *
   * Zero when every lookbehind in the pattern matches exactly one length.
   * Apart because that is the distinction PCRE2 bounds: a fixed body of any
   * length is cheap to check and a variable one is not.
   */
  size_t max_variable_lookbehind;
  size_t capture_count;  ///< Capturing groups, excluding group 0.
  size_t program_size;   ///< Instructions in the compiled program.
  const char * literal_prefix;   ///< Bytes every match starts with, or NULL.
  size_t literal_prefix_length;  ///< Length of `literal_prefix`.
  const char * required_literal; ///< Bytes every match contains, or NULL.
  size_t required_literal_length; ///< Length of `required_literal`.
  int first_bytes_known;   ///< Non-zero when `first_bytes` has been computed.
  uint8_t first_bytes[32]; ///< Bitmap of bytes a match may start with.
} GRX_Facts;

/**
 * @brief Read what compiling a pattern discovered about it.
 *
 * @param regex The regex. NULL is invalid.
 * @param out_facts Receives the facts on success.
 * @return GRX_OK, or GRX_ERR_INVALID for a NULL argument.
 */
GRX_API GRX_Result grx_regex_facts(
    const GRX_Regex * regex, GRX_Facts * out_facts);

/**
 * @brief Set a facts structure to "nothing is known".
 *
 * Every flag 0, every length its widest value, and no prefilter. This is the
 * state from which analysis narrows, and it is the state a compiled program
 * is in before analysis has run, so that a consumer reading facts from a
 * regex compiled by an earlier phase is told nothing rather than something
 * false.
 *
 * @param facts Structure to populate. NULL is ignored.
 */
GRX_API void grx_facts_init(GRX_Facts * facts);


/**
 * @brief The number of capturing groups.
 *
 * Group 0, the whole match, is not counted.
 *
 * @param regex The regex. NULL returns 0.
 * @return The number of capturing groups.
 */
GRX_API size_t grx_regex_capture_count(const GRX_Regex * regex);

/**
 * @brief The name of a capturing group, for a dialect that has named groups.
 *
 * The returned string is owned by the regex and is valid until it is freed.
 *
 * @param regex The regex.
 * @param index One-based group index; 0 is the whole match and is never named.
 * @return The name, or NULL for an unnamed or out-of-range group.
 */
GRX_API const char * grx_regex_capture_name(
    const GRX_Regex * regex, size_t index);

/**
 * @brief The index of a named capturing group.
 *
 * @param regex The regex.
 * @param name The group name. NULL is invalid.
 * @param out_index Receives the one-based index on success.
 * @return GRX_OK, or GRX_ERR_INVALID for an unknown name or a NULL argument.
 */
GRX_API GRX_Result grx_regex_capture_index(
    const GRX_Regex * regex, const char * name, size_t * out_index);

/**
 * @brief The dialect the regex was compiled from.
 *
 * @param regex The regex. NULL returns GRX_SYNTAX_COUNT.
 * @return The dialect.
 */
GRX_API GRX_Syntax grx_regex_syntax(const GRX_Regex * regex);

/**
 * @brief The number of instructions in the compiled program.
 *
 * @param regex The regex. NULL returns 0.
 * @return The program size.
 */
GRX_API size_t grx_regex_program_size(const GRX_Regex * regex);

/**
 * @brief Write a human-readable disassembly of the compiled program.
 *
 * For debugging and for tests; the format is not stable across versions.
 *
 * @param regex The regex.
 * @param out Destination.
 * @return GRX_OK, or GRX_ERR_INVALID for a NULL argument.
 */
GRX_API GRX_Result grx_regex_dump(const GRX_Regex * regex, FILE * out);

/**
 * @brief Release a compiled regex. NULL is ignored.
 *
 * @param regex The regex.
 */
GRX_API void grx_regex_free(GRX_Regex * regex);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_COMPILE_H
