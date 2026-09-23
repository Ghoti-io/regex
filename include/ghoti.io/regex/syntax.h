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
 * Dialect selection and syntax options for the Ghoti.io Regex library.
 *
 * A "dialect" here is a named point in the space of regular-expression
 * syntaxes - POSIX ERE, PCRE, ECMAScript, and so on. It is represented as
 * data (@ref GRX_SyntaxSpec) rather than as branches through the parser, so
 * that adding a dialect is a table entry and the difference between two
 * dialects can be read in one place instead of inferred from `if` statements
 * scattered through the parse.
 *
 * Status: the dialect list is fixed here; the per-dialect feature tables are
 * the subject of documentation/dialects.md and are not yet populated.
 */

#ifndef GHOTI_IO_GRX_SYNTAX_H
#define GHOTI_IO_GRX_SYNTAX_H

#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/macros.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief The regular-expression dialect a pattern is written in.
 *
 * Each names a real implementation's syntax rather than a family, because the
 * families are not internally consistent: GNU grep's BRE is not POSIX BRE,
 * and Python's `re` is not Perl's. Where two implementations do agree, they
 * share a @ref GRX_SyntaxSpec rather than an enum constant.
 */
typedef enum {
  GRX_SYNTAX_POSIX_BRE = 0, ///< POSIX basic REs (IEEE Std 1003.1 chapter 9.3).
  GRX_SYNTAX_POSIX_ERE,     ///< POSIX extended REs (chapter 9.4).
  GRX_SYNTAX_GNU_BRE,       ///< POSIX BRE plus GNU's `\|`, `\+`, `\?`, `\b`.
  GRX_SYNTAX_GNU_ERE,       ///< POSIX ERE plus the GNU escapes.
  GRX_SYNTAX_PERL,          ///< Perl 5.
  GRX_SYNTAX_PCRE,          ///< PCRE2, which is Perl 5 with deviations.
  GRX_SYNTAX_ECMASCRIPT,    ///< ECMA-262, the `RegExp` grammar.
  GRX_SYNTAX_PYTHON,        ///< CPython's `re`.
  GRX_SYNTAX_JAVA,          ///< `java.util.regex.Pattern`.
  GRX_SYNTAX_DOTNET,        ///< .NET `System.Text.RegularExpressions`.
  GRX_SYNTAX_RUBY,          ///< Ruby's Onigmo.
  GRX_SYNTAX_RE2,           ///< RE2, and so Go's `regexp`.
  GRX_SYNTAX_RUST,          ///< The Rust `regex` crate.
  GRX_SYNTAX_TCL,           ///< Tcl's advanced REs (Spencer).
  GRX_SYNTAX_VIM,           ///< Vim's "magic" syntax.
  GRX_SYNTAX_EMACS,         ///< Emacs Lisp regexps.
  GRX_SYNTAX_COUNT          ///< Closes the enum; not a dialect.
} GRX_Syntax;

/**
 * @brief Compile-time options, orthogonal to the dialect.
 *
 * These are the switches every dialect spells differently - `/i`, `re.I`,
 * `(?i)` - reduced to one set of bits. A dialect that cannot express one of
 * them in its own syntax still accepts it here, because the caller is not
 * limited to what the pattern text can say.
 */
