/**
 * @file
 *
 * UAX #29 and UAX #14 boundaries. See break_internal.h.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include "break_internal.h"
#include "tables/tables_internal.h"
#include "unicode_internal.h"

// --------------------------------------------------------------------------
// Property values
// --------------------------------------------------------------------------

/**
 * Grapheme_Cluster_Break, numbered as tools/unicode/gen_tables.py numbers it.
 *
 * The four enums below and the lists in BREAK_VALUES are one thing written
 * twice, which is why the generator refuses a UCD value it does not know:
 * a new break value has to stop the build rather than quietly become Other.
 */
typedef enum {
  GCB_OTHER = 0, GCB_CR, GCB_LF, GCB_CONTROL, GCB_EXTEND, GCB_ZWJ,
  GCB_RI, GCB_PREPEND, GCB_SPACINGMARK, GCB_L, GCB_V, GCB_T, GCB_LV, GCB_LVT
} GcbValue;

/** Word_Break. */
typedef enum {
  WB_OTHER = 0, WB_CR, WB_LF, WB_NEWLINE, WB_EXTEND, WB_ZWJ, WB_RI,
  WB_FORMAT, WB_KATAKANA, WB_HEBREW_LETTER, WB_ALETTER, WB_SINGLE_QUOTE,
  WB_DOUBLE_QUOTE, WB_MIDNUMLET, WB_MIDLETTER, WB_MIDNUM, WB_NUMERIC,
  WB_EXTENDNUMLET, WB_WSEGSPACE
} WbValue;

/** Sentence_Break. */
typedef enum {
  SB_OTHER = 0, SB_CR, SB_LF, SB_SEP, SB_FORMAT, SB_SP, SB_LOWER, SB_UPPER,
  SB_OLETTER, SB_NUMERIC, SB_ATERM, SB_SCONTINUE, SB_STERM, SB_CLOSE,
  SB_EXTEND
} SbValue;

/** Line_Break. */
typedef enum {
  LB_XX = 0, LB_AI, LB_AK, LB_AL, LB_AP, LB_AS, LB_B2, LB_BA, LB_BB, LB_BK,
  LB_CB, LB_CJ, LB_CL, LB_CM, LB_CP, LB_CR, LB_EB, LB_EM, LB_EX, LB_GL,
  LB_H2, LB_H3, LB_HH, LB_HL, LB_HY, LB_ID, LB_IN, LB_IS, LB_JL, LB_JT,
  LB_JV, LB_LF, LB_NL, LB_NS, LB_NU, LB_OP, LB_PO, LB_PR, LB_QU, LB_RI,
  LB_SA, LB_SG, LB_SP, LB_SY, LB_VF, LB_VI, LB_WJ, LB_ZW, LB_ZWJ,
  /**
   * QU split by the punctuation category LB15a, LB15b and LB19 ask about.
   *
   * Not a Line_Break value: `[\p{Pi}&QU]` and `[\p{Pf}&QU]` are how the
   * rules are written, and the generator resolves them into classes so that
   * a boundary test reads one table instead of two. LB1's resolutions are
   * done there too, which is why LB_AI, LB_CJ, LB_SA, LB_SG and LB_XX are
   * in this enum and never in the table.
   */
  LB_QU_PI, LB_QU_PF
} LbValue;

/** Indic_Conjunct_Break, for GB9c. */
typedef enum {
  INCB_NONE = 0, INCB_CONSONANT, INCB_EXTEND, INCB_LINKER
} IncbValue;

/** Binary search one break table; a code point in no run takes value 0. */
static uint32_t break_value(const GRX_UnicodeBreakRange * ranges,
    size_t count, uint32_t codepoint) {
  size_t low = 0;
  size_t high = count;
  while (low < high) {
    size_t mid = low + (high - low) / 2;
    if (codepoint < ranges[mid].low) {
      high = mid;
    }
    else if (codepoint > ranges[mid].high) {
      low = mid + 1;
    }
    else {
      return ranges[mid].value;
    }
  }

  return 0;
}

static uint32_t gcb_of(uint32_t codepoint) {
  return break_value(
      grx_unicode_gcb_ranges, grx_unicode_gcb_range_count, codepoint);
}

static uint32_t wb_of(uint32_t codepoint) {
  return break_value(
      grx_unicode_wb_ranges, grx_unicode_wb_range_count, codepoint);
}

static uint32_t sb_of(uint32_t codepoint) {
  return break_value(
      grx_unicode_sb_ranges, grx_unicode_sb_range_count, codepoint);
}

static uint32_t lb_of(uint32_t codepoint) {
  uint32_t value = break_value(
      grx_unicode_lb_ranges, grx_unicode_lb_range_count, codepoint);
  // LineBreak.txt's `@missing` default is XX, and LB1 resolves XX to AL. The
  // generator has already resolved away every XX the file lists, so a code
  // point with no run of its own is the only XX left - and it resolves the
  // same way. Unassigned code points reach here by that path, and they are
  // most of Unicode.
  return value == LB_XX ? (uint32_t)LB_AL : value;
}

/** East_Asian_Width in {F, W, H}, which UAX #14 spells `$EastAsian`. */
static int east_asian(uint32_t codepoint) {
  return break_value(
             grx_unicode_ea_ranges, grx_unicode_ea_range_count, codepoint)
      != 0;
}

