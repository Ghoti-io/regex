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
 * The dialect table: what each supported regular-expression syntax has.
 *
 * Status: provisional. The entries below record the features each
 * implementation is widely documented to have, as a starting point for the
 * parser. documentation/dialects.md is the authority once it is written, and
 * every entry here is expected to be checked against the reference named
 * beside it and corrected.
 *
 * References, one per dialect:
 *   POSIX BRE/ERE   IEEE Std 1003.1-2017, chapter 9
 *   GNU BRE/ERE     GNU grep manual, "Regular Expressions"
 *   Perl            perlre (Perl 5.38)
 *   PCRE            pcre2pattern (PCRE2 10.44)
 *   ECMAScript      ECMA-262, clause 22.2
 *   Python          CPython `re` module documentation (3.13)
 *   Java            java.util.regex.Pattern (JDK 21)
 *   .NET            System.Text.RegularExpressions, "Regular Expression
 *                   Language - Quick Reference"
 *   Ruby            Onigmo RE.txt
 *   RE2             RE2 "Syntax" wiki page
 *   Rust            the `regex` crate's syntax documentation
 *   Tcl             Tcl re_syntax(n)
 *   Vim             Vim :help pattern
 *   Emacs           GNU Emacs Lisp Reference Manual, "Regular Expressions"
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/syntax.h>
#include <stddef.h>

#include "../core/core_internal.h"
#include "syntax_internal.h"

// Shorthand, so that a table row fits on a screen and the difference between
// two dialects can be read by eye.
#define ALT GRX_FEATURE_ALTERNATION
#define REP GRX_FEATURE_BOUNDED_REPEAT
#define LAZY GRX_FEATURE_NON_GREEDY
#define POSS GRX_FEATURE_POSSESSIVE
#define NCAP GRX_FEATURE_NON_CAPTURING
#define NAME GRX_FEATURE_NAMED_CAPTURE
#define BREF GRX_FEATURE_BACKREFERENCE
#define LAH GRX_FEATURE_LOOKAHEAD
#define LBH GRX_FEATURE_LOOKBEHIND
#define ATOM GRX_FEATURE_ATOMIC_GROUP
#define COND GRX_FEATURE_CONDITIONAL
#define RECU GRX_FEATURE_RECURSION
#define FLAG GRX_FEATURE_INLINE_FLAGS
#define SFLG GRX_FEATURE_SCOPED_FLAGS
#define CMNT GRX_FEATURE_COMMENT_GROUP
#define PCLS GRX_FEATURE_POSIX_CLASS
#define UPRP GRX_FEATURE_UNICODE_PROPERTY
#define CSET GRX_FEATURE_CLASS_SET_OPS
#define WORD GRX_FEATURE_WORD_BOUNDARY
#define ANCH GRX_FEATURE_ANCHOR_ESCAPES
#define QUOT GRX_FEATURE_QUOTING
#define HEX GRX_FEATURE_HEX_ESCAPE
#define OCT GRX_FEATURE_OCTAL_ESCAPE
#define CTRL GRX_FEATURE_CONTROL_ESCAPE
#define SUBR GRX_FEATURE_SUBROUTINE
#define VERB GRX_FEATURE_BACKTRACK_CONTROL
#define CASE GRX_FEATURE_CASE_TRANSFORM

