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
 *
 * Copyright 2026 by Corey Pennycuff
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

// One row per GRX_Syntax constant. Designated initialisers, so the rows may
// be reordered without breaking and a row that is simply missing is zero
// rather than someone else's - which no compiler can catch, and which
// EveryDialectHasASpec in tests/unit/test_syntax.cpp exists to catch instead.
static const GRX_SyntaxSpec spec_table[GRX_SYNTAX_COUNT] = {
  [GRX_SYNTAX_POSIX_BRE] = {
    .features = REP | BREF | PCLS,
    .escaped_specials = 1,
  },
  [GRX_SYNTAX_POSIX_ERE] = {
    // No backreference: POSIX leaves them out of the extended syntax, which
    // is the one difference people are most often surprised by.
    .features = ALT | REP | PCLS,
  },
  [GRX_SYNTAX_GNU_BRE] = {
    .features = ALT | REP | BREF | PCLS | WORD | ANCH,
    .escaped_specials = 1,
  },
  [GRX_SYNTAX_GNU_ERE] = {
    .features = ALT | REP | BREF | PCLS | WORD | ANCH,
  },
  [GRX_SYNTAX_PERL] = {
    .features = ALT | REP | LAZY | POSS | NCAP | NAME | BREF | LAH | LBH
        | ATOM | COND | RECU | FLAG | CMNT | PCLS | UPRP | WORD | ANCH | QUOT
        | HEX | OCT | CTRL | SUBR,
    // documentation/dialects.md section 5.15: Perl's subject is a Unicode
    // string, so UTF is on unless the caller turns it off. The field had
    // been empty since the table was written, and what it cost was visible
    // only once there was a Perl front end to read it: `\N{U+0100}` is
    // refused outside UTF mode, and `\400` is one code point in UTF and a
    // byte that cannot be one without it.
    .default_options = GRX_OPT_UTF,
  },
  [GRX_SYNTAX_PCRE] = {
    .features = ALT | REP | LAZY | POSS | NCAP | NAME | BREF | LAH | LBH
        | ATOM | COND | RECU | FLAG | CMNT | PCLS | UPRP | WORD | ANCH | QUOT
        | HEX | OCT | CTRL | SUBR | VERB,
  },
  [GRX_SYNTAX_ECMASCRIPT] = {
    // No inline flags: ECMAScript puts them after the closing delimiter, not
    // inside the pattern, so `(?i)` is a syntax error rather than a flag.
    .features = ALT | REP | LAZY | NCAP | NAME | BREF | LAH | LBH | UPRP
        | CSET | WORD | HEX | CTRL,
    // The only tier-1 dialect where `[]` is an empty class rather than a
    // class containing `]`. Read by the prescan as well as by the front end:
    // `(?2)[]a()b](abc)` has one capturing group in PCRE2 and two in
    // ECMAScript, and a prescan that guessed would refuse a valid pattern.
    .allow_empty_class = 1,
  },
  [GRX_SYNTAX_PYTHON] = {
    .features = ALT | REP | LAZY | POSS | NCAP | NAME | BREF | LAH | LBH
        | ATOM | COND | FLAG | CMNT | WORD | ANCH | HEX | OCT,
  },
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
    .features = ALT | REP | LAZY | NCAP | BREF | LAH | LBH | ATOM | PCLS
        | WORD | ANCH | HEX,
    .escaped_specials = 1,
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
};

