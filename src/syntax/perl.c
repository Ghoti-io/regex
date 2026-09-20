/**
 * @file
 *
 * The Perl-family front end: how PCRE2 and Perl spell what they have.
 *
 * One file for two dialects, because they are one grammar with a short list
 * of differences (documentation/dialects.md section 9). Where they differ the
 * code asks flavour() rather than switching on GRX_Syntax at each use, so
 * that the list of differences is something a reader can count.
 *
 * The reference is pcre2pattern for PCRE2 10.46 and perlre for Perl 5.40, and
 * the oracle for every rule below is the installed pcre2test 10.46 - which
 * corrected several of them, each marked where it did.
 *
 * What this front end does *not* read is as much a decision as what it does.
 * A construct PCRE2 has and this library does not implement is
 * GRX_ERR_UNSUPPORTED with its own diagnostic, never a silent approximation:
 * `\X` is a grapheme cluster and not "any character", `(*script_run:...)`
 * constrains what its body may match and an ordinary group does not, and
 * accepting either as something close would tell a caller their pattern means
 * what it does not.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/pattern.h>
#include <ghoti.io/regex/syntax.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../core/core_internal.h"
#include "../parse/parse_internal.h"
#include "../unicode/unicode_internal.h"

/** Which of the two dialects this parse is reading. */
typedef enum {
  FLAVOUR_PCRE = 0, ///< PCRE2 10.46.
  FLAVOUR_PERL      ///< Perl 5.40.
} Flavour;

static Flavour flavour(const GRX_Parser * parser) {
  return parser->syntax == GRX_SYNTAX_PERL ? FLAVOUR_PERL : FLAVOUR_PCRE;
}

/** The longest group name this front end will accept, in bytes. */
#define GRX_PCRE_NAME_MAX 128

static GRX_Result pcre_skip_ignorable(GRX_Parser * parser);

/** The byte at an offset from the current position, or 0 past the end. */
static char byte_at(const GRX_Parser * parser, size_t ahead) {
  size_t index = parser->position + ahead;
  return index < parser->length ? parser->text[index] : '\0';
}

static int is_decimal(char c) { return c >= '0' && c <= '9'; }
static int is_octal(char c) { return c >= '0' && c <= '7'; }
static int is_hex(char c) {
  return is_decimal(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
}

static uint32_t hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return (uint32_t)(c - '0');
  }
  if (c >= 'a' && c <= 'f') {
    return (uint32_t)(c - 'a' + 10);
  }
  return (uint32_t)(c - 'A' + 10);
}

/** A name character: PCRE2's names are `[A-Za-z_][A-Za-z0-9_]*`. */
static int is_name_start(char c) {
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_';
}

static int is_name_char(char c) { return is_name_start(c) || is_decimal(c); }

/** Whether `text` continues with `word` at the current position. */
static int looking_at(const GRX_Parser * parser, const char * word) {
  size_t length = strlen(word);
  if (parser->position + length > parser->length) {
    return 0;
  }

  return memcmp(parser->text + parser->position, word, length) == 0;
}

// --------------------------------------------------------------------------
// What an escape turned out to be
// --------------------------------------------------------------------------

/**
 * The escapes of this family, as a small vocabulary.
 *
 * One reader serves both contexts - an atom and a class item - because the
 * escapes are the same list in both and only a handful behave differently:
 * `\b` is a word boundary outside a class and a backspace inside one, and
 * the anchors, `\R`, `\X` and `\K` are errors inside one. Reading the list
 * twice would be two places for a new escape to be forgotten.
 */
typedef enum {
  ESC_LITERAL = 0, ///< One code point, in `codepoint`.
  ESC_SHORTHAND,   ///< `\d` and kin; `shorthand` and `negated`.
  ESC_PROPERTY,    ///< `\p{...}`; `name` and `negated`.
  ESC_BACKSPACE,   ///< `\b` inside a class.
  ESC_ANCHOR,      ///< `\b`, `\A`, `\z`, `\Z`, `\G`; `anchor`.
  ESC_BACKREF,     ///< `\1`, `\g{-1}`, `\k<name>`.
  ESC_SUBROUTINE,  ///< `\g<1>`, `\g'name'`.
  ESC_KEEP,        ///< `\K`.
  ESC_NEWLINE_SET, ///< `\R`.
  ESC_GRAPHEME,    ///< `\X`.
  ESC_NOT_NEWLINE  ///< `\N` with no `{`.
} EscapeKind;

/** What read_escape() found. */
typedef struct {
  EscapeKind kind;
  uint32_t codepoint;  ///< ESC_LITERAL.
  int shorthand;       ///< ESC_SHORTHAND: a GRX_ShorthandKind.
  int negated;         ///< ESC_SHORTHAND, ESC_PROPERTY.
  uint32_t name;       ///< ESC_PROPERTY, ESC_BACKREF, ESC_SUBROUTINE.
  uint32_t group;      ///< ESC_BACKREF, ESC_SUBROUTINE: a number, or 0.
  int relative;        ///< ESC_BACKREF, ESC_SUBROUTINE: `\g{-1}`.
  GRX_AnchorKind anchor; ///< ESC_ANCHOR.
  size_t offset;       ///< Where the backslash was.
  size_t length;       ///< Bytes the whole escape spans.
} Escape;

// --------------------------------------------------------------------------
// Numeric escapes
// --------------------------------------------------------------------------

/**
 * Read `\x`, the `x` already consumed.
 *
 * `\x{...}` is any number of hex digits; `\xHH` is *up to* two, so `\x` alone
 * is NUL and `\xg` is NUL followed by a literal `g`. That last rule is the
 * one that differs from ECMAScript, where the digits are required.
 */
static GRX_Result read_hex(GRX_Parser * parser, size_t start,
    uint32_t * out_value) {
  if (byte_at(parser, 0) == '{') {
    // Perl lets an underscore separate the digits, the way a numeric literal
    // does: `\x{_1_0000}` is U+10000. PCRE2 does not, and says "Malformed
    // \x{ escape" for the same pattern. Perl also reads `\x{_}` as zero,
    // which this still refuses: a digit is what the escape is for.
    int separators = flavour(parser) == FLAVOUR_PERL;
    size_t scan = 1;
    uint32_t value = 0;
    int digits = 0;
    for (;;) {
      if (separators && byte_at(parser, scan) == '_') {
        scan++;
        continue;
      }
      if (!is_hex(byte_at(parser, scan))) {
        break;
      }
      value = (value << 4) | hex_value(byte_at(parser, scan));
      if (value > GRX_CODEPOINT_MAX) {
        return grx_parse_fail(
            parser, GRX_DIAG_CODEPOINT_OUT_OF_RANGE, start, scan + 2);
      }
      digits++;
      scan++;
    }
    if (!digits || byte_at(parser, scan) != '}') {
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_HEX_ESCAPE, start, scan + 2);
    }
    parser->position += scan + 1;
    *out_value = value;
    return GRX_OK;
  }

  uint32_t value = 0;
  int digits = 0;
  for (; digits < 2 && is_hex(byte_at(parser, 0)); digits++) {
    value = (value << 4) | hex_value(byte_at(parser, 0));
    parser->position++;
  }
  if (!digits) {
    // pcre2test 10.46: "digits missing after \x". Older PCRE2 read `\x` with
    // no digits as NUL, and the corpus is what says which of the two this
    // library has to be.
    return grx_parse_fail(parser, GRX_DIAG_INVALID_HEX_ESCAPE, start, 2);
  }
  *out_value = value;
  return GRX_OK;
}

/** Read `\o{...}`, the `o` already consumed. */
static GRX_Result read_braced_octal(GRX_Parser * parser, size_t start,
    uint32_t * out_value) {
  if (byte_at(parser, 0) != '{') {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_OCTAL_ESCAPE, start, 2);
  }

  size_t scan = 1;
  uint64_t value = 0;
  int digits = 0;
  while (is_octal(byte_at(parser, scan))) {
    value = value * 8 + (uint64_t)(byte_at(parser, scan) - '0');
    if (value > GRX_CODEPOINT_MAX) {
      return grx_parse_fail(
          parser, GRX_DIAG_CODEPOINT_OUT_OF_RANGE, start, scan + 2);
    }
    digits++;
    scan++;
  }
  if (!digits || byte_at(parser, scan) != '}') {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_OCTAL_ESCAPE, start, scan + 2);
  }

  parser->position += scan + 1;
  *out_value = (uint32_t)value;
  return GRX_OK;
}

/** Read up to `allowed` octal digits from the current position. */
static uint32_t read_octal_digits(GRX_Parser * parser, size_t allowed) {
  uint32_t value = 0;
  for (size_t i = 0; i < allowed && is_octal(byte_at(parser, 0)); i++) {
    value = value * 8 + (uint32_t)(byte_at(parser, 0) - '0');
    parser->position++;
  }

  return value;
}

/**
 * Read `\cX`, the `c` already consumed.
 *
 * PCRE2 takes the character, upper-cases a lower-case letter, and XORs with
 * 0x40 - so `\cA` is 1, `\c{` is 0x3B, and only a value above 127 is an
 * error. That is wider than ECMAScript's letters-only rule, and `\c]` in the
 * corpus is why it is written out rather than borrowed.
 */
static GRX_Result read_control(GRX_Parser * parser, size_t start,
    uint32_t * out_value) {
  if (grx_parse_at_end(parser)) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_CONTROL_ESCAPE, start, 2);
  }

  unsigned char c = (unsigned char)byte_at(parser, 0);
  if (c > 127) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_CONTROL_ESCAPE, start, 3);
  }
  if (c >= 'a' && c <= 'z') {
    c = (unsigned char)(c - 'a' + 'A');
  }
  parser->position++;
  *out_value = (uint32_t)(c ^ 0x40u);
  return GRX_OK;
}

// --------------------------------------------------------------------------
// Names
// --------------------------------------------------------------------------

/**
 * Read a group name up to `terminator` and store it in the pattern.
 *
 * PCRE2's names are `[A-Za-z_][A-Za-z0-9_]*`. A name starting with a digit is
 * refused here rather than being read as a number, because `(?<1>a)` is an
 * error in PCRE2 and reading it as group 1 would accept a pattern the
 * reference rejects.
 */
static GRX_Result read_name(GRX_Parser * parser, char terminator,
    size_t start, uint32_t * out_offset) {
  // `\k{ name }` and `\g{ 1 }`: both references allow space around what is
  // inside the braces, and pcre2test accepts them.
  if (terminator == '}') {
    while (byte_at(parser, 0) == ' ' || byte_at(parser, 0) == '\t') {
      parser->position++;
    }
  }

  size_t first = parser->position;
  if (!is_name_start(byte_at(parser, 0))) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_NAME, start,
        parser->position - start + 1);
  }

  while (is_name_char(byte_at(parser, 0))) {
    parser->position++;
  }
  size_t length = parser->position - first;
  if (terminator == '}') {
    while (byte_at(parser, 0) == ' ' || byte_at(parser, 0) == '\t') {
      parser->position++;
    }
  }
  if (length > GRX_PCRE_NAME_MAX) {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_GROUP_NAME, start, parser->position - start);
  }
  if (!grx_parse_eat(parser, terminator)) {
    return grx_parse_fail(
        parser, GRX_DIAG_UNTERMINATED_NAME, start, parser->position - start);
  }

  GRX_Result result = grx_pattern_add_name(
      parser->pattern, parser->text + first, length, out_offset);
  if (result != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }

  return GRX_OK;
}

/** Whether a capturing group with this name has already been parsed. */
static int name_already_used(const GRX_Parser * parser, uint32_t offset) {
  const char * name = grx_pattern_name(parser->pattern, offset);
  if (!name) {
    return 0;
  }

  for (size_t i = 0; i < parser->pattern->nodes.count; i++) {
    const GRX_Node * node = grx_pattern_node(parser->pattern, (uint32_t)i);
    if (!node || node->kind != GRX_NODE_GROUP
        || !(node->flags & GRX_NODE_NAMED) || node->b == offset) {
      continue;
    }
    const char * existing = grx_pattern_name(parser->pattern, node->b);
    if (existing && strcmp(existing, name) == 0) {
      return 1;
    }
  }

  return 0;
}

// --------------------------------------------------------------------------
// The escape table, shared by both contexts
// --------------------------------------------------------------------------

/** The shorthand a letter names, or -1. */
static int shorthand_for(char c, int * out_negated) {
  *out_negated = 0;
  switch (c) {
    case 'd': return GRX_SHORTHAND_DIGIT;
    case 'D': *out_negated = 1; return GRX_SHORTHAND_DIGIT;
    case 'w': return GRX_SHORTHAND_WORD;
    case 'W': *out_negated = 1; return GRX_SHORTHAND_WORD;
    case 's': return GRX_SHORTHAND_SPACE;
    case 'S': *out_negated = 1; return GRX_SHORTHAND_SPACE;
    case 'h': return GRX_SHORTHAND_HSPACE;
    case 'H': *out_negated = 1; return GRX_SHORTHAND_HSPACE;
    case 'v': return GRX_SHORTHAND_VSPACE;
    case 'V': *out_negated = 1; return GRX_SHORTHAND_VSPACE;
    default: return -1;
  }
}

/** The code point a single-letter escape denotes, or 0 for "not one". */
static uint32_t simple_escape(char c) {
  switch (c) {
    case 'a': return 0x07;
    case 'e': return 0x1B;
    case 'f': return 0x0C;
    case 'n': return 0x0A;
    case 'r': return 0x0D;
    case 't': return 0x09;
    default: return 0;
  }
}

/**
 * Read `\p{...}` or `\P{...}`, the letter already consumed.
 *
 * Three spellings: `\p{Name}`, `\p{^Name}` (negated inside the braces, which
 * ECMAScript does not have) and the one-letter `\pL`. The name is stored as
 * written; resolving it against the Unicode tables is lowering's job.
 */
