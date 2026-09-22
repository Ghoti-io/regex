/*
 * SPDX-License-Identifier: LGPL-3.0-only
 *
 * Copyright (C) 2026 Corey Pennycuff
 *
 * This file is part of Ghoti.io Regex.
 *
 * Ghoti.io Regex is free software: you can redistribute it and/or modify it
 * under the terms of the GNU Lesser General Public License version 3 as
 * published by the Free Software Foundation.
 *
 * Ghoti.io Regex is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY
 * or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU Lesser General Public
 * License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/**
 * @file
 *
 * The vocabulary that survives lowering.
 *
 * These are the semantic choices a dialect makes, named as data so that the
 * IR, the compiled program and the engines can all speak about them without
 * any of the three knowing which dialect chose what. This is where the
 * design's central invariant lives in practice (documentation/design.md
 * section 3): the dialect is gone after lowering, and what replaces it is
 * exactly this list.
 *
 * A dialect difference that cannot be said with one of these enums is a
 * dialect difference the engines would have to branch on, which is the shape
 * of defect the invariant exists to prevent. Adding a value here is the
 * correct response; adding a GRX_Syntax test to an engine is not.
 */

#ifndef GHOTI_IO_GRX_SRC_CORE_SEMANTICS_INTERNAL_H
#define GHOTI_IO_GRX_SRC_CORE_SEMANTICS_INTERNAL_H

#include <ghoti.io/regex/macros.h>

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/** @brief The repeat bound meaning "no upper bound". */
#define GRX_REPEAT_INF UINT32_MAX

/**
 * @brief The subject is UTF-8 and one step is a code point, not a byte.
 *
 * Program-wide rather than per-node, and the only option that survives
 * lowering: it decides what "one character" means to every consuming
 * instruction, which is not something a node can decide for itself.
 */
#define GRX_PROGRAM_UTF GRX_BIT(0)

/**
 * @brief The bit-state engine's memo would be unsound for this program.
 *
 * `(pc, position)` identifies a state only when what the program does next
 * depends on nothing else. `\K` breaks that by writing the reported start,
 * which the empty-match rule then reads; a conditional breaks it by reading
 * a capture; and a control verb breaks it by deciding something about the
 * whole *search* rather than about this path. Codegen sets the bit when it
 * emits any of them, so that the engine chooser reads one flag instead of
 * walking the program.
 *
 * An **atomic region** is on the list for a subtler reason, and it is the
 * one that was found by a test rather than by reading. The memo's soundness
 * rests on "a state that was visited and did not return is a state that
 * failed". An atomic group breaks that: ATOMIC_END *abandons* the branch
 * points of a body that succeeded, so the states on that successful path are
 * marked as though they had failed, and a later start that reaches them is
 * pushed onto a shorter path the atomic group should have forbidden.
 * `(?>a{1,5})a` against "aaaaa" is the whole of the argument - no match on
 * the backtracker, and "1-5" on the bit-state engine until this bit covered
 * it. ECMAScript has no atomic group, which is why six differential gates
 * and 28,559 vectors had never put one through the memo.
 *
 * Backreferences, lookarounds and recursion are excluded by
 * @ref GRX_Facts instead - they were known before this bit existed, and the
 * facts are where a caller can also see them.
 */
#define GRX_PROGRAM_NO_MEMO GRX_BIT(1)

/**
 * @brief Every group keeps the span it had when it last *closed*.
 *
 * What a backreference to a group that is still open reads. While a group is
 * between its two SAVEs its slots hold a start from this iteration and an
 * end from the last, which is not a span anybody wrote: `^(a\1?){4}$`
 * against "aaaaaa" matches in perl and in pcre2test, where the second
 * iteration's `\1` is the "a" the first one took, and reading the live
 * slots there gives the empty string instead.
 *
 * So the closing SAVE writes the pair a second time, to slots after the
 * registers, and a backreference reads those. For a group that is *not*
 * open the two are the same thing, which is why nothing else changes; a
 * RESET or a RESET_STALE clears both, so a loop taking a capture away takes
 * it away from a reference outside the loop too.
 *
 * Set only for a program that contains a backreference. Those are already
 * excluded from the memo by @ref GRX_Facts, so the extra slots cost nothing
 * a reference had not already cost, and no other program carries them.
 */
#define GRX_PROGRAM_SHADOW_CAPTURES GRX_BIT(2)

/**
 * @brief A zero-width assertion.
 *
 * The line and boundary kinds are parameterised by a character class - the
 * dialect's newline set, or its word set - carried alongside the kind rather
 * than built into it, because those sets differ between dialects
 * (documentation/dialects.md sections 5.2 and 5.9) and an engine must not
 * know which dialect it is running.
 */
