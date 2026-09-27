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
 * The compiled form of a replacement template.
 *
 * A template is parsed once against the regex's group count and names, and
 * then applied once per match. Parsing per match would be the obvious
 * implementation and the wrong one: a global replace over a large subject
 * would re-lex the same template thousands of times, and - worse - would
 * discover a bad reference on the thousandth match rather than before the
 * first.
 */

#ifndef GHOTI_IO_GRX_SRC_SUBST_SUBST_INTERNAL_H
#define GHOTI_IO_GRX_SRC_SUBST_SUBST_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/subst.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/arena_internal.h"
#include "../syntax/syntax_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief One piece of a parsed template. */
typedef enum {
  GRX_TPL_LITERAL = 0, ///< Bytes of the template itself.
  GRX_TPL_GROUP,       ///< The span group `a` matched, or nothing if unset.
  GRX_TPL_WHOLE,       ///< The whole match.
  GRX_TPL_PREFIX,      ///< The subject before the match.
  GRX_TPL_SUFFIX,      ///< The subject after the match.
  GRX_TPL_SUBJECT,     ///< The whole subject, match and all.
  GRX_TPL_GROUP_NAMED, ///< A group named by `offset`/`length` in the text.
  GRX_TPL_NOTHING,     ///< Substitutes the empty string.
  /**
   * @brief The code point in `a`, encoded as UTF-8.
   *
   * Every other piece of a template is either a slice of the template text
   * or a slice of the subject. Python's is the first grammar here whose
   * escapes *decode*: `\n` in a `re.sub` template is a newline and not the
   * letter, and the byte it stands for is in neither string. The dialects
   * with a `$` sigil have no such escape, and sed's `\n` is the letter n.
   */
  GRX_TPL_CODEPOINT,
  /**
   * @brief Change the case of what follows; `a` is a @ref GRX_TemplateCase.
   *
   * Vim's `\u`, `\U`, `\l`, `\L`, `\E` and `\e`, and the first piece
   * here that substitutes nothing and still changes the result. Every other
   * grammar in section 5.11 builds its output by concatenation alone.
   */
  GRX_TPL_CASE,
  GRX_TPL_COUNT        ///< Closes the enum; not a piece.
} GRX_TemplateOpKind;

/**
 * @brief What a @ref GRX_TPL_CASE piece asks for.
 *
 * Two states rather than one, because vim keeps both at once and they are
 * measured to compose: `\Uab\lcd` is "ABcD" there, so a one-character
 * modifier *suspends* a run for one character rather than ending it. `\E`
 * and `\e` clear both - `\u\Ex` is "x".
 */
typedef enum {
  GRX_TPL_CASE_NONE = 0,  ///< `\E` and `\e`: clear the run and the one-shot.
  GRX_TPL_CASE_UPPER_ONE, ///< `\u`: the next character only.
  GRX_TPL_CASE_LOWER_ONE, ///< `\l`: the next character only.
  GRX_TPL_CASE_UPPER_RUN, ///< `\U`: until a clear.
  GRX_TPL_CASE_LOWER_RUN, ///< `\L`: until a clear.
  /**
   * `\Q`: quote the metacharacters, until a clear.
   *
   * In this enum and not beside it because it is the same *shape* - a state a
   * marker leaves behind that changes what the rest of the template writes -
   * and because `\E` ends it, which is the one thing every member here has in
   * common. It is not a case, and the enum's name is that much wrong; the
   * alternative was a second enum whose only member is this and a second
   * op kind to carry it.
   *
   * It composes with a case run rather than replacing one: `\Q\U$1` and
   * `\U\Q$1` both quote *and* upper-case, measured both ways round.
   */
  /**
   * `\F`: case-fold, until a clear. Perl's, and no other dialect's here.
   *
   * A third *run* beside `\U` and `\L` rather than a variant of `\L`: the
   * fold of U+00DF is "ss" where its lowercase is U+00DF, and the fold of
   * U+0130 is two code points. It replaces a case run and is replaced by one,
   * which is what `\U$1\F$1` over "AB" giving "ABab" says.
   */
  GRX_TPL_CASE_FOLD_RUN,
  GRX_TPL_CASE_QUOTE_RUN,
  /**
   * Closes the enum; not a marker.
   *
   * Needed because GRX_TPL_CASE_NONE is a real answer here - it is what `\E`
   * asks for - so a reader cannot use zero to mean "this letter is not one of
   * mine". perl_case() returns this.
   */
  GRX_TPL_CASE_COUNT
} GRX_TemplateCase;

/**
 * @brief One instruction of a parsed template.
 *
 * LITERAL names a run of the template text by offset and length rather than
 * copying it, so a parsed template allocates once for its ops and never for
 * its bytes.
 */
typedef struct GRX_TemplateOp {
  uint8_t kind;  ///< A @ref GRX_TemplateOpKind.
  uint32_t a;    ///< GROUP: the group index.
  size_t offset; ///< LITERAL, GROUP_NAMED: byte offset into the template.
  size_t length; ///< LITERAL, GROUP_NAMED: bytes.
} GRX_TemplateOp;

/** @brief A parsed template: the ops, and the text they point into. */
typedef struct GRX_Template {
  GRX_Arena ops;         ///< GRX_TemplateOp, in order.
  const char * text;     ///< The template text; borrowed, not owned.
  size_t length;         ///< Its length in bytes.
  /**
   * GRX_TMPL_CASE_FULL, carried from the spec.
   *
   * The one fact about the dialect that applying a template needs and the ops
   * cannot say: `\U` is one op whichever mapping it means. Copied here rather
   * than passed to the applier beside the template, because every caller of
   * the applier already has the template and none of them has the spec.
   */
  /**
   * GRX_TMPL_CASE_ESCAPES, carried from the spec.
   *
   * The one fact about the dialect that applying a template needs and the ops
   * cannot say: `\U` is one op whether it means perl's operator or vim's, and
   * the four ways those differ are on that bit's own documentation. Copied
   * here rather than passed beside the template, because every caller of the
   * applier already has the template and none of them has the spec.
   */
  uint8_t perl_case_ops;
} GRX_Template;

/**
 * @brief Parse a replacement template against a regex.
 *
 * @param spec The dialect's grammar. Never NULL here.
 * @param regex The regex the references are resolved against. Never NULL.
 * @param text The template. May be NULL only when `length` is 0.
 * @param length Its length in bytes.
 * @param allocator Allocator for the ops. Never NULL here.
 * @param out_error Receives the offset and diagnostic on failure. Optional.
 * @param out_template Receives the parsed template; release with
 *   grx_template_clear().
 * @return GRX_OK, GRX_ERR_SYNTAX, GRX_ERR_OOM, or GRX_ERR_INVALID.
 */
GRX_Result grx_template_parse(const GRX_TemplateSpec * spec,
    const GRX_Regex * regex, const char * text, size_t length,
    const GRX_Allocator * allocator, GRX_Error * out_error,
    GRX_Template * out_template);

/**
 * @brief Release a parsed template. NULL is ignored.
 *
 * @param tmpl The template.
 */
void grx_template_clear(GRX_Template * tmpl);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_SUBST_SUBST_INTERNAL_H
