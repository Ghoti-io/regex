/**
 * @file
 *
 * Reading `.rxt` conformance vectors.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include "rxt.h"

#include "../../src/unicode/unicode_internal.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <dirent.h>
#include <fstream>
#include <sstream>
#include <sys/stat.h>

namespace grxtest {

namespace {

/** Strip leading and trailing spaces and tabs. */
std::string trim(const std::string & text) {
  size_t first = text.find_first_not_of(" \t\r");
  if (first == std::string::npos) {
    return std::string();
  }
  size_t last = text.find_last_not_of(" \t\r");
  return text.substr(first, last - first + 1);
}

/** Append one code point as UTF-8. */
void append_utf8(std::string * out, uint32_t codepoint) {
  char buffer[4];
  size_t width = grx_unicode_utf8_encode(codepoint, buffer);
  out->append(buffer, width);
}

int hex_value(char c) {
  if (c >= '0' && c <= '9') {
    return c - '0';
  }
  if (c >= 'a' && c <= 'f') {
    return c - 'a' + 10;
  }
  if (c >= 'A' && c <= 'F') {
    return c - 'A' + 10;
  }
  return -1;
}

} // namespace

bool decode_field(const std::string & field, std::string * out_value,
    std::string * out_error) {
  out_value->clear();

  for (size_t i = 0; i < field.size(); i++) {
    if (field[i] != '\\') {
      out_value->push_back(field[i]);
      continue;
    }
    if (i + 1 >= field.size()) {
      *out_error = "a field ends in a backslash";
      return false;
    }

    char kind = field[++i];
    switch (kind) {
      case 'n': out_value->push_back('\n'); break;
      case 'r': out_value->push_back('\r'); break;
      case 't': out_value->push_back('\t'); break;
      case 'f': out_value->push_back('\f'); break;
      case 'v': out_value->push_back('\v'); break;
      case '0': out_value->push_back('\0'); break;
      case '\\': out_value->push_back('\\'); break;

      case 'x': {
        if (i + 2 >= field.size()) {
          *out_error = "\\x needs two hex digits";
          return false;
        }
        int high = hex_value(field[i + 1]);
        int low = hex_value(field[i + 2]);
        if (high < 0 || low < 0) {
          *out_error = "\\x needs two hex digits";
          return false;
        }
        // A byte, not a code point: this is how a vector holds a subject
        // that is deliberately not valid UTF-8.
        out_value->push_back((char)((high << 4) | low));
        i += 2;
        break;
      }

      case 'u': {
        uint32_t codepoint = 0;
        if (i + 1 < field.size() && field[i + 1] == '{') {
          size_t close = field.find('}', i + 2);
          if (close == std::string::npos || close == i + 2) {
            *out_error = "\\u{...} is not closed";
            return false;
          }
          for (size_t j = i + 2; j < close; j++) {
            int digit = hex_value(field[j]);
            if (digit < 0) {
              *out_error = "\\u{...} needs hex digits";
              return false;
            }
            codepoint = (codepoint << 4) | (uint32_t)digit;
          }
          i = close;
        }
        else {
          if (i + 4 >= field.size()) {
            *out_error = "\\u needs four hex digits";
            return false;
          }
          for (size_t j = 1; j <= 4; j++) {
            int digit = hex_value(field[i + j]);
            if (digit < 0) {
              *out_error = "\\u needs four hex digits";
              return false;
            }
            codepoint = (codepoint << 4) | (uint32_t)digit;
          }
          i += 4;
        }
        append_utf8(out_value, codepoint);
        break;
      }

      default:
        // An escape this format does not define keeps *both* characters, so
        // that a hand-written vector can say `pattern: \d+` and mean it.
        // Writing it the other way - dropping the backslash - meant a
        // `pattern: (a|b)\1` in a fixture silently became `(a|b)1`, which
        // then failed as a wrong answer rather than as a wrong vector.
        //
        // It stays unambiguous because `\\` is defined: `\n` is a newline
        // and `\\n` is a backslash followed by an `n`.
        out_value->push_back('\\');
        out_value->push_back(kind);
        break;
    }
  }

  return true;
}