static GRX_Result read_property(GRX_Parser * parser, int negated, size_t start,
    Escape * out) {
  size_t first = parser->position;
  size_t length = 0;

  if (grx_parse_eat(parser, '{')) {
    if (grx_parse_eat(parser, '^')) {
      negated = !negated;
    }
    first = parser->position;
    while (!grx_parse_at_end(parser) && byte_at(parser, 0) != '}') {
      parser->position++;
    }
    if (!grx_parse_eat(parser, '}')) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_PROPERTY_SYNTAX, start,
          parser->position - start);
    }
    length = parser->position - first - 1;
  }
  else {
    // `\pL`: exactly one character names the property.
    if (grx_parse_at_end(parser)) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_PROPERTY_SYNTAX, start, 2);
    }
    parser->position++;
    length = 1;
  }

  if (!length) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_PROPERTY_SYNTAX, start,
        parser->position - start);
  }

  // Resolved here as well as at lowering, and the answer thrown away. That
  // is not a duplicate check: lowering reports GRX_DIAG_INTERNAL for a name
  // it cannot resolve, on the stated grounds that the parser has already
  // resolved it - and for this front end that was not true, so `\p{Nosuch}`
  // came back as an internal fault rather than as the unknown property it is.
  {
    const char * text = parser->text + first;
    const char * equals = memchr(text, '=', length);
    uint32_t property = 0;
    GRX_Result known = equals
        ? grx_unicode_property_lookup(text, (size_t)(equals - text),
              equals + 1, length - (size_t)(equals - text) - 1,
              parser->profile.property_match, &property)
        : grx_unicode_property_lookup(
              text, length, NULL, 0, parser->profile.property_match, &property);
    if (known != GRX_OK) {
      return grx_parse_fail(parser, GRX_DIAG_UNKNOWN_PROPERTY, start,
          parser->position - start);
    }
  }

  uint32_t offset = GRX_INDEX_NONE;
  GRX_Result result = grx_pattern_add_name(
      parser->pattern, parser->text + first, length, &offset);
  if (result != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }

  out->kind = ESC_PROPERTY;
  out->name = offset;
  out->negated = negated;
  return GRX_OK;
}

/**
 * Whether the `{` at an offset from here could be a quantifier's.
 *
 * Both references allow spaces inside the braces - `\N{ 3 }` is three of
 * anything - so the shape is digits, commas and blanks, with at least one
 * digit, closed by a brace. What it cannot be is a name.
 */
static int brace_is_repeat(const GRX_Parser * parser, size_t at) {
  size_t scan = at + 1;
  int digits = 0;
  while (is_decimal(byte_at(parser, scan)) || byte_at(parser, scan) == ','
      || byte_at(parser, scan) == ' ' || byte_at(parser, scan) == '\t') {
    digits += is_decimal(byte_at(parser, scan));
    scan++;
  }

  return digits && byte_at(parser, scan) == '}';
}

/**
 * Read `\N`, the `N` already consumed.
 *
 * `\N` alone is "any character that is not a newline". `\N{U+hhhh}` is a code
 * point, and PCRE2 accepts it only in UTF mode. `\N{name}` is a Perl
 * character-name lookup that PCRE2 does not implement and neither does this.
 */
static GRX_Result read_named_codepoint(GRX_Parser * parser, int in_class,
    size_t start, Escape * out) {
  if (byte_at(parser, 0) != '{') {
    if (in_class) {
      // "not a newline" is not a set operation a class can express, and
      // pcre2test refuses `[\N]`. Checked before the look-ahead below,
      // because a class does not ignore what a pattern ignores.
      return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_ITEM, start, 2);
    }

    // A comment or an extended-mode space may stand between the `N` and a
    // brace, and both references still read `\N {3}` as a quantified `\N`.
    // They part over a brace that is *not* a quantifier: perl refuses
    // `/abc\N {SPACE}/x` with "Missing braces on \N{}", while pcre2test
    // compiles `/\N {U+41}/x,utf` as `\N` followed by six literals. Only
    // Perl's half is checked here, and the skip is undone: what it passed
    // over is ignorable, but only the ordinary path may consume it.
    if (flavour(parser) == FLAVOUR_PERL) {
      size_t saved_position = parser->position;
      size_t saved_quote = parser->quote_end;
      (void)pcre_skip_ignorable(parser);
      int detached_name = byte_at(parser, 0) == '{'
          && !brace_is_repeat(parser, 0);
      parser->position = saved_position;
      parser->quote_end = saved_quote;
      if (detached_name) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_ESCAPE, start, 2);
      }
    }

    out->kind = ESC_NOT_NEWLINE;
    return GRX_OK;
  }

  // `\N{ U+0100 }`: both references allow space inside the braces. `scan`
  // counts from the `{`, so scan 1 is the first character inside it.
  size_t scan = 1;
  while (byte_at(parser, scan) == ' ' || byte_at(parser, scan) == '\t') {
    scan++;
  }

  if (byte_at(parser, scan) == 'U' && byte_at(parser, scan + 1) == '+') {
    scan += 2;
    uint64_t value = 0;
    int digits = 0;
    while (is_hex(byte_at(parser, scan))) {
      value = (value << 4) | hex_value(byte_at(parser, scan));
      if (value > GRX_CODEPOINT_MAX) {
        return grx_parse_fail(
            parser, GRX_DIAG_CODEPOINT_OUT_OF_RANGE, start, scan + 2);
      }
      digits++;
      scan++;
    }
    while (byte_at(parser, scan) == ' ' || byte_at(parser, scan) == '\t') {
      scan++;
    }
    if (!digits || byte_at(parser, scan) != '}') {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_ESCAPE, start, scan + 2);
    }
    if (!(parser->options & GRX_OPT_UTF)) {
      // pcre2test: "\N{U+dddd} is supported only in Unicode (UTF) mode".
      return grx_parse_fail(parser, GRX_DIAG_INVALID_ESCAPE, start, scan + 2);
    }
    parser->position += scan + 1;
    out->kind = ESC_LITERAL;
    out->codepoint = (uint32_t)value;
    return GRX_OK;
  }

  // `\N{2,3}` is `\N` quantified, and the `{` is the quantifier's. Only a
  // brace that cannot be a quantifier is the `\N{name}` form, which neither
  // PCRE2 nor this library implements.
  if (brace_is_repeat(parser, 0)) {
    if (in_class) {
      // `[\N{4}]` is a class item that quantifies nothing; pcre2test reads
      // the brace as a name it does not support and refuses the pattern.
      return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_ITEM, start, 2);
    }
    out->kind = ESC_NOT_NEWLINE;
    return GRX_OK;
  }

  return grx_parse_fail(parser, GRX_DIAG_INVALID_ESCAPE, start, 3);
}

/**
 * Resolve a reference written relative to where it stands.
 *
 * `\g{-1}` is the most recently opened group and `\g{+1}` is the next one to
 * be opened, so both are the running count plus an offset. Resolved here
 * rather than at lowering because the parser is what knows the count; the
 * node keeps GRX_NODE_RELATIVE so that a dump can still say how it was
 * written.
 */
static GRX_Result resolve_relative(GRX_Parser * parser, int sign,
    uint64_t magnitude, size_t start, uint32_t * out_group) {
  uint64_t base = (uint64_t)parser->groups_opened;

  // Computed only once the subtraction is known not to wrap: `\g{-9}` in a
  // pattern with two groups is an error, not group 4294967289.
  if (!magnitude || (sign < 0 && magnitude > base)) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_BACKREFERENCE, start,
        parser->position - start);
  }

  uint64_t target = sign < 0 ? base + 1 - magnitude : base + magnitude;
  if (!target || target > (uint64_t)parser->group_count) {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_BACKREFERENCE, start,
        parser->position - start);
  }

  *out_group = (uint32_t)target;
  return GRX_OK;
}

/**
 * Read a `\g` reference, the `g` already consumed.
 *
 * Five spellings and two meanings. `\g1`, `\g{1}`, `\g{-1}` and `\g{name}`
 * are backreferences; `\g<1>`, `\g<name>`, `\g'1'` and `\g'name'` are
 * subroutine calls, which is a different construct with the same sigil.
 */
static GRX_Result read_g_reference(GRX_Parser * parser, size_t start,
    Escape * out) {
  char open = byte_at(parser, 0);
  int subroutine = open == '<' || open == '\'';
  char terminator = open == '<' ? '>' : (open == '\'' ? '\'' : '}');

  if (open == '{' || subroutine) {
    parser->position++;
    if (open == '{') {
      while (byte_at(parser, 0) == ' ' || byte_at(parser, 0) == '\t') {
        parser->position++;
      }
    }
  }
  else {
    terminator = '\0';
  }

  int sign = 0;
  if (byte_at(parser, 0) == '-') {
    sign = -1;
    parser->position++;
  }
  else if (byte_at(parser, 0) == '+') {
    sign = 1;
    parser->position++;
  }

  if (is_decimal(byte_at(parser, 0))) {
    uint64_t value = 0;
    while (is_decimal(byte_at(parser, 0)) && value < 0x7FFFFFFFu) {
      value = value * 10 + (uint64_t)(byte_at(parser, 0) - '0');
      parser->position++;
    }
    if (terminator == '}') {
      while (byte_at(parser, 0) == ' ' || byte_at(parser, 0) == '\t') {
        parser->position++;
      }
    }
    if (terminator && !grx_parse_eat(parser, terminator)) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_BACKREFERENCE, start,
          parser->position - start);
    }

    out->kind = subroutine ? ESC_SUBROUTINE : ESC_BACKREF;
    out->name = GRX_INDEX_NONE;
    out->relative = sign != 0;
    if (sign) {
      return resolve_relative(parser, sign, value, start, &out->group);
    }
    if (!value) {
      // `\g0` is an error; `\g<0>` is a call to the whole pattern, which is
      // what `(?R)` also spells.
      if (!subroutine) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_BACKREFERENCE, start,
            parser->position - start);
      }
      out->group = 0;
      return GRX_OK;
    }
    if (value > (uint64_t)parser->group_count) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_BACKREFERENCE, start,
          parser->position - start);
    }
    out->group = (uint32_t)value;
    return GRX_OK;
  }

  if (!terminator || sign) {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_BACKREFERENCE, start, parser->position - start);
  }

  uint32_t offset = GRX_INDEX_NONE;
  GRX_Result result = read_name(parser, terminator, start, &offset);
  if (result != GRX_OK) {
    return result;
  }

  out->kind = subroutine ? ESC_SUBROUTINE : ESC_BACKREF;
  out->group = 0;
  out->name = offset;
  out->relative = 0;
  return GRX_OK;
}

/** Read `\k<name>`, `\k'name'` or `\k{name}`, the `k` already consumed. */
static GRX_Result read_k_reference(GRX_Parser * parser, size_t start,
    Escape * out) {
  char open = byte_at(parser, 0);
  char terminator = open == '<' ? '>' : (open == '\'' ? '\'' : '}');
  if (open != '<' && open != '\'' && open != '{') {
    return grx_parse_fail(parser, GRX_DIAG_INVALID_BACKREFERENCE, start, 2);
  }
  parser->position++;

  uint32_t offset = GRX_INDEX_NONE;
  GRX_Result result = read_name(parser, terminator, start, &offset);
  if (result != GRX_OK) {
    return result;
  }

  out->kind = ESC_BACKREF;
  out->group = 0;
  out->name = offset;
  out->relative = 0;
  return GRX_OK;
}

/**
 * Read a numeric escape that may be a backreference or may be octal.
 *
 * pcre2pattern: a sequence of digits after `\` is a backreference when the
 * value is below 10, or when at least that many capturing parentheses have
 * been opened. Otherwise up to three octal digits are read. The whole number
 * decides, not the first digit, which is why `\10` in a pattern with twelve
 * groups is group 10 and in a pattern with two is a tab.
 */
static GRX_Result read_numeric_escape(GRX_Parser * parser, size_t start,
    Escape * out) {
  size_t digits = 0;
  uint64_t value = 0;
  while (is_decimal(byte_at(parser, digits)) && value < 0x7FFFFFFFu) {
    value = value * 10 + (uint64_t)(byte_at(parser, digits) - '0');
    digits++;
  }

  if (value < 10 || value <= parser->groups_opened) {
    if (value > parser->group_count) {
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_BACKREFERENCE, start, digits + 1);
    }
    parser->position += digits;
    out->kind = ESC_BACKREF;
    out->group = (uint32_t)value;
    out->name = GRX_INDEX_NONE;
    out->relative = 0;
    return GRX_OK;
  }

  if (!is_octal(byte_at(parser, 0))) {
    // `\8` and `\81` are neither a reference nor octal, and pcre2test calls
    // both "reference to non-existent subpattern" rather than reading the
    // digits as literals.
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_BACKREFERENCE, start, digits + 1);
  }

  out->kind = ESC_LITERAL;
  out->codepoint = read_octal_digits(parser, 3);
  return GRX_OK;
}

/**
 * Read one escape, the backslash already consumed.
 *
 * `in_class` selects the handful of rules that differ inside a bracket
 * expression rather than a second copy of the table.
 */
