/**
 * @file
 *
 * The backtracking engine: every construct, bounded by the limits.
 *
 * This is the engine that can run a backreference, a lookaround and an atomic
 * group, and it is the one whose worst case is exponential in the subject
 * length. `GRX_Limits::max_steps` and `max_backtrack` are not optional extras
 * here - they are the only thing standing between a pattern such as `(a+)+$`
 * and a process that never returns.
 *
 * The backtrack stack is explicit and never the C stack. A frame is one of
 * five things: an alternative to resume, a capture to undo, a progress
 * register to undo, an atomic barrier, or a lookaround's boundary. Undo
 * frames rather than snapshots, because a snapshot of the capture array at
 * every SPLIT is O(groups) per branch point and the writes that actually
 * need undoing are few.
 *
 * The one thing that *does* recurse on the C stack is a lookaround, and it
 * recurses exactly as deep as the pattern nests lookarounds - which the
 * parser has already capped with `max_nesting_depth`. A lookaround is atomic
 * by nature: it either holds at this position or it does not, and there is
 * nothing to backtrack into, so running it as a sub-match is not a shortcut.
 *
 * A lookbehind is the same sub-match with the body's instructions carrying
 * GRX_INST_REVERSE, so they step backwards through the subject. That is the
 * whole of what makes a lookbehind of any length work without a second
 * engine, and it is what the ECMAScript specification itself describes
 * (22.2.2.4, direction -1).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../unicode/unicode_internal.h"
#include "exec_internal.h"

/** What a stack frame is. */
typedef enum {
  FRAME_RESUME = 0,   ///< An alternative not yet tried: `pc` and `position`.
  FRAME_CAPTURE,      ///< A capture slot to put back: `index` and `value`.
  FRAME_REGISTER,     ///< A progress register to put back.
  FRAME_ATOMIC,       ///< A barrier ATOMIC_END pops through and discards.
} FrameKind;

/** One entry of the backtrack stack. */
typedef struct {
  uint8_t kind;      ///< A @ref FrameKind.
  uint32_t pc;       ///< RESUME: where to resume. Others: the slot index.
  size_t position;   ///< RESUME: the subject position. Others: the old value.
} Frame;

/** What one run of the engine carries. */
typedef struct {
  const GRX_ExecRequest * request;
  const GRX_Program * program;
  const GRX_Allocator * allocator;
  size_t captures;       ///< Capture slots: twice the group count plus two.
  size_t registers;      ///< Progress registers.
  size_t * slots;        ///< Captures, then registers.
  Frame * stack;
  size_t depth;          ///< Frames in use.
  size_t capacity;       ///< Frames allocated.
  size_t steps;          ///< Instructions executed, against max_steps.
  int utf;               ///< Whether a step is a code point or a byte.
  GRX_Result failure;    ///< Set when a limit stopped the run.
} Backtrack;

/** The smallest stack the engine allocates, so a simple match grows nothing. */
#define GRX_BACKTRACK_MIN_STACK 64

// --------------------------------------------------------------------------
// The stack
// --------------------------------------------------------------------------

static int push(Backtrack * bt, FrameKind kind, uint32_t pc, size_t position) {
  const GRX_Limits * limits = bt->request->limits;
  if (limits->max_backtrack && bt->depth + 1 > limits->max_backtrack) {
    bt->failure = GRX_ERR_LIMIT;
    return 0;
  }

  if (bt->depth == bt->capacity) {
    size_t capacity = bt->capacity ? bt->capacity * 2 : GRX_BACKTRACK_MIN_STACK;
    if (limits->max_backtrack && capacity > limits->max_backtrack) {
      capacity = limits->max_backtrack;
    }
    Frame * grown = gcu_allocator_realloc(
        bt->allocator, bt->stack, capacity * sizeof(Frame));
    if (!grown) {
      bt->failure = GRX_ERR_OOM;
      return 0;
    }
    bt->stack = grown;
    bt->capacity = capacity;
  }

  bt->stack[bt->depth].kind = (uint8_t)kind;
  bt->stack[bt->depth].pc = pc;
  bt->stack[bt->depth].position = position;
  bt->depth++;
  return 1;
}

/** Record a slot's old value so that backtracking past this point restores it. */
static int save_slot(Backtrack * bt, size_t index) {
  if (index >= bt->captures + bt->registers) {
    return 1;
  }
  FrameKind kind = index < bt->captures ? FRAME_CAPTURE : FRAME_REGISTER;
  return push(bt, kind, (uint32_t)index, bt->slots[index]);
}

