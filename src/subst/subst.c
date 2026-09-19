/**
 * @file
 *
 * Replacement and splitting.
 *
 * Both are loops over grx_regex_search_next(), which is why WP-15 came
 * first: the dialect's rule for what follows an empty match is exactly the
 * rule that decides whether replacing `a*` in "aab" produces "XXbX" or an
 * infinite loop, and it is already written down once.
 *
 * Copyright 2026 by Corey Pennycuff
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
    size_t at, size_t captures, uint32_t * out_group) {
  size_t available = length - at;
  for (size_t take = available < 2 ? available : 2; take >= 1; take--) {
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
    size_t length, size_t at, GRX_TemplateOpKind * out_kind,
    uint32_t * out_group) {
  if (at >= length || text[at] != '<') {
    return 0;
  }
  const char * close = memchr(text + at, '>', length - at);
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
    *out_kind = GRX_TPL_GROUP;
    *out_group = (uint32_t)index;
  }
  else {
    *out_kind = GRX_TPL_NOTHING;
  }
  return name_length + 2;
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

  const size_t captures = grx_regex_capture_count(regex);
  const int named = has_named_groups(regex);
  const char sigil = spec->sigil;

  size_t i = 0;
  size_t literal_from = 0;
  while (i < length) {
    if (text[i] != sigil) {
      i++;
      continue;
    }

    size_t start = i;
    size_t after = i + 1;
    GRX_TemplateOpKind kind = GRX_TPL_COUNT;
    uint32_t group = 0;
    size_t consumed = 0;

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
      else if (c == '<' && (spec->features & GRX_TMPL_NAME_ANGLE)
          && (named || !(spec->features & GRX_TMPL_NAME_NEEDS_NAMED_GROUPS))) {
        consumed = named_reference(regex, text, length, after, &kind, &group);
      }
      else if (c >= '0' && c <= '9' && (spec->features & GRX_TMPL_NUMBER)) {
        consumed
            = digits_naming_a_group(text, length, after, captures, &group);
        if (consumed) {
          kind = GRX_TPL_GROUP;
        }
        else if (spec->missing != GRX_TMPL_MISSING_LITERAL) {
          grx_template_clear(out_template);
          return fail(out_error, GRX_DIAG_TEMPLATE_UNKNOWN_GROUP, start, 2);
        }
      }
    }

    if (!consumed || kind == GRX_TPL_COUNT) {
      // Not a reference. The sigil is ordinary text, which is ECMAScript's
      // rule for every spelling it does not recognise - including `$3` in a
      // pattern with two groups.
      i = after;
      continue;
    }

    GRX_Result result
        = emit_literal(out_template, literal_from, start - literal_from);
    if (result == GRX_OK) {
      result = emit(out_template, kind, group, 0, 0);
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

/** Apply a parsed template to one match. */
static GRX_Result expand(const GRX_Template * tmpl, const GRX_Match * match,
    const char * subject, size_t end, GRX_Arena * out) {
  GRX_Capture whole = {GRX_NPOS, GRX_NPOS};
  if (grx_match_span(match, &whole) != GRX_OK || whole.start == GRX_NPOS) {
    return GRX_ERR_INTERNAL;
  }

  for (size_t i = 0; i < tmpl->ops.count; i++) {
    const GRX_TemplateOp * op
        = GRX_ARENA_AT(const GRX_TemplateOp, &tmpl->ops, i);
    GRX_Result result = GRX_OK;
    switch ((GRX_TemplateOpKind)op->kind) {
      case GRX_TPL_LITERAL:
        result = push(out, tmpl->text + op->offset, op->length);
        break;
      case GRX_TPL_WHOLE:
        result = push(out, subject + whole.start, whole.end - whole.start);
        break;
      case GRX_TPL_PREFIX:
        result = push(out, subject, whole.start);
        break;
      case GRX_TPL_SUFFIX:
        result = push(out, subject + whole.end, end - whole.end);
        break;
      case GRX_TPL_GROUP: {
        GRX_Capture capture = {GRX_NPOS, GRX_NPOS};
        if (grx_match_group(match, op->a, &capture) == GRX_OK
            && capture.start != GRX_NPOS) {
          result = push(
              out, subject + capture.start, capture.end - capture.start);
        }
        // A group that did not participate substitutes nothing, which is
        // every dialect in section 5.11 but PCRE2's default.
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
    // The dialect is named but its template grammar is not written. Refused
    // rather than run under somebody else's grammar, which would quietly
    // turn a literal `$1` into a group reference or the reverse.
    return fail(out_error, GRX_DIAG_DIALECT_NOT_IMPLEMENTED, 0, 0);
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
      result = expand(&tmpl, match, subject, end, &out);
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

  GRX_Arena pieces;
  grx_arena_init(&pieces, allocator, sizeof(GRX_Capture), 0,
      GRX_DIAG_OUT_OF_MEMORY);

  GRX_Result result = GRX_OK;
  if (!limit) {
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

    // The empty subject is its own rule: one empty piece, unless the pattern
    // matches the empty string, in which case none at all.
    if (resolved.begin == end) {
      GRX_SearchOptions probe = resolved;
      result = grx_regex_search_ex(regex, subject, length, &probe, match,
          &matched);
      if (result == GRX_OK && !matched) {
        result = add_piece(&pieces, resolved.begin, end);
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
      // A separator at or past the end is not a separator: the trailing
      // piece already covers it.
      if (whole.start >= end) {
        break;
      }
      if (whole.end == piece_start) {
        // An empty match where this piece begins. Not a separator; step on.
        result = grx_regex_search_next(regex, subject, length, &resolved,
            match, &matched);
        continue;
      }

      result = add_piece(&pieces, piece_start, whole.start);
      if (result != GRX_OK || pieces.count >= limit) {
        goto done;
      }

      for (size_t group = 1; group < grx_match_count(match); group++) {
        GRX_Capture capture = {GRX_NPOS, GRX_NPOS};
        grx_match_group(match, group, &capture);
        result = grx_arena_append(&pieces, &capture, NULL);
        if (result != GRX_OK || pieces.count >= limit) {
          goto done;
        }
      }

      piece_start = whole.end;
      result = grx_regex_search_next(regex, subject, length, &resolved, match,
          &matched);
    }

    if (result == GRX_OK) {
      result = add_piece(&pieces, piece_start, end);
    }

  done:
    grx_match_destroy(match);
  }

publish_pieces:
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
