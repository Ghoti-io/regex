/**
 * @file
 *
 * The text-segmentation boundaries: UAX #29's and UAX #14's.
 *
 * Perl spells these `\b{gcb}`, `\b{wb}`, `\b{sb}` and `\b{lb}` (and `\b{g}`
 * for the first), and they are the only assertions in this library whose
 * answer is an algorithm over the subject rather than a set the pattern
 * named. Everything else a `\b` needs is a character class; a grapheme
 * cluster boundary is a dozen rules and a line break is thirty.
 *
 * They are still *assertions*, which is what keeps them cheap: zero width,
 * no capture state, and so nothing that stops the lockstep engine running a
 * program that contains one.
 *
 * `\X` is not here. A grapheme cluster is "one character, then every
 * following character that is not a cluster boundary", so lowering builds it
 * out of the boundary this file answers rather than out of a second reading
 * of the same rules - one algorithm, and no way for the two to disagree.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRX_SRC_UNICODE_BREAK_INTERNAL_H
#define GHOTI_IO_GRX_SRC_UNICODE_BREAK_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief Which segmentation a boundary question is about. */
typedef enum {
  GRX_BREAK_GRAPHEME = 0, ///< UAX #29 extended grapheme clusters; `\b{gcb}`.
  GRX_BREAK_WORD,         ///< UAX #29 word boundaries; `\b{wb}`.
  GRX_BREAK_SENTENCE,     ///< UAX #29 sentence boundaries; `\b{sb}`.
  GRX_BREAK_LINE,         ///< UAX #14 line break opportunities; `\b{lb}`.
  GRX_BREAK_COUNT         ///< Closes the enum; not a segmentation.
} GRX_BreakKind;

/**
 * @brief Whether a segmentation boundary falls at a byte offset.
 *
 * The subject must be valid UTF-8; every dialect that can spell one of these
 * reads its subject as text, so that is checked once per search and not
 * again here.
 *
 * **The ends of the subject.** UAX #29 breaks at the start and the end of
 * text (GB1, GB2, WB1, WB2, SB1, SB2) and UAX #14 does not break at the
 * start (LB2) but always does at the end (LB3). An *empty* subject has no
 * boundary of any kind, which is neither standard's wording and is what Perl
 * does: there are no characters, so there is nothing for a boundary to fall
 * between. All four were checked against Perl 5.40 rather than read off the
 * rules, because this is exactly the corner where the two could differ.
 *
 * @param kind Which segmentation.
 * @param subject The subject. NULL is valid only when `length` is 0.
 * @param length Bytes in the subject.
 * @param position Byte offset to ask about; at most `length`.
 * @return Non-zero when a boundary falls there.
 */
int grx_unicode_break_at(GRX_BreakKind kind, const char * subject,
    size_t length, size_t position);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_UNICODE_BREAK_INTERNAL_H
