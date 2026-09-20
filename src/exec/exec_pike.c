/**
 * @file
 *
 * The Pike VM: a lockstep simulation of the compiled program.
 *
 * Every live thread of the NFA advances one input position at a time, and a
 * `(pc, position)` pair is visited at most once per position, so the cost is
 * O(subject x program) however the pattern is written. That bound is the
 * reason this engine exists: it is what makes a pattern supplied by an
 * untrusted party safe to run, and `GRX_Facts::is_regular` is how a caller
 * finds out whether they will get it.
 *
 * The bound is structural rather than counted. Nothing here consults
 * `max_steps` to stay linear - the sparse set does that, by refusing to add a
 * thread at a program counter another thread already occupies at this
 * position.
 *
 * Priority is the thread list's order, and that is what makes the result
 * leftmost-*first*: a SPLIT pushes its preferred continuation so that it is
 * explored, and appended, before the other. When a thread reaches MATCH,
 * every thread after it in the list is lower priority and is dropped; every
 * thread before it has already put its successors in the next list and may
 * still win.
 *
 * Per-thread state is the capture array and the progress registers, in one
 * reference-counted block that is copied only when a thread that shares it
 * writes. A SPLIT therefore costs a reference and a SAVE costs a copy only
 * when the block is shared.
 *
 * Reference: Russ Cox, "Regular Expression Matching: the Virtual Machine
 * Approach" (swtch.com/~rsc/regexp/regexp2.html).
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "../unicode/break_internal.h"
#include "../unicode/unicode_internal.h"
#include "exec_internal.h"

/**
 * One thread's mutable state: the capture slots, then the progress registers.
 *
 * Both live in one block because they are copied together and for the same
 * reason - a thread that diverges from another must not see the other's
 * writes. The registers are what implement the dialect's empty-iteration
 * rule, so they are per-thread exactly as the captures are.
 */
typedef struct PikeState {
  size_t refs;      ///< Threads sharing this block.
  size_t capacity;  ///< Slots, captures and registers together.
  size_t slots[1];  ///< `capacity` of them; GRX_NPOS where unset.
} PikeState;

/** One live thread: where it is, what it has recorded, and why it is not a
 * duplicate of another thread at the same program counter. */
typedef struct {
  uint32_t pc;
  PikeState * state;
  uint64_t stalls;   ///< Which progress registers equal this position.
  uint32_t next;     ///< The next thread at this pc, or PIKE_NO_THREAD.
} PikeThread;

#define PIKE_NO_THREAD ((uint32_t)-1)

/**
 * A set of threads, at most one per (program counter, stall mask).
 *
 * The sparse-set trick: `threads` lists the occupied program counters in the
 * order they were added, `sparse` maps a program counter to its place, and
 * membership is "the entry `sparse` points at names me". That makes the
 * membership test O(1) with no clearing between positions, which is what
 * turns the simulation from exponential into linear.
 *
 * The mask is what makes the set *sound*. A plain (pc, position) key assumes
 * two threads at one program counter have the same future, which is true of a
 * pure NFA and false here: GRX_OP_PROGRESS_CHECK consults a register, and two
 * arrivals can carry different values in it. `(a?b??)*` against "abc" is the
 * case that showed it. Iteration one reaches the lazy `b??` at position 1
 * with its register holding 0; iteration two reaches the same program counter
 * at the same position with the register holding 1, so one of them is a
 * stalled iteration and the other is not - and the second arrival was being
 * dropped as a duplicate, which threw away the thread that goes on to match
 * `b`. The library answered 0-1 where every other engine, and ECMA-262,
 * answer 0-2.
 *
 * Only *equality with the current position* can distinguish two register
 * values, because that is the only question GRX_OP_PROGRESS_CHECK asks, and a
 * register holding anything other than the current position can never equal a
 * later one - positions only advance. So one bit per register is the whole of
 * the distinction, and threads at one pc with the same bits really are
 * interchangeable. Entries sharing a pc are chained through `next`.
 */
typedef struct {
  uint32_t * sparse;
  PikeThread * threads;
  size_t count;
  size_t capacity;
} PikeList;

