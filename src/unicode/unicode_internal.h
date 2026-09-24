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
 * The UTF-8 codec, the case-fold operations, and property lookup.
 *
 * Everything here is compile-time except the codec: a caseless match is an
 * ordinary class match by the time an engine sees it, because lowering
 * expanded the literal into its fold orbit (documentation/design.md
 * section 3.2). Nothing below the IR calls a function on this page but the
 * decoder.
 */

#ifndef GHOTI_IO_GRX_SRC_UNICODE_UNICODE_INTERNAL_H
#define GHOTI_IO_GRX_SRC_UNICODE_UNICODE_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stddef.h>
#include <stdint.h>

#include "../core/range_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The largest number of members any simple fold orbit has. */
#define GRX_FOLD_ORBIT_MAX 4

/**
 * @brief The most code points a full case fold produces.
 *
 * CaseFolding.txt status F: `ß` is two, `ΐ` is three, and nothing in
 * Unicode 17 is longer.
 */
#define GRX_FULL_FOLD_MAX 3

/**
 * @brief The most code points that can share one full fold.
 *
 * The sources of a single-code-point target are its simple orbit, so this
 * is at least @ref GRX_FOLD_ORBIT_MAX; the longest multi-code-point target
 * has two sources (`ß` and `ẞ` both fold to "ss"). Four is the larger of
 * the two and covers both questions.
 */
#define GRX_FULL_FOLD_SOURCE_MAX 4

/**
 * @brief The most multi-code-point folds one character class can carry.
 *
 * Perl full-folds a class member written as a single code point, so `[ß]`
 * matches "ss" and a class lowers to an alternation with one branch per
 * distinct such fold (src/ir/lower.c, lower_class_full_folds()). The bound
 * is a property of Unicode rather than of the pattern: a class cannot name
 * more distinct multi-code-point folds than exist, which in UCD 17.0.0 is
 * **73** over 104 CaseFolding.txt `F` lines.
 *
 * 128 rather than 73 so that a UCD revision cannot reach it between one
 * table regeneration and the next. test_unicode.cpp asserts
 * `grx_unicode_full_fold_count <= GRX_CLASS_FULL_FOLD_MAX`, which is the
 * sufficient form of the same bound - every distinct fold needs at least one
 * `F` line - and lowering fails with GRX_DIAG_INTERNAL rather than
 * truncating if it is ever wrong, because a silently dropped branch is a
 * wrong answer and not a slow one.
 */
#define GRX_CLASS_FULL_FOLD_MAX 128

/**
 * @brief Decode one UTF-8 sequence.
 *
 * Strict: an overlong encoding, a surrogate (U+D800..U+DFFF), a code point
 * above U+10FFFF and a truncated or malformed sequence are all rejected
 * rather than decoded to U+FFFD. A regex engine that substituted a
 * replacement character here would match a pattern against bytes the subject
 * does not contain.
 *
 * @param text The bytes to decode. NULL is invalid.
 * @param length Bytes available at `text`.
 * @param out_codepoint Receives the code point on success. Required.
 * @return The number of bytes consumed, or 0 when the sequence is not valid
 *   UTF-8.
 */
size_t grx_unicode_utf8_decode(
    const char * text, size_t length, uint32_t * out_codepoint);

/**
 * @brief Decode the code point that *ends* at an offset.
 *
 * What a reverse-direction instruction steps with: a lookbehind body runs
 * right to left, and "one character back" in UTF-8 means stepping over at
 * most three continuation bytes and then validating forward, because a
 * backwards scan alone cannot tell a well-formed sequence from a truncated
 * one.
 *
 * @param text The buffer. NULL is invalid.
 * @param offset Byte offset one past the end of the sequence wanted.
 * @param out_codepoint Receives the code point on success. Required.
 * @return The number of bytes consumed, so that the sequence begins at
 *   `offset - result`, or 0 when there is no valid sequence ending there.
 */
size_t grx_unicode_utf8_decode_prev(
    const char * text, size_t offset, uint32_t * out_codepoint);

/**
 * @brief Encode one code point as UTF-8.
 *
 * @param codepoint The code point. Surrogates and values above U+10FFFF are
 *   invalid.
 * @param buffer Destination, at least 4 bytes. Not NUL-terminated.
 * @return The number of bytes written (1 to 4), or 0 for an invalid code
 *   point.
 */
