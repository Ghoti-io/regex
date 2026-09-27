/**
 * @file
 *
 * The parser and the ECMAScript front end.
 *
 * Every test here states a rule from documentation/dialects.md section 8 and
 * names the clause of ECMA-262 it comes from, rather than recording what the
 * parser currently does. Where the rule was checked against the pinned node
 * - the oracle the conformance vectors come from - the test says so, because
 * "the reference implementation agrees" is a stronger claim than "the
 * specification seems to say". A comment naming "Node 22" records which
 * release the rule was taken against; the pin is node 24 / V8 13.6 now, and
 * tools/oracle/syntax_diff.py re-asks the whole accept/reject surface every
 * run.
 *
 * The accept/reject behaviour as a whole is checked against Node by
 * tools/oracle/syntax_diff.py, which runs millions of patterns through both.
 * These tests are for the rules a sweep cannot state: which node came out,
 * which diagnostic was reported, and at what offset.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <string>

#include "test_helpers.h"

#include "../../src/parse/parse_internal.h"

namespace {

/** The options a flag letter turns on, as ECMAScript spells them. */
uint32_t flags_to_options(const std::string & flags) {
  uint32_t options = 0;
  for (char f : flags) {
    switch (f) {
      case 'i': options |= GRX_OPT_CASELESS; break;
      case 'm': options |= GRX_OPT_MULTILINE; break;
      case 's': options |= GRX_OPT_DOTALL; break;
      case 'u': options |= GRX_OPT_UTF; break;
      case 'v': options |= GRX_OPT_UNICODE_SETS | GRX_OPT_UTF; break;
      default: break;
    }
  }
  return options;
}

/** A parsed pattern that frees itself. */
class Parsed {
public:
  Parsed(const std::string & pattern, const std::string & flags = "",
      const GRX_Limits * limits = nullptr) {
    grx_error_clear(&error_);
    result_ = grx_pattern_parse_with_allocator(pattern.data(), pattern.size(),
        GRX_SYNTAX_ECMASCRIPT, flags_to_options(flags), limits, nullptr,
        &error_, &pattern_);
  }
  Parsed(const Parsed &) = delete;
  Parsed & operator=(const Parsed &) = delete;
  ~Parsed() { grx_pattern_free(pattern_); }

  GRX_Result result() const { return result_; }
  GRX_Diag diag() const { return error_.diag; }
  size_t offset() const { return error_.offset; }
  const GRX_Pattern * get() const { return pattern_; }
  bool ok() const { return result_ == GRX_OK; }

  /** The dump, minus its header line, so a test names the tree and not the counts. */
  std::string tree() const {
    if (!pattern_) {
      return "<failed>";
    }
    std::string dump = grxtest::capture_dump(
        [this](FILE * out) { grx_pattern_dump(pattern_, out); });
    size_t newline = dump.find('\n');
    return newline == std::string::npos ? dump : dump.substr(newline + 1);
  }

private:
  GRX_Pattern * pattern_ = nullptr;
  GRX_Result result_ = GRX_ERR_INTERNAL;
  GRX_Error error_ {};
};

/** The dump of one pattern, with indentation collapsed to single spaces. */
std::string flat(const std::string & pattern, const std::string & flags = "") {
  Parsed parsed(pattern, flags);
  if (!parsed.ok()) {
    return "<error>";
  }

  std::string out;
  std::string dump = parsed.tree();
  bool leading = true;
  for (char c : dump) {
    if (c == '\n') {
      out += " |";
      leading = true;
      continue;
    }
    if (leading && c == ' ') {
      continue;
    }
    leading = false;
    out += c;
  }
  return out;
}

} // namespace

// --------------------------------------------------------------------------
// The shared grammar
// --------------------------------------------------------------------------

TEST(Parse, ConcatenationAlternationAndGrouping) {
  EXPECT_EQ(flat("ab"), "concat @0+2 |literal @0+1 \"a\" |literal @1+1 \"b\" |");

  // One term is not a concatenation of one thing: the node exists only when
  // there is something to concatenate, which keeps the tree readable and the
  // node count honest.
  EXPECT_EQ(flat("a"), "literal @0+1 \"a\" |");

  EXPECT_EQ(flat("a|b"),
      "alternate @0+3 |literal @0+1 \"a\" |literal @2+1 \"b\" |");

  // An empty branch is an explicit node rather than a missing one, so that a
  // walker never has to ask whether a child is there.
  EXPECT_EQ(flat("a|"),
      "alternate @0+2 |literal @0+1 \"a\" |empty @2+0 |");
  EXPECT_EQ(flat("|a"),
      "alternate @0+2 |empty @0+0 |literal @1+1 \"a\" |");
  EXPECT_EQ(flat("()"), "group @0+2 #1 |empty @1+0 |");
}