static GRX_Result read_escape(GRX_Parser * parser, int in_class, Escape * out) {
  size_t start = parser->position - 1;
  *out = (Escape) {
    .kind = ESC_LITERAL,
    .codepoint = 0,
    .shorthand = 0,
    .negated = 0,
    .name = GRX_INDEX_NONE,
    .group = 0,
    .relative = 0,
    .anchor = GRX_ANCHOR_CARET,
    .offset = start,
    .length = 0,
  };

  if (grx_parse_at_end(parser)) {
    return grx_parse_fail(parser, GRX_DIAG_TRAILING_BACKSLASH, start, 1);
  }

  char c = byte_at(parser, 0);

  int negated = 0;
  int shorthand = shorthand_for(c, &negated);
  if (shorthand >= 0) {
    parser->position++;
    out->kind = ESC_SHORTHAND;
    out->shorthand = shorthand;
    out->negated = negated;
    out->length = parser->position - start;
    return GRX_OK;
  }

  uint32_t simple = simple_escape(c);
  if (simple) {
    parser->position++;
    out->codepoint = simple;
    out->length = parser->position - start;
    return GRX_OK;
  }

  switch (c) {
    case 'b':
      parser->position++;
      // The one escape whose meaning depends on where it is: a word boundary
      // outside a class and the backspace character inside one. Both
      // dialects, and both references say so in the same sentence.
      out->kind = in_class ? ESC_BACKSPACE : ESC_ANCHOR;
      out->codepoint = 0x08;
      out->anchor = GRX_ANCHOR_WORD_BOUNDARY;
      out->length = parser->position - start;
      return GRX_OK;

    case 'B':
    case 'A':
    case 'Z':
    case 'z':
    case 'G':
      if (in_class) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_ITEM, start, 2);
      }
      parser->position++;
      out->kind = ESC_ANCHOR;
      out->anchor = c == 'B'   ? GRX_ANCHOR_NOT_WORD_BOUNDARY
          : c == 'A'           ? GRX_ANCHOR_START_SUBJECT
          : c == 'Z'           ? GRX_ANCHOR_END_BEFORE_NEWLINE
          : c == 'z'           ? GRX_ANCHOR_END_SUBJECT
                               : GRX_ANCHOR_SEARCH_START;
      out->length = parser->position - start;
      return GRX_OK;

    case 'K':
    case 'R':
    case 'X':
      if (in_class) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_ITEM, start, 2);
      }
      parser->position++;
      out->kind = c == 'K' ? ESC_KEEP
          : c == 'R'       ? ESC_NEWLINE_SET
                           : ESC_GRAPHEME;
      out->length = parser->position - start;
      return GRX_OK;

    case 'N': {
      // `\N{U+hhhh}` is a code point and so is a class item; every other
      // spelling of `\N` is "not a newline" and is not. read_named_codepoint
      // is what knows which of the two this is.
      parser->position++;
      GRX_Result result = read_named_codepoint(parser, in_class, start, out);
      out->length = parser->position - start;
      return result;
    }

    case 'C':
      // One code unit. Meaningless here: this library's subject is code
      // points, and a construct that can land in the middle of a character
      // has no honest approximation.
      parser->position++;
      return grx_parse_fail(
          parser, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, start, 2);

    case 'p':
    case 'P': {
      parser->position++;
      GRX_Result result = read_property(parser, c == 'P', start, out);
      out->length = parser->position - start;
      return result;
    }

    case 'x': {
      parser->position++;
      GRX_Result result = read_hex(parser, start, &out->codepoint);
      out->length = parser->position - start;
      return result;
    }

    case 'o': {
      parser->position++;
      GRX_Result result = read_braced_octal(parser, start, &out->codepoint);
      out->length = parser->position - start;
      return result;
    }

    case 'c': {
      parser->position++;
      GRX_Result result = read_control(parser, start, &out->codepoint);
      out->length = parser->position - start;
      return result;
    }

    case '0':
      parser->position++;
      out->codepoint = read_octal_digits(parser, 2);
      out->length = parser->position - start;
      return GRX_OK;

    case 'g': {
      if (in_class) {
        // There are no references inside a bracket expression, and pcre2test
        // accepts `[\ga]` and `[\g<a>]` as classes containing the letter.
        parser->position++;
        out->codepoint = (uint32_t)c;
        out->length = parser->position - start;
        return GRX_OK;
      }
      parser->position++;
      GRX_Result result = read_g_reference(parser, start, out);
      out->length = parser->position - start;
      return result;
    }

    case 'k': {
      if (in_class) {
        parser->position++;
        out->codepoint = (uint32_t)c;
        out->length = parser->position - start;
        return GRX_OK;
      }
      parser->position++;
      GRX_Result result = read_k_reference(parser, start, out);
      out->length = parser->position - start;
      return result;
    }

    default:
      break;
  }

  if (c >= '1' && c <= '9') {
    if (in_class) {
      // Inside a class there are no backreferences, so a digit escape is
      // octal from the first digit: `[\1]` is U+0001.
      out->codepoint = read_octal_digits(parser, 3);
      out->length = parser->position - start;
      return GRX_OK;
    }
    GRX_Result result = read_numeric_escape(parser, start, out);
    out->length = parser->position - start;
    return result;
  }

  // An identity escape. Both dialects allow a backslash before any
  // non-alphanumeric character and reject it before a letter or digit that
  // has no meaning, which is what keeps `\j` from silently being "j".
  uint32_t codepoint = 0;
  GRX_Result result = grx_parse_take(parser, &codepoint);
  if (result != GRX_OK) {
    return result;
  }
  if ((codepoint >= 'a' && codepoint <= 'z')
      || (codepoint >= 'A' && codepoint <= 'Z')
      || (codepoint >= '0' && codepoint <= '9')) {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_ESCAPE, start, parser->position - start);
  }

  out->codepoint = codepoint;
  out->length = parser->position - start;
  return GRX_OK;
}

// --------------------------------------------------------------------------
// Turning an escape into a node or a class item
// --------------------------------------------------------------------------

/** Fill a class item from an escape that denotes one. */
static GRX_Result escape_as_item(GRX_Parser * parser, const Escape * escape,
    GRX_ClassItem * out) {
  *out = (GRX_ClassItem) {
    .kind = GRX_CLASS_ITEM_SINGLE,
    .flags = escape->negated ? GRX_CLASS_ITEM_NEGATED : 0,
    .lo = 0,
    .hi = 0,
    .a = 0,
    .offset = escape->offset,
    .length = escape->length,
  };

  switch (escape->kind) {
    case ESC_LITERAL:
    case ESC_BACKSPACE:
      out->lo = escape->kind == ESC_BACKSPACE ? 0x08 : escape->codepoint;
      return GRX_OK;

    case ESC_SHORTHAND:
      out->kind = GRX_CLASS_ITEM_SHORTHAND;
      out->a = (uint32_t)escape->shorthand;
      return GRX_OK;

    case ESC_PROPERTY:
      out->kind = GRX_CLASS_ITEM_PROPERTY;
      out->a = escape->name;
      return GRX_OK;

    default:
      // Everything else - the anchors, `\K`, `\R`, `\X`, `\Q` - has already
      // been refused by read_escape() in class context, so arriving here is
      // this file disagreeing with itself.
      return grx_parse_fail(parser, GRX_DIAG_INTERNAL, escape->offset, 1);
  }
}

/** Append a node holding one class item. */
static GRX_Result item_as_node(GRX_Parser * parser, const GRX_ClassItem * item,
    uint32_t * out_node) {
  GRX_Result result = grx_parse_class_node(parser, item->offset, out_node);
  if (result != GRX_OK) {
    return result;
  }
  result = grx_parse_class_add(parser, *out_node, item);
  if (result != GRX_OK) {
    return result;
  }

  grx_pattern_node(parser->pattern, *out_node)->length = item->length;
  return GRX_OK;
}

/** Append a node of a kind with no payload, reporting failures as diagnostics. */
static GRX_Result plain_node(GRX_Parser * parser, GRX_NodeKind kind,
    size_t offset, size_t length, uint32_t * out_node) {
  GRX_Result result
      = grx_pattern_add_node(parser->pattern, kind, offset, length, out_node);
  if (result != GRX_OK) {
    return grx_parse_fail(parser,
        result == GRX_ERR_LIMIT ? GRX_DIAG_LIMIT_NODES : GRX_DIAG_OUT_OF_MEMORY,
        offset, length);
  }

  return GRX_OK;
}

/**
 * The code point runs `\R` stands for.
 *
 * pcre2pattern: `\R` is `(?>\r\n|\n|\x0b|\f|\r|\x85)`, and with the default
 * BSR_UNICODE also U+2028 and U+2029. CR LF is a *sequence*, which is why
 * this is a set of strings and not a character class - and why it is atomic:
 * `\R` never gives back the LF to let `\n` match it again.
 */
static GRX_Result newline_set_node(GRX_Parser * parser, size_t start,
    uint32_t * out_node) {
  static const uint32_t crlf[2] = {0x0D, 0x0A};
  static const uint32_t singles[]
      = {0x0A, 0x0B, 0x0C, 0x0D, 0x85, 0x2028, 0x2029};

  GRX_Result result = plain_node(
      parser, GRX_NODE_STRING_SET, start, parser->position - start, out_node);
  if (result != GRX_OK) {
    return result;
  }

  uint32_t first = GRX_INDEX_NONE;
  result = grx_pattern_add_string(parser->pattern, crlf, 2, &first);
  for (size_t i = 0; result == GRX_OK && i < sizeof(singles) / sizeof(*singles);
      i++) {
    uint32_t ignored = GRX_INDEX_NONE;
    result = grx_pattern_add_string(parser->pattern, &singles[i], 1, &ignored);
  }
  if (result != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }

  GRX_Node * node = grx_pattern_node(parser->pattern, *out_node);
  node->a = first;
  node->b = 1 + (uint32_t)(sizeof(singles) / sizeof(*singles));
  node->flags |= GRX_NODE_ATOMIC;
  return GRX_OK;
}

static GRX_Result pcre_atom_escape(GRX_Parser * parser, uint32_t * out_node) {
  size_t start = parser->position - 1;
  Escape escape;
  GRX_Result result = read_escape(parser, 0, &escape);
  if (result != GRX_OK) {
    return result;
  }

  switch (escape.kind) {
    case ESC_LITERAL:
      return grx_parse_literal_node(
          parser, escape.codepoint, start, escape.length, out_node);

    case ESC_SHORTHAND:
      return grx_parse_shorthand_node(parser,
          (GRX_ShorthandKind)escape.shorthand, escape.negated, start,
          escape.length, out_node);

    case ESC_PROPERTY: {
      GRX_ClassItem item;
      result = escape_as_item(parser, &escape, &item);
      if (result != GRX_OK) {
        return result;
      }
      return item_as_node(parser, &item, out_node);
    }

    case ESC_ANCHOR:
      result = plain_node(
          parser, GRX_NODE_ANCHOR, start, escape.length, out_node);
      if (result == GRX_OK) {
        grx_pattern_node(parser->pattern, *out_node)->a
            = (uint32_t)escape.anchor;
      }
      return result;

    case ESC_BACKREF:
      result = plain_node(
          parser, GRX_NODE_BACKREF, start, escape.length, out_node);
      if (result == GRX_OK) {
        GRX_Node * node = grx_pattern_node(parser->pattern, *out_node);
        node->a = escape.group;
        node->b = escape.name;
        if (escape.name != GRX_INDEX_NONE) {
          node->flags |= GRX_NODE_NAMED;
        }
        if (escape.relative) {
          node->flags |= GRX_NODE_RELATIVE;
        }
      }
      return result;

    case ESC_SUBROUTINE:
      result = plain_node(
          parser, GRX_NODE_RECURSE, start, escape.length, out_node);
      if (result == GRX_OK) {
        GRX_Node * node = grx_pattern_node(parser->pattern, *out_node);
        node->a = escape.group;
        node->b = escape.name;
        if (escape.name != GRX_INDEX_NONE) {
          node->flags |= GRX_NODE_NAMED;
        }
        if (escape.relative) {
          node->flags |= GRX_NODE_RELATIVE;
        }
      }
      return result;

    case ESC_KEEP:
      if (parser->in_lookaround) {
        // pcre2test: "\K is not allowed in lookarounds". What it would mean
        // there has no answer - a lookaround consumes nothing, so there is
        // no reported start for `\K` to move.
        return grx_parse_fail(
            parser, GRX_DIAG_INVALID_LOOKAROUND, start, escape.length);
      }
      return plain_node(parser, GRX_NODE_KEEP, start, escape.length, out_node);

    case ESC_NEWLINE_SET:
      return newline_set_node(parser, start, out_node);

    case ESC_GRAPHEME:
      // An extended grapheme cluster. The break rules are
      // documentation/plan.md WP-12's table and are not generated yet, and
      // "any character" is not a grapheme cluster.
      return grx_parse_fail(
          parser, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, start, escape.length);

    case ESC_NOT_NEWLINE:
      // An ANY node that is not allowed to be dot-all, rather than a class:
      // what `\N` excludes is the dialect's newline *set*, which is the one
      // thing GRX_IR_ANY already carries and a class item would have to
      // resolve a second time.
      result = plain_node(parser, GRX_NODE_ANY, start, escape.length, out_node);
      if (result == GRX_OK) {
        grx_pattern_node(parser->pattern, *out_node)->flags
            |= GRX_NODE_NEGATED;
      }
      return result;

    case ESC_BACKSPACE:
    default:
      return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 1);
  }
}

static GRX_Result pcre_class_escape(GRX_Parser * parser, GRX_ClassItem * out) {
  Escape escape;
  GRX_Result result = read_escape(parser, 1, &escape);
  if (result != GRX_OK) {
    return result;
  }

  return escape_as_item(parser, &escape, out);
}

// --------------------------------------------------------------------------
// Character classes
// --------------------------------------------------------------------------

/** The POSIX class names both dialects accept, in `[[:name:]]`. */
static const char * const posix_class_names[] = {
  "alnum", "alpha", "ascii", "blank", "cntrl", "digit", "graph", "lower",
  "print", "punct", "space", "upper", "word", "xdigit", NULL
};

/**
 * Whether `[` at the current position begins a POSIX bracket construct, and
 * where its terminator is.
 *
 * This is pcre2_compile.c's check_posix_syntax(), transcribed, and it is
 * transcribed rather than approximated because every rule in it decides a
 * corpus case. Scanning starts after the `[` and its `:`, `.` or `=`:
 *
 *   - a `\` before `]` or `\` skips both, so `[abc[:x\]pqr:]]` still finds
 *     its `:]` and reports an unknown class name rather than a stray `]`;
 *   - a `[` followed by the same terminator, or any `]`, means this was not
 *     a POSIX construct after all;
 *   - the terminator followed by `]` ends it.
 *
 * What is inside is *not* checked here: `[[:1234:]]` is POSIX syntax with a
 * name nobody has, which is an error, and `[[:]` is not POSIX syntax at all,
 * which is a class of two characters.
 */
static int posix_construct_at(const GRX_Parser * parser, size_t * out_end) {
  char terminator = byte_at(parser, 1);
  if (terminator != ':' && terminator != '.' && terminator != '=') {
    return 0;
  }

  for (size_t scan = 2;; scan++) {
    char c = byte_at(parser, scan);
    if (!c && parser->position + scan >= parser->length) {
      return 0;
    }
    if (c == '\\' && (byte_at(parser, scan + 1) == ']'
            || byte_at(parser, scan + 1) == '\\')) {
      scan++;
      continue;
    }
    if ((c == '[' && byte_at(parser, scan + 1) == terminator) || c == ']') {
      return 0;
    }
    if (c == terminator && byte_at(parser, scan + 1) == ']') {
      *out_end = scan;
      return 1;
    }
  }
}

/**
 * Read `[:name:]`, the position on the `[`.
 *
 * `out_matched` is cleared when this was not a POSIX construct, so that the
 * caller reads the `[` as the ordinary character it then is. The sentinel is
 * an output rather than a result code because GRX_ERR_SYNTAX is what an
 * *unknown class name* returns, and the two were the same value for one
 * revision of this file - which made `[[:foo:]]` a class of six characters.
 */
