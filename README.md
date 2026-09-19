# Ghoti.io Regex

Regular expressions across the major dialects, in C17. One parser reads
sixteen syntaxes - POSIX BRE and ERE, GNU's extensions, Perl, PCRE,
ECMAScript, Python, Java, .NET, Ruby, RE2, Rust, Tcl, Vim and Emacs - from a
table that says what each one has, and two engines run the result: a Pike VM
that is linear in the subject length, and a backtracking engine for the
constructs no lockstep simulation can express.

**Status: under construction.** The Unicode tables, the character-class
algebra, the parser and the ECMAScript front end are built and checked
against Node 22. The compiler and both engines are still stubs that report
`GRX_ERR_UNSUPPORTED`, so nothing *matches* yet; see [Status](#status) below
for exactly what works today.

## Example

```c
#include <ghoti.io/regex/regex.h>
#include <stdio.h>

int main(void) {
  GRX_Error error;
  GRX_Regex * regex = NULL;

  // ECMAScript with the `u` flag, which is the only dialect built today.
  if (grx_regex_compile_with_allocator("(\\w+)@(\\w+)", 11,
          GRX_SYNTAX_ECMASCRIPT, GRX_OPT_CASELESS | GRX_OPT_UTF, NULL, NULL,
          &error, &regex)
      != GRX_OK) {
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
| Build, install, Doxygen | working |
| Gates: `check-symbols`, `check-layering`, `check-unicode-tables` | working, in `TEST_GATES` |
| Gates: `check-oracles` - syntax, match, properties, properties of strings, cross-engine | working; not in `TEST_GATES`, because they need Node |
| Gate: `check-limits` - what real patterns cost against the defaults | working; the report behind dialects.md section 7 |
| Result codes, limits, allocator, version | working |
| Diagnostics and error reporting | working |
| The arena behind every table | working |
| AST, IR and instruction set, with their dumps | working |
| `GRX_Facts` and `grx_regex_facts()` | working; analysis computes them at compile time |
| Canonical character-class table | working |
| Dialect table, semantic profile, `grx_options_parse()` | working; the rows for dialects with no oracle installed are provisional |
| UTF-8 decode and encode | working, strict |
| Character classes: membership, insertion, set algebra, fold closure | working |
| Unicode tables: 454 properties, both foldings, both name resolvers | working, UCD 17.0.0 |
| UTF-8 reverse decode and whole-buffer validation | working |
| Parser skeleton and hook interface | working |
| ECMAScript front end, legacy and Unicode modes | working |
| ECMAScript UnicodeSets (`v`) mode | working - set operations, string disjunctions, the seven properties of strings |
| Every dialect but ECMAScript | named, `GRX_ERR_UNSUPPORTED` |
| Lowering, analysis and code generation | working |
| Pike VM | working - the regular subset, in linear time |
| Backtracking engine | working - backreferences, lookaround, atomic groups |
| Conditionals, recursion, the control verbs | opcodes exist; refused until WP-19 |
| Bit-state engine | working - the backtracker with a memo, and the linear bound back |
| Search window, NOTBOL/NOTEOL/NOTEMPTY, `grx_regex_search_next()` | working |
| `grx_regex_replace()` and `grx_regex_split()` | working - ECMAScript's template grammar and split rule |
| `grx_pattern_lint()`, the JSON Schema subset check | working |
| Limits | measured, not guessed; dialects.md section 7 |
| The `text` seam for JSON Schema | not started; WP-11 |

**Conformance.** Five differential checks against Node 22, which is the
pinned ECMAScript oracle. `make check-oracles` runs all five.

`make check-oracle-syntax` compares accept and reject over 960,000 patterns
per seed - an exhaustive corpus of every string up to three characters over
the grammar's punctuation, plus a random corpus of longer ones, each run with
eight flag sets including `v` and `iv`. No disagreement.

`make check-oracle-match` compares every group's span for random patterns
against random subjects, including backreferences, lookahead, lookbehind and
UnicodeSets classes. No disagreement over six seeds.

`make check-oracle-properties` asks both implementations which code points
match each `\p{...}` - all 1,114,112 of them, for all 454 properties. No
disagreement.

`make check-oracle-string-properties` does the same for the seven properties
of *strings*, which have no code-point space to walk: the universe is every
emoji sequence UTS #51 knows about, qualified and not, so a table that is too
large fails on one half and one that is too small fails on the other. 36,575
cases, no disagreement.

`make check-engine-equivalence` requires every engine that can run a program
to give the same answer for it, which is the invariant of
[design.md](documentation/design.md) §3.5.4. No disagreement. The
`crossengine` fuzzer checks the same thing on random input.

One `v`-mode rule goes the other way and the oracle is not followed:
[dialects.md](documentation/dialects.md) §8.6.1 has the table and the
reasoning.

**Vectors.** 1,567 checked-in `.rxt` records run in `make test`, with no
oracle needed: 100% pass. Their expectations are Node's, not this library's.
A deliberately wrong record sits beside them in a self-test corpus, and a
test expects the runner to fail it - so that "the suite passes" cannot mean
"the suite ran nothing".

**Linear time.** `(a|aa)*b`, `(a+)+b` and `(a*)*b` - the patterns that make a
backtracking engine hang - run against 100,000 characters in around fifty
milliseconds, and a test asserts the *scaling* rather than the wall clock.

**Limits.** Measured, not guessed. 251 real patterns were asked how much of
each resource they need and the tightest default leaves six times what the
costliest of them uses; the 17 pairs in the ReDoS corpus are refused in 105
to 173 milliseconds on an idle machine - 276 to 414 on a busy one, which is
the number that matters - and answered by another engine in under one.
[dialects.md](documentation/dialects.md) §7 is the report.

339 tests plus the vector corpus, clean under Valgrind and under
ASan+UBSan. `make coverage` reports 89%. The tests that record a
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
