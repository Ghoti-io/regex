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
 * UTF-8 decoding and case folding.
 *
 * Reference: RFC 3629 (UTF-8), and Unicode 15.1 chapter 3 for the constraints
 * on what a conformant decoder may accept.
 */

#include <ghoti.io/regex/macros.h>

#include <stddef.h>
#include <stdint.h>

#include <ghoti.io/regex/unicode.h>

#include "tables/tables_internal.h"
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

size_t grx_unicode_utf8_decode_prev(
    const char * text, size_t offset, uint32_t * out_codepoint) {
  if (!text || !offset || !out_codepoint) {
    return 0;
  }

  const unsigned char * bytes = (const unsigned char *)text;

  // A lead byte is at most four back. Step over continuation bytes, then
  // decode forward from the first non-continuation byte and require the
  // sequence to end exactly where the caller said it does: a backwards scan
  // alone cannot tell "the three bytes of U+20AC" from "a truncated sequence
  // followed by two stray continuation bytes", and accepting the second
  // would let a lookbehind step into the middle of a character.
  size_t back = 1;
  while (back <= 4 && back <= offset) {
    unsigned char candidate = bytes[offset - back];
    if ((candidate & 0xC0u) != 0x80u) {
      uint32_t codepoint = 0;
      size_t used
          = grx_unicode_utf8_decode(text + offset - back, back, &codepoint);
      if (used != back) {
        return 0;
      }
      *out_codepoint = codepoint;
      return back;
    }
    back++;
  }

  return 0;
}

GRX_Result grx_utf8_validate(
    const char * text, size_t length, size_t * out_offset) {
  if (!text && length) {
    if (out_offset) {
      *out_offset = 0;
    }
    return GRX_ERR_INVALID;
  }

  size_t position = 0;
  while (position < length) {
    uint32_t codepoint = 0;
    size_t used
        = grx_unicode_utf8_decode(text + position, length - position,
            &codepoint);
    if (!used) {
      if (out_offset) {
        *out_offset = position;
      }
      return GRX_ERR_INVALID;
    }
    position += used;
  }

  return GRX_OK;
}

const char * grx_unicode_version(void) {
  return GRX_UCD_VERSION;
}