static GRX_Result read_posix_class(
    GRX_Parser * parser, GRX_ClassItem * out, int * out_matched) {
  size_t start = parser->position;
  size_t end = 0;
  *out_matched = 0;

  if (!posix_construct_at(parser, &end)) {
    return GRX_OK;
  }
  *out_matched = 1;

  char terminator = byte_at(parser, 1);
  if (terminator != ':') {
    // `[[.a.]]` and `[[=a=]]`: collating elements and equivalence classes.
    // Both need a locale's collation table, which this library does not
    // have and will not invent.
    return grx_parse_fail(
        parser, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, start, end + 2);
  }

  size_t scan = 2;
  int negated = 0;
  if (byte_at(parser, scan) == '^') {
    negated = 1;
    scan++;
  }

  const char * name = parser->text + parser->position + scan;
  size_t length = end - scan;
  int known = 0;
  for (size_t i = 0; posix_class_names[i]; i++) {
    if (strlen(posix_class_names[i]) == length
        && memcmp(posix_class_names[i], name, length) == 0) {
      known = 1;
      break;
    }
  }
  if (!known) {
    return grx_parse_fail(
        parser, GRX_DIAG_UNKNOWN_POSIX_CLASS, start, end + 2);
  }

  uint32_t offset = GRX_INDEX_NONE;
  GRX_Result result
      = grx_pattern_add_name(parser->pattern, name, length, &offset);
  if (result != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }

  parser->position += end + 2;
  *out = (GRX_ClassItem) {
    .kind = GRX_CLASS_ITEM_POSIX,
    .flags = negated ? GRX_CLASS_ITEM_NEGATED : 0,
    .lo = 0,
    .hi = 0,
    .a = offset,
    .offset = start,
    .length = parser->position - start,
  };
  return GRX_OK;
}

/** Whether a class item is one code point, and so may end a range. */
static int is_single(const GRX_ClassItem * item) {
  return item->kind == GRX_CLASS_ITEM_SINGLE;
}

/** Skip what `xx` mode ignores inside a bracket expression. */
static void skip_class_space(GRX_Parser * parser) {
  if (!(parser->options & GRX_OPT_EXTENDED_MORE)) {
    return;
  }

  while (byte_at(parser, 0) == ' ' || byte_at(parser, 0) == '\t') {
    parser->position++;
  }
}

/**
 * Read one class atom: an escape, a POSIX class, or a single character.
 *
 * `out_is_item` is cleared when the atom was consumed but produced nothing -
 * a `\Q` run's boundary or a stray `\E` - so that the caller reads the next
 * one rather than adding an item that is not there.
 */
static GRX_Result read_class_atom(GRX_Parser * parser, GRX_ClassItem * out,
    int * out_is_item, size_t * quote_end) {
  size_t start = parser->position;
  *out_is_item = 1;

  if (parser->position < *quote_end) {
    uint32_t quoted = 0;
    GRX_Result result = grx_parse_take(parser, &quoted);
    if (result != GRX_OK) {
      return result;
    }
    *out = (GRX_ClassItem) {
      .kind = GRX_CLASS_ITEM_SINGLE,
      .flags = 0,
      .lo = quoted,
      .hi = 0,
      .a = 0,
      .offset = start,
      .length = parser->position - start,
    };
    return GRX_OK;
  }

  if (byte_at(parser, 0) == '[') {
    int matched = 0;
    GRX_Result result = read_posix_class(parser, out, &matched);
    if (matched || result != GRX_OK) {
      return result;
    }
  }

  if (grx_parse_eat(parser, '\\')) {
    if (grx_parse_at_end(parser)) {
      return grx_parse_fail(parser, GRX_DIAG_TRAILING_BACKSLASH, start, 1);
    }
    if (byte_at(parser, 0) == 'Q') {
      parser->position++;
      size_t end = parser->length;
      for (size_t i = parser->position; i + 1 < parser->length; i++) {
        if (parser->text[i] == '\\' && parser->text[i + 1] == 'E') {
          end = i;
          break;
        }
      }
      *quote_end = end;
      *out_is_item = 0;
      return GRX_OK;
    }
    if (byte_at(parser, 0) == 'E') {
      parser->position++;
      *quote_end = 0;
      *out_is_item = 0;
      return GRX_OK;
    }
    return pcre_class_escape(parser, out);
  }

  uint32_t codepoint = 0;
  GRX_Result result = grx_parse_take(parser, &codepoint);
  if (result != GRX_OK) {
    return result;
  }

  *out = (GRX_ClassItem) {
    .kind = GRX_CLASS_ITEM_SINGLE,
    .flags = 0,
    .lo = codepoint,
    .hi = 0,
    .a = 0,
    .offset = start,
    .length = parser->position - start,
  };
  return GRX_OK;
}

/**
 * Consume what a bracket expression ignores between two items.
 *
 * `\Q` and `\E` are lexically invisible here in the same way they are
 * outside a class: `[\Qa\E-z]` is the range a to z, not three characters, so
 * the `\E` between the low end and the `-` has to disappear before the
 * range is decided rather than after.
 */
static void skip_class_ignorable(GRX_Parser * parser, size_t * quote_end) {
  for (;;) {
    if (parser->position < *quote_end) {
      return;
    }
    skip_class_space(parser);
    if (byte_at(parser, 0) == '\\' && byte_at(parser, 1) == 'Q') {
      parser->position += 2;
      size_t end = parser->length;
      for (size_t i = parser->position; i + 1 < parser->length; i++) {
        if (parser->text[i] == '\\' && parser->text[i + 1] == 'E') {
          end = i;
          break;
        }
      }
      *quote_end = end;
      if (end > parser->position) {
        return;
      }
      continue;
    }
    if (byte_at(parser, 0) == '\\' && byte_at(parser, 1) == 'E') {
      parser->position += 2;
      *quote_end = 0;
      continue;
    }
    return;
  }
}

static GRX_Result pcre_char_class(GRX_Parser * parser, uint32_t * out_node) {
  size_t start = parser->position - 1; // The `[`.
  size_t quote_end = 0;

  GRX_Result result = grx_parse_class_node(parser, start, out_node);
  if (result != GRX_OK) {
    return result;
  }

  // `[\E^]` is a negated class: the `\E` vanishes and the `^` is then the
  // first thing in the brackets. pcre2test reports that pattern as an
  // unterminated class, because the `]` after the caret is a member.
  skip_class_ignorable(parser, &quote_end);
  if (parser->position >= quote_end && grx_parse_eat(parser, '^')) {
    grx_pattern_node(parser->pattern, *out_node)->flags |= GRX_NODE_NEGATED;
  }

  // A `]` in the first position is a literal, which is the rule that makes
  // `[]` unterminated rather than empty: the `]` is a member and the class
  // runs on looking for another.
  int first = 1;

  for (;;) {
    skip_class_ignorable(parser, &quote_end);
    if (grx_parse_at_end(parser)) {
      return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_BRACKET, start, 1);
    }
    if (byte_at(parser, 0) == ']' && !first && parser->position >= quote_end) {
      break;
    }

    GRX_ClassItem low;
    int is_item = 0;
    result = read_class_atom(parser, &low, &is_item, &quote_end);
    if (result != GRX_OK) {
      return result;
    }
    if (!is_item) {
      continue;
    }
    first = 0;

    // A `-` before anything but `]` begins a range. Inside a quoted run it
    // does not: `[\Qa-z\E]` is three characters.
    size_t after_low = parser->position;
    if (parser->position >= quote_end) {
      skip_class_ignorable(parser, &quote_end);
    }
    if (parser->position >= quote_end && byte_at(parser, 0) == '-'
        && parser->position + 1 < parser->length) {
      parser->position++;
      skip_class_ignorable(parser, &quote_end);
      if (parser->position >= quote_end
          && (byte_at(parser, 0) == ']' || grx_parse_at_end(parser))) {
        // `[a-]`, and `[[:digit:]-   ]` under `xx` where the spaces are not
        // there: the `-` is the last member, not the start of a range.
        parser->position = after_low;
        result = grx_parse_class_add(parser, *out_node, &low);
        if (result != GRX_OK) {
          return result;
        }
        continue;
      }

      GRX_ClassItem high;
      int high_is_item = 0;
      result = read_class_atom(parser, &high, &high_is_item, &quote_end);
      if (result != GRX_OK) {
        return result;
      }
      if (!high_is_item || !is_single(&low) || !is_single(&high)) {
        // pcre2test: "invalid range in character class". Both ends must be
        // single characters, which is where PCRE2 and Perl differ - Perl
        // warns and takes the `-` as a literal.
        return grx_parse_fail(parser, GRX_DIAG_CLASS_ESCAPE_IN_RANGE,
            low.offset, parser->position - low.offset);
      }
      if (low.lo > high.lo) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_RANGE, low.offset,
            parser->position - low.offset);
      }

      low.kind = GRX_CLASS_ITEM_RANGE;
      low.hi = high.lo;
      low.length = parser->position - low.offset;
    }
    else {
      parser->position = after_low;
    }

    result = grx_parse_class_add(parser, *out_node, &low);
    if (result != GRX_OK) {
      return result;
    }
  }

  if (!grx_parse_eat(parser, ']')) {
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_BRACKET, start, 1);
  }

  grx_pattern_node(parser->pattern, *out_node)->length
      = parser->position - start;
  return GRX_OK;
}

// --------------------------------------------------------------------------
// Option letters
// --------------------------------------------------------------------------

/** The option a letter sets, or 0 when the letter is not one. */
static uint32_t option_for_letter(char c) {
  switch (c) {
    case 'i': return GRX_OPT_CASELESS;
    case 'm': return GRX_OPT_MULTILINE;
    case 's': return GRX_OPT_DOTALL;
    case 'x': return GRX_OPT_EXTENDED;
    case 'n': return GRX_OPT_NO_CAPTURE;
    case 'U': return GRX_OPT_UNGREEDY;
    case 'J': return GRX_OPT_DUPLICATE_NAMES;
    default: return 0;
  }
}

/**
 * Read the letters of an inline option setting, up to `:` or `)`.
 *
 * `(?i-m:...)`, `(?^i...)` and `(?xx)` are all this grammar. `^` means
 * "start from the dialect's defaults", which both references spell as
 * unsetting `imnsx` before applying what follows; `xx` is extended mode
 * widened to bracket expressions, so it sets a second bit as well as the
 * first.
 */
static GRX_Result read_option_letters(GRX_Parser * parser, size_t start,
    uint32_t * out_set, uint32_t * out_clear) {
  uint32_t set = 0;
  uint32_t clear = 0;
  int clearing = 0;

  int reset = grx_parse_eat(parser, '^');
  if (reset) {
    clear = GRX_OPT_CASELESS | GRX_OPT_MULTILINE | GRX_OPT_DOTALL
        | GRX_OPT_EXTENDED | GRX_OPT_EXTENDED_MORE | GRX_OPT_NO_CAPTURE;
  }

  for (;;) {
    char c = byte_at(parser, 0);
    if (c == ':' || c == ')') {
      break;
    }
    if (c == '-') {
      // pcre2test: "invalid hyphen in option setting". `^` has already
      // cleared everything, so a second way to clear would say nothing.
      if (clearing || reset) {
        return grx_parse_fail(parser, GRX_DIAG_UNKNOWN_FLAG,
            parser->position, 1);
      }
      clearing = 1;
      parser->position++;
      continue;
    }

    uint32_t option = option_for_letter(c);
    if (!option) {
      if (flavour(parser) == FLAVOUR_PERL && (c == 'p' || c == 'a' || c == 'd'
              || c == 'l' || c == 'u')) {
        // Perl's charset and preserve modifiers. `p` has no compile-time
        // effect; the other four choose a character-set semantics this
        // library does not have a second of.
        parser->position++;
        if (c == 'p') {
          continue;
        }
        return grx_parse_fail(parser, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED,
            parser->position - 1, 1);
      }
      return grx_parse_fail(parser, GRX_DIAG_UNKNOWN_FLAG, parser->position, 1);
    }

    parser->position++;
    if (option == GRX_OPT_EXTENDED && byte_at(parser, 0) == 'x') {
      parser->position++;
      option |= GRX_OPT_EXTENDED_MORE;
    }
    if (clearing) {
      clear |= option;
      if (option & GRX_OPT_EXTENDED) {
        clear |= GRX_OPT_EXTENDED_MORE;
      }
    }
    else {
      set |= option;
    }
  }

  if (grx_parse_at_end(parser)) {
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 1);
  }

  *out_set = set;
  *out_clear = clear;
  return GRX_OK;
}

// --------------------------------------------------------------------------
// The `(*...)` constructs: verbs, lookaround spellings and directives
// --------------------------------------------------------------------------

/** One `(*NAME)` the parser knows. */
typedef struct {
  const char * name;
  int verb;            ///< A GRX_VerbKind, or -1 when this is not a verb.
  int takes_argument;  ///< Whether `(*NAME:arg)` is allowed.
} VerbRow;

// Every row takes an argument. pcre2test 10.46 accepts `(*ACCEPT:X)` and
// `(*COMMIT:X)` as readily as `(*MARK:X)`, which the first draft of this
// table did not: it had the three verbs perlre describes as argument-less
// refusing one, and three corpus records said otherwise.
static const VerbRow verb_table[] = {
  {"ACCEPT", GRX_VERB_ACCEPT, 1},
  {"FAIL", GRX_VERB_FAIL, 1},
  {"F", GRX_VERB_FAIL, 1},
  {"COMMIT", GRX_VERB_COMMIT, 1},
  {"PRUNE", GRX_VERB_PRUNE, 1},
  {"SKIP", GRX_VERB_SKIP, 1},
  {"THEN", GRX_VERB_THEN, 1},
  {"MARK", -1, 1},
  {NULL, -1, 0},
};

/** One `(*name:` that is another spelling of a group this library has. */
typedef struct {
  const char * name;
  GRX_NodeKind kind;
  uint32_t a;
} AltGroupRow;