/** What one run of the simulation carries. */
typedef struct {
  const GRX_ExecRequest * request;
  const GRX_Program * program;
  const GRX_Allocator * allocator;
  size_t slots;          ///< Capture slots plus progress registers.
  size_t captures;       ///< Capture slots alone; the registers follow them.
  PikeList current;
  PikeList next;
  PikeThread * stack;    ///< The epsilon-closure walk's own stack.
  size_t stack_capacity;
  PikeState * matched;   ///< The best match so far, or NULL.
  size_t steps;          ///< Instructions executed, against max_steps.
  size_t memory;         ///< Bytes handed out, against max_match_memory.
  int utf;               ///< Whether a step is a code point or a byte.
  GRX_Result failure;    ///< Set when a limit stopped the run.
} Pike;

// --------------------------------------------------------------------------
// Thread state
// --------------------------------------------------------------------------

static PikeState * state_create(Pike * pike) {
  size_t bytes = sizeof(PikeState) + (pike->slots) * sizeof(size_t);
  if (pike->request->limits->max_match_memory
      && pike->memory + bytes > pike->request->limits->max_match_memory) {
    pike->failure = GRX_ERR_LIMIT;
    return NULL;
  }

  PikeState * state = gcu_allocator_malloc(pike->allocator, bytes);
  if (!state) {
    pike->failure = GRX_ERR_OOM;
    return NULL;
  }

  pike->memory += bytes;
  state->refs = 1;
  state->capacity = pike->slots;
  for (size_t i = 0; i < pike->slots; i++) {
    state->slots[i] = GRX_NPOS;
  }
  return state;
}

static PikeState * state_retain(PikeState * state) {
  if (state) {
    state->refs++;
  }
  return state;
}

static void state_release(Pike * pike, PikeState * state) {
  if (!state || --state->refs) {
    return;
  }

  pike->memory -= sizeof(PikeState) + state->capacity * sizeof(size_t);
  gcu_allocator_free(pike->allocator, state);
}

/**
 * Get a block this thread may write to, copying it if it is shared.
 *
 * Consumes the caller's reference either way, so the caller always holds
 * exactly one reference to whatever comes back.
 */
static PikeState * state_for_write(Pike * pike, PikeState * state) {
  if (state && state->refs == 1) {
    return state;
  }

  PikeState * copy = state_create(pike);
  if (!copy) {
    state_release(pike, state);
    return NULL;
  }
  if (state) {
    memcpy(copy->slots, state->slots, pike->slots * sizeof(size_t));
    state_release(pike, state);
  }
  return copy;
}

// --------------------------------------------------------------------------
// Thread lists
// --------------------------------------------------------------------------

static int list_init(Pike * pike, PikeList * list, size_t program_size) {
  list->sparse = gcu_allocator_malloc(
      pike->allocator, program_size * sizeof(uint32_t));
  list->threads = gcu_allocator_malloc(
      pike->allocator, program_size * sizeof(PikeThread));
  list->count = 0;
  list->capacity = program_size;
  if (!list->sparse || !list->threads) {
    return 0;
  }

  // `sparse` is deliberately left uninitialised in the classic formulation.
  // It is zeroed here anyway: the membership test reads it before writing
  // it, and reading uninitialised memory is undefined behaviour even when
  // every value it could hold gives the right answer. The sanitizers say so,
  // and they are right.
  memset(list->sparse, 0, program_size * sizeof(uint32_t));
  return 1;
}

/**
 * Make room for one more thread, growing the list if it is full.
 *
 * Before the stall mask existed a list held at most one thread per program
 * counter, so `program_size` entries were an exact bound and this could not
 * be reached. It can now: a program counter inside nested potentially-empty
 * loops can carry one thread per distinct mask. Growth is charged against
 * max_match_memory like everything else the simulation allocates, so a
 * pathological pattern becomes GRX_ERR_LIMIT rather than a wrong answer or a
 * write past the end - the two outcomes a fixed cap would have chosen
 * between.
 */
