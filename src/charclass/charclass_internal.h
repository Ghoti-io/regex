/**
 * @file
 *
 * Private declarations for character classes.
 *
 * A class is a set of code-point ranges rather than a bitmap, because the
 * library's classes are over the whole of Unicode: `\p{L}` is some 700 ranges
 * and a bitmap of 0x110000 bits per class is not a reasonable price for it.
 *
 * Status: stub.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRX_SRC_CHARCLASS_CHARCLASS_INTERNAL_H
#define GHOTI_IO_GRX_SRC_CHARCLASS_CHARCLASS_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** One inclusive range of code points. */
typedef struct GRX_CharRange {
  uint32_t low;  ///< First code point in the range.
  uint32_t high; ///< Last code point in the range.
} GRX_CharRange;

/**
 * @brief A set of code points, as sorted disjoint ranges.
 *
 * Ranges are kept sorted and non-overlapping so that a membership test is a
 * binary search and a negated class is the complement of the same array.
 */
typedef struct GRX_CharClass {
  const GRX_Allocator * allocator; ///< The allocator the ranges came from.
  GRX_CharRange * ranges;          ///< Sorted, disjoint.
  size_t count;                    ///< Ranges in use.
  size_t capacity;                 ///< Ranges allocated.
  int negated;                     ///< Non-zero for `[^...]`.
} GRX_CharClass;

/**
 * @brief Add one inclusive range to a class.
 *
 * @param cls The class. NULL is invalid.
 * @param low First code point.
 * @param high Last code point; must be at least `low`.
 * @param limits Caps to apply. NULL applies none.
 * @return GRX_OK, GRX_ERR_INVALID, GRX_ERR_LIMIT, or GRX_ERR_OOM.
 */
GRX_Result grx_charclass_add_range(GRX_CharClass * cls, uint32_t low,
    uint32_t high, const GRX_Limits * limits);

/**
 * @brief Whether a class contains a code point.
 *
 * @param cls The class. NULL returns 0.
 * @param codepoint The code point to test.
 * @return Non-zero when the class matches it, negation included.
 */
int grx_charclass_contains(const GRX_CharClass * cls, uint32_t codepoint);

/**
 * @brief Release a class's ranges and reset it. NULL is ignored.
 *
 * @param cls The class.
 */
void grx_charclass_clear(GRX_CharClass * cls);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_CHARCLASS_CHARCLASS_INTERNAL_H
