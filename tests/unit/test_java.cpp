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

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
