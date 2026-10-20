/**
 * @file
 *
 * JDK 25 parser checkpoint: spellings that Java accepts and rejects.
 * The pinned cases live in tests/data/probe/java25.json.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <string>

#include "test_helpers.h"

namespace {

GRX_Result compile_java(const char * text, GRX_Error * error = nullptr) {
  GRX_Regex * regex = nullptr;
  GRX_Error local;
  GRX_Result result = grx_regex_compile_with_allocator(text, std::strlen(text),
      GRX_SYNTAX_JAVA, GRX_OPT_NONE, nullptr, nullptr,
      error ? error : &local, &regex);
  grx_regex_free(regex);
  return result;
}

bool matches_java(const char * pattern, const char * subject,
    uint32_t options = GRX_OPT_NONE) {
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  if (grx_regex_compile_with_allocator(pattern, std::strlen(pattern),
          GRX_SYNTAX_JAVA, options, nullptr, nullptr, &error,
          &regex) != GRX_OK) return false;
  GRX_Match * match = nullptr;
  if (grx_match_create(regex, nullptr, &match) != GRX_OK) {
    grx_regex_free(regex);
    return false;
  }
  int matched = 0;
  GRX_Result result = grx_regex_search(regex, subject, std::strlen(subject),
      0, GRX_ENGINE_AUTO, nullptr, match, &matched);
  grx_match_destroy(match);
  grx_regex_free(regex);
  return result == GRX_OK && matched;
}

std::string search_all_java(const char * pattern, const char * subject,
    size_t length, size_t begin = 0) {
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  if (grx_regex_compile_with_allocator(pattern, std::strlen(pattern),
          GRX_SYNTAX_JAVA, GRX_OPT_NONE, nullptr, nullptr, &error,
          &regex) != GRX_OK) return "compile";
  GRX_Match * match = nullptr;
  if (grx_match_create(regex, nullptr, &match) != GRX_OK) {
    grx_regex_free(regex);
    return "create";
  }
  std::string out;
  int matched = 0;
  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.begin = begin;
  GRX_Result result = grx_regex_search_ex(regex, subject, length, &options,
      match, &matched);
  for (int guard = 0; result == GRX_OK && matched && guard < 32; guard++) {
    if (!out.empty()) out += " ";
    for (size_t i = 0; i < grx_match_count(match); i++) {
      if (i) out += ",";
      GRX_Capture capture;
      grx_match_group(match, i, &capture);
      if (capture.start == GRX_NPOS) out += "-";
      else out += std::to_string(capture.start) + ":"
          + std::to_string(capture.end);
    }
    result = grx_regex_search_next(regex, subject, length, &options,
        match, &matched);
  }
  grx_match_destroy(match);
  grx_regex_free(regex);
  return result == GRX_OK && !matched ? out : "error";
}

std::string anchored_java(const char * pattern, const char * subject,
    size_t length, size_t begin, size_t end, bool whole) {
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  uint32_t flags = GRX_OPT_ANCHORED | (whole ? GRX_OPT_ANCHORED_END : 0);
  if (grx_regex_compile_with_allocator(pattern, std::strlen(pattern),
          GRX_SYNTAX_JAVA, flags, nullptr, nullptr, &error,
          &regex) != GRX_OK) return "compile";
  GRX_Match * match = nullptr;
  if (grx_match_create(regex, nullptr, &match) != GRX_OK) {
    grx_regex_free(regex);
    return "create";
  }
  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.begin = begin;
  options.end = end;
  int matched = 0;
  GRX_Result result = grx_regex_search_ex(regex, subject, length, &options,
      match, &matched);
  std::string out = result == GRX_OK ? matched ? "match" : "nomatch" : "error";
  if (result == GRX_OK && matched) {
    for (size_t i = 0; i < grx_match_count(match); i++) {
      GRX_Capture capture;
      grx_match_group(match, i, &capture);
      out += capture.start == GRX_NPOS ? " -" : " "
          + std::to_string(capture.start) + ":"
          + std::to_string(capture.end);
    }
  }
  grx_match_destroy(match);
  grx_regex_free(regex);
  return out;
}

} // namespace

TEST(JavaParser, AcceptsMeasuredForms) {
  for (const char * pattern : {
           "(?<n>a)\\k<n>", "\\Q[a]+\\E", "\\x{1F600}", "\\u00E9",
           "\\0101", "\\cA", "\\h", "\\v", "\\R", "\\X",
           "\\b{g}", "[a&&[a-z]]", "[a&&[^a]]", "[[a-z][0-9]]",
           "[a-z&&[^aeiou]]", "[[:alpha:]]", "[\\d-z]", "[]]",
           "[-a]", "[a-]", "(?=a)*", "\\1", "(?i:a)", "(?U)\\w"}) {
    GRX_Error error;
    EXPECT_EQ(compile_java(pattern, &error), GRX_OK)
        << pattern << ": " << error.message;
  }
}

TEST(JavaParser, AcceptsRemainingPinnedSyntaxForms) {
  for (const char * pattern : {
           "(?d).", "(?s).", "(?m)^b", "(?x)a # hi\n b",
           "(?<=ab)c", "(?<=a|ab)c", "(?<=a{1,3})b", "(?<=a+)b",
           "\\p{IsGreek}", "\\p{InGreek}", "\\p{javaLowerCase}",
           "(?U)\\d", "(?iu)k", "(?dm)^b", "(?x)[a b]", "(?-i:A)",
           "[&&a]", "[a&&]", "[a-[b]]", "\\N{LATIN CAPITAL LETTER A}",
           "a{65536}", "\\p{javaUpperCase}", "\\p{javaDigit}",
           "\\p{javaWhitespace}", "\\p{javaMirrored}"}) {
    GRX_Error error;
    EXPECT_EQ(compile_java(pattern, &error), GRX_OK)
        << pattern << ": " << error.message;
  }
}

TEST(JavaParser, RefusesMeasuredForeignAndAmbiguousForms) {
  for (const char * pattern : {
           "(?R)", "(?(1)a|b)", "(*SKIP)", "\\g{1}", "(?P<n>a)",
           "[a-\\d]", "a{,3}", "a**", "\\0", "(?<=(a)\\1)b",
           "\\p{NotAProperty}", "[\\1]", "[\\k<n>]", "\\xA",
           "\\N", "\\N{U+0041}", "\\x{100000000}", "a{}",
           "\\p{Lowercase}", "\\p{Greek}", "\\p{scx=Greek}"}) {
    GRX_Error error;
    EXPECT_EQ(compile_java(pattern, &error), GRX_ERR_SYNTAX)
        << pattern << ": " << error.message;
    EXPECT_NE(error.diag, GRX_DIAG_DIALECT_NOT_IMPLEMENTED) << pattern;
  }
}

TEST(JavaParser, DistinguishesMeasuredClassAndReferenceMatches) {
  EXPECT_FALSE(matches_java("[a&&[^a]]", "b"));
  EXPECT_FALSE(matches_java("[a&&[^a]]", "a"));
  EXPECT_TRUE(matches_java("[a&&[a-z]]", "a"));
  EXPECT_FALSE(matches_java("[a&&[a-z]]", "b"));
  EXPECT_TRUE(matches_java("(a)\\1", "aa"));
  EXPECT_FALSE(matches_java("(a)\\1", "ab"));
  EXPECT_TRUE(matches_java("[&&a]", "a"));
  EXPECT_FALSE(matches_java("[&&a]", "&"));
  EXPECT_TRUE(matches_java("[a&&]", "a"));
  EXPECT_FALSE(matches_java("[a&&]", "&"));
  for (const char * subject : {"a", "-", "b"}) {
    EXPECT_TRUE(matches_java("[a-[b]]", subject)) << subject;
  }
  EXPECT_TRUE(matches_java("\\N{LATIN CAPITAL LETTER A}", "A"));
}

TEST(JavaParser, AFlatClassIsOneUnionNodeAndNotAChainOfThem) {
  // Adjacent atoms in a Java class are one flat union. Rewrapping the
  // accumulated operand per atom built a left-associated chain instead, and
  // `evaluate_class_set()` walks a chain with one stack frame per link: the
  // soak of 2026-10-19 overflowed the stack under ASan on a 7KB class.
  //
  // The assertion is the *node count*, because "it compiles and matches" is
  // not a witness. The first version of this test asked exactly that over
  // 512 atoms and passed at the unfixed commit - a chain of 511 links
  // compiles and matches perfectly well, and even 9,999 of them survive an
  // 8MB stack in a release build. What fails is the shape, every time:
  // N atoms are N+1 nodes flat and 2N-1 nodes chained.
  const size_t atoms = 512;
  std::string pattern = "[";
  pattern.append(atoms, 'a');
  pattern += ']';

  GRX_Pattern * parsed = nullptr;
  GRX_Error error;
  ASSERT_EQ(grx_pattern_parse_with_allocator(pattern.data(), pattern.size(),
                GRX_SYNTAX_JAVA, GRX_OPT_NONE, nullptr, nullptr, &error,
                &parsed),
      GRX_OK)
      << error.message;
  EXPECT_EQ(grx_pattern_node_count(parsed), atoms + 1)
      << "a flat class of " << atoms << " atoms should be one union node "
         "over " << atoms << " items; " << (2 * atoms - 1)
      << " nodes is the left-associated chain this guards against";
  grx_pattern_free(parsed);

  EXPECT_EQ(compile_java(pattern.c_str(), &error), GRX_OK) << error.message;
  EXPECT_TRUE(matches_java(pattern.c_str(), "a"));
  EXPECT_FALSE(matches_java(pattern.c_str(), "b"));
}

TEST(JavaParser, ConsecutiveIntersectionsAreOneNodeToo) {
  // The `&&` operands accumulate in a second variable, so they are a second
  // place the same chain could be built, and the fix has to cover both.
  const size_t operands = 256;
  std::string pattern = "[a";
  for (size_t i = 1; i < operands; i++) {
    pattern += "&&a";
  }
  pattern += ']';

  GRX_Pattern * parsed = nullptr;
  GRX_Error error;
  ASSERT_EQ(grx_pattern_parse_with_allocator(pattern.data(), pattern.size(),
                GRX_SYNTAX_JAVA, GRX_OPT_NONE, nullptr, nullptr, &error,
                &parsed),
      GRX_OK)
      << error.message;
  EXPECT_EQ(grx_pattern_node_count(parsed), operands + 1)
      << "consecutive && operands should be one intersection node";
  grx_pattern_free(parsed);

  EXPECT_TRUE(matches_java(pattern.c_str(), "a"));
  EXPECT_FALSE(matches_java(pattern.c_str(), "b"));
}

TEST(JavaParser, ANestedBracketStaysItsOwnNode) {
  // Flattening must not reach across a bracket: `[ab[cd]]` is a union of
  // three things, one of which is a class of its own, and the guard that
  // keeps them apart is the operator's own start offset. If flattening
  // ignored that, a nested class would be spliced into its parent and the
  // diagnostic for a bad range inside it would underline the wrong span.
  GRX_Pattern * parsed = nullptr;
  GRX_Error error;
  ASSERT_EQ(grx_pattern_parse_with_allocator("[ab[cd]]", 8, GRX_SYNTAX_JAVA,
                GRX_OPT_NONE, nullptr, nullptr, &error, &parsed),
      GRX_OK)
      << error.message;
  // a, b, the nested union node, c, d, and the outer union: six.
  EXPECT_EQ(grx_pattern_node_count(parsed), 6u);
  grx_pattern_free(parsed);

  EXPECT_TRUE(matches_java("[ab[cd]]", "c"));
  EXPECT_FALSE(matches_java("[ab[cd]]", "e"));
}

TEST(JavaParser, ThePosixClassesAreAsciiUntilTheUnicodeFlag) {
  // `java.util.regex.Pattern` documents these as US-ASCII, and maps every
  // one of them to its Unicode equivalent under UNICODE_CHARACTER_CLASS.
  //
  // Three of them used to resolve as Unicode *properties* instead - loose
  // matching found a property of the same name - so they compiled and then
  // matched where the JDK does not. A wrong answer rather than a refusal,
  // which is why neither the committed corpus nor a compile-only syntax
  // differential saw it: both ask only whether the pattern is accepted.
  // These three rows are that regression.
  struct Row { const char * klass; const char * subject; };
  static const Row rows[] = {
    {"Digit", "\u0660"},   // ARABIC-INDIC DIGIT ZERO, Nd but not [0-9]
    {"Punct", "\u2010"},   // HYPHEN, P but not ASCII punctuation
    {"Cntrl", "\u0085"},   // NEL, Cc but not [\x00-\x1F\x7F]
    {"Lower", "\u00e9"},
    {"Upper", "\u00c9"},
    {"Alpha", "\u00e9"},
    {"Alnum", "\u00e9"},
    {"Space", "\u00a0"},
    {"XDigit", "\uff10"},
    {"Graph", "\u00e9"},
    {"Print", "\u00e9"},
    {"Blank", "\u3000"},
  };
  for (const Row & row : rows) {
    const std::string pattern = std::string("\\p{") + row.klass + "}";
    EXPECT_FALSE(matches_java(pattern.c_str(), row.subject))
        << row.klass << " is US-ASCII without the U flag";
    EXPECT_TRUE(matches_java(pattern.c_str(), row.subject, GRX_OPT_UCP))
        << row.klass << " is its Unicode equivalent under U";
    // The ASCII member is in both.
    EXPECT_TRUE(matches_java(pattern.c_str(), "0")
                || matches_java(pattern.c_str(), "a")
                || matches_java(pattern.c_str(), "A")
                || matches_java(pattern.c_str(), "!")
                || matches_java(pattern.c_str(), " ")
                || matches_java(pattern.c_str(), "\x01"))
        << row.klass << " matches nothing in ASCII, so it is not that class";
  }
}

TEST(JavaParser, TheTwoPosixClassesWithNoPosixSpelling) {
  // `L1` is Latin-1 and `all` is every code point, and neither moves when
  // UNICODE_CHARACTER_CLASS is on, because neither is a narrowed form of
  // something wider.
  const uint32_t both[] = {(uint32_t)GRX_OPT_NONE, (uint32_t)GRX_OPT_UCP};
  for (uint32_t options : both) {
    EXPECT_TRUE(matches_java("\\p{L1}", "\u00ff", options));
    EXPECT_FALSE(matches_java("\\p{L1}", "\u0100", options));
    EXPECT_TRUE(matches_java("\\p{all}", "\u0100", options));
    EXPECT_TRUE(matches_java("\\p{all}", "a", options));
  }
  // And the two names did not become POSIX bracket classes anywhere. They
  // are in the lowering set builder, which is shared, and the only thing
  // keeping `[[:l1:]]` from resolving is that the parsers' own name lists
  // were left alone. Asked in Perl, which has POSIX classes - Java has no
  // `[[:name:]]` syntax at all, so asking there would prove nothing.
  GRX_Error error;
  GRX_Regex * regex = nullptr;
  for (const char * spelling : {"[[:l1:]]", "[[:all:]]"}) {
    EXPECT_EQ(grx_regex_compile_with_allocator(spelling, std::strlen(spelling),
                  GRX_SYNTAX_PERL, GRX_OPT_NONE, nullptr, nullptr, &error,
                  &regex),
        GRX_ERR_SYNTAX)
        << spelling << " resolved as a POSIX class";
    grx_regex_free(regex);
    regex = nullptr;
  }
}

TEST(JavaParser, TheExactSpellingIsTheOnlyOneAccepted) {
  // The JDK takes `\p{Alpha}` and refuses `\p{alpha}`; loose property
  // matching would have taken both, which is the other half of routing
  // these away from the property table.
  GRX_Error error;
  EXPECT_EQ(compile_java("\\p{Alpha}", &error), GRX_OK) << error.message;
  EXPECT_EQ(compile_java("\\p{alpha}", &error), GRX_ERR_SYNTAX);
  EXPECT_EQ(compile_java("\\p{ASCII}", &error), GRX_OK) << error.message;
  EXPECT_EQ(compile_java("\\p{ascii}", &error), GRX_ERR_SYNTAX);
}

TEST(JavaParser, ACaretInsidePropertyBracesIsASyntaxError) {
  // Perl and PCRE2 read it as the negation; the JDK refuses it and wants
  // `\P{Name}`. Measured on JDK 25 - every one of these compiled here
  // before, and the JDK rejects all six.
  GRX_Error error;
  for (const char * pattern : {"\\p{^L}", "\\P{^L}", "\\p{^InGreek}",
                               "\\P{^InGreek}", "[\\p{^InGreek}]",
                               "[\\P{^L}]"}) {
    EXPECT_EQ(compile_java(pattern, &error), GRX_ERR_SYNTAX) << pattern;
  }
  // The spelling the JDK does want still works, and still negates.
  EXPECT_TRUE(matches_java("\\P{L}", "1"));
  EXPECT_FALSE(matches_java("\\P{L}", "a"));
}

// U+D800 as WTF-8, and U+10061 - the scalar a D800/DC61 pair means - as the
// four-byte UTF-8 every encoding agrees on.
static const char kLoneHigh[] = "\xED\xA0\x80";
static const char kLoneLow[] = "\xED\xB1\xA1";
static const char kCesu8Pair[] = "\xED\xA0\x80\xED\xB1\xA1";
static const char kScalarPair[] = "\xF0\x90\x81\xA1";

/** Search `subject` for `pattern` under the given options. */
static GRX_Result search_java(const char * pattern, size_t pattern_length,
    const char * subject, size_t subject_length, uint32_t options,
    int * out_matched, GRX_Capture * out_span,
    GRX_Syntax syntax = GRX_SYNTAX_JAVA) {
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  GRX_Result result = grx_regex_compile_with_allocator(pattern,
      pattern_length, syntax, options, nullptr, nullptr, &error, &regex);
  if (result != GRX_OK) {
    return result;
  }
  GRX_Match * match = nullptr;
  if (grx_match_create(regex, nullptr, &match) != GRX_OK) {
    grx_regex_free(regex);
    return GRX_ERR_INTERNAL;
  }
  *out_matched = 0;
  result = grx_regex_search(regex, subject, subject_length, 0,
      GRX_ENGINE_AUTO, nullptr, match, out_matched);
  if (result == GRX_OK && *out_matched && out_span) {
    grx_match_span(match, out_span);
  }
  grx_match_destroy(match);
  grx_regex_free(regex);
  return result;
}

