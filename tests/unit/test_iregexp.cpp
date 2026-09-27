/**
 * @file
 *
 * The I-Regexp front end: RFC 9485, and the refusals that make it checking.
 *
 * The authority for every expectation here is the ABNF in Figure 1 of RFC
 * 9485, quoted beside the tests that read it. That is a different kind of
 * authority from the one the other dialect tests use - Perl, PCRE2, CPython
 * and glibc are *implementations*, asked by a differential and recorded in
 * vectors, because a specification they diverge from is not what a caller
 * gets. I-Regexp has no shipped reference on this machine, so the grammar is
 * the oracle, which is the one case where reading a document is the
 * measurement rather than a substitute for it.
 *
 * Two consequences of that, both deliberate:
 *
 * - **The refusals are the subject.** A checking implementation is defined by
 *   what it will not accept (section 3.1), so most of what follows is a
 *   pattern that is legal ECMAScript and illegal here. A test suite that only
 *   matched strings would pass against a front end that was ECMAScript's with
 *   a different name.
 * - **Each test names its production.** When one of these fails, the failure
 *   should send a reader to a line of the ABNF rather than to a guess.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <string>
#include <vector>

#include "test_helpers.h"

namespace {

struct Attempt {
  GRX_Result result;
  GRX_Diag diag;
  GRX_Regex * regex;
};

Attempt compile(const std::string & pattern, uint32_t options = 0) {
  Attempt attempt = {GRX_OK, GRX_DIAG_NONE, nullptr};
  GRX_Error error;
  grx_error_clear(&error);
  attempt.result = grx_regex_compile_with_allocator(pattern.data(),
      pattern.size(), GRX_SYNTAX_IREGEXP, options, nullptr, nullptr, &error,
      &attempt.regex);
  attempt.diag = error.diag;
  return attempt;
}

/** "ok" or "refused", so that a failure message says which way it went. */
std::string accepts(const std::string & pattern, uint32_t options = 0) {
  Attempt attempt = compile(pattern, options);
  grx_regex_free(attempt.regex);
  return attempt.result == GRX_OK ? "ok" : "refused";
}

/** Which diagnostic a refusal carried. */
GRX_Diag why(const std::string & pattern) {
  Attempt attempt = compile(pattern);
  grx_regex_free(attempt.regex);
  return attempt.diag;
}

/** Which result code a refusal carried: syntax and limit are not the same. */
GRX_Result code(const std::string & pattern, const GRX_Limits * limits) {
  GRX_Error error;
  grx_error_clear(&error);
  GRX_Regex * regex = nullptr;
  GRX_Result result = grx_regex_compile_with_allocator(pattern.data(),
      pattern.size(), GRX_SYNTAX_IREGEXP, 0, limits, nullptr, &error, &regex);
  grx_regex_free(regex);
  return result;
}

/**
 * JSONPath's two questions, as RFC 9535 sections 2.4.6 and 2.4.7 put them.
 *
 * `match` is the whole string; `search` is any substring. The only difference
 * here is the two option bits, which is the point of them: one compile of one
 * pattern answers both, and neither answer is the front end's doing.
 */
std::string boolean(const std::string & pattern, const std::string & subject,
    bool whole) {
  uint32_t options = whole ? (GRX_OPT_ANCHORED | GRX_OPT_ANCHORED_END) : 0u;
  Attempt attempt = compile(pattern, options);
  if (attempt.result != GRX_OK) {
    grx_regex_free(attempt.regex);
    return "error";
  }
  int matched = 0;
  std::string answer = "error";
  if (grx_regex_search(attempt.regex, subject.data(), subject.size(), 0,
          GRX_ENGINE_AUTO, nullptr, nullptr, &matched)
      == GRX_OK) {
    answer = matched ? "true" : "false";
  }
  grx_regex_free(attempt.regex);
  return answer;
}

std::string match_of(const std::string & pattern, const std::string & subject) {
  return boolean(pattern, subject, true);
}

std::string search_of(const std::string & pattern, const std::string & subject) {
  return boolean(pattern, subject, false);
}

} // namespace