TEST(Parse, QuantifiersNormaliseToBounds) {
  struct {
    const char * pattern;
    const char * bounds;
  } cases[] = {
    {"a*", "{0,} greedy"},
    {"a+", "{1,} greedy"},
    {"a?", "{0,1} greedy"},
    {"a*?", "{0,} lazy"},
    {"a+?", "{1,} lazy"},
    {"a??", "{0,1} lazy"},
    {"a{3}", "{3,3} greedy"},
    {"a{3,}", "{3,} greedy"},
    {"a{3,5}", "{3,5} greedy"},
    {"a{3,5}?", "{3,5} lazy"},
    {"a{0,0}", "{0,0} greedy"},
  };

  for (const auto & test : cases) {
    Parsed parsed(test.pattern);
    ASSERT_TRUE(parsed.ok()) << test.pattern;
    EXPECT_NE(parsed.tree().find(test.bounds), std::string::npos)
        << test.pattern << " gave " << parsed.tree();
  }
}

TEST(Parse, NestingDepthIsCapped) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_nesting_depth = 4;

  Parsed shallow("((((a))))", "", &limits);
  EXPECT_TRUE(shallow.ok());

  Parsed deep("(((((a)))))", "", &limits);
  EXPECT_EQ(deep.result(), GRX_ERR_LIMIT);
  EXPECT_EQ(deep.diag(), GRX_DIAG_LIMIT_NESTING_DEPTH);

  // Alternation nests too: a chain of branches is as deep as a chain of
  // groups, and a cap that counted only parentheses would let a pattern
  // recurse as far as it liked through `|`.
  Parsed alternating("(((((a|b)))))", "", &limits);
  EXPECT_EQ(alternating.result(), GRX_ERR_LIMIT);
}

TEST(Parse, RepeatCountIsCapped) {
  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_repeat_count = 100;

  Parsed fits("a{100}", "", &limits);
  EXPECT_TRUE(fits.ok());

  // ECMA-262's grammar admits a count up to 2^53-1. This library caps it,
  // and reports a *limit* rather than a syntax error, because the pattern is
  // well-formed and the refusal is this library's policy.
  Parsed exceeds("a{101}", "", &limits);
  EXPECT_EQ(exceeds.result(), GRX_ERR_LIMIT);
  EXPECT_EQ(exceeds.diag(), GRX_DIAG_LIMIT_REPEAT_COUNT);
}

TEST(Parse, StructuralErrorsCarryAPosition) {
  struct {
    const char * pattern;
    GRX_Diag diag;
    size_t offset;
  } cases[] = {
    {"(a", GRX_DIAG_UNMATCHED_OPEN_PAREN, 0},
    {"a)", GRX_DIAG_UNMATCHED_CLOSE_PAREN, 1},
    {"a(b(c)", GRX_DIAG_UNMATCHED_OPEN_PAREN, 1},
    {"[a", GRX_DIAG_UNMATCHED_OPEN_BRACKET, 0},
    {"a\\", GRX_DIAG_TRAILING_BACKSLASH, 1},
    {"*a", GRX_DIAG_NOTHING_TO_REPEAT, 0},
    {"+", GRX_DIAG_NOTHING_TO_REPEAT, 0},
    {"a{3,1}", GRX_DIAG_QUANTIFIER_OUT_OF_ORDER, 1},
    {"a**", GRX_DIAG_DOUBLE_QUANTIFIER, 2},
    {"[z-a]", GRX_DIAG_INVALID_CLASS_RANGE, 1},
  };

  for (const auto & test : cases) {
    Parsed parsed(test.pattern);
    EXPECT_EQ(parsed.result(), GRX_ERR_SYNTAX) << test.pattern;
    EXPECT_EQ(parsed.diag(), test.diag)
        << test.pattern << " reported " << grx_diag_string(parsed.diag());
    EXPECT_EQ(parsed.offset(), test.offset)
        << test.pattern << ": the offset is what a caller underlines";
  }
}

TEST(Parse, MalformedUtf8InThePatternIsRejected) {
  const std::string bad = std::string("a") + '\xC3' + "b";
  Parsed parsed(bad);
  EXPECT_EQ(parsed.result(), GRX_ERR_SYNTAX);
  EXPECT_EQ(parsed.diag(), GRX_DIAG_INVALID_UTF8_IN_PATTERN);
}

TEST(Parse, LiteralOptionTakesThePatternAsText) {
  Parsed parsed("a(b|c)*", "");
  ASSERT_TRUE(parsed.ok());

  GRX_Pattern * literal = nullptr;
  GRX_Error error;
  const std::string text = "a(b|c)*";
  ASSERT_EQ(grx_pattern_parse_with_allocator(text.data(), text.size(),
                GRX_SYNTAX_ECMASCRIPT, GRX_OPT_LITERAL, nullptr, nullptr,
                &error, &literal),
      GRX_OK);
  std::string dump = grxtest::capture_dump(
      [literal](FILE * out) { grx_pattern_dump(literal, out); });
  EXPECT_NE(dump.find("literal @0+7 \"a(b|c)*\""), std::string::npos) << dump;
  grx_pattern_free(literal);
}

