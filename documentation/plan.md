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
Phase 4  POSIX and GNU            WP-23 .. WP-26      → M4: tier 1 complete
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
zero vectors. Complete as of the corpus imports: `make vectors`, `make vectors-ecmascript`, `make vectors-pcre` and `make vectors-perl` regenerate each dialect's records, the reader takes `expect: compiles` for a corpus that states syntax verdicts and not spans, and the runner counts a dialect with no front end as skipped rather than failed so that a corpus can be imported before the front end that reads it. *Done:* a vector file with one deliberately wrong expectation
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

**WP-09 ECMAScript conformance.** *conformance, M.* **Landed.** The test262 importer
of [testing.md](testing.md) §7 for the directories in
[dialects.md](dialects.md) §8.6, producing `.rxt` vectors run through Node
so the expectation is the oracle's, not the test's assumed one; the
property-escapes import, which is the real check on WP-02; the
random-pattern generator for ECMAScript with a nightly run against Node.
*Done:* the pass rate is published in `README.md`; every failure is either
a bug filed against WP-06/07/08 or a deviation recorded in
[dialects.md](dialects.md) §6. *Depends on:* WP-04; consumes WP-06/07/08.

`tools/corpus/import_test262.py` reads the `test/built-ins/RegExp`
tree and writes two files: the cases whose expectation test262 states in a
machine-readable form, which is a pass rate and is **394 of 394**, and the
patterns harvested from the files whose assertions are about JavaScript rather
than about the pattern, which is a corpus import and is 26,569 records over
4,566 expressions. The import found five defects on its first run, with every
differential gate green at the time; [dialects.md](dialects.md) §8.6 has
them.

One piece is deliberately not done. The package names the property-escapes
import as "the real check on WP-02", and `make check-oracle-properties`
already walks all 1,114,112 code points against Node for all 457 properties
with no disagreement - so committing a bounded sample of the same comparison
adds little. It is written down here rather than quietly dropped.

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
L.* **Landed**, less the constructs named in
`tests/data/vectors/known-gaps.txt`. The default hooks become the PCRE2 hooks: everything in
[dialects.md](dialects.md) §9 "PCRE2"; the leading directives mapped to
options and limits; `\Q..\E`; extended modes; the class syntax of §5.12;
the numeric-escape rules of §5.7; branch reset and duplicate names;
`grx_options_parse()` for the PCRE2 alphabet. *Done:* pcre2test's
`testinput1` and `testinput2` syntax verdicts match. *Depends on:* WP-06's
skeleton.

**WP-19 The backtracker, completed.** *engines, L.* **Landed.** Atomic groups and
possessive quantifiers (as atomic), conditionals of every kind, recursion
and subroutines with capture frames and `max_recursion_depth`, `\K`, the
verbs with their exact backtracking semantics, `\G`. *Done:* `testinput1`
and `testinput2` match vectors pass on the backtracker; bit-state and Pike
equivalence holds on their eligible subsets. *Depends on:* WP-08, WP-18.

**WP-20 PCRE2 and Perl conformance.** *conformance, M.* **Landed** for
perl. The pcre2test format converter of [testing.md](testing.md) §7; Perl's
`re_tests` converter; both run through their oracles; random-pattern
generation for the Perl family. *Done:* rates published. *Depends on:*
WP-04.

The random-pattern generation was the last piece and it earned its place
immediately. `tools/oracle/perl_diff.py` puts generated patterns through
`tools/corpus/perl_match.pl` and this library side by side, and found
`(?^i:...)` applying the reset and dropping the letter after it: `^` put
`i` into the clear mask, the `i` put it into the set mask, and the caller
applied them as `(options | set) & ~clear`, so the clear won and the
construct silently meant `(?^:...)`. Neither imported corpus contains `(?^`
at all, which is the argument for a generator in one line. There was already
a test saying `(?^i)` compiles; asking whether a construct is accepted is
not asking whether it works.

`GRX_SYNTAX_PCRE` has no generated comparison. pcre2 is its definition and
`pcre2test` is here, but it reports matched *text* rather than offsets and
this machine has no `pcre2.h` to link a driver against, so the shape the
other oracles use is not available for it. `perl_diff.py` says so at the
foot of the file rather than implying its coverage extends there.