TEST(IRegexp, TheDialectIsNamedTheWayTheRfcNamesIt) {
  // RFC 9485's title is "I-Regexp: An Interoperable Regular Expression
  // Format", its grammar's start rule is `i-regexp`, and its section 7
  // registers nothing - no media type, no other identifier - so the RFC's own
  // spelling is the whole of the naming question.
  EXPECT_STREQ(grx_syntax_name(GRX_SYNTAX_IREGEXP), "i-regexp");

  GRX_Syntax syntax = GRX_SYNTAX_COUNT;
  EXPECT_EQ(grx_syntax_from_name("i-regexp", &syntax), GRX_OK);
  EXPECT_EQ(syntax, GRX_SYNTAX_IREGEXP);

  // The lookup ignores case, so the document's own capitalisation finds it.
  syntax = GRX_SYNTAX_COUNT;
  EXPECT_EQ(grx_syntax_from_name("I-Regexp", &syntax), GRX_OK);
  EXPECT_EQ(syntax, GRX_SYNTAX_IREGEXP);
}

TEST(IRegexp, TheCaretAndDollarAreOrdinaryCharacters) {
  // NormalChar is `%x00-27 / "," / "-" / %x2F-3E / %x40-5A / %x5E-7A / ...`,
  // which contains `$` (%x24) and begins its sixth range at `^` (%x5E). There
  // is no anchor anywhere in the grammar, so `^a$` is three characters.
  //
  // This is the case that makes RFC 9485 section 5.3's mapping wrong on raw
  // text: `^(?:^a$)$` as ECMAScript matches "a", and the I-Regexp does not.
  EXPECT_EQ(match_of("^a$", "^a$"), "true");
  EXPECT_EQ(search_of("^a$", "^a$"), "true");
  EXPECT_EQ(match_of("^a$", "a"), "false");
  EXPECT_EQ(search_of("^a$", "a"), "false");

  // And `\$` is not an escape: SingleCharEsc does not list it, because the
  // character it would escape is not special.
  EXPECT_EQ(accepts("\\$"), "refused");
  EXPECT_EQ(why("\\$"), GRX_DIAG_INVALID_ESCAPE);
}

TEST(IRegexp, ABraceMustBeEscapedAndIsNotALiteral) {
  // NormalChar skips %x7B-7D, so `{`, `|` and `}` are not ordinary
  // characters: `|` is the alternation operator and the other two have to be
  // written `\{` and `\}`. A bare `{` is neither a literal brace (Perl's
  // reading) nor the start of a quantifier that may fail (ECMAScript's Annex
  // B reading); it is a syntax error, and so is a bare `}`.
  //
  // `]` goes the same way for the same reason - %x5B-5D is skipped too - and
  // it is the one of the three that is easy to write by accident.
  for (const char * pattern : {"{", "}", "]", "a{", "a}b", "a]b"}) {
    EXPECT_EQ(accepts(pattern), "refused") << pattern;
    EXPECT_EQ(why(pattern), GRX_DIAG_UNESCAPED_METACHARACTER) << pattern;
  }

  EXPECT_EQ(match_of("\\{", "{"), "true");
  EXPECT_EQ(match_of("\\}", "}"), "true");
  EXPECT_EQ(match_of("\\]", "]"), "true");
  EXPECT_EQ(match_of("\\{\\}", "{}"), "true");

  // `{,3}` has no QuantExact before the comma - `QuantExact = 1*%x30-39` is
  // not optional - so it is not a quantifier, and a `{` that is not one has
  // nowhere else to go.
  EXPECT_EQ(accepts("a{,3}"), "refused");
  EXPECT_EQ(why("a{,3}"), GRX_DIAG_UNESCAPED_METACHARACTER);
}

TEST(IRegexp, TheDotIsXsdsDotAndNotEcmaScripts) {
  // XSD's `.` excludes CR and LF and nothing else, where ECMAScript's also
  // excludes U+2028 and U+2029. So the line separator is a character here.
  // Section 5.3's rewrite to `[^\n\r]` is the right *meaning*; it is the
  // enveloping step of that mapping that is wrong, not this one.
  EXPECT_EQ(match_of(".", "\xE2\x80\xA8"), "true");  // U+2028
  EXPECT_EQ(match_of(".", "\xE2\x80\xA9"), "true");  // U+2029
  EXPECT_EQ(match_of(".", "\n"), "false");
  EXPECT_EQ(match_of(".", "\r"), "false");
  EXPECT_EQ(match_of(".", "a"), "true");
  EXPECT_EQ(match_of(".", "\xC3\xA9"), "true");      // one character, not two
}

