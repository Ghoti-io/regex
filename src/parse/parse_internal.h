/**
 * @file
 *
 * Private declarations for the pattern parser: the node type the parse
 * produces, and the parser's own entry point.
 *
 * The node type is here rather than in a public header because its shape is
 * the parser's business. A consumer that wants to walk a pattern gets
 * grx_pattern_dump() and the GRX_NodeKind enum; a consumer that wants more
 * than that is a reason to widen the public API deliberately, not a reason to
 * install this file.
 *
 * Status: stub. The fields below are the minimum the dump and the free path
 * need; the parser's spec will add to them.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRX_SRC_PARSE_PARSE_INTERNAL_H
#define GHOTI_IO_GRX_SRC_PARSE_PARSE_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/pattern.h>
#include <ghoti.io/regex/syntax.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One node of a parsed pattern.
 *
 * Children are held as indices into the pattern's node array rather than as
 * pointers, so that the whole tree is one allocation that can be grown, freed
 * and bounds-checked as a unit.
 */
typedef struct GRX_Node {
  GRX_NodeKind kind;  ///< What this node is.
  uint32_t first_child; ///< Index of the first child, or 0 for none.
  uint32_t next_sibling; ///< Index of the next sibling, or 0 for none.
  uint32_t data;      ///< Kind-specific payload; see the parser.
  uint32_t min;       ///< Lower bound, for GRX_NODE_REPEAT.
  uint32_t max;       ///< Upper bound, for GRX_NODE_REPEAT. 0 means unbounded.
  size_t offset;      ///< Byte offset in the pattern, for error reporting.
} GRX_Node;

/**
 * @brief The parsed pattern.
 *
 * Declared here because the compiler reads it; the public headers see only an
 * opaque GRX_Pattern.
 */
struct GRX_Pattern {
  const GRX_Allocator * allocator; ///< The allocator everything came from.
  GRX_Syntax syntax;               ///< Dialect the text was read in.
  uint32_t options;                ///< GRX_Option bits it was parsed with.
  GRX_Node * nodes;                ///< Node array; index 0 is the root.
  size_t node_count;               ///< Nodes in use.
  size_t capture_count;            ///< Capturing groups, excluding group 0.
};

/**
 * @brief Parse pattern text into a node tree.
 *
 * The entry point the public wrappers call once they have resolved the
 * allocator and the limits.
 *
 * @param pattern The pattern text. May be NULL only when `length` is 0.
 * @param length Length of `pattern` in bytes.
 * @param syntax The dialect to read it in.
 * @param options GRX_Option bits.
 * @param limits Caps to apply. Never NULL here.
 * @param allocator Allocator to use. Never NULL here.
 * @param out_error Receives the failure position and message. May be NULL.
 * @param out_pattern Receives the parsed pattern on success.
 * @return GRX_OK or a failure code.
 */
GRX_Result grx_parse_pattern(const char * pattern, size_t length,
    GRX_Syntax syntax, uint32_t options, const GRX_Limits * limits,
    const GRX_Allocator * allocator, GRX_Error * out_error,
    GRX_Pattern ** out_pattern);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_PARSE_PARSE_INTERNAL_H