size_t grx_unicode_utf8_encode(uint32_t codepoint, char * buffer);

/**
 * @brief Simple case folding of one code point.
 *
 * "Simple" in the Unicode sense: CaseFolding.txt statuses C and S, one code
 * point in and one out, so the mappings that expand (U+00DF to "ss") are not
 * applied. That is the folding a regex engine can do without changing the
 * length of what it matched; see documentation/design.md section 10 for why
 * full folding is deliberately absent.
 *
 * @param codepoint The code point to fold.
 * @return The folded code point, or the input where no mapping applies.
 */
uint32_t grx_unicode_fold_simple(uint32_t codepoint);

/**
 * @brief How many terminal cells a character occupies. Vim's, not Unicode's.
 *
 * Measured with vim 9.1's `strdisplaywidth()` over every code point; see
 * src/unicode/display.c for what the four answers are and why the set is
 * written out rather than named after a property.
 *
 * @param codepoint The code point.
 * @param first Non-zero when nothing precedes it, which is what makes a
 *   combining character one cell rather than none.
 * @return The number of cells, one for everything not in the table.
 */
uint32_t grx_display_cell_width(uint32_t codepoint, int first);

/**
 * @brief The screen column a character leaves the cursor in.
 *
 * Columns count from one. Everything but the tab is
 * grx_display_cell_width() added on; the tab reaches the next multiple of
 * `tabstop`, which is why this exists at all.
 *
 * @param codepoint The code point.
 * @param column The column it starts in, counting from one.
 * @param tabstop Columns per tab stop; zero makes a tab an ordinary
 *   character.
 * @param first Non-zero when nothing precedes it.
 * @return The column the next character starts in.
 */
size_t grx_display_column_after(
    uint32_t codepoint, size_t column, size_t tabstop, int first);

/** @brief Vim's class for a blank, and for nothing at all. */
#define GRX_VIM_CLASS_BLANK 0u

/**
 * @brief Vim's class for a keyword character, and the floor of a word.
 *
 * Every class at least this is a word character to `\<` and `\>`; the two
 * below it, blank and punctuation, are not. The classes above it are the
 * scripts vim separates - see src/unicode/vim_class.c for the nine.
 */
#define GRX_VIM_CLASS_KEYWORD 2u

/**
 * @brief Which character class a code point is in. Vim's, not Unicode's.
 *
 * Measured with vim 9.1's `charclass()` over every code point; see
 * src/unicode/vim_class.c for the nine classes and why they are not a
 * property.
 *
 * @param codepoint The code point.
 * @return Vim's class number, GRX_VIM_CLASS_KEYWORD for everything not in
 *   the table.
 */
uint32_t grx_vim_char_class(uint32_t codepoint);

/**
 * @brief Vim's `\<`: a word character whose class differs from the one
 *   before it.
 *
 * `\<` and `\>` are not the word set's two halves in vim, because vim has
 * more than one word class: `\>` holds between U+65E5 and "x" there, where
 * both are keyword characters. GRX_ASSERT_WORD_START is the rule this
 * library's other dialects take and GRX_ASSERT_WORD_CLASS_START is this one.
 *
 * @param has_before Non-zero when a character precedes the position.
 * @param before That character.
 * @param has_after Non-zero when a character follows the position.
 * @param after That character.
 * @return Non-zero where a word begins.
 */
int grx_vim_word_start(
    int has_before, uint32_t before, int has_after, uint32_t after);

/**
 * @brief Vim's `\>`: the position after a word character whose class the
 *   next character does not share.
 *
 * @param has_before Non-zero when a character precedes the position.
 * @param before That character.
 * @param has_after Non-zero when a character follows the position.
 * @param after That character.
 * @return Non-zero where a word ends.
 */
int grx_vim_word_end(
    int has_before, uint32_t before, int has_after, uint32_t after);

/**
 * @brief Simple_Uppercase_Mapping, or the code point where there is none.
 *
 * One code point to one code point: U+01F3 gives U+01F1 and not the
 * titlecase U+01F2, and U+00DF and U+FB01 give themselves, where their
 * *full* uppercase mappings are two characters. That is the mapping a
 * replacement template's "upper case the next character" wants, and it is
 * neither a folding nor a canonicalisation.
 *
 * @param codepoint The code point.
 * @return Its simple uppercase, or the input where it has none.
 */
