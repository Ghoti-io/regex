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
 * @brief The one flag letter the drivers add to a dialect's own alphabet.
 *
 * `W` is GRX_OPT_WTF8, and no dialect has a letter for it: it is a caller's
 * choice rather than something a pattern author writes, like the POSIX
 * options the `.rxt` format spells by name. Every driver that takes flag
 * letters therefore has to lift it out before handing the rest to
 * grx_options_parse(), which answers for the dialect's alphabet and refuses
 * anything outside it.
 *
 * In a header because three drivers need it - grx_match, grx_replace and
 * grx_split - and a letter lifted three ways is three chances for one of
 * them to drift. A differential that spells the same request differently
 * from its neighbour is one edit away from asking two different questions,
 * and the silent flag-letter drop this family has already paid for was
 * exactly that shape.
 */

#ifndef GRX_ORACLE_FLAGS_H
#define GRX_ORACLE_FLAGS_H

#include <stddef.h>
#include <stdint.h>

#include <ghoti.io/regex/regex.h>

/**
 * @brief Split `W` out of a flag-letter string.
 *
 * @param letters The flags as the driver's input spells them.
 * @param out Receives the letters with every `W` removed, NUL terminated.
 * @param capacity Size of `out`, which must be at least 1.
 * @param out_options Receives GRX_OPT_WTF8 and GRX_OPT_UTF when a `W` was
 *   present, ORed into whatever is already there. GRX_OPT_UTF because the
 *   option requires UTF mode, and a caller passing one without the other
 *   would get a precondition failure reported as a flag-letter failure.
 * @return Non-zero on success; zero when the letters do not fit, which a
 *   caller must report rather than truncate - a dropped letter is the defect
 *   this file exists to prevent.
 */
static inline int grx_oracle_take_wtf8(const char * letters, char * out,
    size_t capacity, uint32_t * out_options) {
  size_t kept = 0;
  int want = 0;
  for (const char * f = letters; *f; f++) {
    if (*f == 'W') {
      want = 1;
      continue;
    }
    if (kept + 1 >= capacity) {
      return 0;
    }
    out[kept++] = *f;
  }
  out[kept] = '\0';
  if (want) {
    *out_options |= GRX_OPT_WTF8 | GRX_OPT_UTF;
  }
  return 1;
}

#endif
