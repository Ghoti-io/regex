# The master design

**Status:** design. Everything below the dialect table is unwritten; this
page says what will exist and why, so the work can be split across teams
without each team first having to agree the architecture with the others.
Where it disagrees with the scaffold as committed - the headers, the internal
headers, `development.md` - this page wins, and the scaffold changes to match
in the same commit as the code that needs it.

Companion pages, each owned by this one:

| Page | What it settles |
| --- | --- |
| [dialects.md](dialects.md) | What each syntax accepts and what it means; the semantic profile per dialect; deviations |
| [unicode.md](unicode.md) | The Unicode data: which version, which tables, how they are generated and checked |
| [testing.md](testing.md) | How correctness is established: oracles, conformance vectors, cross-engine checks, fuzzing |
| [plan.md](plan.md) | The order of work, the work packages, and what "done" means for each |

## 1. What the library is for

The brief is a C library that is correct, stable, full-featured, safe, robust
and useful, across every regular-expression dialect a programmer is likely to
meet. Each of those words is a mechanism in this design, and it is worth
saying which, because a design that only promises them is a design nobody can
check against the code.

| Property | Mechanism |
| --- | --- |
| **Correct** | Semantics are written down per dialect *before* they are coded ([dialects.md](dialects.md)), every dialect is checked against the real implementation it names ([testing.md](testing.md) §2), and every pattern both engines can run is checked to produce identical captures from both (§3.5). |
| **Stable** | `GRX_Regex` is immutable and carries no match state. Dialect behaviour is data (§4), so correcting one dialect cannot change another. Symbols are versioned per `CONVENTIONS.md` §4. |
| **Full-featured** | Named groups, lookbehind of any length, Unicode properties, class set operations, atomic groups, conditionals, recursion, backtracking verbs, substitution with per-dialect templates. Feature bits per dialect say which of these a syntax *has*; the library implements the union. |
| **Safe** | Time and memory are bounded by default (§6). No engine recurses on the C stack in proportion to the subject or the program. Every allocation goes through the caller's allocator. Both the pattern and the subject are fuzzed, because both are untrusted (§1.2). |
| **Robust** | A limit is a promise: exceeding one is `GRX_ERR_LIMIT`, never a truncated result and never a hang. Invalid input at any layer is an error with a position, not undefined behaviour. |
| **Useful** | The first consumer exists today (§1.1). The library says *which* engine can run a pattern, so a caller can ask for the linear-time one by name. Errors carry an offset and a message a user can be shown. |

### 1.1 The first consumer

`text`'s JSON Schema engine refuses schemas that use `pattern` or
`patternProperties`, and its header says why: "these need a regular-expression
engine, which is a dependency decision rather than an implementation detail"
(`text/include/ghoti.io/text/json/json_schema.h`). This library is that
engine. The dialect it needs is fixed by the specification:

> JSON Schema 2020-12 core, §6.4 *Regular Expressions*: keywords that use
> regular expressions "SHOULD be valid according to the regular expression
> dialect described in ECMA-262, section 21.2.1", "SHOULD be built with the
> `u` flag (or equivalent)", and "implementations MUST NOT take regular
> expressions to be anchored, neither at the beginning nor at the end".

(ECMA-262 §21.2.1 is the ES2015 numbering; the same grammar is clause 22.2.1
*Patterns* in the current edition.) So the first dialect is
`GRX_SYNTAX_ECMASCRIPT` with `GRX_OPT_UTF`, searched rather than matched.
§6.4 also lists the subset schema authors "SHOULD limit themselves to" -
characters, classes, the simple and range quantifiers and their lazy forms,
`^`, `$`, groups and alternation - which is the subset that is regular in the
formal sense and so the subset the linear-time engine runs. That is not a
coincidence and it is the reason the engine split in §3.5 is worth having.

The integration is a decision for the `text` team and is recorded in
[plan.md](plan.md) as its own work package, but the design constraint is
stated here: `text` depends on nothing, and this library depends on `cutil`.
`text` should therefore not link this library. It should accept a
regular-expression *provider* - a small vtable in `GTEXT_JSON_Schema_Options`
with compile, search and free entries - and the application supplies one
backed by `ghoti.io-regex`. `text` stays standalone, this library stays
optional, and a third party could supply PCRE2 through the same seam.

### 1.2 The threat model

A schema is often loaded from a file the application did not write. The
patterns in it are therefore untrusted input, and a regular expression is the
one kind of untrusted input where a *small* input can cost *unbounded* time:
`(a+)+$` against thirty `a`s and a `b` is the standard demonstration, and it
is a real class of vulnerability with a name (ReDoS). The rest of the suite
fuzzes bytes against a fixed parser; here the bytes *are* the program.

The design consequences:

- The pattern is untrusted: the parser is bounded in depth, node count and
  pattern length, and is fuzzed.
- The subject is untrusted: the engines are bounded in steps and stack, and
  are fuzzed with the pattern held fixed and with both varying.
