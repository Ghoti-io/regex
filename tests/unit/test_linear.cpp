/**
 * @file
 *
 * The two guaranteed-linear dialects: RE2 (Go's `regexp`) and Rust's `regex`.
 *
 * The authority for every expectation here is one of the two pinned
 * references in `tools/oracle/containers/IMAGES`, asked through
 * `tools/oracle/linear_diff.py`, and the committed answers are in
 * `tests/data/vectors/re2/` and `tests/data/vectors/rust/`. So this file is
 * deliberately *not* a second corpus: the vectors carry the per-row
 * behaviour, and what is here is the two things a row cannot say.
 *
 * **The guarantee.** These dialects exist because a pattern they accept is
 * regular - no backreference, no lookaround - which makes them the only two
 * dialects in this library where a refusal is a promise rather than a
 * limitation. A vector can record that `(a)\1` is refused; it cannot record
 * that every accepted pattern runs on the linear engine, and that is the
 * property the dialect is for.
 *
 * **The seam between the two.** src/syntax/re2.c reads both, deny-by-default,
 * with a two-valued `flavour()`; twelve constructs belong to exactly one of
 * them. Every one of those is a branch whose *other* side is what a
 * single-dialect test would leave unasked, so they are asked here as pairs -
 * accepted under one dialect and refused under the other, in one assertion.
 * That is the shape that would have caught the defect linear_diff.py found
 * in `shorthand_for()`, where `\S` built a class matching nothing.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <string>
#include <vector>

#include "test_helpers.h"

namespace {

GRX_Result compile_with(const std::string & pattern, GRX_Syntax syntax,
    uint32_t options, GRX_Diag * out_diag, GRX_Regex ** out_regex) {
  GRX_Error error;
  grx_error_clear(&error);
  GRX_Regex * regex = nullptr;
  GRX_Result result = grx_regex_compile_with_allocator(pattern.data(),
      pattern.size(), syntax, options, nullptr, nullptr, &error, &regex);
  if (out_diag) {
    *out_diag = error.diag;
  }
  if (out_regex) {
    *out_regex = regex;
  }
  else {
    grx_regex_free(regex);
  }
  return result;
}

/** "ok" or "refused", so a failure message says which way it went. */
std::string accepts(const std::string & pattern, GRX_Syntax syntax,
    uint32_t options = 0) {
  return compile_with(pattern, syntax, options, nullptr, nullptr) == GRX_OK
      ? "ok"
      : "refused";
}

std::string re2(const std::string & pattern, uint32_t options = 0) {
  return accepts(pattern, GRX_SYNTAX_RE2, options);
}

std::string rust(const std::string & pattern, uint32_t options = 0) {
  return accepts(pattern, GRX_SYNTAX_RUST, options);
}

GRX_Diag why(const std::string & pattern, GRX_Syntax syntax) {
  GRX_Diag diag = GRX_DIAG_NONE;
  compile_with(pattern, syntax, 0, &diag, nullptr);
  return diag;
}

/**
 * The match, as `start-end` per group, or "nomatch" / "refused".
 *
 * The engine is a parameter because one of the cells below is a claim about
 * all three of them rather than about the answer: see
 * GRX_PROGRAM_SIMULATED_LOOP.
 */
std::string search_on(const std::string & pattern, const std::string & subject,
    GRX_Syntax syntax, uint32_t options, GRX_Engine engine) {
  GRX_Regex * regex = nullptr;
  if (compile_with(pattern, syntax, options, nullptr, &regex) != GRX_OK) {
    grx_regex_free(regex);
    return "refused";
  }
  GRX_Match * match = nullptr;
  grx_match_create(regex, nullptr, &match);
  int matched = 0;
  std::string answer = "error";
  if (grx_regex_search(regex, subject.data(), subject.size(), 0,
          engine, nullptr, match, &matched)
      == GRX_OK) {
    if (!matched) {
      answer = "nomatch";
    }
    else {
      answer.clear();
      size_t count = grx_match_count(match);
      for (size_t i = 0; i < count; i++) {
        GRX_Capture capture;
        if (!answer.empty()) {
          answer += " ";
        }
        if (grx_match_group(match, i, &capture) == GRX_OK
            && capture.start != GRX_NPOS) {
          answer += std::to_string(capture.start) + "-"
              + std::to_string(capture.end);
        }
        else {
          answer += "-";
        }
      }
    }
  }
  grx_match_destroy(match);
  grx_regex_free(regex);
  return answer;
}

std::string search(const std::string & pattern, const std::string & subject,
    GRX_Syntax syntax, uint32_t options = 0) {
  return search_on(pattern, subject, syntax, options, GRX_ENGINE_AUTO);
}

/** `grx_regex_replace()`'s answer, or "refused" / "template". */
std::string replaced(const std::string & pattern, const std::string & subject,
    const std::string & template_text, GRX_Syntax syntax,
    uint32_t options = 0) {
  GRX_Regex * regex = nullptr;
  if (compile_with(pattern, syntax, options, nullptr, &regex) != GRX_OK) {
    grx_regex_free(regex);
    return "refused";
  }
  GRX_Text text = {nullptr, 0, nullptr};
  GRX_Result result = grx_regex_replace(regex, subject.data(), subject.size(),
      template_text.data(), template_text.size(), GRX_REPLACE_GLOBAL, nullptr,
      nullptr, nullptr, &text);
  std::string answer = result == GRX_OK
      ? std::string(text.data ? text.data : "", text.length)
      : (result == GRX_ERR_UNSUPPORTED ? "unsupported" : "template");
  grx_text_free(&text);
  grx_regex_free(regex);
  return answer;
}

/** `grx_regex_split()`'s pieces as `a|b|c`, with `-` for an unset one. */
std::string split_into(const std::string & pattern,
    const std::string & subject, size_t limit, GRX_Syntax syntax,
    uint32_t options = 0) {
  GRX_Regex * regex = nullptr;
  if (compile_with(pattern, syntax, options, nullptr, &regex) != GRX_OK) {
    grx_regex_free(regex);
    return "refused";
  }
  GRX_Split split = {nullptr, 0, nullptr};
  std::string answer = "error";
  if (grx_regex_split(regex, subject.data(), subject.size(), limit, nullptr,
          nullptr, nullptr, &split)
      == GRX_OK) {
    // The count leads, because "no pieces" and "one empty piece" are the two
    // answers a limit rule exists to tell apart and they print the same.
    answer = std::to_string(split.count);
    for (size_t i = 0; i < split.count; i++) {
      answer += i ? "|" : " ";
      answer += split.pieces[i].start == GRX_NPOS
          ? "-"
          : subject.substr(split.pieces[i].start,
                split.pieces[i].end - split.pieces[i].start);
    }
  }
  grx_split_free(&split);
  grx_regex_free(regex);
  return answer;
}