uint32_t grx_unicode_upper_simple(uint32_t codepoint);

/**
 * @brief Simple_Lowercase_Mapping, or the code point where there is none.
 *
 * @param codepoint The code point.
 * @return Its simple lowercase, or the input where it has none.
 */
uint32_t grx_unicode_lower_simple(uint32_t codepoint);

/**
 * @brief Every code point whose simple fold equals this one's.
 *
 * What a caseless literal becomes, and what a caseless class is closed
 * under. The orbit always contains the input, so the result is at least 1
 * and a caller need not special-case "no orbit".
 *
 * @param codepoint The code point.
 * @param out Receives the members, in ascending order. Required, with room
 *   for @ref GRX_FOLD_ORBIT_MAX.
 * @return The number of members written, at least 1.
 */
size_t grx_unicode_fold_orbit(
    uint32_t codepoint, uint32_t out[GRX_FOLD_ORBIT_MAX]);

/**
 * @brief Full case folding of one code point.
 *
 * CaseFolding.txt status F where there is one and the simple fold
 * otherwise, so the answer is one to three code points and is always the
 * *full* fold: `ß` gives "ss", `ẞ` gives "ss" as well, and `a` gives "a".
 *
 * A fold that is longer than what it folded is what makes a caseless match
 * change length, which is why this is not something a single instruction
 * can apply (documentation/design.md section 3.4.4).
 *
 * @param codepoint The code point to fold.
 * @param out Receives the fold. Required, with room for
 *   @ref GRX_FULL_FOLD_MAX.
 * @return The number of code points written, at least 1.
 */
size_t grx_unicode_fold_full(
    uint32_t codepoint, uint32_t out[GRX_FULL_FOLD_MAX]);

/**
 * @brief Every code point whose full fold is exactly this sequence.
 *
 * The reverse of grx_unicode_fold_full(), and the question lowering asks to
 * turn a folded string into the classes that can produce it: what matches
 * "ss" in one character is `ß` and `ẞ`, and what matches "s" in one
 * character is the simple orbit of `s` less those two.
 *
 * @param sequence The folded code points. NULL or empty is 0.
 * @param length How many, at most @ref GRX_FULL_FOLD_MAX.
 * @param out Receives the sources. Required, with room for
 *   @ref GRX_FULL_FOLD_SOURCE_MAX.
 * @return The number written; 0 when nothing folds to this sequence.
 */
size_t grx_unicode_fold_full_sources(const uint32_t * sequence, size_t length,
    uint32_t out[GRX_FULL_FOLD_SOURCE_MAX]);

/**
 * @brief The code point a `\N{NAME}` spelling names.
 *
 * Perl's rule, and exact: the match is case sensitive and the separators
 * must be the ones the UCD uses, because `\N{latin small letter a}` and
 * `\N{LATIN-SMALL-LETTER-A}` are both errors in perl 5.40.1. Leading and
 * trailing space inside the braces is the caller's to trim.
 *
 * Resolves the stored names, every alias in `NameAliases.txt` (all five
 * kinds - Perl accepts corrections and figments as readily as
 * abbreviations), and the computed families, which are not stored: the CJK
 * ideographs, the Tangut ideographs and the Hangul syllables.
 *
 * @param name The text between the braces.
 * @param length Its length in bytes.
 * @param out_codepoint Receives the code point. Required.
 * @return 1 when the name resolves, 0 when it does not.
 */
int grx_unicode_codepoint_from_name(
    const char * name, size_t length, uint32_t * out_codepoint);

/**
 * @brief Which folding a dialect uses for a caseless match.
 *
 * documentation/dialects.md section 5.8. One of the five values there is
 * absent because it is implemented as SIMPLE and recorded as a deviation:
 * .NET's culture-sensitive folding, which would need a locale this library
 * does not have.
 */
