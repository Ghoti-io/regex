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
 * Replacement and splitting.
 *
 * Both are loops over grx_regex_search_next(), which is why WP-15 came
 * first: the dialect's rule for what follows an empty match is exactly the
 * rule that decides whether replacing `a*` in "aab" produces "XXbX" or an
 * infinite loop, and it is already written down once.
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/exec.h>
#include <ghoti.io/regex/subst.h>
#include <stddef.h>
#include <string.h>

#include "../compile/compile_internal.h"
#include "../core/core_internal.h"
#include "../parse/parse_internal.h"
#include "../unicode/unicode_internal.h"
#include "subst_internal.h"

// --------------------------------------------------------------------------
// What the caller owns
// --------------------------------------------------------------------------

void grx_text_free(GRX_Text * text) {
  if (!text || !text->data) {
    return;
  }
  const GRX_Allocator * allocator = text->allocator;
  if (!allocator) {
    allocator = grx_allocator_default();
  }
  gcu_allocator_free(allocator, text->data);
  *text = (GRX_Text) {NULL, 0, NULL};
}

void grx_split_free(GRX_Split * split) {
  if (!split || !split->pieces) {
    return;
  }
  const GRX_Allocator * allocator = split->allocator;
  if (!allocator) {
    allocator = grx_allocator_default();
  }
  gcu_allocator_free(allocator, split->pieces);
  *split = (GRX_Split) {NULL, 0, NULL};
}

// --------------------------------------------------------------------------
// Parsing a template
// --------------------------------------------------------------------------

/** Record a failure and its offset in the *template*, not the pattern. */
static GRX_Result fail(
    GRX_Error * out_error, GRX_Diag diag, size_t offset, size_t length) {
  return grx_error_set(
      out_error, grx_diag_result(diag), diag, offset, length);
}

/** Append one op. */
static GRX_Result emit(GRX_Template * tmpl, GRX_TemplateOpKind kind,
    uint32_t a, size_t offset, size_t length) {
  GRX_TemplateOp op = {(uint8_t)kind, a, offset, length};
  return grx_arena_append(&tmpl->ops, &op, NULL);
}

/**
 * Append literal bytes, merging with the op before when it is also literal
 * and abuts.
 *
 * Without the merge a template of plain text would cost one op per byte,
 * which is a parsed form larger than the thing it parsed.
 */
static GRX_Result emit_literal(
    GRX_Template * tmpl, size_t offset, size_t length) {
  if (!length) {
    return GRX_OK;
  }
  if (tmpl->ops.count) {
    GRX_TemplateOp * last
        = GRX_ARENA_AT(GRX_TemplateOp, &tmpl->ops, tmpl->ops.count - 1);
    if (last && last->kind == GRX_TPL_LITERAL
        && last->offset + last->length == offset) {
      last->length += length;
      return GRX_OK;
    }
  }
  return emit(tmpl, GRX_TPL_LITERAL, 0, offset, length);
}

/** Whether the pattern has at least one named group. */
static int has_named_groups(const GRX_Regex * regex) {
  size_t count = grx_regex_capture_count(regex);
  for (size_t i = 1; i <= count; i++) {
    if (grx_regex_capture_name(regex, i)) {
      return 1;
    }
  }
  return 0;
}

/**
 * The longest run of digits at `at` that names a group the pattern has, up to
 * two digits.
 *
 * ECMAScript's rule, and the reason `$12` against a two-group pattern is
 * group 1 followed by a literal "2" while `$12` against a twelve-group
 * pattern is group 12. Two digits are tried first; one digit second; and if
 * neither names a group there is no reference here at all.
 *
 * @return Digits consumed, or 0 when no group is named.
 */
static size_t digits_naming_a_group(const char * text, size_t length,
    size_t at, size_t captures, int fallback, uint32_t * out_group) {
  size_t available = length - at;
  // How many digits are actually *there*, up to two. Not the window: `\1X`
  // has one digit in a window of two, and a rule written on the window would
  // refuse it.
  size_t digits = 0;
  while (digits < 2 && digits < available && text[at + digits] >= '0'
      && text[at + digits] <= '9') {
    digits++;
  }
  for (size_t take = available < 2 ? available : 2; take >= 1; take--) {
    // Python does not fall back to a shorter reading. `\12` against a
    // two-group pattern is "invalid group reference 12" in `re`, where
    // ECMAScript reads it as group 1 and a literal "2" - so the reference's
    // width is decided by the digits present, and if that group does not
    // exist there is no shorter reference to try.
    if (!fallback && take != digits) {
      continue;
    }
    uint32_t value = 0;
    size_t i = 0;
    for (; i < take; i++) {
      char c = text[at + i];
      if (c < '0' || c > '9') {
        break;
      }
      value = value * 10 + (uint32_t)(c - '0');
    }
    if (i != take || !value || value > captures) {
      continue;
    }
    *out_group = value;
    return take;
  }
  return 0;
}

/**
 * Resolve `$<name>`.
 *
 * Three outcomes, and ECMAScript gives all three different meanings: with no
 * named groups in the pattern the whole thing is literal text; with named
 * groups but no closing `>` it is literal text; with named groups and a
 * closing `>` naming nothing, it substitutes the empty string. The third is
 * the surprising one and is what Node does.
 *
 * @return Bytes consumed from `<` onwards, or 0 when this is not a reference.
 */
static size_t named_reference(const GRX_Regex * regex, const char * text,
    size_t length, size_t at, char opener, char closer,
    GRX_TemplateOpKind * out_kind, uint32_t * out_group,
    size_t * out_name_at, size_t * out_name_length) {
  if (at >= length || text[at] != opener) {
    return 0;
  }
  const char * close = memchr(text + at, closer, length - at);
  if (!close) {
    return 0;
  }
  size_t name_length = (size_t)(close - (text + at)) - 1;

  // A group name is an IdentifierName and cannot contain a NUL, so a bounded
  // copy onto the stack is enough to use the NUL-terminated lookup. A name
  // longer than any the parser would have accepted cannot match one.
  char name[256];
  if (name_length >= sizeof(name)) {
    *out_kind = GRX_TPL_NOTHING;
    return name_length + 2;
  }
  memcpy(name, text + at + 1, name_length);
  name[name_length] = '\0';

  size_t index = 0;
  if (grx_regex_capture_index(regex, name, &index) == GRX_OK) {
    // GROUP_NAMED rather than GROUP, so that the name is resolved when the
    // substitution happens rather than now. One name may stand for several
    // groups - perl always, PCRE2 under `(?J)` - and which of them the
    // reference means is the first that *took part*, which is not knowable
    // until there is a match. grx_match_group_named() is that rule and was
    // already written; this makes the template ask it instead of resolving
    // the name to the first group and comparing against that one alone.
    *out_kind = GRX_TPL_GROUP_NAMED;
    *out_group = (uint32_t)index;
    *out_name_at = at + 1;
    *out_name_length = name_length;
  }
  else {
    *out_kind = GRX_TPL_NOTHING;
  }
  return name_length + 2;
}

/**
 * Read one of Python's template escapes, the backslash already located.
 *
 * `re.sub`'s alphabet is closed the way its pattern alphabet is, so an
 * unknown letter is an error rather than the letter. The numeric forms are
 * not here: `\1` is a group and is read by the caller's GRX_TMPL_NUMBER
 * branch, and only a *leading zero* makes a digit run octal, which is what
 * lets `\0` be NUL while `\g<0>` is the whole match.
 *
 * @param text The template.
 * @param length Its length.
 * @param after The offset just past the backslash.
 * @param out_codepoint Receives the code point the escape denotes.
 * @return Bytes consumed from `after` onwards, or 0 when this is not one.
 */
/**
 * How many digits after a backslash Python reads as *octal* rather than as a
 * group number, or 0 when they are a group reference.
 *
 * Two shapes, probed against CPython 3.13 with twelve groups in the pattern
 * so that both readings were available for every case:
 *
 *   `\0`, `\01`, `\012`   a leading zero is octal, up to three digits
 *   `\101`                three octal digits are octal - `\1234` is "S4"
 *   `\1`, `\12`, `\18`    one or two digits otherwise are a group
 *   `\108`                `8` is not octal, so this is group 10 then "8"
 *
 * The three-digit case is why this cannot be a test on the first digit
 * alone: `\12` is group 12 and `\123` is a code point.
 *
 * @param text The template.
 * @param length Its length.
 * @param after The offset just past the backslash.
 * @return Digits to consume as octal, or 0 for "this is a group".
 */