TEST(JavaSurrogates, WithTheOptionTheReferenceAnswersAreMatched) {
  // Every row is a pinned JDK 25 answer from tests/data/probe/java25.json's
  // `surrogate` area, which is where these came from rather than from
  // reasoning about what UTF-16 ought to do.
  struct Row {
    const char * pattern;
    const char * subject;
    size_t subject_length;
    int matched;
    size_t start;
    size_t end;
  };
  static const Row rows[] = {
    // `.` takes an unpaired surrogate as one position of three bytes, and a
    // pair as one of four.
    {".", kLoneHigh, 3, 1, 0, 3},
    {".", kLoneLow, 3, 1, 0, 3},
    {".", kScalarPair, 4, 1, 0, 4},
    // Surrogates are general category Cs, and are not unassigned.
    {"\\p{Cs}", kLoneHigh, 3, 1, 0, 3},
    {"\\p{Cn}", kLoneHigh, 3, 0, 0, 0},
    // Not word characters, and the complement says so.
    {"\\w", kLoneHigh, 3, 0, 0, 0},
    {"\\W", kLoneHigh, 3, 1, 0, 3},
    {"\\p{L}", kLoneHigh, 3, 0, 0, 0},
    // A negated class, and a range spanning the block, include them.
    {"[^a]", kLoneHigh, 3, 1, 0, 3},
    {"[\\uD7FF-\\uE000]", kLoneHigh, 3, 1, 0, 3},
    // And a pattern may name one, bare or in a class.
    {"\\uD800", kLoneHigh, 3, 1, 0, 3},
    {"[\\uD800]", kLoneHigh, 3, 1, 0, 3},
  };
  const uint32_t options = GRX_OPT_UTF | GRX_OPT_WTF8;
  for (const Row & row : rows) {
    int matched = 0;
    GRX_Capture span = {GRX_NPOS, GRX_NPOS};
    ASSERT_EQ(search_java(row.pattern, std::strlen(row.pattern), row.subject,
                  row.subject_length, options, &matched, &span),
        GRX_OK)
        << row.pattern;
    EXPECT_EQ(matched, row.matched) << row.pattern;
    if (row.matched) {
      EXPECT_EQ(span.start, row.start) << row.pattern;
      EXPECT_EQ(span.end, row.end) << row.pattern;
    }
  }
}

