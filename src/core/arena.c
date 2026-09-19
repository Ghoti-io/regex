/**
 * @file
 *
 * The growable array behind every table in the library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/cutil/safemath.h>
#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "arena_internal.h"

/** The first allocation's capacity. Small: most patterns are small. */
#define GRX_ARENA_INITIAL 8

void grx_arena_init(GRX_Arena * arena, const GRX_Allocator * allocator,
    size_t element_size, size_t limit, GRX_Diag diag) {
  if (!arena) {
    return;
  }

  *arena = (GRX_Arena) {
    .allocator = allocator ? allocator : grx_allocator_default(),
    .data = NULL,
    .element_size = element_size,
    .count = 0,
    .capacity = 0,
    .limit = limit,
    .diag = diag,
  };
}

GRX_Result grx_arena_reserve(GRX_Arena * arena, size_t needed) {
  if (!arena || !arena->element_size) {
    return GRX_ERR_INVALID;
  }
  if (arena->limit && needed > arena->limit) {
    return GRX_ERR_LIMIT;
  }
  if (needed <= arena->capacity) {
    return GRX_OK;
  }

  // Double until it fits, so that filling an arena of n elements costs O(n)
  // allocations' worth of copying rather than O(n) allocations.
  size_t capacity = arena->capacity ? arena->capacity : GRX_ARENA_INITIAL;
  while (capacity < needed) {
    size_t doubled;
    if (!gcu_safe_mul_size(capacity, 2, &doubled)) {
      // The doubling overflowed before reaching what was asked for. Ask for
      // exactly that instead; if the byte count overflows too, the allocation
      // below reports it.
      capacity = needed;
      break;
    }
    capacity = doubled;
  }
  if (arena->limit && capacity > arena->limit) {
    capacity = arena->limit;
  }

  size_t bytes;
  if (!gcu_safe_mul_size(capacity, arena->element_size, &bytes)) {
    return GRX_ERR_OOM;
  }

  void * grown = gcu_allocator_realloc(arena->allocator, arena->data, bytes);
  if (!grown) {
    return GRX_ERR_OOM;
  }

  arena->data = grown;
  arena->capacity = capacity;
  return GRX_OK;
}

GRX_Result grx_arena_append(
    GRX_Arena * arena, const void * element, uint32_t * out_index) {
  if (!arena || !arena->element_size) {
    return GRX_ERR_INVALID;
  }

  // An index is a uint32_t everywhere it is stored, so an arena may not grow
  // past what one can name. GRX_INDEX_NONE is reserved for "no element".
  if (arena->count >= (size_t)GRX_INDEX_NONE) {
    return GRX_ERR_LIMIT;
  }

  GRX_Result result = grx_arena_reserve(arena, arena->count + 1);
  if (result != GRX_OK) {
    return result;
  }

  unsigned char * slot
      = (unsigned char *)arena->data + arena->count * arena->element_size;
  if (element) {
    memcpy(slot, element, arena->element_size);
  }
  else {
    memset(slot, 0, arena->element_size);
  }

  if (out_index) {
    *out_index = (uint32_t)arena->count;
  }
  arena->count++;
  return GRX_OK;
}

void * grx_arena_at(const GRX_Arena * arena, size_t index) {
  if (!arena || !arena->data || index >= arena->count) {
    return NULL;
  }

  return (unsigned char *)arena->data + index * arena->element_size;
}

void grx_arena_clear(GRX_Arena * arena) {
  if (!arena) {
    return;
  }

  if (arena->data) {
    gcu_allocator_free(arena->allocator, arena->data);
  }
  arena->data = NULL;
  arena->count = 0;
  arena->capacity = 0;
}
