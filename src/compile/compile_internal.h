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
 * The compiled program: the instruction set, the program that holds it, and
 * the compiled regex that owns the program.
 *
 * One instruction set serves every engine. That is what makes the
 * equivalence invariant meaningful (documentation/design.md section 3.5.4):
 * when the Pike VM and the backtracker disagree about a program, they
 * disagree about the same program, and one of them is wrong about an
 * instruction both of them read.
 *
 * Nothing here knows about dialects. Every dialect-dependent decision was
 * made during lowering and arrives as an explicit opcode, mode or class
 * index; a file under src/compile or src/exec that includes syntax.h has
 * broken the design's central invariant, and `make test` says so.
 */

#ifndef GHOTI_IO_GRX_SRC_COMPILE_COMPILE_INTERNAL_H
#define GHOTI_IO_GRX_SRC_COMPILE_COMPILE_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/compile.h>
#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/pattern.h>
#include <stddef.h>
#include <stdint.h>

#include "../charclass/charclass_internal.h"
#include "../core/arena_internal.h"
#include "../core/semantics_internal.h"
#include "../core/core_internal.h"

/** Declared, not included: codegen takes an IR and the IR does not take a program. */
struct GRX_IR;

#ifdef __cplusplus
extern "C" {
#endif

/** Bytes of a 256-bit byte set; GRX_Facts::first_bytes is the same shape. */
#define GRX_FIRST_BYTES_SIZE 32

/** @brief The most byte ranges a first-byte set may be described by.
 *
 * Two, because that is where the return stops: over the conformance corpus,
 * of the patterns that reach the byte-set scan, 23% are one range and 56%
 * are two - `[A-Za-z]` and its relatives - while a third range buys the
 * remaining 21% at the cost of another compare per lane on every one of the
 * other 79%. A set needing more keeps the bitmap loop, which is never wrong,
 * only slower. */
#define GRX_BYTE_RANGES_MAX 2

/** @brief A byte set expressed as up to two closed ranges, when it is one. */
typedef struct {
  unsigned char lo[GRX_BYTE_RANGES_MAX];
  unsigned char hi[GRX_BYTE_RANGES_MAX];
  unsigned count;   /**< Zero when the set does not decompose this way. */
} GRX_ByteRanges;

/**
 * @brief Describe a 256-bit byte set as up to two closed ranges.
 *
 * @param set A 256-bit bitmap, GRX_FIRST_BYTES_SIZE bytes.
 * @param out_ranges Receives the description; `count` is 0 when there is
 *   none, which includes the empty set.
 * @return Non-zero when the set decomposes, in which case `out_ranges`
 *   describes exactly the same set.
 */
int grx_byte_ranges_from_set(
    const unsigned char * set, GRX_ByteRanges * out_ranges);


/**
 * @brief Longest literal run either prefilter literal will carry.
 *
 * A cap rather than an allocation, because truncating is *sound* for both
 * facts and nothing is gained past it. A prefix of a string every match
 * starts with is still a string every match starts with, and a substring of
 * one every match contains is still one every match contains - so a truncated
 * answer is a weaker true claim, not a wrong one. Sixty-four bytes is already
 * more than any substring search profits from: the scan cost is set by the
 * needle's rarest byte and its length, and both have stopped mattering long
 * before this.
 */
#define GRX_LITERAL_MAX 64

/**
 * @brief One instruction of the compiled program.
 *
 * The first group is what a lockstep NFA simulation can run; a program made
 * only of those is *regular*, and @ref GRX_Facts::is_regular says so. The
 * second group is what it cannot, and an occurrence of any of them is what
 * forces the backtracking engine.
 *
 * Operands, where `x` and `y` are the instruction's two 32-bit fields and
 * `mode` its byte:
 *
 * | Opcode | `x` | `y` | `mode` |
 * | --- | --- | --- | --- |
 * | MATCH | - | - | - |
 * | CHAR | the code point | - | - |
 * | CLASS | class index | - | - |
 * | ANY | class index of the excluded set | - | - |
 * | ANY_NL | - | - | - |
 * | SPLIT | preferred continuation | other continuation | - |
 * | JMP | continuation | - | - |
 * | SAVE | capture slot | - | - |
 * | ASSERT | class index for the line or word set, else GRX_INDEX_NONE | - | GRX_AssertKind |
 * | PROGRESS_SET | register | - | - |
 * | RESET | first capture slot | one past the last | - |
 * | RESET_STALE | the register holding this iteration's start | one group's first slot | - |
 * | PROGRESS_CHECK | register | continuation when the loop must exit | GRX_EmptyLoopMode |
 * | BACKREF | group number | - | GRX_BackrefUnsetMode |
 * | LOOK | length-span offset, or GRX_INDEX_NONE | continuation after the body; with GRX_INST_COND_ELSE, the first of two jumps | GRX_LookKind |
 * | ATOMIC_BEGIN | matching ATOMIC_END | - | - |
 * | ATOMIC_END | - | - | - |
 * | COND | group number | continuation for the false branch | GRX_CondKind |
 * | CALL | first instruction of the target | the group being called | - |
 * | RET | - | - | - |
 * | KEEP | - | - | - |
 * | VERB | - | - | GRX_VerbKind |
 *
 * A SAVE's slot is twice the group number for a start and twice plus one for
 * an end, so group 0's start is slot 0 and the whole match is slots 0 and 1.
 */
typedef enum {
  GRX_OP_MATCH = 0,    ///< The match succeeded.
  GRX_OP_CHAR,         ///< Consume one specific code point.
  GRX_OP_CLASS,        ///< Consume one code point in a class.
  GRX_OP_ANY,          ///< Consume one code point outside the excluded set.
  GRX_OP_ANY_NL,       ///< Consume any code point at all.
  GRX_OP_SPLIT,        ///< Try two continuations, in priority order.
  GRX_OP_JMP,          ///< Continue at another instruction.
  GRX_OP_SAVE,         ///< Record a capture boundary.
  GRX_OP_ASSERT,       ///< A zero-width assertion.
  /**
   * Record the current position in register `x`.
   *
   * Two readers, and neither owns it. `GRX_OP_PROGRESS_CHECK` reads it at a
   * loop's end to apply the empty-iteration rule, which is what the name
   * comes from; `GRX_OP_SCRIPT_RUN` reads it to know where the run it is
   * checking began. Both want the same thing - "where were we" - and a
   * second opcode that recorded a position would be this one with another
   * name, including the undo frame that puts it back on a backtrack.
   */
  GRX_OP_PROGRESS_SET,
  /**
   * @brief Clear a span of capture slots.
   *
   * The dialect's capture-reset rule, made explicit. ECMA-262's RepeatMatcher
   * step 4 clears every capture inside a repeated group at the start of each
   * iteration, so that `((a)|b)+` against "ab" reports group 2 as unset;
   * Perl's loop does not, and reports it as "a". Emitted only where the
   * profile asks for it, so a dialect that keeps its captures pays nothing.
   *
   * This opcode is not in documentation/design.md's first instruction table.
   * It was added when the rule turned out to have no other representation:
   * the alternative is a rewrite of the loop body, and a rewrite cannot
   * express "clear these on entry but keep them if the loop exits here".
   */
  GRX_OP_RESET,
  /**
   * @brief Clear one group if this iteration did not set it.
   *
   * The late half of GRX_CAPTURE_RESET_AFTER_EACH. `x` names the register a
   * PROGRESS_SET filled with the position this iteration began at, and `y`
   * is one group's first slot: a group whose start is before that was set by
   * an earlier iteration, and an iteration that ends without setting it
   * takes it away on the way out.
   *
   * One instruction per group rather than a slot range, so that the register
   * fits alongside without a third operand or a side table. A loop's body
   * has as many of these as it has captures, which is the same order as the
   * SAVEs already in it.
   */
  GRX_OP_RESET_STALE,
  GRX_OP_PROGRESS_CHECK, ///< Apply the empty-iteration rule at a loop's end.
  GRX_OP_BACKREF,      ///< Match what a group matched earlier.
  /**
   * Run a sub-program without consuming input.
   *
   * The body begins at the next instruction and ends in a MATCH of its own,
   * exactly as GRX_OP_SCAN's does, so `x` carries something else: the offset
   * of this lookbehind's length span in the program's `look_spans`, or
   * GRX_INDEX_NONE for every lookahead and for a lookbehind whose body runs
   * in reverse. A span is present exactly when the assertion is one the
   * engine matches forwards from a candidate start.
   */
  GRX_OP_LOOK,
  /**
   * Run the sub-program that follows over a captured substring.
   *
   * `x` is the scan list's offset in the program's `scan_lists`; `y` is
   * where the outer program resumes. The body begins at the next
   * instruction, the way a LOOK's does, and ends in its own MATCH.
   */
  GRX_OP_SCAN,
  /**
   * Put the position back where `x`'s register recorded it.
   *
   * What makes a non-atomic lookaround non-atomic: its body is inlined into
   * the program rather than run as a sub-match, so its backtrack points stay
   * on the stack and can be returned to. All that separates the body from
   * ordinary text is that the position is restored afterwards.
   */
  GRX_OP_REWIND,
  GRX_OP_ATOMIC_BEGIN, ///< Start a region whose backtrack points are dropped.
  GRX_OP_ATOMIC_END,   ///< End it, discarding them.
  GRX_OP_COND,         ///< Branch on whether a group participated, and kin.
  GRX_OP_CALL,         ///< Re-enter the program, or one group of it.
  GRX_OP_RET,          ///< Return from a CALL.
  GRX_OP_KEEP,         ///< Reset the reported start of the match.
  /**
   * Pin the reported end of the match to here.
   *
   * Vim's `\ze`, and the mirror of GRX_OP_KEEP: it writes slot 1 where
   * KEEP writes slot 0, through the same undo stack, so a path abandoned
   * after passing one puts the old value back. The program's closing
   * `SAVE 1` then carries GRX_INST_SAVE_IF_UNSET, and so leaves a slot this
   * instruction has already claimed alone - which is what makes the *last*
   * one reached on the winning path the one that decides, with no flag to
   * unwind beside the slot itself.
   */
  GRX_OP_KEEP_END,
  GRX_OP_VERB,         ///< A backtracking control verb.
  /**
   * Fail unless the text from register `x` to here is a script run.
   *
   * PCRE2's and Perl's `(*script_run:...)`. The only instruction whose
   * condition is on text already consumed, which is why no engine but the
   * backtracker runs it: a thread set that merged two paths reaching this
   * instruction has thrown away the one thing it needs to ask.
   *
   * Inside a lookbehind the body runs backwards and the register holds the
   * *later* offset, so the span is taken in whichever order the two come.
   */
  GRX_OP_SCRIPT_RUN,
  /**
   * Report this position to the caller's @ref GRX_CalloutFn.
   *
   * `x` is the index of a @ref GRX_ProgramCallout in the program's
   * `callouts`. The only instruction with an effect outside the match, and
   * the only one whose behaviour depends on the *search* rather than on the
   * program: with no function registered it is a no-op every engine steps
   * over, and with one it runs on the backtracker alone.
   */
  GRX_OP_CALLOUT,
  GRX_OP_COUNT         ///< Closes the enum; not an opcode.
} GRX_Opcode;

/**
 * @brief The instruction runs right to left.
 *
 * Set on every instruction of a lookbehind body. A reverse consuming
 * instruction steps back one code point instead of forward, and that is the
 * whole of what makes lookbehind of arbitrary length work: the body is an
 * ordinary sub-program that happens to run backwards.
 */
#define GRX_INST_REVERSE GRX_BIT(0)

/**
 * @brief This SPLIT is one arm of an alternation, not a quantifier's.
 *
 * Read by one construct: `(*THEN)`, whose whole definition is "advance to the
 * next alternative of the innermost enclosing group". Every SPLIT looks the
 * same to an engine otherwise, and `(a(*THEN)b)*` is the case that says the
 * distinction is needed - the verb has to leave the group, not take another
 * turn of the loop.
 */
#define GRX_INST_ALTERNATION GRX_BIT(1)

/**
 * @brief LOOK: keep what the body captured when the body *fails*.
 *
 * documentation/dialects.md section 5.17, resolved. Perl reports group 1 of
 * `a(?!(b)c)` against "abd" as "b"; pcre2test and Node report it unset,
 * which is what ECMA-262 22.2.2.4 requires. The writes still become undo
 * frames, so backtracking past the whole assertion puts them back - what the
 * flag changes is whether the assertion itself does.
 *
 * Stated in terms of the *body* rather than of a negative assertion because
 * GRX_INST_COND_ELSE has the same question without a sign to hang it on: a
 * conditional's assertion that fails is the branch-choosing form of the
 * same event, and both references treat it the same way.
 */
#define GRX_INST_KEEP_CAPTURES GRX_BIT(2)

/**
 * @brief SAVE: write the slot only where it is still unset.
 *
 * On the program's closing `SAVE 1` alone, and only where the program holds
 * a GRX_OP_KEEP_END. Without it the epilogue would overwrite the end a
 * `\ze` pinned, every time; with it the two cannot both claim the slot and
 * the marker wins, because it ran first.
 */
#define GRX_INST_SAVE_IF_UNSET GRX_BIT(3)

/**
 * @brief BACKREF: `x` is a scan list, not a group number.
 *
 * A name may belong to several groups - `(?J)` in PCRE2, and nothing at all
 * in perl, which allows it by default - and a reference written with that
 * name means *the first of them that is set*. Both references agree on the
 * rule and it is the same one grx_match_group_named() follows:
 * `(?(DEFINE)(?<n>a))(?<n>b)\k<n>` matches "abb" because the DEFINE's group
 * never ran, while `(?<n>a)(?<n>b)\k<n>` does not, because the first group
 * did run and captured "a".
 *
 * Without this the reference resolved to one group at lowering time and the
 * engine compared against that group alone, so a name whose first group was
 * unset could never match anything. GRX_IR_AMBIGUOUS_REF had recorded the
 * situation since it was written and only the analyser read it - the same
 * unread-constant shape `escaped_specials` and GRX_PREFER_LEFTMOST_LONGEST
 * were, and found the same way, by generating patterns rather than reading
 * the grammar.
 */
#define GRX_INST_AMBIGUOUS_REF GRX_BIT(3)

/**
 * @brief ASSERT: a line anchor, which GRX_SEARCH_NOTBOL/NOTEOL suppress.
 *
 * GRX_IR_LINE_ANCHOR carried through codegen; see the reasoning there. An
 * engine reads it beside the kind: the kind says which position, this says
 * whether the caller's "my buffer is a piece of a longer line" flags have
 * anything to say about it.
 */
#define GRX_INST_LINE_ANCHOR GRX_BIT(4)

/**
 * @brief ASSERT: a CR LF pair is one line terminator.
 *
 * GRX_IR_NEWLINE_CRLF carried through codegen; the reasoning is there. The
 * three line assertions read it beside their class, and every other
 * instruction ignores it.
 */
#define GRX_INST_NEWLINE_CRLF GRX_BIT(5)

/**
 * @brief LOOK: this assertion chooses a branch instead of failing.
 *
 * A conditional whose condition is an assertion - `(?(?=A)X|Y)`. The
 * instruction runs its body exactly as any other LOOK does; what changes
 * is where it goes afterwards, and that it never reports failure.
 *
 * **`y` and `y + 1` are a pair of jumps.** The first is taken when the
 * condition holds and the second when it does not, and codegen emits them
 * adjacently and in that order - the same shape GRX_OP_RESET_STALE's
 * register pair has, and for the same reason: two targets and one operand
 * left. In a disassembly the pair reads as the branch it is.
 *
 * It replaces a rewrite. `(?(?=A)X|Y)` was lowered as `(?:(?=A)X|(?!A)Y)`,
 * which is exact and compiles A twice - so A ran twice whenever it failed,
 * and a `(?C...)` in A reported itself twice where pcre2test reports it
 * once. What was paid was time for every pattern and meaning for that one.
 */
#define GRX_INST_COND_ELSE GRX_BIT(6)

/**
 * @brief One compiled instruction.
 *
 * Twelve bytes and fixed-size, so that a program is one array a jump can
 * index rather than a structure it has to walk, and so that a class of any
 * size costs one index here.
 */
typedef struct GRX_Inst {
  uint8_t op;       ///< A @ref GRX_Opcode.
  uint8_t mode;     ///< The opcode's small enum; see the table above.
  uint8_t flags;    ///< GRX_INST_* bits.
  uint8_t reserved; ///< Padding; zero. Keeps the struct's layout explicit.
  uint32_t x;       ///< First operand; see the table above.
  uint32_t y;       ///< Second operand; see the table above.
} GRX_Inst;

/**
 * @brief One `(?C...)`, as an engine needs it.
 *
 * A record rather than the two operands of @ref GRX_Inst, because a callout
 * carries four independent things and an instruction holds two. The offsets
 * are size_t for the reason GRX_IR::look_spans is: a pattern offset is a
 * size_t everywhere else, and truncating one here would be a silent lie in
 * the field a caller uses to point at the pattern.
 */
typedef struct GRX_ProgramCallout {
  size_t pattern_offset; ///< Where the next item begins: past this `)`.
  size_t string_offset;  ///< Where the string's body began, before collapsing.
  size_t string_length;  ///< Its length in bytes.
  uint32_t number;       ///< `(?C7)`; 0 for `(?C)` and for a string callout.
  uint32_t string;       ///< Offset in `callout_strings`, or GRX_INDEX_NONE.
} GRX_ProgramCallout;

/**
 * @brief A compiled program: instructions, the classes they name, and the
 * two properties that belong to the whole of it rather than to any node.
 */
typedef struct GRX_Program {
  GRX_Arena insts;                ///< GRX_Inst; execution starts at index 0.
  GRX_ClassTable classes;         ///< Every class an instruction names.
  /**
   * The group lists `GRX_OP_SCAN` names, as length-prefixed runs.
   *
   * A count, then that many group numbers. Copied out of the IR rather than
   * shared with it, because a program outlives the tree it came from.
   */
  GRX_Arena scan_lists;
  /**
   * The body lengths `GRX_OP_LOOK` names, as minimum-then-maximum pairs.
   *
   * A lookbehind the engine runs forwards has to know which starts to try,
   * and those are exactly the positions its body's length allows. Copied out
   * of the IR for the same reason the scan lists are: a program outlives the
   * tree it came from.
   */
  GRX_Arena look_spans;
  /**
   * The `(?C...)` records a `GRX_OP_CALLOUT`'s `x` indexes.
   *
   * Copied out of the IR rather than shared with it, as the scan lists and
   * the look spans are: a program outlives the tree it came from.
   */
  GRX_Arena callouts;
  /**
   * The bytes of every string callout, NUL-terminated and by offset.
   *
   * A byte arena rather than one `char *` each, so that the strings are one
   * allocation. The terminator is a convenience for a caller printing one;
   * GRX_ProgramCallout::string_length is the authority, because the bytes
   * came from a counted pattern and may include a NUL.
   */
  GRX_Arena callout_strings;
  uint32_t flags;                 ///< GRX_PROGRAM_* bits.
  uint32_t register_count;        ///< Progress registers a thread needs.
  GRX_MatchPreference preference; ///< Which match a search reports.
  GRX_SubmatchRule submatch; ///< Which division of it the groups get.
  GRX_IterationRule iteration;    ///< Search-all after an empty match.
  GRX_SearchStartRule search_start; ///< What `\G` asserts while iterating.
  /**
   * Which bytes a match can begin with, as a 256-bit set; see prefilter.c.
   *
   * Here rather than only in GRX_Facts because the engines are what read it,
   * and they are handed the program. A search that has nothing in flight and
   * has not matched can skip every position whose byte is absent, which is the
   * difference between touching the subject once and running the thread set at
   * every byte of it.
   *
   * Meaningless unless `first_bytes_known`. Unknown is the safe state: it
   * means "check every position", which is what every search did before this
   * existed.
   */
  unsigned char first_bytes[GRX_FIRST_BYTES_SIZE];
  int first_bytes_known;
  /**
   * `first_bytes` as one or two byte ranges, when it is exactly that.
   *
   * A property of the set and therefore of the program, so it is derived once
   * here rather than per search: the scan that reads it runs at every
   * candidate position. `first_byte_ranges.count` of zero means the set does
   * not decompose and the bitmap loop answers, which it always can.
   *
   */
  GRX_ByteRanges first_byte_ranges;
  /**
   * Bytes every match begins with, and bytes every match contains.
   *
   * Here for the same reason the byte set is: the engines read them. The byte
   * set turns a closure per position into a byte test per position, and these
   * two turn it into a substring search - which is the difference the set
   * cannot make for a pattern like `aaaaaaaaab`, where every position passes
   * the byte test and only the tenth passes the string test.
   *
   * Zero length means "not known", which is the same answer as an empty
   * literal would be and is the safe one: check every position.
   */
  char literal_prefix[GRX_LITERAL_MAX];
  size_t literal_prefix_length;
  char required_literal[GRX_LITERAL_MAX];
  size_t required_literal_length;
  /**
   * @brief Where `required_literal` sits, in bytes from the start of a match.
   *
   * A match containing the literal at subject position `p` began somewhere in
   * `[p - required_offset_max, p - required_offset_min]`, so an occurrence
   * names a window of start positions rather than only proving the subject is
   * worth searching at all. `required_offset_max == GRX_NPOS` means unbounded
   * - anything that can repeat without limit before the literal - and leaves
   * only the weaker claim.
   *
   * Both are conservative in the direction that keeps starts: a minimum too
   * small or a maximum too large widens the window, and a wider window
   * contains every position a true one would.
   */
  size_t required_offset_min;
  size_t required_offset_max;
} GRX_Program;

/**
 * @brief A compiled regular expression.
 *
 * Declared here because the engines read it; the public headers see only an
 * opaque GRX_Regex. Immutable once built, which is what lets one be shared
 * between threads: everything that changes during a match lives in
 * @ref GRX_Match.
 */
struct GRX_Regex {
  const GRX_Allocator * allocator; ///< The allocator everything came from.
  GRX_Syntax syntax;               ///< Dialect the pattern was read in.
  uint32_t options;                ///< GRX_Option bits it was compiled with.
  GRX_Program program;             ///< What the engines run.
  GRX_Facts facts;                 ///< What analysis discovered.
  size_t capture_count;            ///< Capturing groups, excluding group 0.
  char ** capture_names;           ///< One per group, NULL where unnamed.
  size_t mark_count;               ///< Distinct `(*MARK:NAME)` names.
  char ** mark_names;              ///< One per mark, indexed as the program.
  GRX_PatternLimits limits;        ///< What `(*LIMIT_MATCH=d)` and kin asked.
};

/**
 * @brief Prepare an empty program. Allocates nothing.
 *
 * @param program The program. NULL is ignored.
 * @param allocator Where storage will come from. NULL uses the default.
 * @param limits Caps to apply. Never NULL here.
 */
void grx_program_init(GRX_Program * program, const GRX_Allocator * allocator,
    const GRX_Limits * limits);

/**
 * @brief Append one instruction.
 *
 * @param program The program. NULL is GRX_ERR_INVALID.
 * @param inst The instruction to copy in.
 * @param out_index Receives its index. Optional.
 * @return GRX_OK, GRX_ERR_LIMIT when max_program_size is reached,
 *   GRX_ERR_OOM, or GRX_ERR_INVALID.
 */
GRX_Result grx_program_add(
    GRX_Program * program, const GRX_Inst * inst, uint32_t * out_index);

/**
 * @brief The instruction at an index.
 *
 * @param program The program.
 * @param index The instruction index.
 * @return The instruction, or NULL when the index is out of range.
 *   Invalidated by the next grx_program_add().
 */
GRX_Inst * grx_program_at(const GRX_Program * program, uint32_t index);

/**
 * @brief The callout record at an index in a program.
 *
 * @param program The program.
 * @param index The index, as a GRX_OP_CALLOUT's `x` holds it.
 * @return The record, or NULL when the index is out of range.
 */
const GRX_ProgramCallout * grx_program_callout(
    const GRX_Program * program, uint32_t index);

/**
 * @brief The bytes of a callout's string.
 *
 * @param program The program.
 * @param offset The offset, as GRX_ProgramCallout::string holds it.
 * @return The NUL-terminated bytes, or NULL for GRX_INDEX_NONE or an
 *   offset out of range.
 */
const char * grx_program_callout_string(
    const GRX_Program * program, uint32_t offset);

/**
 * @brief The mnemonic for an opcode.
 *
 * @param op The opcode.
 * @return A lowercase mnemonic, or "?" for a value out of range. Never NULL.
 */
const char * grx_opcode_name(GRX_Opcode op);

/**
 * @brief The scan list at an offset in a program.
 *
 * @param program The program.
 * @param offset The run's offset, as a GRX_OP_SCAN's `x` holds it.
 * @param out_count Receives how many groups it names. Required.
 * @return The first group number, or NULL when the offset is out of range.
 */
/**
 * @brief The body length a LOOK instruction's `x` names.
 *
 * @param program The program.
 * @param offset The instruction's `x`.
 * @param out_min Receives the body's shortest match, in bytes.
 * @param out_max Receives its longest.
 * @return Non-zero when the offset named a span.
 */
int grx_program_look_span(const GRX_Program * program, uint32_t offset,
    size_t * out_min, size_t * out_max);

const uint32_t * grx_program_scan_list(
    const GRX_Program * program, uint32_t offset, size_t * out_count);

/**
 * @brief Write a disassembly of a program.
 *
 * Shared by grx_regex_dump() and by any test that has built a program by
 * hand without a regex around it.
 *
 * @param program The program.
 * @param out Destination.
 * @return GRX_OK, or GRX_ERR_INVALID for a NULL argument.
 */
GRX_Result grx_program_dump(const GRX_Program * program, FILE * out);

/**
 * @brief Release a program's storage. NULL is ignored.
 *
 * @param program The program.
 */
void grx_program_clear(GRX_Program * program);

/**
 * @brief Generate instructions for a lowered pattern.
 *
 * @param ir The lowered pattern. Never NULL here.
 * @param limits Caps to apply. Never NULL here.
 * @param out_error Receives the failure position and message. May be NULL.
 * @param out_program A program already prepared by grx_program_init().
 * @return GRX_OK or a failure code.
 */
GRX_Result grx_codegen_program(const struct GRX_IR * ir,
    const GRX_Limits * limits, GRX_Error * out_error,
    GRX_Program * out_program);

/**
 * @brief Compile a lowered pattern into a program.
 *
 * @param pattern The parsed pattern. Never NULL here.
 * @param limits Caps to apply. Never NULL here.
 * @param allocator Allocator to use. Never NULL here.
 * @param out_error Receives the failure position and message. May be NULL.
 * @param out_regex Receives the compiled regex on success.
 * @return GRX_OK or a failure code.
 */
GRX_Result grx_compile_program(const GRX_Pattern * pattern,
    const GRX_Limits * limits, const GRX_Allocator * allocator,
    GRX_Error * out_error, GRX_Regex ** out_regex);

/**
 * @brief Which bytes a match of `program` can begin with.
 *
 * A walk from the entry point over every path that reaches a consuming
 * instruction; see the file comment in prefilter.c for what it promises. The
 * answer is a *superset* when it is known at all, so a caller may use it to
 * skip a position and may never use it to accept one.
 *
 * @param program The compiled program.
 * @param out_set Receives GRX_FIRST_BYTES_SIZE bytes of bitmap, zeroed when
 *   the answer is not known.
 * @return Non-zero when the set is known.
 */
int grx_program_first_bytes(
    const GRX_Program * program, unsigned char * out_set);

/**
 * @brief The bytes every match of `program` begins with.
 *
 * The forced path out of the entry point: while there is exactly one way
 * onwards and it consumes one known code point, that code point's bytes are
 * bytes every match starts with. The walk stops at the first branch, at
 * anything that consumes a choice of code points, and at anything it does not
 * model - so the answer is a *prefix* of the true one, never longer than it.
 *
 * Nothing is reported for a program containing `\K` or `\ze`, which move the
 * reported start and end away from where the attempt began: what the attempt
 * consumes first is then not what the match begins with, and this fact is
 * about the match.
 *
 * @param program The compiled program.
 * @param out Receives up to `cap` bytes.
 * @param cap How many bytes `out` holds.
 * @return How many bytes were written; 0 when there is no answer.
 */
size_t grx_program_literal_prefix(
    const GRX_Program * program, char * out, size_t cap);

/**
 * @brief Bytes every match of `program` contains, wherever they fall in it.
 *
 * Dominance, not a walk: an instruction on every path from the entry point to
 * every `MATCH` is executed by every match, so a run of such instructions that
 * consume one known code point each, chained so that each is the only way on
 * from the one before and the only way into the one after, is a string every
 * match contains.
 *
 * Weaker than it could be, on purpose. `cat|car` has the required literal
 * `ca` and this does not find it, because the two `c` instructions are
 * different instructions and neither is on every path; finding it needs the
 * tree rather than the program. What this does find is the case the byte set
 * cannot help with at all - `.*foo`, `[0-9]+-[0-9]+` - and an answer it
 * cannot justify is no answer.
 *
 * Among candidates the longest wins, as the one that is rarest under any
 * subject distribution worth assuming. The tie-break is where this used to go
 * wrong: `a.{20}q` has `a` and `q` both dominating and both one byte, and
 * taking the earlier left the answer identical to the literal prefix, which
 * tells an engine nothing it did not already have. A candidate that always
 * sits at offset 0 is therefore the last choice among equals, and a bounded
 * offset beats an unbounded one.
 *
 * @param program The compiled program. `literal_prefix` must already be
 *   computed, since the choice among equal-length candidates reads it.
 * @param out Receives up to `cap` bytes.
 * @param cap How many bytes `out` holds.
 * @param out_offset_min Receives the least distance in bytes from the start
 *   of a match to the literal. Never NULL.
 * @param out_offset_max Receives the greatest such distance, or GRX_NPOS
 *   when it is unbounded or was not established. Never NULL.
 * @return How many bytes were written; 0 when there is no answer.
 */
size_t grx_program_required_literal(const GRX_Program * program, char * out,
    size_t cap, size_t * out_offset_min, size_t * out_offset_max);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_COMPILE_COMPILE_INTERNAL_H
