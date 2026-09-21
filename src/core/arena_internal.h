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
 * A growable array of fixed-size elements, with its own cap and its own
 * diagnostic.
 *
 * Every table in this library - the AST's nodes, its literal runs, its class
 * items, the IR's nodes, the program's instructions, the class ranges - is
 * one of these. They are arrays rather than linked structures so that a tree
 * is one allocation that can be grown, freed and bounds-checked as a unit,
 * and so that a child reference is an index a range check can validate
 * rather than a pointer it cannot.
 *
 * The cap and the diagnostic live on the arena because that is what makes
 * limit enforcement a single tested path: a phase says "these are nodes,
 * there may be at most max_nodes of them, and exceeding that is
 * GRX_DIAG_LIMIT_NODES" once, at init, and every append afterwards is
 * checked without the caller remembering to.
 *
 * cutil's typed vectors serve where the element is an integer and the caller
 * wants cutil's own iteration; these serve where the element is a struct.
 */

#ifndef GHOTI_IO_GRX_SRC_CORE_ARENA_INTERNAL_H
#define GHOTI_IO_GRX_SRC_CORE_ARENA_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The index meaning "no element".
 *
 * Distinct from index 0, which is a real element - the root of a tree lives
 * there - so a link field cannot use 0 to mean "none".
 */
#define GRX_INDEX_NONE UINT32_MAX

/** @brief A growable array of fixed-size elements. */
typedef struct GRX_Arena {
  const GRX_Allocator * allocator; ///< Where the storage comes from.
  void * data;         ///< The elements, `count` of them in use.
  size_t element_size; ///< Bytes per element; fixed at init.
  size_t count;        ///< Elements in use.
  size_t capacity;     ///< Elements allocated.
  size_t limit;        ///< Cap on `count`; 0 means no cap.
  GRX_Diag diag;       ///< Reported when `limit` would be exceeded.
} GRX_Arena;

/**
 * @brief Prepare an arena. Allocates nothing.
 *
 * @param arena The arena. NULL is ignored.
 * @param allocator Where storage will come from. NULL uses the default.
 * @param element_size Bytes per element. Must not be 0.
 * @param limit Cap on the element count; 0 for no cap.
 * @param diag The diagnostic to report when `limit` would be exceeded.
 */
void grx_arena_init(GRX_Arena * arena, const GRX_Allocator * allocator,
    size_t element_size, size_t limit, GRX_Diag diag);

/**
 * @brief Make room for at least `needed` elements in total.
 *
 * @param arena The arena. NULL is GRX_ERR_INVALID.
 * @param needed Total element capacity wanted.
 * @return GRX_OK; GRX_ERR_LIMIT when `needed` exceeds the arena's cap;
 *   GRX_ERR_OOM when the allocator refused; GRX_ERR_INVALID for a NULL arena.
 */
GRX_Result grx_arena_reserve(GRX_Arena * arena, size_t needed);

/**
 * @brief Append one element, copying `element_size` bytes from `element`.
 *
 * @param arena The arena. NULL is GRX_ERR_INVALID.
 * @param element The element to copy in. NULL appends a zeroed element,
 *   which is how a node is reserved before its fields are known.
 * @param out_index Receives the new element's index. Optional.
 * @return GRX_OK, GRX_ERR_LIMIT, GRX_ERR_OOM, or GRX_ERR_INVALID.
 */
GRX_Result grx_arena_append(
    GRX_Arena * arena, const void * element, uint32_t * out_index);

/**
 * @brief The element at an index.
 *
 * @param arena The arena.
 * @param index The index.
 * @return A pointer to the element, or NULL when the index is out of range
 *   or the arena is NULL. Invalidated by the next append.
 */
void * grx_arena_at(const GRX_Arena * arena, size_t index);

/**
 * @brief Release an arena's storage and reset it to empty.
 *
 * The arena stays usable: its allocator, element size, cap and diagnostic
 * survive, so it may be filled again.
 *
 * @param arena The arena. NULL is ignored.
 */
void grx_arena_clear(GRX_Arena * arena);

/** @brief Typed access, for a caller that knows the element type. */
#define GRX_ARENA_AT(type, arena, index)                                       \
  ((type *)grx_arena_at((arena), (index)))

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_CORE_ARENA_INTERNAL_H
