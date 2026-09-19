/**
 * @file
 *
 * Private declarations for the compiler: the instruction set, the program,
 * and the compiled regex.
 *
 * Status: stub. The opcode list is the one a Thompson/Pike construction needs
 * plus the instructions only a backtracking engine can run; it is expected to
 * grow as the dialects are specified.
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

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief One instruction of the compiled program.
 *
 * The first group is what a lockstep NFA simulation can run. The second is
 * what it cannot, and an occurrence of any of them is what makes a program
 * need the backtracking engine - see grx_exec_program_needs_backtracking().
 */
typedef enum {
  GRX_OP_MATCH = 0, ///< The match succeeded.
  GRX_OP_CHAR,      ///< Consume one specific code point.
  GRX_OP_CLASS,     ///< Consume one code point in a class.
  GRX_OP_ANY,       ///< Consume one code point.
  GRX_OP_SPLIT,     ///< Try two continuations, in priority order.
  GRX_OP_JMP,       ///< Continue at another instruction.
  GRX_OP_SAVE,      ///< Record a capture boundary.
  GRX_OP_ASSERT,    ///< A zero-width assertion: `^`, `$`, `\b`.
  GRX_OP_BACKREF,   ///< Match what a group matched earlier.
  GRX_OP_LOOKAROUND, ///< Run a sub-program without consuming input.
  GRX_OP_ATOMIC,    ///< Run a sub-program and discard its backtrack points.
  GRX_OP_RECURSE,   ///< Re-enter the program, or one group of it.
  GRX_OP_COUNT      ///< Closes the enum; not an opcode.
} GRX_Opcode;

/**
 * @brief One compiled instruction.
 */
typedef struct GRX_Inst {
  GRX_Opcode op; ///< What to do.
  uint32_t x;    ///< First operand; opcode-specific.
  uint32_t y;    ///< Second operand; opcode-specific.
} GRX_Inst;

/**
 * @brief A compiled program.
 */
typedef struct GRX_Program {
  GRX_Inst * insts; ///< Instruction array; execution starts at index 0.
  size_t count;     ///< Instructions in use.
} GRX_Program;

/**
 * @brief A compiled regular expression.
 *
 * Declared here because the engines read it; the public headers see only an
 * opaque GRX_Regex.
 */
struct GRX_Regex {
  const GRX_Allocator * allocator; ///< The allocator everything came from.
  GRX_Syntax syntax;               ///< Dialect the pattern was read in.
  uint32_t options;                ///< GRX_Option bits it was compiled with.
  GRX_Program program;             ///< What the engines run.
  size_t capture_count;            ///< Capturing groups, excluding group 0.
  char ** capture_names;           ///< One per group, NULL where unnamed.
};

/**
 * @brief Compile a parsed pattern into a program.
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
