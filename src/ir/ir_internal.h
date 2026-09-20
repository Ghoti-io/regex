/**
 * @file
 *
 * The intermediate representation: what a pattern *means*, with the dialect
 * gone.
 *
 * This is the hinge of the whole design (documentation/design.md section 3).
 * Above it, the front ends know everything about dialects and nothing about
 * engines. Below it, the engines know everything about execution and nothing
 * about dialects. The IR is where the first hands over to the second, and the
 * invariant that makes it work is that every dialect-dependent decision has
 * already been made: no file that consumes an IR may include syntax.h, and a
 * check in `make test` enforces it (documentation/testing.md section 6).
 *
 * What lowering resolved, and so what is *not* here:
 *
 * - **Options.** There is no per-node options field, because every option has
 *   become something explicit. Caseless folded literals into classes;
 *   multiline chose between GRX_ASSERT_START_SUBJECT and
 *   GRX_ASSERT_START_LINE; dot-all chose what GRX_IR_ANY excludes; extended
 *   and literal were consumed by the lexer; ungreedy chose a
 *   @ref GRX_RepeatMode. The only survivor is UTF mode, which is a property
 *   of the whole program - it decides whether a step is a byte or a code
 *   point - and so lives on @ref GRX_IR, not on a node.
 * - **Spellings.** `\(` and `(`, `(?P<n>)` and `(?<n>)`, `\d` and
 *   `[[:digit:]]` under UCP: all gone. What is left is a capture, or a class
 *   with those code points in it.
 * - **Dialect rules.** The empty-iteration rule and the capture-reset rule
 *   are fields on a repeat, and the unset-backreference rule is a field on a
 *   backreference, rather than facts an engine would have to look up.
 *
 * The shape is the AST's: an arena of nodes linked by index, so that a tree
 * is one allocation and every link is bounds-checkable.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRX_SRC_IR_IR_INTERNAL_H
#define GHOTI_IO_GRX_SRC_IR_IR_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/core.h>
#include <stdio.h>
#include <stddef.h>
#include <stdint.h>

#include "../charclass/charclass_internal.h"
#include "../core/arena_internal.h"
#include "../core/semantics_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief What one IR node is.
 *
 * Deliberately smaller than @ref GRX_NodeKind: GROUP became CAPTURE or
 * nothing at all, OPTIONS and BRANCH_RESET were resolved away, CLASS_OP was
 * evaluated, and STRING_SET became an alternation of literal sequences.
 */
typedef enum {
  GRX_IR_EMPTY = 0,  ///< Matches the empty string.
  GRX_IR_CHAR,       ///< One exact code point; `a` holds it.
  GRX_IR_CLASS,      ///< One code point from a class; `a` is its index.
  GRX_IR_ANY,        ///< Any code point, less the set `a` names.
  GRX_IR_CONCAT,     ///< Its children in order.
  GRX_IR_ALTERNATE,  ///< One of its children, in priority order.
  GRX_IR_REPEAT,     ///< Its one child, `min` to `max` times.
  GRX_IR_CAPTURE,    ///< Its one child, recording the span as group `a`.
  GRX_IR_BACKREF,    ///< What group `a` matched.
  GRX_IR_ASSERT,     ///< A zero-width assertion; `mode` says which.
  GRX_IR_LOOK,       ///< A lookaround over its one child.
  GRX_IR_ATOMIC,     ///< Its one child, with no backtracking back into it.
  GRX_IR_COND,       ///< A conditional; children are condition, then, else.
  GRX_IR_RECURSE,    ///< Re-enter group `a`, or the whole pattern when 0.
  GRX_IR_KEEP,       ///< Reset the reported start of the match.
  GRX_IR_VERB,       ///< A backtracking control verb; `mode` says which.
  GRX_IR_SCAN,       ///< Match the one child against the substring `a` names.
  GRX_IR_COUNT       ///< Closes the enum; not a node kind.
} GRX_IRKind;

/** @brief BACKREF: compare case-insensitively, using the folded code points. */
#define GRX_IR_CASELESS GRX_BIT(0)
/** @brief COND: an else branch is present. */
#define GRX_IR_HAS_ELSE GRX_BIT(1)
/**
 * @brief The node runs right to left.
 *
 * Set on every node inside a lookbehind body. It is what lets a lookbehind of
 * any length be an ordinary sub-program rather than a second engine: the
 * instructions it compiles to step backwards, and everything else about them
 * is unchanged.
 */
#define GRX_IR_REVERSE GRX_BIT(2)

