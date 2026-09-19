/**
 * @file
 *
 * UTF-8 decoding and case folding.
 *
 * Reference: RFC 3629 (UTF-8), and Unicode 15.1 chapter 3 for the constraints
 * on what a conformant decoder may accept.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <stddef.h>
#include <stdint.h>

#include "unicode_internal.h"

size_t grx_unicode_utf8_decode(
    const char * text, size_t length, uint32_t * out_codepoint) {
  if (!text || !length || !out_codepoint) {
    return 0;
  }

  const unsigned char * bytes = (const unsigned char *)text;
  unsigned char lead = bytes[0];

  size_t needed;
  uint32_t codepoint;
  uint32_t lowest; // The smallest code point this length may encode.

  if (lead < 0x80u) {
    *out_codepoint = lead;
    return 1;
  }
  else if ((lead & 0xE0u) == 0xC0u) {
    needed = 2;
    codepoint = lead & 0x1Fu;
    lowest = 0x80u;
  }
  else if ((lead & 0xF0u) == 0xE0u) {
    needed = 3;
    codepoint = lead & 0x0Fu;
    lowest = 0x800u;
  }
  else if ((lead & 0xF8u) == 0xF0u) {
    needed = 4;
    codepoint = lead & 0x07u;
    lowest = 0x10000u;
  }
  else {
    // A continuation byte in lead position, or one of the 5- and 6-byte forms
    // RFC 3629 removed.
    return 0;
  }

  if (length < needed) {
    return 0;
  }

  for (size_t i = 1; i < needed; i++) {
    if ((bytes[i] & 0xC0u) != 0x80u) {
      return 0;
    }
    codepoint = (codepoint << 6) | (bytes[i] & 0x3Fu);
  }

  // Overlong: the same code point had a shorter encoding available. Accepting
  // these is the classic way a filter and a consumer disagree about what a
  // string says.
  if (codepoint < lowest) {
    return 0;
  }
  if (codepoint > GRX_CODEPOINT_MAX) {
    return 0;
  }
  // Surrogates are not characters; UTF-8 may not encode them.
  if (codepoint >= 0xD800u && codepoint <= 0xDFFFu) {
    return 0;
  }

  *out_codepoint = codepoint;
  return needed;
}

size_t grx_unicode_utf8_encode(uint32_t codepoint, char * buffer) {
  if (!buffer || codepoint > GRX_CODEPOINT_MAX
      || (codepoint >= 0xD800u && codepoint <= 0xDFFFu)) {
    return 0;
  }

  unsigned char * out = (unsigned char *)buffer;

  if (codepoint < 0x80u) {
    out[0] = (unsigned char)codepoint;
    return 1;
  }
  if (codepoint < 0x800u) {
    out[0] = (unsigned char)(0xC0u | (codepoint >> 6));
    out[1] = (unsigned char)(0x80u | (codepoint & 0x3Fu));
    return 2;
  }
  if (codepoint < 0x10000u) {
    out[0] = (unsigned char)(0xE0u | (codepoint >> 12));
    out[1] = (unsigned char)(0x80u | ((codepoint >> 6) & 0x3Fu));
    out[2] = (unsigned char)(0x80u | (codepoint & 0x3Fu));
    return 3;
  }

  out[0] = (unsigned char)(0xF0u | (codepoint >> 18));
  out[1] = (unsigned char)(0x80u | ((codepoint >> 12) & 0x3Fu));
  out[2] = (unsigned char)(0x80u | ((codepoint >> 6) & 0x3Fu));
  out[3] = (unsigned char)(0x80u | (codepoint & 0x3Fu));
  return 4;
}

uint32_t grx_unicode_fold_simple(uint32_t codepoint) {
  // TODO: the rest of Unicode. CaseFolding.txt's C and S entries are the
  // table this needs; until it exists, a caseless match outside ASCII is
  // simply a match on the unfolded code point, which is wrong rather than
  // approximate - hence the note in documentation/dialects.md.
  if (codepoint >= 'A' && codepoint <= 'Z') {
    return codepoint - 'A' + 'a';
  }

  return codepoint;
}
