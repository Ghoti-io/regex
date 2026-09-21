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
 * One inclusive code-point range, and membership in a sorted array of them.
 *
 * Its own header because two modules that must not depend on each other both
 * need it: the character-class module builds these arrays, and the Unicode
 * tables are these arrays. Putting the type in either one would make the
 * other include it, and the dependency that matters - a caseless class is
 * closed over the Unicode fold orbits - only runs one way.
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

/**
 * @brief Whether a sorted, disjoint range array contains a code point.
 *
 * A binary search, which is the whole reason every set in this library is
 * kept sorted and disjoint: a property of 700 ranges costs ten comparisons
 * rather than 700.
 *
 * @param ranges The ranges. May be NULL only when `count` is 0.
 * @param count Number of ranges.
 * @param codepoint The code point to test.
 * @return Non-zero when some range covers it.
 */
int grx_range_contains(
    const GRX_CharRange * ranges, size_t count, uint32_t codepoint);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_CORE_RANGE_INTERNAL_H
