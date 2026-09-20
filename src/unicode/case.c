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

size_t grx_unicode_orbit(GRX_FoldKind kind, uint32_t codepoint,
    uint32_t out[GRX_FOLD_ORBIT_MAX]) {
  if (!out) {
    return 0;
  }

  switch (kind) {
    case GRX_FOLD_SIMPLE:
      return grx_unicode_fold_orbit(codepoint, out);
    case GRX_FOLD_SIMPLE_ASCII_APART: {
      // The simple orbit, less whatever is on the other side of U+0080.
      size_t count = grx_unicode_fold_orbit(codepoint, out);
      int ascii = codepoint < 0x80;
      size_t kept = 0;
      for (size_t i = 0; i < count; i++) {
        if ((out[i] < 0x80) == ascii) {
          out[kept++] = out[i];
        }
      }
      return kept;
    }
    case GRX_FOLD_ES_LEGACY:
      return grx_unicode_es_legacy_orbit(codepoint, out);
    case GRX_FOLD_ASCII:
      if (codepoint >= 'A' && codepoint <= 'Z') {
        out[0] = codepoint;
        out[1] = codepoint - 'A' + 'a';
        return 2;
      }
      if (codepoint >= 'a' && codepoint <= 'z') {
        out[0] = codepoint - 'a' + 'A';
        out[1] = codepoint;
        return 2;
      }
      out[0] = codepoint;
      return 1;
    case GRX_FOLD_NONE:
    case GRX_FOLD_COUNT:
    default:
      out[0] = codepoint;
      return 1;
  }
}

size_t grx_unicode_orbit_table_size(GRX_FoldKind kind) {
  switch (kind) {
    case GRX_FOLD_SIMPLE:
    case GRX_FOLD_SIMPLE_ASCII_APART:
      return grx_unicode_fold_orbit_count;
    case GRX_FOLD_ES_LEGACY:
      return grx_unicode_es_legacy_orbit_count;
    case GRX_FOLD_ASCII:
      return 26;
    case GRX_FOLD_NONE:
    case GRX_FOLD_COUNT:
    default:
      return 0;
  }
}

size_t grx_unicode_orbit_table_at(GRX_FoldKind kind, size_t index,
    uint32_t out[GRX_FOLD_ORBIT_MAX]) {
  if (!out || index >= grx_unicode_orbit_table_size(kind)) {
    return 0;
  }

  const GRX_UnicodeOrbit * entry;
  const uint32_t * members;
  switch (kind) {
    case GRX_FOLD_SIMPLE:
    case GRX_FOLD_SIMPLE_ASCII_APART:
      // The same table. Where the two differ is in what a *class* closure
      // does with an orbit that straddles U+0080, which is
      // grx_charclass_fold_closure()'s business and not this table's.
      entry = &grx_unicode_fold_orbits[index];
      members = grx_unicode_fold_orbit_members;
      break;
    case GRX_FOLD_ES_LEGACY:
      entry = &grx_unicode_es_legacy_orbits[index];
      members = grx_unicode_es_legacy_orbit_members;
      break;
    case GRX_FOLD_ASCII:
      out[0] = (uint32_t)('A' + index);
      out[1] = (uint32_t)('a' + index);
      return 2;
    case GRX_FOLD_NONE:
    case GRX_FOLD_COUNT:
    default:
      return 0;
  }

  size_t written = entry->count;
  if (written > GRX_FOLD_ORBIT_MAX) {
    written = GRX_FOLD_ORBIT_MAX;
  }
  for (size_t i = 0; i < written; i++) {
    out[i] = members[entry->first + i];
  }
  return written;
}
