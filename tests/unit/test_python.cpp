/**
 * @file
 *
 * The Python front end: what CPython's `re` spells differently, and what it
 * does not have at all.
 *
 * Every expectation here was asked of CPython 3.13 before it was written
 * down, and most of them were asked by `tools/oracle/python_diff.py` rather
 * than by hand - the differential runs the reference in-process, so it asks
 * hundreds of thousands of questions in the time a subprocess oracle asks
 * hundreds. What is written here is the shape of each rule, so that a
 * failure names the rule; the differential is what says the rule holds over
 * patterns nobody wrote.
 *
 * Three of the cases below are not about Python at all. The prescan's group
 * count, the unread GRX_LOOKBEHIND_FIXED and the ungated `\Q` were defects
 * in shared code that this dialect's differential was simply the first gate
 * able to reach.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <string>

#include "test_helpers.h"

namespace {

struct Attempt {
  GRX_Result result;
  GRX_Diag diag;
  GRX_Regex * regex;
};

Attempt compile(const std::string & pattern, GRX_Syntax syntax,
    uint32_t options = 0) {
  Attempt attempt = {GRX_OK, GRX_DIAG_NONE, nullptr};
  GRX_Error error;
  grx_error_clear(&error);
  attempt.result = grx_regex_compile_with_allocator(pattern.data(),
      pattern.size(), syntax, options, nullptr, nullptr, &error,
      &attempt.regex);
  attempt.diag = error.diag;
  return attempt;
}

/** Whether a pattern compiles at all, as a word a failure message can print. */
std::string accepts(const std::string & pattern,
    GRX_Syntax syntax = GRX_SYNTAX_PYTHON) {
  Attempt attempt = compile(pattern, syntax);
  grx_regex_free(attempt.regex);
  return attempt.result == GRX_OK ? "ok" : "refused";
}

GRX_Diag why(const std::string & pattern,
    GRX_Syntax syntax = GRX_SYNTAX_PYTHON) {
  Attempt attempt = compile(pattern, syntax);
  grx_regex_free(attempt.regex);
  return attempt.diag;
}

/** Where a pattern matches, as `start-end`, or "nomatch", or "error". */
std::string span(const std::string & pattern, const std::string & subject,
    GRX_Syntax syntax = GRX_SYNTAX_PYTHON, uint32_t options = 0) {
  Attempt attempt = compile(pattern, syntax, options);
  if (attempt.result != GRX_OK) {
    grx_regex_free(attempt.regex);
    return "error";
  }
  GRX_Match * match = nullptr;
  grx_match_create(attempt.regex, nullptr, &match);
  int matched = 0;
  std::string answer = "nomatch";
  if (grx_regex_search(attempt.regex, subject.data(), subject.size(), 0,
          GRX_ENGINE_AUTO, nullptr, match, &matched) == GRX_OK
      && matched) {
    GRX_Capture whole;
    grx_match_group(match, 0, &whole);
    answer = std::to_string(whole.start) + "-" + std::to_string(whole.end);
  }
  grx_match_destroy(match);
  grx_regex_free(attempt.regex);
  return answer;
}

/** One group's span, as `start-end`, or "unset"/"nomatch"/"error". */
std::string group(const std::string & pattern, const std::string & subject,
    size_t index, GRX_Syntax syntax = GRX_SYNTAX_PYTHON) {
  Attempt attempt = compile(pattern, syntax);
  if (attempt.result != GRX_OK) {
    grx_regex_free(attempt.regex);
    return "error";
  }
  GRX_Match * match = nullptr;
  grx_match_create(attempt.regex, nullptr, &match);
  int matched = 0;
  std::string answer = "nomatch";
  if (grx_regex_search(attempt.regex, subject.data(), subject.size(), 0,
          GRX_ENGINE_AUTO, nullptr, match, &matched) == GRX_OK
      && matched) {
    GRX_Capture capture;
    grx_match_group(match, index, &capture);
    answer = capture.start == GRX_NPOS
        ? "unset"
        : std::to_string(capture.start) + "-" + std::to_string(capture.end);
  }
  grx_match_destroy(match);
  grx_regex_free(attempt.regex);
  return answer;
}