- The *combination* is untrusted: a pattern that is fine on every subject the
  author tried can be pathological on one an attacker chooses. The default
  engine for a regular pattern is the one whose worst case is linear, and the
  backtracking engine's worst case is capped by `max_steps`, so that the
  answer to a pathological pair is `GRX_ERR_LIMIT` in bounded time rather than
  a stalled process.
- Nothing recurses on the C stack in proportion to input. The parser's
  recursion depth is `max_nesting_depth` (a small number; §6.2); the engines
  use explicit stacks the allocator provides and the limits cap.

## 2. Non-goals for 1.0

Stated so that nobody builds them by accident and so that the reason is on
record when one of them is wanted.

- **A JIT.** The engines are interpreters. A JIT is a large, platform-specific
  piece of work that changes nothing about correctness, and the suite has no
  other platform-specific code of that kind.
- **Streaming or non-contiguous subjects.** A subject is one buffer. The
  rest of the suite streams because its input is a file of unknown size; a
  subject here is a string the caller already holds. (`development.md`
  records the same decision for the scaffold.)
- **UTF-16 or UTF-32 subjects.** Subjects are UTF-8 or bytes (§5.1). Offsets
  are byte offsets. ECMAScript's `lastIndex` and the `d` flag's indices are
  UTF-16 code-unit offsets; that is a documented deviation, and a caller who
  needs them converts. Lone surrogates, which a JavaScript string can hold,
  cannot occur in UTF-8 and so cannot be matched; also documented.
- **Locale-dependent behaviour.** Nothing consults `LC_*`. POSIX collating
  elements (`[[.ch.]]`) and equivalence classes (`[[=a=]]`) are accepted only
  for single characters. Every dialect's "current locale" is Unicode.
- **Perl code blocks** `(?{ ... })` and `(??{ ... })`. They run Perl.
- **.NET balancing groups** `(?<open-close>...)` are deferred to the .NET
  tier ([plan.md](plan.md)); they are a capture-stack semantics no other
  dialect has.
- **Partial matching** (PCRE2's `PCRE2_PARTIAL_SOFT`/`HARD`). Reserved as a
  future flag on the search request; nothing in the engines precludes it.
- **Exact POSIX submatch rules in 1.0.** The overall leftmost-*longest*
  match is exact from the first POSIX release. Which substrings the
  parenthesised subexpressions report under POSIX's "each subexpression, from
  left to right, longest possible consistent with the whole match" rule is a
  known-hard problem (Laurikari, Okui-Suzuki); the first release documents its
  approximation as a deviation and a later work package makes it exact.

## 3. The pipeline

```
   pattern text  +  dialect  +  options
          │
          ▼
   ┌─────────────┐  profile-driven lexer and recursive-descent parser
   │  front end  │  (src/parse, reads src/syntax)
   └─────────────┘
          │  GRX_Pattern: the AST - what the text SAYS, dialect-shaped
          ▼
   ┌─────────────┐  resolve options, fold case, canonicalise classes,
   │  lowering   │  number and name groups, resolve references,
   └─────────────┘  mark lookbehind bodies as reverse (src/ir)
          │  IR: what the pattern MEANS, dialect-free
          ▼
   ┌─────────────┐  is it regular? min/max length, anchoring, literal
   │  analysis   │  prefix, first-byte set, can-match-empty (src/ir)
   └─────────────┘
          │  facts
          ▼
   ┌─────────────┐  IR to instructions; counted repeats expanded under
   │  codegen    │  max_program_size (src/compile)
   └─────────────┘
          │  GRX_Program
          ▼
   GRX_Regex = program + facts + capture names + the profile it was built under
          │
          │   subject + search request
          ▼
   ┌─────────────┐  engine selection from facts and the request
   │  exec       │  Pike VM  |  bit-state backtracker  |  backtracker
   └─────────────┘  (src/exec)
          │
          ▼
   GRX_Match: capture spans, which engine ran, step count
```

The invariant the whole design rests on: **the dialect is gone after
lowering.** The engines never consult `GRX_Syntax`, a `GRX_SyntaxSpec`, or
anything derived from one; every dialect-dependent decision has been made by
the time the IR exists, and it is encoded in the IR as an explicit node,
flag or instruction. Two consequences:

- A dialect bug is a front-end or lowering bug, and is fixed there. An
  engine bug affects every dialect equally and shows up in the cross-engine
  check (§3.5) regardless of which dialect found it.
- Adding a dialect adds no code to an engine. If it seems to, the construct
  it needs is missing from the IR, and that is what gets added.

### 3.1 The front end: text to AST

One recursive-descent parser, parameterised by the dialect's *profile* and
*hooks* (§4), producing a `GRX_Pattern`. The AST is what the text says, with
the dialect's spelling resolved but its semantics not yet applied: `\(` in
BRE and `(` in ERE are both `GRX_NODE_GROUP`, but a literal under `(?i)` is
still a literal, and a class is still a list of the items the user wrote.

The AST keeps that fidelity because three consumers need it and the engines
are not among them: `grx_pattern_dump()` (tests and debugging), a future lint
(the JSON Schema subset check, §1.1), and a future translator between
dialects. All three want to know what was written, not what it compiles to.

Shape, keeping the scaffold's choice: a node array with index links, one
allocation that grows, so a pattern is freed as a unit and every child
reference is bounds-checkable. Node kinds are the scaffold's plus what the
dialects need and it lacks:

| Kind | For |
| --- | --- |
| `GRX_NODE_EMPTY`, `LITERAL`, `CLASS`, `ANY`, `CONCAT`, `ALTERNATE`, `REPEAT`, `GROUP`, `BACKREF`, `ANCHOR`, `LOOKAROUND`, `CONDITIONAL`, `RECURSE`, `CONTROL` | as scaffolded |
| `GRX_NODE_OPTIONS` | an inline flag change, `(?i)` bare or `(?i:...)` scoped; carries set/clear masks and applies to what follows in its group |
| `GRX_NODE_CLASS_OP` | a set operation on classes: union, intersection (`&&`), subtraction (`--`), nested class - ECMAScript `v`, Java, Rust, Ruby |
| `GRX_NODE_STRING_SET` | a class item that is a *string*, not a code point: ECMAScript `v`'s `\q{abc\|de}` and the properties of strings such as `\p{RGI_Emoji}` |
| `GRX_NODE_KEEP` | `\K`, which resets the reported match start |
| `GRX_NODE_BRANCH_RESET` | `(?\|...)`, which renumbers captures per alternative |

Per node: kind, flags, the byte offset and length in the pattern (every
error and every dump reports a position), the payload for its kind, and the
`min`/`max`/greediness for a repeat. Literal runs, class item lists and
group names live in side tables the node indexes.

### 3.2 Lowering: AST to IR

The IR is the same node-array shape with a different vocabulary, and it is
the point at which the dialect's semantics are applied and then discarded:

- **Options are resolved away, not carried.** An earlier draft of this page
  said each IR node knows the effective caseless/multiline/dotall bits it was
  written under. Building it showed that is weaker than what the design
  actually needs and than what is achievable: every option becomes something
  explicit instead. Caseless folds literals into classes; multiline chooses
  between the subject and line assertion kinds; dot-all chooses what `ANY`
  excludes; extended and literal are consumed by the lexer; ungreedy chooses a
  repeat mode. `GRX_NODE_OPTIONS` does not survive, and neither does an
  options field. The single survivor is UTF mode, which decides whether a step
  is a byte or a code point and so belongs to the whole program rather than to
  any node. A node that still needed to know its options would be a node an
  engine has to interpret two ways, which is the thing §3's invariant
  forbids.
- **Case is folded at compile time.** Under caseless, a literal becomes a
  class containing its fold orbit (every code point with the same simple
  case fold), and a class is closed under the same operation. The fold
  function is the dialect's ([dialects.md](dialects.md) §5.8; ECMAScript
  without `u` folds differently from everything else). The engines never
  fold: a caseless match is an ordinary class match.
