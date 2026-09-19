/**
 * @file
 *
 * The UTF-8 codec, the case-fold operations, and property lookup.
 *
 * Everything here is compile-time except the codec: a caseless match is an
 * ordinary class match by the time an engine sees it, because lowering
 * expanded the literal into its fold orbit (documentation/design.md
 * section 3.2). Nothing below the IR calls a function on this page but the
 * decoder.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRX_SRC_UNICODE_UNICODE_INTERNAL_H
#define GHOTI_IO_GRX_SRC_UNICODE_UNICODE_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/range_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The largest number of members any simple fold orbit has. */
#define GRX_FOLD_ORBIT_MAX 4

/**
 * @brief Decode one UTF-8 sequence.
 *
 * Strict: an overlong encoding, a surrogate (U+D800..U+DFFF), a code point
 * above U+10FFFF and a truncated or malformed sequence are all rejected
 * rather than decoded to U+FFFD. A regex engine that substituted a
 * replacement character here would match a pattern against bytes the subject
 * does not contain.
 *
 * @param text The bytes to decode. NULL is invalid.
 * @param length Bytes available at `text`.
 * @param out_codepoint Receives the code point on success. Required.
 * @return The number of bytes consumed, or 0 when the sequence is not valid
 *   UTF-8.
 */
size_t grx_unicode_utf8_decode(
    const char * text, size_t length, uint32_t * out_codepoint);

/**
 * @brief Decode the code point that *ends* at an offset.
 *
 * What a reverse-direction instruction steps with: a lookbehind body runs
 * right to left, and "one character back" in UTF-8 means stepping over at
 * most three continuation bytes and then validating forward, because a
 * backwards scan alone cannot tell a well-formed sequence from a truncated
 * one.
 *
 * @param text The buffer. NULL is invalid.
 * @param offset Byte offset one past the end of the sequence wanted.
 * @param out_codepoint Receives the code point on success. Required.
 * @return The number of bytes consumed, so that the sequence begins at
 *   `offset - result`, or 0 when there is no valid sequence ending there.
 */
size_t grx_unicode_utf8_decode_prev(
    const char * text, size_t offset, uint32_t * out_codepoint);

/**
 * @brief Encode one code point as UTF-8.
 *
 * @param codepoint The code point. Surrogates and values above U+10FFFF are
 *   invalid.
 * @param buffer Destination, at least 4 bytes. Not NUL-terminated.
 * @return The number of bytes written (1 to 4), or 0 for an invalid code
 *   point.
 */
size_t grx_unicode_utf8_encode(uint32_t codepoint, char * buffer);

/**
 * @brief Simple case folding of one code point.
 *
 * "Simple" in the Unicode sense: CaseFolding.txt statuses C and S, one code
 * point in and one out, so the mappings that expand (U+00DF to "ss") are not
 * applied. That is the folding a regex engine can do without changing the
 * length of what it matched; see documentation/design.md section 10 for why
 * full folding is deliberately absent.
 *
 * @param codepoint The code point to fold.
 * @return The folded code point, or the input where no mapping applies.
 */
uint32_t grx_unicode_fold_simple(uint32_t codepoint);

/**
 * @brief Every code point whose simple fold equals this one's.
 *
 * What a caseless literal becomes, and what a caseless class is closed
 * under. The orbit always contains the input, so the result is at least 1
 * and a caller need not special-case "no orbit".
 *
 * @param codepoint The code point.
 * @param out Receives the members, in ascending order. Required, with room
 *   for @ref GRX_FOLD_ORBIT_MAX.
 * @return The number of members written, at least 1.
 */
size_t grx_unicode_fold_orbit(
    uint32_t codepoint, uint32_t out[GRX_FOLD_ORBIT_MAX]);

/**
 * @brief ECMA-262 Canonicalize for a pattern without the `u` flag.
 *
 * Applies the full uppercase mapping unless the result is more than one
 * UTF-16 code unit, or maps a non-ASCII code point into ASCII. This is the
 * rule under which JavaScript's `/[a-z]/i` does not match U+017F while
 * `/[a-z]/iu` does, and ECMAScript is the only dialect that has it
 * (documentation/dialects.md section 5.8).
 *
 * @param codepoint The code point.
 * @return Its canonical form, or the input where the rule does not apply.
 */
uint32_t grx_unicode_es_legacy_canonicalize(uint32_t codepoint);

/**
 * @brief Every code point sharing this one's ECMA-262 legacy canonical form.
 *
 * As grx_unicode_fold_orbit(), for the other folding.
 *
 * @param codepoint The code point.
 * @param out Receives the members, in ascending order. Required, with room
 *   for @ref GRX_FOLD_ORBIT_MAX.
 * @return The number of members written, at least 1.
 */
size_t grx_unicode_es_legacy_orbit(
    uint32_t codepoint, uint32_t out[GRX_FOLD_ORBIT_MAX]);

/**
 * @brief How a dialect spells property names.
 *
 * Two resolvers over one set of tables (documentation/unicode.md section 6):
 * ECMAScript requires the exact canonical spelling or a listed alias, and
 * everything else in tier 1 that has properties at all matches loosely.
 */
typedef enum {
  GRX_PROPERTY_STRICT = 0, ///< Exact, case-sensitive. ECMAScript.
  GRX_PROPERTY_LOOSE       ///< UAX #44 section 5.9.2. Perl, PCRE2.
} GRX_PropertyMatch;

/**
 * @brief Resolve a property name, with or without a value, to a table index.
 *
 * Three spellings reach here. `\p{Lu}` and `\p{Alphabetic}` arrive with
 * `name` set and `value` NULL, and resolve against General_Category values
 * and binary property names respectively. `\p{Script=Greek}` arrives with
 * both, and the name selects which value table to search.
 *
 * A lone script value is deliberately *not* accepted: `\p{Greek}` is a
 * SyntaxError in ECMAScript, which is the only dialect using the strict
 * resolver today, and accepting it loosely for the others is the loose
 * resolver's job rather than this one's.
 *
 * @param name The property name, or the lone name. Need not be
 *   NUL-terminated.
 * @param name_length Its length in bytes.
 * @param value The value, or NULL for the lone form.
 * @param value_length Its length in bytes; 0 when `value` is NULL.
 * @param match Which spelling rule to apply.
 * @param out_property Receives the property index. Required.
 * @return GRX_OK, or GRX_ERR_SYNTAX when no property has that spelling.
 */
GRX_Result grx_unicode_property_lookup(const char * name, size_t name_length,
    const char * value, size_t value_length, GRX_PropertyMatch match,
    uint32_t * out_property);

/**
 * @brief The code points a resolved property covers.
 *
 * @param property The index grx_unicode_property_lookup() returned.
 * @param out_count Receives the range count. Required.
 * @return The first range, or NULL for an index out of range.
 */
const GRX_CharRange * grx_unicode_property_ranges(
    uint32_t property, size_t * out_count);

/**
 * @brief The canonical long name of a resolved property, for a dump.
 *
 * @param property The index grx_unicode_property_lookup() returned.
 * @return The name, or NULL for an index out of range.
 */
const char * grx_unicode_property_name(uint32_t property);

/**
 * @brief The number of code points a resolved property covers.
 *
 * Carried from the UCD's own arithmetic rather than counted here, so that a
 * test can check the table against the standard (documentation/unicode.md
 * section 4).
 *
 * @param property The index grx_unicode_property_lookup() returned.
 * @return The count, or 0 for an index out of range.
 */
size_t grx_unicode_property_total(uint32_t property);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_UNICODE_UNICODE_INTERNAL_H