typedef enum {
  GRX_OPT_NONE = 0,
  GRX_OPT_CASELESS = GRX_BIT(0),   ///< Case-insensitive matching.
  GRX_OPT_MULTILINE = GRX_BIT(1),  ///< `^`/`$` match at internal line breaks.
  GRX_OPT_DOTALL = GRX_BIT(2),     ///< `.` matches a newline.
  GRX_OPT_EXTENDED = GRX_BIT(3),   ///< Ignore whitespace and `#` comments.
  GRX_OPT_UNGREEDY = GRX_BIT(4),   ///< Invert the greediness of quantifiers.
  GRX_OPT_ANCHORED = GRX_BIT(5),   ///< Match only at the start offset.
  GRX_OPT_UTF = GRX_BIT(6),        ///< Treat pattern and subject as UTF-8.
  GRX_OPT_UCP = GRX_BIT(7),        ///< `\w`, `\d`, `\b` use Unicode properties.
  GRX_OPT_NO_CAPTURE = GRX_BIT(8), ///< Treat `(` as non-capturing.
  GRX_OPT_LITERAL = GRX_BIT(9),    ///< Match the pattern as plain text.
  /**
   * ECMAScript's `v` flag: class set operations and string disjunctions.
   *
   * Implies GRX_OPT_UTF, and is mutually exclusive with writing `u` as well
   * - which is a rule about the *flag string*, so grx_options_parse()
   * enforces it rather than this bit.
   */
  GRX_OPT_UNICODE_SETS = GRX_BIT(10),
  /**
   * PCRE2's and Perl's `xx`: extended mode, and inside a class as well.
   *
   * A separate bit rather than a second meaning for GRX_OPT_EXTENDED,
   * because `xx` is strictly wider: it ignores unescaped space and tab
   * *within* a bracket expression, where `x` does not. Setting it without
   * GRX_OPT_EXTENDED is not a mode either dialect has, and the front end
   * that reads `xx` sets both.
   */
  GRX_OPT_EXTENDED_MORE = GRX_BIT(11),
  /**
   * PCRE2's `J` and `PCRE2_DUPNAMES`: two groups may share a name.
   *
   * An option rather than a front-end detail because a caller can set it
   * without writing `(?J)`, which is exactly what pcre2_compile()'s flag
   * does. What it turns off is a check, so a pattern that is valid without
   * it is valid with it.
   */
  GRX_OPT_DUPLICATE_NAMES = GRX_BIT(12),
  /**
   * @brief `\w`, `\d`, `\s`, `\b` and the POSIX classes are ASCII.
   *
   * Perl's `/a`, and the narrowing counterpart of GRX_OPT_UCP: it holds the
   * shorthands to their ASCII definitions whatever the subject is. Perl's
   * `/l` sets it too - this library has no locale and says so
   * (documentation/dialects.md section 6), and the C locale's answer to
   * "which characters are word characters" is the ASCII one.
   */
  GRX_OPT_ASCII_CLASSES = GRX_BIT(13),
  /**
   * @brief Caseless matching does not fold across the ASCII boundary.
   *
   * The second `a` of Perl's `/aa`. Under `/ai` the letter `s` matches
   * U+017F, because simple folding puts them in one orbit; under `/aai` it
   * does not, while U+00C0 and U+00E0 still match each other. It is
   * therefore not "fold ASCII only", which would stop the second pair as
   * well.
   */
  GRX_OPT_ASCII_FOLD_SEPARATE = GRX_BIT(14),
  /**
   * @brief A line terminator is not "any character".
   *
   * The half of POSIX's `REG_NEWLINE` that GRX_OPT_MULTILINE is not.
   * `REG_NEWLINE` is two rules: `^` and `$` become line anchors, *and* a
   * newline is matched by neither `.` nor a non-matching list that does not
   * contain one. A caller emulating `regcomp(..., REG_NEWLINE)` sets both
   * bits; they are separate because the two rules are independent and
   * nothing else in the library wants them welded.
   *
   * What it names is the dialect's line-terminator set and not the byte
   * `\n`, so under a dialect whose terminators are ECMAScript's it takes
   * all four out. In the POSIX and GNU dialects, where it is what
   * `REG_NEWLINE` means, that set is `\n` alone.
   *
   * It reaches `.` and an ordinary bracket expression, which is what
   * POSIX's rule names. A *positive* list is untouched: `[\n]` under
   * `REG_NEWLINE` still matches a newline in glibc and in musl both.
   */
  GRX_OPT_NEWLINE_TERMINATES = GRX_BIT(15)
} GRX_Option;

/**
 * @brief Features a dialect may or may not have.
 *
 * The parser consults these rather than the dialect constant, so that a
 * construct is accepted or rejected in one place. A construct absent from the
 * dialect is GRX_ERR_SYNTAX when the dialect gives the character another
 * meaning (`+` in POSIX BRE is a literal plus), and GRX_ERR_UNSUPPORTED when
 * the dialect has it and this library does not yet.
 */