- **Classes are canonical:** sorted, disjoint code-point ranges, negation
  applied, set operations evaluated, `\d`/`\w`/`\s`/POSIX/property items
  expanded from the Unicode tables under the dialect's definition of each.
- **Groups are numbered and named** under the dialect's rule (branch reset,
  duplicate names), and every backreference, conditional and recursion
  target is an index.
- **Lookbehind bodies are marked reverse.** They compile to instructions
  that step backwards through the subject (§3.4); this is how lookbehind of
  arbitrary length works without a second engine, and it is the approach
  the ECMAScript specification itself describes.
- **Repeats are normalised** to `{min,max}` plus one of greedy, lazy,
  possessive, with `?`/`*`/`+` gone.
- **Semantic rules become nodes.** The empty-iteration rule and the
  capture-reset-per-iteration rule ([dialects.md](dialects.md) §5.5) are
  attached to the repeat node as explicit modes, so the engines implement a
  mode, not a dialect.

A small set of simplifications runs here - `x{0}` to empty, empty
non-capturing groups removed, adjacent literals merged - and every one of
them has a written justification and a test, because the dialects' rules for
empty iterations mean that rewrites which are "obviously" equivalent are not:
`(a*)*` and `(a*)?` report different captures under ECMAScript.

### 3.3 Analysis: facts about the program

Computed once at compile time, stored in the `GRX_Regex`, and exposed to the
caller (§7):

| Fact | Used for |
| --- | --- |
| `is_regular` | no backreference, lookaround, atomic group, possessive quantifier, conditional, recursion, verb or `\K`: the Pike VM can run it, and the caller can be promised linear time |
| `min_length`, `max_length` (or unbounded) | rejecting a subject that is too short without running; sizing lookbehind |
| `anchored_start`, `anchored_end` | skipping the unanchored-search loop |
| `can_match_empty` | the iteration rule in `grx_regex_search_next()` |
| literal prefix, required literal, first-byte set | prefilters (§3.5.5), which are a later phase but whose slot exists from the first commit so that adding them changes no interface |
| capture count, names, name-to-index map, duplicate-name groups | the match API |
| program size, lookbehind maximum, recursion present | limits and engine selection |

### 3.4 The program

One instruction set for every engine. The scaffold's opcodes are the core;
the additions are what the dialects need:

