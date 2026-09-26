# Conformance and testing

**Status:** design. The unit-test harness, the pattern fuzzer and the
`CountingAllocator` exist; everything else on this page is specified here
and built by the conformance lane ([plan.md](plan.md)).

## 1. Principles

- **The oracle is the authority.** A dialect names a real implementation
  ([dialects.md](dialects.md) §2). A test for that dialect asserts what the
  implementation does, obtained by running it, not what a person remembered
  or a manual page implied. When the reference document and the
  implementation disagree, the vector records the implementation's answer
  and the disagreement is noted in [dialects.md](dialects.md) §6.
- **Tests state requirements, not observations** (`CONVENTIONS.md` §7). A
  vector generated from an oracle is a requirement by construction. A unit
  test that pins what a function returned is the defect this suite has met
  before and is not written here.
- **Two engines are a test of each other.** Every vector runs on every
  engine eligible for it, and the spans must agree ([design.md](design.md)
  §3.5.4).
- **Skips are counted, never silent.** A vector skipped because its oracle
  is absent, or its Unicode version is newer than the tables, is reported
  with a count so a green run cannot hide a missing oracle.

## 2. Oracles

Every version below is a **pin**, in
[`tools/oracle/containers/IMAGES`](../tools/oracle/containers/IMAGES), not an
observation of this machine - see §2.1.

