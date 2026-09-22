/**
 * @file
 *
 * The library through an allocator that behaves differently and legally.
 *
 * `test_oom.cpp` asks what happens when an allocation *fails*. This asks
 * what happens when one succeeds in a way the system allocator almost never
 * does: `realloc` moving the block.
 *
 * An arena that grows invalidates every pointer into it. Code that obtains a
 * node or an instruction, appends something, and then writes through the
 * pointer it obtained is reading and writing freed memory - and the system
 * allocator hides it, because `realloc` grows a small block in place whenever
 * the bytes after it are free, which for the mostly sequential allocations a
 * compile makes is nearly every time. Such a defect passes every test, passes
 * valgrind, passes ASan, and surfaces the day a pattern is a few nodes
 * longer than the ones anybody tried.
 *
 * grxtest::MovingAllocator removes the hiding place: every `realloc`
 * allocates fresh, copies, scribbles 0xDD over the old block and frees it. A
 * stale read is then a use-after-free under ASan and wrong bytes without it.
 *
 * **Two halves, and they catch different things.** The digest comparison
 * below catches a stale pointer whose value something *reads*; ASan catches
 * one whose value nothing reads, which is not a harmless bug so much as a
 * bug waiting for the field to acquire a reader. Proven by planting one of
 * each: holding the SPLIT across an alternation branch's codegen fails 730
 * conformance vectors with the default allocator and 783 with this one,
 * and holding the ATOMIC_BEGIN across its body's codegen fails nothing
 * anywhere and is a clean `heap-use-after-free` under ASan.
 *
 * The widest run of this is not here but in tests/conformance, which puts
 * all 33,829 vectors through the same allocator.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <string>
#include <vector>

#include "test_helpers.h"

namespace {

using grxtest::MovingAllocator;

/**
 * A whole compile, match, replace and split through one allocator, as text.
 *
 * Returning a digest rather than asserting inside is the whole design: the
 * property being checked is that **the answer does not depend on the
 * allocator**, and that can only be checked by running the same work twice
 * and comparing. A test that merely ran the pipeline and looked for a crash
 * would pass against a stale pointer whose read happened to produce a
 * plausible wrong answer - which is most of them.
 */
std::string digest(const GRX_Allocator * allocator, const char * pattern,
    GRX_Syntax syntax, uint32_t options, const std::string & subject,
    GRX_Engine engine) {
  GRX_Error error;
  grx_error_clear(&error);

  GRX_Regex * regex = nullptr;
  GRX_Result compiled = grx_regex_compile_with_allocator(pattern,
      std::strlen(pattern), syntax, options, nullptr, allocator, &error,
      &regex);
  if (compiled != GRX_OK) {
    // A refused pattern is an answer too, and it still had to unwind.
    return std::string("compile ") + grx_result_string(compiled) + " "
        + std::to_string((int)error.diag);
  }

  std::string out = "ok";

  GRX_SearchOptions search;
  grx_search_options_default(&search);
  search.engine = engine;

  GRX_Match * match = nullptr;
  if (grx_match_create(regex, allocator, &match) == GRX_OK) {
    int matched = 0;
    // The search-all loop, because it is the one that keeps a match object
    // across calls and the one that reaches grx_regex_search_next().
    GRX_Result result = grx_regex_search_ex(regex, subject.data(),
        subject.size(), &search, match, &matched);
    for (int guard = 0; result == GRX_OK && matched && guard < 64; guard++) {
      out += " |";
      for (size_t i = 0; i < grx_match_count(match); i++) {
        GRX_Capture span {};
        grx_match_group(match, i, &span);
        out += span.start == GRX_NPOS
            ? " -"
            : " " + std::to_string(span.start) + ":"
                + std::to_string(span.end);
      }
      result = grx_regex_search_next(regex, subject.data(), subject.size(),
          &search, match, &matched);
    }
    out += " r" + std::to_string((int)result);
    grx_match_destroy(match);
  }

  GRX_Text text {};
  GRX_Result replaced = grx_regex_replace(regex, subject.data(),
      subject.size(), "[$&]", 4, GRX_REPLACE_GLOBAL, &search, allocator,
      nullptr, &text);
  out += " replace " + std::to_string((int)replaced);
  if (replaced == GRX_OK) {
    out += " " + std::string(text.data, text.length);
    grx_text_free(&text);
  }

  GRX_Split split {};
  GRX_Result divided = grx_regex_split(regex, subject.data(), subject.size(),
      GRX_NPOS, &search, allocator, nullptr, &split);
  out += " split " + std::to_string((int)divided);
  if (divided == GRX_OK) {
    for (size_t i = 0; i < split.count; i++) {
      out += split.pieces[i].start == GRX_NPOS
          ? " -"
          : " " + std::to_string(split.pieces[i].start) + ":"
              + std::to_string(split.pieces[i].end);
    }
    grx_split_free(&split);
  }

  // Facts are read from tables analysis built, so they are worth comparing:
  // a prefilter string or a literal prefix held across a growth would show
  // here and nowhere else.
  GRX_Facts facts {};
  if (grx_regex_facts(regex, &facts) == GRX_OK) {
    out += " facts " + std::to_string(facts.is_regular)
        + "," + std::to_string(facts.anchored_start)
        + "," + std::to_string(facts.min_length)
        + "," + std::to_string(facts.capture_count)
        + "," + std::to_string(facts.program_size)
        + "," + std::to_string(facts.max_lookbehind);
    if (facts.literal_prefix && facts.literal_prefix_length) {
      out += ",\"" + std::string(facts.literal_prefix,
          facts.literal_prefix_length) + "\"";
    }
    if (facts.required_literal && facts.required_literal_length) {
      out += ",\"" + std::string(facts.required_literal,
          facts.required_literal_length) + "\"";
    }
  }

  grx_regex_free(regex);
  return out;
}

