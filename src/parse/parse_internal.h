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

#include "../core/arena_internal.h"
#include "../core/semantics_internal.h"

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
  GRX_SHORTHAND_NOT_NEWLINE,  ///< `\N`, "any code point that is not a newline".
  GRX_SHORTHAND_COUNT         ///< Closes the enum; not a shorthand.
} GRX_ShorthandKind;

/** @brief A set operation between character classes. */
typedef enum {
  GRX_CLASS_OP_UNION = 0,   ///< Implicit in `[ab]`; explicit nowhere.
  GRX_CLASS_OP_INTERSECT,   ///< `&&`.
  GRX_CLASS_OP_SUBTRACT,    ///< `--`, and .NET's `[a-z-[aeiou]]`.
  GRX_CLASS_OP_SYMDIFF,     ///< Rust's `~~`.
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
  GRX_CLASS_ITEM_COUNT       ///< Closes the enum; not an item kind.
} GRX_ClassItemKind;

/** @brief The item is negated: `[:^alpha:]`, `\P{...}`, `\D`. */
#define GRX_CLASS_ITEM_NEGATED GRX_BIT(0)

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
 * | LOOKAROUND | GRX_LookKind | - | one child |
 * | CONDITIONAL | GRX_CondKind | group number or name offset | HAS_ELSE; children are the condition (for ASSERTION), then, else |
 * | RECURSE | target group number, 0 for the whole pattern | name offset | NAMED, RELATIVE |
 * | CONTROL | GRX_VerbKind | argument name offset, or GRX_INDEX_NONE | - |
 * | OPTIONS | options to set | options to clear | SCOPED; one child when scoped |
 * | CLASS_OP | GRX_ClassOpKind | - | children are the operands |
 * | STRING_SET | first string in `strings` | count of strings | - |
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
