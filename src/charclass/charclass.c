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
 * Character-class sets: sorted disjoint code-point ranges.
 *
 * The representation is the whole design: ranges sorted ascending, disjoint,
 * and never merely adjacent, so that the canonical form of a set is unique.
 * Uniqueness is what lets the class table deduplicate, what lets a test
 * compare two classes by their bytes, and what lets membership be a binary
 * search rather than a scan.
 *
 * The set operations are one sweep, parameterised by which combinations of
 * "in the left" and "in the right" it keeps. Four operations written four
 * times would be four places for the boundary arithmetic to be subtly
 * different; written once, a bug in it fails every operation's tests at
 * once, which is the failure mode to prefer.
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "charclass_internal.h"

/** The smallest allocation a class makes, so a `[abc]` does not grow twice. */
#define GRX_CLASS_MIN_CAPACITY 8

/** One past the last code point, used as the sweep's end sentinel. */
#define GRX_CODEPOINT_END (GRX_CODEPOINT_MAX + 1u)

/** Which combinations of membership a set operation keeps. */
typedef enum {
  CLASS_OP_UNION = 0,
  CLASS_OP_INTERSECT,
  CLASS_OP_SUBTRACT,
  CLASS_OP_SYMDIFF
} ClassOp;

/** The truth table the sweep consults; the only difference between the four. */
static int operation_keeps(ClassOp op, int in_left, int in_right) {
  switch (op) {
    case CLASS_OP_UNION:
      return in_left || in_right;
    case CLASS_OP_INTERSECT:
      return in_left && in_right;
    case CLASS_OP_SUBTRACT:
      return in_left && !in_right;
    case CLASS_OP_SYMDIFF:
      return in_left != in_right;
    default:
      return 0;
  }
}

/** Make room for `needed` ranges, growing geometrically. */
static GRX_Result reserve(
    GRX_CharClass * cls, size_t needed, const GRX_Limits * limits) {
  if (limits && limits->max_class_ranges && needed > limits->max_class_ranges) {
    return GRX_ERR_LIMIT;
  }
  if (needed <= cls->capacity) {
    return GRX_OK;
  }

  size_t capacity = cls->capacity ? cls->capacity : GRX_CLASS_MIN_CAPACITY;
  while (capacity < needed) {
    if (capacity > (size_t)-1 / 2) {
      return GRX_ERR_OOM;
    }
    capacity *= 2;
  }

  const GRX_Allocator * allocator = cls->allocator;
  if (!allocator) {
    allocator = grx_allocator_default();
    cls->allocator = allocator;
  }

  GRX_CharRange * grown = gcu_allocator_realloc(
      allocator, cls->ranges, capacity * sizeof(GRX_CharRange));
  if (!grown) {
    return GRX_ERR_OOM;
  }

  cls->ranges = grown;
  cls->capacity = capacity;
  return GRX_OK;
}

/**
 * Append a range that starts at or after everything already present.
 *
 * The sweep and the copy both produce ranges in order, so they use this
 * rather than the general insert: it is O(1), and merging with the previous
 * range when they touch is what keeps the canonical form unique.
 */
static GRX_Result push_sorted(GRX_CharClass * cls, uint32_t low, uint32_t high,
    const GRX_Limits * limits) {
  if (cls->count && low <= cls->ranges[cls->count - 1].high + 1) {
    if (high > cls->ranges[cls->count - 1].high) {
      cls->ranges[cls->count - 1].high = high;
    }
    return GRX_OK;
  }

  GRX_Result result = reserve(cls, cls->count + 1, limits);
  if (result != GRX_OK) {
    return result;
  }

  cls->ranges[cls->count].low = low;
  cls->ranges[cls->count].high = high;
  cls->count++;
  return GRX_OK;
}

/** Hand one class's storage to another, leaving the source empty. */
static void adopt(GRX_CharClass * cls, GRX_CharClass * source) {
  grx_charclass_clear(cls);
  cls->allocator = source->allocator;
  cls->ranges = source->ranges;
  cls->count = source->count;
  cls->capacity = source->capacity;
  cls->negated = 0;
  source->ranges = NULL;
  source->count = 0;
  source->capacity = 0;
}