/**
 * The semantic profile of each dialect: what the constructs mean.
 *
 * Status: ECMAScript's row is filled from ECMA-262 and checked against Node
 * 22; every other row holds the value documentation/dialects.md section 5
 * states, and the cells that page marks **probe** are resolved by
 * documentation/plan.md WP-03 before code depends on them. A zeroed field is
 * the first value of its enum, and for every enum here that is the value the
 * Perl family takes - so a dialect whose row is not yet written behaves as
 * Perl rather than as nothing, which is the failure a reader can see.
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
    .empty_loop = GRX_EMPTY_LOOP_ALLOW,
    .backref_unset = GRX_BACKREF_UNSET_FAILS,
    .lookbehind = GRX_LOOKBEHIND_NONE,
    .dollar = GRX_DOLLAR_END_ONLY,
    .newlines = GRX_NEWLINES_NONE,
    .fold = GRX_FOLD_ASCII,
    .fold_utf = GRX_FOLD_ASCII,
  },
  [GRX_SYNTAX_POSIX_ERE] = {
    .preference = GRX_PREFER_LEFTMOST_LONGEST,
    .empty_loop = GRX_EMPTY_LOOP_ALLOW,
    .backref_unset = GRX_BACKREF_UNSET_FAILS,
    .lookbehind = GRX_LOOKBEHIND_NONE,
    .dollar = GRX_DOLLAR_END_ONLY,
    .newlines = GRX_NEWLINES_NONE,
    .fold = GRX_FOLD_ASCII,
    .fold_utf = GRX_FOLD_ASCII,
  },
  [GRX_SYNTAX_GNU_BRE] = {
    .preference = GRX_PREFER_LEFTMOST_LONGEST,
    .empty_loop = GRX_EMPTY_LOOP_ALLOW,
    .backref_unset = GRX_BACKREF_UNSET_FAILS,
    .lookbehind = GRX_LOOKBEHIND_NONE,
    .dollar = GRX_DOLLAR_END_ONLY,
    .newlines = GRX_NEWLINES_NONE,
    .fold = GRX_FOLD_ASCII,
    .fold_utf = GRX_FOLD_ASCII,
  },
  [GRX_SYNTAX_GNU_ERE] = {
    .preference = GRX_PREFER_LEFTMOST_LONGEST,
    .empty_loop = GRX_EMPTY_LOOP_ALLOW,
    .backref_unset = GRX_BACKREF_UNSET_FAILS,
    .lookbehind = GRX_LOOKBEHIND_NONE,
    .dollar = GRX_DOLLAR_END_ONLY,
    .newlines = GRX_NEWLINES_NONE,
    .fold = GRX_FOLD_ASCII,
    .fold_utf = GRX_FOLD_ASCII,
  },

  // The Perl family. Full folding is implemented as simple folding and
  // recorded as a deviation (design.md section 10).
  [GRX_SYNTAX_PERL] = {
    // RESET_EACH, not KEEP_LAST_SET: Perl 5.40 reports group 2 of
    // `((a)|b)+` against "ab" as unset, where PCRE2 and Python report "a".
    // Probed rather than read; see tests/data/probe/report.md and
    // documentation/dialects.md section 5.5.
    .capture_reset = GRX_CAPTURE_RESET_EACH,
    .lookbehind = GRX_LOOKBEHIND_BOUNDED,
    .dollar = GRX_DOLLAR_BEFORE_FINAL_NEWLINE,
    .shorthands = GRX_SHORTHANDS_UNICODE,
    .shorthands_utf = GRX_SHORTHANDS_UNICODE,
    .fold = GRX_FOLD_SIMPLE,
    .fold_utf = GRX_FOLD_SIMPLE,
    .property_match = GRX_PROPERTY_LOOSE_PERL,
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
      .features = GRX_TMPL_NUMBER | GRX_TMPL_NUMBER_BRACED
          | GRX_TMPL_NAME_PLUS_BRACE | GRX_TMPL_WHOLE | GRX_TMPL_PREFIX
          | GRX_TMPL_SUFFIX | GRX_TMPL_BACKSLASH_ESCAPE,
      .missing = GRX_TMPL_MISSING_EMPTY,
    },
  },
  [GRX_SYNTAX_PCRE] = {
    .recursion_is_atomic = 1,
    .lookbehind = GRX_LOOKBEHIND_BOUNDED,
    .dollar = GRX_DOLLAR_BEFORE_FINAL_NEWLINE,
    .shorthands = GRX_SHORTHANDS_ASCII,
    .shorthands_utf = GRX_SHORTHANDS_UNICODE,
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
      .features = GRX_TMPL_NUMBER | GRX_TMPL_NUMBER_BRACED
          | GRX_TMPL_NAME_BRACED | GRX_TMPL_NAME_BARE
          | GRX_TMPL_DOUBLE_SIGIL,
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
    .shorthands_utf = GRX_SHORTHANDS_ECMASCRIPT,
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

  [GRX_SYNTAX_PYTHON] = {
    .caret_after_final_newline = 1,
    .lookbehind = GRX_LOOKBEHIND_FIXED,
    .dollar = GRX_DOLLAR_BEFORE_FINAL_NEWLINE,
    .shorthands = GRX_SHORTHANDS_UNICODE,
    .shorthands_utf = GRX_SHORTHANDS_UNICODE,
    .fold = GRX_FOLD_SIMPLE,
    .fold_utf = GRX_FOLD_SIMPLE,
  },
  [GRX_SYNTAX_JAVA] = {
    .lookbehind = GRX_LOOKBEHIND_BOUNDED,
    .dollar = GRX_DOLLAR_BEFORE_FINAL_NEWLINE,
    .newlines = GRX_NEWLINES_UNICODE,
    .shorthands = GRX_SHORTHANDS_ASCII,
    .shorthands_utf = GRX_SHORTHANDS_UNICODE,
    .fold = GRX_FOLD_ASCII,
    .fold_utf = GRX_FOLD_SIMPLE,
    .property_match = GRX_PROPERTY_LOOSE,
  },
  [GRX_SYNTAX_DOTNET] = {
    .lookbehind = GRX_LOOKBEHIND_UNBOUNDED,
    .iteration = GRX_ITERATE_ADVANCE_ONE,
    .dollar = GRX_DOLLAR_BEFORE_FINAL_NEWLINE,
    .shorthands = GRX_SHORTHANDS_UNICODE,
    .shorthands_utf = GRX_SHORTHANDS_UNICODE,
    .fold = GRX_FOLD_SIMPLE,
    .fold_utf = GRX_FOLD_SIMPLE,
  },
  [GRX_SYNTAX_RUBY] = {
    .lookbehind = GRX_LOOKBEHIND_FIXED_PER_BRANCH,
    .iteration = GRX_ITERATE_ADVANCE_ONE,
    .dollar = GRX_DOLLAR_ALWAYS_LINE,
    .multiline_by_default = 1,
    .shorthands = GRX_SHORTHANDS_ASCII,
    .shorthands_utf = GRX_SHORTHANDS_ASCII,
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
    .shorthands_utf = GRX_SHORTHANDS_ASCII,
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
    .shorthands_utf = GRX_SHORTHANDS_UNICODE,
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
    .shorthands_utf = GRX_SHORTHANDS_UNICODE,
    .fold = GRX_FOLD_SIMPLE,
    .fold_utf = GRX_FOLD_SIMPLE,
  },
  [GRX_SYNTAX_VIM] = {
    .lookbehind = GRX_LOOKBEHIND_UNBOUNDED,
    .dollar = GRX_DOLLAR_ALWAYS_LINE,
    .multiline_by_default = 1,
    .shorthands = GRX_SHORTHANDS_ASCII,
    .shorthands_utf = GRX_SHORTHANDS_ASCII,
    .fold = GRX_FOLD_ASCII,
    .fold_utf = GRX_FOLD_ASCII,
  },
  [GRX_SYNTAX_EMACS] = {
    .lookbehind = GRX_LOOKBEHIND_NONE,
    .dollar = GRX_DOLLAR_ALWAYS_LINE,
    .multiline_by_default = 1,
    .shorthands = GRX_SHORTHANDS_UNICODE,
    .shorthands_utf = GRX_SHORTHANDS_UNICODE,
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
  {0, FLAG_OPTION, 0, 0, 0},
};

static const FlagRow perl_flags[] = {
  {'m', FLAG_OPTION, GRX_OPT_MULTILINE, 0, 0},
  {'s', FLAG_OPTION, GRX_OPT_DOTALL, 0, 0},
  {'i', FLAG_OPTION, GRX_OPT_CASELESS, 0, 0},
  {'x', FLAG_OPTION, GRX_OPT_EXTENDED, 0, GRX_OPT_EXTENDED_MORE},
  {'n', FLAG_OPTION, GRX_OPT_NO_CAPTURE, 0, 0},
  {'p', FLAG_NO_EFFECT, 0, 0, 0}, // Preserve the match; a search-API concern.
  {'a', FLAG_UNSUPPORTED, 0, 0, 0}, // ASCII-restrict; WP-21.
  {'u', FLAG_OPTION, GRX_OPT_UTF, 0, 0},
  {0, FLAG_OPTION, 0, 0, 0},
};

static const FlagRow python_flags[] = {
  {'a', FLAG_UNSUPPORTED, 0, 0, 0}, // re.ASCII; WP-30.
  {'i', FLAG_OPTION, GRX_OPT_CASELESS, 0, 0},
  {'L', FLAG_UNSUPPORTED, 0, 0, 0}, // re.LOCALE; there is no locale here.
  {'m', FLAG_OPTION, GRX_OPT_MULTILINE, 0, 0},
  {'s', FLAG_OPTION, GRX_OPT_DOTALL, 0, 0},
  {'u', FLAG_OPTION, GRX_OPT_UTF, 0, 0},
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
