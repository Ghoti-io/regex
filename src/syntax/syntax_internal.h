/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Regex.
 *
 * Ghoti.io Regex is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Regex is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * The dialect, as the parser and lowering see it: the feature table, the
 * semantic profile, and the lexical hooks.
 *
 * A dialect is three things (documentation/design.md section 4). The feature
 * bits say which constructs exist; the profile says what they *mean*, as
 * small enums that lowering reads and turns into IR; the hooks say how to
 * *read* the ones whose spelling a table cannot describe. A hook never
 * decides meaning and the profile never decides spelling, and keeping the two
 * apart is what keeps the hooks small.
 */

#ifndef GHOTI_IO_GRX_SRC_SYNTAX_SYNTAX_INTERNAL_H
#define GHOTI_IO_GRX_SRC_SYNTAX_SYNTAX_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/syntax.h>
#include <stdint.h>

#include "../core/semantics_internal.h"
#include "../unicode/unicode_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Which code points end a line for `.`, `^` and `$`.
 *
 * documentation/dialects.md section 5.2. The set is named here and built in
 * src/unicode/sets.c, so that "ECMAScript's four line terminators" is written
 * down once rather than in each of the three constructs that need it.
 */
typedef enum {
  GRX_NEWLINES_LF = 0,     ///< `\n` alone. Perl, PCRE2, Python, RE2, Rust.
  GRX_NEWLINES_ECMASCRIPT, ///< LF, CR, U+2028, U+2029.
  GRX_NEWLINES_UNICODE,    ///< The `\R` set: adds CR LF, VT, FF, U+0085.
  GRX_NEWLINES_NONE,       ///< No line breaks at all. POSIX without REG_NEWLINE.
  /**
   * PCRE2's newline conventions, which a pattern may choose for itself.
   *
   * `(*CR)`, `(*CRLF)`, `(*ANYCRLF)`, `(*ANY)` and `(*NUL)`; `(*LF)` is
   * GRX_NEWLINES_LF and is the default both here and there. Each decides
   * two things at once - which single characters `.` refuses, and where
   * `^` and `$` hold - and the three that include a CR LF *pair* make the
   * second question one a code-point set cannot answer on its own. So the
   * pair is asked for separately, with grx_newline_has_crlf().
   *
   * GRX_NEWLINES_CRLF's set of single characters is *empty*: under
   * `(*CRLF)` nothing ends a line by itself, and pcre2test matches `a.b`
   * against both "a\nb" and "a\rb" there.
   */
  GRX_NEWLINES_CR,         ///< `\r` alone.
  GRX_NEWLINES_CRLF,       ///< The two-character pair, and nothing else.
  GRX_NEWLINES_ANYCRLF,    ///< `\r`, `\n`, or the pair.
  GRX_NEWLINES_ANY,        ///< The Unicode set, and the pair.
  GRX_NEWLINES_NUL,        ///< NUL alone.
  GRX_NEWLINES_COUNT       ///< Closes the enum; not a set.
} GRX_NewlineSet;

/**
 * @brief What `$` means when multiline is off.
 *
 * documentation/dialects.md section 5.3, and one of the three differences
 * that a feature bit cannot express: every dialect here spells it `$`.
 */
typedef enum {
  GRX_DOLLAR_END_ONLY = 0,        ///< The end of the subject. ECMAScript, RE2.
  GRX_DOLLAR_BEFORE_FINAL_NEWLINE, ///< Or just before a final newline. Perl.
  GRX_DOLLAR_ALWAYS_LINE,         ///< A line anchor regardless. Ruby.
  GRX_DOLLAR_COUNT                ///< Closes the enum; not a rule.
} GRX_DollarRule;

/**
 * @brief How long a lookbehind body a dialect will accept.
 *
 * A *parse-time* restriction only: the engine implements UNBOUNDED
 * (documentation/design.md section 3.5.2), and this is what makes the library
 * reject what the reference would reject rather than accepting more than the
 * dialect does.
 */
typedef enum {
  GRX_LOOKBEHIND_NONE = 0,          ///< No lookbehind. POSIX, RE2, Rust.
  GRX_LOOKBEHIND_FIXED,             ///< One fixed length overall. Python.
  GRX_LOOKBEHIND_FIXED_PER_BRANCH,  ///< Each alternative fixed. Ruby.
  GRX_LOOKBEHIND_BOUNDED,           ///< Finite maximum. PCRE2, Java.
  GRX_LOOKBEHIND_UNBOUNDED,         ///< Any length. ECMAScript, .NET.
  GRX_LOOKBEHIND_LIMIT_COUNT        ///< Closes the enum; not a constraint.
} GRX_LookbehindLimit;

/**
 * @brief What a *negative* lookaround leaves behind in the capture slots.
 *
 * documentation/dialects.md section 5.17. A negative lookaround succeeds by
 * having its body fail, and the body may have captured something on its way
 * to failing. ECMA-262 22.2.2.4 says those writes are discarded; Perl keeps
 * them, so `a(?!(b)c)` against "abd" reports group 1 as "b" there and unset
 * in both of the others. Probed three ways rather than read: see
 * tests/data/probe/report.md.
 *
 * A *positive* lookaround needs no axis. One that succeeded keeps what its
 * body captured in every dialect, and one that failed takes the whole
 * construct with it, so there is nothing left to disagree about.
 */