/**
 * The sweep both set operations and the complement are built from.
 *
 * Walks the code-point line once, stopping at every position where either
 * operand's membership could change, and emits the stretches the operation
 * keeps. Both operands are read as the sets they *denote*, so a negated
 * class contributes its complement without the caller applying it first.
 *
 * The result is built in a scratch class and adopted at the end, so that an
 * operation which runs out of memory - or out of `max_class_ranges` halfway
 * through - leaves `cls` exactly as it was, and so that `cls` and `other`
 * may be the same object.
 */
static GRX_Result sweep(GRX_CharClass * cls, const GRX_CharClass * other,
    ClassOp op, const GRX_Limits * limits) {
  if (!cls || !other) {
    return GRX_ERR_INVALID;
  }

  GRX_CharClass result;
  grx_charclass_init(&result, cls->allocator);

  size_t left_index = 0;
  size_t right_index = 0;
  uint32_t position = 0;
  GRX_Result status = GRX_OK;

  for (;;) {
    while (left_index < cls->count
        && cls->ranges[left_index].high < position) {
      left_index++;
    }
    while (right_index < other->count
        && other->ranges[right_index].high < position) {
      right_index++;
    }

    int left_raw = left_index < cls->count
        && cls->ranges[left_index].low <= position;
    int right_raw = right_index < other->count
        && other->ranges[right_index].low <= position;
    int in_left = left_raw ^ (cls->negated != 0);
    int in_right = right_raw ^ (other->negated != 0);

    // The next position at which either side's membership can change: the
    // end of the range we are inside, or the start of the one ahead.
    uint32_t next = GRX_CODEPOINT_END;
    if (left_index < cls->count) {
      uint32_t edge = left_raw ? cls->ranges[left_index].high + 1
                               : cls->ranges[left_index].low;
      if (edge < next) {
        next = edge;
      }
    }
    if (right_index < other->count) {
      uint32_t edge = right_raw ? other->ranges[right_index].high + 1
                                : other->ranges[right_index].low;
      if (edge < next) {
        next = edge;
      }
    }

    if (operation_keeps(op, in_left, in_right)) {
      status = push_sorted(&result, position, next - 1, limits);
      if (status != GRX_OK) {
        break;
      }
    }

    if (next >= GRX_CODEPOINT_END) {
      break;
    }
    position = next;
  }

  if (status != GRX_OK) {
    grx_charclass_clear(&result);
    return status;
  }

  adopt(cls, &result);
  return GRX_OK;
}

void grx_charclass_init(GRX_CharClass * cls, const GRX_Allocator * allocator) {
  if (!cls) {
    return;
  }

  cls->allocator = allocator ? allocator : grx_allocator_default();
  cls->ranges = NULL;
  cls->count = 0;
  cls->capacity = 0;
  cls->negated = 0;
}

GRX_Result grx_charclass_add_range(GRX_CharClass * cls, uint32_t low,
    uint32_t high, const GRX_Limits * limits) {
  if (!cls || low > high || high > GRX_CODEPOINT_MAX) {
    return GRX_ERR_INVALID;
  }

  // The first range whose end is at or after `low - 1`: everything before it
  // is strictly below the new range and stays where it is.
  size_t first = 0;
  size_t search_high = cls->count;
  while (first < search_high) {
    size_t mid = first + (search_high - first) / 2;
    if (cls->ranges[mid].high + 1 < low) {
      first = mid + 1;
    }
    else {
      search_high = mid;
    }
  }

  // Everything from there that overlaps or touches the new range merges into
  // it. `high + 1` rather than `high` because two ranges that merely touch
  // are one range in the canonical form.
  size_t last = first;
  while (last < cls->count && cls->ranges[last].low <= high + 1) {
    last++;
  }

  uint32_t merged_low = low;
  uint32_t merged_high = high;
  if (last > first) {
    if (cls->ranges[first].low < merged_low) {
      merged_low = cls->ranges[first].low;
    }
    if (cls->ranges[last - 1].high > merged_high) {
      merged_high = cls->ranges[last - 1].high;
    }
  }

  size_t removed = last - first;
  size_t final_count = cls->count - removed + 1;
  GRX_Result result = reserve(cls, final_count, limits);
  if (result != GRX_OK) {
    return result;
  }

  if (removed != 1) {
    memmove(&cls->ranges[first + 1], &cls->ranges[last],
        (cls->count - last) * sizeof(GRX_CharRange));
  }
  cls->ranges[first].low = merged_low;
  cls->ranges[first].high = merged_high;
  cls->count = final_count;

  return GRX_OK;
}