TEST(IRegexp, TheSeventeenSingleCharacterEscapesAndNoOthers) {
  // SingleCharEsc = "\" ( %x28-2B / "-" / "." / "?" / %x5B-5E / %s"n" /
  //                      %s"r" / %s"t" / %x7B-7D )
  const char * const legal[] = {
    "\\(", "\\)", "\\*", "\\+", "\\-", "\\.", "\\?",
    "\\[", "\\\\", "\\]", "\\^", "\\{", "\\|", "\\}",
    "\\n", "\\r", "\\t",
  };
  for (const char * pattern : legal) {
    EXPECT_EQ(accepts(pattern), "ok") << pattern;
  }
  EXPECT_EQ(match_of("\\n", "\n"), "true");
  EXPECT_EQ(match_of("\\t", "\t"), "true");
  EXPECT_EQ(match_of("\\r", "\r"), "true");

  // `%s` is RFC 7405's case-sensitive literal, so the three letters are lower
  // case only. And every multi-character escape is absent: the RFC removes
  // them from XSD by name (section 3), because the dialects disagree about
  // what they mean - `\d` is `\p{Nd}` in XSD and `[0-9]` almost everywhere
  // else, which is the disagreement that made it not worth keeping.
  for (const char * pattern : {"\\d", "\\D", "\\s", "\\S", "\\w", "\\W",
           "\\i", "\\I", "\\c", "\\C", "\\N", "\\R", "\\T",
           "\\b", "\\B", "\\A", "\\z", "\\Z", "\\G", "\\K",
           "\\x41", "\\u0041", "\\0", "\\1", "\\e", "\\a", "\\f", "\\v",
           "\\Qa\\E", "\\ ", "\\/"}) {
    EXPECT_EQ(accepts(pattern), "refused") << pattern;
    EXPECT_EQ(why(pattern), GRX_DIAG_INVALID_ESCAPE) << pattern;
  }
}

TEST(IRegexp, TheCategoryNamesAreFigureOnesAndNothingElse) {
  // IsCategory is seven productions with an optional second letter each,
  // which is thirty-six names. All of them, because RFC 9485 makes full
  // Unicode support REQUIRED rather than optional (section 3).
  const char * const categories[] = {
    "C", "Cc", "Cf", "Cn", "Co",
    "L", "Ll", "Lm", "Lo", "Lt", "Lu",
    "M", "Mc", "Me", "Mn",
    "N", "Nd", "Nl", "No",
    "P", "Pc", "Pd", "Pe", "Pf", "Pi", "Po", "Ps",
    "S", "Sc", "Sk", "Sm", "So",
    "Z", "Zl", "Zp", "Zs",
  };
  for (const char * name : categories) {
    EXPECT_EQ(accepts(std::string("\\p{") + name + "}"), "ok") << name;
    EXPECT_EQ(accepts(std::string("\\P{") + name + "}"), "ok") << name;
  }

  // `Cs` is the one general category the grammar leaves out: `Others = %s"C"
  // [ ( %s"c" / %s"f" / %s"n" / %s"o" ) ]` has no `s`, which is of a piece
  // with NormalChar skipping the surrogate code points.
  EXPECT_EQ(accepts("\\p{Cs}"), "refused");
  EXPECT_EQ(why("\\p{Cs}"), GRX_DIAG_UNKNOWN_PROPERTY);

  // Everything else a `\p` can name in another dialect. `\p{IsBasicLatin}` is
  // the interesting one: RFC 9485 section 5.1 offers it as the way to say
  // legacy ASCII and cannot itself spell it, which erratum 8505 against that
  // section points out. Blocks are named as removed from XSD in section 3.
  for (const char * name : {"Letter", "gc=Lu", "General_Category=Lu",
           "Script=Latin", "sc=Latn", "IsBasicLatin", "IsGreek", "Alphabetic",
           "Any", "ASCII", "lu", "LU", "nd", ""}) {
    EXPECT_EQ(accepts(std::string("\\p{") + name + "}"), "refused") << name;
  }
  EXPECT_EQ(why("\\p{Letter}"), GRX_DIAG_UNKNOWN_PROPERTY);
  EXPECT_EQ(why("\\p{Script=Latin}"), GRX_DIAG_UNKNOWN_PROPERTY);

  // And what the accepted ones mean, one case out of the thirty-six: the
  // categories are Unicode's, not ASCII's.
  EXPECT_EQ(match_of("\\p{Nd}", "\xD9\xA3"), "true");   // U+0663 ARABIC-INDIC 3
  EXPECT_EQ(match_of("\\p{Nd}", "7"), "true");
  EXPECT_EQ(match_of("\\p{Nd}", "x"), "false");
  EXPECT_EQ(match_of("\\P{C}", "a"), "true");
}