// One row per GRX_Syntax constant. Designated initialisers, so the rows may
// be reordered without breaking and a row that is simply missing is zero
// rather than someone else's - which no compiler can catch, and which
// EveryDialectHasASpec in tests/unit/test_syntax.cpp exists to catch instead.
static const GRX_SyntaxSpec spec_table[GRX_SYNTAX_COUNT] = {
  // `.` matches a newline in all four: POSIX has no "dot-all" option because
  // dot-all is what it does, and `.` against "\n" matches in glibc with no
  // flags at all. REG_NEWLINE is what turns it off, together with three
  // other things - see documentation/dialects.md section 5.2.
  //
  // A second quantifier is accepted because glibc accepts one: `a**` and
  // `a{2}{3}` both match there, and so does `a*\?` in a BRE.
  [GRX_SYNTAX_POSIX_BRE] = {
    .features = REP | BREF | PCLS,
    .default_options = GRX_OPT_DOTALL,
    .escaped_specials = 1,
    .allow_double_quantifier = 1,
    .unmatched_close_is_literal = 1,
  },
  [GRX_SYNTAX_POSIX_ERE] = {
    // No backreference: POSIX leaves them out of the extended syntax, which
    // is the one difference people are most often surprised by.
    .features = ALT | REP | PCLS,
    .default_options = GRX_OPT_DOTALL,
    .allow_double_quantifier = 1,
    .unmatched_close_is_literal = 1,
  },
  [GRX_SYNTAX_GNU_BRE] = {
    .features = ALT | REP | BREF | PCLS | WORD | ANCH,
    .default_options = GRX_OPT_DOTALL,
    .escaped_specials = 1,
    .allow_double_quantifier = 1,
    .unmatched_close_is_literal = 1,
  },
  [GRX_SYNTAX_GNU_ERE] = {
    // Backreferences and `\w`/`\b`, which POSIX ERE has not: glibc's
    // regcomp answers `(a)\1` against "aa" with a match, so an ERE here is
    // the GNU one whenever it is reached through regcomp.
    .features = ALT | REP | BREF | PCLS | WORD | ANCH,
    .default_options = GRX_OPT_DOTALL,
    .allow_double_quantifier = 1,
    .unmatched_close_is_literal = 1,
  },
  [GRX_SYNTAX_PERL] = {
    // CSET is `(?[...])`, and it is here as well as on PCRE2's row because
    // the two grammars turned out to be one. This row said otherwise -
    // "Perl's nests and PCRE2's does not" - and perl refuses a textual
    // `(?[ (?[ [a] ]) ])`: what perl nests is an *interpolated* qr//,
    // which a pattern arriving as text cannot do. Compared over 13,440
    // generated rows, the operands, the operators and their precedence all
    // agree; what differs is only which characters are ignorable, and
    // skip_extended_ignorable() is where that lives.
    .features = ALT | REP | LAZY | POSS | NCAP | NAME | BREF | LAH | LBH
        | ATOM | COND | RECU | FLAG | SFLG | CMNT | PCLS | UPRP | CSET | WORD
        | ANCH | QUOT | HEX | OCT | CTRL | SUBR | CASE,
    // perl warns "Quantifier {n,m} with n > m can't match" and compiles it
    // anyway, as a group that never matches. pcre2test refuses the same
    // pattern outright.
    .allow_impossible_repeat = 1,
    // `(?(99)a|b)` matches "b" in perl - a condition naming a group the
    // pattern has not got is simply false - where pcre2test and CPython
    // both refuse it. Measured; `(?(0)...)` is an error everywhere and a
    // missing *name* is an error everywhere, so this is the numeric forms
    // alone, `(?(R99)...)` included.
    .condition_group_may_be_absent = 1,
    // documentation/dialects.md section 5.15: Perl's subject is a Unicode
    // string, so UTF is on unless the caller turns it off. The field had
    // been empty since the table was written, and what it cost was visible
    // only once there was a Perl front end to read it: `\N{U+0100}` is
    // refused outside UTF mode, and `\400` is one code point in UTF and a
    // byte that cannot be one without it.
    //
    // DUPLICATE_NAMES because perl has no switch for it: `(?<a>x)(?<a>y)`
    // compiles in 5.44.0 with no pragma and no warning, where pcre2test
    // refuses it as error 143 "(PCRE2_DUPNAMES not set)" and wants `(?J)`.
    // The option existed for `(?J)` and this row simply has it on - which is
    // the whole of the difference, so there is no second mechanism.
    //
    // ECMAScript is a third answer and not a second: ES2025 allows the name
    // twice where no single match could fill both, so its front end decides
    // per pattern rather than per dialect and leaves this option clear. A
    // caller that sets it anyway gets the check switched off, which is what
    // the option says it does.
    .default_options = GRX_OPT_UTF | GRX_OPT_DUPLICATE_NAMES,
    // `[a-\d]` is "a", a literal "-" and a digit in perl, which warns
    // "False [] range" and compiles; pcre2test refuses the same pattern
    // with error 150 and CPython raises "bad character range". Section
    // 5.12 of documentation/dialects.md has said so since the table was
    // written, and the reader refused it for every dialect until a soak
    // seed spelled `[a-\p{L}[[:alpha:]]`.
    .false_range_is_union = 1,
  },
  [GRX_SYNTAX_PCRE] = {
    // CSET is `(?[...])` here, not `&&` inside brackets: PCRE2 10.45 added
    // the extended class and did not add the Java spelling.
    .features = ALT | REP | LAZY | POSS | NCAP | NAME | BREF | LAH | LBH
        | ATOM | COND | RECU | FLAG | SFLG | CMNT | PCLS | UPRP | CSET | WORD | ANCH
        | QUOT | HEX | OCT | CTRL | SUBR | VERB,
  },
  [GRX_SYNTAX_ECMASCRIPT] = {
    // SFLG and not FLAG. ECMAScript puts the regexp's own flags after the
    // closing delimiter rather than inside the pattern, so a bare `(?i)` is
    // a syntax error - "Invalid group" in V8 - and ES2025's RegExp Modifiers
    // are the scoped spelling only: `(?i:a)`, `(?-s:a)`, `(?im-s:a)`, for
    // `i`, `m` and `s`. That is the deciding case the two bits exist for.
    .features = ALT | REP | LAZY | NCAP | NAME | BREF | LAH | LBH | UPRP
        | CSET | WORD | HEX | CTRL | SFLG,
    // The only tier-1 dialect where `[]` is an empty class rather than a
    // class containing `]`. Read by the prescan as well as by the front end:
    // `(?2)[]a()b](abc)` has one capturing group in PCRE2 and two in
    // ECMAScript, and a prescan that guessed would refuse a valid pattern.
    .allow_empty_class = 1,
  },
  // I-Regexp, RFC 9485 Figure 1. Three features out of twenty-seven, and the
  // absences are the specification: no lazy quantifier (`piece = atom
  // [quantifier]`, and a quantifier is not an atom, so `a*?` is two
  // quantifiers on one atom), no non-capturing group or any other `(?` form
  // (`atom` has one group production), no backreference, no lookaround, no
  // inline flags, no `\Q`, no hex, octal or control escape, no POSIX class,
  // and no class set operations - class subtraction being one of the three
  // things the RFC names as removed from XSD.
  //
  // What it *has* that ECMAScript's row also has is `\p{...}`, over 36
  // general-category names and nothing else; src/syntax/iregexp.c holds the
  // list, because a name outside it is a refusal rather than a lookup.
  //
  // `default_options` is UTF and only UTF: an I-Regexp is a sequence of
  // Unicode scalar values (section 1.1), and RFC 9485 requires full Unicode
  // support rather than offering it as a mode. Anchoring is deliberately not
  // here - see GRX_OPT_ANCHORED_END.
  [GRX_SYNTAX_IREGEXP] = {
    .features = ALT | REP | UPRP,
    .default_options = GRX_OPT_UTF,
    // `a{3,1}` compiles and can never match, which is Perl's answer rather
    // than PCRE2's - and here it is not a choice between references but what
    // the grammar says: `range-quantifier = "{" QuantExact [ "," [ QuantExact
    // ] ] "}"` puts no condition on the two numbers, so a pattern with them
    // the wrong way round conforms to Figure 1 and a checking implementation
    // has nothing to refuse. Both references agree: iregexp-check accepts it,
    // and libxml2 compiles it into a pattern that matches nothing.
    //
    // This library refused it until the oracle was built, which is the whole
    // argument for building one - the refusal was inherited from the shared
    // default and no test could see that the default was wrong here.
    .allow_impossible_repeat = 1,
  },
  [GRX_SYNTAX_PYTHON] = {
    .features = ALT | REP | LAZY | POSS | NCAP | NAME | BREF | LAH | LBH
        | ATOM | COND | FLAG | SFLG | CMNT | WORD | ANCH | HEX | OCT,
    // A `str` pattern is Unicode in every mode: the subject is a sequence
    // of code points, `\N{...}` is available, and `.` matches a character
    // rather than a byte. `(?a)` does not take this away - it narrows the
    // shorthands and the folding and nothing else, which is why it is
    // GRX_OPT_ASCII_CLASSES and not the absence of this.
    .default_options = GRX_OPT_UTF,
    // `a*(?#c)?` and `(?x)a* ?` are both "multiple repeat" in `re`, where
    // perl reads each as a lazy `a*`. See the field's own comment.
    .quantifier_suffix_is_adjacent = 1,
  },
  // **No SFLG below this line**, and that is a statement about what has
  // been probed rather than about the dialects. GRX_FEATURE_SCOPED_FLAGS
  // was split out of GRX_FEATURE_INLINE_FLAGS on 2026-09-26 because
  // ECMAScript has the scoped spelling and not the bare one, and the four
  // rows above were each asked for both spellings, one dialect at a time.
  // These have no front end and their profile rows are read off their
  // reference documents; dialects.md section 5 marks their cells `probe`
  // for the same reason, and WP-31..WP-37 each begin with the probing.
  // Setting a bit here from memory is how a table starts answering for a
  // dialect nobody asked.
  [GRX_SYNTAX_JAVA] = {
    .features = ALT | REP | LAZY | POSS | NCAP | NAME | BREF | LAH | LBH
        | ATOM | FLAG | PCLS | UPRP | CSET | WORD | ANCH | QUOT | HEX | OCT
        | CTRL,
  },
  [GRX_SYNTAX_DOTNET] = {
    .features = ALT | REP | LAZY | NCAP | NAME | BREF | LAH | LBH | ATOM
        | COND | FLAG | CMNT | UPRP | CSET | WORD | ANCH | HEX | OCT | CTRL,
  },
  [GRX_SYNTAX_RUBY] = {
    .features = ALT | REP | LAZY | POSS | NCAP | NAME | BREF | LAH | LBH
        | ATOM | COND | RECU | FLAG | CMNT | PCLS | UPRP | CSET | WORD | ANCH
        | HEX | OCT | CTRL | SUBR,
  },
  [GRX_SYNTAX_RE2] = {
    // The point of RE2: no backreference and no lookaround, because neither
    // can be run in linear time. A dialect table is how that becomes a parse
    // error instead of a silent fallback to the backtracking engine.
    .features = ALT | REP | LAZY | NCAP | NAME | FLAG | PCLS | UPRP | WORD
        | ANCH | QUOT | HEX | OCT,
  },
  [GRX_SYNTAX_RUST] = {
    .features = ALT | REP | LAZY | NCAP | NAME | FLAG | PCLS | UPRP | CSET
        | WORD | ANCH | QUOT | HEX | OCT,
  },
  [GRX_SYNTAX_TCL] = {
    .features = ALT | REP | LAZY | NCAP | BREF | LAH | LBH | FLAG | PCLS
        | WORD | ANCH | HEX | OCT,
  },
  [GRX_SYNTAX_VIM] = {
    // No FLAG: vim has no `(?i)`. Its `\c` is not an inline flag either -
    // it decides caseless matching for the whole pattern from wherever it
    // stands, which GRX_Frontend::initial_options is what reads. No CMNT,
    // no NAME, no COND, no RECU, no QUOT, no UPRP, no CSET, no POSS. OCT
    // and HEX are `\%o40` and `\%x2a`, which are spelled inside the `\%`
    // family rather than as `\x` - `\x` alone is the hex-digit *class*.
    .features = ALT | REP | LAZY | NCAP | BREF | LAH | LBH | ATOM | PCLS
        | WORD | ANCH | HEX | OCT,
    // The subject is text: `\%u00e9` matches the two bytes of "é", and `.`
    // takes a whole character rather than one of them.
    .default_options = GRX_OPT_UTF,
    // Read only by the prescan here, the operator spellings being the front
    // end's own per-position answer: four magic levels cannot be a flag.
    // It is still the right value for the prescan, whose one job is to
    // count `\(`.
    .escaped_specials = 1,
    // `\\&`, the only dialect here with a second joining operator: it is a
    // grammar level between `\\|` and concatenation, not a construct.
    .branch_and_operator = 1,
  },
  [GRX_SYNTAX_EMACS] = {
    .features = ALT | REP | LAZY | NCAP | BREF | PCLS | WORD | ANCH,
    .escaped_specials = 1,
  },
};

// The name a caller writes to select a dialect. Indexed by GRX_Syntax.
static const char * const spec_names[GRX_SYNTAX_COUNT] = {
  [GRX_SYNTAX_POSIX_BRE] = "posix-bre",
  [GRX_SYNTAX_POSIX_ERE] = "posix-ere",
  [GRX_SYNTAX_GNU_BRE] = "gnu-bre",
  [GRX_SYNTAX_GNU_ERE] = "gnu-ere",
  [GRX_SYNTAX_PERL] = "perl",
  [GRX_SYNTAX_PCRE] = "pcre",
  [GRX_SYNTAX_ECMASCRIPT] = "ecmascript",
  [GRX_SYNTAX_PYTHON] = "python",
  [GRX_SYNTAX_JAVA] = "java",
  [GRX_SYNTAX_DOTNET] = "dotnet",
  [GRX_SYNTAX_RUBY] = "ruby",
  [GRX_SYNTAX_RE2] = "re2",
  [GRX_SYNTAX_RUST] = "rust",
  [GRX_SYNTAX_TCL] = "tcl",
  [GRX_SYNTAX_VIM] = "vim",
  [GRX_SYNTAX_EMACS] = "emacs",
  // The RFC's own spelling. Its title calls the format "I-Regexp", its
  // grammar's start rule is `i-regexp`, and it registers no media type and no
  // other identifier (section 7 has no IANA actions), so there is nothing
  // more official to borrow. Lower case with a hyphen is this table's house
  // style, and grx_syntax_from_name() ignores case, so "I-Regexp" finds it.
  [GRX_SYNTAX_IREGEXP] = "i-regexp",
};