| Oracle | Drives | Driver |
| --- | --- | --- |
| node 22.23.2 (V8 12.4.254.21, Unicode 17.0) | ECMAScript | `tools/oracle/node_match.mjs` and its siblings, through `node_runner.py`, which adds `--regexp-interpret-all`: without it V8's interpreter and its compiled code disagree and a row's answer depends on how many rows preceded it |
| perl v5.40.1 | Perl | `tools/corpus/perl_match.pl`: `@-`/`@+`, `%+`. A second pin, `perl-next` (v5.44.0, UCD 17.0.0), is reachable with `GHOTI_ORACLE_ALIAS=perl=perl-next` |
| PCRE2 10.46 | PCRE2 | `tools/oracle/pcre2_match.c`, compiled **inside** the image against its libpcre2-dev and run there. pcre2test reports matched *text* rather than offsets and omits a trailing group that did not participate, which is most of what a match comparison asks; `pcre2test` itself answers the corpus import and the probe |
| python 3.13.5 (UCD 15.1.0) | Python | `tools/oracle/python_match.py`, the same batch protocol as the rest, in three modes for `re`, `re.split` and `re.sub`. Converts CPython's character offsets to UTF-8 byte offsets the way `node_match.mjs` converts UTF-16 ones |
| glibc 2.41 `regcomp` | GNU BRE/ERE | `tools/oracle/posix_match.c`, compiled inside the image. Nothing of this library is linked into it |
| musl v1.2.6 `regcomp` | POSIX BRE/ERE, with glibc | `tools/oracle/musl_match.c`: musl's own regex sources, fetched by `tools/corpus/fetch.sh musl`, compiled into the driver **against the image's glibc** - so it declines a NUL (musl has no `REG_STARTEND`) and any byte >= 0x80 (that glibc's `mbtowc`). Never decides alone - see below |
| GNU sed 4.9 | the POSIX and GNU replacement templates | `tools/oracle/sed_match.py`, which runs sed's per-case loop *inside* the image: sed's `s` command takes one script and one subject, so it is the one reference here with no batch protocol of its own |
| GNU grep 3.11 | the `gnu-ere` probe column | `tools/oracle/probe.py`. Only "did a line match", which is all `grep -c` can answer |
| vim 9.2, patches 1-1129 | Vim | `tools/oracle/vim_diff.py`: **one** `vim -es` for a whole run, reading a file of cases and writing a file of answers, through `matchstrpos()` and `matchlist()`. It asks vim's *other* engine (`set re=1`) about the rows that came back different, because vim ships two and they do not always agree. `vim_runner.py` is the one place its command line is spelled |
| OpenJDK, .NET, Ruby, Go, Rust `regex`, Tcl, Emacs | Java, .NET, Ruby, RE2, Rust, Tcl, Emacs | one driver each, same output form. **None is installed on the machine this was written on**, and with the references in images that is no longer what decides whether they can be asked |

Every driver reads a pattern, a flag string and a subject from a JSON line
and prints one JSON line: `{"ok":true,"spans":[[0,3],[1,2],null]}`,
`{"ok":true,"spans":null}` for no match, or `{"ok":false,"error":"..."}`.
Offsets are converted to UTF-8 bytes by the driver, because the driver knows
its runtime's indexing and the runner should not.

Regeneration is `make vectors-<dialect>`, which runs the generator scripts
against the gated oracle and rewrites `tests/data/vectors/<dialect>/`. The
vectors are committed; `make test` never needs an oracle. A CI job with a
container engine regenerates and fails on a diff, which is how an oracle
upgrade is noticed rather than absorbed - and with the references pinned that
job needs nothing installed beyond the engine, which is most of the point.

That gate does not exist yet, and the gap is measurable: regenerating the
ECMAScript vectors today rewrites 3,742 lines of `generated.rxt`, because the
corpus and its generator have drifted apart with nothing watching. The perl,
PCRE2 and POSIX corpora do regenerate byte-identical.

### 2.1 Where a reference comes from

A reference runs in a **pinned container image**, not on whatever this machine
happens to have installed. The pins are
[`tools/oracle/containers/IMAGES`](../tools/oracle/containers/IMAGES), one
line per reference; `tools/oracle/oracle_env.py` is the only place a reference
is spelled, and every gate goes through `tools/oracle/oracle_run.py`, which
resolves the reference and asks its version *before* the gate runs and prints
what answered above the gate's numbers:

```
$ make check-oracle-perl-syntax
oracle(container): perl perl v5.40.1
perl-syntax: 183 constructs, 1 known deviations, 0 stale entries, 0 disagreements
```

This library supplied the finding that pays for pinning the environment:
`check-oracle-vim` took `&encoding`
from `$LANG`, which no file here recorded, and answered **1,855 of 50,980
rows** wrongly at its own defaults on a shell with `LANG=C`. The environment
was invisible until it had to be written down. The same argument applies to
every row of the table above that this machine cannot run at all - OpenJDK,
Ruby, .NET, Go - which stay unasked only because installing them is a
decision about this laptop rather than about the library.

Four things it is built around:

- **No silent fallback.** `GHOTI_ORACLE_MODE` is `container` (the default) or
  `host`. There is deliberately no "try the container, fall back to the host":
  a gate whose reference is not the one it names is worse than one that did
  not run, because it prints the same green line.
- **No `command -v`.** That asks whether something of the right name is on
  `PATH`, which is not the question; the question is whether this gate can
  reach the reference it names, and the only honest way to answer it is to
  reach. A missing reference is an error naming what is missing, not a
  `skipped` line and an exit status of 0.
- **The pin names what decides an answer**, which for a reference carrying
  bundled Unicode tables is the data version rather than the release. Two
  pins on one reference are spelled `perl` and `perl-next`, and
  `GHOTI_ORACLE_ALIAS=perl=perl-next` asks the second one the first's
  questions - which is how the 46 boundary rows excluded for perl's UCD 15.0.0
  can be measured against a perl that has the rules.
- **Paths mean the same on both sides.** The repository is bind-mounted at its
  own path, read-only, with `--network none`. A tool that needs to write says
  so explicitly, so one that forgets fails on a missing path rather than
  writing where nobody looks.

| command | what it is for |
| --- | --- |
| `make oracle-version` | resolve every pin and print what would answer |
| `make oracle-images` | build the references that have no official image |
| `make oracle-clean` | remove this library's built-here images (stock ones are left) |
| `make check-oracles ORACLE_MODE=host` | this machine's own tools, printed as `host, unpinned` with the pin named beside it |
| `make check-oracles ORACLE_REQUIRED=0` | decline loudly instead of failing |

`make check-oracle-env` is the one oracle target inside `make test`, and the
difference is that it consults no reference: it checks that `IMAGES` parses
into the fields its reader expects, that every pin has a way of being asked
its version, and that the filter which keeps the container engine's own
chatter out of a generated corpus header still fires. Each of its checks is
paired with a planted violation it must catch.

## 3. The vector format

`tests/data/vectors/<dialect>/<source>.rxt`. Line-oriented, records
separated by blank lines, `#` comments, so that a diff is readable and a
record can be pasted into a bug report:

```
# generated by tools/oracle/node.js  node v22.23.2  unicode 17.0  2026-09-19
dialect: ecmascript

pattern: (a*)*
flags: u
subject: b
expect: 0-0 -

pattern: ((a)|b)+
flags:
subject: ab
expect: 0-2 1-2 -

pattern: \p{Script=Foo}
flags: u
expect: error syntax

pattern: (?<=a+)b
flags: u
subject: aaab
engines: backtrack
expect: 3-4

pattern: (x+x+)+y
subject: xxxxxxxxxxxxxxxxxxxxxxxxxxxxxx
expect: limit

pattern: ^a+$
flags: u
subject: a
repeat: 1048576
engines: pike
expect: 0-$
```

- `pattern`, `subject`: escaped with `\xHH`, `\uHHHH`, `\u{H+}`, `\n`,
  `\t`, `\\`; anything else literal UTF-8. A subject may be absent for an
  `error` record.
- `flags`: the dialect's alphabet, parsed by `grx_options_parse()`; the
  test of that function is that every vector's flags parse.
- `options:` names an option directly - `caseless`, `multiline`, `dotall`,
  `extended`, `ungreedy`, `anchored`, `utf`, `ucp`, `no-capture`, `literal` -
  and combines with `flags:` in whichever order the two are written. It
  exists because POSIX and GNU have no flag alphabet at all: their options
  are arguments to `regcomp`, `REG_ICASE` and `REG_NEWLINE`, not letters a
  pattern author writes, so a vector for those dialects has no letter to put
  in `flags:`. A name the reader does not know is a failure rather than a
  silently dropped line - a vector that quietly lost its option is a vector
  asserting the wrong thing and passing.
- `expect`: `<start>-<end>` per group, `-` for a group that did not
  participate; `nomatch`; `error <syntax|unsupported|limit|invalid>`;
  `limit` for a search that must hit a limit; or `compiles`, which asserts
  that the pattern compiles and nothing else. Either end of a span may be
  `$`, meaning the subject's length.

  `compiles` exists because a corpus can say more about syntax than about
  matching. pcre2test answers "does this compile" directly and
  unambiguously - which is exactly what [plan.md](plan.md)'s WP-18 is
  measured on - while its match answers need an oracle driver nothing has
  written yet. Without it such a corpus could contribute only its
  *rejections*, and a syntax corpus of rejections alone cannot catch a
  parser that refuses too much, which is the likelier failure for a front
  end being written from a specification.
- `repeat: <n>`: the subject is `subject:` repeated `n` times. A megabyte of
  `a` written out is a record nobody reads, and the whole design requirement
  here is that a record can be pasted into a bug report - so
  `subject: a` with `repeat: 1048576` says it instead, and `expect: 0-$`
  avoids an end offset a person would have to compute and keep in step.
  Bounded at 16,777,216 repetitions and 64 MB of subject, because a typo
  here is an out-of-memory rather than a failed assertion, and a suite that
  dies has told nobody anything.
- `limits:` sets any field of `GRX_Limits` as `name=value`, `0` meaning no
  limit. All thirteen fields, since it accepted only the seven somebody
  happened to need first until that was looked at - which meant most of what
  `tests/data/vectors/ecmascript/limits.rxt` now says could not have been
  written as a vector at all.
- `engines:` restricts which engines are asked - `pike`, `backtrack`,
  `bitstate` - and defaults to every eligible one; `skip: <reason>` records a
  known deviation without deleting the evidence.
- `unicode:` at file level: the oracle's Unicode version, for the skip rule
  in [unicode.md](unicode.md) §1.

The reader is a few hundred lines of C++ in `tests/conformance/rxt.cpp`
and has its own tests; the format is deliberately too simple to need the
`text` library.

## 4. The conformance runner

`tests/conformance/test_vectors.cpp` is one gtest executable that
discovers every `.rxt` under the data directory and, per record:

1. Compiles under the record's dialect and flags. An `error` expectation
   is checked here, with the result code and, where the oracle supplies
   one, the diagnostic.
2. Determines the eligible engines from `grx_regex_facts()` and the
   record's `engines:`.
3. Runs `grx_regex_search()` on each, with the record's limits.
4. Checks the spans against `expect`, and checks every engine's spans
   against every other's.
5. Reports, at the end, the counts: passed, failed, skipped by reason,
   known gaps, per dialect and per source file. The per-dialect pass rate is
   what `README.md` publishes, and it has the known gaps in its denominator.

A dialect with **no front end at all** is a skip rather than a failure, and
the runner tells the two apart by compiling the pattern `a` in that dialect
once: a dialect that refuses *that* has not been built yet, which is a
different thing from a dialect that refuses one construct. This is what let
the PCRE2 and Perl corpora be imported before the front ends that read them,
so that WP-18's and WP-21's first run had something to be measured against on
the day it existed rather than months later.

### Known gaps

A dialect arrives one construct at a time, and between a front end's first
commit and its last there are patterns the reference compiles and this
library does not. `tests/data/vectors/known-gaps.txt` is where those live:
one line per record - dialect, flags, pattern, subject, **category** and
**why** - and the runner reads it.

Two ways of holding this were rejected. Letting the suite be red makes a gate
nobody reads, and one nobody reads is one that stops catching the regression
it exists for. Publishing a percentage with no list behind it makes a number
nobody can check. The file is the third way, and it is a gate in **both**
directions:

- a record that fails and is not listed fails the suite, as before;
- a record that *is* listed and passes fails the suite too, with "remove the
  entry" - so an entry cannot outlive the gap it names.

- an entry whose category is not one of the two below, or which carries no
  reason at all, fails the suite rather than being guessed at.

The list can therefore only shrink by somebody noticing, and never grows by
accident. The key includes the subject, because one pattern appears with
several and an entry naming only the pattern excuses the subjects that were
already right: `^(a\1?){4}$` answers three of its four and the fourth is the
gap.

#### The two categories, and why one of them is dangerous

A **gap** is this library answering differently from a reference that is
right. It stays in the denominator and counts as a failure.

A **reference defect** is the reference being wrong. It leaves the
denominator, because a wrong expectation is not a question this library can
be scored against - agreeing with it would be the defect.

That second category is a lever that raises the published rate, which is
exactly the move that should not be available on an implementer's own say-so.
Three things hold it down. The reason must **demonstrate** the defect - a
reproduction another person can run, or an upstream issue with its fix commit
- and never merely argue for it; the eight entries there today carry perl
one-liners, the spans three implementations return, and in two cases the
upstream PR and the commit that fixed it. The excluded count is printed
beside the rate everywhere the rate appears, so a reader who sees 100% sees
on the same line what it is 100% of. And an unreadable category is a hard
failure rather than a default: defaulting to `gap` would silently demote a
row, and defaulting to `reference-defect` would silently raise the score.

All four of those checks were proven by planting a violation and watching the
suite go red - a mistyped category, an entry with its reason removed, a
reference defect named for a vector that passes, and an entry naming a record
the corpus does not hold.

The asymmetry this fixed had been there a while: `tools/oracle/perl_diff.py`
had a reference-defect concept from the day it was written, excluding one
defect in each of perl and pcre2 by name, while the vector corpus recorded an
oracle's defect and a library's gap identically and counted both against this
library. Two gates asking one question, and only one of them able to express
the answer.

A reason beginning "not yet classified" is one nobody has looked at, and is
meant to read as the admission it is. There are none of them today: the
fifteen that were there have each been read, and six of them turned out to
be defects rather than missing constructs - among them a multiline `^` that
matched after a newline ending the subject, which is ECMAScript's rule and
neither reference's.

A reason is a claim, and a claim can be wrong. Twenty-one entries blamed "an
optimisation this library does not have" and nine blamed a lookbehind's
length; asked directly, the first twenty-one were three different things and
seven of the nine were a bound this library could have computed and did not.
Both were rewritten from what was measured. An entry whose reason has never
been checked against the reference is an entry that has not been read.

The file is a gate in a third direction too: an entry naming a record the
corpus no longer has fails the suite. A corpus shrinks - the pcre2test
importer learned that `hex`, `expand` and `tables` are not modifiers it can
drop, and the thirty-nine cases carrying them left - and nothing in the run
would otherwise notice the entries they left behind, because the loop only
ever sees records.

A failure prints the record verbatim, the engine, the expected and actual
spans, and both dump outputs.

## 5. The semantic probe suite

The cases that discriminate the profile values in [dialects.md](dialects.md)
§5, run through every oracle by `tools/oracle/probe.py`, whose output is a
table of dialect × case and a report that names the profile value each
answer implies. This is how the `probe` cells are filled and how they stay
filled when an oracle is upgraded. The initial list; each maps to a §5
subsection:

| Case | Pattern | Subject | Discriminates |
| --- | --- | --- | --- |
| 5.1 | `(a\|ab)(c\|bcd)(d*)` | `abcd` | leftmost-first vs longest |
| 5.1 | `x*` vs `x*?` as the first quantifier of `(x*?)(x*)` | `xxx` | `TCL_ARE` |
| 5.2 | `.` | `\r`, `U+2028`, `U+0085` | newline set for `.` |
| 5.3 | `a$` | `a\n` | `$` before final newline |
| 5.3 | `^b` | `a\nb` | multiline by default |
| 5.4 | `(?<=a\|bc)d`, `(?<=a+)b` | `bcd`, `aab` | lookbehind constraint |
| 5.5 | `(a*)*` | `b` | empty-iteration rule |
| 5.5 | `((a)\|b)+` | `ab` | capture reset |
| 5.5 | `(a*)+` | `b` | min forces one iteration |
| 5.6 | `\1(a)` | `a` | forward reference |
| 5.6 | `(a)\|b\1` | `b` | unset backreference |
| 5.7 | `\1` with no groups; `(a)\10`; `\0`; `\012`; `\8` | `a`, `a\n` | backref vs octal |
| 5.8 | `[a-z]` with `i` | `U+017F (long s)`, `U+212A (Kelvin sign)`, `U+00DF (sharp s)` | folding rule |
| 5.8 | `ss` with `i` | `U+00DF (sharp s)` | full folding |
| 5.9 | `\w`, `\d`, `\s`, `\b` | `U+00E9`, `U+0661 (Arabic-Indic one)`, `U+00A0 (no-break space)`, `U+FEFF (BOM)`, `U+2028` | class definitions |
| 5.9 | `\p{lowercase_letter}`, `\p{ LU }`, `\p{IsGreek}` | `a` | property-name matching |
| 5.10 | `a*` find-all | `baab` | iteration rule |
| 5.11 | replace `(a)(b)?` with `[$2][\2][${2}][$<x>]` | `a` | template grammar and unset groups |
| 5.12 | `[]a]`, `[^]`, `[\d-z]`, `[[:alpha:]]`, `[a-z&&[^m]]`, `[a--b]` | `]`, `[`, `-`, `m` | class syntax |
| 5.13 | `a{,3}`, `a{`, `a**`, `(?=a)*`, `a*+` | `aaa`, `a{` | quantifier syntax |
| 5.14 | `(?<n>a)\k<n>`, `(?P<n>a)(?P=n)`, `(?'n'a)`, `\g{-1}` | `aa` | spellings |
| 5.15 | flags `s`, `m`, `x`, `U`, `n` | `a\nb` | flag meanings |

Any later disagreement between an oracle and its profile row is added here
as a case, so that the suite is the record of every semantic question the
project has had to ask.

§5.16's cells have a probe of their own, `tools/oracle/split_probe.py`,
writing `tests/data/probe/split.md`. Splitting could not join the table
above: its answer is a list of pieces rather than a span, it takes a `limit`
whose meaning is one of the things being measured, and only three of this
machine's references have a split at all. It has a column for this library
as well as for the references, because splitting is now a per-dialect axis
and a row where a dialect differs from *its own* reference is a defect rather
than an entry in the table.

Its twenty-four cases are chosen so that no two rules ride on one example.
That is not a stylistic preference: `split /b*/, "abb"` differs between perl
and ECMAScript, and the difference is perl dropping a trailing empty field
rather than anything about the empty-match rule the case looks like it is
about. Asking perl the same split with a negative limit makes the two agree.
Section 5.5's capture-reset cell was wrong for exactly that reason before
WP-03 probed it.

**And twenty-four careful cases were still not enough.** When Perl's split
became its own profile row, all twenty-four agreed with perl immediately -
and `tools/oracle/split_diff.py --dialect perl` then found two rules they had
missed, in 2,000 generated rows. Both hide for the same reason: they are
visible only when the other rules are held out of the way. A zero-width match
at the end of the subject is a separator in perl, which shows only under a
positive limit because perl's trailing-empty drop otherwise removes the field
again; and that drop removes *elements* rather than fields, which shows only
when the trailing capture is empty or unset rather than a comma. A probe
picks cases a person can think of one at a time, which is what makes it a
good place to *start* an axis and a bad place to finish one.

### The vector corpus, and proving it can fail

`tests/data/vectors/` holds checked-in `.rxt` records, and `testVectors` runs
every one of them in `make test` - on a machine with no Node, no network and
no Python. The differential harnesses find defects; the vectors keep them
found.

Every expectation in a generated file is the **oracle's**. A vector generated
from what this library currently does would record the bug rather than the
rule, and would then pass forever. `tools/oracle/make_vectors.py` asks Node
and writes down the answer; if this library disagrees, the vector fails, which
is the correct outcome whichever side is wrong.

The corpus is in two files, for two reasons. `named.rxt` holds the cases worth
writing down by name - the two loop rules, the two foldings, the `$` rule,
the shorthands, the lookbehinds - each of which a random corpus would reach
only by accident. `generated.rxt` holds the random ones, which reach
combinations nobody would think to write.

`tests/data/vectors_selftest/` is how the runner is kept honest. It holds a
record whose expectation is deliberately wrong, and a test that *expects the
runner to fail it*. Without that, "the conformance suite passes" would be
indistinguishable from "the conformance suite ran nothing" - which is the
failure mode a corpus discovered by directory walk is most prone to, and the
one section 9 of this page is about. The runner separately refuses to pass
when it found no vectors at all.

### The property check

`make check-oracle-properties` asks this library and the reference which code
points match each `\p{...}` - all 1,114,112 of them, for all 457 properties -
and compares the range arrays.

This is what plan.md WP-09 calls "the real check on WP-02", and it is
stronger than importing test262's generated `property-escapes/` files. Those
files are themselves generated from the UCD, so they check that a table
agrees with the UCD; this checks that it agrees with the UCD *as a shipping
engine reads it*, which is the question a conformance rate is about. It also
covers every property rather than the subset test262 happens to have
generated, and it needs nothing cloned.

Surrogates are excluded on both sides: `String.fromCodePoint` of a lone
surrogate matches nothing in JavaScript and UTF-8 cannot hold one, so neither
side is asked. Properties the reference cannot spell - the binary properties
outside ECMA-262's table 69 - are counted and skipped, because which
spellings each side accepts is the syntax check's question.

### The two-readings check

`make check-unicode-agreement` is the gate that exists because the Unicode
data **left**. Every `\p{...}` set is ghoti.io-unicode's now, so the obvious
gate - ask this library for a property's code points, ask the Unicode library
for the same property's code points, compare - would be asking one source
twice and printing "601 identical" whatever was wrong. That is the
self-consistency shape: a sweep can look thorough and be reading its own
output back.

What it compares instead is the generator's *reading* of
`third_party/ucd/17.0.0` against the library's answer, through the two
figures each property record still carries: `total`, the code-point count,
and `digest`, an FNV-1a 64 over the ranges the generator built. Two readings
of one release, which is only possible because both libraries pin it - and
the digest rather than the count alone, because two different sets of the
same size pass a count.

It also asks all 601 records rather than the 457 the Node check reaches. The
144 Numeric_Value properties were never compared as *sets* by anything before
this; they had no set on either side to compare, and now one of them is
derived here from a domain the Unicode library encloses.

Third, it is what makes an unresolvable record loud. A property whose name
the Unicode library does not know yields no code points rather than an error,
and `\p{Whatever}` would quietly match nothing - so a record that resolves to
nothing is reported as a failure and never counted as a comparison.
`Property.EveryTableIsSortedDisjointAndCounted` asks the same question inside
`make test`, so a fresh clone with no containers is not the case that misses
it. Both were armed: renaming one record to a name ghoti.io-unicode does not
have fails the suite, the conformance vectors and this gate together.

### The cell width check

`make check-vim-widths` regenerates `src/unicode/display.c`'s table from
the pinned vim and diffs it - 1,112,062 code points, 0 disagreements against
vim 9.2, and 0 against 9.1 before the raise: the widths did not move at all.

Every other table here is generated from the UCD and gated by
`make check-unicode-tables`. This one cannot be, and the reason is the point:
**it is not Unicode data.** It is vim's, it disagrees with East_Asian_Width
on hundreds of code points, and regenerating it from the UCD is precisely the
defect to guard against - `\%23v` has to report the column vim would, so
Tangut drawn in one cell and emoji drawn in two are the table being right.
Until this gate existed the file header's provenance sentence was the only
evidence, the harness that measured it was not in the repository, and nothing
recorded which vim it came from.

The probe is the character's **contribution after a base**,
`strdisplaywidth("a" . c) - 1`, and not the width of a lone one.
`strdisplaywidth()` of an isolated combining character is vim's escape
rendering - `<180b>` is six columns - which is a question about drawing an
unprintable rather than about cells. Nine code points differ that way and
none is a disagreement. The delta is what `grx_display_cell_width(c, 0)`
answers, and for everything that is not a combining character the two
readings are identical.

Two code points are excluded, each a different question rather than a
disagreement, and the differ asserts they still *are* different so that a
third cannot be absorbed silently: U+0000, because `nr2char(0, 1)` is a
zero-length string and vim cannot hold NUL in one; and U+0009, because
`strdisplaywidth()` applies the tab stop while this library splits that
between `grx_display_cell_width()` and `grx_display_column_after()`.

`ambiwidth` is pinned to `single` in the dumper. It is a user setting, so a
vim with `set ambiwidth=double` answers differently for every East Asian
Ambiguous code point - a dialect this library does not offer and must not
acquire from whoever happens to run the gate. That is the same class of pin
as the `encoding` and `iskeyword` ones in `tools/oracle/vim_diff.py`.

### The vim character class check

`make check-vim-classes` does for `src/unicode/vim_class.c` what the check
above does for the widths: regenerates vim's class table from the pinned
vim and diffs it. 1,112,063 code points, 0 disagreements against vim 9.2;
U+0000 and the surrogates are unaskable and both dumpers write them as the
same sentinel, so a mismatch in *which* are unaskable is itself a
disagreement.

It is also the gate that made the 9.2 raise a piece of work rather than a
pin bump. Against 9.1.1244 it reported **48 disagreements**, all in
Superscripts and Subscripts: 9.1 reads U+2070..U+209F as punctuation, and 9.2
gives U+2070..U+207F and U+2080..U+2094 classes of their own while making
U+2095..U+209F ordinary keyword characters. `src/unicode/vim_class.c` moved
with the pin, in the same commit, because the gate would have been red in
between.

This is the table with the most to lose from an unpinned option, and it is
the one that proves the family matters. `charclass()` consults the buffer's
chartab for a code point below 256, so **'iskeyword' decides the answer
there** - and `vim -u NONE` leaves vim Vi-compatible, where 'iskeyword'
defaults to `@,48-57,_` rather than the `@,48-57,_,192-255` vim's own help
calls the Vim default. U+00D7 and U+00F7 are the only members of 192-255
that vim's `@` does not cover, so they are exactly the two the mode decides,
and they sat in this table as punctuation until 2026-09-24 because the
one-off sweep that built it ran without the pin.

That is not a hypothetical control: reverting those two entries makes this
gate report `U+000D7 vim class 2, ours 1` and exit 1. It would have caught
the defect on the day the table was written.

### The vim option-set check

`make check-vim-sets` regenerates the four sets vim decides from *options*
rather than from Unicode - `\i` is 'isident', `\k` is 'iskeyword', `\f` is
'isfname', `\p` is 'isprint' - and diffs each against `src/syntax/vim.c`.
1,112,063 code points per set, four sets, 0 disagreements against vim 9.2.

**Three of the four had no gate at all until 2026-09-25**, and the fourth had
one only sideways: a unit test requires `\k` to agree with
`src/unicode/vim_class.c`, which catches a `\k` that disagrees with the class
table and not a pair of them wrong together. The tables were built by a
one-off sweep, and `src/syntax/vim.c`'s header is a record of what that cost:
three of the four were wrong when first written, `\i` and `\k` both missed
U+00B5, `\k` took in 5,463 code points vim excludes, and the correction then
overshot by two.

A sweep that is not a tool cannot be re-run when the reference moves, and the
reference moves. The vim 9.2 raise put all 48 of U+2070..U+209F into
'iskeyword' - the 37 that got classes of their own as much as the 11 that
became plain keyword characters, since `\k` is every class from the keyword
class up. This gate is what said *which* 48 and that `\i`, `\f` and `\p`
were untouched; the sideways unit test said only that something was wrong.

The prints carry each set's size as well as its disagreement count, because a
set that collapsed to nothing would report zero disagreements if the
reference collapsed with it, and a bare 0 cannot be told from that.

### The case-fold orbit check

`make check-oracle-folds` compares the fold table as a **partition**: not
which code points fold, but which ones fold *together*. That is the half the
property check above cannot reach. `Changes_When_Casefolded` is one of its
457 properties and agrees exactly, and a table that folded `A` to `b` would
still pass it - the set of code points that change under folding would be
identical.

Perl is the oracle because `fc()` gives the answer without a pattern: two
code points are case-equivalent exactly when their full fold keys are equal,
so grouping by `fc()` is the partition itself, with nothing a matching bug
could bend. This library's side is `tools/oracle/grx_folds`, which walks
`grx_unicode_fold_orbit()`.

Two restrictions, and both are the difference between measuring the tables
and measuring something else:

* **Depth.** `fc()` is the *full* casefold and the orbit table is the
  *simple* one. A code point whose only fold is multi-code-point - U+00DF to
  "ss", U+0149 to U+02BC U+006E - is a singleton here and a group member
  there, which is two questions rather than a disagreement. The 104 of them
  are found by asking perl which keys are multi-code-point, never by a list
  in the differ: a list would go stale at the next UCD without saying so.
* **Version.** This library is UCD 17.0.0 and perl 5.40.1 is older. The
  restriction is on the whole **orbit**, not on the code point, and the
  difference is not academic - U+019B is assigned in perl and its partner
  U+A7DC is not, so perl calls U+019B uncased and restricting per code point
  reports four disagreements that are all UCD 17.0.0 additions. A relation
  is comparable only when the reference has heard of both ends of it.

2,822 code points compared, 1,396 orbits, 104 and 110 skipped for those two
reasons, 0 disagreements. Armed before it was believed: a driver that splits
one orbit, one that merges two, and one that names a wrong partner are each
reported and each exit 1.

### The numeric property check

`make check-oracle-numeric-properties` does the same job for `\p{nv=...}`,
which the check above cannot touch: Node has no such property, so all 144 of
its values would be "the reference cannot spell this" and skipped. Perl is
the only engine that implements it, so perl is the oracle.

Two things make it a different shape of check. It compares by **set
membership** rather than by value, because perl will report a code point's
numeric value only as a decimal and `1/3` comes back as `0.33333333` - two
values differing past that width would compare equal. And it does not demand
equality, because the pinned perl carries UCD 15.0.0 against these tables'
17.0.0 (`tools/corpus/VERSIONS`). The invariant is the one that skew cannot
break: *every code point perl gives a numeric value must get the same value
here.* A code point assigned here and `NaN` there is skew in the documented
direction, counted and reported; a code point both assign and disagree about,
or one perl assigns and no table here claims, fails.

It currently compares 142 values with no disagreement, and reports the two
values and 111 code points that perl's older UCD does not carry.

### The POSIX differential

`make check-oracle-posix` does for the POSIX and GNU front ends what the
match check does for ECMAScript: builds patterns by combination - one entry
per construct the dialect has, one to three of them per pattern - and asks
the references and this library where each matches. About 17,000 cases a
run, across all four grammars.

Which reference decides depends on the dialect, and the difference is the
point. `gnu-bre` and `gnu-ere` *are* glibc, so glibc alone decides them.
`posix-bre` and `posix-ere` are nobody's: glibc's `regcomp` is GNU, and
musl's basic RE takes the same GNU operators while refusing the `[[.x.]]`
POSIX requires. For those two the standard is the *agreement* of glibc and
musl, and a case they answer differently is counted as unsettled and judged
by neither. The atom sets are chosen so a construct these dialects do not
have never arises.

It exists because the imported vectors are 429 cases somebody chose, and the
pairs nobody thought to write down are where a front end goes wrong. It
earned its place immediately: it found the one place `^` disagrees, which no
imported vector reaches. Without `REG_NEWLINE` glibc answers `^b` against
"a\nb" with nomatch - so `^` is not a line anchor for a search - and `.*^b`
against the same subject with a match at 0-3. Those two cannot both be the
rule, and [dialects.md](dialects.md) §6 records which one is implemented
here. That family is counted and excluded by name; everything else fails the
gate. Four seeds over about 44,000 cases found nothing else.

It then earned its place a second time, and the way it did is worth keeping.
Adding the second reference meant looking at how answers are compared, and
`normalise_ours()` turned out not to map `compile <diag>` to the drivers'
bare `compile`. That should have produced a flood of disagreements and
produced none - because every atom was a well-formed construct, so no
generated pattern was ever rejected by anybody, and the whole accept-or-
reject half of the check had never once been exercised. A gate reporting zero
on a question it never asked is worse than no gate. Ill-formed atoms now sit
beside the well-formed ones, and together with the mapping they found the
anchor-quantifier rules in `src/syntax/posix.c`, written for `^` and `$` and
never extended to `\<`, `\>`, `\b` and `\B`.

A third lesson, and the same one a third time: every alternation in the atom
lists had branches of the same length, so `a|ab` could not be built and
leftmost-longest could not show up. Four dialects were answering the wrong
extent for basic alternation and 17,000 generated cases a run said nothing,
because the generator could not spell the question. The atom lists now carry
`a|ab`, `ab|a` and their kin, and the subjects carry the strings that tell
them apart.

Flags are not varied, because POSIX's options are arguments to `regcomp` and
the driver spells options as a dialect's flag letters, of which these
dialects have none. `REG_ICASE` and `REG_NEWLINE` are covered by the
imported vectors, which carry `options:` instead.

### The submatch differential

`make check-oracle-submatch` asks the question underneath the POSIX
differential. That one asks whether the same *text* matched; this asks which
group got which part of it, in the cases where more than one answer fits the
same extent.

The difference is the generator. The POSIX differential builds patterns for
construct coverage - one entry per construct the dialect has - so a case
where two group assignments share one extent arises only by accident. This
one builds patterns out of ambiguous pieces on purpose: groups whose
branches overlap (`(a|ab)`, `(a|aa)`), groups that can match empty (`(|a)`,
`(a?)`, `(a{0}|a)`), quantified groups side by side, and groups with
*untagged* material in front of them (`a*(a|)`, `[ab]a*(a|)`). Almost every
case has two answers to choose between.

That was the whole of its value on the first run. Spencer's imported vectors
contain no such case at all - not one of the 809 POSIX and GNU rows - and
the POSIX differential had been reporting zero disagreements for as long as
it had existed. Built to ask, this reported **1,050** across three dialects
immediately, every one of them one rule, and that rule is now
[dialects.md](dialects.md) §5.1's `FIRST_PATH` rewrite.

It then earned its place a second time as the gate for WP-26, and the way it
did is the reason the atom lists look the way they do. Three wrong versions
of POSIX's comparison were written, and **the first tool to catch each one
was a different tool**:

- comparing group *starts* as well as ends - caught here, 1,098 rows;
- letting a group only one candidate entered decide - caught by the
  conformance corpus (`a(b+|((c)*))+d`), and **not** by this, whose atoms
  had no empty-iteration shape in them at all;
- shortening a subexpression with no group around it - caught by
  `posix_diff.py` (`[ab]a*(a|)`), the wrong tool having to find it.

Both of those gaps are closed: `(b+|((c)*))+` and the untagged-prefix pieces
are atoms now, and re-planting each mistake makes this report 264 and 880
rows respectively. A gate that was clean on two of the three questions it
exists to ask was clean because it could not spell them.

Who decides is the POSIX differential's arrangement, with two additions.
glibc alone decides `gnu-bre` and `gnu-ere`; for `posix-bre` and
`posix-ere` the standard is glibc and musl agreeing.

The first addition is that their *disagreements* are counted and named
rather than skipped, with the side this library came down on, because that
set is the measurable part of what §5.1's two values differ about: 470 of
15,246 generated `posix-ere` rows. `--strict`, which is what the Makefile
runs, fails if it ever empties.

The second addition is the harder one. Since WP-26 the POSIX rows follow the
*standard*, so a row where both references agree and both are wrong is a
row this library must fail the consensus on. There are six, all Fowler's
`(a|ab)(c|bcd)(d*)` shape, and they are listed by pattern **with the answer
each must give** - an exemption that still checks the result, so a defect
inside the class fails like any other. The list is guarded in both
directions: if a listed row stops being generated, the tool says the list or
the atoms have gone stale and fails. That guard fired on its first run, when
"abcd" turned out not to be in the subject list.

`posix-bre` is the thin row and the tool says so rather than leaving it to
look like a clean result: POSIX basic REs have no alternation, so the
empty-branch half of this question cannot be spelled in one. Its cases ask
the other half, two quantified groups next to each other.

### The Vim differential

`tools/oracle/vim_diff.py`, WP-36's gate. vim cannot be imported, so the
saving is one fork for a *run* rather than one per case: vim reads a file of
cases and writes a file of answers, which is what makes tens of thousands of
rows possible against a reference `probe.py` starts a process per case for
(22 cases, so it can afford to). `make check-oracle-vim` runs it with `--strict`. Current standing:
**1,080,000 rows over thirty seeds, no disagreements** - with 801 rows
excluded as vim artifacts, 1,407 where its two engines disagree and `set
re=1` gives this library's answer, and 21 where they disagree and neither
does, which are printed.

**The subject is a string, not a buffer**, and that is a decision the tool
makes rather than a detail of it. vim's help describes matching against a
buffer, where `.` refuses the line break; `matchstrpos()` over a string is
the question this library can answer, and there the break is an ordinary
character. Driving a buffer oracle instead would measure a dialect this
library does not offer. The profile row follows the measurement, which is
why it reads GRX_NEWLINES_NONE where vim's own documentation reads the
opposite - see [dialects.md](dialects.md) §6.

**What it cannot see, and what is done about it.** `matchlist()` returns the
*text* of each group and returns "" for a group that did not participate, so
an unset group and one that matched empty are one answer there. The
comparison folds ours the same way, and the unset axis is stated by
`tests/unit/test_vim.cpp` instead, from the one probe that separates them:
`\(a\)\?\1` matches the empty string against "b", which a dialect that
failed on an unset reference could not do.

**Nine classes of disagreement are vim disagreeing with something**, and the
tool counts each rather than dropping it, so that the number moving is
visible:

- **vim's two engines.** `\%^\|a\?` against "a" is the empty match at 0
  under `set re=1` and "a" under the default `re=2`. This is not recognised
  from the shape of the pattern - it is *asked*: every row that came back
  different is put to the old engine. A row where both of vim's engines
  agree with each other and not with this library is a defect here.

  The count is split in two, because the rows are not equally informative.
  Most turn on a single axis and `set re=1` gives exactly this library's
  answer. A few turn on *two*, and then neither engine gives it: this
  library follows the old engine where a mark sits inside an assertion and
  the new one where `\c` meets `[[:lower:]]` or an abandoned `\@>` group's
  captures are read, each time because that engine is the one whose answer
  can be stated as a rule, so a pattern touching two of them agrees with
  neither. 29 rows in 1,044,885 - `\c[[:lower:]]\|\(a\zsb\)\@=`
  against "ABC" is the shape - and the tool *prints* each of them with both
  engines' answers beside its own, because a defect of this library's could
  hide in that set and a person looking at it is the only defence there is.
- **A forward backreference with a lookbehind after it.** `\1\(a\)\@<!`
  is accepted where `\1\(a\)`, `\(a\1\)`, `\1\(a\)\@=`,
  `\1\(a\)\@>` and `\1\%(a\)` are all "E65: Illegal back reference" in
  both engines. A construct that is legal only when a *later* part of the
  pattern takes a particular shape is an artifact of how vim compiles a
  lookbehind, not a rule a second implementation could follow.
- **A capture that outlived its branch**, and **a capture that vanished**:
  vim keeps what an abandoned `\@>` group wrote and loses one written
  before a `\@=` that a repeat follows. pcre2test disagrees with vim in both
  directions. The two are not treated alike - vim *losing* a capture is
  excluded for any `\@` operator, because a defect of this library's would
  be the same loss and would still be reported; vim *gaining* one is
  excluded only where the atomic operator is written.
- **A postfix lookbehind with a backreference after it.** Two measured
  instances, pcre2test agreeing with this library in both:
  `\(a\)\@<=\(a\)\1\l` against "aaab" is "aa" at 1:3 in both of vim's
  engines - text with nothing in it for the trailing `\l` to have matched -
  where `(?<=(a))(a)\1[a-z]` is 1:4 in pcre2test; and
  `\v\D(a)@<=\m\(a\)\1\(a\+\)\@>` is 0:3 in both engines with a
  third group the match has no room for, where the pcre2 equivalent is no
  match at all. Drop the atomic group from the second and vim's two engines
  stop agreeing with *each other* - 0:2 under `re=1`, 0:3 under `re=2`.
  This one is excluded whatever the disagreement looks like, which is a real
  narrowing: a regression of this library's inside that shape would not be
  reported. It is written out in the tool for that reason.
- **One spelling out of four that means nothing.** `\v\_^*` matches
  nothing at all in vim, while `\m\_^*`, `\v\_$*`, `\v\_^{0,1}` and
  `\v(\_^)*` all match the empty string there. Only the very magic level,
  only `\_^`, only the `*` spelling, and both engines alike.
- **A bare `*` with nothing to repeat.** It is the literal asterisk at
  every level, which is a basic RE's rule and vim's - `*a`, `\m*`, `^*`,
  `\(*\)`, `x\|*`, `\&*`, `\v%(*)` and `\M\%(*\)` all match one -
  and two spellings out of that set are refused instead. `^\m*` is an
  error where `^*` matches and `\(\m*\)` matches, so a marker between
  the caret and the star loses the caret; and `\%(*\)` is an error where
  the same construct spelled three other ways is not. Both engines alike.
- **`\%23l*`.** `\%23l` names a buffer line and never matches over a
  string, so a `*` on it should leave the empty match a zero-iteration
  repeat always has - and vim agrees five ways: `\%23l\{}`,
  `\%23l\{-}`, `\%23l\{0,1}`, `\(\%23l\)*` and the very magic
  `%23l*` all match the empty string there. Only the bare `*`, only
  outside very magic, only after an `l` form, and both engines alike.
  Three answers come out of it and the tool looks at its own as well as
  vim's: vim finds nothing where this matches empty, vim finds a longer
  match because another branch won, and vim *compiles* `\%23l\v*` where
  this refuses it - a marker between an atom and a bare `*` being "E871"
  in vim after every atom but an `l` form.
- **A group inside `\%[...]`.** `a\%[\(bc\)]`, `a\%[\%(bc\)]` and a
  nested `a\%[b\%[cd]]` are "E54: Unmatched \(" under `set re=1` and
  compile under `re=2`. Every other member vim's help calls an atom is
  built - a class, a collection, `\%d98`, `\zs`, `\<`, `\_s`, a
  backreference - so the refusal is this one shape and not the member
  grammar.
- **A `\zs` or a `\ze` inside an assertion**, where the default engine's
  answers cannot be stated as a rule. 140 patterns put each of four marker
  bodies inside each of `\@=`, `\@!`, `\@<=`, `\@<!`, `\@>` and `\&`
  with five tails: `set re=1` answers every one of them the way a single
  mark register does, and `re=2` agrees with it on 110. The 30 left are not
  a second rule. `\(ab\zec\)\@=a` against "abc" is 0-2 old and 0-1 new,
  while `x\(ab\zec\)\@=` against "xabc" is 0-3 in *both* - one character
  consumed outside the same assertion, and the answer turning on which side
  of it the character was written. `a\zeb` is 0-1 everywhere, so it is not
  that later text overwrites a mark. And `re=2` drops a `\zs` where it
  keeps a `\ze`. This library follows the engine that can be written down,
  and every such row reaches the engine-split count above rather than being
  recognised from the pattern's shape.

**Both of the rules its subjects used to leave out are built**, and the
subjects say so in two different ways. Characters outside Latin's word class
are in the ordinary list - six of vim's nine classes, each beside Latin and
beside one another - because `\<` and `\>` follow those classes now rather
than a word set.

**Composing characters are a block of their own**: every atom in the
vocabulary, with and without `\Z`, against ten subjects that hold a base and
its marks - 2,900 rows a run, beside the 20,000 the generator writes. One
atom per pattern, and that is the point rather than a convenience. Vim's
default engine shares one step length between every thread alive at a
position, so a second atom in the pattern can take the marks away from the
first: `a\|` beside a cluster branch finds nothing where that branch alone
finds the cluster, and `b\|` beside it finds it. A rule cannot depend on
what a failed alternative begins with, so rows like that would measure how
often the generator spelled one. Nineteen atoms are named in `COMPOSING_SKIP`
with the reason for each, every one a row already in
[dialects.md](dialects.md) §6.

**The vocabulary carries what vim refuses**, at one row in eight - `\z(`,
`\z1`, `\1` with no group, `a\{2`, `\(a`, `a**`, `\v+a`, `\v@a` and
more. It is spliced *between atoms* rather than at a random byte offset,
which is a narrowing with a reason: a byte offset lands inside a construct
as often as not, and vim compiles a collection with a corrupted
`[[:name:]]` into a pattern that can never match anything at all -
`[[:foo:]]*a` does not match "a". Thousands of those would put a floor under
the disagreement count and hide the next real one under it.

**Two defects it found were not in this dialect.** A lookaround restored the
live capture slots when its body's path was abandoned and left the *shadow*
spans - the ones a backreference reads - where the body had written them, so
`(?=(a))$|(a)\1` matched "aa" here, "a" in node, and nothing in pcre2test;
the live captures were right throughout, so the only visible symptom was the
width of the match and no conformance vector had caught it. And the Vim
front end's own `\c` scan repeated a defect the shared prescan had carried
until WP-30 - a `]` after an escape inside a collection was read as a
member, so the collection never ended and a later `\c` was read as being
inside one. The same mistake, written a second time in a second scan, four
hours apart.

**A trap for anyone editing its vocabulary:** Python's raw strings still
decode `\u`, so `r"\u0041"` is three characters and not six. Every atom in
the generator is an ordinary string with the backslashes doubled for that
reason.

### The Python differential

`tools/oracle/python_diff.py`, WP-30's gate, and the only one here whose
reference is not a subprocess. CPython's `re` is importable by the tool that
generates the cases, so a row costs a function call: **600,000 rows in under
four seconds**, against the tens of thousands a fork-per-batch oracle manages
in the same time. `make check-oracle-python` runs it with `--strict`.

That difference is the finding, not a footnote about speed. The front end was
built, the first run of 1,200 rows drove it from 85% disagreement down to
three, and then **every remaining defect came from turning the dial up** -
600,000 rows and seven seeds, 4.2 million rows in all. None of them came from
reading the `re` documentation more carefully.

The tool reports what the *reference* answered beside the disagreement count:

```
python_diff: 600000 rows (146460 compile, 180716 match, 272824 nomatch), 0 disagreements
```

A run whose `match` share collapses has stopped asking the question even
though its disagreement count is still zero. That line is there because
"0 disagreements" over rows the oracle refused outright is a gate agreeing
about nothing - the shape `posix_diff.py` and `sed_diff.py` were each blind
to in turn.

**Its vocabulary carries what Python refuses**, not only what it accepts. A
front end's refusals are code too: `\p{L}`, `\G`, `(?R)`, `(*FAIL)`,
`(?<n>a)`, `a*(?#c)?` and two dozen more are generated at one row in eight,
and a row where this library accepts one is a pattern it calls valid Python
that `re` rejects. That half found the `(*...)` family and the
global-flag-placement rule, both of which the accepting vocabulary had no way
to reach.

**And the vocabulary was narrowed to match a belief, once.** Its lookbehind
atoms were all fixed-width, with a comment saying "lookbehind is fixed-width
here, so every one generated is" - which described the dialect correctly and
made the gate unable to ask whether *this library* enforced it. It did not:
`GRX_LOOKBEHIND_FIXED` had been in the profile since the table was written
and was read by nothing. Four variable-width atoms later, the gate found it
in one run. A generator written from what the reference does is a generator
that cannot find where the implementation diverges.

Two other defects it found were in shared code and had been reachable by the
Perl-family differentials for as long as those had existed: the prescan lost
its group count after any class holding an escape (`[\d](a)\1` was an invalid
backreference in `perl` and `pcre` too), and `GRX_FEATURE_QUOTING` was read
by nothing because both dialects that reached the `\Q` code had it.

The split and template halves have arms of their own -
`split_diff.py --dialect python` and `replace_diff.py --dialect python`, both
also in-process - and both earned their place immediately. Six hand-written
split cases passed while 1,848 generated rows did not; twelve hand-written
template cases passed while the generator found six rules they had missed,
including that `\u`, `\U`, `\N{...}` and `\x` are *pattern* escapes in Python
and errors in a template. One dialect, two closed alphabets.

### The cross-engine check

`make check-engine-equivalence` runs the same rows through every engine that
can run them and requires the same answer. design.md section 3.5.4 is the
invariant; this is what enforces it, along with the `crossengine` fuzz
harness.

It is the strongest cheap test the library has and it needs no oracle
installed. The Pike VM merges threads in lockstep and the backtracker walks
one path with an explicit undo stack; they share the instruction set and
nothing else, so a disagreement is a defect in one of them and there is
nowhere for a shared mistake to hide.

The bit-state engine is the exception to "nothing else": it *is* the
backtracker, with a visited bitmap over (instruction, position). Comparing
it against the backtracker therefore checks one specific claim rather than
two implementations - that skipping a state already tried changes no answer
- and it checks that claim on 322 of the 1,567 vectors, which is every one
whose program it is allowed to run. Against the Pike VM it is a real
second opinion again.

A limit reached by one engine and not another is not a disagreement. It is
the exponential engine running out of budget, which is what the budget is
for - or the bit-state engine refusing a bitmap that will not fit in
`max_match_memory`, which is what that budget is for.

**What the check could not see until Phase 4 was audited.** The paragraphs
above describe what the *vectors* compare. `tools/oracle/engine_diff.py`,
which generates rows rather than reading them, was doing less than they say:
it ran two engines rather than three, and every row it generated was
ECMAScript, because it borrowed `match_diff`'s generator wholesale.

The dialect is the part that mattered. The one thing that makes two engines
disagree about a match they can both find is the *preference* - leftmost-
first against leftmost-longest - and the two engines implement longest by
entirely different means: the Pike VM keeps the best of its live threads,
the backtracker reports failure from `MATCH` and carries on searching. With
no POSIX row in the corpus, switching the Pike VM's longest mode off
entirely changed nothing the check could see. It now generates from
`posix_diff`'s per-dialect atoms as well, across all five dialects and all
three engines, and the same experiment reports `(a|ab)` on "ab" as 0-1
against 0-2 on the first screen.

The per-engine row counts are printed for the same reason the oracle checks
refuse to skip: "0 disagreements" is also what an engine that answered
nothing would report. Fewer than two engines answering anything is an error
rather than a pass. The bit-state engine answering nothing on the four
longest dialects is expected and visible - it cannot do leftmost-longest and
says so rather than guessing (dialects.md section 5.1).

Also fixed there: `tools/oracle/grx_match.c` had no name for the bit-state
engine and printed `?`, which is what turned the first run of the extended
check into 1,304 spurious disagreements.

**And the dialects whose constructs the engines implement separately.** The
preference was one axis; the other is that *every assertion in this library
is written twice*, once in each engine, and a dialect that brings new ones
brings two implementations of them. Vim's screen column, its byte column,
its two word-class boundaries and its cluster boundary were each added in a
pair, and nothing compared the two halves: the Vim differential asks
`GRX_ENGINE_AUTO`, which is one engine per program. The check generates from
Vim's, Perl's, PCRE2's and Python's vocabularies as well now - nine dialects
- and the Vim rows alone put 3,214 programs through two engines or more.

Seen to fail, on the code the new rows exist for: making the Pike VM's
cluster boundary always hold reports 171 disagreements against the
backtracker and the bit-state engine, and none without it.

### The Perl-family differential

`make check-oracle-perl` runs `tools/oracle/perl_diff.py`, which builds
patterns from an atom vocabulary and puts them through a reference and this
library side by side. Each dialect has one definition, the way glibc is the
definition of `gnu-bre`: perl decides `GRX_SYNTAX_PERL` and pcre2 decides
`GRX_SYNTAX_PCRE`, so there is no agreement to take and the vocabularies are
separate - `(?J)` is PCRE2's and perl refuses it, and the charset modifiers
are in both vocabularies with different grammars: perl's `a`, `d`, `l` and
`u`, against PCRE2's `a` alone with a letter after it (`(?aD)`, `(?aS)`,
`(?aW)`, `(?aP)`, `(?aT)`), which perl refuses.