static int list_reserve(Pike * pike, PikeList * list) {
  if (list->count < list->capacity) {
    return 1;
  }
  size_t capacity = list->capacity ? list->capacity * 2 : 16;
  size_t bytes = capacity * sizeof(PikeThread);
  size_t added = (capacity - list->capacity) * sizeof(PikeThread);
  if (pike->request->limits->max_match_memory
      && pike->memory + added > pike->request->limits->max_match_memory) {
    pike->failure = GRX_ERR_LIMIT;
    return 0;
  }
  PikeThread * grown
      = gcu_allocator_realloc(pike->allocator, list->threads, bytes);
  if (!grown) {
    pike->failure = GRX_ERR_OOM;
    return 0;
  }
  pike->memory += added;
  list->threads = grown;
  list->capacity = capacity;
  return 1;
}

static void list_clear(Pike * pike, PikeList * list) {
  for (size_t i = 0; i < list->count; i++) {
    state_release(pike, list->threads[i].state);
  }
  list->count = 0;
}

static void list_free(Pike * pike, PikeList * list) {
  list_clear(pike, list);
  gcu_allocator_free(pike->allocator, list->sparse);
  gcu_allocator_free(pike->allocator, list->threads);
  list->sparse = NULL;
  list->threads = NULL;
}

static int list_contains(
    const PikeList * list, uint32_t pc, uint64_t stalls) {
  uint32_t index = list->sparse[pc];
  while (index < list->count && list->threads[index].pc == pc) {
    if (list->threads[index].stalls == stalls) {
      return 1;
    }
    index = list->threads[index].next;
  }
  return 0;
}

// --------------------------------------------------------------------------
// Reading the subject
// --------------------------------------------------------------------------

/** Decode the code point at an offset, or the byte when UTF mode is off. */
static int read_forward(const Pike * pike, size_t position,
    uint32_t * out_codepoint, size_t * out_width) {
  const GRX_ExecRequest * request = pike->request;
  if (position >= request->length) {
    return 0;
  }

  if (!pike->utf) {
    *out_codepoint = (unsigned char)request->subject[position];
    *out_width = 1;
    return 1;
  }

  size_t width = grx_unicode_utf8_decode(request->subject + position,
      request->length - position, out_codepoint);
  if (!width) {
    return 0;
  }
  *out_width = width;
  return 1;
}

/** Decode the code point ending at an offset, for the assertions that look back. */
static int read_backward(
    const Pike * pike, size_t position, uint32_t * out_codepoint) {
  if (!position) {
    return 0;
  }
  if (!pike->utf) {
    *out_codepoint = (unsigned char)pike->request->subject[position - 1];
    return 1;
  }

  return grx_unicode_utf8_decode_prev(
             pike->request->subject, position, out_codepoint)
      != 0;
}

/** Whether a class index holds a code point; GRX_INDEX_NONE holds nothing. */
static int in_class(const Pike * pike, uint32_t index, uint32_t codepoint) {
  if (index == GRX_INDEX_NONE) {
    return 0;
  }
  return grx_class_table_contains(&pike->program->classes, index, codepoint);
}

/**
 * Whether a zero-width assertion holds here.
 *
 * Every one of these is a dialect decision that lowering already made: which
 * of the three `$` rules applies, whether `^` is a line anchor, which code
 * points end a line, which are word characters. What arrives here is a kind
 * and a class index, and this function could not name a dialect if it tried.
 */
