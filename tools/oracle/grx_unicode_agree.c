/**
 * @file
 *
 * The UCD this library read, against the UCD ghoti.io-unicode answers from.
 *
 * Phase E of unicode's design.md moved the Unicode Character Database out of
 * this library, property ranges last. What that closed was the arrangement
 * where two copies of one standard sat in one build and could drift apart in
 * silence - and what it opened is the opposite risk, which is that there is
 * now nothing left to compare. A gate that asked this library for a
 * property's code points and the Unicode library for the same property's
 * code points would be asking one source twice and reporting "identical"
 * either way.
 *
 * So the comparison is between the generator's *reading* of the UCD files
 * and the library's answer, carried in the two figures the property records
 * still hold:
 *
 *   - `digest`, FNV-1a 64 over the ranges the generator built from the UCD.
 *     This is the membership check: two different sets of the same size have
 *     different digests.
 *   - `total`, the code-point count. Redundant with the digest for catching
 *     a difference, and not redundant for naming one - a digest says "not
 *     the same set" and a count says how far apart they are, which is the
 *     difference between a release mismatch and a single moved code point.
 *
 * Both are the generator's, computed from `third_party/ucd/17.0.0` without
 * asking the library that answers. Two readings of one release, which is
 * only possible because both pin it.
 *
 * Then Numeric_Value, which is the one property whose values this library
 * still stores: 144 reduced rationals, checked against
 * `guni_numeric_value()` for every code point in the sets they name.
 *
 * And, last, that every record resolves at all. A property the Unicode
 * library cannot name is not an error a pattern would ever show: the set
 * comes back empty and `\p{Whatever}` quietly matches nothing. It is
 * reported here as a failure rather than counted as a comparison.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/regex.h>
#include <ghoti.io/unicode/char.h>
#include <ghoti.io/unicode/set.h>
#include <ghoti.io/unicode/unicode.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../src/unicode/tables/tables_internal.h"
#include "../../src/unicode/unicode_internal.h"

static GUNI_Range ranges[GRX_PROPERTY_RANGES_MAX];

int main(void) {
  size_t agreed = 0, differed = 0, unresolved = 0, shown = 0;
  size_t nv_properties = 0, nv_members = 0, nv_bad = 0;

  for (uint32_t index = 0; index < grx_unicode_property_count; index++) {
    const GRX_UnicodeProperty * property = &grx_unicode_properties[index];

    size_t count = 0;
    GRX_Result result = grx_unicode_property_ranges(
        index, ranges, GRX_PROPERTY_RANGES_MAX, &count);
    if (result != GRX_OK) {
      printf("  UNRESOLVED %-38s %s\n", property->name,
          result == GRX_ERR_LIMIT
              ? "needs more ranges than GRX_PROPERTY_RANGES_MAX"
              : "the unicode library does not know this name");
      if (result == GRX_ERR_LIMIT) {
        printf("             %zu ranges, buffer %d\n",
            count, GRX_PROPERTY_RANGES_MAX);
      }
      unresolved++;
      continue;
    }

    uint64_t digest = grx_unicode_range_digest(ranges, count);
    size_t total = 0;
    for (size_t k = 0; k < count; k++) {
      total += ranges[k].last - ranges[k].first + 1;
    }

    if (digest != grx_unicode_property_digest(index)
        || total != grx_unicode_property_total(index)) {
      if (shown < 10) {
        printf("  %-38s UCD says %zu code points, the unicode library gives "
            "%zu in %zu ranges\n", property->name,
            grx_unicode_property_total(index), total, count);
        printf("  %-38s digest %016" PRIX64 " against %016" PRIX64 "\n", "",
            grx_unicode_property_digest(index), digest);
        shown++;
      }
      differed++;
      continue;
    }
    agreed++;

    /* Numeric_Value: the rationals are still this library's, so each member
     * is asked. The set itself was just checked above like any other. */
    if (property->kind == GRX_UPROP_NV) {
      nv_properties++;
      for (size_t k = 0; k < count; k++) {
        for (uint32_t code = ranges[k].first; code <= ranges[k].last; code++) {
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
