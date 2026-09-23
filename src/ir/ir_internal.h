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
  /**
   * Re-enter group `a`, or the whole pattern when 0.
   *
   * `b` names *which definition* of that group, by the byte offset it was
   * written at, or is GRX_INDEX_NONE for the first one. A `(?|...)` is the
   * only thing that makes those differ, and it makes them differ enough to
   * matter: the three branches of `(?|(?<a>a)|(?<b>b)|(?<c>c))` are one
   * number and three programs.
   */
  GRX_IR_RECURSE,
  GRX_IR_KEEP,       ///< Reset the reported start of the match.
  GRX_IR_VERB,       ///< A backtracking control verb; `mode` says which.
  GRX_IR_SCAN,       ///< Match the one child against the substring `a` names.
  /**
   * Its one child, whose matched text must be a script run.
   *
   * PCRE2's and Perl's `(*script_run:...)`. The only construct here whose
   * condition is on the *text a body consumed* rather than on the text
   * ahead of a position, which is why it is a node kind rather than an
   * assertion: nothing it could lower to already exists.
   *
   * `(*asr:...)` is this with a GRX_IR_ATOMIC as its child, which is the
   * nesting pcre2pattern specifies - an atomic group outside the run would
   * not stop backtracking into it.
   */
  GRX_IR_SCRIPT_RUN,
  /**
   * A run of literal text matched under *full* case folding.
   *
   * Every other caseless construct is a class, because simple folding maps
   * one code point to one code point and a class is exactly the set of the
   * ones that agree. Full folding does not: `ß` folds to "ss", so one
   * pattern character can stand for two subject characters - and `ff` folds
   * to "ff", so two pattern characters can stand for one subject character,
   * the `ﬀ` ligature. Neither fits in a class, and neither fits in a chain
   * of classes, because the two can meet in the middle: `sß` and `ßs` both
   * fold to "sss" and Perl matches one against the other.
   *
   * So the run is matched against its *fold*. `a` names a packed list of
   * edges over the positions 0..`b` of that folded string, where an edge
   * from `i` to `j` carries the class of characters whose full fold is
   * exactly positions `i` to `j`. A path from 0 to `b` is a sequence of
   * subject characters whose folds concatenate to the whole of it, which is
   * the definition of a caseless match under full folding.
   *
   * The edges leaving a position are disjoint - a character has one full
   * fold and so appears on one of them - so the walk is deterministic and
   * there is at most one path for any subject. Codegen emits it as
   * instructions rather than lowering emitting it as nodes, because the
   * paths share their tails and a tree cannot say so: `s` repeated thirty
   * times has a Fibonacci number of paths and thirty-one states.
   */
  GRX_IR_FOLD_RUN,
  /**
   * `(?C1)`, `(?C"text")`: report this position to the caller's function.
   *
   * It survives lowering rather than becoming an EMPTY because it is a
   * *side effect*, and a side effect with no matching behaviour is exactly
   * the kind of node an optimiser deletes. Nothing here may treat it as
   * removable: with a @ref GRX_CalloutFn registered it can also fail the
   * path it stands on, so it is not even reliably zero-width in the sense
   * that matters to analysis.
   */
  GRX_IR_CALLOUT,
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
 * @brief CAPTURE: this group is one branch of a `(?|...)`.
 *
 * The branch reset itself is gone by here - it lowers to an alternation, and
 * the numbering it shared is already in each capture's `a`. What survives is
 * the fact that a reference to that number may mean any of them, which is
 * what stops a lookbehind containing one from being measured: pcre2test
 * refuses `(?|([ab]))...(?<=\1)z` and takes the same lookbehind over a group
 * written once.
 */
#define GRX_IR_BRANCH_RESET GRX_BIT(3)

/**
 * @brief LOOK: a lookbehind whose body runs forwards from a candidate start.
 *
 * The second of the two lookbehind models (documentation/design.md section
 * 3.5.2). GRX_IR_REVERSE runs the body right to left from the current
 * position, which is what ECMA-262 describes and what lets a lookbehind of
 * *any* length cost what its body costs. This one instead tries each start
 * the body's length allows, furthest back first, and requires the body to
 * arrive exactly where the assertion stands.
 *
 * The two differ in three observable places, all of them in the corpus:
 * which candidate a variable-length body prefers - Perl takes the longest,
 * where running the body backwards takes whichever the *body* prefers - what
 * a capture inside such a body holds, and where `(*ACCEPT)` can end it,
 * since running forwards there is a place for the verb to stop.
 *
 * It is affordable only because a dialect that wants it also bounds the
 * body's variation: GRX_IR::max_variable_lookbehind caps the number of
 * candidate starts at 256, so the assertion stays linear in the subject
 * rather than becoming quadratic. That is why the model follows
 * GRX_LookbehindLimit and is not a free choice.
 *
 * Set on the LOOK node; the body is *not* marked GRX_IR_REVERSE. `a` names
 * the body's byte length in GRX_IR::look_spans, filled in by the analysis
 * pass - which is the pass that measures a subtree, and the reason the field
 * is empty until then.
 */