/**
 * The semantic profile of each dialect: what the constructs mean.
 *
 * Status: ECMAScript's row is filled from ECMA-262 and checked against Node
 * 22; every other row holds the value documentation/dialects.md section 5
 * states, and the cells that page marks **probe** are resolved by
 * documentation/plan.md WP-03 before code depends on them.
 *
 * A zeroed field is the first value of its enum, and it is worth knowing
 * exactly which value that is before leaving one out. It is *not* the Perl
 * family's: `GRX_CAPTURE_KEEP_LAST_SET`, `GRX_NEGATIVE_LOOK_CLEAR` and
 * `GRX_SHORTHANDS_ASCII` are all zero, and Perl takes none of the three -
 * its row states twelve fields explicitly for that reason. What zero gives
 * is the plainest reading of each axis, which is what a dialect with no row
 * written should behave as: ASCII shorthands rather than Unicode ones, a
 * subject of bytes rather than of text, captures kept rather than cleared.
 * A new row inherits that and overrides what its dialect actually differs
 * about - which for the POSIX and GNU rows is why six of these fields are
 * absent from them and right anyway.
 *
 * Reading a row: the second fold and shorthand columns are the values under
 * UTF or UCP. A dialect where the two differ is one where the same pattern
 * means different things with and without the flag, which is most of them.
 *
 * `template_spec` is the exception to the "zeroed means Perl" rule, and
 * deliberately: a zeroed spec has no sigil, and grx_regex_replace() refuses
 * a dialect with no sigil rather than applying a grammar that is not that
 * dialect's. Section 5.11 has all twelve rows written down; each is
 * transcribed when its dialect gets a front end and a test that can see it
 * be wrong, because a template grammar nothing can reach is a grammar
 * nothing can check.
 */