// --------------------------------------------------------------------------
// ECMAScript: the two modes
// --------------------------------------------------------------------------

TEST(EcmaScript, AnnexBRelaxationsApplyWithoutUAndNotWithIt) {
  // documentation/dialects.md section 8.2. Each row is a relaxation Annex
  // B.1.2 makes and Unicode mode removes; each was checked against Node 22,
  // which accepts the left column without `u` and rejects it with.
  const char * relaxed[] = {
    "\\p",      // Property escapes do not exist; `\p` is the identity escape.
    "\\-",      // An identity escape for a character with no escape.
    "\\a",
    "a{",       // A `{` that is not a quantifier is a literal.
    "a{1",
    "a{,3}",
    "]",        // A `]` that closes nothing is a literal.
    "}",
    "{",
    "(?=a)*",   // A lookahead may be quantified.
    "(?!a)+",
    "[\\d-z]",  // A class escape at a range end makes a union, not a range.
    "\\c",      // `\c` with no letter after it is the character `\`.
    "\\x",      // A malformed `\x` is the identity escape `x`.
    "\\u",
    "\\8",      // `\8` and `\9` are not octal, so they are identity escapes.
    "\\9",
    "\\01",     // Legacy octal.
    "\\377",
    "\\1",      // A decimal escape with no such group is octal or identity.
    "\\k",      // `\k` with no named group in the pattern is a literal `k`.
  };

  for (const char * pattern : relaxed) {
    Parsed legacy(pattern, "");
    EXPECT_TRUE(legacy.ok())
        << pattern << " should be accepted without `u`: "
        << grx_diag_string(legacy.diag());

    Parsed unicode(pattern, "u");
    EXPECT_EQ(unicode.result(), GRX_ERR_SYNTAX)
        << pattern << " should be rejected with `u`";
  }
}

TEST(EcmaScript, LookbehindIsNeverQuantifiable) {
  // Annex B quantifies a lookahead and not a lookbehind, so this one is a
  // syntax error in both modes - the single row where the two agree and the
  // relaxation does not apply.
  for (const char * flags : {"", "u"}) {
    Parsed parsed("(?<=a)*", flags);
    EXPECT_EQ(parsed.result(), GRX_ERR_SYNTAX) << "flags=" << flags;
    EXPECT_EQ(parsed.diag(), GRX_DIAG_QUANTIFIED_ASSERTION);
  }
}

TEST(EcmaScript, BackslashCWithoutALetterIsTheBackslashItself) {
  // Annex B ExtendedAtom: `\` followed by `c` with no letter after it is the
  // single character `\`, and the `c` is read as the next atom. Node agrees:
  // /\c/ matches the two characters "\c" and does not match "\" alone.
  EXPECT_EQ(flat("\\c"),
      "concat @0+2 |literal @0+1 \"\\\\\" |literal @1+1 \"c\" |");

  // Inside a class the same `\c` may be followed by a digit or `_`, and then
  // it *is* a control escape: `[\c1]` is U+0011.
  Parsed parsed("[\\c1]");
  ASSERT_TRUE(parsed.ok());
  EXPECT_NE(parsed.tree().find("[\\x11]"), std::string::npos)
      << parsed.tree();
}

TEST(EcmaScript, DecimalEscapesChooseBetweenBackreferenceAndOctal) {
  // documentation/dialects.md section 5.7. The choice is made on the whole
  // number's value against the group count, which the prescan established
  // before parsing began - so `\1` in a pattern whose group 1 comes later is
  // still a backreference.
  EXPECT_NE(flat("(a)\\1").find("backref"), std::string::npos);
  EXPECT_NE(flat("\\1(a)").find("backref"), std::string::npos);

  // No group 1, so Annex B reads it as a legacy octal escape: U+0001.
  Parsed octal("\\1");
  ASSERT_TRUE(octal.ok());
  EXPECT_NE(octal.tree().find("\\x01"), std::string::npos) << octal.tree();

  // Three digits when the first is 0 to 3, two otherwise. `\400` is
  // therefore `\40` - a space - followed by a literal `0`.
  Parsed four_hundred("\\400");
  ASSERT_TRUE(four_hundred.ok());
  EXPECT_EQ(flat("\\400"),
      "concat @0+4 |literal @0+3 \" \" |literal @3+1 \"0\" |");

  // In Unicode mode a decimal escape must name a group that exists.
  Parsed unicode("\\1", "u");
  EXPECT_EQ(unicode.result(), GRX_ERR_SYNTAX);
  EXPECT_EQ(unicode.diag(), GRX_DIAG_INVALID_BACKREFERENCE);
}

