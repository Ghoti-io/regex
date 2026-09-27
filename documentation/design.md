# Design

This page is the design: the pipeline from pattern text to a program, the
engines, and the rules for memory, limits and errors. Which dialects compile,
and what each one means, is in [dialects.md](dialects.md).

Companion pages, each owned by this one:

| Page | What it settles |
| --- | --- |
| [dialects.md](dialects.md) | What each syntax accepts and what it means; the semantic profile per dialect; deviations |
| [unicode.md](unicode.md) | The Unicode data: which version, which tables, how they are generated and checked |
| [testing.md](testing.md) | How correctness is established: oracles, conformance vectors, cross-engine checks, fuzzing |
| [plan.md](plan.md) | The order the work was planned in. What compiles is in [dialects.md](dialects.md) |

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

`text`'s JSON Schema engine used to refuse every schema that used `pattern` or
`patternProperties`, and its header said why: "these need a regular-expression
engine, which is a dependency decision rather than an implementation detail"
(`text/include/ghoti.io/text/json/json_schema.h`). This library is that
engine, and as of WP-11 the seam below exists and both keywords work through
it; a schema is refused only when no provider was supplied. The dialect is
fixed by the specification:

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

The integration is [plan.md](plan.md)'s WP-11, and it went the way the
constraint here required: `text` does not link this library. It accepts a
regular-expression *provider* - `GTEXT_JSON_Regex_Provider` in
`GTEXT_JSON_Schema_Options`, with `compile_fn`, `search_fn`, `free_fn` and a
`ctx` - and the application supplies one backed by `ghoti.io-regex`.
`examples/json_schema_provider.c` is that adapter. This library stays
optional, and a third party could supply PCRE2 through the same seam.

One thing the sketch above did not have, and the implementation needed:
`search_fn` returns **three** answers, not two. Positive is a match, zero is
no match, and negative is *the search could not be completed* - a step budget
spent, an allocation refused. That third answer is the whole point of §1.2's
threat model reaching the caller: a pattern that exhausted its budget has not
said the instance is invalid, and folding that into "no match" would turn the
defence into a wrong validation result. `text` surfaces it as
`GTEXT_JSON_E_LIMIT`, which is neither its OK nor its schema-failure code.

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

## 2. Non-goals for the first stable release

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
  dialect ([plan.md](plan.md)); they are a capture-stack semantics no other
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
| **Pike VM** | the regular subset | O(subject × program × masks) time, O(program × masks) memory | linear in the subject; `GRX_ERR_LIMIT` for time when `max_steps` is set below what the closure walks |
| **Bit-state backtracker** | anything without a backreference, lookaround or recursion, when `program × subject` fits a memory budget | O(subject × program) time, O(subject × program / 8) memory | linear; leftmost-first captures exactly as the backtracker would report them |
| **Backtracker** | everything | exponential worst case, capped by `max_steps` and `max_backtrack` | terminates; `GRX_ERR_LIMIT` when the cap is hit |

`GRX_ENGINE_AUTO` picks the first row that applies. A caller who names
`GRX_ENGINE_PIKE` gets `GRX_ERR_UNSUPPORTED` for a program the Pike VM cannot
run, never a quiet substitution.

`masks` in the first row is the number of distinct stall masks a program
counter can carry, and it is there because the row used to read `O(subject ×
program)` and that stopped being true when the stall mask arrived (section
3.5.1). A program counter holds one thread per distinct mask, and `n`
potentially-empty loops *in sequence* produce 2^n of them - so a
forty-five-byte pattern is not bounded by its length the way the old row
implied. The Pike VM is still linear in the *subject*, which is the guarantee
that distinguishes it from the backtracker; it is the other factor that is
not small.

That is why the row no longer says "never `GRX_ERR_LIMIT` for time". It said
so on the reasoning that a closure visits each program counter at most once
per position, which the mask ended; and because the closure walk was not
charged to `max_steps` at all, the sentence was true for the wrong reason -
the engine could not report a limit for time because it was not counting the
time. Both halves are fixed: the walk is charged (section 3.5.1) and a caller
who sets `max_steps` gets a bound that holds.

#### 3.5.1 Pike VM

Cox's lockstep simulation: two thread lists as sparse sets keyed by program
counter *and by a stall mask*, so a thread's survival is decided structurally
rather than by a counter. What that costs is counted, and charged (below).