TEST(JavaSurrogates, WithoutTheOptionNothingChanges) {
  // The whole point of an option: today's behaviour is what happens when
  // nobody asks. A refusal rather than a wrong answer, by name.
  int matched = 0;
  EXPECT_EQ(search_java(".", 1, kLoneHigh, 3, GRX_OPT_UTF, &matched, nullptr),
      GRX_ERR_INVALID);
  GRX_Error error;
  EXPECT_EQ(compile_java(kLoneHigh, &error), GRX_ERR_SYNTAX);
  EXPECT_EQ(error.diag, GRX_DIAG_INVALID_UTF8_IN_PATTERN);
}

TEST(JavaSurrogates, ACesu8PairIsStillRefused) {
  // WTF-8 wants a pair as the four-byte scalar. The two-sequence spelling is
  // CESU-8 and taking it would make `.` match half a character while every
  // offset drifted from the reference - so accepting WTF-8 is *more*
  // validation than strict UTF-8, not less.
  int matched = 0;
  const uint32_t options = GRX_OPT_UTF | GRX_OPT_WTF8;
  EXPECT_EQ(search_java(".", 1, kCesu8Pair, 6, options, &matched, nullptr),
      GRX_ERR_INVALID);
  // The correctly spelled pair is fine, and is one position.
  GRX_Capture span = {GRX_NPOS, GRX_NPOS};
  ASSERT_EQ(search_java(".", 1, kScalarPair, 4, options, &matched, &span),
      GRX_OK);
  EXPECT_EQ(matched, 1);
  EXPECT_EQ(span.end, 4u);
}