typedef enum {
  GRX_NEGATIVE_LOOK_CLEAR = 0, ///< Discarded. ECMAScript, PCRE2.
  GRX_NEGATIVE_LOOK_KEEP,      ///< Kept as the body left them. Perl.
  GRX_NEGATIVE_LOOK_COUNT      ///< Closes the enum; not a rule.
} GRX_NegativeLookCaptures;

/**
 * @brief How grx_regex_split() divides a subject.
 *
 * documentation/dialects.md section 5.16. Three rules move together and are
 * one axis rather than three, because no implementation mixes them:
 *
 * - what an empty subject yields;
 * - whether trailing empty fields survive;
 * - what `limit` counts, and what zero means.
 *
 * Unlike the other axes here this one is a fact about a *library function*
 * rather than about a grammar, so most dialects have no answer of their own:
 * PCRE2, POSIX and GNU define no split at all. Those take `ECMASCRIPT`,
 * which is the library's default rather than a claim about them.
 */
typedef enum {
  /**
   * ECMA-262 22.2.6.14. An empty subject yields one empty piece unless the
   * pattern matches empty; trailing empty pieces are kept; `limit` counts
   * pieces with captures among them, and zero yields none.
   */
  GRX_SPLIT_ECMASCRIPT = 0,
  /**
   * perlfunc. An empty subject yields nothing whatever the pattern; trailing
   * empty *fields* are dropped when no limit was given, a trailing capture
   * standing after them; `limit` counts fields and not captures, the last
   * field is the unsplit remainder, and zero means no limit.
   */
  GRX_SPLIT_PERL,
  /**
   * CPython `re.split`. A hybrid, and the reason this enum has three values
   * rather than two: the empty-subject and trailing-field rules are
   * ECMAScript's - `re.split(",", "")` is `['']` and `re.split(",", "a,b,,")`
   * keeps both trailing empties - while `maxsplit` is Perl's, counting
   * *splits* rather than pieces and leaving the unsplit remainder as the
   * last field, with zero meaning no limit instead of no pieces.
   *
   * Neither existing value was within rounding distance of it in both
   * halves at once: `re.split(",", "a,b,c", maxsplit=1)` is `['a', 'b,c']`
   * where `"a,b,c".split(",", 1)` is `['a']` and perl's `split` drops the
   * trailing empties ECMAScript keeps.
   */
  GRX_SPLIT_PYTHON,
  GRX_SPLIT_COUNT              ///< Closes the enum; not a rule.
} GRX_SplitRule;

/**
 * @brief Which code points `\w`, `\d` and `\s` stand for.
 *
 * documentation/dialects.md section 5.9. One enum rather than three because
 * no dialect mixes them: a dialect that makes `\w` Unicode makes `\d` and
 * `\s` Unicode too.
 */
typedef enum {
  GRX_SHORTHANDS_ASCII = 0,  ///< `[A-Za-z0-9_]`, `[0-9]`, `[ \t\n\v\f\r]`.
  GRX_SHORTHANDS_ECMASCRIPT, ///< ASCII `\w` and `\d`; ECMAScript's wider `\s`.
  GRX_SHORTHANDS_UNICODE,    ///< UTS #18: `\p{Word}`, `\p{Nd}`, `\p{White_Space}`.
  GRX_SHORTHANDS_COUNT       ///< Closes the enum; not a definition.
} GRX_ShorthandSet;

/**
 * @brief Which code points a *Unicode-width* `\w` stands for.
 *
 * documentation/dialects.md section 5.9. Beside GRX_ShorthandSet and not
 * inside it, for the reason GRX_SHORTHANDS_UNICODE's own comment gives about
 * the other two: a dialect that makes `\w` Unicode makes `\d` and `\s`
 * Unicode too, and the three references that answer here agree on `\d` and
 * `\s` exactly while disagreeing about `\w`. A fourth GRX_ShorthandSet value
 * would therefore be claiming a difference in `\d` and `\s` that no reference
 * has, which is the trap two-flags-one-letter describes from the other side.
 *
 * Three values because three were measured, a code point at a time over the
 * 286,719 that perl 5.40.1, pcre2test 10.46 and this library's UCD 17.0.0 all
 * call assigned. Restricting to that intersection is what makes the figures
 * mean anything: unrestricted, this library has 4,800-odd code points neither
 * reference has heard of, and the comparison measures the Unicode release
 * rather than the rule.
 *
 * `\d` and `\s` do not move with this, and neither does the *narrow* `\w`:
 * every dialect's ASCII `\w` is `[0-9A-Za-z_]`.
 */
