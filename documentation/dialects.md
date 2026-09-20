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
| Python | `python` | CPython 3.13 | `re` module documentation, 3.13 | `python3` (available); `Lib/test/re_tests.py` |
| Java | `java` | JDK 21 | `java.util.regex.Pattern` javadoc, 21 | OpenJDK (install) |
| .NET | `dotnet` | .NET 8 | "Regular Expression Language - Quick Reference"; "Regular expression options" | .NET SDK (install) |
| Ruby | `ruby` | Ruby 3.3 / Onigmo 6.2 | Onigmo `doc/RE`; Ruby `Regexp` documentation | `ruby` (install) |
| RE2 | `re2` | Go 1.22 `regexp` | RE2 "Syntax" wiki; Go `regexp/syntax` documentation | `go` (install) |
| Rust | `rust` | `regex` 1.10 | `regex-syntax` documentation | `cargo` (install) |
| Tcl | `tcl` | Tcl 8.6 | `re_syntax(n)` | `tclsh` (install) |
| Vim | `vim` | Vim 9.1 | `:help pattern` | `vim -es` with `matchlist()` (available) |
| Emacs | `emacs` | GNU Emacs 29 | Elisp Reference Manual, "Regular Expressions" | `emacs --batch` (install) |

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
| NON_ATOMIC_LOOKAROUND | - | - | - | - | `(*napla:`, `(*naplb:` | same, and `(?*`, `(?<*` | - |
| LOOKBEHIND | - | - | - | - | yes (§5.4) | yes | yes |
| ATOMIC_GROUP | - | - | - | - | yes | yes | - |
| CONDITIONAL | - | - | - | - | yes | yes | - |
| RECURSION / SUBROUTINE | - | - | - | - | yes | yes | - |
| INLINE_FLAGS / SCOPED_FLAGS | - | - | - | - | both | both | scoped only (ES2025; **probe** Node 22 support) |
| COMMENT_GROUP | - | - | - | - | yes | yes | - |
| POSIX_CLASS | yes | yes | yes | yes | yes (in brackets) | yes | - |
| UNICODE_PROPERTY | - | - | - | - | yes | yes | `u`/`v` only |
| CLASS_SET_OPS | - | - | - | - | - | `(?[ ])` extended classes | `v` only |
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
| DUPLICATE_NAMES | - | - | - | - | with `(?\|)` or `(?P<n>)` twice under `use re 'eval'`? no: **probe** | `(?J)` | yes (ES2025; **probe** Node 22) |
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
| `LEFTMOST_LONGEST` | among matches starting at the leftmost position, the longest; submatches per POSIX's rule, approximated in 1.0 ([design.md](design.md) §2) |
| `TCL_ARE` | Tcl's rule: leftmost, then the whole RE is greedy or non-greedy according to its first quantifier, and length is preferred accordingly (`re_syntax(n)`, "Matching") |

POSIX BRE/ERE, GNU BRE/ERE: `LEFTMOST_LONGEST`. Tcl: `TCL_ARE`. Every
other dialect: `LEFTMOST_FIRST`.

### 5.2 Newlines and `.`

| Dialect | Newline set for `.`, `^`, `$` | `.` excludes | Dot-all spelling |
| --- | --- | --- | --- |
| POSIX, GNU | `\n` only with `REG_NEWLINE`; otherwise none, and `.` matches `\n` | `\n` under `REG_NEWLINE` | n/a |
| Perl, PCRE2 | `\n` (PCRE2: build default; `(*CRLF)`, `(*ANYCRLF)`, `(*ANY)` change it per pattern - supported as `GRX_OPT_NEWLINE_*` options) | the newline set | `s` |
| ECMAScript | LF, CR, U+2028, U+2029 | all four | `s` |
| Python | `\n` | `\n` | `s` (`DOTALL`) |
| Java | `\n`, `\r`, `\r\n`, U+0085, U+2028, U+2029; `\n` alone under `UNIX_LINES` | all | `s` |
| .NET | `\n` | `\n` | `s` |
| Ruby | `\n` | `\n` | **`m`** |
| RE2, Rust | `\n` | `\n` | `s` |
| Tcl | `\n` | `\n` under `(?n)`/`(?p)` only | default is dot-all; `(?n)` turns it off |
| Vim | line-based: the subject is a line; `\n` matches a line break only via `\n`/`\_` forms | end of line | `\_.` |
| Emacs | `\n` | `\n` | none (`[^z-a]` idiom) |

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

