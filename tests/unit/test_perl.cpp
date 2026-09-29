/**
 * @file
 *
 * The Perl-family front end, and the constructs only it reaches.
 *
 * Every test states a rule from documentation/dialects.md section 9 and names
 * where it came from. Most of them came from pcre2test 10.46 rather than from
 * pcre2pattern, and where the two differed the test says so - "the reference
 * implementation does this" is a stronger claim than "the documentation seems
 * to say", and five of the rules below were written the wrong way round until
 * the oracle was asked.
 *
 * The accept/reject behaviour as a whole is checked by the imported corpora:
 * 1,884 records from pcre2test's own test files and 1,726 from Perl's
 * `re_tests`. What is here is what a corpus cannot state - which node came
 * out, which diagnostic was reported, and which of two constructs a spelling
 * turned into.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cctype>
#include <cstring>
#include <string>

#include "test_helpers.h"

namespace {

/** Compile under a dialect, and report the result and the diagnostic. */
struct Attempt {
  GRX_Result result;
  GRX_Diag diag;
  GRX_Regex * regex;
};

Attempt compile(const std::string & pattern, GRX_Syntax syntax = GRX_SYNTAX_PCRE,
    const char * flags = "") {
  uint32_t options = 0;
  GRX_Error flag_error;
  EXPECT_EQ(grx_options_parse(syntax, flags, &options, &flag_error), GRX_OK)
      << "flags /" << flags;

  Attempt attempt = {GRX_OK, GRX_DIAG_NONE, nullptr};
  GRX_Error error;
  grx_error_clear(&error);
  attempt.result = grx_regex_compile_with_allocator(pattern.data(),
      pattern.size(), syntax, options, nullptr, nullptr, &error,
      &attempt.regex);
  attempt.diag = error.diag;
  return attempt;
}

/**
 * Compile, discard, and report the result alone.
 *
 * compile() hands back the regex, which a test that only wants the verdict
 * then has to free; forgetting is a leak ASan finds and a reader does not.
 */
GRX_Result compile_result(const std::string & pattern,
    GRX_Syntax syntax = GRX_SYNTAX_PCRE, const char * flags = "") {
  Attempt attempt = compile(pattern, syntax, flags);
  grx_regex_free(attempt.regex);
  return attempt.result;
}

/** Whether a pattern matches a subject, and where. */
struct Found {
  bool matched;
  size_t start;
  size_t end;
  GRX_Result result;
};

Found search(const std::string & pattern, const std::string & subject,
    GRX_Syntax syntax = GRX_SYNTAX_PCRE, const char * flags = "") {
  Found found = {false, GRX_NPOS, GRX_NPOS, GRX_OK};
  Attempt attempt = compile(pattern, syntax, flags);
  if (attempt.result != GRX_OK) {
    found.result = attempt.result;
    return found;
  }

  GRX_Match * match = nullptr;
  grx_match_create(attempt.regex, nullptr, &match);
  int matched = 0;
  found.result = grx_regex_search(attempt.regex, subject.data(),
      subject.size(), 0, GRX_ENGINE_AUTO, nullptr, match, &matched);
  if (found.result == GRX_OK && matched) {
    GRX_Capture whole;
    grx_match_group(match, 0, &whole);
    found.matched = true;
    found.start = whole.start;
    found.end = whole.end;
  }
  grx_match_destroy(match);
  grx_regex_free(attempt.regex);
  return found;
}

/** The mark the attempt passed, or "" when it passed none. */
std::string mark_of(const std::string & pattern, const std::string & subject,
    GRX_Syntax syntax = GRX_SYNTAX_PCRE) {
  Attempt attempt = compile(pattern, syntax);
  if (attempt.result != GRX_OK) {
    return "compile failed";
  }

  GRX_Match * match = nullptr;
  grx_match_create(attempt.regex, nullptr, &match);
  int matched = 0;
  grx_regex_search(attempt.regex, subject.data(), subject.size(), 0,
      GRX_ENGINE_AUTO, nullptr, match, &matched);
  const char * mark = grx_match_mark(match);
  std::string answer = mark ? mark : "";
  grx_match_destroy(match);
  grx_regex_free(attempt.regex);
  return answer;
}

/** Where the whole match landed, as "start-end", or "nomatch". */
std::string span_of(const std::string & pattern, const std::string & subject,
    GRX_Syntax syntax = GRX_SYNTAX_PCRE) {
  Found found = search(pattern, subject, syntax);
  if (found.result != GRX_OK) {
    return "error";
  }
  return found.matched
      ? std::to_string(found.start) + "-" + std::to_string(found.end)
      : "nomatch";
}

/** The span of one capturing group, or "-" when it did not participate. */
std::string group_of(const std::string & pattern, const std::string & subject,
    size_t group, GRX_Syntax syntax = GRX_SYNTAX_PCRE) {
  Attempt attempt = compile(pattern, syntax);
  if (attempt.result != GRX_OK) {
    return "compile failed";
  }

  GRX_Match * match = nullptr;
  grx_match_create(attempt.regex, nullptr, &match);
  int matched = 0;
  std::string answer = "nomatch";
  if (grx_regex_search(attempt.regex, subject.data(), subject.size(), 0,
          GRX_ENGINE_AUTO, nullptr, match, &matched) == GRX_OK
      && matched) {
    GRX_Capture capture;
    grx_match_group(match, group, &capture);
    answer = capture.start == GRX_NPOS
        ? "-"
        : std::to_string(capture.start) + "-" + std::to_string(capture.end);
  }
  grx_match_destroy(match);
  grx_regex_free(attempt.regex);
  return answer;
}


// --------------------------------------------------------------------------
// Quoting, comments and extended mode: the three lexical skips
// --------------------------------------------------------------------------

/** The text one *named* group matched, or "-"/"unknown". */
std::string named_of(const std::string & pattern, const std::string & subject,
    const char * name, GRX_Syntax syntax = GRX_SYNTAX_PCRE) {
  Attempt attempt = compile(pattern, syntax);
  if (attempt.result != GRX_OK) {
    return "compile failed";
  }
  GRX_Match * match = nullptr;
  grx_match_create(attempt.regex, nullptr, &match);
  int matched = 0;
  std::string answer = "nomatch";
  if (grx_regex_search(attempt.regex, subject.data(), subject.size(), 0,
          GRX_ENGINE_AUTO, nullptr, match, &matched) == GRX_OK
      && matched) {
    GRX_Capture capture;
    if (grx_match_group_named(match, name, &capture) != GRX_OK) {
      answer = "unknown";
    }
    else if (capture.start == GRX_NPOS) {
      answer = "-";
    }
    else {
      answer = subject.substr(capture.start, capture.end - capture.start);
    }
  }
  grx_match_destroy(match);
  grx_regex_free(attempt.regex);
  return answer;
}

} // namespace

