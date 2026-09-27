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
 * The parsed pattern: the node type the parse produces, its side tables, and
 * the parser's own entry point.
 *
 * The AST is what the text *says*, with the dialect's spelling resolved but
 * its semantics not yet applied. `\(` in POSIX BRE and `(` in ERE both
 * arrive here as GRX_NODE_GROUP, because that is a spelling difference; but a
 * literal written under `(?i)` is still a literal, a class is still the list
 * of items the user wrote, and `^` is still `^` rather than "start of line"
 * or "start of subject". Applying the dialect's meaning is lowering's job
 * (src/ir), and keeping the two apart is what lets three consumers that are
 * not the engines read this tree: grx_pattern_dump(), the JSON Schema lint,
 * and a future translator between dialects. All three want to know what was
 * written.
 *
 * The node type is here rather than in a public header because its shape is
 * the parser's business. A consumer that wants to walk a pattern gets
 * grx_pattern_dump() and the GRX_NodeKind enum; a consumer that wants more
 * than that is a reason to widen the public API deliberately, not a reason to
 * install this file.
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

#include "../core/arena_internal.h"
#include "../core/core_internal.h"
#include "../core/semantics_internal.h"
#include "../syntax/syntax_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief An anchor, as it was spelled.
 *
 * The AST records the spelling; lowering turns it into a
 * @ref GRX_AssertKind under the dialect's rules, which is where `^` becomes
 * either a subject anchor or a line anchor depending on multiline, and where
 * `$` acquires the dialect's answer to "before a final newline?"
 * (documentation/dialects.md section 5.3).
 */
typedef enum {
  GRX_ANCHOR_CARET = 0,       ///< `^`.
  GRX_ANCHOR_DOLLAR,          ///< `$`.
  GRX_ANCHOR_START_SUBJECT,   ///< `\A`.
  GRX_ANCHOR_END_SUBJECT,     ///< `\z`.
  GRX_ANCHOR_END_BEFORE_NEWLINE, ///< `\Z`.
  GRX_ANCHOR_WORD_BOUNDARY,   ///< `\b`.
  GRX_ANCHOR_NOT_WORD_BOUNDARY, ///< `\B`.
  GRX_ANCHOR_SEARCH_START,    ///< `\G`.
  GRX_ANCHOR_START_BUFFER,    ///< GNU's backtick anchor.
  GRX_ANCHOR_END_BUFFER,      ///< GNU's `\'`.
  GRX_ANCHOR_WORD_START,      ///< GNU's `\<`.
  GRX_ANCHOR_WORD_END,        ///< GNU's `\>`.
  /**
   * Perl's segmentation boundaries: `\b{gcb}` and its three relatives.
   *
   * Written out one spelling per constant rather than a kind plus a negated
   * flag, because that is how the rest of this enum is written and how
   * lowering reads it: `\b` and `\B` are two constants here too.
   */
  GRX_ANCHOR_GRAPHEME_BOUNDARY,     ///< `\b{gcb}`, and its alias `\b{g}`.
  GRX_ANCHOR_NOT_GRAPHEME_BOUNDARY, ///< `\B{gcb}`, `\B{g}`.
  GRX_ANCHOR_WORD_SEG_BOUNDARY,     ///< `\b{wb}`.
  GRX_ANCHOR_NOT_WORD_SEG_BOUNDARY, ///< `\B{wb}`.
  GRX_ANCHOR_SENTENCE_BOUNDARY,     ///< `\b{sb}`.
  GRX_ANCHOR_NOT_SENTENCE_BOUNDARY, ///< `\B{sb}`.
  GRX_ANCHOR_LINE_BOUNDARY,         ///< `\b{lb}`.
  GRX_ANCHOR_NOT_LINE_BOUNDARY,     ///< `\B{lb}`.
  /**
   * Vim's `\%23c`, `\%<23c` and `\%>23c`: the byte column.
   *
   * The one anchor with a payload: `min` and `max` on the node hold the
   * inclusive range of byte offsets it accepts, already converted from
   * vim's one-based column to this library's zero-based offset. Written
   * as a range rather than a number and a comparison because that is one
   * question for the engine to answer instead of three.
   */
  GRX_ANCHOR_BYTE_COLUMN,
  /**
   * Never holds: vim's `\%V`, `\%#` and `\%23l`.
   *
   * Each names something a *buffer* has and a subject has not - the Visual
   * area, the cursor, a line number. vim compiles them all and none of
   * them ever matches over a string, so this carries that answer rather
   * than refusing a pattern vim accepts.
   */
  GRX_ANCHOR_NEVER,
  /**
   * Vim's `\%23v` and its two comparisons: the screen column.
   *
   * `min` and `max` on the node hold the inclusive range, exactly as
   * GRX_ANCHOR_BYTE_COLUMN's do - but these are columns and not offsets,
   * and they are counted from one at both ends, because a column of zero
   * is not a position a subject has.
   */
  GRX_ANCHOR_SCREEN_COLUMN,
  GRX_ANCHOR_COUNT            ///< Closes the enum; not an anchor.
} GRX_AnchorKind;

/**
 * @brief A named set of code points written as an escape.
 *
 * What each of these *contains* is a dialect decision
 * (documentation/dialects.md section 5.9) that lowering resolves against the
 * Unicode tables. The parser only records which one was written.
 */
typedef enum {
  GRX_SHORTHAND_DIGIT = 0,    ///< `\d`.
  GRX_SHORTHAND_NOT_DIGIT,    ///< `\D`.
  GRX_SHORTHAND_WORD,         ///< `\w`.
  GRX_SHORTHAND_NOT_WORD,     ///< `\W`.
  GRX_SHORTHAND_SPACE,        ///< `\s`.
  GRX_SHORTHAND_NOT_SPACE,    ///< `\S`.
  GRX_SHORTHAND_HSPACE,       ///< `\h`.
  GRX_SHORTHAND_NOT_HSPACE,   ///< `\H`.
  GRX_SHORTHAND_VSPACE,       ///< `\v`.
  GRX_SHORTHAND_NOT_VSPACE,   ///< `\V`.
  /**
   * `\N` is deliberately not here.
   *
   * It is "any character that is not a newline", which is GRX_NODE_ANY with
   * the dialect's newline set excluded - the node kind that already carries
   * that set. It had a shorthand of its own for one revision, and the
   * shorthand had no reader: nothing could turn it into a set, because the
   * set it names is not a property of the character but of the dialect.
   */
  GRX_SHORTHAND_COUNT         ///< Closes the enum; not a shorthand.
} GRX_ShorthandKind;