/** Extended_Pictographic and unassigned: LB30b's second line. */
static int pictographic_unassigned(uint32_t codepoint) {
  return break_value(
             grx_unicode_epcn_ranges, grx_unicode_epcn_range_count, codepoint)
      != 0;
}

static uint32_t incb_of(uint32_t codepoint) {
  return break_value(
      grx_unicode_incb_ranges, grx_unicode_incb_range_count, codepoint);
}

/** Extended_Pictographic, which GB11 and WB3c need. */
static int extended_pictographic(uint32_t codepoint) {
  return break_value(grx_unicode_extpict_ranges,
             grx_unicode_extpict_range_count, codepoint)
      != 0;
}

// --------------------------------------------------------------------------
// Walking the subject
// --------------------------------------------------------------------------

/** The subject, as the boundary rules read it. */
typedef struct {
  const char * text;
  size_t length;
} Text;

/**
 * The code point beginning at `at`, and where the next one begins.
 *
 * A byte that does not start a valid sequence cannot occur - the subject was
 * validated - but is stepped over as one code point rather than looping, so
 * that a caller who reached here another way still terminates.
 */
static int at_next(const Text * text, size_t at, uint32_t * out, size_t * out_end) {
  if (at >= text->length) {
    return 0;
  }
  size_t width
      = grx_unicode_utf8_decode(text->text + at, text->length - at, out);
  if (!width) {
    *out = (uint32_t)(unsigned char)text->text[at];
    width = 1;
  }
  *out_end = at + width;
  return 1;
}

/** The code point ending at `at`, and where it begins. */
static int at_prev(const Text * text, size_t at, uint32_t * out, size_t * out_start) {
  if (!at) {
    return 0;
  }
  size_t width = grx_unicode_utf8_decode_prev(text->text, at, out);
  if (!width) {
    *out = (uint32_t)(unsigned char)text->text[at - 1];
    width = 1;
  }
  *out_start = at - width;
  return 1;
}

// --------------------------------------------------------------------------
// UAX #29: grapheme cluster boundaries
// --------------------------------------------------------------------------

/**
 * Whether an even number of Regional_Indicators precedes `at`.
 *
 * GB12 and GB13 break between the pairs of a flag sequence, so what decides
 * a boundary before an RI is the parity of the unbroken run behind it. The
 * scan is bounded by that run, not by the subject: the first character that
 * is not an RI ends it.
 */
static int even_regional_indicators(const Text * text, size_t at) {
  size_t count = 0;
  size_t scan = at;
  for (;;) {
    uint32_t codepoint = 0;
    size_t start = 0;
    if (!at_prev(text, scan, &codepoint, &start) || gcb_of(codepoint) != GCB_RI) {
      break;
    }
    count++;
    scan = start;
  }

  return (count % 2) == 0;
}

/**
 * GB9c: whether an Indic conjunct sequence ends at `at`.
 *
 * `Consonant [Extend Linker]* Linker [Extend Linker]*` immediately before
 * the position, which is to say: walk back over InCB Extend and Linker, see
 * at least one Linker on the way, and land on a Consonant.
 */
static int indic_conjunct_before(const Text * text, size_t at) {
  int seen_linker = 0;
  size_t scan = at;
  for (;;) {
    uint32_t codepoint = 0;
    size_t start = 0;
    if (!at_prev(text, scan, &codepoint, &start)) {
      return 0;
    }
    uint32_t value = incb_of(codepoint);
    if (value == INCB_LINKER) {
      seen_linker = 1;
      scan = start;
      continue;
    }
    if (value == INCB_EXTEND) {
      scan = start;
      continue;
    }
    return seen_linker && value == INCB_CONSONANT;
  }
}

/**
 * GB11: whether `ExtPict Extend* ZWJ` ends at `at`.
 *
 * The ZWJ immediately before the position is the caller's business; this
 * walks the Extend run behind it and asks what is on the far side.
 */
static int pictographic_zwj_before(const Text * text, size_t at) {
  size_t scan = at;
  for (;;) {
    uint32_t codepoint = 0;
    size_t start = 0;
    if (!at_prev(text, scan, &codepoint, &start)) {
      return 0;
    }
    if (gcb_of(codepoint) == GCB_EXTEND) {
      scan = start;
      continue;
    }
    return extended_pictographic(codepoint);
  }
}

/** UAX #29 section 3.1.1, rules GB3 to GB999. `before` ends at `at`. */
static int grapheme_break(const Text * text, size_t at, uint32_t before,
    size_t before_start, uint32_t after) {
  uint32_t left = gcb_of(before);
  uint32_t right = gcb_of(after);

  if (left == GCB_CR && right == GCB_LF) {
    return 0; // GB3
  }
  if (left == GCB_CONTROL || left == GCB_CR || left == GCB_LF) {
    return 1; // GB4
  }
  if (right == GCB_CONTROL || right == GCB_CR || right == GCB_LF) {
    return 1; // GB5
  }
  if (left == GCB_L
      && (right == GCB_L || right == GCB_V || right == GCB_LV
          || right == GCB_LVT)) {
    return 0; // GB6
  }
  if ((left == GCB_LV || left == GCB_V) && (right == GCB_V || right == GCB_T)) {
    return 0; // GB7
  }
  if ((left == GCB_LVT || left == GCB_T) && right == GCB_T) {
    return 0; // GB8
  }
  if (right == GCB_EXTEND || right == GCB_ZWJ) {
    return 0; // GB9
  }
  if (right == GCB_SPACINGMARK) {
    return 0; // GB9a
  }
  if (left == GCB_PREPEND) {
    return 0; // GB9b
  }
  if (incb_of(after) == INCB_CONSONANT && indic_conjunct_before(text, at)) {
    return 0; // GB9c
  }
  if (left == GCB_ZWJ && extended_pictographic(after)
      && pictographic_zwj_before(text, before_start)) {
    return 0; // GB11
  }
  if (left == GCB_RI && right == GCB_RI
      && even_regional_indicators(text, before_start)) {
    // GB12, GB13: `sot (RI RI)* RI x RI`. An even run behind the left RI
    // makes that RI the *first* of a pair, so this position is inside a flag
    // sequence; an odd one means the pair is already complete.
    return 0;
  }

  return 1; // GB999
}