typedef enum {
  /**
   * `\A`, and `^` without multiline.
   *
   * Two spellings of one position, which is why they are one kind - and
   * GRX_INST_LINE_ANCHOR is what tells them apart, because
   * GRX_SEARCH_NOTBOL reaches the `^` spelling and not the `\A` one. The
   * next two kinds are each shared the same way.
   */
  GRX_ASSERT_START_SUBJECT = 0,
  GRX_ASSERT_END_SUBJECT,       ///< `\z`, and `$` where it means the end.
  GRX_ASSERT_END_BEFORE_NEWLINE, ///< `\Z`, and `$` before a final newline.
  GRX_ASSERT_START_LINE,        ///< `^` with multiline; uses the newline set.
  /**
   * `^` with multiline, except after a newline that ends the subject.
   *
   * Two kinds rather than one, because the dialects genuinely disagree and
   * neither answer is the other one with a flag set. ECMA-262 asks only
   * whether the preceding character is a line terminator, so `/^/gm` finds
   * three positions in "a\nb\n"; pcre2pattern says a circumflex "does not
   * match after a newline that ends the string", and Perl agrees, so those
   * two find two. An engine is told which it is running.
   */
  GRX_ASSERT_START_LINE_INTERIOR,
  GRX_ASSERT_END_LINE,          ///< `$` with multiline; uses the newline set.
  GRX_ASSERT_WORD_BOUNDARY,     ///< `\b`; uses the word set.
  GRX_ASSERT_NOT_WORD_BOUNDARY, ///< `\B`; uses the word set.
  /**
   * GNU's `\<` and `\>`: the two halves of a word boundary.
   *
   * `\b` asks whether the characters either side differ in wordness; these
   * ask *which way round*. They carry the same word set and cost the same
   * two class lookups, which is why they are assertions here rather than the
   * lookarounds a dialect without them has to write.
   */
  GRX_ASSERT_WORD_START,
  GRX_ASSERT_WORD_END,
  GRX_ASSERT_SEARCH_START,      ///< `\G`: where this search attempt began.
  /**
   * The four segmentation boundaries, and their negations.
   *
   * The only assertions here whose answer is an algorithm rather than a set:
   * `\b` asks whether the characters either side are in the word class, and
   * a class is what the instruction carries, but a grapheme cluster boundary
   * is a dozen rules over the surrounding text and a line break is thirty.
   * They still consume nothing and carry no state, which is what keeps a
   * program containing one runnable by every engine.
   */
  GRX_ASSERT_GRAPHEME_BOUNDARY,
  GRX_ASSERT_NOT_GRAPHEME_BOUNDARY,
  GRX_ASSERT_WORD_SEG_BOUNDARY,
  GRX_ASSERT_NOT_WORD_SEG_BOUNDARY,
  GRX_ASSERT_SENTENCE_BOUNDARY,
  GRX_ASSERT_NOT_SENTENCE_BOUNDARY,
  GRX_ASSERT_LINE_BOUNDARY,
  GRX_ASSERT_NOT_LINE_BOUNDARY,
  /**
   * There are between `min` and `max` bytes left before the assertion ends.
   *
   * The one assertion no pattern spells. It exists inside a lookbehind body
   * the engine runs forwards (documentation/design.md section 3.5.2), where
   * the body must arrive exactly where the assertion stands: an alternative
   * that cannot span the distance still left is one there is no point
   * trying, and this is what says so before it is tried rather than after.
   *
   * Without it `(?<=(a|aa|aaa))b` walks every branch from every candidate
   * start and discards all but one, which is how a bounded lookbehind turns
   * a constant into a large constant. Perl prunes the same way, and the
   * difference is measurable: see documentation/testing.md section 10.
   *
   * Zero-width and stateless like every other assertion here, so it does not
   * change which engines may run a program - though in practice only the
   * backtracker ever sees one, since a lookaround is what puts it there.
   */
  GRX_ASSERT_LOOK_LENGTH,
  GRX_ASSERT_COUNT              ///< Closes the enum; not an assertion.
} GRX_AssertKind;

/** @brief Which way a lookaround looks, and what it wants to find. */
typedef enum {
  GRX_LOOK_AHEAD_POSITIVE = 0, ///< `(?=a)`.
  GRX_LOOK_AHEAD_NEGATIVE,     ///< `(?!a)`.
  GRX_LOOK_BEHIND_POSITIVE,    ///< `(?<=a)`.
  GRX_LOOK_BEHIND_NEGATIVE,    ///< `(?<!a)`.
  /**
   * `(*napla:a)`, also spelt `(?*a)`: a lookahead that can be re-entered.
   *
   * An ordinary lookaround is atomic - once its body has succeeded, nothing
   * goes back in to find a second way through it, so the captures it set are
   * the ones the first success left. PCRE2 10.43 added the other kind:
   * `(*napla:a|(.))\1\1` can give back the `a` and take the `(.)` branch
   * instead when the backreferences that follow do not match.
   *
   * Negative forms do not exist and cannot: a negative assertion holds or
   * does not, and there is nothing inside it to come back for.
   */
  GRX_LOOK_AHEAD_NON_ATOMIC,
  GRX_LOOK_BEHIND_NON_ATOMIC,  ///< `(*naplb:a)`, also spelt `(?<*a)`.
  GRX_LOOK_COUNT               ///< Closes the enum; not a lookaround.
} GRX_LookKind;