static const GRX_Profile profiles[GRX_SYNTAX_COUNT] = {
  // POSIX and GNU: leftmost-longest, no lookbehind, the C locale treated as
  // ASCII (a deviation, recorded in dialects.md section 6).
  [GRX_SYNTAX_POSIX_BRE] = {
    .preference = GRX_PREFER_LEFTMOST_LONGEST,
    .submatch = GRX_SUBMATCH_POSIX,
    // BREAK rather than ALLOW. ALLOW means "nothing special; the longest
    // match decides", which presumes a leftmost-longest engine; until
    // plan.md's WP-24 builds one, a backtracker given ALLOW repeats an empty
    // iteration until it runs out of budget, and `(a*)*` against "bc"
    // answered with a limit rather than with a match. BREAK is what
    // reproduces glibc for both that and `a(b|c?)+d` against "ad".
    .empty_loop = GRX_EMPTY_LOOP_BREAK_FIRST,
    .backref_unset = GRX_BACKREF_UNSET_FAILS,
    .lookbehind = GRX_LOOKBEHIND_NONE,
    .dollar = GRX_DOLLAR_END_ONLY,
    // A newline is the line terminator REG_NEWLINE makes `^` and `$` match
    // at. Without that option `dollar = END_ONLY` keeps them at the ends of
    // the subject, so naming the set here costs nothing until it is asked
    // for - and leaving it NONE made REG_NEWLINE a flag with no terminator
    // to act on, which is a flag that does nothing.
    .newlines = GRX_NEWLINES_LF,
    .fold = GRX_FOLD_ASCII,
    .fold_utf = GRX_FOLD_ASCII,
    // glibc matches `^$` against "abc\n" at offset 4 under REG_NEWLINE: the
    // empty run after a final newline *is* a line here, where PCRE2 and Perl
    // say it is not.
    .caret_after_final_newline = 1,
    // sed's `s` command right-hand side, which is what POSIX defines for
    // replacement and the only replacement grammar these dialects have -
    // POSIX's regular expressions say nothing about substitution, so the
    // reference is sed rather than a regex standard. Probed against GNU sed
    // 4.9: `&` is the whole match, `\&` is a literal one, `\1` to `\9` name
    // groups one digit at a time (`\10` is group 1 and a `0`, even with ten
    // groups), `\q` is a `q`, and a reference to a group the pattern has not
    // got is refused rather than substituted.
    .template_spec = {
      .sigil = '\\',
      .features = GRX_TMPL_NUMBER_SINGLE | GRX_TMPL_WHOLE_BARE
          | GRX_TMPL_ESCAPE_ANY,
      .missing = GRX_TMPL_MISSING_ERROR,
    },
  },
  [GRX_SYNTAX_POSIX_ERE] = {
    .preference = GRX_PREFER_LEFTMOST_LONGEST,
    .submatch = GRX_SUBMATCH_POSIX,
    // BREAK rather than ALLOW. ALLOW means "nothing special; the longest
    // match decides", which presumes a leftmost-longest engine; until
    // plan.md's WP-24 builds one, a backtracker given ALLOW repeats an empty
    // iteration until it runs out of budget, and `(a*)*` against "bc"
    // answered with a limit rather than with a match. BREAK is what
    // reproduces glibc for both that and `a(b|c?)+d` against "ad".
    .empty_loop = GRX_EMPTY_LOOP_BREAK_FIRST,
    .backref_unset = GRX_BACKREF_UNSET_FAILS,
    .lookbehind = GRX_LOOKBEHIND_NONE,
    .dollar = GRX_DOLLAR_END_ONLY,
    // A newline is the line terminator REG_NEWLINE makes `^` and `$` match
    // at. Without that option `dollar = END_ONLY` keeps them at the ends of
    // the subject, so naming the set here costs nothing until it is asked
    // for - and leaving it NONE made REG_NEWLINE a flag with no terminator
    // to act on, which is a flag that does nothing.
    .newlines = GRX_NEWLINES_LF,
    .fold = GRX_FOLD_ASCII,
    .fold_utf = GRX_FOLD_ASCII,
    // glibc matches `^$` against "abc\n" at offset 4 under REG_NEWLINE: the
    // empty run after a final newline *is* a line here, where PCRE2 and Perl
    // say it is not.
    .caret_after_final_newline = 1,
    // sed's `s` command right-hand side, which is what POSIX defines for
    // replacement and the only replacement grammar these dialects have -
    // POSIX's regular expressions say nothing about substitution, so the
    // reference is sed rather than a regex standard. Probed against GNU sed
    // 4.9: `&` is the whole match, `\&` is a literal one, `\1` to `\9` name
    // groups one digit at a time (`\10` is group 1 and a `0`, even with ten
    // groups), `\q` is a `q`, and a reference to a group the pattern has not
    // got is refused rather than substituted.
    .template_spec = {
      .sigil = '\\',
      .features = GRX_TMPL_NUMBER_SINGLE | GRX_TMPL_WHOLE_BARE
          | GRX_TMPL_ESCAPE_ANY,
      .missing = GRX_TMPL_MISSING_ERROR,
    },
  },
  [GRX_SYNTAX_GNU_BRE] = {
    .preference = GRX_PREFER_LEFTMOST_LONGEST,
    // BREAK rather than ALLOW. ALLOW means "nothing special; the longest
    // match decides", which presumes a leftmost-longest engine; until
    // plan.md's WP-24 builds one, a backtracker given ALLOW repeats an empty
    // iteration until it runs out of budget, and `(a*)*` against "bc"
    // answered with a limit rather than with a match. BREAK is what
    // reproduces glibc for both that and `a(b|c?)+d` against "ad".
    .empty_loop = GRX_EMPTY_LOOP_BREAK_FIRST,
    .backref_unset = GRX_BACKREF_UNSET_FAILS,
    .lookbehind = GRX_LOOKBEHIND_NONE,
    .dollar = GRX_DOLLAR_END_ONLY,
    // A newline is the line terminator REG_NEWLINE makes `^` and `$` match
    // at. Without that option `dollar = END_ONLY` keeps them at the ends of
    // the subject, so naming the set here costs nothing until it is asked
    // for - and leaving it NONE made REG_NEWLINE a flag with no terminator
    // to act on, which is a flag that does nothing.
    .newlines = GRX_NEWLINES_LF,
    .fold = GRX_FOLD_ASCII,
    .fold_utf = GRX_FOLD_ASCII,
    // glibc matches `^$` against "abc\n" at offset 4 under REG_NEWLINE: the
    // empty run after a final newline *is* a line here, where PCRE2 and Perl
    // say it is not.
    .caret_after_final_newline = 1,
    // sed's `s` command right-hand side, which is what POSIX defines for
    // replacement and the only replacement grammar these dialects have -
    // POSIX's regular expressions say nothing about substitution, so the
    // reference is sed rather than a regex standard. Probed against GNU sed
    // 4.9: `&` is the whole match, `\&` is a literal one, `\1` to `\9` name
    // groups one digit at a time (`\10` is group 1 and a `0`, even with ten
    // groups), `\q` is a `q`, and a reference to a group the pattern has not
    // got is refused rather than substituted. GNU adds `\0` for the whole
    // match, which POSIX does not have.
    .template_spec = {
      .sigil = '\\',
      .features = GRX_TMPL_NUMBER_SINGLE | GRX_TMPL_WHOLE_BARE
          | GRX_TMPL_WHOLE_ZERO | GRX_TMPL_ESCAPE_ANY,
      .missing = GRX_TMPL_MISSING_ERROR,
    },
  },
  [GRX_SYNTAX_GNU_ERE] = {
    .preference = GRX_PREFER_LEFTMOST_LONGEST,
    // BREAK rather than ALLOW. ALLOW means "nothing special; the longest
    // match decides", which presumes a leftmost-longest engine; until
    // plan.md's WP-24 builds one, a backtracker given ALLOW repeats an empty
    // iteration until it runs out of budget, and `(a*)*` against "bc"
    // answered with a limit rather than with a match. BREAK is what
    // reproduces glibc for both that and `a(b|c?)+d` against "ad".
    .empty_loop = GRX_EMPTY_LOOP_BREAK_FIRST,
    .backref_unset = GRX_BACKREF_UNSET_FAILS,
    .lookbehind = GRX_LOOKBEHIND_NONE,
    .dollar = GRX_DOLLAR_END_ONLY,
    // A newline is the line terminator REG_NEWLINE makes `^` and `$` match
    // at. Without that option `dollar = END_ONLY` keeps them at the ends of
    // the subject, so naming the set here costs nothing until it is asked
    // for - and leaving it NONE made REG_NEWLINE a flag with no terminator
    // to act on, which is a flag that does nothing.
    .newlines = GRX_NEWLINES_LF,
    .fold = GRX_FOLD_ASCII,
    .fold_utf = GRX_FOLD_ASCII,
    // glibc matches `^$` against "abc\n" at offset 4 under REG_NEWLINE: the
    // empty run after a final newline *is* a line here, where PCRE2 and Perl
    // say it is not.
    .caret_after_final_newline = 1,
    // sed's `s` command right-hand side, which is what POSIX defines for
    // replacement and the only replacement grammar these dialects have -
    // POSIX's regular expressions say nothing about substitution, so the
    // reference is sed rather than a regex standard. Probed against GNU sed
    // 4.9: `&` is the whole match, `\&` is a literal one, `\1` to `\9` name
    // groups one digit at a time (`\10` is group 1 and a `0`, even with ten
    // groups), `\q` is a `q`, and a reference to a group the pattern has not
    // got is refused rather than substituted. GNU adds `\0` for the whole
    // match, which POSIX does not have.
    .template_spec = {
      .sigil = '\\',
      .features = GRX_TMPL_NUMBER_SINGLE | GRX_TMPL_WHOLE_BARE
          | GRX_TMPL_WHOLE_ZERO | GRX_TMPL_ESCAPE_ANY,
      .missing = GRX_TMPL_MISSING_ERROR,
    },
  },

  // The Perl family. Full folding is implemented as simple folding and
  // recorded as a deviation (design.md section 10).
  [GRX_SYNTAX_PERL] = {
    // An iteration that consumed nothing succeeds and stops the loop, which
    // is section 5.5's BREAK_ON_EMPTY. `(a*)*` against "b" reports group 1
    // as the empty string in perl and in pcre2test, and as unset in
    // ECMAScript - and this row said nothing, so it got ECMAScript's answer.
    .empty_loop = GRX_EMPTY_LOOP_BREAK,
    // RESET_AFTER_EACH, not KEEP_LAST_SET: Perl 5.40 reports group 2 of
    // `((a)|b)+` against "ab" as unset, where PCRE2 and Python report "a".
    // *After* each, not before: `((?(2)x|y)(a))+` against "yaxa" matches the
    // whole subject in perl and in pcre2test, which it can only do if the
    // second iteration's conditional still sees what the first captured.
    // Probed rather than read; see tests/data/probe/report.md and
    // documentation/dialects.md section 5.5.
    .capture_reset = GRX_CAPTURE_RESET_AFTER_EACH,
    // `[:lower:]` and `[:upper:]` are one class under `/i` at both widths
    // here, where pcre2 stops at the ASCII one. Measured with the subject
    // upgraded, because below U+0100 an unupgraded perl string gets ASCII
    // semantics: `/i` takes "A", U+017F, U+212A and U+00C9, and refuses
    // U+05D0. See the field.
    .posix_case_classes_collapse_wide = 1,
    .lookbehind = GRX_LOOKBEHIND_BOUNDED,
    // Perl alone. `a(?!(b)c)` against "abd" reports group 1 as "b" here and
    // unset in pcre2test and in Node: the body captured it before failing,
    // and Perl does not take it back. ECMA-262 22.2.2.4 says the other two.
    .negative_look = GRX_NEGATIVE_LOOK_KEEP,
    .split = GRX_SPLIT_PERL,
    // Perl alone again, and for a reason that is about who writes the loop.
    // `\G` is `pos()`, a property of the string that a *failed* match does
    // not move; PCRE2 has no loop of its own, so its `\G` can only mean the
    // start offset its caller passed this time. `\Ga*` against "baac" is
    // four matches in pcre2test and one in perl. Found by
    // tools/oracle/iterate_diff.py, which is the first thing here to ask
    // either reference for every match rather than for the first.
    .search_start = GRX_SEARCH_START_PREVIOUS_END,
    .dollar = GRX_DOLLAR_BEFORE_FINAL_NEWLINE,
    .shorthands = GRX_SHORTHANDS_UNICODE,
    .shorthands_wide = GRX_SHORTHANDS_UNICODE,
    // GRX_WORD_UTS18 by omission, and this is the dialect that makes it the
    // default: perl's `\w` *is* Annex C, to the code point, over all 286,719
    // that it and pcre2 and UCD 17.0.0 agree are assigned.
    //
    // `[[:graph:]]` is the other half, and there perl is the outlier: it
    // counts the 137,468 private-use code points as graphic and pcre2 does
    // not. U+E000 with the subject upgraded settles it.
    .posix_graph_takes_private_use = 1,
    // Full folding, which is the one thing about Perl's `/i` that a class
    // cannot express: `ß` matches "ss" and `ff` matches the `ﬀ` ligature,
    // so a caseless match here can be a different length from the pattern
    // that asked for it. See GRX_IR_FOLD_RUN.
    .fold = GRX_FOLD_FULL,
    .fold_utf = GRX_FOLD_FULL,
    .property_match = GRX_PROPERTY_LOOSE_PERL,
    // Perl has no byte mode. A Perl string is a sequence of characters and
    // `/u` says which *rules* to apply to them, not whether to decode them;
    // the row said nothing and so took PCRE2's answer, which is that a
    // subject is bytes until the caller says otherwise. The corpus is the
    // evidence: its subjects are Perl's own character strings written out
    // as UTF-8, and its offsets are the byte offsets into that.
    .subject_is_text = 1,
    // The interpolation subset of section 5.11. A Perl template is a double-
    // quoted string, so the escape is `\$` and not `$$` - `$$` is the
    // process id, which is why the two rules cannot both be true of one
    // dialect. A reference to a group that does not exist interpolates
    // undef, which is the empty string.
    //
    // The case operators `\U \L \E \u \l \Q` are not here: they are string
    // operators that happen to be legal in a replacement, and implementing
    // them without the rest of Perl's interpolation would be a grammar this
    // library invented.
    .template_spec = {
      .sigil = '$',
      // CASE_ESCAPES and CASE_FULL, and the pair is perl's alone here: vim has
      // the same six letters with the *simple* mapping, and PCRE2 has them
      // only under PCRE2_SUBSTITUTE_EXTENDED, which this library does not
      // expose (WP-22). Measured against the pinned perl through
      // tools/corpus/perl_subst.pl a spelling at a time.
      .features = GRX_TMPL_NUMBER | GRX_TMPL_NUMBER_BRACED
          | GRX_TMPL_NAME_PLUS_BRACE | GRX_TMPL_WHOLE | GRX_TMPL_PREFIX
          | GRX_TMPL_SUFFIX | GRX_TMPL_BACKSLASH_ESCAPE
          | GRX_TMPL_CASE_ESCAPES,
      .missing = GRX_TMPL_MISSING_EMPTY,
    },
  },
  [GRX_SYNTAX_PCRE] = {
    .empty_loop = GRX_EMPTY_LOOP_BREAK,
    .lookbehind = GRX_LOOKBEHIND_BOUNDED,
    .dollar = GRX_DOLLAR_BEFORE_FINAL_NEWLINE,
    .shorthands = GRX_SHORTHANDS_ASCII,
    .shorthands_wide = GRX_SHORTHANDS_UNICODE,
    // `\w` under `PCRE2_UCP` is `\p{L}\p{N}\p{Mn}\p{Pc}`, which crosses UTS
    // #18 rather than narrowing or widening it: 915 code points of `No` that
    // Annex C has not got, against 598 of `Mc`, `Me`, alphabetic `So` and
    // the join controls that it has. This dialect answered Annex C's set
    // until 2026-09-24, wrong by all 1,513.
    .word_set = GRX_WORD_CATEGORIES,
    // And `[[:graph:]]` drops six `Cf` characters by name. Private use it
    // already dropped, which is why there is no second field here.
    .posix_graph_drops_invisibles = 1,
    // `[[:alpha:]]` is `\p{L}` here and `\p{Alphabetic}` in perl, and the
    // same choice runs through `alnum`, `lower` and `upper`. 1,694, 2,373,
    // 312 and 120 code points; all four were perl's set until 2026-09-24.
    .posix_wide_general_category = 1,
    // U+180E was `Zs` until Unicode 6.3 and PCRE2 kept it a space. `\h`,
    // `\s`, `[[:blank:]]` and `[[:space:]]` take it here and in no other
    // dialect; `\v` does not, in either.
    .mongolian_separator_is_space = 1,
    .fold = GRX_FOLD_ASCII,
    .fold_utf = GRX_FOLD_SIMPLE,
    .property_match = GRX_PROPERTY_LOOSE,
    // pcre2_substitute()'s grammar, section 5.11. No `$&`, no `` $` `` and
    // no `$'`: PCRE2 never took those, and a reference to a group the
    // pattern does not have is an error rather than literal text.
    //
    // What is *not* here is the extended substitution syntax - `\U`, `\L`,
    // `${n:+a:b}`, `$*MARK` - which pcre2_substitute() reads only under
    // PCRE2_SUBSTITUTE_EXTENDED. It is an option this library does not
    // expose, and the ordinary grammar is the one a caller gets by default.
    .template_spec = {
      .sigil = '$',
      // WHOLE and WHOLE_ZERO because pcre2 spells the whole match three
      // ways - `$&`, `$0` and `${0}` - and this row had none of them. Before
      // GRX_TMPL_SIGIL_STRICT arrived they fell through to "unrecognised is
      // literal text" and `$&` substituted the two characters `$&`, which is
      // what a generated template found. pcre2_substitute() substitutes the
      // match.
      .features = GRX_TMPL_NUMBER_GREEDY | GRX_TMPL_NUMBER_BRACED
          | GRX_TMPL_NAME_BRACED | GRX_TMPL_NAME_BARE | GRX_TMPL_NAME_ANGLE
          | GRX_TMPL_DOUBLE_SIGIL | GRX_TMPL_SIGIL_STRICT
          | GRX_TMPL_WHOLE | GRX_TMPL_WHOLE_ZERO
          | GRX_TMPL_PREFIX | GRX_TMPL_SUFFIX | GRX_TMPL_SUBJECT
          | GRX_TMPL_UNSET_ERROR,
      .missing = GRX_TMPL_MISSING_ERROR,
    },
  },

  // ECMAScript. Every value here is ECMA-262's, and each of the four that
  // distinguish it from the Perl family has a test that states the rule:
  // an empty iteration fails rather than breaking the loop (22.2.2.3.1
  // RepeatMatcher step 2.b), captures reset each iteration (step 4), a
  // reference to an unset group matches the empty string, and `$` without
  // `m` means the end of the subject and nothing else.
  [GRX_SYNTAX_ECMASCRIPT] = {
    .preference = GRX_PREFER_LEFTMOST_FIRST,
    .caret_after_final_newline = 1,
    .empty_loop = GRX_EMPTY_LOOP_FAIL,
    .capture_reset = GRX_CAPTURE_RESET_EACH,
    .backref_unset = GRX_BACKREF_UNSET_EMPTY,
    .lookbehind = GRX_LOOKBEHIND_UNBOUNDED,
    .iteration = GRX_ITERATE_ADVANCE_ONE,
    .dollar = GRX_DOLLAR_END_ONLY,
    .newlines = GRX_NEWLINES_ECMASCRIPT,
    // The same sets in both modes, deliberately. ECMAScript's `\w` gains
    // U+017F and U+212A under `iu`, but that is not a different set - it is
    // what closing the ASCII one under simple folding produces, which is
    // exactly how ECMA-262 22.2.2.9.3 defines it. Writing it as a second set
    // here would apply the widening under `u` alone, where `/\w/u` does not
    // match U+017F.
    .shorthands = GRX_SHORTHANDS_ECMASCRIPT,
    .shorthands_wide = GRX_SHORTHANDS_ECMASCRIPT,
    // And the widening the folding does to them is ECMAScript's alone. See
    // the field: pcre2, perl and CPython all leave `\w` and `\b` where
    // they are under a caseless flag, and this library did it for them too.
    .caseless_widens_shorthands = 1,
    .fold = GRX_FOLD_ES_LEGACY,
    .fold_utf = GRX_FOLD_SIMPLE,
    .property_match = GRX_PROPERTY_STRICT,
    .subject_is_text = 1,
    // String.prototype.replace's grammar, GetSubstitution (22.1.3.19.1).
    // No case operators and no `${n}`: both are Perl spellings ECMAScript
    // never took. `$<name>` is conditional on the pattern having named
    // groups, which is the one rule here that is not a spelling.
    .template_spec = {
      .sigil = '$',
      .features = GRX_TMPL_NUMBER | GRX_TMPL_NAME_ANGLE | GRX_TMPL_WHOLE
          | GRX_TMPL_PREFIX | GRX_TMPL_SUFFIX | GRX_TMPL_DOUBLE_SIGIL
          | GRX_TMPL_NAME_NEEDS_NAMED_GROUPS,
      .missing = GRX_TMPL_MISSING_LITERAL,
    },
  },

  // I-Regexp. Most of a profile answers "what does this construct mean", and
  // this dialect does not have most of the constructs - so the cells below
  // are the few with a reader, and the ones left at zero are left there
  // because nothing can ask them: there is no `$`, no lookbehind, no
  // backreference, no `\w`, no `\<` and no composing rule in a grammar that
  // has no such spelling. A value in those cells would be a value no test
  // could distinguish from any other.
  //
  // What does have a reader:
  //
  // - **newlines**, because `.` is defined by them. XSD's `.` excludes CR and
  //   LF and nothing else, which is ANYCRLF's set of single characters - so
  //   `.` matches U+2028 here where ECMAScript's `.` does not. The CR LF
  //   *pair* half of that convention has no reader, the anchors it moves not
  //   existing.
  // - **property_match**, which must be STRICT: Figure 1's names are
  //   `%s`-literals, so `\p{lu}` is not `\p{Lu}` and loose UAX #44 matching
  //   would accept the spelling the ABNF refuses.
  // - **subject_is_text**, because the subject is Unicode scalar values.
  // - **fold**, which the *pattern* cannot ask for - there is no flag in the
  //   syntax - and a caller can, GRX_OPT_CASELESS being the caller's to set
  //   in any dialect. Simple folding is the answer then, not full: a full
  //   fold makes one pattern character match two subject characters, and
  //   nothing in RFC 9485 or XSD contemplates that.
  // - **preference**, which is unobservable in the RFC's own terms - it has
  //   only a Boolean to return - and observable here, because this library
  //   reports spans. Leftmost-first is what every engine a JSONPath
  //   implementation is likely to be using does.
  //
  // - **template_spec is deliberately absent.** RFC 9485 defines a Boolean
  //   match and no replacement grammar, so there is no sigil to write here
  //   and grx_regex_replace() refuses a template in this dialect with
  //   GRX_DIAG_NOT_IN_DIALECT unless the caller passes GRX_REPLACE_LITERAL.
  [GRX_SYNTAX_IREGEXP] = {
    .preference = GRX_PREFER_LEFTMOST_FIRST,
    .empty_loop = GRX_EMPTY_LOOP_FAIL,
    .newlines = GRX_NEWLINES_ANYCRLF,
    .fold = GRX_FOLD_SIMPLE,
    .fold_utf = GRX_FOLD_SIMPLE,
    .property_match = GRX_PROPERTY_STRICT,
    .subject_is_text = 1,
  },
  [GRX_SYNTAX_PYTHON] = {
    // BREAK, not the FAIL a silent row gets. `(a*)*` against "b" reports
    // group 1 as the empty string in CPython 3.13, as it does in perl and
    // pcre2test, and as unset in ECMAScript - and this row said nothing, so
    // it took ECMAScript's answer. Exactly the defect the Perl row carried
    // until WP-21 probed it, still sitting here because nothing had ever
    // run a Python pattern to notice.
    .empty_loop = GRX_EMPTY_LOOP_BREAK,
    // KEEP_LAST_SET, and probed rather than assumed: `((a)|b)+` against
    // "ab" reports group 2 as "a" here and in pcre2test, and *unset* in
    // perl 5.40. Python sides with PCRE2 on the axis where the two
    // Perl-family references disagree.
    .capture_reset = GRX_CAPTURE_KEEP_LAST_SET,
    .lookbehind = GRX_LOOKBEHIND_FIXED,
    // Python's split is neither of the two rules this library had, which is
    // why there is now a third. It keeps trailing empty fields and yields
    // one empty piece from an empty subject, which is ECMAScript's half;
    // its `maxsplit` counts *splits* and leaves the remainder as the last
    // field, and zero means no limit at all, which is Perl's. Taking either
    // whole would have been wrong in one direction or the other:
    // `re.split(",", "a,b,c", maxsplit=1)` is `['a', 'b,c']` where
    // ECMAScript's `"a,b,c".split(",", 1)` is `['a']`.
    .split = GRX_SPLIT_PYTHON,
    .dollar = GRX_DOLLAR_BEFORE_FINAL_NEWLINE,
    // Unicode in both columns: a `str` pattern's shorthands are Unicode
    // whatever else is set, and `(?a)` narrows them with
    // GRX_OPT_ASCII_CLASSES rather than by turning a widening flag off.
    // Written the other way round first, with UCP on by default and `(?a)`
    // clearing it - which narrowed the shorthands correctly and also
    // stopped `.` matching a whole character and `\N{BULLET}` compiling,
    // because those key on UTF and `re`'s ASCII mode touches neither.
    .shorthands = GRX_SHORTHANDS_UNICODE,
    .shorthands_wide = GRX_SHORTHANDS_UNICODE,
    // `re`'s `\w` is `isalnum` plus `_`, the narrowest of the three word
    // sets: no marks at all, and no connector punctuation but the underscore
    // itself. This dialect answered Annex C's set until 2026-09-24, wrong by
    // 3,506 code points - every `Mn`, `Mc` and `Me`, the alphabetic `So`,
    // the join controls and eight of the nine other `Pc`, against the `No`
    // that `re` has and Annex C has not.
    //
    // `re` has no POSIX classes, so neither graph field says anything here.
    .word_set = GRX_WORD_ALNUM,
    .fold = GRX_FOLD_SIMPLE,
    .fold_utf = GRX_FOLD_SIMPLE,
    // ...and the same flag narrows the folding, which is where Python and
    // Perl's `/a` part company. See the field's own comment.
    .ascii_classes_fold_ascii = 1,
    .caret_after_final_newline = 1,
    // `re.sub`'s template grammar. The sigil is a backslash, as POSIX's is:
    // `\1`, `\g<1>` and `\g<name>` are the three references, `\g<0>` is the
    // whole match and `\0` is NUL rather than the whole match, and an
    // unknown escape such as `\q` is an error rather than the literal.
    .template_spec = {
      .sigil = '\\',
      // SIGIL_STRICT for the one spelling the escape reader cannot see: a
      // backslash at the very end of the template, where there is no next
      // character to classify. `re` calls it "bad escape (end of pattern)".
      .features = GRX_TMPL_NUMBER | GRX_TMPL_G_ANGLE
          | GRX_TMPL_PYTHON_ESCAPES | GRX_TMPL_SIGIL_STRICT,
      .missing = GRX_TMPL_MISSING_ERROR,
    },
    // The subject of `re` is a sequence of code points in every mode. A
    // `bytes` pattern is a separate API rather than a flag, and not one this
    // library models, so the decoding here is unconditional the way
    // ECMAScript's is - and `(?a)` moves the *rules*, not the decoding.
    .subject_is_text = 1,
  },
  [GRX_SYNTAX_JAVA] = {
    .lookbehind = GRX_LOOKBEHIND_BOUNDED,
    .dollar = GRX_DOLLAR_BEFORE_FINAL_NEWLINE,
    .newlines = GRX_NEWLINES_UNICODE,
    .shorthands = GRX_SHORTHANDS_ASCII,
    .shorthands_wide = GRX_SHORTHANDS_UNICODE,
    .fold = GRX_FOLD_ASCII,
    .fold_utf = GRX_FOLD_SIMPLE,
    .property_match = GRX_PROPERTY_LOOSE,
  },
  [GRX_SYNTAX_DOTNET] = {
    .lookbehind = GRX_LOOKBEHIND_UNBOUNDED,
    .iteration = GRX_ITERATE_ADVANCE_ONE,
    .dollar = GRX_DOLLAR_BEFORE_FINAL_NEWLINE,
    .shorthands = GRX_SHORTHANDS_UNICODE,
    .shorthands_wide = GRX_SHORTHANDS_UNICODE,
    .fold = GRX_FOLD_SIMPLE,
    .fold_utf = GRX_FOLD_SIMPLE,
  },
  [GRX_SYNTAX_RUBY] = {
    .lookbehind = GRX_LOOKBEHIND_FIXED_PER_BRANCH,
    .iteration = GRX_ITERATE_ADVANCE_ONE,
    .dollar = GRX_DOLLAR_ALWAYS_LINE,
    .multiline_by_default = 1,
    .shorthands = GRX_SHORTHANDS_ASCII,
    .shorthands_wide = GRX_SHORTHANDS_ASCII,
    .fold = GRX_FOLD_SIMPLE,
    .fold_utf = GRX_FOLD_SIMPLE,
    .property_match = GRX_PROPERTY_LOOSE,
  },

  // RE2 and Rust: no backreference and no lookaround at all, which is the
  // point of them - a pattern they accept is regular by construction.
  [GRX_SYNTAX_RE2] = {
    .empty_loop = GRX_EMPTY_LOOP_BREAK,
    .lookbehind = GRX_LOOKBEHIND_NONE,
    .dollar = GRX_DOLLAR_END_ONLY,
    .shorthands = GRX_SHORTHANDS_ASCII,
    .shorthands_wide = GRX_SHORTHANDS_ASCII,
    .fold = GRX_FOLD_SIMPLE,
    .fold_utf = GRX_FOLD_SIMPLE,
    .property_match = GRX_PROPERTY_STRICT,
  },
  [GRX_SYNTAX_RUST] = {
    .empty_loop = GRX_EMPTY_LOOP_BREAK,
    .lookbehind = GRX_LOOKBEHIND_NONE,
    .iteration = GRX_ITERATE_ADVANCE_SKIP_ABUTTING,
    .dollar = GRX_DOLLAR_END_ONLY,
    .shorthands = GRX_SHORTHANDS_UNICODE,
    .shorthands_wide = GRX_SHORTHANDS_UNICODE,
    .fold = GRX_FOLD_SIMPLE,
    .fold_utf = GRX_FOLD_SIMPLE,
    .property_match = GRX_PROPERTY_LOOSE,
  },

  [GRX_SYNTAX_TCL] = {
    .preference = GRX_PREFER_LEFTMOST_LONGEST,
    .empty_loop = GRX_EMPTY_LOOP_ALLOW,
    .lookbehind = GRX_LOOKBEHIND_FIXED_PER_BRANCH,
    .dollar = GRX_DOLLAR_END_ONLY,
    .shorthands = GRX_SHORTHANDS_UNICODE,
    .shorthands_wide = GRX_SHORTHANDS_UNICODE,
    .fold = GRX_FOLD_SIMPLE,
    .fold_utf = GRX_FOLD_SIMPLE,
  },
  [GRX_SYNTAX_VIM] = {
    // KEEP_LAST_SET, measured: `\v(a|b)*` against "ab" reports group 1 as
    // "b", so an iteration that does not write does not clear either.
    .capture_reset = GRX_CAPTURE_KEEP_LAST_SET,
    // MATCH_EMPTY, measured: `\(a\)\?\1` matches the empty string
    // against "b", where a dialect that fails on an unset reference would
    // report no match. documentation/dialects.md section 5.6 had this cell
    // marked **probe**; this is the probe.
    .backref_unset = GRX_BACKREF_UNSET_EMPTY,
    .lookbehind = GRX_LOOKBEHIND_UNBOUNDED,
    // END_ONLY and no lines, where this row said ALWAYS_LINE and multiline.
    // Both were written from vim's help, which describes matching against a
    // *buffer*. The subject this library has is a string, and over a string
    // vim answers differently and consistently: `a.b` matches "a\nb",
    // `[^x]` matches the newline, `^b` does not match "a\nb" and neither
    // does `\_^b`. So the line break is an ordinary character at both ends
    // of the question, which is GRX_NEWLINES_NONE and GRX_DOLLAR_END_ONLY.
    // The `\_x` forms still differ from their plain spellings, because
    // `\s` refuses a newline and `\_s` accepts one.
    .dollar = GRX_DOLLAR_END_ONLY,
    .newlines = GRX_NEWLINES_NONE,
    // Measured, where this cell had never been probed either. An empty
    // iteration that is the loop's *first* runs in vim and its writes
    // stick: `a\%(\zs\)*b` over "ab" is 1-2 there, in both engines, and
    // would be 0-2 under ECMA-262's rule - which is what a zeroed cell
    // gave. A trailing one, after an iteration has consumed, is where
    // vim's two engines part: `\%(a\|\zs\)*` over "aa" is 2-2 under
    // `re=2` and 0-2 under `re=1`, and the old engine is the one whose
    // answer BREAK_FIRST is.
    .empty_loop = GRX_EMPTY_LOOP_BREAK_FIRST,
    // Measured, where this cell had never been probed and so read Perl's.
    // Two clauses, and vim differs from every other reference here on both:
    // after an empty match it *advances* rather than retrying without one -
    // `substitute("aab", '\\|a', "<>", "g")` is "<>a<>a<>b<>" there and
    // would be "<><><><><>b<>" under Perl's rule - and a match that reaches
    // the end of the subject ends the loop, so `b*` over "ab" is "<>a<>"
    // and not node's, perl's and `re`'s "<>a<><>".
    .iteration = GRX_ITERATE_ADVANCE_ONE_STOP_AT_END,
    // Read by the POSIX classes alone, and only for whether they widen:
    // vim's eleven named classes are built out as explicit sets by the
    // front end, because three of them have a counterpart here and all
    // three differ - vim's `\s` is space and tab alone. This row named a
    // word set of its own while `\<` and `\>` were the word set's two
    // halves; they are GRX_WORD_BOUNDARY_VIM_CLASS now and read no set at
    // all, so what is left for this field to say is "not Unicode".
    .shorthands = GRX_SHORTHANDS_ASCII,
    .shorthands_wide = GRX_SHORTHANDS_ASCII,
    // `\<` and `\>` hold where vim's character *class* changes, and it has
    // nine of them: `\>` holds between U+65E5 and "x" there, where both are
    // keyword characters. Measured with `charclass()` over every code
    // point; src/unicode/vim_class.c is the table and the rule.
    .word_boundary = GRX_WORD_BOUNDARY_VIM_CLASS,
    // A base character and the composing characters after it are one
    // character to vim: `.` over "a" U+0301 is 0-3 there, a literal `a`
    // matches none of it, and no match begins or ends inside the cluster.
    // `\Z` is the same model with the marks made optional, and sets
    // GRX_OPT_IGNORE_COMBINING from wherever it stands.
    .composing = GRX_COMPOSING_CLUSTER,
    // SIMPLE, where this row said ASCII. Measured: `\cÉ` matches "é" in
    // vim 9.1, so the folding is Unicode and the page that said otherwise
    // was describing a version of vim without multibyte support.
    .fold = GRX_FOLD_SIMPLE,
    .fold_utf = GRX_FOLD_SIMPLE,
    .subject_is_text = 1,
    // `[[:lower:]]` matches "é" and `[[:upper:]]` matches "É", where
    // `[[:alpha:]]` matches neither. Measured; see the field.
    .posix_case_classes_wide = 1,
    // ...and they are the case-mapped pair, not the Unicode properties:
    // 1,478 and 1,460 code points against `\p{Lowercase}`'s 2,595 and
    // `\p{Uppercase}`'s 2,006. See the field.
    .posix_case_classes_case_mapped = 1,
    // Probed against vim 9.1's `substitute()`, which is the string form of
    // `:s` and so the one this library can be: `&` is the whole match,
    // `\&` a literal one, `\0` the whole match again, `\1` to `\9` name
    // groups one digit at a time, and a reference to a group the pattern
    // has not got substitutes nothing rather than failing. `~` is a literal
    // tilde, there having been no previous substitution.
    //
    // The two rules nothing else here has are both real and neither is what
    // this row used to say they were. Over a *buffer* `\r` writes a line
    // break and `\n` a NUL; over a string they are U+000D and U+000A, and
    // `\t` and `\b` join them - four decoded escapes, GRX_TMPL_VIM_ESCAPES.
    // And `\u`, `\l`, `\U`, `\L`, `\E` and `\e` change the case of what
    // follows, which is the same feature bit and the only construct in
    // section 5.11 that emits nothing and still changes the answer.
    .template_spec = {
      .sigil = '\\',
      .features = GRX_TMPL_NUMBER_SINGLE | GRX_TMPL_WHOLE_ZERO
          | GRX_TMPL_WHOLE_BARE | GRX_TMPL_VIM_ESCAPES
          | GRX_TMPL_ESCAPE_ANY,
      .missing = GRX_TMPL_MISSING_EMPTY,
    },
  },
  [GRX_SYNTAX_EMACS] = {
    .lookbehind = GRX_LOOKBEHIND_NONE,
    .dollar = GRX_DOLLAR_ALWAYS_LINE,
    .multiline_by_default = 1,
    .shorthands = GRX_SHORTHANDS_UNICODE,
    .shorthands_wide = GRX_SHORTHANDS_UNICODE,
    .fold = GRX_FOLD_ASCII,
    .fold_utf = GRX_FOLD_ASCII,
  },
};

