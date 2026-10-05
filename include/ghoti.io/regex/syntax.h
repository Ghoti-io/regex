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
  /**
   * I-Regexp, RFC 9485: the interoperable subset JSONPath requires.
   *
   * Last rather than beside the dialect it is a subset of, because these
   * constants are ABI and a caller compiled against an earlier header holds
   * the old numbers. It is not a family member either: I-Regexp is defined
   * as XSD's syntax less three things rather than as anyone's extension of
   * anyone else, and the front end is a *checking* one - a pattern outside
   * the ABNF in Figure 1 is GRX_ERR_SYNTAX with an offset, not something
   * read as ECMAScript.
   */
  GRX_SYNTAX_IREGEXP,
  /**
   * RE/flex, the regex library ugrep and the RE/flex lexer generator use.
   *
   * Appended for the same ABI reason I-Regexp is: a caller compiled against
   * an earlier header holds the old numbers, so a constant is added at the
   * end and never inserted beside the family it belongs to.
   *
   * It earns a constant rather than being read as POSIX ERE because its own
   * matcher is a DFA that took the *Perl* side of two arguments POSIX does
   * not have: `a*?` is lazy there, with no backtracking and no `-P`, and
   * `(?#...)` is a comment. It took neither of the two that cost linear
   * time - no backreference, no lookaround - which is the same shape as
   * RE2's row and, like RE2's, is the reason the dialect exists.
   */
  GRX_SYNTAX_REFLEX,
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
   *
   * It is the whole family, and PCRE2 can ask for less: setting this is
   * exactly setting GRX_OPT_ASCII_DIGIT, GRX_OPT_ASCII_SPACE,
   * GRX_OPT_ASCII_WORD, GRX_OPT_ASCII_POSIX and GRX_OPT_ASCII_POSIX_DIGIT
   * together. It stays a bit of its own rather than becoming a mask of
   * those, because a caller may already hold it and because Perl's `/a` is
   * one letter with one meaning rather than five that happen to coincide.
   *
   * What it does *not* narrow is `\h` and `\v`: neither reference narrows
   * those for anything, and they are written-out constants rather than a
   * family a mode selects. See documentation/dialects.md section 5.9.
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
  GRX_OPT_NEWLINE_TERMINATES = GRX_BIT(15),
  /**
   * @brief Composing characters are not matched, only carried.
   *
   * Vim's `\Z`, and meaningful only in a dialect whose matching model makes
   * a base character and the composing characters after it one character -
   * which today is Vim alone. With it, every atom that matches a character
   * takes the composing characters after it too, a composing character
   * written in the pattern beside a base is ignored, and a match may end
   * before a composing character; without it a literal matches its own code
   * point and no more, and a composing character in the pattern is part of
   * the atom it follows.
   *
   * A pattern option rather than a caller's choice, in the dialect that has
   * it: `\Z` anywhere in a Vim pattern decides the whole of it, exactly as
   * `\c` does, and the front end sets this bit before the parse begins. It
   * is public because a caller compiling a Vim pattern by hand may want the
   * same rule without writing the marker into the text.
   */
  GRX_OPT_IGNORE_COMBINING = GRX_BIT(16),
  /**
   * @brief `\d` and `\D` are ASCII.
   *
   * PCRE2's `(?aD)`, and one fifth of GRX_OPT_ASCII_CLASSES. PCRE2 narrows
   * each of these things on its own where Perl's `/a` narrows the family,
   * so the family bit above is defined as all five of these together and
   * each of them is spellable alone.
   */
  GRX_OPT_ASCII_DIGIT = GRX_BIT(17),
  /** @brief `\s` and `\S` are ASCII. PCRE2's `(?aS)`. */
  GRX_OPT_ASCII_SPACE = GRX_BIT(18),
  /**
   * @brief `\w`, `\W`, `\b` and `\B` are ASCII.
   *
   * PCRE2's `(?aW)`. `\b` moves with it because `\b` is defined from `\w`:
   * pcre2test matches `(?aW)\bx` against "é x" at the "x", where plain
   * `\bx` finds no boundary there.
   */
  GRX_OPT_ASCII_WORD = GRX_BIT(19),
  /**
   * @brief Every POSIX class is ASCII.
   *
   * PCRE2's `(?aP)`. Implies GRX_OPT_ASCII_POSIX_DIGIT, which is why
   * `(?aP)(?-aT)[[:digit:]]` still refuses U+0661 in pcre2test: clearing
   * the narrower bit leaves the wider one standing.
   */
  GRX_OPT_ASCII_POSIX = GRX_BIT(20),
  /**
   * @brief `[[:digit:]]` and `[[:xdigit:]]` are ASCII.
   *
   * PCRE2's `(?aT)`, a strict subset of GRX_OPT_ASCII_POSIX: it leaves
   * `[[:alpha:]]` and `[[:word:]]` Unicode, and it touches neither `\d`
   * nor any other shorthand. The two names travel together in pcre2test -
   * `(?aT)[[:xdigit:]]` refuses U+FF10 and takes "f".
   */
  GRX_OPT_ASCII_POSIX_DIGIT = GRX_BIT(21),
  /**
   * @brief Every match must end where the subject ends.
   *
   * PCRE2's `PCRE2_ENDANCHORED`, and the other half of GRX_OPT_ANCHORED:
   * with both set, a pattern matches the whole subject or nothing, which is
   * the Boolean question XSD and RFC 9485 ask and what JSONPath's `match()`
   * needs. Without them, `a` matches inside "ab" - which is JSONPath's
   * `search()`, and the reason this is an option rather than the dialect's
   * doing. A dialect that anchored its own patterns could answer only one of
   * the two questions.
   *
   * Both are honoured by lowering, as the assertions `\A` and `\z` around
   * the whole pattern, so they mean the subject and not the line however the
   * dialect defines a line break: GRX_SEARCH_NOTEOL does not reach them, and
   * a final newline does not satisfy them. A dialect whose syntax cannot
   * write those assertions - I-Regexp has no anchors at all - still gets
   * them from here, because the option belongs to the caller rather than to
   * the pattern text.
   */
  GRX_OPT_ANCHORED_END = GRX_BIT(22)
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
  GRX_FEATURE_INLINE_FLAGS = GRX_BIT(12),  ///< `(?i)`, unscoped.
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
  GRX_FEATURE_BACKTRACK_CONTROL = GRX_BIT(24), ///< `(*SKIP)`, `(*FAIL)`.
  /**
   * @brief Case transforms over the pattern text: `\U`, `\L`, `\F`, `\u`,
   *        `\l`, ended by `\E`.
   *
   * A sibling of GRX_FEATURE_QUOTING rather than a separate idea: both are
   * operators of the *source* the pattern was written in rather than of the
   * pattern, and Perl applies both in the same pass before its engine sees a
   * character. `qr/[\lAB]c/` is `(?^:[aB]c)`; the same six characters
   * arriving through a variable are `(?^:[\lAB]c)`, because a variable's
   * contents are not rescanned.
   *
   * Separate bits because the two do not travel together. PCRE2 has `\Q`
   * and refuses `\U` with an error of its own - error 137 names
   * `\F \L \l \N{name} \U \u` - and so does Python, so only Perl's row
   * carries this one.
   */
  GRX_FEATURE_CASE_TRANSFORM = GRX_BIT(25),
  /**
   * @brief A flag setting scoped to a subexpression: `(?i:a)`, `(?-i:a)`.
   *
   * Separate from GRX_FEATURE_INLINE_FLAGS because one dialect has exactly
   * one of the two. ES2025's RegExp Modifiers are always scoped - a bare
   * `(?i)` is "Invalid group" in V8 - where every Perl-family dialect here
   * accepts both spellings, so a single bit would answer "can I write
   * `(?i)`?" wrongly for ECMAScript whichever way it was set. The feature
   * table in documentation/dialects.md has carried the distinction as one
   * row reading "scoped only" since it was written; this is that row's
   * second bit.
   *
   * The alphabets differ too and are not this bit's business:
   * `grx_options_parse()` answers which letters a dialect takes.
   */
  GRX_FEATURE_SCOPED_FLAGS = GRX_BIT(26)
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
  uint64_t features;        ///< Bitwise OR of `GRX_Feature`.
  uint32_t default_options; ///< `GRX_Option` bits implied by the dialect.
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
   * Non-zero when something ignorable between two quantifiers separates
   * them, so that the second is not a second quantifier at all.
   *
   * RE2, and it is the mirror of quantifier_suffix_is_adjacent: that field
   * says a lazy suffix must touch its quantifier, this one says a *repeat*
   * must. Go's `regexp` refuses `a**` and accepts `a\Q\E*` - the empty
   * quoted run makes the two non-adjacent and the check does not fire -
   * and `\Q\E*` on its own is still "missing argument to repetition
   * operator", so the run is not an atom either. Both probed.
   *
   * It costs nothing in the dialects that have no ignorable run between a
   * quantifier and the next character, which is every other one here: the
   * Perl family has comments and extended mode, but both of those are
   * *inside* the suffix rule above rather than beside it.
   */
  int ignorable_separates_quantifiers;
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
  /**
   * Non-zero when the dialect has a second joining operator between `|` and
   * concatenation.
   *
   * Vim's `\&`, and nothing else here. `foo\&..` matches where *both*
   * concatenations match at the same position and reports the last one's
   * text, so it is an "and" to `\|`'s "or" and sits one level tighter:
   * `a\|b\&c` is `a` or (`b` and `c`). It is a grammar level rather than
   * a construct, which is why it is a flag here and not a GRX_Feature bit -
   * a feature bit says a spelling exists, and this says the grammar has a
   * rule the other dialects' grammars do not.
   *
   * What it means is not a second axis: `A\&B\&C` is exactly
   * `(?=A)(?=B)C`, which is how the parser builds it, so nothing below the
   * AST learns a new node kind.
   */
  int branch_and_operator;
  /**
   * Non-zero when a conditional may name a group the pattern has not got.
   *
   * Perl reads `(?(99)a|b)` as a condition that is simply false - the
   * pattern matches "b" - where PCRE2 and CPython both refuse it as a
   * reference to a subpattern that does not exist. The same split applies
   * to the recursion form: `(?(R99)a|b)` matches "b" in perl and is an
   * error in pcre2test. `(?(0)...)` is an error in every one of them, and
   * a *name* no group has is an error in every one of them too, so this is
   * about the numeric forms alone.
   *
   * A flag rather than a rule in the reader, because the reader is shared
   * and the question is one line of it.
   */
  int condition_group_may_be_absent;
  /**
   * Non-zero when a range endpoint that is a class makes the `-` a literal.
   *
   * `[a-\d]` is three members in Perl - "a", a literal "-" and a digit -
   * and an error in PCRE2 and in Python. Perl warns "False [] range" and
   * compiles; pcre2test answers error 150, "invalid range in character
   * class". Every spelling of a multi-character endpoint does the same
   * thing there: `\d`, `\s`, `\w` and their negations, `\p{L}`, and
   * `[:alpha:]`, at either end or at both. It is ECMAScript's Annex B rule
   * as well, where the `u` flag is what turns it back into an error, so
   * the reader already had the path; this flag is what lets the shared
   * Perl-family reader take it.
   *
   * It does not extend to a *reversed* range - `[z-a]` is an error in all
   * of them - or to an endpoint that is a quote boundary: perl answers
   * `[a-\E]` with "Invalid [] range", which is an error and not this
   * warning.
   */
  int false_range_is_union;
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
