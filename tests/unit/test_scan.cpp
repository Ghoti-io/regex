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

#include <cstring>
#include <string>
#include <vector>

#include "test_helpers.h"

#include "../../src/compile/compile_internal.h"
#include "../../src/exec/exec_internal.h"

namespace {

// The bitmap the ranges claim to describe, so the two can be compared over
// the same set rather than over two descriptions of it.
std::vector<unsigned char> set_from(const GRX_ByteRanges & r) {
  std::vector<unsigned char> set(GRX_FIRST_BYTES_SIZE, 0);
  for (unsigned i = 0; i < r.count; i++) {
    for (unsigned b = r.lo[i]; b <= r.hi[i]; b++) {
      set[b >> 3] |= (unsigned char)(1u << (b & 7u));
    }
  }
  return set;
}

GRX_ByteRanges ranges_of(unsigned lo1, unsigned hi1, int two = 0,
    unsigned lo2 = 0, unsigned hi2 = 0) {
  GRX_ByteRanges r {};
  r.lo[0] = (unsigned char)lo1;
  r.hi[0] = (unsigned char)hi1;
  r.count = 1;
  if (two) {
    r.lo[1] = (unsigned char)lo2;
    r.hi[1] = (unsigned char)hi2;
    r.count = 2;
  }
  return r;
}

} // namespace

TEST(Scan, TheVectorPathAndTheBitmapAgreeOnEveryByteValue) {
  // The whole of the vector path's correctness argument. A disagreement here
  // is not a slow search: the prefilter would step over a position where a
  // match begins, which is a wrong answer.
  //
  // Every hit byte 0..255 and not a sample, because the cases that go wrong
  // are the ones at the edges of the representation. Writing this test found
  // two: a nibble-shuffle scan that dropped every byte >= 0x80 because its
  // lookup table shifted past eight bits, and a SWAR scan that got 52 of the
  // 256 wrong. Both passed a check written over ASCII.
  const GRX_ByteRanges cases[] = {
      ranges_of('b', 'z'),
      ranges_of('0', '9'),
      ranges_of('A', 'Z', 1, 'a', 'z'),
      ranges_of(0x00, 0x00),          // the low edge, alone
      ranges_of(0xFF, 0xFF),          // the high edge, alone
      ranges_of(0x00, 0xFF),          // everything
      ranges_of(0x00, 0x7F),          // exactly the signed-positive half
      ranges_of(0x80, 0xFF),          // exactly the signed-negative half
      ranges_of(0x7F, 0x81),          // straddling the sign boundary
      ranges_of(0x00, 0x00, 1, 0xFF, 0xFF),   // both edges, two ranges
      ranges_of(0xC2, 0xF4),          // UTF-8 lead bytes
  };
  size_t compared = 0;
  for (const GRX_ByteRanges & r : cases) {
    std::vector<unsigned char> set = set_from(r);
    for (int hit = 0; hit < 256; hit++) {
      // A filler byte that is definitely NOT in the set, or the hit is not
      // the first member and the test proves nothing about where it is.
      int filler = -1;
      for (int f = 0; f < 256; f++) {
        if (!(set[f >> 3] & (1u << (f & 7)))) { filler = f; break; }
      }
      if (filler < 0) { continue; }   // the set is everything; no filler
      // Every offset through a vector's width and past it, so that a hit in
      // the first block, a later block and the scalar tail are all covered.
      for (size_t at : {size_t(0), size_t(1), size_t(15), size_t(16),
               size_t(17), size_t(31), size_t(63), size_t(64), size_t(100),
               size_t(4000)}) {
        std::string subject(4096, (char)filler);
        subject[at] = (char)hit;
        size_t want = grx_exec_scan_bitmap(
            set.data(), subject.data(), 0, subject.size());
        size_t got
            = grx_exec_scan_ranges(&r, subject.data(), 0, subject.size());
        ASSERT_EQ(want, got)
            << "set [" << (int)r.lo[0] << "," << (int)r.hi[0] << "]"
            << (r.count > 1 ? " + [" + std::to_string(r.lo[1]) + ","
                        + std::to_string(r.hi[1]) + "]"
                            : "")
            << ", hit byte 0x" << std::hex << hit << std::dec
            << " at offset " << at;
        compared++;
      }
      // And with no member present at all, so "not found" agrees too.
      std::string none(4096, (char)filler);
      ASSERT_EQ(grx_exec_scan_bitmap(set.data(), none.data(), 0, none.size()),
          grx_exec_scan_ranges(&r, none.data(), 0, none.size()));
    }
  }
  EXPECT_GT(compared, 20000u) << "the sweep did not run";
}

