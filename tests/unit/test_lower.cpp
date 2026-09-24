/**
 * @file
 *
 * Lowering, analysis and code generation: what a pattern becomes.
 *
 * The claim these tests are for is the one the whole design rests on: after
 * lowering, the dialect is gone. Every rule a dialect chose has become an
 * explicit class, mode or opcode, and the tests below say which - so that a
 * second dialect implementing the same rule reuses the mode rather than
 * adding a branch.
 *
 * What a pattern *matches* is checked against Node by
 * tools/oracle/match_diff.py over tens of thousands of rows. These are for
 * the things a span comparison cannot see: which instruction came out, what
 * the facts say, and where a limit fires.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <cstdint>
#include <string>

#include "test_helpers.h"

#include "../../src/compile/compile_internal.h"
#include "../../src/ir/lower_internal.h"

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
      default: break;
    }
  }
  return options;
}

/** A compiled regex that frees itself, with its error kept. */
class Compiled {
public:
  Compiled(const std::string & pattern, const std::string & flags = "",
      const GRX_Limits * limits = nullptr,
      GRX_Syntax syntax = GRX_SYNTAX_ECMASCRIPT) {
    grx_error_clear(&error_);
    result_ = grx_regex_compile_with_allocator(pattern.data(), pattern.size(),
        syntax, flags_to_options(flags), limits, nullptr, &error_, &regex_);
  }
  Compiled(const Compiled &) = delete;
  Compiled & operator=(const Compiled &) = delete;
  ~Compiled() { grx_regex_free(regex_); }

  GRX_Result result() const { return result_; }
  GRX_Diag diag() const { return error_.diag; }
  GRX_Regex * get() const { return regex_; }
  bool ok() const { return result_ == GRX_OK; }

  GRX_Facts facts() const {
    GRX_Facts facts;
    grx_facts_init(&facts);
    if (regex_) {
      grx_regex_facts(regex_, &facts);
    }
    return facts;
  }

  std::string disassembly() const {
    if (!regex_) {
      return "<failed>";
    }
    return grxtest::capture_dump(
        [this](FILE * out) { grx_regex_dump(regex_, out); });
  }

private:
  GRX_Regex * regex_ = nullptr;
  GRX_Result result_ = GRX_ERR_INTERNAL;
  GRX_Error error_ {};
};

/** Whether the compiled program contains an opcode. */
bool uses(const Compiled & compiled, const char * mnemonic) {
  return compiled.disassembly().find(std::string(" ") + mnemonic + " ")
      != std::string::npos;
}

/** Search a subject and return the spans as text, or "nomatch". */
std::string spans(const Compiled & compiled, const std::string & subject,
    GRX_Engine engine = GRX_ENGINE_AUTO) {
  if (!compiled.ok()) {
    return "<failed>";
  }

  GRX_Match * match = nullptr;
  if (grx_match_create(compiled.get(), nullptr, &match) != GRX_OK) {
    return "<oom>";
  }

  int matched = 0;
  GRX_Result result = grx_regex_search(compiled.get(), subject.data(),
      subject.size(), 0, engine, nullptr, match, &matched);

  std::string out;
  if (result != GRX_OK) {
    out = std::string("<") + grx_result_string(result) + ">";
  }
  else if (!matched) {
    out = "nomatch";
  }
  else {
    for (size_t i = 0; i < grx_match_count(match); i++) {
      GRX_Capture capture;
      grx_match_group(match, i, &capture);
      if (i) {
        out += " ";
      }
      out += capture.start == GRX_NPOS
          ? std::string("-")
          : std::to_string(capture.start) + ":" + std::to_string(capture.end);
    }
  }

  grx_match_destroy(match);
  return out;
}

} // namespace

// --------------------------------------------------------------------------
// The dialect is gone
// --------------------------------------------------------------------------

TEST(Lower, OptionsBecomeExplicitAndDoNotSurvive) {
  // documentation/design.md section 3.2. Each of these is an option in the
  // pattern and something explicit in the program, and the program is what an
  // engine sees.

  // Caseless folds a literal into the class of everything that matches it, so
  // no engine ever folds anything.
  Compiled caseless("a", "i");
  ASSERT_TRUE(caseless.ok());
  EXPECT_TRUE(uses(caseless, "class"));
  EXPECT_FALSE(uses(caseless, "char"));
  EXPECT_EQ(spans(caseless, "A"), "0:1");

  Compiled plain("a", "");
  ASSERT_TRUE(plain.ok());
  EXPECT_TRUE(uses(plain, "char"));

  // Multiline chooses between two assertion kinds.
  EXPECT_NE(Compiled("^", "").disassembly().find("start-subject"),
      std::string::npos);
  EXPECT_NE(Compiled("^", "m").disassembly().find("start-line"),
      std::string::npos);

  // Dot-all chooses what `.` excludes: a class of line terminators, or
  // nothing at all, which is a different opcode.
  EXPECT_TRUE(uses(Compiled(".", ""), "any"));
  EXPECT_TRUE(uses(Compiled(".", "s"), "any-nl"));
}

TEST(Lower, TheDollarRuleIsTheProfilesAndNotTheEngines) {
  // documentation/dialects.md section 5.3. ECMAScript's `$` without `m` is
  // the end of the subject and nothing else - not "or before a final
  // newline", which is what the Perl family means by the same character.
  Compiled dollar("a$", "");
  ASSERT_TRUE(dollar.ok());
  EXPECT_NE(dollar.disassembly().find("end-subject"), std::string::npos);
  EXPECT_EQ(dollar.disassembly().find("end-before-newline"),
      std::string::npos);
  EXPECT_EQ(spans(dollar, "a\n"), "nomatch");
  EXPECT_EQ(spans(dollar, "a"), "0:1");

  Compiled multiline("a$", "m");
  ASSERT_TRUE(multiline.ok());
  EXPECT_NE(multiline.disassembly().find("end-line"), std::string::npos);
  EXPECT_EQ(spans(multiline, "a\nb"), "0:1");
}