static size_t python_octal_run(
    const char * text, size_t length, size_t after) {
  if (after >= length) {
    return 0;
  }
  int octal_digit = text[after] >= '0' && text[after] <= '7';
  if (text[after] == '0') {
    size_t taken = 0;
    while (taken < 3 && after + taken < length
        && text[after + taken] >= '0' && text[after + taken] <= '7') {
      taken++;
    }
    return taken;
  }
  if (!octal_digit || after + 2 >= length) {
    return 0;
  }
  for (size_t digit = 1; digit < 3; digit++) {
    if (text[after + digit] < '0' || text[after + digit] > '7') {
      return 0;
    }
  }
  return 3;
}

static size_t python_escape(const char * text, size_t length, size_t after,
    uint32_t * out_codepoint, int * out_verbatim) {
  *out_verbatim = 0;
  if (after >= length) {
    return 0;
  }

  // A backslash before anything that is not a letter or a digit keeps *both*
  // characters: `re.sub("(a)", r"\$", "a")` is `\$`, two characters, where
  // sed's rule would give `$` and Perl's would give `$`. So the error case
  // below is only ever an unknown *alphanumeric*.
  {
    char c = text[after];
    int alnum = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
        || (c >= '0' && c <= '9');
    if (!alnum && c != '\\') {
      *out_verbatim = 1;
      return 1;
    }
  }
  switch (text[after]) {
    case 'n': *out_codepoint = 0x0A; return 1;
    case 't': *out_codepoint = 0x09; return 1;
    case 'r': *out_codepoint = 0x0D; return 1;
    case 'f': *out_codepoint = 0x0C; return 1;
    case 'v': *out_codepoint = 0x0B; return 1;
    case 'a': *out_codepoint = 0x07; return 1;
    case 'b': *out_codepoint = 0x08; return 1;
    case '\\': *out_codepoint = '\\'; return 1;
    default: break;
  }

  size_t octal = python_octal_run(text, length, after);
  if (octal) {
    uint32_t value = 0;
    for (size_t digit = 0; digit < octal; digit++) {
      value = value * 8 + (uint32_t)(text[after + digit] - '0');
    }
    if (value > 0377u) {
      // `re`: "octal escape value \777 outside of range 0-0o377". A byte,
      // not a code point, which is what "octal" means in both references
      // that have it.
      return 0;
    }
    *out_codepoint = value;
    return octal;
  }

  // Nothing else. `\u`, `\U`, `\N{NAME}` and `\x` are all *pattern* escapes
  // in Python and all "bad escape" in a template - the two alphabets are
  // closed separately, and the template's is the smaller. This file
  // implemented the pattern's set here first, on the reasonable-looking
  // assumption that one dialect has one alphabet, and
  // tools/oracle/replace_diff.py's python arm is what said otherwise.
  return 0;
}

/**
 * Read a bare name: `$name`, PCRE2's and Go's spelling.
 *
 * Greedily, which is the rule and not an implementation choice: Go's
 * documentation says `$1x` is the group named `1x`, and PCRE2 reads a name
 * the same way. A reader that stopped at the first character which cannot
 * continue a *number* would resolve `$1x` as group 1 followed by "x".
 */
static size_t bare_name_reference(const GRX_Regex * regex, const char * text,
    size_t length, size_t at, GRX_TemplateOpKind * out_kind,
    uint32_t * out_group, size_t * out_name_at, size_t * out_name_length) {
  size_t end = at;
  while (end < length) {
    char c = text[end];
    int name_char = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z')
        || (c >= '0' && c <= '9') || c == '_';
    if (!name_char) {
      break;
    }
    end++;
  }

  size_t name_length = end - at;
  if (!name_length) {
    return 0;
  }

  char name[256];
  if (name_length >= sizeof(name)) {
    *out_kind = GRX_TPL_NOTHING;
    return name_length;
  }
  memcpy(name, text + at, name_length);
  name[name_length] = '\0';

  size_t index = 0;
  if (grx_regex_capture_index(regex, name, &index) == GRX_OK) {
    *out_kind = GRX_TPL_GROUP_NAMED;
    *out_group = (uint32_t)index;
    *out_name_at = at;
    *out_name_length = name_length;
    return name_length;
  }

  // Not a name this pattern has. It may still be a *number*, which the
  // caller reads instead: `${1}` and `$1` are the same reference.
  return 0;
}

/**
 * Read `${...}`: a number or a name, depending on what is inside.
 *
 * One reader for both because the braces do not say which it is, and a
 * template that wrote `${1}` against a group named `1` would otherwise
 * depend on which of two functions was tried first.
 */
static size_t braced_reference(const GRX_TemplateSpec * spec,
    const GRX_Regex * regex, const char * text, size_t length, size_t at,
    size_t captures, GRX_TemplateOpKind * out_kind, uint32_t * out_group,
    size_t * out_name_at, size_t * out_name_length) {
  if (at >= length || text[at] != '{') {
    return 0;
  }
  const char * close = memchr(text + at, '}', length - at);
  if (!close) {
    return 0;
  }
  size_t inner = (size_t)(close - (text + at)) - 1;
  if (!inner) {
    return 0;
  }

  int all_digits = 1;
  for (size_t i = 0; i < inner; i++) {
    char c = text[at + 1 + i];
    if (c < '0' || c > '9') {
      all_digits = 0;
      break;
    }
  }

  if (all_digits) {
    if (!(spec->features & GRX_TMPL_NUMBER_BRACED)) {
      return 0;
    }
    uint32_t value = 0;
    for (size_t i = 0; i < inner && value <= captures; i++) {
      value = value * 10 + (uint32_t)(text[at + 1 + i] - '0');
    }
    if (!value && (spec->features & GRX_TMPL_WHOLE_ZERO)) {
      // `${0}` is the whole match wherever `$0` is. pcre2 spells it three
      // ways - `$&`, `$0` and `${0}` - and this read the braced one as a
      // reference to group zero, which is no group, so it became "a group
      // the pattern has not got" and an error. The bare form was handled
      // where the digits are read and the braced form was not.
      *out_kind = GRX_TPL_WHOLE;
      *out_group = 0;
      return inner + 2;
    }
    *out_kind = value && value <= captures ? GRX_TPL_GROUP : GRX_TPL_NOTHING;
    *out_group = value;
    return inner + 2;
  }

  if (!(spec->features & GRX_TMPL_NAME_BRACED)) {
    return 0;
  }
  return named_reference(regex, text, length, at, '{', '}', out_kind,
      out_group, out_name_at, out_name_length);
}

/**
 * Which piece vim's replacement spells with this letter, or GRX_TPL_COUNT.
 *
 * Enumerated against vim 9.1's `substitute()` over the whole printable
 * alphabet: these ten letters mean something and every other escape is the
 * bare character, which GRX_TMPL_ESCAPE_ANY already gives.
 */
static GRX_TemplateOpKind vim_escape(char c) {
  switch (c) {
    case 'n': case 'r': case 't': case 'b':
      return GRX_TPL_CODEPOINT;
    case 'u': case 'l': case 'U': case 'L': case 'E': case 'e':
      return GRX_TPL_CASE;
    default:
      return GRX_TPL_COUNT;
  }
}

/** The code point one of vim's four control escapes stands for. */
static uint32_t vim_control(char c) {
  switch (c) {
    case 'n': return 0x0A;
    case 'r': return 0x0D;
    case 't': return 0x09;
    default: return 0x08;
  }
}

/**
 * The case operator one of Perl's six letters asks for, or GRX_TPL_CASE_NONE.
 *
 * Seven, and not vim's seven: `\F` is a third *run* - a case fold - which no
 * other dialect here has, and `\e` is **not** a terminator, which vim's is.
 * dialects.md section 5.11 listed six for years and the seventh was found by
 * enumerating the printable alphabet against perl rather than by reading.
 *
 * `\e` is **not** a terminator here: In a perl
 * replacement `\e` is U+001B, the string being double-quotish, and reading it
 * as `\E` would make `s/x/\Uab\ecd/` write "ABcd" where perl writes
 * "AB\x{1b}CD". vim's `\e` and `\E` are the same marker and vim_case() takes
 * both, which is why the two dialects need two tables rather than one.
 *
 * What is still *not* read here is the rest of the double-quotish alphabet -
 * `\n`, `\t`, `\x41`, `\N{}` - which GRX_TMPL_BACKSLASH_ESCAPE turns into
 * the bare letter. That is a wider gap than the case operators and is not this
 * one; dialects.md section 5.11 says so.
 */
static GRX_TemplateCase perl_case(char c) {
  switch (c) {
    case 'u': return GRX_TPL_CASE_UPPER_ONE;
    case 'l': return GRX_TPL_CASE_LOWER_ONE;
    case 'U': return GRX_TPL_CASE_UPPER_RUN;
    case 'L': return GRX_TPL_CASE_LOWER_RUN;
    case 'F': return GRX_TPL_CASE_FOLD_RUN;
    case 'Q': return GRX_TPL_CASE_QUOTE_RUN;
    case 'E': return GRX_TPL_CASE_NONE;
    default: return GRX_TPL_CASE_COUNT;
  }
}

