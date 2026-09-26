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
 * Private declarations for character classes.
 *
 * A class is a set of code-point ranges rather than a bitmap, because the
 * library's classes are over the whole of Unicode: `\p{L}` is some 700 ranges
 * and a bitmap of 0x110000 bits per class is not a reasonable price for it.
 *
 * There are two forms, and the difference is whether anything may still edit
 * the set. A @ref GRX_CharClass is the editable one, used while lowering
 * evaluates a class expression: it grows, it takes set operations, it closes
 * under a case folding. A @ref GRX_ClassRef in a @ref GRX_ClassTable is what
 * is left when lowering is done - a span of a shared range array that the IR,
 * the program and the engines all read and none of them writes.
 */

#ifndef GHOTI_IO_GRX_SRC_CHARCLASS_CHARCLASS_INTERNAL_H
#define GHOTI_IO_GRX_SRC_CHARCLASS_CHARCLASS_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/arena_internal.h"
#include "../core/range_internal.h"
#include "../unicode/unicode_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

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
 * @brief Prepare an empty class. Allocates nothing.
 *
 * @param cls The class. NULL is ignored.
 * @param allocator Where ranges will come from. NULL uses the default.
 */
void grx_charclass_init(GRX_CharClass * cls, const GRX_Allocator * allocator);

/**
 * @brief Add one inclusive range to a class, keeping it sorted and disjoint.
 *
 * Merges with every range the new one overlaps *or touches*: `[a-c]` and
 * `[d-f]` become one range, not two. That matters beyond tidiness - the
 * canonical form has to be unique, or two spellings of the same set would
 * produce two classes and the table's deduplication would miss them.
 *
 * @param cls The class. NULL is invalid.
 * @param low First code point.
 * @param high Last code point; must be at least `low` and at most
 *   GRX_CODEPOINT_MAX.
 * @param limits Caps to apply. NULL applies none.
 * @return GRX_OK, GRX_ERR_INVALID, GRX_ERR_LIMIT, or GRX_ERR_OOM.
 */
GRX_Result grx_charclass_add_range(GRX_CharClass * cls, uint32_t low,
    uint32_t high, const GRX_Limits * limits);

/**
 * @brief Add a whole sorted, disjoint range array - a Unicode property, say.
 *
 * @param cls The class. NULL is invalid.
 * @param ranges The ranges. May be NULL only when `count` is 0.
 * @param count Number of ranges.
 * @param limits Caps to apply. NULL applies none.
 * @return GRX_OK, GRX_ERR_INVALID, GRX_ERR_LIMIT, or GRX_ERR_OOM.
 */
GRX_Result grx_charclass_add_ranges(GRX_CharClass * cls,
    const GRX_CharRange * ranges, size_t count, const GRX_Limits * limits);

/**
 * @brief Add every code point of a resolved Unicode property.
 *
 * The one place that materialises a property's ranges, so that the buffer
 * they land in is written down once rather than at each of the three call
 * sites that used to read the table directly. The ranges come from
 * ghoti.io-unicode; `grx_unicode_property_ranges()` says what bounds them.
 *
 * @param cls The class. NULL is invalid.
 * @param property The index grx_unicode_property_lookup() returned.
 * @param limits Caps to apply. NULL applies none.
 * @return GRX_OK, or what grx_unicode_property_ranges() and
 *   grx_charclass_add_range() return.
 */
GRX_Result grx_charclass_add_property(GRX_CharClass * cls, uint32_t property,
    const GRX_Limits * limits);

/**
 * @brief The set operations, each replacing `cls` with the result.
 *
 * Both operands are read as the sets they *denote*, so a negated class
 * behaves as its complement without the caller having to apply the negation
 * first; the result is never negated, because the operation has already been
 * evaluated over the real sets.
 *
 * `cls` and `other` may be the same object.
 *
 * @param cls Left operand, and where the result lands. NULL is invalid.
 * @param other Right operand. NULL is invalid.
 * @param limits Caps to apply. NULL applies none.
 * @return GRX_OK, GRX_ERR_INVALID, GRX_ERR_LIMIT, or GRX_ERR_OOM.
 */