static const AltGroupRow alt_group_table[] = {
  {"atomic", GRX_NODE_GROUP, 0},
  {"pla", GRX_NODE_LOOKAROUND, GRX_LOOK_AHEAD_POSITIVE},
  {"positive_lookahead", GRX_NODE_LOOKAROUND, GRX_LOOK_AHEAD_POSITIVE},
  {"nla", GRX_NODE_LOOKAROUND, GRX_LOOK_AHEAD_NEGATIVE},
  {"negative_lookahead", GRX_NODE_LOOKAROUND, GRX_LOOK_AHEAD_NEGATIVE},
  {"plb", GRX_NODE_LOOKAROUND, GRX_LOOK_BEHIND_POSITIVE},
  {"positive_lookbehind", GRX_NODE_LOOKAROUND, GRX_LOOK_BEHIND_POSITIVE},
  {"nlb", GRX_NODE_LOOKAROUND, GRX_LOOK_BEHIND_NEGATIVE},
  {"negative_lookbehind", GRX_NODE_LOOKAROUND, GRX_LOOK_BEHIND_NEGATIVE},
  {NULL, GRX_NODE_GROUP, 0},
};

/**
 * The `(*...)` spellings this library refuses on purpose.
 *
 * Each constrains what its body may match in a way an ordinary group does
 * not, so reading one as a group would accept the pattern and answer a
 * different question. `napla` and `naplb` are the non-atomic lookarounds,
 * which differ from the ordinary ones only in what a `(*SKIP)` inside them
 * may do - a difference this engine cannot express until WP-19's verbs are
 * in, and one an ordinary lookaround would silently get wrong.
 */
static const char * const unsupported_star[] = {
  "script_run", "sr", "atomic_script_run", "asr", "scs", "scan_substring",
  "napla", "non_atomic_positive_lookahead",
  "naplb", "non_atomic_positive_lookbehind",
  NULL
};

/** Whether everything before `start` is leading `(*...)` directives. */
static int only_directives_before(const GRX_Parser * parser, size_t start) {
  size_t i = 0;
  while (i < start) {
    if (parser->text[i] != '(' || i + 1 >= start || parser->text[i + 1] != '*') {
      return 0;
    }
    size_t close = i + 2;
    while (close < parser->length && parser->text[close] != ')') {
      close++;
    }
    if (close >= parser->length) {
      return 0;
    }
    i = close + 1;
  }

  return i == start;
}

/**
 * Apply a leading `(*NAME)` or `(*NAME=value)` directive.
 *
 * Returns GRX_ERR_SYNTAX when the name is not a directive, so that the
 * caller can try the verb and lookaround tables instead.
 */
static GRX_Result apply_directive(GRX_Parser * parser, const char * name,
    size_t length, size_t start) {
  static const struct {
    const char * name;
    uint32_t option;
  } option_directives[] = {
    {"UTF", GRX_OPT_UTF},
    {"UCP", GRX_OPT_UCP},
    {NULL, 0},
  };
  // Directives that say something about how the *caller* will search, or
  // about an optimisation. Neither changes which subjects a pattern
  // matches, so both are accepted and have no effect here - and saying so
  // is not the same as ignoring a construct that does change the answer.
  static const char * const inert_directives[] = {
    "NO_AUTO_POSSESS", "NO_START_OPT", "NO_DOTSTAR_ANCHOR", "NO_JIT",
    "NOTEMPTY", "NOTEMPTY_ATSTART", "CR", "LF", "CRLF", "ANYCRLF", "ANY",
    "NUL", "BSR_ANYCRLF", "BSR_UNICODE", NULL
  };

  for (size_t i = 0; option_directives[i].name; i++) {
    if (strlen(option_directives[i].name) == length
        && memcmp(option_directives[i].name, name, length) == 0) {
      if (!only_directives_before(parser, start)) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
            parser->position - start);
      }
      // Both: the parse needs it now, and lowering reads the *pattern's*
      // copy rather than the caller's argument. Only a leading directive may
      // write that copy - an inline `(?i)` must not, or `ab(?i)cd` would
      // fold the "ab" as well, which is what happened when the two were
      // written back at the end of the parse instead of here.
      parser->options |= option_directives[i].option;
      parser->pattern->options |= option_directives[i].option;
      return GRX_OK;
    }
  }

  for (size_t i = 0; inert_directives[i]; i++) {
    if (strlen(inert_directives[i]) == length
        && memcmp(inert_directives[i], name, length) == 0) {
      if (!only_directives_before(parser, start)) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
            parser->position - start);
      }
      return GRX_OK;
    }
  }

  // `(*LIMIT_MATCH=d)` and kin. PCRE2 lets a pattern lower a limit and never
  // raise one, and that rule is the whole reason a pattern may set a limit
  // at all: a caller's cap is a policy and a pattern may not overrule it.
  static const char * const limit_names[]
      = {"LIMIT_MATCH", "LIMIT_DEPTH", "LIMIT_HEAP", NULL};
  for (size_t i = 0; limit_names[i]; i++) {
    size_t name_length = strlen(limit_names[i]);
    if (length <= name_length || memcmp(limit_names[i], name, name_length) != 0
        || name[name_length] != '=') {
      continue;
    }
    if (!only_directives_before(parser, start)) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
          parser->position - start);
    }
    for (size_t j = name_length + 1; j < length; j++) {
      if (!is_decimal(name[j])) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
            parser->position - start);
      }
    }
    if (length == name_length + 1) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
          parser->position - start);
    }
    // Accepted and not applied: `limits` is the caller's, and this front end
    // has no writable copy of it. Lowering a limit from inside the pattern
    // is documentation/plan.md WP-19's, where the limits are read.
    return GRX_OK;
  }

  return GRX_ERR_SYNTAX;
}

/** Read a `(*...)` construct, the `(` consumed and the `*` next. */
static GRX_Result read_star_construct(GRX_Parser * parser, size_t start,
    GRX_GroupOpen * out) {
  parser->position++; // The `*`.
  size_t first = parser->position;
  while (!grx_parse_at_end(parser) && byte_at(parser, 0) != ')'
      && byte_at(parser, 0) != ':') {
    parser->position++;
  }
  if (grx_parse_at_end(parser)) {
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 1);
  }

  const char * name = parser->text + first;
  size_t length = parser->position - first;
  int has_argument = byte_at(parser, 0) == ':';

  // `(*:name)` is `(*MARK:name)` written short.
  if (!length && has_argument) {
    name = "MARK";
    length = 4;
  }

  for (size_t i = 0; unsupported_star[i]; i++) {
    if (strlen(unsupported_star[i]) == length
        && memcmp(unsupported_star[i], name, length) == 0) {
      return grx_parse_fail(parser, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, start,
          parser->position - start + 1);
    }
  }

  for (size_t i = 0; alt_group_table[i].name; i++) {
    if (strlen(alt_group_table[i].name) != length
        || memcmp(alt_group_table[i].name, name, length) != 0) {
      continue;
    }
    if (!has_argument) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
          parser->position - start + 1);
    }
    parser->position++; // The `:`.
    out->kind = alt_group_table[i].kind;
    out->a = alt_group_table[i].a;
    out->flags = alt_group_table[i].kind == GRX_NODE_GROUP
        ? GRX_NODE_ATOMIC
        : 0;
    out->has_body = 1;
    return GRX_OK;
  }

  for (size_t i = 0; verb_table[i].name; i++) {
    if (strlen(verb_table[i].name) != length
        || memcmp(verb_table[i].name, name, length) != 0) {
      continue;
    }

    uint32_t argument = GRX_INDEX_NONE;
    if (has_argument) {
      if (!verb_table[i].takes_argument) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
            parser->position - start + 1);
      }
      parser->position++;
      size_t argument_first = parser->position;
      while (!grx_parse_at_end(parser) && byte_at(parser, 0) != ')') {
        parser->position++;
      }
      GRX_Result result = grx_pattern_add_name(parser->pattern,
          parser->text + argument_first, parser->position - argument_first,
          &argument);
      if (result != GRX_OK) {
        return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
      }
    }
    if (!grx_parse_eat(parser, ')')) {
      return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 1);
    }

    if (verb_table[i].verb < 0) {
      // `(*MARK:name)` names a position for `(*SKIP:name)` to return to and
      // for the caller to read back. Neither is expressible yet, and a mark
      // that is silently dropped would make `(*SKIP:x)` mean `(*SKIP)`.
      return grx_parse_fail(parser, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, start,
          parser->position - start);
    }
    out->kind = GRX_NODE_CONTROL;
    out->a = (uint32_t)verb_table[i].verb;
    out->b = argument;
    out->has_body = 0;
    return GRX_OK;
  }

  if (!has_argument) {
    GRX_Result result = apply_directive(parser, name, length, start);
    if (result != GRX_ERR_SYNTAX) {
      if (result != GRX_OK) {
        return result;
      }
      parser->position++; // The `)`.
      out->kind = GRX_NODE_EMPTY;
      out->has_body = 0;
      return GRX_OK;
    }
  }

  return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start,
      parser->position - start + 1);
}

// --------------------------------------------------------------------------
// Extended character classes: `(?[ ... ])`
// --------------------------------------------------------------------------

/**
 * Skip what an extended class ignores.
 *
 * Space and tab - a literal newline inside `(?[...])` is error 216 in
 * pcre2test, which is the same set `xx` ignores inside an ordinary bracket
 * expression, so the two rules agree. Plus a `\E` with no run open and an
 * empty `\Q\E`, both of which pcre2test lets stand between an operand and
 * an operator: `(?[\n \Q\E])` compiles. A `\Q` run with anything in it is
 * not ignorable and is left for read_extended_term() to refuse.
 */
static void skip_extended_ignorable(GRX_Parser * parser) {
  for (;;) {
    if (byte_at(parser, 0) == ' ' || byte_at(parser, 0) == '\t') {
      parser->position++;
      continue;
    }
    if (byte_at(parser, 0) == '\\' && byte_at(parser, 1) == 'E') {
      parser->position += 2;
      continue;
    }
    if (byte_at(parser, 0) == '\\' && byte_at(parser, 1) == 'Q'
        && byte_at(parser, 2) == '\\' && byte_at(parser, 3) == 'E') {
      parser->position += 4;
      continue;
    }
    return;
  }
}

/**
 * How deep `(?[...])` may nest, the `(?[` itself counting as the first.
 *
 * A bracket expression counts as a level too, which is how the corpus
 * settles the number: `(?[` with fourteen nested parentheses compiles, and
 * the same pattern with one `[\n]` at the bottom does not. Transcribed
 * rather than chosen, the way the 65535 repeat bound is, because a cap this
 * library picked for itself would refuse patterns the reference accepts.
 */
#define GRX_PCRE_ECLASS_NEST_MAX 15

static GRX_Result read_extended_expression(
    GRX_Parser * parser, unsigned depth, uint32_t * out_node);

/**
 * Build a class-operation node over one operand already read.
 *
 * The node takes the span from `start` so that a diagnostic underlines the
 * operator and both of its operands rather than the second alone.
 */
static GRX_Result class_op_node(GRX_Parser * parser, GRX_ClassOpKind op,
    size_t start, uint32_t first, uint32_t * out_node) {
  GRX_Result result = plain_node(
      parser, GRX_NODE_CLASS_OP, start, parser->position - start, out_node);
  if (result != GRX_OK) {
    return result;
  }
  grx_pattern_node(parser->pattern, *out_node)->a = (uint32_t)op;
  if (grx_pattern_add_child(parser->pattern, *out_node, first) != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 0);
  }

  return GRX_OK;
}

/**
 * Wrap a term in a complement, if an odd number of `!` asked for one.
 *
 * An even number asks for none: complementing a set twice over a fixed
 * universe gives the set back, so `!![a]` is `[a]`. Collapsing them here
 * rather than building one node per `!` keeps a pattern of a thousand from
 * becoming a tree a thousand deep, which lowering would have to walk.
 */
static GRX_Result complement_wrap(GRX_Parser * parser, unsigned complements,
    size_t start, uint32_t * out_node) {
  if (!(complements & 1)) {
    return GRX_OK;
  }

  uint32_t operand = *out_node;
  return class_op_node(
      parser, GRX_CLASS_OP_COMPLEMENT, start, operand, out_node);
}

/**
 * Read one operand of an extended class.
 *
 * Five things can stand here, and a bare character is not one of them:
 * pcre2test reports `(?[a])` as "unexpected character in (?[...]) extended
 * character class", because an operand has to be a *set* and `a` is a
 * character. `[a]` is how that set is written.
 */
static GRX_Result read_extended_term(
    GRX_Parser * parser, unsigned depth, uint32_t * out_node) {
  skip_extended_ignorable(parser);
  size_t start = parser->position;

  // Unary complement, and it binds tighter than every binary operator:
  // `! [a] & [ab]` is `(!a) & ab` and matches "b". Counted rather than
  // recursed on, and *not* against the nesting cap: pcre2test compiles a
  // hundred of them, so a reader that recursed once per `!` would be a
  // reader a pattern can overflow.
  unsigned complements = 0;
  for (;;) {
    skip_extended_ignorable(parser);
    if (byte_at(parser, 0) != '!') {
      break;
    }
    parser->position++;
    complements++;
  }

  if (grx_parse_at_end(parser)) {
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_BRACKET, start, 1);
  }

  char c = byte_at(parser, 0);

  if (c == '(') {
    parser->position++;
    if (depth + 1 > GRX_PCRE_ECLASS_NEST_MAX) {
      return grx_parse_fail(
          parser, GRX_DIAG_CLASS_NESTING_TOO_DEEP, start, 1);
    }
    GRX_Result result = read_extended_expression(parser, depth + 1, out_node);
    if (result != GRX_OK) {
      return result;
    }
    skip_extended_ignorable(parser);
    if (!grx_parse_eat(parser, ')')) {
      return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 1);
    }
    return complement_wrap(parser, complements, start, out_node);
  }

  if (c == '[') {
    if (depth + 1 > GRX_PCRE_ECLASS_NEST_MAX) {
      return grx_parse_fail(
          parser, GRX_DIAG_CLASS_NESTING_TOO_DEEP, start, 1);
    }
    GRX_ClassItem item;
    int matched = 0;
    GRX_Result result = read_posix_class(parser, &item, &matched);
    if (result != GRX_OK) {
      return result;
    }
    if (matched) {
      // `[:alpha:]` stands alone here, where inside brackets it would need a
      // second pair around it.
      result = item_as_node(parser, &item, out_node);
      if (result != GRX_OK) {
        return result;
      }
      return complement_wrap(parser, complements, start, out_node);
    }

    // An ordinary bracket expression, read by the ordinary reader - with the
    // whitespace rule `xx` gives it, because `(?[ [ a ] ])` does not match a
    // space in pcre2test. The flag is restored afterwards: what is inside
    // the brackets is the only place it applies.
    uint32_t outer = parser->options;
    parser->options |= GRX_OPT_EXTENDED_MORE;
    parser->position++;
    result = pcre_char_class(parser, out_node);
    parser->options = outer;
    if (result != GRX_OK) {
      return result;
    }
    return complement_wrap(parser, complements, start, out_node);
  }

  if (c == '\\') {
    if (byte_at(parser, 1) == 'Q') {
      // The empty run is ignorable and never arrives here. A run with
      // anything in it is a sequence of characters, and a sequence is not a
      // set: pcre2test refuses `(?[ \Qab\E ])`.
      return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_ITEM, start, 2);
    }

    parser->position++;
    GRX_ClassItem item;
    GRX_Result result = pcre_class_escape(parser, &item);
    if (result != GRX_OK) {
      return result;
    }
    result = item_as_node(parser, &item, out_node);
    if (result != GRX_OK) {
      return result;
    }
    return complement_wrap(parser, complements, start, out_node);
  }

  return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_ITEM, start, 1);
}

