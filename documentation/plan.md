# The plan of attack

**Status:** design. This is the order of work and the split into packages
that different teams can take without stepping on each other. It is derived
from [design.md](design.md); when the two disagree, the design wins and this
page is corrected.

## 1. How the work is organised

**Lanes.** Five, matching the layout in [design.md](design.md) §8. A lane
is a team or a person who owns a set of directories and the tests for them.

| Lane | Owns | Needs to know |
| --- | --- | --- |
| **core** | `src/core`, `src/charclass`, `src/parse` (the parser skeleton), `src/ir`, `src/compile` | the AST and IR shapes; the instruction set |
| **engines** | `src/exec` | the instruction set; nothing about dialects |
| **front ends** | `src/syntax` and the dialect-specific parts of `src/parse` | [dialects.md](dialects.md); the AST; nothing about engines |
| **unicode** | `src/unicode`, `tools/unicode` | the UCD; the range-set representation |
| **conformance** | `tests/conformance`, `tests/data`, `tests/fuzz`, `tools/oracle` | the oracles; the vector format; every other lane's output |

The **surface** work - substitution, iteration, the search options, the
lint - is small and moves between core and front ends by phase. The `text`
integration is a sixth lane owned by the `text` team.

**The contract between lanes is the IR and the instruction set**, and
WP-01 exists to write them down before anyone builds on them. After WP-01,
engines and front ends do not need to talk: a front end produces IR, an
engine consumes a program, and the conformance lane checks that the pair
gives the oracle's answer.

**Sizes** are for one engineer already familiar with the suite's
conventions: **S** is up to a week, **M** two to four weeks, **L** four to
eight. They are estimates of effort, not calendar, and the plan's
parallelism is what makes the calendar shorter than their sum.

## 2. Phases and milestones

```
Phase 0  Foundations              WP-01 .. WP-05      all lanes, all parallel
Phase 1  ECMAScript + engines     WP-06 .. WP-11      → M1: JSON Schema ready
Phase 2  Full ECMAScript, safety  WP-12 .. WP-17      → M2: ECMAScript complete
Phase 3  PCRE2 and Perl           WP-18 .. WP-22      → M3
Phase 4  POSIX and GNU            WP-23 .. WP-26      → M4: tier 1 complete = 1.0
Phase 5  Tier 2                   WP-30 .. WP-33
Phase 6  Tier 3                   WP-34 .. WP-35
Phase 7  Tier 4                   WP-36 .. WP-38
Phase 8  Performance, translation WP-40 .. WP-43
```

Phases 1 and 3 each contain a front-end package and an engine package that
run in parallel against the WP-01 contract. Phase 2 can overlap phase 3:
the `v`-mode work touches the ECMAScript front end and the class algebra,
the PCRE2 work touches the Perl-family front end and the backtracker.

## 3. Work packages

Each package states what it produces, what it needs, and what "done" means.
"Done" always includes: tests that state the requirement (not the
observation), Valgrind and ASan+UBSan clean, `make check-symbols` green,
the design page updated if a decision changed, and the STUB-marked tests
the package makes obsolete deleted.

### Phase 0: foundations

**WP-01 The contract: AST, IR, program, facts, diagnostics.** *core, M.*
**Landed.**
The data structures of [design.md](design.md) §3.1-3.4 as internal headers
with full Doxygen; the growable arenas; `GRX_Diag` and the message
catalogue; `GRX_Error` and `GRX_Limits` extended per §6; `GRX_Facts` and
`grx_regex_facts()`; `grx_pattern_dump()` and `grx_regex_dump()` producing
the formats the conformance runner will read. No parser, no engine.
*Done:* every other lane can write code against these headers without
asking questions; the dump formats are documented in `development.md`.
*Depends on:* nothing. **Start first.**

**WP-02 Unicode tables.** *unicode, M.* **Landed.** `tools/unicode/fetch.sh`,
`gen_tables.py` with its own tests, `UCD_VERSION` = 17.0.0, the generated
tables of [unicode.md](unicode.md) §3 (all but the grapheme rules and the
properties of strings, which are WP-12), the resolvers of §6, the fold
operations of §5, `grx_utf8_validate()`, reverse decoding,
`make check-unicode-tables`. *Done:* the C tests of [unicode.md](unicode.md)
§4 pass; the check target is byte-identical; the tables' size is recorded.
*Depends on:* nothing.