/**
 * Unwind to the most recent alternative, undoing everything on the way.
 *
 * Returns 0 when the stack is empty, which means every alternative from this
 * starting position has been tried and failed.
 */
static int backtrack(Backtrack * bt, uint32_t * out_pc, size_t * out_position,
    size_t floor) {
  while (bt->depth > floor) {
    Frame frame = bt->stack[--bt->depth];
    switch ((FrameKind)frame.kind) {
      case FRAME_RESUME:
        *out_pc = frame.pc;
        *out_position = frame.position;
        return 1;
      case FRAME_CAPTURE:
      case FRAME_REGISTER:
        bt->slots[frame.pc] = frame.position;
        break;
      case FRAME_ATOMIC:
      default:
        break;
    }
  }

  return 0;
}

// --------------------------------------------------------------------------
// Reading the subject
// --------------------------------------------------------------------------

static int read_forward(const Backtrack * bt, size_t position,
    uint32_t * out_codepoint, size_t * out_width) {
  if (position >= bt->request->length) {
    return 0;
  }
  if (!bt->utf) {
    *out_codepoint = (unsigned char)bt->request->subject[position];
    *out_width = 1;
    return 1;
  }

  size_t width = grx_unicode_utf8_decode(bt->request->subject + position,
      bt->request->length - position, out_codepoint);
  if (!width) {
    return 0;
  }
  *out_width = width;
  return 1;
}

static int read_backward(const Backtrack * bt, size_t position,
    uint32_t * out_codepoint, size_t * out_width) {
  if (!position) {
    return 0;
  }
  if (!bt->utf) {
    *out_codepoint = (unsigned char)bt->request->subject[position - 1];
    *out_width = 1;
    return 1;
  }

  size_t width = grx_unicode_utf8_decode_prev(
      bt->request->subject, position, out_codepoint);
  if (!width) {
    return 0;
  }
  *out_width = width;
  return 1;
}

static int in_class(const Backtrack * bt, uint32_t index, uint32_t codepoint) {
  if (index == GRX_INDEX_NONE) {
    return 0;
  }
  return grx_class_table_contains(&bt->program->classes, index, codepoint);
}

/** As the Pike VM's: a kind and a class index, and no dialect in sight. */
static int assertion_holds(
    const Backtrack * bt, const GRX_Inst * inst, size_t position) {
  const GRX_ExecRequest * request = bt->request;
  uint32_t before = 0;
  uint32_t after = 0;
  size_t width = 0;
  int has_before = read_backward(bt, position, &before, &width);
  int has_after = read_forward(bt, position, &after, &width);

  // NOTBOL and NOTEOL suppress only the end-of-subject halves; see the same
  // switch in exec_pike.c, which this one has to agree with exactly.
  switch ((GRX_AssertKind)inst->mode) {
    case GRX_ASSERT_START_SUBJECT:
      return position == 0 && !request->not_bol;
    case GRX_ASSERT_END_SUBJECT:
      return position == request->length && !request->not_eol;
    case GRX_ASSERT_END_BEFORE_NEWLINE:
      if (request->not_eol) {
        return 0;
      }
      if (position == request->length) {
        return 1;
      }
      return has_after && in_class(bt, inst->x, after)
          && position + width == request->length;
    case GRX_ASSERT_START_LINE:
      return (position == 0 && !request->not_bol)
          || (has_before && in_class(bt, inst->x, before));
    case GRX_ASSERT_END_LINE:
      return (position == request->length && !request->not_eol)
          || (has_after && in_class(bt, inst->x, after));
    case GRX_ASSERT_WORD_BOUNDARY:
    case GRX_ASSERT_NOT_WORD_BOUNDARY: {
      int word_before = has_before && in_class(bt, inst->x, before);
      int word_after = has_after && in_class(bt, inst->x, after);
      int boundary = word_before != word_after;
      return inst->mode == GRX_ASSERT_WORD_BOUNDARY ? boundary : !boundary;
    }
    case GRX_ASSERT_SEARCH_START:
      return position == request->start;
    case GRX_ASSERT_COUNT:
    default:
      return 0;
  }
}

/**
 * Whether the text a group matched appears again at `position`.
 *
 * Under a caseless match the comparison is on folded code points rather than
 * on bytes, because the two runs may be the same characters in different
 * cases and so different lengths. This is the one place folding survives
 * lowering, and it has to: what a backreference matches is not known until
 * the match runs.
 */
