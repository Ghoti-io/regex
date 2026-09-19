# Development

**Status:** describes what exists. The target layout, the pipeline and the
engines are specified in [design.md](design.md), which takes precedence where
the two differ; this page is corrected as each work package in
[plan.md](plan.md) lands.

**Landed:** Phase 0 and Phase 1 of [plan.md](plan.md), less WP-11, and
Phase 2's WP-12, WP-13, WP-15 and WP-16. A pattern in any of ECMAScript's
three modes - legacy, Unicode and UnicodeSets - parses, lowers, compiles and
matches on whichever of the three engines can run it, and
`grx_regex_replace()`, `grx_regex_split()` and `grx_regex_search_next()`
apply the dialect's own rules for templates, pieces and what follows an empty
match. Every other dialect is named and reports `GRX_ERR_UNSUPPORTED`;
conditionals, recursion and the backtracking control verbs compile as far as
they can and are then refused, rather than approximated.

## Layout

```
include/ghoti.io/regex/   Public headers
src/core/                 Result strings, limits, the allocator,
                          the diagnostic catalogue, the arena, and the
                          semantic vocabulary that survives lowering
src/syntax/               The dialect table
src/parse/                Pattern text to a syntax tree (the AST)
src/ir/                   The AST lowered to a dialect-free IR
src/compile/              IR to a program, and the compiled regex
src/exec/                 The Pike VM, the backtracker and its bit-state form
src/subst/                Replacement templates and splitting
src/charclass/            Character-class sets and the canonical class table
src/unicode/              UTF-8, case folding, property lookup
src/unicode/tables/       Generated from the UCD; do not edit by hand
src/regex.c               Version entry points
tests/unit/               Unit tests (gtest)
tests/conformance/        The .rxt vector reader and its runner
tests/fuzz/               libFuzzer harnesses and seed corpora
tests/data/vectors/       Checked-in conformance vectors
tests/data/probe/         What each reference implementation answered
tools/unicode/            Fetch the UCD and generate the tables
tools/oracle/             Drivers and harnesses that ask a reference
                          implementation the same question this library
                          was asked
```

The headers mirror the modules, with two exceptions: `regex.h` is the umbrella
rather than the compiled-regex header - that is `compile.h` - and
`charclass`, `unicode` and `ir` have no public header at all, because nothing
outside the library has a reason to build a character class, fold a code point
or walk an IR by hand.

## The three representations

A pattern passes through three shapes, and knowing which one you are looking
at is most of knowing where a bug is.

| | Header | Holds | Built by |
| --- | --- | --- | --- |
| **AST** | `src/parse/parse_internal.h` | what the text *says*, dialect-shaped | `src/parse/parse.c` and the dialect's hooks |
| **IR** | `src/ir/ir_internal.h` | what it *means*, dialect-free | `src/ir/lower.c` |
| **Program** | `src/compile/compile_internal.h` | what the engines *run* | `src/compile/codegen.c` |

All three are arenas of fixed-size nodes linked by `uint32_t` index, with
`GRX_INDEX_NONE` for "no node" - never 0, because index 0 is the root. That
shape means a tree is one allocation, is freed as a unit, and has no link a
range check cannot validate.

**The dialect is gone after lowering.** No file under `src/exec`, and neither
`src/compile/codegen.c` nor `src/compile/program.c`, may name a `GRX_Syntax`,
a `GRX_SYNTAX_` constant or a `grx_syntax_*` function - nor read
`GRX_Regex::syntax` through the header. `make check-layering` fails the build
if one does, and the Makefile names the three files that are legitimately
*above* the line with the reason for each.

When a dialect difference seems to need an engine to know which dialect it is
running, the construct it needs is missing from the IR - add it to
`src/core/semantics_internal.h` and to the IR, not to the engine. That has
happened twice so far and both times the addition was right:
`GRX_OP_RESET` for the capture-reset rule, and the `empty_loop` mode on
`PROGRESS_CHECK`.

`src/compile/compile.c` does name `GRX_Syntax`, and legitimately: it holds the
public accessors, and `grx_regex_syntax()` reports which dialect a regex was
compiled from. Reporting the dialect is not branching on it. The gate is
scoped to `src/exec` for that reason, and grows to cover `codegen.c` when
WP-07 splits it out from the API file.

## Limits are enforced by the arena

Each arena carries the cap it is subject to and the diagnostic that cap
reports, set once at init:

```c
grx_arena_init(&pattern->nodes, allocator, sizeof(GRX_Node),
    limits->max_nodes, GRX_DIAG_LIMIT_NODES);
```

Every append is checked from then on, so a phase cannot forget. A refused
append leaves the count unchanged - a limit is a promise, and half a class or
half a node is worse than an error.

## Diagnostics, not message strings

A failure is a `GRX_Diag`, and `grx_error_set()` is the only thing that builds
a `GRX_Error`. A test asserts `GRX_DIAG_UNMATCHED_OPEN_PAREN`, never English,
so the messages can be reworded without touching a test. A new diagnostic is a
constant in `core.h` plus a row in `src/core/diag.c`; the row carries both the
text and the result code it implies, so a call site cannot pair the wrong two.
`DiagnosticCatalogueIsComplete` fails if a constant has no row.