**WP-21 The Perl profile.** *front ends, S.* The differences from PCRE2 in
[dialects.md](dialects.md) §9 "Perl"; the folding deviation recorded;
`(?{})` refused with its diagnostic. *Done:* `re_tests` vectors pass.
*Depends on:* WP-18.

**WP-22 PCRE2 and Perl replacement templates.** *core, S.* **Landed**, less
PCRE2's extended substitution syntax. Both rows of
[dialects.md](dialects.md) section 5.11 are written and checked against their
references: `${n}`, `${name}` and the bare `$name` for PCRE2, `$+{name}` and
`\$` escaping for Perl, and the three different answers to a reference the
pattern has no group for - an error, nothing, and the text as written.

What is *not* here is `PCRE2_SUBSTITUTE_EXTENDED`: `\U`, `\L`, `${n:+a:b}`
and `$*MARK`. It is an option pcre2_substitute() reads only when asked, this
library does not expose it, and the ordinary grammar is what a caller gets by
default. Perl's `\U` and its kin are left out for a different reason - they
are string operators that happen to be legal in a replacement, and
implementing them without the rest of interpolation would be a grammar this
library invented. *Depends on:* WP-16.

**M3.** **Reached**, less what `tests/data/vectors/known-gaps.txt` names.
PCRE2 and Perl parse, compile, match and substitute; the engine feature set
is complete.

**The POSIX and GNU templates are here too**, which no package named and §4's
fifth condition wanted: `grx_regex_replace()` answered `GRX_ERR_UNSUPPORTED`
for four shipped dialects. POSIX defines no replacement syntax at all, so the
grammar is sed's `s` command - `&` for the whole match, `\&` for the literal
one, `\1` to `\9` a single digit at a time, and a total backslash escape -
with `\0` on the GNU rows only. `tools/oracle/sed_diff.py` and
`tools/oracle/grx_replace.c` measure it the way every other rule here is
measured; 396 cases, no disagreements.

### Phase 4: POSIX and GNU

**WP-23 The POSIX and GNU front end.** *front ends, M.* **Landed.**
Escaped-operator lexing, no escapes in brackets,
`[:class:]`/`[=e=]`/`[.x.]` with the single-character restriction, the BRE
anchor and `*` rules, GNU's escapes, `REG_NEWLINE` as `GRX_OPT_MULTILINE`.
*Depends on:* WP-06's skeleton.

The escaped-operator lexing turned out to be already designed: the spec
table had carried `escaped_specials` on the two BRE rows since it was
written, and nothing read it. Making it real is four questions in the shared
parser - is the group opener here, the closer, the alternation, the interval
- and the front end is then one reader for all four dialects.
`REG_NEWLINE` is only half done, and section 6 of [dialects.md](dialects.md)
says which half.

**WP-24 Leftmost-longest.** *engines, M.* **Landed.** The Pike VM's
longest mode; the backtracker's exhaustive mode for BRE backreferences
(exponential, and bounded); the documented submatch approximation.
*Depends on:* WP-07, WP-08.

The profile had said `GRX_PREFER_LEFTMOST_LONGEST` for these four rows since
the table was written and no engine read it - the same unread-constant shape
`escaped_specials` was - and the cost of that was larger than the one vector
that had found it. `a|ab` against "ab" is the smallest case: no
backreference, so it runs on the Pike VM, and it was answering 0-1 where both
references answer 0-2. Neither the imported corpus nor
`tools/oracle/posix_diff.py` contained it, because every alternation in the
differential's atom list had branches of the same length and Spencer's file
happens not to ask. Adding atoms that tell the two preferences apart is part
of this package for that reason.

The Pike VM stops cutting lower-priority threads at the first match and keeps
the leftmost-start, greatest-end one; it stays linear. The backtracker
searches *past* each match by reporting failure from `MATCH`, which is
exhaustive and exponential and bounded by `max_steps`, with one
short-circuit: a match that reaches the end of the subject ends the search.
The bit-state engine cannot do it at all and says so
([dialects.md](dialects.md) §5.1).

