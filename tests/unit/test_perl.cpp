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
    "(*napla:a)",              // a non-atomic lookahead
    "(*MARK:name)",            // no API reads a mark back
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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