TEST(Lower, TheTwoLoopRulesBecomeModesOnTheInstruction) {
  // The pair documentation/dialects.md section 5.5 is about. ECMAScript fails
  // an iteration that consumed nothing and clears the captures inside a
  // repeat at each iteration; the Perl family does neither. Both are modes
  // here, so a dialect choosing the other value costs no engine code.
  Compiled star("(a*)*", "");
  ASSERT_TRUE(star.ok());
  EXPECT_NE(star.disassembly().find("progress-check"), std::string::npos);
  EXPECT_NE(star.disassembly().find("(fail)"), std::string::npos);

  // `(a*)*` against "b": the outer loop's one iteration consumes nothing, so
  // ECMAScript fails it and group 1 is never set. Perl would report it as
  // the empty string. Checked against Node.
  EXPECT_EQ(spans(star, "b"), "0:0 -");

  // `(a*)+` against "b": the minimum forces one iteration, which is allowed
  // to be empty, so group 1 *is* set - to the empty string.
  EXPECT_EQ(spans(Compiled("(a*)+", ""), "b"), "0:0 0:0");

  // The capture reset, which is the other half. `((a)|b)+` against "ab"
  // reports group 2 as unset, because the second iteration cleared what the
  // first one set.
  Compiled reset("((a)|b)+", "");
  ASSERT_TRUE(reset.ok());
  EXPECT_TRUE(uses(reset, "reset"));
  EXPECT_EQ(spans(reset, "ab"), "0:2 1:2 -");

  // It applies to a *bounded* repeat too, for every iteration past the
  // minimum: ECMA-262 passes the remaining count into the same RepeatMatcher.
  EXPECT_EQ(spans(Compiled("(?:(a)|b){2}", ""), "ab"), "0:2 -");
  EXPECT_EQ(spans(Compiled("(?:(a)|b){2}", ""), "ba"), "0:2 1:2");
}

TEST(Lower, PerlClearsALoopsCapturesOnTheWayOutOfTheIteration) {
  // Both halves of the rule at once. ECMA-262 clears at the *start* of an
  // iteration (RepeatMatcher step 4); Perl clears at the end, and of an
  // iteration that ends without setting them. The two report the same spans
  // - after the last iteration, what it did not set is gone either way - and
  // differ only in what the *next* iteration can see.
  Compiled perl("((?(2)x|y)(a))+", "", nullptr, GRX_SYNTAX_PERL);
  ASSERT_TRUE(perl.ok());
  // The second iteration's conditional still sees what the first captured,
  // so it takes `x` and the whole subject matches. Clearing early would hide
  // group 2, take `y`, and stop after "ya". Checked against perl 5.40 and
  // pcre2test 10.46, which agree.
  EXPECT_EQ(spans(perl, "yaxa"), "0:4 2:4 3:4");
  EXPECT_EQ(spans(Compiled("((?(2)x|y)(a)){2}", "", nullptr, GRX_SYNTAX_PERL),
                "yaxa"), "0:4 2:4 3:4");

  // A group the repeated group itself names, which is the same question one
  // level up: `(?(1)` in the second iteration sees what the first captured.
  EXPECT_EQ(spans(Compiled("((?(1)a|b))+", "", nullptr, GRX_SYNTAX_PERL),
                "baaa"), "0:4 3:4");

  // And the reported answer is unchanged, which is the point: an iteration
  // that does not set a capture still takes it away.
  Compiled reset("((a)|b)+", "", nullptr, GRX_SYNTAX_PERL);
  ASSERT_TRUE(reset.ok());
  EXPECT_EQ(spans(reset, "ab"), "0:2 1:2 -");
  EXPECT_EQ(spans(reset, "ba"), "0:2 1:2 1:2");
  // Including to a conditional *after* the loop, where the clearing has
  // happened: perl does not match this either.
  EXPECT_EQ(spans(Compiled("((a)|b)+(?(2)x|y)", "", nullptr, GRX_SYNTAX_PERL),
                "abx"), "nomatch");

  // The late form costs a register, so it is emitted only where the
  // difference can be seen - by a conditional or a backreference. A pattern
  // with neither reports the same spans from the early form, one
  // instruction less, and stays memoizable.
  EXPECT_FALSE(uses(reset, "reset-stale")) << reset.disassembly();
  EXPECT_TRUE(uses(reset, "reset")) << reset.disassembly();
  EXPECT_TRUE(uses(perl, "reset-stale")) << perl.disassembly();

  // ECMAScript keeps the early form whatever the pattern contains: 22.2.2.3.1
  // says the clearing happens before the iteration runs.
  Compiled ecma("((a)|b)+", "u");
  ASSERT_TRUE(ecma.ok());
  EXPECT_FALSE(uses(ecma, "reset-stale")) << ecma.disassembly();
  EXPECT_EQ(spans(ecma, "ab"), "0:2 1:2 -");
}

TEST(Lower, AReferenceToAnOpenGroupReadsWhatItLastClosedWith) {
  // While a group is between its two SAVEs its slots hold a start from this
  // iteration and an end from the last, which is not a span anybody wrote.
  // perl 5.40 and pcre2test 10.46 both read the span the group last *closed*
  // with: in `^(a\1?){4}$` the second iteration's `\1` is the "a" the first
  // one took, so "aaaaaa" matches as 1 + 1 + 1 + (1 + "a").
  for (GRX_Syntax syntax : {GRX_SYNTAX_PERL, GRX_SYNTAX_PCRE}) {
    Compiled self("^(a\\1?){4}$", "", nullptr, syntax);
    ASSERT_TRUE(self.ok());
    EXPECT_EQ(spans(self, "aaaaaa"), "0:6 4:6");
    EXPECT_EQ(spans(self, "aaaaaaaaaa"), "0:10 6:10");
    // An odd length is reached by taking the empty branch more often, so it
    // matches too - checked against both references rather than assumed.
    EXPECT_EQ(spans(self, "aaaaa"), "0:5 4:5");
    EXPECT_EQ(spans(self, "aaa"), "nomatch");

    // A loop rather than a counted repeat, where the reference reaches back
    // one iteration each time round.
    EXPECT_EQ(spans(Compiled("(a\\1?)+", "", nullptr, syntax), "aaa"),
        "0:3 1:3");
  }

  // A group that has never closed is unset, not half-written: the `?` takes
  // the empty branch rather than the reference matching something.
  EXPECT_EQ(spans(Compiled("(a(b\\1)?)", "", nullptr, GRX_SYNTAX_PERL), "ab"),
      "0:1 0:1 -");

  // And a reference to a *closed* group reads what it always did - the two
  // are the same span whenever the group is not open, which is why nothing
  // else in the corpus moved.
  EXPECT_EQ(spans(Compiled("(x)(\\1y)", "", nullptr, GRX_SYNTAX_PERL), "xxy"),
      "0:3 0:1 1:3");
  // Including after a loop has cleared it: the clearing takes the remembered
  // span away too, so perl does not match this and neither does this.
  EXPECT_EQ(spans(Compiled("((a)|b)+\\2c", "", nullptr, GRX_SYNTAX_PERL),
                "abac"), "nomatch");

  // Inside a lookbehind the body runs backwards and a group writes its *end*
  // first, so the slot that closes it is the other one. Keying on the odd
  // slot alone broke eight ECMAScript records; this is one of them.
  EXPECT_EQ(spans(Compiled("(?<=([abc]+)).\\1", "u"), "aaa"), "1:3 0:1");
  EXPECT_EQ(spans(Compiled("(?<=(.))(\\w+)(?=\\1)", "u"), "aaa"),
      "1:2 0:1 1:2");
}