typedef enum {
  /**
   * UTS #18 Annex C: `\p{alpha}`, `\p{M}`, `\p{Nd}`, `\p{Pc}` and the two
   * join controls. Perl's `\w`, exactly, over all 286,719.
   *
   * The default, so a dialect that has not been measured keeps the answer it
   * had before this enum existed. Java, .NET, Rust, Tcl and Emacs are all in
   * that position - their references are not on this machine, which is open
   * item 4 - and none of them changed when the two that *were* measured moved
   * off it.
   */
  GRX_WORD_UTS18 = 0,
  /**
   * `\p{L}`, `\p{N}`, `\p{Mn}` and `\p{Pc}`. PCRE2's `\w` under `PCRE2_UCP`,
   * exactly.
   *
   * It is not UTS #18 narrowed or widened but crossed: against Annex C it
   * *adds* `\p{No}` (915 code points, U+00B2 among them) and *drops* `Mc`
   * (452), `Me` (13), the alphabetic `So` (130), one `Mn` and the two join
   * controls - 1,513 code points of symmetric difference. There is no subset
   * relation to lean on, which is why this is a set of its own rather than a
   * bit that turns something off.
   */
  GRX_WORD_CATEGORIES,
  /**
   * `\p{L}`, `\p{N}` and `_`. CPython `re`'s `\w` for a `str` pattern,
   * exactly: `SRE_UNI_IS_WORD` is `Py_UNICODE_ISALNUM(ch) || ch == '_'`, and
   * `isalnum` is `L*` plus the numerics.
   *
   * The narrowest of the three - no marks at all, and no connector
   * punctuation but the underscore itself, so U+203F UNDERTIE is a word
   * character in the other two and not here.
   */
  GRX_WORD_ALNUM,
  GRX_WORD_COUNT             ///< Closes the enum; not a definition.
} GRX_WordSet;

/**
 * @brief What `\<` and `\>` compare either side of a position.
 *
 * documentation/dialects.md section 5.12. GNU's two assertions are the word
 * set's two halves: a word character on one side and not on the other. Vim's
 * are not, because vim classifies a code point into one of *nine* classes
 * rather than two, and its assertions hold where the class changes - so `\>`
 * holds between U+65E5 and "x" there, where both are keyword characters and
 * a set can see no boundary at all.
 *
 * The distinction is invisible at the start or end of a word, which is why
 * it survived a differential: the keyword set and "class two or more" are
 * the same 1,108,520 code points, so only a boundary *between* two word
 * characters moves.
 */
typedef enum {
  GRX_WORD_BOUNDARY_SET = 0,   ///< Word on one side, not on the other.
  GRX_WORD_BOUNDARY_VIM_CLASS, ///< Vim: the character class changes.
  GRX_WORD_BOUNDARY_COUNT      ///< Closes the enum; not a rule.
} GRX_WordBoundaryRule;

/**
 * @brief What a composing character is to the matcher.
 *
 * documentation/dialects.md section 5.20. Everywhere but Vim a composing
 * character is a character: `.` matches one, a literal beside it is two
 * atoms, and a match may start or end between a base and its mark. Vim's
 * matcher reads a base character and the composing characters after it as
 * **one** character, which reaches every atom that consumes anything and
 * the two ends of the match as well - so it is a matching model rather
 * than a construct, and this is the axis that says which model.
 */
typedef enum {
  GRX_COMPOSING_SEPARATE = 0, ///< A composing character is a character.
  GRX_COMPOSING_CLUSTER,      ///< Vim: a base and its marks are one.
  GRX_COMPOSING_COUNT         ///< Closes the enum; not a rule.
} GRX_ComposingRule;

/**
 * @brief What a replacement template may say, and how.
 *
 * documentation/dialects.md section 5.11 as bits. A template is a second
 * language with a second grammar, and the grammars differ more than the
 * patterns do: ECMAScript spells a group `$1`, Python spells it `\1`, sed
 * spells the whole match `&`. What they have in common is the *kinds* of
 * thing a template can name, which is what these bits enumerate.
 */
#define GRX_TMPL_NUMBER GRX_BIT(0)        ///< `$1`, one or two digits.
#define GRX_TMPL_NUMBER_BRACED GRX_BIT(1) ///< `${1}`.
#define GRX_TMPL_NAME_ANGLE GRX_BIT(2)    ///< `$<name>`.
#define GRX_TMPL_NAME_BRACED GRX_BIT(3)   ///< `${name}`.
#define GRX_TMPL_WHOLE GRX_BIT(4)         ///< `$&`.
#define GRX_TMPL_PREFIX GRX_BIT(5)        ///< `` $` ``: the text before it.
#define GRX_TMPL_SUFFIX GRX_BIT(6)        ///< `$'`: the text after it.
#define GRX_TMPL_DOUBLE_SIGIL GRX_BIT(7)  ///< `$$` is a literal `$`.
/**
 * @brief A named reference exists only when the pattern has named groups.
 *
 * ECMAScript's rule, and it is not a nicety: `$<x>` against a pattern with
 * no named groups is *literal text*, so a caller who mistypes a group name
 * gets their template back rather than an empty string. With named groups
 * present the same spelling is a reference, and an unknown name there
 * substitutes nothing.
 */