/** Every match replaced, or "template" when the template is refused. */
std::string replace_all(const std::string & pattern,
    const std::string & subject, const std::string & tmpl) {
  Attempt attempt = compile(pattern, GRX_SYNTAX_PYTHON);
  if (attempt.result != GRX_OK) {
    grx_regex_free(attempt.regex);
    return "error";
  }
  GRX_Text out;
  std::memset(&out, 0, sizeof(out));
  GRX_Error error;
  grx_error_clear(&error);
  GRX_Result result = grx_regex_replace(attempt.regex, subject.data(),
      subject.size(), tmpl.data(), tmpl.size(), GRX_REPLACE_GLOBAL, nullptr,
      nullptr, &error, &out);
  std::string answer = result == GRX_OK
      ? std::string(out.data, out.length)
      : (result == GRX_ERR_SYNTAX ? "template" : "error");
  if (result == GRX_OK) {
    grx_text_free(&out);
  }
  grx_regex_free(attempt.regex);
  return answer;
}

const GRX_Syntax kPy = GRX_SYNTAX_PYTHON;

} // namespace

TEST(Python, TheEscapeAlphabetIsClosed) {
  // `re` reports "bad escape \q" for every letter it has no meaning for,
  // where the Perl family reads an unknown letter as an identity escape.
  // Asserted both ways: the letters Python has, and a sample of the ones it
  // refuses that perl accepts, because a front end that refused everything
  // would pass half of this.
  EXPECT_EQ(accepts("\\d\\w\\s\\b\\A\\Z\\N{BULLET}\\x41\\u0041\\U00000041"),
      "ok");
  // Refused here and accepted by perl, which is what makes each of them a
  // *dialect* difference rather than a construct this library lacks. The
  // reference-side spellings carry a group where the construct needs one:
  // `\g1` and `\k<n>` are references, and perl refuses them with nothing to
  // refer to, so asserting the bare form would have tested nothing.
  const struct {
    const char * python;
    const char * perl;
  } elsewhere[] = {
      {"\\p{L}", "\\p{L}"},
      {"\\G", "\\G"},
      {"\\K", "\\K"},
      {"\\R", "\\R"},
      {"\\h", "\\h"},
      {"\\H", "\\H"},
      {"\\V", "\\V"},
      {"\\e", "\\e"},
      {"\\cA", "\\cA"},
      {"\\o{101}", "\\o{101}"},
      {"(a)\\g1", "(a)\\g1"},
      {"(?P<n>a)\\k<n>", "(?<n>a)\\k<n>"},
      {"\\Q", "\\Q"},
      {"\\E", "\\E"},
  };
  for (const auto & pair : elsewhere) {
    EXPECT_EQ(accepts(pair.python), "refused") << pair.python;
    EXPECT_EQ(accepts(pair.perl, GRX_SYNTAX_PERL), "ok") << pair.perl;
  }

  // Refused here and by perl too. `\C` is one code unit and this library
  // will not have it whatever the dialect (its subject is code points);
  // `\l` and `\y` are letters no Perl-family dialect defines either, and
  // they are here so that the list above is not read as "everything Python
  // refuses, perl takes".
  for (const char * refused : {"\\C", "\\l", "\\y"}) {
    EXPECT_EQ(accepts(refused), "refused") << refused;
  }
  // ...but perl reads `\X` and Python does not, so it belongs above in
  // spirit and below in fact: this library refuses it in every dialect,
  // because `\X` is an unbounded pattern and not a set of strings.
  EXPECT_EQ(accepts("\\X"), "refused");
}

