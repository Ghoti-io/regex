/**
 * @file
 *
 * Every table this library still owns, against ghoti.io-unicode.
 *
 * Phase E of unicode's design.md moved most of the Unicode Character
 * Database out of this library. What it could not move is here, and each
 * piece is a *second copy* of data the suite already has - which is the
 * exact arrangement that puts two versions of one standard in one build and
 * lets them drift apart in silence. Nothing else would notice: both sides
 * are generated from a UCD, both gates are green, and the first symptom is
 * a pattern that answers differently depending on which table a construct
 * happens to read.
 *
 * So this is the gate design.md asked for and deferred twice - "the sweep
 * sums match regex's property.c for every property both have" - and it is
 * only possible because both libraries pin UCD 17.0.0. The suite-level
 * check-ucd-pins.sh is what keeps that true; this is what makes it mean
 * something.
 *
 * What is compared, exhaustively:
 *
 *   - every property in grx_unicode_properties[] that is not a
 *     Numeric_Value, as a set of code points, against guni_set_ranges();
 *   - the General_Category *groups*, which this library spells as values
 *     and the unicode library spells as masks;
 *   - every Numeric_Value property, by asking guni_numeric_value() of each
 *     member, since a numeric value is reached by arithmetic rather than by
 *     name and neither library has a set for it.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/regex.h>
#include <ghoti.io/unicode/char.h>
#include <ghoti.io/unicode/set.h>
#include <ghoti.io/unicode/unicode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/unicode/tables/tables_internal.h"
#include "../../src/unicode/unicode_internal.h"

#define MAXCP 0x110000

static unsigned char ours[MAXCP];
static unsigned char theirs[MAXCP];

/* The unicode library merges adjacent runs, so its count is never larger
 * than this library's; the buffer is sized from the larger of the two plus
 * room, and a set that outgrew it is reported rather than truncated. */
#define MAX_RANGES 4096
static GUNI_Range ranges[MAX_RANGES];

/**
 * The General_Category groups, which are not values.
 *
 * `\p{L}` is five categories. This library stores the union as a property of
 * its own; the unicode library spells it as a mask, because giving a group a
 * value would have put a fake member in its General_Category enum. Same set,
 * two spellings, which is why the mapping is here rather than being a
 * disagreement.
 */
static uint32_t gc_mask_for(const char * value) {
  if (!strcmp(value, "Other")) { return GUNI_GC_MASK_C; }
  if (!strcmp(value, "Letter")) { return GUNI_GC_MASK_L; }
  if (!strcmp(value, "Cased_Letter")) { return GUNI_GC_MASK_LC; }
  if (!strcmp(value, "Mark")) { return GUNI_GC_MASK_M; }
  if (!strcmp(value, "Number")) { return GUNI_GC_MASK_N; }
  if (!strcmp(value, "Punctuation")) { return GUNI_GC_MASK_P; }
  if (!strcmp(value, "Symbol")) { return GUNI_GC_MASK_S; }
  if (!strcmp(value, "Separator")) { return GUNI_GC_MASK_Z; }
  return 0;
}