TEST(IRegexp, AClassIsCharactersRangesAndCategoryEscapes) {
  //     charClassExpr = "[" [ "^" ] ( "-" / CCE1 ) *CCE1 [ "-" ] "]"
  //     CCE1 = ( CCchar [ "-" CCchar ] ) / charClassEsc
  //     CCchar = ( %x00-2C / %x2E-5A / %x5E-D7FF / %xE000-10FFFF )
  //              / SingleCharEsc
  for (const char * pattern : {"[a]", "[a-z]", "[^a-z]", "[-a]", "[a-]", "[-]",
           "[\\p{L}]", "[^\\p{L}\\p{Nd}]", "[\\]]", "[\\\\]", "[\\n\\r\\t]",
           "[\\--a]", "[abc-]", "[^-]"}) {
    EXPECT_EQ(accepts(pattern), "ok") << pattern;
  }

  // A `]` cannot be a member (CCchar's %x2E-5A stops at `Z`), and the
  // production requires at least one item - so `[]` is not an empty class the
  // way it is in ECMAScript, and not a class holding `]` the way it is in
  // POSIX. It is an error.
  EXPECT_EQ(accepts("[]"), "refused");
  EXPECT_EQ(why("[]"), GRX_DIAG_EMPTY_CLASS);
  EXPECT_EQ(accepts("[]a]"), "refused");

  // `[^]` is refused by a restriction the RFC states in prose right after
  // Figure 1: by the grammar alone it would be a positive class whose one
  // member is `^`, and the spelling is not to mean that.
  EXPECT_EQ(accepts("[^]"), "refused");
  EXPECT_EQ(why("[^]"), GRX_DIAG_NOT_IN_DIALECT);

  // Class subtraction is the first of the three things section 3 names as
  // removed from XSD. It fails at the inner `[`, which is not a CCchar.
  EXPECT_EQ(accepts("[a-z-[aeiou]]"), "refused");
  EXPECT_EQ(why("[a-z-[aeiou]]"), GRX_DIAG_INVALID_CLASS_ITEM);
  EXPECT_EQ(accepts("[a[b]]"), "refused");

  // A bare `-` is a member only where the production has one: right after the
  // `[` or `[^`, or right before the `]`. It is never a range endpoint -
  // CCchar skips %x2D - so `[--a]` is not the range `-` to `a` here, which is
  // what every other dialect in this library reads it as. `[\--a]` is how
  // that range is written.
  EXPECT_EQ(accepts("[--a]"), "refused");
  EXPECT_EQ(why("[--a]"), GRX_DIAG_INVALID_CLASS_ITEM);
  EXPECT_EQ(accepts("[a-b-c]"), "refused");
  EXPECT_EQ(match_of("[\\--a]", "0"), "true");

  // A range's endpoints are CCchars, and a charClassEsc is not one. So a
  // category at either end is not a range - and not a union of three members
  // either, which is what Perl reads `[\d-z]` as.
  EXPECT_EQ(accepts("[\\p{L}-z]"), "refused");
  EXPECT_EQ(why("[\\p{L}-z]"), GRX_DIAG_CLASS_ESCAPE_IN_RANGE);
  EXPECT_EQ(accepts("[a-\\p{L}]"), "refused");
  EXPECT_EQ(why("[a-\\p{L}]"), GRX_DIAG_CLASS_ESCAPE_IN_RANGE);

  // And the escapes inside a class are the same seventeen, so a multi-
  // character escape is refused there exactly as it is outside.
  EXPECT_EQ(accepts("[\\d]"), "refused");
  EXPECT_EQ(why("[\\d]"), GRX_DIAG_INVALID_ESCAPE);
  EXPECT_EQ(accepts("[[:alpha:]]"), "refused");

  // What the classes match, so that the reader is not only a checker.
  EXPECT_EQ(match_of("[a-]", "-"), "true");
  EXPECT_EQ(match_of("[-a]", "-"), "true");
  EXPECT_EQ(match_of("[^a-z]", "A"), "true");
  EXPECT_EQ(match_of("[^a-z]", "q"), "false");
}