TEST(Python, TheThreeEscapesSpelledDifferently) {
  // `\v` is the vertical tab here and the vertical-space shorthand in the
  // Perl family - one character against five. Routing it to the shared
  // table would have widened it silently.
  EXPECT_EQ(span("\\v", "\x0b"), "0-1");
  EXPECT_EQ(span("\\v", "\n"), "nomatch");
  EXPECT_EQ(span("\\v", "\n", GRX_SYNTAX_PERL), "0-1");

  // `\Z` is the end of the subject here and the position before a final
  // newline there: Python spells with `\Z` what perl spells with `\z`.
  EXPECT_EQ(span("a\\Z", "a\n"), "nomatch");
  EXPECT_EQ(span("a\\Z", "a"), "0-1");
  EXPECT_EQ(span("a\\Z", "a\n", GRX_SYNTAX_PERL), "0-1");

  // CPython 3.14 added `\z` as the preferred spelling of that same anchor,
  // so the two are synonyms here and `\z` is perl's `\z` unchanged. It is
  // still refused inside a class, where `re` calls it "bad escape \z" -
  // which is what `\A`, `\B` and `\Z` have always done.
  EXPECT_EQ(span("a\\z", "a"), "0-1");
  EXPECT_EQ(span("a\\z", "a\n"), "nomatch");
  EXPECT_EQ(span("a\\z", "a\n", GRX_SYNTAX_PERL), "nomatch");
  EXPECT_EQ(accepts("[\\z]"), "refused");

  // `\x` takes exactly two digits. `\x{41}`, `\x4` and `\xg` are all
  // "incomplete escape" in `re` and all legal in perl.
  EXPECT_EQ(span("\\x41", "A"), "0-1");
  for (const char * refused : {"\\x{41}", "\\x4", "\\xg", "\\x"}) {
    EXPECT_EQ(accepts(refused), "refused") << refused;
  }
}

TEST(Python, NTakesANameAndOnlyAName) {
  // Perl's `\N` is three constructs: a bare "not a newline", `\N{U+hhhh}`
  // and `\N{NAME}`. Python has the last of them alone.
  EXPECT_EQ(span("\\N{BULLET}", "\xe2\x80\xa2"), "0-3");
  EXPECT_EQ(accepts("\\N"), "refused");
  EXPECT_EQ(accepts("\\N{U+0041}"), "refused");
  EXPECT_EQ(accepts("\\N{U+0041}", GRX_SYNTAX_PERL), "ok");
}

TEST(Python, NamedGroupsUseThePSpellingAlone) {
  EXPECT_EQ(group("(?P<n>a)", "a", 1), "0-1");
  EXPECT_EQ(span("(?P<n>a)(?P=n)", "aa"), "0-2");
  // The spellings `re` answers "unknown extension" for. Each is a spelling
  // and not a construct: Python has named groups, so accepting `(?<n>a)`
  // would be calling a pattern valid Python that `re` rejects.
  for (const char * refused : {"(?<n>a)", "(?'n'a)", "(?&n)", "(?P>n)",
           "(?R)", "(?1)", "(?|a|b)", "(?C1)"}) {
    EXPECT_EQ(accepts(refused), "refused") << refused;
  }
  // ...but not the lookbehinds, which begin with the same two characters.
  EXPECT_EQ(span("(?<=a)b", "ab"), "1-2");
  EXPECT_EQ(span("(?<!a)b", "cb"), "1-2");
  // Two groups may not share a name, where perl allows it with no pragma.
  EXPECT_EQ(accepts("(?P<n>a)(?P<n>b)"), "refused");
}

TEST(Python, NoStarConstructAtAll) {
  // The control verbs, the directives, the script runs and the alternative
  // group spellings are one family and all of it is PCRE2's or perl's. `re`
  // reads the `*` as a quantifier with nothing before it.
  for (const char * refused : {"(*FAIL)", "(*ACCEPT)", "(*UTF)", "(*PRUNE)",
           "(*sr:a)", "(*pla:a)", "(*:mark)"}) {
    EXPECT_EQ(accepts(refused), "refused") << refused;
    EXPECT_EQ(accepts(refused, GRX_SYNTAX_PCRE), "ok") << refused;
  }
}

