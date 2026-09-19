# Unicode data

**Status:** built, except the properties of strings and the grapheme rules
(WP-12). The codec, the tables, both foldings and both name resolvers are in
`src/unicode`; `make check-unicode-tables` proves the committed tables are
what the generator produces. Owned by [design.md](design.md) §5.

Nothing else in the suite carries Unicode data - `text` parses JSON, YAML
and CSV without character properties, `cutil` has none, and `ctang` uses
ICU, which this library will not: ICU is a large dependency for a set of
tables that a regex engine wants in its own shape. So this library owns its
tables, and this page is the specification for them.

## 1. Which Unicode

One UCD release is pinned per library minor version, named in one place
(`tools/unicode/UCD_VERSION`), and every table carries it in a comment and
in `grx_unicode_version()`.

**17.0.0** for the first release. The reason is the oracle: Node 22, which
generates the ECMAScript conformance vectors ([testing.md](testing.md) §2),
reports Unicode 17.0, and a vector that says `\p{Script=Foo}` matches a
character added in 17.0 must not fail against tables from 16.0 for a reason
that is not a bug. When an oracle and the tables disagree on version, the
generator records the oracle's version in the vector file and the runner
skips property vectors newer than the tables, with a count, rather than
failing them.

The UCD files themselves are not committed. They are fetched into
`third_party/ucd/<version>/` (gitignored, like every `third_party`) by
`tools/unicode/fetch.sh`; the *generated* tables are committed, because a
build must not need the network and must not need Python.

## 2. UTF-8

Already written and kept: `grx_unicode_utf8_decode()` is strict - it
rejects overlong forms, surrogates, code points above U+10FFFF, the
five-and-six-byte forms and every truncated sequence - and
`grx_unicode_utf8_encode()` is its inverse. Two additions:

- `grx_unicode_utf8_decode_prev()`: decode the code point *ending* at an
  offset, for reverse-direction instructions (lookbehind). Steps back over
  continuation bytes, at most three, then validates forward; a malformed
  sequence is reported the same way as forward decoding reports it.