/** Every match of a search-all loop as `start-end`, space separated. */
std::string all_matches(const std::string & pattern,
    const std::string & subject, GRX_Syntax syntax, uint32_t options = 0) {
  GRX_Regex * regex = nullptr;
  if (compile_with(pattern, syntax, options, nullptr, &regex) != GRX_OK) {
    grx_regex_free(regex);
    return "refused";
  }
  GRX_Match * match = nullptr;
  grx_match_create(regex, nullptr, &match);
  std::string answer;
  int matched = 0;
  GRX_Result result = grx_regex_search(regex, subject.data(), subject.size(),
      0, GRX_ENGINE_AUTO, nullptr, match, &matched);
  while (result == GRX_OK && matched) {
    GRX_Capture whole;
    if (grx_match_span(match, &whole) != GRX_OK) {
      answer = "error";
      break;
    }
    if (!answer.empty()) {
      answer += " ";
    }
    answer += std::to_string(whole.start) + "-" + std::to_string(whole.end);
    result = grx_regex_search_next(regex, subject.data(), subject.size(),
        nullptr, match, &matched);
  }
  grx_match_destroy(match);
  grx_regex_free(regex);
  return answer.empty() ? "none" : answer;
}

/**
 * Whether the linear engine will run this pattern, asked by name.
 *
 * `GRX_ENGINE_PIKE` answers GRX_ERR_UNSUPPORTED for a program it cannot run
 * (README, the engine table), so asking for it by name is how the guarantee
 * is tested rather than asserted. `GRX_ENGINE_AUTO` would pick the
 * backtracker and report a match either way, which is exactly the answer
 * that would hide a broken promise.
 */
bool runs_linear(const std::string & pattern, GRX_Syntax syntax) {
  GRX_Regex * regex = nullptr;
  if (compile_with(pattern, syntax, 0, nullptr, &regex) != GRX_OK) {
    grx_regex_free(regex);
    return false;
  }
  GRX_Match * match = nullptr;
  grx_match_create(regex, nullptr, &match);
  int matched = 0;
  GRX_Result result = grx_regex_search(regex, "abcabc", 6, 0,
      GRX_ENGINE_PIKE, nullptr, match, &matched);
  grx_match_destroy(match);
  grx_regex_free(regex);
  return result == GRX_OK;
}

} // namespace

TEST(Linear, BothDialectsAreNamedAndBuilt) {
  EXPECT_STREQ(grx_syntax_name(GRX_SYNTAX_RE2), "re2");
  EXPECT_STREQ(grx_syntax_name(GRX_SYNTAX_RUST), "rust");

  GRX_Syntax syntax = GRX_SYNTAX_COUNT;
  EXPECT_EQ(grx_syntax_from_name("re2", &syntax), GRX_OK);
  EXPECT_EQ(syntax, GRX_SYNTAX_RE2);
  EXPECT_EQ(grx_syntax_from_name("RUST", &syntax), GRX_OK);
  EXPECT_EQ(syntax, GRX_SYNTAX_RUST);

  // Built, not merely named: a dialect with no front end answers
  // GRX_ERR_UNSUPPORTED with GRX_DIAG_DIALECT_NOT_IMPLEMENTED, and that is
  // what these two did before src/syntax/re2.c existed.
  EXPECT_EQ(re2("a"), "ok");
  EXPECT_EQ(rust("a"), "ok");
  EXPECT_NE(why("a", GRX_SYNTAX_RE2), GRX_DIAG_DIALECT_NOT_IMPLEMENTED);
  EXPECT_NE(why("a", GRX_SYNTAX_RUST), GRX_DIAG_DIALECT_NOT_IMPLEMENTED);
}

TEST(Linear, EveryPatternTheseDialectsAcceptRunsOnTheLinearEngine) {
  // The property both dialects exist for, and the one thing a vector file
  // cannot record. Asked of the constructs most likely to compile into
  // something the Pike VM refuses - a quantified group, a nested repeat, an
  // alternation of unequal branches, a class with a property in it - rather
  // than of `a`.
  static const char * const patterns[] = {
    "a", "(a|ab)(c|bcd)", "(a*)*", "(?:(a)|b){2}", "a{2,5}?",
    "(?P<n>[[:alpha:]]+)\\s*\\p{Nd}*", "^(a|b)*$", "\\b\\w+\\b",
    "(a)(b)(c)(d)(e)", "[^a-z]{1,3}", "a|b|c|d|", "(?i)(abc|def)+",
  };
  for (const char * pattern : patterns) {
    EXPECT_TRUE(runs_linear(pattern, GRX_SYNTAX_RE2))
        << "re2 accepted /" << pattern << "/ and the Pike VM refused it";
    EXPECT_TRUE(runs_linear(pattern, GRX_SYNTAX_RUST))
        << "rust accepted /" << pattern << "/ and the Pike VM refused it";
  }
}

TEST(Linear, TheTwoConstructsThatWouldBreakTheGuaranteeAreRefused) {
  // Not "these happen to be absent": a backreference and a lookaround are
  // the two constructs that cannot be run in linear time, and refusing them
  // is the whole of what makes the test above true. Every spelling either
  // reference has for them, so that a reader coming from another dialect
  // cannot reach one by writing it differently.
  static const char * const patterns[] = {
    "(a)\\1", "\\1", "\\g{1}", "\\g1", "\\k<n>", "(?P=n)",
    "(?=a)", "(?!a)", "(?<=a)", "(?<!a)", "(?>a)", "(*atomic:a)",
    "(?(1)a|b)", "(?1)", "(?&n)", "(?P>n)",
  };
  for (const char * pattern : patterns) {
    EXPECT_EQ(re2(pattern), "refused") << pattern;
    EXPECT_EQ(rust(pattern), "refused") << pattern;
  }
  // `(?R)` is recursion in the Perl family and so belongs on that list for
  // RE2 - but in the crate the same two characters are the CRLF *flag*, so
  // it is not a refusal there and asserting one would be asserting the
  // wrong dialect's rule.
  EXPECT_EQ(re2("(?R)"), "refused");
}

