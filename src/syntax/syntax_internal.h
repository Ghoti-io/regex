/**
 * @file
 *
 * Private declarations for the dialect table.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRX_SRC_SYNTAX_SYNTAX_INTERNAL_H
#define GHOTI_IO_GRX_SRC_SYNTAX_SYNTAX_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/syntax.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The dialect table, indexed by GRX_Syntax.
 *
 * The parser reads this rather than switching on the dialect constant, so
 * that a construct is accepted or rejected in exactly one place.
 *
 * @return A table of GRX_SYNTAX_COUNT entries. Never NULL.
 */
const GRX_SyntaxSpec * grx_syntax_spec_table(void);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_SYNTAX_SYNTAX_INTERNAL_H