static int backref_matches(const Backtrack * bt, const GRX_Inst * inst,
    size_t position, size_t * out_width) {
  size_t start_slot = (size_t)inst->x * 2;
  size_t end_slot = start_slot + 1;
  *out_width = 0;

  if (end_slot >= bt->captures || bt->slots[start_slot] == GRX_NPOS
      || bt->slots[end_slot] == GRX_NPOS) {
    // The group did not participate. Which of two things that means is the
    // dialect's answer, carried here as a mode: ECMAScript matches the empty
    // string, everything else fails.
    return inst->mode == GRX_BACKREF_UNSET_EMPTY;
  }

  size_t from = bt->slots[start_slot];
  size_t to = bt->slots[end_slot];
  if (to < from) {
    return 0;
  }

  if (!inst->y) {
    size_t length = to - from;
    if (position + length > bt->request->length) {
      return 0;
    }
    if (memcmp(bt->request->subject + from, bt->request->subject + position,
            length)
        != 0) {
      return 0;
    }
    *out_width = length;
    return 1;
  }

  size_t source = from;
  size_t target = position;
  while (source < to) {
    uint32_t wanted = 0;
    uint32_t found = 0;
    size_t source_width = 0;
    size_t target_width = 0;
    if (!read_forward(bt, source, &wanted, &source_width)
        || !read_forward(bt, target, &found, &target_width)) {
      return 0;
    }
    if (grx_unicode_fold_simple(wanted) != grx_unicode_fold_simple(found)) {
      return 0;
    }
    source += source_width;
    target += target_width;
  }

  *out_width = target - position;
  return 1;
}

// --------------------------------------------------------------------------
// The machine
// --------------------------------------------------------------------------

/**
 * Run the program from `pc` at `position` until it matches or runs out of
 * alternatives.
 *
 * `floor` is the stack height this run may not unwind below, so that a
 * lookaround's sub-match cannot backtrack into its caller's alternatives.
 * The caller unwinds to the floor itself afterwards, which is what makes a
 * lookaround atomic.
 *
 * `toplevel` distinguishes the whole-pattern run from a lookaround body.
 * Both end in a MATCH - a lookaround body is compiled as a sub-program with
 * a success state of its own - but only the outer one is a match the caller
 * asked for, and so only the outer one is subject to the request's rule
 * about empty matches. Applying it to a body would make `(?=)` fail under
 * NOTEMPTY, which is a rule about the *result*, not about an assertion.
 */