TEST(Linear, TwelveConstructsBelongToExactlyOneOfTheTwo) {
  // Each row is a branch of `flavour()` in src/syntax/re2.c, and the pair is
  // what makes it a gate: a reader that lost the flavour test would accept
  // both sides, and a test that asked only the dialect that has the
  // construct would not notice.
  //
  // Every row was probed against both pinned references before it was
  // written here; `make check-oracle-linear` re-asks them.
  struct Row {
    const char * pattern;
    const char * in_re2;
    const char * in_rust;
  };
  static const Row rows[] = {
    {"\\Qa+b\\E", "ok", "refused"},   // Quoting is RE2's.
    {"\\101", "ok", "refused"},       // Octal is RE2's; the crate has none.
    {"\\0", "ok", "refused"},
    {"\\p{^L}", "ok", "refused"},     // In-brace negation is RE2's.
    {"a{,3}", "ok", "refused"},       // A `{` that begins no quantifier.
    {"a{2,", "ok", "refused"},
    {"{", "ok", "refused"},
    {"[\\d-x]", "ok", "refused"},     // A class escape at the low end.
    {"(?P<n>a)(?P<n>b)", "ok", "refused"},  // Duplicate names.
    {"(?P<1a>a)", "ok", "refused"},   // A name starting with a digit.
    {"a{1001}", "refused", "ok"},     // RE2's thousand-copy ceiling.
    {"(?x) a  b", "refused", "ok"},   // Extended mode is the crate's.
    {"\\u0041", "refused", "ok"},     // `\u` and `\U` are the crate's.
    {"\\U0001F600", "refused", "ok"},
    {"a**", "refused", "ok"},         // A second quantifier.
    {"\\p{Script=Greek}", "refused", "ok"},  // A qualified property name.
    {"[[:nosuch:]]", "refused", "ok"},  // The crate falls back to members.
  };
  for (const Row & row : rows) {
    EXPECT_EQ(re2(row.pattern), row.in_re2) << "re2 /" << row.pattern << "/";
    EXPECT_EQ(rust(row.pattern), row.in_rust)
        << "rust /" << row.pattern << "/";
  }
}

TEST(Linear, FourConstructsAreLegalInBothAndMeanDifferentThings) {
  // The trap in a table of accept-and-refuse: a construct one dialect has
  // is not always *refused* by the other - sometimes it parses as something
  // else, and that is worse, because a caller gets an answer rather than an
  // error. Each of these compiles under RE2 and denotes a different
  // language there; each was probed against both references.
  for (const char * pattern :
      {"[a-z--[aeiou]]", "[\\w&&\\d]", "[ab~~bc]", "\\b{start}"}) {
    EXPECT_EQ(re2(pattern), "ok") << pattern;
    EXPECT_EQ(rust(pattern), "ok") << pattern;
  }

  // The set operators are operators in the crate and ordinary members in
  // RE2, so `[\w&&\d]` is the digits there and `\w`, `&` and the digits
  // here.
  EXPECT_EQ(search("[\\w&&\\d]", "x", GRX_SYNTAX_RE2), "0-1");
  EXPECT_EQ(search("[\\w&&\\d]", "x", GRX_SYNTAX_RUST), "nomatch");
  EXPECT_EQ(search("[\\w&&\\d]", "&", GRX_SYNTAX_RE2), "0-1");
  EXPECT_EQ(search("[ab~~bc]", "b", GRX_SYNTAX_RE2), "0-1");
  EXPECT_EQ(search("[ab~~bc]", "b", GRX_SYNTAX_RUST), "nomatch");
  EXPECT_EQ(search("[a-z--[aeiou]]", "e", GRX_SYNTAX_RUST), "nomatch");

  // `\b{start}` is an assertion in the crate and a boundary followed by
  // seven literal characters in RE2.
  EXPECT_EQ(search("\\b{start}a", "a", GRX_SYNTAX_RUST), "0-1");
  EXPECT_EQ(search("\\b{start}a", "a", GRX_SYNTAX_RE2), "nomatch");
}

TEST(Linear, ARefusedConstructSaysItIsNotInTheDialect) {
  // The diagnostic matters here more than in most dialects, because these
  // two refuse constructs that *exist* elsewhere in this library rather than
  // constructs that are malformed. A caller who wrote `(?=a)` has written a
  // real lookahead; telling them the `?` has nothing to repeat would send
  // them looking for the wrong thing.
  EXPECT_EQ(why("(?=a)", GRX_SYNTAX_RE2), GRX_DIAG_NOT_IN_DIALECT);
  EXPECT_EQ(why("(?<=a)", GRX_SYNTAX_RE2), GRX_DIAG_NOT_IN_DIALECT);
  EXPECT_EQ(why("(?>a)", GRX_SYNTAX_RE2), GRX_DIAG_NOT_IN_DIALECT);
  EXPECT_EQ(why("(?#c)", GRX_SYNTAX_RE2), GRX_DIAG_NOT_IN_DIALECT);
  EXPECT_EQ(why("\\1", GRX_SYNTAX_RE2), GRX_DIAG_NOT_IN_DIALECT);
  EXPECT_EQ(why("(?=a)", GRX_SYNTAX_RUST), GRX_DIAG_NOT_IN_DIALECT);
}

TEST(Linear, AnUnknownEscapeLetterIsRefusedAndPunctuationIsNot) {
  // The rule most likely to be got wrong by a reader written from the Perl
  // family, where an unknown letter is the letter itself. Both references
  // close the alphabet for letters and open it for everything else.
  for (GRX_Syntax syntax : {GRX_SYNTAX_RE2, GRX_SYNTAX_RUST}) {
    EXPECT_EQ(accepts("\\y", syntax), "refused");
    EXPECT_EQ(accepts("\\e", syntax), "refused");
    EXPECT_EQ(accepts("\\cA", syntax), "refused");
    EXPECT_EQ(accepts("\\Z", syntax), "refused");
    EXPECT_EQ(accepts("\\R", syntax), "refused");
    EXPECT_EQ(accepts("\\X", syntax), "refused");
    EXPECT_EQ(accepts("\\h", syntax), "refused");

    EXPECT_EQ(accepts("\\-", syntax), "ok");
    EXPECT_EQ(accepts("\\#", syntax), "ok");
    EXPECT_EQ(accepts("\\$", syntax), "ok");
    EXPECT_EQ(accepts("\\.", syntax), "ok");
  }
}

TEST(Linear, AnEmptyIterationRunsOnceAndACountedRepeatRunsAlways) {
  // The three measurements that look like an empty-iteration rule, and are
  // the ones a reader coming from the Perl family will reach for:
  //
  //   `(a*)*` over "b"      group 1 is 0-0, so an empty iteration runs when
  //                         nothing else has. FAIL would leave it unset.
  //   `(a|)*` over "aaaa"   group 1 is 3-4, so a trailing empty one does
  //                         not. BREAK would give 4-4.
  //   `(a|){1,2}` over "a"  group 1 is 1-1: a counted repeat is copies
  //                         rather than a loop, so no rule applies to it.
  //
  // All three still hold under GRX_EMPTY_LOOP_SIMULATE, which is why they
  // did not catch the cell the next test covers - they were satisfied by
  // BREAK_FIRST, and so was every row of two 105,000-row differentials bar
  // one.
  for (GRX_Syntax syntax : {GRX_SYNTAX_RE2, GRX_SYNTAX_RUST}) {
    EXPECT_EQ(search("(a*)*", "b", syntax), "0-0 0-0");
    EXPECT_EQ(search("(a|)*", "aaaa", syntax), "0-4 3-4");
    EXPECT_EQ(search("(a|){1,2}", "a", syntax), "0-1 1-1");
    EXPECT_EQ(search("(a|){1,3}", "aa", syntax), "0-2 2-2");
  }
}