/** The case change one of vim's six markers asks for. */
static GRX_TemplateCase vim_case(char c) {
  switch (c) {
    case 'u': return GRX_TPL_CASE_UPPER_ONE;
    case 'l': return GRX_TPL_CASE_LOWER_ONE;
    case 'U': return GRX_TPL_CASE_UPPER_RUN;
    case 'L': return GRX_TPL_CASE_LOWER_RUN;
    default: return GRX_TPL_CASE_NONE;
  }
}

GRX_Result grx_template_parse(const GRX_TemplateSpec * spec,
    const GRX_Regex * regex, const char * text, size_t length,
    const GRX_Allocator * allocator, GRX_Error * out_error,
    GRX_Template * out_template) {
  if (!spec || !regex || !out_template || (!text && length)) {
    return GRX_ERR_INVALID;
  }

  grx_arena_init(&out_template->ops, allocator, sizeof(GRX_TemplateOp), 0,
      GRX_DIAG_OUT_OF_MEMORY);
  out_template->text = text;
  out_template->length = length;
  out_template->perl_case_ops
      = (uint8_t)((spec->features & GRX_TMPL_CASE_ESCAPES) ? 1 : 0);

  const size_t captures = grx_regex_capture_count(regex);
  const int named = has_named_groups(regex);
  const char sigil = spec->sigil;

  size_t i = 0;
  size_t literal_from = 0;
  while (i < length) {
    if (text[i] == '\\' && (spec->features & GRX_TMPL_CASE_ESCAPES)
        && i + 1 < length
        && perl_case(text[i + 1]) != GRX_TPL_CASE_COUNT) {
      // **Before GRX_TMPL_BACKSLASH_ESCAPE**, which a Perl row also has and
      // which would otherwise claim the letter: `\U` was the letter "U" in a
      // Perl template until 2026-09-26 for exactly that reason. The two
      // cannot be merged, because what is left after these six *is* the
      // escape-anything rule - `\$` is a dollar and `\q` is a "q".
      GRX_Result result
          = emit_literal(out_template, literal_from, i - literal_from);
      if (result == GRX_OK) {
        result = emit(out_template, GRX_TPL_CASE,
            (uint32_t)perl_case(text[i + 1]), 0, 0);
      }
      if (result != GRX_OK) {
        grx_template_clear(out_template);
        return fail(out_error, GRX_DIAG_OUT_OF_MEMORY, i, 2);
      }
      i += 2;
      literal_from = i;
      continue;
    }
    if (text[i] == '\\' && (spec->features & GRX_TMPL_BACKSLASH_ESCAPE)
        && i + 1 < length) {
      // Perl's templates are interpolated strings: `\$` is a literal dollar
      // and `$$` is the process id, so the two escaping rules are mutually
      // exclusive and a dialect has one or the other.
      GRX_Result result
          = emit_literal(out_template, literal_from, i - literal_from);
      if (result == GRX_OK) {
        result = emit_literal(out_template, i + 1, 1);
      }
      if (result != GRX_OK) {
        grx_template_clear(out_template);
        return fail(out_error, GRX_DIAG_OUT_OF_MEMORY, i, 2);
      }
      i += 2;
      literal_from = i;
      continue;
    }
    if (text[i] == '&' && (spec->features & GRX_TMPL_WHOLE_BARE)) {
      // sed's whole match. Not introduced by the sigil - `\&` is the
      // *literal* `&` here, which is GRX_TMPL_ESCAPE_ANY's job - so it is
      // caught before the sigil test rather than inside it.
      GRX_Result result
          = emit_literal(out_template, literal_from, i - literal_from);
      if (result == GRX_OK) {
        result = emit(out_template, GRX_TPL_WHOLE, 0, 0, 0);
      }
      if (result != GRX_OK) {
        grx_template_clear(out_template);
        return fail(out_error, GRX_DIAG_OUT_OF_MEMORY, i, 1);
      }
      i++;
      literal_from = i;
      continue;
    }
    if (text[i] != sigil) {
      i++;
      continue;
    }

    size_t start = i;
    size_t after = i + 1;
    GRX_TemplateOpKind kind = GRX_TPL_COUNT;
    uint32_t group = 0;
    size_t consumed = 0;
    // Where the name sits inside the template, for GRX_TPL_GROUP_NAMED.
    size_t name_at = 0;
    size_t name_length = 0;

    if (after < length) {
      char c = text[after];
      if (c == sigil && (spec->features & GRX_TMPL_DOUBLE_SIGIL)) {
        // A literal sigil. Emitted as a one-byte literal run naming the
        // second of the two, so no bytes have to be copied anywhere.
        GRX_Result result = emit_literal(out_template, literal_from,
            start - literal_from);
        if (result == GRX_OK) {
          result = emit_literal(out_template, after, 1);
        }
        if (result != GRX_OK) {
          grx_template_clear(out_template);
          return fail(out_error, GRX_DIAG_OUT_OF_MEMORY, start, 2);
        }
        i = after + 1;
        literal_from = i;
        continue;
      }
      if (c == '&' && (spec->features & GRX_TMPL_WHOLE)) {
        kind = GRX_TPL_WHOLE;
        consumed = 1;
      }
      else if (c == '`' && (spec->features & GRX_TMPL_PREFIX)) {
        kind = GRX_TPL_PREFIX;
        consumed = 1;
      }
      else if (c == '\'' && (spec->features & GRX_TMPL_SUFFIX)) {
        kind = GRX_TPL_SUFFIX;
        consumed = 1;
      }
      else if (c == '_' && (spec->features & GRX_TMPL_SUBJECT)) {
        kind = GRX_TPL_SUBJECT;
        consumed = 1;
      }
      else if (c == '<' && (spec->features & GRX_TMPL_NAME_ANGLE)
          && (named || !(spec->features & GRX_TMPL_NAME_NEEDS_NAMED_GROUPS))) {
        consumed = named_reference(regex, text, length, after, '<', '>',
            &kind, &group, &name_at, &name_length);
        // The same check the braced form has had: a name that resolves to
        // nothing is an error where the dialect says so. Its absence here
        // was invisible while `$<...>` belonged to ECMAScript alone, whose
        // rule is that it substitutes the empty string.
        if (consumed && kind == GRX_TPL_NOTHING
            && spec->missing == GRX_TMPL_MISSING_ERROR) {
          grx_template_clear(out_template);
          return fail(out_error, GRX_DIAG_TEMPLATE_UNKNOWN_GROUP, start,
              1 + consumed);
        }
      }
      else if (c == '{'
          && (spec->features
              & (GRX_TMPL_NUMBER_BRACED | GRX_TMPL_NAME_BRACED))) {
        consumed = braced_reference(spec, regex, text, length, after,
            captures, &kind, &group, &name_at, &name_length);
        if (consumed && kind == GRX_TPL_NOTHING
            && spec->missing == GRX_TMPL_MISSING_ERROR) {
          grx_template_clear(out_template);
          return fail(out_error, GRX_DIAG_TEMPLATE_UNKNOWN_GROUP, start,
              1 + consumed);
        }
      }
      else if (c == 'g' && (spec->features & GRX_TMPL_G_ANGLE)
          && after + 1 < length && text[after + 1] == '<') {
        // Python's `\g<...>`, the one spelling that takes either kind of
        // reference. A number is tried first and a name second, because
        // `re` resolves it that way: a group *named* `1` cannot be written
        // in a Python pattern - `(?P<1>x)` is "bad character in group name"
        // - so there is no case where the two readings compete.
        size_t close = after + 2;
        while (close < length && text[close] != '>') {
          close++;
        }
        if (close >= length) {
          // `re`: "missing >, unterminated name". An unterminated `\g<` is
          // an error and not a literal run - leaving `consumed` at zero here
          // let the whole thing stand as text, because this branch had
          // already been taken and the escape branch below could not see it.
          grx_template_clear(out_template);
          return fail(out_error, GRX_DIAG_TEMPLATE_UNKNOWN_GROUP, start,
              length - start);
        }
        {
          size_t inner = after + 2;
          size_t inner_length = close - inner;
          int numeric = inner_length > 0;
          uint32_t value = 0;
          for (size_t digit = 0; digit < inner_length; digit++) {
            if (text[inner + digit] < '0' || text[inner + digit] > '9') {
              numeric = 0;
              break;
            }
            value = value * 10 + (uint32_t)(text[inner + digit] - '0');
          }
          if (numeric) {
            // `\g<0>` is the whole match, where a bare `\0` is NUL.
            kind = value == 0 ? GRX_TPL_WHOLE
                : value <= captures ? GRX_TPL_GROUP
                                    : GRX_TPL_NOTHING;
            group = value;
            consumed = close - after + 1;
          }
          else {
            consumed = named_reference(regex, text, length, after + 1, '<',
                '>', &kind, &group, &name_at, &name_length);
            if (consumed) {
              consumed += 1;
            }
          }
          if (consumed && kind == GRX_TPL_NOTHING
              && spec->missing == GRX_TMPL_MISSING_ERROR) {
            grx_template_clear(out_template);
            return fail(out_error, GRX_DIAG_TEMPLATE_UNKNOWN_GROUP, start,
                1 + consumed);
          }
        }
      }
      else if (c == '+' && (spec->features & GRX_TMPL_NAME_PLUS_BRACE)
          && after + 1 < length && text[after + 1] == '{') {
        // Perl's `$+{name}`: the named-capture hash, spelled as a lookup.
        consumed = named_reference(regex, text, length, after + 1, '{', '}',
            &kind, &group, &name_at, &name_length);
        if (consumed) {
          consumed += 1;
        }
      }
      else if (c >= '0' && c <= '9'
          && (spec->features & GRX_TMPL_NUMBER_GREEDY)) {
        // Before the WHOLE_ZERO branch, because a leading zero belongs to
        // the number here: `$01` is group 1 and only a number that *is*
        // zero is the whole match.
        uint32_t value = 0;
        size_t taken = 0;
        while (after + taken < length && text[after + taken] >= '0'
            && text[after + taken] <= '9' && value <= captures + 1) {
          value = value * 10 + (uint32_t)(text[after + taken] - '0');
          taken++;
        }
        consumed = taken;
        if (!value && (spec->features & GRX_TMPL_WHOLE_ZERO)) {
          kind = GRX_TPL_WHOLE;
        }
        else if (value && value <= captures) {
          group = value;
          kind = GRX_TPL_GROUP;
        }
        else if (spec->missing == GRX_TMPL_MISSING_ERROR) {
          grx_template_clear(out_template);
          return fail(out_error, GRX_DIAG_TEMPLATE_UNKNOWN_GROUP, start,
              1 + taken);
        }
        else {
          kind = GRX_TPL_NOTHING;
        }
      }
      else if (c == '0' && (spec->features & GRX_TMPL_WHOLE_ZERO)) {
        kind = GRX_TPL_WHOLE;
        consumed = 1;
      }
      else if (c >= '1' && c <= '9'
          && (spec->features & GRX_TMPL_NUMBER_SINGLE)) {
        uint32_t value = (uint32_t)(c - '0');
        if (value <= captures) {
          group = value;
          kind = GRX_TPL_GROUP;
          consumed = 1;
        }
        else if (spec->missing == GRX_TMPL_MISSING_ERROR) {
          grx_template_clear(out_template);
          return fail(out_error, GRX_DIAG_TEMPLATE_UNKNOWN_GROUP, start, 2);
        }
        else if (spec->missing == GRX_TMPL_MISSING_EMPTY) {
          // vim: `\9` in a pattern with two groups substitutes nothing and
          // consumes the digit. sed is the ERROR row above and never
          // reaches this; without it the digit stood as text.
          kind = GRX_TPL_NOTHING;
          consumed = 1;
        }
      }
      else if (c >= '0' && c <= '9' && (spec->features & GRX_TMPL_NUMBER)
          && !((spec->features & GRX_TMPL_PYTHON_ESCAPES)
              && python_octal_run(text, length, after))) {
        // The octal exclusion is Python's: a leading zero, or three octal
        // digits, is a code point there rather than a group - `\0` is NUL,
        // `\012` is a newline and `\101` is "A", while `\1` and `\12` are
        // groups. Without it this branch claimed all of them as group
        // numbers and reported the ones with no such group as missing.
        consumed = digits_naming_a_group(text, length, after, captures,
            !(spec->features & GRX_TMPL_PYTHON_ESCAPES), &group);
        if (consumed) {
          kind = GRX_TPL_GROUP;
        }
        else if (spec->missing == GRX_TMPL_MISSING_ERROR) {
          grx_template_clear(out_template);
          return fail(out_error, GRX_DIAG_TEMPLATE_UNKNOWN_GROUP, start, 2);
        }
        else if (spec->missing == GRX_TMPL_MISSING_EMPTY) {
          // Perl interpolates undef, which is the empty string, and consumes
          // the digits. ECMAScript is the LITERAL row and leaves them alone,
          // which is the branch below. The three rows were two for one
          // revision, and `[$9]` under Perl was an error rather than "[]".
          size_t digits = 0;
          while (after + digits < length && text[after + digits] >= '0'
              && text[after + digits] <= '9') {
            digits++;
          }
          consumed = digits;
          kind = GRX_TPL_NOTHING;
        }
      }
      else if (spec->features & GRX_TMPL_NAME_BARE) {
        consumed
            = bare_name_reference(regex, text, length, after, &kind, &group,
                &name_at, &name_length);
        if (!consumed && spec->missing == GRX_TMPL_MISSING_ERROR) {
          grx_template_clear(out_template);
          return fail(out_error, GRX_DIAG_TEMPLATE_UNKNOWN_GROUP, start, 2);
        }
      }
      else if (spec->features & GRX_TMPL_PYTHON_ESCAPES) {
        // Before the fallback below and after every reference form above,
        // so that `\1` is still a group and `\g<1>` is still a group. What
        // reaches here is a backslash that no reference claimed.
        uint32_t codepoint = 0;
        int verbatim = 0;
        size_t taken
            = python_escape(text, length, after, &codepoint, &verbatim);
        if (!taken) {
          // "bad escape \\q": an unknown *alphanumeric* is an error here,
          // which is the whole difference from GRX_TMPL_ESCAPE_ANY. A
          // non-alphanumeric never reaches this - it is verbatim above.
          grx_template_clear(out_template);
          return fail(out_error, GRX_DIAG_INVALID_ESCAPE, start, 2);
        }
        GRX_Result result = emit_literal(out_template, literal_from,
            start - literal_from);
        if (result == GRX_OK) {
          // Verbatim keeps the backslash too, so the run named here is two
          // characters of the template rather than one decoded code point.
          result = verbatim
              ? emit_literal(out_template, start, 2)
              : emit(out_template, GRX_TPL_CODEPOINT, codepoint, 0, 0);
        }
        if (result != GRX_OK) {
          grx_template_clear(out_template);
          return fail(out_error, GRX_DIAG_OUT_OF_MEMORY, start, 2);
        }
        i = after + taken;
        literal_from = i;
        continue;
      }
      else if ((spec->features & GRX_TMPL_VIM_ESCAPES)
          && vim_escape(c) != GRX_TPL_COUNT) {
        // Before GRX_TMPL_ESCAPE_ANY, which vim also has and which would
        // otherwise make `\n` an "n". Ten characters, enumerated over the
        // whole printable alphabet; everything else falls through.
        GRX_TemplateOpKind vim_kind = vim_escape(c);
        GRX_Result result = emit_literal(out_template, literal_from,
            start - literal_from);
        if (result == GRX_OK) {
          result = emit(out_template, vim_kind,
              vim_kind == GRX_TPL_CODEPOINT ? vim_control(c)
                                            : (uint32_t)vim_case(c),
              0, 0);
        }
        if (result != GRX_OK) {
          grx_template_clear(out_template);
          return fail(out_error, GRX_DIAG_OUT_OF_MEMORY, start, 2);
        }
        i = after + 1;
        literal_from = i;
        continue;
      }
      else if (spec->features & GRX_TMPL_ESCAPE_ANY) {
        // Last, so that every rule above claims its character first. What is
        // left is sed's total escape: `\&` is an `&`, `\\` is a backslash,
        // and `\q` is a `q`.
        GRX_Result result = emit_literal(out_template, literal_from,
            start - literal_from);
        if (result == GRX_OK) {
          result = emit_literal(out_template, after, 1);
        }
        if (result != GRX_OK) {
          grx_template_clear(out_template);
          return fail(out_error, GRX_DIAG_OUT_OF_MEMORY, start, 2);
        }
        i = after + 1;
        literal_from = i;
        continue;
      }
    }
    else if (spec->features & GRX_TMPL_ESCAPE_ANY) {
      // A sigil with nothing after it, which is this library's decision and
      // not an oracle's: sed cannot be asked, because the closing delimiter
      // of its `s` command is exactly what a trailing backslash escapes, so
      // a template ending in one never reaches it. Dropped rather than kept,
      // because the alternative is a template whose last character means
      // "escape" and has nothing to escape.
      GRX_Result result = emit_literal(out_template, literal_from,
          start - literal_from);
      if (result != GRX_OK) {
        grx_template_clear(out_template);
        return fail(out_error, GRX_DIAG_OUT_OF_MEMORY, start, 1);
      }
      i = after;
      literal_from = i;
      continue;
    }

    if (!consumed || kind == GRX_TPL_COUNT) {
      if (spec->features & GRX_TMPL_SIGIL_STRICT) {
        // PCRE2's rule: a sigil that begins no complete reference is an
        // error. `$` at the end of a template, `${1` with no closing brace
        // and `${}` with nothing between are the three this reaches - every
        // other spelling is claimed by a rule above, and a well-formed
        // reference to a group that does not exist is GRX_TemplateMissing's
        // question rather than this one.
        grx_template_clear(out_template);
        return fail(out_error, GRX_DIAG_INVALID_TEMPLATE, start,
            after > start ? after - start + 1 : 1);
      }
      // Not a reference. The sigil is ordinary text, which is ECMAScript's
      // rule for every spelling it does not recognise - including `$3` in a
      // pattern with two groups.
      i = after;
      continue;
    }

    GRX_Result result
        = emit_literal(out_template, literal_from, start - literal_from);
    if (result == GRX_OK) {
      result = kind == GRX_TPL_GROUP_NAMED
          ? emit(out_template, kind, group, name_at, name_length)
          : emit(out_template, kind, group, 0, 0);
    }
    if (result != GRX_OK) {
      grx_template_clear(out_template);
      return fail(out_error, GRX_DIAG_OUT_OF_MEMORY, start, 1 + consumed);
    }
    i = after + consumed;
    literal_from = i;
  }

  if (emit_literal(out_template, literal_from, length - literal_from)
      != GRX_OK) {
    grx_template_clear(out_template);
    return fail(out_error, GRX_DIAG_OUT_OF_MEMORY, length, 0);
  }
  return GRX_OK;
}