typedef enum {
  GRX_FEATURE_NONE = 0,
  GRX_FEATURE_ALTERNATION = GRX_BIT(0),    ///< `a|b`.
  GRX_FEATURE_BOUNDED_REPEAT = GRX_BIT(1), ///< `a{2,5}`.
  GRX_FEATURE_NON_GREEDY = GRX_BIT(2),     ///< `a+?`.
  GRX_FEATURE_POSSESSIVE = GRX_BIT(3),     ///< `a++`.
  GRX_FEATURE_NON_CAPTURING = GRX_BIT(4),  ///< `(?:a)`.
  GRX_FEATURE_NAMED_CAPTURE = GRX_BIT(5),  ///< `(?<name>a)`.
  GRX_FEATURE_BACKREFERENCE = GRX_BIT(6),  ///< `\1`, `\k<name>`.
  GRX_FEATURE_LOOKAHEAD = GRX_BIT(7),      ///< `(?=a)`, `(?!a)`.
  GRX_FEATURE_LOOKBEHIND = GRX_BIT(8),     ///< `(?<=a)`, `(?<!a)`.
  GRX_FEATURE_ATOMIC_GROUP = GRX_BIT(9),   ///< `(?>a)`.
  GRX_FEATURE_CONDITIONAL = GRX_BIT(10),   ///< `(?(1)a|b)`.
  GRX_FEATURE_RECURSION = GRX_BIT(11),     ///< `(?R)`, `(?1)`.
  GRX_FEATURE_INLINE_FLAGS = GRX_BIT(12),  ///< `(?i)`, `(?i:a)`.
  GRX_FEATURE_COMMENT_GROUP = GRX_BIT(13), ///< `(?#...)`.
  GRX_FEATURE_POSIX_CLASS = GRX_BIT(14),   ///< `[[:alpha:]]`.
  GRX_FEATURE_UNICODE_PROPERTY = GRX_BIT(15), ///< `\p{L}`.
  /**
   * @brief Set operations between classes, however the dialect spells them.
   *
   * `[a-z&&[^aeiou]]` in Java and in ECMAScript's `v` mode, `(?[ ... ])` in
   * PCRE2 and Perl. One bit rather than two: what it says is that the
   * dialect can intersect and subtract sets at all, and the spelling is the
   * front end's business.
   */
  GRX_FEATURE_CLASS_SET_OPS = GRX_BIT(16),
  GRX_FEATURE_WORD_BOUNDARY = GRX_BIT(17), ///< `\b`, `\B`.
  GRX_FEATURE_ANCHOR_ESCAPES = GRX_BIT(18), ///< `\A`, `\z`, `\Z`.
  GRX_FEATURE_QUOTING = GRX_BIT(19),       ///< `\Q...\E`.
  GRX_FEATURE_HEX_ESCAPE = GRX_BIT(20),    ///< `\xFF`, `\x{10FFFF}`.
  GRX_FEATURE_OCTAL_ESCAPE = GRX_BIT(21),  ///< `\077`, `\o{77}`.
  GRX_FEATURE_CONTROL_ESCAPE = GRX_BIT(22), ///< `\cA`.
  GRX_FEATURE_SUBROUTINE = GRX_BIT(23),    ///< `(?&name)`.
  GRX_FEATURE_BACKTRACK_CONTROL = GRX_BIT(24) ///< `(*SKIP)`, `(*FAIL)`.
} GRX_Feature;

/**
 * @brief One dialect, as data.
 *
 * `escaped_specials` is what separates a BRE from an ERE: in POSIX BRE the
 * grouping and alternation operators are spelled `\(`, `\)` and (in GNU's
 * extension) `\|`, and the bare characters are literals. Rather than a second
 * parser, the same parser reads this flag and swaps the two meanings.
 */
typedef struct GRX_SyntaxSpec {
  uint64_t features;        ///< Bitwise OR of @ref GRX_Feature.
  uint32_t default_options; ///< @ref GRX_Option bits implied by the dialect.
  int escaped_specials;     ///< Non-zero when `\(` groups and `(` is literal.
  int allow_empty_class;    ///< Non-zero when `[]` is an empty class, not `]`.
  int newline_is_line_break; ///< Non-zero when `$` stops at `\n` by default.
  /**
   * Non-zero when a second quantifier on one atom is accepted.
   *
   * `a**` is a syntax error in almost every dialect and a legal (if odd)
   * pattern in GNU's and Ruby's. It is a flag rather than a hook because the
   * parser is what notices the second quantifier; a hook would have to be
   * told that one had already been applied.
   */
  int allow_double_quantifier;
  /**
   * Non-zero when `{3,1}` is a quantifier that can never be satisfied.
   *
   * Perl compiles `((def){37,17})?ABC` and matches "ABC": the inner repeat
   * never matches, so the optional group takes its empty branch. PCRE2
   * refuses the same pattern, and so does every other dialect here, which is
   * why this is a flag rather than the shared parser's default.
   */
  int allow_impossible_repeat;
  /**
   * Non-zero when a lazy or possessive suffix must touch its quantifier.
   *
   * `a*?` is a lazy repeat in every dialect that has one, but the dialects
   * disagree about what may stand between the `*` and the `?`. Perl and
   * PCRE2 let the suffix be written apart from the rest - extended mode
   * makes `a + +` a possessive repeat, and a comment is invisible, so
   * `a*(?#c)?` is lazy. CPython requires adjacency: `a*?` is lazy, `a*+` is
   * possessive, and `a*(?#c)?` and `(?x)a* ?` are both "multiple repeat".
   *
   * A flag rather than a hook because the shared parser is what skips the
   * ignorable run, and the question is only whether to skip it here.
   */
  int quantifier_suffix_is_adjacent;
  /**
   * Non-zero when a `)` with no `(` is an ordinary character.
   *
   * POSIX's own test set calls this one out - "gag me with a right
   * parenthesis -- 1003.2 goofed here" - and glibc matches "a)" with `a)`
   * in both grammars. Everywhere else an unmatched `)` is a syntax error,
   * which is why this is a flag and not the shared parser's default.
   */
  int unmatched_close_is_literal;
} GRX_SyntaxSpec;