/**
 * Patterns chosen to make the arenas grow, not to be interesting to match.
 *
 * Every arena in the pipeline has to be pushed past its initial capacity, so
 * the list is long alternations, deep nesting, many groups, many classes,
 * many named groups, and the constructs that build side tables of their own -
 * scan lists for duplicated names, look spans for lookbehinds, the newline
 * and word sets. A short pattern grows nothing and proves nothing.
 */
const char * const kPatterns[] = {
  "(a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q|r|s|t|u|v|w|x|y|z)+",
  "((((((((((a))))))))))+",
  "(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)(m)(n)(o)(p)\\16\\15\\1",
  "[a-c][d-f][g-i][j-l][m-o][p-r][s-u][v-x][y-z][0-9][A-Z][^a-z]",
  "(?<one>a)(?<two>b)(?<three>c)\\k<one>\\k<two>\\k<three>",
  "(?=a)(?!b)(?<=c)(?<!d)(?=e)(?!f)(?<=g)(?<!h)",
  "(?<=abcdefgh)(?<=abcdefg)(?<=abcdef)(?<=abcde)x",
  "a{1,10}b{1,10}c{1,10}d{1,10}e{1,10}f{1,10}",
  "^(?:ab|cd|ef|gh|ij|kl|mn|op|qr|st|uv|wx|yz)*$",
  "\\p{L}\\p{Lu}\\p{Ll}\\p{N}\\p{P}\\p{S}\\p{Z}\\p{C}",
  "(a*)*(b*)*(c*)*(d*)*",
  "(?:(?:(?:(?:(?:(?:(?:(?:a)b)c)d)e)f)g)h)+",
  "x*|y*|z*|(a)|(b)|(c)|(d)|(e)|(f)|(g)|(h)|(i)|(j)",
  "\\b\\B\\b\\B\\b\\B\\b\\B\\w+\\W+\\s+\\S+\\d+\\D+",
};

/** The same, in the dialects that have constructs ECMAScript has not. */
struct Dialect {
  GRX_Syntax syntax;
  const char * pattern;
};

const Dialect kDialects[] = {
  {GRX_SYNTAX_PERL, "(?J)(?<n>a)|(?<n>b)|(?<n>c)|(?<n>d)\\k<n>"},
  {GRX_SYNTAX_PERL, "\\Ga*(?:b|c|d|e|f|g)+\\Z"},
  // An atomic group with a long body: the instruction arena grows *between*
  // the ATOMIC_BEGIN that has to be patched and the ATOMIC_END that patches
  // it, which is the exact span a held pointer would not survive.
  {GRX_SYNTAX_PERL,
      "(?>a|b|c|d|e|f|g|h|i|j|k|l|m|n|o|p|q|r|s|t|u|v|w|x|y|z|aa|bb|cc)+"},
  {GRX_SYNTAX_PCRE,
      "(?>(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)(m)(n)(o)(p)(q)(r))"},
  {GRX_SYNTAX_PERL, "(?(DEFINE)(?<x>a)(?<y>b))(?&x)(?&y)"},
  {GRX_SYNTAX_PCRE, "(?J)(?<n>a)(?<n>b)(?<n>c)\\k<n>"},
  {GRX_SYNTAX_PCRE, "(*MARK:one)a|(*MARK:two)b|(*MARK:three)c"},
  {GRX_SYNTAX_POSIX_ERE, "(a|b|c|d|e|f|g|h)+[[:alpha:]][[:digit:]]"},
  {GRX_SYNTAX_POSIX_BRE, "\\(a\\|b\\|c\\)\\{1,8\\}[[:space:]]"},
  {GRX_SYNTAX_GNU_ERE, "\\<(a|b|c|d|e)+\\>\\`x\\'"},
  {GRX_SYNTAX_GNU_BRE, "\\(a\\|b\\)\\1\\<c\\>"},
};

