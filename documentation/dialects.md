# The dialect specification

**Status:** design. This page is the authority the parser and the engines
are written against, per [design.md](design.md) §4. Two kinds of cell appear
in its tables: a value with a citation, which is settled; and **probe**,
which means the reference document does not say or says something the
implementation is known not to do, and the value will be filled in by
running the real implementation ([testing.md](testing.md) §5, work package
WP-03 in [plan.md](plan.md)). No code is written against a **probe** cell.
The feature rows in [`src/syntax/syntax.c`](../src/syntax/syntax.c) remain
provisional until §3 below replaces them.

## 1. Tiers

Sixteen dialects are named. They are not equal in demand, in difficulty, or
in how well they can be checked, and they are delivered in this order:

| Tier | Dialects | Why this tier |
| --- | --- | --- |
| 1 | **ECMAScript**, then **PCRE2**, **Perl**, **POSIX BRE/ERE** and **GNU BRE/ERE** | ECMAScript is the first consumer's dialect ([design.md](design.md) §1.1). PCRE2 and Perl are the widest feature set, so building them builds the engine. POSIX is the other match semantics (leftmost-longest) and the other lexical family (escaped operators), so after tier 1 the architecture has met every kind of variation it will meet. Every tier-1 dialect has an oracle on this machine. |
| 2 | Python, Java, .NET, Ruby | Perl-family syntax with their own profiles; mostly profile rows and small hooks once tier 1 exists. Python has an oracle here; the others need one installed. |
| 3 | RE2 (Go), Rust | Deliberate subsets of tier 1. Almost entirely profile work; the value is that a pattern accepted under `GRX_SYNTAX_RE2` is one the linear engine is guaranteed to run. |
| 4 | Tcl, Vim, Emacs | Lexically furthest from the others (Tcl's directors, Vim's magic levels, Emacs's syntax classes), each needing hooks of its own. Vim has an oracle here. |

A dialect is *listed* in `GRX_Syntax` from the start so that the enum is
stable; until its tier ships, selecting it is `GRX_ERR_UNSUPPORTED` with the
diagnostic `GRX_DIAG_DIALECT_NOT_IMPLEMENTED`, never a silent fallback to
another dialect.

## 2. References and oracles

Each dialect is pinned to one version of the implementation it names. The
version is the one that generates its conformance vectors, and the reference
document is the one for that version. "Available" is what is on the machine
this design was written on; a CI job installs the rest
([testing.md](testing.md) §2).

| Dialect | Name | Pinned to | Reference | Oracle |
| --- | --- | --- | --- | --- |
| POSIX BRE | `posix-bre` | IEEE Std 1003.1-2024 | XBD chapter 9.3 | glibc 2.41 `regcomp()` without `REG_EXTENDED` (available); Spencer's test suite |
| POSIX ERE | `posix-ere` | IEEE Std 1003.1-2024 | XBD chapter 9.4 | glibc `regcomp(REG_EXTENDED)` (available) |
| GNU BRE | `gnu-bre` | GNU grep 3.11 / sed 4.9 | GNU grep manual, "Regular Expressions"; glibc manual, "GNU Regular Expression Compiling" | `grep -G`, `sed` (available) |
| GNU ERE | `gnu-ere` | GNU grep 3.11 / sed 4.9 | same | `grep -E`, `sed -E` (available) |
| Perl | `perl` | Perl 5.40.1 | `perlre`, `perlrebackslash`, `perlrecharclass` for 5.40 | `perl` (available); `t/re/re_tests` |
| PCRE2 | `pcre` | PCRE2 10.46 | `pcre2pattern(3)`, `pcre2syntax(3)` for 10.46 | `pcre2test` 10.46 (available); `testdata/testinput1`, `testinput2` |
| ECMAScript | `ecmascript` | ECMA-262 16th edition (ES2025) | clause 22.2 *RegExp (Regular Expression) Objects*; Annex B.1.2 *Regular Expressions Patterns* | Node 22.23 / V8 12.4, Unicode 17.0 (available); test262 |
| Python | `python` | CPython 3.13.5 | `re` module documentation, 3.13 | `python3` (available), **in-process**; no corpus - see below |
| Java | `java` | JDK 21 | `java.util.regex.Pattern` javadoc, 21 | OpenJDK (install) |
| .NET | `dotnet` | .NET 8 | "Regular Expression Language - Quick Reference"; "Regular expression options" | .NET SDK (install) |
| Ruby | `ruby` | Ruby 3.3 / Onigmo 6.2 | Onigmo `doc/RE`; Ruby `Regexp` documentation | `ruby` (install) |
| RE2 | `re2` | Go 1.22 `regexp` | RE2 "Syntax" wiki; Go `regexp/syntax` documentation | `go` (install) |
| Rust | `rust` | `regex` 1.10 | `regex-syntax` documentation | `cargo` (install) |
| Tcl | `tcl` | Tcl 8.6 | `re_syntax(n)` | `tclsh` (install) |
| Vim | `vim` | Vim 9.1 | `:help pattern` | `vim -es` with `matchlist()` (available) |
| Emacs | `emacs` | GNU Emacs 29 | Elisp Reference Manual, "Regular Expressions" | `emacs --batch` (install) |

### 2.1 Python's oracle is the only one that is not a subprocess

Every other reference here is driven by spawning it: `pcre2test`, `perl`,
`node`, a `grep`, a small C program linked against glibc or musl. CPython's
`re` is importable by the Python program that generates the cases, so a row
costs a function call rather than a fork. `tools/oracle/python_diff.py` runs
600,000 rows in under four seconds; the subprocess differentials manage tens
of thousands in the same time.

That is not a footnote about speed. Every defect WP-30 found after the first
build came out of scaling the run up, and two of them were in code this
dialect does not own - the prescan lost its group count after a class
containing an escape, and `GRX_LOOKBEHIND_FIXED` was in the profile and read
by nothing. Both had been reachable by the Perl-family differentials for as
long as they had existed.

**There is no Python corpus.** The plan named `Lib/test/re_tests.py`, which
CPython removed and which Debian's `python3.13` does not ship in any case -
`/usr/lib/python3.13/test/` holds `libregrtest` and nothing else. The
generator is therefore the whole of the gate for this dialect, which is the
arrangement `posix_diff.py`'s note recommends anyway.

## 3. Features: which constructs exist

`GRX_Feature` says whether a dialect *has* a construct. The scaffold's 25
bits are extended to cover what tiers 1 and 2 need; the values below for
tier 1 are from the reference documents, with the dialect's own spelling
where it is not the common one. Tier 2-4 rows are corrected where the
scaffold's provisional table is known to be wrong and otherwise stay
provisional until their tier.

New bits, in addition to the scaffold's:

| Bit | Construct |
| --- | --- |
| `BRANCH_RESET` | `(?\|...)` |
| `KEEP` | `\K` |
| `STRING_CLASS` | class items that are strings: `\q{...}` and properties of strings |
| `DUPLICATE_NAMES` | the same group name in more than one alternative |
| `ANCHOR_G` | `\G` |
| `NEWLINE_R` | `\R` (any Unicode newline sequence) |
| `GRAPHEME_X` | `\X` |
| `NOT_NEWLINE_N` | `\N` as "not a newline" |
| `HV_SPACE` | `\h`, `\H`, `\v`, `\V` |
| `UNICODE_ESCAPE` | `\u{...}` or `\uHHHH` |
| `NAMED_CHAR` | `\N{name}` |
| `SCOPED_FLAGS` | `(?i:...)` scoped, as distinct from bare `(?i)` |
| `EMPTY_CLASS` | `[]` is an empty class rather than the start of a class containing `]` |
| `NEGATED_EMPTY_CLASS` | `[^]` matches any code point |

**Tier 1, from the references:**

| Feature | POSIX BRE | POSIX ERE | GNU BRE | GNU ERE | Perl 5.40 | PCRE2 10.46 | ECMAScript 2025 |
| --- | --- | --- | --- | --- | --- | --- | --- |
| ALTERNATION | - | `\|` | `\\|` | `\|` | `\|` | `\|` | `\|` |
| BOUNDED_REPEAT | `\{m,n\}` | `{m,n}` | `\{m,n\}` | `{m,n}` | yes, and `{,n}` | yes, and `{,n}` | yes; `{,n}` is literal (legacy) or error (`u`) |
| NON_GREEDY | - | - | - | - | yes | yes | yes |
| POSSESSIVE | - | - | - | - | yes | yes | - |
| NON_CAPTURING | - | - | - | - | yes | yes | yes |
| NAMED_CAPTURE | - | - | - | - | `(?<n>)` `(?'n')` `(?P<n>)` | same three | `(?<n>)` |
| BACKREFERENCE | `\1`-`\9` | - (undefined; glibc accepts) | `\1`-`\9` | `\1`-`\9` (GNU extension) | yes, and `\g{-1}` relative | yes | yes |
| LOOKAHEAD | - | - | - | - | yes | yes | yes |
| NON_ATOMIC_LOOKAROUND | - | - | - | - | **-** (probed: perl 5.40.1 answers "Unknown '(*...)' construct 'napla'", and "Sequence (?*...) not recognized") | `(*napla:`, `(*naplb:`, `(?*`, `(?<*` | - |
| LOOKBEHIND | - | - | - | - | yes (§5.4) | yes | yes |
| ATOMIC_GROUP | - | - | - | - | yes | yes | - |
| CONDITIONAL | - | - | - | - | yes | yes | - |
| RECURSION / SUBROUTINE | - | - | - | - | `(?R)`, `(?1)`, `(?&name)`, `(?P>name)` | same, and `\g<1>`, `\g'name'` (probed: perl answers "Unterminated \g... pattern" for those two) | - |
| INLINE_FLAGS / SCOPED_FLAGS | - | - | - | - | both | both | scoped only (ES2025), and not in Node 22 (probed: `(?i:X)` is "Invalid group"), so refused here |
| COMMENT_GROUP | - | - | - | - | yes | yes | - |
| POSIX_CLASS | yes | yes | yes | yes | yes (in brackets) | yes | - |
| UNICODE_PROPERTY | - | - | - | - | yes | yes | `u`/`v` only |
| CLASS_SET_OPS | - | - | - | - | `(?[ ])` extended classes | same, less what each ignores | `v` only |
| WORD_BOUNDARY | - | - | `\b \B \< \>` | same | `\b \B`, `\b{wb}` | `\b \B` | `\b \B` |
| ANCHOR_ESCAPES | - | - | `` \` `` `\'` | same | `\A \z \Z` | `\A \z \Z` | - |
| ANCHOR_G | - | - | - | - | yes | yes | - |
| QUOTING | - | - | - | - | `\Q..\E` | `\Q..\E` | - |
| HEX_ESCAPE | - | - | - | - | `\xHH \x{...}` | same | `\xHH`; `\u{...}` under `u` |
| OCTAL_ESCAPE | - | - | - | - | `\0oo \o{...}` | `\0oo \o{...}` | legacy only (Annex B) |
| CONTROL_ESCAPE | - | - | - | - | `\cX` | `\cX` | `\cX` |
| BACKTRACK_CONTROL | - | - | - | - | `(*PRUNE)` etc. | yes | - |
| BRANCH_RESET | - | - | - | - | yes | yes | - |
| KEEP | - | - | - | - | yes | yes | - |
| DUPLICATE_NAMES | - | - | - | - | yes, with no pragma and no warning (probed: perl 5.40.1) | `(?J)` | ES2025, and not in Node 22 (probed), so refused here |
| NEWLINE_R, GRAPHEME_X, NOT_NEWLINE_N, HV_SPACE | - | - | - | - | all | all | - |
| NAMED_CHAR | - | - | - | - | `\N{U+..}`, `\N{name}` | `\N{U+..}` only | - |
| EMPTY_CLASS / NEGATED_EMPTY_CLASS | - | - | - | - | - | - | both |

Corrections to the scaffold's provisional rows for later tiers, from the
references and worth recording now so nobody builds on the wrong row:

- **Emacs** has no `\d`; digits are `[[:digit:]]` or `[0-9]`. It has no
  lookahead or lookbehind at all.
- **Python** 3.11 added possessive quantifiers and atomic groups; the
  scaffold row is right for 3.13.
- **Python** has no POSIX classes and no `\p{}`; `[[:alpha:]]` is a class
  containing `[`, `:`, `a`, `l`, `p`, `h` and a stray `]`, with a
  `FutureWarning`.
- **Java** has no `[[:alpha:]]`; it spells the same thing `\p{Alpha}`.
- **.NET** class subtraction is `[a-z-[aeiou]]`, its own syntax, not
  `&&` or `--`.
- **Ruby**'s `m` flag is dot-all, and `^`/`$` are always line anchors.
- **RE2** and **Go** accept `(?P<n>)` and, since Go 1.22, `(?<n>)`.
- **Rust** has `&&`, `--`, `~~` and nested classes; it has no lookaround
  and no backreferences, like RE2.
- **Vim** has lookaround as `\@=`, `\@!`, `\@<=`, `\@<!`, atomic as `\@>`,
  and non-greedy as `\{-}`; none is spelled the Perl way.

## 4. What an absent construct does

Three answers, and the parser must give the one the dialect gives:

1. **A literal**, when the dialect assigns the characters no other meaning.
   `+` at the start of a POSIX BRE, `{` in ECMAScript without `u` where
   it does not begin a valid quantifier, `\p` in ECMAScript without `u`
   (an identity escape for `p`).
2. **`GRX_ERR_SYNTAX`**, when the dialect rejects it. `(?>` in ECMAScript,
   `\z` in ECMAScript under `u`, `(?i)` bare in ECMAScript, `\1` in RE2.
3. **`GRX_ERR_UNSUPPORTED`**, only when the dialect has it and this library
   does not yet. Every such case is listed in §6 with the diagnostic it
   returns, so that "unsupported" is a finite, published list and not a
   catch-all.

The rule for deciding between 1 and 2 when a reference is silent: what the
oracle does. The probe suite runs every ambiguous spelling through the
oracle and records which of the three it is.

## 5. The semantic profile: what the constructs mean

Each subsection is one axis of `GRX_SyntaxSpec`. The engines implement the
*values*; the profile says which value each dialect takes. Cells marked
**probe** are filled by WP-03 before code depends on them.

### 5.1 Match preference

| Value | Meaning |
| --- | --- |
| `LEFTMOST_FIRST` | the first match in backtracking priority order: alternatives left to right, greedy quantifiers longest first, lazy ones shortest first |
| `LEFTMOST_LONGEST` | among matches starting at the leftmost position, the longest |
| `TCL_ARE` | Tcl's rule: leftmost, then the whole RE is greedy or non-greedy according to its first quantifier, and length is preferred accordingly (`re_syntax(n)`, "Matching") |

POSIX BRE/ERE, GNU BRE/ERE: `LEFTMOST_LONGEST`. Tcl: `TCL_ARE`. Every
other dialect: `LEFTMOST_FIRST`.

**What each engine does with it** (plan.md's WP-24). The Pike VM runs every
thread to the end instead of cutting the lower-priority ones at the first
match, and keeps the match with the leftmost start and the greatest end -
still linear, because the thread list is still bounded by the program. The
backtracker cannot do that: it has no thread list, so it *searches past*
each match, reporting failure from `MATCH` on purpose so that the search it
would have stopped carries on. That is exhaustive and exponential, and it is
bounded by `max_steps` like everything else here, with one short-circuit
that matters more than it looks - a match reaching the end of the subject
ends the search, because nothing can be longer than everything.

Only a program that needs backtracking takes the second path, which under
these dialects means one with a backreference; everything else is regular
and goes to the Pike VM. The bit-state engine cannot do it at all - its
bitmap records that a state failed, and this mode reports failure from a
match - so naming `GRX_ENGINE_BITSTATE` for one of these dialects is
`GRX_ERR_UNSUPPORTED` rather than a quiet answer from the wrong rule.
`GRX_ENGINE_AUTO` never selects it there.

**Which spans the *groups* get** is a second question, and the preference
above does not answer it. POSIX specifies not only the longest overall match
but, among the ways of matching that same extent, how the subexpressions
divide it. POSIX.1 §9.4.8 requires each subpattern, taken from left to
right, to match the longest string it can while the whole match stays the
longest one at the leftmost start. `(|a)(a|)` against "a" is the
smallest case - either group can have the `a` and the match ends at 1 either
way.

The four leftmost-longest dialects do **not** agree about this, so it is an
axis of its own:

| Value | Meaning |
| --- | --- |
| `FIRST_PATH` | whichever division the engine reached first, which is its own alternation order |
| `POSIX` | the division POSIX's rule asks for, compared rather than stumbled upon |

POSIX BRE/ERE: `POSIX`. GNU BRE/ERE: `FIRST_PATH`, because §2 makes glibc
their definition and glibc does not implement the standard's rule here.
Every leftmost-first dialect is `FIRST_PATH` by construction: there the
first path *is* the answer, so there is no second candidate to compare.

**What `POSIX` compares.** Each group's *end*, in group-number order -
which is POSIX's own order, since groups are numbered by their opening
parenthesis, so an enclosing group comes before the groups inside it and a
left one before a right one. The first group where two candidates differ
decides, and the later end wins.

Ends only, and that is a correction rather than a shortcut. Preferring an
earlier *start* looks like preferring a longer group and is really
preferring a shorter something to its left - and that something may have no
group around it, and so nothing here to be compared. `[ab]a*(a|)` against
"aab" is the case: `a*` is the leftmost subexpression and takes the second
"a", so group 1 gets the empty match at 2, and comparing starts would hand
it 1-2 instead by shortening an `a*` that has no tag to defend itself with.
A group's start belongs to whatever precedes it, which the engines settle by
their own order; the length is what this decides. A group only one candidate
entered is skipped rather than preferred either way, or an iteration that
lost reports its spans: `a(b+|((c)*))+d` against "abd" leaves group 2 unset.

**What `FIRST_PATH` does instead**, and it is not simply "whatever
happens": lowering makes an empty alternative yield to the branch written
beside it, which is the one part of POSIX's rule glibc does follow. One
position, not a sort - `(|b|a)` behaves as `(b||a)` - and it is about what a
branch generates rather than what it can match, so `(a{0}|a)` counts and
`(b*|a)` and `(()|a)` do not. Read off glibc and musl over 7,360 generated
rows, where it reproduced glibc exactly and never contradicted a case musl
agreed with.

**Both engines implement `POSIX`**, which is what keeps them
interchangeable: the Pike VM compares two arrivals at one program counter
and keeps the better, walking on from it again so the improvement reaches
everything downstream; the backtracker compares two finished candidates of
the same extent. The one place the backtracker does not is a program only it
can run - a backreference - where the comparison costs the short-circuit
that stops `\(a*\)*\1` walking an exponential tree, and no reference can
decide the answer anyway because musl refuses a basic RE with a
backreference outright. §6 carries that as a deviation.

### 5.2 Newlines and `.`

| Dialect | Newline set for `.`, `^`, `$` | `.` excludes | Dot-all spelling |
| --- | --- | --- | --- |
| POSIX, GNU | `\n` only with `REG_NEWLINE`; otherwise none, and `.` matches `\n` | `\n` under `REG_NEWLINE` | n/a |
| Perl, PCRE2 | `\n` (PCRE2: build default; `(*CR)`, `(*CRLF)`, `(*ANYCRLF)`, `(*ANY)` and `(*NUL)` change it per pattern, and all six are built - see below) | the newline set | `s` |
| ECMAScript | LF, CR, U+2028, U+2029 | all four | `s` |
| Python | `\n` | `\n` | `s` (`DOTALL`) |
| Java | `\n`, `\r`, `\r\n`, U+0085, U+2028, U+2029; `\n` alone under `UNIX_LINES` | all | `s` |
| .NET | `\n` | `\n` | `s` |
| Ruby | `\n` | `\n` | **`m`** |
| RE2, Rust | `\n` | `\n` | `s` |
| Tcl | `\n` | `\n` under `(?n)`/`(?p)` only | default is dot-all; `(?n)` turns it off |
| Vim | line-based: the subject is a line; `\n` matches a line break only via `\n`/`\_` forms | end of line | `\_.` |
| Emacs | `\n` | `\n` | none (`[^z-a]` idiom) |

**PCRE2's newline conventions**, measured against pcre2test 10.46 and
checked by `make check-oracle-newlines` over 360,360 rows:

| Convention | `.` and `\N` refuse | Line terminator |
| --- | --- | --- |
| `(*CR)` | CR | CR |
| `(*LF)` | LF - the default, here and there | LF |
| `(*CRLF)` | **nothing** | the two-character CR LF |
| `(*ANYCRLF)` | CR, LF | CR, LF, or the pair |
| `(*ANY)` | LF VT FF CR NEL LS PS | those, and the pair |
| `(*NUL)` | NUL | NUL |

`(*CRLF)`'s empty column is the case that shapes the implementation: a
convention whose terminator is two characters long cannot be a set of code
points, so the pair travels as a flag beside the set. Four consequences,
each measured rather than derived:

- **`^` does not hold between the CR and the LF, and `$` does.** pcre2test
  is asymmetric here and is followed as measured: `(*ANY)^\n` does not
  match "a\r\n" while `(*ANY)\r$` does, because under `(*ANY)` a lone LF
  ends a line and so `$` has a reason that does not involve the pair.
- **`.` refuses the place a terminator *begins*.** Under `(*CRLF)` that is
  the CR of a pair and not the LF: `a..b` refuses "a\r\nb" and `\r.b`
  accepts "\r\nb". Dot-all lifts it; a negated class never had it.
- **`$` and `\Z` outside multiline** take the pair as the final terminator:
  `(*CRLF)abc$` matches "abc\r\n" and not "abc\n".
- **An unanchored search does not begin between the two.** pcre2api states
  it and calls it a compromise: an attempt that failed at a CR LF resumes
  after the LF, *unless the pattern contains an explicit match for CR or
  LF*. Its own example is `.+A`, which does not match "\r\nA" where
  `[\r\n]A` does. "Explicit" is narrower than "can match" and the
  difference is only in the spelling - `[\x0a-\x0f]` names LF and
  `[\x09-\x0f]` does not, and `\s` contains both CR and LF and names
  neither.

### 5.3 `^` and `$`

| Dialect | `$` without multiline | `^`/`$` multiline by default | `\Z`, `\z` |
| --- | --- | --- | --- |
| POSIX, GNU | end of string only | no (`REG_NEWLINE` makes them line anchors) | GNU: `` \` `` `\'` for buffer edges |
| Perl, PCRE2, Python, Java, .NET | end, **or before a final newline** | no | `\Z` before final newline, `\z` end only (Python: `\Z` is end only) |
| ECMAScript | end only | no | none |
| Ruby | **always** a line anchor | **yes** | `\Z`, `\z` as Perl |
| RE2, Rust | end only | no | `\z` (Rust: `\z`; Go: `\z`) |
| Tcl | end only | `(?n)`/`(?w)` | `\Z` |
| Vim, Emacs | end of line (the subject is a line) | n/a | Vim `\_$`; Emacs `` \` `` `\'` |

### 5.4 Lookbehind constraint

| Value | Meaning | Dialects |
| --- | --- | --- |
| `NONE` | no lookbehind | POSIX, GNU, RE2, Rust, Emacs |
| `FIXED` | every alternative the same fixed length | Python |
| `FIXED_PER_ALTERNATIVE` | each alternative fixed, alternatives may differ | Perl before 5.30; Ruby; Tcl (**probe**) |
| `BOUNDED` | finite maximum length | Perl 5.30+ (experimental, 255), PCRE2 10.43+ (`max_varlookbehind`, default 255), Java |
| `UNBOUNDED` | any length; matched right to left | ECMAScript, .NET, Vim |

This axis does two things, and the second was added when the first turned out
not to be enough. It is still the *parse-time* restriction that makes the
library reject what the reference would reject — `max_lookbehind_length` caps
the bounded forms on top of it — and it now also chooses **which of the two
lookbehind models** runs the body ([design.md](design.md) §3.5.2):

- `UNBOUNDED` runs the body backwards from the current position, which costs
  what the body costs however far back it reaches. ECMAScript, .NET.
- `BOUNDED` matches the body forwards from each start its length allows,
  furthest back first, and requires it to arrive exactly where the assertion
  stands. Perl, PCRE2. Affordable *because* the dialect bounds the variation:
  the candidate starts number `max - min + 1`, which that bound caps at 256.

The two are not interchangeable. Which candidate wins, what a capture inside
a variable-length body holds, and whether `(*ACCEPT)` has anywhere to stop
all follow from the model, and Perl and PCRE2 answer each of them the way
their model does — seven records of the corpus turn on it. `FIXED` and
`FIXED_PER_ALTERNATIVE` are not distinguished here because a body of one
length has one candidate start: both models run the same body between the
same two positions, so there is nothing for the axis to choose between. A
non-atomic lookbehind keeps the reverse model in every dialect, because it is
inlined rather than run as a sub-match — that is what makes it non-atomic —
and a candidate-start loop has nowhere to put the backtrack points it has to
leave live.

### 5.5 Empty iterations and captures in loops

The two rules that make `(a*)*` against `b` report different things:

| Axis | Value | Dialects |
| --- | --- | --- |
| Empty iteration | `FAIL_IF_EMPTY_AFTER_MIN`: an iteration that consumes nothing, once `min` is satisfied, fails (22.2.2.3.1 RepeatMatcher step 2.b) | ECMAScript |
| | `BREAK_ON_EMPTY`: the iteration succeeds and the loop stops | Perl, PCRE2, Python, Java (**probe**), .NET (**probe**), Ruby (**probe**), RE2, Rust |
| | `BREAK_IF_UNMOVED`: the iteration succeeds and the loop stops, but only while the *repeat* has consumed nothing; once it has, the empty iteration does not run at all | POSIX, GNU |
| | `LONGEST`: irrelevant; the match is the longest, and an empty iteration adds nothing | Tcl |
| Capture reset | `RESET_EACH_ITERATION`: captures inside the group are cleared at the **start** of every iteration (RepeatMatcher step 4) | ECMAScript |
| | `RESET_AFTER_EACH_ITERATION`: an iteration clears, on the way **out**, the ones it did not itself set | **Perl** (probed) |
| | `KEEP_LAST_SET`: a capture set in an earlier iteration survives if a later one does not set it | PCRE2, Python, Java (**probe**), .NET, Ruby (**probe**), RE2 (**probe**), Rust (**probe**) |

`BREAK_IF_UNMOVED` is neither of the two above it, and it took both
references to see that. `(a*)*` against `"b"` reports group 1 as 0-0 in glibc
and in musl, so an empty iteration *does* run when nothing else has - the
subexpression takes part rather than going unset, which `FAIL_IF_EMPTY` would
give. And `(a|)*` against `"aaaa"` reports group 1 as 3-4 in both, not 4-4,
so once an iteration has consumed, a trailing empty one does *not* run -
which is what plain `BREAK_ON_EMPTY` gives, the empty body having written its
captures before the loop exited. Both halves are the same rule seen from two
sides, and the thing that divides them is where the repeat began.

"The repeat", not "its optional tail", is the whole of the distinction.
`(b+|(c)*)+` against `"b"` has one mandatory copy, which consumes the `b`; if
the tail counted as the loop then its first iteration would be the loop's
first and would take the empty alternative, reporting group 1 as 1-1 where
both references say 0-1. So the position is taken before the mandatory copies
run.

This is what the ten `known-gaps.txt` rows filed under "POSIX subexpression
disambiguation" actually were. They were read as needing WP-26's
tagged-transition machinery - Okui-Suzuki or Laurikari - and they needed an
empty-iteration rule instead. Every one of them passes now, and WP-26 is
still unbuilt.

The first two report the same spans. After the last iteration, a capture it
did not set is gone either way, which is why one value stood for both until
something asked the other question: what the *next* iteration can see.
`((?(2)x|y)(a))+` against `yaxa` matches the whole subject in perl and in
pcre2test, and it can only do that if the second iteration's conditional
still sees what the first captured — clearing early hides group 2, takes the
`y` branch, and stops after `ya`. The clearing is real all the same:
`((a)|b)+(?(2)x|y)` against `abx` does not match in perl, because by the time
the conditional outside the loop runs, the iteration that took `b` has taken
group 2 away.

Only a conditional or a backreference can tell the two apart, so codegen
emits the late form only for a pattern that has one — which is also why it is
free: anything that reads a capture back already keeps a program off the
memoising engines ([design.md](design.md) §3.5.3).

Consequences the tests state:

- `(a*)*` on `b`: ECMAScript group 1 unset; Perl group 1 = `""`. This is the
  **empty-iteration** axis: ECMAScript fails the iteration that consumed
  nothing, so there are none, so nothing was captured. Perl's iteration
  succeeds and the loop stops.
- `((a)\|b)+` on `ab`: ECMAScript group 2 unset; **Perl group 2 unset**;
  PCRE2 and Python group 2 = `"a"`. This is the **capture-reset** axis, and
  it is a different axis: Perl resets and PCRE2 does not.
- `(a*)+` on `b`: group 1 = `""` in both (min = 1 forces the one empty
  iteration).

**Corrected by WP-03's probe.** An earlier version of this page put Perl in
the `KEEP_LAST_SET` row and gave `((a)|b)+` as the example that shows it,
with Perl reporting group 2 as `"a"`. Perl 5.40 reports it as **unset**, and
so does `(?:(a)|b){2}` against `"ab"`. PCRE2 10.46 and Python 3.13 report
`"a"`. The page had attributed the `(a*)*` difference - which is the
empty-iteration axis - to the capture-reset axis as well, and the two are
independent. `tests/data/probe/report.md` has the transcript.

**Perl's rule is not RESET_EACH either.** The row above says Perl resets, on
the strength of `((a)|b)+`. Four more patterns, asked of Perl 5.40, say that
whatever Perl does, "captures inside the atom are cleared at the start of
each iteration" is not it:

| Pattern | Subject | Perl | ECMAScript |
| --- | --- | --- | --- |
| `^(a\1?){4}$` | `aaaaaa` | match, `$1` = `aa` | no match |
| `^(\2?(a)){2}$` | `aaa` | match, `$1` = `aa` | no match |
| `^((a)\|b\2?){2}$` | `aba` | **no match** | no match |
| `^(b\2?\|(a)){2}$` | `aba` | match, `$1` = `ba` | no match |

Row one needs the repeated group's own capture to survive into the next
iteration; row two needs a group *inside* it to survive as well. Row three
needs it not to - and rows three and four differ in nothing but the order of
the alternatives, so what cleared the capture in row three was the branch
that was *entered and failed*. The unrolled form disagrees too:
`(?:(a)\|b)(?:(a)\|b)` against `ab` keeps `$1` = `"a"`, so whatever this is,
it is not simply "a failed attempt clears what it touched".

No single rule this page could write covers all four, and a rule nobody can
state is a rule this library will not implement. The two records in
`tests/data/vectors/known-gaps.txt` that turn on it are categorised there as
`reference-defect` rather than as gaps, and the minimal pair is why: perl
answers `((a){2})+` and `((aa){2})+` differently, so its answer is a fact
about which repeat opcode its compiler picked and not a rule a second engine
could follow.

### 5.6 Backreferences to unset groups; forward and nested references

| Axis | Value | Dialects |
| --- | --- | --- |
| Unset group | `MATCH_EMPTY` | ECMAScript; PCRE2 with `PCRE2_MATCH_UNSET_BACKREF` (exposed as `GRX_OPT_MATCH_UNSET_BACKREF`); Vim (**probe**) |
| | `FAIL` | Perl, PCRE2, Python, Java, .NET, Ruby, GNU, Tcl (**probe**), Emacs (**probe**) |
| Forward reference `\2(a)(b)` | allowed, behaves as unset | ECMAScript, Perl, PCRE2 |
| | syntax error | Python ("invalid group reference"), Java (**probe**), RE2/Rust (no backreferences) |
| Reference to the group it is inside, `(a\1)` | allowed; reads the span the group last **closed** with | Perl, PCRE2, ECMAScript |
| | syntax error | Python |

"Unset on first entry" is the first half of that last row and was for a long
time the only half written down. The second half is what a *later* entry
reads: while a group is between its two SAVEs its slots hold a start from
this iteration and an end from the one before, which is not a span anything
wrote, and both references read the last completed one instead. `^(a\1?){4}$`
against `aaaaaa` matches in perl and in pcre2test — the second iteration's
`\1` is the "a" the first took — and reading the live slots gives the empty
string and no match at all.

The remembered span is not a second life for a capture the dialect has taken
away: a `RESET` clears it with the live one, so `((a)|b)+\2c` against `abac`
does not match in perl and does not here. It only exists for the window
between a group opening and closing, which is why nothing a reference could
already see changed. See `GRX_PROGRAM_SHADOW_CAPTURES`.

### 5.7 Backreference versus octal, and the numeric escapes

| Dialect | `\1`..`\9` | `\10` and up | `\0` | Octal | `\x` |
| --- | --- | --- | --- | --- | --- |
| POSIX, GNU | backreference | `\1` then `0` | literal `0` (probed: glibc 2.41 and musl 1.2.6 both match `\0` against "0" and neither against a NUL) | none | none |
| Perl, PCRE2 | backreference | backreference if that many groups exist, else octal if the digits are octal, else literal | `\0` then up to two octal digits | `\0oo`, `\o{...}` | `\xHH` (0-2 digits), `\x{...}` |
| ECMAScript legacy | backreference if the pattern has that many groups, else legacy octal (up to `\377`), else identity (`\8`, `\9`) | same rule on the whole number, then the prefix | NUL, or legacy octal if followed by a digit | legacy only | `\xHH`; `\x` alone is identity |
| ECMAScript `u`/`v` | backreference; error if no such group | same | NUL; error if followed by a digit | error | `\xHH` (exactly 2) or error; `\u{...}` |
| Python | backreference | backreference if the group exists, else error | NUL | exactly three digits `\0oo` or `\ooo` starting `0`; `\1oo` as octal only with three digits | `\xHH` exactly |
| Java | backreference; longest prefix that is a valid group number | same | `\0` + 1-3 octal digits | `\0` prefix only | `\xHH`, `\x{...}` |
| .NET | backreference | backreference if group exists, else octal, else error | octal | `\0oo`, and `\1oo` when no such group | `\xHH` exactly |
| Ruby | backreference | backreference if group exists, else octal | NUL | `\ooo` | `\xHH`, `\x{...}` |
| RE2, Rust | error | error | NUL (RE2: `\0` is octal `\000`) | `\123` RE2 yes; Rust no | `\xHH`, `\x{...}` |

### 5.8 Case folding

| Value | Meaning | Dialects |
| --- | --- | --- |
| `SIMPLE_FOLD` | Unicode simple case folding; a caseless literal is its fold orbit ([unicode.md](unicode.md) §5) | PCRE2, Python (its `_sre` equivalence table is this), Java with `UNICODE_CASE`, RE2, Rust, Tcl, Ruby (**probe**: Onigmo can do multi-char folds), ECMAScript `u`/`v` |
| `FULL_FOLD` | full folding, including length-changing (`ß` ~ `ss`) | Perl. Built; [design.md](design.md) §5.2 |
| `ES_LEGACY` | ECMA-262 Canonicalize without `u`: simple uppercase mapping, rejected if it is multi-unit or maps non-ASCII to ASCII | ECMAScript without `u` |
| `ASCII_ONLY` | A-Z only | Java without `UNICODE_CASE`; PCRE2 without UTF; POSIX and GNU (deviation: the locale is treated as C); Vim `\c`, Emacs (**probe**) |
| `CULTURE` | .NET's culture-sensitive `ToLower`. **Deviation:** implemented as `SIMPLE_FOLD`, i.e. `CultureInvariant` | .NET |

**Full folding is a property of a run, not of a character.** `ß` folds to
"ss" and the `ﬀ` ligature folds to "ff", so under Perl's `/i` one pattern
character can match two subject characters and two pattern characters can
match one. The two meet in the middle, which is why the run is what is
folded: `sß` and `ßs` both fold to "sss", and Perl matches either against
the other.

It stops at two boundaries. **`/aa` drops a full fold exactly when a code
point of the fold is ASCII** - a fold that crosses the ASCII line is what
that flag exists to prevent, and the same cut it makes in an orbit, where
`s` stops matching `ſ`. So `ß` stops matching "ss" and the `ﬀ` ligature
stops matching "ff", while `U+0390` goes on matching `U+03B9 U+0308 U+0301`
and `U+1FB3` matching `U+03B1 U+03B9`, because neither of those touches
ASCII. That is **87 of the 104**, so it is the majority that survives rather
than the exception.

This page and the code both used to say `/aa` dropped full folding whole, on
the ground that "every code point with a full fold is outside ASCII and every
one of those folds is at least partly inside it". The first half is true and
the second is false; the two were written as one sentence and read as one
fact. It held in both directions - `(?aa)ff` was not matched by the ligature
either - and was found by `tools/corpus/make_fold_vectors.py` on the day it
was written.

And PCRE2 does not have full folding at all, which is one of the reasons
`GRX_SYNTAX_PERL` and `GRX_SYNTAX_PCRE` are separate dialects.

**A character class is not the third boundary, though this page said it was.**
It claimed a class folds simply "and Perl agrees". Perl does not agree:
`"ss" =~ /^(?:[ß])$/iu` matches there. Measured over the 104 `F` lines of
`CaseFolding.txt` - every code point whose full fold is longer than one code
point - bare and in a class, both directions, this library agreed 104 of 104
bare and **0 of 104** in a class. Both are 104 of 104 now, on all three
engines, and `tests/data/vectors/perl/folding.rxt` holds it.

The rule Perl actually has is narrower than "a class folds fully", and every
row below was measured rather than argued:

| In a class | Full-folds | Why |
| --- | --- | --- |
| `[ß]`, `[\x{df}]`, `[\N{U+00DF}]` | **yes** | one code point, written out |
| `[ß-ß]` | **yes** | a degenerate range is a single member |
| `[ßq]`, `[qß]` | **yes** | position among the other members is irrelevant |
| `[^ß]` | no | a negated class does not, in Perl either |
| `[a-ÿ]` | no | a real range does not |
| `[\w]`, `[[:alpha:]]`, `[\p{L}]` | no | nor a shorthand, a POSIX class or a property |

It is **one-directional**, which is what keeps a class a class: `[s]` does not
match `ß` in Perl, though the literal `ss` does. A class stands for one
character, so there is nothing for the second half of a two-character fold to
come from. And it composes like any other branch - `[ß]{2}` matches "ssss".

What it lowers to is an alternation: the class as it already was, or one
branch per distinct multi-code-point fold its single-code-point members have,
each branch a concatenation whose code points match by their simple orbit -
which is why `[ß]` matches "SS" and "sſ" as well as "ss". Two members with one
fold, `ß` and `ẞ`, produce one branch. The bound is a property of Unicode
rather than of the pattern: 104 `F` lines, 73 distinct sequences, and
`GRX_CLASS_FULL_FOLD_MAX` is 128 so that it cannot be reached.

No imported vector reaches any of this, which is why `re_tests` read 2,592 of
2,592 throughout. `tools/corpus/make_fold_vectors.py` is what asks the
question; it generates 1,300 rows from Perl, and found a second defect on the
day it was written, which §6 records.

**Perl's subject is text.** There is no byte mode in Perl: a Perl string is a
sequence of characters, and `/u`, `/a` and `/l` say which *rules* apply to
them rather than whether to decode them. This library's Perl dialect
therefore reads its subject as UTF-8 whatever the flags say, where PCRE2's
reads bytes until `PCRE2_UTF` says otherwise.

### 5.9 `\w`, `\d`, `\s`, `\b`, POSIX classes, property names

| Dialect | `\w` | `\d` | `\s` | Property names |
| --- | --- | --- | --- | --- |
| POSIX, GNU | GNU: `[[:alnum:]_]`, ASCII here | none (GNU: none; `[[:digit:]]`) | GNU: `[[:space:]]` | none |
| Perl | Unicode: `\p{Word}` = `L`, `M`, `N`, `Pc`, join controls | `\p{Nd}` | `\p{White_Space}` | loose (UAX #44), `Is`/`In` prefixes, many synonyms |
| PCRE2 | ASCII; Unicode under `UCP` | ASCII; `Nd` under `UCP` | ASCII `[ \t\n\v\f\r]`; `White_Space` under `UCP` | loose; plus `Xan Xps Xsp Xwd Xuc` |
| ECMAScript | `[A-Za-z0-9_]`; plus U+017F, U+212A under `iu` (22.2.2.9.3 WordCharacters) | `[0-9]` | WhiteSpace ∪ LineTerminator: `\t \v \f \r \n`, U+0020, U+00A0, U+1680, U+2000-200A, U+2028, U+2029, U+202F, U+205F, U+3000, U+FEFF (`\p{Zs}` plus the named ones) | strict, case-sensitive, canonical names and aliases only; `gc`, `sc`, `scx` and the listed binaries |
| Python | Unicode (`str` patterns): alphanumeric per `str.isalnum()` plus `_`; ASCII under `re.ASCII` | Unicode `Nd`; ASCII under `re.ASCII` | Unicode whitespace per `str.isspace()`; ASCII `[ \t\n\r\f\v]` under `re.ASCII` | none |
| Java | ASCII; Unicode under `UNICODE_CHARACTER_CLASS` | ASCII; `Nd` under the flag | `[ \t\n\x0B\f\r]`; `White_Space` under the flag | loose-ish: `\p{IsAlphabetic}`, `\p{Lu}`, `\p{IsGreek}`, `\p{InGreek}` blocks, `\p{javaLowerCase}` (**probe** exact rules) |
| .NET | Unicode: `L`, `Mn`, `Nd`, `Pc`; ASCII under `ECMAScript` option | `Nd`; ASCII under `ECMAScript` | Unicode; `[ \f\n\r\t\v\x85\p{Z}]` | `\p{Lu}` categories and named blocks `\p{IsGreek}` only |
| Ruby | ASCII `[a-zA-Z0-9_]` | ASCII | ASCII `[ \t\r\n\f\v]` | `\p{Word}`, `\p{Alpha}`, scripts, categories; loose case |
| RE2 | ASCII | ASCII | `[\t\n\f\r ]` | `\pL`, `\p{Greek}`: categories and scripts, exact case |
| Rust | Unicode (UTS #18); ASCII under `(?-u)` | `Nd` | `White_Space` | loose (UAX #44) |
| Tcl | Unicode `[[:alnum:]_]` | Unicode `[[:digit:]]` | Unicode `[[:space:]]` | none |
| Vim | `[0-9A-Za-z_]` | `[0-9]` | `[ \t]` | none |
| Emacs | syntax table: word constituents (**deviation:** treated as `[[:word:]]` = Unicode letters and digits) | none | `\s-` (syntax class), not `\s` | none |

**`UCP` widens the shorthands and `UTF` widens the folding**, and they are
two different questions. `(*UTF)\w` does not match "é" in pcre2test and
`(*UTF)(*UCP)\w` does, while `(*UTF)(?i)é` matches "É" with no `UCP`
anywhere. This library read one bit for both until 2026-09-22 - the row
above has said "Unicode under `UCP`" since it was written, and the field
the code read was called `shorthands_utf`, which is the whole of how a
correct page and wrong code coexisted. `\b` moves with `\w`, being defined
from it. Perl is neither case: its subject is a Unicode string and its
shorthands are Unicode with no flag, so `/a` is the interesting direction
there.

POSIX bracket classes (`[:alpha:]` and the other eleven) are ASCII in POSIX
and GNU (C locale), Unicode in Perl, PCRE2 under `UCP`, Ruby, Tcl, Rust,
Vim; RE2 is ASCII. `\b` is defined from `\w` in every dialect; Perl's
`\b{wb}` and its relatives are built. Perl accepts exactly five spellings -
`\b{gcb}`, `\b{g}` (an alias for it), `\b{wb}`, `\b{sb}` and `\b{lb}`, each
with a `\B{...}` negation and optional blanks inside the braces - and they
are **four algorithms across two standards**: the first three are UAX #29,
and `lb` is UAX #14. `\X` is the fifth construct built on the same tables:
one extended grapheme cluster, defined as one character plus every character
that does not begin a new one, so it and `\b{gcb}` are one algorithm.

Three things about them are worth knowing.

**The ends of the subject.** UAX #29 breaks at the start and the end of text;
UAX #14 never breaks at the start (LB2) and always does at the end (LB3). An
*empty* subject has no boundary of any kind, which is neither standard's
wording and is what Perl does - there are no characters, so there is nothing
for a boundary to fall between.

**They are assertions.** Zero width, no capture state, so a program holding
one is still regular and the lockstep engine still runs it. `\X` consumes,
but it lowers to ordinary nodes - one `ANY`, then a greedy loop of
"not-a-boundary and another `ANY`", wrapped atomically because a cluster does
not come apart.

**PCRE2 has none of it.** pcre2test reads `\b{wb}` as a word boundary
followed by four ordinary characters, which is a wrong answer wearing a right
one's clothes, and is why these are Perl's alone here.

**`\p{nv=...}` is Perl's alone too**, for a plainer reason: pcre2test 10.46
and V8 both answer it with "unknown property". Its values are numbers rather
than names and match by arithmetic, so `\p{nv=2/4}` and `\p{nv=0.5}` are
`\p{nv=1/2}`, while `\p{nv=-1/2}` is a different single code point - the
hyphen is a sign, and is the one piece of punctuation loose matching must not
drop. [unicode.md](unicode.md) §6.1 has the rule and where the values come
from.

### 5.10 Iteration after an empty match

| Value | Rule | Dialects |
| --- | --- | --- |
| `RETRY_NONEMPTY_THEN_ADVANCE` | at the same position, retry refusing an empty match; if that fails, advance one character | Perl, PCRE2 (its documented `NOTEMPTY_ATSTART` loop), Python 3.7+ |
| `ADVANCE_ONE` | advance one code point (one code unit without `u`) and search again; an empty match immediately after a non-empty one is reported | ECMAScript (`RegExpBuiltinExec` / `AdvanceStringIndex`), Java (**probe**), .NET (**probe**), Ruby (**probe**) |
| `ADVANCE_ONE_SKIP_ABUTTING` | as above, but an empty match abutting the previous match is not reported | Go (`regexp` documentation: "empty matches abutting a preceding match are ignored"); Rust (**probe**) |

**What `\G` asserts is a second axis**, independent of the rule above, and
Perl and PCRE2 share the first row and differ on this one:

| Value | Rule | Dialects |
| --- | --- | --- |
| `SEARCH_START_ATTEMPT` | where the current attempt began | PCRE2 |
| `SEARCH_START_PREVIOUS_END` | where the previous match ended - `pos()`, which a failed attempt does not move | Perl |

It shows only after a *failure* forces the loop to advance. `\Ga*` against
`"baac"`: both find the empty match at 0, both fail to find a non-empty one
there, and both step to 1. PCRE2's `\G` follows the step and "aa" matches;
perl's does not, and the loop is over. pcre2test 10.46 reports four matches
and `while ("baac" =~ /\Ga*/g)` reports one.

Neither is a quirk of its implementation. PCRE2 does not provide the loop -
its caller writes it and bumps the start offset - so `\G` can only mean the
attempt. Perl does provide it, and `pos()` is a property of the string.
`grx_regex_search_next()` provides it too, which is why this library has to
choose per dialect rather than inherit one answer. Found by
`tools/oracle/iterate_diff.py`, the first thing here to ask either reference
for *every* match rather than for the first; 58,254 cases of `perl_diff.py`
could not reach it, because every one of them asks for one match and stops.

### 5.11 Replacement templates

The template is parsed by a per-dialect grammar into a small sequence
(literal, group by number, group by name, whole match, prefix, suffix,
subject, case operator), then applied; a template is validated at
`grx_regex_replace()` time against the regex's group count and names, with
the dialect's rule for a reference to a group that does not exist.

Two things are settled later than that, and have to be. A *name* may belong
to several groups, and the reference then means the first of them that is
set - so it is resolved when the substitution happens, exactly as a
backreference is (section 5.17). And PCRE2's rule for a group that exists and
did not participate is an error, which is not knowable until there is a match
to ask: `(a)?b` with `$1` substitutes against "ab" and fails against "b".

**And the Python row was wrong in six places until the same generator was
pointed at it** (WP-30). Its twelve hand-written tests all passed while the
generator found that `\u`, `\U`, `\N{...}` and `\x` are *pattern* escapes in
Python and errors in a template - one dialect with two closed alphabets, the
template's being the smaller; that a backslash before a non-alphanumeric
keeps both characters, where sed's rule drops the backslash and Perl's drops
it too; that three octal digits outrank a group reference, so `\101` is "A"
and `\1234` is "S4" while `\12` is group 12; and that `\12` against a
two-group pattern is an error rather than group 1 followed by a literal "2",
because Python does not fall back to a shorter reading the way ECMAScript
does. The row said "other C escapes processed", which was true and was not
the question.

**The PCRE2 row above was wrong in four places until a generator was pointed
at it** (`tools/oracle/replace_diff.py`, testing.md section 8). It said PCRE2
had no whole-match or context forms and it has six; it did not have
`$<name>`; and it did not say that the numbered form takes every digit rather
than falling back to a shorter prefix the way ECMAScript's does. Each of
those was a defect in `src/syntax/syntax.c`'s row as well, because the row
was written from this table. The corpora could not have found them:
`testinput1` and `testinput2` carry patterns and subjects, and no templates
at all.

| Dialect | Group | Named | Whole / prefix / suffix | Escape | Missing group | Unset group | Case ops |
| --- | --- | --- | --- | --- | --- | --- | --- |
| ECMAScript (`String.prototype.replace`) | `$n`, `$nn` (1-99) | `$<name>` (only if the regex has named groups) | `$&`, `` $` ``, `$'` | `$$` | literal `$n` | empty | none |
| PCRE2 (`pcre2_substitute`) | `$n`, `${n}` - every digit, no fallback | `$name`, `${name}`, `$<name>` | `$&`, `$0`, `${0}`, `` $` ``, `$'`, `$_` (the whole subject) | `$$` | error | error, or empty with `SUBSTITUTE_UNSET_EMPTY` (exposed as an option) | extended mode: `\U \L \E \u \l`, and `${n:+a:b}`, `${n:-d}` |
| Perl (interpolation subset) | `$n`, `${n}`, `\n` (deprecated) | `$+{name}` | `$&`, `` $` ``, `$'` | `\$`, `\\` | empty (undef) | empty | `\U \L \E \u \l \Q` |
| Python (`re.sub`) | `\n`, `\nn` (1-99, no fallback) | `\g<name>`, `\g<n>` | `\g<0>` only | `\\`, `\a \b \f \n \r \t \v`, octal; `\` before a non-alphanumeric keeps **both**; every other letter is an error | error | empty | none |
| Java (`appendReplacement`) | `$n` (longest valid prefix) | `${name}` | none | `\` quotes the next character | error | empty (**probe**) | none |
| .NET | `$n`, `${n}` | `${name}` | `$&`, `` $` ``, `$'`, `$+`, `$_` | `$$` | literal | empty | none |
| Ruby (`sub`) | `\n` | `\k<name>` | `\0`, `\&`, `` \` ``, `\'` | `\\` | empty | empty | none |
| Go, Rust | `$n`, `${n}` | `$name`, `${name}` - the name is parsed greedily, so `$1x` is the group named `1x` | none | `$$` | empty | empty | none |
| POSIX BRE/ERE | `\1`-`\9`, one digit | none | `&` | `\&`, `\\`, and `\c` for any other `c` | error | empty | none |
| GNU BRE/ERE | as POSIX, plus `\0` for the whole match | none | `&`, `\0` | as POSIX | error | empty | none - see below |
| Vim (`:s`) | `\n` | none | `&`, `\0` | `\&`, `\\` | empty | empty | `\u \U \l \L \e \E` |
| Tcl (`regsub`) | `\n` | none | `&`, `\0` | `\\`, `\&` | empty | empty | none |
| Emacs (`replace-match`) | `\n` | none | `\&` | `\\` | error | empty | none |

**The POSIX and GNU rows are sed's**, because POSIX's regular expressions say
nothing about substitution and sed's `s` command is what defines one. That
makes sed the oracle here for the same reason glibc is the oracle for the
front end, and with the same division: `posix-bre` and `posix-ere` take only
what POSIX's sed states - `&`, `\&`, `\\` and `\1` to `\9` - and the two GNU
rows add `\0`. `tools/oracle/sed_diff.py` puts both sets to sed on every
`make check-oracles`.

Three details are worth stating because none of them is what a reader coming
from the `$` dialects would guess. A **bare** `&` is the whole match and
`\&` is the literal one, which is the reverse of ECMAScript's `$&`. A group
number is **one digit and never two**, so `\10` is group 1 followed by a `0`
even in a pattern with ten groups - sed answers that way and it is what the
`s` command has always meant. And the escape is **total** rather than a list:
`\q` is a `q`, so a backslash before anything the grammar does not claim is
that character, where ECMAScript's `$q` stays two characters.

**Case operators are not implemented for any dialect.** GNU sed has
`\U \L \l \u \E` and so does PCRE2 under `PCRE2_SUBSTITUTE_EXTENDED`, and
this library implements neither; they are one feature and it is a single
omission rather than two. A template using one gets the letter as a literal
under the GNU rows, because the escape is total.

A template ending in a **trailing backslash** is this library's own decision
rather than an oracle's: sed cannot be asked, since the closing delimiter of
its `s` command is exactly what such a backslash escapes, so the template
never reaches it. Here the backslash is dropped.

### 5.12 Character-class syntax

| Rule | POSIX/GNU | Perl/PCRE2 | ECMAScript | Python | Java | Ruby | RE2/Rust |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `]` first is a literal | yes | yes | no: `[]` is empty, `[^]` is everything | yes | **probe** | yes, with a warning | yes |
| Backslash inside brackets | literal | escape | escape | escape | escape | escape | escape |
| `-` literal at the ends | yes | yes | yes (legacy); `u`: yes; `v`: must be escaped | yes | yes | yes | yes |
| Class escape as a range endpoint, `[\d-z]` | n/a | PCRE2: error; Perl: `-` literal, with a warning | legacy: union; `u`: error | error (probed: Python 3.13 raises) | error | **probe** | error |
| `[[:alpha:]]` | yes | yes | no | no | no | yes | yes |
| Set operations | no | `(?[ ])`: `\|` `+` `&` `-` `^` `!`, nesting to 15 - Perl and PCRE2 alike, differing only in what they ignore (§6) | `v`: `&&`, `--`, nesting, `\q{}` | no | `&&`, nesting | `&&`, nesting | Rust: `&&`, `--`, `~~`, nesting; RE2: no |
| Reserved double punctuators | - | - | `v`: `&&`, `!!`, `##`, ... must be escaped | - | - | - | - |

### 5.13 Quantifier syntax

| Rule | POSIX/GNU | Perl/PCRE2 | ECMAScript | Python | Java | Ruby | RE2/Rust |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `{,n}` | BRE: literal; ERE: undefined (glibc: literal) | `{0,n}` (Perl 5.34+, PCRE2 10.43+) | legacy: literal; `u`: error | `{0,n}` | error | `{0,n}` | RE2: literal; Rust: **probe** |
| `{` not starting a valid quantifier | literal | literal | legacy: literal; `u`: error | literal | error | literal | literal (RE2); error (Rust) |
| `a**`, `a+*` | GNU: allowed | error | error | error | **probe** | allowed with warning | error |
| Quantifier on an assertion | ERE: error, for every anchor; BRE: a `*` after one is a literal asterisk (§5.18) | error (`(?=a)*`) | legacy: lookahead is quantifiable; `u`: error | error | **probe** | error | n/a |
| Possessive spelling | - | `*+ ++ ?+ {m,n}+` | - | same (3.11+) | same | same | - |
| Lazy spelling | - | `*? +? ?? {m,n}?` | same | same | same | same | same |
| Nothing to repeat: `*a`, `(*a)` | BRE: literal `*` at start; ERE: undefined (glibc: error) | error | error | error | error | error | error |

### 5.14 Group and reference spellings

| Spelling | Perl/PCRE2 | ECMAScript | Python | Java | .NET | Ruby | RE2/Rust |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `(?<n>...)` | yes | yes | 3.13: no (`(?P<n>)`) | yes | yes | yes | RE2: Go 1.22+; Rust: yes |
| `(?P<n>...)` | yes | no | yes | no | no | no | yes |
| `(?'n'...)` | yes | no | no | no | yes | yes | no |
| `\k<n>` | yes | yes | no | yes | yes | yes | no |
| `(?P=n)` | yes | no | yes | no | no | no | no |
| `\g{n}`, `\g{-1}` | yes | no | no | no | no | no | no |
| Subroutine `(?&n)`, `(?1)`, `\g<n>` | yes | no | no | no | no | `\g<n>` | no |
| Name syntax | `[A-Za-z_][A-Za-z0-9_]*`, 32 chars (PCRE2) | IdentifierName, Unicode | identifier | `[a-zA-Z][a-zA-Z0-9]*` | identifier | identifier | `[A-Za-z0-9_]+` |

**A subroutine call names a definition, not a number.** `(?|...)` renumbers
each branch from the same start, so one number can have a definition in every
branch - and those are different programs. In
`((?|(?<a>a)(?-1)|(?<b>b)(?-1)|(?<c>c)(?-1)))` all three calls resolve to
group 2, and Perl runs each against the group beside it, so the pattern
matches `aa`, `bb` and `cc`. `(?&b)` in `(?|(?<a>a)|(?<b>b))` is the same
question asked by name.

So a call carries where its target was written, and not only which number it
is. `(?R)`, a call by number, and a forward `(?+1)` have no definition to
point at and take the first, which is what those spellings mean.

A *conditional* asks a different question - whether the group participated -
and that one this library cannot answer per definition. `(?(<a>)x|y)` after
`(?|(?<a>a)|(?<b>b))` is false in Perl when the `b` branch matched, but the
branches have one capture slot between them, and a slot records that the
number participated rather than which definition set it. The two records that
turn on it are listed in `tests/data/vectors/known-gaps.txt` with that
sentence beside them.

### 5.15 Flags and default options

`grx_options_parse(syntax, string)` reads the dialect's own alphabet;
search-mode letters (`g`, `y`) are rejected with a diagnostic naming the
API call that expresses them.

**Built** for ECMAScript, PCRE2, Perl and Python; the rest arrive with their
dialects. Four answers, kept apart because a caller deciding what to tell a
user needs them apart: a letter the alphabet lacks is `GRX_DIAG_UNKNOWN_FLAG`,
the same letter twice is `GRX_DIAG_DUPLICATE_FLAG`, a search mode is
`GRX_DIAG_SEARCH_FLAG_IN_PATTERN`, and a letter the dialect has and this
library does not implement is `GRX_ERR_UNSUPPORTED`. Two letters that exclude
each other - `u` and `v` - are `GRX_DIAG_CONFLICTING_FLAGS` in either order,
which needed an exclusion *group* rather than a mask of forbidden option bits:
`v` implies `u`, so a mask catches `vu` and misses `uv`.

Perl's four charset modifiers - `a`, `d`, `l`, `u` - are one such group, with
one wrinkle no other letter has: `a` may be written twice, and the two need
not be adjacent, so `(?aia:s)` is `/aa`. What each chooses:

| Letter | Meaning here |
| --- | --- |
| `u`, `d` | the dialect's own semantics, which for Perl is Unicode |
| `a` | `\w`, `\d`, `\s`, `\b` and the POSIX classes are ASCII |
| `aa` | the same, and no fold orbit crosses U+0080: `/ai` lets `s` match U+017F and `/aai` does not, while U+00C0 still matches U+00E0 under both |
| `l` | the locale's semantics, which is the C locale's here - see section 6 |

| Dialect | Alphabet | Notes |
| --- | --- | --- |
| POSIX, GNU | none (API flags `REG_ICASE`, `REG_NEWLINE`) | `GRX_OPT_CASELESS`, `GRX_OPT_MULTILINE` |
| Perl | `msixxnpadlu` | `xx` is extended-more; `n` is no-capture; `a`, `d`, `l`, `u` are the charset modifiers and exclude each other, and `a` twice is `/aa` |
| PCRE2 | `imsxnUJ` and the `(*...)` leading directives | `U` ungreedy, `J` dupnames |
| ECMAScript | `dgimsuvy` | `u` and `v` exclusive; `g`/`y` rejected here |
| Python | `aimsux` | `a` and `u` are one choice written two ways and share an exclusion group; `a` narrows the shorthands *and* the folding (§5.9, §5.8), which perl's `/a` does not; `L` (locale) rejected as unsupported; **no `n`**, and an unscoped `(?i)` may stand only in a run at the very start of the pattern |
| Java | `idmsuxU` (embedded) | `U` is `UNICODE_CHARACTER_CLASS` |
| .NET | `imnsx` | `n` explicit capture |
| Ruby | `imx` | `m` is dot-all |
| RE2, Rust | `imsU` (Rust adds `u`, `x`, `R`) | `U` ungreedy |
| Tcl | `bceimnpqstwx` | `b` (BRE) and `e` (ERE) switch dialect: rejected here, since the dialect is the caller's choice |
| Vim | `\c`, `\C`, `\v`, `\m`, `\M`, `\V` in the pattern | magic level is a hook concern |
| Emacs | none | `case-fold-search` is `GRX_OPT_CASELESS` |

Default options per dialect: Ruby `MULTILINE`; Rust and Perl and Python
`UTF` (their subjects are Unicode strings); ECMAScript none (the caller adds
`UTF` for `u`); everything else none.

Python's `(?a)` is spelled as `GRX_OPT_ASCII_CLASSES` - a *narrowing* flag -
rather than as the absence of a widening one, and that is not a matter of
taste. Written the other way round, with `UCP` and `UTF` on by default and
`(?a)` clearing them, the shorthands narrowed correctly and `(?a).` stopped
matching a character whole while `(?a)\N{BULLET}` stopped compiling: both of
those key on `UTF`, and `re`'s ASCII mode touches neither. The profile's
`ascii_classes_fold_ascii` carries the other half, which is where Python and
perl's `/a` part company.

### 5.16 Splitting

`grx_regex_split()` divides a subject at every match, and **how it divides is
a per-dialect axis** - `GRX_SplitRule` in the profile, the way §5.5's
capture-reset cell is. Perl splits the way perl does; ECMAScript the way
ECMA-262 does; Python the way `re.split` does. PCRE2, POSIX and GNU define no
split at all, so they take ECMAScript's, which is the library's default
rather than a claim about them.

**Three values, not two.** Python's is a genuine third rule and not a blend
either of the others can be bent into: its empty-subject and trailing-field
behaviour is ECMAScript's, its `maxsplit` is perl's - counting splits, not
pieces, and leaving the unsplit remainder as the last field, with zero
meaning no limit rather than no pieces - and *every match separates*, which
neither of the others does. `split("x*", "")` is the shortest question the
function has and the three references answer it three ways:

| | `split("x*", "")` | `split(",", "a,b,c", 1)` | `split("x*", "abc")` |
| --- | --- | --- | --- |
| ECMAScript | `[]` | `["a"]` | `["", "a", "b", "c", ""]` |
| perl | `()` | `("a,b,c")` | `("", "a", "b", "c")` |
| Python | `['', '']` | `['a', 'b,c']` | `['', 'a', 'b', 'c', '']` |

Six hand-written cases agreed with `re` on all of this while 1,848 generated
rows did not; `tools/oracle/split_diff.py --dialect python` is the gate.

Unlike every other axis on this page this one is a fact about a *library
function* rather than about a grammar, which is why the languages below
disagree so widely: nothing in a regular expression says what a split should
do with a trailing empty field.

| Rule | ECMAScript | Perl | Python | Java | Go, Rust |
| --- | --- | --- | --- | --- | --- |
| Capturing groups appear in the output | yes | yes | yes (`re.split` since 3.7) | no | no |
| An empty match where a piece begins | not a separator | not a separator | a separator | **probe** | **probe** |
| A zero-width match at the end of the subject | not a separator | **a separator** | **probe** | **probe** | **probe** |
| An empty subject | one empty piece, or none if the pattern matches empty | none, whatever the pattern | one empty piece, or two if the pattern matches empty | **probe** | **probe** |
| Trailing empties | kept | dropped unless a limit was given, and *elements* rather than fields - an unset capture goes too | kept | dropped unless a negative limit | kept |
| `limit` counts | pieces, captures included; 0 yields none | fields, captures not counted; the last is the unsplit remainder; 0 means no limit | splits, not fields; 0 means no limit | fields | splits, not fields |

Two of Perl's cells came from `tools/oracle/split_diff.py` and not from
perlfunc, after the twenty-four probe cases below had agreed with perl
exactly. Both are invisible to a small corpus for the same reason: they show
only when the *other* rules are held out of the way.

- **A zero-width match at the very end is a separator in perl.**
  `split /$/m, "aab", 2` is `("aab", "")` there and `["aab"]` in node, whose
  loop runs while `q < size` and never looks at the end. With no limit perl's
  own trailing-empty drop removes the field again, so the difference is
  visible only with a positive limit.
- **The trailing drop removes elements, not fields.** `split /(,)/, "a,b,,"`
  keeps its final `","`, which reads as "a trailing capture stands" and is
  not the rule - that capture is simply not empty. `split /(a)|(b)/, "xa"`
  settles it: perl answers `"x", "a"`, having dropped the empty field *and*
  the unset capture behind it, where a negative limit gives
  `"x", "a", undef, ""`.

The ECMAScript rule in full, because the second row above is the one that
surprises people: the walk keeps a `piece_start`, and a match whose *end*
equals `piece_start` is skipped rather than ending a piece. That is what
makes `x*` split `"abc"` into three pieces rather than seven, and it is a
rule about the piece boundary rather than about the match - `a*` splitting
`"baac"` yields `b` and `c`, with the `aa` consumed as a separator and the
empty matches at either end of it ignored.

Perl's and Python's cells were **probe** and are now measured, by
`tools/oracle/split_probe.py` against perl 5.40.1 and CPython 3.13; the
twenty-four cases and their answers are in
[tests/data/probe/split.md](../tests/data/probe/split.md). Two things came
out of it that reading the documentation would not have given:

- **Perl and ECMAScript agree on the hard row exactly**, not approximately.
  perlfunc states the rule as one about a zero-width match at the *start of
  the string*, which reads like the weaker rule; it is the same rule.
  `(?=b)` on `"abc"`, `(?=,)` on `",a,"`, `,|` on `"a,b"` and `/^/m` on
  `"a\nb\n"` are the same in both. Ask perl for a *negative* limit, which
  turns off its trailing-empty drop, and every case in the report agrees with
  ECMAScript except the empty subject. Two rules, not five.
- **What looked like a third disagreement was the trailing-empty rule wearing
  the empty-match rule's clothes.** `split /b*/, "abb"` yields `a` in perl
  and `a`, `""` in ECMAScript, which reads as a difference about where the
  walk stops; with `-1` perl yields `a`, `""` too. A table built from that
  one example would have recorded the wrong axis, which is exactly what
  section 5.5's capture-reset cell did before it was probed.

Two smaller things the report settles: perl's trailing-empty drop removes
trailing *fields* and leaves a trailing capture standing, so
`split /(,)/, "a,b,,"` keeps its final `","`; and perl's `LIMIT` of 0 means
*no limit* where ECMAScript's `limit` of 0 means no pieces at all - two
functions whose zero means opposite things.

Java's and Go's cells stay **probe** because neither toolchain is on the
machine this was measured on, and a cell copied out of their documentation is
what the first two rows of this table used to be.

Splitting is reachable from every dialect that compiles - ECMAScript, Perl,
PCRE2, and the POSIX and GNU rows - and an earlier version of this section
said it was not, which was true when only ECMAScript had a front end.

**`split /^/` is `split /^/m`.** perlfunc says so, and which patterns it
covers is not what that sentence suggests, so perl was asked rather than
read. Against `"a\nb\nc"`:

| pattern | perl | pattern | perl |
| --- | --- | --- | --- |
| `^` | 3 pieces | `(^)` | 1 piece |
| `(?:^)` | 3 pieces | `^\|x` | 1 piece |
| `(?:(?:^))` | 3 pieces | `\A` | 1 piece |
| `(?i)^` | 3 pieces | `^a` | 2 pieces |

The rule is not about the source text - `(?:^)` and `(?i)^` are different
text and get it, `(^)` is barely different and does not. It is "the pattern
is **nothing but** a `^`", which is a property of what the pattern compiled
to: one line-anchor assertion, group 0's two saves, and the match. A
capturing group adds saves, an alternation adds a split, a literal adds a
char. `\A` is the same *kind* of assertion and is not a line anchor, which
is the distinction `GRX_INST_LINE_ANCHOR` exists for (§5.3). All eight
spellings above agree with perl.

### 5.17 Captures a failed negative lookaround made

| Value | Meaning | Dialects |
| --- | --- | --- |
| `CLEAR` | the writes the body made before failing are discarded | ECMAScript (22.2.2.4), PCRE2 |
| `KEEP` | they stand | Perl |

A negative lookaround succeeds by having its body fail, and the body may have
captured something on its way to failing. `a(?!(b)c)` against `abd` is the
whole of it: the body matches the `b`, fails on the `c`, and perl 5.40
reports group 1 as `"b"` where pcre2test 10.46 and Node report it unset.
Probed three ways rather than read from one.

A *positive* lookaround needs no axis. One that succeeded keeps what its body
captured in every dialect, and one that failed takes the whole construct with
it, so there is nothing left to disagree about.

**A conditional's assertion is the same axis reached from the other side**,
and it is the one place a *positive* assertion has the question: `(?(?=A)X|Y)`
does not fail when A fails - it takes the else-branch - so A's writes survive
the construct and the value decides what they are worth. `^(?(?=(a)b)x|a)`
against "ay" reports group 1 as `"a"` in perl 5.40.1 and unset in pcre2test,
which is the same split as `a(?!(b)c)` with the sign moved. So the rule is
stated on the *body*: when a lookaround's body fails, this value says whether
what it wrote stands. This library read it off the sign until 2026-09-22 and
answered the conditional case wrongly for Perl in consequence - the old
lowering carried the rule on a second, inverted copy of the assertion, and
the flag that sets it is keyed on the spelling.

Under `KEEP` the writes still become undo frames, so backtracking past the
whole assertion puts them back; what the value changes is whether the
assertion itself does. What it does *not* settle is what the last write was
when the body's failing part is a loop — see
`tests/data/vectors/known-gaps.txt`, where four records turn on where Perl's
own engine happens to restore an offset rather than on any rule, and are
categorised there as `reference-defect` for that reason. The minimal pair is
`(?!(a){2}$)` and `(?!(aa){2}$)` against "aaa": perl discards the write for
the first and keeps it for the second, which is the width of the repeated
body selecting between repeat opcodes and not a rule a second engine can
follow.

**What `KEEP` keeps here is the last value an iteration *finished* writing.**
The question only arises when the body fails part way through an iteration of
a repeat. A repeat clears its group at the top of every iteration (§5.5), so
at that moment the group holds the clear and no replacement yet, and an
implementation that reports the raw slots answers by where in the iteration
the failure happened - which is a fact about its own lowering, not a rule.
So against "aaa" `(?!(a){2}$)` reports group 1 as 1-2, both iterations having
finished, and `(?!(aa){2}$)` reports 0-2, the first having finished and the
second having died inside its body. Perl answers those two the other way
round, unset and 0-2, and so states no rule; this library answers both by the
one above. Where no repeat is involved the two agree, which is the case §5.17
is really about: `a(?!(b)c)` against "abd" reports group 1 as 1-2 in both.

### 5.18 The POSIX and GNU grammars

Four dialects out of one reader, because what separates them is what they
*have* rather than how anything is spelled. Two axes carry all of it: whether
the grouping operators are written with a backslash (`escaped_specials`), and
which feature bits are set.

|  | `posix-bre` | `posix-ere` | `gnu-bre` | `gnu-ere` |
| --- | --- | --- | --- | --- |
| Group | `\(`…`\)` | `(`…`)` | `\(`…`\)` | `(`…`)` |
| Interval | `\{m,n\}` | `{m,n}` | `\{m,n\}` | `{m,n}` |
| Alternation | none | `|` | `\|` | `|` |
| Backreference | `\1` | none | `\1` | `\1` |
| `\w`, `\b`, `\<`, `` \` `` | none | none | yes | yes |

**glibc's `regcomp` is the GNU pair, not the POSIX pair.** It accepts `\|`,
`\+`, `\?`, `\w`, `\b` and `\<` in a basic RE and `\w` and `\b` in an
extended one. POSIX leaves a backslash before an ordinary character undefined
and GNU defines it, so that is a conforming extension rather than a
disagreement - but it is why the vectors imported from glibc say `gnu-bre`
and `gnu-ere`. The two POSIX rows have a second reference now, and only a
partial one: musl's regex, which shares no code with glibc's, agrees with it
on 420 of Spencer's 463 cases, and those agreements are the `posix-*`
vectors. musl is not strict POSIX either - its basic RE takes the same GNU
operators, and it refuses the collating and equivalence classes POSIX
requires - so what actually *defines* these two rows, refusing `\|` and `\+`
in a basic RE and `\1` in an extended one, is still built from the standard
alone.

Six rules are worth stating, because none of them is what a reader coming
from Perl would guess, and each was asked of glibc rather than reasoned out:

**`.` matches a newline.** POSIX has no dot-all option because dot-all is
what it does; `.` against `"\n"` matches with no flags at all. The spec rows
carry `GRX_OPT_DOTALL` as a default option for exactly this.

**`REG_NEWLINE` is more than one rule.** It makes `^` and `$` line anchors,
*and* takes the newline out of `.` and out of a negated bracket expression.
Only the first is modelled here, as `GRX_OPT_MULTILINE`; the other half is
listed in section 6. The empty run after a final newline **is** a line here -
glibc matches `^$` against `"abc\n"` at offset 4 - where PCRE2 and Perl say
it is not, which is the `caret_after_final_newline` axis.

**There are no escapes inside a bracket expression.** `[\]]` is the class
holding a backslash, followed by a literal `]`; it matches the two characters
`\]` and neither one alone. A `]` that is to be a member is written first
instead, `[]a]`, and a `-` first or last is itself.

**An `*` with nothing to repeat is an asterisk**, in a basic RE: first in the
RE or in a subexpression, after an initial `^` if there is one. `^*a` matches
`"*a"`. An *extended* RE refuses the same three characters outright, which is
the one place the two grammars disagree about what `^*` even is.

**No anchor is a quantifier's target**, and the rule above is about anchors
rather than about `^`. In a basic RE an `*` after any of them is an ordinary
character too: `\>*` against `"a*"` matches 1-2 and `\>**` against `"a**"`
matches 1-3, which is a literal asterisk and then a quantifier over it. An
extended RE refuses a quantifier on any anchor, `\<*` and `\b*` exactly as
`^*`, and an interval is refused in both grammars because an interval is
never made ordinary. A *group* holding an anchor is an ordinary target and
stays one, so `\(\<\)*` is fine. The one place this departs from glibc is
the basic RE's `\<\?` and `\>\+`, which it compiles into a pattern that
can never match; section 6 says why that is not followed.

**A quantifier may be quantified**, and the two grammars differ about which.
An extended RE stacks them freely - `a**` is `(a*)*`, `a{2}{3}` is thirty-six
characters' worth of `a`, and `a*?` is `(a*)?` rather than a lazy repeat,
there being no lazy repeat in POSIX. A basic RE accepts only `\+` and `\?`
as the second: `a*\?` and `a*\+` match, while `a**`, `a\+*`, `a*\{1\}`
and `a\{1\}\{1\}` are all refused.

And one that is neither grammar's fault. **An unmatched `)` is an ordinary
character.** Spencer's own corpus calls it out - *"gag me with a right
parenthesis -- 1003.2 goofed here"* - and glibc matches `"a)"` with `a)` in
both. A basic RE's `\)` is still an operator, so an unmatched one is still
an error; the two questions are about two spellings.

## 6. Deviations

Every place this library knowingly differs from the implementation a
dialect names. A deviation has a reason and, where it is a restriction, a
diagnostic. This list is the one that §4's answer 3 refers to; it is meant
to be complete for every shipped tier.

| Dialect | Deviation | Reason | Reported as |
| --- | --- | --- | --- |
| all | Offsets are UTF-8 byte offsets | [design.md](design.md) §2 | - |
| all | `{m,n}` bounds above `max_repeat_count`, and expansions above `max_program_size`, are refused | bounded compile time | `GRX_ERR_LIMIT` |
| all | No locale; POSIX classes and case folding are C-locale ASCII or Unicode, never `LC_CTYPE` | [unicode.md](unicode.md) §7 | - |
| ECMAScript | Lone surrogates cannot occur in the subject | UTF-8 | - |
| ECMAScript | Repeat counts are limited (the grammar admits 2^53 - 1) | as above | `GRX_ERR_LIMIT` |
| ECMAScript | **The subject is code points, not UTF-16 code units, in *both* modes** | see below | - |
| ECMAScript | A match cannot begin or end between the halves of a surrogate pair | as above | - |
| Perl | `(?{ })`, `(??{ })` | code execution | `GRX_ERR_UNSUPPORTED` |
| Perl | `\N{name}` resolves against UCD 17.0.0, so a name Perl's UCD 15.0.0 does not carry works here and not there | version skew, the same as [unicode.md](unicode.md) §1's. Over a 2,531-name differential the two agree everywhere they share a Unicode version: of 204 disagreements, 196 name characters perl has not been told about and 8 are `NameAliases.txt` corrections newer than its tables, and **none** is a name perl resolves and this library does not | - |
| PCRE2 | `(?{ })` is not a construct it has at all | pcre2test: "unrecognized character after (? or (?-" | `GRX_ERR_SYNTAX` |
| Perl | `(?[ ])` accepts one unmatched `)` after a complete operand; this does not | `(?[ [a]) ])` compiles in perl 5.40.1 and is an error in pcre2test. Perl refuses two of them, a leading one, and an unmatched `(` - so it is one stray close parenthesis and no more, which is an off-by-one in its accounting rather than a rule to follow. Everything else about the two grammars is the same, which is not what this row used to say: it claimed Perl's "nests and takes different operands", and perl refuses a textual `(?[ (?[ [a] ]) ])` - what it nests is an *interpolated* `qr//`, which a pattern arriving as text cannot be. Compared over 13,440 generated rows | `GRX_ERR_SYNTAX` |
| Perl, PCRE2 | What an extended class **ignores** differs, and is followed | Perl skips all of `Pattern_White_Space` - all eleven code points probed - and takes `#` comments to the next **line feed**, which CR, VT and U+2028 do not end; pcre2test refuses a literal newline inside `(?[ ])` with error 216 and refuses `#` outright. U+00A0 is ignored by neither, which is the case that says the rule is the property and not a notion of "space" | - |
| Perl | Where a failed negative lookaround's body stopped *part way through an iteration*, the group reports the last value an iteration **finished** | §5.17 is followed as written - Perl keeps, ECMAScript and PCRE2 discard - but "what the body last wrote" is only well defined if the body failed between iterations, and Perl states no rule for the rest: it decides on the width of the repeated body, answering `(?!(a){2}$)` and `(?!(aa){2}$)` against "aaa" as unset and 0-2. The rule here answers them 1-2 and 0-2, so the two agree wherever Perl is self-consistent and differ on the narrow case where it is not | - |
| PCRE2 | A callout written where a conditional's **condition** goes is dropped | `(?(?C9)(?=a)b\|c)` prints callout 9 in pcre2test and prints nothing here. A conditional's children are the condition and the branches positionally, and there is no fourth slot to carry a callout in. The *match* is identical, and only a caller watching the trace can tell. 150 of 25,600 rows of `callout_diff.py` | - |
| PCRE2 | A script run may mix Han with **two** of Hiragana/Katakana, Hangul and Bopomofo | pcre2 10.46 accepts the mixture its own manual denies. pcre2unicode says a run may hold "a mixture of Hiragana, Katakana, and Han, or a mixture of Hangul and Han, or a mixture of Bopomofo and Han, but not, for example, a mixture of Hangul and Bopomofo and Han", and pcre2test matches that last one. All twenty two- and three-way combinations of U+6F22, U+304B, U+30AB, U+D55C and U+3105 were put to both references: they agree on fourteen - including `Hiragana+Hangul`, which both refuse, so it is not that Han lets anything through - and differ on exactly the six that mix two families. perl 5.40.1 refuses all six, which is UTS #39 section 5.1, and so does this library | - |
| PCRE2, Perl | `\C`, one code unit | the subject here is code points, and a construct that can land inside a character has no honest approximation | `GRX_ERR_UNSUPPORTED` |
| PCRE2 | `(*BSR_ANYCRLF)`, `(*BSR_UNICODE)` | built: `\R` is an alternation the parser writes and a directive may only lead the pattern, so the flag is set before the `\R` it governs. `(*BSR_ANYCRLF)\R` refuses a vertical tab and plain `\R` takes one, in pcre2test and here | - |
| PCRE2 | `(*LIMIT_MATCH=n)` and kin are applied in this library's units, not PCRE2's | the directive is honoured - §7.1 below - but `(*LIMIT_MATCH=n)` lands on `max_steps` and PCRE2's match limit counts calls to its internal match function, so the same `n` buys a different amount of work in each. A pattern that asks for a limit gets one, and the *number* is not portable | `GRX_ERR_LIMIT` |
| PCRE2 | A limit directive whose number does not fit a `size_t` is refused | pcre2test answers error 160, "(*VERB) not recognized or malformed", for `(*LIMIT_MATCH=4294967294)`, because its counter is 32 bits wide. This library's ceiling is its own and far higher, so the two disagree only between 2^32 and 2^64; what they share is refusing an unrepresentable request rather than turning it into another number | `GRX_ERR_SYNTAX` |
| PCRE2 | `(?(VERSION>=n.n))` is answered against 10.46 | this library emulates that version rather than being it. PCRE2's alone: perl answers "Unknown switch condition (?(...))" for every spelling, asking about its own version with `$]` outside the pattern | - |
| POSIX, GNU | Without `REG_NEWLINE`, `^` is the start of the subject and `$` its end, wherever in the pattern they stand | glibc answers the same question two ways. `^b` against "a\nb" is **nomatch** there, so `^` is not a line anchor for a search - but `.*^b` against the same subject **matches 0-3**, and `.^` matches 1-2, so a `^` reached after something consumed the newline *does* succeed. `a*^b`, `()^b`, `(^)b` and `(a\|)^b` are all nomatch again, which is the same position reached without consuming. Five of 7,033 differential cases turn on it and no imported vector does; the rule here is the consistent reading of the two | - |
| POSIX BRE, POSIX ERE | Measured only where two references agree, and not at all where the dialects differ from both | glibc's `regcomp` defines what POSIX leaves undefined and so answers as GNU; musl's regex, from Laurikari's TRE, shares no code with it but is not strict POSIX either - its basic RE takes `\|`, `\+` and `\?`, and it refuses the `[[.x.]]` POSIX requires. Neither decides alone. The 380 `posix-*` vectors are Spencer's rows the two answer *identically*; 41 they answer differently are left out as open questions and 8 use a construct these dialects do not have. What defines these rows - refusing the GNU operators - has no reference on this machine and is still built from the standard alone | - |
| GNU BRE | `\<\?` and `\>\+`: a quantifier on an anchor is refused rather than compiled | glibc compiles it and then cannot match with it - `\<\?` against "" is **nomatch** there, and an optional assertion that declines to match the empty string is an artifact rather than a rule. musl compiles the same pattern and matches, so the two references disagree and there is nothing to reproduce. An `*` after an anchor is a different question and is followed exactly: it is an ordinary character, as it is after `^`, which is why `\>*` against "a*" matches 1-2 | `GRX_ERR_SYNTAX` |
| Perl | `\p{nv=1/1}` and its kin resolve; perl refuses a fraction that reduces to an integer | UAX #44 §5.9.2 says numeric values match by "numeric equivalencies", and `1/1` is `1`. Perl keys its table by the *spelling* instead, so `1/1`, `2/2` and `0/3` are errors there while `2/4` and `9/12` resolve. Following the stated rule accepts a spelling perl rejects and never changes a match set | - |
| Perl | `/l` asks for the locale's semantics and gets the C locale's | there is no other locale here (section 6), and the C locale's word characters are the ASCII ones | - |
| POSIX | `[[.ch.]]`: a collating element of more than one character | no collation, and none is needed for the rest. `[[.a.]]` and `[[=e=]]` are *built* and answer as glibc 2.41 does in the C locale - each names the one character inside it, so `[[=a=]]` does not match "A" - which is the only locale this library has (§7 of [unicode.md](unicode.md)). What is refused is a name glibc also refuses there: `[[.ch.]]` and `[[.hyphen.]]` are errors in both. Row corrected 2026-09-22 after probing; it had said `[[=e=]]` was refused, and had named the wrong result code | `GRX_ERR_SYNTAX`, as glibc; **`GRX_ERR_SYNTAX`** in Perl and PCRE2 too, which do not have the construct at all — pcre2test raises error 113 and perl calls the syntax "reserved for future extensions", so a pattern using one there is not valid rather than not built |
| POSIX BRE, POSIX ERE | **Group spans follow POSIX where glibc and musl agree with each other and are both wrong** | §5.1's `POSIX` submatch rule is implemented, and these two rows are held to the standard rather than to an implementation. `(a\|ab)(c\|bcd)(d*)` against "abcd" is Fowler's case: group 1 can be "ab" with the whole match still reaching 4, so POSIX requires "ab", and both references give it the single "a". This library answers `0-4 0-2 2-3 3-4`. Six patterns, 6 of 15,246 generated `posix-ere` rows, each carried by name in `tools/oracle/submatch_diff.py` with the answer it must give - an exempt row whose *answer* is still checked, so a defect inside the class still fails the gate | - |
| GNU BRE, GNU ERE | Group spans follow glibc, which is not POSIX | §2 makes glibc the definition of these two rows, and it answers `(a\|aa)(a\|)` against "aa" with group 1 taking the shorter branch where POSIX's rule takes the longer. Following the standard here would mean leaving the reference these rows are named for, so the axis is per-dialect: `FIRST_PATH` for these two and `POSIX` for the other two. The two answers differ on 470 of the 15,246 generated `posix-ere` rows | - |
| POSIX BRE | A pattern with a backreference keeps the first-path division, not POSIX's | the comparison costs the short-circuit that ends the search when a match reaches the end of the window, and without it `\(a*\)*\1` against twenty characters runs out of steps where it used to answer at once. There is also nothing to be exact against: musl refuses a basic RE with a backreference outright, so no two references can decide such a row. The rule therefore applies to a program the Pike VM could also run - every POSIX ERE, and every basic RE without a backreference - which is also what keeps the two engines answering alike | - |
| POSIX, GNU | `(\<)*` reports the group as having matched empty where glibc reports it unset | the references disagree, so there is no rule to follow: glibc says a zero-width iteration did not happen, musl says it did, and this library says it did. Left where musl is, because the alternative is to special-case an iteration that consumed nothing *and* wrote nothing, which neither reference describes and only glibc does | - |
| Python | `\N{NAME}` resolves against UCD 17.0.0 rather than against CPython's own table | this library has one Unicode version and names come from it ([unicode.md](unicode.md) §2). A name added or renamed between CPython's table and UCD 17.0.0 would resolve differently; no such case has been found, and the differential would report one as a disagreement | - |
| Python | `bytes` patterns are not modelled: a `str` pattern is the whole of this dialect | `re` has two subjects, `str` and `bytes`, selected by the *type of the pattern object* rather than by a flag - a distinction a C API taking a byte string cannot make. The `str` half is the one with rules of its own; the `bytes` half is a narrower ASCII mode of it. `(?L)` is refused for the same reason it is refused in `re` for a `str` pattern | `GRX_ERR_UNSUPPORTED` for `L` |
| .NET | Culture-sensitive folding is invariant; balancing groups deferred | §5.8; [design.md](design.md) §2 | `GRX_ERR_UNSUPPORTED` for balancing groups |
| Emacs | Syntax classes (`\s-`, `\w`) use fixed Unicode definitions, not a syntax table | no syntax table | - |
| Vim | `\%[...]`, `\%d123`, `\z(`, `\=` in replacements | later tier | `GRX_ERR_UNSUPPORTED` |

### 6.1 ECMAScript and the unit of a subject

ECMA-262 defines matching over UTF-16 code units, and the `u` flag changes
the *grammar* and the folding rather than what a subject is made of. Without
`u`, `.` against an emoji matches one surrogate half and `RegExp.prototype.exec`
can report an index between the two.

This library's subject is UTF-8 bytes, and the ECMAScript profile decodes it
as text in both modes. That is a deviation, and the alternative was worse:
reading the bytes as units instead would make `.` match one third of a
character, `[^x]` match a continuation byte, and `\s` fail against U+FEFF. The
profile records the choice as `subject_is_text`, so a dialect whose subject
genuinely is a byte string - PCRE2 without `PCRE2_UTF` - is not affected by it.

Two consequences, both only observable with astral characters:

- `.` matches one character where ECMA-262 without `u` matches one code unit,
  so a subject containing an emoji gives a different count.
- A zero-width assertion can match *between* the halves of a surrogate pair
  in ECMAScript, including under `u`: `/\B/u` against `"0"` + an emoji + `"B"`
  reports index 2. UTF-8 has no such position, so this library reports the
  next match instead. The conformance harness skips these rather than
  counting them, and says how many it skipped.

## 7. Limits

`grx_limits_default()`'s values are measured, not guessed, from two corpora
that pull in opposite directions: patterns that must keep working, and pairs
that must be refused. `tools/limits/measure.py` produces the whole report;
what follows is its output, and `tests/unit/test_limits.cpp` and
`tests/conformance/test_redos.cpp` are the parts of it that keep being
checked.

### 7.1 The limits a pattern may ask for

PCRE2 lets a pattern set three of them from inside itself, and this library
honours all three:

| Directive | `GRX_Limits` field | Units |
| --- | --- | --- |
| `(*LIMIT_MATCH=n)` | `max_steps` | engine steps, not PCRE2's match count |
| `(*LIMIT_DEPTH=n)` | `max_backtrack` | backtrack stack entries |
| `(*LIMIT_HEAP=n)` | `max_match_memory` | *kibibytes*, scaled to bytes |

Three rules, each read off pcre2test 10.46 rather than argued from the
manual:

- **A pattern may lower a limit and may never raise one.** A caller's cap
  is a policy and a pattern that arrived from outside must not lift it, so
  the two are resolved by taking the smaller. Since `GRX_Limits` reads 0 as
  "no limit", a request always narrows a caller who set none.
- **The last directive wins, not the smallest.** `(*LIMIT_MATCH=1)`
  followed by `(*LIMIT_MATCH=1000000)` matches "abc" in pcre2test and the
  other order does not. The "may only lower" rule is about the caller's
  limit, not about an earlier directive.
- **Zero is a request, not an absence.** `(*LIMIT_MATCH=0)abc` fails every
  match in pcre2test with "match limit exceeded". That is the one value
  where the two encodings disagree and they disagree *backwards* - a
  `GRX_Limits` field of 0 means no limit at all - so a requested zero is
  answered as `GRX_ERR_LIMIT` at the top of the search rather than stored.

What is not portable is the number. `max_steps` counts steps this library
takes and PCRE2's match limit counts calls into its own matcher, so the
same `n` buys a different amount of work in each. A pattern that asks to be
bounded is bounded; a pattern tuned against PCRE2's counter is not tuned
against this one.

### What real patterns cost

264 distinct patterns: every `.rxt` vector this repository holds, plus
`tools/limits/real_world.txt` - the patterns that appear in JSON Schemas,
configuration files and validation code, collected for their *upper* end.
For each, the smallest value of each limit at which the pattern still
compiles, found by asking the limit itself rather than by adding a counter
to every phase.

| Limit | median | p99 | max | default | headroom | what needs the most |
| --- | --- | --- | --- | --- | --- | --- |
| `max_pattern_length` | 19 | 426 | 14005 | 65536 | 5× | a 2,000-branch alternation |
| `max_nesting_depth` | 1 | 10 | 10 | 128 | 13× | the RFC 822 address regex |
| `max_nodes` | 11 | 710 | 14005 | 100000 | 7× | the same alternation |
| `max_captures` | 1 | 7 | 10 | 1000 | 100× | `^((((((((((a))))))))))$` |
| `max_repeat_count` | 1 | 253 | 1000 | 65536 | 66× | `^.{0,1000}$` |
| `max_class_ranges` | 5 | 702 | 1463 | 10000 | 7× | `\p{L}` |
| `max_program_size` | 25 | 3074 | 33445 | 200000 | 6× | `^\p{RGI_Emoji}+$` |

The corpus grew a long tail after this table was first written, and the
`max_pattern_length` row is why it had to. Its longest entry had been 179
bytes - shorter than the RFC 5322 address regex most people have met - so a
366× headroom was being reported against a corpus with nothing long in it.
With the published RFC 5322 and RFC 822 address regexes and a generated
alternation added, the same default has 5× headroom and a p99 five times
higher. Nothing about the library changed; the measurement stopped
flattering it.

Nothing in the corpus is refused by a default. The tightest is now
`max_pattern_length` at five times, bound by the generated alternation added
when the corpus grew its long tail; `max_program_size` and `max_nodes`
follow at six and seven, and both are bound by the same pattern:
`\p{RGI_Emoji}` is the largest thing ECMAScript can name, and it lowers to an
alternation of 3,953 sequences.

Where the ceiling actually is depends on how that atom is spelled, which is
worth knowing before a caller meets it. `^\p{RGI_Emoji}{n}$` is one node
whatever `n` is and adds 16,719 instructions a repetition: `{11}` compiles
to 183,914 and matches, and `{12}` is refused by `max_program_size`. Writing
the atom out `n` times instead adds 13,958 *nodes* each time, and is refused
by `max_nodes` at eight copies with the program still at 133,757. Both
refusals name their field, which is the right answer rather than a gap - but
the field they name is not the same one.

### `max_backtrack` bounds the subject, not just the pattern

The limit that reads as a defence against a hostile pattern is also a
ceiling on how long a subject the two backtracking engines will scan, and
that was not known until `long.rxt` was written. A greedy loop over the
subject pushes one backtrack entry per position, so at the default 100,000
entries `/^a+$/u` is answered at 65,536 bytes and refused at 131,072 - on an
ordinary anchored scan, with the work itself nowhere near any other limit.
Raising `max_backtrack` to a million makes the same 1 MB subject match in
3,145,733 steps, comfortably inside `max_steps`.

The Pike VM has no such stack and answers at any length, which is why
`GRX_ENGINE_AUTO` picks it and why a caller who does not name an engine
never meets this. It matters to a caller who *does* name one:
`GRX_ENGINE_BACKTRACK` and `GRX_ENGINE_BITSTATE` are for patterns the Pike
VM cannot run - backreferences, lookaround, recursion - and on a long
subject those callers need `max_backtrack` raised to match.
`tests/data/vectors/ecmascript/long_limits.rxt` pins all of this, so the
ceiling moves deliberately rather than silently.

### Every limit is tunable, and zero means none

`core.h` has said "zero means no limit for every field" since the structure
existed. Checking that sentence found it was not quite true:
`max_lookbehind_length` and `max_recursion_depth` were in the structure and
in the documentation while nothing read either, so setting one to 1 was
exactly as unbounded as setting it to 0. A limit a caller can set and cannot
feel is worse than no limit, because it is a defence they believe they have.

Twelve of the thirteen are now enforced and each refuses with its own
diagnostic, so a caller who has to raise one is told which. The matrix is a
test rather than a paragraph:
`tests/unit/test_limits.cpp:EveryEnforcedLimitRefusesWhenTightAndCapsNothingAtZero`
sets each field tight, checks the refusal names that field, sets it to zero
and checks the same input goes through.

`grx_limits_unlimited()` fills in the all-zero structure, which is the shape
you want when measuring what a pattern costs rather than defending against
it. It is a loaded foot-gun: with `max_steps` and `max_backtrack` at zero
there is nothing between the backtracker and an unbounded run.

`max_recursion_depth` is the thirteenth, and is reserved rather than
enforced: no dialect here has recursion or subroutine calls, which arrive
with Perl and PCRE2 in WP-18. A test pins that, so the first dialect to
compile `(?R)` fails until the limit is wired up with it.

### `max_lookbehind_length`, and what a caller's policy means

`max_lookbehind_length`'s default is now 0 rather than 255. The field had a number while nothing enforced it; enforcing
it meant deciding what the default should *do*, and 255 would have started
refusing `(?<=a+)x` - valid ECMAScript, and in the corpus. ECMAScript's
lookbehind is unbounded (§5.4) and the profile says so, so a bound here is
the caller's own policy on top of the dialect rather than a property of it.
The default is to have no policy. A dialect that bounds its own lookbehind
enforces that through its profile, which is a different check with a
different diagnostic.

Enforcing it also fixed the fact it reads. `GRX_Facts::max_lookbehind` used
to report **0** for `(?<=a+)x` - the same answer as a pattern with no
lookbehind at all - because the analysis skipped a body whose length it
could not bound. That is backwards: the body it cannot bound is the one that
may read the whole subject. It is `GRX_NPOS` now, which is what
`GRX_Facts::max_length` has always meant by unbounded, and which exceeds
every finite cap.

`max_lookbehind_length` is measured now and was not when this table was
written - it was inert then, and "measuring an inert field against an
ECMAScript corpus would produce a number that meant nothing" was the reason
given. Enforcing it made the number mean something: the corpus's longest
lookbehind body is **4 bytes** (`(?<=\{\{)` in a template pattern), p99 is
2, and the default is `none`. There is no headroom column for it because
there is no cap to have headroom against, which is the point.

### The C stack, which is what `max_nesting_depth` is really about

Measured on the 256 KB stack the fuzzers run under: a pattern nested 480
deep parses and one nested 496 deep overflows, which is about 525 bytes of
stack per level. The default of 128 is therefore a factor of about four
inside the smallest stack this library claims to work on, and thirty inside
the usual 8 MB one. This is the one limit design.md §6.2 says a caller
should not lift casually, and that is the number the warning is about.

### What a match costs, and what refusing one costs

`max_steps` has to sit above what a legitimate match costs and below what a
hostile one does, and the two are measured separately.

Above: on whichever engine `GRX_ENGINE_AUTO` picks, a scanning pattern costs
between 0.02 and 3.0 steps per subject byte - the high end being an
unanchored `[a-z]+@[a-z]+`, which restarts at every position. At
`max_steps = 10,000,000` the costliest of those scans about 3.3 MB before
the limit binds.

This paragraph said 2.0 and 5 MB until the driver behind it was checked
rather than trusted. `measure.py` asks for a 100,000-byte subject;
`grx_limits` held 65,536 and quietly scanned those, and the report divided
the steps by the length it had asked for. Every rate here was 1.53 times too
low. The tool now refuses a record it cannot hold, which is the only way a
number like this stays honest: a measurement that silently measures
something smaller than it claims is worse than one that fails.

Below: 15 of the 17 pairs in the ReDoS corpus are no longer refused at all.
The backtracker arms the bit-state memo once a run has cost more than a
memoised one could ([design.md](design.md) §3.5.2), and those fifteen come
back with an answer in one to eight thousand steps - under a millisecond.

The two that remain are `(a|a?)+$` and `(a*)*$`, whose bodies can match
empty: the loop carries a progress register, the register is state the memo's
key does not include, and so the memo is never armed. They are refused in 120
to 134 milliseconds on an idle machine, and in 140 to 231 on the same machine
with six other cores busy - against plan.md WP-08's bound of one second. Both
ranges are recorded because the second is the one that matters: a bound is
worth having only if it holds when the machine is under load, which is when
an attack would be happening. Both are *answered* by the Pike VM in under a
millisecond at the same limits, which is the point: a limit is a defence only
because there is another engine that does not need it.

**The POSIX and GNU rows reach the ceiling from a shorter pattern**, and it
is worth knowing before one of them is pointed at an untrusted pattern -
which, being the grep-shaped dialects, is what they are for. Those two
grammars stack quantifiers, so `a*+*+*+*+` is legal there and nothing else
here accepts it; ECMAScript answers `GRX_DIAG_NOTHING_TO_REPEAT` at the
second one. Nine characters against a three-byte subject then spend the whole
step budget - about 2.2 seconds on an idle machine - before `GRX_ERR_LIMIT`
comes back. Nothing is unbounded and nothing is wrong: the limit is doing
exactly what it is for, and the observation is only that these dialects can
get there from a pattern a reader would not look twice at. A caller taking
patterns from outside should say so with `max_steps` rather than with the
default, and the same advice applies to `a*+*+*+` under any dialect that
would take it.

That shape is *not* the nested one. `((((a*)*)*)*)*` answers in four
milliseconds here and in two under ECMAScript, because the Pike VM's thread
list is keyed on the instruction and a nested loop revisits instructions
rather than multiplying them. It is the stacked spelling that costs, and only
where the grammar allows it.

`max_steps` stays at 10,000,000, and the numbers above are why rather than a
preference. Lowering it to a million would refuse a still-pathological pair
in about 15 ms instead of 130 - but would also cap a legitimate scan at 333 KB,
and a caller scanning documents that large is not the one being attacked. A
caller who *is* - one compiling patterns from a file it did not write -
should lower it, and now has the arithmetic to choose by: divide it by three
to get the bytes it will scan, and multiply it by 15 nanoseconds to get the
time it will spend refusing on an idle core.

## 8. ECMAScript in full

The first dialect, specified to the level the parser and lowering are
written against. Clause numbers are ECMA-262 16th edition.

### 8.1 Three modes

| Mode | Selected by | Grammar |
| --- | --- | --- |
| **Legacy** | neither `u` nor `v` | 22.2.1 *as modified by Annex B.1.2*. This is what every browser and Node run for a pattern without `u`, so it is the mode, not a compatibility option. |
| **Unicode** | `GRX_OPT_UTF` (the `u` flag) | 22.2.1 with the `[+UnicodeMode]` productions; Annex B does not apply |
| **UnicodeSets** | `GRX_OPT_UNICODE_SETS` (the `v` flag; implies Unicode mode) | 22.2.1 with `[+UnicodeSetsMode]`: class set operations, string disjunctions, properties of strings |

JSON Schema's profile is Unicode mode ([design.md](design.md) §1.1).

### 8.2 Legacy mode: what Annex B.1.2 changes

Each of these is a hook rule in `src/syntax/ecmascript.c`, and each has a
test that states it:

- **Identity escapes**: `\` followed by any character that is not `c`, and
  not a syntax character with a defined escape, is that character. So `\p`
  is `p`, `\-` is `-`, `\k` is `k` *unless the pattern contains a named
  group*, in which case `\k` must be a named reference.
- **Legacy octal**: `\0` followed by octal digits, and `\1`-`\377` when the
  number exceeds the group count, are octal escapes; `\8` and `\9` are
  identity escapes. A backreference `\N` where N > group count is
  reinterpreted as the longest octal prefix.
- **`\c`**: `\c` not followed by a letter is a literal backslash followed
  by `c`; inside a class, `\c` may also be followed by a digit or `_`.
- **`\x`, `\u` malformed**: `\x` not followed by two hex digits is `x`;
  `\u` not followed by four is `u`. Surrogate pairs written as `😀`
  are two code units and cannot be matched (UTF-8; deviation §6).
- **Braces and brackets**: `{`, `}` and `]` are literals where they do not
  form a valid quantifier or close a class. `a{` , `a{1`, `a{,3}` are
  literal sequences.
- **Quantified assertions**: a lookahead may be quantified, `(?=a)*`;
  lookbehind may not.
- **Class ranges with class escapes**: `[\d-z]` is the union of `\d`, `-`
  and `z`.
- **`\p{...}`** is an identity escape; property escapes do not exist.
- **Case-insensitivity** uses `ES_LEGACY` folding (§5.8).
- **`.`** matches any code unit except the four line terminators.

### 8.3 Unicode mode

- Every Annex B relaxation above is a syntax error.
- `\u{H...}` up to 10FFFF; `\uHHHH\uHHHH` as a surrogate pair is one code
  point.
- `\p{...}` and `\P{...}` per 22.2.2.9.7 and table 68: `General_Category`
  (`gc`), `Script` (`sc`), `Script_Extensions` (`scx`), and the binary
  properties of table 69, with strict name matching ([unicode.md](unicode.md)
  §6). A property of strings is a syntax error in `u` mode.
- `.` matches a code point; the line terminator set is unchanged.
- Case-insensitivity uses simple case folding; `\w` gains U+017F and U+212A.
- `{` , `}` and `]` must be escaped when literal; `\-` is valid only in a
  class.

### 8.4 UnicodeSets mode

- Nested classes `[[a-z]--[aeiou]]`, intersection `&&`, subtraction `--`,
  each operand a class or a single item; an operator may not be mixed with
  union in the same class without nesting.
- `\q{abc|de|}` string disjunctions; a class containing strings is matched
  longest-string-first.
- Properties of strings (`\p{RGI_Emoji}` and the six others in
  [unicode.md](unicode.md) §3) are allowed in a positive class, forbidden
  under negation.
- The double-punctuator characters and `( ) [ ] { } / - \ |` must be
  escaped inside a class.
- `i` with `v` uses the same folding as `u`, and a negated class is
  complemented *after* folding (the `MaybeSimpleCaseFolding` rule), which
  differs from `u` for `[^\P{Lu}]`-shaped patterns and has a test.

**Built.** The last rule was worth the note it was given: writing it found
that `u` mode had been applying it too. Under `u`, `\P{X}` is the complement
of X itself and the folding is the matcher's, so `[^\P{Lu}]` matches nothing
under `iu` - the complement of Lu folds up to everything - where under `iv`
it matches every cased letter. A *shorthand* is the exception and does fold
before its complement, because ECMA-262 22.2.2.9.3 builds the widening into
`WordCharacters` rather than into the negation, which is why `\W` under `iu`
excludes U+017F. Two rules, not one.

A class whose members are all one code point long lowers to exactly the
instruction it would have without `v`. One with strings in it lowers to an
alternation ordered longest first, then the single characters, then the empty
string - which is what makes `[\q{abc|ab|a}]` report `abc` against "abc"
rather than the `a` that leftmost-first would otherwise prefer.

### 8.5 Semantics that lowering and the engines implement

- `LEFTMOST_FIRST`; `FAIL_IF_EMPTY_AFTER_MIN`; `RESET_EACH_ITERATION`;
  unset backreference `MATCH_EMPTY`; lookbehind `UNBOUNDED`, evaluated right
  to left with its captures set in reverse order (22.2.2.4, direction -1);
  `ADVANCE_ONE` iteration; `$` end-only; `\b` from the `\w` of §5.9.
- Named groups: `(?<name>...)`, `\k<name>`; a name is an IdentifierName
  with `\u` escapes allowed in it. Duplicate names only across
  alternatives (ES2025); `grx_match_group_named()` returns the one that
  participated.
- Backreference comparison under `i` compares canonicalised code points.
- `{n,m}` with n > m is a syntax error; the counts are decimal without
  limit in the grammar and limited here (§6).
- Flags `d`, `g`, `y` have no compile-time meaning: `d` is always
  satisfied, `g` is `grx_regex_search_next()`, `y` is `grx_regex_match()`.
- Modifiers `(?i:...)`, `(?-i:...)`, `(?i-m:...)` for `i`, `m`, `s` only
  (ES2025). **Probe resolved:** Node 22.23 rejects all three as
  `SyntaxError`, with and without `u`. This library therefore rejects them
  too, because the oracle is what the conformance vectors come from and a
  library that accepted what the oracle rejects would tell a caller their
  pattern is valid for an engine that refuses it. When a runtime that
  implements them is pinned as the oracle, this rule and its test change
  together. A bare `(?i)` is a syntax error in every version.
- Duplicate named groups across alternatives (ES2025). **Probe resolved:**
  Node 22.23 rejects `(?<a>x)|(?<a>y)`, so this library does. Same reason,
  same change when the oracle moves.

### 8.6 What the oracle corrected

Three rules were implemented from the specification, checked against Node 22,
and found wrong. Each is now a test that says so:

1. **A lone script value.** [unicode.md](unicode.md) §6 said
   `\p{Greek}` was allowed. It is a `SyntaxError`: only a General_Category
   value or a binary property name may stand alone.
2. **Quantified assertions.** Annex B lets a *lookahead* be quantified, so
   `(?=a)*` is valid without `u`. A lookbehind never is, and neither is
   under `u`. The first draft allowed all four combinations.
3. **`\k` inside a class.** Annex B's
   `SourceCharacterIdentityEscape[+N]` excludes `k` when the pattern names a
   group *anywhere*, and the exclusion reaches inside a character class,
   where a named reference cannot appear at all. So `[\k]` is a literal `k`
   on its own and a `SyntaxError` in `[\k](?<n>x)`. This one was found by
   differential fuzzing, not by reading: the rule hangs off a production
   parameter rather than off the class.

The sweep that found the third is `tools/oracle/syntax_diff.py`, run by
`make check-oracle-syntax`. It compares accept and reject over an exhaustive
corpus of short patterns and a random corpus of long ones, and currently
finds no disagreement over 720,000 cases per seed.

Five more were found the day test262 was imported, and they are the argument
for importing a corpus somebody else wrote rather than only generating one.
Every differential gate here was green at the time; all five had been green
for as long as the code existed.

4. **The binary property list is closed.** `\p{Other_Alphabetic}` is a real
   UCD property and a `SyntaxError` in JavaScript: ECMA-262 names the binary
   properties a pattern may spell, and that list does not include the
   `Other_*` family, `Grapheme_Link`, `Hyphen` or
   `Prepended_Concatenation_Mark`. The generator had the list, under a
   comment saying what it was for, and nothing read it - so every binary
   property in the UCD was reachable. Eleven names, accepted where the
   specification requires refusal.
5. **`Script=Unknown` exists.** `Scripts.txt` lists what is assigned;
   everything else is `Unknown` (`Zzzz`), a value ECMA-262 accepts and the
   file does not carry, so it had to be synthesised and was not.
6. **`Changes_When_NFKC_Casefolded` was missing.** It is in ECMA-262's list
   and lives in `DerivedNormalizationProps.txt`, which was fetched and never
   read.
7. **A group name always uses the Unicode escape grammar.**
   `RegExpIdentifierStart` is `\ RegExpUnicodeEscapeSequence[+UnicodeMode]`
   with the parameter set unconditionally, so `(?<\u{1d5b0}x>y)` is a valid
   name in a pattern with no flags. The parser passed it the `u` flag
   instead, and rejected fifty-five of test262's named-group cases.
8. **A backreference inside a lookbehind ran forwards.** A lookbehind body
   carries `GRX_INST_REVERSE` and steps backwards; `GRX_OP_BACKREF` compared
   forward from the position and advanced forward inside it. The visible
   result was captures whose end preceded their start - `(.)(?<=(\1\1))`
   against `"aaa"` reported group 2 as `3-1` - and lookbehinds that matched
   when they must not.

A ninth was not a rule but an engine: `(a?b??)*` against `"abc"` answered
`0-1` on the Pike VM where every other engine and ECMA-262 answer `0-2`. See
[design.md](design.md) §3.5.2 for what was wrong and why a corpus of
hand-written patterns was what found it.

### 8.6.1 Where the oracle is wrong

One `v`-mode rule goes the other way: the oracle is wrong and this library
does not follow it.

Under `iv`, Node 22.23 does not apply case folding to a class-set operand
that is a bare character or a one-character `\q{}`:

| Pattern | Subject | Node 22.23 | Here, and ECMA-262 |
| --- | --- | --- | --- |
| `[abc]` | `A` | matches | matches |
| `[[a]&&a]` | `A` | matches | matches |
| `[a&&[a]]` | `A` | **no match** | matches |
| `[a&&a]` | `A` | **no match** | matches |
| `[a--b]` | `A` | **no match** | matches |
| `[\q{a}]` | `A` | **no match** | matches |
| `[\q{ss}]` | `SS` | matches | matches |

The third and fourth rows are what settle it. `[[a]&&a]` matches and
`[a&&[a]]` does not, and those two are the same intersection written in the
opposite order. Set intersection is commutative; no reading of 22.2.1 makes
one of them fold and the other not. It is an implementation defect, and
following it would mean disagreeing with the specification *and* with every
other engine in order to agree with this one.

So this is the one place where §4's "the reference implementation is the
authority" does not apply. The rule it yields to instead is the one §8.6
already implies: the oracle is the authority on what a *dialect* means, not
on what a program does when it contradicts itself. The rows above are a test
in `tests/unit/test_unicodesets.cpp` that asserts this library's answer and
records Node's beside it, and `tools/oracle/match_diff.py` keeps these shapes
out of its `v` corpus with the same reason written where it does so. When a
Node whose answers are symmetric is pinned, the test and the corpus change
together and this section goes away.

### 8.7 Conformance sources

- test262: `test/built-ins/RegExp/` (including `named-groups/`,
  `lookBehind/`, `dotall/`, `unicodeSets/`, `match-indices/`,
  `regexp-modifiers/`, `duplicate-named-groups/`), `test/annexB/built-ins/RegExp/`,
  `test/language/literals/regexp/`, and
  `test/built-ins/RegExp/property-escapes/generated/`, whose per-property
  files are the most thorough check of the Unicode tables available
  anywhere. [testing.md](testing.md) §7 describes the import.
- Vectors generated from Node for the probe suite and the random-pattern
  generator.

### 8.8 The JSON Schema profile

Not a dialect and not an option: a documented way of using this one.
`GRX_SYNTAX_ECMASCRIPT | GRX_OPT_UTF`, `grx_regex_search()` (never
`grx_regex_match()`), and `matched` is the only output consulted. The
recommended subset in JSON Schema core §6.4 is exactly the regular subset,
so a schema that follows the recommendation runs on the Pike VM with the
linear-time guarantee; `grx_regex_facts()` tells the validator whether it
did, and `grx_pattern_lint()` reports which construct took a pattern outside
the subset and where it was written.

The two answer different questions and a validator wants both. `is_regular`
is about *this* library and says whether the linear-time engine can run the
pattern. The lint is about *every other* validator and says whether the
pattern means the same thing to them - which `\d` does not, being ASCII in
ECMAScript and Unicode-aware in Python and .NET, and which `.` does not,
over line terminators and over whether an astral character is one thing or
two. `\d+` is regular and outside the subset; both answers are correct and
neither implies the other.

Section 6.4's list is short enough to quote and the lint reports anything
not on it: individual Unicode characters; `[abc]` and `[a-z]`; `[^abc]` and
`[^a-z]`; `+`, `*`, `?` and their lazy forms; `{x}`, `{x,y}`, `{x,}` and
theirs; `^` and `$`; and `(...)` with `|`. That leaves `.`, the shorthand
classes and `(?:...)` outside it. The first two are the portability hazards
above. The third is harmless in practice and is reported anyway, because the
alternative is to start deciding which bullets were meant loosely, and a
lint that does that is a lint nobody can rely on.

## 9. Notes for the rest of tier 1

Facts that shape the front end and are easy to get wrong; each becomes a
test.

**PCRE2 10.46.** What follows was written from pcre2pattern and corrected by
pcre2test, which is the only reason several of these lines are right. Five of
them the documentation did not settle: `\x` with no digits is an error in
10.46 where older PCRE2 read it as NUL; a quantifier on a lookaround
*compiles* (`/(?=a)*/` is fine, where ECMAScript's `u` mode calls it a syntax
error) but one on `(*FAIL)` does not, because `(*FAIL)` is `(?!)` written
short; `(*ACCEPT:X)` takes an argument that perlre describes as
argument-less; `xx` ignores space inside a bracket expression and `x` does
not; and `(?^-i)` is "invalid hyphen in option setting" because `^` has
already cleared everything.

Script runs `(*script_run:`, `(*sr:`, `(*atomic_script_run:`, `(*asr:`,
each checking that the text its body matched could have been written in one
script - UTS #39 section 5.1's rule, with the augmented script sets applied
in the generated table so that Han's three combinations fall out of an
ordinary intersection. `(*asr:...)` is `(*sr:(?>...))`, the atomic group
*inside*. A conditional's condition may not be one: pcre2test answers
"atomic assertion expected after `(?(`" for `(?(*script_run:x)y)` and so
does this, which is a statement about conditions and not about script runs.

Verbs `(*ACCEPT)`, `(*FAIL)`, `(*COMMIT)`, `(*PRUNE)`,
`(*SKIP)`, `(*THEN)`, with and without names; leading directives
`(*UTF)`, `(*UCP)`, `(*CRLF)`, `(*LF)`, `(*ANYCRLF)`, `(*ANY)`, `(*NUL)`,
`(*NO_AUTO_POSSESS)`, `(*LIMIT_MATCH=d)` and kin (the limit directives map
onto `GRX_Limits` and may only lower a limit); `\K` disallowed in lookaround
by default; `(?|` branch reset; `(?J)` duplicate names; recursion `(?R)`,
`(?1)`, `(?-1)`, `(?+1)`, `(?&name)`, `(?P>name)`, `\g<1>` (that last
spelling PCRE2's alone); conditionals on
group number, name, `R`, `Rn`, `R&name`, `DEFINE`, `VERSION>=n`, and on an
assertion; `\Q...\E` including inside classes; `(?x)` and `(?xx)` extended
modes; `\h \H \v \V \R \N \X`; `\C` (single code unit: refused,
`GRX_ERR_UNSUPPORTED`); auto-possessification is an optimisation and has no
semantic effect, so it is not modelled.

**Perl 5.40.** As PCRE2 minus verbs-with-arguments differences, and minus
**every leading directive**: `(*UTF)`, `(*UCP)`, `(*CRLF)` and kin,
`(*NO_AUTO_POSSESS)` and kin, and `(*LIMIT_MATCH=d)` and kin are each
"Unknown verb pattern" in perl. All nineteen probed there rather than read
off perlre, because the list is one this front end shares between the two
dialects and a name added for PCRE2 is accepted by both unless something
says otherwise - and since none of them changes a match, the difference
would be silent.

Minus five more construct families, found the same way and each accepted
here until 2026-09-22: callouts `(?C...)` ("Sequence (?C...) not
recognized"); the subroutine-call spellings `\g<1>` and `\g'name'`
("Unterminated \g... pattern" - perl's `\g` takes `\g1`, `\g-1` and
`\g{...}`, and its subroutine calls are `(?1)` and `(?&name)`);
`(?(VERSION>=n))` ("Unknown switch condition"); `(?J)` in every position
("Sequence (?J...) not recognized" - perl lets two groups share a name
inside `(?|...)`, which is a rule about branch reset rather than a flag);
and the non-atomic lookarounds in all four spellings, `(*napla:`,
`(*naplb:`, `(?*` and `(?<*`.

Plus:
`\b{wb}` and friends (later); `(?<name>)` with duplicate names via `(?|`;
`\N{U+263A}`; `(?^...)` caret to reset flags; `\g{-1}` relative
backreferences; `/n` no-capture; `/xx`. Perl's real difference from PCRE2 is
in §5.8 (full folding) and in how far `(?{})` is from anything this library
does.

**POSIX BRE and ERE.** BRE: `\(` `\)` `\{` `\}` are the operators; `*` at the
start of an RE or after `\(` or `^` is literal; `^` is an anchor only at
the start and `$` only at the end; `\+`, `\?`, `\|` are undefined in POSIX
and are the GNU extensions. ERE: `+`, `?`, `|`, `{`, `()` are operators
everywhere; `{` not starting a valid interval is undefined (glibc: literal,
probed and implemented as such - §5.18); backreferences are undefined
(glibc: accepted, and `GRX_SYNTAX_POSIX_ERE` refuses them since a
"portable" ERE must not use them; `GRX_SYNTAX_GNU_ERE` accepts). Bracket
expressions: no escapes inside; `]` first is literal; `[:class:]`,
`[=e=]`, `[.x.]`; a range endpoint may be a collating element. Anchors in
the middle of a BRE are literals. Matching is `LEFTMOST_LONGEST`
throughout, including inside groups by the POSIX subexpression rule.

**GNU BRE and ERE.** POSIX plus `\w \W \s \S \b \B \< \> `` \` `` `\'`,
`\|` `\+` `\?` in BRE, backreferences in ERE, `a**` accepted, `\{` in ERE
as a literal brace. `grep` and `sed` are line-oriented, so the vectors from
them are single-line subjects; glibc `regcomp()` is the oracle for
multi-line behaviour and `REG_NEWLINE`.