void grx_template_clear(GRX_Template * tmpl) {
  if (!tmpl) {
    return;
  }
  grx_arena_clear(&tmpl->ops);
  tmpl->text = NULL;
  tmpl->length = 0;
}

// --------------------------------------------------------------------------
// Building the result
// --------------------------------------------------------------------------

/** Append bytes to a byte arena. */
static GRX_Result push(GRX_Arena * out, const char * bytes, size_t length) {
  if (!length) {
    return GRX_OK;
  }
  GRX_Result result = grx_arena_reserve(out, out->count + length);
  if (result != GRX_OK) {
    return result;
  }
  memcpy((char *)out->data + out->count, bytes, length);
  out->count += length;
  return GRX_OK;
}


/**
 * @brief The case state a template's `\u`, `\U` and kin leave behind.
 *
 * `run` and `one` because vim keeps two: a one-character modifier suspends a
 * run for exactly one character and the run resumes after it - `\Uab\lcd` is
 * "ABcD" there. All three are cleared together by `\E` and `\e`.
 *
 * `quote` is a third field and not a third value of the other two, because
 * `\Q` **composes** with a case run instead of replacing one: `\Q\U$1` and
 * `\U\Q$1` both quote and upper-case, measured both ways round against the
 * pinned perl. `full` is not a marker at all - it is the dialect's, from
 * GRX_TMPL_CASE_FULL - and it is here rather than passed separately so that
 * push_cased() has one argument to read.
 */
