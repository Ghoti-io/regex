# Ghoti.io Regex

Regular expressions across the major dialects, in C17. One parser reads
sixteen syntaxes - POSIX BRE and ERE, GNU's extensions, Perl, PCRE,
ECMAScript, Python, Java, .NET, Ruby, RE2, Rust, Tcl, Vim and Emacs - from a
table that says what each one has, and three engines run the result: a Pike VM
that is linear in the subject length, a backtracking engine for the constructs
no lockstep simulation can express, and a bit-state engine that is the
backtracker with a memo and the linear bound back.

**Status: under construction.** Nine dialects parse, compile and match on
all three engines: ECMAScript in its legacy, `u` and `v` modes, checked
against Node 24; PCRE2 and Perl against pcre2 10.46 and perl 5.44.0;
`posix-bre`, `posix-ere`, `gnu-bre` and `gnu-ere` against glibc and musl;
Python against CPython 3.13; and Vim against both of vim's own engines,
`set re=1` and `set re=2`. Every one of those references runs in a **pinned
container image** rather than being whatever the machine has installed -
[`tools/oracle/containers/IMAGES`](tools/oracle/containers/IMAGES) is the
list, and `make oracle-version` prints what would answer.
`text` validates JSON Schema's `pattern` and `patternProperties` through
this library. Of 37,396 conformance vectors, **every dialect passes 100%
except Perl**, which passes 6,159 of 6,164: five are a gap - perl 5.44 made
its `\l`, `\u`, `\L`, `\U` and `\F` escapes operators of the pattern and
this library has not built them - and eight more are defects in Perl itself,
excluded from the denominator and each named in
`tests/data/vectors/known-gaps.txt` with the reproduction that demonstrates
it. There are no gaps left in that file. Two of the nine have no vector
corpus and are gated by a generator alone: Python, because CPython removed
`re_tests.py` and Debian does not ship it, over 4.2 million rows against
`re` itself; and Vim, which never had one, over 10,180 rows asked of both
of its engines. The other seven dialects are named and report
`GRX_ERR_UNSUPPORTED`. See [Status](#status) below for exactly what
works today.

(This paragraph has been wrong four times, every time by lagging: first it said
the compiler and the engines were stubs, after they had stopped being stubs;
then it quoted 90.9% and 94.7% for PCRE2 and Perl, and named twelve
unsupported dialects, after four more had front ends; then it said nine
unsupported, after Python made it eight; then eight unsupported and eight
working, after Vim made it seven and nine - and that one sat directly above
a table row reading "Vim front end | working". A status line that is
wrong in the *safe* direction is still wrong, and it sits above a table that
contradicts it. It was "checked against a fresh `make test` when it
changes", which is exactly the discipline that let it lag four times: the
paragraph goes stale when something *else* changes, so the moment it needs
checking is the moment nobody is looking at it. `make check-status-line`
counts the arms of `grx_frontend_for()` and fails if the two numbers here
disagree with it.)

## Example

```c
#include <ghoti.io/regex/regex.h>
#include <stdio.h>

int main(void) {
  GRX_Error error;
  GRX_Regex * regex = NULL;

  // ECMAScript with the `u` flag. PCRE2 and Perl are built too.
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
make examples   # examples/*.c - see below
make help       # every target
```

`examples/` is one program per thing worth showing rather than one per API
call: `regex_info` prints what a dialect has and tries a pattern in it,
`posix_stream` is a grep and a sed over the four POSIX and GNU rows,
`perl_extract` puts one named-group pattern to both PCRE2 and Perl to show
where their templates part, `vim_substitute` is a `:s` over a string - the
four magic levels, `\zs`, and a replacement that changes case - and the two
`json_schema_*` programs are the `text` seam.

The dependencies are [`ghoti.io-cutil`](../cutil) for the allocator and the
growable array, and [`ghoti.io-unicode`](../unicode) for the Unicode
Character Database and the algorithms over it - case folding, segmentation,
`\N{NAME}` and script runs all read it. Both are found through pkg-config
and nothing else. Build the suite into a local prefix first:

```bash
# from the workspace root, which holds bootstrap.sh and libs/
./bootstrap.sh
export PKG_CONFIG_PATH="$PWD/.local/share/pkgconfig"
make -C libs/regex test PREFIX="$PWD/.local"
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

**Limits.** `GRX_Limits` caps every unbounded quantity, and unlike the rest
of the suite most fields have a non-zero default. A regular expression is the
one input where a small pattern can cost unbounded time: `(a+)+$` against
thirty `a`s is the standard demonstration. `max_steps` and `max_backtrack`
are what turn that into `GRX_ERR_LIMIT` instead of a hang.

Every field is tunable and `0` means no limit, with `grx_limits_unlimited()`
for the all-zero structure. That sentence was in the header for a long time
before anything checked it, and checking it found two fields that were in the
documentation while nothing read them - a limit a caller can set and cannot
feel is a defence they only think they have. Twelve of the thirteen are
enforced now, each refusing with its own diagnostic so a caller who has to
raise one is told which; the thirteenth is `max_recursion_depth`, reserved
until a dialect has recursion, with a test that fails the moment one does.

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
| Gates: `check-symbols`, `check-layering`, `check-unicode-tables`, `check-diagnostics`, `check-engine-equivalence`, `check-json-schema-suite` | working, in `TEST_GATES` |
| Gates: `check-oracles` - fifteen differential checks: syntax, match, iteration, the search window, properties, numeric properties, properties of strings, POSIX, the Perl family, the Perl/PCRE2 syntax split, script runs, newline conventions, and three replacement and split grammars | working; not in `TEST_GATES`, because they need Node, perl or pcre2 |
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
| Unicode tables: 457 properties, both foldings, both property-name resolvers, 40,951 character names, and the script sets a script run is checked against | working, UCD 17.0.0 |
| UTF-8 reverse decode and whole-buffer validation | working |
| Parser skeleton and hook interface | working |
| ECMAScript front end, legacy and Unicode modes | working |
| ECMAScript UnicodeSets (`v`) mode | working - set operations, string disjunctions, the seven properties of strings |
| PCRE2 and Perl front ends | working - verbs, conditionals, recursion, branch reset, `\Q..\E`, extended modes, the leading directives (PCRE2's alone; Perl has none of them), Perl's `\N{NAME}` against the full character-name table, `(*LIMIT_MATCH=n)` applied rather than parsed and dropped, script runs `(*sr:`/`(*asr:` against UTS #39's augmented script sets, `(?[ ])` extended classes in **both** dialects, and the six newline conventions `(*CR)` and kin |
| POSIX and GNU front ends | working - one reader for `posix-bre`, `posix-ere`, `gnu-bre` and `gnu-ere`; both halves of `REG_NEWLINE`, as `GRX_OPT_MULTILINE` and `GRX_OPT_NEWLINE_TERMINATES` |
| Python front end | working - CPython 3.13's grammar, which is the Perl family's with a closed escape alphabet, `(?P<n>)` as the only named spelling, no `(*...)` construct, a reference that must name a group that has *closed*, and global flags only at the start |
| Vim front end | working - all four magic levels, chosen inside the pattern; postfix lookaround `\@=` and kin, with the byte bound `\@123<=` applied; `\&`; `\zs` and `\ze`; `\%[...]` over atoms; the whole `\%` family including the byte column `\%23c` and the screen column `\%23v`; its named classes and its seven own `[:name:]` classes, all enumerated against vim one code point at a time; and its `:s` replacement with the six case markers. Its two matching models are built as well: a base and the composing characters after it are **one character**, with `\Z` to carry them rather than match them, and `\<`/`\>` hold where its character *class* changes, of which it has nine |
| Every dialect but those nine | named, `GRX_ERR_UNSUPPORTED` |
| Lowering, analysis and code generation | working |
| Pike VM | working - the regular subset, in linear time |
| Backtracking engine | working - backreferences, lookaround, atomic and possessive |
| Conditionals, recursion and subroutine calls, `\K`, the control verbs | working - on the backtracking engine, which is the only one that can run them |
| Bit-state engine | working - the backtracker with a memo, and the linear bound back |
| Search window, NOTBOL/NOTEOL/NOTEMPTY, `grx_regex_search_next()` | working - generated against pcre2 and against perl's own loop |
| `grx_regex_replace()` and `grx_regex_split()` | working - ECMAScript's, PCRE2's, Perl's, sed's and Python's template grammars; splitting is a per-dialect axis with three values, ECMAScript's, perl's and Python's (dialects.md section 5.16) |
| `grx_pattern_lint()`, the JSON Schema subset check | working |
| Limits | measured, not guessed; dialects.md section 7 |
| The `text` seam for JSON Schema | working - `pattern` and `patternProperties` validate through this library |

**Conformance.** Sixteen differential checks, each against whichever
implementation *defines* the thing it asks about: Node 24 for ECMAScript,
pcre2 10.46 and perl 5.44.0 for the Perl family, glibc and musl for POSIX and
GNU, GNU sed for the POSIX replacement grammar, and CPython 3.13 for Python.
Every one of them runs in a pinned image, so what a green run names is a set
of versions rather than a set of programs that happened to be installed;
`make check-oracles` runs all sixteen and each prints which reference
answered it above its numbers. A few are described below; [testing.md](documentation/testing.md)
§5 has every one, and says for each what was broken on purpose to prove the
check can fail.

`make check-oracle-syntax` compares accept and reject over 960,000 patterns
per seed - an exhaustive corpus of every string up to three characters over
the grammar's punctuation, plus a random corpus of longer ones, each run with
eight flag sets including `v` and `iv`. No disagreement.

`make check-oracle-match` compares every group's span for random patterns
against random subjects, including backreferences, lookahead, lookbehind and
UnicodeSets classes. No disagreement over six seeds.

`make check-oracle-properties` asks both implementations which code points
match each `\p{...}` - all 1,114,112 of them, for all 457 properties. No
disagreement.

`make check-oracle-string-properties` does the same for the seven properties
of *strings*, which have no code-point space to walk: the universe is every
emoji sequence UTS #51 knows about, qualified and not, so a table that is too
large fails on one half and one that is too small fails on the other. 36,575
cases, no disagreement.

`make check-oracle-window` varies the six fields of `GRX_SearchOptions` that
decide an answer - `begin`, `end` and the four subject-side flags - against
`pcre2_match()`, whose `startoffset`, `length` and `NOT*` options map onto
them exactly. Every other check in this list searches the whole subject with
no flags, so all six sat at one value across the whole suite until this
existed; the first run found that `NOTBOL` and `NOTEOL` were suppressing
`\A`, `\Z` and `\z` as well as `^` and `$`, which neither pcre2 nor glibc
does.

`make check-oracle-iterate` asks for *every* match rather than the first,
against `String.prototype.matchAll` and perl's `while ($s =~ /$re/g)` - both
of them loops the reference itself provides, which is why not pcre2, whose
find-all loop its caller writes. It found that what `\G` asserts is a second
axis: Perl and PCRE2 share the empty-match rule and disagree about whether
`\G` follows the loop when it advances past a failure.

`make check-engine-equivalence` requires every engine that can run a program
to give the same answer for it, which is the invariant of
[design.md](documentation/design.md) §3.5.4. Five dialects, three engines,
no disagreement; it runs as part of `make test` rather than only under
`make check-oracles`, because it consults no reference implementation and
nothing else enforces that invariant. The `crossengine` fuzzer checks the
same thing on random input.

One `v`-mode rule goes the other way and the oracle is not followed:
[dialects.md](documentation/dialects.md) §8.6.1 has the table and the
reasoning.

**test262.** Every one of those gates was green the day tc39's own corpus was
imported, and the import found five defects within a minute. Three were
rules: ECMA-262's list of binary property names is *closed*, so
`\p{Other_Alphabetic}` is a `SyntaxError` and eleven such names were being
accepted; `\p{Script=Unknown}` and `\p{Changes_When_NFKC_Casefolded}` are
required and were missing; and a group name uses the Unicode escape grammar
whatever the flags say, so `(?<\u{1d5b0}x>y)` is valid without `u`. Two were
engines: a backreference inside a lookbehind ran forwards, reporting captures
whose end preceded their start, and the Pike VM's thread set discarded a
thread it should have kept. [dialects.md](documentation/dialects.md) §8.6 has
all five.

That is the case for a corpus somebody else wrote. A generator explores the
grammar this library already implements; it does not think to ask whether
`\p{Other_Alphabetic}` should be refused.

`tools/corpus/import_test262.py` produces two files, because the corpus
answers two different questions. **394 records** are cases whose expectation
test262 states in a machine-readable form - the `negative:` frontmatter and
`assert.throws(SyntaxError, ...)` - so a rate over them is a test262 pass
rate, and it is **394 of 394**. **26,569 records** are patterns harvested
from the files whose assertions are about JavaScript rather than about the
pattern; the corpus contributes 4,566 hand-written expressions and the
oracle contributes every answer. That is a corpus import and not a
conformance rate, and the two files say which they are.

**JSON Schema.** `text` has no regular-expression engine and is not going to
grow one, so its `pattern` and `patternProperties` keywords arrive through a
provider vtable that a caller fills in. `examples/json_schema_provider.c` is
that adapter written against this library - about sixty lines - and
`make check-json-schema-suite` runs JSON-Schema-Test-Suite files through `text`
with it: **51 of 51** cases in draft2020-12 and **46 of 46** in draft7, no
group skipped. `tools/jsonschema/fetch.sh` fetches the corpus at the commit
pinned in `tools/jsonschema/SUITE_COMMIT`; it is not vendored, for the same
reason the UCD is not. It is one of `TEST_GATES`, and a missing `text` or an
unfetched corpus fails the run rather than skipping it - as does a run that
answers fewer questions than the pinned corpus asks.

The files run are `pattern` and `patternProperties`, which is what the seam is
for, plus `maxLength` and `minLength`, which are not. Those two are there
because the first version of this check ran only the pattern files and so
could not have caught a defect in the pair it exists to validate - and there
was one. `text` measured string length in bytes where the specification counts
characters, so `{"maxLength": 1}` rejected `"é"`. `maxLength.json` catches
that on sight, because it asks whether two astral characters satisfy
`maxLength: 2`.

The interesting part of that seam is `search_fn`'s third answer. A search that
could not finish - a budget spent on a pattern whose worst case is exponential
- has not said the instance is invalid, and reporting it as "no match" would
turn a denial-of-service defence into a wrong validation result. It becomes
`GTEXT_JSON_E_LIMIT`, which is neither valid nor invalid, and a caller can
tell the difference.

**Vectors.** **33,837 checked-in `.rxt` records** run in `make test`, with no
oracle needed. Their expectations are the references' own, not this
library's. A deliberately wrong record sits beside them in a self-test
corpus, and a test expects the runner to fail it - so that "the suite passes"
cannot mean "the suite ran nothing".

| Corpus | Records | Agreeing |
| --- | --- | --- |
| ECMAScript, from Node 22 and test262 | 28,559 | **100%** |
| PCRE2, from pcre2test 10.46's `testinput1` and `testinput2` | 1,869 | **100%** |
| Perl, from `re_tests` under Perl 5.44, and generated boundary, case-folding and character-name vectors | 6,164 | **99.92%**; 5 gaps, 8 excluded |
| GNU ERE, from Spencer's test set answered by glibc 2.41 | 270 | **100%** |
| GNU BRE, the same set read as a basic RE | 159 | **100%** |
| POSIX ERE, the same set where glibc 2.41 and musl 1.2.6 agree | 245 | **100%** |
| POSIX BRE, the same set read as a basic RE | 135 | **100%** |

The POSIX rows are measured differently, and the difference is the point.
glibc's `regcomp` is GNU - it accepts `\|`, `\+`, `\w` and `\<` in a basic
RE - so it can be asked what GNU does and cannot be asked what POSIX does.
musl's regex descends from Laurikari's TRE and shares no code with it, but is
not strict POSIX either: its basic RE takes the same GNU operators, and it
refuses the `[[.x.]]` and `[[=x=]]` that POSIX requires. Neither can decide
alone. So the POSIX vectors are only the cases the two answer *identically* -
41 of Spencer's rows they answer differently are left out, and so are the 8
using a construct these dialects do not have. Where two implementations
sharing no code agree, that is the strongest evidence this machine can offer
for what POSIX means in practice; where they differ, the question is recorded
as open rather than settled by picking a side.

The 8 that do not agree are listed one per line in
`tests/data/vectors/known-gaps.txt`, with the reason written beside each. That file is a gate in both directions: a vector that
fails and is not listed fails the suite; a vector that *is* listed and passes
fails it too, with "remove the entry"; and an entry naming a record the
corpus no longer has fails it as well. So the list can only shrink by
somebody noticing.

Entries come in two categories, and they are counted differently. A **gap**
is this library answering differently from a reference that is right; it
stays in the denominator and counts as a failure, never as a pass. A
**reference defect** is the reference being wrong, and it leaves the
denominator, because a wrong expectation is not a question this library can
be scored against. That second category is a lever that raises the published
rate, so it carries two rules: the entry must *demonstrate* the defect with a
reproduction someone else can run rather than argue for it, and the excluded
count is printed beside the rate everywhere the rate appears - which is why
the table above says "100% of 4,516; 8 excluded" and not "100%". A rate that
rose because rows left the denominator has to say so.

All eight are reference defects, and all eight are Perl's. The **gap**
category is empty, and was not for a day: seven PCRE2 vectors named a
newline convention this library read and ignored, and refusing them put the
row at 99.63% until the conventions were built. Every one of those seven
says only `expect: compiles`, which is how a compiles-only vector reads as
coverage while the construct is inert. **Six** turn on
what a *failed* attempt leaves in the capture slots, and a minimal pair shows
that Perl has no rule there: `((a){2})+` against "aaa" gives group 2 as 1-2
in Perl, in pcre2 10.46 and here, but widen the repeated body by one
character and only Perl moves - `((aa){2})+` against ten a's is 8-10 in Perl
and 6-8 in both others, `((aaa){2})+` is 6-9 against 3-6. Identical shape,
one operand wider, opposite answers, because the width is what Perl's
compiler uses to choose among its repeat opcodes and those differ in whether
they restore a capture offset on failure. **Two** are a branch reset defect,
[#24577](https://github.com/Perl/perl5/issues/24577), a regression in 5.38
through 5.44 fixed upstream on 2026-07-22 by
[#24588](https://github.com/Perl/perl5/pull/24588); the pinned perl reports
group 1 of `(?|(a)|(b))` against "b" as `"b"` and then its own `(?(1)x|y)`
reads that same group as unset, so one interpreter calls it set and unset in
two lines. Both are re-checked when the pinned reference versions move, and
that has now happened once: the pin went to 5.44.0 on 2026-09-25 and both
are still wrong there, the fix having landed in blead after 5.44 shipped.

The POSIX and GNU rows have no gaps left. Twelve of them closed in one
session and eleven were mislabelled: one was leftmost-longest, which the
profile had asked for since the table was written and no engine read
(WP-24), and ten were read as needing the tagged-transition machinery WP-26
defers and turned out to be a single empty-iteration rule that glibc and musl
both state plainly - an empty iteration runs while the repeat has consumed
nothing and not after. `(a*)*` against "b" and `(a|)*` against "aaaa" are the
two halves of it.

**Linear time.** `(a|aa)*b`, `(a+)+b` and `(a*)*b` - the patterns that make a
backtracking engine hang - run against 100,000 characters in around fifty
milliseconds, and a test asserts the *scaling* rather than the wall clock.
The backtracking engine finishes most of them too: on a program whose
behaviour is a function of `(instruction, position)` alone it arms the
bit-state bitmap once a run has cost more than a memoised one could, which
is Perl's super-linear cache under another name.

**Limits.** Measured, not guessed. 264 real patterns were asked how much of
each resource they need and the tightest default leaves five times what the
costliest of them uses; 15 of the 17 pairs in the ReDoS corpus are now
answered by the backtracker itself in under a millisecond, and the two whose
body can match empty - the shape whose progress register makes the memo
unsound - are refused in about 120 milliseconds and answered by another
engine in under one.
[dialects.md](documentation/dialects.md) §7 is the report.

387 tests plus the vector corpus, clean under Valgrind and under
ASan+UBSan. `make coverage` reports 90.8%; three of the five directories
[testing.md](documentation/testing.md) §12 sets a 90% floor for are over it
and two are under, with the shortfall counted there rather than explained
away. The most recent fuzz campaign was 3,834,620 runs across the three
harnesses in two modes - length-controlled and full-length - with no crash. The tests that record a
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

LGPL-3.0-only. See [COPYING.LESSER](COPYING.LESSER) for the license, and
[COPYING](COPYING) for the GPL text it is written as additional permissions
on top of.

Contributions are not being accepted at this time; see
[CONTRIBUTING.md](CONTRIBUTING.md) for what is useful instead.