TEST(EcmaScript, NamedGroupsAndReferences) {
  EXPECT_EQ(flat("(?<n>a)\\k<n>", "u"),
      "concat @0+12 |group @0+7 #1 name=n |literal @5+1 \"a\" "
      "|backref @7+5 name=n |");

  // The group may come after the reference: `\k` is resolved against the
  // whole pattern, which is why the parser prescans.
  EXPECT_TRUE(Parsed("\\k<n>(?<n>a)", "u").ok());

  // A reference to a name no group has is a syntax error, reported at the
  // reference rather than at the end of the pattern.
  Parsed unknown("\\k<nosuch>(?<n>a)", "u");
  EXPECT_EQ(unknown.result(), GRX_ERR_SYNTAX);
  EXPECT_EQ(unknown.diag(), GRX_DIAG_UNKNOWN_GROUP_NAME);
  EXPECT_EQ(unknown.offset(), 0u);

  // A name is an IdentifierName, so `\u` escapes inside it are resolved and
  // two spellings of one name are one name.
  EXPECT_TRUE(Parsed("(?<\\u0061>x)\\k<a>", "u").ok());
  EXPECT_TRUE(Parsed("(?<$a>x)", "u").ok());
  EXPECT_TRUE(Parsed("(?<_a>x)", "u").ok());
  EXPECT_TRUE(Parsed("(?<\xC3\xA9>x)", "u").ok());

  Parsed digit_first("(?<1a>x)", "u");
  EXPECT_EQ(digit_first.result(), GRX_ERR_SYNTAX);
  EXPECT_EQ(digit_first.diag(), GRX_DIAG_INVALID_GROUP_NAME);

  Parsed empty("(?<>x)", "u");
  EXPECT_EQ(empty.result(), GRX_ERR_SYNTAX);

  Parsed duplicate("(?<n>a)(?<n>b)", "u");
  EXPECT_EQ(duplicate.result(), GRX_ERR_SYNTAX);
  EXPECT_EQ(duplicate.diag(), GRX_DIAG_DUPLICATE_GROUP_NAME);
  // ...and across alternatives it is legal, which is ES2025. See
  // DuplicateNamesAreLegalOnlyAcrossAlternatives for the rule.
  EXPECT_TRUE(Parsed("(?<n>a)|(?<n>b)", "u").ok());
}

TEST(EcmaScript, DuplicateNamesAreLegalOnlyAcrossAlternatives) {
  // ES2025 allows a name twice, and the proposal states the whole of the rule
  // as reusing it only "in different `|` alternatives, so that it's
  // impossible for a single match to actually use the same name multiple
  // times". So the question is whether one match could fill both, and the
  // only construct here that makes two things exclusive is an alternation.
  const char * legal[] = {
    "(?<n>a)|(?<n>b)",              // The rule itself.
    "(?<n>a)|(?<n>b)|(?<n>c)",      // Three branches, three groups.
    "(?:(?<n>a)|(?<n>b))\\k<n>",     // A reference to whichever filled.
    "((?:(?<n>a)|(?<n>b)))",        // Nested inside plain groups.
    "(?<n>a)*|(?<n>b)",             // A quantifier on one of them.
    "(?=(?<n>a))|(?<n>b)",          // One inside an assertion.
    "(?i:(?<n>a))|(?<n>b)",         // One inside a modifier group.
  };
  for (const char * pattern : legal) {
    EXPECT_TRUE(Parsed(pattern, "u").ok()) << pattern;
  }

  // Every one of these could fill both in a single match, so every one is an
  // error - and the diagnostic says which error, because
  // GRX_DIAG_INVALID_GROUP_SYNTAX would also "fail" and would tell a caller
  // the group was malformed.
  const char * illegal[] = {
    "(?<n>a)(?<n>b)",               // Sequential, the original rule.
    "(?<n>a)(?:(?<n>b))",           // Sequential through a plain group.
    "(?:(?<n>a)|x)(?:(?<n>b)|y)",   // Two alternations, not one.
    "(?<n>a)?(?<n>b)",              // Optional does not make them exclusive.
    "(?<n>a){0}(?<n>b)",            // Nor does a zero repeat.
    "(?=(?<n>a))(?<n>b)",           // A lookahead's captures survive it.
    "(?<n>a)(?!(?<n>b))",           // So do a failed negative one's, here.
    "(?<=(?<n>a))(?<n>b)",          // Lookbehind too.
    "(?<n>x(?<n>y))",               // One inside the other.
    "(?:|(?<n>a))(?<n>b)",          // An empty first branch is still a branch.
    "(?<n>a)|(?<n>b)(?<n>c)",       // Legal pair, illegal pair, one pattern.
    // **And the family V8 13.6 accepts.** Over "ac" it fills both - group 1
    // "a", group 2 "c", `groups.n` "c" - which is the thing the rule exists
    // to prevent, so this library refuses it and dialects.md section 6
    // carries the row. `tools/oracle/syntax_diff.py` re-asks node every run.
    "(?<n>a)(?:b|(?<n>c))",
    "(?<n>a)(?:x|(?<n>b))",
    "(?<n>a)(?:b|c(?<n>d))",
  };
  for (const char * pattern : illegal) {
    Parsed parsed(pattern, "u");
    EXPECT_EQ(parsed.result(), GRX_ERR_SYNTAX) << pattern;
    EXPECT_EQ(parsed.diag(), GRX_DIAG_DUPLICATE_GROUP_NAME) << pattern;
  }

  // GRX_OPT_DUPLICATE_NAMES turns the check off, which is what that option
  // documents itself as doing. ECMAScript has no `(?J)` to set it with, so
  // this is only reachable from a caller that asked for it by name.
  GRX_Pattern * pattern = nullptr;
  GRX_Error error;
  grx_error_clear(&error);
  EXPECT_EQ(grx_pattern_parse_with_allocator("(?<n>a)(?<n>b)", 14,
                GRX_SYNTAX_ECMASCRIPT,
                GRX_OPT_UTF | GRX_OPT_DUPLICATE_NAMES, nullptr, nullptr,
                &error, &pattern),
      GRX_OK);
  grx_pattern_free(pattern);
}