TEST(JavaSurrogates, ADialectWhoseReferenceHasNoneRefusesTheOption) {
  // Asking for it where the reference has no concept of an unpaired
  // surrogate is GRX_ERR_UNSUPPORTED, not a bit that quietly does nothing.
  int matched = 0;
  for (GRX_Syntax syntax : {GRX_SYNTAX_POSIX_ERE, GRX_SYNTAX_PYTHON}) {
    EXPECT_EQ(search_java(".", 1, kLoneHigh, 3, GRX_OPT_UTF | GRX_OPT_WTF8,
                  &matched, nullptr, syntax),
        GRX_ERR_UNSUPPORTED);
  }
  // And Java accepts it, which is what makes the refusal a statement about
  // the dialect rather than about the option.
  GRX_Capture span = {GRX_NPOS, GRX_NPOS};
  EXPECT_EQ(search_java(".", 1, kLoneHigh, 3, GRX_OPT_UTF | GRX_OPT_WTF8,
                &matched, &span),
      GRX_OK);
}

TEST(JavaSurrogates, SubstitutionCarriesTheOptionThrough) {
  // Substitution reads the option from the match's program rather than from
  // a caller, so this is what says the path is connected.
  //
  // Through the *subject*, deliberately. A first version of this test
  // expected `\x{D800}` in a replacement to emit three bytes and got seven:
  // Java's replacement grammar has no codepoint escape, so `\x` quotes `x`
  // and the rest is literal. GRX_TPL_CODEPOINT comes from vim's `\n`, `\r`,
  // `\t` and `\b` and from the control-escape path, none of which can name
  // a surrogate - so the encoder's flag in CaseState is defensive for the
  // dialects that have those escapes, and unreachable from this one. Worth
  // knowing rather than asserting something that cannot happen.
  const uint32_t options = GRX_OPT_UTF | GRX_OPT_WTF8;
  GRX_Error error;
  GRX_Regex * dash = nullptr;
  ASSERT_EQ(grx_regex_compile_with_allocator("-", 1, GRX_SYNTAX_JAVA, options,
                nullptr, nullptr, &error, &dash),
      GRX_OK)
      << error.message;

  const std::string lone(kLoneHigh, 3);
  const std::string subject = lone + "-" + lone;

  GRX_Text text;
  memset(&text, 0, sizeof(text));
  ASSERT_EQ(grx_regex_replace(dash, subject.data(), subject.size(), "=", 1,
                GRX_REPLACE_GLOBAL, nullptr, nullptr, &error, &text),
      GRX_OK)
      << error.message;
  EXPECT_EQ(std::string(text.data, text.length), lone + "=" + lone);
  grx_text_free(&text);

  // A group reference carries one out of the subject untouched.
  GRX_Regex * group = nullptr;
  ASSERT_EQ(grx_regex_compile_with_allocator("(.)-", 4, GRX_SYNTAX_JAVA,
                options, nullptr, nullptr, &error, &group),
      GRX_OK)
      << error.message;
  memset(&text, 0, sizeof(text));
  ASSERT_EQ(grx_regex_replace(group, subject.data(), subject.size(), "[$1]", 4,
                GRX_REPLACE_GLOBAL, nullptr, nullptr, &error, &text),
      GRX_OK)
      << error.message;
  EXPECT_EQ(std::string(text.data, text.length), "[" + lone + "]" + lone);
  grx_text_free(&text);
  grx_regex_free(group);

  // And splitting gives two fields, each the three bytes of a surrogate.
  GRX_Split split;
  memset(&split, 0, sizeof(split));
  ASSERT_EQ(grx_regex_split_java(dash, subject.data(), subject.size(), -1,
                nullptr, nullptr, &error, &split),
      GRX_OK)
      << error.message;
  ASSERT_EQ(split.count, 2u);
  EXPECT_EQ(split.pieces[0].end - split.pieces[0].start, 3u);
  EXPECT_EQ(split.pieces[1].end - split.pieces[1].start, 3u);
  grx_split_free(&split);
  grx_regex_free(dash);
}

TEST(JavaSurrogates, WithoutTheOptionSubstitutionStillRefuses) {
  // The other half: a caller who does not ask sees exactly what they saw
  // before, from substitution as much as from matching.
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  ASSERT_EQ(grx_regex_compile_with_allocator("-", 1, GRX_SYNTAX_JAVA,
                GRX_OPT_UTF, nullptr, nullptr, &error, &regex),
      GRX_OK);
  const std::string subject = std::string(kLoneHigh, 3) + "-";

  GRX_Text text;
  memset(&text, 0, sizeof(text));
  EXPECT_EQ(grx_regex_replace(regex, subject.data(), subject.size(), "=", 1,
                GRX_REPLACE_GLOBAL, nullptr, nullptr, &error, &text),
      GRX_ERR_INVALID);

  GRX_Split split;
  memset(&split, 0, sizeof(split));
  EXPECT_EQ(grx_regex_split_java(regex, subject.data(), subject.size(), -1,
                nullptr, nullptr, &error, &split),
      GRX_ERR_INVALID);
  grx_regex_free(regex);
}