GRX_Result grx_charclass_union(GRX_CharClass * cls, const GRX_CharClass * other,
    const GRX_Limits * limits);

/** @copydoc grx_charclass_union */
GRX_Result grx_charclass_intersect(GRX_CharClass * cls,
    const GRX_CharClass * other, const GRX_Limits * limits);

/** @copydoc grx_charclass_union */
GRX_Result grx_charclass_subtract(GRX_CharClass * cls,
    const GRX_CharClass * other, const GRX_Limits * limits);

/** @copydoc grx_charclass_union */
GRX_Result grx_charclass_symdiff(GRX_CharClass * cls,
    const GRX_CharClass * other, const GRX_Limits * limits);

/**
 * @brief Replace a class with every code point it does not contain.
 *
 * Clears the negation flag, because the negation has now been performed.
 *
 * @param cls The class. NULL is invalid.
 * @param limits Caps to apply. NULL applies none.
 * @return GRX_OK, GRX_ERR_INVALID, GRX_ERR_LIMIT, or GRX_ERR_OOM.
 */
GRX_Result grx_charclass_complement(
    GRX_CharClass * cls, const GRX_Limits * limits);

/**
 * @brief Apply the negation flag, leaving a class that denotes itself.
 *
 * What lowering calls before handing a class to the class table: past this
 * point nothing consults `negated`, and an engine testing membership does a
 * binary search and believes the answer.
 *
 * @param cls The class. NULL is invalid.
 * @param limits Caps to apply. NULL applies none.
 * @return GRX_OK, GRX_ERR_INVALID, GRX_ERR_LIMIT, or GRX_ERR_OOM.
 */
GRX_Result grx_charclass_canonicalize(
    GRX_CharClass * cls, const GRX_Limits * limits);

/**
 * @brief Add every code point that matches one already present, under a
 * folding.
 *
 * This is what makes a caseless class an ordinary class: after it, no engine
 * needs to know that the pattern said `i`. It walks the folding's orbit
 * table rather than the class, so the cost is the size of the table and not
 * the size of the class - `[^\x00]` is 1.1 million code points and 2,994
 * orbits.
 *
 * Applied to the *denoted* set, so it must run after any negation is
 * resolved; a caller closing `[^a]` under folding wants the complement of
 * the closure of `{a}`, and grx_charclass_canonicalize() first is how it
 * says so.
 *
 * @param cls The class. NULL is invalid.
 * @param kind Which folding. GRX_FOLD_NONE does nothing.
 * @param limits Caps to apply. NULL applies none.
 * @return GRX_OK, GRX_ERR_INVALID, GRX_ERR_LIMIT, or GRX_ERR_OOM.
 */
GRX_Result grx_charclass_fold_closure(GRX_CharClass * cls, GRX_FoldKind kind,
    const GRX_Limits * limits);

/**
 * @brief Replace one class's contents with a copy of another's.
 *
 * @param cls Destination. NULL is invalid.
 * @param other Source. NULL is invalid.
 * @param limits Caps to apply. NULL applies none.
 * @return GRX_OK, GRX_ERR_INVALID, GRX_ERR_LIMIT, or GRX_ERR_OOM.
 */
GRX_Result grx_charclass_copy(GRX_CharClass * cls, const GRX_CharClass * other,
    const GRX_Limits * limits);

/**
 * @brief Whether two classes denote the same set.
 *
 * @param a First class. NULL denotes nothing.
 * @param b Second class.
 * @return Non-zero when they are equal.
 */
int grx_charclass_equals(const GRX_CharClass * a, const GRX_CharClass * b);

/**
 * @brief The number of code points a class denotes.
 *
 * @param cls The class. NULL returns 0.
 * @return The count.
 */
size_t grx_charclass_size(const GRX_CharClass * cls);

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

/**
 * @brief One canonical class in a @ref GRX_ClassTable: a span of ranges.
 *
 * A class becomes a span rather than an object once it is canonical, because
 * from that point nothing edits it: lowering has applied negation, evaluated
 * the set operations, expanded the properties and closed it under case
 * folding, and what remains is a sorted disjoint array that the IR, the
 * program and the engines all read and none of them writes.
 */