TEST(Perl, DuplicateGroupNamesAreOnlyPerlsByDefault) {
  // Probed rather than reasoned: perl 5.40.1 compiles `(?<a>x)(?<a>y)` with
  // no pragma and no warning; pcre2test 10.46 refuses it as error 143,
  // "two named subpatterns have the same name (PCRE2_DUPNAMES not set)", and
  // wants `(?J)`; Node 22 refuses it as "Duplicate capture group name". The
  // three answers are three rows, and the option `(?J)` already set is the
  // whole of the mechanism - Perl's row simply has it on.
  EXPECT_EQ(span_of("(?<a>x)(?<a>y)", "xy", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(compile_result("(?<a>x)(?<a>y)", GRX_SYNTAX_PCRE),
      GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(?<a>x)(?<a>y)", GRX_SYNTAX_ECMASCRIPT),
      GRX_ERR_SYNTAX);
  EXPECT_EQ(span_of("(?J)(?<a>x)(?<a>y)", "xy", GRX_SYNTAX_PCRE), "0-2");
}

TEST(Perl, ANamedLookupTakesTheFirstDuplicateThatTookPart) {
  // Not the first duplicate. `(?<a>x)|(?<a>y)` against "y" leaves the first
  // group unset and the second holding "y", and perl reports "y" for
  // `$+{a}`; pcre2_substring_get_byname() documents the same rule, "the
  // first one that is set". Reporting the first *group* instead gives
  // "unset" for a name that plainly matched something.
  EXPECT_EQ(named_of("(?<a>x)|(?<a>y)", "y", "a", GRX_SYNTAX_PERL), "y");
  EXPECT_EQ(named_of("(?J)(?<a>x)|(?<a>y)", "y", "a", GRX_SYNTAX_PCRE), "y");

  // With both set it is the leftmost, which perl also reports.
  EXPECT_EQ(named_of("(?<a>x)(?<a>y)", "xy", "a", GRX_SYNTAX_PERL), "x");

  // A name that matched nothing anywhere is still unset rather than an
  // error, which is what a single-group name would have given.
  EXPECT_EQ(named_of("(?<a>x)(?<a>y)|z", "z", "a", GRX_SYNTAX_PERL), "-");
  EXPECT_EQ(named_of("(?<a>x)", "x", "b", GRX_SYNTAX_PERL), "unknown");
}

TEST(Perl, AQuotedRunIsLiteralAndNotAnAtom) {
  // `\Q...\E` suspends every operator meaning, which is the easy half. The
  // half that decides where the code lives is that an *empty* run is not an
  // atom at all: pcre2test compiles `a\Q\E*` as `a*`, so a front end that
  // gave `\Q\E` a node of its own would make the `*` repeat that node and
  // match a different language.
  EXPECT_TRUE(search("a\\Q\\E*", "aaa").matched);
  EXPECT_EQ(search("a\\Q\\E*", "aaa").end, 3u);

  // Inside a run, an operator is a character.
  EXPECT_TRUE(search("\\Qa*b\\E", "xa*by").matched);
  EXPECT_EQ(search("\\Qa*b\\E", "xa*by").start, 1u);
  EXPECT_FALSE(search("\\Qa*b\\E", "aab").matched);

  // And a quantifier after the run applies to the run's last character, not
  // to the whole of it.
  EXPECT_EQ(search("\\Qab\\E+", "xabbb").end, 5u);

  // A `\E` with no `\Q` is harmless in both references.
  EXPECT_TRUE(search("a\\Eb", "ab").matched);

  // A run that *opens* where a quantifier would go. This is the case the
  // test above could not see: the check for an open run happened before
  // the step that reads `\Q`, so `a\Q*\E` arrived at the quantifier with
  // the run unopened and repeated the "a". pcre2test matches "a*" here and
  // not "aaa", and every quantifier spelling did it.
  EXPECT_TRUE(search("a\\Q*\\E", "a*").matched);
  EXPECT_FALSE(search("a\\Q*\\E", "aaa").matched);
  EXPECT_FALSE(search("a\\Q*\\E", "a").matched);
  EXPECT_FALSE(search("a\\Q+\\E", "aaa").matched);
  EXPECT_TRUE(search("a\\Q+\\E", "a+").matched);
  EXPECT_FALSE(search("a\\Q{2}\\E", "aa").matched);
  EXPECT_TRUE(search("a\\Q{2}\\E", "a{2}").matched);

  // Which is not the same as suspending the quantifier that follows the
  // run: `a\Q*\E*` is "a", a literal asterisk, and a repeat of it.
  EXPECT_TRUE(search("a\\Q*\\E*", "a**").matched);
  EXPECT_EQ(search("a\\Q*\\E*", "a**").end, 3u);

  // The quantifier was the only operator that could reach this, because
  // every other one is read after the step that opens a run rather than
  // before it. These were right the whole time and are here so that a
  // change to the ordering has to keep them right.
  EXPECT_TRUE(search("a\\Q|\\Eb", "a|b").matched);
  EXPECT_FALSE(search("a\\Q|\\Eb", "a").matched);
  EXPECT_TRUE(search("a\\Q)\\E", "a)").matched);
  EXPECT_TRUE(search("a\\Q(\\E", "a(").matched);
  EXPECT_TRUE(search("a\\Q[\\E", "a[").matched);
  EXPECT_FALSE(search("a\\Q.\\E", "ax").matched);
}

TEST(Perl, ACommentGroupIsLexicalAndAQuantifierSeesPastIt) {
  // pcre2test compiles `a(?#x)*` and refuses `(?#x)*` for having nothing to
  // repeat. Both answers follow from the comment being invisible rather than
  // empty, and neither follows from it being a node.
  EXPECT_EQ(search("a(?#x)*", "aaa").end, 3u);

  Attempt nothing = compile("(?#x)*");
  EXPECT_NE(nothing.result, GRX_OK);
  EXPECT_EQ(nothing.diag, GRX_DIAG_NOTHING_TO_REPEAT);
  grx_regex_free(nothing.regex);

  // A callout is *not* in that list, and the difference is pcre2test's:
  // `a(?C1)*` is an error, so a callout is an atom that may not be repeated.
  Attempt callout = compile("a(?C1)*");
  EXPECT_NE(callout.result, GRX_OK);
  grx_regex_free(callout.regex);
}

TEST(Perl, ExtendedModeDropsSpaceBetweenEveryPartOfAQuantifier) {
  // `a + +` is `a++`, which the corpus has and which needs the skip to run
  // before the quantifier *and* before its possessive suffix.
  EXPECT_TRUE(search("^ a + + b $", "aab", GRX_SYNTAX_PCRE, "x").matched);
  EXPECT_TRUE(search("^a {2} $", "aa", GRX_SYNTAX_PCRE, "x").matched);
  EXPECT_FALSE(search("^a {2} $", "a", GRX_SYNTAX_PCRE, "x").matched);

  // A comment runs to the line break.
  EXPECT_TRUE(search("a # not a pattern\nb", "ab", GRX_SYNTAX_PCRE, "x").matched);

  // Inside a quoted run the space is a space again.
  EXPECT_TRUE(search("\\Qa b\\E", "a b", GRX_SYNTAX_PCRE, "x").matched);
  EXPECT_FALSE(search("\\Qa b\\E", "ab", GRX_SYNTAX_PCRE, "x").matched);
}

TEST(Perl, TheSecondXIgnoresSpaceInsideABracketExpressionAndTheFirstDoesNot) {
  // The one letter in any alphabet here that means something different
  // written twice, and the pair of verdicts that caught the pcre2test
  // importer folding `xx` into `x`.
  EXPECT_TRUE(search("[a-  z]", "m", GRX_SYNTAX_PCRE, "xx").matched);

  Attempt narrow = compile("[a-  z]", GRX_SYNTAX_PCRE, "x");
  EXPECT_NE(narrow.result, GRX_OK)
      << "under `x` the spaces are members and `a` to ` ` is out of order";
  grx_regex_free(narrow.regex);
}

// --------------------------------------------------------------------------
// Option settings and their scope
// --------------------------------------------------------------------------

TEST(Perl, AnInlineOptionEndsAtTheEnclosingCloseParenAndNotAtTheAlternation) {
  // Checked against pcre2test: `/(a(?i)b|c)/` matches "C", so the setting
  // survives the `|` and not the `)`. Both halves matter - the first says the
  // scope is not the branch, the second says it is not the rest of the
  // pattern.
  EXPECT_TRUE(search("^(a(?i)b|c)$", "C").matched);
  EXPECT_TRUE(search("^(a(?i)b|c)$", "aB").matched);
  EXPECT_FALSE(search("^(a(?i)b)c$", "aBC").matched);
  EXPECT_TRUE(search("^(a(?i)b)c$", "aBc").matched);

  // The scoped form applies to its body alone.
  EXPECT_TRUE(search("^a(?i:b)c$", "aBc").matched);
  EXPECT_FALSE(search("^a(?i:b)c$", "aBC").matched);
}

TEST(Perl, TheCaretResetFormRefusesAHyphen) {
  // pcre2test: "invalid hyphen in option setting". `^` has already cleared
  // everything, so a second way to clear says nothing.
  Attempt reset = compile("(?^i)AB");
  EXPECT_EQ(reset.result, GRX_OK);
  grx_regex_free(reset.regex);

  Attempt hyphen = compile("(?^-i)AB");
  EXPECT_NE(hyphen.result, GRX_OK);
  grx_regex_free(hyphen.regex);
}

TEST(Perl, TheCaretResetAppliesTheLettersThatFollowIt) {
  // `^` resets the modifiers to the dialect's defaults and *then* the
  // letters after it apply, so `(?^i:a)` is caseless and matches "A". Both
  // references agree, and this library did not: the reset put `i` into the
  // clear mask, the letter put it into the set mask, and the caller applied
  // them as `(options | set) & ~clear`, so the clear won and `(?^i:...)`
  // silently meant `(?^:...)`.
  //
  // The test above it says `(?^i)` *compiles*, which is what let this sit:
  // asking whether a construct is accepted is not asking whether it works.
  // It took generating patterns and asking perl - tools/oracle/perl_diff.py
  // - to notice, because no imported corpus has the form.
  EXPECT_TRUE(search("(?^i:a)", "A").matched);
  EXPECT_TRUE(search("(?^i)a", "A").matched);
  EXPECT_TRUE(search("(?^s:a.b)", "a\nb").matched);
  EXPECT_TRUE(search("^(?^m:a$)", "a\nb", GRX_SYNTAX_PCRE, "m").matched);

  // And the reset itself still resets: without a letter after it, an option
  // set outside is gone inside.
  EXPECT_FALSE(search("(?i)(?^:a)", "A").matched);
  EXPECT_FALSE(search("(?^:a)", "A", GRX_SYNTAX_PCRE, "i").matched);

  // A reset that names one letter does not carry the others in with it.
  EXPECT_FALSE(search("(?i)(?^s:a)", "A").matched);

  // An explicit `-` still beats an earlier letter, which is why the fix is
  // not "set always wins": `(?i-i:a)` does not match "A" in either
  // reference. The two cannot both be explicit - a hyphen after `^` is
  // refused, see the test above - so they never have to be reconciled.
  EXPECT_FALSE(search("(?i-i:a)", "A").matched);
}

/**
 * PCRE2's `(?r)`, and the half of Perl's `/aa` it is not.
 *
 * The options this needs cannot be spelled in a flags string - the PCRE2
 * alphabet has no letter for UTF or UCP, those being API arguments here -
 * so the options go in directly rather than through grx_options_parse().
 */
static bool matches_with(const std::string & pattern,
    const std::string & subject, uint32_t options,
    GRX_Syntax syntax = GRX_SYNTAX_PCRE) {
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  grx_error_clear(&error);
  if (grx_regex_compile_with_allocator(pattern.data(), pattern.size(), syntax,
          options, nullptr, nullptr, &error, &regex) != GRX_OK) {
    // Not `return false`. Half the assertions below are EXPECT_FALSE, and a
    // helper that answers "did not match" for "did not compile" makes every
    // one of them pass against a build where `(?r)` is an unknown flag -
    // which is exactly the build this test exists to distinguish from.
    ADD_FAILURE() << "did not compile: /" << pattern << "/ diag "
                  << (int)error.diag;
    return false;
  }
  GRX_Match * match = nullptr;
  grx_match_create(regex, nullptr, &match);
  int matched = 0;
  grx_regex_search(regex, subject.data(), subject.size(), 0, GRX_ENGINE_AUTO,
      nullptr, match, &matched);
  grx_match_destroy(match);
  grx_regex_free(regex);
  return matched != 0;
}

TEST(Perl, PcreCaselessRestrictIsTheFoldHalfOfPerlsDoubledA) {
  const uint32_t utf_fold = GRX_OPT_UTF | GRX_OPT_CASELESS;
  const std::string kelvin = "\xe2\x84\xaa";         // U+212A KELVIN SIGN
  const std::string a_grave_lower = "\xc3\xa0";       // U+00E0
  const std::string arabic_one = "\xd9\xa1";          // U+0661 ARABIC-INDIC 1

  // The control: without it, caseless folds across the ASCII boundary.
  EXPECT_TRUE(matches_with("k", kelvin, utf_fold));
  EXPECT_FALSE(matches_with("(?r)k", kelvin, utf_fold))
      << "(?r) is PCRE2_EXTRA_CASELESS_RESTRICT";

  // It is not "fold ASCII only": both of the pairs that stay inside one
  // side of the boundary still fold. pcre2test 10.46 answers both this way.
  EXPECT_TRUE(matches_with("(?r)k", "K", utf_fold));
  EXPECT_TRUE(matches_with("(?r)\xc3\x80", a_grave_lower, utf_fold));

  // And it is the *fold* half alone, which is what makes it not Perl's
  // `/aa`: the classes are untouched, where `/aa` narrows them because it
  // is the letter `a` twice and carries `a`'s meaning with it.
  EXPECT_TRUE(matches_with("(?r)\\d", arabic_one,
      GRX_OPT_UTF | GRX_OPT_UCP));

  // It scopes and negates like any other flag, and does not leak out of a
  // group it was set inside.
  EXPECT_FALSE(matches_with("(?i)(?r:k)", kelvin, GRX_OPT_UTF));
  EXPECT_TRUE(matches_with("(?i)(?:(?r))k", kelvin, GRX_OPT_UTF));
  EXPECT_TRUE(matches_with("(?r)(?-r)k", kelvin, utf_fold));

  // Perl has no such letter, so it stays unknown there rather than
  // becoming a second spelling of `/aa`.
  Attempt perl = compile("(?r)k", GRX_SYNTAX_PERL);
  EXPECT_NE(perl.result, GRX_OK);
  EXPECT_EQ(perl.diag, GRX_DIAG_UNKNOWN_FLAG);
  grx_regex_free(perl.regex);
}

TEST(Perl, DuplicateNamesNeedJOrABranchReset) {
  Attempt plain = compile("(?<a>x)(?<a>y)");
  EXPECT_NE(plain.result, GRX_OK);
  EXPECT_EQ(plain.diag, GRX_DIAG_DUPLICATE_GROUP_NAME);
  grx_regex_free(plain.regex);

  Attempt allowed = compile("(?J)(?<a>x)(?<a>y)");
  EXPECT_EQ(allowed.result, GRX_OK) << "`(?J)` is PCRE2_DUPNAMES";
  grx_regex_free(allowed.regex);

  // A branch reset names one group twice because it *is* one group, and
  // pcre2test accepts it without `(?J)`.
  Attempt reset = compile("(?|(?'a'x)|(?'a'y))");
  EXPECT_EQ(reset.result, GRX_OK);
  grx_regex_free(reset.regex);
}

TEST(Perl, ACallIntoALookbehindsGroupGetsItsOwnCopy) {
  // A subroutine block's instructions step the way its *definition* does,
  // so a group written inside a lookbehind that runs backwards has a
  // backwards block. A call from outside it walked the subject the wrong
  // way: `(*naplb:(a))(?1)` against "aa" is 1-2 in pcre2test and was 1-1
  // here, with group one left holding a span outside the match. It was
  // refused outright until 2026-09-24; now the direction is part of the
  // block's key and the group gets a copy laid out each way.
  EXPECT_EQ(span_of("(*naplb:(a))(?1)", "aa", GRX_SYNTAX_PCRE), "1-2");
  EXPECT_EQ(group_of("(*naplb:(a))(?1)", "aa", 1, GRX_SYNTAX_PCRE), "0-1");
  EXPECT_EQ(span_of("(*naplb:(a))(?1)", "ab", GRX_SYNTAX_PCRE), "nomatch");
  EXPECT_EQ(span_of("(*naplb:(a))x(?1)", "aaxa", GRX_SYNTAX_PCRE), "2-4");
  EXPECT_EQ(span_of("(*naplb:(ab))(?1)", "abab", GRX_SYNTAX_PCRE), "2-4");
  EXPECT_EQ(span_of("(*naplb:((a)b))(?1)", "abab", GRX_SYNTAX_PCRE), "2-4");
  EXPECT_EQ(span_of("(*naplb:(a|bc))(?1)", "bcbc", GRX_SYNTAX_PCRE), "2-4");
  EXPECT_EQ(span_of("(*naplb:(?<x>a))(?&x)", "aa", GRX_SYNTAX_PCRE), "1-2");
  EXPECT_EQ(span_of("(*naplb:(a))(?1)(?1)", "aaa", GRX_SYNTAX_PCRE), "1-3");

  // A lookaround written *inside* the called group keeps the direction
  // lowering gave it, which is why the copy is an XOR on the way down and
  // not a rewrite of the flags. The discriminating case is a call in the
  // other mixture - a group defined forwards, called from inside a
  // backwards body, so the copy is the reversed one: `(x(?=y))` must go on
  // looking *ahead* for the "y" there. A copy that straightened the
  // lookahead along with everything else looks behind from the "x", finds
  // nothing, and answers no match. pcre2test: 0-1.
  EXPECT_EQ(span_of("(x(?=y))(*naplb:(?1))", "xy", GRX_SYNTAX_PCRE), "0-1");
  EXPECT_EQ(span_of("(x(?=y))(*naplb:(?1))", "xyxy", GRX_SYNTAX_PCRE), "0-1");
  EXPECT_EQ(span_of("(x(?<=zx))(*naplb:(?1))", "zxzx", GRX_SYNTAX_PCRE),
      "1-2");
  // The non-atomic spellings take a different path through codegen and
  // have to clear it in their own place. Without these two the clear in
  // gen_non_atomic_look() could be deleted and every other assertion here
  // would still pass. pcre2test: 0-1 and 1-2.
  EXPECT_EQ(span_of("(x(*napla:y))(*naplb:(?1))", "xy", GRX_SYNTAX_PCRE),
      "0-1");
  EXPECT_EQ(span_of("(x(*naplb:zx))(*naplb:(?1))", "zxzx", GRX_SYNTAX_PCRE),
      "1-2");
  // And the same shape with the call outside, which refuses on both sides.
  EXPECT_EQ(span_of("(*naplb:(a(?=b)))(?1)", "abab", GRX_SYNTAX_PCRE),
      "nomatch");
  EXPECT_EQ(span_of("(*naplb:(a(?<=xa)))(?1)", "xaxa", GRX_SYNTAX_PCRE),
      "nomatch");

  // One group called *both* ways in one pattern, which is what makes the
  // direction part of the block's key rather than a property of the group:
  // the inner `(?1)` runs backwards and the outer one forwards, and a
  // cache keyed on the pair alone hands the second call the first's block.
  // pcre2test: 2-3.
  EXPECT_EQ(span_of("(*naplb:(a)(?1))(?1)", "aaa", GRX_SYNTAX_PCRE), "2-3");
  EXPECT_EQ(span_of("(*naplb:(a)(?1))(?1)", "aaaa", GRX_SYNTAX_PCRE), "2-3");

  // The forward-model lookbehind is the same question from the other side,
  // and it was already right: its group's block is forwards too.
  EXPECT_EQ(span_of("(?<=(a))(?1)", "aa"), "1-2");
  EXPECT_EQ(span_of("(?<=(a))x(?1)", "axa"), "1-3");
  // A call from inside a lookbehind to a group outside it, which always
  // agreed, and a call to a group inside the same lookbehind.
  EXPECT_EQ(span_of("(a)(*naplb:(?1))", "aa"), "0-1");
  EXPECT_EQ(span_of("(*naplb:(a)(?1))b", "aab"), "2-3");
  // And an ordinary call is an ordinary call.
  EXPECT_EQ(span_of("(a)(?1)", "aa"), "0-2");
}

TEST(Perl, AConditionsParenthesisIsNotAGroup) {
  // The prescan counts capturing parentheses so that `\1` can be told from
  // an octal escape, and it counted the `(` of `(?(...)` - which opens a
  // *condition* - as one of them. Two symptoms, and the references agree
  // with each other on both: a backreference to a group the pattern has not
  // got compiled, and a condition naming the parenthesis it was written in
  // compiled too.
  EXPECT_EQ(compile_result("(?(R)a|b)\\1", GRX_SYNTAX_PCRE), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(?(R)a|b)\\1", GRX_SYNTAX_PERL), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("\\1(?(R)a|b)", GRX_SYNTAX_PCRE), GRX_ERR_SYNTAX);
  // An escaped parenthesis before one is still an escaped parenthesis:
  // `\(?(a)\1` is a literal "(", optional, then a group and a reference to
  // it, and it matches "aa" as well as "(aa".
  EXPECT_EQ(span_of("\\(?(a)\\1", "aa"), "0-2");
  EXPECT_EQ(span_of("\\(?(a)\\1", "(aa"), "0-3");
  // And the assertion condition, whose inner `(` has a `?` after it and was
  // never counted, still compiles and still runs.
  EXPECT_EQ(span_of("(?(?=a)b|c)", "c"), "0-1");
}

TEST(Perl, WhetherAConditionMayNameAGroupThatIsNotThere) {
  // perl reads `(?(99)a|b)` as a condition that is false and matches "b";
  // pcre2test refuses it as a reference to a subpattern that does not
  // exist, and so does CPython. Both were asked.
  EXPECT_EQ(span_of("(?(99)a|b)", "b", GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("(?(R99)a|b)", "b", GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(compile_result("(?(99)a|b)", GRX_SYNTAX_PCRE), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(?(R99)a|b)", GRX_SYNTAX_PCRE), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(?(99)a|b)", GRX_SYNTAX_PYTHON), GRX_ERR_SYNTAX);
  // `(?(0)` is an error in every one of them, and so is a name no group
  // has, so the leniency is the numeric forms alone.
  EXPECT_EQ(compile_result("(?(0)a|b)", GRX_SYNTAX_PERL), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(?(<n>)a|b)", GRX_SYNTAX_PERL), GRX_ERR_SYNTAX);
  // A group that *is* there is unaffected in both.
  EXPECT_EQ(span_of("(a)(?(1)b|c)", "ab", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("(a)(?(1)b|c)", "ab", GRX_SYNTAX_PCRE), "0-2");
}

TEST(Perl, AConditionalOnADuplicateNameAsksWhetherAnyOfThemIsSet) {
  // One name, several groups, and a conditional that names it: the question
  // is whether *any* group of that name participated, which is the rule a
  // backreference to a duplicate name already followed. It asked the first
  // group alone - the one that may never run - so these two were exactly
  // inverted. perl 5.40.1 and pcre2 10.46 agree with each other here, and
  // both were asked before this was written.
  EXPECT_EQ(span_of("(?J)(?<n>x)?(?<n>b)(?(<n>)c|d)", "bc"), "0-2");
  EXPECT_EQ(span_of("(?J)(?<n>x)?(?<n>b)(?(<n>)c|d)", "bd"), "nomatch");
  // The first group of the name is the one that is set here, which is the
  // case that passed before: a rule that reads the first group looks right
  // until the first group is the one that did not run.
  EXPECT_EQ(span_of("(?J)(?<n>b)(?<n>a)?(?(<n>)b|c)", "bab"), "0-3");
  EXPECT_EQ(span_of("(?J)(?<n>b)(?<n>a)?(?(<n>)b|c)", "bc"), "nomatch");
  // And the same in perl's spelling, where duplicate names need no switch.
  EXPECT_EQ(span_of("(?<n>x)|(?<n>b)(?(<n>)c|d)", "bc", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("(?<n>a)(?(<n>)b|c)", "ab", GRX_SYNTAX_PERL), "0-2");
  // A name belonging to one group is unchanged: no list, no lookup.
  EXPECT_EQ(span_of("(?<n>a)?(?(<n>)b|c)", "ab"), "0-2");
  EXPECT_EQ(span_of("(?<n>a)?(?(<n>)b|c)", "c"), "0-1");
}

// --------------------------------------------------------------------------
// The numeric escapes
// --------------------------------------------------------------------------

TEST(Perl, ADigitEscapeIsAReferenceOrOctalByItsWholeValue) {
  // pcre2pattern: a backreference when the value is below 10, or when that
  // many capturing parentheses have been opened; otherwise up to three octal
  // digits. The *whole* number decides, not the first digit.
  EXPECT_TRUE(search("(a)\\1", "aa").matched);
  EXPECT_TRUE(search("()()()()()()()()()()()(a)\\12", "aa").matched);

  // Two groups, so `\12` is a tab.
  EXPECT_TRUE(search("(a)(b)\\12", "ab\n").matched);

  // `\8` is neither: pcre2test calls it "reference to non-existent
  // subpattern" rather than reading the digits as literals.
  Attempt eight = compile("\\8");
  EXPECT_NE(eight.result, GRX_OK);
  EXPECT_EQ(eight.diag, GRX_DIAG_INVALID_BACKREFERENCE);
  grx_regex_free(eight.regex);
}

TEST(Perl, ADigitlessHexEscapeIsNulInPerlAndAnErrorInTheOthers) {
  // The two references disagree and each dialect follows its own. perl reads
  // every degenerate spelling as U+0000 - measured one at a time against
  // 5.44.0, the last two with a warning this library has no channel for -
  // where pcre2test 10.46 answers "digits missing after \x" to all of them
  // and `re` says "incomplete escape".
  //
  // This library refused all five in every dialect until 2026-09-26, which
  // was pcre2's answer applied to perl as well, and it contradicted
  // read_hex()'s own docstring.
  const char * degenerate[] = {
    "\\x", "a\\x", "\\xg", "\\x{}", "\\x{_}",
  };
  for (const char * pattern : degenerate) {
    for (GRX_Syntax syntax : {GRX_SYNTAX_PCRE, GRX_SYNTAX_PYTHON}) {
      Attempt bare = compile(pattern, syntax);
      EXPECT_NE(bare.result, GRX_OK) << pattern;
      EXPECT_EQ(bare.diag, GRX_DIAG_INVALID_HEX_ESCAPE) << pattern;
      grx_regex_free(bare.regex);
    }
    Attempt perl = compile(pattern, GRX_SYNTAX_PERL);
    EXPECT_EQ(perl.result, GRX_OK) << pattern;
    grx_regex_free(perl.regex);
  }

  // And what each one *matches* there, which is the half a compile check
  // cannot state: a NUL, and for two of them a NUL followed by the character
  // that stopped the digits.
  EXPECT_EQ(span_of("\\x", std::string("\0", 1), GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("a\\x", std::string("a\0", 2), GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("\\xg", std::string("\0g", 2), GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("\\x{}", std::string("\0", 1), GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("\\x{_}", std::string("\0", 1), GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("\\x", "x", GRX_SYNTAX_PERL), "nomatch");

  // A digit still means what it says, in both.
  EXPECT_TRUE(search("\\x41", "A").matched);
  EXPECT_TRUE(search("\\x{1F600}", "\xF0\x9F\x98\x80", GRX_SYNTAX_PERL).matched);
}

TEST(Perl, TheSingleCodeUnitEscapeIsRefusedInBothDialects) {
  // `\C` matches one *code unit* in PCRE2, and it is the only construct in
  // the `pcre` dialect that pcre2test accepts and this library refuses. The
  // reason is what it would mean here: under `u` over "é" pcre2test matches
  // 0:1, which is half a character, and this library's spans are byte offsets
  // into UTF-8 where a match may not end inside one (design.md section 2).
  //
  // perl refuses it too, for its own reason - "\C no longer supported in
  // regex" since 5.24 - so the refusal is only a gap against PCRE2, and
  // tools/oracle/perl_diff.py counts those rows rather than dropping them.
  for (const char * pattern : {"\\C", "a\\Cb", "\\C+"}) {
    for (GRX_Syntax syntax : {GRX_SYNTAX_PERL, GRX_SYNTAX_PCRE}) {
      Attempt attempt = compile(pattern, syntax);
      EXPECT_EQ(attempt.result, GRX_ERR_UNSUPPORTED) << pattern;
      EXPECT_EQ(attempt.diag, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED) << pattern;
      grx_regex_free(attempt.regex);
    }
  }

  // Lower case `\c` is the control escape and is unaffected, which is what
  // says the refusal is keyed to the letter and not to the pair.
  EXPECT_TRUE(search("\\cA", "\x01").matched);
}

// --------------------------------------------------------------------------
// The constructs the backtracker had to grow
// --------------------------------------------------------------------------

TEST(Perl, APossessiveRepeatIsAnAtomicOne) {
  // pcre2pattern says `a*+` is "equivalent to (?>a*)" in those words, and the
  // difference is visible in one pattern: `a++a` can never match, because the
  // possessive run gives nothing back for the second `a`.
  EXPECT_FALSE(search("a++a", "aaaaa").matched);
  EXPECT_FALSE(search("a*+a", "aaaaa").matched);
  EXPECT_FALSE(search("a{1,5}+a", "aaaaa").matched);
  EXPECT_TRUE(search("a++b", "aaab").matched);
  // `a?+a` *does* match "aa": the possessive `a?` takes one character and
  // the second `a` takes the other, so there is nothing to give back.
  EXPECT_TRUE(search("a?+a", "aa").matched);

  // The greedy form does give it back.
  EXPECT_TRUE(search("a+a", "aaaaa").matched);
}

TEST(Perl, KeepMovesTheReportedStartAndIsUndoneByBacktracking) {
  Found kept = search("ab\\Kcd", "abcd");
  EXPECT_TRUE(kept.matched);
  EXPECT_EQ(kept.start, 2u) << "\\K moves the reported start past `ab`";
  EXPECT_EQ(kept.end, 4u);

  // The branch that used `\K` fails, and the one that matches must report
  // from where it really began.
  Found other = search("(a\\Kb|ac)", "ac");
  EXPECT_TRUE(other.matched);
  EXPECT_EQ(other.start, 0u);

  // pcre2test refuses it inside a lookaround: a lookaround consumes nothing,
  // so there is no reported start for `\K` to move.
  Attempt inside = compile("(?=a\\Kb)ab");
  EXPECT_NE(inside.result, GRX_OK);
  EXPECT_EQ(inside.diag, GRX_DIAG_INVALID_LOOKAROUND);
  grx_regex_free(inside.regex);
}

TEST(Perl, AcceptEndsTheMatchAndClosesTheGroupsThatAreOpen) {
  // pcre2pattern: the match succeeds there, and any capturing parentheses
  // that are open are closed. Without the second half this pattern reports
  // no groups at all, where both references report two.
  EXPECT_EQ(group_of("(A(A|B(*ACCEPT)|C)D)(E)", "AB", 0), "0-2");
  EXPECT_EQ(group_of("(A(A|B(*ACCEPT)|C)D)(E)", "AB", 1), "0-2");
  EXPECT_EQ(group_of("(A(A|B(*ACCEPT)|C)D)(E)", "AB", 2), "1-2");
}

TEST(Perl, PcreRepeatsOnlyAcceptAndPerlRepeatsEveryVerb) {
  // Under PCRE2, `(*ACCEPT)*` compiles and `(*FAIL)*` does not, which is not
  // an inconsistency: `(*ACCEPT)` is the one verb that ends the match where
  // it stands, so a quantifier on it is unreachable rather than meaningless.
  Attempt accept = compile("a(*ACCEPT)*b");
  EXPECT_EQ(accept.result, GRX_OK);
  grx_regex_free(accept.regex);

  Attempt fail = compile("a(*FAIL)*b");
  EXPECT_NE(fail.result, GRX_OK);
  EXPECT_EQ(fail.diag, GRX_DIAG_NOTHING_TO_REPEAT);
  grx_regex_free(fail.regex);

  // perl quantifies every one of them, as it quantifies `\K` and the
  // anchors - the third rule in pcre_check_quantifier_target() of that
  // shape. This half was missing until tools/oracle/perl_diff.py generated
  // `(*FAIL)*` and perl matched it; no imported corpus has a repeated verb
  // outside the pcre2 files, which is why the rule had only pcre2's half.
  for (const char * pattern :
      {"(*FAIL)*", "(*PRUNE)*", "(*SKIP)*", "(*COMMIT)*", "(*THEN)*",
       "(*MARK:x)*", "(*:x)*", "(*ACCEPT)*"}) {
    Attempt perl = compile(pattern, GRX_SYNTAX_PERL);
    EXPECT_EQ(perl.result, GRX_OK) << pattern;
    grx_regex_free(perl.regex);
  }

  // And the repeat is taken zero times, so the verb never fires: perl
  // matches "ab" against `a(*FAIL)*b` where the same pattern without the
  // quantifier cannot match at all.
  EXPECT_EQ(span_of("a(*FAIL)*b", "ab", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("a(*FAIL)b", "ab", GRX_SYNTAX_PERL), "nomatch");
}

TEST(Perl, CommitStopsTheSearchWherePruneOnlyStopsTheAttempt) {
  // Neither does anything when it is reached: "xab" matches both, because
  // the first attempt fails on `a` before the verb is ever executed.
  EXPECT_TRUE(search("a(*COMMIT)b", "xab").matched);
  EXPECT_TRUE(search("a(*PRUNE)b", "xab").matched);

  // "axab" is where they part. The attempt from offset 0 passes the verb and
  // then fails on `b`, so backtracking returns to it: `(*PRUNE)` abandons
  // that starting position and the search moves on to find "ab" at 2;
  // `(*COMMIT)` abandons the search. Both verdicts are pcre2test's.
  EXPECT_TRUE(search("a(*PRUNE)b", "axab").matched);
  EXPECT_FALSE(search("a(*COMMIT)b", "axab").matched);
}

TEST(Perl, AConditionalRunsOneBranchAndNeverTheOther) {
  // `(?(1)yes|no)` on whether group 1 participated.
  EXPECT_TRUE(search("^(a)?(?(1)b|c)$", "ab").matched);
  EXPECT_TRUE(search("^(a)?(?(1)b|c)$", "c").matched);
  EXPECT_FALSE(search("^(a)?(?(1)b|c)$", "ac").matched);

  // An assertion as the condition. `(?(?=a)b|c)` against "ac" must fail:
  // the condition holds, `b` does not match, and the else-branch is not
  // reachable by backtracking - which is what makes this a conditional and
  // not an alternation.
  EXPECT_FALSE(search("^(?(?=a)b|c)", "ac").matched);
  EXPECT_TRUE(search("^(?(?=a)ab|c)", "ab").matched);
  EXPECT_TRUE(search("^(?(?=a)ab|c)", "c").matched);

  // Three branches is an error in both references.
  Attempt three = compile("(?(1)a|b|c)");
  EXPECT_NE(three.result, GRX_OK);
  grx_regex_free(three.regex);
}

TEST(Perl, AnAssertionConditionRunsOnce) {
  // It used to run twice. `(?(?=A)X|Y)` was lowered as
  // `(?:(?=A)X|(?!A)Y)` - exact, and two copies of A, so A ran again
  // whenever it failed. The single-run form is one assertion that chooses
  // a branch instead of failing (GRX_INST_COND_ELSE).
  //
  // The behaviour is unchanged and the *program* is the evidence, so this
  // counts instructions: the body `abc` appears once, and a rewrite that
  // duplicated it would show up here however the branches were laid out.
  Attempt attempt = compile("(?(?=abc)x|y)");
  ASSERT_EQ(attempt.result, GRX_OK);
  GRX_Facts facts;
  grx_facts_init(&facts);
  ASSERT_EQ(grx_regex_facts(attempt.regex, &facts), GRX_OK);
  // save, look, a, b, c, match, jmp, jmp, x, jmp, y, save, match.
  EXPECT_EQ(facts.program_size, 13u);
  grx_regex_free(attempt.regex);
}

TEST(Perl, AnAssertionConditionsFactsAreTheBranchesAndNotTheCondition) {
  // The condition is zero-width where it stands, so the lengths are the
  // *branches'* - and the condition is still walked, because what it
  // contains reaches the facts. Both halves are here because the single-run
  // conditional made the condition a child of the COND node rather than
  // part of each branch, and an analysis that folded it in with the
  // branches would answer every one of these too small.
  struct {
    const char * pattern;
    size_t min;
    size_t max;
    size_t lookbehind;
    bool can_be_empty;
  } cases[] = {
    {"(?(?=a)bbb|cc)", 2, 3, 0, false},
    // No else-part, so one path through it is empty.
    {"(?(?=a)b)", 0, 1, 0, true},
    // A lookbehind *inside* the condition still has to be measured, or the
    // one around it cannot be. This is the case `(void)walk()` exists for.
    {"(?<=x(?(?=(?<=ab))c|d))z", 1, 1, 2, false},
    {"(?(?=abc)xy|z)", 1, 2, 0, false},
  };

  for (const auto & test : cases) {
    Attempt attempt = compile(test.pattern);
    ASSERT_EQ(attempt.result, GRX_OK) << test.pattern;
    GRX_Facts facts;
    grx_facts_init(&facts);
    ASSERT_EQ(grx_regex_facts(attempt.regex, &facts), GRX_OK) << test.pattern;
    EXPECT_EQ(facts.min_length, test.min) << test.pattern << " min";
    EXPECT_EQ(facts.max_length, test.max) << test.pattern << " max";
    EXPECT_EQ(facts.max_lookbehind, test.lookbehind)
        << test.pattern << " lookbehind";
    EXPECT_EQ(facts.can_match_empty != 0, test.can_be_empty)
        << test.pattern << " can match empty";
    grx_regex_free(attempt.regex);
  }

  // Anchoring comes from the branches too, and holds only where every one
  // of them says so. `\z` rather than `$`, because PCRE2's `$` also holds
  // before a final newline and so anchors nothing.
  Attempt anchored = compile("^(?(?=a)bbb|cc)");
  ASSERT_EQ(anchored.result, GRX_OK);
  GRX_Facts facts;
  grx_facts_init(&facts);
  ASSERT_EQ(grx_regex_facts(anchored.regex, &facts), GRX_OK);
  EXPECT_TRUE(facts.anchored_start);
  grx_regex_free(anchored.regex);
}

TEST(Perl, WhatAFailedAssertionConditionCapturedFollowsTheDialect) {
  // The same split as section 5.17's negative lookaround, reached by a
  // different route: when a conditional's assertion *fails*, what its body
  // wrote is kept by Perl and discarded by PCRE2.
  //
  // `^(?(?=(a)b)x|a)` against "ay": the condition captures "a" into group
  // one and then fails on the `b`, so the else-branch matches the `a`.
  // perl 5.40.1 reports group one as "a"; pcre2test reports it unset, and
  // prints `1: <unset>` in as many words for `^(?(?=(a)b)ab|a)(.*)`.
  //
  // This was wrong for Perl until the single-run conditional was built,
  // and nothing noticed: the old rewrite carried the rule on the *second*
  // copy of the assertion, which it made negative by flipping the mode
  // after lowering - and lower_look() sets the keep-captures flag only for
  // an assertion written negative. A flag set by the spelling and a mode
  // changed afterwards is a pair that comes apart silently.
  EXPECT_EQ(group_of("^(?(?=(a)b)x|a)", "ay", 1, GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(group_of("^(?(?=(a)b)x|a)", "ay", 1, GRX_SYNTAX_PCRE), "-");

  // A condition written *negative* is the same question with the signs
  // swapped, and both references answer it the same way: the body failed,
  // so the dialect decides. `^(?(?!(a)b)a|x)` against "ay".
  EXPECT_EQ(group_of("^(?(?!(a)b)a|x)", "ay", 1, GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(group_of("^(?(?!(a)b)a|x)", "ay", 1, GRX_SYNTAX_PCRE), "-");

  // And where the condition *holds*, its writes stand in both: a positive
  // assertion that matched keeps what it captured everywhere.
  EXPECT_EQ(group_of("^(?(?=(a)b)ab|c)", "ab", 1, GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(group_of("^(?(?=(a)b)ab|c)", "ab", 1, GRX_SYNTAX_PCRE), "0-1");
}

TEST(Perl, ADefineBlockIsNeverEnteredInSequenceAndIsStillCallable) {
  // `(?(DEFINE)(a))b(?1)c` matches "bac" and not "abac": the body defines
  // group 1 and runs only when something calls it.
  EXPECT_TRUE(search("^(?(DEFINE)(A))B(?1)C$", "BAC").matched);
  EXPECT_FALSE(search("^(?(DEFINE)(A))B(?1)C$", "ABAC").matched);

  // By name, and with the definition after the call.
  EXPECT_TRUE(search("^(?&t)$(?(DEFINE)(?<t>ab))", "ab").matched);
}

TEST(Perl, RecursionReEntersAGroupAndRestoresWhatItCaptured) {
  // The balanced-parenthesis pattern, which is what recursion is for.
  EXPECT_TRUE(search("^(\\((?:[^()]|(?1))*\\))$", "(a(b)c)").matched);
  EXPECT_FALSE(search("^(\\((?:[^()]|(?1))*\\))$", "(a(bc)").matched);

  // `(?R)` is the whole pattern, anchors included - which is why this does
  // *not* match "aabb": the recursive call has to satisfy the `^` and `$`
  // too. pcre2test agrees, and the first version of this test did not.
  EXPECT_FALSE(search("^(a(?R)?b)$", "aabb").matched);
  EXPECT_TRUE(search("^(a(?R)?b)$", "ab").matched);
  EXPECT_TRUE(search("(a(?R)?b)", "xaabbx").matched);

  // pcre2pattern: the values a call captured are reset to what they were
  // before it. Group 1 keeps the outer "a", not the "b" the call found.
  EXPECT_EQ(group_of("^(a|b)(?1)$", "ab", 1), "0-1");

  // A call can be backtracked into, in both dialects. This was a profile
  // axis - "pcre2pattern says a recursive call is treated as an atomic
  // group" - and pcre2test 10.46 does not do that: `^(a|ab)(?1)b$` against
  // "aabb" matches there and in perl, which it can only do by going back
  // into the call for its second alternative, and `aa$|a(?R)a|a` against
  // "aaa" is the whole string in both where an atomic call gives one
  // character. Measured 2026-09-24 in four spellings - `(?R)`, `(?1)`,
  // `(?-1)` and a call from inside another group - and the axis went with
  // the measurement.
  EXPECT_EQ(span_of("^(a|ab)(?1)b$", "aabb", GRX_SYNTAX_PCRE), "0-4");
  EXPECT_EQ(span_of("^(a|ab)(?1)b$", "aabb", GRX_SYNTAX_PERL), "0-4");
  EXPECT_EQ(span_of("^(a|ab)(?-1)b$", "aabb", GRX_SYNTAX_PCRE), "0-4");
  EXPECT_EQ(span_of("^((a|ab)(?2)b)$", "aabb", GRX_SYNTAX_PCRE), "0-4");
  EXPECT_EQ(span_of("aa$|a(?R)a|a", "aaa", GRX_SYNTAX_PCRE), "0-3");
  EXPECT_EQ(span_of("aa$|a(?R)a|a", "aaa", GRX_SYNTAX_PERL), "0-3");

  // The row a soak seed found, where an atomic call took the wrong path
  // and then could not give it back: pcre2test and perl both report the
  // five-character match with group two holding "bab".
  EXPECT_EQ(span_of("(a|ab)*(a|b(?1))a", "ababaaa", GRX_SYNTAX_PCRE), "0-5");
  EXPECT_EQ(group_of("(a|ab)*(a|b(?1))a", "ababaaa", 2, GRX_SYNTAX_PCRE),
      "1-4");
}

TEST(Perl, ABranchResetNumbersEveryBranchFromTheSameBase) {
  // `(?|(a)|(b))` has one capturing group, not two, and the count the
  // pattern ends with is the largest any branch reached.
  Attempt reset = compile("^X(?|(a)|(b))(c)Y$");
  ASSERT_EQ(reset.result, GRX_OK);
  EXPECT_EQ(grx_regex_capture_count(reset.regex), 2u);
  grx_regex_free(reset.regex);

  EXPECT_EQ(group_of("^(?|(a)|(b))$", "b", 1), "0-1");
}

// --------------------------------------------------------------------------
// Classes and the sets an escape names
// --------------------------------------------------------------------------

TEST(Perl, ABracketedPosixClassIsCheckedTheWayPcre2ChecksIt) {
  EXPECT_TRUE(search("^[[:digit:]]+$", "123").matched);
  EXPECT_TRUE(search("^[[:^digit:]]+$", "abc").matched);

  // `[:` followed by a terminator that is reached is POSIX syntax, and then
  // the name must be one that exists. `[[:foo:]]` is an error and `[[:]` is
  // a class of two characters - the distinction is pcre2_compile.c's
  // check_posix_syntax(), transcribed.
  Attempt unknown = compile("[[:foo:]]");
  EXPECT_NE(unknown.result, GRX_OK);
  EXPECT_EQ(unknown.diag, GRX_DIAG_UNKNOWN_POSIX_CLASS);
  grx_regex_free(unknown.regex);

  Attempt digits = compile("[[:1234:]]");
  EXPECT_NE(digits.result, GRX_OK) << "POSIX syntax with a name nobody has";
  grx_regex_free(digits.regex);

  // Collating elements and equivalence classes need a locale, and there is
  // none here - but a *syntax* error rather than an unsupported one, because
  // neither reference has them either: pcre2test says "POSIX collating
  // elements are not supported" and perl says the syntax "is reserved for
  // future extensions". The pattern is not one this library has yet to
  // build; it is one that will never be valid in this dialect, and the two
  // are different promises to a caller.
  Attempt collating = compile("[[.a.]]");
  EXPECT_EQ(collating.result, GRX_ERR_SYNTAX);
  EXPECT_EQ(collating.diag, GRX_DIAG_NOT_IN_DIALECT);
  grx_regex_free(collating.regex);

  Attempt equivalence = compile("[[=a=]]");
  EXPECT_EQ(equivalence.result, GRX_ERR_SYNTAX);
  grx_regex_free(equivalence.regex);
}

TEST(Perl, AConditionsConditionHasToBeAnAssertion) {
  // pcre2test: "atomic assertion expected after (?( or (?(?C)". The
  // alphabetic spellings of the lookarounds are conditions; the other
  // `(*name:` constructs are groups and are not, however useful they would
  // be. Checked against pcre2test 10.46 one form at a time.
  for (const char * pattern :
      {"(?(*pla:a)b)", "(?(*nlb:a)b)", "(?(*positive_lookahead:a)b)",
       "(?(?=a)b)"}) {
    Attempt good = compile(pattern);
    EXPECT_EQ(good.result, GRX_OK) << pattern;
    grx_regex_free(good.regex);
  }

  for (const char * pattern :
      {"(?(*atomic:a)b)", "(?(*script_run:a)b)", "(?(*sr:a)b)",
       "(?(*scs:(1)a)b)"}) {
    Attempt bad = compile(pattern);
    EXPECT_EQ(bad.result, GRX_ERR_SYNTAX) << pattern;
    EXPECT_EQ(bad.diag, GRX_DIAG_INVALID_CONDITION) << pattern;
    grx_regex_free(bad.regex);
  }

  // Outside a conditional the same construct compiles, which is the point
  // of the pair: what a conditional refuses is an assertion it cannot use,
  // not a construct this library lacks.
  Attempt alone = compile("(*script_run:abc)");
  EXPECT_EQ(alone.result, GRX_OK);
  grx_regex_free(alone.regex);
}

TEST(Perl, ADuplicatedNameCannotBoundALookbehind) {
  // `(?J)` lets one name belong to several groups, and a reference written
  // with it means all of them - so its length is several lengths and a
  // dialect that bounds its lookbehind cannot take it. pcre2test refuses
  // this and compiles the same lookbehind over a name written once.
  Attempt ambiguous
      = compile("(?J)(?<A>[ab])...(?<=\\k'A')(?<A>)z");
  EXPECT_EQ(ambiguous.result, GRX_ERR_SYNTAX);
  grx_regex_free(ambiguous.regex);

  Attempt single = compile("(?<A>[ab])...(?<=\\k'A')z");
  EXPECT_EQ(single.result, GRX_OK);
  grx_regex_free(single.regex);
}

TEST(Perl, ALeadingCloseBracketIsAMemberAndNotTheEndOfTheClass) {
  // The rule that makes `[]` unterminated rather than empty, and the one
  // ECMAScript does not share.
  EXPECT_TRUE(search("^[]]$", "]").matched);
  EXPECT_TRUE(search("^[^]]$", "a").matched);

  Attempt empty = compile("[]");
  EXPECT_NE(empty.result, GRX_OK);
  EXPECT_EQ(empty.diag, GRX_DIAG_UNMATCHED_OPEN_BRACKET);
  grx_regex_free(empty.regex);
}

TEST(Perl, AClassEscapeMayNotEndARange) {
  // pcre2test: "invalid range in character class". PCRE2 refuses it where
  // Perl warns and takes the `-` as a literal, which is one of the places
  // the two dialects genuinely differ.
  Attempt range = compile("[\\d-z]");
  EXPECT_NE(range.result, GRX_OK);
  EXPECT_EQ(range.diag, GRX_DIAG_CLASS_ESCAPE_IN_RANGE);
  grx_regex_free(range.regex);

  // A `-` whose other end is the closing bracket is a member.
  EXPECT_TRUE(search("^[\\d-]$", "-").matched);
}

TEST(Perl, AQuotedRunInsideAClassStillEndsARange) {
  // `[\Qa\E-z]` is the range a to z: the `\E` disappears before the range is
  // decided, not after.
  EXPECT_TRUE(search("^[\\Qa\\E-z]$", "m").matched);
  // And `[\Qa-z\E]` is three characters, because the `-` is inside the run.
  EXPECT_FALSE(search("^[\\Qa-z\\E]$", "m").matched);
  EXPECT_TRUE(search("^[\\Qa-z\\E]$", "-").matched);
}

TEST(Perl, TheNewlineEscapeIsASetOfStringsAndIsAtomic) {
  // pcre2pattern writes `\R` as `(?>\r\n|\n|...)`, and the `(?>` is not
  // decoration: without it `\R\n` would match "\r\n\n" by giving the LF back.
  EXPECT_TRUE(search("^\\R$", "\r\n").matched);
  EXPECT_TRUE(search("^\\R$", "\n").matched);
  EXPECT_FALSE(search("^\\R\\n$", "\r\n").matched);

  EXPECT_FALSE(search("^\\R$", "a").matched);
}

TEST(Perl, TheNotNewlineEscapeIgnoresDotAll) {
  // `\N` is "any character that is not a newline", full stop: `(?s)\N` still
  // refuses one, where `(?s).` accepts it.
  EXPECT_TRUE(search("^(?s).$", "\n").matched);
  EXPECT_FALSE(search("^(?s)\\N$", "\n").matched);
  EXPECT_TRUE(search("^\\N$", "a").matched);
}

TEST(Perl, EveryPosixClassNameResolves) {
  // All fourteen, because the table that maps them is the kind that acquires
  // a hole nobody notices: a name the parser accepts and lowering cannot
  // build reports an internal fault, not a syntax error.
  struct { const char * name; const char * in; const char * out; } cases[] = {
    {"alnum", "a", "-"}, {"alpha", "z", "1"}, {"ascii", "A", "\xC3\xA9"},
    {"blank", " ", "x"}, {"cntrl", "\x01", "x"}, {"digit", "5", "x"},
    {"graph", "!", " "}, {"lower", "q", "Q"}, {"print", " ", "\x01"},
    {"punct", ",", "a"}, {"space", "\t", "a"}, {"upper", "Q", "q"},
    {"word", "_", "-"}, {"xdigit", "f", "g"},
  };

  for (const auto & row : cases) {
    std::string pattern = std::string("^[[:") + row.name + ":]]$";
    EXPECT_TRUE(search(pattern, row.in).matched) << pattern << " vs " << row.in;
    EXPECT_FALSE(search(pattern, row.out).matched)
        << pattern << " vs " << row.out;
  }
}

TEST(Perl, UcpWidensThePosixClassesItCanAndRefusesTheOnesItCannot) {
  // With `(*UCP)` the classes are Unicode's, not ASCII's.
  EXPECT_TRUE(search("(*UTF)(*UCP)^[[:alpha:]]$", "\xC3\xA9").matched);
  EXPECT_FALSE(search("(*UTF)^[[:alpha:]]$", "\xC3\xA9").matched);

  // `blank` keeps the tab, which `Zs` does not contain.
  EXPECT_TRUE(search("(*UTF)(*UCP)^[[:blank:]]$", "\t").matched);
  EXPECT_TRUE(search("(*UTF)(*UCP)^[[:blank:]]$", "\xE2\x80\x83").matched);

  // `graph` and `print` are built by complement - everything that is neither
  // a separator nor a control or unassigned code point.
  EXPECT_TRUE(search("(*UTF)(*UCP)^[[:graph:]]$", "\xC3\xA9").matched);
  EXPECT_FALSE(search("(*UTF)(*UCP)^[[:graph:]]$", " ").matched);
  EXPECT_TRUE(search("(*UTF)(*UCP)^[[:print:]]$", " ").matched);

  // `xdigit` widens to exactly 44 code points: the 22 ASCII hexadecimal
  // digits and their fullwidth forms, and nothing else. Swept a code point
  // at a time over all 1,112,064 in pcre2test 10.46 and in perl - same
  // count, same six ranges - because no property names this set and a
  // written-out constant has to be checked against something.
  //
  // It answered the ASCII 22 under UCP until 2026-09-24, which is the
  // failure mode this class of code has: not a refusal, not a wrong
  // property, just the narrow table falling through with nothing to widen
  // it and a plausible answer coming out.
  EXPECT_TRUE(search("(*UTF)(*UCP)^[[:xdigit:]]$", "f").matched);
  EXPECT_TRUE(search("(*UTF)(*UCP)^[[:xdigit:]]$", "\xEF\xBC\x90").matched);
  EXPECT_TRUE(search("(*UTF)(*UCP)^[[:xdigit:]]$", "\xEF\xBC\xA6").matched);
  EXPECT_TRUE(search("(*UTF)(*UCP)^[[:xdigit:]]$", "\xEF\xBD\x86").matched);
  // One past the end of each of the two letter ranges: U+FF27 is fullwidth
  // "G" and U+FF47 is fullwidth "g". Both boundaries, because a table
  // written out by hand is exactly where an off-by-one lives - the first
  // draft of this test asked about U+FF06 and caught its own typo.
  EXPECT_FALSE(search("(*UTF)(*UCP)^[[:xdigit:]]$", "\xEF\xBC\xA7").matched);
  EXPECT_FALSE(search("(*UTF)(*UCP)^[[:xdigit:]]$", "\xEF\xBD\x87").matched);
  // And U+0661, which `[[:digit:]]` takes under UCP and this does not.
  EXPECT_FALSE(search("(*UTF)(*UCP)^[[:xdigit:]]$", "\xD9\xA1").matched);
  EXPECT_TRUE(search("(*UTF)(*UCP)^[[:digit:]]$", "\xD9\xA1").matched);

  // The control: without UCP it is the ASCII 22 again.
  EXPECT_FALSE(search("(*UTF)^[[:xdigit:]]$", "\xEF\xBC\x90").matched);
  EXPECT_TRUE(search("(*UTF)^[[:xdigit:]]$", "f").matched);
}

TEST(Perl, TheWidePosixClassesWereSweptAgainstBothReferences) {
  // Every one of the fourteen names asked about all 1,112,064 code points
  // in pcre2test 10.46 and in perl, against this library, on 2026-09-24.
  // Three rules were wrong, and none of them could be seen from the
  // conformance corpus: 37,212 cases hold no letter-number, no format
  // character inside `[[:graph:]]`, and no non-ASCII symbol.

  // 1. `[[:punct:]]` is `\p{P}` plus exactly nine ASCII symbols - the same
  // nine in both references - and was `\p{P}` plus the whole of `\p{S}`,
  // which is 7,766 code points neither of them has.
  EXPECT_TRUE(search("(*UTF)(*UCP)^[[:punct:]]$", "$").matched);
  EXPECT_TRUE(search("(*UTF)(*UCP)^[[:punct:]]$", "~").matched);
  EXPECT_TRUE(search("(*UTF)(*UCP)^[[:punct:]]$", "\xE2\x80\x94").matched);
  EXPECT_FALSE(search("(*UTF)(*UCP)^[[:punct:]]$", "\xC2\xA2").matched);
  EXPECT_FALSE(search("(*UTF)(*UCP)^[[:punct:]]$", "\xE2\x81\x84").matched);
  EXPECT_FALSE(search("(*UTF)(*UCP)^[[:punct:]]$", "\xE2\x98\x83").matched);

  // 2. `[[:graph:]]` and `[[:print:]]` keep the format characters. The
  // excluded set is the four other `C` categories, not `C`.
  EXPECT_TRUE(search("(*UTF)(*UCP)^[[:graph:]]$", "\xC2\xAD").matched);
  EXPECT_TRUE(search("(*UTF)(*UCP)^[[:graph:]]$", "\xE2\x80\x8B").matched);
  EXPECT_TRUE(search("(*UTF)(*UCP)^[[:print:]]$", "\xEF\xBB\xBF").matched);
  EXPECT_FALSE(search("(*UTF)(*UCP)^[[:graph:]]$", "\x01").matched);
  EXPECT_FALSE(search("(*UTF)(*UCP)^[[:graph:]]$", "\xE2\x80\x83").matched);

  // 3. `\w`, `[[:word:]]` and `\b` take the letter-numbers. UTS #18 Annex C
  // spells the first term `\p{alpha}`, which is `Alphabetic` and not `L`:
  // `Alphabetic` carries `Nl`, and using `L` dropped 236 code points that
  // both references keep.
  const char * roman_one = "\xE2\x85\xA0"; // U+2160, gc=Nl
  EXPECT_TRUE(search("^\\w$", roman_one, GRX_SYNTAX_PERL).matched);
  EXPECT_TRUE(search("^[[:word:]]$", roman_one, GRX_SYNTAX_PERL).matched);
  EXPECT_TRUE(search("^[[:alpha:]]$", roman_one, GRX_SYNTAX_PERL).matched);
  // `\b` is defined from `\w`, so it moved with it: there is no boundary
  // between a Roman numeral and a letter now, and there was one.
  EXPECT_FALSE(search("\\bx", "\xE2\x85\xA0x", GRX_SYNTAX_PERL).matched);
  EXPECT_TRUE(search("\\bx", " x", GRX_SYNTAX_PERL).matched);
  // And the narrowing still reaches it, which is what says the set moved
  // rather than the test being satisfied by something wider.
  EXPECT_FALSE(search("(*UTF)(*UCP)(?aW)^\\w$", roman_one).matched);
  EXPECT_FALSE(search("(*UTF)^\\w$", roman_one).matched);
}

// The wide classes are not one set shared by two dialects, and four of the
// fourteen names plus `\w` and `\h` are where that shows. PCRE2 spells its
// wide classes with general categories and perl with the derived properties
// UTS #18 names; PCRE2 also keeps U+180E a space, which it was until
// Unicode 6.3, and perl does not.
//
// Every row was measured a code point at a time against pcre2test 10.46 and
// against perl on 2026-09-24, over the 286,719 code points those two and UCD
// 17.0.0 all call assigned. Restricting to that intersection is the whole
// reason the figures mean anything: unrestricted, this library knows 4,803
// code points pcre2 has never heard of and the comparison measures the
// Unicode release instead of the rule. With the profile axes set, all 28
// dialect/name pairs and all 16 dialect/shorthand pairs agree, less four
// known version artifacts - U+0295, `Ll` here and `Lo` in UCD 17.0.0, and
// 33 combining Latin letters that gained Other_Alphabetic in 17.0.0. node,
// whose Unicode is also 17.0, sides with this library on all 34.
TEST(Perl, TheWideClassesAreEachDialectsOwnAndNotOneSharedSet) {
  struct {
    const char * label;
    const char * subject;
    const char * pattern;
    bool pcre;
    bool perl;
  } cases[] = {
    // `[[:alpha:]]` is `\p{L}` in PCRE2 and `\p{Alphabetic}` in perl, so
    // every member of Other_Alphabetic parts them: a letter-number, a
    // circled letter, a spacing mark.
    {"U+2160 Nl", "\xE2\x85\xA0", "^[[:alpha:]]$", false, true},
    {"U+24B6 So", "\xE2\x92\xB6", "^[[:alpha:]]$", false, true},
    {"U+0903 Mc", "\xE0\xA4\x83", "^[[:alpha:]]$", false, true},
    // ...and a titlecase letter and a modifier letter are `L`, so they are
    // in both and would pass whichever spelling were used. Present so that
    // the rows above are known to be about the *property* and not about
    // this library refusing everything unusual.
    {"U+01C5 Lt", "\xC7\x85", "^[[:alpha:]]$", true, true},
    {"U+02B0 Lm", "\xCA\xB0", "^[[:alpha:]]$", true, true},
    // `[[:alnum:]]` is `L` plus `N` there, so a letter-number is alnum in
    // PCRE2 while not being alpha - the one place the two names part.
    {"U+2160 alnum", "\xE2\x85\xA0", "^[[:alnum:]]$", true, true},
    {"U+24B6 alnum", "\xE2\x92\xB6", "^[[:alnum:]]$", false, true},
    // `[[:lower:]]`/`[[:upper:]]` are `Ll`/`Lu` there and Lowercase/
    // Uppercase here: Other_Lowercase carries the modifier letters and the
    // small Roman numerals, Other_Uppercase the circled capitals.
    {"U+02B0 lower", "\xCA\xB0", "^[[:lower:]]$", false, true},
    {"U+2170 lower", "\xE2\x85\xB0", "^[[:lower:]]$", false, true},
    {"U+24B6 upper", "\xE2\x92\xB6", "^[[:upper:]]$", false, true},
    {"U+2160 upper", "\xE2\x85\xA0", "^[[:upper:]]$", false, true},
    {"U+00E9 lower", "\xC3\xA9", "^[[:lower:]]$", true, true},
    {"U+00C9 upper", "\xC3\x89", "^[[:upper:]]$", true, true},
    // `\w` is `\p{L}\p{N}\p{Mn}\p{Pc}` there and Annex C's here. `No` is
    // PCRE2's and not perl's; `Mc`, `Me` and the alphabetic `So` are
    // perl's and not PCRE2's. Neither set contains the other.
    {"U+00B2 No", "\xC2\xB2", "^\\w$", true, false},
    {"U+0903 Mc", "\xE0\xA4\x83", "^\\w$", false, true},
    {"U+24B6 So", "\xE2\x92\xB6", "^\\w$", false, true},
    {"U+200C JC", "\xE2\x80\x8C", "^\\w$", false, true},
    {"U+0301 Mn", "\xCC\x81", "^\\w$", true, true},
    {"U+203F Pc", "\xE2\x80\xBF", "^\\w$", true, true},
    // `[[:word:]]` is the same set as `\w` in both references, so every row
    // above holds for it too. Two of them here, because an axis that
    // reached one spelling and not the other would pass all of the above.
    {"U+00B2 word", "\xC2\xB2", "^[[:word:]]$", true, false},
    {"U+0903 word", "\xE0\xA4\x83", "^[[:word:]]$", false, true},
    // `[[:graph:]]` takes private use in perl and not in PCRE2 - 137,468
    // code points, the widest disagreement between them anywhere here -
    // and PCRE2 drops six `Cf` characters that perl keeps.
    {"U+E000 Co", "\xEE\x80\x80", "^[[:graph:]]$", false, true},
    {"U+E000 print", "\xEE\x80\x80", "^[[:print:]]$", false, true},
    {"U+2066 LRI", "\xE2\x81\xA6", "^[[:graph:]]$", false, true},
    {"U+061C ALM", "\xD8\x9C", "^[[:graph:]]$", false, true},
    {"U+00AD SHY", "\xC2\xAD", "^[[:graph:]]$", true, true},
    // U+180E is the six-character list's odd one: PCRE2 drops it from
    // `graph` and keeps it in `print`, because `print` is `graph` plus the
    // space separators and PCRE2 still classes it as one.
    {"U+180E graph", "\xE1\xA0\x8E", "^[[:graph:]]$", false, true},
    {"U+180E print", "\xE1\xA0\x8E", "^[[:print:]]$", true, true},
    // ...and that same classing reaches four space spellings in PCRE2 and
    // none in perl. `\v` takes it in neither, which is what says this is
    // one code point's category and not a blanket widening.
    {"U+180E \\h", "\xE1\xA0\x8E", "^\\h$", true, false},
    {"U+180E \\s", "\xE1\xA0\x8E", "^\\s$", true, false},
    {"U+180E blank", "\xE1\xA0\x8E", "^[[:blank:]]$", true, false},
    {"U+180E space", "\xE1\xA0\x8E", "^[[:space:]]$", true, false},
    {"U+180E \\v", "\xE1\xA0\x8E", "^\\v$", false, false},
    {"U+00A0 \\h", "\xC2\xA0", "^\\h$", true, true},
  };

  for (const auto & row : cases) {
    // PCRE2 needs both verbs; perl's shorthands are Unicode either way, and
    // its subject is text in every mode.
    std::string pcre_pattern = std::string("(*UTF)(*UCP)") + row.pattern;
    EXPECT_EQ(search(pcre_pattern, row.subject, GRX_SYNTAX_PCRE).matched,
        row.pcre) << row.label << "  " << row.pattern << "  PCRE";
    EXPECT_EQ(search(row.pattern, row.subject, GRX_SYNTAX_PERL).matched,
        row.perl) << row.label << "  " << row.pattern << "  PERL";
  }

  // `\b` is defined from `\w`, so the word-set axis has to reach it. U+00B2
  // is a word character in PCRE2 and not in perl, so "a²" holds a boundary
  // in one dialect and not the other - and a set that moved while `\b` kept
  // its own copy would answer the same in both.
  EXPECT_FALSE(search("(*UTF)(*UCP)a\\b", "a\xC2\xB2", GRX_SYNTAX_PCRE).matched);
  EXPECT_TRUE(search("a\\b", "a\xC2\xB2", GRX_SYNTAX_PERL).matched);
  // ...and the other way round, so neither answer is "no boundary ever".
  EXPECT_TRUE(search("(*UTF)(*UCP)a\\b", "a\xE0\xA4\x83", GRX_SYNTAX_PCRE).matched);
  EXPECT_FALSE(search("a\\b", "a\xE0\xA4\x83", GRX_SYNTAX_PERL).matched);

  // Python is the third word set, and the one neither of the other two can
  // reach: `re`'s `\w` is `isalnum` plus `_`, so it takes `No` with PCRE2
  // and refuses every mark, and refuses the connectors that are not `_`.
  EXPECT_TRUE(search("^\\w$", "\xC2\xB2", GRX_SYNTAX_PYTHON).matched);
  EXPECT_FALSE(search("^\\w$", "\xCC\x81", GRX_SYNTAX_PYTHON).matched);
  EXPECT_FALSE(search("^\\w$", "\xE2\x80\xBF", GRX_SYNTAX_PYTHON).matched);
  EXPECT_TRUE(search("^\\w$", "_", GRX_SYNTAX_PYTHON).matched);
  EXPECT_TRUE(search("^\\w$", "\xE2\x85\xA0", GRX_SYNTAX_PYTHON).matched);
}

TEST(Perl, AVersionConditionIsAnsweredWhenThePatternIsRead) {
  // `(?(VERSION>=n.n))` is decided at compile time against the version this
  // library emulates, which documentation/dialects.md section 9 names.
  EXPECT_TRUE(search("^(?(VERSION>=10.0)a|b)$", "a").matched);
  EXPECT_FALSE(search("^(?(VERSION>=10.0)a|b)$", "b").matched);

  // A version this library does not claim takes the other branch.
  EXPECT_TRUE(search("^(?(VERSION>=99.0)a|b)$", "b").matched);
  EXPECT_FALSE(search("^(?(VERSION>=99.0)a|b)$", "a").matched);

  // With no else-branch, a false test matches nothing at all.
  EXPECT_TRUE(search("^(?(VERSION>=99.0)a)$", "").matched);

  // A minor number above 99 is not a version pcre2test will read.
  Attempt silly = compile("(?(VERSION=10.101)yes|no)");
  EXPECT_NE(silly.result, GRX_OK);
  grx_regex_free(silly.regex);
}

TEST(Perl, AReferenceToANameNoGroupHasIsReportedAsThat) {
  // Checked when the whole pattern is known, because a name may be defined
  // later: `(?&later)(?<later>a)` is valid and a check at the reference
  // would have to guess.
  const char * patterns[] = {
    "\\k<nosuch>", "(?&nosuch)", "(?(<nosuch>)a|b)",
  };

  for (const char * pattern : patterns) {
    Attempt attempt = compile(pattern);
    EXPECT_NE(attempt.result, GRX_OK) << pattern;
    EXPECT_EQ(attempt.diag, GRX_DIAG_UNKNOWN_GROUP_NAME) << pattern;
    grx_regex_free(attempt.regex);
  }

  EXPECT_TRUE(search("^(?&later)$(?(DEFINE)(?<later>ab))", "ab").matched);
}

// --------------------------------------------------------------------------
// The leading directives
// --------------------------------------------------------------------------

TEST(Perl, ALeadingDirectiveAppliesAndOnlyAtTheStart) {
  // `(*UTF)` turns on UTF mode for the whole pattern.
  EXPECT_TRUE(search("(*UTF)^.$", "\xC3\xA9").matched);
  EXPECT_EQ(search("(*UTF)^.$", "\xC3\xA9").end, 2u)
      << "one character, two bytes";

  // The same directive in the middle is not a directive.
  Attempt late = compile("a(*UTF)b");
  EXPECT_NE(late.result, GRX_OK);
  grx_regex_free(late.regex);

  // Several in a row is the form pcre2test's corpus uses.
  Attempt several = compile("(*UTF)(*UCP)^\\w$");
  EXPECT_EQ(several.result, GRX_OK);
  grx_regex_free(several.regex);
}

TEST(Perl, TheLeadingDirectivesArePcre2sAndPerlRefusesEveryOne) {
  // All nineteen, because the list is the kind that grows a hole: a name
  // added for PCRE2 is accepted by both dialects unless something says
  // otherwise, and nothing here changes a match, so the difference is
  // silent. Probed against perl 5.40.1 - every one is "Unknown verb
  // pattern 'NAME' in regex" there.
  const char * directives[] = {
    "(*UTF)", "(*UCP)", "(*NO_AUTO_POSSESS)", "(*NO_START_OPT)",
    "(*NO_DOTSTAR_ANCHOR)", "(*NO_JIT)", "(*NOTEMPTY)",
    "(*NOTEMPTY_ATSTART)", "(*CR)", "(*LF)", "(*CRLF)", "(*ANYCRLF)",
    "(*ANY)", "(*NUL)", "(*BSR_ANYCRLF)", "(*BSR_UNICODE)",
    "(*LIMIT_MATCH=5)", "(*LIMIT_DEPTH=5)", "(*LIMIT_HEAP=5)",
  };
  for (const char * directive : directives) {
    std::string pattern = std::string(directive) + "abc";

    Attempt pcre = compile(pattern, GRX_SYNTAX_PCRE);
    EXPECT_EQ(pcre.result, GRX_OK) << pattern << " under PCRE2";
    grx_regex_free(pcre.regex);

    // Under Perl it is a syntax error either way, and that is the point:
    // "this dialect does not have the construct" is a different answer
    // from "this library has not built it", and the Perl row must be the
    // first one even for a directive PCRE2 has and this library lacks.
    Attempt perl = compile(pattern, GRX_SYNTAX_PERL);
    EXPECT_EQ(perl.result, GRX_ERR_SYNTAX) << pattern << " under Perl";
    EXPECT_EQ(perl.diag, GRX_DIAG_INVALID_GROUP_SYNTAX) << pattern;
    grx_regex_free(perl.regex);
  }

  // The verbs proper are a different list and Perl does have them, so the
  // gate above must not have taken those with it.
  Attempt mark = compile("(*MARK:x)abc", GRX_SYNTAX_PERL);
  EXPECT_EQ(mark.result, GRX_OK);
  grx_regex_free(mark.regex);
  Attempt skip = compile("a(*SKIP)b", GRX_SYNTAX_PERL);
  EXPECT_EQ(skip.result, GRX_OK);
  grx_regex_free(skip.regex);
}

TEST(Perl, FiveConstructFamiliesArePcre2sAndPerlHasNoneOfThem) {
  // Found by sweeping the Perl-family grammar against perl 5.40.1 rather
  // than by reading perlre, after the leading directives and the callouts
  // turned out to be two instances of the same shape: one front end, one
  // table, and nothing saying which of the two dialects a row belongs to.
  //
  // Each row is `pattern`, and each is "accept under PCRE2, refuse under
  // Perl". Perl's own message is in the comment above the group.
  //
  // `tools/oracle/perl_syntax_diff.py` is the durable half of the same
  // sweep - one entry per construct the front end reads, run by
  // `make check-oracle-perl-syntax` - and is what found the fifth family
  // after these four were fixed.
  struct { const char * pattern; } pcre_only[] = {
    // "Unterminated \g... pattern": perl's `\g` takes `\g1`, `\g-1` and
    // `\g{...}` and not the angle or quote spellings, which are PCRE2's
    // subroutine call. Perl *has* subroutine calls - `(?1)`, `(?&name)` -
    // so this is the spelling and not the construct.
    {"\\g<1>(a)"},
    {"\\g<name>(?<name>a)"},
    {"\\g'1'(a)"},
    {"\\g'name'(?<name>a)"},
    // "Unknown switch condition (?(...))": perl asks about its own version
    // with `$]`, outside the pattern.
    {"(?(VERSION>=10.0)a|b)"},
    {"(?(VERSION>=5.40)a|b)"},
    {"(?(VERSION=10.0)a|b)"},
    // "Sequence (?J...) not recognized". Perl does let two groups share a
    // name inside `(?|...)`, which is a rule about branch reset rather
    // than a flag a pattern may set.
    {"(?J)(?<n>a)(?<n>b)"},
    {"(?J:(?<n>a)(?<n>b))"},
    {"(?-J)a"},
    {"(?iJ)a"},
    // "Sequence (?U...) not recognized": inverting the greediness of every
    // quantifier is something perl has no spelling for at all.
    {"(?U)a"},
    {"(?-U)a"},
    {"(?U:a)"},
    {"(?iU)a"},
    // "Unknown '(*...)' construct 'napla'" and "Sequence (?*...) not
    // recognized": perl has no non-atomic lookaround in any spelling.
    {"(*napla:a)"},
    {"(*naplb:a)"},
    {"(*non_atomic_positive_lookahead:a)"},
    {"(*non_atomic_positive_lookbehind:a)"},
    {"(?*a)"},
    {"(?<*a)"},
  };

  for (const auto & row : pcre_only) {
    Attempt pcre = compile(row.pattern, GRX_SYNTAX_PCRE);
    EXPECT_EQ(pcre.result, GRX_OK) << row.pattern << " under PCRE2";
    grx_regex_free(pcre.regex);

    Attempt perl = compile(row.pattern, GRX_SYNTAX_PERL);
    EXPECT_EQ(perl.result, GRX_ERR_SYNTAX) << row.pattern << " under Perl";
    grx_regex_free(perl.regex);
  }

  // The neighbours each gate has to leave alone, because every one of
  // these compiles in perl and a gate written one character wider would
  // have taken them: the `\g` spellings perl does have, the lookaround
  // names it does have, and `(?|` itself.
  for (const char * pattern : {"\\g{1}(a)", "\\g1(a)", "\\g{name}(?<name>a)",
           "(?1)(a)", "(?&name)(?<name>a)", "(*atomic:a)", "(*pla:a)",
           "(*plb:a)", "(*nla:a)", "(*nlb:a)", "(*positive_lookahead:a)",
           "(*negative_lookbehind:a)", "(?|(a)|(b))",
           "(?|(?<n>a)|(?<n>b))", "(?(1)a|b)(c)", "(?(<n>)a|b)(?<n>c)",
           "(?(R)a|b)", "(?(DEFINE)(?<n>a))"}) {
    Attempt kept = compile(pattern, GRX_SYNTAX_PERL);
    EXPECT_EQ(kept.result, GRX_OK) << pattern << " is perl's too";
    grx_regex_free(kept.regex);
  }
}

TEST(Perl, TheNewlineConventionsAndWhatEachOneChanges) {
  // `(*BSR_ANYCRLF)` cuts `\R` to the three ASCII line endings and
  // `(*BSR_UNICODE)` restores it. Both are built, because `\R` is an
  // alternation the parser writes and a directive may only lead the
  // pattern, so the flag is always set before the `\R` it governs.
  EXPECT_TRUE(search("\\R", "\x0b").matched) << "plain \\R takes a VT";
  EXPECT_FALSE(search("(*BSR_ANYCRLF)\\R", "\x0b").matched);
  EXPECT_TRUE(search("(*BSR_UNICODE)\\R", "\x0b").matched);
  EXPECT_TRUE(search("(*BSR_ANYCRLF)\\R", "\r\n").matched);
  EXPECT_EQ(search("(*BSR_ANYCRLF)\\R", "\r\n").end, 2u)
      << "CR LF is still one unit under ANYCRLF";
  EXPECT_TRUE(search("(*BSR_ANYCRLF)\\R", "\n").matched);
  // The last one written wins, as the directives are read in order.
  EXPECT_TRUE(search("(*BSR_ANYCRLF)(*BSR_UNICODE)\\R", "\x0b").matched);

  // Which single characters each convention takes out of `.`. Every row
  // is pcre2test 10.46's answer, and `(*CRLF)` taking out *nothing* is the
  // one that shows why the CR LF pair travels as a flag rather than as a
  // member of the set.
  //
  // The subjects are built with an explicit length, because two of them
  // hold a NUL: `std::string` from a `const char *` would stop there and
  // ask about "a". The form-feed rows are split across two literals for
  // the neighbouring reason - `"a\x0cb"` is one hex escape reading
  // "0cb", which is U+00CB and not a form feed followed by a b.
  const std::string with_lf("a\nb");
  const std::string with_cr("a\rb");
  const std::string ff = std::string("a\x0c") + "b";
  const std::string nul = std::string("a\0b", 3);
  struct { const char * conv; const std::string & subject; int dot; }
  dots[] = {
    {"(*LF)", with_lf, 0},      {"(*LF)", with_cr, 1},
    {"(*CR)", with_lf, 1},      {"(*CR)", with_cr, 0},
    {"(*CRLF)", with_lf, 1},    {"(*CRLF)", with_cr, 1},
    {"(*ANYCRLF)", with_lf, 0}, {"(*ANYCRLF)", with_cr, 0},
    {"(*ANY)", ff, 0},          {"(*ANY)", nul, 1},
    {"(*NUL)", nul, 0},         {"(*NUL)", with_lf, 1},
  };
  for (const auto & row : dots) {
    std::string pattern = std::string("(*UTF)") + row.conv + "a.b";
    EXPECT_EQ(search(pattern, row.subject).matched ? 1 : 0, row.dot)
        << pattern;
    // `\N` is `.` with dot-all taken away, so it answers the same way.
    std::string not_newline
        = std::string("(*UTF)") + row.conv + "a\\Nb";
    EXPECT_EQ(search(not_newline, row.subject).matched ? 1 : 0, row.dot)
        << not_newline;
  }

  // Where `^` and `$` hold, which is the half a code-point set cannot
  // answer. Under `(*CRLF)` the terminator is the pair and nothing else.
  EXPECT_TRUE(search("(*CRLF)^b", "a\r\nb", GRX_SYNTAX_PCRE,
                  "m").matched);
  EXPECT_FALSE(search("(*CRLF)^b", "a\nb", GRX_SYNTAX_PCRE, "m").matched);
  EXPECT_FALSE(search("(*CRLF)^b", "a\rb", GRX_SYNTAX_PCRE, "m").matched);
  EXPECT_TRUE(search("(*ANYCRLF)^b", "a\rb", GRX_SYNTAX_PCRE, "m").matched);

  // `^` does not hold *between* the CR and the LF, and `$` does - which is
  // asymmetric in pcre2test and followed as measured rather than tidied.
  EXPECT_FALSE(search("(*ANY)^\n", "a\r\n", GRX_SYNTAX_PCRE, "m").matched);
  EXPECT_TRUE(search("(*ANY)\r$", "a\r\n", GRX_SYNTAX_PCRE, "m").matched);
  EXPECT_FALSE(search("(*CRLF)\r$", "a\r\n", GRX_SYNTAX_PCRE, "m").matched);

  // `$` and `\Z` outside multiline: before a *final* terminator, and the
  // pair counts as one.
  EXPECT_TRUE(search("(*CRLF)abc$", "abc\r\n").matched);
  EXPECT_FALSE(search("(*CRLF)abc$", "abc\n").matched);
  EXPECT_TRUE(search("(*CRLF)abc\\Z", "abc\r\n").matched);

  // `.` also refuses the place a terminator *begins*, which under
  // `(*CRLF)` is the CR of a pair and not the LF: pcre2test refuses
  // `a..b` against "a\r\nb" and accepts `\rb.` reaching the LF. Dot-all
  // lifts it, and a negated class never had it.
  EXPECT_FALSE(search("(*CRLF)a..b", "a\r\nb").matched);
  EXPECT_TRUE(search("(*CRLF)a..b", "a\r\nb", GRX_SYNTAX_PCRE, "s").matched);
  EXPECT_TRUE(search("(*CRLF)a[^q][^q]b", "a\r\nb").matched);

  // pcre2api's compromise: an unanchored attempt that failed at a CR LF
  // resumes after the LF, so `.+A` does not match "\r\nA" - unless the
  // pattern names CR or LF itself, which `[\r\n]A` does.
  EXPECT_FALSE(search("(*CRLF).+A", "\r\nA").matched);
  EXPECT_TRUE(search("(*CRLF)[\r\n]A", "\r\nA").matched);
  EXPECT_FALSE(search("(*CRLF)[^q]A", "\r\nA").matched);
  // A range endpoint names it and a range that merely spans it does not,
  // which is what "explicit" means and is measured, not assumed.
  EXPECT_TRUE(search("(*CRLF)[\x0a-\x0f]A", "\r\nA").matched);
  EXPECT_FALSE(search("(*CRLF)[\x09-\x0f]A", "\r\nA").matched);

  // `(*LF)` names the convention this library and PCRE2 both already use,
  // so it asks for what is already true.
  Attempt lf = compile("(*LF)a.b", GRX_SYNTAX_PCRE);
  EXPECT_EQ(lf.result, GRX_OK);
  grx_regex_free(lf.regex);

  // Mid-pattern it is not a directive at all, in either spelling.
  Attempt late = compile("a(*CR)b", GRX_SYNTAX_PCRE);
  EXPECT_EQ(late.result, GRX_ERR_SYNTAX);
  grx_regex_free(late.regex);
  Attempt late_bsr = compile("a(*BSR_ANYCRLF)b", GRX_SYNTAX_PCRE);
  EXPECT_EQ(late_bsr.result, GRX_ERR_SYNTAX);
  grx_regex_free(late_bsr.regex);
}

TEST(Perl, ACalloutIsPcre2sAndItsNumberIsBounded) {
  // A callout reports a position to a caller that registered a function;
  // there is no such API here, so accepting one changes no answer. That is
  // a reason to accept the construct and not a reason to accept anything
  // spelled like it.
  //
  // The number is one byte wide in PCRE2 - `(?C256)` is its error 138,
  // "number after (?C is greater than 255" - and a number this library
  // hands to nobody is still a number the reference refuses.
  for (const char * pattern : {"(?C)abc", "(?C0)abc", "(?C255)abc",
           "(?C{x})abc", "(?C\"x\")abc"}) {
    Attempt ok = compile(pattern, GRX_SYNTAX_PCRE);
    EXPECT_EQ(ok.result, GRX_OK) << pattern;
    grx_regex_free(ok.regex);
  }
  for (const char * pattern : {"(?C256)abc", "(?C1000)abc",
           "(?C99999999999999999999)abc"}) {
    Attempt over = compile(pattern, GRX_SYNTAX_PCRE);
    EXPECT_EQ(over.result, GRX_ERR_SYNTAX) << pattern;
    EXPECT_EQ(over.diag, GRX_DIAG_INVALID_GROUP_SYNTAX) << pattern;
    grx_regex_free(over.regex);
  }

  // Perl has no callouts at all: every spelling is "Sequence (?C...) not
  // recognized in regex" there, `(?C)` included.
  for (const char * pattern : {"(?C)abc", "(?C0)abc", "(?C255)abc",
           "(?C{x})abc", "(?C\"x\")abc"}) {
    Attempt perl = compile(pattern, GRX_SYNTAX_PERL);
    EXPECT_EQ(perl.result, GRX_ERR_SYNTAX) << pattern << " under Perl";
    EXPECT_EQ(perl.diag, GRX_DIAG_INVALID_GROUP_SYNTAX) << pattern;
    grx_regex_free(perl.regex);
  }
}

TEST(Perl, TheShorthandsWidenOnUcpAndTheFoldingWidensOnUtf) {
  // Two triggers, and they were one. pcre2pattern says `\w`, `\d` and `\s`
  // use Unicode "only if PCRE2_UCP is set" - exactly what it says about the
  // POSIX classes - while caseless matching of a non-ASCII character needs
  // only UTF. Every row below is pcre2test 10.46's answer.
  const char * e_acute = "\xc3\xa9";          // U+00E9
  const char * E_acute = "\xc3\x89";          // U+00C9
  const char * arabic_one = "\xd9\xa1";       // U+0661
  const char * nbsp = "\xc2\xa0";             // U+00A0

  // UTF alone leaves the shorthands ASCII.
  EXPECT_FALSE(search("(*UTF)\\w", e_acute).matched);
  EXPECT_FALSE(search("(*UTF)\\d", arabic_one).matched);
  EXPECT_FALSE(search("(*UTF)\\s", nbsp).matched);
  // `\b` is defined from `\w`, so it moves with it.
  EXPECT_FALSE(search("(*UTF)\\b\\w", e_acute).matched);

  // UCP widens all four.
  EXPECT_TRUE(search("(*UTF)(*UCP)\\w", e_acute).matched);
  EXPECT_TRUE(search("(*UTF)(*UCP)\\d", arabic_one).matched);
  EXPECT_TRUE(search("(*UTF)(*UCP)\\s", nbsp).matched);
  EXPECT_TRUE(search("(*UTF)(*UCP)\\b\\w", e_acute).matched);

  // The POSIX classes always keyed on UCP, and the extended class reads
  // the same sets, so both follow.
  EXPECT_FALSE(search("(*UTF)[[:alpha:]]", e_acute).matched);
  EXPECT_TRUE(search("(*UTF)(*UCP)[[:alpha:]]", e_acute).matched);
  EXPECT_FALSE(search("(*UTF)(?[ \\w ])", e_acute).matched);
  EXPECT_TRUE(search("(*UTF)(*UCP)(?[ \\w ])", e_acute).matched);

  // Folding is the other way round: UTF alone is enough, and no UCP is
  // needed. This is why the two are computed from different bits.
  EXPECT_TRUE(search("(*UTF)(?i)" + std::string(e_acute), E_acute).matched);
  EXPECT_FALSE(search("(?i)" + std::string(e_acute), E_acute).matched);

  // Perl is neither case: its subject is a Unicode string and its
  // shorthands are Unicode with no flag at all, which is what makes `/a`
  // the interesting direction there.
  EXPECT_TRUE(search("\\w", e_acute, GRX_SYNTAX_PERL).matched);
  EXPECT_FALSE(search("(?a)\\w", e_acute, GRX_SYNTAX_PERL).matched);
}

TEST(Perl, AScriptRunIsCheckedAgainstTheTextItsBodyMatched) {
  // UTS #39 section 5.1, as pcre2unicode's "Script Runs" states it. Every
  // expectation here is one perl 5.40.1 and pcre2test 10.46 both give,
  // except where noted; tools/oracle/script_run_diff.py is the sweep -
  // 25,764 subjects over an alphabet chosen to hit each clause.
  //
  // `(*UTF)` throughout, because PCRE2's subject is bytes until it is said.
  const char * utf = "(*UTF)";

  struct { const char * subject; int expected; const char * why; } rows[] = {
    // The example the construct exists for. "google.com" is Latin plus a
    // Common full stop; the same string with a Cyrillic "o" is not.
    {"google.com", 1, "all Latin, and the dot is Common"},
    {"goog\xd0\xbele.com", 0, "U+043E is Cyrillic and the rest is Latin"},

    // Common and Inherited on their own constrain nothing.
    {"...", 1, "nothing but Common"},
    {" - .", 1, "nothing but Common"},
    {".a", 1, "a Common *first* character must not constrain the rest"},

    // The decimal-digit rule is separate from the script rule, and the
    // ASCII digits are Common - so this is not something the intersection
    // would have caught.
    {"abc123", 1, "one set of ten"},
    {"abc\xd9\xa1", 0, "Arabic-Indic digit one, with Latin letters"},
    {"1\xd9\xa1", 0, "two sets of ten, both otherwise acceptable"},

    // The three Han combinations, and the pairs that are none of them.
    {"\xe6\xbc\xa2\xe3\x81\x8b", 1, "Han and Hiragana: Japanese"},
    {"\xe6\xbc\xa2\xe3\x82\xab", 1, "Han and Katakana: Japanese"},
    {"\xe6\xbc\xa2\xed\x95\x9c", 1, "Han and Hangul: Korean"},
    {"\xe6\xbc\xa2\xe3\x84\x85", 1, "Han and Bopomofo: HanBopomofo"},
    {"\xe3\x81\x8b\xed\x95\x9c", 0, "Hiragana and Hangul: no virtual script"},
    {"\xe3\x81\x8b\xe3\x84\x85", 0, "Hiragana and Bopomofo"},
    {"\xed\x95\x9c\xe3\x84\x85", 0, "Hangul and Bopomofo"},
    // Two families at once. pcre2test 10.46 *matches* these, against its
    // own manual, which names "a mixture of Hangul and Bopomofo and Han"
    // as not a script run; perl refuses all six. dialects.md section 6.
    {"\xe6\xbc\xa2\xe3\x81\x8b\xed\x95\x9c", 0, "Han, Hiragana and Hangul"},
    {"\xe6\xbc\xa2\xed\x95\x9c\xe3\x84\x85", 0, "Han, Hangul and Bopomofo"},

    // Fewer than two characters is always a script run, and that is the
    // only way an unassigned code point can be in one.
    {"\xf3\xa0\x80\x80", 1, "U+E0000 alone is unassigned and still a run"},
    {"a\xf3\xa0\x80\x80", 0, "two characters, one of them unassigned"},
    {"\xf3\xa0\x80\x80.", 0, "and a Common second character does not save it"},
  };

  for (const auto & row : rows) {
    std::string pattern = std::string(utf) + "^(*sr:.+)$";
    EXPECT_EQ(search(pattern, row.subject).matched ? 1 : 0, row.expected)
        << row.why;
  }

  // All four spellings, because they are four table rows and a table row
  // is where a hole goes unnoticed.
  for (const char * spelling :
      {"script_run", "sr", "atomic_script_run", "asr"}) {
    std::string good = std::string(utf) + "^(*" + spelling + ":.+)$";
    EXPECT_TRUE(search(good, "google.com").matched) << spelling;
    EXPECT_FALSE(search(good, "goog\xd0\xbele.com").matched) << spelling;
  }

  // Backtracking into a script run shortens it until it is one. Unanchored,
  // `\S+` gives back characters until the run holds: "goog" before the
  // Cyrillic letter.
  EXPECT_EQ(search("(*UTF)(*sr:\\S+)", "goog\xd0\xbele.com").end, 4u);

  // `(*asr:...)` is `(*sr:(?>...))` - the atomic group *inside* - so the
  // body cannot give anything back at the start it began from. The search
  // moves on instead and finds the run after the Cyrillic letter: 6-12,
  // which is what pcre2test and perl both answer, and not the nomatch a
  // reading of "atomic" alone would predict.
  EXPECT_EQ(span_of("(*UTF)(*asr:\\S+)", "goog\xd0\xbele.com"), "6-12");

  // Anchored at both ends there is nowhere to move to, and both spellings
  // fail - the check is on the run and not on the greediness.
  EXPECT_EQ(span_of("(*UTF)^(*asr:\\S+)$", "goog\xd0\xbele.com"), "nomatch");
  EXPECT_EQ(span_of("(*UTF)^(*sr:\\S+)$", "goog\xd0\xbele.com"), "nomatch");

  // Perl has the construct too, in every spelling.
  EXPECT_TRUE(
      search("^(*sr:.+)$", "google.com", GRX_SYNTAX_PERL).matched);
  EXPECT_FALSE(
      search("^(*sr:.+)$", "goog\xd0\xbele.com", GRX_SYNTAX_PERL).matched);

  // It is an ordinary group otherwise: quantifiable, and not capturing.
  Attempt quantified = compile("(*UTF)(*sr:a)+");
  EXPECT_EQ(quantified.result, GRX_OK);
  grx_regex_free(quantified.regex);
  Attempt plain = compile("(*UTF)(*sr:abc)");
  ASSERT_EQ(plain.result, GRX_OK);
  GRX_Facts facts;
  grx_regex_facts(plain.regex, &facts);
  EXPECT_EQ(facts.capture_count, 0u)
      << "a script run groups without capturing";
  EXPECT_NE(facts.has_script_run, 0);
  EXPECT_EQ(facts.is_regular, 0)
      << "it reads the text consumed, which a thread set has merged away";
  grx_regex_free(plain.regex);
}

TEST(Perl, TheConstructsThisLibraryRefusesSayWhyAndNotSomethingElse) {
  // Each of these is real syntax the reference compiles. Refusing them is a
  // decision; refusing them as *syntax errors* would be a lie about the
  // pattern, so each reports GRX_ERR_UNSUPPORTED instead.
  //
  // `(*script_run:` was on this list until the construct was built and is
  // the reason the list is worth keeping short: an entry here is a promise
  // that stays true only until somebody does the work.
  const char * refused[] = {
    "\\C",                     // one code unit
  };

  for (const char * pattern : refused) {
    Attempt attempt = compile(pattern);
    EXPECT_EQ(attempt.result, GRX_ERR_UNSUPPORTED) << pattern;
    EXPECT_EQ(attempt.diag, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED) << pattern;
    grx_regex_free(attempt.regex);
  }
}

// --------------------------------------------------------------------------
// Where Perl and PCRE2 differ
// --------------------------------------------------------------------------

TEST(Perl, PerlsSubjectIsUnicodeAndPcre2sIsNot) {
  // documentation/dialects.md section 5.15: Perl's subject is a Unicode
  // string, so UTF is on unless the caller turns it off. PCRE2's is a byte
  // string until `(*UTF)` says otherwise. The field had been empty in the
  // dialect table since it was written, and what it cost was visible only
  // once there was a front end to read it.
  EXPECT_EQ(search("^.$", "\xC3\xA9", GRX_SYNTAX_PERL).end, 2u)
      << "Perl: one character";
  EXPECT_FALSE(search("^.$", "\xC3\xA9", GRX_SYNTAX_PCRE).matched)
      << "PCRE2 without (*UTF): two bytes, and `.` is one";
}

TEST(Perl, PerlAcceptsALoneScriptNameAndEcmascriptDoesNot) {
  // `\p{Latin}` is `\p{Script=Latin}` in the loose dialects and a syntax
  // error in ECMAScript, which is exactly the difference the two property
  // resolvers exist to keep apart.
  EXPECT_TRUE(search("^\\p{Latin}$", "a", GRX_SYNTAX_PERL).matched);

  Attempt strict = compile("^\\p{Latin}$", GRX_SYNTAX_ECMASCRIPT, "u");
  EXPECT_NE(strict.result, GRX_OK);
  grx_regex_free(strict.regex);

  // A name nobody has is an unknown property and not an internal fault.
  Attempt unknown = compile("\\p{Nosuch}", GRX_SYNTAX_PERL);
  EXPECT_NE(unknown.result, GRX_OK);
  EXPECT_EQ(unknown.diag, GRX_DIAG_UNKNOWN_PROPERTY);
  grx_regex_free(unknown.regex);
}

TEST(Perl, TheVariableLookbehindBoundIsTheDialectsAndNotTheCallers) {
  // pcre2test compiles `(?<=a{256})` and refuses `(?<=\d{1,256})`: the number
  // bounds the *variation*, not the distance, and a fixed body of any length
  // is cheap to check.
  Attempt fixed = compile("(?<=a{256})x");
  EXPECT_EQ(fixed.result, GRX_OK);
  grx_regex_free(fixed.regex);

  Attempt varying = compile("(?<=\\d{1,256})x");
  EXPECT_NE(varying.result, GRX_OK);
  EXPECT_EQ(varying.diag, GRX_DIAG_VARIABLE_LOOKBEHIND);
  grx_regex_free(varying.regex);

  // An unbounded body is the case the bound exists for.
  Attempt unbounded = compile("(?<=a+)x");
  EXPECT_NE(unbounded.result, GRX_OK);
  grx_regex_free(unbounded.regex);

  // ECMAScript's lookbehind is unbounded, so the same body is fine there.
  Attempt es = compile("(?<=a+)x", GRX_SYNTAX_ECMASCRIPT, "u");
  EXPECT_EQ(es.result, GRX_OK);
  grx_regex_free(es.regex);
}

// --------------------------------------------------------------------------
// The replacement templates
// --------------------------------------------------------------------------

namespace {

/** Replace every match, and report the text or the diagnostic. */
std::string replace_all(GRX_Syntax syntax, const std::string & pattern,
    const std::string & subject, const std::string & tmpl) {
  Attempt attempt = compile(pattern, syntax);
  if (attempt.result != GRX_OK) {
    return "compile failed";
  }

  GRX_Text text = {nullptr, 0, nullptr};
  GRX_Error error;
  grx_error_clear(&error);
  GRX_Result result = grx_regex_replace(attempt.regex, subject.data(),
      subject.size(), tmpl.data(), tmpl.size(), GRX_REPLACE_GLOBAL, nullptr,
      nullptr, &error, &text);
  std::string answer = result == GRX_OK
      ? std::string(text.data, text.length)
      : std::string("error: ") + grx_diag_string(error.diag);
  grx_text_free(&text);
  grx_regex_free(attempt.regex);
  return answer;
}

} // namespace

TEST(Perl, Pcre2sTemplateGrammarIsPcre2Substitutes) {
  // Every answer here is pcre2test's, asked with `\=replace=`.
  EXPECT_EQ(replace_all(GRX_SYNTAX_PCRE, "(a)(b)", "ab", "$1-$2"), "a-b");
  EXPECT_EQ(replace_all(GRX_SYNTAX_PCRE, "(a)(b)", "ab", "${1}${2}"), "ab");
  EXPECT_EQ(
      replace_all(GRX_SYNTAX_PCRE, "(?<x>a)(?<y>b)", "ab", "$x-${y}"), "a-b");
  EXPECT_EQ(replace_all(GRX_SYNTAX_PCRE, "(a)", "a", "$$ and $1"), "$ and a");

  // A reference to a group the pattern does not have is an error, where
  // ECMAScript leaves the text alone and Perl interpolates nothing. All
  // three rows of section 5.11 differ here, which is why the rule is a
  // field and not a shared default.
  EXPECT_EQ(replace_all(GRX_SYNTAX_PCRE, "(a)", "a", "$9"),
      "error: replacement template names a group the pattern does not have");
  EXPECT_EQ(replace_all(GRX_SYNTAX_ECMASCRIPT, "(a)", "a", "$9"), "$9");
  EXPECT_EQ(replace_all(GRX_SYNTAX_PERL, "(a)", "a", "[$9]"), "[]");
}

TEST(Perl, PerlsTemplateGrammarIsTheInterpolationSubset) {
  // Every answer here is Perl 5.40's, asked with `s///g`.
  EXPECT_EQ(replace_all(GRX_SYNTAX_PERL, "(a)(b)", "ab", "[$1|${2}]"), "[a|b]");
  EXPECT_EQ(replace_all(GRX_SYNTAX_PERL, "(?<x>a)", "a", "<$+{x}>"), "<a>");
  EXPECT_EQ(
      replace_all(GRX_SYNTAX_PERL, "b", "abc", "[$`|$&|$']"), "a[a|b|c]c");

  // A Perl template is a double-quoted string, so the escape is `\$` and not
  // `$$` - `$$` is the process id, which is why one dialect cannot have both
  // rules.
  EXPECT_EQ(replace_all(GRX_SYNTAX_PERL, "(a)", "a", "\\$1 is $1"), "$1 is a");
}

TEST(Perl, MultilineCaretStopsAtANewlineThatEndsTheSubject) {
  // pcre2pattern, PCRE2_MULTILINE: a circumflex "does not match after a
  // newline that ends the string". Perl agrees - `"a\nb\n" =~ /^/gm` reports
  // 0 and 2, not 0, 2 and 4 - and ECMAScript does not, because ECMA-262
  // 22.2.2.7 asks only whether the character before this position is a line
  // terminator. So the two engines run two different assertions, and this is
  // the pattern that tells them apart.
  EXPECT_FALSE(search("b\\s^", "a\nb\n", GRX_SYNTAX_PCRE, "m").matched);
  EXPECT_FALSE(search("b\\s^", "a\nb\n", GRX_SYNTAX_PERL, "m").matched);
  EXPECT_TRUE(search("b\\s^", "a\nb\n", GRX_SYNTAX_ECMASCRIPT, "m").matched);

  // The interior newline still starts a line in all three.
  EXPECT_TRUE(search("a\\s^b", "a\nb\n", GRX_SYNTAX_PCRE, "m").matched);
  EXPECT_TRUE(search("a\\s^b", "a\nb\n", GRX_SYNTAX_ECMASCRIPT, "m").matched);
}

TEST(Perl, ABraceAfterBackslashNIsTheQuantifierOnlyWhenItCouldBeOne) {
  // `\N{3}` is three of anything in both references, and the spaces and the
  // comment do not change that: pcre2test reports "abb" for every one of
  // these against "abbbbc".
  EXPECT_EQ(search("\\N{3}", "abbbbc").end, 3u);
  EXPECT_EQ(search("\\N{ 3 }", "abbbbc").end, 3u);
  EXPECT_EQ(search("\\N (?#c) {3}", "abbbbc", GRX_SYNTAX_PCRE, "x").end, 3u);
  EXPECT_EQ(search("\\N {3,4}", "abbbbc", GRX_SYNTAX_PERL, "x").end, 4u);

  // A brace that cannot be a quantifier is the `\N{name}` spelling, which
  // neither reference lets this library reach: PCRE2 refuses it outright
  // ("PCRE2 does not support \N{name}") and Perl's needs a table of
  // character names that is not generated here.
  EXPECT_EQ(compile("\\N{SPACE}").diag, GRX_DIAG_INVALID_ESCAPE);

  // Where they part: perl refuses a *detached* name with "Missing braces on
  // \N{}", while pcre2test compiles `/\N {U+41}/x,utf` as `\N` followed by
  // six ordinary characters. Perl's half is the one with corpus records.
  EXPECT_EQ(compile("abc\\N {SPACE}", GRX_SYNTAX_PERL, "x").diag,
      GRX_DIAG_INVALID_ESCAPE);
  EXPECT_EQ(compile("\\N(?#c){SPACE}", GRX_SYNTAX_PERL).diag,
      GRX_DIAG_INVALID_ESCAPE);
  EXPECT_EQ(compile_result("a\\N {SPACE}", GRX_SYNTAX_PCRE, "x"), GRX_OK);
}

TEST(Perl, BackslashNInAClassIsOnlyTheCodePointForm) {
  // "not a newline" is not a set operation a class can express, so pcre2test
  // refuses `[\N]`. `[\N{4}]` is the same refusal wearing a quantifier's
  // clothes, and reported it as an internal fault here until it was asked.
  EXPECT_EQ(compile("[\\N]").diag, GRX_DIAG_INVALID_CLASS_ITEM);
  EXPECT_EQ(compile("[\\N{4}]").diag, GRX_DIAG_INVALID_CLASS_ITEM);

  // A code point is a class item like any other.
  EXPECT_TRUE(search("(*UTF)[\\N{U+0041}]", "A", GRX_SYNTAX_PCRE).matched);
}

TEST(Perl, PerlLetsAnUnderscoreSeparateHexDigits) {
  // `\x{_1_0000}` is U+10000 in Perl, the way an underscore separates the
  // digits of a numeric literal. PCRE2 says "Malformed \x{ escape" for the
  // same pattern, so this is a flavour difference and not a shared leniency.
  EXPECT_TRUE(
      search("\\x{_1_0000}", "\xF0\x90\x80\x80", GRX_SYNTAX_PERL).matched);
  EXPECT_EQ(compile("\\x{_1_0000}", GRX_SYNTAX_PCRE).diag,
      GRX_DIAG_INVALID_HEX_ESCAPE);

  // A digit *was* still required here, and that was this library refusing
  // what perl accepts. perl reads `\x{_}` as zero and so does this now; the
  // reversal and its reason are in read_hex(), and
  // ADigitlessHexEscapeIsNulInPerlAndAnErrorInTheOthers has the whole family.
  // PCRE2 refuses it, which is what keeps this test about the flavour split.
  EXPECT_EQ(span_of("\\x{_}", std::string("\0", 1), GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(compile("\\x{_}", GRX_SYNTAX_PCRE).diag,
      GRX_DIAG_INVALID_HEX_ESCAPE);
}

TEST(Perl, TheCasedLetterAliasesAreSpeltThreeWays) {
  // U+3400 is Lo: a letter, and not a cased one. `\p{L&}` is Cased_Letter in
  // both references, so it does not match. `\p{L_}` is Cased_Letter in Perl
  // and plain Letter in PCRE2, which reaches it through the loose rule that
  // deletes underscores - one character's difference between two dialects
  // that otherwise share a spelling rule.
  const std::string han = "\xE3\x90\x80";
  EXPECT_FALSE(search("(*UTF)\\p{L&}", han, GRX_SYNTAX_PCRE).matched);
  EXPECT_FALSE(search("\\p{L&}", han, GRX_SYNTAX_PERL).matched);
  EXPECT_FALSE(search("\\p{L_}", han, GRX_SYNTAX_PERL).matched);
  EXPECT_TRUE(search("(*UTF)\\p{L_}", han, GRX_SYNTAX_PCRE).matched);

  // All four spellings still agree about a lower-case letter.
  for (const char * spelling : {"\\p{L&}", "\\p{L_}", "\\p{Lc}",
           "\\p{Cased_Letter}"}) {
    EXPECT_TRUE(search(spelling, "a", GRX_SYNTAX_PERL).matched) << spelling;
  }
}

TEST(Perl, AnExtendedClassIsAnExpressionOverSets) {
  // PCRE2 10.45's `(?[...])`. Every answer here is pcre2test's.
  EXPECT_TRUE(search("(?[ [a] | [b] & [b] ])", "b").matched);
  EXPECT_FALSE(search("(?[ [abc] - [b] ])", "b").matched);
  EXPECT_FALSE(search("(?[ [ab] ^ [bc] ])", "b").matched);
  EXPECT_TRUE(search("(?[ [ab] ^ [bc] ])", "c").matched);
  EXPECT_TRUE(search("(?[ \\d ])", "5").matched);
  EXPECT_TRUE(search("(?[ [:alpha:] & [a-z\\t] ])", "q").matched);
  EXPECT_TRUE(search("(?[ (\\n + \\t) ])", "\t").matched);

  // `+` is union, the same operator as `|`, and not a synonym invented here.
  EXPECT_TRUE(search("(?[ [\\t] + [\\n] ])", "\n").matched);
}

TEST(Perl, AnExtendedClassHasTwoPrecedenceLevels) {
  // `&` binds tighter than the rest: `[a] | [b] & [b]` is `a | (b & b)` and
  // so still matches "a".
  EXPECT_TRUE(search("(?[ [a] | [b] & [b] ])", "a").matched);

  // Everything else is one level and associates to the left, which is what
  // these two patterns differ by: `((abc - a) | a)` has "a" and
  // `((a | abc) - a)` does not.
  EXPECT_TRUE(search("(?[ [abc] - [a] | [a] ])", "a").matched);
  EXPECT_FALSE(search("(?[ [a] | [abc] - [a] ])", "a").matched);

  // `!` binds tighter than `&`: `(!a) & ab` is "b" alone.
  EXPECT_FALSE(search("(?[ ! [a] & [ab] ])", "a").matched);
  EXPECT_TRUE(search("(?[ ! [a] & [ab] ])", "b").matched);

  // Two complements are none. pcre2test compiles a hundred of them, so they
  // are counted rather than recursed on, and an even count wraps nothing.
  EXPECT_TRUE(search("(?[ !![a] ])", "a").matched);
  EXPECT_TRUE(search("(?[ " + std::string(100, '!') + "[a] ])", "a").matched);
}

TEST(Perl, AnExtendedClassOperandIsASetAndNotACharacter) {
  // pcre2test: "unexpected character in (?[...]) extended character class".
  // `a` is a character; `[a]` is how the set containing it is written.
  EXPECT_EQ(compile("(?[a])").diag, GRX_DIAG_INVALID_CLASS_ITEM);
  EXPECT_EQ(compile("(?[])").diag, GRX_DIAG_EMPTY_CLASS);
  EXPECT_EQ(compile("(?[ \\d \\n ])").diag, GRX_DIAG_INVALID_CLASS_SET_OP);
  EXPECT_EQ(compile("(?[ - [a] ])").diag, GRX_DIAG_INVALID_CLASS_SET_OP);

  // A quoted run is a sequence of characters, and a sequence is not a set -
  // but an empty one is invisible, and so is a `\E` with no run open.
  EXPECT_EQ(compile("(?[ \\Qab\\E ])").diag, GRX_DIAG_INVALID_CLASS_ITEM);
  EXPECT_TRUE(search("(?[\\n \\Q\\E])", "\n").matched);
  EXPECT_TRUE(search("(?[\\E\\n])", "\n").matched);

  // Perl reads the same grammar, and answers the same way here.
  EXPECT_EQ(compile("(?[a])", GRX_SYNTAX_PERL).diag,
      GRX_DIAG_INVALID_CLASS_ITEM);
  EXPECT_EQ(compile("(?[ - [a] ])", GRX_SYNTAX_PERL).diag,
      GRX_DIAG_INVALID_CLASS_SET_OP);
}

TEST(Perl, PerlsExtendedClassIsPcre2sExceptForWhatItIgnores) {
  // The `(?[...])` grammar was PCRE2's alone here, on the belief that
  // Perl's "nests where PCRE2's does not". Perl refuses a textual
  // `(?[ (?[ [a] ]) ])` - what it nests is an *interpolated* `qr//`, which
  // a pattern arriving as text cannot be. Compared over 13,440 generated
  // rows the operands, the operators and their precedence all agree.
  for (const char * pattern : {"(?[ [a] ])", "(?[ [a] | [b] ])",
           "(?[ [a] + [b] ])", "(?[ [ab] & [b] ])", "(?[ [ab] - [b] ])",
           "(?[ [ab] ^ [bc] ])", "(?[ ! [a] ])", "(?[ ( [a] | [b] ) & [b] ])",
           "(?[ \\d ])", "(?[ \\p{L} ])", "(?[ [:alpha:] ])",
           "(?[ \\N{U+0041} ])"}) {
    Attempt perl = compile(pattern, GRX_SYNTAX_PERL);
    EXPECT_EQ(perl.result, GRX_OK) << pattern << " under Perl";
    grx_regex_free(perl.regex);
    // `(*UTF)` on the PCRE2 side and not on Perl's, because `\N{U+...}`
    // needs UTF mode in pcre2 and Perl's subject is a Unicode string
    // already. That asymmetry is section 5.15 and not this test's subject.
    Attempt pcre = compile(std::string("(*UTF)") + pattern, GRX_SYNTAX_PCRE);
    EXPECT_EQ(pcre.result, GRX_OK) << pattern << " under PCRE2";
    grx_regex_free(pcre.regex);
  }

  // Neither nests textually.
  EXPECT_EQ(compile("(?[ (?[ [a] ]) ])", GRX_SYNTAX_PERL).result,
      GRX_ERR_SYNTAX);
  EXPECT_EQ(compile("(?[ (?[ [a] ]) ])", GRX_SYNTAX_PCRE).result,
      GRX_ERR_SYNTAX);

  // What differs is which characters are ignorable. Perl skips all of
  // Pattern_White_Space - probed, all eleven - and takes `#` comments to
  // the next line feed; pcre2test refuses a literal newline there with
  // error 216 and refuses `#` outright.
  for (const char * pattern : {"(?[ [a]\n])", "(?[ [a]\x0b])",
           "(?[ [a]\x0c])", "(?[ [a]\r])", "(?[ [a]\xc2\x85])",
           "(?[ [a]\xe2\x80\x8e])", "(?[ [a]\xe2\x80\xa8])",
           "(?[ [a]\xe2\x80\xa9])", "(?[ [a] # c\n | [b] ])",
           "(?[ # c\n [a] ])"}) {
    Attempt perl = compile(pattern, GRX_SYNTAX_PERL);
    EXPECT_EQ(perl.result, GRX_OK) << pattern << " under Perl";
    grx_regex_free(perl.regex);
    Attempt pcre = compile(std::string("(*UTF)") + pattern, GRX_SYNTAX_PCRE);
    EXPECT_NE(pcre.result, GRX_OK) << pattern << " under PCRE2";
    grx_regex_free(pcre.regex);
  }

  // U+00A0 is whitespace and is *not* Pattern_White_Space, which is the
  // case that says this is the property and not a notion of "space".
  EXPECT_NE(compile("(?[ [a]\xc2\xa0])", GRX_SYNTAX_PERL).result, GRX_OK);

  // A comment ends at a line feed and at nothing else - CR, VT and U+2028
  // all leave it open, so the `]` is swallowed and the pattern is an error
  // in perl too. So is a `#` with no line feed after it.
  EXPECT_NE(compile("(?[ [a] # c\r ])", GRX_SYNTAX_PERL).result, GRX_OK);
  EXPECT_NE(compile("(?[ [a] # no line feed ])", GRX_SYNTAX_PERL).result,
      GRX_OK);

  // And the semantics are the same set: `[a]` holds "a" and not "b".
  EXPECT_TRUE(search("(?[ [a] | [b] ])", "b", GRX_SYNTAX_PERL).matched);
  EXPECT_FALSE(search("(?[ [a] - [a] ])", "a", GRX_SYNTAX_PERL).matched);
  EXPECT_TRUE(search("(?[ [a]\n| [b] ])", "b", GRX_SYNTAX_PERL).matched);
}

TEST(Perl, AnExtendedClassNestsFifteenDeepAndNoFurther) {
  // pcre2test's number, and a bracket expression counts as a level: `(?[`
  // with fourteen nested parentheses compiles, and the same pattern with one
  // `[a]` at the bottom does not.
  const std::string opens(14, '(');
  const std::string closes(14, ')');
  EXPECT_EQ(compile_result("(?[" + opens + "\\n&\\n" + closes + "])"), GRX_OK);
  EXPECT_EQ(compile("(?[" + opens + "[a]" + closes + "])").diag,
      GRX_DIAG_CLASS_NESTING_TOO_DEEP);

  // The cap is the dialect's and not a resource limit, so it is a syntax
  // error: no caller can raise it, which is exactly what separates the two.
  EXPECT_EQ(compile_result("(?[" + opens + "[a]" + closes + "])"),
      GRX_ERR_SYNTAX);
}

TEST(Perl, AnExtendedClassIgnoresSpacesAndTabsOnly) {
  // Inside the brackets too: `(?[ [ a ] ])` does not match a space in
  // pcre2test, which is the rule `xx` gives an ordinary class.
  EXPECT_TRUE(search("(?[ [ a ] ])", "a").matched);
  EXPECT_FALSE(search("(?[ [ a ] ])", " ").matched);

  // A literal newline is not ignorable: pcre2test reports it as an
  // unexpected character rather than skipping it. Here it is the operand
  // with no operator before it, which is the same refusal reached from the
  // other side - what matters is that the pattern does not compile.
  EXPECT_EQ(compile_result("(?[ [a]\n| [b] ])"), GRX_ERR_SYNTAX);
}

TEST(Perl, EmbeddedCodeIsPerlsAndPcre2HasNoneAtAll) {
  // Two different refusals for one spelling. Perl has `(?{...})` and this
  // library will not run code, so it says so; PCRE2 has no embedded code at
  // all, so `(?{` is a `{` after `(?` and pcre2test reports "unrecognized
  // character after (? or (?-". Saying "not implemented yet" there would
  // promise a construct the dialect does not have.
  EXPECT_EQ(compile("a(?{ 1 })b", GRX_SYNTAX_PERL).result, GRX_ERR_UNSUPPORTED);
  EXPECT_EQ(compile("a(?{ 1 })b", GRX_SYNTAX_PCRE).diag,
      GRX_DIAG_INVALID_GROUP_SYNTAX);
  EXPECT_EQ(compile("a(??{ 1 })b", GRX_SYNTAX_PERL).result,
      GRX_ERR_UNSUPPORTED);
  EXPECT_EQ(compile("a(?{)b", GRX_SYNTAX_PCRE).result, GRX_ERR_SYNTAX);
}

TEST(Perl, AMarkNamesAPositionAndTheMatchReportsIt) {
  // pcre2_get_mark(), and every answer here is pcre2test's with `mark`.
  EXPECT_EQ(mark_of("a(*MARK:A)b|a(*MARK:B)c", "ab"), "A");
  EXPECT_EQ(mark_of("a(*MARK:A)b|a(*MARK:B)c", "ac"), "B");

  // A mark on a branch that was abandoned is not on the path that matched,
  // so it is not reported: `X(*MARK:m)Y|.*` matches "XZ" through the second
  // branch, and pcre2test prints no MK line for it.
  EXPECT_EQ(mark_of("X(*MARK:m)Y|.*", "XZ"), "");

  // After a failure it is the last mark *reached*, which is what makes a
  // mark useful for saying why a pattern did not match.
  EXPECT_EQ(mark_of("a(*MARK:A)b|a(*MARK:B)c", "ad"), "B");

  // Every verb that takes a name sets it, except the one whose name is a
  // question rather than an answer.
  EXPECT_EQ(mark_of("a(*PRUNE:X)b", "ab"), "X");
  EXPECT_EQ(mark_of("a(*THEN:X)b", "ab"), "X");
  EXPECT_EQ(mark_of("a(*COMMIT:X)b", "ab"), "X");
  EXPECT_EQ(mark_of("a(*ACCEPT:X)b", "ab"), "X");
  EXPECT_EQ(mark_of("a(*SKIP:X)b", "ab"), "");
}

TEST(Perl, ANamedSkipResumesWhereItsMarkWasSet) {
  // `(*SKIP:NAME)` restarts the search at the position the most recent
  // `(*MARK:NAME)` was reached, not at its own position. pcre2test reports
  // "aaac" for the first of these and "ac" for the second, and the
  // difference is entirely which mark the name picks out.
  EXPECT_EQ(span_of("a(*MARK:A)aa(*MARK:B)a(*SKIP:A)b|a+c", "aaaac"), "1-5");
  EXPECT_EQ(span_of("aaa(*MARK:A)a(*SKIP:A)b|a+c", "aaaac"), "3-5");

  // A name no mark has is *ignored*, not treated as a bare `(*SKIP)`. The
  // two differ here: pcre2test reports the whole subject for the first and
  // no match at all for the second, because a bare `(*SKIP)` forbids every
  // attempt that starts before it as well as the one it fired on.
  EXPECT_EQ(span_of("a(*SKIP:X)b|a+c", "aac"), "0-3");
  EXPECT_EQ(span_of("a(*SKIP)b|a+c", "aac"), "nomatch");
}

TEST(Perl, AMarkMustHaveAName) {
  // pcre2test reports all three as "(*MARK) must have an argument": a mark
  // nothing can refer to marks nothing.
  EXPECT_EQ(compile_result("a(*MARK)b"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("abc(*MARK:)pqr"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("abc(*:)pqr"), GRX_ERR_SYNTAX);

  // `(*:NAME)` is `(*MARK:NAME)` written short, and works the same way.
  EXPECT_EQ(mark_of("a(*:A)b", "ab"), "A");
}

TEST(Perl, OnlyAcceptMayBeQuantified) {
  // `(*ACCEPT)` is the one verb that ends the match where it stands, so a
  // quantifier on it is unreachable rather than meaningless. pcre2test
  // reports "quantifier does not follow a repeatable item" for the other
  // six, and only `(*FAIL)` was refused here until a corpus record asked
  // about a repeated mark.
  EXPECT_EQ(compile_result("a(*ACCEPT)*b"), GRX_OK);
  for (const char * verb : {"FAIL", "COMMIT", "PRUNE", "SKIP", "THEN",
           "MARK:x"}) {
    EXPECT_EQ(compile_result(std::string("a(*") + verb + ")*b"),
        GRX_ERR_SYNTAX)
        << verb;
  }
  EXPECT_EQ(compile_result("a(*:x)*b"), GRX_ERR_SYNTAX);
}

TEST(Perl, AScanSubstringRunsItsBodyOverACapturedSubstring) {
  // PCRE2 10.45's `(*scs:(n)...)`. Every answer here is pcre2test's.
  //
  // Anchored at the substring's start, and not required to reach its end:
  // `(*scs:(1)a)` holds for a group that captured "ab" and `(*scs:(1)b)`
  // does not.
  EXPECT_EQ(span_of("(ab)(*scs:(1)a)", "ab"), "0-2");
  EXPECT_EQ(span_of("(ab)(*scs:(1)b)", "ab"), "nomatch");

  // The substring stands in for the whole subject, so `\z` is its end and
  // `^` its start - and the outer text on either side is unreadable.
  EXPECT_EQ(span_of("(\\d++)(*scs:(1)\\d+\\z)(\\w+)", "12ab"), "0-4");
  EXPECT_EQ(span_of("\\b(\\w++)(*scs:(1)^)", "hello world"), "0-5");

  // Zero-width where it stands: the outer match carries on from the same
  // place, which is what lets `(\w+)=(*scs:(1)\d+)(\w+)` drive `\w+` back
  // until group one is all digits.
  EXPECT_EQ(span_of("(\\w+)=(*scs:(1)\\d+)(\\w+)", "a1=xx"), "1-5");
  EXPECT_EQ(span_of("(\\w+)=(*scs:(1)\\d+)(\\w+)", "11=xx"), "0-5");
}

TEST(Perl, AScanSubstringTakesTheFirstGroupInItsListThatIsSet) {
  // Not the first that matches: pcre2test answers `(?:(x)|y)(b)(*scs:(1,2)b)`
  // against "yb" by scanning group two, because group one never captured.
  EXPECT_EQ(span_of("(?:(x)|y)(b)(*scs:(1,2)b)", "yb"), "0-2");
  EXPECT_EQ(span_of("(?:(x)|y)(b)(*scs:(1,2)x)", "yb"), "nomatch");

  // A group that captured nothing at all leaves no substring to scan, so the
  // assertion cannot hold.
  EXPECT_EQ(span_of("(*scs:(1)a)(a)|x", "a"), "nomatch");

  // The list takes the five spellings a subroutine call takes, and the
  // relative ones are resolved where they stand.
  EXPECT_EQ(span_of("(xyz)(abc)(*scs:(-1)abc)", "xyzabc"), "0-6");

  // A duplicated name is one name for several groups, so it contributes
  // several entries and the first *set* one is scanned - here the second.
  EXPECT_EQ(span_of("(?J)(?:(?'A'a)|(?<A>b))(*scs:('A')b)c", "bc"), "0-2");

  // A group the pattern does not have, and group zero, are both refused -
  // and `(*scs:(<x>)a)(?<x>a)` is not, because a name may be written later.
  EXPECT_EQ(compile_result("(*scs:(1)a|b)"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(*scs:(0)a)"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(*scs:(<name>)a|b)"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(*scs:(<x>)a)(?<x>a)"), GRX_OK);
  EXPECT_EQ(compile_result("(*scan_substring:(<x>)a)(?<x>a)"), GRX_OK);
}

TEST(Perl, AVerbInsideAPositiveAssertionEndsTheAttemptOutsideIt) {
  // pcre2test settles all four, and this library confined them to the
  // assertion until `(*scs:...)` made the question unavoidable. A `(*PRUNE)`
  // that fires inside a positive lookahead ends the attempt at that starting
  // position, so the second branch is tried one character later.
  EXPECT_EQ(span_of("a(?=b(*PRUNE)x).+|(.+)", "abcd"), "1-4");
  EXPECT_EQ(span_of("a(?=b(*COMMIT)x).+|(.+)", "abcd"), "nomatch");
  EXPECT_EQ(span_of("a(?=b(*SKIP)x).+|(.+)", "abcd"), "2-4");

  // A negative assertion discards it: the assertion succeeded, so nothing
  // failed and there is nothing to stop.
  EXPECT_EQ(span_of("a(?!b(*PRUNE)x).+|(.+)", "abcd"), "0-4");
  EXPECT_EQ(span_of("a(?!b(*COMMIT)x).+|(.+)", "abcd"), "0-4");

  // A `(*THEN)` with no alternative to go to is confined whatever the sign.
  // At the top level the same exhaustion is a `(*PRUNE)`, which is why it
  // needs a stop of its own rather than borrowing that one.
  EXPECT_EQ(span_of("a(?=b(*THEN)x)c|a.+|(.+)", "abcd"), "0-4");

  // And out of a scan substring, which is a positive assertion too.
  EXPECT_EQ(span_of("(a)(b)(*scs:(2)(*scs:(1)a(*PRUNE)x)).+|(.+)", "abcd"),
      "1-4");
  EXPECT_EQ(span_of("(a)(b)(*scs:(2)(*scs:(1)a(*COMMIT)x)).+|(.+)", "abcd"),
      "nomatch");
}

TEST(Perl, ALookbehindIsBoundedByWhatItsReferencesCanMatch) {
  // pcre2test measures a backreference by the group it names rather than
  // giving up on it: `(a)(?<=\1)` is a lookbehind of one character and
  // compiles, and `(a)(?<=\1+)` has no bound at all and does not.
  EXPECT_EQ(compile_result("(a)(?<=\\1)"), GRX_OK);
  EXPECT_EQ(compile_result("(ab)(?<=\\1)"), GRX_OK);
  EXPECT_EQ(compile_result("(a|bc)(?<=\\1)"), GRX_OK);
  EXPECT_EQ(compile_result("(a)(?<=\\1+)"), GRX_ERR_SYNTAX);

  // A subroutine call asks the same question, and `(?0)` - the whole
  // pattern - is the one with no answer.
  EXPECT_EQ(compile_result("(a+)(?<=b(?1))"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("()(?<=(?0))"), GRX_ERR_SYNTAX);

  // Two groups each as long as the other have no bound this can compute,
  // and a bound nobody can compute is not a bound.
  EXPECT_EQ(compile_result("(a\\2)(b\\1)(?<=\\2)"), GRX_ERR_SYNTAX);

  // ECMAScript bounds no lookbehind at all, so every one of these compiles
  // there - the measuring is the same, and what differs is the dialect's
  // answer to a body it cannot bound.
  EXPECT_EQ(compile_result("(a)(?<=\\1+)", GRX_SYNTAX_ECMASCRIPT), GRX_OK);
}

TEST(Perl, AThenGoesToTheNextAlternativeOfItsEnclosingGroup) {
  // The half of `(*THEN)` the other tests never reach: one with somewhere to
  // go. pcre2test matches "ac" here, because the verb sends the search to
  // the second branch of the group rather than to the next starting
  // position, which is what a `(*PRUNE)` in the same place would do.
  EXPECT_EQ(span_of("(?:a(*THEN)b|ac)", "ac"), "0-2");
  EXPECT_EQ(span_of("(?:a(*PRUNE)b|ac)", "ac"), "nomatch");

  // A mark set inside the branch the verb abandons goes with it.
  EXPECT_EQ(mark_of("(?:a(*MARK:M)(*THEN)b|ac)", "ac"), "");
  EXPECT_EQ(span_of("(?:a(*MARK:M)(*THEN)b|ac)", "ac"), "0-2");
}

TEST(Perl, TheTwoConstructsRefuseTheirOwnMalformedSpellings) {
  // An operator with nothing before it, which is the one place the `&` arm
  // of the extended-class operator table is reached: everywhere else the
  // intersection reader has already consumed it.
  EXPECT_EQ(compile_result("(?[ & [a] ])"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(?[ ^ [a] ])"), GRX_ERR_SYNTAX);

  // A scan-substring name with no closing delimiter.
  EXPECT_EQ(compile_result("(*scs:(<x)a)"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(*scs:('x)a)"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(*scs:()a)"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(*scs:"), GRX_ERR_SYNTAX);
}

TEST(Perl, ANonAtomicLookaroundCanBeReEntered) {
  // An ordinary lookaround is atomic: once its body has succeeded, nothing
  // goes back in for a second way through it. `(?=a|(.))\1` against "aa"
  // therefore fails - the first branch matched, group one is unset, and the
  // backreference has nothing to compare. `(*napla:a|(.))\1` gives the `a`
  // back, takes `(.)` instead, and matches. pcre2test answers both that way.
  EXPECT_EQ(span_of("(?=a|(.))\\1", "aa"), "nomatch");
  EXPECT_EQ(span_of("(*napla:a|(.))\\1", "aa"), "0-1");
  EXPECT_EQ(group_of("(*napla:a|(.))\\1", "aa", 1), "0-1");

  // Behind as well, where the body runs backwards and the rewind undoes
  // that the same way.
  EXPECT_EQ(span_of("(*naplb:(.)|x)\\1", "aa"), "1-2");

  // `(?*` and `(?<*` are the same two constructs written short.
  EXPECT_EQ(span_of("(?*a|(.))\\1", "aa"), "0-1");
  EXPECT_EQ(compile_result("(?<*a)"), GRX_OK);

  // Still zero-width, so it may be repeated, and a non-atomic lookbehind is
  // still a lookbehind: pcre2test bounds its body like any other.
  EXPECT_EQ(compile_result("(*napla:)+"), GRX_OK);
  EXPECT_EQ(compile_result("(*naplb:a{1,4})"), GRX_OK);
  EXPECT_EQ(compile_result("(*naplb:a+)"), GRX_ERR_SYNTAX);

  // A condition is asked once and answered once, so there is nowhere for a
  // second way through it to be tried from. pcre2test refuses this.
  EXPECT_EQ(compile_result("(?(*napla:xx)bc)"), GRX_ERR_SYNTAX);
}

TEST(Perl, KeepReportsTheLastValueAnIterationFinished) {
  // Perl keeps what a failed negative lookaround's body captured
  // (dialects.md section 5.17), and this says *which* value that is: the
  // last one an iteration finished writing, not whatever happens to be in
  // the slot when the body runs out of paths.
  //
  // The distinction is invisible until the body fails part way through an
  // iteration. A repeat clears its group at the top of every iteration, so
  // at that moment the group holds the clear and no replacement, and an
  // implementation that reports the raw slots answers by where in the
  // iteration the failure fell. Against "aaa":
  //
  //   (?!(a){2}$)   both iterations finish, then `$` fails
  //   (?!(aa){2}$)  the first finishes, the second dies inside its body
  //
  // Same construct, one operand wider. This library reported 1-2 for the
  // first and *unset* for the second, which is the shape of its own lowering
  // showing through. perl reports unset for the first and 0-2 for the
  // second, which is the shape of perl's opcode selection showing through -
  // each of us right once and wrong once, and neither stating a rule.
  EXPECT_EQ(group_of("(?!(a){2}$)", "aaa", 1, GRX_SYNTAX_PERL), "1-2");
  EXPECT_EQ(group_of("(?!(aa){2}$)", "aaa", 1, GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(group_of("(?!(aaa){2}$)", "aaa", 1, GRX_SYNTAX_PERL), "0-3");

  // Widening the count rather than the body asks the same question, and the
  // rule answers it the same way: the second iteration finishes at 2-3 and
  // the third dies, so 2-3 is the last finished value.
  EXPECT_EQ(group_of("(?!(a){3}$)", "aaa", 1, GRX_SYNTAX_PERL), "2-3");

  // No repeat, so nothing is ever cleared and the question does not arise.
  // perl agrees on both of these, and they are what section 5.17 is about.
  EXPECT_EQ(group_of("a(?!(b)c)", "abd", 1, GRX_SYNTAX_PERL), "1-2");
  EXPECT_EQ(group_of("(?!(a)(a)$)", "aaa", 1, GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(group_of("(?!(a)(a)$)", "aaa", 2, GRX_SYNTAX_PERL), "1-2");

  // A group inside a repeat that never completes an iteration has no value
  // to keep, and stays unset rather than inheriting one.
  EXPECT_EQ(group_of("a(?!(b)*c)", "aaa", 1, GRX_SYNTAX_PERL), "-");

  // The spans reported are always coherent. The first implementation of this
  // rule read the group's start out of the live slots at the moment its end
  // was written, and under the old arrangement - where a KEEP body's capture
  // undos were skipped - those slots could hold a start from one abandoned
  // path and an end from another: this pattern reported 28-27, a group
  // ending before it began. Captures are undone normally now.
  const std::string thirty_one(31, 'a');
  const std::string span
      = group_of("^(a*?)(?!(a{6}|a{5})*$)", thirty_one, 2, GRX_SYNTAX_PERL);
  ASSERT_NE(span, "-");
  const size_t dash = span.find('-');
  ASSERT_NE(dash, std::string::npos);
  EXPECT_LE(std::stoul(span.substr(0, dash)), std::stoul(span.substr(dash + 1)))
      << "group 2 reported as " << span;

  // ECMAScript and PCRE2 discard the writes instead, which this does not
  // touch: the axis is which of the two a dialect takes, and only the value
  // Perl keeps was ever in question.
  EXPECT_EQ(group_of("(?!(aa){2}$)", "aaa", 1, GRX_SYNTAX_ECMASCRIPT), "-");
  EXPECT_EQ(group_of("(?!(aa){2}$)", "aaa", 1, GRX_SYNTAX_PCRE), "-");
}

TEST(Perl, PerlsBoundTypesAreReadRatherThanMisread) {
  // `\b{wb}` is Perl's, and only Perl's: pcre2test compiles it as a word
  // boundary followed by four ordinary characters. This library did the same
  // for both until it was asked, which is a wrong answer wearing a right
  // one's clothes.
  //
  // `g` is in this list because it was missing from the one in the parser:
  // perlrebackslash gives it as an alias for `gcb`, and `\b{g}` was refused
  // here while Perl compiled it. No corpus record uses it, so the check was
  // against perl 5.40 rather than against the vectors.
  for (const char * spelling : {"\\b{wb}", "\\b{ wb }", "\\B{gcb}",
           "\\b{g}", "\\B{ g }", "\\b{sb}", "\\b{lb}"}) {
    EXPECT_EQ(compile_result(spelling, GRX_SYNTAX_PERL), GRX_OK) << spelling;
    EXPECT_EQ(compile_result(spelling, GRX_SYNTAX_PCRE), GRX_OK) << spelling;
  }

  // Uppercase and long names are refused, because Perl refuses them: the
  // accepted spellings are five, lower-case, with optional blanks inside the
  // braces and nothing else.
  for (const char * spelling : {"\\b{WB}", "\\b{Word_Boundary}", "\\b{GCB}"}) {
    EXPECT_EQ(compile_result(spelling, GRX_SYNTAX_PERL), GRX_ERR_SYNTAX)
        << spelling;
  }

  // An unknown one is a syntax error, not an unimplemented construct, and
  // `\b{3}` is one of those rather than a quantifier - which is where the
  // two dialects part, because pcre2test reads it as a quantifier on an
  // assertion and refuses it for that instead.
  EXPECT_EQ(compile_result("\\b{nosuch}", GRX_SYNTAX_PERL), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("\\b{}", GRX_SYNTAX_PERL), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("\\b{3}", GRX_SYNTAX_PERL), GRX_ERR_SYNTAX);

  // A brace that is not a bound at all leaves the boundary alone.
  EXPECT_TRUE(search("\\bx", "x", GRX_SYNTAX_PERL).matched);
}

TEST(Perl, ABranchResetGivesOneNumberOneName) {
  // pcre2test: "different names for subpatterns of the same number are not
  // allowed". The branches of `(?|...)` share their numbering, so naming the
  // same group two things asks for something a name cannot be.
  EXPECT_EQ(compile_result("(?|(?<a>A)|(?<b>B))"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(?|(?<a>A)(?<b>x)|(?<b>B)(?<a>y))"),
      GRX_ERR_SYNTAX);

  // The same name twice is what a branch reset is for, and a branch that
  // leaves the group unnamed is no conflict either.
  EXPECT_EQ(compile_result("(?|(?<a>A)|(?<a>B))"), GRX_OK);
  EXPECT_EQ(compile_result("(?|(?<a>A)|(B))"), GRX_OK);
  EXPECT_EQ(compile_result("(?|(A)|(?<b>B))"), GRX_OK);

  // Outside a branch reset the numbers differ, so the names may too.
  EXPECT_EQ(compile_result("(?<a>A)|(?<b>B)"), GRX_OK);

  // Perl's rule is the other one: it compiles this and lets each name mean
  // the branch it was written in, which is a different model of what a name
  // is and not one this library has.
  EXPECT_EQ(compile_result("(?|(?<a>A)|(?<b>B))", GRX_SYNTAX_PERL), GRX_OK);
}

TEST(Perl, TheCharsetModifiersChooseASemanticsAndExcludeEachOther) {
  // Perl's `/a` holds the shorthands and the POSIX classes to ASCII, which
  // is the narrowing counterpart of `(*UCP)`. `\xC3\x80` is U+00C0.
  const std::string agrave = "\xC3\x80";
  EXPECT_FALSE(search("(?a:\\w)", agrave, GRX_SYNTAX_PERL).matched);
  EXPECT_TRUE(search("(?u:\\w)", agrave, GRX_SYNTAX_PERL).matched);
  EXPECT_TRUE(search("(?d:\\w)", agrave, GRX_SYNTAX_PERL).matched);

  // `/l` asks for the locale's semantics and gets the C locale's, which is
  // the only locale this library has.
  EXPECT_FALSE(search("(?l:\\w)", agrave, GRX_SYNTAX_PERL).matched);
  EXPECT_TRUE(search("(?l:a?\\w)", "b", GRX_SYNTAX_PERL).matched);

  // The scope is the group's, so an inner letter wins inside it.
  EXPECT_TRUE(search("(?a:((?u)\\w)\\W)", agrave + agrave,
      GRX_SYNTAX_PERL).matched);

  // One semantics per pattern, and `a` is the one letter that may be written
  // twice - the two need not be adjacent, so `(?aia:` is `/aa`.
  EXPECT_EQ(compile_result("(?al:a)", GRX_SYNTAX_PERL), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(?au:a)", GRX_SYNTAX_PERL), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(?aaa:a)", GRX_SYNTAX_PERL), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(?uu:a)", GRX_SYNTAX_PERL), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(?aia:a)", GRX_SYNTAX_PERL), GRX_OK);
}

TEST(Perl, TheSecondAOfSlashAaCutsEveryFoldOrbitAtAscii) {
  // U+017F is the long s, which simple folding puts in one orbit with `s`
  // and `S`. Under `/ai` the letter `s` matches it; under `/aai` it does
  // not - and U+00C0 still matches U+00E0, because both of those are
  // outside ASCII. So `/aa` is not "fold ASCII only", which would stop the
  // second pair too.
  const std::string long_s = "\xC5\xBF";
  EXPECT_TRUE(search("(?ai:s)", long_s, GRX_SYNTAX_PERL).matched);
  EXPECT_FALSE(search("(?aai:s)", long_s, GRX_SYNTAX_PERL).matched);
  EXPECT_TRUE(search("(?aia:s)", "S", GRX_SYNTAX_PERL).matched);
  EXPECT_TRUE(search("(?aai:\xC3\x80)", "\xC3\xA0", GRX_SYNTAX_PERL).matched);

  // And `/aa` does not take *full* folding with it, though this library and
  // its documentation both said it did. The claim was that every full fold
  // has an ASCII character somewhere in it, so none could survive the cut.
  // That is false for 87 of the 104 code points with a full fold: Perl keeps
  // a fold under `/aa` exactly when no code point of the fold is ASCII.
  //
  // `ss` is ASCII, so `\u00df` loses it. `\u0390` folds to
  // U+03B9 U+0308 U+0301 and `\u1fb3` to U+03B1 U+03B9, neither of which
  // touches ASCII, so both keep it - in a class as well as bare, and in the
  // reverse direction, which is the half a corpus of forward cases misses.
  const std::string sharp = "\xC3\x9F";              // U+00DF -> "ss"
  const std::string iota = "\xCE\x90";               // U+0390 -> 3, no ASCII
  const std::string iota_fold = "\xCE\xB9\xCC\x88\xCC\x81";
  const std::string alpha_i = "\xE1\xBE\xB3";        // U+1FB3 -> U+03B1 U+03B9
  const std::string alpha_i_fold = "\xCE\xB1\xCE\xB9";
  const std::string ff = "\xEF\xAC\x80";             // U+FB00 -> "ff"

  EXPECT_EQ(span_of("(?i)^(?:" + sharp + ")$", "ss", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("(?aai)^(?:" + sharp + ")$", "ss", GRX_SYNTAX_PERL),
      "nomatch");
  EXPECT_EQ(span_of("(?aai)^(?:" + iota + ")$", iota_fold, GRX_SYNTAX_PERL),
      "0-6");
  EXPECT_EQ(span_of("(?aai)^(?:[" + iota + "])$", iota_fold, GRX_SYNTAX_PERL),
      "0-6");
  EXPECT_EQ(span_of("(?aai)^(?:" + alpha_i + ")$", alpha_i_fold,
                GRX_SYNTAX_PERL),
      "0-4");

  // The reverse direction: a fold written out, matched by the single
  // character that folds to it. Kept when the fold is not ASCII, dropped
  // when it is.
  EXPECT_EQ(span_of("(?aai)^(?:" + alpha_i_fold + ")$", alpha_i,
                GRX_SYNTAX_PERL),
      "0-3");
  EXPECT_EQ(span_of("(?i)^(?:ff)$", ff, GRX_SYNTAX_PERL), "0-3");
  EXPECT_EQ(span_of("(?aai)^(?:ff)$", ff, GRX_SYNTAX_PERL), "nomatch");

  // `\K` may be repeated in Perl and is error 109 in pcre2test. Repeating it
  // changes nothing - it moves the reported start to here, and moving it
  // here again leaves it here - which is why `(?iaa:A?\K*)` reports 1-1.
  EXPECT_EQ(span_of("(?iaa:A?\\K*)", "African_Feh", GRX_SYNTAX_PERL), "1-1");
  EXPECT_EQ(compile_result("A\\K*", GRX_SYNTAX_PCRE), GRX_ERR_SYNTAX);
}

TEST(Perl, ANamedCodePointResolvesAgainstTheCharacterNameTable) {
  // `\N{NAME}` is Perl's, and PCRE2 does not have it at all - pcre2test
  // answers error 137, "PCRE2 does not support \F, \L, \l, \N{name}, \U,
  // or \u" - so one spelling gets two answers.
  //
  // Matching is exact and case sensitive, which is Perl's rule rather than a
  // simplification of it: every loose spelling below is an error in perl
  // 5.40.1 too, and each was tried there before this test was written.
  EXPECT_EQ(span_of("\\N{LATIN SMALL LETTER A}", "a", GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("\\N{GREEK SMALL LETTER ALPHA}", "\xCE\xB1",
                GRX_SYNTAX_PERL),
      "0-2");
  EXPECT_EQ(compile_result("\\N{LATIN SMALL LETTER A}", GRX_SYNTAX_PCRE),
      GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("\\N{latin small letter a}", GRX_SYNTAX_PERL),
      GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("\\N{LATIN-SMALL-LETTER-A}", GRX_SYNTAX_PERL),
      GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("\\N{LATIN  SMALL  LETTER  A}", GRX_SYNTAX_PERL),
      GRX_ERR_SYNTAX);

  // An unknown name is a *syntax* error and not an unsupported construct,
  // because Perl rejects it too: `/abc\N{def}/` does not compile there, and
  // `re_tests` carries four rows that turn on it. GRX_ERR_UNSUPPORTED would
  // promise that the dialect accepts this.
  EXPECT_EQ(compile_result("abc\\N{def}", GRX_SYNTAX_PERL), GRX_ERR_SYNTAX);
  {
    Attempt attempt = compile("\\N{NO SUCH CHARACTER}", GRX_SYNTAX_PERL);
    EXPECT_EQ(attempt.result, GRX_ERR_SYNTAX);
    EXPECT_EQ(attempt.diag, GRX_DIAG_UNKNOWN_CHARACTER_NAME);
    grx_regex_free(attempt.regex);
  }

  // All five alias kinds from NameAliases.txt, because Perl resolves all
  // five and a table built from UnicodeData alone would answer "unknown" to
  // things that work there. Abbreviation, control, correction, figment.
  EXPECT_EQ(span_of("\\N{LF}", "\n", GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("\\N{ALERT}", "\a", GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("\\N{LATIN CAPITAL LETTER GHA}", "\xC6\xA2",
                GRX_SYNTAX_PERL),
      "0-2");
  EXPECT_EQ(span_of("\\N{WEIERSTRASS ELLIPTIC FUNCTION}", "\xE2\x84\x98",
                GRX_SYNTAX_PERL),
      "0-3");

  // The computed families, which are not in the table: storing the Hangul
  // syllables alone would add 11,172 rows and the CJK ideographs 100,000.
  EXPECT_EQ(span_of("\\N{CJK UNIFIED IDEOGRAPH-4E00}", "\xE4\xB8\x80",
                GRX_SYNTAX_PERL),
      "0-3");
  EXPECT_EQ(span_of("\\N{HANGUL SYLLABLE GA}", "\xEA\xB0\x80",
                GRX_SYNTAX_PERL),
      "0-3");
  EXPECT_EQ(span_of("\\N{HANGUL SYLLABLE GAG}", "\xEA\xB0\x81",
                GRX_SYNTAX_PERL),
      "0-3");
  EXPECT_EQ(span_of("\\N{HANGUL SYLLABLE HIH}", "\xED\x9E\xA3",
                GRX_SYNTAX_PERL),
      "0-3");
  EXPECT_EQ(span_of("\\N{TANGUT IDEOGRAPH-17000}", "\xF0\x97\x80\x80",
                GRX_SYNTAX_PERL),
      "0-4");
  // A name of the right shape for a range it does not fall in is not a name.
  EXPECT_EQ(compile_result("\\N{CJK UNIFIED IDEOGRAPH-0041}",
                GRX_SYNTAX_PERL),
      GRX_ERR_SYNTAX);

  // `BELL` is U+1F514 and U+0007 is `ALERT`. Perl agrees, and the pair is
  // here because "the obvious answer" and "the Unicode answer" differ.
  EXPECT_EQ(span_of("\\N{BELL}", "\xF0\x9F\x94\x94", GRX_SYNTAX_PERL),
      "0-4");
  EXPECT_EQ(span_of("\\N{BELL}", "\a", GRX_SYNTAX_PERL), "nomatch");

  // Nineteen names have two adjacent separators, and the encoder dropped one
  // of the pair until it was caught. That lost more than those nineteen: the
  // table is sorted by the real name and searched by the decoded one, so the
  // ordering invariant broke and the search walked past healthy neighbours -
  // DDHA below has no adjacent separators and was unreachable all the same.
  EXPECT_EQ(span_of("\\N{ZANABAZAR SQUARE LETTER -A}", "\xF0\x91\xA8\xA9",
                GRX_SYNTAX_PERL),
      "0-4");
  EXPECT_EQ(span_of("\\N{ZANABAZAR SQUARE LETTER DDHA}",
                "\xF0\x91\xA8\x97", GRX_SYNTAX_PERL),
      "0-4");

  // Space inside the braces is ignored at both ends, as it is for `\N{U+h}`.
  EXPECT_EQ(span_of("\\N{ SPACE }", " ", GRX_SYNTAX_PERL), "0-1");

  // The spellings that were already here keep working, so the name branch
  // cannot have swallowed them. `(*UTF)` because pcre2 makes `\N{U+hh}`
  // UTF-only and refuses it without, exactly as this library does.
  EXPECT_EQ(span_of("\\N{U+0041}", "A", GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("(*UTF)\\N{U+0041}", "A", GRX_SYNTAX_PCRE), "0-1");
  EXPECT_EQ(span_of("\\N", "a", GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("\\N", "\n", GRX_SYNTAX_PCRE), "nomatch");
  EXPECT_EQ(span_of("a\\N{2}b", "axyb", GRX_SYNTAX_PERL), "0-4");

  // And in a class, where it is a member and a range endpoint.
  EXPECT_EQ(span_of("^[\\N{LATIN SMALL LETTER A}]$", "a", GRX_SYNTAX_PERL),
      "0-1");
  EXPECT_EQ(span_of("^[\\N{LATIN SMALL LETTER A}-\\N{LATIN SMALL LETTER C}]$",
                "b", GRX_SYNTAX_PERL),
      "0-1");
}

TEST(Perl, AnIterationThatConsumedNothingStopsTheLoopRatherThanFailing) {
  // documentation/dialects.md section 5.5's BREAK_ON_EMPTY. `(a*)*` against
  // "b" reports group 1 as the empty string in perl and in pcre2test, and as
  // unset in ECMAScript, whose RepeatMatcher fails the iteration instead.
  // Neither the Perl row nor the PCRE2 row said which it was, so both got
  // the zero that means ECMAScript's - and nothing asked until `\K*` did.
  EXPECT_EQ(group_of("(a*)*", "b", 1, GRX_SYNTAX_PERL), "0-0");
  EXPECT_EQ(group_of("(a*)*", "b", 1, GRX_SYNTAX_PCRE), "0-0");
  EXPECT_EQ(group_of("(a*)*", "b", 1, GRX_SYNTAX_ECMASCRIPT), "-");

  // `(a*)+` sets it in all three: one iteration is forced.
  EXPECT_EQ(group_of("(a*)+", "b", 1, GRX_SYNTAX_PERL), "0-0");
  EXPECT_EQ(group_of("(a*)+", "b", 1, GRX_SYNTAX_ECMASCRIPT), "0-0");
}

TEST(Perl, TheTwoRepeatCountsTheDialectsAnswerDifferently) {
  // 65535 is the dialect's cap and no option raises it, so it is a syntax
  // error and not a limit: pcre2test reports `/z{65536}/` as "number too big
  // in {} quantifier". GRX_Limits::max_repeat_count is the other thing, and
  // reporting both as one made a corpus record look like a resource failure.
  EXPECT_EQ(compile("z{65536}").diag, GRX_DIAG_REPEAT_COUNT_TOO_LARGE);
  EXPECT_EQ(compile_result("z{65536}"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("z{65535}"), GRX_OK);

  // perl warns about `{n,m}` with n > m and compiles it as a construct that
  // never matches, so the optional group around it takes its empty branch.
  // pcre2test refuses the same pattern outright.
  EXPECT_EQ(span_of("((def){37,17})?ABC", "ABC", GRX_SYNTAX_PERL), "0-3");
  EXPECT_EQ(group_of("((def){37,17})?ABC", "ABC", 1, GRX_SYNTAX_PERL), "-");
  EXPECT_FALSE(search("(def){37,17}", "def", GRX_SYNTAX_PERL).matched);
  EXPECT_EQ(compile_result("(a){3,1}", GRX_SYNTAX_PCRE), GRX_ERR_SYNTAX);
}

TEST(Perl, PerlQuantifiesAnAnchorAndPcre2DoesNot) {
  // `a$?` and `^*` compile in perl and are error 109 in pcre2test. The
  // corpus row that asked is `m?^xy\?$?`, which Perl's own harness reads as
  // the match operator and every importer here reads as a pattern ending in
  // a quantified `$`.
  EXPECT_EQ(span_of("m?^xy\\?$?", "xy?", GRX_SYNTAX_PERL), "0-3");
  EXPECT_EQ(compile_result("a$?", GRX_SYNTAX_PERL), GRX_OK);
  EXPECT_EQ(compile_result("^*", GRX_SYNTAX_PERL), GRX_OK);
  EXPECT_EQ(compile_result("a$?", GRX_SYNTAX_PCRE), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("^*", GRX_SYNTAX_PCRE), GRX_ERR_SYNTAX);
}

TEST(Perl, AGroupInsideABranchResetHasNoLengthToMeasure) {
  // Its number is shared with the other branches', so which of them a
  // reference means is not decided until the match runs. pcre2test refuses
  // this even with one branch, and takes the same lookbehind over a group
  // written outside one.
  EXPECT_EQ(compile_result("(?|([ab]))...(?<=\\1)z"), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("([ab])...(?<=\\1)z"), GRX_OK);
}

TEST(Perl, ASubroutineCallNamesADefinitionRatherThanANumber) {
  // A `(?|...)` gives one number a definition per branch, and they are not
  // the same program: the three branches below match `a`, `b` and `c`. Which
  // one a call re-enters is therefore not a question the number can answer,
  // and the call has to carry where its target was written.
  //
  // `(?-1)` means the definition beside it - the group most recently opened
  // where the call stands - so each branch calls its own and the pattern
  // matches a doubled letter of any of the three. Resolving it to the number
  // instead sent all three calls to the first branch's, which matched "aa"
  // and refused "bb" and "cc".
  const char * doubled = "((?|(?<a>a)(?-1)|(?<b>b)(?-1)|(?<c>c)(?-1)))";
  EXPECT_EQ(span_of(doubled, "aa", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of(doubled, "bb", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of(doubled, "cc", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of(doubled, "ab", GRX_SYNTAX_PERL), "nomatch");

  // A name names a definition too, and here the two names are the same
  // number. `(?&a)` matches `a` and `(?&b)` matches `b`, so "bbab" is the
  // branch's own `b`, the backreference to it, then one of each.
  EXPECT_EQ(span_of("(?|(?<a>a)|(?<b>b))\\1(?&a)(?&b)", "bbab",
                GRX_SYNTAX_PERL),
      "0-4");

  // Outside a branch reset nothing changes: one number, one definition, and
  // a call by number still finds it.
  EXPECT_EQ(span_of("(a|b)(?1)", "ab", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("(?<x>a|b)(?&x)", "ba", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("(a(?R)?b)", "aabb", GRX_SYNTAX_PERL), "0-4");
}

TEST(Perl, CaselessMatchingUsesFullFoldingAndSoCanChangeLength) {
  // Perl's `/i` is Unicode *full* case folding, and full folding is the one
  // kind a class cannot express: it maps one code point to a sequence. So a
  // caseless match can be a different length from the pattern that asked for
  // it, in either direction.
  const std::string sharp_s = "\xC3\x9F";        // U+00DF
  const std::string capital_sharp_s = "\xE1\xBA\x9E"; // U+1E9E
  const std::string ff = "\xEF\xAC\x80";        // U+FB00, the ff ligature
  const std::string fi = "\xEF\xAC\x81";        // U+FB01
  const std::string ffi = "\xEF\xAC\x83";       // U+FB03
  const std::string ffl = "\xEF\xAC\x84";       // U+FB04
  const std::string st = "\xEF\xAC\x85";        // U+FB05

  // One pattern character standing for two subject characters.
  EXPECT_EQ(span_of("(?i)" + sharp_s, "ss", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("(?i)" + sharp_s, sharp_s, GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("(?i)" + sharp_s, capital_sharp_s, GRX_SYNTAX_PERL), "0-3");

  // Two pattern characters standing for one subject character.
  EXPECT_EQ(span_of("(?i)ss", sharp_s, GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("(?i)ff", ff, GRX_SYNTAX_PERL), "0-3");
  EXPECT_EQ(span_of("(?i)fi", fi, GRX_SYNTAX_PERL), "0-3");
  EXPECT_EQ(span_of("(?i)ffiffl", ffi + ffl, GRX_SYNTAX_PERL), "0-6");
  EXPECT_EQ(span_of("(?i)ssst", sharp_s + st, GRX_SYNTAX_PERL), "0-5");

  // And the case that makes this a property of the *run* rather than of each
  // character: the two meet in the middle. `sß` and `ßs` both fold to "sss",
  // so the pattern's first `s` is matched by the first half of the subject's
  // `ß` and the pattern's `ß` finishes inside the subject's `s`.
  EXPECT_EQ(span_of("(?i)s" + sharp_s, sharp_s + "s", GRX_SYNTAX_PERL), "0-3");

  // A fold run is an ordinary part of a pattern: quantified, inside a
  // lookbehind, and next to things that are not folded at all.
  EXPECT_EQ(span_of("(?i)" + sharp_s + "+", "ssss", GRX_SYNTAX_PERL), "0-4");
  EXPECT_EQ(span_of("(?i)x(?<=" + sharp_s + "x)", "ssx", GRX_SYNTAX_PERL),
      "2-3");
  EXPECT_EQ(span_of("(?i)a" + sharp_s + "b", "aSSb", GRX_SYNTAX_PERL), "0-4");

  // Nothing that does not need it is changed: an ASCII run is still an
  // ASCII run, and a non-caseless one is not folded at all.
  EXPECT_EQ(span_of("(?i)abc", "ABC", GRX_SYNTAX_PERL), "0-3");
  EXPECT_EQ(span_of(sharp_s, "ss", GRX_SYNTAX_PERL), "nomatch");

  // A class takes part in full folding too, which is the one place a class
  // is not simply "one character". `[\u00df]` matches "ss" in perl, and now
  // here. This comment used to say Perl agreed that a class folds simply; it
  // does not, and the measurement is what settled it - every code point whose
  // full fold is longer than one code point, the 104 `F` lines of
  // CaseFolding.txt, bare and in a class, both directions. Bare agreed 104 of
  // 104 before the fix and in a class 0 of 104; both are 104 of 104 now, on
  // all three engines.
  //
  // tests/data/vectors/perl/folding.rxt is the corpus that holds it, so these
  // are the readable statement of the rule rather than its coverage.
  EXPECT_EQ(span_of("(?i)[" + sharp_s + "]", "ss", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("(?i)^(?:[" + sharp_s + "])$", "ss", GRX_SYNTAX_PERL),
      "0-2");

  // The fold's own code points match by their orbit, so the subject may be
  // cased any way at all - "SS", and the long s as well.
  EXPECT_EQ(span_of("(?i)^(?:[" + sharp_s + "])$", "SS", GRX_SYNTAX_PERL),
      "0-2");
  EXPECT_EQ(span_of("(?i)^(?:[" + sharp_s + "])$", "s\xC5\xBF",
                GRX_SYNTAX_PERL),
      "0-3");

  // And the class still matches what it always did.
  EXPECT_EQ(span_of("(?i)[" + sharp_s + "]", sharp_s, GRX_SYNTAX_PERL), "0-2");

  // It is **one-directional**, which is what keeps a class a class: `[s]`
  // does not match `\u00df`, though the literal "ss" does. A class stands for
  // one character, so there is nothing for the second half of a two-character
  // fold to come from.
  EXPECT_EQ(span_of("(?i)^(?:[s])$", sharp_s, GRX_SYNTAX_PERL), "nomatch");
  EXPECT_EQ(span_of("(?i)^(?:ss)$", sharp_s, GRX_SYNTAX_PERL), "0-2");

  // Three code points, and a fold that is itself a combining sequence.
  EXPECT_EQ(span_of("(?i)^(?:[\xEF\xAC\x83])$", "ffi", GRX_SYNTAX_PERL),
      "0-3");
  EXPECT_EQ(span_of("(?i)^(?:[\xC7\xB0])$", "j\xCC\x8C", GRX_SYNTAX_PERL),
      "0-3");

  // It composes like any other branch.
  EXPECT_EQ(span_of("(?i)^(?:[" + sharp_s + "]{2})$", "ssss",
                GRX_SYNTAX_PERL),
      "0-4");

  // Two members with one fold produce one branch, not two, and the answer is
  // the same either way round.
  EXPECT_EQ(span_of("(?i)^(?:[" + sharp_s + capital_sharp_s + "])$", "ss",
                GRX_SYNTAX_PERL),
      "0-2");

  // The shapes perl does *not* full-fold in a class, which this library
  // answers the same way. Anchored, because the question is whether the class
  // can match *both* characters - unanchored, every one of these matches the
  // first `s` and answers a different question. A negated class is the one
  // that would be easiest to get wrong by generalising.
  EXPECT_EQ(span_of("(?i)^(?:[^" + sharp_s + "])$", "ss", GRX_SYNTAX_PERL),
      "nomatch");
  EXPECT_EQ(span_of("(?i)^(?:[a-\xC3\xBF])$", "ss", GRX_SYNTAX_PERL),
      "nomatch");
  EXPECT_EQ(span_of("(?i)^(?:[\\w])$", "ss", GRX_SYNTAX_PERL), "nomatch");
  EXPECT_EQ(span_of("(?i)^(?:[\\p{Latin}])$", "ss", GRX_SYNTAX_PERL),
      "nomatch");
  EXPECT_EQ(span_of("(?i)^(?:[[:alpha:]])$", "ss", GRX_SYNTAX_PERL),
      "nomatch");

  // A degenerate range is a single member, and perl treats it as one.
  EXPECT_EQ(span_of("(?i)^(?:[" + sharp_s + "-" + sharp_s + "])$", "ss",
                GRX_SYNTAX_PERL),
      "0-2");
}

TEST(Perl, PcreFoldsSimplyWherePerlFoldsFully) {
  // The same pattern, the same subject, two dialects. PCRE2 has no full
  // folding - pcre2pattern says so, and pcre2test agrees - so this is a
  // difference between the dialects rather than a gap in one of them, and
  // it is the reason the fold kind is a row in the profile table.
  const std::string sharp_s = "\xC3\x9F";
  EXPECT_EQ(span_of("(?i)" + sharp_s, "ss", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("(?i)" + sharp_s, "ss", GRX_SYNTAX_PCRE), "nomatch");
  EXPECT_EQ(span_of("(?i)ss", sharp_s, GRX_SYNTAX_PCRE), "nomatch");
}

TEST(Perl, TheSegmentationBoundariesAreAlgorithmsRatherThanSets) {
  // `\b` asks whether the characters either side are in the word class, and
  // a class is what the instruction carries. These four cannot be a class:
  // where a grapheme cluster ends is a dozen rules over the surrounding
  // text, and a line break is thirty. src/unicode/break.c has them, gated
  // against the Unicode Consortium's own conformance files in
  // tests/unit/test_break.cpp.
  const std::string combining = "e\xCC\x81";      // e + U+0301
  const std::string hangul = "\xE1\x84\x80\xE1\x85\xA1"; // U+1100 U+1161

  // A grapheme boundary falls between letters and not inside a combining
  // sequence or a Hangul syllable.
  EXPECT_EQ(span_of("a\\b{gcb}b", "ab", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("e\\b{gcb}", combining, GRX_SYNTAX_PERL), "nomatch");
  EXPECT_EQ(span_of("e\\B{gcb}", combining, GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("\xE1\x84\x80\\B{gcb}", hangul, GRX_SYNTAX_PERL), "0-3");

  // A word boundary is not a grapheme boundary: "ab" has one of the second
  // between the letters and none of the first.
  EXPECT_EQ(span_of("a\\b{wb}b", "ab", GRX_SYNTAX_PERL), "nomatch");
  EXPECT_EQ(span_of("a\\B{wb}b", "ab", GRX_SYNTAX_PERL), "0-2");

  // UAX #14 never breaks at the start of the text (LB2) where UAX #29 always
  // does (GB1, WB1, SB1). That is the one place the four disagree about the
  // same position, so it is the one worth pinning.
  EXPECT_EQ(span_of("^\\b{gcb}", "a", GRX_SYNTAX_PERL), "0-0");
  EXPECT_EQ(span_of("^\\b{wb}", "a", GRX_SYNTAX_PERL), "0-0");
  EXPECT_EQ(span_of("^\\b{sb}", "a", GRX_SYNTAX_PERL), "0-0");
  EXPECT_EQ(span_of("^\\b{lb}", "a", GRX_SYNTAX_PERL), "nomatch");

  // An empty subject has no boundary of any kind. Neither standard says so -
  // both break at the ends - but there are no characters, so there is
  // nothing for a boundary to fall between, and it is what Perl answers.
  for (const char * kind : {"gcb", "g", "wb", "sb", "lb"}) {
    EXPECT_EQ(span_of(std::string("\\b{") + kind + "}", "", GRX_SYNTAX_PERL),
        "nomatch")
        << kind;
    EXPECT_EQ(span_of(std::string("\\B{") + kind + "}", "", GRX_SYNTAX_PERL),
        "0-0")
        << kind;
  }

  // PCRE2 has none of them: it reads `\b{wb}` as a word boundary and four
  // ordinary characters, which is a wrong answer wearing a right one's
  // clothes and is why the spelling is Perl's alone here.
  EXPECT_EQ(span_of("\\b{wb}", "a{wb}", GRX_SYNTAX_PCRE), "1-5");
}

TEST(Perl, AGraphemeClusterIsOneThingAndDoesNotComeApart) {
  // `\X` is built out of the grapheme boundary rather than out of a second
  // reading of UAX #29: one character, then every character that does not
  // begin a new cluster. One algorithm, so the two cannot disagree.
  const std::string combining = "e\xCC\x81";              // e + U+0301
  const std::string crlf = "a\r\nb";
  const std::string flag = "\xF0\x9F\x87\xA6\xF0\x9F\x87\xA7"; // two RI

  EXPECT_EQ(span_of("\\X", combining, GRX_SYNTAX_PERL), "0-3");
  EXPECT_EQ(span_of("a\\K\\X", crlf, GRX_SYNTAX_PERL), "1-3");
  EXPECT_EQ(span_of("\\X", flag, GRX_SYNTAX_PERL), "0-8");

  // Atomic, because a cluster does not come apart: `\X\X` against one
  // cluster must not match by letting the first give back half of it. Perl
  // reports no match here, and so does this.
  EXPECT_EQ(span_of("\\X\\X", combining, GRX_SYNTAX_PERL), "nomatch");
  EXPECT_EQ(span_of("\\X\\X", flag, GRX_SYNTAX_PERL), "nomatch");
  EXPECT_EQ(span_of("\\X\\X", combining + combining, GRX_SYNTAX_PERL), "0-6");

  // An empty subject has no cluster in it.
  EXPECT_EQ(span_of("\\X", "", GRX_SYNTAX_PERL), "nomatch");
}

TEST(Perl, AFalseRangeIsThreeMembersInPerlAndAnErrorInPcre2) {
  // `[a-\d]` cannot be a range: one end is a set. Perl warns "False []
  // range" and compiles it as three members - "a", a literal "-" and a
  // digit - where pcre2test answers error 150, "invalid range in character
  // class", and CPython raises "bad character range". Section 5.12 of
  // documentation/dialects.md has carried that split since the table was
  // written; the reader refused it for every dialect until a soak seed
  // spelled `[a-\p{L}[[:alpha:]]`.
  //
  // Measured against perl 5.40.1 a member at a time, which is what says
  // the `-` is really there and not swallowed.
  EXPECT_EQ(span_of("[a-\\d]", "-", GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("[a-\\d]", "1", GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("[a-\\d]", "a", GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("[a-\\d]", "b", GRX_SYNTAX_PERL), "nomatch");

  // Either end, and every spelling of a set: a class escape, a property
  // and a POSIX class all do it, and so does a set at both ends.
  EXPECT_EQ(span_of("[\\d-a]", "-", GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("[a-\\p{L}]", "-", GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("[a-\\p{L}]", "z", GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("[a-[:alpha:]]", "-", GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("[[:alpha:]-a]", "-", GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("[\\w-\\d]", "-", GRX_SYNTAX_PERL), "0-1");

  // The row the seed spelled, which is a false range and a POSIX class in
  // one collection.
  EXPECT_EQ(span_of("[a-\\p{L}[[:alpha:]]", "a", GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("[a-\\p{L}[[:alpha:]]", "1", GRX_SYNTAX_PERL), "nomatch");

  // PCRE2 and Python refuse every one of them, which is the other half of
  // the rule and the reason this is a dialect flag rather than a change to
  // the reader.
  EXPECT_EQ(compile_result("[a-\\d]", GRX_SYNTAX_PCRE), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("[a-\\d]", GRX_SYNTAX_PYTHON), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("[a-\\p{L}]", GRX_SYNTAX_PCRE), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("[a-[:alpha:]]", GRX_SYNTAX_PCRE), GRX_ERR_SYNTAX);

  // And a range that *is* one stays a range, in Perl as well: the flag is
  // about an endpoint that is a set, not about the `-`. A reversed range
  // is an error in perl too - "Invalid [] range" rather than the warning.
  EXPECT_EQ(span_of("[a-z]", "b", GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("[a-z]", "-", GRX_SYNTAX_PERL), "nomatch");
  EXPECT_EQ(compile_result("[z-a]", GRX_SYNTAX_PERL), GRX_ERR_SYNTAX);
}

TEST(Perl, NoCaptureModeLeavesNoGroupForANumberToName) {
  // `/n` and `(?n)` stop a bare parenthesis capturing, so `(?n)(a)\1`
  // names a group the pattern has not got: "Reference to nonexistent
  // group" in perl 5.40.1 and error 15 in pcre2test. It compiled here and
  // answered no match, because the check made while reading counts
  // parentheses in a prescan and the prescan cannot know what a flag
  // written inside the pattern will do.
  EXPECT_EQ(compile_result("(?n)(a)\\1", GRX_SYNTAX_PERL), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(?n)(a)\\1", GRX_SYNTAX_PCRE), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(?n)\\1(a)", GRX_SYNTAX_PCRE), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(?n:(a))\\1", GRX_SYNTAX_PCRE), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(?n:(a)\\1)", GRX_SYNTAX_PCRE), GRX_ERR_SYNTAX);

  // The scope is the flag's own, and the numbering was already right: a
  // group opened *before* the flag still captures and may still be named.
  EXPECT_EQ(span_of("(a)(?n)(b)\\1", "aba", GRX_SYNTAX_PERL), "0-3");
  EXPECT_EQ(group_of("(a)(?n)(b)\\1", "aba", 1, GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(compile_result("(a)(?n)(b)\\2", GRX_SYNTAX_PERL), GRX_ERR_SYNTAX);
  EXPECT_EQ(span_of("((?n)(a))\\1", "aa", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("(?n)((?-n)(a))\\1", "aa", GRX_SYNTAX_PERL), "0-2");

  // A named group captures under `/n` in both references, and a name
  // still resolves.
  EXPECT_EQ(span_of("(?n)(?<x>a)\\k<x>", "aa", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("(?n)(?<x>a)\\1", "aa", GRX_SYNTAX_PERL), "0-2");

  // A conditional asks the same question, and the two dialects answer it
  // the way they answer `(?(99)a|b)`: perl takes the false branch, pcre2
  // refuses a condition naming a subpattern that is not there.
  EXPECT_EQ(span_of("(?n)(a)(?(1)b|c)", "ac", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(compile_result("(?n)(a)(?(1)b|c)", GRX_SYNTAX_PCRE),
      GRX_ERR_SYNTAX);

  // Nothing that was legal became illegal. A forward reference is still
  // read as one, a branch reset still numbers its branches alike, and the
  // number one past the last is still the error it always was.
  EXPECT_EQ(span_of("\\1(a)", "aa", GRX_SYNTAX_PERL), "nomatch");
  EXPECT_EQ(span_of("(?|(a)|(b)(c))\\2", "ab", GRX_SYNTAX_PERL), "nomatch");
  EXPECT_EQ(compile_result("(?|(a)|(b)(c))\\3", GRX_SYNTAX_PERL),
      GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("(a)\\2", GRX_SYNTAX_PERL), GRX_ERR_SYNTAX);
}

TEST(Perl, HorizontalAndVerticalSpaceAreFixedSets) {
  // `\h` and `\v` were lowered through the same switch as `\d`, `\w` and
  // `\s`, which chooses an ASCII set whenever the dialect's shorthands are
  // not Unicode. That is the right question for those three and the wrong
  // one for these two: neither reference narrows `\h` or `\v` for anything.
  //
  // Measured 2026-09-24. pcre2test 10.46 matches U+00A0 with `\h` and
  // U+2028 with `\v` under `utf` alone, under `utf,ucp`, and under `(?a)`
  // with either; in 8-bit mode with no UTF at all it matches the bytes 0xA0
  // and 0x85. perl agrees on all of it, with the subject upgraded so the
  // rule being read is the Unicode one.
  const std::string nbsp = "\xc2\xa0";        // U+00A0
  const std::string line_sep = "\xe2\x80\xa8"; // U+2028
  const std::string next_line = "\xc2\x85";    // U+0085

  // The three modes the old code distinguished and should not have. UCP is
  // the one that used to decide it in the PCRE2 dialect, where the profile's
  // narrow shorthands are ASCII.
  const uint32_t modes[] = {
    (uint32_t)GRX_OPT_UTF,
    (uint32_t)(GRX_OPT_UTF | GRX_OPT_UCP),
    (uint32_t)(GRX_OPT_UTF | GRX_OPT_UCP | GRX_OPT_ASCII_CLASSES),
    (uint32_t)(GRX_OPT_UTF | GRX_OPT_ASCII_CLASSES),
  };
  for (uint32_t options : modes) {
    for (GRX_Syntax syntax : {GRX_SYNTAX_PCRE, GRX_SYNTAX_PERL}) {
      EXPECT_TRUE(matches_with("\\h", nbsp, options, syntax))
          << "options " << options << " syntax " << (int)syntax;
      EXPECT_TRUE(matches_with("\\v", line_sep, options, syntax))
          << "options " << options << " syntax " << (int)syntax;
      EXPECT_TRUE(matches_with("\\v", next_line, options, syntax))
          << "options " << options << " syntax " << (int)syntax;

      // The control, and it is the point of the test: `\s` *is* one of the
      // three, so it narrows under `/a` and widens under UCP. A build that
      // answered "Unicode always" for every shorthand would pass the
      // assertions above and fail these.
      const bool ascii = (options & GRX_OPT_ASCII_CLASSES) != 0;
      const bool wide = !ascii
          && ((options & GRX_OPT_UCP) != 0 || syntax == GRX_SYNTAX_PERL);
      EXPECT_EQ(matches_with("\\s", nbsp, options, syntax), wide)
          << "options " << options << " syntax " << (int)syntax;
    }
  }

  // The negations follow their own sets, so `\H` and `\V` do not quietly
  // keep the ASCII complement.
  EXPECT_FALSE(matches_with("\\H", nbsp, GRX_OPT_UTF));
  EXPECT_FALSE(matches_with("\\V", line_sep, GRX_OPT_UTF));

  // A member that is in both the ASCII set and the Unicode one still
  // matches, which is what kept this invisible: every ordinary pattern
  // asks about a tab or a space.
  EXPECT_TRUE(matches_with("\\h", "\t", GRX_OPT_UTF));
  EXPECT_TRUE(matches_with("\\v", "\n", GRX_OPT_UTF));
}

TEST(Perl, PcresCharsetModifiersNarrowOneThingEach) {
  // WP-46. PCRE2 narrows one thing at a time where Perl's `/a` narrows the
  // family, so this library's one ASCII bit is five: `(?aD)` for `\d`,
  // `(?aS)` for `\s`, `(?aW)` for `\w` and `\b`, `(?aP)` for every POSIX
  // class, and `(?aT)` for `[[:digit:]]` and `[[:xdigit:]]` alone.
  //
  // Every expectation below was taken from pcre2test 10.46 under
  // `utf,ucp`, and the whole matrix was then re-run as a differential:
  // 500 rows, 0 differences.
  const uint32_t utf_ucp = GRX_OPT_UTF | GRX_OPT_UCP;
  const std::string arabic_one = "\xd9\xa1";  // U+0661, `\d` and [[:digit:]]
  const std::string nbsp = "\xc2\xa0";        // U+00A0, `\s` and [[:space:]]
  const std::string e_acute = "\xc3\xa9";     // U+00E9, `\w` and [[:alpha:]]
  const std::string full_zero = "\xef\xbc\x90"; // U+FF10, [[:xdigit:]]

  // Each letter narrows its own and leaves the others alone. The second
  // half of each pair is what makes this a test of five bits rather than
  // of one: a build that kept the single bit would pass every EXPECT_FALSE
  // here and fail every EXPECT_TRUE beside it.
  EXPECT_FALSE(matches_with("(?aD)\\d", arabic_one, utf_ucp));
  EXPECT_TRUE(matches_with("(?aD)\\s", nbsp, utf_ucp));
  EXPECT_TRUE(matches_with("(?aD)\\w", e_acute, utf_ucp));
  EXPECT_TRUE(matches_with("(?aD)[[:digit:]]", arabic_one, utf_ucp));

  EXPECT_FALSE(matches_with("(?aS)\\s", nbsp, utf_ucp));
  EXPECT_TRUE(matches_with("(?aS)\\d", arabic_one, utf_ucp));

  EXPECT_FALSE(matches_with("(?aW)\\w", e_acute, utf_ucp));
  EXPECT_TRUE(matches_with("(?aW)[[:word:]]", e_acute, utf_ucp));

  // `(?aP)` is every POSIX class; `(?aT)` is two names and no shorthand.
  EXPECT_FALSE(matches_with("(?aP)[[:alpha:]]", e_acute, utf_ucp));
  EXPECT_FALSE(matches_with("(?aP)[[:digit:]]", arabic_one, utf_ucp));
  EXPECT_TRUE(matches_with("(?aP)\\w", e_acute, utf_ucp));

  EXPECT_FALSE(matches_with("(?aT)[[:digit:]]", arabic_one, utf_ucp));
  EXPECT_FALSE(matches_with("(?aT)[[:xdigit:]]", full_zero, utf_ucp));
  EXPECT_TRUE(matches_with("(?aT)[[:alpha:]]", e_acute, utf_ucp));
  EXPECT_TRUE(matches_with("(?aT)[[:word:]]", e_acute, utf_ucp));
  EXPECT_TRUE(matches_with("(?aT)\\d", arabic_one, utf_ucp));

  // `(?a)` is all of them at once.
  for (const char * pattern : {"(?a)\\d", "(?a)[[:digit:]]"}) {
    EXPECT_FALSE(matches_with(pattern, arabic_one, utf_ucp)) << pattern;
  }
  EXPECT_FALSE(matches_with("(?a)\\s", nbsp, utf_ucp));
  EXPECT_FALSE(matches_with("(?a)\\w", e_acute, utf_ucp));
  EXPECT_FALSE(matches_with("(?a)[[:alpha:]]", e_acute, utf_ucp));

  // `\b` moves with `W` and with nothing else, being defined from `\w`.
  // "é x" has no boundary before the "x" while "é" is a word character.
  const std::string e_acute_x = e_acute + "x";
  EXPECT_FALSE(matches_with("\\bx", e_acute_x, utf_ucp));
  EXPECT_TRUE(matches_with("(?aW)\\bx", e_acute_x, utf_ucp));
  EXPECT_TRUE(matches_with("(?a)\\bx", e_acute_x, utf_ucp));
  EXPECT_FALSE(matches_with("(?aD)\\bx", e_acute_x, utf_ucp));
  EXPECT_FALSE(matches_with("(?a)(?-aW)\\bx", e_acute_x, utf_ucp));

  // The hyphen goes in front of the `a`, and `P` carries `T` in both
  // directions: clearing the narrow letter leaves the wide one standing,
  // and clearing the wide one reaches the narrow set.
  EXPECT_TRUE(matches_with("(?a)(?-aD)\\d", arabic_one, utf_ucp));
  EXPECT_FALSE(matches_with("(?a)(?-aD)\\w", e_acute, utf_ucp));
  EXPECT_TRUE(matches_with("(?a)(?-a)\\d", arabic_one, utf_ucp));
  EXPECT_TRUE(matches_with("(?a)(?-a)[[:alpha:]]", e_acute, utf_ucp));
  EXPECT_FALSE(matches_with("(?aP)(?-aT)[[:digit:]]", arabic_one, utf_ucp));
  EXPECT_TRUE(matches_with("(?aT)(?-aP)[[:digit:]]", arabic_one, utf_ucp));

  // Scope is the group's, like any other modifier, and a setting inside a
  // non-capturing group does not leak past it.
  EXPECT_FALSE(matches_with("(?aD:\\d)", arabic_one, utf_ucp));
  EXPECT_TRUE(matches_with("(?aD:x)\\d", "x" + arabic_one, utf_ucp));
  EXPECT_TRUE(matches_with("(?:(?aD))\\d", arabic_one, utf_ucp));

  // `(?aa)` is *not* Perl's `/aa`. It is `a` applied twice: it compiles,
  // behaves as `(?a)`, and still folds "k" with U+212A - the letter PCRE2
  // has for that is `r`. This is the trap in the construct, and the reason
  // WP-46 could not be reached by way of the already-built `/aa`.
  const uint32_t utf_fold = GRX_OPT_UTF | GRX_OPT_CASELESS;
  const std::string kelvin = "\xe2\x84\xaa";
  EXPECT_TRUE(matches_with("(?aa)k", kelvin, utf_fold));
  EXPECT_TRUE(matches_with("(?aaa)k", kelvin, utf_fold));
  EXPECT_TRUE(matches_with("(?ai)k", kelvin, GRX_OPT_UTF));
  EXPECT_FALSE(matches_with("(?ri)k", kelvin, GRX_OPT_UTF));

  // Exactly one letter may follow the `a`. Nothing enforces that: the
  // second one is read as an ordinary flag, which is unknown for all five
  // and the same refusal pcre2test gives - error 111 for `(?aDS)` and for
  // `(?aTP)`, and for `(?a-D)`, where the hyphen is on the wrong side.
  for (const char * pattern :
      {"(?aDS)x", "(?aTP)x", "(?aDD)x", "(?a-D)x", "(?aX)x"}) {
    Attempt bad = compile(pattern, GRX_SYNTAX_PCRE);
    EXPECT_EQ(bad.result, GRX_ERR_SYNTAX) << pattern;
    EXPECT_EQ(bad.diag, GRX_DIAG_UNKNOWN_FLAG) << pattern;
    grx_regex_free(bad.regex);
  }

  // And an `a` modifier mixes with the ordinary letters in either order,
  // which is the half a rule spelled "one letter then stop" would break.
  for (const char * pattern : {"(?aDi)x", "(?iaD)x", "(?aD-i)x", "(?-aDi)x"}) {
    Attempt good = compile(pattern, GRX_SYNTAX_PCRE);
    EXPECT_EQ(good.result, GRX_OK) << pattern;
    grx_regex_free(good.regex);
  }

  // The four letters PCRE2 does not have stay unknown flags, which is what
  // pcre2test answers for them: error 111, "unrecognized character after
  // (? or (?-".
  for (const char * pattern : {"(?u)a", "(?d)a", "(?l)a", "(?p)a"}) {
    Attempt unknown = compile(pattern, GRX_SYNTAX_PCRE);
    EXPECT_EQ(unknown.result, GRX_ERR_SYNTAX) << pattern;
    EXPECT_EQ(unknown.diag, GRX_DIAG_UNKNOWN_FLAG) << pattern;
    grx_regex_free(unknown.regex);
  }

  // Perl is untouched: its `a` is the whole family and it has no suffix
  // letters at all. perl refuses `(?aD)`, `(?aS)`, `(?aW)`, `(?aP)` and
  // `(?aT)` and compiles `(?a)`, so this dialect does too.
  EXPECT_EQ(span_of("(?a)\\w", "\xC3\xA9", GRX_SYNTAX_PERL), "nomatch");
  EXPECT_EQ(span_of("(?a)\\w", "a", GRX_SYNTAX_PERL), "0-1");
  for (const char * pattern :
      {"(?aD)x", "(?aS)x", "(?aW)x", "(?aP)x", "(?aT)x"}) {
    Attempt perl_refuses = compile(pattern, GRX_SYNTAX_PERL);
    EXPECT_NE(perl_refuses.result, GRX_OK) << pattern;
    grx_regex_free(perl_refuses.regex);
  }
}

// A pattern that stops inside an option setting has run out of pattern, and
// that is a different complaint from a letter the dialect has not got. All
// three references keep the two apart, and keep them apart by the same rule:
// it is whether the *letter* was acceptable, not whether the end was near.
//
//   pcre2test  `(?n`  error 114 "missing closing parenthesis"
//              `(?u`  error 111 "unrecognized character after (? or (?-"
//   perl       `(?i`  "Sequence (?... not terminated"
//              `(?z`  "Sequence (?z...) not recognized"
//   re         `(?i`  "missing -, : or )"
//              `(?z`  "unknown extension ?z"
//
// `n` and `i` are letters those dialects have, so the end of the pattern is
// what is wrong; `u` and `z` are not, so the letter is what is wrong, and it
// stays wrong even though the pattern ends immediately after it.
//
// Every one of these answered "unknown flag" here until 2026-09-24, because
// the letter loop read the terminating NUL as a letter and refused it before
// the end-of-pattern check below the loop could run. Both refuse either way,
// so no match was ever wrong; the reason was, and the reason is what a
// caller puts in front of a user.
TEST(Perl, RunningOutOfPatternIsNotAnUnknownLetter) {
  struct { GRX_Syntax syntax; const char * pattern; } truncated[] = {
    // Letters each dialect has, then nothing.
    {GRX_SYNTAX_PCRE, "(?i"}, {GRX_SYNTAX_PCRE, "(?im"},
    {GRX_SYNTAX_PCRE, "(?-i"}, {GRX_SYNTAX_PCRE, "(?^i"},
    {GRX_SYNTAX_PCRE, "(?x"}, {GRX_SYNTAX_PCRE, "(?xx"},
    {GRX_SYNTAX_PCRE, "(?n"},
    // The two built today, which is what brought this to notice: `(?a`
    // takes the whole family and `(?aD` one member, and both then end.
    {GRX_SYNTAX_PCRE, "(?r"}, {GRX_SYNTAX_PCRE, "(?a"},
    {GRX_SYNTAX_PCRE, "(?aD"}, {GRX_SYNTAX_PCRE, "(?aS"},
    {GRX_SYNTAX_PCRE, "(?aW"}, {GRX_SYNTAX_PCRE, "(?aP"},
    {GRX_SYNTAX_PCRE, "(?aT"}, {GRX_SYNTAX_PCRE, "(?a-"},
    {GRX_SYNTAX_PCRE, "(?aDi"},
    // `(?` with no letter at all. perl separates this one ("Sequence (?
    // incomplete") and so does `re` ("unexpected end of pattern"), but
    // both put it on the termination side, which is the side that matters.
    {GRX_SYNTAX_PCRE, "(?"},
    // Perl's charset letters, which take a different branch of the loop.
    {GRX_SYNTAX_PERL, "(?i"}, {GRX_SYNTAX_PERL, "(?a"},
    {GRX_SYNTAX_PERL, "(?aa"}, {GRX_SYNTAX_PERL, "(?u"},
    {GRX_SYNTAX_PERL, "(?l"}, {GRX_SYNTAX_PERL, "(?p"},
    {GRX_SYNTAX_PERL, "(?-i"}, {GRX_SYNTAX_PERL, "(?"},
    // Python's, which take a third branch.
    {GRX_SYNTAX_PYTHON, "(?i"}, {GRX_SYNTAX_PYTHON, "(?a"},
    {GRX_SYNTAX_PYTHON, "(?u"}, {GRX_SYNTAX_PYTHON, "(?-i"},
    {GRX_SYNTAX_PYTHON, "(?"},
  };

  for (const auto & row : truncated) {
    Attempt cut = compile(row.pattern, row.syntax);
    EXPECT_EQ(cut.result, GRX_ERR_SYNTAX) << row.pattern;
    EXPECT_EQ(cut.diag, GRX_DIAG_UNMATCHED_OPEN_PAREN) << row.pattern;
    grx_regex_free(cut.regex);
  }

  // The other side of the rule, and the half that makes this a fix rather
  // than a blanket "end of pattern wins": a letter the dialect has not got
  // is an unknown flag whether or not the pattern ends after it. Without
  // these rows, moving the end check above the letter read would pass just
  // as well as moving it below, and only one of those is what pcre2 does.
  struct { GRX_Syntax syntax; const char * pattern; } unknown[] = {
    {GRX_SYNTAX_PCRE, "(?u"}, {GRX_SYNTAX_PCRE, "(?d"},
    {GRX_SYNTAX_PCRE, "(?l"}, {GRX_SYNTAX_PCRE, "(?p"},
    {GRX_SYNTAX_PCRE, "(?z"}, {GRX_SYNTAX_PCRE, "(?iz"},
    {GRX_SYNTAX_PERL, "(?r"}, {GRX_SYNTAX_PERL, "(?z"},
    {GRX_SYNTAX_PYTHON, "(?z"}, {GRX_SYNTAX_PYTHON, "(?r"},
  };

  for (const auto & row : unknown) {
    Attempt bad = compile(row.pattern, row.syntax);
    EXPECT_EQ(bad.result, GRX_ERR_SYNTAX) << row.pattern;
    EXPECT_EQ(bad.diag, GRX_DIAG_UNKNOWN_FLAG) << row.pattern;
    grx_regex_free(bad.regex);
  }

  // And the reason the check is grx_parse_at_end() and not `c == '\0'`.
  // Patterns carry a length, so an embedded NUL is an ordinary byte that no
  // dialect has a letter for. It must stay an unknown flag, and a fix
  // written against the byte would call it the end of the pattern.
  const std::string embedded("(?i\0m)x", 7);
  Attempt nul = compile(embedded, GRX_SYNTAX_PCRE);
  EXPECT_EQ(nul.result, GRX_ERR_SYNTAX);
  EXPECT_EQ(nul.diag, GRX_DIAG_UNKNOWN_FLAG);
  grx_regex_free(nul.regex);

  // A group whose letters parsed and whose *body* then ran out has always
  // reported this, and the truncated settings above now join it rather than
  // getting a code of their own.
  Attempt body = compile("(?i:a", GRX_SYNTAX_PCRE);
  EXPECT_EQ(body.result, GRX_ERR_SYNTAX);
  EXPECT_EQ(body.diag, GRX_DIAG_UNMATCHED_OPEN_PAREN);
  grx_regex_free(body.regex);
}

// The family bit is still one bit for the callers that hold it, and setting
// it is setting all five. A caller reaching GRX_OPT_ASCII_CLASSES through
// the API - which is how Perl's `/a`, Perl's `/l` and Python's `re.ASCII`
// arrive - must get exactly what the five spell together.
TEST(Perl, TheFamilyBitIsTheFiveLettersTogether) {
  const uint32_t utf_ucp = GRX_OPT_UTF | GRX_OPT_UCP;
  const uint32_t every = GRX_OPT_ASCII_DIGIT | GRX_OPT_ASCII_SPACE
      | GRX_OPT_ASCII_WORD | GRX_OPT_ASCII_POSIX | GRX_OPT_ASCII_POSIX_DIGIT;

  struct { const char * pattern; const char * subject; } cases[] = {
    {"\\d", "\xd9\xa1"}, {"\\s", "\xc2\xa0"}, {"\\w", "\xc3\xa9"},
    {"[[:digit:]]", "\xd9\xa1"}, {"[[:xdigit:]]", "\xef\xbc\x90"},
    {"[[:alpha:]]", "\xc3\xa9"}, {"[[:word:]]", "\xc3\xa9"},
    {"[[:space:]]", "\xc2\xa0"},
    // Members that are in the ASCII set too, so that a build narrowing
    // everything to nothing would not pass this by refusing it all.
    {"\\d", "7"}, {"\\w", "x"}, {"[[:alpha:]]", "q"},
  };

  for (const auto & row : cases) {
    EXPECT_EQ(matches_with(row.pattern, row.subject,
                  utf_ucp | GRX_OPT_ASCII_CLASSES),
        matches_with(row.pattern, row.subject, utf_ucp | every))
        << row.pattern;
  }

  // GRX_OPT_ASCII_POSIX means *every* POSIX class, so it has to carry the
  // digit bit for a caller who sets it alone - the front end never does,
  // because `(?aP)` sets both, and a mutation removing the implication
  // therefore passed every other assertion in this file. This is the one
  // reader it has.
  EXPECT_FALSE(matches_with("[[:xdigit:]]", "\xef\xbc\x90",
      utf_ucp | GRX_OPT_ASCII_POSIX));
  EXPECT_FALSE(matches_with("[[:digit:]]", "\xd9\xa1",
      utf_ucp | GRX_OPT_ASCII_POSIX));
  // And the narrow bit alone does not reach the rest, in either direction.
  EXPECT_TRUE(matches_with("[[:alpha:]]", "\xc3\xa9",
      utf_ucp | GRX_OPT_ASCII_POSIX_DIGIT));
  EXPECT_FALSE(matches_with("[[:xdigit:]]", "\xef\xbc\x90",
      utf_ucp | GRX_OPT_ASCII_POSIX_DIGIT));
}

TEST(Perl, ACaseTransformIsAnOperatorOverThePatternSource) {
  // `\U`, `\L`, `\F`, `\u` and `\l` are perl's, applied by the pass that
  // reads a pattern typed in a program rather than by its engine - a sibling
  // of `\Q...\E` and not a construct of the grammar. Every answer below is
  // perl 5.44.0's, asked through tools/corpus/perl_match.pl with the
  // `source` reading, which is the reading a `/`-delimited row of
  // `t/re/re_tests` means.
  //
  // A run lasts until `\E`.
  EXPECT_EQ(span_of("\\Uab\\E", "AB", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("\\Uab\\E", "ab", GRX_SYNTAX_PERL), "nomatch");
  EXPECT_EQ(span_of("\\LABC\\E", "abc", GRX_SYNTAX_PERL), "0-3");
  // ...or, with no `\E`, until the end of the pattern.
  EXPECT_EQ(span_of("\\Uab", "AB", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("\\LABC", "abc", GRX_SYNTAX_PERL), "0-3");
  // `\F` is the fold, which for these letters is the lowercase.
  EXPECT_EQ(span_of("\\FAB\\E", "ab", GRX_SYNTAX_PERL), "0-2");

  // `\u` and `\l` transform one character.
  EXPECT_EQ(span_of("\\uab", "Ab", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("\\uab", "ab", GRX_SYNTAX_PERL), "nomatch");
  EXPECT_EQ(span_of("\\lAB", "aB", GRX_SYNTAX_PERL), "0-2");

  // The runs do not nest and do not stack: a second one replaces the first,
  // and one `\E` ends everything. perl warns "Useless use of \E" about the
  // second `\E` here, which is how one can see that.
  EXPECT_EQ(span_of("\\Ua\\Lb\\Ec\\E", "Abc", GRX_SYNTAX_PERL), "0-3");

  // A pending one-character operator wins over a run for its character, and
  // survives another run opening in front of it.
  EXPECT_EQ(span_of("\\u\\Lab", "Ab", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("\\l\\Uab", "aB", GRX_SYNTAX_PERL), "0-2");
  // But not `\E`, which cancels it. This asymmetry is measured, not derived:
  // `\u\Lab` matches "Ab" and `\u\Eab` does not.
  EXPECT_EQ(span_of("\\u\\Eab", "Ab", GRX_SYNTAX_PERL), "nomatch");
  EXPECT_EQ(span_of("\\u\\Eab", "ab", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("\\U\\Ea", "a", GRX_SYNTAX_PERL), "0-1");

  // An operator with nothing after it to transform is an empty match, not an
  // error.
  EXPECT_EQ(span_of("\\U", "x", GRX_SYNTAX_PERL), "0-0");
  EXPECT_EQ(span_of("\\u", "x", GRX_SYNTAX_PERL), "0-0");
  EXPECT_EQ(span_of("a\\Eb", "ab", GRX_SYNTAX_PERL), "0-2");

  // The transform is over the source, so it reaches through every construct
  // the source happens to contain: a group, a range, a quantifier's target.
  EXPECT_EQ(span_of("\\U(ab)\\E", "AB", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("\\U[a-z]\\E", "A", GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("\\U[a-z]\\E", "a", GRX_SYNTAX_PERL), "nomatch");
  EXPECT_EQ(span_of("[\\Ua-c\\E]", "B", GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("\\Uab\\E*", "ABB", GRX_SYNTAX_PERL), "0-3");
  // And through a `\Q` run in that order, whose characters are literals
  // getting the same treatment every other literal gets: "A\.B".
  EXPECT_EQ(span_of("\\U\\Qa.b\\E", "A.B", GRX_SYNTAX_PERL), "0-3");
  EXPECT_EQ(span_of("\\U\\Qa.b\\E", "AxB", GRX_SYNTAX_PERL), "nomatch");
}

TEST(Perl, OneCharacterCaseOperatorsTransformTheNextCharacterAndNotTheNextAtom) {
  // The case that decides how the two are implemented. perl's is a pass over
  // *text*, so `\u` upper-cases whatever character stands next even when the
  // grammar makes that character an operator - and `[` has no case, so
  // `\u[ab]` is `[ab]`.
  //
  // Read as "the next literal" instead it would be `[Ab]`, which is a
  // different language and is what perl says it is not: "a" matches there
  // and "A" does not.
  EXPECT_EQ(span_of("\\u[ab]", "a", GRX_SYNTAX_PERL), "0-1");
  EXPECT_EQ(span_of("\\u[ab]", "A", GRX_SYNTAX_PERL), "nomatch");
  // The same for a group's parenthesis and for a quantifier.
  EXPECT_EQ(span_of("\\u(ab)", "ab", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("a\\u*", "aaa", GRX_SYNTAX_PERL), "0-3");
}

TEST(Perl, WhatACaseTransformRefusesRatherThanApproximate) {
  // perl's pass upper-cases the pattern's *text*, so inside `\U` an escape
  // becomes a different escape: `\U\d\E` is `\D` and matches "x" rather
  // than "5", and `\U\x{df}\E` is the syntax error "Unescaped left brace"
  // because the `x` was upper-cased and `\X{DF}` is not a pattern.
  //
  // Neither is a rule about the language; each is a fact about a textual
  // pass. So this library says the construct exists and is not built, which
  // is the one answer that is neither perl's nor a silent substitute for it.
  for (const char * pattern : {"\\U\\d\\E", "\\U\\w\\E", "\\U\\p{L}\\E",
      "\\U\\x{df}\\E", "\\U\\n\\E", "\\u\\d", "[\\U\\d\\E]",
      "\\U[[:lower:]]\\E"}) {
    Attempt attempt = compile(pattern, GRX_SYNTAX_PERL);
    EXPECT_EQ(attempt.result, GRX_ERR_UNSUPPORTED) << pattern;
    EXPECT_EQ(attempt.diag, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED) << pattern;
    grx_regex_free(attempt.regex);
  }

  // A code point whose full mapping is longer than one code point, for the
  // same reason: perl's `\Uß` is "SS", two characters of pattern from one,
  // and the simple mapping that would fit leaves `ß` alone. Matching `ß`
  // where the reference matches "SS" is the approximation this refuses.
  Attempt sharp = compile("\\U\xc3\x9f\\E", GRX_SYNTAX_PERL);
  EXPECT_EQ(sharp.result, GRX_ERR_UNSUPPORTED);
  EXPECT_EQ(sharp.diag, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED);
  grx_regex_free(sharp.regex);
  // The control: the same letter with no transform over it is an ordinary
  // literal, so the refusal is the transform's and not the character's.
  EXPECT_EQ(span_of("\xc3\x9f", "\xc3\x9f", GRX_SYNTAX_PERL), "0-2");

  // A case operator *inside* a `\Q` run is the other order, and it is still
  // live in perl - `\Q\Ua\E\E` matches "A" there, not the four characters
  // `\Ua`. Here the run's characters are already literals by the time the
  // `\U` is reached, so the run is refused rather than read two ways.
  Attempt inside = compile("\\Q\\Ua\\E\\E", GRX_SYNTAX_PERL);
  EXPECT_EQ(inside.result, GRX_ERR_UNSUPPORTED);
  EXPECT_EQ(inside.diag, GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED);
  grx_regex_free(inside.regex);
  // ...and a `\Q` run with no case operator in it is untouched, which is the
  // control that the guard is keyed on the operator and not on `\Q`.
  EXPECT_EQ(span_of("\\Qa.b\\E", "a.b", GRX_SYNTAX_PERL), "0-3");
}

TEST(Perl, OnlyPerlHasTheCaseTransformAndOnlyFiveLettersSpellIt) {
  // Swept rather than asserted one letter at a time, because the question is
  // which letters are in the family and a spot check answers a different
  // one. The five are `\L`, `\U`, `\F`, `\l` and `\u`; `\E` ends a run and
  // is not itself a transform.
  //
  // Swept on the upper-casing direction, because that is the one no other
  // escape can fake: the pattern is `\<letter>ab`, so a letter that reaches
  // "AB" upper-cased two characters and a letter that reaches "Ab"
  // upper-cased one. Every other escape in this dialect leaves "ab" as it is,
  // whatever else it does, and "ab" matches neither subject.
  //
  // Both directions of both claims, so a letter that stops transforming and a
  // letter that starts are each a failure.
  for (char letter = 'A'; letter <= 'z'; letter++) {
    if (!isalpha((unsigned char)letter)) {
      continue;
    }
    std::string pattern = std::string("\\") + letter + "ab";
    EXPECT_EQ(span_of(pattern, "AB", GRX_SYNTAX_PERL) == "0-2",
        letter == 'U') << pattern << " against AB";
    EXPECT_EQ(span_of(pattern, "Ab", GRX_SYNTAX_PERL) == "0-2",
        letter == 'u') << pattern << " against Ab";
  }

  // The three lower-casing spellings, asserted rather than swept: "ab"
  // lower-cases to itself, so no sweep over it can separate `\L` from an
  // escape that simply matched.
  EXPECT_EQ(span_of("\\LAB\\E", "ab", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("\\FAB\\E", "ab", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("\\lAB\\E", "aB", GRX_SYNTAX_PERL), "0-2");
  EXPECT_EQ(span_of("\\lAB\\E", "ab", GRX_SYNTAX_PERL), "nomatch");

  // And not in the two dialects that share this front end. PCRE2 refuses all
  // sixteen letters of the wider family with an error of its own - 137, which
  // names `\F \L \l \N{name} \U \u` - and CPython calls each a bad escape.
  // documentation/dialects.md section 9 has the measured table.
  for (const char * pattern : {"\\Uab\\E", "\\Lab\\E", "\\Fab\\E", "\\uab",
      "\\lab"}) {
    EXPECT_NE(compile_result(pattern, GRX_SYNTAX_PCRE), GRX_OK) << pattern;
    EXPECT_NE(compile_result(pattern, GRX_SYNTAX_PYTHON), GRX_OK) << pattern;
    // Refused as syntax, not as something PCRE2 has and this library lacks:
    // PCRE2 does not have it either.
    EXPECT_NE(compile_result(pattern, GRX_SYNTAX_PCRE), GRX_ERR_UNSUPPORTED)
        << pattern;
  }
  // `\E` stays harmless in all three, as it was before this construct
  // existed: a `\E` with no run open is what perl and pcre2test both accept.
  EXPECT_EQ(span_of("a\\Eb", "ab", GRX_SYNTAX_PCRE), "0-2");
  EXPECT_EQ(span_of("a\\Eb", "ab", GRX_SYNTAX_PERL), "0-2");
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

// U+0374 GREEK NUMERAL SIGN is in the Greek and Coptic **block** and its
// Script is Common; U+1F00 is Script=Greek and in the Greek Extended block.
// That pair separates the three readings of "Greek" from each other, which
// no single code point can do - U+03B1 is all three at once.
static const char * kU0374 = "\xCD\xB4";
static const char * kU1F00 = "\xE1\xBC\x80";

TEST(Perl, BlockPropertiesAreSpelledFourWaysAndAreOnlyPerls) {
  // Measured against perl 5.44.0: all four spellings take U+0374 and refuse
  // U+1F00, which is the block and not either script reading.
  for (const char * pattern : {"\\p{InGreek}", "\\p{Block=Greek}",
           "\\p{Block=Greek_And_Coptic}", "\\p{blk=Greek_And_Coptic}"}) {
    EXPECT_TRUE(search(pattern, kU0374, GRX_SYNTAX_PERL, "u").matched)
        << pattern << " missed U+0374, which is in the block";
    EXPECT_FALSE(search(pattern, kU1F00, GRX_SYNTAX_PERL, "u").matched)
        << pattern << " took U+1F00, which is in Greek Extended";

    // pcre2test 10.46 answers error 147 "unknown property" to every one of
    // these, and ECMA-262 has no block property at all. Both share this
    // resolver with perl, so the refusal is the thing to pin: a change that
    // let blocks leak into either dialect would pass every test above.
    EXPECT_EQ(compile_result(pattern, GRX_SYNTAX_PCRE), GRX_ERR_SYNTAX)
        << pattern;
    EXPECT_EQ(compile_result(pattern, GRX_SYNTAX_ECMASCRIPT, "u"),
        GRX_ERR_SYNTAX)
        << pattern;
  }
}

TEST(Perl, ABareBlockNameResolvesOnlyWhenNothingElseClaimsIt) {
  // `Greek_Extended` is a block name and nothing else, so a bare
  // `\p{GreekExtended}` is the block - perl matches U+1F00 with it.
  EXPECT_TRUE(search("\\p{GreekExtended}", kU1F00, GRX_SYNTAX_PERL, "u")
                  .matched);
  EXPECT_FALSE(search("\\p{GreekExtended}", kU0374, GRX_SYNTAX_PERL, "u")
                   .matched);

  // `Greek` is a script *and* a block, and the script wins: a bare
  // `\p{Greek}` refuses U+0374, where `\p{InGreek}` takes it. This is the
  // ordering assertion - resolve blocks any earlier in the chain and this
  // is the test that fails.
  EXPECT_FALSE(search("\\p{Greek}", kU0374, GRX_SYNTAX_PERL, "u").matched);
  EXPECT_TRUE(search("\\p{InGreek}", kU0374, GRX_SYNTAX_PERL, "u").matched);

  // Still Perl's alone. PCRE2 takes `\p{Greek}` as a script and refuses the
  // block name outright.
  EXPECT_EQ(compile_result("\\p{GreekExtended}", GRX_SYNTAX_PCRE),
      GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("\\p{Greek}", GRX_SYNTAX_PCRE), GRX_OK);
}

TEST(Perl, TheInPrefixIsABlockAndTheIsPrefixIsNot) {
  // The two prefixes are not the same question, which is the thing most
  // easily got wrong by treating them as a pair.
  //
  // `In` is the block prefix and has no other reading: perl's
  // `\p{InGreek}` is the block.
  EXPECT_TRUE(search("\\p{InGreek}", kU0374, GRX_SYNTAX_PERL, "u").matched);

  // `Is` re-runs the ordinary chain first, so `\p{IsGreek}` is the script
  // reading - it refuses U+0374 exactly as a bare `\p{Greek}` does - and
  // only reaches a block when no other kind claims the name, which is what
  // `\p{IsGreekAndCoptic}` does.
  EXPECT_FALSE(search("\\p{IsGreek}", kU0374, GRX_SYNTAX_PERL, "u").matched);
  EXPECT_TRUE(
      search("\\p{IsGreekAndCoptic}", kU0374, GRX_SYNTAX_PERL, "u").matched);
  EXPECT_FALSE(
      search("\\p{IsGreekAndCoptic}", kU1F00, GRX_SYNTAX_PERL, "u").matched);

  // Neither prefix exists in PCRE2: error 147 for both, measured.
  EXPECT_EQ(compile_result("\\p{InGreek}", GRX_SYNTAX_PCRE), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_result("\\p{IsGreek}", GRX_SYNTAX_PCRE), GRX_ERR_SYNTAX);
}

TEST(Perl, AShortBlockAliasResolvesAndIsWhereTheValueIs) {
  // The short names are the half `PropertyValueAliases.txt` carries and
  // `Blocks.txt` does not, and they are the reason this library reads the
  // alias file for blocks rather than taking Blocks.txt's spelling alone.
  // 143 of the 347 blocks have one that differs from the long name.
  //
  // ghoti.io-unicode cannot resolve any of them today - the two UCD files
  // spell the long name differently and its generator joins them exactly -
  // so this is also the assertion that this library is not simply passing
  // the name through. See notes/unicode/BLOCK-VALUE-ALIASES.md.
  EXPECT_TRUE(search("\\p{blk=Greek}", kU0374, GRX_SYNTAX_PERL, "u").matched);
  EXPECT_TRUE(search("\\p{blk=Greek_Ext}", kU1F00, GRX_SYNTAX_PERL, "u")
                  .matched);
  EXPECT_FALSE(search("\\p{blk=Greek_Ext}", kU0374, GRX_SYNTAX_PERL, "u")
                   .matched);

  // `Blocks.txt` spells it with spaces, and a caller copying the name out
  // of that file finds it too.
  EXPECT_TRUE(
      search("\\p{Block=Greek and Coptic}", kU0374, GRX_SYNTAX_PERL, "u")
          .matched);
}