TEST(Lower, WhatANegativeLookaroundLeavesInTheCaptureSlots) {
  // documentation/dialects.md section 5.17. A negative lookaround succeeds by
  // having its body fail, and the body may have captured on its way to
  // failing. ECMA-262 22.2.2.4 discards those writes; Perl keeps them.
  // Probed three ways: perl 5.40 reports group 1 here, pcre2test 10.46 and
  // Node report it unset.
  EXPECT_EQ(spans(Compiled("a(?!(b)c)", "", nullptr, GRX_SYNTAX_PERL), "abd"),
      "0:1 1:2");
  EXPECT_EQ(spans(Compiled("a(?!(b)c)", "", nullptr, GRX_SYNTAX_PCRE), "abd"),
      "0:1 -");
  EXPECT_EQ(spans(Compiled("a(?!(b)c)", "u"), "abd"), "0:1 -");

  // A body that never got as far as the capture leaves it unset in all three:
  // there is nothing to keep.
  EXPECT_EQ(spans(Compiled("(x)(?!(y))z", "", nullptr, GRX_SYNTAX_PERL), "xz"),
      "0:2 0:1 -");

  // A *positive* lookaround needs no rule. One that succeeded keeps what its
  // body captured everywhere, and one that failed takes the construct with
  // it, so there is nothing left to disagree about.
  EXPECT_EQ(spans(Compiled("a(?=(b))", "", nullptr, GRX_SYNTAX_PERL), "ab"),
      "0:1 1:2");
  EXPECT_EQ(spans(Compiled("a(?=(b))", "u"), "ab"), "0:1 1:2");
}

TEST(Lower, AcceptInsideASubroutineCallReturnsFromIt) {
  // pcre2pattern: "in a recursion or subroutine call, (*ACCEPT) causes only
  // that subroutine to return". Perl agrees, and both match the whole of
  // "xz" - ending the match at the verb would stop at the `x`.
  for (GRX_Syntax syntax : {GRX_SYNTAX_PERL, GRX_SYNTAX_PCRE}) {
    Compiled called(
        "(?(DEFINE)(?<a>x(*ACCEPT)y))(?&a)z", "", nullptr, syntax);
    ASSERT_TRUE(called.ok());
    EXPECT_EQ(spans(called, "xz"), "0:2 -");

    // Reached outside any call, the same verb ends the match - which is what
    // it does everywhere else, and what this must not have changed.
    Compiled direct("(a(*ACCEPT)b)c", "", nullptr, syntax);
    ASSERT_TRUE(direct.ok());
    EXPECT_EQ(spans(direct, "ac"), "0:1 0:1");

    // Inside a call, the groups the call opened are still closed at the verb,
    // and then thrown away on return like any other call's captures.
    Compiled both("(a(*ACCEPT)b)(?1)c", "", nullptr, syntax);
    ASSERT_TRUE(both.ok());
    EXPECT_EQ(spans(both, "aac"), "0:1 0:1");
  }
}

TEST(Lower, FoldingHappensBeforeNegationAndNotAfter) {
  // The rule that decides whether `/[^a]/i` matches "A". A caseless class is
  // the closure of its positive content, *then* complemented; the other order
  // gives a class that matches "A" and is wrong in every dialect that has
  // both features.
  EXPECT_EQ(spans(Compiled("[^a]", "i"), "A"), "nomatch");
  EXPECT_EQ(spans(Compiled("[^a]", "i"), "b"), "0:1");

  // The same rule one level down. Under `iu`, ECMA-262's `\w` gains U+017F
  // and U+212A because folding put them there - so `\W`, which is the
  // complement of the folded set, must not match them either.
  const std::string long_s = "\xC5\xBF";  // U+017F
  const std::string kelvin = "\xE2\x84\xAA"; // U+212A
  EXPECT_EQ(spans(Compiled("\\w", "iu"), long_s), "0:2");
  EXPECT_EQ(spans(Compiled("\\w", "iu"), kelvin), "0:3");
  EXPECT_EQ(spans(Compiled("\\W", "iu"), long_s), "nomatch");
  EXPECT_EQ(spans(Compiled("\\W", "iu"), kelvin), "nomatch");

  // Without the caseless flag, neither is a word character in either mode.
  EXPECT_EQ(spans(Compiled("\\w", "u"), long_s), "nomatch");
  EXPECT_EQ(spans(Compiled("\\W", "u"), long_s), "0:2");
}

TEST(Lower, OnlyECMAScriptWidensAShorthandByFolding) {
  // ECMA-262 22.2.2.9.3 puts every character that canonicalises to a word
  // character into WordCharacters, so `\w` under `iu` gains U+017F and
  // U+212A and `\b` reads them as word characters. That is written into one
  // specification and into no other, and this library applied it to every
  // dialect - which made three references wrong at once. Measured against
  // each, with `\w` narrowed to ASCII so that the question is the folding
  // and not the width: `(?i)\w` over U+017F is no match in pcre2test 10.46,
  // in perl 5.40.1 under `/ai`, and in CPython 3.13 under `(?ai)`.
  const std::string long_s = "\xC5\xBF";     // U+017F
  const std::string kelvin = "\xE2\x84\xAA"; // U+212A

  EXPECT_EQ(spans(Compiled("(?i)\\w", "u", nullptr, GRX_SYNTAX_PCRE),
      long_s), "nomatch");
  EXPECT_EQ(spans(Compiled("(?i)\\w", "u", nullptr, GRX_SYNTAX_PCRE),
      kelvin), "nomatch");
  // Both of these need the `i` as well as the `a`: without a caseless mode
  // there is no folding to widen anything, so `(?a)\\w` passes whatever
  // this rule says and states nothing.
  EXPECT_EQ(spans(Compiled("(?ai)\\w", "", nullptr, GRX_SYNTAX_PERL),
      long_s), "nomatch");
  EXPECT_EQ(spans(Compiled("(?ai)\\w", "", nullptr, GRX_SYNTAX_PYTHON),
      long_s), "nomatch");

  // The literal and the range are a different question and fold in all
  // four, which is what says this is the shorthand's rule and not the
  // class's: `(?i)s` and `(?i)[a-z]` both match U+017F in pcre2test.
  EXPECT_EQ(spans(Compiled("(?i)s", "u", nullptr, GRX_SYNTAX_PCRE),
      long_s), "0:2");
  EXPECT_EQ(spans(Compiled("(?i)[a-z]", "u", nullptr, GRX_SYNTAX_PCRE),
      long_s), "0:2");
  // And a shorthand written inside a class takes the shorthand's rule, not
  // the neighbouring range's.
  EXPECT_EQ(spans(Compiled("(?i)[\\w]", "u", nullptr, GRX_SYNTAX_PCRE),
      long_s), "nomatch");

  // `\b` is the same set seen from the other side. `(?i)x\b` over "x"
  // U+212A is 0-1 in all three references, the boundary being there
  // because U+212A is not a word character; it was no match here.
  EXPECT_EQ(spans(Compiled("(?i)x\\b", "u", nullptr, GRX_SYNTAX_PCRE),
      "x" + kelvin), "0:1");
  EXPECT_EQ(spans(Compiled("(?i)s\\b", "u", nullptr, GRX_SYNTAX_PCRE),
      "s" + long_s), "0:1");

  // ECMAScript keeps all of it, which is the control: a change that simply
  // stopped folding shorthands would pass every line above.
  EXPECT_EQ(spans(Compiled("\\w", "iu"), long_s), "0:2");
  EXPECT_EQ(spans(Compiled("[\\w]", "iu"), long_s), "0:2");
  EXPECT_EQ(spans(Compiled("x\\b", "iu"), "x" + kelvin), "nomatch");
  EXPECT_EQ(spans(Compiled("s\\b", "iu"), "s" + long_s), "1:3");
}

