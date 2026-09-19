/**
 * @file
 *
 * Private declarations for UTF-8 decoding and case folding.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRX_SRC_UNICODE_UNICODE_INTERNAL_H
#define GHOTI_IO_GRX_SRC_UNICODE_UNICODE_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** The largest valid Unicode code point. */
#define GRX_CODEPOINT_MAX 0x10FFFFu

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
 * "Simple" in the Unicode sense: one code point in, one out, so the mappings
 * that expand (U+00DF to "ss") are not applied. That is the folding a regex
 * engine can do without changing the length of what it matched.
 *
 * Status: ASCII only. The full table is a data-generation task; see
 * documentation/dialects.md.
 *
 * @param codepoint The code point to fold.
 * @return The folded code point, or the input where no mapping applies.
 */
uint32_t grx_unicode_fold_simple(uint32_t codepoint);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_UNICODE_UNICODE_INTERNAL_H