GRX_Result grx_charclass_add_ranges(GRX_CharClass * cls,
    const GRX_CharRange * ranges, size_t count, const GRX_Limits * limits) {
  if (!cls || (!ranges && count)) {
    return GRX_ERR_INVALID;
  }

  for (size_t i = 0; i < count; i++) {
    GRX_Result result
        = grx_charclass_add_range(cls, ranges[i].low, ranges[i].high, limits);
    if (result != GRX_OK) {
      return result;
    }
  }

  return GRX_OK;
}

GRX_Result grx_charclass_add_property(GRX_CharClass * cls, uint32_t property,
    const GRX_Limits * limits) {
  if (!cls) {
    return GRX_ERR_INVALID;
  }

  // Eight kilobytes of stack on the compile path, and the reason it is here
  // rather than allocated: `\p{L}` cost no heap while the ranges were in
  // .rodata, and a property resolved once per pattern is not worth making
  // that untrue. GRX_PROPERTY_RANGES_MAX says what bounds it and what
  // happens to a set that outgrows it.
  GUNI_Range ranges[GRX_PROPERTY_RANGES_MAX];
  size_t count = 0;
  GRX_Result result = grx_unicode_property_ranges(
      property, ranges, GRX_PROPERTY_RANGES_MAX, &count);
  if (result != GRX_OK) {
    return result;
  }

  for (size_t i = 0; i < count; i++) {
    result = grx_charclass_add_range(
        cls, ranges[i].first, ranges[i].last, limits);
    if (result != GRX_OK) {
      return result;
    }
  }

  return GRX_OK;
}

GRX_Result grx_charclass_union(GRX_CharClass * cls, const GRX_CharClass * other,
    const GRX_Limits * limits) {
  return sweep(cls, other, CLASS_OP_UNION, limits);
}

GRX_Result grx_charclass_intersect(GRX_CharClass * cls,
    const GRX_CharClass * other, const GRX_Limits * limits) {
  return sweep(cls, other, CLASS_OP_INTERSECT, limits);
}

GRX_Result grx_charclass_subtract(GRX_CharClass * cls,
    const GRX_CharClass * other, const GRX_Limits * limits) {
  return sweep(cls, other, CLASS_OP_SUBTRACT, limits);
}

GRX_Result grx_charclass_symdiff(GRX_CharClass * cls,
    const GRX_CharClass * other, const GRX_Limits * limits) {
  return sweep(cls, other, CLASS_OP_SYMDIFF, limits);
}

GRX_Result grx_charclass_complement(
    GRX_CharClass * cls, const GRX_Limits * limits) {
  if (!cls) {
    return GRX_ERR_INVALID;
  }

  // The complement of a set is the set subtracted from everything, and
  // "everything" is one range, so the sweep serves here too.
  GRX_CharRange everything = {0, GRX_CODEPOINT_MAX};
  GRX_CharClass universe = {
    .allocator = cls->allocator,
    .ranges = &everything,
    .count = 1,
    .capacity = 0,
    .negated = 0,
  };

  // Symmetric difference with everything is the complement: a code point is
  // kept exactly when it is in one operand and not the other, and it is in
  // the universe always.
  return sweep(cls, &universe, CLASS_OP_SYMDIFF, limits);
}

GRX_Result grx_charclass_canonicalize(
    GRX_CharClass * cls, const GRX_Limits * limits) {
  if (!cls) {
    return GRX_ERR_INVALID;
  }
  if (!cls->negated) {
    return GRX_OK;
  }

  cls->negated = 0;
  return grx_charclass_complement(cls, limits);
}