static int assertion_holds(
    const Pike * pike, const GRX_Inst * inst, size_t position) {
  const GRX_ExecRequest * request = pike->request;
  uint32_t before = 0;
  uint32_t after = 0;
  int has_before = read_backward(pike, position, &before);
  size_t width = 0;
  int has_after = read_forward(pike, position, &after, &width);

  // NOTBOL and NOTEOL say the caller's buffer is a *piece* of the text, so
  // its two ends are not the text's two ends. They suppress only the
  // end-of-subject halves of these rules: a `^` that holds because a newline
  // precedes it is still holding for a reason inside the buffer.
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
      // The end, or immediately before a line terminator that is the last
      // thing in the subject.
      return has_after && in_class(pike, inst->x, after)
          && position + width == request->length;

    case GRX_ASSERT_START_LINE:
      return (position == 0 && !request->not_bol)
          || (has_before && in_class(pike, inst->x, before));

    case GRX_ASSERT_START_LINE_INTERIOR:
      // The same, less the position after a newline that ends the
      // subject: there is no line there to be at the start of.
      return (position == 0 && !request->not_bol)
          || (position != request->length && has_before
              && in_class(pike, inst->x, before));

    case GRX_ASSERT_END_LINE:
      return (position == request->length && !request->not_eol)
          || (has_after && in_class(pike, inst->x, after));

    case GRX_ASSERT_WORD_BOUNDARY:
    case GRX_ASSERT_NOT_WORD_BOUNDARY: {
      int word_before = has_before && in_class(pike, inst->x, before);
      int word_after = has_after && in_class(pike, inst->x, after);
      int boundary = word_before != word_after;
      return inst->mode == GRX_ASSERT_WORD_BOUNDARY ? boundary : !boundary;
    }


    // The four segmentation boundaries. Each reads the subject itself, so
    // the instruction carries no class and the window is not consulted: a
    // boundary is a fact about the text, and `(*scs:` narrowing what may be
    // *matched* does not move where a sentence ends.
    case GRX_ASSERT_GRAPHEME_BOUNDARY:
    case GRX_ASSERT_NOT_GRAPHEME_BOUNDARY:
    case GRX_ASSERT_WORD_SEG_BOUNDARY:
    case GRX_ASSERT_NOT_WORD_SEG_BOUNDARY:
    case GRX_ASSERT_SENTENCE_BOUNDARY:
    case GRX_ASSERT_NOT_SENTENCE_BOUNDARY:
    case GRX_ASSERT_LINE_BOUNDARY:
    case GRX_ASSERT_NOT_LINE_BOUNDARY: {
      static const GRX_BreakKind kinds[] = {
        GRX_BREAK_GRAPHEME, GRX_BREAK_GRAPHEME,
        GRX_BREAK_WORD, GRX_BREAK_WORD,
        GRX_BREAK_SENTENCE, GRX_BREAK_SENTENCE,
        GRX_BREAK_LINE, GRX_BREAK_LINE,
      };
      size_t which = (size_t)inst->mode - GRX_ASSERT_GRAPHEME_BOUNDARY;
      int boundary = grx_unicode_break_at(
          kinds[which], request->subject, request->length, position);
      // The odd members of the run are the `\B{...}` spellings.
      return (which % 2) ? !boundary : boundary;
    }
    case GRX_ASSERT_SEARCH_START:
      return position == request->start;

    case GRX_ASSERT_LOOK_LENGTH:
      // Unreachable here: the guard only appears inside a lookbehind body,
      // and a program with a lookaround in it is not regular, so this engine
      // is never handed one. It holds rather than failing all the same - an
      // assertion that prunes work is not one that decides a match.
      return 1;

    case GRX_ASSERT_COUNT:
    default:
      return 0;
  }
}

// --------------------------------------------------------------------------
// The epsilon closure
// --------------------------------------------------------------------------

/**
 * Add a thread and everything it reaches without consuming input.
 *
 * Written with an explicit stack rather than recursion because the depth is
 * the program's size, and a program may be two hundred thousand instructions
 * - a depth no C stack should be asked for. The stack is LIFO and a SPLIT
 * pushes its second continuation first, so the preferred one is explored and
 * appended first and the list stays in priority order.
 */
/**
 * Make room on the epsilon-closure walk's stack.
 *
 * Charged against max_match_memory, like the thread lists: a pattern that
 * needs an unreasonable amount of either is refused with GRX_ERR_LIMIT, which
 * is an answer, where GRX_ERR_INTERNAL was an admission.
 */
static int stack_reserve(Pike * pike, size_t needed) {
  if (needed <= pike->stack_capacity) {
    return 1;
  }
  size_t capacity = pike->stack_capacity ? pike->stack_capacity * 2 : 32;
  while (capacity < needed) {
    capacity *= 2;
  }
  size_t added = (capacity - pike->stack_capacity) * sizeof(PikeThread);
  if (pike->request->limits->max_match_memory
      && pike->memory + added > pike->request->limits->max_match_memory) {
    pike->failure = GRX_ERR_LIMIT;
    return 0;
  }
  PikeThread * grown = gcu_allocator_realloc(
      pike->allocator, pike->stack, capacity * sizeof(PikeThread));
  if (!grown) {
    pike->failure = GRX_ERR_OOM;
    return 0;
  }
  pike->memory += added;
  pike->stack = grown;
  pike->stack_capacity = capacity;
  return 1;
}