/**
 * @brief What one entry of a `(*scs:(...)...)` group list is.
 *
 * The list reaches the pattern as a run of code points, two per entry: one
 * of these, then either the group number or the offset of the name. Two
 * kinds because a number can be resolved while parsing and a name cannot -
 * `(*scs:(<x>)a)(?<x>a)` names a group the parser has not reached.
 */
#define GRX_SCAN_ENTRY_GROUP 0u
#define GRX_SCAN_ENTRY_NAME 1u

/** @brief A set operation between character classes. */
typedef enum {
  GRX_CLASS_OP_UNION = 0,   ///< Implicit in `[ab]`; explicit nowhere.
  GRX_CLASS_OP_INTERSECT,   ///< `&&`.
  GRX_CLASS_OP_SUBTRACT,    ///< `--`, and .NET's `[a-z-[aeiou]]`.
  GRX_CLASS_OP_SYMDIFF,     ///< Rust's `~~`.
  /**
   * Unary complement: PCRE2's `(?[ ! [a] ])`.
   *
   * The one operation with a single operand, and the only one that needs to
   * know how wide "everything" is - which is why it is an operation here
   * rather than a negation flag on the operand. A flag would have to mean
   * two different things on `[^a]`, where the complement is of the class's
   * own members, and on `!(...)`, where it is of whatever the expression
   * evaluated to.
   */
  GRX_CLASS_OP_COMPLEMENT,
  GRX_CLASS_OP_COUNT        ///< Closes the enum; not an operation.
} GRX_ClassOpKind;

/** @brief What one item inside `[...]` is. */
typedef enum {
  GRX_CLASS_ITEM_SINGLE = 0, ///< One code point; `lo` holds it.
  GRX_CLASS_ITEM_RANGE,      ///< `lo` to `hi`, inclusive.
  GRX_CLASS_ITEM_SHORTHAND,  ///< `\d` and kin; `a` is a GRX_ShorthandKind.
  GRX_CLASS_ITEM_POSIX,      ///< `[:alpha:]`; `a` is a name offset.
  GRX_CLASS_ITEM_PROPERTY,   ///< `\p{...}`; `a` is a name offset.
  GRX_CLASS_ITEM_NESTED,     ///< A nested class; `a` is a node index.
  GRX_CLASS_ITEM_STRING,     ///< A `\q{...}` string; `a` is a string index.
  /**
   * A property of *strings*: `\p{RGI_Emoji}` in `v` mode. `a` is a name
   * offset, resolved again at lowering the way GRX_CLASS_ITEM_PROPERTY is.
   *
   * Separate from PROPERTY because its members are not code points: it
   * lowers to an alternation of literal sequences rather than into a set,
   * and it may not appear under a negation.
   */
  GRX_CLASS_ITEM_STRING_PROPERTY,
  GRX_CLASS_ITEM_COUNT       ///< Closes the enum; not an item kind.
} GRX_ClassItemKind;

/** @brief The item is negated: `[:^alpha:]`, `\P{...}`, `\D`. */
#define GRX_CLASS_ITEM_NEGATED GRX_BIT(0)

/**
 * @brief Caseless matching does not fold this item.
 *
 * Vim is the dialect that needs it, and the rule is about *spelling*: its
 * named classes and its POSIX bracket classes are predicates and are not
 * folded, while an explicit collection is a set of code points and is.
 * Measured against vim 9.1 - `\c[a-z]` matches "A" and `\c[[:lower:]]`,
 * `\c\l` and `\c\L` do not, so the same set written two ways gets two
 * answers and no class-level rule can give both.
 *
 * Set by the front end, which is what knows which spelling it read.
 */
#define GRX_CLASS_ITEM_NO_FOLD GRX_BIT(1)

/** @brief One item inside a character class, as written. */
typedef struct GRX_ClassItem {
  GRX_ClassItemKind kind; ///< What this item is.
  uint32_t flags;         ///< GRX_CLASS_ITEM_* bits.
  uint32_t lo;            ///< First code point, for SINGLE and RANGE.
  uint32_t hi;            ///< Last code point, for RANGE.
  uint32_t a;             ///< Kind-specific payload; see @ref GRX_ClassItemKind.
  size_t offset;          ///< Byte offset in the pattern.
  size_t length;          ///< Bytes spanned, for error reporting.
} GRX_ClassItem;

// Node flags. Which apply is a function of the node's kind; the rest are 0.
#define GRX_NODE_NEGATED GRX_BIT(0)     ///< CLASS: `[^...]`.
#define GRX_NODE_CAPTURING GRX_BIT(1)   ///< GROUP: `(a)` rather than `(?:a)`.
#define GRX_NODE_ATOMIC GRX_BIT(2)      ///< GROUP: `(?>a)`.
#define GRX_NODE_NAMED GRX_BIT(3)       ///< GROUP, BACKREF, RECURSE: by name.
#define GRX_NODE_RELATIVE GRX_BIT(4)    ///< BACKREF, RECURSE: `\g{-1}`, `(?-1)`.
#define GRX_NODE_SCOPED GRX_BIT(5)      ///< OPTIONS: `(?i:a)` rather than `(?i)`.
#define GRX_NODE_HAS_ELSE GRX_BIT(6)    ///< CONDITIONAL: a second branch exists.
/**
 * @brief GROUP: `(*script_run:a)`, whose body must match one script.
 *
 * A flag rather than a kind, because everything else about it is an
 * ordinary non-capturing group: it groups, it can be quantified, and its
 * body is a plain alternation. What it adds is a check on the *text* the
 * body matched, which no other construct here has and which is why it
 * cannot be expressed by lowering to something that already exists.
 *
 * `(*asr:a)` is this flag and GRX_NODE_ATOMIC together, in that nesting:
 * pcre2pattern says `(*asr:...)` is `(*sr:(?>...))` and that putting the
 * atomic group outside instead would not stop backtracking into the run.
 */
#define GRX_NODE_SCRIPT_RUN GRX_BIT(7)

