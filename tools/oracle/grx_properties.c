/**
 * @file
 *
 * This library's Unicode property tables, dumped for comparison.
 *
 * `--list` prints every canonical property name, one per line. Otherwise each
 * input line is a property name and each output line is
 * `<name>\t<lo>-<hi> <lo>-<hi> ...` in hexadecimal, or `<name>\tunknown`.
 *
 * Ranges rather than code points, because a property is stored as ranges and
 * printing 1.1 million lines to compare 700 would be slower than the
 * comparison. Used by tools/oracle/property_diff.py.
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

int main(int argc, char ** argv) {
  if (argc > 1 && strcmp(argv[1], "--list") == 0) {
    // Qualified where the kind needs it. A script value has no lone spelling
    // - `\p{Greek}` is a SyntaxError - so listing the bare name would give a
    // comparison harness a name neither side can look up, and 364 of the 454
    // properties would silently drop out of the check.
    for (uint32_t index = 0; index < grx_unicode_property_count; index++) {
      const GRX_UnicodeProperty * property = &grx_unicode_properties[index];
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
              GRX_PROPERTY_STRICT, &property)
        : grx_unicode_property_lookup(
              line, length, NULL, 0, GRX_PROPERTY_STRICT, &property);
    if (result != GRX_OK) {
      printf("%s\tunknown\n", line);
      continue;
    }

    size_t count = 0;
    const GRX_CharRange * ranges
        = grx_unicode_property_ranges(property, &count);
    printf("%s\t", line);
    for (size_t i = 0; i < count; i++) {
      printf("%s%X-%X", i ? " " : "", ranges[i].low, ranges[i].high);
    }
    printf("\n");
  }

  return 0;
}