TEST(EcmaScript, ModifierGroupsTakeThreeLettersAndMustBeScoped) {
  // ES2025's RegExp Modifiers. Three letters, and the `:` is not optional -
  // which is what makes this a different construct from the Perl family's
  // inline flags rather than the same one with a shorter alphabet.
  for (const char * pattern : {"(?i:a)", "(?m:a)", "(?s:a)", "(?im:a)",
                               "(?ims:a)", "(?-i:a)", "(?-ims:a)", "(?i-s:a)",
                               "(?im-s:a)", "(?i-:a)", "(?i:)"}) {
    EXPECT_TRUE(Parsed(pattern, "u").ok()) << pattern;
  }

  // A letter may not be both set and cleared, nor repeated within a list;
  // the empty modifier is only refused when it is the *only* list; and the
  // letters are the three, not the regexp's whole alphabet.
  for (const char * pattern : {"(?i)a", "(?i-i:a)", "(?ii:a)", "(?--i:a)",
                               "(?-:a)", "(?x:a)", "(?d:a)", "(?u:a)",
                               "(?v:a)", "(?g:a)", "(?y:a)", "(?i a)",
                               "(?i", "(?-"}) {
    Parsed parsed(pattern, "u");
    EXPECT_EQ(parsed.result(), GRX_ERR_SYNTAX) << pattern;
  }
}

TEST(EcmaScript, BackslashKIsALiteralOnlyWhenNoGroupIsNamed) {
  // Annex B's SourceCharacterIdentityEscape[+N] excludes `k` when the pattern
  // names a group anywhere - and the exclusion reaches inside a class, where
  // a named reference cannot appear at all. Found by differential fuzzing
  // against Node: reading the grammar had missed it, because the rule hangs
  // off a production parameter rather than off the class.
  EXPECT_TRUE(Parsed("[\\k]").ok());
  EXPECT_EQ(Parsed("[\\k](?<n>x)").result(), GRX_ERR_SYNTAX);
  EXPECT_EQ(Parsed("[\\k]", "u").result(), GRX_ERR_SYNTAX);
}

TEST(EcmaScript, GroupsThePerlFamilyHasAndThisDialectDoesNot) {
  // design.md section 4: a dialect that accepts everything is a bug. A caller
  // uses this library to learn whether a pattern is valid *for ECMAScript*.
  const char * foreign[] = {
    "(?>a)",    // Atomic group: PCRE and Perl.
    "(?#c)",    // Comment group.
    "(?(1)a)",  // Conditional.
    "(?P<n>a)", // Python's named group.
    "(?'n'a)",  // Perl's other named group.
    "(?i)a",    // A bare inline flag: ES2025's modifier must be scoped.
  };

  for (const char * pattern : foreign) {
    for (const char * flags : {"", "u"}) {
      Parsed parsed(pattern, flags);
      EXPECT_EQ(parsed.result(), GRX_ERR_SYNTAX)
          << pattern << " flags=" << flags;
    }
  }

  // `a++` is a possessive quantifier in Perl and a double quantifier here.
  EXPECT_EQ(Parsed("a++").result(), GRX_ERR_SYNTAX);
}

TEST(EcmaScript, EscapesTheDialectDoesNotDefine) {
  // Each of these is an anchor or a construct in the Perl family. In
  // ECMAScript without `u` they are identity escapes - `\A` matches "A" -
  // and with `u` they are syntax errors. Node agrees on both.
  const char * perlish[] = {"\\A", "\\z", "\\Z", "\\G", "\\h", "\\R", "\\K",
      "\\Q", "\\E", "\\g"};

  for (const char * pattern : perlish) {
    Parsed legacy(pattern, "");
    ASSERT_TRUE(legacy.ok()) << pattern;
    EXPECT_NE(legacy.tree().find("literal"), std::string::npos)
        << pattern << " should be an identity escape, not an anchor: "
        << legacy.tree();

    EXPECT_EQ(Parsed(pattern, "u").result(), GRX_ERR_SYNTAX) << pattern;
  }

  // The two that *are* anchors in every mode.
  EXPECT_NE(flat("\\b", "u").find("anchor @0+2 word-boundary"),
      std::string::npos);
  EXPECT_NE(flat("\\B", "u").find("anchor @0+2 not-word-boundary"),
      std::string::npos);
}