/**
 * @brief One node of a parsed pattern.
 *
 * Children are held as indices into the pattern's node array rather than as
 * pointers, so that the whole tree is one allocation that can be grown, freed
 * and bounds-checked as a unit. @ref GRX_INDEX_NONE means "no node"; index 0
 * is a real node, so 0 cannot serve as the sentinel.
 *
 * `a`, `b`, `min` and `max` are the kind-specific payload:
 *
 * | Kind | `a` | `b` | `min`, `max`, flags |
 * | --- | --- | --- | --- |
 * | EMPTY, ANY, KEEP | - | - | - |
 * | LITERAL | first code point in `literals` | count of code points | - |
 * | CLASS | first item in `class_items` | count of items | NEGATED |
 * | CONCAT, ALTERNATE, BRANCH_RESET | - | - | children are the parts |
 * | REPEAT | GRX_RepeatMode | - | `min`, `max`; one child |
 * | GROUP | group number, 0 when not capturing | name offset, or GRX_INDEX_NONE | CAPTURING, ATOMIC, NAMED |
 * | BACKREF | group number | name offset | NAMED, RELATIVE |
 * | ANCHOR | GRX_AnchorKind | - | - |
 * | LOOKAROUND | GRX_LookKind | - | `min` is a byte bound on a lookbehind, 0 for none; one child |
 * | CONDITIONAL | GRX_CondKind | group number or name offset | HAS_ELSE; children are any callouts written where the condition goes, then the condition (for ASSERTION), then, else |
 * | RECURSE | target group number, 0 for the whole pattern | name offset when NAMED, else the definition's own byte offset or GRX_INDEX_NONE | NAMED, RELATIVE |
 * | CONTROL | GRX_VerbKind | argument name offset, or GRX_INDEX_NONE | - |
 * | OPTIONS | options to set | options to clear | SCOPED; one child when scoped |
 * | CLASS_OP | GRX_ClassOpKind | - | children are the operands |
 * | STRING_SET | first string in `strings` | count of strings | - |
 * | CALLOUT | the number, 0 for a string callout | string offset in `names`, or GRX_INDEX_NONE | `min` is the string's length, `max` its offset in the pattern |
 *
 * CALLOUT is the one non-REPEAT kind that uses `min` and `max`, which is
 * cheaper than a side table for two numbers: a string's length is not its
 * span in the pattern whenever a delimiter was doubled, so neither can be
 * computed from the other. `max` is recorded where the delimiter actually
 * was rather than derived from the spelling of `(?C` afterwards.
 *
 * RECURSE's `b` carries two things because a call names a *definition*, not
 * a number, and the two spellings say so differently. `(?&b)` names one and
 * the name is what has to survive, because the group it names may be written
 * later. `(?-1)` names one by position, and a `(?|...)` can give several
 * definitions the same number - so what survives there is where the
 * definition was written, which is the only thing that tells them apart.
 * CONDITIONAL's `b` is shared between a number and a name for the same
 * reason: one field, and a flag that says how to read it.
 *
 * `\R` is a STRING_SET: it matches one of a fixed set of sequences, which is
 * what that kind is for. `\X` has no representation here yet - it is an
 * unbounded pattern rather than a set, and the tier that specifies it
 * (documentation/plan.md WP-18) adds its node kind then.
 */
typedef struct GRX_Node {
  GRX_NodeKind kind;     ///< What this node is.
  uint32_t flags;        ///< GRX_NODE_* bits that apply to this kind.
  uint32_t first_child;  ///< First child, or GRX_INDEX_NONE.
  uint32_t last_child;   ///< Last child, or GRX_INDEX_NONE; append is O(1).
  uint32_t next_sibling; ///< Next sibling, or GRX_INDEX_NONE.
  uint32_t a;            ///< Kind-specific payload; see the table above.
  uint32_t b;            ///< Kind-specific payload; see the table above.
  uint32_t min;          ///< Lower bound, for REPEAT.
  uint32_t max;          ///< Upper bound, for REPEAT; GRX_REPEAT_INF if none.
  size_t offset;         ///< Byte offset in the pattern, for errors and dumps.
  size_t length;         ///< Bytes spanned in the pattern.
} GRX_Node;

/**
 * @brief The parsed pattern.
 *
 * Declared here because lowering reads it; the public headers see only an
 * opaque GRX_Pattern.
 *
 * The side tables hold what does not fit in a node's fixed-size payload.
 * `names` is a byte arena of NUL-terminated strings addressed by offset, so
 * that a node stores one index for a name of any length. `strings` holds
 * length-prefixed runs of code points: a count, then that many code points.
 */
struct GRX_Pattern {
  const GRX_Allocator * allocator; ///< The allocator everything came from.
  GRX_Syntax syntax;               ///< Dialect the text was read in.
  uint32_t options;                ///< GRX_Option bits it was parsed with.
  uint32_t root;                   ///< Root node, or GRX_INDEX_NONE if empty.
  GRX_Arena nodes;                 ///< GRX_Node.
  GRX_Arena literals;              ///< uint32_t code points.
  GRX_Arena class_items;           ///< GRX_ClassItem.
  GRX_Arena names;                 ///< char; NUL-terminated, by offset.
  GRX_Arena strings;               ///< uint32_t; length-prefixed runs.
  size_t capture_count;            ///< Capturing groups, excluding group 0.
  GRX_PatternLimits limits;        ///< What `(*LIMIT_MATCH=d)` and kin asked.
  /**
   * The newline convention `(*CR)` and kin chose, or GRX_NEWLINES_COUNT.
   *
   * A pattern may name its own, and it decides what `.` refuses and where
   * `^` and `$` hold - so it has to travel from the front end to lowering,
   * which is where the dialect's default would otherwise be the only
   * answer. GRX_NEWLINES_COUNT is "the pattern did not choose".
   */
  GRX_NewlineSet newlines;
  /**
   * The pattern names CR or LF as a literal code point.
   *
   * PCRE2's HASCRORLF, and it suppresses the CRLF skip in an unanchored
   * search (documentation/dialects.md section 5.2). Recorded here rather
   * than derived from the lowered class, because "explicit" is narrower
   * than "can match" and the difference is only visible in the *spelling*:
   * `[\x{0a}-\x{0f}]` counts and `[\x{09}-\x{0f}]` does not, and `\s`
   * contains both CR and LF and counts for neither.
   */
  int has_cr_or_lf;
};

/**
 * @brief Prepare an empty pattern, with every arena capped by the limits.
 *
 * Allocates the GRX_Pattern itself and nothing else; the arenas allocate on
 * their first append.
 *
 * @param allocator Allocator to use. NULL uses the default.
 * @param syntax Dialect the text will be read in.
 * @param options GRX_Option bits it is being parsed with.
 * @param limits Caps to apply. Never NULL here.
 * @param out_pattern Receives the new pattern on success.
 * @return GRX_OK, GRX_ERR_INVALID, or GRX_ERR_OOM.
 */
GRX_Result grx_pattern_create(const GRX_Allocator * allocator,
    GRX_Syntax syntax, uint32_t options, const GRX_Limits * limits,
    GRX_Pattern ** out_pattern);