GRX_Result grx_syntax_profile(GRX_Syntax syntax, GRX_Profile * out_profile) {
  if (!out_profile || (unsigned)syntax >= (unsigned)GRX_SYNTAX_COUNT) {
    return GRX_ERR_INVALID;
  }

  *out_profile = profiles[syntax];
  return GRX_OK;
}

const GRX_SyntaxSpec * grx_syntax_spec_table(void) {
  return spec_table;
}

GRX_Result grx_syntax_spec(GRX_Syntax syntax, GRX_SyntaxSpec * out_spec) {
  // Cast rather than testing `syntax < 0` as well: the enum's underlying
  // type is implementation-defined, and where it is unsigned that test is
  // always false and -Wextra says so, which under -Werror is a build failure.
  if (!out_spec || (unsigned)syntax >= (unsigned)GRX_SYNTAX_COUNT) {
    return GRX_ERR_INVALID;
  }

  *out_spec = spec_table[syntax];
  return GRX_OK;
}

const char * grx_syntax_name(GRX_Syntax syntax) {
  if ((unsigned)syntax >= (unsigned)GRX_SYNTAX_COUNT || !spec_names[syntax]) {
    return "unknown";
  }

  return spec_names[syntax];
}

GRX_Result grx_syntax_from_name(const char * name, GRX_Syntax * out_syntax) {
  if (!name || !out_syntax) {
    return GRX_ERR_INVALID;
  }

  for (size_t i = 0; i < (size_t)GRX_SYNTAX_COUNT; i++) {
    const char * candidate = spec_names[i];
    if (!candidate) {
      continue;
    }
    size_t j = 0;
    // Case-insensitive without strcasecmp, which is POSIX rather than C17 and
    // so is not declared under -std=c17 -pedantic-errors.
    for (;; j++) {
      unsigned char a = (unsigned char)name[j];
      unsigned char b = (unsigned char)candidate[j];
      if (a >= 'A' && a <= 'Z') {
        a = (unsigned char)(a - 'A' + 'a');
      }
      if (a != b) {
        break;
      }
      if (!a) {
        *out_syntax = (GRX_Syntax)i;
        return GRX_OK;
      }
    }
  }

  return GRX_ERR_INVALID;
}