static int run(Backtrack * bt, uint32_t pc, size_t position, size_t floor,
    int toplevel, size_t * out_end) {
  const GRX_Limits * limits = bt->request->limits;

  for (;;) {
    bt->steps++;
    if (limits->max_steps && bt->steps > limits->max_steps) {
      bt->failure = GRX_ERR_LIMIT;
      return 0;
    }

    const GRX_Inst * inst
        = GRX_ARENA_AT(const GRX_Inst, &bt->program->insts, pc);
    if (!inst) {
      bt->failure = GRX_ERR_INTERNAL;
      return 0;
    }

    int reverse = (inst->flags & GRX_INST_REVERSE) != 0;
    uint32_t codepoint = 0;
    size_t width = 0;
    int consumed = 0;
    int ok = 1;

    switch ((GRX_Opcode)inst->op) {
      case GRX_OP_CHAR:
      case GRX_OP_CLASS:
      case GRX_OP_ANY:
      case GRX_OP_ANY_NL: {
        int have = reverse
            ? read_backward(bt, position, &codepoint, &width)
            : read_forward(bt, position, &codepoint, &width);
        if (!have) {
          ok = 0;
          break;
        }
        switch ((GRX_Opcode)inst->op) {
          case GRX_OP_CHAR:
            ok = codepoint == inst->x;
            break;
          case GRX_OP_CLASS:
            ok = in_class(bt, inst->x, codepoint);
            break;
          case GRX_OP_ANY:
            ok = !in_class(bt, inst->x, codepoint);
            break;
          default:
            ok = 1;
            break;
        }
        consumed = ok;
        break;
      }

      case GRX_OP_SPLIT:
        // The other continuation is an alternative to come back to. The
        // preferred one is taken now, and that is what makes the result
        // leftmost-first.
        if (!push(bt, FRAME_RESUME, inst->y, position)) {
          return 0;
        }
        pc = inst->x;
        continue;

      case GRX_OP_JMP:
        pc = inst->x;
        continue;

      case GRX_OP_SAVE:
        if (!save_slot(bt, inst->x)) {
          return 0;
        }
        if (inst->x < bt->captures) {
          bt->slots[inst->x] = position;
        }
        pc++;
        continue;

      case GRX_OP_RESET:
        for (uint32_t slot = inst->x;
            slot < inst->y && slot < bt->captures; slot++) {
          if (!save_slot(bt, slot)) {
            return 0;
          }
          bt->slots[slot] = GRX_NPOS;
        }
        pc++;
        continue;

      case GRX_OP_ASSERT:
        ok = assertion_holds(bt, inst, position);
        break;

      case GRX_OP_PROGRESS_SET: {
        size_t index = bt->captures + inst->x;
        if (!save_slot(bt, index)) {
          return 0;
        }
        if (index < bt->captures + bt->registers) {
          bt->slots[index] = position;
        }
        pc++;
        continue;
      }

      case GRX_OP_PROGRESS_CHECK: {
        size_t index = bt->captures + inst->x;
        int stalled = index < bt->captures + bt->registers
            && bt->slots[index] == position;
        if (!stalled) {
          pc++;
          continue;
        }
        switch ((GRX_EmptyLoopMode)inst->mode) {
          case GRX_EMPTY_LOOP_FAIL:
            ok = 0;
            break;
          case GRX_EMPTY_LOOP_BREAK:
            pc = inst->y;
            continue;
          case GRX_EMPTY_LOOP_ALLOW:
          case GRX_EMPTY_LOOP_COUNT:
          default:
            pc++;
            continue;
        }
        break;
      }

      case GRX_OP_BACKREF:
        ok = backref_matches(bt, inst, position, &width);
        if (ok) {
          position += width;
        }
        pc++;
        if (ok) {
          continue;
        }
        break;

      case GRX_OP_ATOMIC_BEGIN:
        if (!push(bt, FRAME_ATOMIC, pc, position)) {
          return 0;
        }
        pc++;
        continue;

      case GRX_OP_ATOMIC_END: {
        // Everything the region pushed is discarded, so nothing can
        // backtrack into it. The capture and register frames are kept - the
        // region's writes stand, and must still be undone if the *whole*
        // group is backtracked past.
        size_t write = floor;
        size_t read = floor;
        size_t barrier = bt->depth;
        for (size_t i = bt->depth; i > floor; i--) {
          if (bt->stack[i - 1].kind == FRAME_ATOMIC) {
            barrier = i - 1;
            break;
          }
        }
        for (read = barrier + 1, write = barrier; read < bt->depth; read++) {
          if (bt->stack[read].kind != FRAME_RESUME) {
            bt->stack[write++] = bt->stack[read];
          }
        }
        bt->depth = write;
        pc++;
        continue;
      }

      case GRX_OP_LOOK: {
        int negative = inst->mode == GRX_LOOK_AHEAD_NEGATIVE
            || inst->mode == GRX_LOOK_BEHIND_NEGATIVE;

        // The body runs as its own sub-match above this point on the stack,
        // and everything it pushed is discarded afterwards: a lookaround
        // either holds here or does not, and there is nothing to backtrack
        // into. The capture writes it made are turned into undo frames on
        // the caller's stack so that backtracking past the whole lookaround
        // still puts them back.
        size_t * before = gcu_allocator_malloc(
            bt->allocator, bt->captures * sizeof(size_t));
        if (!before) {
          bt->failure = GRX_ERR_OOM;
          return 0;
        }
        memcpy(before, bt->slots, bt->captures * sizeof(size_t));

        size_t body_floor = bt->depth;
        size_t end = 0;
        int body_matched
            = run(bt, inst->x, position, body_floor, 0, &end);
        bt->depth = body_floor;
        if (bt->failure != GRX_OK) {
          gcu_allocator_free(bt->allocator, before);
          return 0;
        }

        if (body_matched == negative) {
          // A positive lookaround whose body failed, or a negative one whose
          // body succeeded. Either way the construct fails and the captures
          // go back to what they were.
          memcpy(bt->slots, before, bt->captures * sizeof(size_t));
          gcu_allocator_free(bt->allocator, before);
          ok = 0;
          break;
        }

        if (negative) {
          // ECMA-262 22.2.2.4: a negative lookaround leaves the captures as
          // they were, whatever its body touched on the way to failing.
          memcpy(bt->slots, before, bt->captures * sizeof(size_t));
        }
        else {
          for (size_t i = 0; i < bt->captures; i++) {
            if (bt->slots[i] == before[i]) {
              continue;
            }
            size_t old = before[i];
            size_t current = bt->slots[i];
            bt->slots[i] = old;
            if (!save_slot(bt, i)) {
              gcu_allocator_free(bt->allocator, before);
              return 0;
            }
            bt->slots[i] = current;
          }
        }
        gcu_allocator_free(bt->allocator, before);

        pc = inst->y;
        continue;
      }

      case GRX_OP_MATCH:
        if (toplevel
            && !grx_exec_accepts(bt->request, bt->slots[0], position)) {
          // Not a match the caller will take. Fall through to backtracking
          // so that a longer alternative from the same position still has
          // its chance.
          ok = 0;
          break;
        }
        *out_end = position;
        return 1;

      case GRX_OP_KEEP:
      case GRX_OP_VERB:
      case GRX_OP_COND:
      case GRX_OP_CALL:
      case GRX_OP_RET:
        // Their opcodes exist and their semantics are plan.md WP-19. A
        // program containing one is refused rather than run with the verb
        // ignored, which would give a plausible wrong answer.
        bt->failure = GRX_ERR_UNSUPPORTED;
        return 0;

      default:
        bt->failure = GRX_ERR_INTERNAL;
        return 0;
    }

    if (ok) {
      if (consumed) {
        position = reverse ? position - width : position + width;
      }
      pc++;
      continue;
    }

    if (!backtrack(bt, &pc, &position, floor)) {
      return 0;
    }
  }
}

