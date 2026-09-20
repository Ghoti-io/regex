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
 *
 * Copyright 2026 by Corey Pennycuff
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
  GRX_SET_UNICODE_WORD,     ///< `\p{L}\p{N}\p{M}\p{Pc}` plus the join controls.
  GRX_SET_UNICODE_SPACE,    ///< `\p{White_Space}`.
  GRX_SET_ASCII_HSPACE,     ///< `[ \t]`, for `\h`.
  GRX_SET_ASCII_VSPACE,     ///< `[\n\v\f\r]`, for `\v`.
  /**
   * The Perl family's `\h` and `\v` in UTF mode.
   *
   * pcre2pattern lists both sets in full, and they are not the ASCII ones
   * widened by a property: `\h` is the space separators *plus* the tab and
   * U+00A0, and `\v` is the line separators plus U+0085. Written out for
   * that reason - a set nobody can derive has to be a set somebody wrote
   * down.
   */
  GRX_SET_UNICODE_HSPACE,
  GRX_SET_UNICODE_VSPACE,
  GRX_SET_NEWLINES_LF,      ///< `[\n]`.
  GRX_SET_NEWLINES_ES,      ///< LF, CR, U+2028 and U+2029.
  GRX_SET_NEWLINES_UNICODE, ///< The `\R` set, less the CR LF pair.
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
 * @param kind Which shorthand was written; the negated spellings are
 *   resolved by the caller, which has to complement *after* folding.
 * @param limits Caps to apply. NULL applies none.
 * @return GRX_OK, GRX_ERR_UNSUPPORTED for a shorthand the definitions do not
 *   cover, or a failure code.
 */
GRX_Result grx_shorthand_set(GRX_CharClass * cls, GRX_ShorthandSet shorthands,
    GRX_ShorthandKind kind, const GRX_Limits * limits);

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
 * @param ir The lowered pattern. Never NULL here.
 * @param out_facts Receives the facts. Never NULL here.
 * @return GRX_OK or a failure code.
 */
GRX_Result grx_analyze_ir(const GRX_IR * ir, GRX_Facts * out_facts);

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
