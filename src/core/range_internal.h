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
 * One inclusive code-point range, and the largest code point there is.
 *
 * Its own header because the character-class module builds arrays of these
 * and the parser, the IR and the engines all read them, so putting the type
 * in the class module would make everything include it.
 *
 * It held a binary search over such an array too, until the Unicode
 * properties stopped being arrays here and became questions put to
 * ghoti.io-unicode: the last caller of `grx_range_contains()` was
 * ECMAScript's `\p{...}` membership test, which asks
 * `grx_unicode_property_contains()` now. What was left was a function whose
 * only caller was its own test, which is a thing the suite makes look alive.
 */

#ifndef GHOTI_IO_GRX_SRC_CORE_RANGE_INTERNAL_H
#define GHOTI_IO_GRX_SRC_CORE_RANGE_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The largest valid Unicode code point. */
#define GRX_CODEPOINT_MAX 0x10FFFFu

/** @brief One inclusive range of code points. */
typedef struct GRX_CharRange {
  uint32_t low;  ///< First code point in the range.
  uint32_t high; ///< Last code point in the range.
} GRX_CharRange;

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_CORE_RANGE_INTERNAL_H