int grx_syntax_has_feature(GRX_Syntax syntax, GRX_Feature feature) {
  if ((unsigned)syntax >= (unsigned)GRX_SYNTAX_COUNT) {
    return 0;
  }

  return (spec_table[syntax].features & (uint64_t)feature) != 0;
}

/**
 * How one letter of a dialect's flag alphabet is treated.
 *
 * Three kinds, because "this dialect does not have that letter" and "this
 * letter is not a compile-time option" and "this library does not implement
 * what that letter asks for" are three different answers, and a caller
 * deciding what to tell a user needs them apart.
 */
typedef enum {
  FLAG_OPTION = 0, ///< Sets the option bits in `options`.
  FLAG_SEARCH,     ///< A search mode, expressed by an API call instead.
  FLAG_NO_EFFECT,  ///< Accepted and meaningless at compile time.
  FLAG_UNSUPPORTED ///< The dialect has it and this library does not.
} FlagKind;

/** One letter of one dialect's alphabet. */
typedef struct {
  char letter;
  FlagKind kind;
  uint32_t options; ///< For FLAG_OPTION.
  /**
   * Letters sharing a non-zero group exclude each other.
   *
   * A group rather than a mask of forbidden option bits, because the
   * exclusion has to be symmetric and a mask is not: ECMAScript's `v`
   * implies `u`, so "refuse `v` when GRX_OPT_UTF is already set" would
   * refuse `v` alone after any earlier letter that set it, and "refuse `u`
   * when GRX_OPT_UNICODE_SETS is set" catches `vu` and misses `uv`.
   */
  uint32_t group;
  /**
   * What a second occurrence of this letter adds, or 0 if there may not be
   * one.
   *
   * PCRE2 and Perl both spell "extended, and inside a class as well" as the
   * letter `x` twice. It is the only letter in any alphabet here that means
   * something different repeated, and without this field the duplicate check
   * below reads `xx` as a mistake - which is what it did until the corpus
   * showed `/[a-  z]/xx` compiling and `/[a-  z]/x` not.
   */
  uint32_t repeat_options;
} FlagRow;