/**
 * @brief Append a node of a kind, with no children and an empty payload.
 *
 * @param pattern The pattern. NULL is GRX_ERR_INVALID.
 * @param kind The node's kind.
 * @param offset Byte offset in the pattern text.
 * @param length Bytes spanned; may be updated later by grx_node_at().
 * @param out_index Receives the new node's index. Optional.
 * @return GRX_OK, GRX_ERR_LIMIT when max_nodes is reached, GRX_ERR_OOM, or
 *   GRX_ERR_INVALID.
 */
GRX_Result grx_pattern_add_node(GRX_Pattern * pattern, GRX_NodeKind kind,
    size_t offset, size_t length, uint32_t * out_index);

/**
 * @brief Append `child` to `parent`'s child list.
 *
 * @param pattern The pattern.
 * @param parent Index of the parent node.
 * @param child Index of the node to append. It must have no siblings yet.
 * @return GRX_OK, or GRX_ERR_INVALID for an index out of range.
 */
GRX_Result grx_pattern_add_child(
    GRX_Pattern * pattern, uint32_t parent, uint32_t child);

/**
 * @brief The node at an index.
 *
 * @param pattern The pattern.
 * @param index The node index.
 * @return The node, or NULL when the index is out of range. Invalidated by
 *   the next grx_pattern_add_node().
 */
GRX_Node * grx_pattern_node(const GRX_Pattern * pattern, uint32_t index);

/**
 * @brief Copy a name into the pattern's name table.
 *
 * @param pattern The pattern.
 * @param name The name. Need not be NUL-terminated.
 * @param length Its length in bytes.
 * @param out_offset Receives the offset to store in a node's payload.
 * @return GRX_OK, GRX_ERR_OOM, or GRX_ERR_INVALID.
 */
GRX_Result grx_pattern_add_name(GRX_Pattern * pattern, const char * name,
    size_t length, uint32_t * out_offset);

/**
 * @brief The name at an offset in the pattern's name table.
 *
 * @param pattern The pattern.
 * @param offset The offset, as grx_pattern_add_name() returned it.
 * @return The NUL-terminated name, or NULL when the offset is out of range.
 */
const char * grx_pattern_name(const GRX_Pattern * pattern, uint32_t offset);

/**
 * @brief Copy a run of code points into the pattern's string table.
 *
 * What `\q{abc|de}` and `\R` need and a node's fixed payload cannot hold. The
 * run is stored length-prefixed - a count, then that many code points - so
 * that a STRING_SET names its first string and a count and the strings
 * themselves need no second index.
 *
 * @param pattern The pattern.
 * @param points The code points. May be NULL only when `count` is 0.
 * @param count How many.
 * @param out_index Receives the index to store in a node's payload.
 * @return GRX_OK, GRX_ERR_OOM, or GRX_ERR_INVALID.
 */
GRX_Result grx_pattern_add_string(GRX_Pattern * pattern,
    const uint32_t * points, size_t count, uint32_t * out_index);

/**
 * @brief Start a run whose length is not known yet.
 *
 * `\q{a|bcd|}` is read one code point at a time and its alternatives may be
 * any length, so the count is written as a placeholder and patched by
 * grx_pattern_string_push() as points arrive. That is cheaper than the two
 * alternatives - a scratch buffer with an allocator, or a cap on how long a
 * string in a class may be - and neither of those is a limit ECMA-262 has.
 *
 * @param pattern The pattern.
 * @param out_index Receives the run's index.
 * @return GRX_OK, GRX_ERR_OOM, or GRX_ERR_INVALID.
 */
GRX_Result grx_pattern_string_begin(GRX_Pattern * pattern,
    uint32_t * out_index);

/**
 * @brief Append one code point to the run begun at `index`.
 *
 * Only the most recently begun run may be appended to: the runs are packed
 * end to end, so appending to an earlier one would write into a later one's
 * points.
 *
 * @param pattern The pattern.
 * @param index The run's index.
 * @param codepoint The code point.
 * @return GRX_OK, GRX_ERR_OOM, or GRX_ERR_INVALID.
 */
GRX_Result grx_pattern_string_push(
    GRX_Pattern * pattern, uint32_t index, uint32_t codepoint);

/**
 * @brief The run at an index, and the index of the one after it.
 *
 * @param pattern The pattern.
 * @param index The index, as grx_pattern_add_string() returned it.
 * @param out_count Receives the run's length in code points. Required.
 * @param out_next Receives the index of the next run. Optional.
 * @return The first code point, or NULL when the index is out of range. A
 *   run of length 0 returns a non-NULL pointer that must not be read.
 */
const uint32_t * grx_pattern_string(const GRX_Pattern * pattern,
    uint32_t index, size_t * out_count, uint32_t * out_next);

/**
 * @brief The parser's state, and what a hook is handed.
 *
 * One recursive-descent parser serves every dialect, parameterised by the
 * dialect's features, its profile and its hooks (documentation/design.md
 * section 4). This is the state all three share.
 *
 * `group_count` and `named_groups` are filled by a lexical prescan before
 * parsing begins, because some dialects cannot read an escape without them:
 * `\1` in ECMAScript without `u` is a backreference when the pattern has a
 * group 1 and a legacy octal escape otherwise, and `\k` is a named reference
 * only when the pattern has a named group *anywhere*, including after the
 * reference. ECMA-262 does the same two-pass reading for the same reason.
 */
/**
 * @brief One capturing group whose body the parser is currently inside.
 *
 * A linked list down the C stack, innermost first, so that "is group N still
 * open" costs no allocation and is bounded by the nesting the parse already
 * has. Python is the dialect that asks: `re` refuses a reference to a group
 * that has not *closed* yet - `(a\1)` and `((a)\1)` are both "cannot refer
 * to an open group" - where perl and PCRE2 allow both.
 *
 * A high-water mark of the highest group closed so far cannot answer it.
 * `((a)\1)` closes group 2 before the reference and leaves group 1 open, so
 * a mark would stand at 2 and accept a reference to 1 that `re` rejects.
 */
typedef struct GRX_OpenGroup {
  uint32_t number;                    ///< Its capture number.
  const struct GRX_OpenGroup * outer; ///< The group around it, or NULL.
} GRX_OpenGroup;