#define GRX_IR_LOOK_FORWARD GRX_BIT(4)

/**
 * @brief LOOK: a negative one keeps what its body captured before failing.
 *
 * The dialect's answer to GRX_NegativeLookCaptures, made explicit on the
 * node - the profile does not reach an engine, and this is a fact about one
 * assertion rather than about the run. Set only on a negative lookaround,
 * because a positive one has no such question.
 */
#define GRX_IR_LOOK_KEEP_CAPTURES GRX_BIT(5)

/**
 * @brief BACKREF: the name it was written with belongs to several groups.
 *
 * `(?J)(?<A>[ab])...\k'A'(?<A>)` - two groups, one name, and which of them
 * the reference means is not decided until the match runs. The number on the
 * node is the first of them, which is what this library matches against; the
 * flag is the part that must not be forgotten, because a length nobody can
 * compute is not a length. Without it pcre2test refuses
 * `(?<A>[ab])...(?<=\k'A')(?<A>)z` and this library measures the lookbehind
 * from the first `A` and takes it.
 *
 * The same thing GRX_IR_BRANCH_RESET says about a number several groups
 * share, said about a name.
 */
#define GRX_IR_AMBIGUOUS_REF GRX_BIT(6)

/**
 * @brief ASSERT: this assertion is a *line* anchor, so NOTBOL/NOTEOL reach it.
 *
 * `^` without multiline and `\A` mean the same position and lower to the
 * same GRX_ASSERT_START_SUBJECT; `$` where it means the end and `\z` lower to
 * the same GRX_ASSERT_END_SUBJECT; `$` before a final newline and `\Z` to the
 * same GRX_ASSERT_END_BEFORE_NEWLINE. Fusing them is right for every search
 * of a whole subject, and GRX_SEARCH_NOTBOL and GRX_SEARCH_NOTEOL are exactly
 * the conditions under which the two stop meaning the same thing:
 * PCRE2_NOTBOL suppresses `^` and says in as many words that it does not
 * affect `\A`, and glibc's REG_NOTBOL suppresses `^` and leaves GNU's
 * `` \` `` alone. Two references, the same rule.
 *
 * So the kind says where the position is and this says which spelling asked
 * for it. Set on everything `^` and `$` produce, including the multiline
 * kinds, which no other spelling reaches.
 */
#define GRX_IR_LINE_ANCHOR GRX_BIT(7)

/**
 * @brief ASSERT: a CR LF pair is one line terminator in this pattern.
 *
 * PCRE2's `(*CRLF)`, `(*ANYCRLF)` and `(*ANY)`. It cannot be carried in the
 * assertion's class, because a class holds code points and this is a
 * sentence about two of them in sequence, so it travels as a flag and the
 * engines read it beside the class.
 *
 * What it changes, measured against pcre2test 10.46 and asymmetric there:
 * `^` does **not** hold between the CR and the LF - `(*ANY)^\n` does not
 * match "a\r\n" - while `$` does, because under `(*ANY)` a lone LF ends a
 * line as well. Under `(*CRLF)`, where no single character ends one, `$`
 * holds before the CR of a pair and nowhere else.
 */
#define GRX_IR_NEWLINE_CRLF GRX_BIT(8)

/**
 * @brief LOOK: this assertion is a conditional's condition, not an atom.
 *
 * It does not *fail* when its body does - it chooses a branch. Everything
 * else about it is an ordinary lookaround, which is why it is a flag and
 * not a kind: the same sub-match, the same body, the same capture rules.
 *
 * Set on the one child of a GRX_IR_COND whose mode is GRX_COND_ASSERTION,
 * and read only by codegen, which turns it into GRX_INST_COND_ELSE.
 */