/**
 * The operator a character spells, or zero when it is not one.
 *
 * `|` and `+` are both union, which is PCRE2's spelling and not a synonym
 * this library invented.
 */
static int extended_operator(char c, GRX_ClassOpKind * out_op) {
  switch (c) {
    case '|': case '+': *out_op = GRX_CLASS_OP_UNION; return 1;
    case '&': *out_op = GRX_CLASS_OP_INTERSECT; return 1;
    case '-': *out_op = GRX_CLASS_OP_SUBTRACT; return 1;
    case '^': *out_op = GRX_CLASS_OP_SYMDIFF; return 1;
    default: return 0;
  }
}

/**
 * Read a run of terms joined by `&`, which binds tighter than the rest.
 *
 * `[a] | [b] & [b]` matches "a" and "b" in pcre2test, so the intersection
 * happens first. Everything else is one level below this and left to
 * read_extended_expression().
 */
static GRX_Result read_extended_intersection(
    GRX_Parser * parser, unsigned depth, uint32_t * out_node) {
  size_t start = parser->position;
  GRX_Result result = read_extended_term(parser, depth, out_node);
  if (result != GRX_OK) {
    return result;
  }

  for (;;) {
    skip_extended_ignorable(parser);
    if (byte_at(parser, 0) != '&') {
      return GRX_OK;
    }
    parser->position++;

    uint32_t right = GRX_INDEX_NONE;
    result = read_extended_term(parser, depth, &right);
    if (result != GRX_OK) {
      return result;
    }

    uint32_t combined = GRX_INDEX_NONE;
    result = class_op_node(
        parser, GRX_CLASS_OP_INTERSECT, start, *out_node, &combined);
    if (result != GRX_OK) {
      return result;
    }
    if (grx_pattern_add_child(parser->pattern, combined, right) != GRX_OK) {
      return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 0);
    }
    *out_node = combined;
  }
}

/**
 * Read a whole extended-class expression.
 *
 * `|`, `+`, `-` and `^` are one precedence level and associate to the left:
 * `[abc] - [a] | [a]` is `((abc - a) | a)` and `[a] | [abc] - [a]` is
 * `((a | abc) - a)`, which is why the two answer differently for "a".
 */
static GRX_Result read_extended_expression(
    GRX_Parser * parser, unsigned depth, uint32_t * out_node) {
  size_t start = parser->position;

  skip_extended_ignorable(parser);
  GRX_ClassOpKind leading = GRX_CLASS_OP_UNION;
  if (extended_operator(byte_at(parser, 0), &leading)) {
    // pcre2test: "unexpected operator in extended character class (no
    // preceding operand)".
    return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_SET_OP,
        parser->position, 1);
  }

  GRX_Result result = read_extended_intersection(parser, depth, out_node);
  if (result != GRX_OK) {
    return result;
  }

  for (;;) {
    skip_extended_ignorable(parser);
    GRX_ClassOpKind op = GRX_CLASS_OP_UNION;
    if (!extended_operator(byte_at(parser, 0), &op)) {
      if (byte_at(parser, 0) == ']' || byte_at(parser, 0) == ')'
          || grx_parse_at_end(parser)) {
        return GRX_OK;
      }
      // Two operands with nothing between them. pcre2test: "unexpected
      // expression in extended character class (no preceding operator)".
      return grx_parse_fail(parser, GRX_DIAG_INVALID_CLASS_SET_OP,
          parser->position, 1);
    }
    parser->position++;

    uint32_t right = GRX_INDEX_NONE;
    result = read_extended_intersection(parser, depth, &right);
    if (result != GRX_OK) {
      return result;
    }

    uint32_t combined = GRX_INDEX_NONE;
    result = class_op_node(parser, op, start, *out_node, &combined);
    if (result != GRX_OK) {
      return result;
    }
    if (grx_pattern_add_child(parser->pattern, combined, right) != GRX_OK) {
      return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 0);
    }
    *out_node = combined;
  }
}

/**
 * Read the body of `(?[...])`, the `(?[` already consumed.
 *
 * Reached through GRX_GroupOpen::read_body, so the parser owns the closing
 * `)` and the nesting depth; this owns everything up to and including the
 * `]`. The node it attaches to is a union of one operand, which is the
 * expression - a wrapper rather than the expression itself, because the
 * parser creates the node before the operator is known.
 */
static GRX_Result read_extended_class_body(
    GRX_Parser * parser, uint32_t node) {
  size_t start = parser->position;

  skip_extended_ignorable(parser);
  if (byte_at(parser, 0) == ']') {
    // pcre2test: "empty expression in extended character class".
    return grx_parse_fail(parser, GRX_DIAG_EMPTY_CLASS, start, 1);
  }

  uint32_t expression = GRX_INDEX_NONE;
  GRX_Result result = read_extended_expression(parser, 1, &expression);
  if (result != GRX_OK) {
    return result;
  }

  skip_extended_ignorable(parser);
  if (!grx_parse_eat(parser, ']')) {
    return grx_parse_fail(
        parser, GRX_DIAG_UNMATCHED_OPEN_BRACKET, start - 1, 1);
  }
  if (grx_pattern_add_child(parser->pattern, node, expression) != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 0);
  }

  return GRX_OK;
}

// --------------------------------------------------------------------------
// Conditionals and branch resets: bodies that are not one alternation
// --------------------------------------------------------------------------

static GRX_Result pcre_group_open(GRX_Parser * parser, GRX_GroupOpen * out);
static GRX_Result pcre_skip_ignorable(GRX_Parser * parser);

/**
 * Read a complete `(...)` group as one node.
 *
 * Only a conditional's assertion needs this: its condition is a group that
 * stands where an atom cannot, so the shared grammar never reaches it. The
 * depth accounting is repeated here because the parser's own copy runs in
 * parse_atom(), which this path does not go through.
 */
static GRX_Result read_group_atom(GRX_Parser * parser, uint32_t * out_node) {
  size_t start = parser->position;
  GRX_GroupOpen open = {GRX_NODE_GROUP, 0, 0, GRX_INDEX_NONE, 1, NULL};

  // `(?(?C9)(?=a)b|c)` puts callouts before the condition, and `(?(?#x)(?=a)`
  // puts a comment there. Both are inert, and both are skipped here rather
  // than being made children of a conditional that has no place for them.
  for (;;) {
    GRX_Result skipped = pcre_skip_ignorable(parser);
    if (skipped != GRX_OK) {
      return skipped;
    }
    start = parser->position;
    if (!grx_parse_eat(parser, '(')) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_CONDITION, start, 1);
    }
    GRX_Result result = pcre_group_open(parser, &open);
    if (result != GRX_OK) {
      return result;
    }
    if (open.kind == GRX_NODE_EMPTY && !open.has_body) {
      continue;
    }
    break;
  }

  if (open.kind != GRX_NODE_LOOKAROUND) {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_CONDITION, start, parser->position - start);
  }

  GRX_Result result;

  result = grx_pattern_add_node(
      parser->pattern, open.kind, start, 0, out_node);
  if (result != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_OUT_OF_MEMORY, start, 0);
  }
  GRX_Node * node = grx_pattern_node(parser->pattern, *out_node);
  node->flags = open.flags;
  node->a = open.a;
  node->b = open.b;

  if (parser->limits->max_nesting_depth
      && parser->depth + 1 > parser->limits->max_nesting_depth) {
    return grx_parse_fail(parser, GRX_DIAG_LIMIT_NESTING_DEPTH, start, 1);
  }
  parser->depth++;
  int outer_lookbehind = parser->in_lookbehind;
  int outer_lookaround = parser->in_lookaround;
  parser->in_lookaround = 1;
  if (open.a == GRX_LOOK_BEHIND_POSITIVE || open.a == GRX_LOOK_BEHIND_NEGATIVE) {
    parser->in_lookbehind = 1;
  }

  uint32_t body = GRX_INDEX_NONE;
  result = grx_parse_alternation(parser, &body);
  parser->depth--;
  parser->in_lookbehind = outer_lookbehind;
  parser->in_lookaround = outer_lookaround;
  if (result != GRX_OK) {
    return result;
  }
  if (grx_pattern_add_child(parser->pattern, *out_node, body) != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 0);
  }
  if (!grx_parse_eat(parser, ')')) {
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 1);
  }

  grx_pattern_node(parser->pattern, *out_node)->length
      = parser->position - start;
  return GRX_OK;
}

/**
 * Read a conditional's body: the condition where there is one, then at most
 * two branches.
 *
 * "At most two" is the rule this exists for. `(?(1)a|b|c)` is an error in
 * both references, and reading the body as an alternation would make it a
 * conditional whose else-branch is `b|c` - a pattern that matches strings
 * neither reference would match.
 */
static GRX_Result read_conditional_body(GRX_Parser * parser, uint32_t node) {
  const GRX_Node * conditional = grx_pattern_node(parser->pattern, node);
  size_t start = conditional->offset;
  int assertion = conditional->a == (uint32_t)GRX_COND_ASSERTION;
  int define = conditional->a == (uint32_t)GRX_COND_DEFINE;

  if (assertion) {
    uint32_t condition = GRX_INDEX_NONE;
    GRX_Result result = read_group_atom(parser, &condition);
    if (result != GRX_OK) {
      return result;
    }
    if (grx_pattern_add_child(parser->pattern, node, condition) != GRX_OK) {
      return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 0);
    }
  }

  uint32_t branch = GRX_INDEX_NONE;
  GRX_Result result = grx_parse_concatenation(parser, &branch);
  if (result != GRX_OK) {
    return result;
  }
  if (grx_pattern_add_child(parser->pattern, node, branch) != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 0);
  }

  if (!grx_parse_eat(parser, '|')) {
    return GRX_OK;
  }
  if (define) {
    // `(?(DEFINE)...)` never runs, so a second branch is unreachable text.
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_CONDITION, start, parser->position - start);
  }

  uint32_t otherwise = GRX_INDEX_NONE;
  result = grx_parse_concatenation(parser, &otherwise);
  if (result != GRX_OK) {
    return result;
  }
  if (grx_pattern_add_child(parser->pattern, node, otherwise) != GRX_OK) {
    return grx_parse_fail(parser, GRX_DIAG_INTERNAL, start, 0);
  }
  grx_pattern_node(parser->pattern, node)->flags |= GRX_NODE_HAS_ELSE;

  if (!grx_parse_at_end(parser) && byte_at(parser, 0) == '|') {
    return grx_parse_fail(
        parser, GRX_DIAG_INVALID_CONDITION, start, parser->position - start);
  }

  return GRX_OK;
}

/**
 * Read a branch reset's body: every branch numbers its groups from the same
 * base.
 *
 * `(?|(a)|(b)|(c))` has one capturing group, not three, and the count the
 * pattern ends with is the largest any branch reached - which is why the
 * running total is saved, reset per branch, and raised rather than summed.
 */
static GRX_Result read_branch_reset_body(GRX_Parser * parser, uint32_t node) {
  size_t base = parser->groups_opened;
  size_t highest = base;

  // `(?|(?'a'x)|(?'a'y))` names one group twice because it *is* one group,
  // and pcre2test accepts it without `(?J)`. Set rather than checked around,
  // because parse_atom() restores the options at the closing `)` - so the
  // relaxation ends exactly where the branch reset does.
  parser->options |= GRX_OPT_DUPLICATE_NAMES;

  for (;;) {
    parser->groups_opened = base;
    uint32_t branch = GRX_INDEX_NONE;
    GRX_Result result = grx_parse_concatenation(parser, &branch);
    if (result != GRX_OK) {
      return result;
    }
    if (grx_pattern_add_child(parser->pattern, node, branch) != GRX_OK) {
      return grx_parse_fail(parser, GRX_DIAG_INTERNAL,
          grx_pattern_node(parser->pattern, node)->offset, 0);
    }
    if (parser->groups_opened > highest) {
      highest = parser->groups_opened;
    }
    if (!grx_parse_eat(parser, '|')) {
      break;
    }
  }

  parser->groups_opened = highest;
  return GRX_OK;
}

// --------------------------------------------------------------------------
// Groups
// --------------------------------------------------------------------------