GRX_Result grx_charclass_fold_closure(GRX_CharClass * cls, GRX_FoldKind kind,
    const GRX_Limits * limits) {
  if (!cls) {
    return GRX_ERR_INVALID;
  }

  size_t orbits = grx_unicode_orbit_table_size(kind);
  if (!orbits) {
    return GRX_OK;
  }

  // Collect first, add second. Adding while walking would let a code point
  // brought in by one orbit pull in a second, which is not what closure
  // under a folding means: the orbits partition the code points, so the
  // answer is "every orbit the *original* class touches" and reading the
  // class as it changes would be reading the answer into the question.
  GRX_CharClass additions;
  grx_charclass_init(&additions, cls->allocator);

  GRX_Result result = GRX_OK;
  for (size_t i = 0; i < orbits; i++) {
    uint32_t members[GRX_FOLD_ORBIT_MAX];
    size_t count = grx_unicode_orbit_table_at(kind, i, members);

    // Two halves rather than one, because Perl's `/aa` cuts every orbit at
    // U+0080: a class holding `s` gains `S` and not U+017F, and a class
    // holding U+00C0 still gains U+00E0. For every other folding the two
    // halves are asked the same question and answer it together.
    int apart = kind == GRX_FOLD_SIMPLE_ASCII_APART
        || kind == GRX_FOLD_FULL_ASCII_APART;
    int touched_ascii = 0;
    int touched_wide = 0;
    for (size_t j = 0; j < count; j++) {
      if (!grx_charclass_contains(cls, members[j])) {
        continue;
      }
      if (!apart || members[j] < 0x80) {
        touched_ascii = 1;
      }
      if (!apart || members[j] >= 0x80) {
        touched_wide = 1;
      }
    }
    if (!touched_ascii && !touched_wide) {
      continue;
    }

    for (size_t j = 0; j < count && result == GRX_OK; j++) {
      int side = !apart || members[j] < 0x80 ? touched_ascii : touched_wide;
      if (!side) {
        continue;
      }
      result = grx_charclass_add_range(
          &additions, members[j], members[j], limits);
    }
    if (result != GRX_OK) {
      break;
    }
  }

  if (result == GRX_OK) {
    result = grx_charclass_union(cls, &additions, limits);
  }
  grx_charclass_clear(&additions);
  return result;
}

GRX_Result grx_charclass_copy(GRX_CharClass * cls, const GRX_CharClass * other,
    const GRX_Limits * limits) {
  if (!cls || !other) {
    return GRX_ERR_INVALID;
  }
  if (cls == other) {
    return GRX_OK;
  }

  GRX_Result result = reserve(cls, other->count, limits);
  if (result != GRX_OK) {
    return result;
  }

  if (other->count) {
    memcpy(cls->ranges, other->ranges, other->count * sizeof(GRX_CharRange));
  }
  cls->count = other->count;
  cls->negated = other->negated;
  return GRX_OK;
}

int grx_charclass_equals(const GRX_CharClass * a, const GRX_CharClass * b) {
  if (!a || !b) {
    return a == b;
  }
  if (a->count != b->count || (a->negated != 0) != (b->negated != 0)) {
    return 0;
  }

  for (size_t i = 0; i < a->count; i++) {
    if (a->ranges[i].low != b->ranges[i].low
        || a->ranges[i].high != b->ranges[i].high) {
      return 0;
    }
  }

  return 1;
}

size_t grx_charclass_size(const GRX_CharClass * cls) {
  if (!cls) {
    return 0;
  }

  size_t total = 0;
  for (size_t i = 0; i < cls->count; i++) {
    total += cls->ranges[i].high - cls->ranges[i].low + 1;
  }

  return cls->negated ? (size_t)GRX_CODEPOINT_END - total : total;
}

int grx_charclass_contains(const GRX_CharClass * cls, uint32_t codepoint) {
  if (!cls) {
    return 0;
  }

  // Binary search over the sorted ranges. The class is disjoint, so the first
  // range whose high is at or above the code point is the only candidate.
  size_t low = 0;
  size_t high = cls->count;
  int found = 0;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    if (codepoint < cls->ranges[mid].low) {
      high = mid;
    }
    else if (codepoint > cls->ranges[mid].high) {
      low = mid + 1;
    }
    else {
      found = 1;
      break;
    }
  }

  return cls->negated ? !found : found;
}