TEST(Python, AReferenceMustNameAGroupThatHasClosed) {
  // The rule that gave GRX_DIAG_FORWARD_BACKREFERENCE its first producer.
  // It had been listed as a candidate for *removal* on the ground that no
  // dialect here made it an error, which was true until this one.
  EXPECT_EQ(span("(a)\\1", "aa"), "0-2");
  EXPECT_EQ(span("((a)\\2)", "aa"), "0-2");
  EXPECT_EQ(span("(?:(a))\\1", "aa"), "0-2");

  // Still open, which a high-water mark of the highest group closed cannot
  // tell apart: `((a)\1)` closes group 2 before the reference and leaves
  // group 1 open, so a mark would stand at 2 and accept it.
  for (const char * refused : {"(a\\1)", "((a)\\1)", "(a|\\1)",
           "(?P<x>(?P<y>a)(?P=x))", "\\1(a)", "(a)(\\2)",
           "(?P=n)(?P<n>a)"}) {
    EXPECT_EQ(accepts(refused), "refused") << refused;
    EXPECT_EQ(why(refused), GRX_DIAG_FORWARD_BACKREFERENCE) << refused;
  }
  // perl takes every one of them: a forward reference simply fails to match.
  EXPECT_EQ(accepts("(a\\1)", GRX_SYNTAX_PERL), "ok");
}

TEST(Python, GlobalFlagsOnlyAtTheStart) {
  EXPECT_EQ(span("(?i)ab", "AB"), "0-2");
  EXPECT_EQ(span("(?i)(?m)ab", "AB"), "0-2");
  // A comment is not a term, so it does not end the leading run.
  EXPECT_EQ(span("(?#c)(?i)ab", "AB"), "0-2");
  // Being first is not enough: the group around it disqualifies it too.
  for (const char * refused : {"a(?i)b", "((?i)a)", "(?:(?i)a)",
           "(?i:(?m)a)", "(?i)(?:a)(?m)b"}) {
    EXPECT_EQ(accepts(refused), "refused") << refused;
  }
  // A *scoped* setting is unaffected, wherever it stands.
  EXPECT_EQ(span("a(?i:B)", "ab"), "0-2");
}

TEST(Python, AsciiModeNarrowsTheClassesAndTheFoldingAndNothingElse) {
  // `(?a)` against perl's `/a`, which is the whole of the difference
  // between them. Both narrow the shorthands.
  EXPECT_EQ(span("\\w", "\xc3\xa9"), "0-2");
  EXPECT_EQ(span("(?a)\\w", "\xc3\xa9"), "nomatch");

  // Perl leaves the folding alone under `/a` and only `/aa` cuts the orbit
  // at U+0080; Python's one flag does both, and harder - under `(?ai)`
  // U+00C0 does not match U+00E0, where `/aai` still matches them.
  EXPECT_EQ(span("k", "\xe2\x84\xaa", kPy, GRX_OPT_CASELESS), "0-3");
  EXPECT_EQ(span("(?a)k", "\xe2\x84\xaa", kPy, GRX_OPT_CASELESS), "nomatch");
  EXPECT_EQ(span("(?a)\xc3\x80", "\xc3\xa0", kPy, GRX_OPT_CASELESS),
      "nomatch");

  // What it does *not* touch: the subject stays text. `.` matches a whole
  // character and `\N{...}` still compiles. Writing this rule as "clear the
  // UTF bit" narrowed the classes correctly and broke both of these.
  EXPECT_EQ(span("(?a).", "\xc3\xa9"), "0-2");
  EXPECT_EQ(span("(?a)\\N{BULLET}", "\xe2\x80\xa2"), "0-3");

  // `a` and `u` are one choice written two ways, and `re` refuses both
  // writing them together and turning either off.
  EXPECT_EQ(accepts("(?au)a"), "refused");
  EXPECT_EQ(accepts("(?-a:a)"), "refused");
  EXPECT_EQ(accepts("(?u)a"), "ok");
  // `(?n)` is PCRE2's and perl's, and "unknown extension" here.
  EXPECT_EQ(accepts("(?n)a"), "refused");
}

TEST(Python, AnIterationThatConsumedNothingLeavesTheGroupEmpty) {
  // Section 5.5's BREAK_ON_EMPTY. `(a*)*` against "b" reports group 1 as
  // the empty string in `re`, as it does in perl and pcre2test, and as
  // unset in ECMAScript - and Python's row said nothing, so it had been
  // taking ECMAScript's answer since it was written.
  EXPECT_EQ(group("(a*)*", "b", 1), "0-0");
  EXPECT_EQ(group("(a*)+", "b", 1), "0-0");
  EXPECT_EQ(group("(a*)*", "b", 1, GRX_SYNTAX_ECMASCRIPT), "unset");
}

