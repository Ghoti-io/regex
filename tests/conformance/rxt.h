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
};

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
  std::string subject;     ///< Decoded.

  Expectation expectation = Expectation::NoMatch;
  std::vector<Span> spans; ///< For Expectation::Spans.
  GRX_Result error = GRX_OK; ///< For Expectation::Error.

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