**WP-03 The semantic probe suite.** *conformance, M.* **Landed for the
dialects this machine can run** - Node, Perl, Python, pcre2test, GNU-style
grep and Vim. The cells for Java, .NET, Ruby, Go, Rust, Tcl and Emacs stay
marked **probe** until their tier installs an oracle, which is the point:
a dialect this machine cannot run is a dialect this library cannot claim.
`tools/oracle/`: one
driver per available oracle (Node, Perl, Python, pcre2test, glibc `regcomp`
via a small C program, GNU grep/sed, Vim) that takes pattern, flags and
subject and prints spans or an error in one common form; `probe.py`, which
runs the discriminating cases of [testing.md](testing.md) §5 through every
driver and writes a report; then **every `probe` cell in
[dialects.md](dialects.md) §5 replaced by a value with the oracle's answer
cited**. Also a CI recipe installing the absent oracles (OpenJDK, .NET,
Ruby, Go, Rust, Tcl, Emacs) so tier 2-4 probes can run when their tier
starts. *Done:* no `probe` cell remains for tier 1; the report is
committed under `tests/data/probe/`. *Depends on:* nothing.

**WP-04 Conformance infrastructure.** *conformance, M.* **Landed**, less
the `make vectors-<dialect>` regeneration targets and the ReDoS corpus
runner. The `.rxt` vector
format of [testing.md](testing.md) §3 with its reader in C++; the gtest
runner that runs every vector under `tests/data/vectors/` on every eligible
engine and checks engine agreement; the `make vectors-<dialect>` targets
that regenerate from an oracle and are env-gated; the ReDoS corpus runner;
the structural check that no engine file includes `syntax.h`. Runs green on
zero vectors. *Done:* a vector file with one deliberately wrong expectation
fails with a message naming the vector. *Depends on:* WP-01 for the dump
formats.

**WP-05 Character-class algebra.** *core, S.* **Landed.** `grx_charclass_add_range()`
for real; union, intersection, subtraction, symmetric difference,
complement; fold closure using WP-02's orbits; canonicalisation; the
`max_class_ranges` limit. *Done:* property-based tests against a bitmap
model over a sampled code-point space. *Depends on:* WP-02's fold table
(can start with the ASCII stub and switch). *As built:* the model is exact
rather than sampled - one bit per code point over the whole of Unicode, which
is 136 KB and a few milliseconds per operation - so there is no sample for a
boundary to fall outside of. The four set operations are one sweep
parameterised by a truth table, and closure walks the *orbit* table rather
than the class, so its cost is the size of the folding rather than the size of
the class.

### Phase 1: ECMAScript and the engines

**WP-06 The parser and the ECMAScript front end.** *front ends, L.*
**Landed**, all three modes. The
recursive-descent parser skeleton (groups, alternation, quantifiers,
classes, escapes, names, `max_nesting_depth`), the `GRX_Frontend` hook
interface with its default, and the ECMAScript hooks for legacy and Unicode
modes per [dialects.md](dialects.md) §8.2-8.3 and §8.5's syntax rules.
Produces AST. *Done:* every syntax rule in §8.2 and §8.3 has a test; the
pattern fuzzer runs eight hours clean with the dialect fixed to ECMAScript;
`grx_pattern_dump()` output for the test262 syntax tests matches the
accept/reject verdicts (WP-09 supplies the vectors, but the parser lane runs
them). *Depends on:* WP-01.

**WP-07 Lowering, analysis, codegen, and the Pike VM.** *core then
engines, L.* **Landed.** Lowering per [design.md](design.md) §3.2 with the ECMAScript
profile values; analysis per §3.3; codegen for the regular subset,
including repeat expansion under `max_program_size`, progress registers,
reverse-direction instructions; the Pike VM per §3.5.1 in leftmost-first
mode with copy-on-write registers; `GRX_ENGINE_AUTO` selection on
`is_regular`. *Done:* every regular-subset vector from WP-09 passes on the
Pike VM; a step-count test shows O(n·m) on `(a|aa)*b` against 10^5 `a`s.
*Depends on:* WP-01, WP-05; the ECMAScript AST from WP-06 to test end to
end (unit tests can build IR by hand before then).

**WP-08 The backtracker.** *engines, L.* **Landed**, less the
constructs WP-19 adds. Per [design.md](design.md) §3.5.2:
the frame stack, `max_steps`, `max_backtrack`, captures with undo,
lookahead and lookbehind (reverse execution), backreferences with the
`MATCH_EMPTY`/`FAIL` modes and caseless comparison, the empty-iteration and
capture-reset modes, `GRX_ENGINE_BACKTRACK` by name. Atomic groups,
possessives, conditionals, recursion and verbs are WP-19. *Done:* every
ECMAScript vector passes; every regular-subset vector gives identical spans
to the Pike VM; every pair in the ReDoS corpus returns `GRX_ERR_LIMIT`
within one second at default limits; the stack fuzzer (8 MB → 256 KB stack)
finds no overflow. *Depends on:* WP-01, WP-07's codegen.

