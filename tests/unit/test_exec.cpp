/**
 * @file
 *
 * The engines: selection, the match object, and the bound that is the whole
 * reason the Pike VM exists.
 *
 * What a pattern matches is checked against Node over tens of thousands of
 * rows by tools/oracle/match_diff.py. These are for what a span comparison
 * cannot see: which engine ran, what a limit does, and that a pattern
 * designed to make a backtracker hang does not make this one hang.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <chrono>
#include <cstdio>
#include <string>

#include "test_helpers.h"

TEST(Exec, SearchRejectsNullArguments) {
  int matched = 1;
  EXPECT_EQ(grx_regex_search(nullptr, "abc", 3, 0, GRX_ENGINE_AUTO, nullptr,
                nullptr, &matched),
      GRX_ERR_INVALID);
  EXPECT_EQ(grx_regex_match(nullptr, "abc", 3, 0, GRX_ENGINE_AUTO, nullptr,
                nullptr, &matched),
      GRX_ERR_INVALID);
}

TEST(Exec, MatchObjectRejectsNullArguments) {
  GRX_Match * match = nullptr;
  EXPECT_EQ(grx_match_create(nullptr, nullptr, &match), GRX_ERR_INVALID);
  EXPECT_EQ(match, nullptr);
}

TEST(Match, AccessorsTolerateNull) {
  EXPECT_EQ(grx_match_count(nullptr), 0u);
  EXPECT_EQ(grx_match_engine(nullptr), GRX_ENGINE_COUNT);
  EXPECT_EQ(grx_match_dump(nullptr, stderr), GRX_ERR_INVALID);
  grx_match_destroy(nullptr);

  GRX_Capture capture;
  EXPECT_EQ(grx_match_group(nullptr, 0, &capture), GRX_ERR_INVALID);
  EXPECT_EQ(grx_match_group_named(nullptr, "name", &capture), GRX_ERR_INVALID);
}

TEST(Capture, UnsetIsDistinctFromEmpty) {
  // A group that did not participate and a group that matched the empty
  // string are different answers, and a caller has to be able to tell them
  // apart. GRX_NPOS is what says "did not participate"; 0..0 is a real span.
  GRX_Capture unset = {GRX_NPOS, GRX_NPOS};
  GRX_Capture empty = {0, 0};

  EXPECT_NE(unset.start, empty.start);
  EXPECT_EQ(empty.start, empty.end);
}

TEST(Exec, UnknownEngineIsInvalid) {
  // Checked before the regex is dereferenced would be wrong: a NULL regex is
  // invalid whatever the engine. This states the order the other way round -
  // an out-of-range engine with a NULL regex is still GRX_ERR_INVALID.
  int matched = 1;
  EXPECT_EQ(grx_regex_search(nullptr, "abc", 3, 0, (GRX_Engine)9999, nullptr,
                nullptr, &matched),
      GRX_ERR_INVALID);
}

// --------------------------------------------------------------------------
// Running a program
// --------------------------------------------------------------------------

namespace {

/** A compiled regex that frees itself. */
class Regex {
public:
  Regex(const char * pattern, uint32_t options = 0,
      GRX_Syntax syntax = GRX_SYNTAX_ECMASCRIPT) {
    result_ = grx_regex_compile(pattern, syntax, options, &regex_);
  }
  Regex(const Regex &) = delete;
  Regex & operator=(const Regex &) = delete;
  ~Regex() { grx_regex_free(regex_); }

  GRX_Regex * get() const { return regex_; }
  bool ok() const { return result_ == GRX_OK; }

private:
  GRX_Regex * regex_ = nullptr;
  GRX_Result result_ = GRX_ERR_INTERNAL;
};

} // namespace

TEST(Exec, SearchFindsTheLeftmostMatchAndMatchRequiresTheStart) {
  Regex regex("b+");
  ASSERT_TRUE(regex.ok());

  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex.get(), nullptr, &match), GRX_OK);

  int matched = 0;
  ASSERT_EQ(grx_regex_search(regex.get(), "aabbc", 5, 0, GRX_ENGINE_AUTO,
                nullptr, match, &matched),
      GRX_OK);
  EXPECT_TRUE(matched);
  GRX_Capture capture;
  ASSERT_EQ(grx_match_group(match, 0, &capture), GRX_OK);
  EXPECT_EQ(capture.start, 2u);
  EXPECT_EQ(capture.end, 4u);
  EXPECT_EQ(grx_match_engine(match), GRX_ENGINE_PIKE);

  // The anchored entry point requires the match to *begin* where it was told,
  // which is what ECMAScript's `y` flag means.
  ASSERT_EQ(grx_regex_match(regex.get(), "aabbc", 5, 0, GRX_ENGINE_AUTO,
                nullptr, match, &matched),
      GRX_OK);
  EXPECT_FALSE(matched);

  ASSERT_EQ(grx_regex_match(regex.get(), "aabbc", 5, 2, GRX_ENGINE_AUTO,
                nullptr, match, &matched),
      GRX_OK);
  EXPECT_TRUE(matched);

  // A search from an offset skips what is before it.
  ASSERT_EQ(grx_regex_search(regex.get(), "aabbc", 5, 3, GRX_ENGINE_AUTO,
                nullptr, match, &matched),
      GRX_OK);
  EXPECT_TRUE(matched);
  ASSERT_EQ(grx_match_group(match, 0, &capture), GRX_OK);
  EXPECT_EQ(capture.start, 3u);

  grx_match_destroy(match);
}