TEST(Linear, ALoopOverAnEmptyBodyEndsWhereTheWalkHasAlreadyBeen) {
  // GRX_EMPTY_LOOP_SIMULATE, and the point of the mode is that none of the
  // three rules above gives these. A fresh iteration that begins where a
  // continuing one already stands is not allowed, broken or failed - it
  // arrives at a state the simulation has already reached and is dropped
  // there, so the continuing iteration is the one that finishes.
  //
  // `(a*)+b` under `U` is `(a*?)+?b`, and over "aab" group 1 is 0-2 in both
  // references: one iteration spanning the whole run. Under BREAK_FIRST
  // this library said 1-2, the last of two iterations, which is what perl,
  // CPython and this library's own backtracker all say. Both references
  // are automata and neither of them backtracks, so this is not a bug in
  // either - it is the semantics, and it is the one cell in this dialect
  // pair that a backtracking reading gets wrong.
  for (GRX_Syntax syntax : {GRX_SYNTAX_RE2, GRX_SYNTAX_RUST}) {
    EXPECT_EQ(search("(a*)+b", "aab", syntax, GRX_OPT_UNGREEDY), "0-3 0-2");
    EXPECT_EQ(search("(a*)*b", "aaab", syntax, GRX_OPT_UNGREEDY), "0-4 0-3");
    EXPECT_EQ(search("(a*?)+b", "aab", syntax), "0-3 0-2");
    EXPECT_EQ(search("((a)*)+b", "aab", syntax, GRX_OPT_UNGREEDY),
        "0-3 0-2 1-2");

    // The minimal pair, and each half agrees under either reading: take the
    // empty body away and the fresh iteration cannot begin where the
    // continuing one stands; take the laziness away and the continuing one
    // is preferred anyway. Neither alone would have found the cell, which
    // is why both are here.
    EXPECT_EQ(search("(a+)+b", "aab", syntax, GRX_OPT_UNGREEDY), "0-3 1-2");
    EXPECT_EQ(search("(a*)+b", "aab", syntax), "0-3 0-2");

    // And the overall extent moves too, so this is not only about which
    // division of a settled match the groups get: the dropped path is the
    // one that would have consumed further.
    EXPECT_EQ(search("(a*b*)+?b", "abab", syntax, GRX_OPT_UNGREEDY),
        "0-2 0-1");
    EXPECT_EQ(search("(a|b*)+?", "aabb", syntax, GRX_OPT_UNGREEDY),
        "0-3 2-3");
  }
}

TEST(Linear, TheThreeEnginesAgreeOnALoopTheSimulationEnds) {
  // The mode is computable by a Pike VM and by a backtracker with a memo,
  // and not by a backtracker without one: the path the simulation drops as
  // already-seen is a path a backtracker completes. So the program carries
  // GRX_PROGRAM_SIMULATED_LOOP and exec.c arms the visited bitmap for it
  // whichever backtracking engine runs it, rather than leaving it as the
  // optimisation it is everywhere else.
  //
  // Asking all three by name is the whole test. Without the flag,
  // GRX_ENGINE_BACKTRACK answered 347 of the shape battery's 5,040 rows
  // differently from the other two.
  const GRX_Engine engines[] = {
    GRX_ENGINE_PIKE, GRX_ENGINE_BITSTATE, GRX_ENGINE_BACKTRACK,
  };
  for (GRX_Syntax syntax : {GRX_SYNTAX_RE2, GRX_SYNTAX_RUST}) {
    for (GRX_Engine engine : engines) {
      EXPECT_EQ(search_on("(a*)*", "aa", syntax, 0, engine), "0-2 0-2")
          << "engine " << (int)engine;
      EXPECT_EQ(search_on("(a*)+b", "aab", syntax, GRX_OPT_UNGREEDY, engine),
          "0-3 0-2")
          << "engine " << (int)engine;
    }
  }
}

TEST(Linear, TheTemplateGrammarIsAWordRunAndNotANumberThenAName) {
  // GRX_TMPL_BARE_RUN_ONLY, which is the cell that separates this grammar
  // from PCRE2's. Both have `$` and both read a bare name; PCRE2 tries a
  // number first and falls back, so `$1a` is group 1 and an "a" there. Here
  // the run is read whole and asked what it is afterwards, so `$1a` is the
  // name "1a", which no pattern has.
  for (GRX_Syntax syntax : {GRX_SYNTAX_RE2, GRX_SYNTAX_RUST}) {
    EXPECT_EQ(replaced("(a)(b)", "zabz", "$1", syntax), "zaz");
    EXPECT_EQ(replaced("(a)(b)", "zabz", "${1}", syntax), "zaz");
    EXPECT_EQ(replaced("(a)(b)", "zabz", "$2", syntax), "zbz");
    EXPECT_EQ(replaced("(a)(b)", "zabz", "$0", syntax), "zabz");
    EXPECT_EQ(replaced("(a)(b)", "zabz", "${0}", syntax), "zabz");
    EXPECT_EQ(replaced("(a)(b)", "zabz", "$$", syntax), "z$z");
    EXPECT_EQ(replaced("(?P<n>a)(b)", "zabz", "$n", syntax), "zaz");
    EXPECT_EQ(replaced("(?P<n>a)(b)", "zabz", "${n}", syntax), "zaz");

    // The run, and the whole of it: `$1a` and `$12_` are names.
    EXPECT_EQ(replaced("(a)(b)", "zabz", "$1a", syntax), "zz");
    EXPECT_EQ(replaced("(a)(b)", "zabz", "$12_", syntax), "zz");
    // A number the pattern has not got, and a name it has not got, both
    // substitute nothing: GRX_TMPL_MISSING_EMPTY, not LITERAL and not ERROR.
    EXPECT_EQ(replaced("(a)(b)", "zabz", "$9", syntax), "zz");
    EXPECT_EQ(replaced("(a)(b)", "zabz", "$12", syntax), "zz");
    EXPECT_EQ(replaced("(a)(b)", "zabz", "$x", syntax), "zz");

    // Four spellings that look like references in the Perl family and are
    // ordinary text here, because each is the sigil followed by something
    // that cannot continue a name. `$_` is the exception that proves it: an
    // underscore *can*, so it is a name and substitutes nothing.
    EXPECT_EQ(replaced("(a)(b)", "zabz", "$&", syntax), "z$&z");
    EXPECT_EQ(replaced("(a)(b)", "zabz", "$`", syntax), "z$`z");
    EXPECT_EQ(replaced("(a)(b)", "zabz", "$'", syntax), "z$'z");
    EXPECT_EQ(replaced("(a)(b)", "zabz", "$_", syntax), "zz");
    // And a sigil that begins nothing at all stands as written, where PCRE2
    // reports an error for every one of them.
    EXPECT_EQ(replaced("(a)(b)", "zabz", "$", syntax), "z$z");
    EXPECT_EQ(replaced("(a)(b)", "zabz", "${1", syntax), "z${1z");
    EXPECT_EQ(replaced("(a)(b)", "zabz", "$-", syntax), "z$-z");
    // The sigil is `$` and the backslash is nothing: no escapes, no `\1`.
    EXPECT_EQ(replaced("(a)(b)", "zabz", "\\1", syntax), "z\\1z");
    EXPECT_EQ(replaced("(a)(b)", "zabz", "\\n", syntax), "z\\nz");
  }
}

