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
 * Membership in a sorted, disjoint range array.
 */

#include <ghoti.io/regex/macros.h>

#include <stddef.h>
#include <stdint.h>

#include "range_internal.h"

int grx_range_contains(
    const GRX_CharRange * ranges, size_t count, uint32_t codepoint) {
  if (!ranges || !count) {
    return 0;
  }

  size_t low = 0;
  size_t high = count;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    if (codepoint < ranges[mid].low) {
      high = mid;
    }
    else if (codepoint > ranges[mid].high) {
      low = mid + 1;
    }
    else {
      return 1;
    }
  }

  return 0;
}