TEST(IRegexp, ThereIsOneKindOfGroupAndNoOtherParenthesisForm) {
  // `atom = NormalChar / charClass / ( "(" i-regexp ")" )`. One group
  // production, so `(?` begins nothing: a `?` would have to start a piece,
  // and a quantifier is not an atom.
  EXPECT_EQ(accepts("(a)"), "ok");
  EXPECT_EQ(accepts("(a|b)+"), "ok");
  EXPECT_EQ(accepts("()"), "ok");        // `branch = *piece`, so a branch may be empty
  EXPECT_EQ(accepts("((a))"), "ok");

  for (const char * pattern : {"(?:a)", "(?=a)", "(?!a)", "(?<=a)", "(?<!a)",
           "(?<n>a)", "(?'n'a)", "(?P<n>a)", "(?i)a", "(?i:a)", "(?#c)",
           "(?>a)", "(?(1)a|b)", "(?R)", "(?1)"}) {
    EXPECT_EQ(accepts(pattern), "refused") << pattern;
    EXPECT_EQ(why(pattern), GRX_DIAG_NOT_IN_DIALECT) << pattern;
  }

  // A backreference has no spelling at all, `\1` being an escape the
  // alphabet does not have - so a group cannot be referred to, and this
  // stays true however the group was written.
  EXPECT_EQ(accepts("(a)\\1"), "refused");
  EXPECT_EQ(why("(a)\\1"), GRX_DIAG_INVALID_ESCAPE);

  // The group captures, which the RFC neither asks for nor forbids: it has
  // only a Boolean to give. A caller who wants the span can have it.
  Attempt attempt = compile("(a)(b)");
  ASSERT_EQ(attempt.result, GRX_OK);
  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(attempt.regex, nullptr, &match), GRX_OK);
  EXPECT_EQ(grx_match_count(match), 3u);
  grx_match_destroy(match);
  grx_regex_free(attempt.regex);
}

TEST(IRegexp, AQuantifierIsGreedyAndNeverRepeatedTwice) {
  // `piece = atom [ quantifier ]` - one quantifier, and a quantifier is not
  // an atom, so a second one has nothing to attach to. That is what makes
  // `a*?` two quantifiers on one atom rather than a lazy repeat: there is no
  // lazy form in the grammar to read it as.
  for (const char * pattern : {"a*", "a+", "a?", "a{2}", "a{2,}", "a{2,4}",
           "(ab){3}", "a{007}"}) {
    EXPECT_EQ(accepts(pattern), "ok") << pattern;
  }
  for (const char * pattern : {"a*?", "a+?", "a??", "a*+", "a**", "a{2}{3}",
           "a{2,3}?"}) {
    EXPECT_EQ(accepts(pattern), "refused") << pattern;
    EXPECT_EQ(why(pattern), GRX_DIAG_DOUBLE_QUANTIFIER) << pattern;
  }

  // A leading zero is `1*%x30-39` like any other digit run.
  EXPECT_EQ(match_of("a{007}", "aaaaaaa"), "true");
  EXPECT_EQ(match_of("a{007}", "aaaaaa"), "false");
}

TEST(IRegexp, RangeQuantifierCostIsALimitAndNotASyntaxError) {
  // Section 8 names range quantifiers as the cost to worry about and offers
  // implementations three ways out: refuse nesting, refuse large ranges, or
  // detect the consumption. This library takes the third - GRX_Limits - which
  // means `a{20,200000}` is GRX_ERR_LIMIT rather than GRX_ERR_SYNTAX.
  //
  // The distinction is not cosmetic. A caller mapping this onto JSONPath
  // turns an invalid regexp into a `false` (RFC 9535 section 2.4.6) and must
  // *not* do that with a budget that ran out, which decided nothing.
  GRX_Limits limits;
  grx_limits_default(&limits);
  EXPECT_EQ(code("a{20,200000}", &limits), GRX_ERR_LIMIT);
  EXPECT_EQ(why("a{20,200000}"), GRX_DIAG_LIMIT_REPEAT_COUNT);

  // Raised, the refusal *moves* rather than going away: 200,000 copies of an
  // atom is a large program, so max_program_size answers next. Which is the
  // point - the cost is a resource question and every answer to it is
  // GRX_ERR_LIMIT, never GRX_ERR_SYNTAX. The pattern is valid I-Regexp at
  // every one of these settings.
  limits.max_repeat_count = 1000000;
  EXPECT_EQ(code("a{20,200000}", &limits), GRX_ERR_LIMIT);

  limits.max_program_size = 0;
  limits.max_nodes = 0;
  EXPECT_EQ(code("a{20,200000}", &limits), GRX_OK)
      << "with no cap left to hit, a valid pattern must compile";

  // Nesting is accepted, the RFC leaving that to the implementation, and the
  // same limits bound it.
  EXPECT_EQ(accepts("(a{2,4}){2,4}"), "ok");
  EXPECT_EQ(code("(a{2,4}){2,4}", nullptr), GRX_OK);
}