/**
 * Which progress registers hold exactly this position.
 *
 * One bit per register, and that is the whole of what distinguishes two
 * threads at one program counter: see PikeList. A state with no registers -
 * a pattern with no potentially-empty loop, which is most of them - gives
 * zero, so the set behaves exactly as it did before this existed.
 */
static uint64_t stall_mask(
    const Pike * pike, const PikeState * state, size_t position) {
  if (!state || pike->slots <= pike->captures) {
    return 0;
  }
  uint64_t mask = 0;
  size_t registers = pike->slots - pike->captures;
  // Beyond 64 registers the mask saturates: every such thread is then treated
  // as distinct from none of the others, which is the conservative direction
  // only for the registers that fit. A program with that many nested
  // potentially-empty loops is bounded by max_program_size long before this
  // matters, and capping the loop is better than reading past the slots.
  if (registers > 64) {
    registers = 64;
  }
  for (size_t index = 0; index < registers; index++) {
    if (state->slots[pike->captures + index] == position) {
      mask |= (uint64_t)1 << index;
    }
  }
  return mask;
}

static void add_thread(
    Pike * pike, PikeList * list, uint32_t pc, PikeState * state,
    size_t position) {
  size_t depth = 0;
  pike->stack[depth].pc = pc;
  pike->stack[depth].state = state;
  depth++;

  while (depth) {
    depth--;
    uint32_t current_pc = pike->stack[depth].pc;
    PikeState * current = pike->stack[depth].state;

    uint64_t stalls = stall_mask(pike, current, position);
    if (current_pc >= pike->program->insts.count
        || list_contains(list, current_pc, stalls)) {
      state_release(pike, current);
      continue;
    }

    if (!list_reserve(pike, list)) {
      state_release(pike, current);
      continue;
    }

    // Occupied now, before the walk goes on: a program counter reached twice
    // at one position with the same stall mask keeps the first arrival, which
    // is the higher-priority one, and that is what makes the result
    // leftmost-first. A second arrival with a different mask is a different
    // thread and is chained behind this one.
    uint32_t previous = list->sparse[current_pc];
    list->threads[list->count].next
        = (previous < list->count && list->threads[previous].pc == current_pc)
        ? previous
        : PIKE_NO_THREAD;
    list->sparse[current_pc] = (uint32_t)list->count;
    list->threads[list->count].pc = current_pc;
    list->threads[list->count].state = NULL;
    list->threads[list->count].stalls = stalls;
    list->count++;
    size_t slot = list->count - 1;

    const GRX_Inst * inst
        = GRX_ARENA_AT(const GRX_Inst, &pike->program->insts, current_pc);
    if (!inst) {
      state_release(pike, current);
      continue;
    }

    // Room for two more tasks. The stack was sized for the program on the
    // reasoning that a program counter is pushed only when it has not been
    // visited - which stopped being an exact bound when the stall mask made
    // one program counter able to hold several threads, so it grows now
    // rather than reporting an internal error for a pattern that is merely
    // bigger than the old assumption.
    if (depth + 2 > pike->stack_capacity && !stack_reserve(pike, depth + 2)) {
      state_release(pike, current);
      continue;
    }

    switch ((GRX_Opcode)inst->op) {
      case GRX_OP_JMP:
        pike->stack[depth].pc = inst->x;
        pike->stack[depth].state = current;
        depth++;
        break;

      case GRX_OP_SPLIT:
        pike->stack[depth].pc = inst->y;
        pike->stack[depth].state = state_retain(current);
        depth++;
        pike->stack[depth].pc = inst->x;
        pike->stack[depth].state = current;
        depth++;
        break;

      case GRX_OP_SAVE: {
        PikeState * writable = state_for_write(pike, current);
        if (!writable) {
          break;
        }
        if (inst->x < pike->captures) {
          writable->slots[inst->x] = position;
        }
        pike->stack[depth].pc = current_pc + 1;
        pike->stack[depth].state = writable;
        depth++;
        break;
      }

      case GRX_OP_ASSERT:
        if (assertion_holds(pike, inst, position)) {
          pike->stack[depth].pc = current_pc + 1;
          pike->stack[depth].state = current;
          depth++;
        }
        else {
          state_release(pike, current);
        }
        break;

      case GRX_OP_PROGRESS_SET: {
        PikeState * writable = state_for_write(pike, current);
        if (!writable) {
          break;
        }
        size_t index = pike->captures + inst->x;
        if (index < pike->slots) {
          writable->slots[index] = position;
        }
        pike->stack[depth].pc = current_pc + 1;
        pike->stack[depth].state = writable;
        depth++;
        break;
      }

      case GRX_OP_RESET: {
        PikeState * writable = state_for_write(pike, current);
        if (!writable) {
          break;
        }
        for (uint32_t slot = inst->x; slot < inst->y && slot < pike->captures;
            slot++) {
          writable->slots[slot] = GRX_NPOS;
        }
        pike->stack[depth].pc = current_pc + 1;
        pike->stack[depth].state = writable;
        depth++;
        break;
      }

      case GRX_OP_RESET_STALE: {
        // The late half of GRX_CAPTURE_RESET_AFTER_EACH: a group set by an
        // earlier iteration than this one, in an iteration that is ending
        // without setting it. Reachable here only in principle - the rule is
        // emitted only for a pattern that reads a capture back, and every
        // way of doing that keeps a program off this engine - but a thread
        // that met it and did nothing would report the wrong spans.
        PikeState * writable = state_for_write(pike, current);
        if (!writable) {
          break;
        }
        size_t index = pike->captures + inst->x;
        size_t first = inst->y;
        if (index < pike->slots && first + 1 < pike->captures
            && writable->slots[first] != GRX_NPOS
            && writable->slots[first] < writable->slots[index]) {
          writable->slots[first] = GRX_NPOS;
          writable->slots[first + 1] = GRX_NPOS;
        }
        pike->stack[depth].pc = current_pc + 1;
        pike->stack[depth].state = writable;
        depth++;
        break;
      }

      case GRX_OP_PROGRESS_CHECK: {
        size_t index = pike->captures + inst->x;
        int stalled = index < pike->slots
            && current->slots[index] == position;
        if (!stalled) {
          pike->stack[depth].pc = current_pc + 1;
          pike->stack[depth].state = current;
          depth++;
          break;
        }
        // The iteration consumed nothing. Which of these three happens is
        // the dialect's answer to `(a*)*`, decided at lowering and arriving
        // here as a mode (documentation/dialects.md section 5.5).
        switch ((GRX_EmptyLoopMode)inst->mode) {
          case GRX_EMPTY_LOOP_FAIL:
            state_release(pike, current);
            break;
          case GRX_EMPTY_LOOP_BREAK:
            pike->stack[depth].pc = inst->y;
            pike->stack[depth].state = current;
            depth++;
            break;
          case GRX_EMPTY_LOOP_ALLOW:
          case GRX_EMPTY_LOOP_COUNT:
          default:
            pike->stack[depth].pc = current_pc + 1;
            pike->stack[depth].state = current;
            depth++;
            break;
        }
        break;
      }

      default:
        // A consuming instruction, or MATCH. This is where the thread rests
        // until the next position.
        list->threads[slot].state = current;
        break;
    }
  }
}