int main(void) {
  size_t agreed = 0, differed = 0, unresolved = 0, shown = 0;
  size_t nv_properties = 0, nv_members = 0, nv_bad = 0;

  for (uint32_t index = 0; index < grx_unicode_property_count; index++) {
    const GRX_UnicodeProperty * property = &grx_unicode_properties[index];
    size_t count = 0;
    const GRX_CharRange * mine = grx_unicode_property_ranges(index, &count);

    /* Numeric_Value: no set on either side, so each member is asked. */
    if (property->kind == GRX_UPROP_NV) {
      nv_properties++;
      for (size_t k = 0; k < count; k++) {
        for (uint32_t code = mine[k].low; code <= mine[k].high; code++) {
          int64_t numerator = 0;
          uint32_t denominator = 0;
          nv_members++;
          if (!guni_numeric_value(code, &numerator, &denominator)) {
            if (shown < 10) {
              printf("  nv  U+%05X is in this library's %s and has no "
                  "Numeric_Value in the unicode library\n",
                  code, property->name);
              shown++;
            }
            nv_bad++;
          }
        }
      }
      continue;
    }

    memset(ours, 0, sizeof ours);
    memset(theirs, 0, sizeof theirs);
    for (size_t k = 0; k < count; k++) {
      for (uint32_t code = mine[k].low; code <= mine[k].high; code++) {
        ours[code] = 1;
      }
    }

    const char * name = "";
    const char * value = property->name;
    uint32_t mask = 0;
    switch (property->kind) {
      case GRX_UPROP_GC:     name = "General_Category";  break;
      case GRX_UPROP_SCRIPT: name = "Script";            break;
      case GRX_UPROP_SCX:    name = "Script_Extensions"; break;
      default:               name = property->name; value = "Y"; break;
    }
    if (property->kind == GRX_UPROP_GC) {
      mask = gc_mask_for(property->name);
    }

    size_t got = 0;
    if (mask) {
      if (guni_gc_mask_ranges(mask, ranges, MAX_RANGES, &got) != GUNI_OK) {
        printf("  UNRESOLVED mask for %s (needs %zu ranges)\n",
            property->name, got);
        unresolved++;
        continue;
      }
    }
    else {
      GUNI_Property which;
      uint32_t number;
      if (guni_property_by_name(name, strlen(name), &which) != GUNI_OK) {
        printf("  UNRESOLVED property %s\n", name);
        unresolved++;
        continue;
      }
      if (guni_value_by_name(which, value, strlen(value), &number)
          != GUNI_OK) {
        /* Indic_Conjunct_Break is this library's one binary reading of an
         * enumerated property: its set is "InCB is not None", which is the
         * union of the other three values. Spelled here rather than left
         * unresolved, because an unresolved row is a row not compared. */
        if (!strcmp(name, "Indic_Conjunct_Break")) {
          GUNI_Property incb;
          guni_property_by_name(name, strlen(name), &incb);
          uint32_t none = 0;
          if (guni_value_by_name(incb, "None", 4, &none) == GUNI_OK) {
            for (uint32_t code = 0; code < MAXCP; code++) {
              theirs[code]
                  = (uint32_t)guni_property_value(code, incb) != none;
            }
            goto compare;
          }
        }
        printf("  UNRESOLVED value %s=%s\n", name, value);
        unresolved++;
        continue;
      }
      if (guni_set_ranges(which, number, ranges, MAX_RANGES, &got)
          != GUNI_OK) {
        printf("  UNRESOLVED ranges for %s=%s (needs %zu, buffer %d)\n",
            name, value, got, MAX_RANGES);
        unresolved++;
        continue;
      }
    }
    for (size_t k = 0; k < got; k++) {
      for (uint32_t code = ranges[k].first;
          code <= ranges[k].last && code < MAXCP; code++) {
        theirs[code] = 1;
      }
    }

  compare:;
    long only_ours = 0, only_theirs = 0;
    for (uint32_t code = 0; code < MAXCP; code++) {
      if (ours[code] && !theirs[code]) { only_ours++; }
      else if (!ours[code] && theirs[code]) { only_theirs++; }
    }
    if (only_ours || only_theirs) {
      if (shown < 10) {
        printf("  %-38s regex-only %ld, unicode-only %ld\n",
            property->name, only_ours, only_theirs);
        shown++;
      }
      differed++;
    }
    else {
      agreed++;
    }
  }

  printf("unicode-agreement: UCD %s here against %s in the unicode library\n",
      GRX_UCD_VERSION, guni_ucd_version());
  printf("unicode-agreement: %zu property sets compared, %zu identical, "
      "%zu differing, %zu unresolved\n",
      agreed + differed + unresolved, agreed, differed, unresolved);
  printf("unicode-agreement: %zu Numeric_Value properties over %zu code "
      "points, %zu without a value in the unicode library\n",
      nv_properties, nv_members, nv_bad);
  return (differed || unresolved || nv_bad) ? 1 : 0;
}