The mask is what makes the set sound. A plain `(pc, position)` key assumes
two threads at one program counter have the same future - true of a pure NFA,
and false here, because `GRX_OP_PROGRESS_CHECK` consults a register and two
arrivals can carry different values in it. `(a?b??)*` against `"abc"` is the
case that showed it: the first iteration reaches the lazy `b??` at position 1
with the register holding 0, the second reaches the same program counter at
the same position with it holding 1, so one of them is a stalled iteration
and the other is not - and the second arrival was dropped as a duplicate,
taking with it the thread that goes on to match `b`. The engine answered
`0-1` where the backtracker, the bit-state engine and ECMA-262 all answer
`0-2`.

Only *equality with the current position* can distinguish two register
values, because that is the only question `PROGRESS_CHECK` asks and positions
only advance, so a register holding anything else can never equal a later
one. One bit per register is therefore the whole of the distinction, and
threads at one program counter with the same bits really are interchangeable.
The lists and the closure stack grow against `max_match_memory` rather than
assuming one thread per program counter, so a pattern that needs an
unreasonable number of them is refused with `GRX_ERR_LIMIT` - an answer,
where the fixed size would have had to choose between a wrong result and a
write past the end.

This is worth stating plainly because the invariant the sparse set exists to
provide is the library's headline claim. It still holds in the subject: the
mask does not grow as the subject does. It is not otherwise small, and this
paragraph used to say it was - "bounded by the number of potentially-empty
loops *enclosing* a program counter". That is not what `stall_mask()`
computes. It walks every progress register in the program and sets a bit for
each one whose slot equals the current position, so loops in *sequence*
contribute bits just as nested ones do: after `(a?)*(a?)*` both registers can
equal the position at once. `n` potentially-empty loops anywhere in a program
therefore admit 2^n masks, and a program counter can hold a thread for each.

"Enclosing" made the factor look like nesting depth, which is small in real
patterns and bounded by `max_nesting_depth` besides. The true factor is a
count of loops, which nothing bounds but `max_program_size`. Forty-five bytes
of `(a?)*` repeated nine times walks 1,587,969 program counters over a
256-byte subject, and 101,079,031 over sixteen kilobytes.

Which is why the closure walk is charged to `max_steps`: one step per program
counter visited, plus one for each extra thread `list_find()` has to walk
past on a program counter's chain. Both, because both grow with the masks -
capping visits alone left a visit costing 427 nanoseconds at nine loops
against a plain pattern's 12, and the bound then held in the subject but not
in the loop count. It was not charged at all, once: `max_steps` counted the
dispatch loop -
the threads that survived into a list - on the old reasoning that the closure
visits each program counter at most once per position. The mask ended that
reasoning and the count was not revisited, so the engine's dominant cost was
free. The pattern above returned `GRX_OK` after forty-six seconds having
charged 163,850 of its ten million steps, and no value of `max_steps` would
have stopped it, because the quantity being capped was not the quantity being
spent. `max_match_memory` did not catch it either, and could not: it bounds
the state alive at one position, and that state is reused at the next. The
cost here is the *sum* over positions, which is what a step count is for.

With both charged, `(a?)*` repeated is refused in 38 to 52 milliseconds at
the default budget - at any subject length from 64 bytes to 64 kilobytes, and
at any loop count from seven to twenty-two, because what ends the run is the
budget rather than either of those. It was 46 seconds and `GRX_OK`.

Threads are ordered by
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

**The late memo.** This engine is the one that has to be able to run
anything, but that is no reason for it to be exponential on the programs it
does *not* have to. When the program is memoizable (§3.5.3) it arms the same
visited bitmap partway through a run: once the step count passes the number
of `(instruction, position)` states, which is exactly the work a memoised run
could do before running out of states to visit, and so the point at which the
run has provably repeated itself. Perl does this and calls it the super-linear
cache. The bitmap is charged against `max_match_memory` and simply not
allocated if it will not fit, because here it is an optimisation rather than
the promise it is on the bit-state engine.

This is why the ReDoS corpus ([testing.md](testing.md) §10) records fifteen
of its seventeen rows as *answered*. The exponential worst case that remains
is the programs the memo would be unsound for, and the corpus's two remaining
`limit` rows are that case written down.

**The two lookbehind models.** There are two ways to match a lookbehind and
the *dialect* picks, because the pick costs what the dialect is willing to
pay. `GRX_LookbehindLimit` ([dialects.md](dialects.md) §5.4) is the axis.

*Reverse* — run the body's reverse-direction instructions from the current
position backwards. This is what ECMA-262 describes (22.2.2.4, direction -1)
and what lets a lookbehind of **any** length cost what its body costs,
however far back the body reaches. It is the model for the dialects whose
lookbehind is unbounded: ECMAScript and .NET. A body may contain anything the
backtracker runs, captures included.

