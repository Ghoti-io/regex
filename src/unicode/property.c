/**
 * @file
 *
 * Resolving `\p{...}` to a set of code points.
 *
 * Two spelling rules over one set of tables. ECMAScript requires the exact
 * canonical name or a listed alias and rejects everything else as a
 * SyntaxError; Perl and PCRE2 ignore case, underscores, hyphens and spaces.
 * Both are binary searches, the second over a table the generator emitted
 * with the loose spelling already applied, so that this file never has to
 * normalise a table entry at run time - only the caller's text.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "tables/tables_internal.h"
#include "unicode_internal.h"

/**
 * The longest property or value spelling the tables hold.
 *
 * Loose matching has to normalise the caller's text before comparing, and
 * doing that in a fixed buffer means a name longer than any table entry is
 * rejected without allocating - which is correct, because a name longer than
 * every entry cannot match one.
 */
#define GRX_PROPERTY_NAME_MAX 96

/** Compare a table entry against a caller's name, which is not terminated. */
static int name_compare(
    const char * entry, const char * name, size_t name_length) {
  int order = strncmp(entry, name, name_length);
  if (order != 0) {
    return order;
  }
  // `entry` is NUL-terminated, `name` is not: equal over `name_length` bytes
  // means the entry is longer, and so sorts after.
  return entry[name_length] == '\0' ? 0 : 1;
}

/**
 * UAX #44 section 5.9.2: fold case and drop `_`, `-` and space.
 *
 * @return The normalised length, or 0 when the name does not fit, which no
 *   table entry does either.
 */
static size_t loosen(const char * name, size_t name_length, char * buffer) {
  size_t written = 0;
  for (size_t i = 0; i < name_length; i++) {
    char c = name[i];
    if (c == '_' || c == '-' || c == ' ') {
      continue;
    }
    if (written + 1 >= GRX_PROPERTY_NAME_MAX) {
      return 0;
    }
    if (c >= 'A' && c <= 'Z') {
      c = (char)(c - 'A' + 'a');
    }
    buffer[written++] = c;
  }
  buffer[written] = '\0';
  return written;
}

/**
 * Find an entry with this spelling and this kind.
 *
 * The table is sorted by name and then by kind, because one spelling names
 * two different sets: `Greek` is a Script value and a Script_Extensions
 * value, and they are not the same code points.
 */
static int find_name(const GRX_UnicodeName * table, size_t count,
    const char * name, size_t name_length, int kind, uint32_t * out_property) {
  size_t low = 0;
  size_t high = count;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    int order = name_compare(table[mid].name, name, name_length);
    if (order == 0) {
      order = (int)table[mid].kind - kind;
    }
    if (order > 0) {
      high = mid;
    }
    else if (order < 0) {
      low = mid + 1;
    }
    else {
      *out_property = table[mid].property;
      return 1;
    }
  }

  return 0;
}

/** Find the kind a property *name* selects: `gc`, `sc`, `scx` and their aliases. */
static int find_prop_kind(const GRX_UnicodeName * table, size_t count,
    const char * name, size_t name_length, int * out_kind) {
  for (size_t i = 0; i < count; i++) {
    if (name_compare(table[i].name, name, name_length) == 0) {
      *out_kind = (int)table[i].kind;
      return 1;
    }
  }

  return 0;
}