/**
 * How deep the markers nest before the applier stops tracking them.
 *
 * The two dimensions - a case run (`\U` or `\L`) and quoting (`\Q`) - nest
 * differently, which took eight templates put to perl to establish and is not
 * what a summary of the construct would say:
 *
 * - **A case run replaces a case run.** `\Q\U$1$2\U$1\E$2` is "ABAbABAb":
 *   the second `\U` did not deepen anything, so the `\E` left only the `\Q`
 *   and `$2` came out lower. `\Uab\Lcd\Eef` is "ABcdef" for the same reason.
 * - **A `\Q` stacks with a `\Q`.** `\Q$1\Q$1\E.x` keeps quoting after the
 *   `\E`, so two were in force and one was ended.
 *
 * So the depth is bounded only by how many `\Q`s a template writes, and this
 * is the cap. A marker past it is *applied* without being pushed, so the
 * matching `\E` ends the entry below - a wrong answer for a template with
 * sixteen unclosed `\Q`s, and the alternative is refusing one at apply time
 * where there is no diagnostic to carry it. No generator here writes such a
 * template and no corpus holds one.
 */
#define GRX_TPL_CASE_DEPTH 16

/** Whether this marker is a case run rather than a `\Q`. */
static int is_case_run(uint8_t marker) {
  return marker == GRX_TPL_CASE_UPPER_RUN || marker == GRX_TPL_CASE_LOWER_RUN
      || marker == GRX_TPL_CASE_FOLD_RUN;
}

typedef struct CaseState {
  uint8_t one;   ///< GRX_TemplateCase: UPPER_ONE, LOWER_ONE, or NONE.
  uint8_t perl;  ///< Perl's operators rather than vim's; see GRX_Template.
  /**
   * The markers in force, most recently armed last, and how many there are.
   *
   * A **stack** because perl's nest and `\E` pops one: `\Uab\Qc.d\Ee.f` is
   * `ABC\.DE.F`, the `\E` having ended the `\Q` and left the `\U` running,
   * and a second `\E` ends that. It is two deep because there are two
   * dimensions and arming one that is in force replaces it; see
   * GRX_TPL_CASE_DEPTH. vim's markers do not nest at all - its `\E` clears
   * everything - so `depth` never exceeds 1 there.
   */
  uint8_t stack[GRX_TPL_CASE_DEPTH];
  uint8_t depth;
} CaseState;

/** The innermost case run in force, or GRX_TPL_CASE_NONE. */
static uint8_t case_run(const CaseState * state) {
  for (size_t i = state->depth; i > 0; i--) {
    uint8_t marker = state->stack[i - 1];
    if (is_case_run(marker)) {
      return marker;
    }
  }
  return GRX_TPL_CASE_NONE;
}

/** Whether a `\Q` is in force anywhere in the stack. */
static int case_quoting(const CaseState * state) {
  for (size_t i = 0; i < state->depth; i++) {
    if (state->stack[i] == GRX_TPL_CASE_QUOTE_RUN) {
      return 1;
    }
  }
  return 0;
}

/**
 * Whether `\Q` puts a backslash in front of this code point.
 *
 * perl's `quotemeta` over an upgraded string escapes every ASCII character
 * that is not a word character and **nothing** above U+007F: `\Q` over
 * "a.b c-d_e" is `a\.b\ c\-d_e`, and over "\u00e9\u00b2!" it is
 * `\u00e9\u00b2\!`. Measured rather than derived from `\w`, which is the
 * set it looks like and is not: `\w` under UCP holds 144,667 code points
 * here and this rule holds 63.
 */
static int quote_needs_backslash(uint32_t codepoint) {
  if (codepoint > 0x7F) {
    return 0;
  }
  return !((codepoint >= '0' && codepoint <= '9')
      || (codepoint >= 'A' && codepoint <= 'Z')
      || (codepoint >= 'a' && codepoint <= 'z') || codepoint == '_');
}

/**
 * Append bytes, changing the case of what passes through.
 *
 * Character by character, because the modifiers count characters: the
 * one-shot is spent on the first code point whatever it is, so `\u\tx` is
 * a tab and then a lowercase "x" in vim - the tab consumed the `\u`. The
 * mapping is the *simple* one, which is what vim applies: U+00DF stays
 * U+00DF under `\u` where its full uppercase would be "SS".
 *
 * With no state set this is push() with a decode in front of it, so every
 * dialect that has no case markers pays nothing: expand() calls this only
 * where the template held one.
 */