| Group | Instructions |
| --- | --- |
| **Consuming** | `CHAR c`, `CLASS id`, `ANY` (with the dialect's newline set), `ANY_NL`. Each carries a *direction* bit; a reverse instruction steps back one code point instead of forward, which is all a lookbehind body needs. |
| **Control** | `SPLIT x, y` (try `x` first), `JMP`, `MATCH` |
| **Captures** | `SAVE n` |
| **Assertions** | `ASSERT kind`: start/end of subject, start/end of line (dialect newline set), end-or-before-final-newline (`\Z`), word/non-word boundary (dialect word set), `\G` |
| **Loop guards** | `PROGRESS_SET r` records the position in per-thread register `r` at loop entry; `PROGRESS_CHECK r, mode` implements the dialect's empty-iteration rule at loop end (fail, or exit the loop, or continue); `RESET lo, hi` clears a span of capture slots, which is the other half of §5.5 - ECMAScript clears the captures inside a repeat at each iteration and the Perl family does not. `RESET` was added during WP-07: the rule has no other representation, because a rewrite of the loop cannot say "clear these on entry but keep them if the loop exits here". |
| **Backtracking only** | `BACKREF n` (with caseless and unset-group mode), `LOOK kind, body` (ahead/behind, positive/negative), `ATOMIC_BEGIN`/`ATOMIC_END`, `COND kind, n`, `CALL n`/`RET` (recursion and subroutines), `KEEP`, `VERB kind` (`(*FAIL)`, `(*ACCEPT)`, `(*COMMIT)`, `(*PRUNE)`, `(*SKIP)`, `(*THEN)`) |

Counted repetition `{m,n}` is compiled by expansion in both engines, bounded
by `max_program_size`, so that a `{1000}` on a large group is
`GRX_ERR_LIMIT` at compile time rather than a surprise at match time. This
is what RE2 does, and it keeps the Pike VM free of counters. It is also a
documented deviation from ECMAScript, whose grammar admits a repeat count of
2^53 - 1 ([dialects.md](dialects.md) §7).

`GRX_Inst` grows from the scaffold's `{op, x, y}` to carry the direction bit
and a mode byte; it stays fixed-size so the program is one array.

### 3.5 The engines

There are three, and the third is not a new algorithm:

| Engine | Runs | Cost | Guarantees |
| --- | --- | --- | --- |
| **Pike VM** | the regular subset | O(subject × program) time, O(program) memory | linear in the subject; never `GRX_ERR_LIMIT` for time unless `max_steps` is set below `subject × program` |
| **Bit-state backtracker** | anything without a backreference, lookaround or recursion, when `program × subject` fits a memory budget | O(subject × program) time, O(subject × program / 8) memory | linear; leftmost-first captures exactly as the backtracker would report them |
| **Backtracker** | everything | exponential worst case, capped by `max_steps` and `max_backtrack` | terminates; `GRX_ERR_LIMIT` when the cap is hit |

`GRX_ENGINE_AUTO` picks the first row that applies. A caller who names
`GRX_ENGINE_PIKE` gets `GRX_ERR_UNSUPPORTED` for a program the Pike VM cannot
run, never a quiet substitution.

#### 3.5.1 Pike VM

Cox's lockstep simulation: two thread lists as sparse sets keyed by program
counter, so each `(pc, position)` pair is visited at most once per position
and the bound is structural rather than counted. Threads are ordered by
priority, which is what makes the result leftmost-*first*; a leftmost-*longest*
mode for the POSIX dialects keeps running after the first `MATCH` and reports
the last one. Per-thread state is the capture array plus the progress
registers, reference-counted and copied on write, so a `SPLIT` costs a
pointer and a `SAVE` costs a copy only when the array is shared.

The subject is decoded once per position, not once per thread.

Unanchored search adds a thread at the program's entry point at each
position, rather than compiling a leading `SPLIT` loop over `ANY_NL` as this
page first specified. The two are equivalent and the second is simpler: the
sparse set already refuses a program counter that is occupied, so the search
costs no more than the match and the linear bound holds for both. Compiling
the loop instead would mean two programs per pattern, or one program that an
anchored match has to enter past its own prologue.

#### 3.5.2 Backtracker

An explicit stack of frames, never the C stack. Frame kinds: an alternative
to resume (`pc`, position), a capture to undo, a progress register to undo,
an atomic barrier (pop through to it and discard on `ATOMIC_END`), a
lookaround frame (the position to restore and the polarity), a call frame
(return address and the capture frame for recursion), a verb marker.
`max_backtrack` caps the stack depth; `max_steps` caps instructions executed.
Either exhausted is `GRX_ERR_LIMIT` with the count in the match object, and
is never reported as "no match", because "no match" is a fact about the
subject and a limit is a fact about the budget.

Lookbehind runs the body's reverse-direction instructions from the current
position backwards; a lookbehind body may itself contain anything the
backtracker runs, including captures, which is the ECMAScript semantics and
a superset of every other dialect's.

#### 3.5.3 Bit-state backtracker

The backtracker plus a visited bitmap over `(pc, position)`: a state already
tried is not tried again, which turns the exponential case into a linear
one for any program whose behaviour at `(pc, position)` does not depend on
history - that is, any program without backreferences (whose success depends
on earlier captures) or lookaround (whose body would need its own bitmap) or
recursion. RE2 uses exactly this for small programs because it delivers
leftmost-first captures more cheaply than the Pike VM. Here its second value
is that it extends the linear-time guarantee to atomic groups, possessive
quantifiers and conditionals on group-set, none of which the Pike VM can run.
The memory budget is `max_match_memory` (§6.2), and a bitmap that will not
fit in it is `GRX_ERR_LIMIT` rather than a silent fall back to the engine
that can hang.

**Built.** One condition was missing from the list above and is worth
naming, because it is not obvious: a program carrying an *empty-iteration
guard* is also not memoizable. The guard's progress register records where
the current iteration began, and two paths reaching the same instruction at
the same position with different register values behave differently - so
the memo would skip a state that had not really been tried. Codegen
therefore emits the guard only for a repeat whose body can match the empty
string, which is both the condition for needing it and, as `registers=0` in
a disassembly, the way to see that a program is memoizable.

#### 3.5.4 The equivalence invariant

Any pattern and subject that two engines can both run must yield the same
`matched`, the same group 0, and the same spans for every group, from both.
This is checked in the unit tests on every conformance vector
([testing.md](testing.md) §4) and by a fuzz harness that runs all eligible
engines and aborts on disagreement. It is the cheapest strong test the
library has, because the two implementations share nothing below the program,
and it is why the Pike VM carries progress registers rather than relying on
a rewrite: the captures have to come out the same.

#### 3.5.5 Later: prefilters and a lazy DFA

Both are speed, not correctness, and are phased after every dialect in tier
one is conformant ([plan.md](plan.md)). Their interfaces are fixed now: the
facts in §3.3 carry the literal prefix, the required literal and the
first-byte set, and engine selection consults them. A lazy DFA (RE2's
strategy: DFA for "is there a match and where does it end", then the Pike VM
or bit-state on the bounded span for captures) would be a fourth row in the
engine table with the same equivalence obligation.

### 3.6 Search semantics

Two entry points, as scaffolded: `grx_regex_search()` finds the leftmost
match at or after `start`; `grx_regex_match()` requires the match to begin
at `start` (ECMAScript's `y`, PCRE2's `PCRE2_ANCHORED`). Both take a search
request (§7) that adds what the scaffold lacks:

- **A window.** `start` and `end` bound the search within a larger buffer,
  with lookbehind still able to see before `start` and `$` still meaning the
  end of the buffer unless told otherwise. This is what makes iteration and
  "search this field of a larger record" correct without copying.
- **`NOTBOL` / `NOTEOL`:** `^` and `$` do not match at the window edges,
  for a caller feeding a buffer in pieces.
- **`NOTEMPTY` / `NOTEMPTY_ATSTART`:** an empty match is refused (anywhere,
  or at `start`), which is one of the two iteration rules below.
- **`NO_UTF_CHECK`:** the caller guarantees the subject is valid UTF-8, and
  the O(n) validation is skipped (§5.1).

**Iteration.** `grx_regex_search_next()` advances from a previous match and
applies the dialect's rule for an empty match, of which there are two in the
wild: retry at the same position refusing an empty match, then advance one
character (Perl, PCRE2, Python), or advance one character unconditionally
(ECMAScript's `lastIndex`, Go). The profile names which
([dialects.md](dialects.md) §5.10); the caller does not have to know.

**"No match" is an outcome, not an error.** `out_matched` carries it; the
result code is reserved for things that went wrong, and `GRX_ERR_LIMIT` is
one of them.

## 4. The dialect model

The scaffold represents a dialect as a set of feature bits. That is
necessary and not sufficient: feature bits say which constructs a syntax
*has*, and most of what separates two dialects is what a construct *means*
once both have it. Three examples that a feature bit cannot express:

- `\1` in ECMAScript without `u` is a backreference if there is a group 1
  and a legacy octal escape otherwise; in PCRE2 it is a backreference, and
  `\01` is octal; in Python `\1` is a backreference and octal needs three
  digits. All three have `BACKREFERENCE` and `OCTAL_ESCAPE`.
- `(a*)*` against `b`: ECMAScript reports group 1 as unset, Perl reports it
  as empty. Both have `BOUNDED_REPEAT` and `NON_CAPTURING`.
- `$` in Ruby matches at every line end whether or not a multiline flag is
  set; in PCRE2 it matches at the end of the subject or before a final
  newline; in ECMAScript it matches at the end only. All three spell it `$`.

So a dialect is three things, and the scaffold's `GRX_SyntaxSpec` grows into
the first two of them:

1. **Features** - which constructs exist. The `GRX_Feature` bits, extended
   as [dialects.md](dialects.md) §3 lists. A construct the dialect lacks is
   `GRX_ERR_SYNTAX` if the dialect gives the characters another meaning
   (`+` is a literal in POSIX BRE), or a literal if the dialect says so
   (`{` in ECMAScript without `u`), and `GRX_ERR_UNSUPPORTED` only when the
   dialect has it and this library does not yet.
2. **The semantic profile** - what the constructs mean. A struct of small
   enums, one per axis on which real implementations differ: match
   preference, empty-iteration rule, capture reset in loops, unset
   backreference behaviour, lookbehind constraint, newline set, `$` rule,
   `.` rule, case-fold rule, the definitions of `\w`/`\d`/`\s`, the
   backreference-versus-octal rule, property-name matching, the iteration
   rule, the replacement-template grammar. The complete list with every
   dialect's value is [dialects.md](dialects.md) §5, and filling that table
   from the real implementations is a work package in its own right
   ([plan.md](plan.md) WP-03).
3. **Hooks** - code, for the syntax that is not expressible as data. A
   `GRX_Frontend` vtable with a default implementation (the Perl family) and
   overrides where a family's *lexical* rules differ: the POSIX/GNU family
   (escaped operators, no escapes inside brackets), ECMAScript (Annex B
   legacy grammar, the `u` and `v` modes), Python, Java, .NET, Ruby, the
   RE2/Rust family, Tcl (advanced-RE directors), Vim (the four "magic"
   levels), Emacs (syntax-class escapes). A hook decides how to *read* a
   construct; it never decides what the construct *means* - that is the
   profile's job, and keeping the two apart is what keeps the hooks small.

Two rules about what a dialect is not:

- **Not a superset.** A dialect that accepts everything is a bug. A caller
  uses this library to learn whether a pattern is valid *for that engine*;
  accepting PCRE's `(?>...)` under `GRX_SYNTAX_ECMASCRIPT` would tell them it
  is, and it is not.
- **Not a family.** `GRX_SYNTAX_PERL` and `GRX_SYNTAX_PCRE` stay separate
  because PCRE2 has verbs and `\K` restrictions Perl does not, and Perl has
  full case folding PCRE2 does not. Where two names genuinely agree, they
  share a profile row rather than being merged.

Each dialect is pinned to a version of the implementation it names
([dialects.md](dialects.md) §2). The versions are the ones this machine can
run as oracles, because a dialect the library cannot check against its
reference is a dialect the library cannot claim.

**Options** are dialect-independent bits, as scaffolded, plus
`grx_options_parse(syntax, "imsu", &bits)` for a flag string in the
dialect's own alphabet, because every consumer has one of those strings and
should not have to know that `s` is dot-all in PCRE and multiline in Ruby.

## 5. Unicode and encodings

Details are in [unicode.md](unicode.md); the decisions that shape the rest
of the design are here.

### 5.1 Two subject encodings

- **UTF mode** (`GRX_OPT_UTF`): pattern and subject are UTF-8; consuming
  instructions step by code point; `.` is a code point; classes are over
  Unicode. The subject is validated once per search, O(n), before the
  engines run, unless `NO_UTF_CHECK` is given; an invalid subject is
  `GRX_ERR_INVALID`, on the reasoning that the caller declared an encoding
  the bytes do not have, which is a wrong argument. The decoder is strict
  ([unicode.md](unicode.md) §2): no overlongs, no surrogates, nothing above
  U+10FFFF, because an engine that decoded those would match against code
  points the subject does not contain.
- **Byte mode** (default): every byte is a code point 0-255. Nothing is
  validated. `\p{...}` and the Unicode definitions of `\w` still work, over
  U+0000-U+00FF.

Which mode a dialect *defaults* to is in its profile; the caller can always
override. There is no third mode, and in particular no "decode leniently"
mode, because there is no correct answer to what a malformed sequence
matches.

### 5.2 Case folding is a compile-time operation

§3.2. The Unicode module provides the simple fold, the fold *orbit* (every
code point sharing a fold), and the per-dialect variants; the engines never
call any of them. A dialect whose real implementation does *full* folding
(Perl and Ruby match `ß` against `ss` under `/i`) gets simple folding here
and a recorded deviation, because a fold that changes the length of what was
matched cannot be expressed as a class and would need a different engine.

### 5.3 Properties and names

`\p{...}` supports General_Category, Script, Script_Extensions and the
binary properties ECMA-262 enumerates, which is the largest list any dialect
requires. Name matching is per profile: ECMAScript requires the exact
canonical name or alias, case-sensitive; Perl and PCRE2 match loosely per
UAX #44 (case, spaces, hyphens and underscores ignored). The tables are
generated from a pinned UCD release, committed, and checked by a Makefile
target that regenerates and diffs when the UCD is present
([unicode.md](unicode.md) §4).

## 6. Memory, limits, threads, errors

### 6.1 Allocation

Every byte comes from the `GRX_Allocator` the caller passed (`NULL` is the
default), per `CONVENTIONS.md` §5. Three ownership units:

- **`GRX_Pattern`**: the node array, the side tables. One growable arena.
- **`GRX_Regex`**: the program, the class table, the facts, the names. Built
  once, then read-only.
- **`GRX_Match`**: the capture array *and every engine's scratch* - the Pike
  VM's thread lists sized to the program, the backtracker's frame stack, the
  bit-state bitmap. Scratch is allocated on first use and kept, so that a
  caller matching in a loop allocates on the first iteration and never again
  for the same regex. This is why `GRX_Match` is created *for* a regex.

`cutil`'s typed vectors (`gcu_vector32` and kin) serve the parser's
integer-indexed tables; the instruction array and the ranges are their own
growable buffers because their element types are structs. Group names go in
a `gcu_hash64` keyed by the string hash.

Nothing is allocated for the caller to free on a failing call, and the
`CountingAllocator` in `tests/test_helpers.h` is how every failure path
proves it.

### 6.2 Limits

Every field of `GRX_Limits` is a promise about a specific quantity, enforced
at a specific place, and reported as `GRX_ERR_LIMIT` with the field named
in the error message. The scaffold's ten fields, plus three the engines
need:

| Field | Enforced in | Bounds |
| --- | --- | --- |
| `max_pattern_length` | entry | bytes of pattern |
| `max_nesting_depth` | parser | recursion depth of groups, classes and lookarounds - this is also the C stack bound, so its default is small (a few hundred) and it is the one limit a caller should not lift casually |
| `max_nodes` | parser | AST nodes |
| `max_captures` | lowering | capturing groups |
| `max_repeat_count` | parser | any bound in `{m,n}` |
| `max_class_ranges` | lowering | ranges in one canonical class, after expansion of properties and folding |
| `max_program_size` | codegen | instructions, after repeat expansion |
| `max_lookbehind_length` | analysis, after lowering | maximum length of a lookbehind body. `GRX_NPOS` for an unbounded body, which exceeds every finite cap. A *caller's* policy rather than a dialect's rule, so its default is 0; a dialect that bounds its own lookbehind enforces that through its profile instead |
| `max_recursion_depth` | *reserved* | nested `CALL` frames. Nothing reads it: no dialect here has recursion, which arrives with WP-18. `tests/unit/test_limits.cpp` fails the moment one does |
| `max_subject_length` | entry | bytes of subject |
| `max_steps` | all engines | instructions executed in one search |
| `max_backtrack` | backtracker | frames on the stack |
| `max_match_memory` *(new)* | exec | bytes of scratch a match object may grow to; selects bit-state eligibility |

Every field reads 0 as "no limit", `grx_limits_unlimited()` fills in the
all-zero structure, and
`tests/unit/test_limits.cpp:EveryEnforcedLimitRefusesWhenTightAndCapsNothingAtZero`
checks both halves of that per field rather than leaving it a sentence.

Defaults are non-zero for every field except the two noted above, because a
regular expression is the one input whose cost is not bounded by its size. The
default values are measured, not guessed: [plan.md](plan.md) WP-14 sets them
from the conformance corpus so that no pattern in any oracle's own test suite
hits a default limit, and from the ReDoS corpus so that every known
pathological pair does.

### 6.3 Threads

`GRX_Regex` and `GRX_Pattern` are immutable after construction and may be
shared. `GRX_Match` is used from one thread at a time, and holds the scratch
that makes this the natural unit of per-thread state. Process-wide state is
the default allocator and the constant tables; nothing else.

### 6.4 Errors

`GRX_Error` grows from the scaffold's `{code, offset, message}` by two
fields: `length` (the span of the offending construct, so a caller can
underline it) and `diag`, an enum identifying the specific diagnostic, so
that tests assert `GRX_DIAG_UNMATCHED_PAREN` and not a string, and so the
strings can be improved without breaking anything. Every `GRX_ERR_SYNTAX`,
`GRX_ERR_UNSUPPORTED` and compile-time `GRX_ERR_LIMIT` carries an offset. A
match-time limit carries the exhausted count in the match object instead;
there is no meaningful pattern offset for "ran out of steps".

The result vocabulary stays as scaffolded. Three of the codes carry a
precise meaning here that is worth restating: `GRX_ERR_SYNTAX` is "this
dialect rejects this text"; `GRX_ERR_UNSUPPORTED` is "this dialect accepts
it and this library does not yet"; `GRX_ERR_INVALID` is a wrong argument,
which includes a subject that is not the encoding the caller declared.

## 7. The public API: what changes from the scaffold

The scaffold's surface is kept. This is what is added, and the two structs
that grow. The library is at 0.0.0 and has no consumers, so growth is free
now and will not be later.

| Header | Change |
| --- | --- |
| `syntax.h` | `GRX_SyntaxSpec` gains the profile fields ([dialects.md](dialects.md) §5). `GRX_Feature` gains bits for branch reset, `\K`, string classes, duplicate names, `\G`, `\R`, `\X`, `\N`, `\h`/`\v`, callouts-rejected. `GRX_Option` gains `UNICODE_SETS` (ECMAScript `v`), `DOLLAR_ENDONLY`, `NO_UTF_CHECK`, `DUPNAMES`. `grx_options_parse()`. |
| `core.h` | `GRX_Error` gains `length` and `diag`; `GRX_Diag` enum; `grx_diag_string()`. `GRX_Limits` gains the three fields in §6.2. |
| `compile.h` | `GRX_Facts` and `grx_regex_facts()`: the analysis results of §3.3, so a caller can ask "is this regular" before deciding to run it. |
| `exec.h` | `GRX_SearchOptions` (window, flags, engine, limits) and `grx_regex_search_ex()` / `grx_regex_match_ex()`; `grx_regex_search_next()`; `grx_match_steps()`; `grx_match_span()` convenience for group 0. |
| `subst.h` *(new)* | `grx_regex_replace()` with the dialect's template grammar, `grx_regex_split()`. See [dialects.md](dialects.md) §5.11 for the template decisions. |
| `unicode.h` *(new, small)* | `grx_utf8_validate()`, so a caller can find the offset of a bad sequence after `GRX_ERR_INVALID`. Nothing else from the Unicode module is public. |
| `pattern.h` | unchanged; `grx_pattern_lint()` reserved for the JSON Schema subset check, a later package. |

Nothing is removed. `grx_regex_search()` and `grx_regex_match()` become thin
wrappers over the `_ex` forms with default options.

## 8. Code layout

The target tree, with the lane in [plan.md](plan.md) that owns each part.
The scaffold's tree is a subset of this; directories appear when their first
file does.

```
include/ghoti.io/regex/
  core.h syntax.h pattern.h compile.h exec.h subst.h unicode.h version.h regex.h

src/core/        result strings, limits, diagnostics, allocator     [core]
src/unicode/     utf8.c casefold.c props.c names.c                  [unicode]
src/unicode/tables/   generated from the UCD; never edited by hand  [unicode]
src/charclass/   range sets: canonicalise, union, intersect,
                 subtract, negate, fold-close                       [core]
src/syntax/      the profile table, one file per family's hooks:
                 syntax.c perl.c posix.c ecmascript.c python.c
                 java.c dotnet.c ruby.c re2.c tcl.c vim.c emacs.c   [front ends]
src/parse/       lexer.c parser.c class_parse.c ast.c dump.c        [core, then front ends]
src/ir/          lower.c fold.c simplify.c analyze.c                [core]
src/compile/     codegen.c program.c dump.c                         [core]
src/exec/        exec.c pike.c backtrack.c bitstate.c
                 sparse.c (sets) stack.c (frames)                    [engines]
src/subst/       template.c replace.c split.c                       [surface]
src/regex.c      version                                            [core]

tests/unit/            one file per module, as scaffolded
tests/conformance/     the vector runner and the cross-engine check
tests/data/vectors/    <dialect>/*.rxt - generated from oracles, committed
tests/data/redos/      known pathological pattern/subject pairs
tests/fuzz/            fuzz_pattern fuzz_subject fuzz_crossengine
tools/unicode/         the table generator and the check script
tools/oracle/          one driver per oracle, and the probe suite
```

## 9. Invariants

The things the tests exist to hold. Each is checked somewhere named in
[testing.md](testing.md).

1. The dialect is gone after lowering: no engine source file includes
   `syntax.h`. (A grep in `make check-symbols`' spirit; testing.md §6.)
2. Every engine that can run a program agrees with every other on every
   span. (§3.5.4.)
3. Every `GRX_ERR_LIMIT` corresponds to a named field, and lifting that
   field makes the same call succeed. (The fuzzers' options byte.)
4. No failing call leaves an allocation. (`CountingAllocator`.)
5. Every pattern in every oracle's own test corpus is accepted or rejected
   as the oracle accepts or rejects it, and matches as the oracle matches.
   (The conformance vectors, per dialect, with the pass rate published.)
6. No engine's C stack depth depends on the subject or the program.
   (Reviewed; and the fuzzers run with a small stack.)
7. The Unicode tables regenerate byte-identical from the pinned UCD.
   (`make check-unicode-tables`.)

## 10. Open decisions

Recorded so they are decided on purpose. Each has a recommendation; none
blocks the first work packages.

1. **The `text` seam** (§1.1): a provider vtable in `text` versus a
   dependency. Recommendation: the vtable. `text`'s standalone status is a
   stated property of the suite, and the vtable costs one struct.
2. **Offsets under ECMAScript** are bytes, not UTF-16 units (§2).
   Recommendation: bytes, documented; a conversion helper if a consumer
   asks.
3. **Invalid UTF-8 subjects** are `GRX_ERR_INVALID` (§5.1), not a new
   code. Recommendation: keep the suite vocabulary; the offset is one call
   away.
4. **Full case folding** for Perl and Ruby is a deviation (§5.2).
   Recommendation: accept it for 1.0; revisit only if a consumer needs
   `ß`/`ss`.
5. **UCD version policy** ([unicode.md](unicode.md) §1): pin one release
   per library minor version. Recommendation: 17.0.0 now, because Node 22 -
   the ECMAScript oracle - reports it, and vectors generated from an oracle
   on a newer UCD than the tables would fail for reasons that are not bugs.
6. **POSIX submatch fidelity** (§2): approximate in the first POSIX release.
   Recommendation: yes, with the deviation written and a work package
   scheduled, rather than holding the POSIX dialects for it.
