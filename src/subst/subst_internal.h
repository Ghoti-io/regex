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
  GRX_TPL_COUNT        ///< Closes the enum; not a piece.
} GRX_TemplateOpKind;

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