// --------------------------------------------------------------------------
// UAX #29: word boundaries
// --------------------------------------------------------------------------

/** WB4's ignorables: what a character absorbs without changing what it is. */
static int wb_ignorable(uint32_t value) {
  return value == WB_EXTEND || value == WB_FORMAT || value == WB_ZWJ;
}

/** AHLetter: ALetter or Hebrew_Letter, which most of the rules pair up. */
static int wb_ah_letter(uint32_t value) {
  return value == WB_ALETTER || value == WB_HEBREW_LETTER;
}

/** MidNumLetQ: MidNumLet or Single_Quote. */
static int wb_mid_num_letq(uint32_t value) {
  return value == WB_MIDNUMLET || value == WB_SINGLE_QUOTE;
}

/**
 * The last character ending at or before `at` that WB4 does not absorb.
 *
 * Returns its Word_Break value and, through `out_start`, where it begins -
 * which is where a rule that needs to look one further back carries on from.
 *
 * UAX #29 section 6.2 states the limit of the ignore rules, and it is not a
 * detail: they do not apply "after sot, CR, LF, and Newline". Those four
 * have already forced a break by the time these rules are consulted, so an
 * Extend or a Format following one has nothing to attach to and stands for
 * itself. Absorbing it anyway would put a CR on the left of rules that are
 * asking about letters.
 */
static uint32_t wb_before(const Text * text, size_t at, size_t * out_start) {
  size_t scan = at;
  uint32_t ignored = WB_OTHER;
  size_t ignored_start = at;
  int any_ignored = 0;
  for (;;) {
    uint32_t codepoint = 0;
    size_t start = 0;
    if (!at_prev(text, scan, &codepoint, &start)) {
      break; // The start of the subject: sot, where WB4 does not reach.
    }
    uint32_t value = wb_of(codepoint);
    if (wb_ignorable(value)) {
      ignored = value;
      ignored_start = start;
      any_ignored = 1;
      scan = start;
      continue;
    }
    if (any_ignored
        && (value == WB_CR || value == WB_LF || value == WB_NEWLINE)) {
      break;
    }
    if (out_start) {
      *out_start = start;
    }
    return value;
  }

  if (out_start) {
    *out_start = any_ignored ? ignored_start : scan;
  }
  return any_ignored ? ignored : WB_OTHER;
}

/** The first character at or after `at` that WB4 does not absorb. */
static uint32_t wb_after(const Text * text, size_t at) {
  size_t scan = at;
  for (;;) {
    uint32_t codepoint = 0;
    size_t end = 0;
    if (!at_next(text, scan, &codepoint, &end)) {
      return WB_OTHER;
    }
    uint32_t value = wb_of(codepoint);
    if (!wb_ignorable(value)) {
      return value;
    }
    scan = end;
  }
}

/** WB15, WB16: as GB12 and GB13, over the characters WB4 leaves. */
static int wb_even_regional_indicators(const Text * text, size_t at) {
  size_t count = 0;
  size_t scan = at;
  for (;;) {
    uint32_t codepoint = 0;
    size_t start = 0;
    if (!at_prev(text, scan, &codepoint, &start)) {
      break;
    }
    uint32_t value = wb_of(codepoint);
    scan = start;
    if (wb_ignorable(value)) {
      continue;
    }
    if (value != WB_RI) {
      break;
    }
    count++;
  }

  return (count % 2) == 0;
}