TEST(IRegexp, MatchIsTheWholeStringAndSearchIsAnySubstring) {
  // RFC 9535 sections 2.4.6 and 2.4.7. The table here is the one the `text`
  // library needs to satisfy, and every row of it comes from one compile of
  // one pattern with two option settings - which is why the anchoring is not
  // in the front end. A dialect that wrapped its own patterns in `\A` and
  // `\z` could answer the first column and not the second.
  struct Row {
    const char * pattern;
    const char * subject;
    const char * match;
    const char * search;
  };
  const Row rows[] = {
    {"a", "ab", "false", "true"},
    {"^a$", "^a$", "true", "true"},
    {"^a$", "a", "false", "false"},
    {"\\{", "{", "true", "true"},
    {".", "\xE2\x80\xA8", "true", "true"},
    {".", "\n", "false", "false"},
    {"\\p{Nd}", "\xD9\xA3", "true", "true"},
    // The empty pattern matches only the empty string, and every string
    // contains the empty substring - including the empty one.
    {"", "", "true", "true"},
    {"", "a", "false", "true"},
    // The case that needs more than "does the span reach the end": leftmost
    // first would report `a` here, and the whole-string question is still
    // true because `ab` is what the other branch matches.
    {"a|ab", "ab", "true", "true"},
    {"a|ab", "abc", "false", "true"},
  };
  for (const Row & row : rows) {
    EXPECT_EQ(match_of(row.pattern, row.subject), row.match)
        << "match(" << row.subject << ", " << row.pattern << ")";
    EXPECT_EQ(search_of(row.pattern, row.subject), row.search)
        << "search(" << row.subject << ", " << row.pattern << ")";
  }
}

TEST(IRegexp, TheEndAnchorIsTheSubjectAndNotALine) {
  // GRX_OPT_ANCHORED_END lowers to `\z` and not to `$`, so a final newline
  // does not satisfy it and GRX_SEARCH_NOTEOL does not reach it. That is
  // PCRE2_ENDANCHORED's meaning, and it is what a whole-string Boolean needs:
  // "abc\n" is not "abc".
  EXPECT_EQ(match_of("abc", "abc\n"), "false");
  EXPECT_EQ(match_of("abc\\n", "abc\n"), "true");
  EXPECT_EQ(search_of("abc", "abc\n"), "true");

  // A window ends the subject for the search that names it, which is what
  // makes iteration over a buffer possible - so the end anchor holds at the
  // window's end rather than the buffer's.
  Attempt attempt = compile("abc", GRX_OPT_ANCHORED | GRX_OPT_ANCHORED_END);
  ASSERT_EQ(attempt.result, GRX_OK);
  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.end = 3;
  int matched = 0;
  EXPECT_EQ(grx_regex_search_ex(attempt.regex, "abcdef", 6, &options, nullptr,
                &matched),
      GRX_OK);
  EXPECT_TRUE(matched);
  grx_regex_free(attempt.regex);
}

TEST(IRegexp, TheStartAnchorOptionIsHonouredAtTheOffsetItNames) {
  // GRX_OPT_ANCHORED is the other half, and it had no reader at all until
  // this dialect needed one: a caller who set it got an unanchored search and
  // no diagnostic. What it says is "match only at the start offset", which is
  // the offset the search was given and not offset 0 - so it is honoured by
  // the search rather than as an `\A` assertion, which would be a different
  // rule wherever a search begins past the start.
  Attempt attempt = compile("b", GRX_OPT_ANCHORED);
  ASSERT_EQ(attempt.result, GRX_OK);

  int matched = 0;
  ASSERT_EQ(grx_regex_search(attempt.regex, "ab", 2, 0, GRX_ENGINE_AUTO,
                nullptr, nullptr, &matched),
      GRX_OK);
  EXPECT_FALSE(matched) << "anchored at 0, where the subject has an `a`";

  matched = 0;
  ASSERT_EQ(grx_regex_search(attempt.regex, "ab", 2, 1, GRX_ENGINE_AUTO,
                nullptr, nullptr, &matched),
      GRX_OK);
  EXPECT_TRUE(matched) << "anchored at 1, which is where the `b` is";
  grx_regex_free(attempt.regex);
}