/**
 * @brief One node of the intermediate representation.
 *
 * `a`, `b`, `mode`, `min` and `max` are the kind-specific payload:
 *
 * | Kind | `a` | `b` | `mode` | other |
 * | --- | --- | --- | --- | --- |
 * | EMPTY, KEEP | - | - | - | - |
 * | CHAR | the code point | - | - | - |
 * | CLASS | class index | - | - | - |
 * | ANY | class index of the excluded set, or GRX_INDEX_NONE for everything | - | - | - |
 * | CONCAT, ALTERNATE | - | - | - | children |
 * | REPEAT | - | - | GRX_RepeatMode | `min`, `max`, `empty_loop`, `capture_reset`; one child |
 * | CAPTURE | group number | name offset, or GRX_INDEX_NONE | - | one child |
 * | BACKREF | group number | - | - | `backref_unset`, CASELESS |
 * | ASSERT | class index for the line or word set, else GRX_INDEX_NONE | - | GRX_AssertKind | - |
 * | LOOK | - | - | GRX_LookKind | one child |
 * | ATOMIC | - | - | - | one child |
 * | COND | group number | - | GRX_CondKind | HAS_ELSE; children |
 * | RECURSE | target group number | - | - | - |
 * | VERB | name offset, or GRX_INDEX_NONE | - | GRX_VerbKind | - |
 */
typedef struct GRX_IRNode {
  GRX_IRKind kind;       ///< What this node is.
  uint32_t flags;        ///< GRX_IR_* bits that apply to this kind.
  uint32_t first_child;  ///< First child, or GRX_INDEX_NONE.
  uint32_t last_child;   ///< Last child, or GRX_INDEX_NONE.
  uint32_t next_sibling; ///< Next sibling, or GRX_INDEX_NONE.
  uint32_t a;            ///< Kind-specific payload; see the table above.
  uint32_t b;            ///< Kind-specific payload; see the table above.
  uint32_t min;          ///< Lower bound, for REPEAT.
  uint32_t max;          ///< Upper bound, for REPEAT; GRX_REPEAT_INF if none.
  uint8_t mode;          ///< The kind's small enum; see the table above.
  uint8_t empty_loop;    ///< GRX_EmptyLoopMode, for REPEAT.
  uint8_t capture_reset; ///< GRX_CaptureResetMode, for REPEAT.
  uint8_t backref_unset; ///< GRX_BackrefUnsetMode, for BACKREF.
  size_t offset;         ///< Byte offset in the original pattern text.
  size_t length;         ///< Bytes spanned in the original pattern text.
} GRX_IRNode;

/**
 * @brief A lowered pattern.
 *
 * Owns its nodes, its canonical classes and its capture names. The names are
 * here rather than in the nodes because the compiled regex keeps them for
 * grx_regex_capture_name(), and copying one table forward is cheaper and
 * harder to get wrong than walking the tree for them.
 */
typedef struct GRX_IR {
  const GRX_Allocator * allocator; ///< The allocator everything came from.
  uint32_t root;                   ///< Root node, or GRX_INDEX_NONE.
  uint32_t flags;                  ///< GRX_PROGRAM_* bits.
  GRX_MatchPreference preference;  ///< Which match a search reports.
  GRX_IterationRule iteration;     ///< Search-all after an empty match.
  GRX_Arena nodes;                 ///< GRX_IRNode.
  GRX_ClassTable classes;          ///< Every canonical class, by index.
  GRX_Arena names;                 ///< char; NUL-terminated, by offset.
  /**
   * The `(*MARK:NAME)` names, as offsets into `names`, one per distinct name.
   *
   * A verb reaches an engine holding an *index* into this rather than a
   * name, because what an engine does with a mark is compare it - the SKIP
   * that looks for it, and nothing else - and comparing an index is what
   * keeps a string out of the matcher. The name itself is needed once, by
   * grx_match_mark(), which has the regex to look it up in.
   */
  GRX_Arena marks;
  /**
   * The group lists of `(*scs:(...)...)`, as length-prefixed runs.
   *
   * A count, then that many group numbers, resolved and checked. The names
   * and the relative forms are gone by here: what an engine needs is "the
   * first of these groups that is set", and a number is the whole of it.
   */
  GRX_Arena scan_lists;
  size_t capture_count;            ///< Capturing groups, excluding group 0.
  /**
   * How long a *variable*-length lookbehind body the dialect allows, or
   * GRX_NPOS when it sets no bound of its own.
   *
   * A dialect fact rather than a caller's limit, and separate from
   * GRX_Limits::max_lookbehind_length for that reason: PCRE2 compiles
   * `(?<=a{256})` because that body is one fixed length, and refuses
   * `(?<=\d{1,256})` because that one is not - so the number bounds the
   * *variation*, not the distance. It reaches an engine through neither:
   * lowering resolves it into an accepted or a refused pattern.
   */
  size_t max_variable_lookbehind;
} GRX_IR;

/**
 * @brief Prepare an empty IR, with its arenas capped by the limits.
 *
 * @param allocator Allocator to use. NULL uses the default.
 * @param limits Caps to apply. Never NULL here.
 * @param out_ir Receives the new IR on success.
 * @return GRX_OK, GRX_ERR_INVALID, or GRX_ERR_OOM.
 */
GRX_Result grx_ir_create(const GRX_Allocator * allocator,
    const GRX_Limits * limits, GRX_IR ** out_ir);