TEST(Exec, AMatchObjectIsOptionalAndSoAreItsSpans) {
  // A caller who only wants to know *whether* it matched should not have to
  // allocate a place to put the spans.
  Regex regex("a(b)c");
  ASSERT_TRUE(regex.ok());

  int matched = 0;
  EXPECT_EQ(grx_regex_search(regex.get(), "xabc", 4, 0, GRX_ENGINE_AUTO,
                nullptr, nullptr, &matched),
      GRX_OK);
  EXPECT_TRUE(matched);
}

TEST(Exec, NamedGroupsComeBackByName) {
  Regex regex("(?<word>[a-z]+)-(?<digits>[0-9]+)");
  ASSERT_TRUE(regex.ok());

  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex.get(), nullptr, &match), GRX_OK);

  int matched = 0;
  ASSERT_EQ(grx_regex_search(regex.get(), "x abc-123 y", 11, 0,
                GRX_ENGINE_AUTO, nullptr, match, &matched),
      GRX_OK);
  ASSERT_TRUE(matched);

  GRX_Capture capture;
  ASSERT_EQ(grx_match_group_named(match, "word", &capture), GRX_OK);
  EXPECT_EQ(capture.start, 2u);
  EXPECT_EQ(capture.end, 5u);
  ASSERT_EQ(grx_match_group_named(match, "digits", &capture), GRX_OK);
  EXPECT_EQ(capture.start, 6u);
  EXPECT_EQ(capture.end, 9u);
  EXPECT_EQ(grx_match_group_named(match, "nosuch", &capture), GRX_ERR_INVALID);

  const std::string dump = grxtest::capture_dump(
      [match](FILE * out) { grx_match_dump(match, out); });
  EXPECT_NE(dump.find("word"), std::string::npos) << dump;

  grx_match_destroy(match);
}

TEST(Exec, AMatchObjectBelongsToOneRegex) {
  // Using one regex's match object with another would write spans sized for
  // the wrong capture count, which is a buffer overrun rather than a wrong
  // answer.
  Regex first("(a)(b)(c)");
  Regex second("x");
  ASSERT_TRUE(first.ok());
  ASSERT_TRUE(second.ok());

  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(first.get(), nullptr, &match), GRX_OK);
  EXPECT_EQ(grx_match_count(match), 4u);

  int matched = 0;
  EXPECT_EQ(grx_regex_search(second.get(), "x", 1, 0, GRX_ENGINE_AUTO,
                nullptr, match, &matched),
      GRX_ERR_INVALID);

  grx_match_destroy(match);
}

TEST(Exec, TheLinearBoundIsWhatThisEngineIsFor) {
  // The patterns every article about catastrophic backtracking opens with.
  // A backtracking engine takes exponential time on each of these; the
  // lockstep simulation takes O(subject x program) because a program counter
  // is occupied at most once per position.
  //
  // The assertion is on *scaling*, not on wall-clock time: an absolute
  // threshold makes a test that fails on a loaded machine and passes on a
  // fast one. Ten times the subject may take appreciably more than ten times
  // as long on a real machine - caches, allocation - so the bar is set at
  // forty, which exponential behaviour clears by many orders of magnitude.
  const char * patterns[] = {"(a|aa)*b", "(a+)+b", "(a*)*b", "(a|a?)*b"};

  for (const char * pattern : patterns) {
    Regex regex(pattern);
    ASSERT_TRUE(regex.ok()) << pattern;

    double timings[2] = {0, 0};
    size_t lengths[2] = {2000, 20000};
    for (int i = 0; i < 2; i++) {
      const std::string subject(lengths[i], 'a');
      int matched = 1;
      auto started = std::chrono::steady_clock::now();
      ASSERT_EQ(grx_regex_search(regex.get(), subject.data(), subject.size(),
                    0, GRX_ENGINE_PIKE, nullptr, nullptr, &matched),
          GRX_OK)
          << pattern;
      auto finished = std::chrono::steady_clock::now();
      EXPECT_FALSE(matched) << pattern << " should not match all a's";
      timings[i] = std::chrono::duration<double>(finished - started).count();
    }

    // Guard against a clock too coarse to measure the small case.
    if (timings[0] > 1e-5) {
      EXPECT_LT(timings[1], timings[0] * 40)
          << pattern << ": ten times the subject took "
          << (timings[1] / timings[0]) << " times as long";
    }
  }
}