TEST(Lower, TheWordSetIsReDerivedWhenAModifierMovesIt) {
  // `\b` reads a word set that the lowering interns once and hands to every
  // `\b` in the pattern. An inline modifier moves it, and the cache did not
  // notice: the second assertion read the set the first one had left.
  //
  // U+0100 is a word character to perl's Unicode shorthands and not to its
  // ASCII ones, so a boundary between it and "x" exists under `/a` and does
  // not otherwise. Both directions, because one fix could be an accident:
  // perl 5.40.1 answers 1-2, 1-2, no match and no match for these four.
  const std::string subject = "\xC4\x80x";  // U+0100 "x"

  EXPECT_EQ(spans(Compiled("(?a)\\bx", "", nullptr, GRX_SYNTAX_PERL),
      subject), "2:3");
  EXPECT_EQ(spans(Compiled("(?:\\b|)(?a)\\bx", "", nullptr,
      GRX_SYNTAX_PERL), subject), "2:3");
  EXPECT_EQ(spans(Compiled("(?u)\\bx", "", nullptr, GRX_SYNTAX_PERL),
      subject), "nomatch");
  EXPECT_EQ(spans(Compiled("(?a)(?:\\b|)(?u)\\bx", "", nullptr,
      GRX_SYNTAX_PERL), subject), "nomatch");
}

TEST(Lower, TheTwoFoldingsAreDifferentFunctions) {
  // documentation/dialects.md section 5.8. ECMAScript without `u` uses
  // Canonicalize, which refuses to map a non-ASCII code point into ASCII;
  // with `u` it uses simple case folding, which does not care. The Kelvin
  // sign is the character that tells them apart.
  const std::string kelvin = "\xE2\x84\xAA";
  EXPECT_EQ(spans(Compiled("k", "iu"), kelvin), "0:3");
  EXPECT_EQ(spans(Compiled("k", "i"), kelvin), "nomatch");

  const std::string long_s = "\xC5\xBF";
  EXPECT_EQ(spans(Compiled("[a-z]", "iu"), long_s), "0:2");
  EXPECT_EQ(spans(Compiled("[a-z]", "i"), long_s), "nomatch");
}

TEST(Lower, AFullFoldRunBecomesAGraphOfOrdinaryClassMatches) {
  // documentation/design.md section 5.2. Full folding cannot be a class, so
  // the run is matched against its fold as a small graph - but the graph is
  // emitted as instructions the engines already had, which is the whole
  // reason none of them needed changing.
  const std::string sharp_s = "\xC3\x9F";
  Compiled folded("(?i)ss", "", nullptr, GRX_SYNTAX_PERL);
  ASSERT_TRUE(folded.ok());

  // Two positions to cross, so two one-character edges and one that crosses
  // both - and a SPLIT to choose. No new opcode appears.
  EXPECT_TRUE(uses(folded, "split")) << folded.disassembly();
  EXPECT_TRUE(uses(folded, "class")) << folded.disassembly();
  EXPECT_FALSE(uses(folded, "char")) << folded.disassembly();
  EXPECT_EQ(spans(folded, sharp_s), "0:2");
  EXPECT_EQ(spans(folded, "ss"), "0:2");

  // Every engine runs it, because a program of splits and classes is
  // regular. That is the property the graph was shaped to keep.
  GRX_Facts facts = folded.facts();
  EXPECT_TRUE(facts.is_regular);
  EXPECT_EQ(spans(folded, sharp_s, GRX_ENGINE_PIKE), "0:2");
  EXPECT_EQ(spans(folded, sharp_s, GRX_ENGINE_BACKTRACK), "0:2");

  // A run that does not need it is still one class per code point: `abc`
  // has no full fold anywhere in it and no fold of a subject character can
  // cover two of its positions.
  Compiled plain("(?i)abc", "", nullptr, GRX_SYNTAX_PERL);
  ASSERT_TRUE(plain.ok());
  EXPECT_FALSE(uses(plain, "split")) << plain.disassembly();
  EXPECT_EQ(spans(plain, "ABC"), "0:3");

  // And the graph works inside a lookbehind, where the body is matched
  // forwards from a candidate start rather than backwards from here: three
  // bytes back is what the two-position graph needs and what it is given.
  // Perl is the only dialect that folds fully, and Perl is one of the two
  // that bound their lookbehind, so the reverse arm of this graph is code no
  // dialect reaches today - it is still there for the dialect that folds
  // fully *and* leaves its lookbehind unbounded, which none does yet.
  Compiled behind("(?i)x(?<=" + sharp_s + "x)", "", nullptr, GRX_SYNTAX_PERL);
  ASSERT_TRUE(behind.ok());
  EXPECT_EQ(behind.disassembly().find("reverse"), std::string::npos)
      << behind.disassembly();
  EXPECT_EQ(spans(behind, "ssx"), "2:3");
  EXPECT_EQ(spans(behind, std::string(sharp_s) + "x"), "2:3");
}