**WP-25 POSIX and GNU conformance.** *conformance, M.* **Landed** for GNU.
Spencer's test suite converted, with glibc as the oracle through
`tools/oracle/posix_match.c`; 429 vectors, `gnu-ere` at 98.15% and `gnu-bre`
at 99.37%. grep and sed were not needed - both read glibc's regex, so they
are the same oracle behind another command. *Depends on:* WP-04.

**Landed** for POSIX too, as far as it can be. musl's regex sources are
fetched and compiled into `tools/oracle/musl_match.c`, giving a second
implementation that shares no code with glibc's - it descends from
Laurikari's TRE. The calibration said what it is worth: musl is *not* strict
POSIX either. Its basic RE takes `\|`, `\+` and `\?` exactly as glibc's
does, and it refuses the `[[.x.]]` and `[[=x=]]` POSIX requires. So neither
reference can decide alone, and the method is their agreement: 380 `posix-*`
vectors from the 420 of Spencer's 463 cases the two answer identically,
`posix-ere` at 97.96% and `posix-bre` at 99.26%. The 41 they answer
differently are recorded as open questions rather than settled by picking a
side, and the 8 using a construct these dialects do not have are left out
because there the *dialect* differs and neither oracle speaks for it.

What is still not measured is what makes a POSIX row a POSIX row: refusing
the GNU operators. No implementation here does that, so the feature table's
`posix-*` entries remain built from the standard, and
[dialects.md](dialects.md) §6 says so rather than implying a rate covers it.

The second oracle paid for itself twice over. It made
`tools/oracle/posix_diff.py` a three-way check, and in making it one exposed
that the differential had never compared a *rejection*: `normalise_ours()`
did not map `compile <diag>` to `compile`, and no generated pattern was
ill-formed enough for anybody to reject, so half of what the tool exists to
ask had never been asked. Ill-formed atoms and the mapping together found the
anchor-quantifier rules in `src/syntax/posix.c`, which had been written for
`^` and `$` and left `\<`, `\>`, `\b` and `\B` alone - the third instance
of a rule stated in a comment and implemented for a subset.

**WP-26 Exact POSIX submatches.** *engines, L, deferred.* Okui-Suzuki or
Laurikari TNFA disambiguation in the Pike VM's longest mode. Scheduled
after M4 unless a consumer needs it.

**Still unbuilt, and no longer the reason for anything.** The ten
`known-gaps.txt` rows filed under "POSIX subexpression disambiguation" were
read as this package's work and were not: they were one empty-iteration rule
([dialects.md](dialects.md) §5.5), which glibc and musl agree on and which
`GRX_EMPTY_LOOP_BREAK_FIRST` now implements. All ten pass, the four POSIX and
GNU rows are at 100%, and no case reached by either corpus or the
differential needs a tagged transition. That does not prove none exists - it
says the ones that were being pointed at were something else, and that this
package is now speculative rather than owed.

**M4 - tier 1 complete.** Every tier-1 dialect at its published
conformance rate with every deviation listed; the invariants of
[design.md](design.md) §9 all enforced by a test; the API frozen.

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
6. `examples/` has one example in the dialect. Five of them so far:
   `regex_info` and the two JSON Schema programs are ECMAScript's,
   `posix_stream` is a grep and a sed across the four POSIX and GNU rows,
   and `perl_extract` is PCRE2's and Perl's - the same pattern under both,
   because the place they part is the template rather than the pattern.

## 5. What "done" means for the first stable release

- M4: every tier-1 dialect done by §4.
- Every invariant in [design.md](design.md) §9 has a named test or check.
- Limits measured (WP-14) and their measurements published.
- The three fuzzers soaked 24 hours before the tag.
- `README.md` status table, the parent `README.md` row, and
  `CONVENTIONS.md`'s known-departures section updated.
- The public headers reviewed against `CONVENTIONS.md` §5 one last time,
  because the namespace token moves with the major version and a review
  after that is a review too late.

The version *number* is deliberately not named here. The ghoti.io libraries
are versioned together rather than one at a time, so this library being ready
is a necessary condition for a release and not a sufficient one, and a number
written into this plan would be a commitment made by the wrong document.
When the number changes, Corey says so.

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
