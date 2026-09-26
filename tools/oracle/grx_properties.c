/**
 * @file
 *
 * This library's Unicode property tables, dumped for comparison.
 *
 * `--list` prints every canonical property name, one per line, and
 * `--list-numeric` prints the Numeric_Value ones as `nv=<value>`. Otherwise
 * each input line is a property name and each output line is
 * `<name>\t<lo>-<hi> <lo>-<hi> ...` in hexadecimal, or `<name>\tunknown`.
 *
 * `--perl` resolves those input lines under Perl's spelling rule instead of
 * ECMAScript's. Only `nv` needs it - it is the one property no strict
 * dialect can reach - and it means the differ measures the lookup a pattern
 * actually performs, rational parser included, rather than the table behind
 * it.
 *
 * Ranges rather than code points, because a property is stored as ranges and
 * printing 1.1 million lines to compare 700 would be slower than the
 * comparison. Used by tools/oracle/property_diff.py and
 * tools/oracle/numeric_property_diff.py.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/regex.h>
#include <stdio.h>
#include <string.h>

#include "../../src/unicode/tables/tables_internal.h"
#include "../../src/unicode/unicode_internal.h"

/** The longest property name this tool will read. */
#define MAX_NAME 256

/** One property's code points, materialised from ghoti.io-unicode. */
static GUNI_Range ranges[GRX_PROPERTY_RANGES_MAX];

int main(int argc, char ** argv) {
  if (argc > 1 && strcmp(argv[1], "--list-numeric") == 0) {
    for (uint32_t index = 0; index < grx_unicode_property_count; index++) {
      if (grx_unicode_properties[index].kind == GRX_UPROP_NV) {
        printf("nv=%s\n", grx_unicode_properties[index].name);
      }
    }
    return 0;
  }

  if (argc > 1 && strcmp(argv[1], "--list") == 0) {
    // Qualified where the kind needs it. A script value has no lone spelling
    // - `\p{Greek}` is a SyntaxError - so listing the bare name would give a
    // comparison harness a name neither side can look up, and 364 of the 454
    // properties would silently drop out of the check.
    for (uint32_t index = 0; index < grx_unicode_property_count; index++) {
      const GRX_UnicodeProperty * property = &grx_unicode_properties[index];
      // Numeric_Value is left out rather than listed and skipped. The
      // reference on the other side of this list is Node, which has no such
      // property, so every one of its 144 values would answer "unsupported"
      // on both sides and pad the count with comparisons that never happen.
      // Perl is the only engine that implements it, and
      // tools/oracle/numeric_property_diff.py is where it is checked.
      if (property->kind == GRX_UPROP_NV) {
        continue;
      }
      switch (property->kind) {
        case GRX_UPROP_SCRIPT:
          printf("Script=%s\n", property->name);
          break;
        case GRX_UPROP_SCX:
          printf("Script_Extensions=%s\n", property->name);
          break;
        case GRX_UPROP_GC:
          printf("General_Category=%s\n", property->name);
          break;
        default:
          printf("%s\n", property->name);
          break;
      }
    }
    return 0;
  }

  GRX_PropertyMatch match = GRX_PROPERTY_STRICT;
  if (argc > 1 && strcmp(argv[1], "--perl") == 0) {
    match = GRX_PROPERTY_LOOSE_PERL;
  }

  char line[MAX_NAME];
  while (fgets(line, (int)sizeof(line), stdin)) {
    size_t length = strlen(line);
    while (length && (line[length - 1] == '\n' || line[length - 1] == '\r')) {
      line[--length] = '\0';
    }
    if (!length) {
      continue;
    }

    // The same three shapes `\p{...}` accepts, so that a qualified name in
    // the list resolves the way a pattern would spell it.
    const char * equals = memchr(line, '=', length);
    uint32_t property = 0;
    GRX_Result result = equals
        ? grx_unicode_property_lookup(line, (size_t)(equals - line),
              equals + 1, length - (size_t)(equals - line) - 1,
              match, &property)
        : grx_unicode_property_lookup(
              line, length, NULL, 0, match, &property);
    if (result != GRX_OK) {
      printf("%s\tunknown\n", line);
      continue;
    }

    size_t count = 0;
    if (grx_unicode_property_ranges(
            property, ranges, GRX_PROPERTY_RANGES_MAX, &count) != GRX_OK) {
      // Not "unknown": the name resolved and the set did not come back, so
      // reporting it as a name nobody has would put a real fault into the
      // bucket the harness counts as agreement.
      fprintf(stderr, "%s: resolved but has no set (%zu ranges needed)\n",
          line, count);
      return 1;
    }
    printf("%s\t", line);
    for (size_t i = 0; i < count; i++) {
      printf("%s%X-%X", i ? " " : "", ranges[i].first, ranges[i].last);
    }
    printf("\n");
  }

  return 0;
}
