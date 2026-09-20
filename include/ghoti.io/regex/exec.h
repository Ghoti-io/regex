/**
 * @file
 *
 * Execution: running a compiled regex against a subject, and the match result.
 *
 * Three engines, because no single one covers the dialects:
 *
 * - The Pike VM runs the whole program in lockstep and so is linear in the
 *   subject length, but cannot express a backreference.
 * - The backtracking engine can, at the cost of an exponential worst case,
 *   which is what GRX_Limits::max_steps and max_backtrack exist to bound.
 * - The bit-state engine is the backtracker with a memo, which buys back the
 *   linear bound for the programs whose behaviour depends on nothing but
 *   where they are.
 *
 * GRX_ENGINE_AUTO picks the one with the strongest guarantee that can run the
 * program. A caller who needs that guarantee asks for an engine by name and
 * gets GRX_ERR_UNSUPPORTED for a pattern it cannot run, rather than silently
 * getting the engine that can hang.
 *
 * Status: all three are built. The constructs none of them runs yet -
 * conditionals, recursion and the backtracking control verbs - compile and
 * are then refused, rather than being ignored: a plausible wrong answer is
 * worse than no answer.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRX_EXEC_H
#define GHOTI_IO_GRX_EXEC_H

#include <ghoti.io/regex/allocator.h>
#include <ghoti.io/regex/compile.h>
#include <ghoti.io/regex/core.h>
#include <ghoti.io/regex/macros.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Which engine runs a match.
 */
typedef enum {
  GRX_ENGINE_AUTO = 0, ///< The fastest engine that can run this program.
  GRX_ENGINE_PIKE,     ///< Lockstep NFA simulation; linear time, no backrefs.
  GRX_ENGINE_BACKTRACK, ///< Backtracking; every feature, bounded by the limits.
  /**
   * Backtracking with a visited bitmap over (instruction, position).
   *
   * The same engine as GRX_ENGINE_BACKTRACK and the same answers, with one
   * addition: a state already tried and failed is not tried again. That
   * turns the exponential case into a linear one, at the cost of one bit per
   * instruction per subject position - which is why it is bounded by
   * GRX_Limits::max_match_memory and refused rather than degraded when the
   * bitmap will not fit.
   *
   * It is sound only for a program whose behaviour at a given instruction
   * and position does not depend on how it got there: no backreference
   * (which depends on what a group captured), no lookaround, no recursion,
   * and no empty-iteration guard (whose progress register is per-thread
   * history). grx_regex_facts() reports the first three; the fourth shows in
   * a disassembly as `registers=0`.
   */
  GRX_ENGINE_BITSTATE,
  GRX_ENGINE_COUNT     ///< Closes the enum; not an engine.
} GRX_Engine;

/**
 * @brief The span one capturing group matched.
 *
 * Both offsets are byte offsets into the subject. A group that did not
 * participate in the match has both set to GRX_NPOS, which is distinct from a
 * group that matched the empty string (`start == end`, both valid).
 */
typedef struct GRX_Capture {
  size_t start; ///< First byte of the span, or GRX_NPOS.
  size_t end;   ///< One past the last byte, or GRX_NPOS.
} GRX_Capture;

/**
 * @brief The mutable state of a match: where the groups landed.
 *
 * Kept separate from GRX_Regex so that the compiled regex stays immutable and
 * shareable, and so that a caller matching in a loop allocates once.
 */
typedef struct GRX_Match GRX_Match;

/**
 * @brief Bits for GRX_SearchOptions::flags.
 *
 * Each says something about the *subject* that the compiled pattern cannot
 * know, and so belongs to the search rather than to the regex. A caller
 * feeding one buffer in pieces, or searching one field of a larger record,
 * needs all of them; a caller with a whole string in hand needs none.
 */