/** Read the condition of `(?(...)`, the `(?(` already consumed. */
static GRX_Result read_condition(GRX_Parser * parser, size_t start,
    GRX_GroupOpen * out) {
  char c = byte_at(parser, 0);

  if (looking_at(parser, "DEFINE)")) {
    parser->position += 6;
    out->a = (uint32_t)GRX_COND_DEFINE;
    return GRX_OK;
  }

  if (looking_at(parser, "VERSION")) {
    // `(?(VERSION>=10.46)...)`. Answered here: this library is not PCRE2 and
    // reports the version it emulates, which documentation/dialects.md
    // section 9 names.
    parser->position += 7;
    int at_least = grx_parse_eat(parser, '>');
    if (!grx_parse_eat(parser, '=')) {
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_CONDITION, start, parser->position - start);
    }
    unsigned major = 0;
    unsigned minor = 0;
    int digits = 0;
    while (is_decimal(byte_at(parser, 0))) {
      major = major * 10 + (unsigned)(byte_at(parser, 0) - '0');
      parser->position++;
      digits++;
    }
    if (grx_parse_eat(parser, '.')) {
      while (is_decimal(byte_at(parser, 0))) {
        minor = minor * 10 + (unsigned)(byte_at(parser, 0) - '0');
        parser->position++;
        digits++;
      }
    }
    if (!digits || minor > 99 || byte_at(parser, 0) != ')') {
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_CONDITION, start, parser->position - start);
    }
    unsigned asked = major * 100 + minor;
    unsigned have = 10 * 100 + 46;
    out->a = (uint32_t)GRX_COND_STATIC;
    out->b = at_least ? (asked <= have) : (asked == have);
    return GRX_OK;
  }

  if (c == 'R') {
    parser->position++;
    if (byte_at(parser, 0) == ')') {
      out->a = (uint32_t)GRX_COND_RECURSION_ANY;
      return GRX_OK;
    }
    if (byte_at(parser, 0) == '&') {
      parser->position++;
      uint32_t offset = GRX_INDEX_NONE;
      GRX_Result result = read_name(parser, ')', start, &offset);
      if (result != GRX_OK) {
        return result;
      }
      parser->position--; // read_name() ate the `)`; the caller wants it.
      out->a = (uint32_t)GRX_COND_RECURSION_GROUP;
      out->b = offset;
      out->flags |= GRX_NODE_NAMED;
      return GRX_OK;
    }
    if (is_decimal(byte_at(parser, 0))) {
      uint64_t value = 0;
      while (is_decimal(byte_at(parser, 0)) && value < 0x7FFFFFFFu) {
        value = value * 10 + (uint64_t)(byte_at(parser, 0) - '0');
        parser->position++;
      }
      if (byte_at(parser, 0) != ')' || value > (uint64_t)parser->group_count) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_CONDITION, start,
            parser->position - start);
      }
      out->a = (uint32_t)GRX_COND_RECURSION_GROUP;
      out->b = (uint32_t)value;
      return GRX_OK;
    }
    // `(?(R` followed by anything else is a group named `R...`, which the
    // name branch below reads. Rewind so that it sees the `R`.
    parser->position--;
  }

  out->a = (uint32_t)GRX_COND_GROUP_SET;

  if (c == '<' || c == '\'') {
    parser->position++;
    uint32_t offset = GRX_INDEX_NONE;
    GRX_Result result
        = read_name(parser, c == '<' ? '>' : '\'', start, &offset);
    if (result != GRX_OK) {
      return result;
    }
    out->b = offset;
    out->flags |= GRX_NODE_NAMED;
    return GRX_OK;
  }

  if (c == '+' || c == '-' || is_decimal(c)) {
    int sign = c == '+' ? 1 : (c == '-' ? -1 : 0);
    if (sign) {
      parser->position++;
    }
    if (!is_decimal(byte_at(parser, 0))) {
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_CONDITION, start, parser->position - start);
    }
    uint64_t value = 0;
    while (is_decimal(byte_at(parser, 0)) && value < 0x7FFFFFFFu) {
      value = value * 10 + (uint64_t)(byte_at(parser, 0) - '0');
      parser->position++;
    }
    if (byte_at(parser, 0) != ')') {
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_CONDITION, start, parser->position - start);
    }
    if (sign) {
      uint32_t group = 0;
      GRX_Result result
          = resolve_relative(parser, sign, value, start, &group);
      if (result != GRX_OK) {
        return result;
      }
      out->b = group;
      out->flags |= GRX_NODE_RELATIVE;
      return GRX_OK;
    }
    if (!value || value > (uint64_t)parser->group_count) {
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_CONDITION, start, parser->position - start);
    }
    out->b = (uint32_t)value;
    return GRX_OK;
  }

  // A bare name: `(?(name)...)`.
  uint32_t offset = GRX_INDEX_NONE;
  GRX_Result result = read_name(parser, ')', start, &offset);
  if (result != GRX_OK) {
    return result;
  }
  parser->position--;
  out->b = offset;
  out->flags |= GRX_NODE_NAMED;
  return GRX_OK;
}

/** Read a recursion or subroutine call spelled `(?...)`, and its `)`. */
static GRX_Result read_recursion(GRX_Parser * parser, size_t start,
    GRX_GroupOpen * out) {
  out->kind = GRX_NODE_RECURSE;
  out->has_body = 0;
  out->a = 0;
  out->b = GRX_INDEX_NONE;

  char c = byte_at(parser, 0);
  if (c == 'R') {
    parser->position++;
  }
  else if (c == '&') {
    parser->position++;
    uint32_t offset = GRX_INDEX_NONE;
    GRX_Result result = read_name(parser, ')', start, &offset);
    if (result != GRX_OK) {
      return result;
    }
    out->b = offset;
    out->flags |= GRX_NODE_NAMED;
    return GRX_OK;
  }
  else if (c == 'P' && byte_at(parser, 1) == '>') {
    parser->position += 2;
    uint32_t offset = GRX_INDEX_NONE;
    GRX_Result result = read_name(parser, ')', start, &offset);
    if (result != GRX_OK) {
      return result;
    }
    out->b = offset;
    out->flags |= GRX_NODE_NAMED;
    return GRX_OK;
  }
  else {
    int sign = c == '+' ? 1 : (c == '-' ? -1 : 0);
    if (sign) {
      parser->position++;
    }
    uint64_t value = 0;
    int digits = 0;
    while (is_decimal(byte_at(parser, 0)) && value < 0x7FFFFFFFu) {
      value = value * 10 + (uint64_t)(byte_at(parser, 0) - '0');
      parser->position++;
      digits++;
    }
    if (!digits) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_RECURSION, start,
          parser->position - start);
    }
    if (sign) {
      GRX_Result result = resolve_relative(parser, sign, value, start, &out->a);
      if (result != GRX_OK) {
        return result;
      }
      out->flags |= GRX_NODE_RELATIVE;
    }
    else {
      if (value > (uint64_t)parser->group_count) {
        return grx_parse_fail(parser, GRX_DIAG_INVALID_RECURSION, start,
            parser->position - start);
      }
      out->a = (uint32_t)value;
    }
  }

  if (!grx_parse_eat(parser, ')')) {
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 1);
  }

  return GRX_OK;
}

static GRX_Result pcre_group_open(GRX_Parser * parser, GRX_GroupOpen * out) {
  size_t start = parser->position - 1; // The `(`.

  *out = (GRX_GroupOpen) {
    .kind = GRX_NODE_GROUP,
    .flags = 0,
    .a = 0,
    .b = GRX_INDEX_NONE,
    .has_body = 1,
    .read_body = NULL,
  };

  if (byte_at(parser, 0) == '*') {
    return read_star_construct(parser, start, out);
  }

  if (!grx_parse_eat(parser, '?')) {
    if (parser->options & GRX_OPT_NO_CAPTURE) {
      return GRX_OK;
    }
    parser->groups_opened++;
    out->flags = GRX_NODE_CAPTURING;
    out->a = (uint32_t)parser->groups_opened;
    return GRX_OK;
  }

  char c = byte_at(parser, 0);

  if (c == ':') {
    parser->position++;
    return GRX_OK;
  }

  if (c == '>') {
    parser->position++;
    out->flags = GRX_NODE_ATOMIC;
    return GRX_OK;
  }

  if (c == '[' && (parser->spec.features & GRX_FEATURE_CLASS_SET_OPS)) {
    // An extended class is an atom, not a group, and the node the parser is
    // about to build is a union of the one expression inside it - the
    // operator is not known until the expression has been read, and the node
    // exists before that.
    parser->position++;
    out->kind = GRX_NODE_CLASS_OP;
    out->a = (uint32_t)GRX_CLASS_OP_UNION;
    out->b = 0;
    out->read_body = read_extended_class_body;
    return GRX_OK;
  }

  if (c == '|') {
    parser->position++;
    out->kind = GRX_NODE_BRANCH_RESET;
    out->read_body = read_branch_reset_body;
    return GRX_OK;
  }

  if (c == '=' || c == '!') {
    parser->position++;
    out->kind = GRX_NODE_LOOKAROUND;
    out->a = c == '=' ? GRX_LOOK_AHEAD_POSITIVE : GRX_LOOK_AHEAD_NEGATIVE;
    return GRX_OK;
  }

  if (c == '*' || (c == '<' && byte_at(parser, 1) == '*')) {
    // `(?*` and `(?<*` are `(*napla:` and `(*naplb:` written short, and are
    // refused for the reason the long spellings are. Before the named-group
    // branch, which would otherwise read `(?<*` as a name beginning `*`.
    return grx_parse_fail(
        parser, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, start, 3);
  }

  if (c == '<' && (byte_at(parser, 1) == '=' || byte_at(parser, 1) == '!')) {
    int positive = byte_at(parser, 1) == '=';
    parser->position += 2;
    out->kind = GRX_NODE_LOOKAROUND;
    out->a = positive ? GRX_LOOK_BEHIND_POSITIVE : GRX_LOOK_BEHIND_NEGATIVE;
    return GRX_OK;
  }

  if (c == '(') {
    out->kind = GRX_NODE_CONDITIONAL;
    out->read_body = read_conditional_body;

    // `(?(?=a)b|c)` shares one parenthesis between the conditional and the
    // assertion: the `(` this hook is looking at opens both. It is left
    // where it is when the condition is an assertion, so that
    // read_conditional_body() reads the whole `(?=a)` as the group it is.
    if (byte_at(parser, 1) == '?' || byte_at(parser, 1) == '*') {
      out->a = (uint32_t)GRX_COND_ASSERTION;
      return GRX_OK;
    }

    parser->position++;
    GRX_Result result = read_condition(parser, start, out);
    if (result != GRX_OK) {
      return result;
    }
    if (!grx_parse_eat(parser, ')')) {
      return grx_parse_fail(
          parser, GRX_DIAG_INVALID_CONDITION, start, parser->position - start);
    }
    return GRX_OK;
  }

  if (c == 'R' || c == '&' || c == '+' || is_decimal(c)
      || (c == 'P' && byte_at(parser, 1) == '>')
      || (c == '-' && is_decimal(byte_at(parser, 1)))) {
    return read_recursion(parser, start, out);
  }

  if (c == 'P' && byte_at(parser, 1) == '=') {
    parser->position += 2;
    uint32_t offset = GRX_INDEX_NONE;
    GRX_Result result = read_name(parser, ')', start, &offset);
    if (result != GRX_OK) {
      return result;
    }
    out->kind = GRX_NODE_BACKREF;
    out->flags = GRX_NODE_NAMED;
    out->a = 0;
    out->b = offset;
    out->has_body = 0;
    return GRX_OK;
  }

  if (c == '<' || c == '\'' || (c == 'P' && byte_at(parser, 1) == '<')) {
    char terminator = '>';
    if (c == 'P') {
      parser->position += 2;
    }
    else {
      terminator = c == '<' ? '>' : '\'';
      parser->position++;
    }

    uint32_t offset = GRX_INDEX_NONE;
    GRX_Result result = read_name(parser, terminator, start, &offset);
    if (result != GRX_OK) {
      return result;
    }
    if (!(parser->options & GRX_OPT_DUPLICATE_NAMES)
        && name_already_used(parser, offset)) {
      return grx_parse_fail(parser, GRX_DIAG_DUPLICATE_GROUP_NAME, start,
          parser->position - start);
    }

    parser->groups_opened++;
    out->flags = GRX_NODE_CAPTURING | GRX_NODE_NAMED;
    out->a = (uint32_t)parser->groups_opened;
    out->b = offset;
    return GRX_OK;
  }

  if (c == 'C') {
    // A callout. It reports a position to a caller that has registered a
    // function, and this library has no such API - so it matches the same
    // subjects with the callout as without, and accepting it changes no
    // answer. Recorded in documentation/dialects.md section 6.
    parser->position++;
    if (byte_at(parser, 0) == '`' || byte_at(parser, 0) == '\''
        || byte_at(parser, 0) == '"' || byte_at(parser, 0) == '^'
        || byte_at(parser, 0) == '%' || byte_at(parser, 0) == '#'
        || byte_at(parser, 0) == '$' || byte_at(parser, 0) == '{') {
      char opener = byte_at(parser, 0);
      char closer = opener == '`' ? '`'
          : opener == '\''       ? '\''
          : opener == '"'        ? '"'
          : opener == '{'        ? '}'
                                 : opener;
      parser->position++;
      while (!grx_parse_at_end(parser) && byte_at(parser, 0) != closer) {
        parser->position++;
      }
      if (grx_parse_at_end(parser)) {
        return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 1);
      }
      parser->position++;
    }
    else {
      while (is_decimal(byte_at(parser, 0))) {
        parser->position++;
      }
    }
    if (!grx_parse_eat(parser, ')')) {
      return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 1);
    }
    out->kind = GRX_NODE_EMPTY;
    out->has_body = 0;
    return GRX_OK;
  }

  if (c == '{' || (c == '?' && byte_at(parser, 1) == '{')) {
    // Perl's embedded code, `(?{...})` and `(??{...})`. Refused with its own
    // diagnostic rather than GRX_DIAG_INVALID_GROUP_SYNTAX: the construct is
    // real and this library will never have it, which is not the same as a
    // malformed group.
    //
    // PCRE2 is the other way round. It has no embedded code at all, so `(?{`
    // is not a construct it declines to run - it is a `{` after `(?`, which
    // pcre2test reports as "unrecognized character after (? or (?-". Saying
    // "not implemented yet" there would promise a construct the dialect does
    // not have.
    if (flavour(parser) == FLAVOUR_PCRE) {
      return grx_parse_fail(parser, GRX_DIAG_INVALID_GROUP_SYNTAX, start, 3);
    }
    return grx_parse_fail(
        parser, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, start, 3);
  }

  // What is left is an option setting: `(?i)`, `(?i:...)`, `(?^x)`, `(?-i:)`.
  uint32_t set = 0;
  uint32_t clear = 0;
  GRX_Result result = read_option_letters(parser, start, &set, &clear);
  if (result != GRX_OK) {
    return result;
  }

  out->kind = GRX_NODE_OPTIONS;
  out->a = set;
  out->b = clear;
  parser->options = (parser->options | set) & ~clear;

  if (grx_parse_eat(parser, ':')) {
    out->flags = GRX_NODE_SCOPED;
    out->has_body = 1;
    return GRX_OK;
  }

  // `(?i)` applies to the rest of the enclosing group, which parse_atom()
  // bounds by saving and restoring the options across a body. There is no
  // body here and no `)` for the parser to eat, so both are this hook's.
  if (!grx_parse_eat(parser, ')')) {
    return grx_parse_fail(parser, GRX_DIAG_UNMATCHED_OPEN_PAREN, start, 1);
  }
  out->has_body = 0;
  return GRX_OK;
}