typedef struct GRX_ClassRef {
  uint32_t first; ///< Index of the first range in the table's range arena.
  uint32_t count; ///< Number of ranges.
} GRX_ClassRef;

/**
 * @brief Every canonical class a pattern needs, in two flat arrays.
 *
 * Referred to by index from an IR node or an instruction, so that a class is
 * stored once however many times it is used, and so that an instruction stays
 * fixed-size however large its class is.
 */
typedef struct GRX_ClassTable {
  GRX_Arena refs;   ///< GRX_ClassRef, one per class.
  GRX_Arena ranges; ///< GRX_CharRange, shared by every class.
} GRX_ClassTable;

/**
 * @brief Prepare an empty class table. Allocates nothing.
 *
 * @param table The table. NULL is ignored.
 * @param allocator Where storage will come from. NULL uses the default.
 * @param max_ranges Cap on the total range count; 0 for no cap.
 */
void grx_class_table_init(GRX_ClassTable * table,
    const GRX_Allocator * allocator, size_t max_ranges);

/**
 * @brief Add one canonical class, copying its ranges in.
 *
 * The ranges must already be sorted and disjoint; this is the point at which
 * a class stops being editable, not the point at which it is made canonical.
 *
 * @param table The table. NULL is GRX_ERR_INVALID.
 * @param ranges The ranges. May be NULL only when `count` is 0, which adds
 *   the empty class - a class that matches nothing, which is what a negated
 *   class covering every code point canonicalises to.
 * @param count Number of ranges.
 * @param out_index Receives the new class's index. Optional.
 * @return GRX_OK, GRX_ERR_LIMIT, GRX_ERR_OOM, or GRX_ERR_INVALID.
 */
GRX_Result grx_class_table_add(GRX_ClassTable * table,
    const GRX_CharRange * ranges, size_t count, uint32_t * out_index);

/**
 * @brief Add a canonical class, reusing an identical one already present.
 *
 * Deduplication is here rather than at the call sites because a pattern
 * names the same set repeatedly - `[a-z]` in four places, `\w` in six - and
 * an instruction stores an index, so one entry serves all of them. The class
 * must already be canonical: this refuses a class whose negation flag is
 * still set rather than quietly storing the wrong set.
 *
 * @param table The table. NULL is GRX_ERR_INVALID.
 * @param cls The class. NULL, or one still flagged negated, is
 *   GRX_ERR_INVALID.
 * @param out_index Receives the class's index. Optional.
 * @return GRX_OK, GRX_ERR_LIMIT, GRX_ERR_OOM, or GRX_ERR_INVALID.
 */
GRX_Result grx_class_table_add_class(GRX_ClassTable * table,
    const GRX_CharClass * cls, uint32_t * out_index);

/**
 * @brief The ranges of one class in a table.
 *
 * @param table The table.
 * @param index The class index.
 * @param out_count Receives the range count. Optional.
 * @return The first range, or NULL when the index is out of range or the
 *   class is empty.
 */
const GRX_CharRange * grx_class_table_get(
    const GRX_ClassTable * table, uint32_t index, size_t * out_count);

/**
 * @brief Whether a class in a table contains a code point.
 *
 * A binary search, which is what the sorted disjoint representation buys.
 *
 * @param table The table.
 * @param index The class index.
 * @param codepoint The code point to test.
 * @return Non-zero when the class matches it.
 */
int grx_class_table_contains(
    const GRX_ClassTable * table, uint32_t index, uint32_t codepoint);

/**
 * @brief The number of classes in a table.
 *
 * @param table The table. NULL returns 0.
 * @return The class count.
 */
size_t grx_class_table_count(const GRX_ClassTable * table);

/**
 * @brief Release a class table's storage. NULL is ignored.
 *
 * @param table The table.
 */
void grx_class_table_clear(GRX_ClassTable * table);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_CHARCLASS_CHARCLASS_INTERNAL_H