// --------------------------------------------------------------------------
// The simulation
// --------------------------------------------------------------------------

/** Whether the program holds an instruction this engine cannot run. */
static int program_is_runnable(const GRX_Program * program) {
  for (size_t i = 0; i < program->insts.count; i++) {
    const GRX_Inst * inst
        = GRX_ARENA_AT(const GRX_Inst, &program->insts, (uint32_t)i);
    if (!inst) {
      return 0;
    }
    if (inst->flags & GRX_INST_REVERSE) {
      return 0;
    }
    switch ((GRX_Opcode)inst->op) {
      case GRX_OP_BACKREF:
      case GRX_OP_LOOK:
      case GRX_OP_ATOMIC_BEGIN:
      case GRX_OP_ATOMIC_END:
      case GRX_OP_COND:
      case GRX_OP_CALL:
      case GRX_OP_RET:
      case GRX_OP_KEEP:
      case GRX_OP_VERB:
      case GRX_OP_SCAN:
      case GRX_OP_REWIND:
        return 0;
      default:
        break;
    }
  }

  return 1;
}

/** Copy a winning thread's slots into the caller's match object. */
static void publish(const Pike * pike, const PikeState * state) {
  GRX_Match * match = pike->request->match;
  if (!match || !state) {
    return;
  }

  for (size_t i = 0; i < match->count; i++) {
    size_t start = 2 * i;
    size_t end = start + 1;
    if (end < pike->captures && state->slots[start] != GRX_NPOS
        && state->slots[end] != GRX_NPOS) {
      match->captures[i].start = state->slots[start];
      match->captures[i].end = state->slots[end];
    }
    else {
      match->captures[i] = (GRX_Capture) {GRX_NPOS, GRX_NPOS};
    }
  }
}