**WP-09 ECMAScript conformance.** *conformance, M.* The test262 importer
of [testing.md](testing.md) §7 for the directories in
[dialects.md](dialects.md) §8.6, producing `.rxt` vectors run through Node
so the expectation is the oracle's, not the test's assumed one; the
property-escapes import, which is the real check on WP-02; the
random-pattern generator for ECMAScript with a nightly run against Node.
*Done:* the pass rate is published in `README.md`; every failure is either
a bug filed against WP-06/07/08 or a deviation recorded in
[dialects.md](dialects.md) §6. *Depends on:* WP-04; consumes WP-06/07/08.

**WP-10 Fuzzing.** *conformance, S.* **Landed**, less the 24-hour soak.
`fuzz_subject` (pattern from the
corpus, subject fuzzed), `fuzz_crossengine` (aborts on engine
disagreement), the options-byte layout documented, corpus seeds derived
from the vectors, the small-stack run, a 24-hour soak of all three before
M1. *Done:* the soak is clean and the coverage report is read for the
branches it missed. *Depends on:* WP-08.

**WP-11 The `text` seam.** *text team, M.* **Landed.** A regular-expression
provider vtable in `GTEXT_JSON_Schema_Options` - `compile_fn`, `search_fn`,
`free_fn` and a `ctx`, spelled as the suite's other vtables are - with
`pattern` and `patternProperties` implemented against it and refused, exactly
as before, when no provider is present; the schema-compile error carries the
provider's own message in `context_snippet` and its offset in `offset`, with
`(size_t)-1` passed through rather than folded to 0, because a refusal about
the whole pattern has no position. In this repository,
`examples/json_schema_provider.c` is the adapter, `tools/jsonschema/` runs
JSON-Schema-Test-Suite's `pattern.json` and `patternProperties.json` through
`text` with this library behind it, and `make check-json-schema-suite` is the
gate. *Done:* 51 of 51 cases in draft2020-12 and 46 of 46 in draft7, none
skipped. (The gate began at the two `pattern` files this package names, which
between them could not have caught the byte-versus-character defect the
`maxLength` file found in `text` the same day; `JSON_SCHEMA_FILES` now carries
the two length files as well.)

Two decisions were made in the course of it that the sketch above did not
anticipate. `search_fn` has **three** answers rather than two: a search that
could not finish - a budget spent, an allocation refused - is not "no match",
and recording it as one would turn a denial-of-service defence into a wrong
validation result; it becomes `GTEXT_JSON_E_LIMIT`, which is neither
`GTEXT_JSON_OK` nor `GTEXT_JSON_E_SCHEMA`. And implementing
`patternProperties` meant fixing `additionalProperties`, which applies to the
properties that neither `properties` named *nor* any pattern matched: the
validator's comment said so while only half of it was true, because the other
half had nothing to be true about.
*Depended on:* WP-07 and WP-08 for a working `grx_regex_search()`.

**M1 - JSON Schema ready.** ECMAScript legacy and Unicode modes; both
engines; limits enforced; `text` validates `pattern` and
`patternProperties`; conformance rate published; fuzz soak clean.

Four of the six are done. `text` validates both keywords through this
library as of WP-11, all three engines are built and limits are measured and
enforced. Two remain, and they are the two that are claims about *numbers*
rather than about features: the conformance rate is published over this
library's own generated corpora and against Node, not over test262, which is
the second half of WP-09; and the longest fuzz campaign so far is thirty
minutes per harness per mode rather than the soak WP-10 asks for.

### Phase 2: the rest of ECMAScript, safety, surface

**WP-12 UnicodeSets mode.** *front ends + unicode, M.* **Landed**, less the
test262 `unicodeSets/` import, which waits on WP-09. `v`-flag syntax per
[dialects.md](dialects.md) §8.4; `GRX_NODE_CLASS_OP` and
`GRX_NODE_STRING_SET` through lowering (a string set lowers to an
alternation, longest first); the properties-of-strings tables and the
grapheme-break data. *Done:* test262 `unicodeSets/` vectors pass.
*Depends on:* WP-05, WP-06, WP-07.

**WP-13 The bit-state engine and engine selection.** *engines, M.*
**Landed.** Per
[design.md](design.md) §3.5.3; `max_match_memory`; the selection policy in
`exec.c` with its table of which facts route to which engine, and a test
per row. *Done:* equivalence with the backtracker on every vector it is
eligible for; the ReDoS corpus patterns without backreferences complete
with a match or no-match, not a limit. *Depends on:* WP-08.