TEST(EcmaScript, NumericEscapes) {
  struct {
    const char * pattern;
    const char * flags;
    uint32_t codepoint;
  } cases[] = {
    {"\\x41", "u", 'A'},
    {"\\u0041", "u", 'A'},
    {"\\u{41}", "u", 'A'},
    {"\\u{1F41F}", "u", 0x1F41F},
    // A surrogate pair is one code point in Unicode mode, because the source
    // is UTF-16 and that pair is how an astral character is written in it.
    {"\\uD83D\\uDC1F", "u", 0x1F41F},
    {"\\cA", "u", 1},
    {"\\ca", "u", 1},
    {"\\n", "u", '\n'},
    {"\\t", "u", '\t'},
    {"\\v", "u", 0x0B},
    {"\\f", "u", 0x0C},
    {"\\r", "u", '\r'},
    {"\\0", "u", 0},
    {"\\$", "u", '$'},
    {"\\/", "u", '/'},
  };

  for (const auto & test : cases) {
    Parsed parsed(test.pattern, test.flags);
    ASSERT_TRUE(parsed.ok()) << test.pattern;
    char encoded[24] = {0};
    if (test.codepoint >= 0x20 && test.codepoint < 0x7F
        && test.codepoint != '"' && test.codepoint != '\\') {
      snprintf(encoded, sizeof(encoded), "\"%c\"", (char)test.codepoint);
    }
    else if (test.codepoint < 0x100) {
      snprintf(encoded, sizeof(encoded), "\"\\x%02X\"", test.codepoint);
    }
    else {
      snprintf(encoded, sizeof(encoded), "\"\\u{%X}\"", test.codepoint);
    }
    EXPECT_NE(parsed.tree().find(encoded), std::string::npos)
        << test.pattern << " gave " << parsed.tree();
  }

  // `\u{...}` is a Unicode-mode form. Without `u` the same text is the
  // identity escape `u` followed by a `{110000}` quantifier, which is why
  // Node accepts it and means something quite different by it.
  EXPECT_EQ(Parsed("\\u{110000}", "u").result(), GRX_ERR_SYNTAX);

  // `\0` followed by a digit is octal in Annex B and an error with `u`.
  EXPECT_EQ(Parsed("\\00", "u").result(), GRX_ERR_SYNTAX);
  EXPECT_TRUE(Parsed("\\00", "").ok());
}

// --------------------------------------------------------------------------
// Character classes
// --------------------------------------------------------------------------

TEST(EcmaScript, ClassSyntax) {
  // ECMAScript is the only tier-1 dialect where a leading `]` closes the
  // class rather than being a literal, so `[]` is the empty class and `[^]`
  // is every code point.
  EXPECT_EQ(flat("[]"), "class @0+2 [] |");
  EXPECT_EQ(flat("[^]"), "class @0+3 [^] |");

  EXPECT_EQ(flat("[abc]"), "class @0+5 [a b c] |");
  EXPECT_EQ(flat("[a-z]"), "class @0+5 [a-z] |");
  EXPECT_EQ(flat("[^a-z0-9_]"), "class @0+10 [^a-z 0-9 _] |");

  // A dash at either end is a literal.
  EXPECT_EQ(flat("[-a]"), "class @0+4 [- a] |");
  EXPECT_EQ(flat("[a-]"), "class @0+4 [a -] |");

  EXPECT_EQ(flat("[\\d\\s]"), "class @0+6 [\\d \\s] |");
  EXPECT_EQ(flat("[\\b]"), "class @0+4 [\\x08] |");
}

TEST(EcmaScript, AClassEscapeAtARangeEndIsAUnionWithoutU) {
  // Annex B NonemptyClassRangesNoDash: `[\d-z]` is `\d`, a literal `-` and
  // `z`, not a range. Unicode mode makes it an error instead, which is the
  // more useful behaviour and not the one the web runs.
  EXPECT_EQ(flat("[\\d-z]"), "class @0+6 [\\d - z] |");

  Parsed unicode("[\\d-z]", "u");
  EXPECT_EQ(unicode.result(), GRX_ERR_SYNTAX);
  EXPECT_EQ(unicode.diag(), GRX_DIAG_CLASS_ESCAPE_IN_RANGE);
}