typedef enum {
  GRX_SEARCH_NONE = 0,                  ///< No flags.
  GRX_SEARCH_NOTBOL = GRX_BIT(0),       ///< Offset 0 is not the start of a
                                        ///< line, so `^` and `\A` fail there.
  GRX_SEARCH_NOTEOL = GRX_BIT(1),       ///< The end is not the end of a line,
                                        ///< so `$`, `\Z` and `\z` fail there.
  GRX_SEARCH_NOTEMPTY = GRX_BIT(2),     ///< An empty match is not a match.
  GRX_SEARCH_NOTEMPTY_ATSTART = GRX_BIT(3), ///< An empty match is not a match
                                        ///< when it begins at `begin`.
  GRX_SEARCH_NO_UTF_CHECK = GRX_BIT(4)  ///< The caller guarantees the subject
                                        ///< is valid UTF-8.
} GRX_SearchFlag;

/**
 * @brief Everything one search needs beyond the subject itself.
 *
 * A struct rather than more parameters, because the list grows: the scaffold's
 * `grx_regex_search()` already took eight arguments and adding a window and
 * five flags to it positionally would make a mis-ordered call something the
 * compiler cannot see.
 *
 * **The window.** `begin` and `end` bound the search inside a larger buffer.
 * `end` is where the subject *ends* for this search: nothing at or past it is
 * read, and `$`, `\Z` and `\z` hold there. `begin` is only where the search
 * starts - a lookbehind may still read the bytes before it, and `^` and `\A`
 * still mean offset 0, not `begin`. That asymmetry is deliberate and is what
 * makes iteration correct: the second call of a search-all loop must not
 * report `^` as holding where the first match happened to stop.
 *
 * Zero-initialising the struct gives a search of the whole subject with no
 * flags on GRX_ENGINE_AUTO with default limits, which is what
 * grx_search_options_default() writes.
 */
typedef struct GRX_SearchOptions {
  size_t begin;   ///< First byte a match may begin at.
  size_t end;     ///< One past the last byte visible; GRX_NPOS for all of it.
  uint32_t flags; ///< GRX_SearchFlag bits.
  GRX_Engine engine; ///< Which engine to use. GRX_ENGINE_AUTO chooses.
  const GRX_Limits * limits; ///< Caps to apply. NULL uses the defaults.
} GRX_SearchOptions;

/**
 * @brief Fill in the options for an ordinary whole-subject search.
 *
 * @param out_options The struct to initialise. NULL is ignored.
 */
GRX_API void grx_search_options_default(GRX_SearchOptions * out_options);

/**
 * @brief Create a match object sized for a regex.
 *
 * @param regex The regex it will be used with. NULL is invalid.
 * @param allocator Allocator for the match. NULL uses the default.
 * @param out_match Receives the new match object on success.
 * @return GRX_OK, GRX_ERR_INVALID, or GRX_ERR_OOM.
 */
GRX_API GRX_Result grx_match_create(const GRX_Regex * regex,
    const GRX_Allocator * allocator, GRX_Match ** out_match);

/**
 * @brief Search a subject for the first match at or after an offset.
 *
 * @param regex The regex. NULL is invalid.
 * @param subject The bytes to search. May be NULL only when `length` is 0.
 * @param length Length of `subject` in bytes.
 * @param start Byte offset to start at; at most `length`.
 * @param engine Which engine to use. GRX_ENGINE_AUTO chooses.
 * @param limits Caps to apply. NULL uses grx_limits_default().
 * @param match Receives the capture spans. Optional; NULL tests for a match
 *   without recording where.
 * @param out_matched Receives non-zero when a match was found. Required; "no
 *   match" is an outcome, not an error, and so is not a result code.
 * @return GRX_OK, GRX_ERR_UNSUPPORTED, GRX_ERR_LIMIT, GRX_ERR_INVALID, or
 *   GRX_ERR_OOM.
 */
GRX_API GRX_Result grx_regex_search(const GRX_Regex * regex,
    const char * subject, size_t length, size_t start, GRX_Engine engine,
    const GRX_Limits * limits, GRX_Match * match, int * out_matched);

