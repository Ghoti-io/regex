# Ghoti.io Regex

Regular expressions across the major dialects, in C17. One parser reads
sixteen syntaxes - POSIX BRE and ERE, GNU's extensions, Perl, PCRE,
ECMAScript, Python, Java, .NET, Ruby, RE2, Rust, Tcl, Vim and Emacs - from a
table that says what each one has, and two engines run the result: a Pike VM
that is linear in the subject length, and a backtracking engine for the
constructs no lockstep simulation can express.

**Status: scaffolding.** The layout, the API surface, the dialect table, the
build and the test harness are in place and green. The parser, the compiler
and both engines are stubs that report `GRX_ERR_UNSUPPORTED`; see
[Status](#status) below for exactly what works today.

## Example

```c
#include <ghoti.io/regex/regex.h>
#include <stdio.h>

int main(void) {
  GRX_Error error;
  GRX_Regex * regex = NULL;

  if (grx_regex_compile_with_allocator("(\\w+)@(\\w+)", 11, GRX_SYNTAX_PCRE,
          GRX_OPT_CASELESS, NULL, NULL, &error, &regex) != GRX_OK) {
    fprintf(stderr, "%s at offset %zu\n", error.message, error.offset);
    return 1;
  }

  GRX_Match * match = NULL;
  grx_match_create(regex, NULL, &match);

  const char * subject = "write to Corey@example";
  int matched = 0;
  grx_regex_search(regex, subject, 22, 0, GRX_ENGINE_AUTO, NULL, match,
      &matched);

  if (matched) {
    GRX_Capture user;
    grx_match_group(match, 1, &user);
    printf("%.*s\n", (int)(user.end - user.start), subject + user.start);
  }

  grx_match_destroy(match);
  grx_regex_free(regex);
  return 0;
}
```

## Building

```bash
make            # the shared and static libraries
make test       # build and run every test, and check-symbols
make examples   # examples/*.c
make help       # every target
```

The only dependency is [`ghoti.io-cutil`](../cutil), found through pkg-config
and nothing else. Build the suite into a local prefix first:

```bash
cd .. && ./bootstrap.sh
export PKG_CONFIG_PATH="$PWD/.local/share/pkgconfig"
make -C regex test PREFIX="$PWD/.local"
```

## The API

**Dialects.** `GRX_Syntax` names one syntax; `grx_syntax_spec()` returns it as
data - a set of `GRX_Feature` bits plus a few structural flags. The parser
consults that rather than the dialect constant, so a construct is accepted or
rejected in exactly one place, and the difference between two dialects can be
read in one table instead of inferred from branches. `grx_syntax_name()` and
`grx_syntax_from_name()` map to and from the spelling a caller writes
(`"pcre"`, `"posix-ere"`).

Options that every dialect spells differently - `/i`, `re.I`, `(?i)` - are one
set of `GRX_Option` bits, and are accepted for a dialect whose own syntax
cannot express them, because the caller is not limited to what the pattern
text can say.

**Parsing and compiling.** `grx_pattern_parse()` produces a `GRX_Pattern`, a
syntax tree with the dialect differences resolved away; `grx_regex_compile()`
goes straight to a `GRX_Regex` when the tree is not wanted. The two are
separate because "this is not valid PCRE" is a statement about the text, and a
caller that is linting or translating between dialects wants it without
building a program it will not run.

A `GRX_Regex` is immutable and carries no match state, so one may be used from
several threads at once. The mutable part is `GRX_Match`, which a caller
matching in a loop allocates once.

**Matching.** `grx_regex_search()` finds the first match at or after an
offset; `grx_regex_match()` requires the match to begin there. "No match" is
an outcome rather than an error, so it arrives in an `int *` out parameter and
the return value is reserved for things that went wrong.

`GRX_ENGINE_AUTO` picks the Pike VM unless the program contains a
backreference, a lookaround, an atomic group or a recursion, none of which a
lockstep simulation can run. A caller that needs the linear-time guarantee
asks for `GRX_ENGINE_PIKE` by name and gets `GRX_ERR_UNSUPPORTED` for a
pattern that cannot have it, rather than silently getting the engine whose
worst case is exponential.

**Limits.** `GRX_Limits` caps every unbounded quantity, and unlike the rest of
the suite every field has a non-zero default. A regular expression is the one
input where a small pattern can cost unbounded time: `(a+)+$` against thirty
`a`s is the standard demonstration. `max_steps` and `max_backtrack` are what
turn that into `GRX_ERR_LIMIT` instead of a hang.

**Errors.** Every call that can fail returns `GRX_Result`; `0` is success.
`GRX_ERR_SYNTAX` is this library's one addition to the suite vocabulary, and
it always arrives with a byte offset into the pattern and a message, in a
caller-supplied `GRX_Error`.

**Allocation.** Every allocation goes through the `GRX_Allocator` the caller
supplied - cutil's vtable under a local name - and `NULL` means the default.
Nothing is allocated for the caller to free on a failing call.

## Status

| Part | State |
| --- | --- |
| Build, install, `check-symbols`, `check-layering`, Doxygen | working |
| Result codes, limits, allocator, version | working |
| Diagnostics and error reporting | working |
| The arena behind every table | working |
| AST, IR and instruction set, with their dumps | working |
| `GRX_Facts` and `grx_regex_facts()` | working; analysis computes them in WP-07 |
| Canonical character-class table | working |
| Dialect table and lookup | working; the feature rows are provisional |
| UTF-8 decode and encode | working, strict |
| Character-class membership | working; `grx_charclass_add_range()` is a stub |
| Simple case folding | ASCII only |
| Parser | stub - `GRX_ERR_UNSUPPORTED` |
| Lowering and codegen | not started |
| Pike VM, backtracking engine | stubs - `GRX_ERR_UNSUPPORTED` |
| Substitution and splitting | designed, not started |

127 tests, clean under Valgrind and under ASan+UBSan. The tests that record a
stub's answer are marked `STUB` in a comment and are meant to be deleted with
the stub.

## Design

The design is written ahead of the code, per the suite's convention, and
lives in `documentation/`:

| Page | What it settles |
| --- | --- |
| [design.md](documentation/design.md) | The architecture: the AST-to-IR-to-program pipeline, the three engines and when each runs, the dialect model, memory, limits, errors, the public API as it will be, the target layout |
| [dialects.md](documentation/dialects.md) | The dialect specification: tiers, references and oracles, which constructs each syntax has, the semantic profile on every axis where implementations differ, deviations, and ECMAScript in full |
| [unicode.md](documentation/unicode.md) | The Unicode data: UCD 17.0.0, the tables, generation and checking, case folding, property names |
| [testing.md](documentation/testing.md) | How correctness is established: oracles, the conformance-vector format, the semantic probe suite, corpus imports, fuzzing, the ReDoS corpus |
| [plan.md](documentation/plan.md) | The plan of attack: lanes, phases, work packages with dependencies and "done" criteria, milestones |
| [development.md](documentation/development.md) | Notes on the scaffold as it stands |

The first milestone is ECMAScript with the `u` flag, searched rather than
matched, which is what JSON Schema requires and what `text`'s schema engine
is waiting for.

## License

MIT. See [LICENSE](LICENSE).