TEST(JavaParser, AnUnpairedSurrogateIsRefusedRatherThanAnswered) {
  // The JDK answers these: a lone surrogate is an ordinary `char` to
  // `Matcher`, and of the 101 cases in the JDK's own corpus that carry one,
  // 62 match there and 39 do not - none is an error. This dialect refuses
  // them instead, on both sides, because a lone surrogate is not a Unicode
  // scalar value and has no well-formed UTF-8 encoding.
  //
  // Pinned here because it is a documented deviation in dialects.md section
  // 6 and a documented deviation nobody tests is a sentence. What must not
  // happen is the third possibility: taking the WTF-8 bytes as opaque and
  // reporting a span the JDK would not. `pcre` does match them bytewise,
  // which is why this is a property of this dialect rather than of the API.
  const char wtf8_d800[] = "\xED\xA0\x80";          // U+D800 in WTF-8
  GRX_Error error;

  // In the pattern: refused by name, not matched and not accepted.
  EXPECT_EQ(compile_java(wtf8_d800, &error), GRX_ERR_SYNTAX);
  EXPECT_EQ(error.diag, GRX_DIAG_INVALID_UTF8_IN_PATTERN);

  // In the subject: refused, rather than scanned as three bytes.
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile_with_allocator(".", 1, GRX_SYNTAX_JAVA,
                GRX_OPT_NONE, nullptr, nullptr, &error, &regex),
      GRX_OK)
      << error.message;
  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
  int matched = 0;
  EXPECT_EQ(grx_regex_search(regex, wtf8_d800, sizeof wtf8_d800 - 1, 0,
                GRX_ENGINE_AUTO, nullptr, match, &matched),
      GRX_ERR_INVALID);
  grx_match_destroy(match);
  grx_regex_free(regex);

  // A surrogate *pair* is a scalar, encodes, and is handled normally: this
  // is U+10061, which the corpus's supplementary cases are full of.
  EXPECT_TRUE(matches_java("\\x{10061}", "\U00010061"));
  EXPECT_TRUE(matches_java(".", "\U00010061"));
}

TEST(JavaParser, ReportsUtf8ByteOffsets) {
  GRX_Error error;
  EXPECT_EQ(compile_java("\xF0\x9F\x98\x80(", &error), GRX_ERR_SYNTAX);
  EXPECT_EQ(error.offset, 4u);
}

TEST(JavaFlags, ExternalAndScopedCaseWidth) {
  uint32_t i = 0, iu = 0, iU = 0, U = 0;
  ASSERT_EQ(grx_options_parse(GRX_SYNTAX_JAVA, "i", &i, nullptr), GRX_OK);
  ASSERT_EQ(grx_options_parse(GRX_SYNTAX_JAVA, "iu", &iu, nullptr), GRX_OK);
  ASSERT_EQ(grx_options_parse(GRX_SYNTAX_JAVA, "iU", &iU, nullptr), GRX_OK);
  ASSERT_EQ(grx_options_parse(GRX_SYNTAX_JAVA, "U", &U, nullptr), GRX_OK);
  EXPECT_FALSE(matches_java("k", "\xE2\x84\xAA", i));
  EXPECT_TRUE(matches_java("k", "\xE2\x84\xAA", iu));
  EXPECT_TRUE(matches_java("k", "\xE2\x84\xAA", iU));
  EXPECT_FALSE(matches_java("k", "\xE2\x84\xAA", U));
  EXPECT_FALSE(matches_java("\\d", "\xD9\xA1"));
  EXPECT_TRUE(matches_java("\\d", "\xD9\xA1", U));
  EXPECT_FALSE(matches_java("\\w", "\xF0\x90\x90\xA8"));
  EXPECT_TRUE(matches_java("\\w", "\xF0\x90\x90\xA8", U));
  EXPECT_FALSE(matches_java("[\\w]", "\xC3\xA9"));
  EXPECT_TRUE(matches_java("[\\w]", "\xC3\xA9", U));
  EXPECT_FALSE(matches_java("\xF0\x90\x90\x80", "\xF0\x90\x90\xA8", i));
  EXPECT_TRUE(matches_java("\xF0\x90\x90\x80", "\xF0\x90\x90\xA8", iu));
  EXPECT_FALSE(matches_java("\\b\xC3\xA9", "\xC3\xA9"));
  EXPECT_TRUE(matches_java("\\B\xC3\xA9", "\xC3\xA9"));
  EXPECT_TRUE(matches_java("\\b\xC3\xA9", "\xC3\xA9", U));
  EXPECT_FALSE(matches_java("\\B\xC3\xA9", "\xC3\xA9", U));
  EXPECT_FALSE(matches_java("(?-u:k)", "\xE2\x84\xAA", iu));
  EXPECT_TRUE(matches_java("(?u:k)", "\xE2\x84\xAA", i));
}

TEST(JavaFlags, UnixLinesAndVerbose) {
  uint32_t d = 0, m = 0, x = 0;
  ASSERT_EQ(grx_options_parse(GRX_SYNTAX_JAVA, "d", &d, nullptr), GRX_OK);
  ASSERT_EQ(grx_options_parse(GRX_SYNTAX_JAVA, "m", &m, nullptr), GRX_OK);
  ASSERT_EQ(grx_options_parse(GRX_SYNTAX_JAVA, "x", &x, nullptr), GRX_OK);
  EXPECT_FALSE(matches_java("a.b", "a\rb"));
  EXPECT_TRUE(matches_java("a.b", "a\rb", d));
  EXPECT_TRUE(matches_java("(?d:a.b)", "a\rb"));
  EXPECT_FALSE(matches_java("(?-d:a.b)", "a\rb", d));
  EXPECT_TRUE(matches_java("^b", "a\rb", m));
  EXPECT_FALSE(matches_java("^b", "a\rb", m | d));
  EXPECT_FALSE(matches_java("^\\n", "\r\n", m));
  EXPECT_TRUE(matches_java("(?d:a.\\nb)", "a\r\nb"));
  EXPECT_FALSE(matches_java("(?-d:a.\\nb)", "a\r\nb", d));
  EXPECT_TRUE(matches_java("a b", "ab", x));
  EXPECT_TRUE(matches_java("[a b]", "b", x));
}

TEST(JavaProperties, CharacterWhitespaceDiffersFromUnicodeWhiteSpace) {
  const char * property = "\\p{javaWhitespace}";
  for (const char * subject : {"\t", "\r", "\x1C", " ",
           "\xE1\x9A\x80", "\xE2\x80\x80", "\xE2\x80\x86",
           "\xE2\x80\x88", "\xE2\x80\x8A", "\xE2\x80\xA8",
           "\xE2\x80\xA9", "\xE2\x81\x9F", "\xE3\x80\x80"}) {
    EXPECT_TRUE(matches_java(property, subject)) << subject;
  }
  for (const char * subject : {"\x1B", "!", "\xC2\x85", "\xC2\xA0",
           "\xE1\x99\xBF", "\xE1\x9A\x81", "\xE1\xBF\xBF",
           "\xE2\x80\x87", "\xE2\x80\x8B", "\xE2\x80\xA7",
           "\xE2\x80\xAA", "\xE2\x80\xAF", "\xE2\x81\x9E",
           "\xE2\x81\xA0", "\xE2\xBF\xBF", "\xE3\x80\x81"}) {
    EXPECT_FALSE(matches_java(property, subject)) << subject;
  }
  EXPECT_FALSE(matches_java("\\p{IsGreek}", "\xCD\xB4"));
  EXPECT_TRUE(matches_java("\\p{IsGreek}", "\xE1\xBC\x80"));
  EXPECT_FALSE(matches_java("\\p{sc=Greek}", "\xCD\xB4"));
  EXPECT_TRUE(matches_java("\\p{sc=Greek}", "\xE1\xBC\x80"));
  EXPECT_TRUE(matches_java("\\p{InGreek}", "\xCD\xB4"));
  EXPECT_TRUE(matches_java("\\p{javaLowerCase}", "\xCA\x95"));
  EXPECT_TRUE(matches_java("\\p{javaLowerCase}", "\xF0\x90\x90\xA8"));
  EXPECT_TRUE(matches_java("\\p{javaDigit}", "\xF0\x9D\x9F\x8E"));
}

