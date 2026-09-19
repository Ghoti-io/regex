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