TEST(Python, ACaptureSetInAnEarlierIterationSurvives) {
  // KEEP_LAST_SET. Python sides with PCRE2 on the axis where the two
  // Perl-family references disagree: perl 5.40 reports group 2 as unset.
  EXPECT_EQ(group("((a)|b)+", "ab", 2), "0-1");
}

TEST(Python, NotAWordBoundaryMatchesTheEmptySubject) {
  // `\B` asks whether a position is *not* a word boundary, and position 0
  // of "" has no word character on either side - so it holds there. That is
  // what perl and Node have always answered; CPython answered the opposite
  // until 3.14, which is the release this dialect now follows, so the empty
  // subject is no longer the one position where the three disagree.
  EXPECT_EQ(span("\\B", ""), "0-0");
  EXPECT_EQ(span("\\B", "", GRX_SYNTAX_PERL), "0-0");
  EXPECT_EQ(span("\\B", "ab"), "1-1");
  EXPECT_EQ(span("\\B", "a"), "nomatch");
  EXPECT_EQ(span("\\B", " "), "0-0");
  // `\b` agrees with everyone, including on the empty subject, and did so
  // before 3.14 as well: only `\B` moved.
  EXPECT_EQ(span("\\b", ""), "nomatch");
  EXPECT_EQ(span("\\b", "a"), "0-0");
}

TEST(Python, AQuantifierSuffixMustTouchItsQuantifier) {
  // perl lets the suffix be written apart from the rest - extended mode
  // makes `a + +` possessive, and a comment is invisible, so `a*(?#c)?` is
  // a lazy repeat. `re` reads anything that intervenes as starting a second
  // quantifier, which is "multiple repeat".
  EXPECT_EQ(span("a*?", "aa"), "0-0");
  EXPECT_EQ(span("a*+a", "aa"), "nomatch");
  EXPECT_EQ(accepts("a*(?#c)?"), "refused");
  EXPECT_EQ(accepts("a*(?#c)?", GRX_SYNTAX_PERL), "ok");
  // A comment is still transparent for the quantifier's *target*.
  EXPECT_EQ(span("a(?#c)*", "aaa"), "0-3");
}

TEST(Python, SplitIsNeitherEcmascriptsNorPerls) {
  // Its empty-subject and trailing-field rules are ECMAScript's; its
  // `maxsplit` is perl's, counting splits rather than pieces and leaving
  // the remainder as the last field, with zero meaning no limit.
  // `re.split("x*", "")` is `['', '']` where ECMAScript's is `[]` and
  // perl's is `()`, which is three references and three answers for the
  // shortest question the function has.
  auto pieces = [](const std::string & pattern, const std::string & subject,
                    size_t limit) {
    Attempt attempt = compile(pattern, GRX_SYNTAX_PYTHON);
    if (attempt.result != GRX_OK) {
      grx_regex_free(attempt.regex);
      return std::string("error");
    }
    GRX_Split split;
    std::memset(&split, 0, sizeof(split));
    std::string answer;
    if (grx_regex_split(attempt.regex, subject.data(), subject.size(), limit,
            nullptr, nullptr, nullptr, &split) == GRX_OK) {
      for (size_t i = 0; i < split.count; i++) {
        answer += "["
            + subject.substr(split.pieces[i].start,
                split.pieces[i].end - split.pieces[i].start)
            + "]";
      }
      grx_split_free(&split);
    }
    grx_regex_free(attempt.regex);
    return answer;
  };

  EXPECT_EQ(pieces(",", "a,b,c", 0), "[a][b][c]");
  EXPECT_EQ(pieces(",", "", 0), "[]");
  EXPECT_EQ(pieces("x*", "", 0), "[][]");
  EXPECT_EQ(pieces(",", "a,b,,", 0), "[a][b][][]");
  EXPECT_EQ(pieces("x*", "abc", 0), "[][a][b][c][]");
  // `maxsplit` of 1 leaves the remainder whole; ECMAScript's limit of 1
  // would discard it, and its limit of 0 would yield nothing at all.
  EXPECT_EQ(pieces(",", "a,b,c", 1), "[a][b,c]");
}