static GRX_Result push_cased(CaseState * state, GRX_Arena * out,
    const char * bytes, size_t length) {
  if (!state->depth && !state->one) {
    // Nothing in force, which is every dialect without case markers and every
    // byte of a template that has them before its first one.
    return push(out, bytes, length);
  }
  size_t at = 0;
  while (at < length) {
    uint32_t codepoint = 0;
    size_t width
        = grx_unicode_utf8_decode(bytes + at, length - at, &codepoint);
    if (!width) {
      // Not valid UTF-8, so there is no character here to change the case
      // of. Passed through as the byte it is, which is what every other
      // path in this file does with the subject's bytes.
      GRX_Result result = push(out, bytes + at, 1);
      if (result != GRX_OK) {
        return result;
      }
      at++;
      continue;
    }
    uint8_t apply = state->one ? state->one : case_run(state);
    state->one = GRX_TPL_CASE_NONE;

    // `\Q` first, so that the backslash it inserts is never itself cased -
    // `\Q\U$1` over "a.b" is `A\.B` in perl and not `A\.B` with a capital
    // backslash, which is not a thing, but the ordering also decides that the
    // *escaped* character is the transformed one rather than the other way
    // round. Measured: `\Q\U$1` and `\U\Q$1` agree.
    if (case_quoting(state) && quote_needs_backslash(codepoint)) {
      GRX_Result escaped = push(out, "\\", 1);
      if (escaped != GRX_OK) {
        return escaped;
      }
    }

    // The full mapping where the dialect asks for it. perl's `\U` over U+00DF
    // is "SS" - one character in, two out - and vim's is U+00DF, so the two
    // dialects part here and not in the parser.
    uint32_t mapped[GRX_CASE_TRANSFORM_MAX];
    size_t produced = 0;
    if (apply && state->perl) {
      int mode = apply == GRX_TPL_CASE_UPPER_RUN ? 'U'
          : apply == GRX_TPL_CASE_LOWER_RUN ? 'L'
          : apply == GRX_TPL_CASE_FOLD_RUN ? 'F'
          : apply == GRX_TPL_CASE_UPPER_ONE ? 'u' : 'l';
      produced = grx_unicode_case_transform(codepoint, mode, mapped);
    }
    uint32_t written = codepoint;
    if (produced > 1) {
      GRX_Result result = GRX_OK;
      for (size_t i = 0; i < produced && result == GRX_OK; i++) {
        char encoded[4];
        size_t encoded_width = grx_unicode_utf8_encode(mapped[i], encoded);
        result = push(out, encoded, encoded_width);
      }
      if (result != GRX_OK) {
        return result;
      }
      at += width;
      continue;
    }
    if (produced == 1) {
      written = mapped[0];
    }
    else if (apply == GRX_TPL_CASE_UPPER_ONE
        || apply == GRX_TPL_CASE_UPPER_RUN) {
      written = grx_unicode_upper_simple(codepoint);
    }
    else if (apply == GRX_TPL_CASE_LOWER_ONE
        || apply == GRX_TPL_CASE_LOWER_RUN) {
      written = grx_unicode_lower_simple(codepoint);
    }
    GRX_Result result;
    if (written == codepoint) {
      result = push(out, bytes + at, width);
    }
    else {
      char encoded[4];
      size_t encoded_width = grx_unicode_utf8_encode(written, encoded);
      result = push(out, encoded, encoded_width);
    }
    if (result != GRX_OK) {
      return result;
    }
    at += width;
  }
  return GRX_OK;
}

/** Apply a parsed template to one match. */
static GRX_Result expand(const GRX_Template * tmpl, const GRX_Match * match,
    const char * subject, size_t end, int unset_is_error, GRX_Arena * out) {
  GRX_Capture whole = {GRX_NPOS, GRX_NPOS};
  if (grx_match_span(match, &whole) != GRX_OK || whole.start == GRX_NPOS) {
    return GRX_ERR_INTERNAL;
  }

  // Zero until a GRX_TPL_CASE piece sets it, and then read by every piece
  // that emits text. Per match, which is what vim does: a `\U` does not
  // reach across to the next one.
  CaseState cased;
  memset(&cased, 0, sizeof(cased));
  cased.perl = tmpl->perl_case_ops;

  for (size_t i = 0; i < tmpl->ops.count; i++) {
    const GRX_TemplateOp * op
        = GRX_ARENA_AT(const GRX_TemplateOp, &tmpl->ops, i);
    GRX_Result result = GRX_OK;
    switch ((GRX_TemplateOpKind)op->kind) {
      case GRX_TPL_LITERAL:
        result = push_cased(
            &cased, out, tmpl->text + op->offset, op->length);
        break;
      case GRX_TPL_CASE:
        // Substitutes nothing and changes what the rest of the template
        // writes. A run replaces a run and a one-shot replaces a one-shot,
        // so `\U\Labc` is "abc" and `\u\labc` is "abc"; `\E` and `\e`
        // clear both, so `\u\Ex` is "x".
        if (op->a == GRX_TPL_CASE_NONE) {
          // `\E`. One level in perl, everything in vim - which is the
          // difference a stack exists for; vim's markers replace rather than
          // nest, so its `\E` empties it.
          cased.one = GRX_TPL_CASE_NONE;
          if (cased.perl && cased.depth) {
            cased.depth--;
          }
          else if (!cased.perl) {
            cased.depth = 0;
          }
        }
        else if (op->a == GRX_TPL_CASE_QUOTE_RUN
            || is_case_run((uint8_t)op->a)) {
          if (!cased.perl) {
            // vim's replace rather than nest, and it has no `\Q`: one entry
            // is the whole of its state.
            cased.stack[0] = (uint8_t)op->a;
            cased.depth = 1;
          }
          else {
            // A case run *replaces* any case run already in force and moves to
            // the top, which is what decides which entry an `\E` then ends; a
            // `\Q` always pushes. Both measured; see GRX_TPL_CASE_DEPTH.
            if (is_case_run((uint8_t)op->a)) {
              uint8_t depth = 0;
              for (uint8_t i = 0; i < cased.depth; i++) {
                if (!is_case_run(cased.stack[i])) {
                  cased.stack[depth++] = cased.stack[i];
                }
              }
              cased.depth = depth;
            }
            if (cased.depth < GRX_TPL_CASE_DEPTH) {
              cased.stack[cased.depth++] = (uint8_t)op->a;
            }
            else {
              // Past the cap: applied without being pushed. See the macro.
              cased.stack[GRX_TPL_CASE_DEPTH - 1] = (uint8_t)op->a;
            }
          }
        }
        else if (!(cased.perl && (case_run(&cased) || cased.one))) {
          // A one-shot, unless this is perl and one is already in force.
          //
          // Two ways it can be. A *run*, where perl ignores the one-shot
          // outright: `\Uab\lcd` is "ABCD" there and "ABcD" in vim. And
          // another *one-shot*, where perl keeps the first and vim the last:
          // `\u\lab` is "Ab" in perl - and `\l\uAB` is "aB", and
          // `\u\l\uab` is "Ab", so it is the first and not the
          // upper-caser that wins.
          //
          // Dropped here rather than in push_cased(), so the state never holds
          // a marker the dialect would not act on.
          cased.one = (uint8_t)op->a;
        }
        break;
      case GRX_TPL_CODEPOINT: {
        // The one piece whose bytes are in neither the template nor the
        // subject: a decoded escape. See GRX_TPL_CODEPOINT.
        char encoded[4];
        size_t width = grx_unicode_utf8_encode(op->a, encoded);
        result = push_cased(&cased, out, encoded, width);
        break;
      }
      case GRX_TPL_WHOLE:
        result = push_cased(
            &cased, out, subject + whole.start, whole.end - whole.start);
        break;
      case GRX_TPL_PREFIX:
        result = push_cased(&cased, out, subject, whole.start);
        break;
      case GRX_TPL_SUFFIX:
        result = push_cased(&cased, out, subject + whole.end, end - whole.end);
        break;
      case GRX_TPL_GROUP_NAMED: {
        // Resolved now rather than when the template was read, because one
        // name may belong to several groups and the reference means the
        // first of them that took part. grx_match_group_named() is that
        // rule; the name is a slice of the template text, so nothing was
        // copied to get here.
        char name[256];
        if (op->length >= sizeof(name)) {
          return GRX_ERR_INTERNAL;
        }
        memcpy(name, tmpl->text + op->offset, op->length);
        name[op->length] = '\0';
        GRX_Capture capture = {GRX_NPOS, GRX_NPOS};
        if (grx_match_group_named(match, name, &capture) == GRX_OK
            && capture.start != GRX_NPOS) {
          result = push_cased(&cased, out, subject + capture.start,
              capture.end - capture.start);
        }
        else if (unset_is_error) {
          return GRX_ERR_SYNTAX;
        }
        break;
      }
      case GRX_TPL_SUBJECT:
        // From 0 to `end`, which is what GRX_TPL_PREFIX and GRX_TPL_SUFFIX
        // between them already call the subject: the prefix starts at 0 and
        // the suffix stops at the window's end, so `$_` spanning both is the
        // same stretch and not a third opinion about where the subject is.
        result = push_cased(&cased, out, subject, end);
        break;
      case GRX_TPL_GROUP: {
        GRX_Capture capture = {GRX_NPOS, GRX_NPOS};
        if (grx_match_group(match, op->a, &capture) == GRX_OK
            && capture.start != GRX_NPOS) {
          result = push_cased(&cased, out, subject + capture.start,
              capture.end - capture.start);
        }
        else if (unset_is_error) {
          // PCRE2's default: "requested value is not set". The one template
          // rule that cannot be settled when the template is read, because
          // whether a group participated is a fact about the match.
          return GRX_ERR_SYNTAX;
        }
        // Otherwise a group that did not participate substitutes nothing,
        // which is every other dialect in section 5.11.
        break;
      }
      case GRX_TPL_NOTHING:
      case GRX_TPL_COUNT:
      default:
        break;
    }
    if (result != GRX_OK) {
      return result;
    }
  }
  return GRX_OK;
}