TEST(Exec, StepsAreCappedEvenThoughTheBoundIsStructural) {
  // The Pike VM does not need max_steps to terminate - the sparse set does
  // that. The cap is still honoured, because a caller who set one is
  // entitled to have it mean something.
  Regex regex("a*b");
  ASSERT_TRUE(regex.ok());

  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_steps = 10;

  const std::string subject(1000, 'a');
  int matched = 1;
  EXPECT_EQ(grx_regex_search(regex.get(), subject.data(), subject.size(), 0,
                GRX_ENGINE_AUTO, &limits, nullptr, &matched),
      GRX_ERR_LIMIT);
  EXPECT_FALSE(matched)
      << "a limit is not a match; the caller must not be told otherwise";
}

TEST(Exec, AnInvalidSubjectIsReportedRatherThanMatchedAsBytes) {
  // ECMAScript's subject is text. A byte sequence that is not valid UTF-8 is
  // not text, and matching it as if each byte were a character would report
  // spans that fall inside characters.
  Regex regex("a.c");
  ASSERT_TRUE(regex.ok());

  const std::string bad = std::string("a") + '\xC3' + "c";
  int matched = 1;
  EXPECT_EQ(grx_regex_search(regex.get(), bad.data(), bad.size(), 0,
                GRX_ENGINE_AUTO, nullptr, nullptr, &matched),
      GRX_ERR_INVALID);
}

TEST(Exec, SubjectLengthIsCapped) {
  Regex regex("a");
  ASSERT_TRUE(regex.ok());

  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_subject_length = 4;

  int matched = 0;
  EXPECT_EQ(grx_regex_search(regex.get(), "aaaaaa", 6, 0, GRX_ENGINE_AUTO,
                &limits, nullptr, &matched),
      GRX_ERR_LIMIT);
  EXPECT_EQ(grx_regex_search(regex.get(), "aaaa", 4, 0, GRX_ENGINE_AUTO,
                &limits, nullptr, &matched),
      GRX_OK);
}

TEST(Exec, EverythingIsFreedThroughTheCallersAllocator) {
  grxtest::CountingAllocator allocator;

  GRX_Regex * regex = nullptr;
  ASSERT_EQ(grx_regex_compile_with_allocator("(a|b)+c", 7,
                GRX_SYNTAX_ECMASCRIPT, 0, nullptr, allocator.get(), nullptr,
                &regex),
      GRX_OK);

  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex, allocator.get(), &match), GRX_OK);

  int matched = 0;
  ASSERT_EQ(grx_regex_search(regex, "xxababc", 7, 0, GRX_ENGINE_AUTO, nullptr,
                match, &matched),
      GRX_OK);
  EXPECT_TRUE(matched);

  grx_match_destroy(match);
  grx_regex_free(regex);
  EXPECT_EQ(allocator.live(), 0) << "the engine leaked its thread state";
}

// --------------------------------------------------------------------------
// The backtracking engine
// --------------------------------------------------------------------------