GRX_Result grx_exec_pike(const GRX_ExecRequest * request, int * out_matched) {
  if (!request || !out_matched) {
    return GRX_ERR_INVALID;
  }
  *out_matched = 0;

  const GRX_Program * program = &request->regex->program;
  if (!program->insts.count) {
    return GRX_ERR_INVALID;
  }
  if (!program_is_runnable(program)) {
    return GRX_ERR_UNSUPPORTED;
  }

  Pike pike = {
    .request = request,
    .program = program,
    .allocator = request->regex->allocator,
    .captures = 2 * (request->regex->capture_count + 1),
    .matched = NULL,
    .steps = 0,
    .memory = 0,
    .utf = (program->flags & GRX_PROGRAM_UTF) != 0,
    .failure = GRX_OK,
  };
  pike.slots = pike.captures + program->register_count;
  if (!pike.allocator) {
    pike.allocator = grx_allocator_default();
  }

  size_t size = program->insts.count;
  GRX_Result result = GRX_OK;
  // The closure walk's stack starts small and grows. It used to be sized at
  // `2 * insts + 4` on the reasoning that a program counter is pushed only
  // once - which the stall mask ended, since one program counter can now hold
  // several threads. Rather than guess a new multiple, it grows on demand:
  // the guess would be either wasteful for the common pattern or wrong for
  // the uncommon one, and growing costs a realloc that almost never happens
  // twice. It also means the growth path is taken by ordinary patterns rather
  // than being code that only a pathological one would ever reach.
  pike.stack = NULL;
  pike.stack_capacity = 0;
  if (!stack_reserve(&pike, 32) || !list_init(&pike, &pike.current, size)
      || !list_init(&pike, &pike.next, size)) {
    result = pike.failure != GRX_OK ? pike.failure : GRX_ERR_OOM;
    goto done;
  }

  size_t position = request->start;
  for (;;) {
    // A fresh thread at the start of the program for every position, until
    // something matches. Not a restart: the sparse set means an occupied
    // program counter is not occupied twice, so the whole search stays
    // linear in the subject rather than quadratic.
    if (!pike.matched && (!request->anchored || position == request->start)) {
      PikeState * state = state_create(&pike);
      if (!state) {
        result = pike.failure;
        goto done;
      }
      add_thread(&pike, &pike.current, 0, state, position);
    }

    if (!pike.current.count) {
      break;
    }

    uint32_t codepoint = 0;
    size_t width = 0;
    int have_input = read_forward(&pike, position, &codepoint, &width);
    if (position < request->length && !have_input) {
      // Not valid UTF-8 where UTF mode says it must be. The caller is told
      // rather than the byte being matched as if it were a character.
      result = GRX_ERR_INVALID;
      goto done;
    }

    for (size_t i = 0; i < pike.current.count; i++) {
      uint32_t pc = pike.current.threads[i].pc;
      PikeState * state = pike.current.threads[i].state;
      pike.current.threads[i].state = NULL;
      if (!state) {
        continue;
      }

      pike.steps++;
      if (request->limits->max_steps
          && pike.steps > request->limits->max_steps) {
        state_release(&pike, state);
        for (size_t j = i + 1; j < pike.current.count; j++) {
          state_release(&pike, pike.current.threads[j].state);
          pike.current.threads[j].state = NULL;
        }
        result = GRX_ERR_LIMIT;
        goto done;
      }

      const GRX_Inst * inst
          = GRX_ARENA_AT(const GRX_Inst, &program->insts, pc);
      int advance = 0;
      switch ((GRX_Opcode)inst->op) {
        case GRX_OP_CHAR:
          advance = have_input && codepoint == inst->x;
          break;
        case GRX_OP_CLASS:
          advance = have_input && in_class(&pike, inst->x, codepoint);
          break;
        case GRX_OP_ANY:
          advance = have_input && !in_class(&pike, inst->x, codepoint);
          break;
        case GRX_OP_ANY_NL:
          advance = have_input;
          break;

        case GRX_OP_MATCH:
          // An empty match the request refuses is not a match at all, so
          // this thread dies and the lower-priority ones live: one of them
          // may still find a non-empty match from the same position.
          if (!grx_exec_accepts(request, state->slots[0], position)) {
            state_release(&pike, state);
            state = NULL;
            break;
          }
          // Every thread after this one in the list is lower priority, so
          // this match beats all of them and they are dropped. Threads
          // before it have already produced successors and may still win.
          state_release(&pike, pike.matched);
          pike.matched = state;
          state = NULL;
          for (size_t j = i + 1; j < pike.current.count; j++) {
            state_release(&pike, pike.current.threads[j].state);
            pike.current.threads[j].state = NULL;
          }
          i = pike.current.count;
          break;

        default:
          // Reached only if program_is_runnable() let something through.
          state_release(&pike, state);
          state = NULL;
          result = GRX_ERR_INTERNAL;
          goto done;
      }

      if (advance) {
        add_thread(&pike, &pike.next, pc + 1, state, position + width);
        if (pike.failure != GRX_OK) {
          result = pike.failure;
          goto done;
        }
      }
      else if (state) {
        state_release(&pike, state);
      }
    }

    list_clear(&pike, &pike.current);
    PikeList swap = pike.current;
    pike.current = pike.next;
    pike.next = swap;

    if (position >= request->length) {
      break;
    }
    position += width ? width : 1;
  }

  if (pike.matched) {
    *out_matched = 1;
    publish(&pike, pike.matched);
  }

done:
  if (request->out_steps) {
    *request->out_steps = pike.steps;
  }
  state_release(&pike, pike.matched);
  list_free(&pike, &pike.current);
  list_free(&pike, &pike.next);
  gcu_allocator_free(pike.allocator, pike.stack);
  return result;
}

