/**
 * @file
 *
 * Library-wide entry points: what version is actually loaded.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/libver.h>
#include <ghoti.io/regex/version.h>

const char * grx_version_string(void) {
  return GRX_VERSION_STRING;
}

unsigned grx_version_number(void) {
  return GRX_VERSION_NUMBER;
}