TEST(Backtrack, RunsWhatTheLockstepEngineCannot) {
  struct {
    const char * pattern;
    uint32_t options;
    const char * subject;
    const char * spans;
  } cases[] = {
    // A backreference: what a group matched, matched again.
    {"(a|b)\\1", 0, "xaay", "1:3 1:2"},
    {"(a|b)\\1", 0, "xaby", ""},
    // A reference to a group that did not participate. ECMAScript matches
    // the empty string where every other tier-1 dialect fails; the rule
    // arrives as a mode on the instruction.
    {"(a)?b\\1c", 0, "bc", "0:2 -"},
    // Caseless comparison is on folded code points, not on bytes, because
    // the two runs may be different lengths.
    {"(a)\\1", GRX_OPT_CASELESS, "aA", "0:2 0:1"},
    // Lookahead, with a capture inside it that survives.
    {"(?=(a))a", 0, "a", "0:1 0:1"},
    {"x(?=y)(?=.z)", 0, "xyz", "0:1"},
    {"(?!a)b", 0, "b", "0:1"},
    // Lookbehind of more than one length, which is what running the body
    // backwards buys.
    {"(?<=(a|ab))c", GRX_OPT_UTF, "abc", "2:3 0:2"},
    {"(?<=(a+))c", GRX_OPT_UTF, "aaac", "3:4 0:3"},
    {"(?<=(?:ab)+)c", GRX_OPT_UTF, "ababc", "4:5"},
    // A negative lookaround leaves the captures as they were, whatever its
    // body touched on the way to failing.
    {"(?<!(a))b", GRX_OPT_UTF, "cb", "1:2 -"},
    {"(?<!(a))b", GRX_OPT_UTF, "ab", ""},
  };

  for (const auto & test : cases) {
    Regex regex(test.pattern, test.options);
    ASSERT_TRUE(regex.ok()) << test.pattern;

    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex.get(), nullptr, &match), GRX_OK);

    int matched = 0;
    ASSERT_EQ(grx_regex_search(regex.get(), test.subject,
                  std::char_traits<char>::length(test.subject), 0,
                  GRX_ENGINE_AUTO, nullptr, match, &matched),
        GRX_OK)
        << test.pattern;

    std::string spans;
    if (matched) {
      for (size_t i = 0; i < grx_match_count(match); i++) {
        GRX_Capture capture;
        grx_match_group(match, i, &capture);
        if (i) {
          spans += " ";
        }
        spans += capture.start == GRX_NPOS
            ? std::string("-")
            : std::to_string(capture.start) + ":"
                + std::to_string(capture.end);
      }
      EXPECT_EQ(grx_match_engine(match), GRX_ENGINE_BACKTRACK)
          << test.pattern << " should have needed the backtracker";
    }
    EXPECT_EQ(spans, test.spans) << test.pattern << " on " << test.subject;

    grx_match_destroy(match);
  }
}

TEST(Backtrack, ExponentialPatternsTheMemoCanHelpAreAnsweredRatherThanRefused) {
  // The patterns a backtracking engine is famous for, on the backtracker,
  // and they are answered. Nothing clever happens to the search itself: the
  // engine arms the bit-state memo once it has taken more steps than there
  // are (instruction, position) states to take them from, which is the point
  // at which it has provably repeated itself. Perl does the same thing under
  // the name "super-linear cache".
  //
  // The step budget here is two hundred thousand and the answers cost a few
  // thousand, so this is not a test of the budget being generous.
  const char * patterns[] = {"(a+)+b", "(a|aa)*b", "(x+x+)+y"};

  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_steps = 200000;

  for (const char * pattern : patterns) {
    Regex regex(pattern);
    ASSERT_TRUE(regex.ok()) << pattern;

    const std::string subject(40, pattern[1] == 'x' ? 'x' : 'a');
    int matched = 1;
    EXPECT_EQ(grx_regex_search(regex.get(), subject.data(), subject.size(), 0,
                  GRX_ENGINE_BACKTRACK, &limits, nullptr, &matched),
        GRX_OK)
        << pattern;
    EXPECT_FALSE(matched) << pattern;

    // The same answer from the lockstep engine, which never needed the memo
    // because its bound is structural.
    matched = 1;
    EXPECT_EQ(grx_regex_search(regex.get(), subject.data(), subject.size(), 0,
                  GRX_ENGINE_PIKE, &limits, nullptr, &matched),
        GRX_OK)
        << pattern;
    EXPECT_FALSE(matched) << pattern;
  }
}

TEST(Backtrack, ABitmapThatWillNotFitLeavesTheRunUnmemoisedRatherThanRefused) {
  // The one place the late memo differs from the bit-state engine in kind
  // rather than in timing. There, the bitmap is the promise the caller asked
  // for by naming the engine, so one that will not fit in max_match_memory
  // is GRX_ERR_LIMIT before a single step. Here it is an optimisation the
  // engine reached for on its own, so a bitmap that will not fit is simply
  // not allocated and the run goes on to whatever limit it was heading for.
  //
  // Sixty-four bytes is under what `(a+)+b` over forty characters needs -
  // about twelve instructions times forty-one positions, which is sixty-two
  // bytes of bits - and over what anything else in the run allocates, so it
  // separates the two.
  Regex regex("(a+)+b");
  ASSERT_TRUE(regex.ok());
  const std::string subject(40, 'a');

  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_steps = 200000;
  limits.max_match_memory = 32;

  int matched = 1;
  EXPECT_EQ(grx_regex_search(regex.get(), subject.data(), subject.size(), 0,
                GRX_ENGINE_BACKTRACK, &limits, nullptr, &matched),
      GRX_ERR_LIMIT);
  EXPECT_FALSE(matched);

  // The same budget on the engine that promised the bitmap refuses before it
  // starts, and raising the budget lets the backtracker answer.
  matched = 1;
  EXPECT_EQ(grx_regex_search(regex.get(), subject.data(), subject.size(), 0,
                GRX_ENGINE_BITSTATE, &limits, nullptr, &matched),
      GRX_ERR_LIMIT);

  limits.max_match_memory = 8 * 1024;
  matched = 1;
  EXPECT_EQ(grx_regex_search(regex.get(), subject.data(), subject.size(), 0,
                GRX_ENGINE_BACKTRACK, &limits, nullptr, &matched),
      GRX_OK);
  EXPECT_FALSE(matched);
}