*Forward from a candidate start* — try each start the body's length allows,
furthest back first, and require the body to arrive exactly where the
assertion stands. This is Perl's and PCRE2's, and it is affordable only
because those dialects also bound the body's *variation* at 255 bytes: the
number of candidate starts is `max - min + 1`, so a body of one fixed length
has one candidate however long it is, and the worst case is a bounded
constant rather than a second factor of the subject. A dialect that did not
bound the variation could not have this model, which is why the axis is the
limit and not a preference.

The two are observably different in three places, all of them in the corpus:

| | reverse | forward |
| --- | --- | --- |
| which of several candidates | whichever the *body* prefers | the longest |
| `(?=foo)(?<=(a??))` on `afoo` | `1-1 1-1` | `1-1 0-1` ← Perl |
| `(*ACCEPT)` in the body | nowhere to stop | ends the assertion where it fires |

An alternative in the **tail** of a forward body carries a length guard
(`GRX_ASSERT_LOOK_LENGTH`): the distance left to the assertion is exactly
what it has to span, so one that cannot is skipped rather than walked and
undone. Perl prunes the same way. Only in the tail — an alternative with more
of the body after it shares the distance, and a guard asking it for all of
that would throw away the branch that matches, which
`(?<=([cd](*ACCEPT)|x)gggg)blrph` is the corpus record for.

The forward model is the more expensive of the two per assertion, and
honestly so: a wide alternation costs the branches the guard cannot rule out,
where running the body backwards costs the first branch that fits. It buys
agreement with the reference, which for these dialects is the point.

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
that can hang. What separates this engine from the plain backtracker on a
program both can run is *when* the bitmap starts working: here from the first
step, there only once the run has shown it needs one. So there is a step
budget at which this engine answers and the other has not yet finished, which
is what `tests/unit/test_bitstate.cpp` measures by binary-searching the
smallest budget each needs.

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

Both are speed, not correctness, and are phased after every implemented
dialect is conformant ([plan.md](plan.md)). Their interfaces are fixed now: the
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
- **`NOTBOL` / `NOTEOL`:** `^` and `$` do not match at the subject's two
  ends, for a caller feeding a buffer in pieces. The subject's ends and not
  the *window's*: `begin` says where a match may start, and a `^` suppressed
  there would make a search-all loop report a different answer on its second
  call than on its first. And `^` and `$` only - `\A`, `\Z`, `\z` and GNU's
  `` \` `` and `\'` stand, which is what PCRE2 and glibc both do and what
  `tools/oracle/window_diff.py` checks.
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

   Three of the hooks were added by the Perl family and are worth naming,
   because each is a thing the shared grammar cannot do without them and none
   of them is a dialect decision in disguise:

   - `skip_ignorable`, called before an atom, before a quantifier, and before
     the `|` or `)` that ends a branch. Extended mode is why: under `(?x)` a
     space is not part of the pattern at all, and a front end that turned one
     into an empty node would make `a +` a repeat of nothing rather than
     `a+`. `\Q...\E` and `(?#...)` are the same shape of problem - `a\Q\E*`
     and `a(?#x)*` are both `a*`, and `(?#x)*` on its own is "nothing to
     repeat", which only a *lexical* skip gets right.
   - `GRX_GroupOpen::read_body`, for a body that is not one alternation. A
     conditional is the construct that needs it: `(?(1)yes|no)` has exactly
     two branches, and reading the body as an alternation would make
     `(?(1)a|b|c)` a conditional with three rather than the error both
     references report. The parser keeps the depth accounting and the closing
     `)`, so those diagnostics still have one home.
   - `GRX_Parser::quote_end`, the extent of a quoted run. The *mechanism* is
     shared and the *spelling* is not: `\Q` and `\E` belong to the front ends
     that have them, but "the characters in this span are literals whatever
     the grammar would make of them" can only be acted on by the grammar,
     which is what decides `*` is a quantifier and `|` ends a branch. The
     feature table has said `GRX_FEATURE_QUOTING` since it was written.

Two rules about what a dialect is not:

- **Not a superset.** A dialect that accepts everything is a bug. A caller
  uses this library to learn whether a pattern is valid *for that engine*;
  accepting PCRE's `(?>...)` under `GRX_SYNTAX_ECMASCRIPT` would tell them it
  is, and it is not.
