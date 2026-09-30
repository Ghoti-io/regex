# Ghoti.io Regex

Regular expressions in C, across eighteen dialects and one parser. Three
engines run the result: a Pike VM that is linear in the subject length, a
backtracking engine for the constructs no lockstep simulation can express,
and a bit-state engine that is the backtracker with a memo and the linear
bound restored.

## Dialects

This is what the library implements.

Twelve dialects compile and match: ECMAScript (legacy, `u` and `v`), PCRE2,
Perl, POSIX BRE and ERE, GNU BRE and ERE, Python, Vim, I-Regexp (RFC 9485,
the interoperable subset JSONPath is specified over), RE2 and Rust.

Java, .NET, Ruby, Tcl, Emacs and RE/flex are named and report
`GRX_ERR_UNSUPPORTED`.

| Dialect | What it means here |
| --- | --- |
| ECMAScript, PCRE2, Perl, POSIX BRE, POSIX ERE, GNU BRE, GNU ERE, Python, Vim, I-Regexp, RE2, Rust | Compiles and matches, on whichever engine can run the pattern. |
| Java, .NET, Ruby, Tcl, Emacs, RE/flex | Named. A pattern reports `GRX_ERR_UNSUPPORTED`. |

RE2 and Rust are the two where a *refusal* is the point. Neither has
backreferences or lookaround, because neither can be run in linear time, so
a pattern this library accepts under `re2` or `rust` is one
`GRX_ENGINE_PIKE` is guaranteed to run - and asking for that engine by name
is how the tests check it rather than assert it.

## Before you call it

- Options that dialects spell differently (`/i`, `re.I`, `(?i)`) are one set of `GRX_Option` bits. They are accepted even for a dialect whose own syntax cannot write them, because the caller is not limited to the pattern text.
- A `GRX_Regex` is immutable and holds no match state, so one may be used from several threads. The mutable part is `GRX_Match`.
- "No match" is an `int *` out parameter. The return value is reserved for things that went wrong. `0` is success. `GRX_ERR_SYNTAX` arrives with a byte offset into the pattern and a message, in a caller-supplied `GRX_Error`.
- `GRX_Limits` caps every unbounded quantity, and most fields have a non-zero default. `(a+)+$` against a long run of `a` is the case they exist for: `max_steps` and `max_backtrack` turn it into `GRX_ERR_LIMIT`. `0` means no limit. `grx_limits_unlimited()` is the all-zero structure.
- Every allocation goes through the `GRX_Allocator` the caller supplied, which is cutil's vtable. `NULL` is the default. A failing call allocates nothing the caller has to free.

| Engine | What it means here |
| --- | --- |
| `GRX_ENGINE_PIKE` | Linear in the subject length. Asking for it by name returns `GRX_ERR_UNSUPPORTED` when the program is not regular. |
| `GRX_ENGINE_BITSTATE` | The backtracker with a memo, for a program that memo can cover. |
| `GRX_ENGINE_BACKTRACK` | Everything else those two refuse, bounded by the limits. |
| `GRX_ENGINE_AUTO` | The Pike VM when the program is regular. A backreference, a lookaround, an atomic group, a possessive quantifier, a script run, a recursion, a conditional or a backtracking control verb is not. Of those, a memoizable program runs on the bit-state engine; the rest run on the backtracker. |

## Examples