#define GRX_IR_LOOK_CONDITION GRX_BIT(9)

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
 * | BACKREF | group number | - | - | `backref_unset`, CASELESS, AMBIGUOUS_REF |
 * | ASSERT | class index for the line or word set, else GRX_INDEX_NONE | - | GRX_AssertKind | - |
 * | LOOK | length-span offset, when LOOK_FORWARD | - | GRX_LookKind | LOOK_FORWARD, LOOK_KEEP_CAPTURES; one child |
 * | ATOMIC | - | - | - | one child |
 * | COND | group number, unused for ASSERTION | - | GRX_CondKind | HAS_ELSE; children |
 *
 * COND's children are the branches, except under GRX_COND_ASSERTION where
 * the *first* is the condition - a GRX_IR_LOOK carrying
 * GRX_IR_LOOK_CONDITION - and the branches follow it. That is the shape
 * the parser produced all along; lowering used to rewrite it away.
 * | RECURSE | target group number | the definition's byte offset, or GRX_INDEX_NONE | - | - |
 * | VERB | name offset, or GRX_INDEX_NONE | - | GRX_VerbKind | - |
 * | SCAN | scan-list offset | - | - | one child |
 * | FOLD_RUN | edge-list offset | positions in the folded string | - | - |
 * | CALLOUT | the number | string offset in `names`, or GRX_INDEX_NONE | - | `min` is the string's length, `max` its offset in the pattern |
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
  GRX_SearchStartRule search_start; ///< What `\G` asserts while iterating.
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
  /**
   * The edge lists of GRX_IR_FOLD_RUN, as length-prefixed runs.
   *
   * A count, then that many triples: the position an edge leaves, the
   * position it arrives at, and the index of the class it consumes one
   * character from. Ordered by the position they leave, so codegen can walk
   * a run's states without sorting.
   */
  GRX_Arena fold_runs;
  /**
   * How long the body of a GRX_IR_LOOK_FORWARD lookbehind can be, in bytes.
   *
   * Pairs - a minimum then a maximum - named by the LOOK node's `a`. The
   * candidate starts the engine tries are exactly the positions those two
   * allow, so this is not an optimisation: it is the assertion's definition
   * under the forward model, and the bound that keeps it from scanning the
   * whole subject.
   *
   * A table rather than the node's `a` and `b` because a length is a size_t
   * and those are uint32_t. Truncating would be silent and wrong for
   * `(?<=(?:a{65535}){65535})`, which is a body no subject can be long
   * enough to match but a length the encoding still has to tell the truth
   * about.
   */
  GRX_Arena look_spans;
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

/** @brief One edge of a GRX_IR_FOLD_RUN: what it consumes, and where to. */
typedef struct GRX_IRFoldEdge {
  uint32_t from;  ///< Position in the folded string this edge leaves.
  uint32_t to;    ///< Position it arrives at; always greater than `from`.
  uint32_t class_index; ///< The class one character comes from.
} GRX_IRFoldEdge;

/**
 * @brief Record how long a forward lookbehind's body can be.
 *
 * Appends; the analysis pass runs once over a tree, so a node is measured
 * once and there is no offset to overwrite.
 *
 * @param ir The IR.
 * @param out_offset Receives the pair's offset, for the node's `a`.
 * @param min The body's shortest match, in bytes.
 * @param max Its longest, or GRX_NPOS when unbounded.
 * @return GRX_OK, GRX_ERR_OOM, or GRX_ERR_INVALID.
 */
GRX_Result grx_ir_look_span_set(
    GRX_IR * ir, uint32_t * out_offset, size_t min, size_t max);

/**
 * @brief Read back a span recorded by grx_ir_look_span_set().
 *
 * @param ir The IR.
 * @param offset The LOOK node's `a`.
 * @param out_min Receives the minimum.
 * @param out_max Receives the maximum.
 * @return Non-zero when the offset named a span.
 */
int grx_ir_look_span(const GRX_IR * ir, uint32_t offset, size_t * out_min,
    size_t * out_max);

/**
 * @brief Begin a fold-run edge list, and report where it starts.
 *
 * @param ir The IR.
 * @param out_offset Receives the run's offset.
 * @return GRX_OK, GRX_ERR_OOM, or GRX_ERR_INVALID.
 */
GRX_Result grx_ir_fold_run_begin(GRX_IR * ir, uint32_t * out_offset);

/**
 * @brief Append one edge to the run begun at `offset`.
 *
 * @param ir The IR.
 * @param offset The run's offset.
 * @param edge The edge.
 * @return GRX_OK, GRX_ERR_OOM, or GRX_ERR_INVALID.
 */
GRX_Result grx_ir_fold_run_push(
    GRX_IR * ir, uint32_t offset, GRX_IRFoldEdge edge);

/**
 * @brief The edges of the fold run at an offset.
 *
 * @param ir The IR.
 * @param offset The run's offset.
 * @param out_count Receives how many edges it holds. Required.
 * @param out_edge Receives the edge at `index`. Required.
 * @param index Which edge; ignored when `out_count` alone is wanted.
 * @return Non-zero when `index` named an edge and it was written.
 */
int grx_ir_fold_run_edge(const GRX_IR * ir, uint32_t offset, size_t index,
    GRX_IRFoldEdge * out_edge);

/**
 * @brief How many edges the fold run at an offset holds.
 *
 * @param ir The IR.
 * @param offset The run's offset.
 * @return The count, or 0 when the offset is out of range.
 */
size_t grx_ir_fold_run_count(const GRX_IR * ir, uint32_t offset);

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