#define GRX_TMPL_NAME_NEEDS_NAMED_GROUPS GRX_BIT(8)
/**
 * @brief `$name`: a name with no bracket around it at all.
 *
 * PCRE2's, and the one spelling that has to be read greedily - `$1x` is the
 * group named `1x` where there is one, so a reader that stopped at the first
 * character that could not continue a number would get it wrong.
 */
#define GRX_TMPL_NAME_BARE GRX_BIT(9)
/** @brief Perl's `$+{name}`, the named-capture hash. */
#define GRX_TMPL_NAME_PLUS_BRACE GRX_BIT(10)
/**
 * @brief Python's `\g<...>`, which takes a name *or* a number.
 *
 * `re.sub` spells both kinds of reference through one form: `\g<name>`,
 * `\g<1>` and `\g<0>` are all it, and `\g<0>` is the whole match where a
 * bare `\0` is NUL. No other bit here fits, because every other spelling
 * decides between a name and a number by which bracket it uses - and
 * because this one is introduced by a *letter* after the sigil rather than
 * by a bracket, so a reader looking only at the next character would read
 * `\g` as an unknown escape.
 *
 * A number here is not the same as GRX_TMPL_NUMBER's: `\g<01>` is group 1,
 * where a leading zero is the whole match in the dialects with
 * GRX_TMPL_WHOLE_ZERO.
 */
#define GRX_TMPL_G_ANGLE GRX_BIT(20)
/**
 * @brief Python's template escapes: the C ones, octal, and an error.
 *
 * `re.sub` reads a template with the same closed-alphabet discipline the
 * pattern has. `\n`, `\t`, `\r`, `\f`, `\v`, `\a` and `\b` are the control
 * characters they name; `\\` is a backslash; `\0`, `\01` and `\012` are
 * octal, so `\0` is NUL where `\g<0>` is the whole match; `A`,
 * `\U00000041` and `\N{NAME}` are code points; and every other letter is
 * "bad escape", not the letter itself.
 *
 * That last clause is why this is not GRX_TMPL_ESCAPE_ANY, which is sed's
 * rule that anything after the backslash stands for itself. Under sed's
 * rule `\n` is the letter n and `\q` is a q; under Python's the first is a
 * newline and the second is an error.
 */
#define GRX_TMPL_PYTHON_ESCAPES GRX_BIT(21)
/**
 * @brief A backslash escapes the next character, rather than a doubled sigil.
 *
 * Perl's templates are interpolated strings: `\$` is a literal dollar and
 * `$$` is the process id. The two rules are mutually exclusive, and a
 * dialect that had both would be one whose templates cannot be written.
 */
/**
 * A bare `&` is the whole match, and `<sigil>&` is a literal `&`.
 *
 * sed's, and the reverse of GRX_TMPL_WHOLE's `$&`. POSIX defines exactly
 * this for the `s` command's right-hand side, and it is the only sigil the
 * POSIX and GNU rows have besides the backslash.
 */
#define GRX_TMPL_WHOLE_BARE GRX_BIT(12)

/**
 * One digit names a group, never two.
 *
 * `\1` through `\9`, so `\10` is group 1 followed by a `0` even where ten
 * groups exist - probed against sed 4.9, which answers "a0" for a pattern
 * with ten of them. GRX_TMPL_NUMBER takes one *or* two and is the other
 * dialects' rule.
 */
#define GRX_TMPL_NUMBER_SINGLE GRX_BIT(13)

/**
 * `<sigil>0` is the whole match.
 *
 * GNU sed's, not POSIX's, which is why it is a bit rather than part of
 * GRX_TMPL_NUMBER_SINGLE.
 */
#define GRX_TMPL_WHOLE_ZERO GRX_BIT(14)

/**
 * `<sigil>c`, for any `c` the rules above do not claim, is a literal `c`.
 *
 * sed again: `\q` is `q` and `\\` is a backslash, so the escape is total
 * rather than a list. The other dialects have no such rule - ECMAScript's
 * `$q` is `$q`, two characters, because there the sigil is only special
 * before something it recognises.
 */
#define GRX_TMPL_ESCAPE_ANY GRX_BIT(15)

/**
 * @brief A sigil that begins no complete reference is an error.
 *
 * ECMAScript's rule is the opposite and is the one this library applied
 * everywhere: every spelling it does not recognise is ordinary text, so `$`
 * at the end of a template is a dollar sign and `${1` is four characters.
 * PCRE2 refuses all of them - "invalid replacement string" for a sigil that
 * begins nothing, "expected closing curly bracket" for a `${` with no `}`.
 *
 * Distinct from GRX_TemplateMissing, which is about a reference that is well
 * formed and names a group the pattern has not got. `$9` against two groups
 * is that; `${1` is this. A dialect can want either answer to either
 * question, and PCRE2 wants an error to both.
 *
 * Not set on the perl row. perl's replacement is an interpolated string
 * rather than a grammar of its own, and it cannot be asked through a driver
 * (tools/oracle/perl_diff.py says why), so the row keeps the rule it was
 * written with rather than gaining one by analogy.
 */