/** @brief What a conditional tests. */
typedef enum {
  GRX_COND_GROUP_SET = 0,     ///< `(?(1)...)`: did group N participate?
  GRX_COND_RECURSION_ANY,     ///< `(?(R)...)`: are we inside any recursion?
  GRX_COND_RECURSION_GROUP,   ///< `(?(R1)...)`: inside recursion of group N?
  GRX_COND_ASSERTION,         ///< `(?(?=a)...)`: the condition is a lookaround.
  GRX_COND_DEFINE,            ///< `(?(DEFINE)...)`: never runs; defines only.
  /**
   * A condition already decided: `(?(VERSION>=10.0)yes|no)`.
   *
   * PCRE2's version test is answered when the pattern is read, not when it
   * is run, and `a` holds the answer. It is a value here rather than the
   * chosen branch substituted at parse time because the AST records what was
   * written - a pattern that tests for a version this library does not claim
   * should still dump as the conditional its author wrote.
   */
  GRX_COND_STATIC,
  GRX_COND_COUNT              ///< Closes the enum; not a condition.
} GRX_CondKind;

/**
 * @brief A backtracking control verb.
 *
 * Only the backtracking engine runs these; a program containing one is not
 * regular (documentation/design.md section 3.3).
 */
typedef enum {
  GRX_VERB_ACCEPT = 0, ///< `(*ACCEPT)`: the match succeeds here.
  GRX_VERB_FAIL,       ///< `(*FAIL)`: this path fails.
  GRX_VERB_COMMIT,     ///< `(*COMMIT)`: no further attempt at any position.
  GRX_VERB_PRUNE,      ///< `(*PRUNE)`: no further attempt at this position.
  GRX_VERB_SKIP,       ///< `(*SKIP)`: resume searching past here.
  GRX_VERB_THEN,       ///< `(*THEN)`: advance to the next alternative.
  /**
   * `(*MARK:NAME)`: name this position.
   *
   * The only verb that changes nothing about where the match goes. What it
   * does is leave a name behind, for two readers: `(*SKIP:NAME)`, which
   * resumes the search at the position the most recent mark of that name was
   * set, and grx_match_mark(), which reports the last one the answer passed
   * through.
   */
  GRX_VERB_MARK,
  GRX_VERB_COUNT       ///< Closes the enum; not a verb.
} GRX_VerbKind;

/** @brief How a repeat prefers to consume. */
typedef enum {
  GRX_REPEAT_GREEDY = 0,  ///< Longest first, backtracking shorter.
  GRX_REPEAT_LAZY,        ///< Shortest first, backtracking longer.
  GRX_REPEAT_POSSESSIVE,  ///< Longest, and never give any of it back.
  GRX_REPEAT_MODE_COUNT   ///< Closes the enum; not a mode.
} GRX_RepeatMode;

/**
 * @brief What an iteration that consumed nothing does, once the minimum is
 * satisfied.
 *
 * The axis on which `(a*)*` against `b` divides the world
 * (documentation/dialects.md section 5.5). The engines implement the mode;
 * the dialect chose it.
 */
typedef enum {
  GRX_EMPTY_LOOP_FAIL = 0, ///< The iteration fails. ECMA-262 RepeatMatcher.
  GRX_EMPTY_LOOP_BREAK,    ///< The iteration succeeds and the loop stops.
  GRX_EMPTY_LOOP_ALLOW,    ///< Nothing special; the longest match decides.
  /**
   * BREAK, but only for an iteration that is the loop's first.
   *
   * POSIX, and neither of the two above on its own. glibc and musl agree on
   * both halves of it: `(a*)*` against "b" reports group 1 as 0-0, so an
   * empty iteration *does* run when nothing else has - the subexpression
   * participates rather than going unset, which FAIL would give. And
   * `(a|)*` against "aaaa" reports group 1 as 3-4, not 4-4, so once an
   * iteration has consumed, a trailing empty one does *not* run - which is
   * what BREAK would give, the empty body having written its captures
   * before the loop exited.
   *
   * The two questions are told apart by where the loop began. The check
   * carries a second progress register holding the position the loop was
   * entered at, and an empty iteration is taken only while the loop still
   * stands there.
   */
  GRX_EMPTY_LOOP_BREAK_FIRST,
  GRX_EMPTY_LOOP_COUNT     ///< Closes the enum; not a mode.
} GRX_EmptyLoopMode;