namespace {

/**
 * What `repeat:` may ask for.
 *
 * The count bound keeps a typo from being an allocation failure; the byte
 * bound is what the whole suite is willing to hold for one record. 64 MB is
 * far above the longest vector here (1 MB) and far below anything that
 * would trouble a CI runner.
 */
const size_t kMaxRepeat = 1u << 24;
const size_t kMaxSubject = 64u << 20;

/** Parse an `expect:` field into a record. */
bool parse_expectation(
    const std::string & value, Record * record, std::string * out_error) {
  if (value == "nomatch") {
    record->expectation = Expectation::NoMatch;
    return true;
  }
  if (value == "limit") {
    record->expectation = Expectation::Limit;
    return true;
  }
  if (value == "compiles") {
    record->expectation = Expectation::Compiles;
    return true;
  }
  if (value.rfind("error", 0) == 0) {
    record->expectation = Expectation::Error;
    std::string which = trim(value.substr(5));
    if (which == "syntax") {
      record->error = GRX_ERR_SYNTAX;
    }
    else if (which == "unsupported") {
      record->error = GRX_ERR_UNSUPPORTED;
    }
    else if (which == "limit") {
      record->error = GRX_ERR_LIMIT;
    }
    else if (which == "invalid") {
      record->error = GRX_ERR_INVALID;
    }
    else {
      *out_error = "unknown error kind: " + which;
      return false;
    }
    return true;
  }

  record->expectation = Expectation::Spans;
  std::istringstream fields(value);
  std::string field;
  while (fields >> field) {
    Span span;
    if (field == "-") {
      record->spans.push_back(span);
      continue;
    }
    size_t dash = field.find('-');
    if (dash == std::string::npos || !dash) {
      *out_error = "a span is <start>-<end> or -: " + field;
      return false;
    }
    span.set = true;
    std::string start = field.substr(0, dash);
    std::string end = field.substr(dash + 1);
    span.start = start == "$" ? kSubjectLength : (size_t)std::stoul(start);
    span.end = end == "$" ? kSubjectLength : (size_t)std::stoul(end);
    record->spans.push_back(span);
  }

  if (record->spans.empty()) {
    *out_error = "an expect: with no spans; use nomatch";
    return false;
  }
  return true;
}

/** Parse an `engines:` field. */
bool parse_engines(
    const std::string & value, Record * record, std::string * out_error) {
  std::istringstream fields(value);
  std::string field;
  while (fields >> field) {
    if (field == "pike") {
      record->engines.push_back(GRX_ENGINE_PIKE);
    }
    else if (field == "backtrack") {
      record->engines.push_back(GRX_ENGINE_BACKTRACK);
    }
    else if (field == "bitstate") {
      record->engines.push_back(GRX_ENGINE_BITSTATE);
    }
    else {
      *out_error = "unknown engine: " + field;
      return false;
    }
  }
  return true;
}

/**
 * Parse an `options:` field of space-separated option names.
 *
 * `flags:` is the dialect's own alphabet, which is the right way to write an
 * option a *pattern author* can write. POSIX and GNU have no such alphabet -
 * their options are arguments to `regcomp`, `REG_ICASE` and `REG_NEWLINE` -
 * so a vector for those dialects has no letter to put in `flags:` and needs
 * to name the option directly. The two combine, in whichever order they
 * appear.
 */
bool parse_options(
    const std::string & value, Record * record, std::string * out_error) {
  static const struct {
    const char * name;
    uint32_t option;
  } known[] = {
    {"caseless", GRX_OPT_CASELESS},
    {"multiline", GRX_OPT_MULTILINE},
    {"dotall", GRX_OPT_DOTALL},
    {"extended", GRX_OPT_EXTENDED},
    {"ungreedy", GRX_OPT_UNGREEDY},
    {"anchored", GRX_OPT_ANCHORED},
    {"utf", GRX_OPT_UTF},
    {"ucp", GRX_OPT_UCP},
    {"no-capture", GRX_OPT_NO_CAPTURE},
    {"literal", GRX_OPT_LITERAL},
  };

  std::istringstream fields(value);
  std::string field;
  while (fields >> field) {
    bool found = false;
    for (const auto & row : known) {
      if (field == row.name) {
        record->options |= row.option;
        found = true;
        break;
      }
    }
    if (!found) {
      *out_error = "unknown option: " + field;
      return false;
    }
  }
  return true;
}

/** Parse a `limits:` field of `name=value` pairs. */
bool parse_limits(
    const std::string & value, Record * record, std::string * out_error) {
  if (!record->has_limits) {
    grx_limits_default(&record->limits);
    record->has_limits = true;
  }

  std::istringstream fields(value);
  std::string field;
  while (fields >> field) {
    size_t equals = field.find('=');
    if (equals == std::string::npos) {
      *out_error = "a limit is name=value: " + field;
      return false;
    }
    std::string name = field.substr(0, equals);
    size_t number = (size_t)std::stoul(field.substr(equals + 1));

    if (name == "max_steps") {
      record->limits.max_steps = number;
    }
    else if (name == "max_backtrack") {
      record->limits.max_backtrack = number;
    }
    else if (name == "max_program_size") {
      record->limits.max_program_size = number;
    }
    else if (name == "max_nodes") {
      record->limits.max_nodes = number;
    }
    else if (name == "max_repeat_count") {
      record->limits.max_repeat_count = number;
    }
    else if (name == "max_nesting_depth") {
      record->limits.max_nesting_depth = number;
    }
    else if (name == "max_match_memory") {
      record->limits.max_match_memory = number;
    }
    // The rest of GRX_Limits, so that a vector can tune any field rather
    // than the seven somebody happened to need first. Every one of them
    // reads 0 as "no limit"; core.h says so and
    // tests/unit/test_limits.cpp checks it field by field.
    else if (name == "max_pattern_length") {
      record->limits.max_pattern_length = number;
    }
    else if (name == "max_captures") {
      record->limits.max_captures = number;
    }
    else if (name == "max_class_ranges") {
      record->limits.max_class_ranges = number;
    }
    else if (name == "max_lookbehind_length") {
      record->limits.max_lookbehind_length = number;
    }
    else if (name == "max_recursion_depth") {
      record->limits.max_recursion_depth = number;
    }
    else if (name == "max_subject_length") {
      record->limits.max_subject_length = number;
    }
    else {
      *out_error = "unknown limit: " + name;
      return false;
    }
  }
  return true;
}

/** The options a dialect's flag letters mean, via the library's own parser. */
bool options_for_flags(GRX_Syntax syntax, const std::string & flags,
    uint32_t * out_options, std::string * out_error) {
  GRX_Error error;
  GRX_Result result = grx_options_parse(
      syntax, flags.c_str(), out_options, &error);
  if (result != GRX_OK) {
    *out_error = std::string("flags: ") + error.message;
    return false;
  }
  return true;
}

} // namespace