TEST(Lower, WhichLookbehindModelIsTheDialectsAndNotTheEngines) {
  // documentation/design.md section 3.5.2. There are two ways to match a
  // lookbehind and the dialect picks, because the pick costs what the
  // dialect is willing to pay: running the body backwards from here costs
  // what the body costs, however far back it reaches, and trying every start
  // the body's length allows costs the *variation* - which only a dialect
  // that bounds it has bounded.
  Compiled ecma("(?<=a{1,3})b", "u");
  ASSERT_TRUE(ecma.ok());
  EXPECT_NE(ecma.disassembly().find("reverse"), std::string::npos)
      << ecma.disassembly();
  EXPECT_EQ(ecma.disassembly().find("forward="), std::string::npos)
      << ecma.disassembly();

  Compiled perl("(?<=a{1,3})b", "", nullptr, GRX_SYNTAX_PERL);
  ASSERT_TRUE(perl.ok());
  EXPECT_EQ(perl.disassembly().find("reverse"), std::string::npos)
      << perl.disassembly();
  EXPECT_NE(perl.disassembly().find("forward=1..3"), std::string::npos)
      << perl.disassembly();

  // Both answer the assertion the same way when the body has one length to
  // find. The models are only distinguishable where the body has several.
  EXPECT_EQ(spans(ecma, "aab"), "2:3");
  EXPECT_EQ(spans(perl, "aab"), "2:3");
  EXPECT_EQ(spans(ecma, "b"), "nomatch");
  EXPECT_EQ(spans(perl, "b"), "nomatch");

  // Where they differ: which of several candidate starts wins, and so what a
  // capture inside the body holds. Perl takes the longest body, whatever the
  // body itself would prefer; running it backwards takes whatever the body
  // prefers, and `a??` prefers nothing at all. Both are in the corpus.
  Compiled lazy("(?=foo)(?<=(a?\?))", "", nullptr, GRX_SYNTAX_PERL);
  ASSERT_TRUE(lazy.ok());
  EXPECT_EQ(spans(lazy, "afoo"), "1:1 0:1");

  Compiled branches("(?=foo)(?<=(|a|aa))", "", nullptr, GRX_SYNTAX_PERL);
  ASSERT_TRUE(branches.ok());
  EXPECT_EQ(spans(branches, "aafoo"), "2:2 0:2");

  // And where a verb can end the body: matching forwards gives `(*ACCEPT)`
  // somewhere to stop, which is why the assertion holds one character back
  // from a body that would otherwise need five.
  Compiled accept(
      "(?<=([cd](*ACCEPT)|x)gggg)blrph", "", nullptr, GRX_SYNTAX_PERL);
  ASSERT_TRUE(accept.ok());
  EXPECT_EQ(spans(accept, "cblrph"), "1:6 0:1");
  EXPECT_EQ(spans(accept, "xggggblrph"), "5:10 0:1");
  EXPECT_EQ(spans(accept, "zblrph"), "nomatch");

  // A non-atomic lookbehind keeps the reverse model whatever the dialect
  // says, because it is inlined rather than run as a sub-match - that is
  // what makes it non-atomic - and a candidate-start loop has nowhere to put
  // the backtrack points that has to leave live.
  Compiled non_atomic("(?<*ab)c", "", nullptr, GRX_SYNTAX_PCRE);
  ASSERT_TRUE(non_atomic.ok());
  EXPECT_NE(non_atomic.disassembly().find("reverse"), std::string::npos)
      << non_atomic.disassembly();
  EXPECT_EQ(spans(non_atomic, "abc"), "2:3");
}

TEST(Lower, AnAlternativeThatCannotSpanTheDistanceIsNotTried) {
  // The candidate-start model tries the furthest start first and requires
  // the body to arrive exactly where the assertion stands, so an alternative
  // shorter or longer than what is left cannot be the one - and finding that
  // out by walking it is what makes a bounded lookbehind cost the bound
  // rather than a constant. The guard says so first.
  Compiled guarded("(?<=(a|aa|aaa))b", "", nullptr, GRX_SYNTAX_PERL);
  ASSERT_TRUE(guarded.ok());
  EXPECT_NE(guarded.disassembly().find("look-length"), std::string::npos)
      << guarded.disassembly();
  EXPECT_EQ(spans(guarded, "aaab"), "3:4 0:3");
  EXPECT_EQ(spans(guarded, "aab"), "2:3 0:2");
  EXPECT_EQ(spans(guarded, "ab"), "1:2 0:1");
  EXPECT_EQ(spans(guarded, "b"), "nomatch");

  // Only where the alternative has to span the distance *alone*. With
  // something after it inside the body, the distance is shared, and a guard
  // that asked one alternative for all of it would throw away the branch
  // that matches: `x` is one byte of the five this assertion needs, and
  // `gggg` is the other four.
  Compiled shared(
      "(?<=([cd](*ACCEPT)|x)gggg)blrph", "", nullptr, GRX_SYNTAX_PERL);
  ASSERT_TRUE(shared.ok());
  EXPECT_EQ(shared.disassembly().find("look-length"), std::string::npos)
      << shared.disassembly();
  EXPECT_EQ(spans(shared, "xggggblrph"), "5:10 0:1");

  // Nor inside a repeat, where one iteration is followed by another rather
  // than by the end of the body.
  Compiled repeated("(?<=(a|aa){2})b", "", nullptr, GRX_SYNTAX_PERL);
  ASSERT_TRUE(repeated.ok());
  EXPECT_EQ(repeated.disassembly().find("look-length"), std::string::npos)
      << repeated.disassembly();
  EXPECT_EQ(spans(repeated, "aaab"), "3:4 1:3");

  // And never for a dialect that runs the body backwards: there is no end to
  // measure a distance to.
  Compiled ecma("(?<=(a|aa|aaa))b", "u");
  ASSERT_TRUE(ecma.ok());
  EXPECT_EQ(ecma.disassembly().find("look-length"), std::string::npos)
      << ecma.disassembly();
}

TEST(Lower, ALookbehindBodyIsCompiledBackwards) {
  // documentation/design.md section 3.5.2: a lookbehind body is an ordinary
  // sub-program whose instructions step backwards. That is what lets it be
  // any length without a second engine, and it is visible in the
  // disassembly: the body's parts come out in the opposite order.
  Compiled behind("(?<=ab)c", "u");
  ASSERT_TRUE(behind.ok());

  const std::string text = behind.disassembly();
  size_t b = text.find("char           'b'");
  size_t a = text.find("char           'a'");
  ASSERT_NE(b, std::string::npos) << text;
  ASSERT_NE(a, std::string::npos) << text;
  EXPECT_LT(b, a) << "the body should be emitted last-character-first:\n"
                  << text;
  EXPECT_NE(text.find("reverse"), std::string::npos) << text;
}