**WP-14 Limits measured.** *conformance, S.* **Landed.** The two-corpus method of
[dialects.md](dialects.md) §7; defaults set; the measurements written into
§7. *Done:* no oracle-corpus pattern hits a default; every ReDoS pair hits
`max_steps` in bounded time. *Depends on:* WP-09, WP-10.

**WP-15 Search options and iteration.** *core, S.* **Landed.**
`GRX_SearchOptions`,
the `_ex` entry points, `NOTBOL`/`NOTEOL`/`NOTEMPTY`/`NOTEMPTY_ATSTART`/
`NO_UTF_CHECK`, the window, `grx_regex_search_next()` with the profile's
iteration rule. *Done:* the iteration vectors (every dialect's oracle's
"find all" output) pass for ECMAScript. *Depends on:* WP-07, WP-08.

**WP-16 Substitution and split.** *core, M.* **Landed.** `subst.h`; the template
grammar as a per-dialect table with the ECMAScript grammar first; unset and
missing group rules; `grx_regex_split()` with the dialect's rule for empty
matches and leading/trailing empties. *Done:* vectors from
`String.prototype.replace` and `split` in Node. *Depends on:* WP-15.

**WP-17 The JSON Schema lint.** *front ends, S, optional.* **Landed.**
`grx_pattern_lint()` reporting the first construct outside JSON Schema core
§6.4's subset with its offset. *Depends on:* WP-06.

**M2 - ECMAScript complete. Reached.** All three modes; three engines;
iteration, replace, split; limits measured. The one thing the milestone
named that is not done is WP-12's test262 `unicodeSets/` import, which is
part of WP-09 and is described there; `v` mode itself is checked against
Node over 960,000 patterns per seed instead.

### Phase 3: PCRE2 and Perl

**WP-18 The Perl-family front end and the PCRE2 profile.** *front ends,
L.* The default hooks become the PCRE2 hooks: everything in
[dialects.md](dialects.md) §9 "PCRE2"; the leading directives mapped to
options and limits; `\Q..\E`; extended modes; the class syntax of §5.12;
the numeric-escape rules of §5.7; branch reset and duplicate names;
`grx_options_parse()` for the PCRE2 alphabet. *Done:* pcre2test's
`testinput1` and `testinput2` syntax verdicts match. *Depends on:* WP-06's
skeleton.

**WP-19 The backtracker, completed.** *engines, L.* Atomic groups and
possessive quantifiers (as atomic), conditionals of every kind, recursion
and subroutines with capture frames and `max_recursion_depth`, `\K`, the
verbs with their exact backtracking semantics, `\G`. *Done:* `testinput1`
and `testinput2` match vectors pass on the backtracker; bit-state and Pike
equivalence holds on their eligible subsets. *Depends on:* WP-08, WP-18.

**WP-20 PCRE2 and Perl conformance.** *conformance, M.* The pcre2test
format converter of [testing.md](testing.md) §7; Perl's `re_tests`
converter; both run through their oracles; random-pattern generation for
the Perl family. *Done:* rates published. *Depends on:* WP-04.

**WP-21 The Perl profile.** *front ends, S.* The differences from PCRE2 in
[dialects.md](dialects.md) §9 "Perl"; the folding deviation recorded;
`(?{})` refused with its diagnostic. *Done:* `re_tests` vectors pass.
*Depends on:* WP-18.

**WP-22 PCRE2 and Perl replacement templates.** *core, S.* Including
PCRE2's extended substitution syntax. *Depends on:* WP-16.

**M3.** PCRE2 and Perl conformant; the engine feature set complete.

### Phase 4: POSIX and GNU

**WP-23 The POSIX and GNU front end.** *front ends, M.* Escaped-operator
lexing, no escapes in brackets, `[:class:]`/`[=e=]`/`[.x.]` with the
single-character restriction, the BRE anchor and `*` rules, GNU's escapes,
`REG_NEWLINE` as `GRX_OPT_MULTILINE`. *Depends on:* WP-06's skeleton.

**WP-24 Leftmost-longest.** *engines, M.* The Pike VM's longest mode;
the backtracker's exhaustive mode for BRE backreferences (exponential, and
bounded); the documented submatch approximation. *Depends on:* WP-07,
WP-08.

**WP-25 POSIX and GNU conformance.** *conformance, M.* Spencer's test
suite converted; glibc, grep and sed as oracles. *Depends on:* WP-04.