TEST(Linear, FourTemplateSpellingsMeanDifferentThingsInTheTwoDialects) {
  // The two dialects agree on all 5,040 rows of the matching battery and
  // part company on four template spellings, which is why the rows carry a
  // template_spec each rather than sharing one.
  //
  // Go computes a digit run's value and then throws it away when the run has
  // a leading zero and more than one digit, so `$01` is the name "01"; the
  // crate strips the zero. And Go's `${...}` has to close on a name, where
  // the crate reads whatever is there - including nothing at all.
  EXPECT_EQ(replaced("(a)(b)", "zabz", "$01", GRX_SYNTAX_RE2), "zz");
  EXPECT_EQ(replaced("(a)(b)", "zabz", "$01", GRX_SYNTAX_RUST), "zaz");
  EXPECT_EQ(replaced("(a)(b)", "zabz", "$00", GRX_SYNTAX_RE2), "zz");
  EXPECT_EQ(replaced("(a)(b)", "zabz", "$00", GRX_SYNTAX_RUST), "zabz");
  EXPECT_EQ(replaced("(a)(b)", "zabz", "${&}", GRX_SYNTAX_RE2), "z${&}z");
  EXPECT_EQ(replaced("(a)(b)", "zabz", "${&}", GRX_SYNTAX_RUST), "zz");
  EXPECT_EQ(replaced("(a)(b)", "zabz", "${}", GRX_SYNTAX_RE2), "z${}z");
  EXPECT_EQ(replaced("(a)(b)", "zabz", "${}", GRX_SYNTAX_RUST), "zz");
}

TEST(Linear, SplittingDropsCapturesAndTheTwoDialectsDisagreeOnEmptyMatches) {
  // Captures first, because it is the one clause both share and no other
  // dialect here has: `(,)` over "a,b" is two pieces, where ECMAScript, perl
  // and Python all interleave the separator and give three.
  for (GRX_Syntax syntax : {GRX_SYNTAX_RE2, GRX_SYNTAX_RUST}) {
    EXPECT_EQ(split_into("(,)", "a,b", GRX_NPOS, syntax), "2 a|b");
    EXPECT_EQ(split_into(",", "a,b,c", GRX_NPOS, syntax), "3 a|b|c");
    // Trailing empties are kept, which is ECMAScript's half and not perl's.
    EXPECT_EQ(split_into(",", "a,b,,", GRX_NPOS, syntax), "4 a|b||");
    // `limit` counts pieces and the last is the unsplit remainder, and zero
    // means no pieces - a fourth reading again, ECMAScript's spelling with
    // perl's remainder.
    EXPECT_EQ(split_into(",", "a,b,c", 1, syntax), "1 a,b,c");
    EXPECT_EQ(split_into(",", "a,b,c", 2, syntax), "2 a|b,c");
    EXPECT_EQ(split_into(",", "a,b,c", 0, syntax), "0");
  }

  // And then the clause where they part. Go drops an empty match at either
  // end of the subject and the crate keeps both, so every one of these
  // differs by exactly the pieces at the ends.
  EXPECT_EQ(split_into("a*", "baac", GRX_NPOS, GRX_SYNTAX_RE2), "2 b|c");
  EXPECT_EQ(split_into("a*", "baac", GRX_NPOS, GRX_SYNTAX_RUST), "4 |b|c|");
  EXPECT_EQ(split_into("", "abc", GRX_NPOS, GRX_SYNTAX_RE2), "3 a|b|c");
  EXPECT_EQ(split_into("", "abc", GRX_NPOS, GRX_SYNTAX_RUST), "5 |a|b|c|");
  EXPECT_EQ(split_into("x*", "a", GRX_NPOS, GRX_SYNTAX_RE2), "1 a");
  EXPECT_EQ(split_into("x*", "a", GRX_NPOS, GRX_SYNTAX_RUST), "3 |a|");
  // With a limit the two differ in both fields, because Go's caps the
  // *matches* as well as the pieces and the crate's caps only the pieces.
  EXPECT_EQ(split_into("a*", "baac", 2, GRX_SYNTAX_RE2), "2 b|c");
  EXPECT_EQ(split_into("a*", "baac", 2, GRX_SYNTAX_RUST), "2 |baac");

  // Go's empty-subject rule reads the length of the pattern's own *text*,
  // which is the only cell in this library that does. `(?:)` and the empty
  // pattern compile to the same program and answer differently, and no
  // property of that program can tell them apart.
  EXPECT_EQ(split_into("(?:)", "", GRX_NPOS, GRX_SYNTAX_RE2), "1 ");
  EXPECT_EQ(split_into("", "", GRX_NPOS, GRX_SYNTAX_RE2), "0");
  EXPECT_EQ(split_into("a*", "", GRX_NPOS, GRX_SYNTAX_RE2), "1 ");
  EXPECT_EQ(split_into("z", "", GRX_NPOS, GRX_SYNTAX_RE2), "1 ");
  // The crate has no such rule: the walk answers it, and every one of those
  // is two empty pieces around the one empty match.
  EXPECT_EQ(split_into("(?:)", "", GRX_NPOS, GRX_SYNTAX_RUST), "2 |");
  EXPECT_EQ(split_into("", "", GRX_NPOS, GRX_SYNTAX_RUST), "2 |");
  EXPECT_EQ(split_into("a*", "", GRX_NPOS, GRX_SYNTAX_RUST), "2 |");
  EXPECT_EQ(split_into("z", "", GRX_NPOS, GRX_SYNTAX_RUST), "1 ");
}

