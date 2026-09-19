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
  GRX_IterationRule iteration;      ///< Search-all after an empty match.
  GRX_DollarRule dollar;            ///< `$` without multiline.
  GRX_NewlineSet newlines;          ///< The line-terminator set.
  GRX_ShorthandSet shorthands;      ///< `\w`, `\d`, `\s` without UTF.
  GRX_ShorthandSet shorthands_utf;  ///< The same, with UTF or UCP.
  GRX_FoldKind fold;                ///< Caseless folding without UTF.
  GRX_FoldKind fold_utf;            ///< Caseless folding with UTF.
  GRX_PropertyMatch property_match; ///< How `\p{...}` names are spelled.
  int multiline_by_default;         ///< Ruby: `^`/`$` are always line anchors.
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
