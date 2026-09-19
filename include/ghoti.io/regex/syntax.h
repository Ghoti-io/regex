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
 *
 * Copyright 2026 by Corey Pennycuff
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
  GRX_OPT_UNICODE_SETS = GRX_BIT(10)
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
  GRX_FEATURE_CLASS_SET_OPS = GRX_BIT(16), ///< `[a-z&&[^aeiou]]`.
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