/** Hand a finished byte arena to the caller as a NUL-terminated GRX_Text. */
static GRX_Result publish(GRX_Arena * out, const GRX_Allocator * allocator,
    GRX_Text * out_text) {
  GRX_Result result = grx_arena_reserve(out, out->count + 1);
  if (result != GRX_OK) {
    return result;
  }
  ((char *)out->data)[out->count] = '\0';

  out_text->data = (char *)out->data;
  out_text->length = out->count;
  out_text->allocator = allocator;

  // The arena's storage is now the caller's, so the arena must not free it.
  *out = (GRX_Arena) {0};
  return GRX_OK;
}

GRX_Result grx_regex_replace(const GRX_Regex * regex, const char * subject,
    size_t length, const char * replacement, size_t replacement_length,
    uint32_t flags, const GRX_SearchOptions * options,
    const GRX_Allocator * allocator, GRX_Error * out_error,
    GRX_Text * out_text) {
  if (out_error) {
    grx_error_clear(out_error);
  }
  if (!regex || !out_text || (!subject && length)
      || (!replacement && replacement_length)) {
    return GRX_ERR_INVALID;
  }
  if (flags & ~(uint32_t)(GRX_REPLACE_GLOBAL | GRX_REPLACE_LITERAL)) {
    return GRX_ERR_INVALID;
  }
  if (!allocator) {
    allocator = grx_allocator_default();
  }

  GRX_SearchOptions resolved;
  if (options) {
    resolved = *options;
  }
  else {
    grx_search_options_default(&resolved);
  }
  size_t end = resolved.end == GRX_NPOS ? length : resolved.end;
  if (end > length || resolved.begin > end) {
    return GRX_ERR_INVALID;
  }

  GRX_Profile profile;
  if (grx_syntax_profile(regex->syntax, &profile) != GRX_OK) {
    return GRX_ERR_INVALID;
  }
  if (!(flags & GRX_REPLACE_LITERAL) && !profile.template_spec.sigil) {
    // No template grammar, and two reasons for that which a caller has to be
    // able to tell apart. For most dialects here it is work not yet done -
    // the dialect is named, its row is empty, and the answer is
    // DIALECT_NOT_IMPLEMENTED. For a dialect that is *built* and still has no
    // sigil, the grammar does not exist to be written: RFC 9485 defines a
    // Boolean match over I-Regexp and no substitution at all, so there is no
    // spelling of "the whole match" to support and never will be.
    //
    // Either way the template is refused rather than run under somebody
    // else's grammar, which would quietly turn a literal `$1` into a group
    // reference or the reverse. GRX_REPLACE_LITERAL is how a caller replaces
    // with plain text in a dialect like that, and it is checked first.
    return fail(out_error,
        grx_frontend_for(regex->syntax) ? GRX_DIAG_NOT_IN_DIALECT
                                       : GRX_DIAG_DIALECT_NOT_IMPLEMENTED,
        0, 0);
  }

  GRX_Template tmpl = {0};
  if (flags & GRX_REPLACE_LITERAL) {
    grx_arena_init(&tmpl.ops, allocator, sizeof(GRX_TemplateOp), 0,
        GRX_DIAG_OUT_OF_MEMORY);
    tmpl.text = replacement;
    tmpl.length = replacement_length;
    if (replacement_length
        && emit_literal(&tmpl, 0, replacement_length) != GRX_OK) {
      grx_template_clear(&tmpl);
      return GRX_ERR_OOM;
    }
  }
  else {
    GRX_Result parsed = grx_template_parse(&profile.template_spec, regex,
        replacement, replacement_length, allocator, out_error, &tmpl);
    if (parsed != GRX_OK) {
      return parsed;
    }
  }

  GRX_Arena out;
  grx_arena_init(&out, allocator, 1, 0, GRX_DIAG_OUT_OF_MEMORY);

  GRX_Match * match = NULL;
  GRX_Result result = grx_match_create(regex, allocator, &match);
  if (result != GRX_OK) {
    grx_template_clear(&tmpl);
    return result;
  }

  size_t copied = 0;
  int matched = 0;
  result = grx_regex_search_ex(regex, subject, length, &resolved, match,
      &matched);

  while (result == GRX_OK && matched) {
    GRX_Capture whole = {GRX_NPOS, GRX_NPOS};
    if (grx_match_span(match, &whole) != GRX_OK) {
      result = GRX_ERR_INTERNAL;
      break;
    }
    result = push(&out, subject + copied, whole.start - copied);
    if (result == GRX_OK) {
      result = expand(&tmpl, match, subject, end,
          (profile.template_spec.features & GRX_TMPL_UNSET_ERROR) != 0,
          &out);
      if (result == GRX_ERR_SYNTAX) {
        // expand() has no error structure of its own; the only failure it
        // reports this way is the unset-group rule, and naming it here keeps
        // the diagnostic beside the feature bit that asked for it.
        fail(out_error, GRX_DIAG_TEMPLATE_UNSET_GROUP, 0, 0);
      }
    }
    if (result != GRX_OK) {
      break;
    }
    copied = whole.end;

    if (!(flags & GRX_REPLACE_GLOBAL)) {
      break;
    }
    result = grx_regex_search_next(regex, subject, length, &resolved, match,
        &matched);
  }

  if (result == GRX_OK) {
    result = push(&out, subject + copied, end - copied);
  }
  if (result == GRX_OK) {
    result = publish(&out, allocator, out_text);
  }

  grx_match_destroy(match);
  grx_template_clear(&tmpl);
  grx_arena_clear(&out);
  if (result != GRX_OK && out_error && out_error->diag == GRX_DIAG_NONE) {
    fail(out_error,
        result == GRX_ERR_OOM ? GRX_DIAG_OUT_OF_MEMORY : GRX_DIAG_INTERNAL, 0,
        0);
  }
  return result;
}

// --------------------------------------------------------------------------
// Splitting
// --------------------------------------------------------------------------

/** Append one piece. */
static GRX_Result add_piece(GRX_Arena * pieces, size_t start, size_t stop) {
  GRX_Capture piece = {start, stop};
  return grx_arena_append(pieces, &piece, NULL);
}