perl is driven through `tools/corpus/perl_match.pl`. pcre2 is driven through
`tools/oracle/pcre2_match.c`, which links the installed libpcre2-8 through
the pinned release's public header, because `pcre2test` reports matched text
rather than offsets and omits a trailing unset group - see
`tools/corpus/VERSIONS`.

It exists because a corpus is a set of questions somebody already knew to
ask. The POSIX differential is the standing example - the corpus was at 100%
while `a|ab` against "ab" answered 0-1 in four shipped dialects, because
every alternation in it happened to have branches of the same length.

Three defects of this library so far, none of them reachable from the
imported corpora:

- `(?^i:...)` applied the reset and dropped the letter after it, so every
  `(?^<letters>...)` silently meant `(?^:...)`. `(?^` appears in none of
  `testinput1`, `testinput2` or `re_tests`.
- A backreference to a name belonging to several groups resolved to one
  group at lowering time, so `(?(DEFINE)(?<n>a))(?<n>b)\k<n>` could never
  match - the first group of that name never participates. Both references
  take the first group that is *set*, which is the rule
  `grx_match_group_named()` already followed. `GRX_IR_AMBIGUOUS_REF` had
  recorded the situation since it was written and only the analyser read it.
- perl quantifies every control verb, as it quantifies `\K` and the anchors.
  The rule had pcre2's half only, which refuses everything but `(*ACCEPT)*`.