- `grx_utf8_validate(text, length, &bad_offset)`: the whole-subject check
  that runs before a search in UTF mode ([design.md](design.md) §5.1). Public,
  so a caller who received `GRX_ERR_INVALID` can find the offset.

  Built as a loop over `grx_unicode_utf8_decode()` rather than as the
  table-driven DFA (Hoehrmann's) this page first specified. The reason is
  that the two must agree on *exactly* what is valid, and a second
  implementation of "valid UTF-8" is a second place for the surrogate rule or
  an overlong bound to be written down slightly differently - a disagreement
  that shows up as a subject the validator accepts and the engine then cannot
  step through. The loop is O(n) with a small constant. If profiling on a
  real workload says the constant matters, the DFA goes in *under* the
  decoder so that both still share one definition.

## 3. The tables

Every table is a sorted array of disjoint inclusive code-point ranges,
`GRX_CharRange {low, high}` as the scaffold has it, so that a class built
from a property is a copy of a range array and membership is the same binary
search the character-class module already does.

This page originally also specified a two-level trie beside the ranges for
the hot lookups. There is none, and there is no hot lookup for it to serve:
every property and every fold is resolved *at compile time*, into the
canonical class an instruction names, so an engine never looks a property up
while matching. A trie would be a second representation of the same data
with no caller. It goes in if and when a lookup appears on a matching path -
derived in the generator from the ranges, never written by hand.

| Table | Source file(s) | Used by |
| --- | --- | --- |
| General_Category, per value and per group (`L`, `LC`, `M`, ...) | `UnicodeData.txt`, `DerivedGeneralCategory.txt` | `\p{L}`, `\p{gc=Lu}`, `\w`/`\d` under UCP |
| Script | `Scripts.txt` | `\p{Script=Greek}`, `\p{sc=Grek}` |
| Script_Extensions | `ScriptExtensions.txt` | `\p{scx=Grek}` |
| Binary properties, the ECMA-262 list | `PropList.txt`, `DerivedCoreProperties.txt`, `emoji-data.txt`, `DerivedBinaryProperties.txt` | `\p{Alphabetic}`, `\p{White_Space}`, `\p{Emoji}`, ... |
| Property and value aliases | `PropertyAliases.txt`, `PropertyValueAliases.txt` | name resolution, both strict (ECMAScript) and loose (UAX #44) |
| Simple case folding, and the fold orbits | `CaseFolding.txt` (statuses C and S) | caseless literals and classes at compile time |
| Simple upper and lower mappings | `UnicodeData.txt` | ECMAScript's non-`u` Canonicalize, which is defined in terms of `toUpperCase` |
| Special-casing exceptions | `SpecialCasing.txt` | the ECMAScript legacy rule that a mapping to more than one code unit, or from non-ASCII to ASCII, is not applied |
| White_Space, and ECMAScript's `\s` set (WhiteSpace ∪ LineTerminator, which adds U+FEFF) | `PropList.txt` | `\s` per dialect |
| Line terminator sets: `\n`; `\r\n`-aware; ECMAScript's four; Java's six; Unicode's `\R` set | fixed, but generated so they are named in one place | `.`, `^`, `$`, `\R` per dialect |
| POSIX classes `[:alpha:]` ... `[:xdigit:]`, ASCII and Unicode definitions | derived from the above per the profile | `[[:alpha:]]` |
| Word characters: ASCII, Unicode (`\p{L}\p{N}\p{M}\p{Pc}` plus join controls, per UTS #18), and ECMAScript's `iu` set (ASCII plus U+017F and U+212A) | derived | `\w`, `\b` per dialect |
| Properties of strings for ECMAScript `v`: `RGI_Emoji`, `Basic_Emoji`, `Emoji_Keycap_Sequence`, `RGI_Emoji_Flag_Sequence`, `RGI_Emoji_Modifier_Sequence`, `RGI_Emoji_Tag_Sequence`, `RGI_Emoji_ZWJ_Sequence` | `emoji-sequences.txt`, `emoji-zwj-sequences.txt` | `\p{RGI_Emoji}` in `v` mode, which matches a *string* and so lowers to an alternation of literal sequences, not a class |
| Extended grapheme cluster rules | `GraphemeBreakProperty.txt`, `emoji-data.txt` | `\X`, PCRE2 and Perl; later tier |

The two emoji sequence files are not in the UCD. UTS #51 publishes them
beside it, at `Public/<version>/emoji/` rather than `Public/<version>/ucd/`,
and that directory is the one to read: `Public/emoji/latest` was already 18.0
while `Public/17.0.0/emoji` held the 17.0 that matches everything else here.
`emoji-test.txt` is fetched from the same place and is not generated from -
it is the universe `tools/oracle/string_property_diff.py` walks, because a
property of strings has no code-point space to enumerate and needs a list of
candidate sequences to compare membership over.

A property of strings is stored as three arrays: the code points end to end,
an index of `{first, length}` per sequence, and a record per property naming
a slice of that index. `RGI_Emoji` is UTS #51's ED-27, the union of the other
six, and its slice is the whole array - so it costs three integers rather
than a second copy of 3,953 sequences.

Size, measured at UCD 17.0.0 rather than estimated: 457 properties over
24,086 ranges, **183 KB** of `.rodata` for the ranges and the property
records, **39 KB** of relocated pointers for the name tables, and **106 KB**
for the two case tables and their orbits, and **80 KB** for the properties of
strings (3,953 sequences over 12,389 code points) - **408 KB** in total,
against the 200 KB this page first guessed. The gap is the name tables, which were not
in the estimate, and the orbit index, which carries a twelve-byte record for
every one of the 2,994 code points in a multi-member orbit. Both are
compressible and neither is on a hot path; the note is here so that a later
decision to compress them is made against a number. What remains
unacceptable is generating any of it at build time, which would put Python
and the network in the build.

## 4. Generation and checking

`tools/unicode/gen_tables.py` reads `third_party/ucd/<version>/` and writes
`src/unicode/tables/*.c` and one header. It is deterministic - sorted
input, fixed formatting, no timestamps - so that the committed output is
reproducible byte for byte, and a Makefile target proves it:

```
make check-unicode-tables   # regenerate into build/, diff against src/;
                            # skipped with a message when the UCD is absent
```

The generator has its own tests (Python, `tools/unicode/test_gen.py`) for
the parsing of each UCD file format - the `First>`/`Last>` range convention
in `UnicodeData.txt`, the `#` comments and `..` ranges in the derived files,
the four-column form of `CaseFolding.txt` - because a generator bug is a
correctness bug in every dialect at once, and a C test that checks
`\p{L}` against a few known letters would not find it.

The C tests then check the *generated* tables against facts stated in the
standard, not against the generator: U+0041 is `Lu`, U+00DF folds to itself
under simple folding (its full fold is the two-code-point `ss`, which is
exactly the case simple folding leaves alone), U+212A folds to `k`, U+FEFF
is in ECMAScript's `\s` and not in `White_Space`, and the range count of
each General_Category value matches the count in `DerivedGeneralCategory.txt`'s
own `# Total code points` trailer, which the generator copies into the
table as a constant for exactly this purpose.

## 5. Case folding

Three operations, all compile-time ([design.md](design.md) §5.2):

- **`fold_simple(cp)`**: `CaseFolding.txt` statuses C and S. One code point
  in, one out.
- **`fold_orbit(cp, out)`**: every code point whose simple fold equals
  `fold_simple(cp)`. This is what a caseless literal becomes and what a
  caseless class is closed under. Precomputed as a table of orbits (most are
  pairs; `K`/`k`/U+212A and `S`/`s`/U+017F are the famous triples; the
  largest, for U+03B8 theta, has four members, which is why
  `GRX_FOLD_ORBIT_MAX` is 4). A code point with no entry is alone in its
  orbit and the function returns it, so "expand to the orbit" is one code
  path whether or not the character has a case.
- **`canonicalize_es_legacy(cp)`**: ECMA-262's Canonicalize for a pattern
  without `u`: apply the simple uppercase mapping unless the result is more
  than one UTF-16 code unit or maps a non-ASCII code point into ASCII.
  ECMAScript is the only dialect with this rule, and it is the rule under
  which JavaScript's `/[a-z]/i` does not match `ſ` while `/[a-z]/iu` does.

Full case folding (status F) is deliberately not implemented; see
[design.md](design.md) §10.

## 6. Property names

Two resolvers over the same alias tables:

- **Strict** (ECMAScript): the name must be exactly a canonical property
  name or alias from `PropertyAliases.txt`, and the value exactly one from
  `PropertyValueAliases.txt`; case-sensitive. Three shapes are accepted, and
  only three (ECMA-262 22.2.2.9.5-9.7): a binary property name alone
  (`\p{Alphabetic}`, `\p{Alpha}`), a **General_Category value** alone
  (`\p{Lu}`, `\p{Uppercase_Letter}`, `\p{L}`), and `name=value` for `gc`,
  `sc` and `scx`. A **lone script value is a `SyntaxError`**: `\p{Greek}`
  is rejected and `\p{Script=Greek}` accepted. An earlier draft of this page
  said otherwise; Node 22 was asked, and it rejects `\p{Greek}` under both
  `u` and `v`. Anything else is `GRX_ERR_SYNTAX` here.

  The set of binary property *names* the strict resolver accepts is **closed**
  and is ECMA-262's own list, not "every binary property in the UCD".
  `\p{Other_Alphabetic}` is a real UCD property and a `SyntaxError` in
  JavaScript; so are `\p{Grapheme_Link}`, `\p{Hyphen}`,
  `\p{Prepended_Concatenation_Mark}` and the rest of the `Other_*` family.
  The list lives in `tools/unicode/gen_tables.py` as `ECMA262_BINARY` and the
  strict name table is built from it. It sat there unread for some time, with
  a comment explaining exactly what it was for, and every UCD binary property
  was reachable from ECMAScript until test262 was imported and said so - the
  same shape as a limit that is in the header and enforced nowhere.

  Two of ECMA-262's names had to be added to the tables rather than filtered
  out of them. `Script=Unknown` (`Zzzz`) is the value of everything
  `Scripts.txt` does not assign, so it is the complement of the file rather
  than an entry in it. `Changes_When_NFKC_Casefolded` lives in
  `DerivedNormalizationProps.txt`, which `fetch.sh` had been downloading and
  the generator had never read.
- **Loose** (Perl, PCRE2, and per UAX #44 §5.9.2): case, whitespace,
  hyphens and underscores are ignored, so `\p{Lowercase_Letter}`,
  `\p{lowercaseletter}` and `\p{LOWERCASE LETTER}` are one property; `Is`
  and `In` prefixes are accepted where the dialect accepts them. PCRE2
  additionally accepts `\p{Xan}`, `\p{Xwd}` and its other synthetic classes,
  which are entries in the PCRE2 hook, not in the tables.

## 7. What the module does not do

- **Normalisation.** No dialect normalises the subject or the pattern; a
  pattern that says `é` as two code points matches two code points.
- **Collation.** POSIX `[[.ch.]]` beyond a single character, and
  `[[=e=]]` equivalence classes, are `GRX_ERR_UNSUPPORTED`; nothing here
  has a collation order.
- **Bidi, line breaking, segmentation** other than the grapheme rules
  `\X` needs.
- **Locale.** There is no locale. A dialect whose reference implementation
  consults one is treated as running in a Unicode locale, and that is
  recorded as a deviation on its [dialects.md](dialects.md) row.