#define GRX_TMPL_SIGIL_STRICT GRX_BIT(16)

/**
 * @brief `<sigil><digits>` takes every digit, and does not fall back.
 *
 * PCRE2's rule. ECMAScript's, GRX_TMPL_NUMBER, tries two digits and then
 * one, so `$12` against a two-group pattern is group 1 followed by "2" and
 * against a twelve-group pattern is group 12 - a reference whose meaning
 * depends on the pattern it is used with. PCRE2 reads the whole run as one
 * number and refuses it if the pattern has not got that group: `$12` with
 * one group is an error, not `$1` and a "2".
 *
 * Leading zeros belong to the number, so `$01` is group 1 - which is why
 * this claims a leading `0` before GRX_TMPL_WHOLE_ZERO can. `$0` on its own
 * is still the whole match, because the number is then zero and zero is no
 * group.
 */
#define GRX_TMPL_NUMBER_GREEDY GRX_BIT(17)

/**
 * @brief `<sigil>_` is the whole subject.
 *
 * PCRE2's, and not the same as GRX_TMPL_WHOLE: `$&` is the text the pattern
 * matched and `$_` is every byte of the subject, match and all. `$&` on
 * "xay" against `(a)` gives "a" and `$_` gives "xay".
 *
 * Not on the perl row. perl's `$_` in a replacement is the default variable,
 * which is a fact about the surrounding program rather than about the
 * template, and a row claiming it would be claiming something else.
 */
#define GRX_TMPL_SUBJECT GRX_BIT(18)

/**
 * @brief A reference to a group that did not participate is an error.
 *
 * PCRE2's default, and the one template rule that cannot be checked when the
 * template is read: whether group 1 is set depends on the match, so
 * `(a)?b` with `$1` is an error against "b" and a substitution against "ab".
 * PCRE2_SUBSTITUTE_UNSET_EMPTY is the option that turns it off, which is why
 * it is a default rather than an oddity.
 *
 * A second axis to GRX_TemplateMissing, not a value of it. That enum answers
 * "the pattern has no such group"; this answers "it has one and it is
 * unset", and ECMAScript says LITERAL to the first and empty to the second
 * while PCRE2 says error to both.
 */
#define GRX_TMPL_UNSET_ERROR GRX_BIT(19)

#define GRX_TMPL_BACKSLASH_ESCAPE GRX_BIT(11)

/**
 * @brief Vim's replacement escapes: the four controls and the case markers.
 *
 * `\n`, `\r`, `\t` and `\b` decode to U+000A, U+000D, U+0009 and
 * U+0008 - over a *string*, which is the subject this library has; in a
 * buffer `:s` writes a line break for `\r` and a NUL for `\n`, which is
 * the same two characters seen through a different container. `\u`, `\l`,
 * `\U`, `\L`, `\E` and `\e` change the case of what follows. Every
 * other escape falls through to GRX_TMPL_ESCAPE_ANY, which vim also has:
 * `\q` is a `q` there. Enumerated over the whole printable alphabet, and
 * these ten are the entire list.
 */
#define GRX_TMPL_VIM_ESCAPES GRX_BIT(22)

/** @brief What a reference to a group the pattern does not have does. */
typedef enum {
  GRX_TMPL_MISSING_LITERAL = 0, ///< The text stands as written. ECMAScript.
  GRX_TMPL_MISSING_EMPTY,       ///< It substitutes nothing. Perl, Ruby, Go.
  GRX_TMPL_MISSING_ERROR,       ///< The call fails. PCRE2, Python, Java.
  GRX_TMPL_MISSING_COUNT        ///< Closes the enum; not a rule.
} GRX_TemplateMissing;

/**
 * @brief A dialect's replacement-template grammar.
 *
 * A zeroed row - `sigil` of 0 - is a dialect whose grammar has not been
 * written yet, and grx_regex_replace() refuses it rather than guessing.
 * Nothing but ECMAScript can reach this today, because nothing but
 * ECMAScript has a front end to compile a pattern with.
 */
typedef struct GRX_TemplateSpec {
  char sigil;          ///< The character that introduces a reference; 0 = none.
  uint32_t features;   ///< GRX_TMPL_* bits.
  GRX_TemplateMissing missing; ///< A reference to a group that does not exist.
} GRX_TemplateSpec;

/**
 * @brief What the constructs mean, once the dialect has them.
 *
 * Every field is a value from documentation/dialects.md section 5, and every
 * one of them is something lowering turns into an explicit IR node, flag or
 * mode. Nothing here reaches an engine: by the time a program exists, these
 * have all been spent.
 *
 * The axes a dialect beyond tier 1 needs are added when that dialect lands,
 * rather than being declared now with no implementation to check them.
 */