typedef enum {
  GRX_FOLD_NONE = 0,   ///< Not caseless; nothing folds.
  GRX_FOLD_SIMPLE,     ///< Unicode simple case folding.
  GRX_FOLD_ES_LEGACY,  ///< ECMA-262 Canonicalize without `u`.
  GRX_FOLD_ASCII,      ///< A-Z and a-z only. POSIX, and PCRE2 without UTF.
  /**
   * Simple folding with the ASCII range held apart from the rest.
   *
   * Perl's `/aa`. Under `/ai` the letter `s` matches U+017F because the two
   * share a fold orbit; under `/aai` it does not, and `\x{C0}` still matches
   * `\x{E0}` because both of those are outside ASCII. So this is not
   * GRX_FOLD_ASCII, which would stop the second pair as well - it is simple
   * folding with every orbit cut at U+0080.
   */
  GRX_FOLD_SIMPLE_ASCII_APART,
  /**
   * Unicode full case folding: Perl's `/i`.
   *
   * The only value here under which a caseless match can change length -
   * `ß` matches "ss" and the `ﬃ` ligature matches "ffi" - so it is the only
   * one a *class* cannot express, and grx_unicode_orbit() answers for it as
   * though it were SIMPLE. A class matches one character, and UTS #18 says
   * so: full folding applies to the text a pattern spells out, not to the
   * sets it names. Lowering is where the difference lives; see
   * GRX_IR_FOLD_RUN.
   */
  GRX_FOLD_FULL,
  /**
   * Full folding with the ASCII range held apart: Perl's `/aai`.
   *
   * GRX_FOLD_SIMPLE_ASCII_APART's rule for the orbits, and Perl's rule for
   * the full folds on top of it: a full fold survives `/aa` exactly when
   * **no code point of the fold is ASCII**.
   *
   * This used to be spelled as SIMPLE_ASCII_APART, on the stated ground that
   * every full fold has an ASCII character somewhere in it and so none could
   * survive. That is false for **87 of the 104** `F` lines of
   * CaseFolding.txt: `U+0390` folds to `U+03B9 U+0308 U+0301` and `U+1FB3`
   * to `U+03B1 U+03B9`, neither of which touches ASCII, and Perl keeps both
   * under `/aa`. The 17 it does drop - `ß` to "ss", the `ﬀ` ligature to
   * "ff", `U+01F0` to `j` and a combining caron - are the ones with an ASCII
   * character in the fold.
   */
  GRX_FOLD_FULL_ASCII_APART,
  GRX_FOLD_COUNT       ///< Closes the enum; not a folding.
} GRX_FoldKind;

/**
 * @brief Every code point that matches this one under a folding.
 *
 * The dispatcher over the two orbit tables and the ASCII rule, so that a
 * caller holding a @ref GRX_FoldKind does not branch on it.
 *
 * @param kind Which folding.
 * @param codepoint The code point.
 * @param out Receives the members, ascending. Required, with room for
 *   @ref GRX_FOLD_ORBIT_MAX.
 * @return The number of members written, at least 1.
 */
size_t grx_unicode_orbit(GRX_FoldKind kind, uint32_t codepoint,
    uint32_t out[GRX_FOLD_ORBIT_MAX]);

/**
 * @brief How many orbits a folding's table holds.
 *
 * Closing a *class* under a folding walks the orbit table and asks which
 * orbits the class touches, rather than walking the class and asking for
 * each code point's orbit. The two give the same answer, and the first costs
 * the size of the table while the second costs the size of the class - which
 * for `[^\x00]` is every code point in Unicode.
 *
 * @param kind Which folding.
 * @return The number of entries, 0 for GRX_FOLD_NONE.
 */
size_t grx_unicode_orbit_table_size(GRX_FoldKind kind);

/**
 * @brief One entry of a folding's orbit table.
 *
 * Every member of an orbit has its own entry, so the same orbit is visited
 * once per member. Closing a class is idempotent, so that costs a little
 * work and no correctness.
 *
 * @param kind Which folding.
 * @param index Entry index, below grx_unicode_orbit_table_size().
 * @param out Receives the members, ascending. Required, with room for
 *   @ref GRX_FOLD_ORBIT_MAX.
 * @return The number of members written, or 0 for an index out of range.
 */
size_t grx_unicode_orbit_table_at(GRX_FoldKind kind, size_t index,
    uint32_t out[GRX_FOLD_ORBIT_MAX]);

