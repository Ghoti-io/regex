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
 * @brief Advance to the first subject byte that could begin a match.
 *
 * The prefilter's byte set answers "can a match start here" in one test, and
 * for a pattern with no literal prefix that test is most of what a search
 * costs: on a four-kilobyte subject that rejects every position,
 * `[0-9]+-[0-9]+` spends 95% of its time here and `[b-z]+q` 86%. The loop
 * that does it reads one byte, indexes a bitmap and advances, which is about
 * 0.39 ns a byte - and the same question asked sixteen bytes at a time is
 * 0.045, because a byte set that happens to be one or two contiguous RANGES
 * is two compares per lane and nothing else.
 *
 * Most real sets are: over the conformance corpus, of the patterns that reach
 * this scan at all, 23% are one range and 56% are two - `[A-Za-z]` and its
 * relatives. The rest are arbitrary and keep the bitmap loop.
 *
 * **The bitmap loop is the definition and is always compiled.** Every other
 * path here is an optimisation of it that must produce the identical answer,
 * which is what tests/unit/test_scan.cpp asserts by running both over every
 * byte value and every offset rather than over a sample. A vector path that
 * disagrees does not make a search slow, it makes the prefilter step over a
 * position where a match begins - a wrong answer - so the two are kept
 * separately derivable and compared rather than one being written in terms of
 * the other.
 *
 * SSE2 is required by the x86-64 ABI, so the vector path here needs no
 * runtime CPU detection and no dispatch table: either the compiler defines
 * `__SSE2__` and it is always safe to execute, or this compiles to the scalar
 * code alone. Anything wider than SSE2 would need detection and is not here.
 *
 * Defining `GRX_SCAN_PORTABLE` removes the vector path from the build. It
 * exists because the arms of an `#if` that is never taken are never compiled,
 * so a library that ships a fallback and never builds it does not know
 * whether the fallback works: `make test` builds testScan a second time with
 * it set, and the same assertions run against the scalar code alone. It is
 * also the switch to reach for when porting, before writing a path for a new
 * vector unit.
 */

#include "exec_internal.h"

#if defined(__SSE2__) && !defined(GRX_SCAN_PORTABLE)
#define GRX_SCAN_SSE2 1
#include <emmintrin.h>
#endif

size_t grx_exec_scan_bitmap(const unsigned char * set, const char * subject,
    size_t from, size_t length) {
  while (from < length) {
    unsigned char byte = (unsigned char)subject[from];
    if (set[byte >> 3] & (unsigned char)(1u << (byte & 7u))) {
      break;
    }
    from++;
  }
  return from;
}

/* The tail, and the whole of the scan where there is no vector unit. Written
 * as the range test rather than as a bitmap lookup so that the two spellings
 * of "in this set" are exercised against each other even on a build with no
 * SSE2 at all. */
static size_t scan_ranges_scalar(const GRX_ByteRanges * ranges,
    const char * subject, size_t from, size_t length) {
  while (from < length) {
    unsigned char byte = (unsigned char)subject[from];
    for (unsigned r = 0; r < ranges->count; r++) {
      if (byte >= ranges->lo[r] && byte <= ranges->hi[r]) {
        return from;
      }
    }
    from++;
  }
  return from;
}

size_t grx_exec_scan_ranges(const GRX_ByteRanges * ranges,
    const char * subject, size_t from, size_t length) {
  if (!ranges->count || from >= length) {
    return from;
  }
#if defined(GRX_SCAN_SSE2)
  /* `byte` is in `[lo, hi]` exactly when two saturating unsigned
   * subtractions both vanish: `byte - hi` is zero for everything at or below
   * `hi`, and `lo - byte` is zero for everything at or above `lo`. Their OR
   * is zero only inside the range.
   *
   * This is the formulation to use rather than a pair of signed compares,
   * for three reasons that all turned up in measurement. It needs two
   * constants per range instead of three, and the constants are what a
   * caller pays for before it reads a byte - which matters because this is
   * asked once per candidate position and a set that most of the subject is
   * IN answers on the first one. It needs no 0x80 bias, so nothing here
   * depends on mapping unsigned order onto signed. And a range touching 0x00
   * or 0xFF needs no special case, where widening a signed bound by one
   * would wrap.
   *
   * A spelling with one fewer instruction per block was tried and rejected:
   * `subs_epu8(v, lo)` followed by a width compare reads every byte BELOW
   * `lo` as in range, because the saturation that makes it cheap is exactly
   * what erases the underflow. */
  /* No scalar prologue before this. Trying a few bytes first, so that a
   * caller whose hit is immediate never builds the constants below, is the
   * obvious fix for `[b-z]+q` over English text - where this is asked once
   * per candidate position and answers on the first byte or two, and where
   * the vector path is 1.37x slower than the scalar loop it replaced. It was
   * measured at one, two, four, eight and sixteen bytes and every length
   * made the whole grid worse than none: -4.4% at zero against -3.8% at one
   * and -2.9% at two, with the count of regressed cells going 2, 4, 6. The
   * prologue buys that one cell back by taxing every scan that was going to
   * be long, and there are more of those. */
  __m128i zero = _mm_setzero_si128();
  __m128i vlo[GRX_BYTE_RANGES_MAX];
  __m128i vhi[GRX_BYTE_RANGES_MAX];
  for (unsigned r = 0; r < ranges->count; r++) {
    vlo[r] = _mm_set1_epi8((char)ranges->lo[r]);
    vhi[r] = _mm_set1_epi8((char)ranges->hi[r]);
  }
  while (from + 16 <= length) {
    __m128i v
        = _mm_loadu_si128((const __m128i *)(const void *)(subject + from));
    __m128i in = zero;
    for (unsigned r = 0; r < ranges->count; r++) {
      __m128i out = _mm_or_si128(
          _mm_subs_epu8(v, vhi[r]), _mm_subs_epu8(vlo[r], v));
      in = _mm_or_si128(in, _mm_cmpeq_epi8(out, zero));
    }
    int mask = _mm_movemask_epi8(in);
    if (mask) {
      return from + (size_t)__builtin_ctz((unsigned)mask);
    }
    from += 16;
  }
#endif
  return scan_ranges_scalar(ranges, subject, from, length);
}

int grx_byte_ranges_from_set(
    const unsigned char * set, GRX_ByteRanges * out_ranges) {
  out_ranges->count = 0;
  unsigned runs = 0;
  int in_run = 0;
  for (unsigned b = 0; b < 256u; b++) {
    int member = (set[b >> 3] >> (b & 7u)) & 1u;
    if (member && !in_run) {
      if (runs == GRX_BYTE_RANGES_MAX) {
        return 0;   /* More ranges than this can describe: keep the bitmap. */
      }
      out_ranges->lo[runs] = (unsigned char)b;
      runs++;
    }
    if (member) {
      out_ranges->hi[runs - 1] = (unsigned char)b;
    }
    in_run = member;
  }
  if (!runs) {
    return 0;   /* Empty set: the bitmap loop says "no position", correctly. */
  }
  out_ranges->count = runs;
  return 1;
}
