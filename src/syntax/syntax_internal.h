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
  GRX_FoldKind fold;                ///< Caseless folding without UTF.
  GRX_FoldKind fold_utf;            ///< Caseless folding with UTF.
  GRX_PropertyMatch property_match; ///< How `\p{...}` names are spelled.
  GRX_TemplateSpec template_spec;   ///< The replacement-template grammar.
  /**
   * A recursion or subroutine call cannot be backtracked into.
   *
   * pcre2pattern says a recursive call is treated as an atomic group; Perl's
   * is not, and `aa$|a(?R)a|a` against "aaa" is where the two part - Perl
   * reports the whole string by letting the call give back what it took, and
   * PCRE2 reports one character. An axis rather than a rule in lowering,
   * because it is exactly the kind of difference a table is for.
   */
  int recursion_is_atomic;
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