const char * const kSubjects[] = {
  "", "a", "abc", "abcdefghij", "aaaabbbbcc", "xyz123", "a\nb\nc",
  "the quick brown fox", "aabbccddeeffgghh",
};

TEST(MovingRealloc, TheAnswerDoesNotDependOnTheAllocator) {
  MovingAllocator allocator;
  long compared = 0;

  for (const char * pattern : kPatterns) {
    for (const std::string subject : kSubjects) {
      for (GRX_Engine engine :
          {GRX_ENGINE_PIKE, GRX_ENGINE_BACKTRACK, GRX_ENGINE_BITSTATE}) {
        const std::string expected = digest(nullptr, pattern,
            GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, subject, engine);
        EXPECT_EQ(digest(allocator.get(), pattern, GRX_SYNTAX_ECMASCRIPT,
                      GRX_OPT_UTF, subject, engine),
            expected)
            << "/" << pattern << "/ on \"" << subject << "\" engine "
            << (int)engine;
        compared++;
      }
    }
  }

  EXPECT_GT(compared, 0);
  // A run in which no realloc ever moved a block would pass every assertion
  // above while testing nothing this file exists to test.
  EXPECT_GT(allocator.moves(), 0)
      << "no realloc moved a block, so nothing about growth was tested";
  EXPECT_EQ(allocator.live(), 0);
}

TEST(MovingRealloc, TheOtherDialectsToo) {
  MovingAllocator allocator;

  for (const Dialect & row : kDialects) {
    for (const std::string subject : kSubjects) {
      for (GRX_Engine engine :
          {GRX_ENGINE_PIKE, GRX_ENGINE_BACKTRACK, GRX_ENGINE_BITSTATE}) {
        const std::string expected = digest(nullptr, row.pattern, row.syntax,
            0, subject, engine);
        EXPECT_EQ(digest(allocator.get(), row.pattern, row.syntax, 0, subject,
                      engine),
            expected)
            << "/" << row.pattern << "/ on \"" << subject << "\"";
      }
    }
  }

  EXPECT_GT(allocator.moves(), 0);
  EXPECT_EQ(allocator.live(), 0);
}

TEST(MovingRealloc, APatternLongEnoughToGrowEveryArena) {
  // The lists above are hand-sized. These are built to be past whatever
  // initial capacity any arena in the pipeline has, so that a stage which
  // simply never grows for a short pattern has to grow here.
  MovingAllocator allocator;

  std::vector<std::string> patterns;
  for (int groups : {32, 64, 128, 256}) {
    std::string repeated;
    std::string alternation;
    std::string nested;
    for (int i = 0; i < groups; i++) {
      repeated += "(a|b)";
      alternation += (i ? "|" : "");
      alternation += "x" + std::to_string(i);
      nested += "(?:";
    }
    nested += "a";
    for (int i = 0; i < groups; i++) {
      nested += ")";
    }
    patterns.push_back(repeated);
    patterns.push_back(alternation);
    patterns.push_back(nested);
  }

  for (const std::string & pattern : patterns) {
    for (const char * subject : {"abababababababab", "x17x200", "a", ""}) {
      const std::string expected = digest(nullptr, pattern.c_str(),
          GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, subject, GRX_ENGINE_PIKE);
      EXPECT_EQ(digest(allocator.get(), pattern.c_str(),
                    GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, subject,
                    GRX_ENGINE_PIKE),
          expected)
          << "a pattern of " << pattern.size() << " bytes on \"" << subject
          << "\"";
    }
  }

  EXPECT_GT(allocator.moves(), 0);
  EXPECT_EQ(allocator.live(), 0);
}

} // namespace

int main(int argc, char ** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