TEST(IRegexp, EveryPatternIsRegularAndRunsOnEveryEngine) {
  // No backreference and no lookaround, so nothing in the dialect can make a
  // pattern non-regular - which is what section 8's "linear time and space"
  // aspiration needs, and what lets the Pike VM run every I-Regexp there is.
  const char * const patterns[] = {
    "a|ab", "(a{2,4}){2,4}", "[^\\p{L}]*", "(a)(b)(c)", ".*", "\\p{Lu}+",
  };
  for (const char * pattern : patterns) {
    Attempt attempt = compile(pattern);
    ASSERT_EQ(attempt.result, GRX_OK) << pattern;
    GRX_Facts facts;
    ASSERT_EQ(grx_regex_facts(attempt.regex, &facts), GRX_OK) << pattern;
    EXPECT_TRUE(facts.is_regular) << pattern;

    int matched = 0;
    EXPECT_EQ(grx_regex_search(attempt.regex, "ab", 2, 0, GRX_ENGINE_PIKE,
                  nullptr, nullptr, &matched),
        GRX_OK)
        << pattern;
    grx_regex_free(attempt.regex);
  }
}

TEST(IRegexp, ThereIsNoFlagInTheSyntaxAndTheCallerMayStillSetOne) {
  // The grammar has no flag anywhere - no `(?i)`, and no trailing letters
  // either, a pattern being the whole string. So a caseless I-Regexp cannot
  // be written, which is what makes `(?i)a` a refusal above.
  //
  // A caller can still ask, because GRX_Option belongs to the caller in every
  // dialect: this library's header says so, and it is what lets a host
  // language offer its own case-insensitive flag over an interoperable
  // pattern. Simple folding is what it gets - not the full folding Perl
  // would use, because a fold that makes one pattern character match two
  // subject characters is not something XSD or RFC 9485 contemplates.
  EXPECT_EQ(match_of("a", "A"), "false");
  EXPECT_EQ(boolean("a", "A", true), "false");

  // The flag alphabet is empty, so every letter is unknown rather than
  // accepted-and-ignored. An empty flag string is still valid: it asks for
  // nothing, which is what every I-Regexp asks for.
  uint32_t options = 0xFFFFFFFFu;
  GRX_Error error;
  grx_error_clear(&error);
  EXPECT_EQ(grx_options_parse(GRX_SYNTAX_IREGEXP, "", &options, &error),
      GRX_OK);
  EXPECT_EQ(options, 0u);
  for (const char * letter : {"i", "u", "v", "m", "s", "x", "a", "g"}) {
    grx_error_clear(&error);
    EXPECT_EQ(grx_options_parse(GRX_SYNTAX_IREGEXP, letter, &options, &error),
        GRX_ERR_SYNTAX)
        << letter;
    EXPECT_EQ(error.diag, GRX_DIAG_UNKNOWN_FLAG) << letter;
  }

  Attempt attempt = compile("a",
      GRX_OPT_CASELESS | GRX_OPT_ANCHORED | GRX_OPT_ANCHORED_END);
  ASSERT_EQ(attempt.result, GRX_OK);
  int matched = 0;
  EXPECT_EQ(grx_regex_search(attempt.regex, "A", 1, 0, GRX_ENGINE_AUTO,
                nullptr, nullptr, &matched),
      GRX_OK);
  EXPECT_TRUE(matched);
  grx_regex_free(attempt.regex);

  // `ß` folds to itself under simple folding, where full folding would make
  // it match "ss" - two characters for one.
  Attempt sharp = compile("\xC3\x9F",
      GRX_OPT_CASELESS | GRX_OPT_ANCHORED | GRX_OPT_ANCHORED_END);
  ASSERT_EQ(sharp.result, GRX_OK);
  matched = 0;
  EXPECT_EQ(grx_regex_search(sharp.regex, "ss", 2, 0, GRX_ENGINE_AUTO, nullptr,
                nullptr, &matched),
      GRX_OK);
  EXPECT_FALSE(matched);
  grx_regex_free(sharp.regex);
}