TEST(Backtrack, TheLongestSearchStopsWhenTheMatchReachesTheEnd) {
  // The exhaustive mode is exponential, and this is what keeps it usable on
  // the shape that matters. `\(a*\)*\1` reaches the end of the subject on
  // its first path; without the short-circuit the engine then walks the rest
  // of the tree to prove no longer match exists, and spends max_steps doing
  // it. Nothing can be longer than the whole subject, so there is nothing to
  // prove. Both references answer this shape at once too.
  Regex regex("\\(a*\\)*\\1", 0, GRX_SYNTAX_GNU_BRE);
  ASSERT_TRUE(regex.ok());

  const std::string subject(40, 'a');
  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_steps = 20000;

  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex.get(), nullptr, &match), GRX_OK);
  int matched = 0;
  EXPECT_EQ(grx_regex_search(regex.get(), subject.data(), subject.size(), 0,
                GRX_ENGINE_BACKTRACK, &limits, match, &matched),
      GRX_OK);
  EXPECT_TRUE(matched);
  GRX_Capture whole;
  grx_match_group(match, 0, &whole);
  EXPECT_EQ(whole.start, 0u);
  EXPECT_EQ(whole.end, subject.size());
  grx_match_destroy(match);
}

TEST(Backtrack, TheMemoIsRefusedWhereTheDialectWantsTheLongestMatch) {
  // A bitmap bit means "this state failed before and will fail again". The
  // leftmost-longest mode reports failure from a *match*, on purpose, so
  // that the search carries on looking for a longer one - and a bitmap would
  // then prune exactly the paths the mode exists to reach.
  //
  // Refused rather than run without the bitmap, which is the rule
  // GRX_ENGINE_PIKE already follows for a program it cannot run: naming an
  // engine is asking for that engine's guarantee, and quietly answering with
  // a different one is worse than saying it cannot be done. AUTO never lands
  // here - a POSIX program that needs backtracking has a backreference, and
  // a backreference is not memoisable - so this is the explicitly named
  // engine only.
  Regex posix("a|ab", 0, GRX_SYNTAX_GNU_ERE);
  ASSERT_TRUE(posix.ok());

  int matched = 1;
  EXPECT_EQ(grx_regex_search(posix.get(), "ab", 2, 0, GRX_ENGINE_BITSTATE,
                nullptr, nullptr, &matched),
      GRX_ERR_UNSUPPORTED);

  // Every other engine answers, and answers with the longest match.
  for (GRX_Engine engine :
      {GRX_ENGINE_AUTO, GRX_ENGINE_PIKE, GRX_ENGINE_BACKTRACK}) {
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(posix.get(), nullptr, &match), GRX_OK);
    matched = 0;
    EXPECT_EQ(grx_regex_search(posix.get(), "ab", 2, 0, engine, nullptr,
                  match, &matched),
        GRX_OK);
    EXPECT_TRUE(matched);
    GRX_Capture whole;
    grx_match_group(match, 0, &whole);
    EXPECT_EQ(whole.end, 2u) << "engine " << (int)engine;
    grx_match_destroy(match);
  }

  // The bit-state engine is untouched for a dialect that wants the first
  // match, which is every other dialect here.
  Regex ecma("a|ab");
  ASSERT_TRUE(ecma.ok());
  matched = 0;
  EXPECT_EQ(grx_regex_search(ecma.get(), "ab", 2, 0, GRX_ENGINE_BITSTATE,
                nullptr, nullptr, &matched),
      GRX_OK);
  EXPECT_TRUE(matched);
}