typedef struct GRX_Parser {
  const char * text;               ///< The pattern text.
  size_t length;                   ///< Its length in bytes.
  size_t position;                 ///< Where the next character is read from.
  GRX_Syntax syntax;               ///< The dialect.
  GRX_SyntaxSpec spec;             ///< Its features.
  GRX_Profile profile;             ///< What its constructs mean.
  const struct GRX_Frontend * frontend; ///< How to read what a table cannot say.
  uint32_t options;                ///< GRX_Option bits in force.
  const GRX_Limits * limits;       ///< Caps to apply. Never NULL.
  GRX_Pattern * pattern;           ///< What is being built.
  GRX_Error * error;               ///< Where a failure is reported. May be NULL.
  size_t depth;                    ///< Nesting, against max_nesting_depth.
  size_t group_count;              ///< Capturing groups the prescan counted.
  size_t groups_opened;            ///< Capturing groups numbered so far.
  /**
   * How many group bodies are open around the current position.
   *
   * Distinct from `depth`, which counts alternation branches and bracket
   * nesting too. This one answers exactly one question: is there a `(` for
   * this `)` to close, or is the `)` an ordinary character - which is what
   * GRX_SyntaxSpec::unmatched_close_is_literal turns on.
   */
  size_t group_depth;
  /** Capturing groups whose bodies enclose the current position. */
  const GRX_OpenGroup * open_groups;
  /**
   * Nothing but global option settings has been read yet.
   *
   * Python is the dialect that asks. `re` accepts `(?i)ab` and
   * `(?i)(?m)ab` and refuses `a(?i)b`, `((?i)a)` and `(?i)(?:a)(?m)b` with
   * "global flags not at the start of the expression" - an unscoped `(?i)`
   * may stand only in a run at the very beginning, with comments allowed
   * among them because a comment is not a term.
   *
   * Cleared by parse_term() when a term that is not one of those is read,
   * which is what makes a comment transparent without a rule of its own:
   * skip_ignorable() eats it before a term is ever built.
   */
  int only_global_flags_so_far;
  int named_groups;                ///< Non-zero if the pattern names a group.
  int in_lookbehind;               ///< Non-zero inside a lookbehind body.
  /**
   * Non-zero inside a lookaround body of any direction.
   *
   * Separate from `in_lookbehind` because two different rules read them.
   * Reverse execution is a property of looking *behind*; PCRE2's refusal of
   * `\K` is a property of being inside a lookaround at all, and `(?=a\Kb)`
   * is refused for the same reason `(?<=a\Kb)` is.
   */
  int in_lookaround;
  /**
   * While inside a quoted run: the offset at which it ends.
   *
   * GRX_NPOS when there is no run in progress, which is always the case for
   * a dialect without GRX_FEATURE_QUOTING.
   *
   * The mechanism is here and the spelling is not, which is the usual split
   * one level down: `\Q` and `\E` are PCRE2's and Java's spelling and
   * belong to their front ends, but "the characters in this span are
   * literals, whatever the shared grammar would otherwise make of them" is
   * something only the shared grammar can act on - it is the shared grammar
   * that decides `*` is a quantifier and `|` ends a branch. A front end
   * opens a run by setting this; the parser closes it by clearing the field
   * when the position reaches it, so the `\E` itself is read as an ordinary
   * escape afterwards and a front end has one rule for it rather than two.
   */
  size_t quote_end;
  /**
   * Non-zero once `(*BSR_ANYCRLF)` has narrowed what `\R` matches.
   *
   * PCRE2's, and the one newline directive whose whole effect is on a
   * construct the *parser* builds: `\R` becomes the literal alternation
   * `(?>\r\n|\n|\r)` rather than the Unicode one. `(*BSR_UNICODE)`
   * clears it again, because both spellings exist and a pattern may name
   * either.
   *
   * A directive may only appear before anything else, so this is set before
   * the first `\R` can be read and there is no ordering question to answer.
   */
  int bsr_anycrlf;
  /**
   * The dialect's own lexical state, meaningless to the shared parser.
   *
   * Vim is why it exists: its *magic level* decides which characters are
   * operators, it is set inside the pattern by `\v`, `\m`, `\M` and
   * `\V`, and it is not scoped to anything - `\v(a\m)b` is "unmatched
   * \(" in vim 9.1, because the `\m` makes the `)` an ordinary character
   * before the group it would have closed is closed.
   *
   * Kept here rather than in a static or in the front end's own struct
   * because the parser is the thing that has a lifetime: one parse, one
   * value, and nothing shared between two parses running at once. The
   * shared parser never reads it; it only passes the parser to the hook
   * that does.
   */
  int dialect_mode;

  /**
   * A second slot of the same kind, for lexical state that is not a level.
   *
   * Vim again, and for one construct: inside `\%[...]` the seven escapes
   * `\v`, `\m`, `\M`, `\V`, `\c`, `\C` and `\Z` are the bare letters
   * rather than what they mean anywhere else - measured one letter at a
   * time over all fifty-two, and those seven are the whole of the list. A
   * member is otherwise an ordinary atom, so the reader is
   * grx_parse_atom() and this is what tells the escape hook where it is.
   */
  int dialect_state;

  /**
   * Perl's source-level case transform: which run is in force.
   *
   * 0, or `'L'`, `'U'` or `'F'` - the operator that opened it. They do not
   * nest and do not stack: a second one replaces the first, and `\E` ends
   * whatever is open. `qr/\Ua\Lb\Ec\E/` is `(?^:Abc)` and perl warns
   * "Useless use of \E" about the second `\E`, which is how one can tell
   * that nothing was left for it to close.
   *
   * Here for the same reason `quote_end` is: what a front end owns is the
   * spelling, and "every literal from here to there is upper-cased" has to
   * outlive the construct it started in. `[\LA]B\Ec` is `(?^:[a]bc)` in
   * perl - the run opens inside a bracket expression and closes three
   * characters after it ends - so a variable local to the class reader
   * cannot hold it.
   */
  int case_mode;
  /**
   * The same, for the two that transform one character rather than a run.
   *
   * 0, or `'l'` or `'u'`. `case_one_at` is the offset it applies to, which
   * is what makes it faithful rather than nearly so: perl's is a pass over
   * *text*, so `\u` upper-cases whatever character comes next even when
   * that character is an operator, and `\u[ab]` is `[ab]` and not `[Ab]`.
   * Keyed on the offset, the transform simply never fires for a character
   * the grammar consumed as something other than a literal - which is the
   * same answer, arrived at without a rule of its own.
   */
  int case_one;
  size_t case_one_at; ///< The offset `case_one` applies to.
} GRX_Parser;

/**
 * @brief Read one atom: no quantifier, no postfix operator.
 *
 * What parse_term() reads before it looks for a multi. Exposed because a
 * dialect can have a construct whose members are atoms and nothing else -
 * Vim's `\%[...]`, where `r\%[ead]` matches "r", "re", "rea" and "read"
 * and a member may be a class or a collection but never a repeat.
 *
 * @param parser The parser.
 * @param out_node Receives the node read.
 * @return GRX_OK, or the failure the atom reader reported.
 */