GRX_Result grx_regex_split(const GRX_Regex * regex, const char * subject,
    size_t length, size_t limit, const GRX_SearchOptions * options,
    const GRX_Allocator * allocator, GRX_Error * out_error,
    GRX_Split * out_split) {
  if (out_error) {
    grx_error_clear(out_error);
  }
  if (!regex || !out_split || (!subject && length)) {
    return GRX_ERR_INVALID;
  }
  if (!allocator) {
    allocator = grx_allocator_default();
  }

  GRX_SearchOptions resolved;
  if (options) {
    resolved = *options;
  }
  else {
    grx_search_options_default(&resolved);
  }
  size_t end = resolved.end == GRX_NPOS ? length : resolved.end;
  if (end > length || resolved.begin > end) {
    return GRX_ERR_INVALID;
  }

  // Which of the two splits this dialect has. Read from the profile rather
  // than threaded through the program, because splitting is a fact about the
  // library function and not about the compiled pattern.
  GRX_Profile profile;
  if (grx_syntax_profile(regex->syntax, &profile) != GRX_OK) {
    return GRX_ERR_INVALID;
  }
  const int perl_rule = profile.split == GRX_SPLIT_PERL;
  // Python's, which is a third rule and not a blend that either of the other
  // two can be bent into. It is the plain walk: every match separates,
  // including an empty one, with no special case for an empty subject and
  // none for a trailing empty field. `re.split("x*", "")` is `['', '']`
  // where ECMAScript's `"".split(/x*/)` is `[]` and perl's is `()` - three
  // references, three answers, for the shortest question the function has.
  const int python_rule = profile.split == GRX_SPLIT_PYTHON;

  // perlfunc: a split pattern of `/^/` "is treated as if the /m modifier
  // were supplied". Which patterns count is not what that sentence suggests,
  // and perl was asked rather than read: `(?:^)`, `(?:(?:^))` and `(?i)^`
  // all get it, `(^)`, `^|x`, `^a` and `\A` all do not. That is not a test
  // on the source text - it is "the pattern is nothing but a `^`", which is
  // a property of what it compiled to.
  //
  // Two instructions: the assertion and the match. A capturing group adds
  // saves, an alternation adds a split, and `\A` is the same *kind* of
  // assertion but is not a line anchor - the distinction GRX_INST_LINE_ANCHOR
  // exists for, so that NOTBOL can suppress `^` without suppressing `\A`.
  GRX_Regex * multiline = NULL;
  if (perl_rule && !(regex->options & GRX_OPT_MULTILINE)) {
    // Nothing but a `^`: one line-anchor assertion, group 0's two saves, and
    // the match. A capturing group adds saves of its own, an alternation
    // adds a split, and a literal adds a char - any of which makes this
    // false, which is what perl does too.
    int caret_only = 0;
    for (size_t i = 0; i < regex->program.insts.count; i++) {
      const GRX_Inst * inst
          = GRX_ARENA_AT(const GRX_Inst, &regex->program.insts, i);
      if (!inst) {
        caret_only = 0;
        break;
      }
      if (inst->op == GRX_OP_MATCH
          || (inst->op == GRX_OP_SAVE && inst->x < 2)) {
        continue;
      }
      if (inst->op == GRX_OP_ASSERT
          && inst->mode == GRX_ASSERT_START_SUBJECT
          && (inst->flags & GRX_INST_LINE_ANCHOR) && !caret_only) {
        caret_only = 1;
        continue;
      }
      caret_only = 0;
      break;
    }
    if (caret_only) {
      // Compiled again with the flag on rather than patched, so the walk
      // runs an ordinary program. Bounded to a pattern of four instructions,
      // so the cost is bounded with it.
      if (grx_regex_compile_with_allocator("^", 1, regex->syntax,
              regex->options | GRX_OPT_MULTILINE, NULL, allocator, NULL,
              &multiline) == GRX_OK) {
        regex = multiline;
      }
      // A failure here leaves `regex` alone: the caller asked for a split,
      // not for a second compile, and the unsplit answer is better than an
      // error about a pattern they did not write.
    }
  }

  // perl's LIMIT: zero or absent means *no limit* and drops trailing empty
  // fields; a positive one keeps them and makes the last field the unsplit
  // remainder. ECMAScript's `limit` of 0 means no pieces at all, which is the
  // same spelling for the opposite thing.
  const int drop_trailing
      = perl_rule && (limit == 0 || limit == GRX_NPOS);
  // Python's `maxsplit` counts splits and not pieces, and zero means no
  // limit - perl's spelling, against ECMAScript's, where zero means no
  // pieces at all. The field count below is what makes the *remainder*
  // stand as the last field, which is the half Python takes from perl.
  const size_t field_limit
      = ((perl_rule || python_rule) && limit == 0) ? GRX_NPOS : limit;

  GRX_Arena pieces;
  grx_arena_init(&pieces, allocator, sizeof(GRX_Capture), 0,
      GRX_DIAG_OUT_OF_MEMORY);

  GRX_Result result = GRX_OK;
  if (!perl_rule && !python_rule && !limit) {
    // ECMAScript's `split(re, 0)`: no pieces, whatever the subject is.
    goto publish_pieces;
  }

  GRX_Match * match = NULL;
  result = grx_match_create(regex, allocator, &match);
  if (result != GRX_OK) {
    grx_arena_clear(&pieces);
    return result;
  }

  {
    // ECMA-262 22.2.6.14 walks the subject itself rather than iterating
    // matches, because its rule for an empty match is a rule about the
    // *piece boundary*, not about the match: an empty match where the
    // current piece begins is not a separator at all. Written as a
    // search_next() loop that skips such matches, which is the same walk
    // with the position stepping done by the engine.
    size_t piece_start = resolved.begin;
    int matched = 0;
    size_t fields = 0;

    // The empty subject is its own rule, and the two dialects disagree
    // flatly: ECMAScript yields one empty piece unless the pattern matches
    // the empty string, and perl yields nothing whatever the pattern does.
    // Python has no empty-subject rule of its own: the walk below answers
    // it. An empty subject with a pattern that does not match yields the one
    // empty piece the final-field step emits, and one that matches empty
    // yields two - the piece before the separator and the piece after it.
    if (resolved.begin == end && !python_rule) {
      if (!perl_rule) {
        GRX_SearchOptions probe = resolved;
        result = grx_regex_search_ex(regex, subject, length, &probe, match,
            &matched);
        if (result == GRX_OK && !matched) {
          result = add_piece(&pieces, resolved.begin, end);
        }
      }
      goto done;
    }

    result = grx_regex_search_ex(regex, subject, length, &resolved, match,
        &matched);

    while (result == GRX_OK && matched) {
      GRX_Capture whole = {GRX_NPOS, GRX_NPOS};
      if (grx_match_span(match, &whole) != GRX_OK) {
        result = GRX_ERR_INTERNAL;
        break;
      }
      // A separator at or past the end. ECMA-262 22.2.6.14 never looks
      // there - its loop runs while `q < size` - so the trailing piece
      // already covers it. perl does look, and a zero-width match at the very
      // end is a separator there, giving a trailing empty field:
      // `split /$/m, "aab", 2` is ("aab", "") in perl and ["aab"] in node.
      // It shows only with a positive limit, because otherwise perl's own
      // trailing-empty drop removes the field again - which is why the
      // twenty-four hand-written probe cases missed it and the generator
      // found it.
      if (whole.start >= end && !perl_rule && !python_rule) {
        break;
      }
      if (whole.end == piece_start && !python_rule) {
        // An empty match where this piece begins. Not a separator; step on.
        //
        // ECMA-262 22.2.6.14's rule, and perl's. Python has no such rule
        // since 3.7: every match separates, so `re.split("x*", "abc")` is
        // `['', 'a', 'b', 'c', '']` with a leading empty piece that the
        // other two do not produce, and `re.split("x*", "")` is `['', '']`
        // rather than one piece or none. The loop still terminates because
        // grx_regex_search_next() is what advances past an empty match.
        result = grx_regex_search_next(regex, subject, length, &resolved,
            match, &matched);
        continue;
      }

      // perl's LIMIT counts *fields* and not the captures between them, and
      // the last field it produces is the whole unsplit remainder rather than
      // a truncation. So it stops one short and falls out to the trailing
      // piece below, where ECMAScript stops dead at `limit` pieces.
      // Python counts splits the way perl counts fields, and leaves the
      // remainder whole for the same reason, so it takes this break too.
      // perl's LIMIT is a count of *fields*, so it stops one short and lets
      // the trailing piece below be the remainder. Python's `maxsplit` is a
      // count of *splits*, which is the same remainder reached one field
      // later: `re.split(",", "a,b,c", maxsplit=1)` is `['a', 'b,c']` where
      // `split /,/, "a,b,c", 1` is `("a,b,c")`.
      if (perl_rule && field_limit != GRX_NPOS && fields + 1 >= field_limit) {
        break;
      }
      if (python_rule && field_limit != GRX_NPOS && fields >= field_limit) {
        break;
      }

      result = add_piece(&pieces, piece_start, whole.start);
      // The `pieces.count >= limit` stop is ECMAScript's alone: its `limit`
      // counts the *pieces* produced, captures among them, and truncates.
      // Python's counts splits and has already been spent above, and with
      // `limit` zero meaning "no limit" there this test would have stopped
      // the walk after the very first piece - which is what it did.
      if (result != GRX_OK
          || (!perl_rule && !python_rule && pieces.count >= limit)) {
        goto done;
      }
      fields++;

      for (size_t group = 1; group < grx_match_count(match); group++) {
        GRX_Capture capture = {GRX_NPOS, GRX_NPOS};
        grx_match_group(match, group, &capture);
        result = grx_arena_append(&pieces, &capture, NULL);
        if (result != GRX_OK
            || (!perl_rule && !python_rule && pieces.count >= limit)) {
          goto done;
        }
      }

      piece_start = whole.end;
      result = grx_regex_search_next(regex, subject, length, &resolved, match,
          &matched);
    }

    if (result == GRX_OK) {
      result = add_piece(&pieces, piece_start, end);
      fields++;
    }

    // perl drops trailing empties when no limit was given, and it drops
    // *elements* rather than fields: a trailing unset capture goes too, and
    // the walk stops at the first non-empty one whatever kind it is.
    //
    // `split /(,)/, "a,b,,"` keeps its final "," - which reads like "a
    // trailing capture stands" and is not the rule, only that capture being
    // non-empty. `split /(a)|(b)/, "xa"` is the case that says so: with a
    // negative limit perl gives "x", "a", undef, "" and without one it gives
    // "x", "a", having removed the empty field *and* the unset capture
    // behind it.
    if (result == GRX_OK && drop_trailing) {
      while (pieces.count) {
        const GRX_Capture * last
            = GRX_ARENA_AT(const GRX_Capture, &pieces, pieces.count - 1);
        if (!last || (last->start != GRX_NPOS && last->start != last->end)) {
          break;
        }
        pieces.count--;
      }
    }

  done:
    grx_match_destroy(match);
  }

publish_pieces:
  grx_regex_free(multiline);
  if (result != GRX_OK) {
    grx_arena_clear(&pieces);
    if (out_error && out_error->diag == GRX_DIAG_NONE) {
      fail(out_error,
          result == GRX_ERR_OOM ? GRX_DIAG_OUT_OF_MEMORY : GRX_DIAG_INTERNAL,
          0, 0);
    }
    return result;
  }

  out_split->pieces = (GRX_Capture *)pieces.data;
  out_split->count = pieces.count;
  out_split->allocator = allocator;
  return GRX_OK;
}