bool read_vector_file(
    const std::string & path, VectorFile * out_file, std::string * out_error) {
  std::ifstream input(path);
  if (!input) {
    *out_error = "cannot open " + path;
    return false;
  }

  out_file->path = path;
  out_file->records.clear();

  GRX_Syntax file_syntax = GRX_SYNTAX_ECMASCRIPT;
  bool file_syntax_set = false;

  Record record;
  std::string record_text;
  size_t record_line = 0;
  bool record_open = false;
  std::string line;
  size_t line_number = 0;

  auto finish = [&]() -> bool {
    if (!record_open) {
      return true;
    }
    record.source = path;
    record.line = record_line;
    record.text = record_text;
    record.syntax = file_syntax;

    if (record.repeat != 1) {
      if (!record.has_subject) {
        *out_error = path + ":" + std::to_string(record_line)
            + ": repeat: with no subject:";
        return false;
      }
      if (record.subject.empty()) {
        *out_error = path + ":" + std::to_string(record_line)
            + ": repeat: of an empty subject:";
        return false;
      }
      if (record.subject.size() > kMaxSubject / record.repeat) {
        *out_error = path + ":" + std::to_string(record_line)
            + ": repeat: would build a subject over "
            + std::to_string(kMaxSubject) + " bytes";
        return false;
      }
      std::string once;
      once.swap(record.subject);
      record.subject.reserve(once.size() * record.repeat);
      for (size_t i = 0; i < record.repeat; i++) {
        record.subject += once;
      }
    }

    // `$` means the subject's length, and the subject is only now its final
    // size. Resolved here rather than at parse time for that reason: a
    // record may say `repeat:` after its `expect:`.
    for (Span & span : record.spans) {
      if (!span.set) {
        continue;
      }
      if (span.start == kSubjectLength) {
        span.start = record.subject.size();
      }
      if (span.end == kSubjectLength) {
        span.end = record.subject.size();
      }
    }
    // The test of grx_options_parse() is that every vector's flags parse
    // (documentation/testing.md section 3). A vector whose flags it rejects
    // is a failure of one or the other, and either way it must be said.
    std::string failure;
    uint32_t from_flags = 0;
    if (!options_for_flags(
            file_syntax, record.flags, &from_flags, &failure)) {
      *out_error = path + ":" + std::to_string(record_line) + ": " + failure;
      return false;
    }
    // OR rather than assign: an `options:` line has already written into
    // `record.options`, and the two must combine whichever order they were
    // written in.
    record.options |= from_flags;
    out_file->records.push_back(record);

    record = Record();
    record_text.clear();
    record_open = false;
    return true;
  };

  while (std::getline(input, line)) {
    line_number++;
    // Only the line ending is stripped here; a value's own spaces are the
    // value's business, and `trim` is applied per key below.
    std::string body = line;
    while (!body.empty() && (body.back() == '\r' || body.back() == '\n')) {
      body.pop_back();
    }
    if (trim(body).empty()) {
      body.clear();
    }

    if (!body.empty() && trim(body)[0] == '#') {
      body = trim(body);
      if (out_file->generator.empty()) {
        out_file->generator = trim(body.substr(1));
      }
      continue;
    }
    if (body.empty()) {
      if (!finish()) {
        return false;
      }
      continue;
    }

    size_t colon = body.find(':');
    if (colon == std::string::npos) {
      *out_error = path + ":" + std::to_string(line_number)
          + ": a line is `key: value`";
      return false;
    }
    std::string key = trim(body.substr(0, colon));

    // `pattern` and `subject` keep their spaces. Only the single space after
    // the colon is removed, because a subject may legitimately begin or end
    // with one - and trimming it shifts every offset in the record, which is
    // a failure that looks like an engine bug rather than a reader bug. The
    // generator escapes edge spaces as well, so a file is readable either
    // way; this is what makes a hand-written vector safe too.
    std::string value;
    if (key == "pattern" || key == "subject") {
      value = body.substr(colon + 1);
      if (!value.empty() && value[0] == ' ') {
        value.erase(0, 1);
      }
    }
    else {
      value = trim(body.substr(colon + 1));
    }

    if (!record_open) {
      record_line = line_number;
    }
    record_text += line + "\n";

    std::string failure;
    if (key == "dialect") {
      GRX_Syntax syntax;
      if (grx_syntax_from_name(value.c_str(), &syntax) != GRX_OK) {
        *out_error = path + ":" + std::to_string(line_number)
            + ": unknown dialect: " + value;
        return false;
      }
      file_syntax = syntax;
      file_syntax_set = true;
      record_text.clear();
      continue;
    }
    if (key == "unicode") {
      out_file->unicode = value;
      record_text.clear();
      continue;
    }

    record_open = true;
    if (key == "pattern") {
      if (!decode_field(value, &record.pattern, &failure)) {
        *out_error = path + ":" + std::to_string(line_number) + ": " + failure;
        return false;
      }
    }
    else if (key == "subject") {
      if (!decode_field(value, &record.subject, &failure)) {
        *out_error = path + ":" + std::to_string(line_number) + ": " + failure;
        return false;
      }
      record.has_subject = true;
    }
    else if (key == "flags") {
      record.flags = value;
    }
    else if (key == "expect") {
      if (!parse_expectation(value, &record, &failure)) {
        *out_error = path + ":" + std::to_string(line_number) + ": " + failure;
        return false;
      }
    }
    else if (key == "options") {
      if (!parse_options(value, &record, &failure)) {
        *out_error = path + ":" + std::to_string(line_number) + ": " + failure;
        return false;
      }
    }
    else if (key == "engines") {
      if (!parse_engines(value, &record, &failure)) {
        *out_error = path + ":" + std::to_string(line_number) + ": " + failure;
        return false;
      }
    }
    else if (key == "limits") {
      if (!parse_limits(value, &record, &failure)) {
        *out_error = path + ":" + std::to_string(line_number) + ": " + failure;
        return false;
      }
    }
    else if (key == "repeat") {
      // Bounded, because a typo here is an out-of-memory rather than a
      // failed assertion, and a suite that dies has told nobody anything.
      unsigned long long count = 0;
      try {
        count = std::stoull(value);
      }
      catch (const std::exception &) {
        *out_error = path + ":" + std::to_string(line_number)
            + ": repeat: wants a count";
        return false;
      }
      if (count < 1 || count > kMaxRepeat) {
        *out_error = path + ":" + std::to_string(line_number)
            + ": repeat: must be 1.." + std::to_string(kMaxRepeat);
        return false;
      }
      record.repeat = (size_t)count;
    }
    else if (key == "skip") {
      record.skip = value;
    }
    else {
      *out_error = path + ":" + std::to_string(line_number)
          + ": unknown key: " + key;
      return false;
    }
  }

  if (!finish()) {
    return false;
  }
  if (!file_syntax_set && !out_file->records.empty()) {
    *out_error = path + ": no `dialect:` header";
    return false;
  }

  return true;
}

std::vector<std::string> find_vector_files(const std::string & directory) {
  std::vector<std::string> found;

  DIR * handle = opendir(directory.c_str());
  if (!handle) {
    return found;
  }

  struct dirent * entry;
  while ((entry = readdir(handle)) != nullptr) {
    std::string name = entry->d_name;
    if (name == "." || name == "..") {
      continue;
    }
    std::string path = directory + "/" + name;

    struct stat info;
    if (stat(path.c_str(), &info) != 0) {
      continue;
    }
    if (S_ISDIR(info.st_mode)) {
      for (const std::string & nested : find_vector_files(path)) {
        found.push_back(nested);
      }
      continue;
    }
    if (name.size() > 4 && name.compare(name.size() - 4, 4, ".rxt") == 0) {
      found.push_back(path);
    }
  }
  closedir(handle);

  std::sort(found.begin(), found.end());
  return found;
}

} // namespace grxtest