/** UAX #29 section 4.1, rules WB3 to WB999. `before` ends at `at`. */
static int word_break(const Text * text, size_t at, uint32_t before,
    uint32_t after, size_t after_end) {
  uint32_t raw_left = wb_of(before);
  uint32_t right = wb_of(after);

  if (raw_left == WB_CR && right == WB_LF) {
    return 0; // WB3
  }
  if (raw_left == WB_NEWLINE || raw_left == WB_CR || raw_left == WB_LF) {
    return 1; // WB3a
  }
  if (right == WB_NEWLINE || right == WB_CR || right == WB_LF) {
    return 1; // WB3b
  }
  if (raw_left == WB_ZWJ && extended_pictographic(after)) {
    return 0; // WB3c
  }
  if (raw_left == WB_WSEGSPACE && right == WB_WSEGSPACE) {
    return 0; // WB3d
  }
  if (wb_ignorable(right)) {
    return 0; // WB4: the character on the right is absorbed by this one.
  }

  // Everything below reads the sequence WB4 leaves behind, so the character
  // on the left is the last one that was not absorbed.
  size_t left_start = 0;
  uint32_t left = wb_before(text, at, &left_start);

  if (wb_ah_letter(left) && wb_ah_letter(right)) {
    return 0; // WB5
  }
  if (wb_ah_letter(left) && (right == WB_MIDLETTER || wb_mid_num_letq(right))
      && wb_ah_letter(wb_after(text, after_end))) {
    return 0; // WB6
  }
  if (wb_ah_letter(right) && (left == WB_MIDLETTER || wb_mid_num_letq(left))
      && wb_ah_letter(wb_before(text, left_start, NULL))) {
    return 0; // WB7
  }
  if (left == WB_HEBREW_LETTER && right == WB_SINGLE_QUOTE) {
    return 0; // WB7a
  }
  if (left == WB_HEBREW_LETTER && right == WB_DOUBLE_QUOTE
      && wb_after(text, after_end) == WB_HEBREW_LETTER) {
    return 0; // WB7b
  }
  if (left == WB_DOUBLE_QUOTE && right == WB_HEBREW_LETTER
      && wb_before(text, left_start, NULL) == WB_HEBREW_LETTER) {
    return 0; // WB7c
  }
  if (left == WB_NUMERIC && right == WB_NUMERIC) {
    return 0; // WB8
  }
  if (wb_ah_letter(left) && right == WB_NUMERIC) {
    return 0; // WB9
  }
  if (left == WB_NUMERIC && wb_ah_letter(right)) {
    return 0; // WB10
  }
  if (right == WB_NUMERIC && (left == WB_MIDNUM || wb_mid_num_letq(left))
      && wb_before(text, left_start, NULL) == WB_NUMERIC) {
    return 0; // WB11
  }
  if (left == WB_NUMERIC && (right == WB_MIDNUM || wb_mid_num_letq(right))
      && wb_after(text, after_end) == WB_NUMERIC) {
    return 0; // WB12
  }
  if (left == WB_KATAKANA && right == WB_KATAKANA) {
    return 0; // WB13
  }
  if ((wb_ah_letter(left) || left == WB_NUMERIC || left == WB_KATAKANA
          || left == WB_EXTENDNUMLET)
      && right == WB_EXTENDNUMLET) {
    return 0; // WB13a
  }
  if (left == WB_EXTENDNUMLET
      && (wb_ah_letter(right) || right == WB_NUMERIC
          || right == WB_KATAKANA)) {
    return 0; // WB13b
  }
  if (left == WB_RI && right == WB_RI
      && wb_even_regional_indicators(text, left_start)) {
    return 0; // WB15, WB16
  }

  return 1; // WB999
}

// --------------------------------------------------------------------------
// UAX #29: sentence boundaries
// --------------------------------------------------------------------------

/** SB5's ignorables. */
static int sb_ignorable(uint32_t value) {
  return value == SB_EXTEND || value == SB_FORMAT;
}

/** ParaSep: the three that end a paragraph. */
static int sb_para_sep(uint32_t value) {
  return value == SB_SEP || value == SB_CR || value == SB_LF;
}

/**
 * The last character ending at or before `at` that SB5 does not absorb.
 *
 * The same shape as wb_before(), and with the same limit from UAX #29
 * section 6.2: the ignore rules do not apply after sot, Sep, CR or LF. An
 * Extend after a line feed is its own character, and treating it as part of
 * the line feed makes SB11 end a sentence that had already ended.
 */
static uint32_t sb_before(const Text * text, size_t at, size_t * out_start) {
  size_t scan = at;
  uint32_t ignored = SB_OTHER;
  size_t ignored_start = at;
  int any_ignored = 0;
  for (;;) {
    uint32_t codepoint = 0;
    size_t start = 0;
    if (!at_prev(text, scan, &codepoint, &start)) {
      break;
    }
    uint32_t value = sb_of(codepoint);
    if (sb_ignorable(value)) {
      ignored = value;
      ignored_start = start;
      any_ignored = 1;
      scan = start;
      continue;
    }
    if (any_ignored && sb_para_sep(value)) {
      break;
    }
    if (out_start) {
      *out_start = start;
    }
    return value;
  }

  if (out_start) {
    *out_start = any_ignored ? ignored_start : scan;
  }
  return any_ignored ? ignored : SB_OTHER;
}

/**
 * What kind of sentence terminator the left context is, if any.
 *
 * Several rules share the shape `(STerm | ATerm) Close* Sp*`, differing only
 * in how much of the tail they allow, so the walk is written once: skip the
 * `Sp*` when `skip_spaces` says to, then the `Close*` always, and report
 * what is on the far side.
 *
 * @return SB_ATERM, SB_STERM, or SB_OTHER for neither.
 */
static uint32_t sentence_terminator(
    const Text * text, size_t at, int skip_spaces) {
  size_t scan = at;
  if (skip_spaces) {
    for (;;) {
      size_t start = 0;
      uint32_t value = sb_before(text, scan, &start);
      if (value != SB_SP) {
        break;
      }
      scan = start;
    }
  }
  for (;;) {
    size_t start = 0;
    uint32_t value = sb_before(text, scan, &start);
    if (value != SB_CLOSE) {
      return value == SB_ATERM || value == SB_STERM ? value : SB_OTHER;
    }
    scan = start;
  }
}