And three defects of the *references*, each excluded by name and counted
in the run so the exclusion cannot go quiet: perl 5.40.1's branch-reset
regression (Perl/perl5#24577), a pcre2 10.46 internal error on a lookbehind
beside an extended class, and perl's search for a pattern that *begins*
with `\b{lb}` missing the end of a one-character subject. The first two are
described in `tools/corpus/VERSIONS`, and the perl exclusions apply to the
perl run only - the same families are still compared against pcre2.

The third came with the four segmentation boundaries, which were in the
syntax differential and in the unit tests and in no match differential at
all: `\b{wb}`, `\b{gcb}`, `\b{sb}` and `\b{lb}` compiled, and nothing
asked perl where they *hold* over a subject. They agree everywhere except
that one shape, where perl contradicts itself: `a\b{lb}` against "a" is 0-1
and `\b{lb}$` against "a" is 1-1, so the boundary is there, while
`\b{lb}` alone against "a" is no match - and 2-2 against "ab", 3-3 against
"abc". UAX #14 breaks at the end of text in all of them.

The vocabulary includes ill-formed atoms for the reason
[posix_diff.py](../tools/oracle/posix_diff.py) does: without them nothing is
ever *refused*, and the accept-versus-reject half of the comparison reports
zero having never been asked. Roughly a third of generated patterns are
rejected by perl, and the two agree on every one.

It also reaches past the constructs a front end is obviously about.
Conditionals, recursion and subroutine calls, branch reset, `\K`, duplicate
names, the control verbs, `\p{}`, `\x{}`, `\o{}`, `\N{U+}` and `\h\v\R\N`
are all in it, each carrying whatever it needs to be a whole question - a
conditional brings its own group rather than referring to one that may not
be there. Every family generates patterns that perl refuses *and* patterns
that match, which is what says the rows are being compared rather than
skipped. A bare `(?R)` is the one deliberate absence: it recurses with
nothing to stop it, so what it asks about is a limit rather than a grammar.

`(?J)` is absent for a different reason - it is PCRE2's spelling for
something perl does not need and does not accept, duplicate names being
ordinary there. It belongs to the pcre vocabulary that does not exist.

Both drivers run under a timeout, because a vocabulary containing recursion
and control verbs can generate a pattern that runs for a very long time, and
that should stop the tool with a message rather than look like a hung
build.

Two things the transport cannot carry, both recorded in the file rather than
left as silence. `\Q...\E` is double-quotish processing that happens when
perl tokenises its *source*, so a pattern arriving in a variable - the only
way a driver can pass one - never goes through it, and `qr/$p/` with `$p`
holding `\Qa.b\E` matches nothing at all; the construct is checked in
`tests/unit/test_perl.cpp` instead. And `x` is left out of the flag sweep
because `grx_match.c` maps flag letters to `GRX_Option` bits and has no
extended-mode bit among them, so a row with `x` would ask perl one question
and this library another.

There is no pcre2 equivalent. `pcre2test` reports matched text rather than
offsets and omits a trailing unset group, and this machine has libpcre2-8
without its header, so the driver shape the other oracles use is not
available. The foot of `perl_diff.py` says so.

### The Perl-family syntax split

`make check-oracle-perl-syntax` runs
[perl_syntax_diff.py](../tools/oracle/perl_syntax_diff.py), which asks perl
and this library the same question about each of 183 constructs: does it
compile at all. It is a **list**, not a generator, and that is the whole of
why it exists.

The differential above generates patterns from an atom vocabulary, and that
vocabulary is this library's own grammar. A construct this library has and
perl has not is therefore never generated as a disagreement - the generator
has no reason to think the question exists. Nothing else asked it either: a
pattern that compiles here and would be refused there matches perfectly
well, so every match-shaped check agrees.

Five construct families were accepted under `GRX_SYNTAX_PERL` and "not
recognized" in perl 5.40.1 until this existed, all of them PCRE2's: the
nineteen leading directives, callouts `(?C...)`, the `\g<1>` and `\g'name'`
subroutine spellings, `(?(VERSION>=n))`, `(?J)` and `(?U)`, and the
non-atomic lookarounds in all four spellings. They arrived the way this
whole class arrives: one front end reads both dialects from one set of
tables - which is the right design, and is why the list of differences is
something a reader can count - and a row added for PCRE2 belongs to both
until something says otherwise.

Two things are worth taking from it beyond the fix. The section above this
one already said "`(?J)` is PCRE2's and perl refuses it", written while the
code accepted it: a page that is right is not a check. And the feature table
in [dialects.md](dialects.md) section 3 had given `NON_ATOMIC_LOOKAROUND` to
Perl, which is where the code's belief came from - a wrong spec table
becomes wrong code that review cannot catch, because review reads the table.

One disagreement remains and it is a decision: `\p{nv=1/1}`, which this
library resolves and perl does not, for the reason dialects.md section 6
gives. There were seven. The other six - the four script-run spellings and
the two `(?[ ])` rows - left when those constructs were built, and the list
is a two-way gate like `known-gaps.txt`, so the run failed with "remove the
KNOWN entry" until somebody did. A list of exceptions that can only be
added to is a list that stops describing anything.

### The script-run differential

`make check-oracle-script-runs` runs
[script_run_diff.py](../tools/oracle/script_run_diff.py). `(*script_run:...)`
is a *table* rule - UTS #39 section 5.1 - and a table rule is not tested by
a handful of cases, because its whole difficulty is which combinations are
allowed. So the corpus is built: every pair and every triple over an
alphabet of 28 characters chosen to hit each clause at least once, plus
3,000 longer random strings, each asked as `^(*sr:.+)$`. 25,764 subjects.

**Both references must agree before either decides.** PCRE2 and Perl
implement this independently, so a rule they answer identically over
thousands of cases is a rule rather than an implementation - and where they
differ there is nothing to hold this library to. 25,037 subjects are decided
that way, with no disagreement.

The other 727 are the useful part, because "the references disagree" is
where a blind spot would go if it were left there. Each is classified by
which reference this library sides with:

- **719 side with pcre2**, which carries Unicode 16.0.0 against perl's
  15.0.0 while these tables are 17.0.0. U+0301's Script_Extensions is the
  bulk of it: eight named scripts in UCD 17, and something wider in UCD 15.
  Siding with the *older* UCD would mean these tables are not the ones being
  read, so the tool fails if any row does.
- **8 side with perl**, and all eight are one pcre2 defect - it accepts a
  script run mixing Han with two of its companion scripts, which
  pcre2unicode says is not one. The tool carries the shape of that defect
  and fails on a row that sides with perl and is not it.

Proven by planting, twice. Removing the Common early-out - so that a run
beginning with a full stop constrains every later character to Common -
gives 2,859 disagreements and exit 1. Accepting an unassigned code point in
a run of two or more gives 96 disagreements, 6 of them landing in the
"sides with perl and is not the Han defect" bucket that exists for exactly
that. Both were checked by reading the *exit status*, not the output: the
first time round the status was read through a pipe to `tail` and came back
0 from `tail`, which is the hazard this page records elsewhere and which
found its way in here while writing this paragraph.

`tools/oracle/perl_diff.py` carries four script-run atoms as well, which is
a different question: not whether the rule is right but whether
backtracking into a run behaves, next to a quantifier or an anchor. Its
subjects are Latin and Common, so the rule itself is never what is being
asked there.

### The newline-convention differential

`make check-oracle-newlines` runs
[newline_diff.py](../tools/oracle/newline_diff.py). PCRE2's six newline
conventions each decide four things at once - which single characters `.`
refuses, where `^` and `$` hold, where they hold *between* the two
characters of a CR LF pair, and where an unanchored search may begin - so
the rule is not one rule and the four interact. Every convention against
every anchor-and-dot pattern against every two- and three-piece subject
over eleven pieces, in two flag settings: **360,360 rows, no
disagreement.**

pcre2 alone decides, which is this file's one asymmetry against its
neighbours: Perl has no newline conventions, so there is no second opinion
to be had and none is pretended.

It was written after the conventions were built, and it found two of the
four rules. The model assembled from hand probes had `.` refusing the
convention's character set and the anchors knowing about the pair; the
sweep came back with 60 disagreements, all `(*CRLF)`, and they said that
`.` also refuses **the position a terminator begins at** - which under
`(*CRLF)` is the CR of a pair and not the LF. Fixing that left 23, all one
shape, which turned out to be pcre2api's documented start-position
compromise. Neither was reachable from a handful of cases: the first needs
a pattern that consumes *two* characters across the pair, and the second
needs an unanchored search whose first attempt fails at the CR.

Proven by planting: with the pair unknown to the assertions, 23
disagreements; with `.` not refusing the CR of a pair, 37. Both exit 1.

One defect of this library's own came out of it and is worth recording,
because it was not in the new code. The Pike VM's search loop stopped when
its thread list was empty, which was sound while an unanchored search
always seeded a thread at every position - and the start-position skip
broke that invariant, so the search stopped one character before the
answer. `b$` against "\x00\r\nb" under `(*ANY)`: the backtracker and the
bit-state engine both reported 3-4 and the Pike VM reported no match. An
invariant that nothing states is an invariant the next change breaks.

### The callout differential

`make check-oracle-callouts` runs
[callout_diff.py](../tools/oracle/callout_diff.py). A callout is the one
construct here whose answer is not "did it match" but "where were you, and
when", which makes it the one construct every other gate in this file is
blind to: `a(?C1)b` and `ab` match the same subjects, so a span comparison
would report agreement about a feature that had not been built. This
compares the **trace** instead - for each row, the sequence both sides
reported, each callout as its number, the offset the attempt began at, how
far matching had got, the pattern offset, pcre2's `capture_top`, and the
string and mark it carried.

Every callout spelling at every position of every skeleton, against every
subject: **25,600 rows**, of which 150 are the one classified divergence
below and **no other disagreement**. Most of the insertions are syntax
errors - `a(?C1)*b` has nothing to repeat - and those rows are kept,
because that both sides refuse them is half of what "the same grammar"
means.

**Two pcre2 optimisations are off**, and the comparison would measure them
rather than the construct with them on. `PCRE2_NO_START_OPTIMIZE`: with it
on, `/a(?C1)b(?C2)c/` against "abd" prints *no* callouts, because pcre2's
required-code-unit test rejects the subject before the match runs.
`PCRE2_NO_AUTO_POSSESS`: with it on, `a*(?C1)b` against four a's prints
five callouts where a plain backtracker prints fifteen, because `a*` before
a disjoint `b` becomes `a*+`. Neither changes what pcre2 matches, and this
library has neither. The backtracking control verbs are absent from the
skeletons for the same reason: pcre2api says `NO_START_OPTIMIZE` changes
what `(*COMMIT)` and `(*SKIP)` do.

**Nothing is excluded any more, and two shapes were.** The first was a
callout written where a conditional's *condition* goes - `(?(?C9)(?=a)b|c)`
- which the parser dropped, because a conditional's children are the
condition and the branches positionally. Our trace was empty where pcre2's
had the callout, 150 rows in 25,600, and the match was identical
throughout, which is the kind of difference only a trace gate can see. The
parser keeps them now and lowering hoists them in front of the whole
conditional, which is where pcre2test prints them; asking that question
also found that pcre2 takes exactly *one* there and refuses two, where
this library had taken any number. A run that produces none of the shape
still exits 2, because a gate that has quietly stopped producing its own
hard case has quietly stopped asking.

The second is gone too. A callout *inside* an assertion
condition fired twice, because `(?(?=A)X|Y)` was lowered as
`(?:(?=A)X|(?!A)Y)` - exact, and two copies of A. This gate is what turned
that rewrite's documented cost, "time and not meaning", into a visible
one; the condition is now a single assertion that chooses a branch instead
of failing, and `(?(?=[a-y]{20}x)A|y)` went from 55 instructions to 31 and
from 204 steps to 128 on a subject that takes the else branch.

It found three things on its first run, none of them in the classified
list. Our `capture_top` was one too high wherever a callout stood inside an
*open* group: the group's start slot was set, and a group that has not
closed has captured nothing - it now reads as unset, which is both pcre2's
answer and the only safe one, since `{start, GRX_NPOS}` is a span a caller
could subtract. The mark was the wrong one of two: pcre2api defines the
callout block's as "the most recently passed" mark, the running value that
an abandoned branch does not take back, where this library was reporting
the narrower "still standing on this path" that `grx_match_mark()` answers
with. And the assertion-condition rewrite's comment claimed its cost was
"time and not meaning", which a side effect in the body is the exception
to - the finding that became the single-run conditional, and with it a
Perl capture rule that had been silently wrong.

Proven by planting: `pattern_offset` off by one gives 9,165
disagreements; the path-local mark gives 15. Both exit non-zero, and both
restore to zero.

### The replacement differential

`make check-oracle-replace` runs `tools/oracle/replace_diff.py`, which builds
patterns *and templates* and puts them through node and `pcre2_substitute()`
beside this library. `sed_diff.py` had done this for the POSIX and GNU rows
since WP-23; the two largest template grammars had no generator at all, and
their rates came from corpora that carry patterns and subjects and no
templates.

ECMAScript agreed from the first run and has never disagreed since. The PCRE2
row had **six** defects, every one of them also an error in the dialects.md
table the row was written from: no `$&`, `$0` or `${0}`; no `` $` ``, `$'` or
`$_`; no `$<name>`; a sigil beginning no complete reference treated as
ordinary text where PCRE2 refuses it; `$12` falling back to `$1` and a "2"
where PCRE2 takes every digit; and a reference to a group that exists and did
not participate substituting nothing where PCRE2's default is an error.

Three things are excluded, each counted in every run so that an exclusion
which stops applying is visible rather than silent:

- **The surrogate-pair deviation** (dialects.md section 6). A global replace
  visits every position, and ECMA-262 lets a zero-width assertion match
  between the halves of a surrogate pair where this library's subject is code
  points. Detected rather than assumed: node's answer then holds unpaired
  surrogates, which is exactly a string that cannot be encoded as UTF-8.
- **Template parsing up front.** pcre2 parses a template only when it has a
  match to put it in, so a malformed one against a subject that does not
  match comes back as the subject unchanged; this library parses it at
  `grx_regex_replace()` and reports the error either way. The exclusion is
  safe because `pcre2_substitute()` returns the substitution *count*, so it
  fires only where pcre2 made none - a template this library wrongly rejects
  on a subject that does match is still a disagreement.
- **The pcre2 10.46 internal error**, through `perl_diff.py`'s predicate
  rather than a second copy of it.

The flag alphabets are each dialect's own. A shared list asked this library
for `u` under `pcre`, which that row rightly refuses because PCRE2's UTF mode
is an option and not a pattern flag, and counted 1,283 refusals as
disagreements.

### The allocator that always moves

`tests/unit/test_allocator.cpp` and a second pass over the whole corpus in
`tests/conformance/test_vectors.cpp` run the library through
`grxtest::MovingAllocator`, whose `realloc` never grows a block in place:
it allocates fresh, copies, scribbles `0xDD` over the old block, and frees
it.

`test_oom.cpp` asks what happens when an allocation *fails*. This asks what
happens when one succeeds in a way the system allocator almost never does.
An arena that grows invalidates every pointer into it, and code that obtains
a node or an instruction, appends something, and then writes through the
pointer it obtained is using freed memory - but `realloc` grows a small block
in place whenever the bytes after it are free, which for the mostly
sequential allocations a compile makes is nearly every time. Such a defect
passes every test, passes valgrind, passes ASan, and surfaces the day a
pattern is a few nodes longer than the ones anybody tried.

**Two halves, catching different things.** The unit test compares a digest -
every match of a search-all loop, the replaced text, the split pieces, and
the facts - between the default allocator and the moving one, so *the answer
must not depend on the allocator*. ASan catches the other kind: a stale write
whose value nothing reads yet.

Both were proven by planting one of each in `src/compile/codegen.c`:

| Planted | default allocator | moving allocator | under ASan |
| --- | --- | --- | --- |
| the alternation's `SPLIT` held across a branch's codegen | 730 vectors fail | 783 fail | fails |
| the `ATOMIC_BEGIN` held across its body's codegen | nothing fails | nothing fails | `heap-use-after-free` |

The first row's 53-vector difference is the hiding place, measured: those are
the vectors where the default allocator's in-place growth left the held
pointer valid. The second row is why ASan is in the table at all -
`GRX_OP_ATOMIC_BEGIN`'s `x` is written by codegen, documented in the
instruction table and printed by the disassembler, and read by no engine, so
corrupting it changes no answer anywhere. That is a fourth instance of the
unread-constant shape this suite keeps finding, and here it was found by a
plant that refused to show up.

The corpus pass costs about 0.8 s on top of the 3.5 s the ordinary one takes,
and relocates 125,792 blocks. The size of each block is kept in a header
before it rather than in a `std::map`: the map version was correct and far
too slow to point at 35,753 vectors, which is the one place worth pointing it.

### The iteration differential

`make check-oracle-iterate` runs `tools/oracle/iterate_diff.py`, which asks
for **every** match rather than the first, against node's
`String.prototype.matchAll` for ECMAScript and `while ($s =~ /$re/g)` for
Perl.

`grx_regex_search_next()` is the one entry point whose answer is a sequence,
and nothing generated had asked it anything. `match_diff.py` and
`perl_diff.py` ask for the first match and stop - 58,254 perl cases at zero
disagreements, none of which is a question about the loop. Replacement and
splitting sit *on* the loop but report text and pieces, so a different set of
matches that happens to produce the same output is a disagreement neither can
see, and with a template of `$&` that is every disagreement about spans.

**Both oracles run their own loop**, which is the reason those two references
and not pcre2: `pcre2_match()` does not iterate, so a pcre2 column would be
this harness's loop compared against this library's - two copies of one idea.
The `pcre` dialect shares Perl's empty-match rule and is covered through the
perl column; where it does *not* share Perl's answer is the finding below.

**It found that `\G` is a second axis.** dialects.md section 5.10 put Perl
and PCRE2 in the same row, which is right about what follows an empty match
and silent about what `\G` means once the loop has moved. `\Ga*` against
"baac" is four matches in pcre2test and one in perl: PCRE2's `\G` is where
the current attempt began and follows the advance, perl's is `pos()`, which
the failed attempt did not move. `GRX_SearchStartRule` carries the two
values.

Each rule was then broken to see the gate bite:

| Change | ecmascript | perl |
| --- | --- | --- |
| every dialect given `RETRY_THEN_ADVANCE` | 182 | 0 |
| every dialect given `ADVANCE_ONE` | 0 | 492 |
| `\G` unpinned, so perl follows PCRE2 | 0 | 766 |

That last row is worth a note about the generator rather than the library.
With `\G` mixed one-in-twenty into the shared atom list, unpinning it gave
**one** disagreement out of 5,700 - a gate a change of seed could switch off.
The two axes fail differently: the empty-match rule shows up on nearly every
atom that can match empty, and this one shows up only where the loop is
forced to advance past a failure. `\G` therefore has a vocabulary of its
own, and the finding is 766 rows rather than one.

### The window differential

`make check-oracle-window` runs `tools/oracle/window_diff.py`, which varies
the six fields of `GRX_SearchOptions` that decide an answer - `begin`, `end`,
and the NOTBOL / NOTEOL / NOTEMPTY / NOTEMPTY_ATSTART flags - and puts the
result through `pcre2_match()` beside this library. Until it existed, every
differential in this suite searched the whole subject with no flags, so all
six knobs sat at one value across every one of the hundreds of thousands of
rows the suite runs. `tests/unit/test_search.cpp` had thirteen hand-written
cases for them.

**pcre2 is the oracle, and it is the only one that can be.** `begin` is
`pcre2_match()`'s `startoffset`, `end` is the `length` it is given, and the
four flags are PCRE2_NOTBOL, PCRE2_NOTEOL, PCRE2_NOTEMPTY and
PCRE2_NOTEMPTY_ATSTART. More than the mapping: PCRE2 is the one reference
here whose offsets are bytes and whose `^` and `\A` mean the start of the
*subject* rather than the start of the search, which is what
`GRX_SearchOptions::begin` means too. node has `lastIndex` and slicing but no
NOTBOL at all and UTF-16 offsets; glibc has `REG_NOTBOL` and `REG_NOTEOL`,
and `REG_STARTEND` looks like a window until you read what it does to `^` -
it matches at `rm_so`, where PCRE2's and this library's stay at offset 0.

**It found one defect, and it was in the header first.** `GRX_SEARCH_NOTBOL`
was documented as making "`^` and `\A` fail", and `GRX_SEARCH_NOTEOL` as
making "`$`, `\Z` and `\z` fail". Neither reference does that: PCRE2_NOTBOL
says in as many words that it does not affect `\A`, PCRE2_NOTEOL says the
same of `\Z` and `\z`, and glibc leaves GNU's `` \` `` and `\'` standing
under REG_NOTBOL and REG_NOTEOL. Two references, one rule, and the
thirteen hand-written cases all used `^` and `$` - the spelling the author
was thinking of - so none of them could see it.

The cause is a lowering that is right everywhere else: `^` without multiline
and `\A` are the same position, so they lower to the same
`GRX_ASSERT_START_SUBJECT`; `$` and `\z` to the same `END_SUBJECT`; `$` and
`\Z` to the same `END_BEFORE_NEWLINE`. Fusing them is correct for every
search of a whole subject, and these flags are precisely the condition under
which the two stop meaning the same thing. `GRX_IR_LINE_ANCHOR` and
`GRX_INST_LINE_ANCHOR` carry which spelling asked.

Each of the six fields was then broken in `src/exec/exec.c` in turn:

| Field ignored | Disagreements in 130,960 compared rows |
| --- | --- |
| `begin` | 30,716 |
| `end` | 10,821 |
| `NOTEMPTY` | 17,086 |
| `NOTEMPTY_ATSTART` | 7,055 |
| `NOTEOL` | 705 |
| `NOTBOL` | 672 |

Every row runs in byte mode, because PCRE2's UTF mode is an option rather
than a pattern flag and the shared flag alphabet has none. Every offset the
generator produces is therefore a valid one and none has to be excluded -
which is worth saying because the two differentials either side of this one
in this file both have an exclusion they have to count.

### The split differential

`make check-oracle-split` runs `tools/oracle/split_diff.py`, which builds
patterns, subjects *and limits* and puts them through node's
`String.prototype.split` beside `grx_regex_split()`. It was the last
documented surface of this library with no generator behind it: matching had
`match_diff.py`, replacement had `replace_diff.py` and `sed_diff.py`, syntax
had `syntax_diff.py`, and splitting had six hand-written tests every one of
which asserts a rule the author had already decided was right.

That gap mattered more than six tests suggests, because ECMA-262 22.2.6.14 is
not a loop over matches. It walks the subject, keeps a `p` for where the
current piece began, and discards a match whose *end* equals `p` - and three
rules fall out of that walk which a loop over matches gets wrong. The
generator's contribution is the interactions between them: a limit that runs
out between a piece and the captures spliced after it, an empty match
immediately following a non-empty one, a capture that participates on one
branch of an alternation and not the other.

**It agreed from the first run**, which is worth as little as any green gate
until the gate is shown to bite. Each of the four rules was deleted from
`src/subst/subst.c` in turn and the run repeated:

| Rule removed | Disagreements in 8,176 compared rows |
| --- | --- |
| an empty match where a piece begins is skipped | 3,025 |
| the empty subject yields one piece, or none | 580 |
| the limit is checked between a piece and its captures | 384 |
| a match at or past the end is not a separator | 1,690 |

None of the six hand-written tests fails for any of those four.

The one exclusion is the surrogate-pair one, for the reason the replacement
differential gives and detected the same way: without `u`, ECMAScript splits
a string of UTF-16 code units, so the empty pattern cuts an astral character
in half and the pieces are lone surrogates. Encoding those as UTF-8 would
substitute U+FFFD and make two different answers compare equal, so node
refuses the row and the harness prints how many it refused.

Patterns only one side compiles are counted apart from patterns neither
compiles. Folding the two together is how a generator ends up measuring
itself: `syntax_diff.py` is the tool for a syntax disagreement, and a
one-sided refusal here means this generator is asking fewer questions than
its row count claims.

The pieces cross the wire as spans and not as strings, `-` for a capturing
group that did not participate. A driver that printed an unset group and an
empty one the same way would have made the whole `(a)|(b)` family agree by
construction.

### The properties of strings

`make check-oracle-string-properties` runs
`tools/oracle/string_property_diff.py`. The property check above walks every
code point; this one cannot, because a property of *strings* has members that
are sequences and there is no space of sequences to walk.

So it walks the one that matters. `emoji-test.txt` lists every emoji sequence
UTS #51 knows about, including the minimally-qualified and unqualified
spellings that are deliberately *not* RGI - which is what makes it a two-sided
test rather than a spot check, because a table that was too large would fail
on those rows and a table that was too small would fail on the others. 5,225
sequences against each of the seven properties is 36,575 cases, and it
currently finds no disagreement. Changing one property's member count by one
fails it with the sequence named.

### The differential match check

`make check-oracle-match` runs `tools/oracle/match_diff.py`, which is the
same idea one level deeper: it asks both implementations what a pattern
*does* to a subject, and compares every group's span.

Spans rather than substrings, because substrings hide the failures that
matter. `(a*)*` and `(a*)+` against "b" both match the empty string; what
distinguishes ECMAScript from Perl is whether group 1 comes back unset or
empty, and only a span says which. Two defects were found this way that no
substring comparison would have shown: a capture-reset rule that was
implemented for unbounded repeats and not for `{m,n}`, and an
empty-iteration guard with the same gap.

The offsets are the hard part. The reference counts UTF-16 code units and
this library counts UTF-8 bytes, so the Node driver converts - by code point,
not by code unit, because the byte length of a lone surrogate is three and
summing that per unit puts every position after an astral character six bytes
out. A match that lands *between* the halves of a surrogate pair has no byte
offset at all, and is reported as such and skipped with a count rather than
given an invented one (dialects.md section 6.1).

Three outcomes are counted rather than compared, and the counts are printed
so that none of them can hide a real difference: a program no implemented
engine can run (a backreference, until WP-08), a pattern *both* sides reject,
and a surrogate position. A pattern only *one* side rejects is a
disagreement and is reported.

### The differential syntax check

`make check-oracle-syntax` runs `tools/oracle/syntax_diff.py`, which asks this
library and the dialect's reference implementation the same question -
"is this a valid pattern?" - about a few hundred thousand patterns, and prints
every disagreement.

It is the cheapest strong test the front end has, and it finds a kind of
defect reading cannot. A parser can be checked line by line against a grammar
and still be wrong, because the rules that matter are the ones attached to a
production *parameter* pages away from the production that uses them:
ECMAScript's Annex B excludes `k` from identity escapes whenever the pattern
names a group, and that exclusion reaches inside a character class, where a
named reference cannot appear at all. `[\k]` is therefore valid alone and a
syntax error in `[\k](?<n>x)`. That was found here, at the cost of one run.

Two corpora, because they fail differently. The **exhaustive** one is every
string of up to three characters over the dialect's punctuation, which is
where a lexer's lookahead is wrong. The **random** one is up to fourteen
tokens drawn from a vocabulary that deliberately includes constructs the
dialect does *not* have, because rejecting what the reference rejects is half
of conformance.

A pattern this library refuses for a **limit** is counted separately and
compared with neither answer. Its syntax was never read, so the comparison
has nothing to say about it, and counting it as accepted would hide a real
disagreement behind a cap.

The target skips with a message when the oracle is absent. `ORACLE_SEED` and
`ORACLE_COUNT` vary the corpus; a soak before a milestone runs several seeds
at a larger count.

## 6. Structural checks

Run by `make test` alongside `check-symbols`:

- **The dialect is gone after lowering:** no file below the IR names a
  `GRX_Syntax`, a `GRX_SYNTAX_` constant or a `grx_syntax_*` function, nor
  reads `GRX_Regex::syntax` through the header. A grep, in the Makefile.
  **Built:** `make check-layering`, in `TEST_GATES`, covering `src/exec/*`,
  `src/compile/codegen.c` and `src/compile/program.c`. Three files are
  legitimately *above* the line and the Makefile names them with the reason:
  `compile.c` holds the public accessors and `grx_regex_syntax()` reports
  rather than branches, `compile_internal.h` declares the field reporting
  reads, and `lower.c` is where the dialect is spent.
  **To check the gate itself:** put `GRX_Syntax x;` in `codegen.c`, or
  `regex->syntax` in `exec_pike.c`. Both must fail the build.
- **A dump's name table is as long as its enum.** Every dump here turns an
  enumerator into a word through a positional table sized by the enum's
  `_COUNT`, and a name left out does not leave a hole at the end - it
  shifts every name after it onto its neighbour. The compiler cannot see
  it: the array is sized and the missing tail is NULL, and every lookup
  guards and returns `"?"`, so nothing crashes and nothing warns.
  **Built:** `make check-dump-names`, in `TEST_GATES`.
  **Seven of twenty-eight tables were short when it was written**, and only
  one of the seven printed anything obviously wrong - `iterate=(null)`,
  from a rule added that afternoon. The other six printed a *neighbour's*
  name: the IR's assertions had been dumping one place out since
  `word-start` and `word-end` were added, three copies of the conditional
  kinds had no name for `static`, and the IR's capture-reset table had none
  for Perl's `after-each`. It finds the enum by the block its `_COUNT`
  closes rather than by a prefix, because `GRX_REPEAT_MODE_COUNT` closes
  `GRX_RepeatMode`, whose members begin `GRX_REPEAT_`.
  **To check the gate itself:** delete a name from the middle of any table,
  or add an enumerator before a `_COUNT`. Both must fail; both were tried.
- **The strict-aliasing warning is still armed.** `-fstrict-aliasing
  -Wstrict-aliasing=1` ride every C compile line, under `-Werror`, so a real
  violation fails the build and no sweep is needed. A *disarmed* warning
  fails nothing and looks exactly like a clean library, which is what
  `make check-aliasing` is for: it compiles a planted type-punning violation
  with the library's own `$(CFLAGS)` and fails if the compiler accepts it.
  **Built:** in `TEST_GATES`, one `-fsyntax-only` invocation.

  This is the one undefined-behaviour class with no runtime instrument at
  all, so `make test-asan` and every fuzzer are blind to it however they are
  compiled. Measured rather than assumed, with a program that writes 7
  through an `int32_t *`, writes `1.0f` through a `float *` aliasing the same
  object, and reads the int back:

  | build | result |
  | --- | --- |
  | `-O0`, no sanitizers | prints `1065353216` |
  | `-O2`, no sanitizers | prints `7` |
  | `-O1` and `-O2`, ASan+UBSan, gcc **and** clang | prints `7`, exits 0, says nothing |

  The optimiser's answer already differs from the unoptimised one - the
  violation is live at the level this library ships at - and four sanitizer
  runs report no error at all. `text` measured the same blindness against
  checks those sanitizers *do* catch (heap-use-after-free, stack overflow,
  signed overflow, float-cast overflow, all caught at `-O1` and `-O2`),
  which is what makes it a gap in the instrument rather than a quiet run.

  The static warning is partial too: it does not follow a violation laundered
  through a function boundary, and no level catches punning through a
  `void *`.

  The **level** is named because `-Wall` already sets one. `gcc -Q
  --help=warnings -Wall` reports `-Wstrict-aliasing=3`, and level 3 is
  silent on shapes level 1 rejects - so `-Wall` at `-O2` gave this library
  the optimiser assumption with no warning behind it until the gate landed.
  All 38 library translation units compile clean at level 1, measured; that
  is a property of this code rather than a general one. For contrast,
  `libs/ctang`'s 61 source translation units give 588 diagnostics across 47
  of them at the same level, every one a downcast to a struct's initial
  member that C17 6.7.2.1p15 makes well defined - where that is the
  architecture, this gate could only ever say that level 1 still works.

  **The shape of the control is load-bearing**, and the requirement for
  anyone changing it is that a control for level N must be caught at N and
  **missed at N+1** - one that survives into the weaker level still passes
  after the gate has silently fallen back to it. Measured with this tree's
  gcc 14.2 at `-O2`, counts of the diagnostic:

  | control | L0 | L1 | L2 | L3 |
  | --- | --- | --- | --- | --- |
  | `*(int *)&obj`, a known object, in place | 0 | 1 | 1 | 1 |
  | `int *p = (int *)&obj; *p` | 0 | 1 | 1 | 0 |
  | `int *p = (int *)f; *p`, `f` a parameter - **this one** | 0 | 1 | 0 | 0 |
  | punning through a `void *` | 0 | 0 | 0 | 0 |

  The first row is useless as a probe at any level, because it fires from 1
  upward and distinguishes nothing - which is the trap, since it is also the
  most natural way to write a type pun. The last is the standing limit.

  The warning is a gcc diagnostic: clang accepts `-Wstrict-aliasing=0`, `=1`
  and `=2` in silence and implements nothing behind them, and rejects `=3`
  as an unknown option. So `make CC=clang` reaches the gate with the flags
  on every compile line and no coverage behind them; that is a true failure
  and the message separates "the flags are wrong" from "the compiler does
  not implement them".
  **To check the gate itself:** `make check-aliasing
  EXTRA_CFLAGS=-Wstrict-aliasing=3`. `CFLAGS` ends with `$(EXTRA_CFLAGS)`
  and an explicit level beats `-Wall`'s implicit 3 from either side, so that
  is the disarm vector, and it must fail.
- **Every `GRX_Diag` has a string** and **some code path raises it**. The
  first half is `DiagnosticCatalogueIsComplete`, which is a statement about
  the table. The second is `make check-diagnostics`
  (`tools/check_diagnostics.py`), which is a statement about the library: a
  diagnostic nothing raises is a promise in a public header that is never
  kept. It waited for the front ends that emit most of them, which is WP-18
  and WP-23, and both have landed.

  Three remain unraised and the script lists each with a reason, as a gate in
  both directions: one that is unraised and unlisted fails, and a listed one
  that becomes raised fails with "remove the entry". It fired on exactly that
  second direction when the match-time channel landed, naming all six rows
  that had just become reachable.

  Six used to sit there as one decision rather than six: the match-time entry
  points took no `GRX_Error`, so a run that stopped at a limit or at a
  subject that is not valid UTF-8 had nowhere to put a diagnostic, and
  `GRX_ERR_LIMIT` alone does not say which knob a caller has to raise. They
  are raised now, through `grx_match_error()` on the match object rather than
  through a parameter added to seven entry points - a match-time failure has
  no offset into the pattern, so it belongs beside `grx_match_steps()` and
  `grx_match_engine()` with the other facts about an attempt. Of the three
  left, two are constructs that turned out not to be errors anywhere - a
  forward backreference compiles in perl and in pcre2test, and an
  unterminated `\Q` runs to the end of the pattern - and one is reached
  through a result code rather than a diagnostic.
  **To check the gate itself:** delete a line from `UNPRODUCED` and it must
  fail; add a diagnostic that *is* raised and it must fail the other way.
- **Every `GRX_Feature` bit is set in at least one profile row** and
  **every profile row is complete** (no zero enum where zero is not a
  value). **Not built.**
- **The documents that quote output are checked against the output.**
  `tests/unit/test_docs.cpp` extracts the fenced blocks from
  `development.md` and `README.md` and compares them with what the library
  actually prints. **Built.** It is a test of the pages rather than of the
  library, and it lives with the code because that is what it compares them
  against. Prose is deliberately not checked: a page that has to be
  word-for-word correct is a page nobody edits.
  **To check the gate itself:** change a digit in one of the dump examples.
- **No STUB-marked test survives the stub it marks:** a test whose comment
  says `STUB` for a function whose implementation no longer returns
  `GRX_ERR_UNSUPPORTED` fails. **Not built, and no longer needed for the
  stubs it was written for** - there are none left. Worth building before
  the next round of stubs rather than after.

## 7. Importing the reference corpora

The corpora are other projects' test suites: large, reproducible from a ref
and a URL, and a copy here would be a snapshot that stops being what everyone
else is measured against the moment it is taken. `tools/corpus/fetch.sh`
fetches them into `third_party/`, which is not committed;
`tools/corpus/VERSIONS` pins the ref of each and *is* committed, because "94%
of test262" means nothing without saying which test262.

Every importer follows one rule: **the corpus contributes the cases and the
oracle contributes the answers.** Where a corpus states its own expectation,
that expectation is checked against the oracle rather than trusted, and a
disagreement drops the case and is reported - the likeliest explanation for
one is that the importer misread the file, and writing down an answer nobody
confirmed is the failure these tools exist to avoid.

`make vectors-<dialect>` regenerates one dialect; `make vectors` does all of
them. Each skips itself with a message when its oracle or its corpus is
absent.

- **test262** (`tools/corpus/import_test262.py`): the `test/built-ins/RegExp`
  tree, 1,879 files. It produces **two** files, because the corpus answers two
  different questions and conflating them would overstate what is measured.

  `test262_syntax.rxt` holds the cases whose expectation test262 states in a
  machine-readable form - the `negative: { phase: parse, type: SyntaxError }`
  frontmatter and `assert.throws(SyntaxError, ... RegExp(p, f) ...)`. The
  corpus supplies both case and answer there, so a pass rate over that file is
  a test262 pass rate.

  `test262_patterns.rxt` holds patterns harvested from the files whose
  assertions are about JavaScript rather than about the pattern - `lastIndex`
  after a `g`-flagged `exec`, what `Symbol.replace` does with a subclass. No
  expectation can be lifted out of those, but the *patterns* are worth having:
  thousands of expressions written by hand by people trying to break
  implementations, which is exactly what a random generator does not produce.
  Every answer in that file is the oracle's. It is a corpus import and not a
  conformance rate, and it says so in its own header.

  Three rules keep the import honest, each of them added after its absence
  produced a false finding: a case whose flags a vector cannot carry is
  skipped rather than mangled (a duplicate letter, or `u` and `v` together -
  collapsing `"ii"` to `"i"` turns a case that must be rejected into one that
  must be accepted); `y` is skipped because sticky anchors the match, which is
  not the question a vector asks; and an astral subject is only tried under
  `u`, because [dialects.md](dialects.md) §6.1's deviation would otherwise
  make eight vectors fail on purpose.

- **pcre2test** (`tools/corpus/import_pcre2test.py`): `testinput1` and
  `testinput2`, as **syntax verdicts only**. pcre2test answers "does this
  compile" directly - it echoes the pattern and follows a rejection with
  `Failed: error N at offset M` - which is exactly what [plan.md](plan.md)'s
  WP-18 is measured on. Its *match* answers are another matter: it prints
  matched text rather than offsets, and turning text back into spans is
  guesswork in the cases worth having, so those wait for an oracle driver
  linked against libpcre2, which is WP-20's business.

  The patterns are put to pcre2test **with their original modifiers**, not
  with the flags this library maps them to: `x` decides whether `#` starts a
  comment, so `/a#)/x` compiles and `/a#)/` does not, and asking the bare
  pattern recorded the wrong verdict for seven of them.

  A modifier written twice is not a modifier written once. `xx` is extended
  mode *and* ignores space inside a bracket expression, so `/[a-  z]/xx`
  compiles and `/[a-  z]/x` is "range out of order" - and this importer folded
  the two together for one revision, by the same two lines that folded
  test262's `"ii"` into `"i"`. Both were found the same way: by a front end
  disagreeing with a verdict the corpus should never have recorded. A
  duplicate letter is now either a *wider* mode the importer knows about, or a
  case it refuses to import rather than answer for a pattern nobody asked
  about.

- **Perl `re_tests`** (`tools/corpus/import_re_tests.py`): the tab-separated
  `pattern subject y/n/c expr expected` format. The corpus's fourth and fifth
  columns are Perl code that states *where* it matched; asking Perl directly
  is simpler and is the rule above, so the spans come from
  `tools/corpus/perl_match.pl` - Perl as a matching oracle in the same shape
  as `node_match.mjs`, converting character offsets to UTF-8 byte offsets -
  and the corpus's own `y/n/c` is used only to check that the importer read
  the row correctly.

  The subject column is a double-quoted Perl string, and its escapes are
  decoded here rather than by handing the text to `eval`: a test corpus is
  still data, and a tool that can be made to run what it reads is a tool with
  a different threat model. A row using an escape this does not know is
  skipped and counted.

- **CPython `re_tests.py`**: the same format as Perl's, in Python. Not yet
  imported; WP-30.
- **Spencer's tests** (`tools/corpus/import_rxspencer.py`): the classic
  `pattern flags subject expected` lines with the `-` conventions, from the
  copy glibc carries and runs as `tst-rxspencer`, so the test set and the
  implementation answering it are versioned together. The answers come from
  `tools/oracle/posix_match.c`, which is glibc's `regcomp`/`regexec` behind
  the same line protocol the other drivers use. It asks through
  `REG_STARTEND`, because a subject may contain NUL and a NUL-terminated API
  cannot be asked about one; a *pattern* containing NUL has no such escape
  and is declined.

  **These are GNU vectors, not POSIX ones.** glibc's `regcomp` accepts `\|`,
  `\+`, `\?`, `\w`, `\b` and `\<` in a BRE and `\w`/`\b` in an ERE -
  POSIX leaves a backslash before an ordinary character undefined and GNU
  defines it - so every row is written as `gnu-bre` or `gnu-ere`, which is
  the question glibc was actually asked. Pure POSIX BRE and ERE need an
  oracle that refuses those constructs, which this machine does not have.

  The cross-check is the strongest of any importer here, because Spencer's
  file states the expected *group* spans and not only the overall match: 429
  of 463 cases agreed with glibc on every span and were written down. Of the
  34 that did not, 24 are `[[:<:]]` and `[[:>:]]`, Spencer's own word-boundary
  classes that glibc does not have, and 8 are empty alternatives - `|`,
  `a||b`, `(a|)b` - which Spencer expects `regcomp` to reject and GNU
  accepts. Both are real divergences rather than misread rows, which is why
  they are dropped rather than recorded. A further 28 rows use flags no
  vector can carry: `REG_NOSPEC` and `REG_PEND` are BSD extensions glibc does
  not have, `REG_NOSUB` the corpus itself calls "not really testable",
  `REG_STARTEND` as a *flag* re-uses the subject's parentheses to mean an
  extent, and `REG_NOTBOL`/`REG_NOTEOL` say the subject's ends are not line
  boundaries, which no option here means.

  The front end that reads them is WP-23, which landed with them, and all
  four dialects now answer every row: `gnu-ere` 270 of 270, `gnu-bre` 159 of
  159, `posix-ere` 245 of 245 and `posix-bre` 135 of 135. The six that used
  to remain were POSIX's submatch rules, and were two things rather than one:
  leftmost-longest, which the profile had asked for and no engine read
  (WP-24), and an empty-iteration rule that had been filed against WP-26's
  tagged transitions and was nothing of the sort
  ([dialects.md](dialects.md) §5.5).
- **Perl case folding** (`tools/corpus/make_fold_vectors.py`): not an import
  either - 1,924 rows generated by asking Perl about every code point whose
  full case fold is longer than one code point, which is the 104 `F` lines of
  `CaseFolding.txt`. Each one as a literal, as a character class member and
  inside a negated class; against its fold, its fold uppercased and itself;
  then under `/aa`, in both directions; then the shapes that bound the rule -
  a range, a degenerate range, a shorthand, a POSIX class, a property.

  It was written because `re_tests` had nothing on the axis and therefore
  could not fail on it, and it found two defects on the day it was written.
  A class did not full-fold a member at all, which was 0 of 104 while the
  imported corpus read 2,592 of 2,592; and `/aa` dropped every full fold
  where Perl drops only those whose fold contains an ASCII code point, which
  is 17 of the 104 rather than all of them. Both are fixed and both are
  [dialects.md](dialects.md) §5.8.

  The second is the one worth the paragraph. The generator swept `/aa` over a
  seven-code-point sample, and only one of those seven had an all-non-ASCII
  fold - so four failing rows stood for eighty-seven, and the number was
  small enough to look like an edge case rather than a rule. **Sampling an
  axis whose answer is a property of each member is not sampling; it is
  guessing.** It now sweeps all 104.

- **JSON-Schema-Test-Suite** (`tools/jsonschema/`): not a vector import -
  these are run *through* `text`, with this library plugged into its
  regular-expression provider vtable, because what they measure is the pair.
  `tools/jsonschema/fetch.sh` fetches the files named in `JSON_SCHEMA_FILES`
  (both drafts) at the commit pinned in `tools/jsonschema/SUITE_COMMIT`, into
  `third_party/`, which is not committed; `make check-json-schema-suite` runs
  them and `JSON_SCHEMA_DRAFT=draft7` selects the older one. The corpus is not
  vendored for the same reason the UCD is not: it is somebody else's, and a
  copy here would be a snapshot that stops being what every other
  implementation is measured against.

  The gate is in `TEST_GATES`. It was not, for as long as it took to notice,
  and the reason it was not is the reason it is now: it needs `text`
  installed and the corpus fetched, and each of those printed `skipped` and
  exited 0. A target nobody types, which cannot fail when it is not run,
  cannot report anything; this seam is the only thing that asks whether this
  library and `text` are right *together*, and nothing was asking. Both
  preconditions are now hard failures naming their fix, and a machine that
  genuinely has no `text` drops the gate on the command line -
  `make test TEST_GATES='$(filter-out check-json-schema-suite,$(ALL_TEST_GATES))'`
  - so that the choice is in the command rather than in the output of a run
  that looked like it passed. `ALL_TEST_GATES` exists for that: a
  command-line `TEST_GATES` that names itself is a make recursion error, not
  a subtraction.

  Current: **51 of 51** cases in draft2020-12 and **46 of 46** in draft7, no
  group skipped. The runner exits non-zero on a wrong answer, which was
  checked by flipping one expectation and watching it fail rather than by
  assuming.

  It also exits non-zero when it does not reach the count it was told to
  reach. `--expect-passed`, which the Makefile fills from
  `JSON_SCHEMA_EXPECT`, is what stops the check from passing by declining to
  run - the exit status alone says only that nothing answered *wrongly*, and
  an empty suite file, a corpus trimmed to nothing, and a group skipped
  because `text` stopped implementing a keyword all satisfy that. That last
  one is not hypothetical in shape: a skip is exactly how this pair would
  come apart, and it would have lowered the pass count and changed nothing
  else. The suite commit is pinned, so the count is a constant, and it moves
  in the commit that moves `tools/jsonschema/SUITE_COMMIT`. Four plants were
  run before this was believed - `text` absent, the suite absent, the count
  one too high, the count one too low - and all four failed the gate.

  `JSON_SCHEMA_FILES` is `pattern patternProperties maxLength minLength`. The
  last two are not about regular expressions, and they are there because the
  first version of this check ran only the first two and therefore could not
  have caught a defect in the pair it validates. There was one: `text`
  measured string length in bytes where JSON Schema counts characters. Adding
  the file turned that from an argument into two failing cases, and then into
  a fix - which is the whole reason to run somebody else's corpus rather than
  one's own.

## 8. Fuzzing

Three harnesses, all under `tests/fuzz/`, all built per the suite's
convention (`-fsanitize=fuzzer,address,undefined`, the library rebuilt
instrumented):

| Harness | Fixed | Fuzzed | Checks |
| --- | --- | --- | --- |
| `fuzz_pattern` (exists) | - | pattern, dialect and limits via the options byte | parse, compile, dump, one search: no crash, no leak, limits honoured |
| `fuzz_subject` | a pattern chosen from the corpus by the options byte | subject | every eligible engine: no crash; `GRX_ERR_LIMIT` only when a limit is set below the structural bound |
| `fuzz_crossengine` | - | pattern and subject, split by a length byte | all eligible engines run; **abort on any disagreement in spans** |

**The options byte, as each harness actually reads it.** Each is documented
at its own use and repeated here because the three differ and a reader
comparing them should not have to open three files.

- `fuzz_pattern`: bits 0-2 choose whether to pick an arbitrary dialect, which
  one input in eight does, because a dialect with no front end is refused at
  the first call and the run is spent. The other seven-eighths are shared
  among the dialects that *do* have one, and the list of those is named in
  the harness rather than derived: it said "ECMAScript" for as long as
  ECMAScript was the only one, and went on saying it through WP-18 and
  WP-23, so each of the six built dialects was getting a thirty-second of
  the campaign instead of an eighth. `GRX_FUZZ_SYNTAX=<dialect>` pins one,
  which is what plan.md §4's fourth condition - eight hours clean *with the
  dialect selected* - needs in order to be something anyone can run. Bits 3-5
  choose the option set, which is the thing that matters: ECMAScript is three
  grammars, not one, and `v` reads `--` as an operator where `u` reads two
  dashes. Bits 6 and 7 tighten two disjoint sets of limits, covering every
  enforced field between them - bit 7 was added when
  `max_lookbehind_length` became enforced, since a limit no harness can
  exercise is a limit nobody finds out is broken. Downward only, never to
  zero: "zero means no limit" is checked where the input is chosen, and a
  fuzzer that can turn off `max_steps` is a fuzzer that hangs.
- `fuzz_subject`: bits 0-1 choose which list the pattern comes from - one
  input in four takes the UnicodeSets list, which needs the flag as well -
  and the rest of the byte chooses within the list, the case flags and the
  tight-limit mode.
- `fuzz_crossengine`: bits 0-2 are `CASELESS`, `MULTILINE` and `DOTALL`; bits
  3-4 together select `v`, so one input in four reads the UnicodeSets
  grammar.

**How long an input, and why that is a second question.** libFuzzer's
default `-max_len` is 4096 and nothing overrode it, so until `FUZZ_MAX_LEN`
was added no pattern and no subject over 4 KB had ever been fuzzed - against
a `max_pattern_length` of 65,536. The ceiling is now that limit, so a
harness can reach the refusal as well as everything below it.

Raising the ceiling is not enough on its own: libFuzzer grows inputs from
short to long over a run, and a run measured in minutes never arrives. So
there are two modes, and neither replaces the other.

| | length control | what it is for |
| --- | --- | --- |
| `make fuzz-run-<h>` | on | the short end, where most bugs are and where throughput is highest |
| `make fuzz-long-<h>` | off | generates at the full length from the first input; the only way the long end is reached at all |

It costs less throughput than it looks, because every harness caps
`max_steps` far below the default: a long subject ends at the step limit
rather than scanning to the end. The first full-length runs held 1,250-1,900
executions a second and pushed the longest corpus entry from 4,088 bytes to
13,865.

**A run count is not coverage.** Until `9bac05f` the pattern fuzzer chose
uniformly from sixteen dialects, fifteen of which refuse everything at the
first call, so about fifteen runs in sixteen were spent on a single early
return. A soak of 974,873 runs found nothing; a seven-minute run after the
weighting was fixed found a read past the end of a property name. The
UnicodeSets grammar had the same problem for longer - no harness set the
flag at all until Phase 2 was finished - and the three lists above are the
fix. The question to ask of a harness is which code its option byte can
reach, not how many times it ran.

Every harness also runs under a 256 KB stack (`ulimit -s 256`) in the
soak, because the promise that no engine's stack depth depends on its input
is one only a small stack can test.

A small stack tests it; it does not measure it, and for a long time nothing
did. `tests/unit/test_stack.cpp` does: it runs a search on a thread whose
stack it owns, paints the memory below the frame, and scans back for the
high-water mark afterwards, which is the engine's actual cost in bytes
rather than a frame count that would need the engine's cooperation to
collect. It sweeps the subject over four orders of magnitude, the program's
size over two, and the program's assertion nesting to the cap - and it is
where the claim in design.md section 9 invariant 6 was found to be half
wrong. The measurement is skipped under AddressSanitizer, which relocates
locals to a heap fake stack, and under Valgrind, which counts a deliberate
write below the stack pointer as an invalid one; the verdicts those runs
check - that nesting past the cap is a `GRX_ERR_LIMIT` and not a crash - are
checked in every build.

Corpus seeds come from the vectors: `make fuzz-seed` writes every vector's
pattern (and subject) as a corpus file, so the fuzzers start from real
syntax.

## 9. The gates are themselves tested

A gate nobody has tried to fail is a gate that might not work, and this suite
has already shipped three that did not: `make test` carried only the *last*
test binary's exit status, so a failure in any earlier one printed and was
discarded; UBSan is recoverable by default, so a violation printed
`runtime error: ...` and the run still reported "suite clean"; and
`-fsanitize=undefined` under **gcc** does not include `float-cast-overflow`,
where clang's does, so converting a float too large for its integer type was
undefined behaviour nothing was watching for. All three were found by
deliberately breaking something and noticing the build stayed green - not by
reading the Makefile, which looked right each time.

The third has a second half worth keeping, because it is the first two
combined: turning the check *on* without also naming it in
`-fno-sanitize-recover=` makes it print the diagnostic and exit 0. Measured
all three ways - off: prints nothing, exits 0; on and recoverable: prints,
exits 0; on and named in both: exits 1. `UBSAN_CHECKS` in the Makefile is one
list feeding both flags so the halves cannot drift apart again.

So each gate has a known way to make it fail, and that is exercised by hand
when the gate changes:

| Gate | Injected fault | Must give |
| --- | --- | --- |
| `make test` | a failing `EXPECT_EQ` in the *first* test binary, not the last | non-zero, naming the suite |
| `make test-quiet` | a *crash* in one binary, not an assertion failure | non-zero; the TOTAL line must not say PASS |
| `make test-valgrind` | a `malloc` never freed, in the first binary | non-zero, naming the suite |
| `make test-asan` | a write past a heap allocation | non-zero, ASan report |
| `make test-asan` | a signed integer overflow | non-zero, UBSan report, no "clean" line |
| `make test-asan` | `(int)1e30`, a float cast that does not fit | non-zero, UBSan report |
| `make fuzz-run-<h>` | a `__builtin_trap()` on a reachable input | non-zero, crash artifact written |
| `make fuzz-run-<h>` | a signed integer overflow on a reachable input | non-zero, UBSan report |
| `check-symbols` | an exported function with no `namespace.h` entry | non-zero, naming the symbol |
| `check-layering` | a `GRX_SYNTAX_` mention under `src/exec/` | non-zero, naming the file |
| `check-aliasing` | `EXTRA_CFLAGS=-Wstrict-aliasing=3`, a later explicit level | non-zero, naming the effective level |
| `check-aliasing` | `CC=clang`, which implements no such diagnostic | non-zero, naming the compiler rather than the flags |
| `check-oracle-vim` | widen one of vim's eleven named classes by a single code point - `\s` to include the line break | non-zero; the run reports the rows where the two now differ |

**The first binary, not the last**, is the point of the first two rows: the
defect they guard against is invisible if the fault is injected at the end.

**A crash, not an assertion failure**, is the point of the third, and it was
added because the gate failed it. `test-quiet` decided its verdict from the
number of failing assertions it could parse out of each suite's output. A
suite that segfaults prints no `[  FAILED  ]` line and no test count, so it
contributed *zero* failures, and a run with one crashed binary printed
`TOTAL ... PASS` and exited 0 - with that binary's own line, four rows
above, saying FAIL. `make test` exited 2 the whole time, so the two
spellings of "run the tests" disagreed about whether the suite passed.

The verdict now counts suites that exited non-zero and the assertion count
is only reported. That is the general form: **the count is a report and the
exit status is the verdict**, and a gate that computes its verdict from the
report can only see the failures that were articulate enough to describe
themselves.

### A driver that shortens its input answers a different question

The three oracle drivers - `grx_syntax`, `grx_match` and `grx_limits` - read
hex-encoded records from stdin into fixed buffers. All three used to stop at
the end of the buffer and carry on with the prefix, and that is the one
failure a differential gate cannot survive: Node is asked about the whole
pattern, this library about a shorter one, and the two answers are compared
as though they were answers to the same question. A gate in that state
reports agreement and means nothing.

It was found by checking a published number rather than by any of the rows
above. `measure.py` asks `grx_limits` for a 100,000-byte subject; the driver
scanned 65,536 of them and the report divided the step count by the length
it had asked for, so every rate in `dialects.md` §7 was 1.53 times too low
and the `max_steps` arithmetic 1.53 times too generous. The oracle corpora
never came near any of the three buffers - their subjects are under twenty
bytes - so no gate had ever been wrong because of it, which is exactly why
nothing found it.

All three now refuse a record they cannot hold, and `syntax_diff.py` and
`engine_diff.py` stop with a non-zero status if a driver ever says
`toolong`. The rule this is an instance of: **a test harness may fail, and
may refuse, but it may never quietly measure something smaller than it was
asked to.**

## 10. The ReDoS corpus

`tests/data/redos/ecmascript.rxt`: 17 pattern and subject pairs that are
exponential or high-polynomial under an unmemoised backtracker - the
classical `(a+)+$`, `(a|aa)+$`, `(.*a){20}$`, `(x+x+)+y`, the shape the 2016
Stack Overflow outage was, the one the Java `Pattern` documentation warns
about, and the "trim and split" patterns that appear in real validation
code. Written by `tools/limits/make_redos_corpus.py`, which carries the
provenance of each row beside it.

Each row records what the **backtracking engine** does with the pair at the
default limits, and fifteen of the seventeen now say a span or `nomatch`
rather than `limit`. That is the late memo (`design.md` §3.5.3): the
backtracker arms the bit-state bitmap once a run has taken more steps than
there are `(instruction, position)` states to take them from, which is the
point at which it has provably repeated itself. The fifteen cost between one
and eight thousand steps.

The two that still say `limit` - `(a|a?)+$` and `(a*)*$` - are the two whose
body can match empty. A loop like that carries a progress register, the
register is state the memo's key does not include, and so the memo would be
unsound and is never armed. They are the shape of the exponential case this
library still has, which is why the corpus is still here.

`tests/conformance/test_redos.cpp` checks two things about every row, and
the second is the one that matters:

- the backtracker reaches the recorded verdict **quickly**, whether that
  verdict is an answer or `GRX_ERR_LIMIT`. A limit reached after a minute is
  not a defence against a hostile pattern; it is the same outage with a
  different ending. Currently under a millisecond for an answered row and
  about 120 ms for a refused one, against a budget of one second.
- the Pike VM or the bit-state engine **answers**, and agrees. A library
  whose only response to `(a+)+$` is "I gave up" has not solved the problem,
  it has renamed it. Currently under a millisecond for every row. A second
  test doubles the subject three times and checks that the step count does
  not square, so the answer stays an answer as the subject grows.

A row whose verdict changes fails this test rather than being absorbed by
it. The memo's reach is meant to be something a person decided and wrote
down, not something that drifted: if a change makes `(a*)*$` answerable, the
corpus is regenerated and this section says so.

This is the corpus that sets `max_steps` and `max_backtrack`
([dialects.md](dialects.md) §7), and it is a regression suite for the
prefilters of Phase 8, which must not make a pathological pair pathological
again by bypassing the engine that handled it.

**What the forward lookbehind costs.** The model Perl and PCRE2 use
([design.md](design.md) §3.5.2) is the more expensive of the two per
assertion, and the cost is worth stating rather than discovering. Measured
against 20,000 candidate positions, `(?<=a{200})b` costs the same under both
models — a body of one length has one candidate start whatever its length —
and `(?<=(a|aa|…|a×59))b` costs about ten times more forwards than
backwards, because the assertion has to find the branch that spans the
distance where running backwards takes the first branch that fits. The length
guard is what keeps that a factor of ten rather than a factor of a hundred:
without it the same pattern exhausts `max_steps`. The bound is the dialect's
own 255 bytes of variation, so the factor is a constant and not a second pass
over the subject — but it is a large constant, and a prefilter (Phase 8) is
what would stop the assertion being reached at most of those positions at
all.

## 10.2 The segmentation conformance files

`tests/unit/test_break.cpp` runs the Unicode Consortium's own data for the
four boundary algorithms: `GraphemeBreakTest.txt` (766 lines),
`WordBreakTest.txt` (1,944), `SentenceBreakTest.txt` (512) and
`LineBreakTest.txt` (19,338). Each line is a string with `÷` where a boundary
falls and `×` where one does not, so a line asserts something about *every*
position rather than about one of them.

This is the strongest gate in the suite by a distance, and it is the reason
these algorithms are in rather than approximated. The sixteen `\b{...}`
records the Perl corpus contributed all use the **empty subject**: they would
pass against an implementation that was wrong everywhere else. Sentences
failed four cases on the first run, and the bug was real - UAX #29 §6.2's
limit on the ignore rules, which made an `Extend` after a line feed part of
the line feed and ended a sentence that had already ended.

The files are not committed (`third_party/ucd/` is excluded), so a checkout
without them skips with a message rather than failing, as
`make check-unicode-tables` does.

`tests/data/vectors/perl/boundaries.rxt` is the second gate and answers a
different question: not where a boundary falls, but what the *dialect* does
with it. Generated from Perl by `tools/corpus/make_boundary_vectors.py`.
Perl 5.40.1 carries UCD 15.0.0 and these tables are 17.0.0, so rows whose
answer changed between those editions are excluded **by name**, with the rule
and the version beside each - and so is one row where the oracle is simply
wrong: Perl finds no `\b{lb}` anywhere in a one-character subject, though
`.\b{lb}` matches at that very position. Excluding a row for a reason that
is written down and checkable is not the same as excluding whatever failed,
and the difference is the whole value of the file.

## 10.1 The limits report

`make check-limits` runs `tools/limits/measure.py`, which is the other half
of §7's method: for 251 distinct patterns - every `.rxt` vector plus
`tools/limits/real_world.txt` - it asks each limit for the smallest value at
which the pattern still compiles, and prints the distribution against the
defaults.

Asking the limit rather than counting inside the library is deliberate. The
alternative is a counter in every phase and an accessor for each, which is a
second way of computing every number and a second chance of being wrong
about it; the limits are already enforced in one place each, so asking them
asks the thing that will do the refusing. The cost is a binary search per
pattern per limit, which for this corpus is a few seconds.

The report is a report. What has to keep being true is in
`tests/unit/test_limits.cpp`: not "the headroom is 7×" but "nothing in the
corpus is refused", which is the claim a future default would break.

## 11. Random patterns

`tools/oracle/gen_random.py`: a grammar-driven generator per dialect that
emits patterns of bounded size over a small alphabet, and subjects over the
same alphabet, runs them through the oracle, and writes vectors. Run
nightly for each dialect with an available oracle; the failures are
minimised (the generator can shrink) and added to the committed vectors.
This is what finds the semantic corners the hand-written cases did not
think of, and it is cheap once the drivers exist.

## 12. Coverage and the unit tests

`make coverage` per module, read for branches without a test. The
per-module floor at each milestone is 90% line coverage for `src/parse`,
`src/ir`, `src/compile`, `src/exec` and `src/charclass`, and 100% of the
generated tables' *accessors* (the tables themselves are data).

**Where it stands, measured rather than asserted.** Three of the five are
over the floor - `src/parse` 91.4%, `src/compile` 92.1%, `src/charclass`
94.6% - and two are not: `src/exec` at 87.9% and `src/ir` at 86.6%, with the
project at 90.8%.

The two that miss it are the two that carry code for dialects that do not
exist yet, and the shortfall is counted rather than waved at: of 133
unexecuted lines in `src/exec`, 58 are switch arms and branches keyed on a
construct no built dialect produces - `\A`, `\z`, `\Z`, `\G`, atomic
groups, POSIX's break-on-empty-iteration, a subject that is a byte string
rather than text. In `src/ir` it is 37 of 149. The rest is mostly defensive
`GRX_ERR_INTERNAL` returns for states the callers make unreachable, which
need fault injection rather than a pattern.

That is an explanation and not an excuse: the floor is a floor, and two
modules are under it. It is written down here so that the number is argued
with rather than quietly restated, and so that WP-18 and WP-19 - which build
exactly the constructs those arms are for - can be expected to close most of
it without a single new test. The unit
tests remain one file per module, as scaffolded, and test the module's own
contract - the class algebra against a bitmap model, the arena's growth,
the diagnostics' offsets - while the conformance runner tests the library's
behaviour. A unit test that could be a vector is a vector.

## 13. The benchmark

`make bench` times this library against glibc and musl on one workload,
compiled three ways from `tools/bench/regex_bench.c` so that the loop being
timed is the same source on every side. Compilation is outside the clock,
every side is asked for the same capture slots, and the figure is a
best-of-seven minimum.

It exists because of a number that was wrong in this repository for a day.
"About 7%" for what the POSIX submatch rule costs was whole-process wall
clock over a `grx_match` run: process start, stdin, hex decoding and
compiling a thousand patterns were all in the denominator, so the thing
being measured was a minority of what was timed and the ratio read as a
statement about matching. It is not a rounding error - the real figure on an
ambiguous workload is six times larger.

Three workloads, because one number hides what is worth knowing: patterns
with several ways to divide the same extent, patterns with one way and
groups to fill, and patterns with no capture group at all.

Best of five runs of each, the C sources at `-O0` and at `-O3`. **`-O0` is
no longer what a release build compiles at** - see the note below the table
- so these rows are kept because the `-O0` column is what every earlier
figure in this repository was measured against, and removing it would leave
those figures without a scale:

| | ambiguous | plain | no groups |
| --- | --- | --- | --- |
| ours -O0, `posix-ere` | 5.92 us | 3.23 us | 3.13 us |
| ours -O0, `gnu-ere` | 4.47 us | 3.04 us | 3.18 us |
| ours -O3, `posix-ere` | 2.72 us | 1.46 us | 1.44 us |
| ours -O3, `gnu-ere` | 1.90 us | 1.42 us | 1.47 us |
| glibc 2.41 | 0.88 us | 0.32 us | 0.33 us |
| musl 1.2.6 | 0.50 us | 0.44 us | 0.53 us |

**What POSIX's submatch rule costs**, which is the pair of rows to read
against each other: **+42.7%** where divisions compete, **+2.7%** where
there is one way to match, and **nothing** where there is no group to
divide - 1.44 against 1.47 us is the noise floor, and the rule is switched
off outright for such a pattern. Engine steps, which are deterministic, rise
1.7% on the first workload and not at all on the other two: the rule does
almost no extra work, and what it charges is a per-arrival overhead that the
`contested` analysis in `exec_pike.c` now confines to the instructions where
a second arrival is actually possible. Before that analysis the third column
was **41%** worse rather than level.

**Where this library stands**, which is a separate question and not WP-26's
doing, since the `gnu-ere` row is no faster: **three to four and a half
times glibc, and three to five times musl.** glibc has a DFA to fall back
on and musl is a TNFA with a compiled tag program; this is a thread-set
simulation carrying a capture array per thread, and it pays for that on
every search. Nothing here has been optimised for speed yet, and the gap is
the size one would expect from that.

**The release build moved from `-O0` to `-O2` on 2026-09-23**, as part of a
suite-wide change. `CFLAGS` had carried a literal `-O0` in *both* build
modes since the file was written, and nobody chose it: early repositories in
this suite were written when the production build doubled as the debugging
build, and newer ones copied what already existed. So the release build was
unoptimised and the debug build was correct by accident.

`$(OPT_CFLAGS)` now carries `-O2` for a release build and `-O0` for
`BUILD=debug`, which fixes both halves. What it cost, measured on this
benchmark over regex's own code:

| workload | dialect | `-O0` | `-O2` | ratio |
| --- | --- | --- | --- | --- |
| ambiguous | `posix-ere` | 5.931 us | 2.487 us | 2.38x |
| ambiguous | `gnu-ere` | 4.462 us | 1.761 us | 2.53x |
| plain | `posix-ere` | 3.198 us | 1.370 us | 2.33x |
| plain | `gnu-ere` | 3.042 us | 1.266 us | 2.40x |
| no groups | `posix-ere` | 3.156 us | 1.359 us | 2.32x |
| no groups | `gnu-ere` | 3.155 us | 1.350 us | 2.34x |

Median **2.38x**, three runs each side, minimum of three, on a quiet
machine. Confirmed a second way, because a benchmark on a shared machine is
not trustworthy on its own: five *interleaved* rounds of both builds, with
glibc's binary run in each round as a load witness, gave 2.19x, 2.34x and
2.40x on the three workloads. The witness is the check that makes that
usable - glibc's code is identical in both legs, so its own timing says
whether the two legs met the same machine. Its minima across the legs
differed by 4.5%, which is what "equally loaded" looks like; its *absolute*
figure over the same period ranged from 0.86 to 2.18 us, which is what the
machine was doing while nothing in the output mentioned it.

Two methods, 2.19x-2.53x, and no reading outside that band.

**The figure measures this library and nothing else**, which is worth
checking rather than assuming: regex links cutil, and a benchmark whose hot
path ran through a dependency would be reporting that dependency's
optimisation level. Counted with an `LD_PRELOAD` interposer over a full run:
16 `malloc`, 63-78 `calloc`, 96-138 `realloc` - a few hundred allocations
across tens of thousands of searches, all of them in setup. The timed loop
allocates nothing, so cutil is not in it at all. An allocation count settles
in one run what reading the call graph does not; image nearly reported
*compress's* optimisation level as its own, because deflate sits inside its
PNG encode path.

Every absolute figure elsewhere in this repository that predates that date
is an `-O0` figure. Two comparisons that are easy to substitute for each
other and are not the same: **`-O0` to `-O2` is 2.38x**, and **`-O0` to
`-O3` is about 2.2x**. `-O3` over `-O2` has not been measured here and the
suite's policy requires a recorded figure before moving to it.

Build into a directory of its own when changing flags:

```
make bench BUILD=o3 EXTRA_CFLAGS=-O3 CUTIL_PC=ghoti.io-cutil-0
```

And do not benchmark a loaded machine - or if you cannot have an idle one,
interleave the legs and carry a witness. The `-O2` rows above were first
measured while `make test-asan` was compiling in another process: the same
build reported 5.609 and then 3.526 us on a workload where a quiet machine
gives 2.487. **Nothing in the output says the machine was busy.**

The witness is the cheap fix, and it is one extra binary per round: run a
reference implementation whose code did not change, and read *its* number.
glibc moving from 0.86 to 2.18 us between runs is not a finding about glibc;
it is the machine telling you that everything measured alongside it is
inflated by about the same factor. Absolute figures from such a run are
worthless and interleaved *ratios* survive, because drift lands on both legs
instead of on one.

`BUILD`, because make does not track the flags an object was built with, so
`-O3` objects left in the release tree are invisible to a later `make test`
and would be linked into it silently. `CUTIL_PC`, because a non-default
`BUILD` renames the `.pc` file the Makefile looks for.