TEST(JavaSearch, EmptyProgressAndPreviousEndAnchor) {
  EXPECT_EQ(search_all_java("", "ab", 2), "0:0 1:1 2:2");
  EXPECT_EQ(search_all_java("a*", "baac", 4),
      "0:0 1:3 3:3 4:4");
  EXPECT_EQ(search_all_java("\\Ga*", "baac", 4), "0:0");
  EXPECT_EQ(search_all_java("\\Ga*", "aab", 3), "0:2 2:2");
  EXPECT_EQ(search_all_java("(?:\\Ga|b)", "cba", 3), "1:2 2:3");
  EXPECT_EQ(search_all_java("\\Ga*", "cbaa", 4, 1), "1:1");
}

TEST(JavaSearch, RetainsCapturesAndChoosesNearestLookbehindStart) {
  EXPECT_EQ(search_all_java("(a*)*", "bc", 2),
      "0:0,0:0 1:1,1:1 2:2,2:2");
  EXPECT_EQ(search_all_java("((a)|b)+", "ab", 2),
      "0:2,1:2,0:1");
  EXPECT_EQ(search_all_java("(?<=(a+))b", "aaabaaab", 8),
      "3:4,2:3 7:8,6:7");
  EXPECT_EQ(search_all_java("(?=(a))a|(b)", "ab", 2),
      "0:1,0:1,- 1:2,-,1:2");
  EXPECT_EQ(search_all_java("(?<=a(?=(b)))b", "abab", 4),
      "1:2,1:2 3:4,3:4");
  EXPECT_EQ(search_all_java("(?<!a{2,})b", "ab aab aaab b", 13),
      "1:2 12:13");
  const char * subject = "\xF0\x9F\x98\x80\xC3\xA9";
  EXPECT_EQ(search_all_java(".", subject, 6), "0:4 4:6");
}

TEST(JavaSearch, AnchoredAndRepresentableRegionOperations) {
  EXPECT_EQ(anchored_java("(?=a)(a*)", "a", 1, 0, 1, true),
      "match 0:1 0:1");
  EXPECT_EQ(anchored_java("(?:)", "", 0, 0, 0, true), "match 0:0");
  EXPECT_EQ(anchored_java("(?=a)a", "a", 1, 0, 1, true), "match 0:1");
  EXPECT_EQ(anchored_java("(?=a)(a*)", "ab", 2, 0, 2, false),
      "match 0:1 0:1");
  EXPECT_EQ(anchored_java("(?=a)", "abc", 3, 0, 3, false),
      "match 0:0");
  EXPECT_EQ(anchored_java("(?<=a)b", "ab", 2, 0, 2, false), "nomatch");
  EXPECT_EQ(anchored_java("a", "ba", 2, 1, 2, true), "match 1:2");
  EXPECT_EQ(anchored_java("a", "ba", 2, 1, 2, false), "match 1:2");
}

TEST(JavaSearch, UnboundedLookbehindHonorsStepLimit) {
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  const char * pattern = "(?<=(a+))b";
  ASSERT_EQ(grx_regex_compile_with_allocator(pattern, std::strlen(pattern),
                GRX_SYNTAX_JAVA, GRX_OPT_NONE, nullptr, nullptr, &error,
                &regex), GRX_OK);
  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_steps = 1;
  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.engine = GRX_ENGINE_BACKTRACK;
  options.limits = &limits;
  int matched = 0;
  EXPECT_EQ(grx_regex_search_ex(regex, "ab", 2, &options, match, &matched),
      GRX_ERR_LIMIT);
  EXPECT_GT(grx_match_steps(match), 0u);
  grx_match_destroy(match);
  grx_regex_free(regex);
}

TEST(JavaSearch, CostlyLookbehindRemainsBounded) {
  const char * pattern = "(?<=((a+)+)b)b";
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  ASSERT_EQ(grx_regex_compile_with_allocator(pattern, std::strlen(pattern),
                GRX_SYNTAX_JAVA, GRX_OPT_NONE, nullptr, nullptr, &error,
                &regex), GRX_OK);
  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_steps = 500;
  GRX_SearchOptions options;
  grx_search_options_default(&options);
  options.engine = GRX_ENGINE_BACKTRACK;
  options.limits = &limits;
  std::string subject(30, 'a');
  subject += 'b';
  int matched = 0;
  EXPECT_EQ(grx_regex_search_ex(regex, subject.data(), subject.size(),
                &options, match, &matched), GRX_ERR_LIMIT);
  EXPECT_GT(grx_match_steps(match), 0u);
  grx_match_destroy(match);
  grx_regex_free(regex);
}

TEST(JavaSearch, SearchWindowKeepsPhysicalAnchorsAndVisibleLookbehind) {
  struct Row {
    const char * pattern;
    int expected;
  };
  for (const Row & row : {
           Row {"a$", 1}, Row {"^a", 0}, Row {"\\Aa", 0},
           Row {"(?<=b)a", 1}}) {
    GRX_Regex * regex = nullptr;
    ASSERT_EQ(grx_regex_compile(row.pattern, GRX_SYNTAX_JAVA, 0, &regex),
        GRX_OK) << row.pattern;
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
    GRX_SearchOptions options;
    grx_search_options_default(&options);
    options.begin = 1;
    options.end = 2;
    int matched = 0;
    EXPECT_EQ(grx_regex_search_ex(regex, "baX", 3, &options, match, &matched),
        GRX_OK) << row.pattern;
    EXPECT_EQ(matched, row.expected) << row.pattern;
    if (matched) {
      GRX_Capture span;
      ASSERT_EQ(grx_match_span(match, &span), GRX_OK);
      EXPECT_EQ(span.start, 1u);
      EXPECT_EQ(span.end, 2u);
    }
    grx_match_destroy(match);
    grx_regex_free(regex);
  }
}