/**
 * SB8: whether a lower-case letter follows, with nothing sentence-ending in
 * between.
 *
 * `( !(OLetter | Upper | Lower | Sep | CR | LF | STerm | ATerm) )* Lower`.
 * The scan is unbounded, which is the one place these rules look arbitrarily
 * far ahead - "Mr. Smith" is not two sentences, and finding that out means
 * reading to the `S`.
 */
static int sentence_lower_follows(const Text * text, size_t at) {
  size_t scan = at;
  for (;;) {
    uint32_t codepoint = 0;
    size_t end = 0;
    if (!at_next(text, scan, &codepoint, &end)) {
      return 0;
    }
    uint32_t value = sb_of(codepoint);
    scan = end;
    if (sb_ignorable(value)) {
      continue; // SB5 applies here too.
    }
    if (value == SB_LOWER) {
      return 1;
    }
    if (value == SB_OLETTER || value == SB_UPPER || sb_para_sep(value)
        || value == SB_STERM || value == SB_ATERM) {
      return 0;
    }
  }
}

/**
 * UAX #29 section 5.1, rules SB3 to SB998.
 *
 * The default is the opposite of the other two: SB998 is "do not break", so
 * a sentence runs on unless a rule ends it.
 */
static int sentence_break(const Text * text, size_t at, uint32_t before,
    uint32_t after) {
  uint32_t raw_left = sb_of(before);
  uint32_t right = sb_of(after);

  if (raw_left == SB_CR && right == SB_LF) {
    return 0; // SB3
  }
  if (sb_para_sep(raw_left)) {
    return 1; // SB4
  }
  if (sb_ignorable(right)) {
    return 0; // SB5
  }

  size_t left_start = 0;
  uint32_t left = sb_before(text, at, &left_start);

  if (left == SB_ATERM && right == SB_NUMERIC) {
    return 0; // SB6
  }
  if (left == SB_ATERM && right == SB_UPPER) {
    uint32_t further = sb_before(text, left_start, NULL);
    if (further == SB_UPPER || further == SB_LOWER) {
      return 0; // SB7
    }
  }
  if (sentence_terminator(text, at, 1) == SB_ATERM
      && sentence_lower_follows(text, at)) {
    return 0; // SB8
  }
  if (sentence_terminator(text, at, 1) != SB_OTHER
      && (right == SB_SCONTINUE || right == SB_STERM || right == SB_ATERM)) {
    return 0; // SB8a
  }
  if (sentence_terminator(text, at, 0) != SB_OTHER
      && (right == SB_CLOSE || right == SB_SP || sb_para_sep(right))) {
    return 0; // SB9
  }
  if (sentence_terminator(text, at, 1) != SB_OTHER
      && (right == SB_SP || sb_para_sep(right))) {
    return 0; // SB10
  }

  // SB11: a terminator, its closes and spaces, and at most one paragraph
  // separator, all end the sentence here.
  size_t tail = at;
  size_t start = 0;
  if (sb_para_sep(sb_before(text, tail, &start))) {
    tail = start;
  }
  if (sentence_terminator(text, tail, 1) != SB_OTHER) {
    return 1; // SB11
  }

  return 0; // SB998
}

// --------------------------------------------------------------------------
// UAX #14: line break opportunities
// --------------------------------------------------------------------------

/** U+25CC DOTTED CIRCLE, which LB28a names on its own. */
#define LB_DOTTED_CIRCLE 0x25CCu

/** One character, as the line break rules read it. */
typedef struct {
  uint32_t codepoint; ///< The character, for the rules that need it directly.
  uint32_t value;     ///< Its resolved Line_Break class.
  size_t start;       ///< Where it begins.
  size_t end;         ///< One past where it ends.
  int present;        ///< Zero at sot or eot.
} LbChar;

/** The three quotation classes LB19 and LB19a treat alike. */
static int lb_quote(uint32_t value) {
  return value == LB_QU || value == LB_QU_PI || value == LB_QU_PF;
}

/** LB9's ignorables. */
static int lb_combining(uint32_t value) {
  return value == LB_CM || value == LB_ZWJ;
}

/** The classes LB9 refuses to attach a combining mark to. */
static int lb_no_attach(uint32_t value) {
  return value == LB_BK || value == LB_CR || value == LB_LF
      || value == LB_NL || value == LB_SP || value == LB_ZW;
}

/**
 * The character ending at `at`, after LB9 and LB10.
 *
 * LB9 folds a `(CM | ZWJ)*` run into the character before it, so the class
 * on the left of a position is the base's; LB10 turns a run that had nothing
 * to attach to - at the start of the text, or after a space or a hard break -
 * into AL.
 */
static LbChar lb_prev(const Text * text, size_t at) {
  LbChar out = {0, LB_AL, at, at, 0};
  size_t scan = at;
  LbChar first_combining = {0, LB_AL, at, at, 0};

  for (;;) {
    uint32_t codepoint = 0;
    size_t start = 0;
    if (!at_prev(text, scan, &codepoint, &start)) {
      break; // sot: LB10 applies to whatever run we walked over.
    }
    uint32_t value = lb_of(codepoint);
    if (lb_combining(value)) {
      first_combining.codepoint = codepoint;
      first_combining.value = LB_AL;
      first_combining.start = start;
      first_combining.end = scan;
      first_combining.present = 1;
      scan = start;
      continue;
    }
    if (first_combining.present && lb_no_attach(value)) {
      return first_combining; // LB10
    }
    out.codepoint = codepoint;
    out.value = value;
    out.start = start;
    out.end = scan;
    out.present = 1;
    return out;
  }

  return first_combining; // LB10 at the start of the text, or nothing at all.
}