/**
 * @brief Look up the specification for a dialect.
 *
 * @param syntax The dialect.
 * @param out_spec Receives the specification on success.
 * @return GRX_OK, or GRX_ERR_INVALID for an unknown dialect or a NULL output.
 */
GRX_API GRX_Result grx_syntax_spec(
    GRX_Syntax syntax, GRX_SyntaxSpec * out_spec);

/**
 * @brief The short name of a dialect, as a caller would write it.
 *
 * The returned string is statically allocated and must not be freed.
 *
 * @param syntax The dialect.
 * @return A name such as "pcre" or "posix-ere", or "unknown".
 */
GRX_API const char * grx_syntax_name(GRX_Syntax syntax);

/**
 * @brief Find a dialect by the name grx_syntax_name() gives it.
 *
 * @param name The name to look up. Case-insensitive; NULL is invalid.
 * @param out_syntax Receives the dialect on success.
 * @return GRX_OK, or GRX_ERR_INVALID for an unknown name or a NULL output.
 */
GRX_API GRX_Result grx_syntax_from_name(
    const char * name, GRX_Syntax * out_syntax);

/**
 * @brief Read a flag string in the dialect's own alphabet.
 *
 * Every consumer has one of these strings - `/gimsu` from a JavaScript
 * literal, `re.I | re.M` spelled as `im`, PCRE2's `(?i)` leading directive -
 * and none of them should have to know that `s` is dot-all in PCRE2 and
 * multiline in Ruby (documentation/dialects.md section 5.15).
 *
 * Three kinds of letter are refused rather than ignored, because a flag
 * string is a statement about how the pattern is to be read and silently
 * dropping part of it changes what matches:
 *
 * - A letter the dialect's alphabet does not contain is
 *   GRX_DIAG_UNKNOWN_FLAG.
 * - The same letter twice is GRX_DIAG_DUPLICATE_FLAG.
 * - A *search-mode* letter - ECMAScript's `g` and `y` - is
 *   GRX_DIAG_SEARCH_FLAG_IN_PATTERN. They are not compile-time options at
 *   all: `g` is grx_regex_search_next() and `y` is grx_regex_match(), and a
 *   caller who passed them here would get a regex that ignored them.
 * - Two letters that exclude each other, such as ECMAScript's `u` and `v`,
 *   are GRX_DIAG_CONFLICTING_FLAGS.
 *
 * @param syntax The dialect whose alphabet to read.
 * @param flags The letters. NULL is invalid; empty is valid and sets nothing.
 * @param out_options Receives the @ref GRX_Option bits. Required.
 * @param out_error Receives the offending letter's offset and a message.
 *   Optional.
 * @return GRX_OK, GRX_ERR_SYNTAX, GRX_ERR_UNSUPPORTED, or GRX_ERR_INVALID.
 */
GRX_API GRX_Result grx_options_parse(GRX_Syntax syntax, const char * flags,
    uint32_t * out_options, GRX_Error * out_error);

/**
 * @brief Whether a dialect has a feature.
 *
 * @param syntax The dialect.
 * @param feature The feature to test. Exactly one bit.
 * @return Non-zero when the dialect has it.
 */
GRX_API int grx_syntax_has_feature(GRX_Syntax syntax, GRX_Feature feature);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SYNTAX_H