GRX_Result grx_parse_atom(GRX_Parser * parser, uint32_t * out_node);

/**
 * @brief What a `{` turned out to be.
 *
 * `is_quantifier` is 0 when the dialect says this `{` is a literal, which is
 * what ECMAScript without `u` says about `a{`, `a{1` and `a{,3}`. The parser
 * then rewinds and reads the brace as an ordinary character.
 */
typedef struct GRX_Quantifier {
  uint32_t min;      ///< Lower bound.
  uint32_t max;      ///< Upper bound, or GRX_REPEAT_INF.
  int is_quantifier; ///< 0 when the `{` is a literal after all.
  /**
   * Greedy, lazy or possessive, for a dialect that says so in the operator.
   *
   * Only GRX_Frontend::read_repeat fills this. The shared reading takes the
   * mode from a suffix - `a*?` - which is a second token, and leaves this
   * at GRX_REPEAT_GREEDY. Vim is the dialect that needs it: its lazy repeat
   * is `\{-n,m}`, a minus *inside* the brace, so by the time the operator
   * has been read the mode is already known and there is no suffix to look
   * for.
   */
  GRX_RepeatMode mode;
} GRX_Quantifier;

/**
 * @brief What a group opener turned out to be.
 *
 * The hook has consumed `(` and whatever followed it that identifies the
 * form; the parser then reads the body and the `)`, so that nesting, depth
 * and the unmatched-parenthesis diagnostic live in one place.
 */
typedef struct GRX_GroupOpen {
  GRX_NodeKind kind; ///< GROUP, LOOKAROUND, CONDITIONAL, RECURSE, CONTROL, OPTIONS.
  uint32_t flags;    ///< GRX_NODE_* bits for the node.
  uint32_t a;        ///< Kind-specific payload; see @ref GRX_Node.
  uint32_t b;        ///< Kind-specific payload.
  /**
   * @name The rest of the node's payload
   *
   * Copied onto the node exactly as `a` and `b` are, and named for the
   * fields they land in. A hook that does not use them leaves them zero,
   * which is what every kind but CALLOUT and REPEAT does - and REPEAT is
   * not a group, so CALLOUT is the only hook here that fills them.
   * @{
   */
  uint32_t min;
  uint32_t max;
  /** @} */
  int has_body;      ///< Non-zero when a body and a `)` follow.
  /**
   * Read a body that is not one alternation. NULL for the usual case.
   *
   * A conditional is the construct that needs this: `(?(1)yes|no)` has two
   * branches separated by exactly one `|`, and reading it as an alternation
   * would make `(?(1)a|b|c)` a conditional with three branches rather than
   * the error PCRE2 reports. The hook is handed the node the parser has
   * already built from the fields above and attaches the children itself.
   *
   * The parser still owns the depth accounting and the closing `)`, so that
   * GRX_DIAG_UNMATCHED_OPEN_PAREN and GRX_DIAG_LIMIT_NESTING_DEPTH have one
   * home whatever shape the body has. Only reached when `has_body` is set.
   *
   * @param parser The parser, positioned at the first byte of the body.
   * @param node The node to attach children to.
   * @return GRX_OK with the position on the closing `)`, or a failure.
   */
  GRX_Result (*read_body)(GRX_Parser * parser, uint32_t node);
} GRX_GroupOpen;

/**
 * @brief The lexical rules a table cannot express.
 *
 * Every hook reads text and produces a node or an item. None of them decides
 * what a construct *means*: that is the profile's job, and a hook that
 * consulted GRX_Profile to choose a node kind would be a hook that has to
 * change when a second dialect shares its spelling.
 *
 * A NULL hook is "this dialect is named but not built" and reports
 * GRX_DIAG_DIALECT_NOT_IMPLEMENTED rather than falling back to a default
 * that would accept the wrong language. documentation/design.md section 4:
 * a dialect that accepts everything is a bug.
 */