## The dumps

Three, all for debugging and for a failing test to print. **None of the
formats is stable across versions**, and nothing parses them - the conformance
runner prints them beside a failure and compares spans, not text.

`grx_pattern_dump()` walks the AST: a header, then one line per node,
indented by depth, each carrying the node's kind, its payload and its
`@offset+length` in the pattern text.

```
pattern: syntax=ecmascript options=0x00000040 nodes=3 captures=1
  repeat @0+4 {1,} greedy
    group @0+3 #1
      literal @1+1 "a"
```

`grx_ir_dump()` is the same shape for the IR, and shows the *resolved*
semantics rather than the spelling - which is the point, because a lowering
bug is a wrong mode and nothing else looks wrong:

```
ir: flags=0x00000001 prefer=leftmost-first iterate=advance-one nodes=3 captures=1 classes=0
  repeat {1,} greedy empty=fail reset=each @0+4
    capture #1 @0+3
      char 'a' @1+1
```

`grx_program_dump()`, which `grx_regex_dump()` calls, is a flat disassembly
indexed by instruction, expanding the encodings a reader should not have to
decode - a save slot as its group and end, a mode byte as its name:

All three examples on this page are the real output for `/(a)+/u`, which is
worth knowing when one of them stops matching: the page is wrong, or the
format changed and this page was not.

```
regex: syntax=ecmascript options=0x00000040 captures=1 regular=yes
program: flags=0x00000001 prefer=leftmost-first iterate=advance-one insts=13 classes=0 registers=0
     0  save           0  (group 0 start)
     1  reset          slots 2..3
     2  save           2  (group 1 start)
     3  char           'a'
     4  save           3  (group 1 end)
     5  split          6, 11
     6  reset          slots 2..3
     7  save           2  (group 1 start)
     8  char           'a'
     9  save           3  (group 1 end)
    10  jmp            5
    11  save           1  (group 0 end)
    12  match
```

Thirteen instructions for four characters of pattern is what expansion costs:
the `+` is one mandatory copy of the body and then an unbounded loop over a
second, and each copy carries the dialect's capture rule as a `reset`. There
is no `progress-set`/`progress-check` pair here because `(a)` cannot match
the empty string, so the loop cannot stall; write `/(a*)+/u` instead and the
pair appears, with `registers=1`. That absence is not only two instructions
saved - a progress register is per-thread history, and a program that has
none is a program the bit-state engine can memoise
([design.md](design.md) section 3.5.3). A `reverse` flag appears on the
consuming instructions of a lookbehind body.

Code points are escaped in all three (`\x0A`, `\u{1F600}`), so a pattern
containing a newline still dumps as one line per node and a diff stays
readable.

## Where a dialect lives

A dialect is three things, in three places
([design.md](design.md) section 4):

- **A row of `spec_table`** in [`src/syntax/syntax.c`](../src/syntax/syntax.c):
  which constructs it has. The parser reads `GRX_SyntaxSpec` rather than
  switching on `GRX_Syntax`, which keeps "does this syntax have possessive
  quantifiers" a single lookup instead of a condition repeated wherever
  quantifiers are parsed.
- **A row of `profiles`**, in the same file: what those constructs *mean*.
  Small enums, one per axis on which real implementations differ, and
  lowering turns each into an explicit IR node, flag or mode. Nothing here
  reaches an engine.
- **A `GRX_Frontend`** - the hooks for the spellings a table cannot
  describe. `src/syntax/ecmascript.c` is the one that exists. A hook reads
  text and produces a node; it never decides what a construct means, because
  that is the profile's job, and keeping the two apart is what keeps the
  hooks small.

Adding a dialect is therefore:

1. A constant in `GRX_Syntax`, before `GRX_SYNTAX_COUNT`.
2. A row in `spec_table`, a row in `profiles`, and a name in `spec_names`.
3. A `GRX_Frontend` and an entry in `grx_frontend_for()`. Until that exists
   the dialect reports `GRX_DIAG_DIALECT_NOT_IMPLEMENTED`, which is the right
   answer: a caller uses this library to learn whether a pattern is valid
   *for that engine*, and reading it with somebody else's grammar would tell
   them it is when it is not.
4. A section in [dialects.md](dialects.md) naming the reference document, and
   an oracle installed so that `tools/oracle/probe.py` can fill its profile
   cells by running the real implementation rather than by reading a manual.
5. Tests for whatever is *definitional* about it - the thing that makes it a
   separate dialect rather than an alias for one already there.

Point 4 is the one to be careful about. `tests/unit/test_syntax.cpp` states
that RE2 has no backreference and that POSIX ERE has none either, because both
are facts about the specifications; it deliberately does not pin the whole
table, because the provisional rows are expected to change as they are checked
against their references, and a test that pins them would record the guess
rather than the specification.

## Adding an engine

