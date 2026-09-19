/**
 * @file
 *
 * Case folding: the two foldings the dialects need, and their orbits.
 *
 * Both are binary searches over generated tables. There is no arithmetic
 * here and there must not be: the "add 32 to fold an upper-case letter"
 * shortcut is wrong for Cherokee, wrong for Deseret, wrong for the Kelvin
 * sign, and wrong in a way that only shows up for the scripts nobody writes
 * a test for. The table is the specification.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <stddef.h>
#include <stdint.h>

#include "tables/tables_internal.h"
#include "unicode_internal.h"

/** Binary search a sorted case-mapping table. */
static uint32_t map_lookup(const GRX_UnicodeCaseMap * table, size_t count,
    uint32_t codepoint) {
  size_t low = 0;
  size_t high = count;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    if (codepoint < table[mid].from) {
      high = mid;
    }
    else if (codepoint > table[mid].from) {
      low = mid + 1;
    }
    else {
      return table[mid].to;
    }
  }

  return codepoint;
}

/**
 * Binary search a sorted orbit index, copying the members out.
 *
 * A code point with no entry is alone in its orbit, and the caller is given
 * a one-member result rather than zero, so that "expand this to its orbit"
 * is one code path whether or not the character has a case.
 */
static size_t orbit_lookup(const GRX_UnicodeOrbit * index, size_t count,
    const uint32_t * members, uint32_t codepoint,
    uint32_t out[GRX_FOLD_ORBIT_MAX]) {
  size_t low = 0;
  size_t high = count;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    if (codepoint < index[mid].code) {
      high = mid;
    }
    else if (codepoint > index[mid].code) {
      low = mid + 1;
    }
    else {
      size_t written = index[mid].count;
      if (written > GRX_FOLD_ORBIT_MAX) {
        written = GRX_FOLD_ORBIT_MAX;
      }
      for (size_t i = 0; i < written; i++) {
        out[i] = members[index[mid].first + i];
      }
      return written;
    }
  }

  out[0] = codepoint;
  return 1;
}

uint32_t grx_unicode_fold_simple(uint32_t codepoint) {
  return map_lookup(
      grx_unicode_fold_map, grx_unicode_fold_map_count, codepoint);
}

size_t grx_unicode_fold_orbit(
    uint32_t codepoint, uint32_t out[GRX_FOLD_ORBIT_MAX]) {
  if (!out) {
    return 0;
  }

  return orbit_lookup(grx_unicode_fold_orbits, grx_unicode_fold_orbit_count,
      grx_unicode_fold_orbit_members, codepoint, out);
}

uint32_t grx_unicode_es_legacy_canonicalize(uint32_t codepoint) {
  return map_lookup(grx_unicode_es_legacy_map,
      grx_unicode_es_legacy_map_count, codepoint);
}

size_t grx_unicode_es_legacy_orbit(
    uint32_t codepoint, uint32_t out[GRX_FOLD_ORBIT_MAX]) {
  if (!out) {
    return 0;
  }

  return orbit_lookup(grx_unicode_es_legacy_orbits,
      grx_unicode_es_legacy_orbit_count, grx_unicode_es_legacy_orbit_members,
      codepoint, out);
}