typedef struct GRX_Profile {
  GRX_MatchPreference preference;   ///< Which match a search reports.
  GRX_SubmatchRule submatch;        ///< Which division of it the groups get.
  GRX_EmptyLoopMode empty_loop;     ///< An iteration that consumed nothing.
  GRX_CaptureResetMode capture_reset; ///< Captures between iterations.
  GRX_BackrefUnsetMode backref_unset; ///< A reference to an unset group.
  GRX_LookbehindLimit lookbehind;   ///< How long a lookbehind may be.
  GRX_NegativeLookCaptures negative_look; ///< Captures a failed body made.
  GRX_SplitRule split;              ///< How grx_regex_split() divides.
  GRX_IterationRule iteration;      ///< Search-all after an empty match.
  GRX_SearchStartRule search_start; ///< What `\G` asserts while iterating.
  GRX_DollarRule dollar;            ///< `$` without multiline.
  GRX_NewlineSet newlines;          ///< The line-terminator set.
  GRX_ShorthandSet shorthands;      ///< `\w`, `\d`, `\s` by default.
  GRX_WordBoundaryRule word_boundary; ///< What `\<` and `\>` compare.
  GRX_ComposingRule composing;      ///< What a composing character is.
  /**
   * @brief The same, once the dialect's *widening* flag is set.
   *
   * GRX_OPT_UCP, and not GRX_OPT_UTF. pcre2pattern is explicit that `\w`,
   * `\d` and `\s` use Unicode "only if PCRE2_UCP is set", exactly as the
   * POSIX classes do, and Java's widening flag is
   * UNICODE_CHARACTER_CLASS - which is what GRX_OPT_UCP stands for. Those
   * two are the only dialects whose columns differ.
   *
   * It was called `shorthands_utf` and was read on `UTF || UCP`, so
   * `(*UTF)\w` matched "é" where pcre2test does not. The field's *name*
   * was the whole of the error: section 5.9 of documentation/dialects.md
   * has said "ASCII; Unicode under `UCP`" since it was written.
   *
   * Case folding is the other way round and keys on UTF, which is why the
   * two are computed separately: `(*UTF)(?i)é` matches "É" in pcre2test
   * with no UCP anywhere.
   */
  GRX_ShorthandSet shorthands_wide;
  /**
   * Which of the three word sets a Unicode-width `\w` denotes.
   *
   * Read wherever `shorthands` or `shorthands_wide` has settled on
   * GRX_SHORTHANDS_UNICODE, and ignored otherwise: an ASCII-width `\w` is
   * `[0-9A-Za-z_]` in every dialect here, so this field says nothing about
   * one. `[[:word:]]` and `\b` are the same set by definition and move with
   * it - measured, not assumed: `\w` and `[[:word:]]` returned the identical
   * 139,612 code points in perl and the identical 139,929 in pcre2test.
   *
   * See GRX_WordSet for what each value is and how it was measured.
   */
  GRX_WordSet word_set;
  /**
   * `[[:graph:]]` and `[[:print:]]` count private-use code points.
   *
   * Perl does and PCRE2 does not, and it is the largest single disagreement
   * between the two anywhere in this library: 137,468 code points, the whole
   * of `\p{Co}`. `[[:graph:]]` matches U+E000 in perl with the subject
   * upgraded and does not in pcre2test under `utf,ucp`.
   *
   * Neither is unreasonable, which is why it is an axis rather than a defect
   * in one of them. A private-use code point has whatever glyph the agreeing
   * parties gave it, so "it marks the page" is unknowable; perl assumes it
   * does and PCRE2 assumes nothing.
   *
   * Left clear by every dialect with no reference on this machine, which is
   * the answer they already had.
   */
  int posix_graph_takes_private_use;
  /**
   * `[[:graph:]]` and `[[:print:]]` drop six format characters by name.
   *
   * PCRE2 excludes U+061C ARABIC LETTER MARK, U+180E MONGOLIAN VOWEL
   * SEPARATOR and U+2066 to U+2069, the four bidi isolates, from
   * `[[:graph:]]`; perl keeps all six. It is a hand-written list in the
   * reference and a hand-written list here, because it is one in Unicode
   * too: they are the `Cf` characters that are defined never to mark the
   * page, and no property collects exactly them.
   *
   * A second field rather than a wider spelling of
   * posix_graph_takes_private_use, because the two are independent facts
   * that happen to point the same way in the only two dialects that have
   * been measured: this one makes PCRE2's set *narrower* than perl's, that
   * one makes perl's *wider* than PCRE2's, and a dialect could want either
   * without the other.
   *
   * `[[:print:]]` drops five of the six and keeps U+180E, which PCRE2
   * classes as a space separator - and `print` is `graph` plus the space
   * separators. That is measured rather than reasoned: pcre2test's `print`
   * matches U+180E and its `graph` does not.
   */
  int posix_graph_drops_invisibles;
  /**
   * The wide POSIX classes are general categories, not derived properties.
   *
   * PCRE2 spells `[[:alpha:]]` as `\p{L}`, `[[:alnum:]]` as `\p{L}\p{N}`,
   * `[[:lower:]]` as `\p{Ll}` and `[[:upper:]]` as `\p{Lu}`. Perl spells the
   * same four with `Alphabetic`, `Alphabetic` plus `Nd`, `Lowercase` and
   * `Uppercase`, which are the derived properties UTS #18 Annex C names.
   *
   * It is one field and not four because it is one decision in the
   * reference, and the four move together: measured a code point at a time
   * over the 286,719 that perl, pcre2test 10.46 and UCD 17.0.0 all call
   * assigned, every one of the four is exactly its general category in
   * pcre2test and exactly its derived property in perl. The differences are
   * 1,694 code points for `alpha`, 2,373 for `alnum`, 312 for `lower` and
   * 120 for `upper` - the marks with Other_Alphabetic, the letter-numbers,
   * the circled letters, and the modifier letters that are Other_Lowercase.
   *
   * GRX_Profile::word_set is the same decision for `\w` and `[[:word:]]`,
   * and is *not* folded in here: PCRE2's word set is `\p{L}\p{N}\p{Mn}\p{Pc}`,
   * which is not the general-category reading of Annex C's `alpha` term but
   * a different list, and `re` needs a third answer that these four names
   * never ask for.
   */
  int posix_wide_general_category;
  /**
   * U+180E MONGOLIAN VOWEL SEPARATOR counts as horizontal whitespace.
   *
   * PCRE2 matches it with `\h`, `\s`, `[[:blank:]]` and `[[:space:]]` and
   * refuses it with `\v`; perl matches it with none of them. It was `Zs`
   * until Unicode 6.3 and has been `Cf` since, and PCRE2 kept it where it
   * was - which is also why its `[[:print:]]` takes it while its
   * `[[:graph:]]` does not, `print` being `graph` plus the space
   * separators.
   *
   * It does not move with a charset modifier the way the rest of `\s` does,
   * and that asymmetry is measured rather than assumed: `(?a)\h` still
   * matches it in pcre2test and `(?a)\s` does not, which is the `\h`/`\v`
   * rule of 2026-09-24 holding - those two are fixed sets that nothing
   * narrows, and this field changes which fixed set PCRE2's is by one
   * member. `\s`, `[[:blank:]]` and `[[:space:]]` take it only at the
   * Unicode width.
   */
  int mongolian_separator_is_space;
  /**
   * GRX_OPT_ASCII_CLASSES makes the folding ASCII-only as well.
   *
   * Python's `(?a)` against Perl's `/a`, which is the whole of the
   * difference between them. Both narrow `\w`, `\d` and `\s` to ASCII.
   * Perl leaves the folding alone - `/ai` still matches `s` against U+017F,
   * and only the second `a` of `/aa` cuts the orbit at U+0080. CPython's
   * one flag does both, and does it harder than `/aa` does: under `(?ai)`
   * U+00C0 does not match U+00E0, where `/aai` still matches them because
   * neither is ASCII.
   *
   * Not spelled as a second option bit, because a caller does not choose it
   * - it is what the dialect's single ASCII flag means.
   */
  int ascii_classes_fold_ascii;
  /**
   * A caseless mode widens `\w`, `\d`, `\s` and `\b` by folding the set.
   *
   * ECMA-262 22.2.2.9.3 defines WordCharacters(rer) as the basic word
   * characters *plus* every character that canonicalises to one, so under
   * `iu` `\w` matches U+017F and U+212A and `\b` reads them as word
   * characters. That is one dialect's rule and it was written here as
   * everyone's, which made three references wrong at once. Measured, each
   * against its own reference, with `\w` narrowed to ASCII so that the
   * question is the folding and not the width:
   *
   * | | `\w` on U+017F | `x\b` on "x" U+212A |
   * | --- | --- | --- |
   * | node, `iu` | match | no match - the boundary is gone |
   * | pcre2test, `(?i)` | no match | 0-1 |
   * | perl, `/ai` | no match | 0-1 |
   * | CPython, `(?ai)` | no match | 0-1 |
   *
   * The literal and the range are a separate question and fold in all four:
   * `(?i)s` and `(?i)[a-z]` both match U+017F in pcre2test. So this is the
   * *shorthand set* alone, which is why it sits beside the shorthand
   * fields rather than beside the fold ones.
   */
  int caseless_widens_shorthands;
  GRX_FoldKind fold;                ///< Caseless folding without UTF.
  GRX_FoldKind fold_utf;            ///< Caseless folding with UTF.
  GRX_PropertyMatch property_match; ///< How `\p{...}` names are spelled.
  GRX_TemplateSpec template_spec;   ///< The replacement-template grammar.
  int multiline_by_default;         ///< Ruby: `^`/`$` are always line anchors.
  /**
   * `^` with multiline matches after a newline that ends the subject.
   *
   * ECMA-262 asks one question - is the character before this position a
   * line terminator - and so answers yes at the end of "a\n". PCRE2 and
   * Perl both say no, on the ground that the empty run after a final
   * newline is not a line. Python answers ECMAScript's way; Java documents
   * PCRE2's. The rows of dialects with no front end hold whichever of the
   * two nobody has had to check yet, which is what a zero here means.
   * See GRX_ASSERT_START_LINE_INTERIOR.
   */
  int caret_after_final_newline;
  /**
   * The dialect's subject is text, not bytes, whatever the options say.
   *
   * PCRE2 without `PCRE2_UTF` matches bytes, and that is the right thing for
   * it: its subject is a byte string. ECMAScript's subject is a sequence of
   * characters in every mode - the `u` flag changes the *grammar* and the
   * folding, not whether a subject is text - so decoding it as UTF-8 is
   * unconditional here. Reading it as bytes instead would make `.` match a
   * third of a character and `[^x]` match a continuation byte, which is not
   * a closer approximation of ECMA-262 than decoding is; it is a worse one.
   */
  int subject_is_text;
  /**
   * `[[:lower:]]` and `[[:upper:]]` are Unicode where the other ten are not.
   *
   * Vim's, and measured rather than read: `[[:lower:]]` matches "é" and
   * `[[:upper:]]` matches "É", while `[[:alpha:]]`, `[[:alnum:]]`,
   * `[[:punct:]]`, `[[:graph:]]` and `[[:word:]]` all refuse them. The
   * split is not arbitrary - vim answers the case classes from its own
   * Unicode tables and the rest from the C library's ASCII predicates - but
   * it is a split no single width can express, which is why this is a flag
   * beside the width rather than a third value of it.
   *
   * `[[:print:]]` is the third class vim widens and is deliberately not
   * here: its members are vim's `utf_printable()` table, which is neither
   * a property nor a range - U+200B is not printable there and U+2028 is -
   * and a flag that claimed it would be claiming something else.
   * documentation/dialects.md section 6.
   */
  int posix_case_classes_wide;
  /**
   * ...and the wide pair asks for a case *counterpart*, not for a property.
   *
   * Read only where posix_case_classes_wide has already widened them, and
   * it is the third answer those two names have here - `\p{Lowercase}` and
   * `\p{Uppercase}` for perl, `\p{Ll}` and `\p{Lu}` for PCRE2 (see
   * posix_wide_general_category), and for vim "there is a code point to map
   * to". The three are 2,595, 2,233 and 1,478 code points, so none of them
   * is a rounding of another.
   *
   * Measured against vim 9.1 a code point at a time over all 1,112,064:
   * `[[:upper:]]` is exactly the domain of the simple lowercase map and
   * `[[:lower:]]` exactly that of the simple uppercase map plus U+00DF,
   * whose full uppercase is "SS" and which therefore has no simple one.
   * This library had been answering `\p{Lowercase}` and `\p{Uppercase}`,
   * wrong by 1,125 and 608 - the modifier letters, the small Roman
   * numerals, the circled capitals and the titlecase letters.
   *
   * The field's neighbour above was measured on "é" and "É" when it was
   * written, and being *wide* was the right answer for both; which wide set
   * is a question two examples cannot reach, and that is how this sat here
   * for as long as it did.
   */
  int posix_case_classes_case_mapped;
  /**
   * A caseless flag collapses `[:lower:]` and `[:upper:]` at the *Unicode*
   * width too, and not only at the ASCII one.
   *
   * The collapse itself is not in question: `[[:lower:]]` under a caseless
   * flag matches "A" in perl, in pcre2test, and in glibc and musl under
   * REG_ICASE. It is the union of the two classes rather than a closure of
   * either, which U+05D0 settles - alphabetic and uncased, refused by
   * `(?i)[[:lower:]]` in both references where `[[:alpha:]]` takes it.
   *
   * Where they part is the other width. perl collapses them there as well,
   * so `/i` over an upgraded subject takes "A", U+017F, U+212A and U+00C9;
   * pcre2 stops, and `(*UCP)(?i)[[:lower:]]` refuses "A" while still
   * matching U+017F, which is `Ll` on its own account. Each dialect
   * follows its own reference, which is why this is an axis and not a
   * deviation: PCRE2 leaves it clear and Perl sets it.
   *
   * Only reached at the Unicode width. At the ASCII width every dialect
   * here collapses, so the field says nothing about POSIX, GNU or vim -
   * and vim makes no collapse at all, which GRX_CLASS_ITEM_NO_FOLD
   * already says per item.
   */
  int posix_case_classes_collapse_wide;
} GRX_Profile;

/**
 * @brief The dialect table, indexed by GRX_Syntax.
 *
 * The parser reads this rather than switching on the dialect constant, so
 * that a construct is accepted or rejected in exactly one place.
 *
 * @return A table of GRX_SYNTAX_COUNT entries. Never NULL.
 */
const GRX_SyntaxSpec * grx_syntax_spec_table(void);

/**
 * @brief The semantic profile of a dialect.
 *
 * @param syntax The dialect.
 * @param out_profile Receives the profile on success.
 * @return GRX_OK, or GRX_ERR_INVALID for an unknown dialect or a NULL output.
 */
GRX_Result grx_syntax_profile(GRX_Syntax syntax, GRX_Profile * out_profile);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_SYNTAX_SYNTAX_INTERNAL_H