/** The character beginning at `at`; no folding, which the caller does. */
static LbChar lb_next(const Text * text, size_t at) {
  LbChar out = {0, LB_AL, at, at, 0};
  uint32_t codepoint = 0;
  size_t end = 0;
  if (!at_next(text, at, &codepoint, &end)) {
    return out;
  }
  out.codepoint = codepoint;
  out.value = lb_of(codepoint);
  out.start = at;
  out.end = end;
  out.present = 1;
  return out;
}

/**
 * The character after `at`, with any `(CM | ZWJ)*` run skipped.
 *
 * What a rule looking two characters ahead wants: LB9 has already made the
 * run part of the character before it, so the next *character* the rules see
 * is the next base.
 */
static LbChar lb_next_base(const Text * text, size_t at) {
  size_t scan = at;
  for (;;) {
    LbChar here = lb_next(text, scan);
    if (!here.present || !lb_combining(here.value)) {
      return here;
    }
    scan = here.end;
  }
}

/** Walk back over a run of one class, and report where it starts. */
static size_t lb_skip_back(const Text * text, size_t at, uint32_t value) {
  size_t scan = at;
  for (;;) {
    LbChar here = lb_prev(text, scan);
    if (!here.present || here.value != value) {
      return scan;
    }
    scan = here.start;
  }
}

/** LB25's `NU ( SY | IS )*` read backwards from `at`. */
static int lb_number_before(const Text * text, size_t at, size_t * out_start) {
  size_t scan = at;
  for (;;) {
    LbChar here = lb_prev(text, scan);
    if (!here.present) {
      return 0;
    }
    if (here.value == LB_SY || here.value == LB_IS) {
      scan = here.start;
      continue;
    }
    if (here.value == LB_NU) {
      if (out_start) {
        *out_start = here.start;
      }
      return 1;
    }
    return 0;
  }
}

/** LB30a: an even number of regional indicators before `at`. */
static int lb_even_regional_indicators(const Text * text, size_t at) {
  size_t count = 0;
  size_t scan = at;
  for (;;) {
    LbChar here = lb_prev(text, scan);
    if (!here.present || here.value != LB_RI) {
      break;
    }
    count++;
    scan = here.start;
  }

  return (count % 2) == 0;
}

/**
 * UAX #14 revision 55 (Unicode 17.0.0), rules LB4 to LB31.
 *
 * In the rules' own order, because they are ordered: the first that applies
 * decides, and LB31 is the "break everywhere else" that ends the list. LB1
 * is not here - the generator resolved it into the table - and LB2 and LB3
 * are the ends of the subject, which the caller answers.
 */