The engine implements `UNBOUNDED` ([design.md](design.md) §3.5.2); the
profile value is a *parse-time* restriction that makes the library reject
what the reference would reject, and `max_lookbehind_length` caps the
bounded forms.

### 5.5 Empty iterations and captures in loops

The two rules that make `(a*)*` against `b` report different things:

| Axis | Value | Dialects |
| --- | --- | --- |
| Empty iteration | `FAIL_IF_EMPTY_AFTER_MIN`: an iteration that consumes nothing, once `min` is satisfied, fails (22.2.2.3.1 RepeatMatcher step 2.b) | ECMAScript |
| | `BREAK_ON_EMPTY`: the iteration succeeds and the loop stops | Perl, PCRE2, Python, Java (**probe**), .NET (**probe**), Ruby (**probe**), RE2, Rust |
| | `LONGEST`: irrelevant; the match is the longest, and an empty iteration adds nothing | POSIX, GNU, Tcl |
| Capture reset | `RESET_EACH_ITERATION`: captures inside the group are cleared at the start of every iteration (RepeatMatcher step 4) | ECMAScript, **Perl** (probed) |
| | `KEEP_LAST_SET`: a capture set in an earlier iteration survives if a later one does not set it | PCRE2, Python, Java (**probe**), .NET, Ruby (**probe**), RE2 (**probe**), Rust (**probe**) |

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
state is a rule this library will not implement. The nineteen records in
`tests/data/vectors/known-gaps.txt` that turn on it are named for the
question rather than for an answer.

### 5.6 Backreferences to unset groups; forward and nested references

| Axis | Value | Dialects |
| --- | --- | --- |
| Unset group | `MATCH_EMPTY` | ECMAScript; PCRE2 with `PCRE2_MATCH_UNSET_BACKREF` (exposed as `GRX_OPT_MATCH_UNSET_BACKREF`); Vim (**probe**) |
| | `FAIL` | Perl, PCRE2, Python, Java, .NET, Ruby, GNU, Tcl (**probe**), Emacs (**probe**) |
| Forward reference `\2(a)(b)` | allowed, behaves as unset | ECMAScript, Perl, PCRE2 |
| | syntax error | Python ("invalid group reference"), Java (**probe**), RE2/Rust (no backreferences) |
| Reference to the group it is inside, `(a\1)` | allowed, unset on first entry | Perl, PCRE2, ECMAScript |
| | syntax error | Python |

### 5.7 Backreference versus octal, and the numeric escapes

| Dialect | `\1`..`\9` | `\10` and up | `\0` | Octal | `\x` |
| --- | --- | --- | --- | --- | --- |
| POSIX, GNU | backreference | `\1` then `0` | undefined; GNU: literal? **probe** | none | none |
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
| `FULL_FOLD` | full folding, including length-changing (`ß` ~ `ss`) | Perl. **Deviation:** implemented as `SIMPLE_FOLD`; [design.md](design.md) §10 |
| `ES_LEGACY` | ECMA-262 Canonicalize without `u`: simple uppercase mapping, rejected if it is multi-unit or maps non-ASCII to ASCII | ECMAScript without `u` |
| `ASCII_ONLY` | A-Z only | Java without `UNICODE_CASE`; PCRE2 without UTF; POSIX and GNU (deviation: the locale is treated as C); Vim `\c`, Emacs (**probe**) |
| `CULTURE` | .NET's culture-sensitive `ToLower`. **Deviation:** implemented as `SIMPLE_FOLD`, i.e. `CultureInvariant` | .NET |

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

