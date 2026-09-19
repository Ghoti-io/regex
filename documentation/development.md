# Development

**Status:** describes what exists. The target layout, the pipeline and the
engines are specified in [design.md](design.md), which takes precedence where
the two differ; this page is corrected as each work package in
[plan.md](plan.md) lands.

**Landed:** WP-01, the contract - the AST, the IR, the instruction set, the
arena, the diagnostics, `GRX_Facts`, and the three dumps. The parser, lowering,
codegen and the engines are still stubs.

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
src/exec/                 The Pike VM and the backtracking engine
src/charclass/            Character-class sets and the canonical class table
src/unicode/              UTF-8 and case folding
src/regex.c               Version entry points
tests/unit/               Unit tests (gtest)
tests/fuzz/               libFuzzer harness and seed corpus
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
| **AST** | `src/parse/parse_internal.h` | what the text *says*, dialect-shaped | the parser (WP-06) |
| **IR** | `src/ir/ir_internal.h` | what it *means*, dialect-free | lowering (WP-07) |
| **Program** | `src/compile/compile_internal.h` | what the engines *run* | codegen (WP-07) |

All three are arenas of fixed-size nodes linked by `uint32_t` index, with
`GRX_INDEX_NONE` for "no node" - never 0, because index 0 is the root. That
shape means a tree is one allocation, is freed as a unit, and has no link a
range check cannot validate.

**The dialect is gone after lowering.** No file under `src/exec` may name a
`GRX_SYNTAX_` constant, a `GRX_SyntaxSpec` or a `grx_syntax_*` function;
`make check-layering` fails the build if one does. When a dialect difference
seems to need an engine to know which dialect it is running, the construct it
needs is missing from the IR - add it to
`src/core/semantics_internal.h` and to the IR, not to the engine.

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
ir: flags=0x00000001 prefer=leftmost-first nodes=3 captures=1 classes=0
  repeat {0,} greedy empty=fail reset=each @0+4
    capture #1 @0+3
      char 'a' @1+1
```

`grx_program_dump()`, which `grx_regex_dump()` calls, is a flat disassembly
indexed by instruction, expanding the encodings a reader should not have to
decode - a save slot as its group and end, a mode byte as its name:

```
program: flags=0x00000001 prefer=leftmost-first insts=2 classes=0 registers=0
     0  save           2  (group 1 start)
     1  char           'a'  reverse
```

Code points are escaped in all three (`\x0A`, `\u{1F600}`), so a pattern
containing a newline still dumps as one line per node and a diff stays
readable.

## Where a dialect lives

A dialect is a row of `spec_table` in [`src/syntax/syntax.c`](../src/syntax/syntax.c)
and nothing else. The parser reads `GRX_SyntaxSpec` rather than switching on
`GRX_Syntax`, which is what keeps "does this syntax have possessive
quantifiers" a single lookup instead of a condition repeated wherever
quantifiers are parsed.

Adding a dialect is therefore:

1. A constant in `GRX_Syntax`, before `GRX_SYNTAX_COUNT`.
2. A row in `spec_table` and a name in `spec_names`.
3. A section in [dialects.md](dialects.md) naming the reference document.
4. Tests for whatever is *definitional* about it - the thing that makes it a
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
  correctness test this library has, and it belongs in the suite as soon as
  either engine runs.
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
first byte of each input selects the dialect and the limits, so that every
syntax and the capped paths are reachable rather than only the defaults in
one dialect. Keep that convention when adding a harness.

```bash
make fuzz FUZZ_TIME=3600
make fuzz-run-pattern FUZZ_TIME=600
```

Coverage is low until the parser exists; that is expected, and is the
measurement to repeat once it does.

## Memory

Every allocation goes through the `GRX_Allocator` the caller supplied.
`grxtest::CountingAllocator` in `tests/test_helpers.h` is how a test states
that a failing call allocated nothing - which is where a parser that unwinds
by hand usually leaks, and where these tests will earn their keep once there
is a parser to unwind.

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
  versus `\1` applies. Not designed yet; see dialects.md.
- **A `GRX_Node` accessor API.** The syntax tree is internal. A consumer that
  wants to walk a pattern - to translate between dialects, say - is a reason
  to widen the public API deliberately, not a reason to install
  `parse_internal.h`.
