/**
 * @file allocator.h
 *
 * Allocator abstraction for the Ghoti.io Regex library.
 *
 * This is cutil's @ref GCU_Allocator under a local name, the same arrangement
 * the compress, image and model libraries use. One definition across the suite
 * means an allocator written for any of them works with all of them, rather
 * than needing a near-identical copy per library.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRX_ALLOCATOR_H
#define GHOTI_IO_GRX_ALLOCATOR_H

#include <ghoti.io/cutil/allocator.h>
#include <ghoti.io/regex/macros.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Allocator interface used by the library.
 *
 * All function pointers must be non-NULL. Each receives the `ctx` pointer from
 * the struct as its first argument.
 *
 * Two requirements beyond the C library equivalents: `calloc_fn` must treat
 * overflow of `nitems * size` as an allocation failure and return NULL rather
 * than allocating a truncated block, and a zero-size request should return a
 * usable non-NULL pointer, so that NULL always means failure.
 */
typedef GCU_Allocator GRX_Allocator;

/**
 * @brief Get the default allocator (stdlib-backed).
 *
 * @return Pointer to a process-global allocator instance.
 */
GRX_API const GRX_Allocator * grx_allocator_default(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_ALLOCATOR_H