TEST(Backtrack, ExponentialPatternsTheMemoCannotHelpHitTheLimitRatherThanHanging) {
  // The rest of them, and the reason this engine still has an exponential
  // worst case to document. A memo keyed on (instruction, position) is only
  // sound when nothing else distinguishes two arrivals at the same pair, and
  // each of these carries something that does: `(a*)*b` a progress register,
  // because its body can match empty; `(a+)+b\1` a capture a
  // backreference will compare against; `((?=a)a+)+b` a lookaround, whose
  // sub-run would need a bitmap of its own.
  //
  // So the promise for these is not speed but *termination*: max_steps turns
  // a hang into GRX_ERR_LIMIT, and GRX_ERR_LIMIT is not "no match", because
  // "no match" is a fact about the subject and a limit is a fact about the
  // budget.
  const char * patterns[] = {"(a*)*b", "(a+)+b\\1", "((?=a)a+)+b"};

  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_steps = 200000;

  for (const char * pattern : patterns) {
    Regex regex(pattern);
    ASSERT_TRUE(regex.ok()) << pattern;

    const std::string subject(40, 'a');
    int matched = 1;
    EXPECT_EQ(grx_regex_search(regex.get(), subject.data(), subject.size(), 0,
                  GRX_ENGINE_BACKTRACK, &limits, nullptr, &matched),
        GRX_ERR_LIMIT)
        << pattern;
    EXPECT_FALSE(matched) << pattern;
  }
}

TEST(Backtrack, TheStackDepthIsCappedSeparatelyFromTheStepCount) {
  // Two different resources, and a caller may want to bound either. The
  // stack is on the heap, so this is a policy cap and not a guard against
  // overflowing the C stack - there is nothing recursive here to overflow it.
  Regex regex("(a|b)*c");
  ASSERT_TRUE(regex.ok());

  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_backtrack = 8;

  const std::string subject(200, 'a');
  int matched = 1;
  EXPECT_EQ(grx_regex_search(regex.get(), subject.data(), subject.size(), 0,
                GRX_ENGINE_BACKTRACK, &limits, nullptr, &matched),
      GRX_ERR_LIMIT);
}

TEST(Backtrack, ConstructsWithoutAnEngineAreRefusedRatherThanIgnored) {
  // `(?>a)` has an opcode and no dialect that emits it yet; the verbs and
  // recursion have opcodes and no engine until WP-19. A program containing
  // one has to be refused: running it with the construct ignored would give
  // a plausible wrong answer, which is worse than no answer.
  GRX_Regex * regex = nullptr;
  GRX_Error error;
  EXPECT_EQ(grx_regex_compile_with_allocator("(?>a)", 5, GRX_SYNTAX_ECMASCRIPT,
                0, nullptr, nullptr, &error, &regex),
      GRX_ERR_SYNTAX)
      << "ECMAScript has no atomic group, so the parser rejects it first";
  EXPECT_EQ(regex, nullptr);
}

TEST(Engines, AgreeOnEveryProgramBothCanRun) {
  // documentation/design.md section 3.5.4. The two engines share nothing
  // below the instruction set - one merges threads in lockstep, the other
  // walks one path with an undo stack - so a disagreement is a defect in one
  // of them and there is nowhere for a shared mistake to hide.
  //
  // tools/oracle/engine_diff.py runs thousands of random rows through this
  // same comparison; these are the cases worth naming.
  struct {
    const char * pattern;
    const char * subject;
  } cases[] = {
    {"a(b|c)*d", "abcbcd"},
    {"(a*)*", "b"},
    {"(a*)+", "b"},
    {"((a)|b)+", "ab"},
    {"(?:(a)|b){2}", "ab"},
    {"a|ab", "ab"},
    {"(a|b)*", "abab"},
    {"^(a+)(b+)$", "aaabbb"},
    {"[a-c]+?", "xbcay"},
    {"(a)(b)?(c)", "ac"},
    {"a{2,4}", "aaaaa"},
    {"a{2,4}?", "aaaaa"},
    {"(?:)", "abc"},
    {"x*", "aaa"},
    // The case that showed the lockstep engine's thread set was unsound.
    // `b??` is lazy, so the empty branch is preferred and the second
    // iteration stalls; ECMA-262 fails that iteration and backtracks into
    // the body, which finds `b`. The (pc, position) key made the second
    // arrival at the body look like a duplicate of the first, so the thread
    // that goes on to match `b` was dropped and the answer was 0-1 where
    // every other engine said 0-2. Found by test262's harvested patterns.
    // `?\?` rather than `??`: `??)` is a trigraph, and -Werror says so.
    {"(a?b?\?)*", "abc"},
    {"(a?b?\?)*", "ab"},
    {"(a??b?)*", "abc"},
    {"(|a)*", "aa"},
    {"(a*?)*", "aa"},
  };

  for (const auto & test : cases) {
    Regex regex(test.pattern);
    ASSERT_TRUE(regex.ok()) << test.pattern;

    std::string reported[2];
    const GRX_Engine engines[2] = {GRX_ENGINE_PIKE, GRX_ENGINE_BACKTRACK};
    for (int i = 0; i < 2; i++) {
      GRX_Match * match = nullptr;
      ASSERT_EQ(grx_match_create(regex.get(), nullptr, &match), GRX_OK);
      int matched = 0;
      ASSERT_EQ(grx_regex_search(regex.get(), test.subject,
                    std::char_traits<char>::length(test.subject), 0,
                    engines[i], nullptr, match, &matched),
          GRX_OK)
          << test.pattern;

      reported[i] = matched ? "" : "nomatch";
      for (size_t g = 0; matched && g < grx_match_count(match); g++) {
        GRX_Capture capture;
        grx_match_group(match, g, &capture);
        reported[i] += capture.start == GRX_NPOS
            ? std::string(" -")
            : " " + std::to_string(capture.start) + ":"
                + std::to_string(capture.end);
      }
      grx_match_destroy(match);
    }

    EXPECT_EQ(reported[0], reported[1])
        << test.pattern << " on " << test.subject
        << ": the engines disagree about the same program";
  }
}