**WP-26 Exact POSIX submatches.** *engines, L, deferred.* Okui-Suzuki or
Laurikari TNFA disambiguation in the Pike VM's longest mode. Scheduled
after 1.0 unless a consumer needs it.

**M4 - tier 1 complete - 1.0.** Every tier-1 dialect at its published
conformance rate with every deviation listed; the invariants of
[design.md](design.md) §9 all enforced by a test; the API frozen and the
version set to 1.0.0.

### Phase 5: tier 2

One package per dialect, each *front ends, S-M* plus *conformance, S*:
**WP-30 Python** (`re_tests.py`; oracle available), **WP-31 Java**,
**WP-32 .NET**, **WP-33 Ruby**. Each is: the probe cells for the dialect
resolved, the hooks, the profile row, the template grammar, vectors from
the oracle, the README row. .NET balancing groups are a separate,
optional *engines, M* package.

### Phase 6: tier 3

**WP-34 RE2 and Go**, **WP-35 Rust**: profile rows and hooks; the value
is the guarantee that a pattern accepted under these dialects is regular,
and a test that says so.

### Phase 7: tier 4

**WP-36 Vim** (oracle available; the magic-level hook is the work),
**WP-37 Tcl** (the `TCL_ARE` match preference is an engine mode, *engines,
M*), **WP-38 Emacs**.

### Phase 8: performance and translation

**WP-40 Prefilters**: literal prefix, required literal via `memmem`,
first-byte set, min-length rejection; benchmarks in `tools/bench`. **WP-41
Lazy DFA** for match/no-match and boundaries, then captures on the bounded
span. **WP-42 Translation**: `grx_pattern_translate(pattern, to_syntax)`
from the AST, with `GRX_ERR_UNSUPPORTED` naming the construct the target
lacks. **WP-43 UTF-16 offset helper** if a consumer asks.

## 4. What "done" means for a dialect

A dialect is claimed in `README.md`'s status table only when all of these
hold:

1. Its profile row in [dialects.md](dialects.md) §5 has no `probe` cell,
   and every value cites either the reference document or the probe report.
2. Its hooks are implemented and every rule in its
   [dialects.md](dialects.md) section has a test that states the rule.
3. Its conformance vectors, generated from the pinned oracle, pass at a
   published rate, with **every** failure listed in
   [dialects.md](dialects.md) §6 as a deviation with a reason. The target is
   100% of the definitional cases and at least 99.5% of the oracle's own
   corpus; the number is published either way.
4. The pattern fuzzer has run eight hours clean with the dialect selected.
5. `grx_options_parse()` accepts its alphabet and its replacement template
   grammar is implemented.
6. `examples/` has one example in the dialect.

## 5. What "done" means for 1.0

- M4: every tier-1 dialect done by §4.
- Every invariant in [design.md](design.md) §9 has a named test or check.
- Limits measured (WP-14) and their measurements published.
- The three fuzzers soaked 24 hours before the tag.
- `README.md` status table, the parent `README.md` row, and
  `CONVENTIONS.md`'s known-departures section updated.
- The public headers reviewed against `CONVENTIONS.md` §5 one last time,
  then `MAJOR_VERSION` set to 1 so the namespace token changes with it.

## 6. Risks, and where the plan absorbs them

| Risk | Where it lands | Mitigation |
| --- | --- | --- |
| Lowering loses a dialect distinction the engines then cannot recover | WP-07 | the equivalence check and the probe cases are in the suite before lowering is written; a distinction that has no IR representation fails a vector, not a code review |
| The IR contract changes after front ends and engines have built on it | WP-01 | WP-01 is small, first, and reviewed by every lane before Phase 1 starts; changes after that go through the design page |
| An oracle is not deterministic or not what the reference says | WP-03, WP-09 | vectors record the oracle version; a disagreement between oracle and reference is recorded as such and the oracle wins for the vector, the reference for the profile, with both cited |
| test262 and pcre2test formats are large and quirky to import | WP-09, WP-20 | the importers run the case through the oracle rather than trusting the file's own expectation, so a misread test produces a wrong vector that the oracle then corrects |
| Unicode version drift between oracles and tables | WP-02, WP-09 | the skip-with-count rule in [unicode.md](unicode.md) §1 |
| The backtracker's verbs and recursion are subtle and underspecified | WP-19 | pcre2test's `testinput2` is the specification in practice; every verb case there is a vector |
| Performance is unacceptable before Phase 8 | WP-07 | the O(n·m) test in WP-07 and a small benchmark from day one, so that a regression is visible even if absolute speed is not yet a goal |