TEST(IRegexp, AClassIsTheSameSetWhateverModeACallerAsksFor) {
  // `GRX_OPT_UNICODE_SETS` selects ECMAScript's `v`-mode *lowering* for every
  // class node, and a caller can set the bit on any dialect - the letter is
  // refused for this one, the bit is not. It changes nothing here, and by
  // construction rather than by luck: `v` mode differs only for a class with
  // a *string* in it, and this grammar has no `\q{...}` and no set operator,
  // so no class item this front end builds can be one.
  //
  // Measured rather than reasoned, because "it cannot matter" is the sentence
  // that precedes finding out that it did: the same class, the same program
  // size, the same answer.
  const char * const classes[] = {"[a-z]", "[^a-z]", "[\\p{L}]", "[-a]"};
  for (const char * pattern : classes) {
    Attempt plain = compile(pattern);
    Attempt sets = compile(pattern, GRX_OPT_UNICODE_SETS);
    ASSERT_EQ(plain.result, GRX_OK) << pattern;
    ASSERT_EQ(sets.result, GRX_OK) << pattern;
    EXPECT_EQ(grx_regex_program_size(plain.regex),
        grx_regex_program_size(sets.regex))
        << pattern;
    grx_regex_free(plain.regex);
    grx_regex_free(sets.regex);

    EXPECT_EQ(boolean(pattern, "q", true),
        [&] {
          Attempt attempt = compile(pattern,
              GRX_OPT_UNICODE_SETS | GRX_OPT_ANCHORED | GRX_OPT_ANCHORED_END);
          int matched = 0;
          grx_regex_search(attempt.regex, "q", 1, 0, GRX_ENGINE_AUTO, nullptr,
              nullptr, &matched);
          grx_regex_free(attempt.regex);
          return std::string(matched ? "true" : "false");
        }())
        << pattern;
  }
}

TEST(IRegexp, ReplacementIsRefusedBecauseTheDialectHasNone) {
  // RFC 9485 defines a Boolean match and no substitution grammar, so there is
  // no sigil for a template to use. The refusal is GRX_DIAG_NOT_IN_DIALECT
  // and not GRX_DIAG_DIALECT_NOT_IMPLEMENTED, which is the answer for a
  // dialect whose template row is simply unwritten - and I-Regexp is the
  // first dialect that is *built* and has no replacement grammar, so it is
  // the first case where those two had to be told apart.
  Attempt attempt = compile("a");
  ASSERT_EQ(attempt.result, GRX_OK);

  GRX_Error error;
  grx_error_clear(&error);
  GRX_Text out = {nullptr, 0, nullptr};
  EXPECT_NE(grx_regex_replace(attempt.regex, "aaa", 3, "b", 1,
                GRX_REPLACE_GLOBAL, nullptr, nullptr, &error, &out),
      GRX_OK);
  EXPECT_EQ(error.diag, GRX_DIAG_NOT_IN_DIALECT);
  grx_text_free(&out);

  // A literal replacement needs no grammar and so is not refused.
  grx_error_clear(&error);
  out = GRX_Text{nullptr, 0, nullptr};
  EXPECT_EQ(grx_regex_replace(attempt.regex, "aaa", 3, "b", 1,
                GRX_REPLACE_GLOBAL | GRX_REPLACE_LITERAL, nullptr, nullptr,
                &error, &out),
      GRX_OK);
  EXPECT_EQ(std::string(out.data ? out.data : ""), "bbb");
  grx_text_free(&out);
  grx_regex_free(attempt.regex);
}

TEST(IRegexp, APatternIsUnicodeScalarValuesWrittenAsThemselves) {
  // Section 1.1: an I-Regexp is a sequence of Unicode scalar values, and
  // there is no `\u` escape - so a non-ASCII character is written as itself,
  // which for a pattern arriving from JSON means UTF-8.
  EXPECT_EQ(match_of("\xC3\xA9", "\xC3\xA9"), "true");
  EXPECT_EQ(match_of("\xE2\x82\xAC{2}", "\xE2\x82\xAC\xE2\x82\xAC"), "true");
  EXPECT_EQ(accepts("\\u00E9"), "refused");

  // A pattern that is not valid UTF-8 is not a sequence of scalar values.
  // The surrogate range is what NormalChar's two ranges skip, and it has no
  // UTF-8 spelling either, so both refusals land in the same place.
  EXPECT_EQ(accepts("\xC3"), "refused");
  EXPECT_EQ(why("\xC3"), GRX_DIAG_INVALID_UTF8_IN_PATTERN);
  EXPECT_EQ(accepts("\xED\xA0\x80"), "refused");
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