/**
 * @brief ECMA-262 Canonicalize for a pattern without the `u` flag.
 *
 * Applies the full uppercase mapping unless the result is more than one
 * UTF-16 code unit, or maps a non-ASCII code point into ASCII. This is the
 * rule under which JavaScript's `/[a-z]/i` does not match U+017F while
 * `/[a-z]/iu` does, and ECMAScript is the only dialect that has it
 * (documentation/dialects.md section 5.8).
 *
 * @param codepoint The code point.
 * @return Its canonical form, or the input where the rule does not apply.
 */
uint32_t grx_unicode_es_legacy_canonicalize(uint32_t codepoint);

/**
 * @brief Every code point sharing this one's ECMA-262 legacy canonical form.
 *
 * As grx_unicode_fold_orbit(), for the other folding.
 *
 * @param codepoint The code point.
 * @param out Receives the members, in ascending order. Required, with room
 *   for @ref GRX_FOLD_ORBIT_MAX.
 * @return The number of members written, at least 1.
 */
size_t grx_unicode_es_legacy_orbit(
    uint32_t codepoint, uint32_t out[GRX_FOLD_ORBIT_MAX]);

/**
 * @brief How a dialect spells property names.
 *
 * Two resolvers over one set of tables (documentation/unicode.md section 6):
 * ECMAScript requires the exact canonical spelling or a listed alias, and
 * everything else in tier 1 that has properties at all matches loosely.
 */
typedef enum {
  GRX_PROPERTY_STRICT = 0, ///< Exact, case-sensitive. ECMAScript.
  GRX_PROPERTY_LOOSE,      ///< UAX #44 section 5.9.2. PCRE2.
  /**
   * The same, plus the one alias Perl has and PCRE2 does not.
   *
   * `\p{L_}` is Cased_Letter in Perl and plain Letter in PCRE2, which reads
   * it through the loose rule that deletes underscores. One character's
   * difference, and it is not expressible as a table row - see
   * cased_letter_alias() - so it is a third spelling rule instead.
   */
  GRX_PROPERTY_LOOSE_PERL
} GRX_PropertyMatch;

/**
 * @brief Resolve a property name, with or without a value, to a table index.
 *
 * Three spellings reach here. `\p{Lu}` and `\p{Alphabetic}` arrive with
 * `name` set and `value` NULL, and resolve against General_Category values
 * and binary property names respectively. `\p{Script=Greek}` arrives with
 * both, and the name selects which value table to search.
 *
 * A lone script value is deliberately *not* accepted: `\p{Greek}` is a
 * SyntaxError in ECMAScript, which is the only dialect using the strict
 * resolver today, and accepting it loosely for the others is the loose
 * resolver's job rather than this one's.
 *
 * @param name The property name, or the lone name. Need not be
 *   NUL-terminated.
 * @param name_length Its length in bytes.
 * @param value The value, or NULL for the lone form.
 * @param value_length Its length in bytes; 0 when `value` is NULL.
 * @param match Which spelling rule to apply.
 * @param out_property Receives the property index. Required.
 * @return GRX_OK, or GRX_ERR_SYNTAX when no property has that spelling.
 */
GRX_Result grx_unicode_property_lookup(const char * name, size_t name_length,
    const char * value, size_t value_length, GRX_PropertyMatch match,
    uint32_t * out_property);

/**
 * @brief The code points a resolved property covers.
 *
 * @param property The index grx_unicode_property_lookup() returned.
 * @param out_count Receives the range count. Required.
 * @return The first range, or NULL for an index out of range.
 */
const GRX_CharRange * grx_unicode_property_ranges(
    uint32_t property, size_t * out_count);

/**
 * @brief The canonical long name of a resolved property, for a dump.
 *
 * @param property The index grx_unicode_property_lookup() returned.
 * @return The name, or NULL for an index out of range.
 */
const char * grx_unicode_property_name(uint32_t property);

/**
 * @brief The number of code points a resolved property covers.
 *
 * Carried from the UCD's own arithmetic rather than counted here, so that a
 * test can check the table against the standard (documentation/unicode.md
 * section 4).
 *
 * @param property The index grx_unicode_property_lookup() returned.
 * @return The count, or 0 for an index out of range.
 */
size_t grx_unicode_property_total(uint32_t property);

