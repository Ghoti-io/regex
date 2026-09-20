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
 * 1,884 records from pcre2test's own test files and 1,707 from Perl's
 * `re_tests`. What is here is what a corpus cannot state - which node came
 * out, which diagnostic was reported, and which of two constructs a spelling
 * turned into.
 *
 * Copyright 2026 by Corey Pennycuff
 */

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

} // namespace

// --------------------------------------------------------------------------
// Quoting, comments and extended mode: the three lexical skips
// --------------------------------------------------------------------------

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

TEST(Perl, HexEscapesNeedTheirDigitsInThisVersion) {
  // pcre2test 10.46: "digits missing after \x". Older PCRE2 read `\x` with no
  // digits as NUL, and the corpus is what says which of the two this library
  // has to be - the documentation describes both.
  Attempt bare = compile("\\x");
  EXPECT_NE(bare.result, GRX_OK);
  EXPECT_EQ(bare.diag, GRX_DIAG_INVALID_HEX_ESCAPE);
  grx_regex_free(bare.regex);

  EXPECT_TRUE(search("\\x41", "A").matched);
  EXPECT_TRUE(search("\\x{1F600}", "\xF0\x9F\x98\x80", GRX_SYNTAX_PERL).matched);
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

TEST(Perl, FailIsTheOneVerbThatCannotBeRepeated) {
  // `(*ACCEPT)*` compiles in pcre2test and `(*FAIL)*` does not, which is not
  // an inconsistency: `(*FAIL)` is `(?!)` written short, and a negative
  // lookahead is the one verb with no extent of its own.
  Attempt accept = compile("a(*ACCEPT)*b");
  EXPECT_EQ(accept.result, GRX_OK);
  grx_regex_free(accept.regex);

  Attempt fail = compile("a(*FAIL)*b");
  EXPECT_NE(fail.result, GRX_OK);
  EXPECT_EQ(fail.diag, GRX_DIAG_NOTHING_TO_REPEAT);
  grx_regex_free(fail.regex);
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
  // none here.
  Attempt collating = compile("[[.a.]]");
  EXPECT_EQ(collating.result, GRX_ERR_UNSUPPORTED);
  grx_regex_free(collating.regex);
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

TEST(Perl, TheConstructsThisLibraryRefusesSayWhyAndNotSomethingElse) {
  // Each of these is real syntax the reference compiles. Refusing them is a
  // decision; refusing them as *syntax errors* would be a lie about the
  // pattern, so each reports GRX_ERR_UNSUPPORTED instead.
  const char * refused[] = {
    "\\X",                     // a grapheme cluster
    "\\C",                     // one code unit
    "(*script_run:abc)",       // constrains the body
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

  // A digit is still required. Perl reads `\x{_}` as zero; this refuses it,
  // which is the one place the two part and is recorded as a deviation
  // rather than left to be discovered.
  EXPECT_EQ(compile("\\x{_}", GRX_SYNTAX_PERL).diag,
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

  // Perl has `(?[...])` too, with a grammar of its own that nests where
  // PCRE2's does not. Claiming it here would be claiming to read Perl's, so
  // the feature bit is PCRE's alone and `(?[` reaches Perl's option-letter
  // reader, which reports the `[` as a flag it does not have.
  EXPECT_EQ(compile("(?[ [a] ])", GRX_SYNTAX_PERL).diag,
      GRX_DIAG_UNKNOWN_FLAG);
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

TEST(Perl, PerlsBoundTypesAreReadAndRefusedRatherThanMisread) {
  // `\b{wb}` is Perl's, and only Perl's: pcre2test compiles it as a word
  // boundary followed by four ordinary characters. This library did the same
  // for both until it was asked, which is a wrong answer wearing a right
  // one's clothes - the four bound types are UAX #29's break algorithms and
  // none of their tables is generated here.
  for (const char * spelling : {"\\b{wb}", "\\b{ wb }", "\\B{gcb}",
           "\\b{sb}", "\\b{lb}"}) {
    EXPECT_EQ(compile_result(spelling, GRX_SYNTAX_PERL), GRX_ERR_UNSUPPORTED)
        << spelling;
    EXPECT_EQ(compile_result(spelling, GRX_SYNTAX_PCRE), GRX_OK) << spelling;
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

  // `\K` may be repeated in Perl and is error 109 in pcre2test. Repeating it
  // changes nothing - it moves the reported start to here, and moving it
  // here again leaves it here - which is why `(?iaa:A?\K*)` reports 1-1.
  EXPECT_EQ(span_of("(?iaa:A?\\K*)", "African_Feh", GRX_SYNTAX_PERL), "1-1");
  EXPECT_EQ(compile_result("A\\K*", GRX_SYNTAX_PCRE), GRX_ERR_SYNTAX);
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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