TEST(Scan, ShortSubjectsAndEmptyRangesAreTheSameAsTheBitmap) {
  // Lengths below one vector, where the fast path is entirely tail, and the
  // degenerate inputs a caller can reach.
  GRX_ByteRanges r = ranges_of('a', 'c');
  std::vector<unsigned char> set = set_from(r);
  for (size_t n = 0; n <= 40; n++) {
    for (size_t at = 0; at < n; at++) {
      std::string subject(n, 'z');
      subject[at] = 'b';
      ASSERT_EQ(grx_exec_scan_bitmap(set.data(), subject.data(), 0, n),
          grx_exec_scan_ranges(&r, subject.data(), 0, n))
          << "length " << n << ", hit at " << at;
    }
    std::string subject(n, 'z');
    ASSERT_EQ(grx_exec_scan_bitmap(set.data(), subject.data(), 0, n),
        grx_exec_scan_ranges(&r, subject.data(), 0, n))
        << "length " << n << ", no hit";
    // Starting partway along, which is how a search resumes.
    for (size_t from = 0; from <= n; from++) {
      std::string s2(n, 'z');
      if (n) { s2[n / 2] = 'a'; }
      ASSERT_EQ(grx_exec_scan_bitmap(set.data(), s2.data(), from, n),
          grx_exec_scan_ranges(&r, s2.data(), from, n))
          << "length " << n << ", from " << from;
    }
  }
  // A set that does not decompose returns `from` untouched rather than
  // scanning, because the caller uses the bitmap in that case.
  GRX_ByteRanges empty {};
  const std::string subject(64, 'a');
  EXPECT_EQ(grx_exec_scan_ranges(&empty, subject.data(), 7, subject.size()),
      7u);
}

TEST(Scan, TheDecompositionDescribesExactlyTheSetItCameFrom) {
  // The ranges are only a faster spelling if they name the same set, and the
  // deriver is the one place that could quietly widen it - a wider set costs
  // speed, a narrower one loses matches.
  uint64_t state = 0x9E3779B97F4A7C15ull;
  auto rnd = [&state](uint32_t n) {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    return (uint32_t)(state % n);
  };
  size_t decomposed = 0;
  size_t refused = 0;
  for (int round = 0; round < 20000; round++) {
    std::vector<unsigned char> set(GRX_FIRST_BYTES_SIZE, 0);
    // A mix of shapes: a few scattered bytes, and some runs, so that both
    // the decomposing and the refusing case are populated.
    int runs = 1 + (int)rnd(4);
    for (int i = 0; i < runs; i++) {
      unsigned lo = rnd(256);
      unsigned hi = lo + rnd(40);
      if (hi > 255) { hi = 255; }
      for (unsigned b = lo; b <= hi; b++) {
        set[b >> 3] |= (unsigned char)(1u << (b & 7u));
      }
    }
    GRX_ByteRanges r {};
    int ok = grx_byte_ranges_from_set(set.data(), &r);
    if (!ok) { refused++; EXPECT_EQ(r.count, 0u); continue; }
    decomposed++;
    ASSERT_LE(r.count, (unsigned)GRX_BYTE_RANGES_MAX);
    // Membership, byte for byte, in both directions.
    for (int b = 0; b < 256; b++) {
      bool in_set = (set[b >> 3] >> (b & 7)) & 1;
      bool in_ranges = false;
      for (unsigned i = 0; i < r.count; i++) {
        if (b >= r.lo[i] && b <= r.hi[i]) { in_ranges = true; }
      }
      ASSERT_EQ(in_set, in_ranges) << "byte " << b << " differs";
    }
  }
  EXPECT_GT(decomposed, 500u) << "nothing decomposed, so nothing was checked";
  EXPECT_GT(refused, 500u)
      << "every set decomposed, so the refusal path was never taken and a "
         "deriver that always said yes would pass this";
}

TEST(Scan, APatternsRangesAgreeWithItsOwnFirstByteSet) {
  // The end of the chain: what compile.c actually stored, against the bitmap
  // it was derived from, for real patterns rather than constructed sets.
  static const char * const kPatterns[] = {
      "[b-z]+q", "[0-9]+-[0-9]+", "[a-zA-Z]+", "[^a]x", "[a-c]|[x-z]",
      "(foo|bar)baz", "a+q", ".*foo", "[[:digit:]]+", "[[:alpha:]]+",
      "[\\x80-\\xff]+", "x", "[aeiou]+",
  };
  size_t with_ranges = 0;
  for (const char * pattern : kPatterns) {
    GRX_Regex * re = nullptr;
    if (grx_regex_compile(pattern, GRX_SYNTAX_POSIX_ERE, GRX_OPT_NONE, &re)
        != GRX_OK) {
      continue;
    }
    const GRX_Program * p = &re->program;
    if (p->first_bytes_known && p->first_byte_ranges.count) {
      with_ranges++;
      for (int b = 0; b < 256; b++) {
        bool in_set = (p->first_bytes[b >> 3] >> (b & 7)) & 1;
        bool in_ranges = false;
        for (unsigned i = 0; i < p->first_byte_ranges.count; i++) {
          if (b >= p->first_byte_ranges.lo[i]
              && b <= p->first_byte_ranges.hi[i]) {
            in_ranges = true;
          }
        }
        ASSERT_EQ(in_set, in_ranges)
            << "/" << pattern << "/ byte " << b << " differs";
      }
    }
    grx_regex_free(re);
  }
  EXPECT_GT(with_ranges, 5u)
      << "almost nothing here decomposed, so this checked almost nothing";
}

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
