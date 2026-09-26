/**
 * @file
 *
 * The `.rxt` conformance vector format: one record per pattern, subject and
 * expected result.
 *
 * documentation/testing.md section 3. Line-oriented with blank-line record
 * separators, so that a diff is readable and a record can be pasted into a
 * bug report without a tool to unpack it. That is the whole design
 * requirement: a vector file is evidence, and evidence a person cannot read
 * is evidence nobody checks.
 *
 * The reader is deliberately too simple to need the `text` library. A
 * conformance suite that depended on another library in the same suite would
 * be a suite whose failures have two possible causes.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRX_TESTS_CONFORMANCE_RXT_H
#define GHOTI_IO_GRX_TESTS_CONFORMANCE_RXT_H

#include <cstdint>
#include <string>
#include <vector>

#include <ghoti.io/regex/regex.h>

namespace grxtest {

/** What a record says should happen. */
enum class Expectation {
  Spans,     ///< A match, with a span per group.
  NoMatch,   ///< The search must not find anything.
  Error,     ///< Compiling must fail, with a named result code.
  Limit,     ///< The search must exhaust a limit.
  /**
   * Compiling must succeed, and nothing else is asserted.
   *
   * A corpus can say more about syntax than about matching. pcre2test's
   * `testinput1` and `testinput2` are thousands of patterns whose accept or
   * reject verdict the reference gives directly and unambiguously, which is
   * exactly what plan.md's WP-18 is measured on - while their *match*
   * answers need an oracle driver that does not exist yet. Without this
   * expectation a corpus like that could only contribute its rejections,
   * and "the patterns we are known to refuse" is the half of a syntax
   * corpus that cannot catch us refusing too much.
   */
  Compiles,
  /**
   * Compiling must fail, and which failure is not asserted.
   *
   * The dual of Compiles, and it exists because most references here fold
   * every refusal into one verdict: `re.compile` raises one exception type,
   * vim's `matchstrpos()` throws, `pcre2_compile` is asked through a driver
   * that prints "compile". A record saying `expect: error syntax` over one of
   * those is asserting something the oracle did not say - that the refusal is
   * a *syntax* refusal rather than `GRX_ERR_UNSUPPORTED` - and which of the
   * two this library gives is a rule of its own API, not of the reference.
   *
   * That distinction is worth a gate and it is not this one:
   * tests/unit/test_python.cpp and test_vim.cpp sweep each dialect's refused
   * vocabulary for the code, with the exceptions named. A generated corpus is
   * the wrong place for it, because a corpus asserts per row what a sweep
   * asserts per construct.
   */
  Refused,
};

/**
 * In a span, the subject's own length.
 *
 * `expect: 0-$` is the only way to write the end of a repeated subject
 * without doing the arithmetic in the file, and a number a person computed
 * by hand is a number that goes stale the moment `repeat:` changes.
 */
const size_t kSubjectLength = (size_t)-1;

/** One group's expected span, or "did not participate". */
struct Span {
  bool set = false;
  size_t start = 0;
  size_t end = 0;
};

/** One vector: everything needed to run a single check. */
struct Record {
  std::string source;      ///< File it came from, for a failure message.
  size_t line = 0;         ///< Line the record began on.
  std::string text;        ///< The record verbatim, for a failure message.

  GRX_Syntax syntax = GRX_SYNTAX_ECMASCRIPT;
  std::string pattern;     ///< Decoded: escapes already applied.
  std::string flags;       ///< The dialect's own alphabet.
  uint32_t options = 0;    ///< What the flags mean.
  bool has_subject = false;
  std::string subject;     ///< Decoded, and already repeated.

  /**
   * How many times `subject:` is repeated.
   *
   * A long subject written out is a vector nobody reads, and the format's
   * whole requirement is that a record can be pasted into a bug report. So
   * a megabyte of `a` is `subject: a` with `repeat: 1048576`, which is
   * still evidence a person can check.
   */
  size_t repeat = 1;

  Expectation expectation = Expectation::NoMatch;
  std::vector<Span> spans; ///< For Expectation::Spans.
  GRX_Result error = GRX_OK; ///< For Expectation::Error.

  /**
   * Each group's expected *text*, for a reference that states no spans.
   *
   * Every other reference here answers in offsets: node, pcre2test, perl,
   * glibc and CPython all name where each group started and stopped. **vim
   * does not, and has no API that does** - `matchstrpos()` gives the span of
   * the whole match and `matchlist()` gives the submatches as strings, so a
   * vector generated from vim can state the whole match exactly and each
   * group only by its contents. Recording spans for those groups would mean
   * taking them from *this library*, which is the one thing a vector must
   * never do: it would write down the current behaviour as the rule.
   *
   * So a record may carry `groups:` instead, and when it does the runner
   * checks each group's text rather than its offsets. That is weaker, and
   * the weakness is stated rather than hidden: a wrong span that cuts the
   * same bytes out of the subject passes such a record.
   *
   * `-` is the entry for a group that did not participate **or** matched
   * empty, because vim gives `''` for both and cannot be asked which. A
   * group past the end of this list is checked the same way, so a trailing
   * run of unset groups need not be written out.
   */
  std::vector<std::string> groups;
  bool has_groups = false; ///< Whether a `groups:` line was present.

  /** Engines to ask; empty means every eligible one. */
  std::vector<GRX_Engine> engines;
  /** Limit overrides, applied over grx_limits_default(). */
  bool has_limits = false;
  GRX_Limits limits {};
  /** A known deviation: run nothing, count it, keep the evidence. */
  std::string skip;
};

/** A parsed file: its records, and the file-level headers. */
struct VectorFile {
  std::string path;
  std::string unicode;    ///< The oracle's Unicode version, or empty.
  std::string generator;  ///< The first comment line, usually provenance.
  std::vector<Record> records;
};

/**
 * Read a `.rxt` file.
 *
 * @param path The file.
 * @param out_file Receives the records.
 * @param out_error Receives a message naming the line, on failure.
 * @return True on success.
 */
bool read_vector_file(
    const std::string & path, VectorFile * out_file, std::string * out_error);

/**
 * Parse the escape sequences a `pattern:` or `subject:` field may hold.
 *
 * `\xHH`, `\uHHHH`, `\u{H+}`, `\n`, `\r`, `\t`, `\0`, `\\`; anything else is
 * literal UTF-8. The result is bytes, because a pattern and a subject are
 * bytes to this library.
 *
 * @param field The field's text.
 * @param out_value Receives the decoded bytes.
 * @param out_error Receives a message on failure.
 * @return True on success.
 */
bool decode_field(const std::string & field, std::string * out_value,
    std::string * out_error);

/** Every `.rxt` file under a directory, sorted, recursively. */
std::vector<std::string> find_vector_files(const std::string & directory);

} // namespace grxtest

#endif // GHOTI_IO_GRX_TESTS_CONFORMANCE_RXT_H