// --------------------------------------------------------------------------
// Facts
// --------------------------------------------------------------------------

TEST(Facts, RegularityIsWhatDecidesWhichEngineMayRun) {
  // The fact a caller wants *before* running anything: whether the
  // linear-time engine can run this program, and so whether matching it
  // against hostile input is safe.
  struct {
    const char * pattern;
    const char * flags;
    bool regular;
  } cases[] = {
    {"a(b|c)*d", "", true},
    {"^[a-z]+$", "", true},
    {"\\p{L}{2,4}", "u", true},
    {"(a)\\1", "", false},       // A backreference.
    {"(?=a)b", "", false},       // A lookahead.
    {"(?<=a)b", "u", false},     // A lookbehind.
  };

  for (const auto & test : cases) {
    Compiled compiled(test.pattern, test.flags);
    ASSERT_TRUE(compiled.ok()) << test.pattern;
    EXPECT_EQ(compiled.facts().is_regular != 0, test.regular)
        << test.pattern;

    // And the routing follows from it: a caller who asks for the Pike VM by
    // name is told it cannot be done rather than quietly given the engine
    // whose worst case is exponential.
    if (!test.regular) {
      EXPECT_EQ(spans(compiled, "ab", GRX_ENGINE_PIKE),
          std::string("<") + grx_result_string(GRX_ERR_UNSUPPORTED) + ">")
          << test.pattern;
    }
  }
}

TEST(Facts, LengthsAndAnchoringAreConservative) {
  struct {
    const char * pattern;
    size_t min;
    size_t max;
    bool anchored_start;
    bool anchored_end;
    bool can_be_empty;
  } cases[] = {
    {"abc", 3, 3, false, false, false},
    {"a?", 0, 1, false, false, true},
    {"a*", 0, GRX_NPOS, false, false, true},
    {"a{2,4}", 2, 4, false, false, false},
    {"^abc", 3, 3, true, false, false},
    {"abc$", 3, 3, false, true, false},
    {"^abc$", 3, 3, true, true, false},
    {"^a|^b", 1, 1, true, false, false},
    {"^a|b", 1, 1, false, false, false}, // One branch is not anchored.
    {"(?:)", 0, 0, false, false, true},
    {"ab|cde", 2, 3, false, false, false},
  };

  for (const auto & test : cases) {
    Compiled compiled(test.pattern, "");
    ASSERT_TRUE(compiled.ok()) << test.pattern;
    GRX_Facts facts = compiled.facts();
    EXPECT_EQ(facts.min_length, test.min) << test.pattern << " min";
    EXPECT_EQ(facts.max_length, test.max) << test.pattern << " max";
    EXPECT_EQ(facts.anchored_start != 0, test.anchored_start)
        << test.pattern << " anchored start";
    EXPECT_EQ(facts.anchored_end != 0, test.anchored_end)
        << test.pattern << " anchored end";
    EXPECT_EQ(facts.can_match_empty != 0, test.can_be_empty)
        << test.pattern << " can match empty";
  }
}

TEST(Facts, AnchoringSurvivesAZeroWidthPartAndNotAConsumingOne) {
  // The arithmetic that is easy to get one step wrong. A zero-width part in
  // front of a `^` does not move where the match begins, so `\b^a` is still
  // anchored; a part that can consume does, so `a^` is not. The mirror image
  // holds at the end.
  struct {
    const char * pattern;
    bool start;
    bool end;
  } cases[] = {
    {"^a", true, false},
    {"\\b^a", true, false},
    {"(?:)^a", true, false},
    {"a^", false, false},
    {"a$", false, true},
    {"a$\\b", false, true},
    {"$a", false, false},
    {"^a$", true, true},
    // `^` alone is anchored at the start and not at the end: it matches the
    // empty string at position 0, and that is the end of the subject only
    // when the subject is empty.
    {"^", true, false},
    {"$", false, true},
    {"^$", true, true},
    {"^a*", true, false},
    {"(^a)", true, false},   // Through a capture.
    {"(?:^a)+", true, false}, // A repeat that must run keeps its anchoring.
    {"(?:^a)*", false, false}, // One that may not, does not.
  };

  for (const auto & test : cases) {
    Compiled compiled(test.pattern, "");
    ASSERT_TRUE(compiled.ok()) << test.pattern;
    EXPECT_EQ(compiled.facts().anchored_start != 0, test.start)
        << test.pattern << " anchored start";
    EXPECT_EQ(compiled.facts().anchored_end != 0, test.end)
        << test.pattern << " anchored end";
  }
}

TEST(Facts, ZeroRepetitionsOfAnythingIsNothing) {
  // Including zero repetitions of something unbounded, which is the case a
  // saturating multiply gets wrong if it checks for "unbounded" first.
  EXPECT_EQ(Compiled("(?:a*){0}", "").facts().max_length, 0u);
  EXPECT_EQ(Compiled("(?:a*){0}", "").facts().min_length, 0u);
  EXPECT_EQ(Compiled("a{0}", "").facts().max_length, 0u);
  EXPECT_EQ(Compiled("(?:){5}", "").facts().max_length, 0u);

  // And the other saturating direction: a large bounded repeat of something
  // unbounded is unbounded, not a wrapped-around small number.
  EXPECT_EQ(Compiled("(?:a*){2}", "").facts().max_length, GRX_NPOS);
}

TEST(Facts, LengthsAreBytesComputedFromWhatEachNodeCanMatch) {
  // A caller sizing a buffer from max_length must never be told a number that
  // is too small, so the lengths are bytes. They are computed per node rather
  // than by scaling a character count: `a{2}` is two bytes even under UTF,
  // and `.` is one to four.
  EXPECT_EQ(Compiled("a{2}", "u").facts().min_length, 2u);
  EXPECT_EQ(Compiled("a{2}", "u").facts().max_length, 2u);

  EXPECT_EQ(Compiled(".", "u").facts().min_length, 1u);
  EXPECT_EQ(Compiled(".", "u").facts().max_length, 4u);

  // A class of ASCII is one byte; one reaching into the astral planes is
  // four. Assuming four for every class would be right and useless.
  EXPECT_EQ(Compiled("[a-z]", "u").facts().max_length, 1u);
  EXPECT_EQ(Compiled("\\u{1F41F}", "u").facts().max_length, 4u);
  EXPECT_EQ(Compiled("\\u{1F41F}", "u").facts().min_length, 4u);

  Compiled unbounded("a*", "u");
  ASSERT_TRUE(unbounded.ok());
  EXPECT_EQ(unbounded.facts().max_length, GRX_NPOS);
}

