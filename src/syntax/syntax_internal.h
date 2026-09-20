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
 *
 * Copyright 2026 by Corey Pennycuff
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
  GRX_IterationRule iteration;      ///< Search-all after an empty match.
  GRX_DollarRule dollar;            ///< `$` without multiline.
  GRX_NewlineSet newlines;          ///< The line-terminator set.
  GRX_ShorthandSet shorthands;      ///< `\w`, `\d`, `\s` without UTF.
  GRX_ShorthandSet shorthands_utf;  ///< The same, with UTF or UCP.
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