TEST(Linear, TheSearchAllLoopSkipsAnEmptyMatchAbuttingTheOneBefore) {
  // GRX_ITERATE_ADVANCE_SKIP_ABUTTING, and the crate's half of the cell had
  // never been measured: the value was Go's, taken from a sentence in
  // `regexp`'s documentation, and the crate's row copied it. Both references
  // now answer through tools/oracle/iterate_diff.py, 4,200 rows each.
  //
  // `a*` over "baac" is the shortest case: three matches, not four. The
  // empty match at 1 abuts nothing and is reported as 1-3 having consumed;
  // the one at 3 abuts it and is skipped; 4-4 is reported.
  for (GRX_Syntax syntax : {GRX_SYNTAX_RE2, GRX_SYNTAX_RUST}) {
    EXPECT_EQ(all_matches("a*", "baac", syntax), "0-0 1-3 4-4");
    EXPECT_EQ(all_matches("b*", "ab", syntax), "0-0 1-2");
    EXPECT_EQ(all_matches("a|", "aab", syntax), "0-1 1-2 3-3");
    EXPECT_EQ(all_matches("", "abc", syntax), "0-0 1-1 2-2 3-3");
    EXPECT_EQ(all_matches("x*", "", syntax), "0-0");
  }
}

TEST(Linear, TheMatchIsLeftmostFirstAndNotLeftmostLongest) {
  // Worth asserting precisely because these are automata: an engine that
  // reports the longest match at the leftmost start would be within its
  // rights, and POSIX's does. Neither of these does.
  for (GRX_Syntax syntax : {GRX_SYNTAX_RE2, GRX_SYNTAX_RUST}) {
    EXPECT_EQ(search("(a|ab)", "ab", syntax), "0-1 0-1");
    EXPECT_EQ(search("a|ab", "ab", syntax), "0-1");
    // And backtracking priority still decides the whole match, so the first
    // branch losing does not mean the leftmost start does.
    EXPECT_EQ(search("(a|ab)(c|bcd)", "abcd", syntax), "0-4 0-1 1-4");
  }
}

TEST(Linear, CaselessWidensTheShorthandAndOnlyRustWidensTheBoundary) {
  // The cell that needed two profile fields where the table had one, and the
  // only pattern pair that separates them. Go's `regexp` widens `\w` and
  // leaves `\b`'s word set alone; the crate widens both, which is
  // ECMAScript's rule; the Perl family widens neither.
  const uint32_t i = GRX_OPT_CASELESS;

  EXPECT_EQ(search("\\w", "ſ", GRX_SYNTAX_RE2, i), "0-2");
  EXPECT_EQ(search("\\w", "ſ", GRX_SYNTAX_RUST, i), "0-2");

  // U+212A is a word character to `\b` in the crate, so the boundary after
  // "x" is gone; in RE2 it is not, so the boundary is there.
  EXPECT_EQ(search("x\\b", "xK", GRX_SYNTAX_RE2, i), "0-1");
  EXPECT_EQ(search("x\\b", "xK", GRX_SYNTAX_RUST, i), "nomatch");
}

TEST(Linear, CaselessFoldsAPosixClassBeforeNegatingIt) {
  // Two rules in one test because the order is what is being asserted.
  // `(?i)[[:alpha:]]` matching U+017F says the class folds at all - which no
  // other dialect here does. `(?i)[[:^alpha:]]` *not* matching it says the
  // fold ran before the complement: folding afterwards would add nothing
  // that removes U+017F, and the answer would flip.
  const uint32_t i = GRX_OPT_CASELESS;
  for (GRX_Syntax syntax : {GRX_SYNTAX_RE2, GRX_SYNTAX_RUST}) {
    EXPECT_EQ(search("[[:alpha:]]", "ſ", syntax, i), "0-2");
    EXPECT_EQ(search("[[:alpha:]]", "ſ", syntax), "nomatch");
    EXPECT_EQ(search("[[:^alpha:]]", "ſ", syntax, i), "nomatch");
    EXPECT_EQ(search("[[:^alpha:]]", "ſ", syntax), "0-2");
  }
}

TEST(Linear, TheShorthandsAreAsciiInRe2AndUnicodeInRust) {
  // The one profile cell where the two part company over every subject above
  // U+007F - and the crate's POSIX classes stay ASCII while its shorthands
  // do not, which is why posix_classes_stay_ascii exists.
  EXPECT_EQ(search("\\w", "é", GRX_SYNTAX_RE2), "nomatch");
  EXPECT_EQ(search("\\w", "é", GRX_SYNTAX_RUST), "0-2");
  EXPECT_EQ(search("\\d", "٣", GRX_SYNTAX_RE2), "nomatch");
  EXPECT_EQ(search("\\d", "٣", GRX_SYNTAX_RUST), "0-2");

  EXPECT_EQ(search("[[:word:]]", "é", GRX_SYNTAX_RE2), "nomatch");
  EXPECT_EQ(search("[[:word:]]", "é", GRX_SYNTAX_RUST), "nomatch");
}

TEST(Linear, RustsMinusUTakesAwayEveryAtomThatCouldSliceACharacter) {
  // `regex::Regex` matches `&str`, so every match is whole characters.
  // Without `u` a negated set is a set of bytes, and the crate refuses the
  // construct rather than producing a match that would cut one in half.
  // Probed: `\w` and `[a]` are fine, their negations and `.` are not.
  const uint32_t a = GRX_OPT_ASCII_CLASSES;
  EXPECT_EQ(rust("\\w", a), "ok");
  EXPECT_EQ(rust("[a]", a), "ok");
  EXPECT_EQ(rust("\\D", a), "refused");
  EXPECT_EQ(rust("\\W", a), "refused");
  EXPECT_EQ(rust("\\S", a), "refused");
  EXPECT_EQ(rust("[^a]", a), "refused");
  EXPECT_EQ(rust(".", a), "refused");
  // And `\p{...}` goes with it, which no other dialect's ASCII flag does:
  // Perl's `/a` and Python's `(?a)` both leave the property escape alone.
  EXPECT_EQ(rust("\\p{L}", a), "refused");

  // The same option on RE2 is a caller asking for something the dialect has
  // no letter for, and it changes none of these.
  EXPECT_EQ(re2("\\D", a), "ok");
  EXPECT_EQ(re2(".", a), "ok");
}

