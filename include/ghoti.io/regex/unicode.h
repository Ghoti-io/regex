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
 * The two Unicode facts a caller of this library needs: which UCD it was
 * built against, and whether a subject is valid UTF-8.
 *
 * Everything else the library knows about Unicode - the properties, the
 * case-fold orbits, the script tables - is applied during compilation and
 * never surfaces, because a caller writes `\p{L}` in a pattern rather than
 * asking this library what `\p{L}` contains. These two are different: a
 * conformance runner has to know whether a `\p{Script=Foo}` vector is newer
 * than the tables (documentation/unicode.md section 1), and a caller handed
 * GRX_ERR_INVALID for a bad subject has to be able to say *where*.
 */

#ifndef GHOTI_IO_GRX_UNICODE_H
#define GHOTI_IO_GRX_UNICODE_H

#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/macros.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The Unicode Character Database release the tables were generated
 * from, as a dotted string such as "17.0.0".
 *
 * The returned string is statically allocated and must not be freed.
 *
 * @return The UCD version. Never NULL.
 */
GRX_API const char * grx_unicode_version(void);

/**
 * @brief Whether a buffer is well-formed UTF-8, and where it first is not.
 *
 * Strict in the same way grx_unicode_utf8_decode() is: an overlong form, a
 * surrogate, a value above U+10FFFF and a truncated sequence are each
 * invalid. This is the check a search in UTF mode runs over the whole
 * subject before matching (documentation/design.md section 5.1), and it is
 * public so that a caller who received GRX_ERR_INVALID can report the
 * position rather than the fact.
 *
 * @param text The bytes to check. May be NULL only when `length` is 0.
 * @param length Length of `text` in bytes.
 * @param out_offset Receives the byte offset of the first invalid sequence
 *   on failure, and is left untouched on success. Optional.
 * @return GRX_OK when the whole buffer is valid, GRX_ERR_INVALID when it is
 *   not or when `text` is NULL with a non-zero length.
 */
GRX_API GRX_Result grx_utf8_validate(
    const char * text, size_t length, size_t * out_offset);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_UNICODE_H