GRX_Result grx_unicode_property_lookup(const char * name, size_t name_length,
    const char * value, size_t value_length, GRX_PropertyMatch match,
    uint32_t * out_property) {
  if (!name || !name_length || !out_property || (!value && value_length)) {
    return GRX_ERR_INVALID;
  }

  char name_buffer[GRX_PROPERTY_NAME_MAX];
  char value_buffer[GRX_PROPERTY_NAME_MAX];
  const GRX_UnicodeName * names = grx_unicode_strict_names;
  size_t name_count = grx_unicode_strict_name_count;
  const GRX_UnicodeName * props = grx_unicode_prop_names;
  size_t prop_count = grx_unicode_prop_name_count;

  if (match == GRX_PROPERTY_LOOSE) {
    name_length = loosen(name, name_length, name_buffer);
    if (!name_length) {
      return GRX_ERR_SYNTAX;
    }
    name = name_buffer;
    if (value) {
      value_length = loosen(value, value_length, value_buffer);
      if (!value_length) {
        return GRX_ERR_SYNTAX;
      }
      value = value_buffer;
    }
    names = grx_unicode_loose_names;
    name_count = grx_unicode_loose_name_count;
    props = grx_unicode_loose_prop_names;
    prop_count = grx_unicode_loose_prop_name_count;
  }

  if (value) {
    int kind = 0;
    if (!find_prop_kind(props, prop_count, name, name_length, &kind)) {
      return GRX_ERR_SYNTAX;
    }
    if (!find_name(names, name_count, value, value_length, kind,
            out_property)) {
      return GRX_ERR_SYNTAX;
    }
    return GRX_OK;
  }

  // The lone form. A binary property name first, then a General_Category
  // value: those are the two things ECMA-262 22.2.2.9.7 allows without a
  // property name, and the loose dialects accept the same two plus the
  // synthetic classes their own hooks add.
  if (find_name(names, name_count, name, name_length, GRX_UPROP_BINARY,
          out_property)) {
    return GRX_OK;
  }
  if (find_name(names, name_count, name, name_length, GRX_UPROP_GC,
          out_property)) {
    return GRX_OK;
  }

  return GRX_ERR_SYNTAX;
}

const GRX_CharRange * grx_unicode_property_ranges(
    uint32_t property, size_t * out_count) {
  if (!out_count) {
    return NULL;
  }
  *out_count = 0;
  if (property >= grx_unicode_property_count) {
    return NULL;
  }

  *out_count = grx_unicode_properties[property].count;
  return &grx_unicode_ranges[grx_unicode_properties[property].first];
}

const char * grx_unicode_property_name(uint32_t property) {
  if (property >= grx_unicode_property_count) {
    return NULL;
  }

  return grx_unicode_properties[property].name;
}

size_t grx_unicode_property_total(uint32_t property) {
  if (property >= grx_unicode_property_count) {
    return 0;
  }

  return grx_unicode_properties[property].total;
}

// --------------------------------------------------------------------------
// Properties of strings
// --------------------------------------------------------------------------

GRX_Result grx_unicode_string_set_lookup(
    const char * name, size_t name_length, uint32_t * out_set) {
  if (!name || !out_set) {
    return GRX_ERR_INVALID;
  }

  // Seven entries, compared linearly. A binary search would need the table
  // sorted by name, and the table is sorted by *layout* so that RGI_Emoji
  // can be the whole of it; seven string comparisons is the cheaper of the
  // two things to give up.
  for (size_t i = 0; i < grx_unicode_string_set_count; i++) {
    const char * candidate = grx_unicode_string_sets[i].name;
    if (strlen(candidate) == name_length
        && memcmp(candidate, name, name_length) == 0) {
      *out_set = (uint32_t)i;
      return GRX_OK;
    }
  }
  return GRX_ERR_SYNTAX;
}

size_t grx_unicode_string_set_size(uint32_t set) {
  if (set >= grx_unicode_string_set_count) {
    return 0;
  }
  return grx_unicode_string_sets[set].count;
}

size_t grx_unicode_string_set_at(
    uint32_t set, size_t index, const uint32_t ** out_points) {
  if (!out_points || set >= grx_unicode_string_set_count
      || index >= grx_unicode_string_sets[set].count) {
    return 0;
  }
  const GRX_UnicodeString * sequence
      = &grx_unicode_strings[grx_unicode_string_sets[set].first + index];
  *out_points = &grx_unicode_string_points[sequence->first];
  return sequence->length;
}
