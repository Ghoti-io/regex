# The dialect specification

**Status: placeholder.** This page is where the specification goes, and it is
not written yet. What is here is the list of questions it has to answer and
the reference document for each dialect, so that the work has a shape before
it starts.

Until it is written, the feature rows in
[`src/syntax/syntax.c`](../src/syntax/syntax.c) are provisional: they record
what each implementation is widely documented to have, as a starting point,
and every one of them needs checking against the reference beside it.

## Reference documents

| Dialect | `grx_syntax_name()` | Reference |
| --- | --- | --- |
| POSIX BRE | `posix-bre` | IEEE Std 1003.1-2017, chapter 9.3 |
| POSIX ERE | `posix-ere` | IEEE Std 1003.1-2017, chapter 9.4 |
| GNU BRE | `gnu-bre` | GNU grep manual, "Regular Expressions" |
| GNU ERE | `gnu-ere` | GNU grep manual, "Regular Expressions" |
| Perl | `perl` | `perlre`, Perl 5.38 |
| PCRE | `pcre` | `pcre2pattern`, PCRE2 10.44 |
| ECMAScript | `ecmascript` | ECMA-262, clause 22.2 |
| Python | `python` | CPython `re` documentation, 3.13 |
| Java | `java` | `java.util.regex.Pattern`, JDK 21 |
| .NET | `dotnet` | "Regular Expression Language - Quick Reference" |
| Ruby | `ruby` | Onigmo `RE.txt` |
| RE2 | `re2` | RE2 "Syntax" |
| Rust | `rust` | the `regex` crate's syntax documentation |
| Tcl | `tcl` | `re_syntax(n)` |
| Vim | `vim` | `:help pattern` |
| Emacs | `emacs` | GNU Emacs Lisp Reference Manual, "Regular Expressions" |

## What the specification has to settle

Each of these is a place where the implementations genuinely disagree, so a
library that claims to support all of them has to have written the answer
down rather than inherited it from whichever dialect was implemented first.

1. **Which constructs each dialect has**, as `GRX_Feature` bits. The table in
   `syntax.c` is the answer; this page is where each entry is justified.

2. **What an absent construct does.** Three different things, and the
   difference matters to a caller: a literal (`+` at the start of a POSIX BRE),
   a syntax error, or `GRX_ERR_UNSUPPORTED` because the dialect has it and
   this library does not yet.

3. **Greediness and the leftmost rule.** POSIX specifies leftmost-longest;
   Perl and everything descended from it specify leftmost-first with
   backtracking order. The same pattern and subject can give different
   captures under the two, so the dialect has to say which, and both engines
   have to implement the one it says.

4. **What `.` and `$` do at a line break**, and which sequences count as one:
   `\n` alone, `\r\n`, or the Unicode set. Dialects differ, and `GRX_OPT_DOTALL`
   and `GRX_OPT_MULTILINE` only cover part of it.

5. **Empty-match and zero-width repetition rules.** `(a*)*` against `""`, and
   what an iteration that consumed nothing does to the loop. Every
   implementation has a rule; they are not the same rule.

6. **Character-class syntax.** Whether `]` first is a literal, whether `[]` is
   empty or the start of a class containing `]`, what a range endpoint may be,
   and which dialects have set operations (`&&`, subtraction, nesting).

7. **Escapes.** `\d`, `\w`, `\s` under ASCII and under `GRX_OPT_UCP`; the octal
   and backreference ambiguity of `\1` versus `\01`; `\x` with and without
   braces; and which dialects have `\Q...\E`.

8. **Unicode.** Which properties `\p{...}` accepts, what case folding a
   caseless match performs (simple, per
   `grx_unicode_fold_simple()`, which is ASCII-only today), and whether the
   subject is validated as UTF-8 or read as bytes.

9. **Capture numbering and naming.** Duplicate names, numbering of branch
   resets, and what a group that did not participate reports - this library
   says `GRX_NPOS`, distinct from an empty match, but a dialect may have its
   own answer to expose.

10. **The replacement template**, once substitution exists: `$1` or `\1`,
    named references, what an unmatched group expands to, and whether case
    operators (`\U`, `\E`) are in scope.

## Deviations

Where this library deliberately differs from a dialect it names, the deviation
and its reason go here. There are none yet, because there is no parser.

## Limits

The values in `grx_limits_default()` are first approximations chosen to sit
far above any pattern a human writes and far below anything that costs a
visible amount of time or memory. They need measuring once the engines exist,
and the measurement belongs on this page.