typedef struct GRX_Frontend {
  const char * name; ///< For diagnostics and dumps.

  /**
   * Read an escape outside a character class, `\` already consumed, and
   * build the node it denotes.
   */
  GRX_Result (*atom_escape)(GRX_Parser * parser, uint32_t * out_node);

  /** Read an escape inside a character class, `\` already consumed. */
  GRX_Result (*class_escape)(GRX_Parser * parser, GRX_ClassItem * out_item);

  /** Read a character class, `[` already consumed, up to and including `]`. */
  GRX_Result (*char_class)(GRX_Parser * parser, uint32_t * out_node);

  /** Read a group opener, `(` already consumed. */
  GRX_Result (*group_open)(GRX_Parser * parser, GRX_GroupOpen * out_open);

  /** Read a `{` quantifier, the brace already consumed. */
  GRX_Result (*brace_quantifier)(
      GRX_Parser * parser, GRX_Quantifier * out_quantifier);

  /**
   * Turn a character with no special meaning into a node.
   *
   * The hook exists because "no special meaning" is itself a dialect rule:
   * `{`, `}` and `]` are literals in ECMAScript without `u` and syntax
   * errors with it, and a dialect that accepted both would be telling a
   * caller their pattern is valid for an engine that rejects it.
   */
  GRX_Result (*literal_atom)(GRX_Parser * parser, uint32_t codepoint,
      size_t offset, size_t length, uint32_t * out_node);

  /**
   * Refuse a quantifier the dialect does not allow on this atom.
   *
   * Which atoms may be repeated is a dialect rule and not a shared one: a
   * lookahead is quantifiable in ECMAScript without `u` and a syntax error
   * with it, a lookbehind is never quantifiable, `^*` is a literal asterisk
   * in POSIX BRE, and Perl rejects `(?=a)*` outright. The parser knows a
   * quantifier has been found and what it is being applied to; the dialect
   * knows whether that is allowed.
   *
   * @param parser The parser.
   * @param node The atom the quantifier would wrap.
   * @param offset Byte offset of the quantifier.
   * @param length Bytes it spans.
   * @return GRX_OK to allow it, or a failure with its diagnostic set.
   */
  GRX_Result (*check_quantifier_target)(GRX_Parser * parser, uint32_t node,
      size_t offset, size_t length);

  /**
   * Whether the quantifier standing here applies to this atom at all.
   * May be NULL, meaning it always does.
   *
   * POSIX's basic RE is why: an `*` that is the first character of the RE or
   * of a subexpression, *after an initial `^` if there is one*, is a literal
   * asterisk. The first half needs nothing - with no atom before it the `*`
   * is read as an atom already - but `^*` does, because by then `^` is an
   * anchor and the parser is about to repeat it. glibc matches "*a" with
   * `^*a`, so the `*` there is a character and not a quantifier.
   *
   * Distinct from check_quantifier_target(), which answers "this quantifier
   * is not allowed here" with a diagnostic. This one answers "that is not a
   * quantifier", and the caller leaves the position alone so the character
   * is read as an atom next. GRX_Quantifier::is_quantifier says the same
   * thing about a `{`.
   *
   * @param parser The parser, positioned at the quantifier character.
   * @param node The atom the quantifier would wrap.
   * @return Non-zero to read it as a quantifier.
   */
  int (*quantifier_applies)(GRX_Parser * parser, uint32_t node);

  /**
   * Consume whatever stands between two atoms and means nothing. May be NULL.
   *
   * Extended mode is why this exists: under `(?x)` an unescaped space and
   * everything from an unescaped `#` to the next newline are not part of the
   * pattern at all. That cannot be done in `literal_atom`, which is only
   * reached once a character has been read and committed to being an atom -
   * `a +` in extended mode is `a+`, and a front end that turned the space
   * into an empty node would make it `a` followed by a repeat of nothing,
   * which matches a different language.
   *
   * Called before an atom, before a quantifier and before the `|` or `)`
   * that ends a branch, and never inside a quoted run.
   *
   * @param parser The parser.
   * @return GRX_OK, or a failure with its diagnostic set.
   */
  GRX_Result (*skip_ignorable)(GRX_Parser * parser);

  /**
   * Whether `op` is written with a leading backslash here. May be NULL.
   *
   * The generalisation of GRX_SyntaxSpec::escaped_specials, which answers
   * the same question for a whole dialect at once: a basic RE spells every
   * operator that nests or joins with a backslash, and an extended one
   * spells none of them that way. Two things that flag cannot say, and Vim
   * says both. Its four *magic levels* move the line one operator at a
   * time - `.` and `[` are bare in `\m` and escaped in `\M`, while `(`,
   * `)` and `|` are escaped in both and bare only in `\v` - and the level
   * is chosen *inside the pattern*, by `\v`, `\m`, `\M` and `\V`, so it
   * is not a property of the dialect at all but of the position.
   *
   * Hence the two parameters. `op` is the operator in its bare spelling,
   * one of `( ) | . [ ^ $ * + ? { &`; the position is the parser's own.
   * Answering non-zero means the operator is `\` followed by that
   * character here, and that the bare character is something else -
   * usually a literal, which is what the dialect's literal_atom() then
   * makes of it.
   *
   * There is a second reason to answer non-zero, and I-Regexp is the dialect
   * with it: the operator may not *exist*. RFC 9485 gives `^` and `$` back
   * to the literal characters and has no anchor at all, so there is no
   * escaped spelling either - but what the parser needs to be told is the
   * same thing, that the bare character is not the operator. The hook is
   * named for the common case and its effect is the general one.
   *
   * NULL is "this dialect does not vary", and the spec flag decides.
   */
  int (*operator_is_escaped)(const GRX_Parser * parser, char op);

  /**
   * Read a repeat operator standing here, instead of the shared reading.
   * May be NULL.
   *
   * The shared reading knows `*`, `+`, `?` and `{m,n}` and takes the lazy
   * or possessive mode from a suffix. Vim's repeats are a different set of
   * tokens - `*`, `\+`, `\=`, `\?`, `\{n,m}` - and its lazy form is
   * `\{-n,m}`, which is neither a suffix nor a second token. A dialect
   * whose operators do not fit the shared shape reads them here and leaves
   * everything after the operator - whether the atom may be repeated, the
   * repeat limits, the node - to the parser, which is the half that is the
   * same everywhere.
   *
   * Sets `out->is_quantifier` to 0, having consumed nothing, when no repeat
   * stands here.
   */
  GRX_Result (*read_repeat)(GRX_Parser * parser, GRX_Quantifier * out);

  /**
   * Let the dialect rewrite the atom just read, from what follows it.
   * May be NULL.
   *
   * Vim's lookaround is *postfix*: `\(foo\)\@=` is a positive lookahead
   * whose body is the group written before it, and `\@!`, `\@<=`, `\@<!`
   * and `\@>` are the other four. Nothing else here is shaped that way -
   * every other dialect writes the assertion as a group opener, where the
   * body has not been read yet - so it cannot be a group_open() hook, and
   * it is not a quantifier either: it changes what the atom *is* rather
   * than how many times it runs.
   *
   * Called after the atom and before any repeat, with `node` the atom the
   * operator would rewrite. A hook that finds no operator leaves `node`
   * alone and returns GRX_OK.
   */
  GRX_Result (*postfix_atom)(GRX_Parser * parser, uint32_t * node);

  /**
   * Settle the options before a byte is read. May be NULL.
   *
   * For an option a pattern can set that applies to text the reader has
   * already passed. Vim's `\c` and `\C` are the case: either one anywhere
   * in the pattern decides caseless matching for the *whole* of it, so
   * `x\ca` matches "XA" in vim 9.1 and `\Ca\cb` matches "AB" - the `\c`
   * wins over an earlier `\C`, and both reach backwards.
   *
   * An option set while reading cannot do that: the pattern's options are
   * fixed when it is created, and a node built before the setting was seen
   * has already been built. So this is handed the raw text instead, before
   * the parser exists, and returns the options to start from.
   *
   * @param text The pattern text. Not NUL-terminated.
   * @param length Its length in bytes.
   * @param options The options so far - the caller's, plus the dialect's
   *   defaults.
   * @return The options to parse with.
   */
  uint32_t (*initial_options)(
      const char * text, size_t length, uint32_t options);

  /** Check what only the finished pattern can show. May be NULL. */
  GRX_Result (*validate)(GRX_Parser * parser);
} GRX_Frontend;

/**
 * @brief The hooks for a dialect, or NULL when it is named but not built.
 *
 * **Seven of the hooks in the table are mandatory**, the parser calling them
 * with no NULL test: `atom_escape`, `class_escape`, `char_class`,
 * `group_open`, `brace_quantifier`, `literal_atom` and
 * `check_quantifier_target`, plus the `name` a diagnostic prints. Every other
 * hook is optional and documented as such where it is declared. A front end
 * that omits a mandatory one crashes rather than falling back to anything,
 * which is what EveryFrontEndFillsTheHooksTheParserCallsUnconditionally in
 * tests/unit/test_parse.cpp is for - a front end may legitimately have
 * nothing to do in one of them and should say so with a stub, the way
 * src/syntax/posix.c's `class_escape` and src/syntax/iregexp.c's
 * `check_quantifier_target` do.
 *
 * @param syntax The dialect.
 * @return Its front end, or NULL.
 */
