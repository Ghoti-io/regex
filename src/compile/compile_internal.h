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
 *
 * Copyright 2026 by Corey Pennycuff
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

#ifdef __cplusplus
extern "C" {
#endif

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
 * | PROGRESS_CHECK | register | continuation when the loop must exit | GRX_EmptyLoopMode |
 * | BACKREF | group number | - | GRX_BackrefUnsetMode |
 * | LOOK | first instruction of the body | continuation after the body | GRX_LookKind |
 * | ATOMIC_BEGIN | matching ATOMIC_END | - | - |
 * | ATOMIC_END | - | - | - |
 * | COND | group number | continuation for the false branch | GRX_CondKind |
 * | CALL | first instruction of the target | continuation after it returns | - |
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
  GRX_OP_PROGRESS_SET, ///< Record the position at the head of a loop.
  GRX_OP_PROGRESS_CHECK, ///< Apply the empty-iteration rule at a loop's end.
  GRX_OP_BACKREF,      ///< Match what a group matched earlier.
  GRX_OP_LOOK,         ///< Run a sub-program without consuming input.
  GRX_OP_ATOMIC_BEGIN, ///< Start a region whose backtrack points are dropped.
  GRX_OP_ATOMIC_END,   ///< End it, discarding them.
  GRX_OP_COND,         ///< Branch on whether a group participated, and kin.
  GRX_OP_CALL,         ///< Re-enter the program, or one group of it.
  GRX_OP_RET,          ///< Return from a CALL.
  GRX_OP_KEEP,         ///< Reset the reported start of the match.
  GRX_OP_VERB,         ///< A backtracking control verb.
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
 * @brief A compiled program: instructions, the classes they name, and the
 * two properties that belong to the whole of it rather than to any node.
 */
typedef struct GRX_Program {
  GRX_Arena insts;                ///< GRX_Inst; execution starts at index 0.
  GRX_ClassTable classes;         ///< Every class an instruction names.
  uint32_t flags;                 ///< GRX_PROGRAM_* bits.
  uint32_t register_count;        ///< Progress registers a thread needs.
  GRX_MatchPreference preference; ///< Which match a search reports.
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
 * @brief The mnemonic for an opcode.
 *
 * @param op The opcode.
 * @return A lowercase mnemonic, or "?" for a value out of range. Never NULL.
 */
const char * grx_opcode_name(GRX_Opcode op);

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

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_COMPILE_COMPILE_INTERNAL_H