static int line_break(const Text * text, size_t at) {
  LbChar left = lb_prev(text, at);
  LbChar right = lb_next(text, at);
  uint32_t a = left.value;
  uint32_t raw_b = right.value;

  // The character immediately on the left, before LB9 folds a combining run
  // into its base. Only LB8a wants it: that rule is listed *before* LB9, so
  // it is about the zero-width joiner itself rather than about whatever the
  // joiner has been made part of.
  uint32_t raw_a = LB_AL;
  {
    uint32_t codepoint = 0;
    size_t start = 0;
    if (at_prev(text, at, &codepoint, &start)) {
      raw_a = lb_of(codepoint);
    }
  }

  if (a == LB_BK) {
    return 1; // LB4
  }
  if (a == LB_CR && raw_b == LB_LF) {
    return 0; // LB5
  }
  if (a == LB_CR || a == LB_LF || a == LB_NL) {
    return 1; // LB5
  }
  if (raw_b == LB_BK || raw_b == LB_CR || raw_b == LB_LF || raw_b == LB_NL) {
    return 0; // LB6
  }
  if (raw_b == LB_SP || raw_b == LB_ZW) {
    return 0; // LB7
  }

  // LB8: `ZW SP* ÷`. The spaces are walked back over before asking, because
  // the rule reaches through them.
  {
    size_t before_spaces = lb_skip_back(text, at, LB_SP);
    LbChar anchor = lb_prev(text, before_spaces);
    if (anchor.present && anchor.value == LB_ZW) {
      return 1;
    }
  }

  if (raw_a == LB_ZWJ) {
    return 0; // LB8a
  }
  if (lb_combining(raw_b) && left.present && !lb_no_attach(a)) {
    return 0; // LB9: the run on the right belongs to the base on the left.
  }

  // LB10: a combining mark with no base is an A.
  uint32_t b = lb_combining(raw_b) ? (uint32_t)LB_AL : raw_b;
  if (b == LB_WJ || a == LB_WJ) {
    return 0; // LB11
  }
  if (a == LB_GL) {
    return 0; // LB12
  }
  if (b == LB_GL && a != LB_SP && a != LB_BA && a != LB_HY && a != LB_HH) {
    return 0; // LB12a
  }
  if (b == LB_CL || b == LB_CP || b == LB_EX || b == LB_SY) {
    return 0; // LB13
  }

  // LB14: `OP SP* ×`.
  {
    size_t before_spaces = lb_skip_back(text, at, LB_SP);
    LbChar anchor = lb_prev(text, before_spaces);
    if (anchor.present && anchor.value == LB_OP) {
      return 0;
    }
  }

  // LB15a: an initial quote at the start of a line, after a space, an
  // opener, another quote, or a hard break - reaching through spaces.
  {
    size_t before_spaces = lb_skip_back(text, at, LB_SP);
    LbChar quote = lb_prev(text, before_spaces);
    if (quote.present && quote.value == LB_QU_PI) {
      LbChar anchor = lb_prev(text, quote.start);
      if (!anchor.present || anchor.value == LB_BK || anchor.value == LB_CR
          || anchor.value == LB_LF || anchor.value == LB_NL
          || anchor.value == LB_OP || lb_quote(anchor.value)
          || anchor.value == LB_GL || anchor.value == LB_SP
          || anchor.value == LB_ZW) {
        return 0;
      }
    }
  }

  // LB15b: a final quote before a space, a prohibited break, another quote,
  // or the end of the text.
  if (b == LB_QU_PF) {
    LbChar after = lb_next_base(text, right.end);
    uint32_t c = after.value;
    if (!after.present || c == LB_SP || c == LB_GL || c == LB_WJ
        || c == LB_CL || lb_quote(c) || c == LB_CP || c == LB_EX
        || c == LB_IS || c == LB_SY || c == LB_BK || c == LB_CR
        || c == LB_LF || c == LB_NL || c == LB_ZW) {
      return 0;
    }
  }

  // LB15c: `SP ÷ IS NU`, which is what makes "subtract .5" break before the
  // decimal mark rather than after the space.
  if (a == LB_SP && b == LB_IS) {
    LbChar after = lb_next_base(text, right.end);
    if (after.present && after.value == LB_NU) {
      return 1;
    }
  }
  if (b == LB_IS) {
    return 0; // LB15d
  }

  // LB16: `(CL | CP) SP* × NS`.
  if (b == LB_NS) {
    size_t before_spaces = lb_skip_back(text, at, LB_SP);
    LbChar anchor = lb_prev(text, before_spaces);
    if (anchor.present && (anchor.value == LB_CL || anchor.value == LB_CP)) {
      return 0;
    }
  }

  // LB17: `B2 SP* × B2`.
  if (b == LB_B2) {
    size_t before_spaces = lb_skip_back(text, at, LB_SP);
    LbChar anchor = lb_prev(text, before_spaces);
    if (anchor.present && anchor.value == LB_B2) {
      return 0;
    }
  }

  if (a == LB_SP) {
    return 1; // LB18
  }
  if (b == LB_QU || b == LB_QU_PF) {
    return 0; // LB19: `x [QU - \p{Pi}]`
  }
  if (a == LB_QU || a == LB_QU_PI) {
    return 0; // LB19: `[QU - \p{Pf}] x`
  }

  // LB19a: a quote is not broken from a neighbour unless East Asian
  // characters surround it.
  if (lb_quote(b)) {
    if (!east_asian(left.codepoint)) {
      return 0;
    }
    LbChar after = lb_next_base(text, right.end);
    if (!after.present || !east_asian(after.codepoint)) {
      return 0;
    }
  }
  if (lb_quote(a)) {
    if (!east_asian(right.codepoint)) {
      return 0;
    }
    LbChar before = lb_prev(text, left.start);
    if (!before.present || !east_asian(before.codepoint)) {
      return 0;
    }
  }

  if (b == LB_CB || a == LB_CB) {
    return 1; // LB20
  }

  // LB20a: a word-initial hyphen keeps its word.
  if ((a == LB_HY || a == LB_HH) && (b == LB_AL || b == LB_HL)) {
    LbChar before = lb_prev(text, left.start);
    if (!before.present || before.value == LB_BK || before.value == LB_CR
        || before.value == LB_LF || before.value == LB_NL
        || before.value == LB_SP || before.value == LB_ZW
        || before.value == LB_CB || before.value == LB_GL) {
      return 0;
    }
  }

  // LB21a: `HL (HY | HH) x [^HL]` comes before LB21, which would otherwise
  // break after the hyphen.
  if ((a == LB_HY || a == LB_HH) && b != LB_HL) {
    LbChar before = lb_prev(text, left.start);
    if (before.present && before.value == LB_HL) {
      return 0;
    }
  }

  if (b == LB_BA || b == LB_HH || b == LB_HY || b == LB_NS || a == LB_BB) {
    return 0; // LB21
  }
  if (a == LB_SY && b == LB_HL) {
    return 0; // LB21b
  }
  if (b == LB_IN) {
    return 0; // LB22
  }
  if ((a == LB_AL || a == LB_HL) && b == LB_NU) {
    return 0; // LB23
  }
  if (a == LB_NU && (b == LB_AL || b == LB_HL)) {
    return 0; // LB23
  }
  if (a == LB_PR && (b == LB_ID || b == LB_EB || b == LB_EM)) {
    return 0; // LB23a
  }
  if ((a == LB_ID || a == LB_EB || a == LB_EM) && b == LB_PO) {
    return 0; // LB23a
  }
  if ((a == LB_PR || a == LB_PO) && (b == LB_AL || b == LB_HL)) {
    return 0; // LB24
  }
  if ((a == LB_AL || a == LB_HL) && (b == LB_PR || b == LB_PO)) {
    return 0; // LB24
  }

  // LB25, the number rule, in the order the annex lists its lines.
  if (b == LB_PO || b == LB_PR) {
    // `NU ( SY | IS )* (CL | CP)? x (PO | PR)`.
    size_t from = at;
    if (a == LB_CL || a == LB_CP) {
      from = left.start;
    }
    if (lb_number_before(text, from, NULL)) {
      return 0;
    }
  }
  if ((a == LB_PO || a == LB_PR) && b == LB_OP) {
    // `(PO | PR) x OP IS? NU`.
    LbChar after = lb_next_base(text, right.end);
    if (after.present && after.value == LB_IS) {
      after = lb_next_base(text, after.end);
    }
    if (after.present && after.value == LB_NU) {
      return 0;
    }
  }
  if ((a == LB_PO || a == LB_PR || a == LB_HY || a == LB_IS) && b == LB_NU) {
    return 0; // LB25
  }
  if (b == LB_NU && lb_number_before(text, at, NULL)) {
    return 0; // LB25: `NU ( SY | IS )* x NU`
  }

  if (a == LB_JL
      && (b == LB_JL || b == LB_JV || b == LB_H2 || b == LB_H3)) {
    return 0; // LB26
  }
  if ((a == LB_JV || a == LB_H2) && (b == LB_JV || b == LB_JT)) {
    return 0; // LB26
  }
  if ((a == LB_JT || a == LB_H3) && b == LB_JT) {
    return 0; // LB26
  }
  if ((a == LB_JL || a == LB_JV || a == LB_JT || a == LB_H2 || a == LB_H3)
      && b == LB_PO) {
    return 0; // LB27
  }
  if (a == LB_PR
      && (b == LB_JL || b == LB_JV || b == LB_JT || b == LB_H2
          || b == LB_H3)) {
    return 0; // LB27
  }
  if ((a == LB_AL || a == LB_HL) && (b == LB_AL || b == LB_HL)) {
    return 0; // LB28
  }

  // LB28a, the Brahmic orthographic syllable. `[◌]` is one code point and
  // stands beside AK and AS in three of the four lines.
  {
    int left_ak = a == LB_AK || a == LB_AS
        || left.codepoint == LB_DOTTED_CIRCLE;
    int right_ak = b == LB_AK || b == LB_AS
        || right.codepoint == LB_DOTTED_CIRCLE;
    int right_dotted
        = b == LB_AK || right.codepoint == LB_DOTTED_CIRCLE;
    if (a == LB_AP && right_ak) {
      return 0;
    }
    if (left_ak && (b == LB_VF || b == LB_VI)) {
      return 0;
    }
    if (a == LB_VI && right_dotted) {
      LbChar before = lb_prev(text, left.start);
      int before_ak = before.present
          && (before.value == LB_AK || before.value == LB_AS
              || before.codepoint == LB_DOTTED_CIRCLE);
      if (before_ak) {
        return 0;
      }
    }
    if (left_ak && right_ak) {
      LbChar after = lb_next_base(text, right.end);
      if (after.present && after.value == LB_VF) {
        return 0;
      }
    }
  }

  if (a == LB_IS && (b == LB_AL || b == LB_HL)) {
    return 0; // LB29
  }
  if ((a == LB_AL || a == LB_HL || a == LB_NU) && b == LB_OP
      && !east_asian(right.codepoint)) {
    return 0; // LB30
  }
  if (a == LB_CP && !east_asian(left.codepoint)
      && (b == LB_AL || b == LB_HL || b == LB_NU)) {
    return 0; // LB30
  }
  if (a == LB_RI && b == LB_RI
      && lb_even_regional_indicators(text, left.start)) {
    return 0; // LB30a
  }
  if (a == LB_EB && b == LB_EM) {
    return 0; // LB30b
  }
  if (pictographic_unassigned(left.codepoint) && b == LB_EM) {
    return 0; // LB30b
  }

  return 1; // LB31
}