- **Not a family.** `GRX_SYNTAX_PERL` and `GRX_SYNTAX_PCRE` stay separate
  because PCRE2 has verbs and `\K` restrictions Perl does not, and Perl has
  full case folding PCRE2 does not - `(?i)ß` matches "ss" under one and
  nothing under the other. Where two names genuinely agree, they
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
code point sharing a fold), the full fold and the per-dialect variants; the
engines never call any of them.

Simple folding is a class: the orbit of `a` is `{A, a}`, and a caseless
literal is that set. **Full folding is not**, because it maps one code point
to a sequence - `ß` to "ss", the `ﬁ` ligature to "fi" - so a caseless match
can be a different length from the pattern that asked for it, in either
direction. That was recorded as a deviation for as long as it was one; it is
implemented now, and the way it stays a compile-time operation is worth
writing down.

A run of literal text is matched against its *fold*. Lowering builds that
folded string and, over its positions, the edges of a graph: an edge from
`i` to `j` carries the class of characters whose full fold is exactly
positions `i` to `j`. A path from one end to the other is a sequence of
subject characters whose folds concatenate to the whole of it, which is the
definition of the match. The edges leaving a position are disjoint - a
character has one full fold - so the walk is deterministic and a subject has
at most one path.

Codegen turns the graph into instructions rather than lowering turning it
into nodes, because the paths share their tails and a tree cannot say so:
`s` written thirty times has thirty-one positions and a Fibonacci number of
paths. As instructions it is `SPLIT`, `CLASS` and `JMP` - nothing an engine
had to learn, so all three still run it and a caseless Perl pattern keeps
the linear-time guarantee.

Most runs need none of this, and finding that out is done **once per run**
rather than once per literal. The plan asks two questions - is the fold longer
than the run, and is any span of two or more in the fold something a single
character folds to - and a "no" to both is inherited by every contiguous piece
of the run, so the whole run then lowers one class per code point. Asking per
literal instead made lowering quadratic in the pattern length, which nothing
in the suite was long enough to notice: a 5,227-character caseless Perl
pattern took 3.5 seconds to compile.

A *class* is still folded simply. UTS #18 applies full folding to the text a
pattern spells out and not to the sets it names, and a class matches one
character, so `[ß]` cannot match two. Perl agrees.

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

The library opens no files and builds no paths. A pattern and a subject
arrive as a pointer and a length, and everything the library reports goes
back the same way, so `cutil`'s file and path modules are used only by the
things *around* it - the examples, the development tools and the test
helpers. That is worth stating because it is load-bearing: a caller can hand
this library bytes that came from a socket, a memory map or a database column
without the library having an opinion, and there is no platform branch inside
it to be wrong about.

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
| `max_nesting_depth` | parser | recursion depth of groups, classes and lookarounds - this is also the C stack bound, so its default is small (a few hundred) and it is the one limit a caller should not lift casually. It bounds the *parser's* stack; the backtracker's own recursion over nested assertions has its own floor under it, `GRX_BACKTRACK_MAX_C_DEPTH`, because this field is the caller's to raise and that one must not be (section 9 invariant 6) |
| `max_nodes` | parser | AST nodes |
| `max_captures` | lowering | capturing groups |
| `max_repeat_count` | parser | any bound in `{m,n}` |
| `max_class_ranges` | lowering | ranges in one canonical class, after expansion of properties and folding |
| `max_program_size` | codegen | instructions, after repeat expansion |
| `max_lookbehind_length` | analysis, after lowering | maximum length of a lookbehind body. `GRX_NPOS` for an unbounded body, which exceeds every finite cap. A *caller's* policy rather than a dialect's rule, so its default is 0; a dialect that bounds its own lookbehind enforces that through its profile instead |
| `max_recursion_depth` | backtracker | nested `CALL` frames. Reserved until WP-18 brought a dialect that recurses; `tests/unit/test_limits.cpp:RecursionDepthIsEnforcedNowThatADialectHasRecursion` is the test that was waiting for it. Frames, not C frames - a subroutine call is heap bookkeeping |
| `max_subject_length` | entry | bytes of subject |
| `max_steps` | all engines | instructions executed in one search. On the Pike VM that is every program counter the closure walk visits and every chained thread it walks past, not only the threads that survive into a list: the walk is where that engine's work is, and a count that skipped it left `max_steps` unable to bound the one thing it exists to bound |
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