/**
 * @brief Append a node of a kind, with no children and an empty payload.
 *
 * @param ir The IR. NULL is GRX_ERR_INVALID.
 * @param kind The node's kind.
 * @param offset Byte offset in the original pattern text.
 * @param length Bytes spanned in the original pattern text.
 * @param out_index Receives the new node's index. Optional.
 * @return GRX_OK, GRX_ERR_LIMIT, GRX_ERR_OOM, or GRX_ERR_INVALID.
 */
GRX_Result grx_ir_add_node(GRX_IR * ir, GRX_IRKind kind, size_t offset,
    size_t length, uint32_t * out_index);

/**
 * @brief Append `child` to `parent`'s child list.
 *
 * @param ir The IR.
 * @param parent Index of the parent node.
 * @param child Index of the node to append. It must have no siblings yet.
 * @return GRX_OK, or GRX_ERR_INVALID for an index out of range.
 */
GRX_Result grx_ir_add_child(GRX_IR * ir, uint32_t parent, uint32_t child);

/**
 * @brief The node at an index.
 *
 * @param ir The IR.
 * @param index The node index.
 * @return The node, or NULL when the index is out of range. Invalidated by
 *   the next grx_ir_add_node().
 */
GRX_IRNode * grx_ir_node(const GRX_IR * ir, uint32_t index);

/**
 * @brief Copy a capture name into the IR's name table.
 *
 * @param ir The IR.
 * @param name The name. Need not be NUL-terminated.
 * @param length Its length in bytes.
 * @param out_offset Receives the offset to store in a node's payload.
 * @return GRX_OK, GRX_ERR_OOM, or GRX_ERR_INVALID.
 */
GRX_Result grx_ir_add_name(
    GRX_IR * ir, const char * name, size_t length, uint32_t * out_offset);

/**
 * @brief The name at an offset in the IR's name table.
 *
 * @param ir The IR.
 * @param offset The offset, as grx_ir_add_name() returned it.
 * @return The NUL-terminated name, or NULL when the offset is out of range.
 */
const char * grx_ir_name(const GRX_IR * ir, uint32_t offset);

/**
 * @brief Find or add a mark name, and report its index.
 *
 * Deduplicated: two `(*MARK:A)` in one pattern are one mark, so that
 * `(*SKIP:A)` can ask "is this the mark I want" with an integer comparison.
 *
 * @param ir The IR.
 * @param name The name. Need not be NUL-terminated.
 * @param length Its length in bytes.
 * @param out_index Receives the mark's index.
 * @return GRX_OK, GRX_ERR_OOM, or GRX_ERR_INVALID.
 */
GRX_Result grx_ir_add_mark(
    GRX_IR * ir, const char * name, size_t length, uint32_t * out_index);

/**
 * @brief How many distinct mark names the pattern has.
 *
 * @param ir The IR.
 * @return The count, or 0 for a NULL IR.
 */
size_t grx_ir_mark_count(const GRX_IR * ir);

/**
 * @brief The name of one mark.
 *
 * @param ir The IR.
 * @param index The mark index, as grx_ir_add_mark() reported it.
 * @return The NUL-terminated name, or NULL when the index is out of range.
 */
const char * grx_ir_mark_name(const GRX_IR * ir, uint32_t index);

/**
 * @brief Begin a scan list, and report where it starts.
 *
 * The count is written as a placeholder and patched by
 * grx_ir_scan_list_push() as the groups arrive, the way the pattern's string
 * runs are built.
 *
 * @param ir The IR.
 * @param out_offset Receives the run's offset.
 * @return GRX_OK, GRX_ERR_OOM, or GRX_ERR_INVALID.
 */
GRX_Result grx_ir_scan_list_begin(GRX_IR * ir, uint32_t * out_offset);

/**
 * @brief Append one group number to the run begun at `offset`.
 *
 * @param ir The IR.
 * @param offset The run's offset.
 * @param group The group number.
 * @return GRX_OK, GRX_ERR_OOM, or GRX_ERR_INVALID.
 */
GRX_Result grx_ir_scan_list_push(
    GRX_IR * ir, uint32_t offset, uint32_t group);

/**
 * @brief The scan list at an offset.
 *
 * @param ir The IR.
 * @param offset The run's offset.
 * @param out_count Receives how many groups it names. Required.
 * @return The first group number, or NULL when the offset is out of range.
 */
const uint32_t * grx_ir_scan_list(
    const GRX_IR * ir, uint32_t offset, size_t * out_count);

/**
 * @brief Write a human-readable form of the IR.
 *
 * For debugging and for tests; the format is not stable across versions. It
 * is documented in documentation/development.md.
 *
 * @param ir The IR.
 * @param out Destination.
 * @return GRX_OK, or GRX_ERR_INVALID for a NULL argument.
 */
GRX_Result grx_ir_dump(const GRX_IR * ir, FILE * out);

/**
 * @brief Release an IR and everything it owns. NULL is ignored.
 *
 * @param ir The IR.
 */
void grx_ir_free(GRX_IR * ir);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_IR_IR_INTERNAL_H