// --------------------------------------------------------------------------
// The entry point
// --------------------------------------------------------------------------

int grx_unicode_break_at(GRX_BreakKind kind, const char * subject,
    size_t length, size_t position) {
  if (position > length || (!subject && length)) {
    return 0;
  }

  // An empty subject has no boundary of any kind. Neither standard says so -
  // both break at the start and UAX #29 breaks at the end - but there are no
  // characters, so there is nothing for a boundary to fall between, and it
  // is what Perl answers. See break_internal.h.
  if (!length) {
    return 0;
  }

  Text text = {subject, length};
  uint32_t before = 0;
  uint32_t after = 0;
  size_t before_start = 0;
  size_t after_end = 0;
  int has_before = at_prev(&text, position, &before, &before_start);
  int has_after = at_next(&text, position, &after, &after_end);

  if (!has_before) {
    // The start of the subject. UAX #29 breaks here (GB1, WB1, SB1); UAX #14
    // never does (LB2).
    return kind != GRX_BREAK_LINE;
  }
  if (!has_after) {
    // The end. Every one of the four breaks here (GB2, WB2, SB2, LB3).
    return 1;
  }

  switch (kind) {
    case GRX_BREAK_GRAPHEME:
      return grapheme_break(&text, position, before, before_start, after);
    case GRX_BREAK_WORD:
      return word_break(&text, position, before, after, after_end);
    case GRX_BREAK_SENTENCE:
      return sentence_break(&text, position, before, after);
    case GRX_BREAK_LINE:
      return line_break(&text, position);
    case GRX_BREAK_COUNT:
    default:
      return 0;
  }
}