void grx_charclass_clear(GRX_CharClass * cls) {
  if (!cls) {
    return;
  }

  if (cls->ranges && cls->allocator) {
    gcu_allocator_free(cls->allocator, cls->ranges);
  }
  cls->ranges = NULL;
  cls->count = 0;
  cls->capacity = 0;
  cls->negated = 0;
}

void grx_class_table_init(GRX_ClassTable * table,
    const GRX_Allocator * allocator, size_t max_ranges) {
  if (!table) {
    return;
  }

  grx_arena_init(&table->refs, allocator, sizeof(GRX_ClassRef), 0,
      GRX_DIAG_NONE);
  grx_arena_init(&table->ranges, allocator, sizeof(GRX_CharRange), max_ranges,
      GRX_DIAG_LIMIT_CLASS_RANGES);
}

GRX_Result grx_class_table_add(GRX_ClassTable * table,
    const GRX_CharRange * ranges, size_t count, uint32_t * out_index) {
  if (!table || (!ranges && count)) {
    return GRX_ERR_INVALID;
  }

  GRX_ClassRef ref = {
    .first = (uint32_t)table->ranges.count,
    .count = (uint32_t)count,
  };

  // Reserve first, so that a table which cannot hold the ranges is left
  // exactly as it was rather than holding a prefix of them.
  GRX_Result result
      = grx_arena_reserve(&table->ranges, table->ranges.count + count);
  if (result != GRX_OK) {
    return result;
  }

  for (size_t i = 0; i < count; i++) {
    result = grx_arena_append(&table->ranges, &ranges[i], NULL);
    if (result != GRX_OK) {
      return result;
    }
  }

  return grx_arena_append(&table->refs, &ref, out_index);
}

GRX_Result grx_class_table_add_class(GRX_ClassTable * table,
    const GRX_CharClass * cls, uint32_t * out_index) {
  if (!table || !cls || cls->negated) {
    return GRX_ERR_INVALID;
  }

  // A pattern names the same set several times over, and an instruction
  // stores an index, so one entry can serve all of them. The scan is linear
  // in the number of *distinct* classes, which is small; the alternative is
  // a hash of the range array, which is more code for a table that rarely
  // exceeds a dozen entries.
  for (size_t i = 0; i < table->refs.count; i++) {
    size_t count = 0;
    const GRX_CharRange * ranges
        = grx_class_table_get(table, (uint32_t)i, &count);
    if (count != cls->count) {
      continue;
    }
    if (!count || memcmp(ranges, cls->ranges,
                      count * sizeof(GRX_CharRange)) == 0) {
      if (out_index) {
        *out_index = (uint32_t)i;
      }
      return GRX_OK;
    }
  }

  return grx_class_table_add(table, cls->ranges, cls->count, out_index);
}

const GRX_CharRange * grx_class_table_get(
    const GRX_ClassTable * table, uint32_t index, size_t * out_count) {
  if (out_count) {
    *out_count = 0;
  }
  if (!table) {
    return NULL;
  }

  const GRX_ClassRef * ref = GRX_ARENA_AT(const GRX_ClassRef, &table->refs,
      index);
  if (!ref) {
    return NULL;
  }
  if (out_count) {
    *out_count = ref->count;
  }
  if (!ref->count) {
    return NULL;
  }

  return GRX_ARENA_AT(const GRX_CharRange, &table->ranges, ref->first);
}

int grx_class_table_contains(
    const GRX_ClassTable * table, uint32_t index, uint32_t codepoint) {
  size_t count = 0;
  const GRX_CharRange * ranges = grx_class_table_get(table, index, &count);
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

size_t grx_class_table_count(const GRX_ClassTable * table) {
  return table ? table->refs.count : 0;
}

void grx_class_table_clear(GRX_ClassTable * table) {
  if (!table) {
    return;
  }

  grx_arena_clear(&table->refs);
  grx_arena_clear(&table->ranges);
}