That "in the match object instead" is `grx_match_error()`, which returns a
`GRX_Error` describing the last attempt: the result code, the diagnostic
naming which limit or check stopped it, and - for a subject that is not
valid UTF-8 - an offset into the *subject* rather than into the pattern.
`GRX_NPOS` is the offset when there is nothing to point at. It sits beside
`grx_match_steps()` because both are facts about an attempt rather than
about the pattern, which is why closing this gap needed no new parameter on
any of the seven entry points, and left all 193 existing call sites alone.

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
| `exec.h` | `GRX_SearchOptions` (window, flags, engine, limits) and `grx_regex_search_ex()` / `grx_regex_match_ex()`; `grx_regex_search_next()`; `grx_match_steps()`; `grx_match_mark()`; `grx_match_error()` (§6.4); `grx_match_span()` convenience for group 0. |
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
src/syntax/      the profile table, one file per family's hooks, and
                 frontend.c - the one place that says which are built:
                 syntax.c frontend.c perl.c posix.c ecmascript.c
                 python.c java.c dotnet.c ruby.c re2.c tcl.c
                 vim.c emacs.c                                      [front ends]
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
6. No engine's C stack depth depends on the subject, and depends on the
   program only through how deeply it nests assertions - which the engine
   caps itself, so that the depth is bounded whatever the caller's limits
   say. (`tests/unit/test_stack.cpp`, which measures the high-water mark
   rather than counting frames; and the fuzzers run with a small stack.)

   This used to read "nor the program", and was enforced by the word
   "Reviewed". Measuring it found the half that was not true: the
   backtracker's `run()` calls itself for an assertion body, a lookbehind's
   forward pass and a sub-match condition, so a nested assertion is a C
   frame and the cost is linear in the nesting - about 600 bytes a level.
   `max_nesting_depth` held it down at the defaults by accident, being 128;
   raised to the 480 the *parser* takes, the matcher wanted 241 KB and a
   256 KB stack gave a segmentation fault instead of a diagnostic.
   `GRX_BACKTRACK_MAX_C_DEPTH` is the floor underneath that field now, and
   over it the answer is `GRX_ERR_LIMIT`. Making the engine iterative here
   would let this invariant go back to its original wording; until somebody
   does, the wording follows the code.

   It says *engine*, and for a while that was a gap rather than a scope. The
   instrument was pointed only at a match, so `grx_codegen_program()` - which
   called itself over the IR at about four frames a level, with no bound of
   any kind - was unmeasured, and a fuzz campaign found it at 106 levels of
   nesting as a stack overflow inside `grx_regex_compile()` with nothing
   returned and nothing logged. A depth cap could not answer it: this suite
   requires a pattern nested 1920 deep to *compile*, so a constant would have
   had to be at least 1920 to keep that and at most 105 to stop the crash.
   Codegen keeps its stack on the heap instead, and its cost is now flat -
   about 4.5 KB from eight levels of nesting to nineteen hundred, measured the
   same way.
7. The Unicode tables regenerate byte-identical from the pinned UCD.
   (`make check-unicode-tables`.)

## 10. Open decisions

Recorded so they are decided on purpose. Each has a recommendation; none
blocks the first work packages.

1. **The `text` seam** (§1.1): a provider vtable in `text` versus a
   dependency. Recommendation: the vtable. `text`'s standalone status is a
   stated property of the suite, and the vtable costs one struct.
   **Decided, and built** in WP-11: the vtable, at four members. The cost was
   one struct as predicted; what was not predicted was that `search_fn` would
   need a third return value (§1.1).
2. **Offsets under ECMAScript** are bytes, not UTF-16 units (§2).
   Recommendation: bytes, documented; a conversion helper if a consumer
   asks.
3. **Invalid UTF-8 subjects** are `GRX_ERR_INVALID` (§5.1), not a new
   code. Recommendation: keep the suite vocabulary; the offset is one call
   away.
4. **Full case folding** for Perl and Ruby was a deviation (§5.2).
   **Decided against, and built**: the recommendation was to accept simple
   folding for now and revisit only if a consumer needed `ß`/`ss`, and what
   made it worth doing sooner was that the graph turns out to compile to
   instructions the engines already had. Eleven corpus records that had been
   listed as gaps now pass. Ruby has no front end yet and will get the same
   profile row when it does.
5. **UCD version policy** ([unicode.md](unicode.md) §1): pin one release
   per library minor version. Recommendation: 17.0.0 now, because Node 22 -
   the ECMAScript oracle - reports it, and vectors generated from an oracle
   on a newer UCD than the tables would fail for reasons that are not bugs.
6. **POSIX submatch fidelity** (§2): approximate in the first POSIX release.
   Recommendation: yes, with the deviation written and a work package
   scheduled, rather than holding the POSIX dialects for it.