An engine takes a `GRX_ExecRequest` and fills in a `GRX_Match`. Three are
intended - a lockstep simulation for the linear-time guarantee, a bit-state
backtracker, and a full backtracker for everything else (design.md section
3.5) - and a fourth would need a reason beyond speed.

Whatever an engine does, three properties are not negotiable:

- **It respects `max_steps` and `max_backtrack`.** An engine that can loop
  without a bound is an engine that turns a pattern into a denial of service.
- **It is checked against the others.** Any pattern two engines can both run
  must produce the same captures from both. That cross-check is the cheapest
  correctness test this library has, because the engines share nothing below
  the instruction set. Three things enforce it: the conformance runner asks
  every eligible engine and compares them, `make check-engine-equivalence`
  sweeps random patterns, and `tests/fuzz/fuzz_crossengine.cpp` aborts on a
  disagreement.
- **It never asks which dialect it is running.** See above.

## Adding a construct

A construct arrives as: a `GRX_FEATURE_*` bit if any dialect lacks it, a
`GRX_NodeKind` if the parser produces a distinct node for it, a `GRX_IRKind`
or a mode in `semantics_internal.h` if it survives lowering, a `GRX_Opcode` if
codegen emits one, and the code in each engine that can run it.

When only a backtracking engine can run it, analysis must clear
`GRX_Facts::is_regular`. That is the one field engine selection reads -
`grx_exec_program_needs_backtracking()` returns `!facts.is_regular` and does
not rescan the program - so missing it is the defect shape to watch for: the
program compiles, `GRX_ENGINE_AUTO` hands it to the Pike VM, and the VM either
mis-executes it or fails at a point far from the cause. Before analysis has
run, `grx_facts_init()` leaves `is_regular` at 0, so the conservative answer
is the default rather than something to remember.

## Fuzzing

The pattern is the untrusted input here, which is the opposite of the rest of
the suite: elsewhere the format is fixed and the bytes are hostile, and here
the *program* is the bytes.

The library is rebuilt with `-fsanitize=fuzzer-no-link` rather than linking
the ordinary shared library, so libFuzzer sees the parser's branches. The
first byte of each input selects the dialect, the options and the limits, so
that every syntax and the capped paths are reachable rather than only the
defaults in one dialect. Keep that convention when adding a harness.

Three harnesses, and the split between them is the point:

- `fuzz_pattern` fuzzes the **pattern**, which finds parser and compiler
  defects.
- `fuzz_subject` holds a corpus of interesting patterns fixed and fuzzes the
  **subject**, which is where the engines' own defects live. A fuzzer that
  varied both would spend almost all its time on patterns that do not
  compile.
- `fuzz_crossengine` runs **both engines** on one program and aborts when
  they disagree.

Keep the budgets small in a harness that runs a subject. `max_steps` defaults
to ten million, and `fuzz_subject` runs every input four ways; with the
defaults it managed 83 executions a second, which is a fuzzer that explores
almost nothing. The limits' own arithmetic is unit-tested, so the fuzzer's job
is to reach many *shapes* of input rather than to exhaust one budget.

```bash
make fuzz FUZZ_TIME=3600
make fuzz-run-crossengine FUZZ_TIME=600
```

## Memory

Every allocation goes through the `GRX_Allocator` the caller supplied.
Two helpers in `tests/test_helpers.h`:

`grxtest::CountingAllocator` is how a test states that a call allocated
nothing, or that everything it allocated came back.

`grxtest::FailingAllocator` refuses the *n*th allocation and lets every other
through. `tests/unit/test_oom.cpp` counts how many allocations a whole
compile-and-match takes and then runs it again once per allocation with that
one refused. That is how the `GRX_ERR_OOM` branches get exercised at all -
they are unreachable by hand - and what it checks is that the refusal comes
back as a result code and that nothing leaked. The failure path is the one
that unwinds a half-built structure and is never taken in ordinary use, so a
missing `free` lives there for years.

## What is deliberately absent

- **A stream abstraction.** The rest of the suite reads input through a
  `<PREFIX>_Stream`, because the input is a file whose size is not known in
  advance. A pattern and a subject are buffers the caller already holds, so
  the stream would be a layer with nothing on the other side of it. If
  streaming subjects are ever wanted, that is a design decision to make
  explicitly rather than a hole to fill by copying model's `stream.h`.
- **Substitution and splitting.** `grx_regex_replace()` and a split are the
  obvious next surface, and they need decisions the matcher does not: what a
  replacement template's syntax is, and which dialect's spelling of `$1`
  versus `\1` applies. Specified in dialects.md section 5.11 and scheduled as
  WP-16; not built.
- **Iteration.** `grx_regex_search_next()` is WP-15. The rule it has to
  apply - what a search-all loop does after an *empty* match - is already on
  the profile as `GRX_IterationRule`, because it is a dialect decision and
  there are three answers in the wild.
- **A `GRX_Node` accessor API.** The syntax tree is internal. A consumer that
  wants to walk a pattern - to translate between dialects, say - is a reason
  to widen the public API deliberately, not a reason to install
  `parse_internal.h`.