int grx_exec_program_needs_backtracking(const GRX_Regex * regex) {
  if (!regex) {
    return 0;
  }

  // Read from the facts rather than by scanning the program again. Analysis
  // computed this once at compile time, and a second opinion here is a second
  // place to be wrong: an opcode added to the backtracking-only group without
  // a matching case in a scan would route a program to the Pike VM, which
  // would then mis-execute it far from the cause. That is the defect shape
  // documentation/development.md warns about, and reading one field is how it
  // stops being possible.
  //
  // GRX_Facts is conservative before analysis has run: grx_facts_init() sets
  // is_regular to 0, so an unanalysed program goes to the engine that can run
  // anything rather than the one that cannot.
  return !regex->facts.is_regular;
}

int grx_exec_program_is_memoizable(const GRX_Regex * regex) {
  if (!regex) {
    return 0;
  }

  // Read from the facts for the first three, for the same reason the line
  // above does. The fourth is read from the program, because "this loop
  // needs an empty-iteration guard" is a codegen decision and the register
  // count is where codegen records it: a program with no progress registers
  // has no per-thread history at all, which is exactly the condition the
  // memo needs.
  return !regex->facts.has_backreference && !regex->facts.has_lookaround
      && !regex->facts.has_recursion && regex->program.register_count == 0
      && !(regex->program.flags & GRX_PROGRAM_NO_MEMO);
}

size_t grx_exec_bitmap_bytes(const GRX_Regex * regex, size_t length) {
  if (!regex) {
    return GRX_NPOS;
  }
  size_t instructions = regex->program.insts.count;
  if (!instructions) {
    return 0;
  }
  if (length == GRX_NPOS) {
    return GRX_NPOS;
  }
  size_t positions = length + 1;
  if (positions > GRX_NPOS / instructions) {
    return GRX_NPOS;
  }
  size_t bits = instructions * positions;
  return bits / 8 + 1;
}
