# Development

**Status:** describes the scaffolding as it stands. The engine sections say
what a new piece has to arrive with, not what exists. The target layout,
the pipeline and the engines are specified in [design.md](design.md), which
takes precedence where the two differ; this page is corrected as each work
package in [plan.md](plan.md) lands.

## Layout

```
include/ghoti.io/regex/   Public headers
src/core/                 Result strings, limits, the allocator
src/syntax/               The dialect table
src/parse/                Pattern text to a syntax tree
src/compile/              Syntax tree to a program
src/exec/                 The Pike VM and the backtracking engine
src/charclass/            Character-class sets
src/unicode/              UTF-8 and case folding
src/regex.c               Version entry points
tests/unit/               Unit tests (gtest)
tests/fuzz/               libFuzzer harness and seed corpus
```

The headers mirror the modules, with two exceptions: `regex.h` is the umbrella
rather than the compiled-regex header - that is `compile.h` - and
`charclass`/`unicode` have no public header at all, because nothing outside
the library has a reason to build a character class by hand.

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

An engine takes a `GRX_ExecRequest` and fills in a `GRX_Match`. The two that
exist are the whole intended set - a lockstep simulation for the linear-time
guarantee, and a backtracker for everything else - and a third would need a
reason beyond speed.

Whatever an engine does, two properties are not negotiable:

- **It respects `max_steps` and `max_backtrack`.** An engine that can loop
  without a bound is an engine that turns a pattern into a denial of service.
- **It is checked against the other one.** Any pattern both engines can run
  must produce the same captures from both. That cross-check is the cheapest
  correctness test this library has, and it belongs in the suite as soon as
  either engine runs.

## Adding a construct

A construct arrives as: a `GRX_FEATURE_*` bit if any dialect lacks it, a
`GRX_NodeKind` if the parser produces a distinct node for it, a `GRX_Opcode`
if the compiler emits one, the code in each engine that can run it, and - when
only the backtracker can - an entry in
`grx_exec_program_needs_backtracking()`. Missing that last one is the defect
shape to watch for: the program compiles, `GRX_ENGINE_AUTO` hands it to the
Pike VM, and the VM either mis-executes it or fails at a point far from the
cause.

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