GRX_Result grx_exec_backtrack(
    const GRX_ExecRequest * request, int * out_matched) {
  if (!request || !out_matched) {
    return GRX_ERR_INVALID;
  }
  *out_matched = 0;

  const GRX_Program * program = &request->regex->program;
  if (!program->insts.count) {
    return GRX_ERR_INVALID;
  }

  Backtrack bt = {
    .request = request,
    .program = program,
    .allocator = request->regex->allocator,
    .captures = 2 * (request->regex->capture_count + 1),
    .registers = program->register_count,
    .slots = NULL,
    .stack = NULL,
    .depth = 0,
    .capacity = 0,
    .steps = 0,
    .utf = (program->flags & GRX_PROGRAM_UTF) != 0,
    .failure = GRX_OK,
  };
  if (!bt.allocator) {
    bt.allocator = grx_allocator_default();
  }

  bt.slots = gcu_allocator_malloc(
      bt.allocator, (bt.captures + bt.registers) * sizeof(size_t));
  if (!bt.slots) {
    return GRX_ERR_OOM;
  }

  GRX_Result result = GRX_OK;
  size_t start = request->start;
  for (;;) {
    for (size_t i = 0; i < bt.captures + bt.registers; i++) {
      bt.slots[i] = GRX_NPOS;
    }
    bt.depth = 0;

    size_t end = 0;
    if (run(&bt, 0, start, 0, 1, &end)) {
      *out_matched = 1;
      break;
    }
    if (bt.failure != GRX_OK) {
      result = bt.failure;
      break;
    }
    if (request->anchored || start >= request->length) {
      break;
    }

    // Leftmost: try each starting position in turn, and take the first that
    // matches. Stepping by a character rather than a byte matters in UTF
    // mode - a match may not begin inside one.
    uint32_t codepoint = 0;
    size_t width = 0;
    if (!read_forward(&bt, start, &codepoint, &width)) {
      result = GRX_ERR_INVALID;
      break;
    }
    start += width;
  }

  if (result == GRX_OK && *out_matched && request->match) {
    GRX_Match * match = request->match;
    for (size_t i = 0; i < match->count; i++) {
      size_t first = 2 * i;
      size_t second = first + 1;
      if (second < bt.captures && bt.slots[first] != GRX_NPOS
          && bt.slots[second] != GRX_NPOS) {
        match->captures[i].start = bt.slots[first];
        match->captures[i].end = bt.slots[second];
      }
      else {
        match->captures[i] = (GRX_Capture) {GRX_NPOS, GRX_NPOS};
      }
    }
  }

  if (request->out_steps) {
    *request->out_steps = bt.steps;
  }
  gcu_allocator_free(bt.allocator, bt.slots);
  gcu_allocator_free(bt.allocator, bt.stack);
  return result;
}
