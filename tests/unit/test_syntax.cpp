/**
 * @file
 *
 * The dialect table.
 *
 * These tests assert the things that define the dialects rather than whatever
 * the table happens to say: that RE2 has no backreference is the reason RE2
 * exists, and if the table ever says otherwise the table is wrong. The
 * provisional entries that are not definitional are deliberately not pinned
 * here - see documentation/dialects.md.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstring>
#include <set>
#include <string>

#include "test_helpers.h"

TEST(Syntax, EveryDialectHasAUniqueName) {
  std::set<std::string> names;
  for (int i = 0; i < GRX_SYNTAX_COUNT; i++) {
    const char * name = grx_syntax_name((GRX_Syntax)i);
    ASSERT_NE(name, nullptr);
    EXPECT_STRNE(name, "unknown") << "dialect " << i << " has no name";
    EXPECT_TRUE(names.insert(name).second) << name << " is used twice";
  }
}

TEST(Syntax, NameRoundTrips) {
  for (int i = 0; i < GRX_SYNTAX_COUNT; i++) {
    GRX_Syntax syntax = GRX_SYNTAX_COUNT;
    ASSERT_EQ(grx_syntax_from_name(grx_syntax_name((GRX_Syntax)i), &syntax),
        GRX_OK);
    EXPECT_EQ(syntax, (GRX_Syntax)i);
  }
}

TEST(Syntax, NameLookupIgnoresCase) {
  GRX_Syntax syntax = GRX_SYNTAX_COUNT;
  ASSERT_EQ(grx_syntax_from_name("PCRE", &syntax), GRX_OK);
  EXPECT_EQ(syntax, GRX_SYNTAX_PCRE);

  ASSERT_EQ(grx_syntax_from_name("PoSiX-Ere", &syntax), GRX_OK);
  EXPECT_EQ(syntax, GRX_SYNTAX_POSIX_ERE);
}

TEST(Syntax, UnknownNameIsInvalid) {
  GRX_Syntax syntax = GRX_SYNTAX_PCRE;
  EXPECT_EQ(grx_syntax_from_name("no-such-dialect", &syntax), GRX_ERR_INVALID);
  EXPECT_EQ(grx_syntax_from_name(nullptr, &syntax), GRX_ERR_INVALID);
  EXPECT_EQ(grx_syntax_from_name("pcre", nullptr), GRX_ERR_INVALID);
  // A prefix of a real name is not that name.
  EXPECT_EQ(grx_syntax_from_name("pcr", &syntax), GRX_ERR_INVALID);
  EXPECT_EQ(grx_syntax_from_name("pcre2", &syntax), GRX_ERR_INVALID);
}

TEST(Syntax, OutOfRangeDialectIsUnknown) {
  EXPECT_STREQ(grx_syntax_name(GRX_SYNTAX_COUNT), "unknown");
  EXPECT_STREQ(grx_syntax_name((GRX_Syntax)9999), "unknown");

  GRX_SyntaxSpec spec;
  EXPECT_EQ(grx_syntax_spec(GRX_SYNTAX_COUNT, &spec), GRX_ERR_INVALID);
  EXPECT_EQ(grx_syntax_spec(GRX_SYNTAX_PCRE, nullptr), GRX_ERR_INVALID);
  EXPECT_EQ(grx_syntax_has_feature(GRX_SYNTAX_COUNT,
                GRX_FEATURE_ALTERNATION),
      0);
}

TEST(Syntax, EveryDialectHasASpec) {
  for (int i = 0; i < GRX_SYNTAX_COUNT; i++) {
    GRX_SyntaxSpec spec;
    std::memset(&spec, 0, sizeof(spec));
    ASSERT_EQ(grx_syntax_spec((GRX_Syntax)i, &spec), GRX_OK)
        << grx_syntax_name((GRX_Syntax)i);
    // A dialect with no features at all is an unfilled table row.
    EXPECT_NE(spec.features, 0u) << grx_syntax_name((GRX_Syntax)i);
  }
}

TEST(Syntax, BasicRegularExpressionsSpellTheOperatorsEscaped) {
  // This is what makes a BRE a BRE: `(` is a literal and `\(` groups. The
  // parser reads this flag rather than the dialect constant, so it is the
  // flag the test states.
  GRX_SyntaxSpec spec;
  ASSERT_EQ(grx_syntax_spec(GRX_SYNTAX_POSIX_BRE, &spec), GRX_OK);
  EXPECT_NE(spec.escaped_specials, 0);

  ASSERT_EQ(grx_syntax_spec(GRX_SYNTAX_POSIX_ERE, &spec), GRX_OK);
  EXPECT_EQ(spec.escaped_specials, 0);
}

TEST(Syntax, PosixExtendedHasNoBackreference) {
  // POSIX puts backreferences in the basic syntax and leaves them out of the
  // extended one (IEEE Std 1003.1 chapter 9.4).
  EXPECT_TRUE(grx_syntax_has_feature(
      GRX_SYNTAX_POSIX_BRE, GRX_FEATURE_BACKREFERENCE));
  EXPECT_FALSE(grx_syntax_has_feature(
      GRX_SYNTAX_POSIX_ERE, GRX_FEATURE_BACKREFERENCE));
}

TEST(Syntax, PosixBasicHasNoAlternation) {
  // `|` is a literal in POSIX BRE. GNU's extension adds `\|`, which is why
  // the two are separate dialects rather than one with a flag.
  EXPECT_FALSE(grx_syntax_has_feature(
      GRX_SYNTAX_POSIX_BRE, GRX_FEATURE_ALTERNATION));
  EXPECT_TRUE(
      grx_syntax_has_feature(GRX_SYNTAX_GNU_BRE, GRX_FEATURE_ALTERNATION));
}

TEST(Syntax, LinearTimeDialectsHaveNeitherBackreferencesNorLookaround) {
  // RE2 and the Rust crate guarantee linear time, and neither construct can
  // be run in linear time. A table that granted them either would let a
  // pattern compile that the Pike VM then could not execute.
  for (GRX_Syntax syntax : {GRX_SYNTAX_RE2, GRX_SYNTAX_RUST}) {
    EXPECT_FALSE(
        grx_syntax_has_feature(syntax, GRX_FEATURE_BACKREFERENCE))
        << grx_syntax_name(syntax);
    EXPECT_FALSE(grx_syntax_has_feature(syntax, GRX_FEATURE_LOOKAHEAD))
        << grx_syntax_name(syntax);
    EXPECT_FALSE(grx_syntax_has_feature(syntax, GRX_FEATURE_LOOKBEHIND))
        << grx_syntax_name(syntax);
  }
}

TEST(Syntax, EveryDialectHasBoundedRepetition) {
  // Every syntax in the list spells `{m,n}` somehow, even where it needs
  // escaping. A row missing it is an unfilled row rather than a dialect.
  for (int i = 0; i < GRX_SYNTAX_COUNT; i++) {
    EXPECT_TRUE(grx_syntax_has_feature(
        (GRX_Syntax)i, GRX_FEATURE_BOUNDED_REPEAT))
        << grx_syntax_name((GRX_Syntax)i);
  }
}

TEST(Syntax, OptionsAreDistinctBits) {
  const uint32_t options[] = {GRX_OPT_CASELESS, GRX_OPT_MULTILINE,
      GRX_OPT_DOTALL, GRX_OPT_EXTENDED, GRX_OPT_UNGREEDY, GRX_OPT_ANCHORED,
      GRX_OPT_UTF, GRX_OPT_UCP, GRX_OPT_NO_CAPTURE, GRX_OPT_LITERAL};

  uint32_t seen = 0;
  for (uint32_t option : options) {
    EXPECT_EQ(option & (option - 1), 0u) << "not a single bit: " << option;
    EXPECT_EQ(seen & option, 0u) << "bit reused: " << option;
    seen |= option;
  }
}

// --------------------------------------------------------------------------
// Flag strings
// --------------------------------------------------------------------------

TEST(Options, ReadsTheDialectsOwnAlphabet) {
  // documentation/dialects.md section 5.15. Every consumer has one of these
  // strings and none of them should have to know that `s` is dot-all in
  // PCRE2 and multiline in Ruby.
  struct {
    GRX_Syntax syntax;
    const char * flags;
    uint32_t expected;
  } cases[] = {
    {GRX_SYNTAX_ECMASCRIPT, "", 0},
    {GRX_SYNTAX_ECMASCRIPT, "i", GRX_OPT_CASELESS},
    {GRX_SYNTAX_ECMASCRIPT, "imsu",
        GRX_OPT_CASELESS | GRX_OPT_MULTILINE | GRX_OPT_DOTALL | GRX_OPT_UTF},
    // `d` is always satisfied here, so it parses and sets nothing.
    {GRX_SYNTAX_ECMASCRIPT, "d", 0},
    {GRX_SYNTAX_ECMASCRIPT, "v", GRX_OPT_UNICODE_SETS | GRX_OPT_UTF},
    {GRX_SYNTAX_PCRE, "imsxU",
        GRX_OPT_CASELESS | GRX_OPT_MULTILINE | GRX_OPT_DOTALL
            | GRX_OPT_EXTENDED | GRX_OPT_UNGREEDY},
    {GRX_SYNTAX_PERL, "msix",
        GRX_OPT_MULTILINE | GRX_OPT_DOTALL | GRX_OPT_CASELESS
            | GRX_OPT_EXTENDED},
    {GRX_SYNTAX_PYTHON, "imsx",
        GRX_OPT_CASELESS | GRX_OPT_MULTILINE | GRX_OPT_DOTALL
            | GRX_OPT_EXTENDED},
  };

  for (const auto & test : cases) {
    uint32_t options = 0xFFFFFFFFu;
    GRX_Error error;
    EXPECT_EQ(grx_options_parse(test.syntax, test.flags, &options, &error),
        GRX_OK)
        << grx_syntax_name(test.syntax) << " /" << test.flags << ": "
        << error.message;
    EXPECT_EQ(options, test.expected)
        << grx_syntax_name(test.syntax) << " /" << test.flags;
  }
}

TEST(Options, RefusesWhatItCannotHonour) {
  struct {
    GRX_Syntax syntax;
    const char * flags;
    GRX_Result result;
    GRX_Diag diag;
    size_t offset;
  } cases[] = {
    // A letter this dialect's alphabet does not contain.
    {GRX_SYNTAX_ECMASCRIPT, "ix", GRX_ERR_SYNTAX, GRX_DIAG_UNKNOWN_FLAG, 1},
    {GRX_SYNTAX_PCRE, "u", GRX_ERR_SYNTAX, GRX_DIAG_UNKNOWN_FLAG, 0},
    // The same letter twice.
    {GRX_SYNTAX_ECMASCRIPT, "ii", GRX_ERR_SYNTAX, GRX_DIAG_DUPLICATE_FLAG, 1},
    // A search mode, which is an API call rather than a compile-time option.
    // Accepting `g` would give a caller a regex that quietly ignored it.
    {GRX_SYNTAX_ECMASCRIPT, "g", GRX_ERR_SYNTAX,
        GRX_DIAG_SEARCH_FLAG_IN_PATTERN, 0},
    {GRX_SYNTAX_ECMASCRIPT, "iy", GRX_ERR_SYNTAX,
        GRX_DIAG_SEARCH_FLAG_IN_PATTERN, 1},
    // `u` and `v` exclude each other.
    {GRX_SYNTAX_ECMASCRIPT, "uv", GRX_ERR_SYNTAX,
        GRX_DIAG_CONFLICTING_FLAGS, 1},
    {GRX_SYNTAX_ECMASCRIPT, "vu", GRX_ERR_SYNTAX,
        GRX_DIAG_CONFLICTING_FLAGS, 1},
    // Letters the dialect has and this library does not implement yet. A
    // different answer from "unknown", because the caller's pattern is fine
    // and the gap is here.
    {GRX_SYNTAX_PYTHON, "L", GRX_ERR_UNSUPPORTED,
        GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, 0},
    {GRX_SYNTAX_PCRE, "J", GRX_ERR_UNSUPPORTED,
        GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED, 0},
    // POSIX has no flag letters at all; its options are API arguments.
    {GRX_SYNTAX_POSIX_ERE, "i", GRX_ERR_SYNTAX, GRX_DIAG_UNKNOWN_FLAG, 0},
  };

  for (const auto & test : cases) {
    uint32_t options = 0;
    GRX_Error error;
    EXPECT_EQ(grx_options_parse(test.syntax, test.flags, &options, &error),
        test.result)
        << grx_syntax_name(test.syntax) << " /" << test.flags;
    EXPECT_EQ(error.diag, test.diag)
        << grx_syntax_name(test.syntax) << " /" << test.flags << ": "
        << grx_diag_string(error.diag);
    EXPECT_EQ(error.offset, test.offset)
        << grx_syntax_name(test.syntax) << " /" << test.flags;
    EXPECT_EQ(options, 0u) << "nothing is set when the string is refused";
  }

  // POSIX with no flags at all is fine: it has none to give.
  uint32_t options = 1;
  EXPECT_EQ(
      grx_options_parse(GRX_SYNTAX_POSIX_ERE, "", &options, nullptr), GRX_OK);
  EXPECT_EQ(options, 0u);
}

TEST(Options, RejectsArgumentsItCannotUse) {
  uint32_t options = 0;
  EXPECT_EQ(
      grx_options_parse(GRX_SYNTAX_ECMASCRIPT, nullptr, &options, nullptr),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_options_parse(GRX_SYNTAX_ECMASCRIPT, "i", nullptr, nullptr),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_options_parse(GRX_SYNTAX_COUNT, "i", &options, nullptr),
      GRX_ERR_INVALID);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