TEST(EcmaScript, PropertyEscapesExistOnlyInUnicodeMode) {
  EXPECT_EQ(flat("\\p{Lu}", "u"), "class @0+6 [\\p{Lu}] |");
  EXPECT_EQ(flat("\\P{Lu}", "u"), "class @0+6 [\\P{Lu}] |");
  EXPECT_EQ(flat("[\\p{L}a]", "u"), "class @0+8 [\\p{L} a] |");

  // Resolved at parse time, not at lowering: ECMA-262 makes an unknown
  // property an early SyntaxError, and a caller showing a user where their
  // pattern went wrong needs the offset.
  Parsed unknown("\\p{Nosuch}", "u");
  EXPECT_EQ(unknown.result(), GRX_ERR_SYNTAX);
  EXPECT_EQ(unknown.diag(), GRX_DIAG_UNKNOWN_PROPERTY);
  EXPECT_EQ(unknown.offset(), 0u);

  // Strict spelling: a lone script value is a SyntaxError, the qualified
  // form is not. Checked against Node 22.
  EXPECT_EQ(Parsed("\\p{Greek}", "u").result(), GRX_ERR_SYNTAX);
  EXPECT_TRUE(Parsed("\\p{Script=Greek}", "u").ok());
  EXPECT_TRUE(Parsed("\\p{sc=Grek}", "u").ok());
  EXPECT_TRUE(Parsed("\\p{scx=Greek}", "u").ok());
  EXPECT_EQ(Parsed("\\p{lu}", "u").result(), GRX_ERR_SYNTAX);

  Parsed malformed("\\p{", "u");
  EXPECT_EQ(malformed.result(), GRX_ERR_SYNTAX);
  EXPECT_EQ(malformed.diag(), GRX_DIAG_INVALID_PROPERTY_SYNTAX);
}

TEST(EcmaScript, UnicodeSetsModeReadsADifferentClassGrammar) {
  // The `v` flag is a different language inside a class, not `u` with
  // extras: `--` is a subtraction where `u` reads two literal dashes, and a
  // bare `-` is a syntax error where `u` reads a literal. The rest of the
  // grammar is tested in test_unicodesets.cpp; this is here because it is
  // the one place a *parse* test would notice the two modes diverging.
  // Each of these is valid in one mode and a syntax error in the other, in
  // opposite directions - which is the shortest statement of "different
  // grammar" there is. Node 22.23 agrees on all four.
  EXPECT_EQ(Parsed("[[a-z]--[aeiou]]", "v").result(), GRX_OK);
  EXPECT_EQ(Parsed("[[a-z]--[aeiou]]", "u").result(), GRX_ERR_SYNTAX);
  EXPECT_EQ(Parsed("[a-]", "u").result(), GRX_OK);
  EXPECT_EQ(Parsed("[a-]", "v").result(), GRX_ERR_SYNTAX);
}

// --------------------------------------------------------------------------
// Dialects that are named but not built
// --------------------------------------------------------------------------

TEST(Parse, AnUnbuiltDialectSaysSoRatherThanGuessing) {
  // Reading a Python pattern with the ECMAScript front end would tell a
  // caller their pattern is valid for an engine that rejects it. Every dialect
  // whose hooks are not written reports GRX_ERR_UNSUPPORTED instead.
  //
  // The exclusion list is the list of front ends that exist, and it is meant
  // to shrink this test rather than to grow silently: a dialect added here
  // without a front end would make the test pass by asserting nothing.
  for (int syntax = 0; syntax < GRX_SYNTAX_COUNT; syntax++) {
    if (syntax == GRX_SYNTAX_ECMASCRIPT || syntax == GRX_SYNTAX_PCRE
        || syntax == GRX_SYNTAX_PERL || syntax == GRX_SYNTAX_POSIX_BRE
        || syntax == GRX_SYNTAX_POSIX_ERE || syntax == GRX_SYNTAX_GNU_BRE
        || syntax == GRX_SYNTAX_GNU_ERE || syntax == GRX_SYNTAX_PYTHON
        || syntax == GRX_SYNTAX_VIM || syntax == GRX_SYNTAX_IREGEXP) {
      continue;
    }
    GRX_Pattern * parsed = nullptr;
    GRX_Error error;
    GRX_Result result = grx_pattern_parse_with_allocator("a", 1,
        (GRX_Syntax)syntax, 0, nullptr, nullptr, &error, &parsed);
    EXPECT_EQ(result, GRX_ERR_UNSUPPORTED) << grx_syntax_name((GRX_Syntax)syntax);
    EXPECT_EQ(error.diag, GRX_DIAG_DIALECT_NOT_IMPLEMENTED);
    EXPECT_EQ(parsed, nullptr);
  }
}