/**
 * @brief Resolve a property *of strings* by name.
 *
 * ECMAScript's `v` mode defines seven (documentation/unicode.md section 3),
 * whose members may be several code points long. They live in a separate
 * namespace from the character properties above and are matched by exact
 * spelling only: their long and short names are the same, and only
 * ECMAScript has them.
 *
 * @param name The property name. Need not be NUL-terminated.
 * @param name_length Its length in bytes.
 * @param out_set Receives the set index. Required.
 * @return GRX_OK, or GRX_ERR_SYNTAX when no property of strings is spelled
 *   that way.
 */
GRX_Result grx_unicode_string_set_lookup(
    const char * name, size_t name_length, uint32_t * out_set);

/**
 * @brief How many sequences a resolved property of strings holds.
 *
 * @param set The index grx_unicode_string_set_lookup() returned.
 * @return The count, or 0 for an index out of range.
 */
size_t grx_unicode_string_set_size(uint32_t set);

/**
 * @brief One sequence of a resolved property of strings.
 *
 * @param set The index grx_unicode_string_set_lookup() returned.
 * @param index Which sequence, below grx_unicode_string_set_size().
 * @param out_points Receives a pointer to its code points. Required.
 * @return The number of code points, or 0 when either index is out of
 *   range - in which case `*out_points` is left alone.
 */
size_t grx_unicode_string_set_at(
    uint32_t set, size_t index, const uint32_t ** out_points);

/**
 * @brief Whether a sequence of code points is a script run.
 *
 * UTS #39's rule, as PCRE2 and Perl implement it. A script run is a
 * sequence that could be written in one script, which is *not* the same as
 * every character having the same Script property: punctuation is shared,
 * diacritics take the script of what they modify, and Han is written
 * alongside Hiragana, Katakana, Hangul and Bopomofo.
 *
 * The rule, each clause from pcre2unicode's "Script Runs" and each checked
 * against pcre2test 10.46 and perl 5.40.1:
 *
 * - Fewer than two code points is always a script run. That is the only
 *   case in which an unassigned code point can be part of one.
 * - Only Script_Extensions is consulted, never the plain Script property.
 * - A code point whose set is exactly Inherited is always accepted, and one
 *   whose set is exactly Common is accepted subject to the digit rule.
 * - Every remaining code point must share at least one script: the
 *   intersection of their sets must not be empty. The tables carry UTS #39's
 *   augmentation already applied, which is what makes the Han combinations
 *   fall out of the intersection rather than needing a rule of their own.
 * - If the run holds any decimal digits, they must all come from the same
 *   block of ten.
 *
 * @param points The code points. May be NULL only when `count` is 0.
 * @param count How many there are.
 * @return Non-zero when the sequence is a script run.
 */
int grx_unicode_script_run(const uint32_t * points, size_t count);

/**
 * @brief The running state of a script-run check, one code point at a time.
 *
 * Offered because the rule is **not** decomposable: a sequence can fail
 * while every adjacent pair passes - three characters whose sets are
 * {A,B}, {B,C} and {C,A} intersect pairwise and not at all together - so a
 * caller with a long span cannot check it in windows and must either hold
 * every code point or hold this. It is three words and a count.
 *
 * Treat the fields as opaque; grx_unicode_script_run() is this used twice.
 */
typedef struct GRX_ScriptRunState {
  uint64_t intersection[3]; ///< Scripts still possible, as a bitmap.
  uint32_t digits;          ///< The digit block seen, or GRX_NPOS for none.
  int unknown;              ///< An unassigned code point has been added.
  size_t count;             ///< Code points added so far.
} GRX_ScriptRunState;

/**
 * @brief Begin a script-run check.
 *
 * @param state The state to initialise. NULL is ignored.
 */
void grx_unicode_script_run_begin(GRX_ScriptRunState * state);

/**
 * @brief Extend a script-run check by one code point.
 *
 * @param state The state. NULL is 0.
 * @param code The next code point.
 * @return Non-zero while what has been added is still a script run. Once
 *   it returns 0 the answer cannot become 1 again, so a caller may stop.
 */
int grx_unicode_script_run_add(GRX_ScriptRunState * state, uint32_t code);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_UNICODE_UNICODE_INTERNAL_H