/**
 * The alphabets of documentation/dialects.md section 5.15.
 *
 * Data rather than a switch, because the difference between two dialects'
 * alphabets is then something a reader can see in one place - which is the
 * same argument as the feature table above.
 */
static const FlagRow ecmascript_flags[] = {
  {'d', FLAG_NO_EFFECT, 0, 0, 0}, // Match indices; always available here.
  {'g', FLAG_SEARCH, 0, 0, 0},
  {'i', FLAG_OPTION, GRX_OPT_CASELESS, 0, 0},
  {'m', FLAG_OPTION, GRX_OPT_MULTILINE, 0, 0},
  {'s', FLAG_OPTION, GRX_OPT_DOTALL, 0, 0},
  {'u', FLAG_OPTION, GRX_OPT_UTF, 1, 0},
  {'v', FLAG_OPTION, GRX_OPT_UNICODE_SETS | GRX_OPT_UTF, 1, 0},
  {'y', FLAG_SEARCH, 0, 0, 0},
  {0, FLAG_OPTION, 0, 0, 0},
};

static const FlagRow pcre_flags[] = {
  {'i', FLAG_OPTION, GRX_OPT_CASELESS, 0, 0},
  {'m', FLAG_OPTION, GRX_OPT_MULTILINE, 0, 0},
  {'s', FLAG_OPTION, GRX_OPT_DOTALL, 0, 0},
  {'x', FLAG_OPTION, GRX_OPT_EXTENDED, 0, GRX_OPT_EXTENDED_MORE},
  {'n', FLAG_OPTION, GRX_OPT_NO_CAPTURE, 0, 0},
  {'U', FLAG_OPTION, GRX_OPT_UNGREEDY, 0, 0},
  {'J', FLAG_OPTION, GRX_OPT_DUPLICATE_NAMES, 0, 0},
  // PCRE2's caseless-restrict, which is where it keeps what Perl spells
  // `/aa`. It is the *fold* half alone: `(?r)\d` still takes U+0661 and
  // `(?r)\w` still takes U+00E9, where Perl's `/aa` narrows the classes
  // too, because `/aa` is the letter `a` twice and carries `a`'s meaning
  // with it. So the two dialects reach one option by different routes and
  // `r` is not in perl_flags - perl has no such letter.
  //
  // Measured against pcre2test 10.46 under UTF and UCP: `(?ri)k` does not
  // match U+212A where `(?i)k` does, still matches "K", and still folds
  // U+00C0 with U+00E0 - which is GRX_OPT_ASCII_FOLD_SEPARATE exactly, and
  // not "fold ASCII only". It scopes and negates like any other flag:
  // `(?r:k)` narrows the group, `(?:(?r))k` narrows nothing outside it,
  // and `(?-r)` turns it back on.
  {'r', FLAG_OPTION, GRX_OPT_ASCII_FOLD_SEPARATE, 0, 0},
  {0, FLAG_OPTION, 0, 0, 0},
};