TEST(Facts, WhatThePatternContains) {
  Compiled backref("(a)\\1", "");
  ASSERT_TRUE(backref.ok());
  EXPECT_TRUE(backref.facts().has_backreference);
  EXPECT_FALSE(backref.facts().has_lookaround);

  Compiled look("(?<=abc)x", "u");
  ASSERT_TRUE(look.ok());
  EXPECT_TRUE(look.facts().has_lookaround);
  EXPECT_EQ(look.facts().max_lookbehind, 3u); // Three ASCII characters.

  Compiled plain("abc", "");
  ASSERT_TRUE(plain.ok());
  EXPECT_FALSE(plain.facts().has_backreference);
  EXPECT_FALSE(plain.facts().has_lookaround);
  EXPECT_FALSE(plain.facts().has_recursion);
  EXPECT_EQ(plain.facts().max_lookbehind, 0u);

  EXPECT_EQ(Compiled("(a)(b)(?<n>c)", "").facts().capture_count, 3u);
  EXPECT_GT(Compiled("abc", "").facts().program_size, 0u);
}

// --------------------------------------------------------------------------
// Code generation
// --------------------------------------------------------------------------

TEST(Codegen, CountedRepetitionIsExpanded) {
  // documentation/design.md section 3.4: `{m,n}` is expanded rather than
  // counted, because a counter would be per-thread state the lockstep
  // simulation cannot merge. The price is that a large count is a large
  // program, which is what max_program_size is for.
  size_t two = Compiled("a{2}", "").facts().program_size;
  size_t five = Compiled("a{5}", "").facts().program_size;
  EXPECT_GT(five, two);
  EXPECT_EQ(five - two, 3u) << "one instruction per extra copy";

  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_program_size = 20;

  Compiled fits("a{5}", "", &limits);
  EXPECT_TRUE(fits.ok());

  Compiled exceeds("a{500}", "", &limits);
  EXPECT_EQ(exceeds.result(), GRX_ERR_LIMIT);
  EXPECT_EQ(exceeds.diag(), GRX_DIAG_LIMIT_PROGRAM_SIZE);
}

TEST(Codegen, ManyAlternativesAndManyOptionalCopiesBothWork) {
  // Both used to be capped by a fixed-size patch list. They share one
  // trampoline instead, so the only bound is max_program_size.
  std::string wide;
  for (int i = 0; i < 300; i++) {
    if (i) {
      wide += "|";
    }
    wide += "a" + std::to_string(i);
  }
  Compiled alternation(wide, "");
  ASSERT_TRUE(alternation.ok()) << grx_diag_string(alternation.diag());
  EXPECT_EQ(spans(alternation, "a299"), "0:2");

  Compiled optional("a{0,400}", "");
  ASSERT_TRUE(optional.ok()) << grx_diag_string(optional.diag());
  EXPECT_EQ(spans(optional, std::string(500, 'a')), "0:400");
}

TEST(Codegen, ClassesAreDeduplicatedAcrossThePattern) {
  // A pattern naming the same set six times costs one entry, because the
  // canonical form of a set is unique and an instruction stores an index.
  Compiled once("[a-z]", "");
  Compiled repeated("[a-z][a-z][a-z]\\w?[a-z]", "");
  ASSERT_TRUE(once.ok());
  ASSERT_TRUE(repeated.ok());

  EXPECT_NE(once.disassembly().find("classes=1"), std::string::npos)
      << once.disassembly();
  // Two distinct sets: `[a-z]` and `\w`.
  EXPECT_NE(repeated.disassembly().find("classes=2"), std::string::npos)
      << repeated.disassembly();
}

// --------------------------------------------------------------------------
// The named sets
// --------------------------------------------------------------------------

TEST(Sets, EcmaScriptsWhitespaceIsNotUnicodesWhiteSpace) {
  // documentation/unicode.md section 3: ECMA-262's `\s` is WhiteSpace united
  // with LineTerminator, and it includes U+FEFF, which has not been
  // White_Space since Unicode 4.0.1. Deriving the set from the UCD would
  // quietly drop it, so it is written out.
  GRX_CharClass cls;
  grx_charclass_init(&cls, nullptr);
  ASSERT_EQ(grx_named_set(&cls, GRX_SET_ES_SPACE, nullptr), GRX_OK);

  EXPECT_TRUE(grx_charclass_contains(&cls, 0xFEFF));
  EXPECT_TRUE(grx_charclass_contains(&cls, 0x0020));
  EXPECT_TRUE(grx_charclass_contains(&cls, 0x00A0));
  EXPECT_TRUE(grx_charclass_contains(&cls, 0x2028));
  EXPECT_TRUE(grx_charclass_contains(&cls, 0x3000));
  EXPECT_FALSE(grx_charclass_contains(&cls, 'a'));
  grx_charclass_clear(&cls);

  // And the whole way through: the pattern matches the character.
  const std::string bom = "\xEF\xBB\xBF";
  EXPECT_EQ(spans(Compiled("\\s", "u"), bom), "0:3");
}

TEST(Sets, TheShorthandsResolveToTheDialectsDefinitions) {
  struct {
    GRX_ShorthandSet shorthands;
    GRX_ShorthandKind kind;
    uint32_t inside;
    uint32_t outside;
  } cases[] = {
    {GRX_SHORTHANDS_ASCII, GRX_SHORTHAND_DIGIT, '5', 0x0661},
    {GRX_SHORTHANDS_UNICODE, GRX_SHORTHAND_DIGIT, 0x0661, 'a'},
    {GRX_SHORTHANDS_ASCII, GRX_SHORTHAND_WORD, '_', 0x00E9},
    {GRX_SHORTHANDS_UNICODE, GRX_SHORTHAND_WORD, 0x00E9, ' '},
    {GRX_SHORTHANDS_ASCII, GRX_SHORTHAND_SPACE, '\t', 0x00A0},
    {GRX_SHORTHANDS_ECMASCRIPT, GRX_SHORTHAND_SPACE, 0x00A0, 'a'},
    {GRX_SHORTHANDS_UNICODE, GRX_SHORTHAND_SPACE, 0x00A0, 'a'},
  };

  for (const auto & test : cases) {
    GRX_CharClass cls;
    grx_charclass_init(&cls, nullptr);
    ASSERT_EQ(grx_shorthand_set(&cls, test.shorthands, test.kind, nullptr),
        GRX_OK);
    EXPECT_TRUE(grx_charclass_contains(&cls, test.inside))
        << "kind " << test.kind << " set " << test.shorthands;
    EXPECT_FALSE(grx_charclass_contains(&cls, test.outside))
        << "kind " << test.kind << " set " << test.shorthands;
    grx_charclass_clear(&cls);
  }
}