TEST(Python, TheReplacementTemplateIsBackslashSpelled) {
  EXPECT_EQ(replace_all("(a)(b)", "ab", "\\1"), "a");
  EXPECT_EQ(replace_all("(a)(b)", "ab", "\\2\\1"), "ba");
  EXPECT_EQ(replace_all("(a)(b)", "ab", "\\g<1>"), "a");
  EXPECT_EQ(replace_all("(?P<x>a)", "a", "\\g<x>"), "a");
  // `\g<0>` is the whole match, where a bare `\0` is NUL - the one place
  // the two numeric spellings mean different things.
  EXPECT_EQ(replace_all("(a)(b)", "ab", "\\g<0>"), "ab");
  EXPECT_EQ(replace_all("(a)(b)", "ab", "\\0"), std::string("\0", 1));
  // A reference to a group that does not exist is an error, not a literal.
  EXPECT_EQ(replace_all("(a)(b)", "ab", "\\3"), "template");
  EXPECT_EQ(replace_all("(a)(b)", "ab", "\\g<nosuch>"), "template");
}

TEST(Python, ALookbehindMustBeOneLength) {
  // GRX_LOOKBEHIND_FIXED was in the profile from the day the table was
  // written and was read by nothing: it chose an execution model and never
  // bounded anything, so every variable-width body compiled. The
  // differential could not see it either, because its lookbehind vocabulary
  // was written on the assumption that the rule already held.
  EXPECT_EQ(span("(?<=ab)c", "abc"), "2-3");
  for (const char * refused : {"(?<=a+)c", "(?<=ab|c)d", "(?<=a{2,4})c",
           "(?<=a*)c", "(?<=a?)c", "(?<!a+)c"}) {
    EXPECT_EQ(accepts(refused), "refused") << refused;
  }
  // A backreference is as long as the group it names, so a lookbehind is
  // fixed when that group is and variable when it is not.
  EXPECT_EQ(accepts("(x)(?<=\\1a)"), "ok");
  EXPECT_EQ(accepts("(|a)(?<=\\1a)"), "refused");
}

TEST(Python, AClassWithAnEscapeStillEnds) {
  // Not a Python rule at all: the prescan cleared `first_in_class` for an
  // ordinary member and not for an escaped one, so the `]` closing `[\d]`
  // was read as a literal, the class ran to the end of the pattern, and
  // every group after it went uncounted. `[\d](a)\1` was an invalid
  // backreference in perl and pcre as well - ECMAScript was immune only
  // because allow_empty_class makes the test it is part of vacuous.
  for (GRX_Syntax syntax : {GRX_SYNTAX_PYTHON, GRX_SYNTAX_PERL,
           GRX_SYNTAX_PCRE}) {
    EXPECT_EQ(span("[\\d](a)\\1", "1aa", syntax), "0-3")
        << grx_syntax_name(syntax);
    EXPECT_EQ(span("[\\w-](a)\\1", "-aa", syntax), "0-3")
        << grx_syntax_name(syntax);
  }
  // And the leading-`]` rule it was part of still holds.
  EXPECT_EQ(span("[]a]", "]"), "0-1");
  EXPECT_EQ(span("[]a]", "a"), "0-1");
}

TEST(Python, QuotingIsGatedOnTheFeatureBit) {
  // GRX_FEATURE_QUOTING was in the table from the start and nothing read
  // it, because the two dialects that reached the code both had it. Python
  // has no `\Q`, so leaving it ungated made `\Qa\E` a quoted run here and
  // `\Q` on its own a silent nothing.
  EXPECT_EQ(accepts("\\Qa.b\\E", GRX_SYNTAX_PERL), "ok");
  EXPECT_EQ(accepts("\\Qa.b\\E"), "refused");
  EXPECT_EQ(accepts("[\\Qa\\E]"), "refused");
  EXPECT_EQ(accepts("\\Q"), "refused");
  EXPECT_EQ(accepts("\\E"), "refused");
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