```c
#include <ghoti.io/regex/regex.h>
#include <stdio.h>

int main(void) {
  GRX_Error error;
  GRX_Regex * regex = NULL;

  if (grx_regex_compile_with_allocator("(\\w+)@(\\w+)", 11,
          GRX_SYNTAX_ECMASCRIPT, GRX_OPT_CASELESS | GRX_OPT_UTF, NULL, NULL,
          &error, &regex) != GRX_OK) {
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

```
Corey
```

`examples/regex_info.c` prints what a dialect has and tries a pattern in it.
`examples/posix_stream.c`, `perl_extract.c`, `vim_substitute.c`,
`python_split.c`, `iregexp_jsonpath.c` and `linear_guarantee.c` are one
program per family; the last is the two linear dialects, where a construct
being refused at compile time is what buys the linear-time bound.
`examples/json_schema_provider.c` is this engine behind `text`'s JSON Schema
`pattern` keyword.

## Compile and link

Once the library is installed, pkg-config carries the include path, the
library, and its dependencies:

```bash
cc -o show show.c $(pkg-config --cflags --libs ghoti.io-regex-0)
```

The module name ends in the major version, `-0` for this release, so two
majors can be installed side by side. A build made with `make BRANCH=-dev`
installs `ghoti.io-regex-dev` instead.

## Building the library

[cutil](https://github.com/Ghoti-io/cutil) and
[unicode](https://github.com/Ghoti-io/unicode) must already be installed
where pkg-config can see them. Case folding, segmentation, `\N{NAME}` and
script runs read the Unicode library. A dependency it cannot find is a hard
error naming the fix.

`text` is optional. The two JSON Schema programs link it when pkg-config
finds it, and are skipped when it does not.

```bash
make
make test
sudo make install
```

From the parent of a suite checkout:

```bash
./suite/install.sh
export PKG_CONFIG_PATH="$PWD/.local/share/pkgconfig"
make -C libs/regex test PREFIX="$PWD/.local"
```

`make test` is the suite. `make help` lists the rest, including
`make test-asan` and `make test-valgrind`. The oracle images are named in
`tools/oracle/containers/IMAGES`.

| Target | What it does |
| --- | --- |
| `make examples` | The programs under `examples/` |
| `make check-oracles` | Differentials against pinned reference engines, in containers |
| `make oracle-version` | Print which reference would answer |
| `make fuzz` | Pattern, subject, and cross-engine harnesses |
| `make docs` | The Doxygen manual, into `./docs` |

## The API

Everything is prefixed `grx_` / `GRX_`, under `<ghoti.io/regex/...>`.

**Dialects.** `GRX_Syntax` names one syntax. `grx_syntax_spec()` returns it
as data: a set of feature bits, so a construct is accepted or rejected in
one place. `grx_syntax_from_name()` maps a spelling (`"pcre"`,
`"posix-ere"`) to the enum.

**Compiling.** `grx_pattern_parse()` produces a syntax tree with the dialect
differences resolved. `grx_regex_compile()` goes straight to a `GRX_Regex`
when the tree is not wanted. "This is not valid PCRE" is a statement about
the text, and a caller that is linting wants it without a program.

**Matching.** `grx_regex_search()` finds the first match at or after an
offset. `grx_regex_match()` requires the match to begin there.
`grx_regex_replace()` and `grx_regex_split()` cover the template grammars of
the dialects that have them.

[Dialects](#dialects) is what is implemented.
[Before you call it](#before-you-call-it) is what that changes about a call.

## Dependencies

Found through pkg-config, and the installed `.pc` file names them, so a
program that links `ghoti.io-regex-0` links these too.

- [ghoti.io-cutil](https://github.com/Ghoti-io/cutil) — the allocator.
- [ghoti.io-unicode](https://github.com/Ghoti-io/unicode) — the Unicode Character Database: every `\p{...}` set, case folding, segmentation, `\N{NAME}` and script runs.

`text` is optional, and the dependency points the other way: `text` does
not depend on `regex`. This library fills in the provider `text` uses for
JSON Schema `pattern` and `patternProperties`.

## Documentation

| Page | What it settles |
| --- | --- |
| [documentation/design.md](documentation/design.md) | The pipeline, the three engines, memory, limits, errors |
| [documentation/dialects.md](documentation/dialects.md) | Which constructs each syntax has, and where implementations disagree |
| [documentation/unicode.md](documentation/unicode.md) | Which Unicode data this library still owns, and which it reads from `unicode` |
| [documentation/testing.md](documentation/testing.md) | Oracles, the vector format, fuzzing |

`make docs` builds the manual.

## Status

The twelve dialects above compile and match on whichever of the three
engines can run the pattern. The other six report `GRX_ERR_UNSUPPORTED`.

### Conformance

`make test` runs 87,215 checked-in conformance vectors and prints this table.
Every expectation in them is a reference implementation's, taken from the
pinned oracle in `tools/oracle/containers/IMAGES`; none is this library's own
output. The corpora are committed, so the run needs no oracle, no container
engine and no network.

| Dialect | Vectors | Pass | Notes |
| --- | ---: | ---: | --- |
| `i-regexp` | 30,015 | 100.00% | generated from two references, RFC 9485 naming none: iregexp-check 0.1.4 for the syntax, libxml2 2.14.6 for XSD's semantics |
| `ecmascript` | 28,559 | 100.00% | test262, plus generated from node 24.21 |
| `perl` | 11,705 | 100.00% | perl 5.44's `re_tests`, plus generated: full folds, `\N{}` names, boundaries, and property names with more than one reading; 8 excluded |
| `vim` | 6,317 | 100.00% | generated from vim 9.2.1129 |
| `re2` | 2,947 | 100.00% | generated from Go 1.25.14's `regexp`, which carries Unicode 15.0.0 |
| `rust` | 2,817 | 100.00% | generated from the `regex` crate 1.13.1, which carries UCD 16.0.0; 1 excluded |
| `python` | 2,177 | 100.00% | generated from CPython 3.14.7 |
| `pcre` | 1,869 | 100.00% | PCRE2 10.46's `testinput` |
| `gnu-ere` | 270 | 100.00% | Spencer's cases, answered by glibc 2.41 |
| `posix-ere` | 245 | 100.00% | Spencer's cases, where glibc and musl agree |
| `gnu-bre` | 159 | 100.00% | as `gnu-ere` |
| `posix-bre` | 135 | 100.00% | as `posix-ere` |

Read the denominators with the rates: they differ by two orders of magnitude,
and 100% of 135 vectors is a smaller claim than 100% of 28,559.

There are no known gaps. The one there was arrived with the `rust` dialect
and turned out not to be one row: `(a*)+b` under `U` reported group 1 as 1-2
where both references report 0-2, and enumerating the shape rather than
sampling it found 62 such rows in 5,040 - a whole cell rather than an
oddity. Neither reference backtracks, so a loop over a body that can match
empty ends for them where the simulation has already been, which is not any
of the three empty-iteration rules a backtracking dialect can hold. The
library now has a fourth, and all three engines compute it.

Before that there were five, and where those were
there is now `\U`, `\L`,
`\F`, `\u` and `\l` - Perl's case transforms over the pattern source, the
operators its own `re_tests` calls "\l works in []". The nine excluded rows
are ones where the reference's own answer is demonstrably wrong; each carries
the reproduction in `tests/data/vectors/known-gaps.txt`, and the count is
printed beside the rate wherever the rate appears, because a rate that rose
because rows left the denominator has to say so. A gap that is closed is
removed from that file in the commit that closes it, and the runner fails on
an entry whose record has started passing, so the file cannot drift into a
list of what once was.

A rate is not the whole of "done" for a dialect: [documentation/plan.md](documentation/plan.md)
§4 lists six conditions and this is one of them.

## License

LGPL-3.0-only. See [COPYING.LESSER](COPYING.LESSER) for the license, and
[COPYING](COPYING) for the GPL text it is written as additional permissions
on top of.

Contributions are not being accepted at this time; see
[CONTRIBUTING.md](CONTRIBUTING.md) for what is useful instead.