TEST(Linear, TheFlagAlphabetsAreTheOnesEachReferenceTakes) {
  // `x` is the letter a caller coming from any other dialect will reach for,
  // and `regexp` has no extended mode at all - so it is the deciding case
  // for RE2's alphabet, and its presence is the deciding case for Rust's.
  auto letters = [](GRX_Syntax syntax, const char * text) {
    uint32_t options = 0;
    GRX_Error error;
    grx_error_clear(&error);
    return grx_options_parse(syntax, text, &options, &error);
  };
  EXPECT_EQ(letters(GRX_SYNTAX_RE2, "imsU"), GRX_OK);
  EXPECT_NE(letters(GRX_SYNTAX_RE2, "x"), GRX_OK);
  EXPECT_NE(letters(GRX_SYNTAX_RE2, "u"), GRX_OK);

  EXPECT_EQ(letters(GRX_SYNTAX_RUST, "imsUxu"), GRX_OK);
  EXPECT_NE(letters(GRX_SYNTAX_RUST, "n"), GRX_OK);

  // Inline, the same letters and one more each way: `(?U)` is in both,
  // `(?x)` only in the crate, and an unknown letter is a syntax error in
  // both rather than a silently ignored one.
  EXPECT_EQ(re2("(?U)a+"), "ok");
  EXPECT_EQ(re2("(?x)a"), "refused");
  EXPECT_EQ(rust("(?x)a"), "ok");
  EXPECT_EQ(re2("(?Z)a"), "refused");
  EXPECT_EQ(rust("(?Z)a"), "refused");
}

TEST(Linear, DollarIsTheEndOfTheSubjectAndCaretReachesPastAFinalNewline) {
  // Two halves of section 5.3, and they point opposite ways: `$` is
  // stricter here than in Perl - no match before a final newline - and `^`
  // under multiline is looser, matching at the very end of a subject that
  // ends in one.
  for (GRX_Syntax syntax : {GRX_SYNTAX_RE2, GRX_SYNTAX_RUST}) {
    EXPECT_EQ(search("a$", "a", syntax), "0-1");
    EXPECT_EQ(search("a$", "a\n", syntax), "nomatch");
    EXPECT_EQ(search("\\p{Any}^", "a\n", syntax, GRX_OPT_MULTILINE), "1-2");
    // The line terminator is the line feed alone: a carriage return is not
    // one, which is what separates these rows from ECMAScript's.
    EXPECT_EQ(search("^b", "a\rb", syntax, GRX_OPT_MULTILINE), "nomatch");
    EXPECT_EQ(search("^b", "a\nb", syntax, GRX_OPT_MULTILINE), "2-3");
  }
}

TEST(Linear, AnAnchorMayCarryAQuantifierHereAndNotInPerl) {
  // The answer a reader would not guess, and both references give it: `^*`
  // compiles and matches the empty string. Perl refuses `(?=a)*` and
  // ECMAScript's `u` mode refuses `^*`, so a hook that borrowed either
  // rule would refuse a pattern these dialects compile.
  for (GRX_Syntax syntax : {GRX_SYNTAX_RE2, GRX_SYNTAX_RUST}) {
    EXPECT_EQ(accepts("^*", syntax), "ok");
    EXPECT_EQ(search("^*", "a", syntax), "0-0");
    EXPECT_EQ(accepts("\\b*", syntax), "ok");
  }
}

TEST(Linear, RustsClassOperatorsAreOnePrecedenceLevelAndLeftAssociative) {
  // `[a-c--b&&a]` is the only case that shows it. Left to right it is
  // `(a-c -- b) && a`, which is {a}; giving `&&` a tighter binding would
  // make it `a-c -- (b&&a)`, which is all three.
  EXPECT_EQ(search("[a-c--b&&a]", "a", GRX_SYNTAX_RUST), "0-1");
  EXPECT_EQ(search("[a-c--b&&a]", "c", GRX_SYNTAX_RUST), "nomatch");

  EXPECT_EQ(search("[\\w&&\\d]", "5", GRX_SYNTAX_RUST), "0-1");
  EXPECT_EQ(search("[\\w&&\\d]", "x", GRX_SYNTAX_RUST), "nomatch");
  // Symmetric difference, which no other dialect here spells.
  EXPECT_EQ(search("[ab~~bc]", "a", GRX_SYNTAX_RUST), "0-1");
  EXPECT_EQ(search("[ab~~bc]", "b", GRX_SYNTAX_RUST), "nomatch");
  EXPECT_EQ(search("[ab~~bc]", "c", GRX_SYNTAX_RUST), "0-1");
  // A negation applies to the whole expression rather than to its first
  // operand.
  EXPECT_EQ(search("[^a--b]", "b", GRX_SYNTAX_RUST), "0-1");
  EXPECT_EQ(search("[^a--b]", "a", GRX_SYNTAX_RUST), "nomatch");
}

TEST(Linear, RustsBoundTypeFallsBackToAQuantifier) {
  // `\b{1,2}` is `\b` repeated in the crate, `\b{}` and `\b{g}` are errors,
  // and `\b{start}` is the assertion. So the name is tried first and the
  // position rewound when it is not one - three answers from one `{`.
  EXPECT_EQ(rust("\\b{1,2}"), "ok");
  EXPECT_EQ(rust("\\b{start}"), "ok");
  EXPECT_EQ(rust("\\b{end}"), "ok");
  EXPECT_EQ(rust("\\b{}"), "refused");
  EXPECT_EQ(rust("\\b{g}"), "refused");
  EXPECT_EQ(search("\\b{start}a", "ba", GRX_SYNTAX_RUST), "nomatch");
  EXPECT_EQ(search("\\b{start}a", "a", GRX_SYNTAX_RUST), "0-1");

  // And in RE2 every one of those is `\b` followed by literal text, because
  // there is no bound type for the `{` to begin.
  EXPECT_EQ(re2("\\b{g}"), "ok");
  // The boundary is after the "a", so the literal `{g}` that follows it
  // matches at 1: `regexp` answers 1-4, and no match for "{g}a" where the
  // brace stands at a position that is not a boundary.
  EXPECT_EQ(search("\\b{g}", "a{g}", GRX_SYNTAX_RE2), "1-4");
  EXPECT_EQ(search("\\b{g}", "{g}a", GRX_SYNTAX_RE2), "nomatch");
}

TEST(Linear, RE2ReadsAQuotedRunAndTheCrateHasNoSuchLetter) {
  EXPECT_EQ(search("\\Qa+b\\E", "a+b", GRX_SYNTAX_RE2), "0-3");
  EXPECT_EQ(search("\\Qa+b\\E", "ab", GRX_SYNTAX_RE2), "nomatch");
  // A run with nothing in it opens nothing; an unterminated one runs to the
  // end of the pattern; and a `\E` that closes nothing is an *error*, which
  // is `regexp`'s answer and not perl's - perl warns "Useless use of \E"
  // and compiles. All four probed.
  EXPECT_EQ(re2("\\Q\\E"), "ok");
  EXPECT_EQ(re2("\\Qa"), "ok");
  EXPECT_EQ(re2("a\\Eb"), "refused");
  EXPECT_EQ(re2("\\Qa\\E\\E"), "refused");
  // Not inside a class, which `regexp` refuses.
  EXPECT_EQ(re2("[\\Q]\\E]"), "refused");
  EXPECT_EQ(rust("\\Qa\\E"), "refused");
}