/**
 * @brief What happens to captures inside a repeat between iterations.
 *
 * The second half of section 5.5: whether `((a)|b)+` against `ab` reports
 * group 2 as `a` or as unset.
 */
typedef enum {
  GRX_CAPTURE_KEEP_LAST_SET = 0, ///< A capture set earlier survives.
  GRX_CAPTURE_RESET_EACH,        ///< Every iteration clears them first.
  /**
   * An iteration clears the ones *it* did not set, on the way out.
   *
   * The same reported answer as RESET_EACH - after the last iteration, the
   * captures it did not set are gone either way - and a different answer to
   * one question: what the *next* iteration can see. Perl clears late, so
   * `((?(2)x|y)(a))+` against "yaxa" takes the `x` branch second time round,
   * where clearing early would hide group 2 from the conditional and take
   * `y`. pcre2test agrees with Perl on that and keeps its captures besides.
   *
   * Only observable through a conditional or a backreference, which is why
   * codegen emits the late form only for a pattern that has one.
   */
  GRX_CAPTURE_RESET_AFTER_EACH,
  GRX_CAPTURE_RESET_COUNT        ///< Closes the enum; not a mode.
} GRX_CaptureResetMode;

/** @brief What a backreference to a group that did not participate does. */
typedef enum {
  GRX_BACKREF_UNSET_FAILS = 0,  ///< The reference fails to match.
  GRX_BACKREF_UNSET_EMPTY,      ///< It matches the empty string. ECMAScript.
  GRX_BACKREF_UNSET_COUNT       ///< Closes the enum; not a mode.
} GRX_BackrefUnsetMode;

/**
 * @brief What a search-all loop does after an empty match.
 *
 * documentation/dialects.md section 5.10. The dialect chose it; lowering
 * carries it onto the program, because an empty match is only discovered
 * during execution and grx_regex_search_next() is the one place the rule is
 * applied. The caller does not have to know there is more than one rule.
 */
typedef enum {
  GRX_ITERATE_RETRY_THEN_ADVANCE = 0, ///< Perl, PCRE2, Python.
  GRX_ITERATE_ADVANCE_ONE,            ///< ECMAScript.
  GRX_ITERATE_ADVANCE_SKIP_ABUTTING,  ///< Go.
  GRX_ITERATE_COUNT                   ///< Closes the enum; not a rule.
} GRX_IterationRule;

/**
 * @brief What `\G` asserts during a search-all loop.
 *
 * documentation/dialects.md section 5.10. The iteration rule above says what
 * position the *next attempt* starts at; this says what `\G` means once it
 * has. They are independent, and Perl and PCRE2 share the first and differ
 * on this one - which nothing noticed until
 * `tools/oracle/iterate_diff.py` asked both of them for every match rather
 * than for the first.
 *
 * The difference only shows after a *failure* forces the loop to advance.
 * `\Ga*` against "baac": both find the empty match at 0, both then fail to
 * find a non-empty one there, and both step to 1. PCRE2's `\G` is where the
 * current attempt begins, so it now holds at 1 and "aa" matches; perl's is
 * where the previous match *ended*, so it still holds only at 0 and the loop
 * is over. pcre2test reports four matches and perl reports one.
 *
 * Neither is a quirk. PCRE2 does not provide the loop - its caller writes it
 * and bumps the start offset - so `\G` can only mean the attempt. perl does
 * provide it, and `pos()` is a property of the string that a failed attempt
 * does not move.
 */
typedef enum {
  GRX_SEARCH_START_ATTEMPT = 0,  ///< PCRE2: where this attempt began.
  GRX_SEARCH_START_PREVIOUS_END, ///< Perl: where the previous match ended.
  GRX_SEARCH_START_RULE_COUNT    ///< Closes the enum; not a rule.
} GRX_SearchStartRule;

/**
 * @brief Which match a search reports when more than one is possible.
 *
 * Program-wide rather than per-node: it is a property of the dialect, and
 * the engines implement it as two search strategies rather than two
 * instruction sets (documentation/dialects.md section 5.1).
 */
typedef enum {
  GRX_PREFER_LEFTMOST_FIRST = 0, ///< Backtracking priority order. Perl, ES.
  GRX_PREFER_LEFTMOST_LONGEST,   ///< The longest at the leftmost start. POSIX.
  GRX_PREFER_COUNT               ///< Closes the enum; not a preference.
} GRX_MatchPreference;

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_SRC_CORE_SEMANTICS_INTERNAL_H
