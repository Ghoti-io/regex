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
 * Case folding: the two foldings the dialects need, and their orbits.
 *
 * Both are binary searches over generated tables. There is no arithmetic
 * here and there must not be: the "add 32 to fold an upper-case letter"
 * shortcut is wrong for Cherokee, wrong for Deseret, wrong for the Kelvin
 * sign, and wrong in a way that only shows up for the scripts nobody writes
 * a test for. The table is the specification.
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

uint32_t grx_unicode_upper_simple(uint32_t codepoint) {
  return map_lookup(grx_unicode_simple_upper_map,
      grx_unicode_simple_upper_map_count, codepoint);
}

uint32_t grx_unicode_lower_simple(uint32_t codepoint) {
  return map_lookup(grx_unicode_simple_lower_map,
      grx_unicode_simple_lower_map_count, codepoint);
}

size_t grx_unicode_fold_orbit(
    uint32_t codepoint, uint32_t out[GRX_FOLD_ORBIT_MAX]) {
  if (!out) {
    return 0;
  }

  return orbit_lookup(grx_unicode_fold_orbits, grx_unicode_fold_orbit_count,
      grx_unicode_fold_orbit_members, codepoint, out);
}

/**
 * Binary search the full-fold table.
 *
 * @return The entry for `codepoint`, or NULL when its full fold is its
 *   simple one - which is every code point but a hundred and four.
 */
static const GRX_UnicodeFullFold * full_fold_lookup(uint32_t codepoint) {
  size_t low = 0;
  size_t high = grx_unicode_full_fold_count;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    if (codepoint < grx_unicode_full_folds[mid].code) {
      high = mid;
    }
    else if (codepoint > grx_unicode_full_folds[mid].code) {
      low = mid + 1;
    }
    else {
      return &grx_unicode_full_folds[mid];
    }
  }

  return NULL;
}

size_t grx_unicode_fold_full(
    uint32_t codepoint, uint32_t out[GRX_FULL_FOLD_MAX]) {
  if (!out) {
    return 0;
  }

  const GRX_UnicodeFullFold * entry = full_fold_lookup(codepoint);
  if (!entry) {
    out[0] = grx_unicode_fold_simple(codepoint);
    return 1;
  }

  for (size_t i = 0; i < entry->length; i++) {
    out[i] = entry->to[i];
  }
  return entry->length;
}

size_t grx_unicode_fold_full_sources(const uint32_t * sequence, size_t length,
    uint32_t out[GRX_FULL_FOLD_SOURCE_MAX]) {
  if (!out || !sequence || !length || length > GRX_FULL_FOLD_MAX) {
    return 0;
  }

  size_t written = 0;

  // A one-code-point target is the ordinary case and its sources are the
  // simple orbit - less any member whose *full* fold is longer, because that
  // member covers more of the target than this one position. U+1E9E is the
  // example: it shares a simple orbit with U+00DF and folds fully to "ss",
  // so it belongs to the two-position edge and not to this one.
  if (length == 1) {
    uint32_t orbit[GRX_FOLD_ORBIT_MAX];
    size_t count = grx_unicode_fold_orbit(sequence[0], orbit);
    for (size_t i = 0; i < count && written < GRX_FULL_FOLD_SOURCE_MAX; i++) {
      if (!full_fold_lookup(orbit[i])) {
        out[written++] = orbit[i];
      }
    }
    return written;
  }

  // A longer target can only come from the full-fold table, which is small
  // enough to walk: a hundred and four entries, asked once per edge while a
  // pattern is being compiled.
  for (size_t i = 0;
      i < grx_unicode_full_fold_count && written < GRX_FULL_FOLD_SOURCE_MAX;
      i++) {
    const GRX_UnicodeFullFold * entry = &grx_unicode_full_folds[i];
    if (entry->length != length) {
      continue;
    }
    size_t same = 0;
    while (same < length && entry->to[same] == sequence[same]) {
      same++;
    }
    if (same == length) {
      out[written++] = entry->code;
    }
  }

  return written;
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
    // Full folding's orbits *are* the simple ones: what it adds is not a
    // wider set of characters but the ability for one of them to stand for
    // several, which no orbit can say.
    case GRX_FOLD_FULL:
      return grx_unicode_fold_orbit(codepoint, out);
    case GRX_FOLD_SIMPLE_ASCII_APART:
    // Full folding's orbits are the simple ones here too; what `/aa` adds is
    // the cut below, and which *full* folds survive it, which is lowering's
    // business and not an orbit's.
    case GRX_FOLD_FULL_ASCII_APART: {
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
    case GRX_FOLD_FULL:
    case GRX_FOLD_FULL_ASCII_APART:
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
    case GRX_FOLD_FULL:
    case GRX_FOLD_FULL_ASCII_APART:
      // The same table. Where they differ is in what a *class* closure
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