// --------------------------------------------------------------------------
// Quantifiers, literals and what extended mode drops
// --------------------------------------------------------------------------

/**
 * Read `{m,n}`, the brace already consumed.
 *
 * A `{` that does not begin a valid quantifier is an ordinary character in
 * both references, so `a{` matches "a{" and `x{,}` matches "x{,}". `{,n}`
 * *is* a quantifier - Perl 5.34 and PCRE2 10.43 made it `{0,n}`, and before
 * that it was a literal, which is the kind of change a corpus catches and a
 * reading of the documentation does not.
 */
/** PCRE2's own cap on a `{}` bound; `z{65536}` is "number too big". */
#define GRX_PCRE_REPEAT_MAX 65535

static GRX_Result check_repeat_bound(
    GRX_Parser * parser, uint64_t min, uint64_t max) {
  if (min > GRX_PCRE_REPEAT_MAX || max > GRX_PCRE_REPEAT_MAX) {
    return grx_parse_fail(parser, GRX_DIAG_LIMIT_REPEAT_COUNT,
        parser->position, 0);
  }

  return GRX_OK;
}

/** Step past the space a quantifier may hold: `{ 3 , 4 }` is `{3,4}`. */
static void skip_quantifier_space(const GRX_Parser * parser, size_t * scan) {
  while (byte_at(parser, *scan) == ' ' || byte_at(parser, *scan) == '\t') {
    (*scan)++;
  }
}

static GRX_Result pcre_brace_quantifier(
    GRX_Parser * parser, GRX_Quantifier * out) {
  size_t scan = 0;
  uint64_t min = 0;
  int min_digits = 0;

  *out = (GRX_Quantifier) {0, GRX_REPEAT_INF, 0};

  skip_quantifier_space(parser, &scan);
  while (is_decimal(byte_at(parser, scan)) && min < 0xFFFFFFFFu) {
    min = min * 10 + (uint64_t)(byte_at(parser, scan) - '0');
    scan++;
    min_digits++;
  }
  skip_quantifier_space(parser, &scan);

  if (byte_at(parser, scan) == '}') {
    if (!min_digits) {
      return GRX_OK;
    }
    parser->position += scan + 1;
    out->min = (uint32_t)min;
    out->max = (uint32_t)min;
    out->is_quantifier = 1;
    return check_repeat_bound(parser, min, min);
  }

  if (byte_at(parser, scan) != ',') {
    return GRX_OK;
  }
  scan++;
  skip_quantifier_space(parser, &scan);

  uint64_t max = 0;
  int max_digits = 0;
  while (is_decimal(byte_at(parser, scan)) && max < 0xFFFFFFFFu) {
    max = max * 10 + (uint64_t)(byte_at(parser, scan) - '0');
    scan++;
    max_digits++;
  }
  skip_quantifier_space(parser, &scan);
  if (byte_at(parser, scan) != '}' || (!min_digits && !max_digits)) {
    return GRX_OK;
  }

  parser->position += scan + 1;
  out->min = (uint32_t)min;
  out->max = max_digits ? (uint32_t)max : GRX_REPEAT_INF;
  out->is_quantifier = 1;
  return check_repeat_bound(parser, min, max_digits ? max : 0);
}

static GRX_Result pcre_literal_atom(GRX_Parser * parser, uint32_t codepoint,
    size_t offset, size_t length, uint32_t * out_node) {
  // `{` is where "a literal" and "an operator" are decided by what follows.
  // `a{` is two characters because `{` begins no quantifier; `{4,5}abc` is an
  // error because it begins one and there is nothing before it to repeat.
  // The parser cannot tell the two apart - it only calls this hook once the
  // character has no operator meaning *at the start of an atom*.
  if (codepoint == '{') {
    size_t resume = parser->position;
    GRX_Quantifier bounds = {0, GRX_REPEAT_INF, 0};
    GRX_Result probe = pcre_brace_quantifier(parser, &bounds);
    parser->position = resume;
    if (probe == GRX_OK && bounds.is_quantifier) {
      return grx_parse_fail(parser, GRX_DIAG_NOTHING_TO_REPEAT, offset, 1);
    }
  }

  return grx_parse_literal_node(parser, codepoint, offset, length, out_node);
}

/** Whether a code point is whitespace for the purpose of extended mode. */
static int is_extended_space(const GRX_Parser * parser, uint32_t codepoint) {
  switch (codepoint) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20:
      return 1;
    default:
      break;
  }
  if (!(parser->options & GRX_OPT_UTF)) {
    return 0;
  }

  // pcre2pattern: in UTF mode the wider set applies. These are the code
  // points with the White_Space property above U+007F.
  switch (codepoint) {
    case 0x85: case 0xA0: case 0x1680: case 0x2028: case 0x2029:
    case 0x202F: case 0x205F: case 0x3000:
      return 1;
    default:
      return codepoint >= 0x2000 && codepoint <= 0x200A;
  }
}

/**
 * Find where a `\Q` run ends: the next `\E`, or the end of the pattern.
 */
static size_t quote_run_end(const GRX_Parser * parser) {
  for (size_t i = parser->position; i + 1 < parser->length; i++) {
    if (parser->text[i] == '\\' && parser->text[i + 1] == 'E') {
      return i;
    }
  }

  return parser->length;
}

/**
 * Consume what stands between atoms and is not one.
 *
 * Two things: `\Q...\E`, always, and what extended mode ignores. `\Q` is
 * here rather than in the escape reader because an empty run is not an atom
 * at all - `a\Q\E*` is `a*` in both references, and a front end that gave
 * `\Q\E` a node of its own would make the `*` repeat that node instead.
 */
static GRX_Result pcre_skip_ignorable(GRX_Parser * parser) {
  for (;;) {
    if (looking_at(parser, "(?#")) {
      // A comment group is lexically invisible, not an empty atom:
      // pcre2test compiles `a(?#x)*` as `a*` and refuses `(?#x)*` for having
      // nothing to repeat. A node here would make the first of those a
      // repeat of the comment. A callout is *not* in this list - `a(?C1)*`
      // is an error, so a callout is an atom that may not be repeated.
      size_t start = parser->position;
      parser->position += 3;
      while (!grx_parse_at_end(parser) && byte_at(parser, 0) != ')') {
        parser->position++;
      }
      if (!grx_parse_eat(parser, ')')) {
        return grx_parse_fail(parser, GRX_DIAG_UNTERMINATED_COMMENT, start,
            parser->position - start);
      }
      continue;
    }
    if (byte_at(parser, 0) == '\\' && byte_at(parser, 1) == 'Q') {
      parser->position += 2;
      parser->quote_end = quote_run_end(parser);
      if (parser->quote_end > parser->position) {
        return GRX_OK;
      }
      parser->quote_end = GRX_NPOS;
      continue;
    }
    if (byte_at(parser, 0) == '\\' && byte_at(parser, 1) == 'E') {
      // A `\E` with no run open. Harmless in both references.
      parser->position += 2;
      continue;
    }
    if (!(parser->options & GRX_OPT_EXTENDED)) {
      return GRX_OK;
    }
    break;
  }

  for (;;) {
    if (grx_parse_at_end(parser)) {
      return GRX_OK;
    }

    uint32_t codepoint = 0;
    size_t width = 0;
    if (grx_parse_peek(parser, &codepoint, &width) != GRX_OK) {
      return GRX_OK; // Malformed UTF-8; the atom reader reports it.
    }

    if (is_extended_space(parser, codepoint)) {
      parser->position += width;
      continue;
    }
    if (codepoint != '#') {
      if ((codepoint == '\\'
              && (byte_at(parser, 1) == 'Q' || byte_at(parser, 1) == 'E'))
          || looking_at(parser, "(?#")) {
        // Whitespace, then something else that is not an atom either: start
        // over, so that `(?x) \Qa b\E` keeps the space inside the run and
        // drops the one before it, and `a (?#x) (?#y) {3}` is `a{3}`.
        return pcre_skip_ignorable(parser);
      }
      return GRX_OK;
    }

    // A comment runs to the next line break, or to the end of the pattern.
    parser->position += width;
    while (!grx_parse_at_end(parser)) {
      if (grx_parse_peek(parser, &codepoint, &width) != GRX_OK) {
        return GRX_OK;
      }
      parser->position += width;
      if (codepoint == 0x0A || codepoint == 0x0D
          || ((parser->options & GRX_OPT_UTF)
              && (codepoint == 0x85 || codepoint == 0x2028
                  || codepoint == 0x2029))) {
        break;
      }
    }
  }
}

/**
 * Refuse a quantifier the dialect does not allow on this atom.
 *
 * Both references report "nothing to repeat" for a quantifier applied to
 * something with no extent: an assertion, a verb, an option setting or a
 * comment. `\K` is in the list for the same reason - it moves the reported
 * start and consumes nothing.
 */
static GRX_Result pcre_check_quantifier_target(
    GRX_Parser * parser, uint32_t node, size_t offset, size_t length) {
  const GRX_Node * atom = grx_pattern_node(parser->pattern, node);
  if (!atom) {
    return GRX_OK;
  }

  switch (atom->kind) {
    // A lookaround *is* quantifiable here, which surprised this file's first
    // draft: `/(?=a)*/` compiles in pcre2test 10.46, where the same pattern
    // is a syntax error in ECMAScript's Unicode mode. So is `(*ACCEPT)*`.
    // Both were refused until the corpus said otherwise.
    case GRX_NODE_ANCHOR:
    case GRX_NODE_KEEP:
    case GRX_NODE_EMPTY:
      return grx_parse_fail(
          parser, GRX_DIAG_NOTHING_TO_REPEAT, offset, length);

    case GRX_NODE_CONTROL:
      // `(*ACCEPT)*` compiles and `(*FAIL)*` does not, which is not an
      // inconsistency: `(*FAIL)` is `(?!)` written short, and a negative
      // lookahead is the one verb with no extent of its own.
      if (atom->a == (uint32_t)GRX_VERB_FAIL) {
        return grx_parse_fail(
            parser, GRX_DIAG_NOTHING_TO_REPEAT, offset, length);
      }
      return GRX_OK;

    case GRX_NODE_OPTIONS:
      // `(?i:a)*` is a group and may be repeated; `(?i)*` is not.
      if (!(atom->flags & GRX_NODE_SCOPED)) {
        return grx_parse_fail(
            parser, GRX_DIAG_NOTHING_TO_REPEAT, offset, length);
      }
      return GRX_OK;

    default:
      return GRX_OK;
  }
}

// --------------------------------------------------------------------------
// What only the finished pattern can show
// --------------------------------------------------------------------------

/** Whether the pattern has a capturing group with this name. */
static int group_named(const GRX_Parser * parser, const char * name) {
  for (size_t i = 0; i < parser->pattern->nodes.count; i++) {
    const GRX_Node * node = grx_pattern_node(parser->pattern, (uint32_t)i);
    if (!node || node->kind != GRX_NODE_GROUP
        || !(node->flags & GRX_NODE_NAMED)) {
      continue;
    }
    const char * existing = grx_pattern_name(parser->pattern, node->b);
    if (existing && strcmp(existing, name) == 0) {
      return 1;
    }
  }

  return 0;
}

/**
 * Check the references the pattern makes against the groups it has.
 *
 * Done afterwards rather than at each reference because a name may be
 * defined later: `(?&later)(?<later>a)` is a valid subroutine call, and a
 * check at the reference would have to guess.
 */
static GRX_Result pcre_validate(GRX_Parser * parser) {
  for (size_t i = 0; i < parser->pattern->nodes.count; i++) {
    const GRX_Node * node = grx_pattern_node(parser->pattern, (uint32_t)i);
    if (!node) {
      continue;
    }

    int names_group = (node->kind == GRX_NODE_BACKREF
                          || node->kind == GRX_NODE_RECURSE
                          || node->kind == GRX_NODE_CONDITIONAL)
        && (node->flags & GRX_NODE_NAMED);
    if (!names_group) {
      continue;
    }

    uint32_t offset = node->kind == GRX_NODE_CONDITIONAL ? node->b
        : node->kind == GRX_NODE_BACKREF                 ? node->b
                                                         : node->b;
    const char * name = grx_pattern_name(parser->pattern, offset);
    if (!name || !group_named(parser, name)) {
      return grx_parse_fail(parser, GRX_DIAG_UNKNOWN_GROUP_NAME, node->offset,
          node->length);
    }
  }

  return GRX_OK;
}

// --------------------------------------------------------------------------
// The tables
// --------------------------------------------------------------------------

const GRX_Frontend grx_frontend_pcre = {
  .name = "pcre",
  .atom_escape = pcre_atom_escape,
  .class_escape = pcre_class_escape,
  .char_class = pcre_char_class,
  .group_open = pcre_group_open,
  .brace_quantifier = pcre_brace_quantifier,
  .literal_atom = pcre_literal_atom,
  .check_quantifier_target = pcre_check_quantifier_target,
  .skip_ignorable = pcre_skip_ignorable,
  .validate = pcre_validate,
};

// Perl shares every hook. The differences between the two are inside them,
// selected by flavour(), and there are few enough to list: `(?^...)` and the
// charset modifiers, `\N{U+...}`, and what a class escape at the end of a
// range does. A second table with the same entries is still worth having,
// because `grx_frontend_for()` returning one front end for two dialects
// would make "which dialect is this" a question with no answer at the point
// a diagnostic is written.
const GRX_Frontend grx_frontend_perl = {
  .name = "perl",
  .atom_escape = pcre_atom_escape,
  .class_escape = pcre_class_escape,
  .char_class = pcre_char_class,
  .group_open = pcre_group_open,
  .brace_quantifier = pcre_brace_quantifier,
  .literal_atom = pcre_literal_atom,
  .check_quantifier_target = pcre_check_quantifier_target,
  .skip_ignorable = pcre_skip_ignorable,
  .validate = pcre_validate,
};