TEST(Parse, EveryFrontEndFillsTheHooksTheParserCallsUnconditionally) {
  // Six of the vtable's hooks are called without a NULL test, and the rest
  // are optional. A front end that leaves a mandatory one out does not fall
  // back to a default: it dereferences NULL, from inside a parse, on some
  // pattern that happens to reach that hook - `a**` was the one that found
  // it, so the crash was two dialects and a quantifier away from the missing
  // line.
  //
  // This is the cheap version of the fix. The expensive version would be to
  // give every hook a default, which would mean inventing a default answer
  // for questions like "what does `(` open here" that only a dialect can
  // answer. Naming them here instead means the next front end finds out at
  // `make test` rather than in a fuzzer.
  for (int syntax = 0; syntax < GRX_SYNTAX_COUNT; syntax++) {
    const GRX_Frontend * frontend = grx_frontend_for((GRX_Syntax)syntax);
    if (!frontend) {
      continue;
    }
    const char * name = grx_syntax_name((GRX_Syntax)syntax);
    EXPECT_NE(frontend->name, nullptr) << name;
    EXPECT_NE(frontend->atom_escape, nullptr) << name;
    EXPECT_NE(frontend->class_escape, nullptr) << name;
    EXPECT_NE(frontend->char_class, nullptr) << name;
    EXPECT_NE(frontend->group_open, nullptr) << name;
    EXPECT_NE(frontend->brace_quantifier, nullptr) << name;
    EXPECT_NE(frontend->literal_atom, nullptr) << name;
    EXPECT_NE(frontend->check_quantifier_target, nullptr) << name;
  }
}

TEST(Parse, ArgumentsItRefuses) {
  GRX_Pattern * parsed = nullptr;
  EXPECT_EQ(grx_pattern_parse(nullptr, GRX_SYNTAX_ECMASCRIPT, 0, &parsed),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_pattern_parse("a", GRX_SYNTAX_ECMASCRIPT, 0, nullptr),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_pattern_parse_with_allocator(nullptr, 1,
                GRX_SYNTAX_ECMASCRIPT, 0, nullptr, nullptr, nullptr, &parsed),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_pattern_parse_with_allocator("a", 1, GRX_SYNTAX_COUNT, 0,
                nullptr, nullptr, nullptr, &parsed),
      GRX_ERR_INVALID);

  // An empty pattern is valid and matches the empty string; NULL with a zero
  // length is the same thing said differently.
  ASSERT_EQ(grx_pattern_parse_with_allocator(nullptr, 0, GRX_SYNTAX_ECMASCRIPT,
                0, nullptr, nullptr, nullptr, &parsed),
      GRX_OK);
  EXPECT_EQ(grx_pattern_node_count(parsed), 1u);
  grx_pattern_free(parsed);
}

TEST(Parse, EverythingIsFreedThroughTheCallersAllocator) {
  grxtest::CountingAllocator allocator;

  GRX_Pattern * parsed = nullptr;
  GRX_Error error;
  const std::string pattern = "(?<n>[a-z\\d]+)\\k<n>|(x)*\\p{Lu}";
  ASSERT_EQ(grx_pattern_parse_with_allocator(pattern.data(), pattern.size(),
                GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, nullptr, allocator.get(),
                &error, &parsed),
      GRX_OK);
  EXPECT_GT(allocator.live(), 0);
  grx_pattern_free(parsed);
  EXPECT_EQ(allocator.live(), 0);

  // And on the failure path, which is the one that leaks: the pattern is
  // half built when the error is found.
  const std::string bad = "(?<n>[a-z]+)\\k<nosuch>";
  parsed = nullptr;
  EXPECT_EQ(grx_pattern_parse_with_allocator(bad.data(), bad.size(),
                GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, nullptr, allocator.get(),
                &error, &parsed),
      GRX_ERR_SYNTAX);
  EXPECT_EQ(parsed, nullptr);
  EXPECT_EQ(allocator.live(), 0) << "the half-built pattern leaked";
}

TEST(Parse, CaptureNumberingAndCounting) {
  // Four captures, numbered by opening parenthesis: `(a)`, `(c(d))`, the
  // nested `(d)`, and the named one. `(?:b)` is a group and not a capture.
  Parsed parsed("(a)(?:b)(c(d))(?<n>e)");
  ASSERT_TRUE(parsed.ok());
  EXPECT_EQ(grx_pattern_capture_count(parsed.get()), 4u);

  const std::string tree = parsed.tree();
  for (const char * expected : {"#1", "#2", "#3", "#4"}) {
    EXPECT_NE(tree.find(expected), std::string::npos) << expected << tree;
  }
  EXPECT_NE(tree.find("non-capturing"), std::string::npos) << tree;

  // GRX_OPT_NO_CAPTURE makes every `(` a group and none of them a capture,
  // which is the option a caller sets when they only want to know whether
  // the pattern matched.
  GRX_Pattern * uncaptured = nullptr;
  GRX_Error error;
  ASSERT_EQ(grx_pattern_parse_with_allocator("(a)(b)", 6,
                GRX_SYNTAX_ECMASCRIPT, GRX_OPT_NO_CAPTURE, nullptr, nullptr,
                &error, &uncaptured),
      GRX_OK);
  EXPECT_EQ(grx_pattern_capture_count(uncaptured), 0u);
  grx_pattern_free(uncaptured);
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