/**
 * @brief Match a subject against a regex anchored at an offset.
 *
 * As grx_regex_search(), except that the match must begin at `start`.
 *
 * @param regex The regex. NULL is invalid.
 * @param subject The bytes to match. May be NULL only when `length` is 0.
 * @param length Length of `subject` in bytes.
 * @param start Byte offset the match must begin at; at most `length`.
 * @param engine Which engine to use. GRX_ENGINE_AUTO chooses.
 * @param limits Caps to apply. NULL uses grx_limits_default().
 * @param match Receives the capture spans. Optional.
 * @param out_matched Receives non-zero when the regex matched. Required.
 * @return GRX_OK, GRX_ERR_UNSUPPORTED, GRX_ERR_LIMIT, GRX_ERR_INVALID, or
 *   GRX_ERR_OOM.
 */
GRX_API GRX_Result grx_regex_match(const GRX_Regex * regex,
    const char * subject, size_t length, size_t start, GRX_Engine engine,
    const GRX_Limits * limits, GRX_Match * match, int * out_matched);

/**
 * @brief Search a subject, with a window and search flags.
 *
 * The general form; grx_regex_search() is this with default options.
 *
 * @param regex The regex. NULL is invalid.
 * @param subject The bytes to search. May be NULL only when `length` is 0.
 * @param length Length of `subject` in bytes.
 * @param options The window, flags, engine and limits. NULL uses the
 *   defaults.
 * @param match Receives the capture spans. Optional.
 * @param out_matched Receives non-zero when a match was found. Required.
 * @return GRX_OK, GRX_ERR_UNSUPPORTED, GRX_ERR_LIMIT, GRX_ERR_INVALID, or
 *   GRX_ERR_OOM.
 */
GRX_API GRX_Result grx_regex_search_ex(const GRX_Regex * regex,
    const char * subject, size_t length, const GRX_SearchOptions * options,
    GRX_Match * match, int * out_matched);

/**
 * @brief Match a subject anchored at the window's start, with search flags.
 *
 * As grx_regex_search_ex(), except that the match must begin at
 * GRX_SearchOptions::begin.
 *
 * @param regex The regex. NULL is invalid.
 * @param subject The bytes to match. May be NULL only when `length` is 0.
 * @param length Length of `subject` in bytes.
 * @param options The window, flags, engine and limits. NULL uses the
 *   defaults.
 * @param match Receives the capture spans. Optional.
 * @param out_matched Receives non-zero when the regex matched. Required.
 * @return GRX_OK, GRX_ERR_UNSUPPORTED, GRX_ERR_LIMIT, GRX_ERR_INVALID, or
 *   GRX_ERR_OOM.
 */
GRX_API GRX_Result grx_regex_match_ex(const GRX_Regex * regex,
    const char * subject, size_t length, const GRX_SearchOptions * options,
    GRX_Match * match, int * out_matched);

/**
 * @brief Find the next match after the one already in `match`.
 *
 * The search-all loop, with the dialect's rule for what follows an empty
 * match applied for the caller. There are three such rules in the wild
 * (documentation/dialects.md section 5.10) and which one a dialect uses is
 * not something a caller should have to look up: a loop written as
 *
 * ```c
 * int matched = 0;
 * grx_regex_search_ex(regex, s, n, NULL, match, &matched);
 * while (matched) {
 *   // ... use the match ...
 *   grx_regex_search_next(regex, s, n, NULL, match, &matched);
 * }
 * ```
 *
 * terminates and reports what the dialect's own engine would report, for
 * every dialect.
 *
 * When `match` holds no previous match - a fresh object, or one whose last
 * search found nothing - this is grx_regex_search_ex().
 *
 * @param regex The regex. NULL is invalid.
 * @param subject The bytes to search. Must be the same bytes the previous
 *   search ran against.
 * @param length Length of `subject` in bytes.
 * @param options The window, flags, engine and limits. NULL uses the
 *   defaults. GRX_SearchOptions::begin is used only when there is no
 *   previous match; `end` and the flags apply to every call.
 * @param match Carries the previous match in and the next one out. Required
 *   here, unlike the other entry points: it is the only record of where the
 *   loop had got to.
 * @param out_matched Receives non-zero when a further match was found.
 *   Required.
 * @return GRX_OK, GRX_ERR_UNSUPPORTED, GRX_ERR_LIMIT, GRX_ERR_INVALID, or
 *   GRX_ERR_OOM.
 */