TEST(Sets, TheSetsNoFrontEndAsksForYet) {
  // `\h` and `\v` are Perl-family shorthands, and the Unicode definitions
  // are what PCRE2 uses under UCP. No front end emits them yet, so they are
  // tested directly rather than through a pattern - the alternative is
  // shipping a set nothing has ever evaluated.
  struct {
    GRX_NamedSet set;
    uint32_t inside;
    uint32_t outside;
  } cases[] = {
    {GRX_SET_ASCII_HSPACE, ' ', '\n'},
    {GRX_SET_ASCII_HSPACE, '\t', 'a'},
    {GRX_SET_ASCII_VSPACE, '\n', ' '},
    {GRX_SET_ASCII_VSPACE, 0x0B, 'a'},
    {GRX_SET_UNICODE_DIGIT, 0x0661, 'a'},
    {GRX_SET_UNICODE_SPACE, 0x00A0, 'a'},
    {GRX_SET_UNICODE_WORD, 0x00E9, ' '},
    {GRX_SET_UNICODE_WORD, 0x200C, ' '}, // A join control, per UTS #18.
    {GRX_SET_UNICODE_WORD, 0x0301, ' '}, // A combining mark.
  };

  for (const auto & test : cases) {
    GRX_CharClass cls;
    grx_charclass_init(&cls, nullptr);
    ASSERT_EQ(grx_named_set(&cls, test.set, nullptr), GRX_OK)
        << "set " << test.set;
    EXPECT_TRUE(grx_charclass_contains(&cls, test.inside)) << "set " << test.set;
    EXPECT_FALSE(grx_charclass_contains(&cls, test.outside))
        << "set " << test.set;
    grx_charclass_clear(&cls);
  }

  GRX_CharClass cls;
  grx_charclass_init(&cls, nullptr);
  EXPECT_EQ(grx_named_set(&cls, GRX_SET_COUNT, nullptr), GRX_ERR_INVALID);
  EXPECT_EQ(grx_named_set(&cls, (GRX_NamedSet)9999, nullptr), GRX_ERR_INVALID);

  // `\h` and `\v`, which the Perl family has and ECMAScript does not.
  EXPECT_EQ(grx_shorthand_set(&cls, GRX_SHORTHANDS_ASCII,
                GRX_SHORTHAND_HSPACE, nullptr), GRX_OK);
  EXPECT_EQ(grx_shorthand_set(&cls, GRX_SHORTHANDS_ASCII,
                GRX_SHORTHAND_VSPACE, nullptr), GRX_OK);
  // Every value of the enum now names a set this function can build, so the
  // only thing left to refuse is a value that is not one.
  EXPECT_EQ(grx_shorthand_set(&cls, GRX_SHORTHANDS_ASCII,
                GRX_SHORTHAND_COUNT, nullptr), GRX_ERR_UNSUPPORTED);
  grx_charclass_clear(&cls);
}

TEST(Sets, TheNewlineSetsAreNamedInOnePlace) {
  struct {
    GRX_NewlineSet newlines;
    uint32_t inside;
    uint32_t outside;
  } cases[] = {
    {GRX_NEWLINES_LF, 0x0A, 0x0D},
    {GRX_NEWLINES_ECMASCRIPT, 0x2028, 0x0B},
    {GRX_NEWLINES_UNICODE, 0x0085, 0x20},
  };

  for (const auto & test : cases) {
    GRX_CharClass cls;
    grx_charclass_init(&cls, nullptr);
    ASSERT_EQ(grx_newline_set(&cls, test.newlines, nullptr), GRX_OK);
    EXPECT_TRUE(grx_charclass_contains(&cls, test.inside));
    EXPECT_FALSE(grx_charclass_contains(&cls, test.outside));
    grx_charclass_clear(&cls);
  }

  // POSIX without REG_NEWLINE has no line terminators at all, and the empty
  // set is a real answer rather than a failure.
  GRX_CharClass none;
  grx_charclass_init(&none, nullptr);
  EXPECT_EQ(grx_newline_set(&none, GRX_NEWLINES_NONE, nullptr), GRX_OK);
  EXPECT_EQ(grx_charclass_size(&none), 0u);
  grx_charclass_clear(&none);

  EXPECT_EQ(grx_newline_set(nullptr, GRX_NEWLINES_LF, nullptr),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_named_set(nullptr, GRX_SET_ASCII_DIGIT, nullptr),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_shorthand_set(nullptr, GRX_SHORTHANDS_ASCII,
                GRX_SHORTHAND_DIGIT, nullptr),
      GRX_ERR_INVALID);
}

// --------------------------------------------------------------------------
// The whole pipeline
// --------------------------------------------------------------------------

TEST(Compile, EverythingIsFreedThroughTheCallersAllocator) {
  grxtest::CountingAllocator allocator;

  GRX_Regex * regex = nullptr;
  GRX_Error error;
  const std::string pattern = "(?<n>[a-z\\d]+)|(x)*\\p{Lu}{2,4}";
  ASSERT_EQ(grx_regex_compile_with_allocator(pattern.data(), pattern.size(),
                GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, nullptr, allocator.get(),
                &error, &regex),
      GRX_OK);
  EXPECT_GT(allocator.live(), 0);
  EXPECT_STREQ(grx_regex_capture_name(regex, 1), "n");
  EXPECT_EQ(grx_regex_capture_name(regex, 2), nullptr);

  size_t index = 0;
  EXPECT_EQ(grx_regex_capture_index(regex, "n", &index), GRX_OK);
  EXPECT_EQ(index, 1u);
  EXPECT_EQ(grx_regex_capture_index(regex, "nosuch", &index), GRX_ERR_INVALID);

  grx_regex_free(regex);
  EXPECT_EQ(allocator.live(), 0) << "the compiled regex leaked";

  // And the failure path, which is the one that leaks: the program is half
  // built when the limit fires.
  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_program_size = 4;
  regex = nullptr;
  EXPECT_EQ(grx_regex_compile_with_allocator("abcdefghij", 10,
                GRX_SYNTAX_ECMASCRIPT, 0, &limits, allocator.get(), &error,
                &regex),
      GRX_ERR_LIMIT);
  EXPECT_EQ(regex, nullptr);
  EXPECT_EQ(allocator.live(), 0) << "the half-built program leaked";
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