POSIX bracket classes (`[:alpha:]` and the other eleven) are ASCII in POSIX
and GNU (C locale), Unicode in Perl, PCRE2 under `UCP`, Ruby, Tcl, Rust,
Vim; RE2 is ASCII. `\b` is defined from `\w` in every dialect; Perl's
`\b{wb}` (UAX #29 word boundaries) is a later tier.

### 5.10 Iteration after an empty match

| Value | Rule | Dialects |
| --- | --- | --- |
| `RETRY_NONEMPTY_THEN_ADVANCE` | at the same position, retry refusing an empty match; if that fails, advance one character | Perl, PCRE2 (its documented `NOTEMPTY_ATSTART` loop), Python 3.7+ |
| `ADVANCE_ONE` | advance one code point (one code unit without `u`) and search again; an empty match immediately after a non-empty one is reported | ECMAScript (`RegExpBuiltinExec` / `AdvanceStringIndex`), Java (**probe**), .NET (**probe**), Ruby (**probe**) |
| `ADVANCE_ONE_SKIP_ABUTTING` | as above, but an empty match abutting the previous match is not reported | Go (`regexp` documentation: "empty matches abutting a preceding match are ignored"); Rust (**probe**) |

### 5.11 Replacement templates

The template is parsed by a per-dialect grammar into a small sequence
(literal, group by number, group by name, whole match, prefix, suffix,
case operator), then applied; a template is validated at
`grx_regex_replace()` time against the regex's group count and names, with
the dialect's rule for a reference to a group that does not exist.

| Dialect | Group | Named | Whole / prefix / suffix | Escape | Missing group | Unset group | Case ops |
| --- | --- | --- | --- | --- | --- | --- | --- |
| ECMAScript (`String.prototype.replace`) | `$n`, `$nn` (1-99) | `$<name>` (only if the regex has named groups) | `$&`, `` $` ``, `$'` | `$$` | literal `$n` | empty | none |
| PCRE2 (`pcre2_substitute`) | `$n`, `${n}` | `$name`, `${name}` | none (extended: `$*MARK`) | `$$` | error | error, or empty with `SUBSTITUTE_UNSET_EMPTY` (exposed as an option) | extended mode: `\U \L \E \u \l`, and `${n:+a:b}`, `${n:-d}` |
| Perl (interpolation subset) | `$n`, `${n}`, `\n` (deprecated) | `$+{name}` | `$&`, `` $` ``, `$'` | `\$`, `\\` | empty (undef) | empty | `\U \L \E \u \l \Q` |
| Python (`re.sub`) | `\n`, `\g<n>` | `\g<name>` | `\g<0>` | `\\`; other C escapes processed | error | empty | none |
| Java (`appendReplacement`) | `$n` (longest valid prefix) | `${name}` | none | `\` quotes the next character | error | empty (**probe**) | none |
| .NET | `$n`, `${n}` | `${name}` | `$&`, `` $` ``, `$'`, `$+`, `$_` | `$$` | literal | empty | none |
| Ruby (`sub`) | `\n` | `\k<name>` | `\0`, `\&`, `` \` ``, `\'` | `\\` | empty | empty | none |
| Go, Rust | `$n`, `${n}` | `$name`, `${name}` - the name is parsed greedily, so `$1x` is the group named `1x` | none | `$$` | empty | empty | none |
| GNU sed | `\n` | none | `&` | `\&`, `\\` | error | empty | GNU: `\U \L \E \u \l` |
| Vim (`:s`) | `\n` | none | `&`, `\0` | `\&`, `\\` | empty | empty | `\u \U \l \L \e \E` |
| Tcl (`regsub`) | `\n` | none | `&`, `\0` | `\\`, `\&` | empty | empty | none |
| Emacs (`replace-match`) | `\n` | none | `\&` | `\\` | error | empty | none |

### 5.12 Character-class syntax

| Rule | POSIX/GNU | Perl/PCRE2 | ECMAScript | Python | Java | Ruby | RE2/Rust |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `]` first is a literal | yes | yes | no: `[]` is empty, `[^]` is everything | yes | **probe** | yes, with a warning | yes |
| Backslash inside brackets | literal | escape | escape | escape | escape | escape | escape |
| `-` literal at the ends | yes | yes | yes (legacy); `u`: yes; `v`: must be escaped | yes | yes | yes | yes |
| Class escape as a range endpoint, `[\d-z]` | n/a | PCRE2: error; Perl: `-` literal, with a warning | legacy: union; `u`: error | error (probed: Python 3.13 raises) | error | **probe** | error |
| `[[:alpha:]]` | yes | yes | no | no | no | yes | yes |
| Set operations | no | `(?[ ])`: `\|` `+` `&` `-` `^` `!`, nesting to 15 | `v`: `&&`, `--`, nesting, `\q{}` | no | `&&`, nesting | `&&`, nesting | Rust: `&&`, `--`, `~~`, nesting; RE2: no |
| Reserved double punctuators | - | - | `v`: `&&`, `!!`, `##`, ... must be escaped | - | - | - | - |

### 5.13 Quantifier syntax

| Rule | POSIX/GNU | Perl/PCRE2 | ECMAScript | Python | Java | Ruby | RE2/Rust |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `{,n}` | BRE: literal; ERE: undefined (glibc: literal) | `{0,n}` (Perl 5.34+, PCRE2 10.43+) | legacy: literal; `u`: error | `{0,n}` | error | `{0,n}` | RE2: literal; Rust: **probe** |
| `{` not starting a valid quantifier | literal | literal | legacy: literal; `u`: error | literal | error | literal | literal (RE2); error (Rust) |
| `a**`, `a+*` | GNU: allowed | error | error | error | **probe** | allowed with warning | error |
| Quantifier on an assertion | n/a | error (`(?=a)*`) | legacy: lookahead is quantifiable; `u`: error | error | **probe** | error | n/a |
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

| Dialect | Alphabet | Notes |
| --- | --- | --- |
| POSIX, GNU | none (API flags `REG_ICASE`, `REG_NEWLINE`) | `GRX_OPT_CASELESS`, `GRX_OPT_MULTILINE` |
| Perl | `msixxnpau` | `xx` is extended-more; `n` is no-capture |
| PCRE2 | `imsxnUJ` and the `(*...)` leading directives | `U` ungreedy, `J` dupnames |
| ECMAScript | `dgimsuvy` | `u` and `v` exclusive; `g`/`y` rejected here |
| Python | `aiLmsux` | `L` (locale) rejected as unsupported; `a` is ASCII |
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

### 5.16 Splitting

`grx_regex_split()` divides a subject at every match. The dialects disagree
on three things, and a caller who reimplements the loop themselves will get
at least one of them wrong.

| Rule | ECMAScript | Perl | Python | Java | Go, Rust |
| --- | --- | --- | --- | --- | --- |
| Capturing groups appear in the output | yes | yes | yes (`re.split` since 3.7) | no | no |
| An empty match where a piece begins | not a separator | **probe** | **probe** | **probe** | **probe** |
| An empty subject | one empty piece, or none if the pattern matches empty | **probe** | **probe** | **probe** | **probe** |
| Trailing empty pieces | kept | dropped unless a negative limit is given | kept | dropped unless a negative limit | kept |
| `limit` counts | pieces, captures included; 0 yields none | fields | splits, not fields | fields | splits, not fields |

ECMAScript's row is ECMA-262 22.2.6.14 and is implemented; every **probe**
is resolved by the work package that gives that dialect a front end, in the
same way §5.5's capture-reset cell was. Until then `grx_regex_split()` can
only be reached with an ECMAScript regex, because no other dialect compiles.

The ECMAScript rule in full, because the second row above is the one that
surprises people: the walk keeps a `piece_start`, and a match whose *end*
equals `piece_start` is skipped rather than ending a piece. That is what
makes `x*` split `"abc"` into three pieces rather than seven, and it is a
rule about the piece boundary rather than about the match - `a*` splitting
`"baac"` yields `b` and `c`, with the `aa` consumed as a separator and the
empty matches at either end of it ignored.

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
| Perl | Full case folding is simple folding | [design.md](design.md) §5.2 | - |
| Perl | `(?{ })`, `(??{ })`, `\N{name}` by name | code execution; name table size | `GRX_ERR_UNSUPPORTED` |
| PCRE2 | `(?{ })` is not a construct it has at all | pcre2test: "unrecognized character after (? or (?-" | `GRX_ERR_SYNTAX` |
| Perl | `(?[ ])` is PCRE2's grammar only | Perl's nests and takes different operands; a shared reader would accept neither exactly | `GRX_ERR_SYNTAX` |
| Perl | A capture set inside a *failed* negative lookahead is discarded | PCRE2 discards it and ECMA-262 22.2.2.4 says to; Perl keeps it | - |
| PCRE2 | Callouts `(?C...)` are read and have no effect | no callback API; a callout with no function registered changes no match, so accepting it answers the same question | - |
| PCRE2, Perl | `\X`, the extended grapheme cluster | the break rules are [plan.md](plan.md) WP-12's and are not generated yet; "any character" is not a grapheme cluster | `GRX_ERR_UNSUPPORTED` |
| PCRE2 | `(*script_run:`, `(*sr:`, `(*asr:` | each constrains what its body may match and an ordinary group does not | `GRX_ERR_UNSUPPORTED` |
| PCRE2, Perl | `\C`, one code unit | the subject here is code points, and a construct that can land inside a character has no honest approximation | `GRX_ERR_UNSUPPORTED` |
| PCRE2 | `(*LIMIT_MATCH=n)` and kin are accepted and not applied | the limits are the caller's and this front end has no writable copy; lowering one from inside a pattern is later work | - |
| PCRE2, Perl | `(?(VERSION>=n.n))` is answered against 10.46 | this library emulates that version rather than being it | - |
| Perl | The charset modifiers `a`, `aa`, `d`, `l`, `u` | one character-set semantics here, not five | `GRX_ERR_UNSUPPORTED` |
| Perl | `\b{wb}`, `\B{gcb}` and the other Unicode boundary escapes | the break rules again | `GRX_ERR_UNSUPPORTED` |
| POSIX | Submatch rules approximated in the first POSIX release | [design.md](design.md) §2 | - |
| POSIX | `[[.ch.]]` multi-character collating elements, `[[=e=]]` | no collation | `GRX_ERR_UNSUPPORTED` |
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

Below: all 17 pairs in the ReDoS corpus are refused in 105 to 173
milliseconds on an idle machine, and in 276 to 414 on the same machine with
six other cores busy - against plan.md WP-08's bound of one second. Both
ranges are recorded because the second is the one that matters: a bound is
worth having only if it holds when the machine is under load, which is when
an attack would be happening. Every pair is *answered* by the Pike VM or the
bit-state engine in under a millisecond at the same limits, which is the
point: a limit is a defence only because there is another engine that does
not need it.

`max_steps` stays at 10,000,000, and the numbers above are why rather than a
preference. Lowering it to a million would refuse a pathological pair in
about 15 ms instead of 150 - but would also cap a legitimate scan at 333 KB,
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

Verbs `(*ACCEPT)`, `(*FAIL)`, `(*COMMIT)`, `(*PRUNE)`,
`(*SKIP)`, `(*THEN)`, with and without names; leading directives
`(*UTF)`, `(*UCP)`, `(*CRLF)`, `(*LF)`, `(*ANYCRLF)`, `(*ANY)`, `(*NUL)`,
`(*NO_AUTO_POSSESS)`, `(*LIMIT_MATCH=d)` and kin (the limit directives map
onto `GRX_Limits` and may only lower a limit); `\K` disallowed in lookaround
by default; `(?|` branch reset; `(?J)` duplicate names; recursion `(?R)`,
`(?1)`, `(?-1)`, `(?+1)`, `(?&name)`, `(?P>name)`, `\g<1>`; conditionals on
group number, name, `R`, `Rn`, `R&name`, `DEFINE`, `VERSION>=n`, and on an
assertion; `\Q...\E` including inside classes; `(?x)` and `(?xx)` extended
modes; `\h \H \v \V \R \N \X`; `\C` (single code unit: refused,
`GRX_ERR_UNSUPPORTED`); auto-possessification is an optimisation and has no
semantic effect, so it is not modelled.

**Perl 5.40.** As PCRE2 minus verbs-with-arguments differences, plus:
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
and the profile says so with a **probe**); backreferences are undefined
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