GRX_API GRX_Result grx_regex_search_next(const GRX_Regex * regex,
    const char * subject, size_t length, const GRX_SearchOptions * options,
    GRX_Match * match, int * out_matched);

/**
 * @brief The span of the whole match.
 *
 * grx_match_group() with index 0, which is the group every caller wants and
 * the one place an off-by-one in a caller's group numbering is silent.
 *
 * @param match The match object. NULL is invalid.
 * @param out_capture Receives the span.
 * @return GRX_OK, or GRX_ERR_INVALID for a NULL argument.
 */
GRX_API GRX_Result grx_match_span(
    const GRX_Match * match, GRX_Capture * out_capture);

/**
 * @brief Instructions executed by the last search.
 *
 * What GRX_Limits::max_steps counts, so that a caller who wants to set that
 * limit from measurement rather than from guesswork can measure it. Reset at
 * the start of every search, including each attempt inside
 * grx_regex_search_next().
 *
 * @param match The match object. NULL returns 0.
 * @return The step count.
 */
GRX_API size_t grx_match_steps(const GRX_Match * match);

/**
 * @brief Which engine actually ran the last match.
 *
 * Meaningful after a successful grx_regex_search() or grx_regex_match() with
 * GRX_ENGINE_AUTO, which is the only way to find out what AUTO chose.
 *
 * @param match The match object. NULL returns GRX_ENGINE_COUNT.
 * @return The engine used.
 */
GRX_API GRX_Engine grx_match_engine(const GRX_Match * match);

/**
 * @brief The number of capture spans a match object holds.
 *
 * This is the regex's capture count plus one, because group 0 is the whole
 * match.
 *
 * @param match The match object. NULL returns 0.
 * @return The number of groups, including group 0.
 */
GRX_API size_t grx_match_count(const GRX_Match * match);

/**
 * @brief The span one group matched.
 *
 * @param match The match object. NULL is invalid.
 * @param index Group index; 0 is the whole match.
 * @param out_capture Receives the span. A group that did not participate
 *   comes back as GRX_NPOS/GRX_NPOS rather than an error.
 * @return GRX_OK, or GRX_ERR_INVALID for an out-of-range index or a NULL
 *   argument.
 */
GRX_API GRX_Result grx_match_group(
    const GRX_Match * match, size_t index, GRX_Capture * out_capture);

/**
 * @brief The span one named group matched.
 *
 * @param match The match object. NULL is invalid.
 * @param name The group name. NULL is invalid.
 * @param out_capture Receives the span.
 * @return GRX_OK, or GRX_ERR_INVALID for an unknown name or a NULL argument.
 */
GRX_API GRX_Result grx_match_group_named(
    const GRX_Match * match, const char * name, GRX_Capture * out_capture);

/**
 * @brief The `(*MARK:NAME)` the last attempt passed through.
 *
 * PCRE2's pcre2_get_mark(). After a match, this is the last mark still
 * standing on the path that matched - a mark passed on a branch that was
 * then abandoned is not one. After a failure it is the last mark reached at
 * all, which is what makes `(*MARK)` useful for saying *why* a pattern did
 * not match.
 *
 * The pointer belongs to the regex the match was created for and is valid
 * for as long as it is.
 *
 * @param match The match object. NULL is invalid.
 * @return The mark name, or NULL when the attempt passed none.
 */
GRX_API const char * grx_match_mark(const GRX_Match * match);

/**
 * @brief Write a human-readable form of a match.
 *
 * For debugging and for tests; the format is not stable across versions.
 *
 * @param match The match object.
 * @param out Destination.
 * @return GRX_OK, or GRX_ERR_INVALID for a NULL argument.
 */
GRX_API GRX_Result grx_match_dump(const GRX_Match * match, FILE * out);

/**
 * @brief Destroy a match object. NULL is ignored.
 *
 * @param match The match object.
 */
GRX_API void grx_match_destroy(GRX_Match * match);

#ifdef __cplusplus
}
#endif

#endif // GHOTI_IO_GRX_EXEC_H