static const FlagRow perl_flags[] = {
  {'m', FLAG_OPTION, GRX_OPT_MULTILINE, 0, 0},
  {'s', FLAG_OPTION, GRX_OPT_DOTALL, 0, 0},
  {'i', FLAG_OPTION, GRX_OPT_CASELESS, 0, 0},
  {'x', FLAG_OPTION, GRX_OPT_EXTENDED, 0, GRX_OPT_EXTENDED_MORE},
  {'n', FLAG_OPTION, GRX_OPT_NO_CAPTURE, 0, 0},
  {'p', FLAG_NO_EFFECT, 0, 0, 0}, // Preserve the match; a search-API concern.
  // The four charset modifiers, which choose one character-set semantics
  // and so exclude each other. `a` is the only letter in any alphabet that
  // may appear twice and mean a third thing: `/aa` also stops caseless
  // matching folding across the ASCII boundary, which `/a` alone allows.
  // `l` asks for the locale's semantics and gets the C locale's, which is
  // the only locale this library has - documentation/dialects.md section 6.
  {'a', FLAG_OPTION, GRX_OPT_ASCII_CLASSES, 2, GRX_OPT_ASCII_FOLD_SEPARATE},
  {'l', FLAG_OPTION, GRX_OPT_ASCII_CLASSES, 2, 0},
  {'u', FLAG_OPTION, GRX_OPT_UTF, 2, 0},
  {'d', FLAG_NO_EFFECT, 0, 2, 0}, // The dialect's own default semantics.
  {0, FLAG_OPTION, 0, 0, 0},
};

static const FlagRow python_flags[] = {
  // `a` and `u` are one choice written two ways, so they share a group:
  // `re.compile(p, re.A | re.U)` is "ASCII and UNICODE flags are
  // incompatible". `u` sets nothing because Unicode is the dialect's
  // default (see its default_options); it is here so that writing it is
  // accepted and so that `au` is caught as the conflict it is.
  {'a', FLAG_OPTION, GRX_OPT_ASCII_CLASSES, 1, 0}, // re.ASCII.
  {'i', FLAG_OPTION, GRX_OPT_CASELESS, 0, 0},
  {'L', FLAG_UNSUPPORTED, 0, 0, 0}, // re.LOCALE; there is no locale here.
  {'m', FLAG_OPTION, GRX_OPT_MULTILINE, 0, 0},
  {'s', FLAG_OPTION, GRX_OPT_DOTALL, 0, 0},
  {'u', FLAG_NO_EFFECT, 0, 1, 0}, // re.UNICODE; already the default.
  {'x', FLAG_OPTION, GRX_OPT_EXTENDED, 0, 0},
  {0, FLAG_OPTION, 0, 0, 0},
};

/** The alphabet of a dialect, or NULL when it has none. */
static const FlagRow * flag_alphabet(GRX_Syntax syntax) {
  switch (syntax) {
    case GRX_SYNTAX_ECMASCRIPT:
      return ecmascript_flags;
    case GRX_SYNTAX_PCRE:
      return pcre_flags;
    case GRX_SYNTAX_PERL:
      return perl_flags;
    case GRX_SYNTAX_PYTHON:
      return python_flags;
    default:
      // POSIX and GNU have no flag letters at all: their options are API
      // arguments (REG_ICASE, REG_NEWLINE). An empty string is still valid
      // for them, and any letter is unknown.
      return NULL;
  }
}

GRX_Result grx_options_parse(GRX_Syntax syntax, const char * flags,
    uint32_t * out_options, GRX_Error * out_error) {
  if (!flags || !out_options || (unsigned)syntax >= (unsigned)GRX_SYNTAX_COUNT) {
    return GRX_ERR_INVALID;
  }
  grx_error_clear(out_error);
  *out_options = 0;

  const FlagRow * alphabet = flag_alphabet(syntax);
  uint32_t options = 0;
  char seen[256] = {0};
  uint32_t groups_used = 0;

  for (size_t i = 0; flags[i]; i++) {
    unsigned char letter = (unsigned char)flags[i];

    const FlagRow * row = NULL;
    for (const FlagRow * candidate = alphabet;
        candidate && candidate->letter; candidate++) {
      if (candidate->letter == (char)letter) {
        row = candidate;
        break;
      }
    }
    if (!row) {
      return grx_error_set(
          out_error, GRX_ERR_SYNTAX, GRX_DIAG_UNKNOWN_FLAG, i, 1);
    }

    if (seen[letter]) {
      // A second occurrence is a mistake unless the row says otherwise, and
      // a third is a mistake even then.
      if (!row->repeat_options || seen[letter] > 1) {
        return grx_error_set(
            out_error, GRX_ERR_SYNTAX, GRX_DIAG_DUPLICATE_FLAG, i, 1);
      }
      seen[letter]++;
      options |= row->repeat_options;
      continue;
    }
    seen[letter] = 1;

    switch (row->kind) {
      case FLAG_SEARCH:
        return grx_error_set(out_error, GRX_ERR_SYNTAX,
            GRX_DIAG_SEARCH_FLAG_IN_PATTERN, i, 1);
      case FLAG_UNSUPPORTED:
        return grx_error_set(out_error, GRX_ERR_UNSUPPORTED,
            GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, i, 1);
      case FLAG_NO_EFFECT:
        break;
      case FLAG_OPTION:
      default:
        if (row->group && (groups_used & (1u << (row->group - 1)))) {
          return grx_error_set(out_error, GRX_ERR_SYNTAX,
              GRX_DIAG_CONFLICTING_FLAGS, i, 1);
        }
        if (row->group) {
          groups_used |= 1u << (row->group - 1);
        }
        options |= row->options;
        break;
    }
  }

  *out_options = options;
  return GRX_OK;
}