TEST(Linear, RE2sOctalNeedsASecondDigitUnlessItStartsWithZero) {
  // The one place RE2's octal syntax is shaped by a construct it has not
  // got: `regexp` reads a single non-zero digit as a backreference and then
  // refuses it. So `\0` is NUL, `\10` and `\101` are octal, and `\1` is an
  // error - which a reader that took any octal digit as a leading one would
  // have made the SOH character.
  EXPECT_EQ(search("\\101", "A", GRX_SYNTAX_RE2), "0-1");
  EXPECT_EQ(search("\\10", "\b", GRX_SYNTAX_RE2), "0-1");
  EXPECT_EQ(re2("\\0"), "ok");
  EXPECT_EQ(re2("\\1"), "refused");
  EXPECT_EQ(re2("\\8"), "refused");
  EXPECT_EQ(re2("\\9"), "refused");
}

TEST(Linear, AnEmptyQuotedRunSeparatesTwoQuantifiers) {
  // `regexp`'s double-quantifier check is an *adjacency* check, which is
  // only visible because RE2 has one thing that can stand between two
  // quantifiers and mean nothing. Four answers from the same two
  // asterisks, all probed:
  EXPECT_EQ(re2("a**"), "refused");        // adjacent
  EXPECT_EQ(re2("a+\\Q\\E*"), "ok");        // the run is between them
  EXPECT_EQ(re2("a??\\Q\\E*"), "ok");       // ...after a lazy suffix too
  EXPECT_EQ(re2("a\\Q\\E**"), "refused");   // the run is before the first

  // And the run is not an atom: a quantifier with only a run before it has
  // nothing to repeat.
  EXPECT_EQ(re2("\\Q\\E*"), "refused");
}

TEST(Linear, RustsCrlfFlagIsALineTerminatorSetAndNotAnOptionBit) {
  // `(?R)` is recursion in the Perl family and RE2 has not got it, so the
  // letter reaches the flag reader only under the crate - which is the one
  // place in this front end where the same two characters are a construct
  // in one dialect and a flag in the other.
  EXPECT_EQ(re2("(?R)a"), "refused");
  EXPECT_EQ(rust("(?R)a"), "ok");

  // What it sets is the line-terminator set, so it lands on the pattern the
  // way PCRE2's `(*CRLF)` does rather than on an option bit.
  EXPECT_EQ(search("(?R)^b", "a\r\nb", GRX_SYNTAX_RUST, GRX_OPT_MULTILINE),
      "3-4");
  EXPECT_EQ(search("(?R).", "\r", GRX_SYNTAX_RUST), "nomatch");
  EXPECT_EQ(search(".", "\r", GRX_SYNTAX_RUST), "0-1");
  EXPECT_EQ(search("(?R)$", "a\r\n", GRX_SYNTAX_RUST), "3-3");
  EXPECT_EQ(search("(?R)$", "a\r", GRX_SYNTAX_RUST), "2-2");
}

TEST(Linear, RustsImplicitUnionBindsTighterThanItsOperators) {
  // `[a-z--[aeiou]{]` is the case that shows it: the right-hand operand is
  // `[aeiou]` *and* `{`, so the brace is subtracted and the class does not
  // match it. Joining the brace at the operator level instead would compute
  // `(a-z -- [aeiou]) | {` and match it.
  EXPECT_EQ(search("[a-z--[aeiou]{]", "{", GRX_SYNTAX_RUST), "nomatch");
  EXPECT_EQ(search("[a-z--[aeiou]{]", "q", GRX_SYNTAX_RUST), "0-1");
  EXPECT_EQ(search("[a-z--[aeiou]{]", "e", GRX_SYNTAX_RUST), "nomatch");

  // A nested class beside members, in each position, and a nested class
  // that is the whole operand.
  EXPECT_EQ(search("[a[b-c]d]", "c", GRX_SYNTAX_RUST), "0-1");
  EXPECT_EQ(search("[a[b-c]d]", "e", GRX_SYNTAX_RUST), "nomatch");
  // `[^[a-c]-(a)-[b]]` matches nothing at all in the crate, which is worth
  // recording rather than guessing at: the `-` between two nested classes
  // is not a union there, and the complement of what it does denote is
  // empty. This library agrees, and did so before and after the union
  // layer was added, so it is not a property of that change.
  EXPECT_EQ(search("[^[a-c]-(a)-[b]]", "b", GRX_SYNTAX_RUST), "nomatch");
  EXPECT_EQ(search("[^[a-c]-(a)-[b]]", "z", GRX_SYNTAX_RUST), "nomatch");
  EXPECT_EQ(search("[^a[b]]", "z", GRX_SYNTAX_RUST), "0-1");
  EXPECT_EQ(search("[^a[b]]", "b", GRX_SYNTAX_RUST), "nomatch");
  // A `-` after a nested or POSIX class is an ordinary member; after a
  // shorthand it is an error. Probed all three.
  EXPECT_EQ(rust("[[a-c]-x]"), "ok");
  EXPECT_EQ(rust("[[:alpha:]-x]"), "ok");
  EXPECT_EQ(rust("[\\d-x]"), "refused");
}

TEST(Linear, AMalformedPosixNameIsAnErrorInRe2AndMembersInTheCrate) {
  // Asked before it is read rather than by reading it and looking at the
  // result code: `grx_parse_fail()` returns GRX_ERR_SYNTAX, which is also
  // how "no POSIX class here" used to be spelled, so a malformed one under
  // RE2 reported its diagnostic and was then read as literal members
  // anyway. It survived because `[[:word(?>a):]]` alone fails for a second
  // reason; only a longer pattern showed it.
  EXPECT_EQ(re2("[[:nosuch:]]"), "refused");
  EXPECT_EQ(re2("[[:word(?>a):]]"), "refused");
  EXPECT_EQ(re2("\\d\\p{Greek}[[:word(?>a):]]*[\\x41-\\x5A]"), "refused");

  // The crate falls back to members, so the same text is a class of `[`,
  // `:`, the letters, and the punctuation.
  EXPECT_EQ(rust("[[:nosuch:]]"), "ok");
  EXPECT_EQ(search("[[:alp(*FAIL)ha:]]*", "", GRX_SYNTAX_RUST), "0-0");
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