TEST(Pike, TheThreadListAndTheClosureStackGrowAgainstTheMemoryLimit) {
  // Both used to be sized from the program: one thread per program counter,
  // and twice that for the walk's stack. The stall mask ended that bound -
  // one program counter can hold a thread per distinct mask - so both grow
  // now, and the growth is charged against max_match_memory like every other
  // allocation the simulation makes.
  //
  // What matters is that an unreasonable demand becomes an *answer*. Before
  // this, exceeding the stack's fixed size reported GRX_ERR_INTERNAL, which
  // tells a caller nothing they can act on.
  struct {
    const char * pattern;
    size_t tight;     // a budget too small for this pattern
    size_t generous;  // one that is not
  } cases[] = {
    {"((a?)*)*", 1024, 1u << 20},
    {"(((a?)*)*)*", 1024, 1u << 20},
    {"((((a?)*)*)*)*", 8192, 1u << 20},
    {"(a?b?\?)*", 1024, 1u << 20},
  };

  const char subject[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
  for (const auto & test : cases) {
    Regex regex(test.pattern, GRX_OPT_UTF);
    ASSERT_TRUE(regex.ok()) << test.pattern;

    GRX_Limits limits;
    grx_limits_default(&limits);
    limits.max_match_memory = test.tight;
    int matched = 0;
    EXPECT_EQ(grx_regex_search(regex.get(), subject, sizeof(subject) - 1, 0,
                  GRX_ENGINE_PIKE, &limits, nullptr, &matched),
        GRX_ERR_LIMIT)
        << test.pattern << ": a budget of " << test.tight
        << " should be refused, not exceeded quietly";

    limits.max_match_memory = test.generous;
    matched = 0;
    EXPECT_EQ(grx_regex_search(regex.get(), subject, sizeof(subject) - 1, 0,
                  GRX_ENGINE_PIKE, &limits, nullptr, &matched),
        GRX_OK)
        << test.pattern;
    EXPECT_TRUE(matched) << test.pattern;
  }
}

TEST(Backtrack, ABackreferenceInsideALookbehindRunsBackwards) {
  // A lookbehind body carries GRX_INST_REVERSE and steps backwards.
  // GRX_OP_BACKREF ignored that: it compared forward from the position and
  // advanced forward, inside a body walking the other way. Two things
  // followed, and the second is the one worth an invariant of its own.
  struct {
    const char * pattern;
    const char * subject;
    const char * expected;   // "nomatch", or spans as "start:end" per group
  } cases[] = {
    {"(.)(?<=(\\1\\1))", "aaa", " 1:2 1:2 0:2"},
    {"(.)(?<=\\1\\1\\1)", "aaa", " 2:3 2:3"},
    {"(?<=(?:\\1|b)(aa)).", "aaa", "nomatch"},
    {"(?<=\\1(\\w+))c", "abc", "nomatch"},
    {"(?<=(a))b", "ab", " 1:2 0:1"},
    {"(?<=(ab))c", "abc", " 2:3 0:2"},
  };

  for (const auto & test : cases) {
    Regex regex(test.pattern);
    ASSERT_TRUE(regex.ok()) << test.pattern;
    GRX_Match * match = nullptr;
    ASSERT_EQ(grx_match_create(regex.get(), nullptr, &match), GRX_OK);
    int matched = 0;
    ASSERT_EQ(grx_regex_search(regex.get(), test.subject,
                  std::char_traits<char>::length(test.subject), 0,
                  GRX_ENGINE_BACKTRACK, nullptr, match, &matched),
        GRX_OK)
        << test.pattern;

    std::string reported = matched ? "" : "nomatch";
    for (size_t g = 0; matched && g < grx_match_count(match); g++) {
      GRX_Capture capture;
      grx_match_group(match, g, &capture);
      // The invariant, and the one the defect broke most visibly: a group
      // that participated has an end at or after its start. `3:1` is not a
      // span, whatever the rest of the answer is.
      if (capture.start != GRX_NPOS) {
        EXPECT_LE(capture.start, capture.end)
            << test.pattern << ": group " << g << " ends before it starts";
      }
      reported += capture.start == GRX_NPOS
          ? std::string(" -")
          : " " + std::to_string(capture.start) + ":"
              + std::to_string(capture.end);
    }
    grx_match_destroy(match);
    EXPECT_EQ(reported, test.expected) << test.pattern;
  }
}

int main(int argc, char ** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}

/**
 * The match-time error channel says which limit, and is cleared each attempt.
 *
 * A match-time failure returns a result code, and GRX_ERR_LIMIT alone does
 * not say *which* limit was reached - which knob a caller has to raise, or
 * whether the subject was the problem rather than the pattern. Six
 * diagnostics existed in the catalogue for exactly these cases and nothing
 * raised any of them, because grx_regex_search() and its kin take no
 * GRX_Error and there was nowhere to put one.
 *
 * They travel on the match object instead of in an out-parameter, because a
 * match-time failure has no offset into the pattern - "ran out of steps" is
 * not a position - so it belongs beside grx_match_steps() and
 * grx_match_engine() with the other facts about an attempt. The subject's
 * UTF-8 check is the one exception and carries an offset into the *subject*.
 */
TEST(Exec, TheMatchTimeErrorChannelNamesWhatStopped) {
  GRX_Regex * regex = nullptr;
  ASSERT_EQ(
      grx_regex_compile("^a+$", GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, &regex),
      GRX_OK);

  GRX_Match * match = nullptr;
  ASSERT_EQ(grx_match_create(regex, nullptr, &match), GRX_OK);

  // A successful attempt leaves the channel empty, so a caller reading it
  // after a match is not handed an older failure.
  int matched = 0;
  EXPECT_EQ(grx_regex_search(regex, "aaa", 3, 0, GRX_ENGINE_AUTO, nullptr,
                match, &matched),
      GRX_OK);
  EXPECT_TRUE(matched);
  ASSERT_NE(grx_match_error(match), nullptr);
  EXPECT_EQ(grx_match_error(match)->code, GRX_OK);
  EXPECT_EQ(grx_match_error(match)->diag, GRX_DIAG_NONE);

  // A step limit names itself rather than leaving the caller to guess.
  GRX_Limits limits;
  grx_limits_default(&limits);
  limits.max_steps = 1;
  EXPECT_EQ(grx_regex_search(regex, "aaaaaaaa", 8, 0, GRX_ENGINE_AUTO, &limits,
                match, &matched),
      GRX_ERR_LIMIT);
  EXPECT_EQ(grx_match_error(match)->code, GRX_ERR_LIMIT);
  EXPECT_EQ(grx_match_error(match)->diag, GRX_DIAG_LIMIT_STEPS);
  EXPECT_NE(grx_match_error(match)->message[0], '\0');

  // And the next attempt clears it again, so the channel describes the last
  // attempt and never an older one.
  EXPECT_EQ(grx_regex_search(regex, "aaa", 3, 0, GRX_ENGINE_AUTO, nullptr,
                match, &matched),
      GRX_OK);
  EXPECT_EQ(grx_match_error(match)->diag, GRX_DIAG_NONE);

  grx_match_destroy(match);
  grx_regex_free(regex);

  // A subject that is not valid UTF-8 is the one match-time diagnostic with
  // a position, and the position is into the subject.
  GRX_Regex * dot = nullptr;
  ASSERT_EQ(
      grx_regex_compile(".", GRX_SYNTAX_ECMASCRIPT, GRX_OPT_UTF, &dot),
      GRX_OK);
  GRX_Match * dot_match = nullptr;
  ASSERT_EQ(grx_match_create(dot, nullptr, &dot_match), GRX_OK);
  EXPECT_NE(grx_regex_search(dot, "ab\xFF", 3, 0, GRX_ENGINE_AUTO, nullptr,
                dot_match, &matched),
      GRX_OK);
  EXPECT_EQ(grx_match_error(dot_match)->diag, GRX_DIAG_INVALID_SUBJECT_UTF8);
  EXPECT_EQ(grx_match_error(dot_match)->offset, 2u);

  // Passing no match object asks only whether the subject matched, and still
  // gets the result code; the accessor is NULL-safe for that caller.
  EXPECT_NE(grx_regex_search(dot, "ab\xFF", 3, 0, GRX_ENGINE_AUTO, nullptr,
                nullptr, &matched),
      GRX_OK);
  EXPECT_EQ(grx_match_error(nullptr), nullptr);

  grx_match_destroy(dot_match);
  grx_regex_free(dot);
}