TEST(JavaBraces, EveryUnescapedBraceBeginsACountedClosure) {
  // Java's rule, which is one rule where this family has two. Everywhere
  // else here a `{` that spells no quantifier is an ordinary character -
  // perl, pcre2 and CPython all match the text "a{foo}" with `/a{foo}/` -
  // and the parser says so by rewinding and reading the brace as a literal.
  // The JDK has no such fallback, and these are its own answers from
  // tests/data/probe/java25.json's `brace` area.
  //
  // A brace is a literal there only where it cannot be an operator at all:
  // inside a class, behind a backslash, or inside `\Q...\E`.
  struct Row { const char * pattern; int compiles; };
  const Row rows[] = {
    {"a{^InGreek}", 0},    // the three rows of the JDK's own corpus that
    {"a{foo}", 0},         // used to be excluded by name were this one
    {"a{", 0},
    {"a{1", 0},            // "Unclosed counted closure" there
    {"a{1,", 0},
    {"a{,3}", 0},
    {"a{}", 0},
    {"a{ 1}", 0},          // no space inside a closure, where perl allows it
    {"a{1,2,3}", 0},
    {"a{-1}", 0},
    {"{foo}", 0},          // and with nothing before it either
    {"{", 0},
    {"{}", 0},
    {"{,3}", 0},
    {"a{2,1}", 0},         // "Illegal repetition range"
    {"a**", 0},            // "Dangling meta character '*'", still an error
    {"a{3}*", 0},          // and so is a `*` after a closure
    {"a{1}", 1},
    {"a{1,}", 1},
    {"a{1,2}", 1},
    {"a{3}+", 1},          // the possessive suffix, not a second quantifier
    {"a{3}?", 1},          // and the lazy one
    {"[{3}]", 1},          // a literal brace: in a class,
    {"\\{3}", 1},          // behind a backslash,
    {"[a{]", 1},
    {"a\\{foo}", 1},
    {"\\Q{3}\\E", 1},      // and inside a quoted run
  };
  for (const Row & row : rows) {
    GRX_Regex * regex = nullptr;
    GRX_Error error;
    GRX_Result result = grx_regex_compile_with_allocator(row.pattern,
        strlen(row.pattern), GRX_SYNTAX_JAVA, GRX_OPT_UTF, nullptr, nullptr,
        &error, &regex);
    EXPECT_EQ(result == GRX_OK, row.compiles != 0)
        << row.pattern << ": " << error.message;
    grx_regex_free(regex);
  }

  // The same patterns the other way round: every one of the refusals above
  // is a *pattern* in perl, which is what makes this a dialect rule rather
  // than a grammar this library tightened for everyone.
  for (const char * pattern : {"a{^InGreek}", "a{foo}", "a{", "a{1",
           "a{1,", "a{1,2,3}", "a{-1}", "{foo}", "{", "{}"}) {
    GRX_Regex * regex = nullptr;
    EXPECT_EQ(grx_regex_compile_with_allocator(pattern, strlen(pattern),
                  GRX_SYNTAX_PERL, GRX_OPT_UTF, nullptr, nullptr, nullptr,
                  &regex),
        GRX_OK)
        << pattern << " is a literal brace in perl";
    grx_regex_free(regex);
  }
}

TEST(JavaBraces, AClosureWithNothingToRepeatRepeatsNothing) {
  // The other half of the rule, and the half a verdict cannot check: the
  // JDK accepts a closure with no atom to its left and *drops* it. `{3}`
  // matches the empty string, `a{3}{4}` matches three rather than twelve,
  // and `x{2}{3}` matches two rather than six - so a parser that nested the
  // second closure would compile every one of these and answer the wrong
  // span. The subjects are long enough for either reading to show.
  //
  // Implemented as a repeat of an *empty node* rather than as a rule that
  // imitates the reference: the brace is left unread, the parser reads the
  // quantifier it is, and a repeat of nothing is nothing. The second
  // closure on an atom that already has one is dropped by
  // GRX_SyntaxSpec::brace_quantifier_after_quantifier_ignored, because
  // there nesting would be observable.
  struct Row {
    const char * pattern;
    const char * subject;
    int matched;
    size_t start;
    size_t end;
  };
  const Row rows[] = {
    {"{3}", "aaa", 1, 0, 0},
    {"{3}", "", 1, 0, 0},
    {"{3}a", "aaa", 1, 0, 1},
    {"{3}{4}", "aaa", 1, 0, 0},
    {"a{3}{4}", "aaaaaaaaaaaa", 1, 0, 3},     // three, not twelve
    {"x{2}{3}", "xxxxxx", 1, 0, 2},           // two, not six
    {"a{3}{4}{5}", "aaaaaaaaaaaa", 1, 0, 3},
    {"a{1,2}{3}", "aaa", 1, 0, 2},
    {"a*{3}", "aaaa", 1, 0, 4},
    {"a+{3}", "aaaa", 1, 0, 4},
    {"a?{3}", "aaaa", 1, 0, 1},               // one, not three
    {"^{3}", "aaa", 1, 0, 0},
    {"a${3}", "aaa", 1, 2, 3},
    {"(?=a){3}", "aaa", 1, 0, 0},
    {"\\b{3}", "aaa", 1, 0, 0},               // `\b{3}` is a closure on the
    {"\\B{3}", "aaa", 1, 1, 1},               // boundary, not a bound name
    {"\\A{3}", "aaa", 1, 0, 0},
    {"(){3}", "", 1, 0, 0},
    {"(?:{3})", "aaa", 1, 0, 0},
    {"a|{3}", "aaa", 1, 0, 1},
    {"\\{3}", "{3}", 1, 0, 3},                // and the literal still is one
    {"[{3}]", "{", 1, 0, 1},
  };
  for (const Row & row : rows) {
    int matched = 0;
    GRX_Capture span = {GRX_NPOS, GRX_NPOS};
    ASSERT_EQ(search_java(row.pattern, strlen(row.pattern), row.subject,
                  strlen(row.subject), GRX_OPT_UTF, &matched, &span),
        GRX_OK)
        << row.pattern;
    EXPECT_EQ(matched != 0, row.matched != 0) << row.pattern;
    if (matched && row.matched) {
      EXPECT_EQ(span.start, row.start) << row.pattern << " start";
      EXPECT_EQ(span.end, row.end) << row.pattern << " end";
    }
  }

  // `\b{g}` is still the grapheme bound, which is the name the quantifier
  // rule must not swallow: it is a brace that cannot be a count.
  int matched = 0;
  GRX_Capture span = {GRX_NPOS, GRX_NPOS};
  ASSERT_EQ(search_java("\\b{g}", 5, "ab", 2, GRX_OPT_UTF, &matched, &span),
      GRX_OK);
  EXPECT_TRUE(matched);
  // And an unknown bound name is still an error rather than a closure.
  GRX_Regex * regex = nullptr;
  EXPECT_NE(grx_regex_compile_with_allocator("\\b{nosuch}", 10,
                GRX_SYNTAX_JAVA, GRX_OPT_UTF, nullptr, nullptr, nullptr,
                &regex),
      GRX_OK);
  grx_regex_free(regex);
}