const GRX_Frontend * grx_frontend_for(GRX_Syntax syntax);

/** @brief The ECMAScript front end. */
extern const GRX_Frontend grx_frontend_ecmascript;

/** @brief The PCRE2 front end. */
extern const GRX_Frontend grx_frontend_pcre;

/** @brief The Perl front end: the PCRE2 rules, less what Perl spells apart. */
extern const GRX_Frontend grx_frontend_perl;

/** @brief CPython's `re`: the Perl-family rules, less what Python lacks. */
extern const GRX_Frontend grx_frontend_python;

/** @brief Vim's four magic levels. */
extern const GRX_Frontend grx_frontend_vim;

/** @brief POSIX and GNU, basic and extended. */
extern const GRX_Frontend grx_frontend_posix_bre;
extern const GRX_Frontend grx_frontend_posix_ere;
extern const GRX_Frontend grx_frontend_gnu_bre;
extern const GRX_Frontend grx_frontend_gnu_ere;

/** @brief I-Regexp, RFC 9485: a checking implementation of Figure 1. */
extern const GRX_Frontend grx_frontend_iregexp;

// --------------------------------------------------------------------------
// The services a hook uses. Declared here so that a front end is a table of
// rules rather than a second parser.
// --------------------------------------------------------------------------

/**
 * @brief Report a failure at a span of the pattern, and return its code.
 *
 * @param parser The parser.
 * @param diag The diagnostic.
 * @param offset Byte offset of the offending construct.
 * @param length Bytes it spans.
 * @return The result code the diagnostic implies, so a hook can
 *   `return grx_parse_fail(...)`.
 */
GRX_Result grx_parse_fail(
    GRX_Parser * parser, GRX_Diag diag, size_t offset, size_t length);

/**
 * @brief Whether the parser has reached the end of the pattern.
 *
 * @param parser The parser.
 * @return Non-zero at the end.
 */
int grx_parse_at_end(const GRX_Parser * parser);

/**
 * @brief The next code point, without consuming it.
 *
 * @param parser The parser.
 * @param out_codepoint Receives the code point. Required.
 * @param out_width Receives its width in bytes. Optional.
 * @return GRX_OK, GRX_ERR_SYNTAX for malformed UTF-8, or GRX_ERR_LIMIT at
 *   the end of the pattern.
 */
GRX_Result grx_parse_peek(
    const GRX_Parser * parser, uint32_t * out_codepoint, size_t * out_width);

/**
 * @brief Consume and return the next code point.
 *
 * @param parser The parser.
 * @param out_codepoint Receives the code point. Required.
 * @return GRX_OK, GRX_ERR_SYNTAX for malformed UTF-8, or GRX_ERR_LIMIT at
 *   the end of the pattern.
 */
GRX_Result grx_parse_take(GRX_Parser * parser, uint32_t * out_codepoint);

/**
 * @brief Consume one byte if it is the one expected.
 *
 * ASCII only, which every construct's punctuation is.
 *
 * @param parser The parser.
 * @param expected The byte.
 * @return Non-zero when it was there and was consumed.
 */
int grx_parse_eat(GRX_Parser * parser, char expected);

/**
 * @brief Append a literal node holding one code point.
 *
 * @param parser The parser.
 * @param codepoint The code point.
 * @param offset Byte offset of the construct that produced it.
 * @param length Bytes it spanned in the pattern.
 * @param out_node Receives the node index.
 * @return GRX_OK or a failure code.
 */
GRX_Result grx_parse_literal_node(GRX_Parser * parser, uint32_t codepoint,
    size_t offset, size_t length, uint32_t * out_node);

/**
 * @brief Add one code point to the literal run a node already holds.
 *
 * For a dialect where a character in the pattern can be longer than one
 * code point: Vim reads a base character and the composing characters
 * after it as one atom, and this is how the atom grows once it has been
 * made. Only the node whose run ends where the literal arena does may
 * grow, which is the node the caller has just made.
 *
 * @param parser The parser.
 * @param node The literal node to extend.
 * @param codepoint The code point to add.
 * @return GRX_OK, or GRX_ERR_INVALID when the node is not a literal whose
 *   run is the last one.
 */
GRX_Result grx_parse_literal_extend(
    GRX_Parser * parser, uint32_t node, uint32_t codepoint);

/**
 * @brief Append an empty character-class node, ready for items.
 *
 * @param parser The parser.
 * @param offset Byte offset in the pattern.
 * @param out_node Receives the node index.
 * @return GRX_OK or a failure code.
 */
GRX_Result grx_parse_class_node(
    GRX_Parser * parser, size_t offset, uint32_t * out_node);

/**
 * @brief Append one item to a class node built by grx_parse_class_node().
 *
 * Items must be added contiguously: a class node names a span of the item
 * table, so a second class opened halfway through would interleave with it.
 *
 * @param parser The parser.
 * @param node The class node.
 * @param item The item.
 * @return GRX_OK or a failure code.
 */
GRX_Result grx_parse_class_add(
    GRX_Parser * parser, uint32_t node, const GRX_ClassItem * item);

/**
 * @brief Append a class node holding a single shorthand, such as `\d`.
 *
 * @param parser The parser.
 * @param shorthand Which shorthand.
 * @param negated Non-zero for the upper-case spelling.
 * @param offset Byte offset in the pattern.
 * @param length Bytes it spans.
 * @param out_node Receives the node index.
 * @return GRX_OK or a failure code.
 */
GRX_Result grx_parse_shorthand_node(GRX_Parser * parser,
    GRX_ShorthandKind shorthand, int negated, size_t offset, size_t length,
    uint32_t * out_node);

/**
 * @brief Parse one branch: a sequence of terms, up to `|`, `)` or the end.
 *
 * What a hook needs when a construct's body is made of branches it must
 * count. A conditional has exactly two and a branch reset numbers each of
 * its own from the same base, and both would be one alternation to
 * grx_parse_alternation() - which is the right answer everywhere else and
 * the wrong one there.
 *
 * @param parser The parser.
 * @param out_node Receives the node index.
 * @return GRX_OK or a failure code.
 */
GRX_Result grx_parse_concatenation(GRX_Parser * parser, uint32_t * out_node);

/**
 * @brief Parse one alternation - the whole grammar below a group.
 *
 * Exposed so that a hook which has read a group opener with a body of its own
 * shape, such as a conditional's two branches, can recurse into the shared
 * grammar rather than reimplementing it.
 *
 * @param parser The parser.
 * @param out_node Receives the node index.
 * @return GRX_OK or a failure code.
 */
GRX_Result grx_parse_alternation(GRX_Parser * parser, uint32_t * out_node);

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