TEST(JavaBounds, AnEmptySubjectHasAGraphemeBoundaryHereAndNotInPerl) {
  // UAX #29 breaks at the start of text and at the end of text, and says
  // nothing about which applies when they are the same offset. So an empty
  // subject is a corner two conforming implementations can split on, and
  // they do: `\b{g}` against "" is 0-0 on JDK 25 and no match in perl 5.44,
  // both measured rather than reasoned about.
  //
  // grx_unicode_break_at() answers perl's way and says so in its published
  // contract, which is why the dialect that differs carries a program flag
  // rather than the segmentation routine growing a parameter. Only `\b{g}`
  // can observe it in Java: the bound-type alphabet there is the single
  // name `g`.
  struct Row {
    const char * pattern;
    const char * subject;
    int java_matches;
    int perl_matches;
  };
  const Row rows[] = {
    {"\\b{g}", "", 1, 0},          // the one row the two references split on
    {"^\\b{g}$", "", 1, 0},
    {"\\b{g}\\b{g}", "", 1, 0},    // twice over, still the one position
    {"\\b{g}", "a", 1, 1},         // and everywhere else they agree
    {"\\b{g}", "ab", 1, 1},
    {"\\b", "", 0, 0},             // the word boundary has none either way
    {"\\B", "", 1, 1},
  };
  for (const Row & row : rows) {
    for (GRX_Syntax syntax : {GRX_SYNTAX_JAVA, GRX_SYNTAX_PERL}) {
      const int want = syntax == GRX_SYNTAX_JAVA ? row.java_matches
                                                 : row.perl_matches;
      // Every engine, because the flag is read in two of them and a
      // dialect axis that only one honours is worse than none.
      for (GRX_Engine engine : {GRX_ENGINE_AUTO, GRX_ENGINE_PIKE,
               GRX_ENGINE_BACKTRACK, GRX_ENGINE_BITSTATE}) {
        GRX_Regex * regex = nullptr;
        GRX_Error error;
        ASSERT_EQ(grx_regex_compile_with_allocator(row.pattern,
                      strlen(row.pattern), syntax, GRX_OPT_UTF, nullptr,
                      nullptr, &error, &regex),
            GRX_OK)
            << row.pattern;
        GRX_Match * match = nullptr;
        ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);
        int matched = 0;
        const GRX_Result result = grx_regex_search(regex, row.subject,
            strlen(row.subject), 0, engine, nullptr, match, &matched);
        if (result == GRX_OK) {
          EXPECT_EQ(matched != 0, want != 0)
              << row.pattern << " in "
              << (syntax == GRX_SYNTAX_JAVA ? "java" : "perl")
              << " on engine " << (int)engine;
          if (matched && want) {
            GRX_Capture span = {GRX_NPOS, GRX_NPOS};
            grx_match_span(match, &span);
            EXPECT_EQ(span.start, span.end) << row.pattern;
          }
        }
        grx_match_destroy(match);
        grx_regex_free(regex);
      }
    }
  }
}

TEST(JavaBounds, ThereIsNoNegatedBoundAndTheMessageSaysWhy) {
  // `\B{g}` is "Illegal repetition" on JDK 25, not "unknown bound type" -
  // and which message it is settles which reader saw the brace. `\B` is the
  // plain non-boundary and the `{g}` after it went to the counted-closure
  // reader, which refuses it. So this library leaves the brace where it is
  // and lets the same reader give the same answer, rather than naming a
  // construct the reference does not have.
  //
  // `\B{3}` is the other half: a closure after `\B` compiles there, so the
  // refusal is about the *name* and not about the brace.
  for (const char * pattern : {"\\B{g}", "\\B{gcb}", "\\B{foo}"}) {
    GRX_Regex * regex = nullptr;
    GRX_Error error;
    EXPECT_EQ(grx_regex_compile_with_allocator(pattern, strlen(pattern),
                  GRX_SYNTAX_JAVA, GRX_OPT_UTF, nullptr, nullptr, &error,
                  &regex),
        GRX_ERR_SYNTAX)
        << pattern;
    EXPECT_EQ(error.diag, GRX_DIAG_INVALID_QUANTIFIER) << pattern;
    grx_regex_free(regex);
  }

  int matched = 0;
  GRX_Capture span = {GRX_NPOS, GRX_NPOS};
  EXPECT_EQ(search_java("\\B{3}", 5, "aaa", 3, GRX_OPT_UTF, &matched, &span),
      GRX_OK);
  EXPECT_TRUE(matched);

  // perl has the negated bound, which is what makes the refusal Java's
  // rather than this library's.
  GRX_Regex * perl = nullptr;
  EXPECT_EQ(grx_regex_compile_with_allocator("\\B{g}", 5, GRX_SYNTAX_PERL,
                GRX_OPT_UTF, nullptr, nullptr, nullptr, &perl),
      GRX_OK);
  grx_regex_free(perl);
}

TEST(JavaClasses, GraphAndPrintKeepThePrivateUseBlockUnderU) {
  // Found by auditing the profile rather than by a failing gate: GRX_Profile
  // has 41 fields, Java's entry assigned 19 of them, and this was one of the
  // 22 sitting at a zero that nobody had checked against the reference. The
  // zero is the answer for "a dialect with no reference on this machine",
  // which Java is not.
  //
  // JDK 25, measured: `(?U)\p{Graph}` and `(?U)\p{Print}` both match U+E000
  // and U+F0000, so Java keeps `Co` - perl's side of the axis, not pcre2's,
  // which drops it. The rows below are the bounds of the fix as much as the
  // fix: unassigned code points stay out, and without `U` the class is
  // ASCII and never reaches the question at all.
  struct Row {
    const char * pattern;
    const char * subject;
    int matches;
  };
  const Row rows[] = {
    {"(?U)\\p{Graph}", "\xEE\x80\x80", 1},          // U+E000, private use
    {"(?U)\\p{Print}", "\xEE\x80\x80", 1},
    {"(?U)\\p{Graph}", "\xF3\xB0\x80\x80", 1},      // U+F0000, plane 15
    {"\\p{Graph}", "\xEE\x80\x80", 0},              // ASCII without `U`
    {"(?U)\\p{Graph}", "\xCD\xB8", 0},              // U+0378, unassigned
    {"(?U)\\p{Graph}", "\xC2\xAD", 1},              // U+00AD, soft hyphen
    {"(?U)\\p{Graph}", "\xE2\x80\x8B", 1},          // U+200B
    {"(?U)\\p{Graph}", "\xE2\x80\xA8", 0},          // U+2028, a line separator
    {"(?U)\\p{Print}", "\xE2\x80\xA8", 0},
  };
  for (const Row & row : rows) {
    int matched = 0;
    GRX_Capture span = {GRX_NPOS, GRX_NPOS};
    ASSERT_EQ(search_java(row.pattern, strlen(row.pattern), row.subject,
                  strlen(row.subject), GRX_OPT_UTF, &matched, &span),
        GRX_OK)
        << row.pattern << " on " << row.subject;
    EXPECT_EQ(matched != 0, row.matches != 0)
        << row.pattern << " on " << row.subject;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
