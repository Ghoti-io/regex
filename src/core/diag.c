/**
 * @file
 *
 * The diagnostic catalogue: one row per GRX_Diag, giving its text and the
 * result code it implies.
 *
 * Kept as a table indexed by the enum rather than a switch, so that a
 * diagnostic added without a row is a NULL the test catches rather than a
 * silent fall through to a default. See DiagnosticCatalogueIsComplete in
 * tests/unit/test_diag.cpp.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#include <ghoti.io/regex/macros.h>

#include <ghoti.io/regex/core.h>
#include <stdio.h>
#include <string.h>

#include "core_internal.h"

/** One diagnostic: what to say about it, and what code it reports as. */
typedef struct {
  const char * text;
  GRX_Result code;
} DiagRow;

// Designated initialisers, so a row may be added in any position and a
// missing one is a zeroed entry - a NULL text - rather than a neighbour's
// message under the wrong name.
static const DiagRow diag_table[GRX_DIAG_COUNT] = {
  [GRX_DIAG_NONE] = {"no error", GRX_OK},

  [GRX_DIAG_UNMATCHED_OPEN_PAREN]
      = {"unmatched opening parenthesis", GRX_ERR_SYNTAX},
  [GRX_DIAG_UNMATCHED_CLOSE_PAREN]
      = {"unmatched closing parenthesis", GRX_ERR_SYNTAX},
  [GRX_DIAG_UNMATCHED_OPEN_BRACKET]
      = {"unterminated character class", GRX_ERR_SYNTAX},
  [GRX_DIAG_UNMATCHED_OPEN_BRACE]
      = {"unterminated quantifier", GRX_ERR_SYNTAX},
  [GRX_DIAG_TRAILING_BACKSLASH]
      = {"pattern ends with a backslash", GRX_ERR_SYNTAX},
  [GRX_DIAG_UNTERMINATED_COMMENT]
      = {"unterminated comment group", GRX_ERR_SYNTAX},
  [GRX_DIAG_UNTERMINATED_QUOTE]
      = {"unterminated quoted sequence", GRX_ERR_SYNTAX},
  [GRX_DIAG_UNTERMINATED_NAME]
      = {"unterminated group name", GRX_ERR_SYNTAX},

  [GRX_DIAG_NOTHING_TO_REPEAT]
      = {"quantifier has nothing to repeat", GRX_ERR_SYNTAX},
  [GRX_DIAG_DOUBLE_QUANTIFIER]
      = {"quantifier applied to a quantifier", GRX_ERR_SYNTAX},
  [GRX_DIAG_QUANTIFIER_OUT_OF_ORDER]
      = {"quantifier minimum exceeds its maximum", GRX_ERR_SYNTAX},
  [GRX_DIAG_INVALID_QUANTIFIER]
      = {"malformed repetition count", GRX_ERR_SYNTAX},
  [GRX_DIAG_QUANTIFIED_ASSERTION]
      = {"quantifier applied to a zero-width assertion", GRX_ERR_SYNTAX},

  [GRX_DIAG_INVALID_ESCAPE]
      = {"unrecognized escape sequence", GRX_ERR_SYNTAX},
  [GRX_DIAG_INVALID_HEX_ESCAPE]
      = {"malformed hexadecimal escape", GRX_ERR_SYNTAX},
  [GRX_DIAG_INVALID_OCTAL_ESCAPE]
      = {"malformed octal escape", GRX_ERR_SYNTAX},
  [GRX_DIAG_INVALID_UNICODE_ESCAPE]
      = {"malformed Unicode escape", GRX_ERR_SYNTAX},
  [GRX_DIAG_INVALID_CONTROL_ESCAPE]
      = {"malformed control escape", GRX_ERR_SYNTAX},
  [GRX_DIAG_CODEPOINT_OUT_OF_RANGE]
      = {"code point is not a Unicode scalar value", GRX_ERR_SYNTAX},
  [GRX_DIAG_UNESCAPED_METACHARACTER]
      = {"this character must be escaped to be a literal", GRX_ERR_SYNTAX},
  [GRX_DIAG_INVALID_UTF8_IN_PATTERN]
      = {"pattern is not valid UTF-8", GRX_ERR_SYNTAX},

  [GRX_DIAG_INVALID_CLASS_RANGE]
      = {"character class range is out of order", GRX_ERR_SYNTAX},
  [GRX_DIAG_INVALID_CLASS_ITEM]
      = {"item is not allowed in a character class", GRX_ERR_SYNTAX},
  [GRX_DIAG_EMPTY_CLASS]
      = {"empty character class", GRX_ERR_SYNTAX},
  [GRX_DIAG_CLASS_ESCAPE_IN_RANGE]
      = {"class escape used as a range endpoint", GRX_ERR_SYNTAX},
  [GRX_DIAG_UNKNOWN_POSIX_CLASS]
      = {"unknown POSIX character class", GRX_ERR_SYNTAX},
  [GRX_DIAG_UNKNOWN_PROPERTY]
      = {"unknown Unicode property", GRX_ERR_SYNTAX},
  [GRX_DIAG_INVALID_PROPERTY_SYNTAX]
      = {"malformed Unicode property escape", GRX_ERR_SYNTAX},
  [GRX_DIAG_INVALID_CLASS_SET_OP]
      = {"invalid character class set operation", GRX_ERR_SYNTAX},
  [GRX_DIAG_CLASS_NESTING_TOO_DEEP]
      = {"character class nesting is too deep", GRX_ERR_SYNTAX},

  [GRX_DIAG_INVALID_GROUP_NAME]
      = {"invalid group name", GRX_ERR_SYNTAX},
  [GRX_DIAG_DUPLICATE_GROUP_NAME]
      = {"group name used more than once", GRX_ERR_SYNTAX},
  [GRX_DIAG_UNKNOWN_GROUP_NAME]
      = {"reference to an unknown group name", GRX_ERR_SYNTAX},
  [GRX_DIAG_INVALID_BACKREFERENCE]
      = {"reference to a group that does not exist", GRX_ERR_SYNTAX},
  [GRX_DIAG_FORWARD_BACKREFERENCE]
      = {"reference to a group that appears later", GRX_ERR_SYNTAX},
  [GRX_DIAG_INVALID_GROUP_SYNTAX]
      = {"unrecognized group construct", GRX_ERR_SYNTAX},
  [GRX_DIAG_INVALID_CONDITION]
      = {"malformed conditional", GRX_ERR_SYNTAX},
  [GRX_DIAG_INVALID_RECURSION]
      = {"invalid recursion or subroutine call", GRX_ERR_SYNTAX},

  [GRX_DIAG_UNKNOWN_FLAG]
      = {"unknown flag for this dialect", GRX_ERR_SYNTAX},
  [GRX_DIAG_DUPLICATE_FLAG]
      = {"flag given more than once", GRX_ERR_SYNTAX},
  [GRX_DIAG_CONFLICTING_FLAGS]
      = {"flags conflict with one another", GRX_ERR_SYNTAX},
  [GRX_DIAG_SEARCH_FLAG_IN_PATTERN]
      = {"flag selects a search mode, not a pattern option", GRX_ERR_SYNTAX},

  [GRX_DIAG_VARIABLE_LOOKBEHIND]
      = {"lookbehind is not a fixed length", GRX_ERR_SYNTAX},
  [GRX_DIAG_INVALID_LOOKAROUND]
      = {"invalid lookaround", GRX_ERR_SYNTAX},

  [GRX_DIAG_NOT_IN_DIALECT]
      = {"construct does not exist in this dialect", GRX_ERR_SYNTAX},
  [GRX_DIAG_DIALECT_NOT_IMPLEMENTED]
      = {"dialect is not implemented yet", GRX_ERR_UNSUPPORTED},
  [GRX_DIAG_CONSTRUCT_NOT_IMPLEMENTED]
      = {"construct is not implemented yet", GRX_ERR_UNSUPPORTED},

  [GRX_DIAG_LIMIT_PATTERN_LENGTH]
      = {"pattern is longer than max_pattern_length", GRX_ERR_LIMIT},
  [GRX_DIAG_LIMIT_NESTING_DEPTH]
      = {"nesting is deeper than max_nesting_depth", GRX_ERR_LIMIT},
  [GRX_DIAG_LIMIT_NODES]
      = {"pattern has more nodes than max_nodes", GRX_ERR_LIMIT},
  [GRX_DIAG_LIMIT_PROGRAM_SIZE]
      = {"program is larger than max_program_size", GRX_ERR_LIMIT},
  [GRX_DIAG_LIMIT_CAPTURES]
      = {"pattern has more groups than max_captures", GRX_ERR_LIMIT},
  [GRX_DIAG_LIMIT_REPEAT_COUNT]
      = {"repeat count is larger than max_repeat_count", GRX_ERR_LIMIT},
  [GRX_DIAG_LIMIT_CLASS_RANGES]
      = {"character class has more ranges than max_class_ranges",
          GRX_ERR_LIMIT},
  [GRX_DIAG_LIMIT_LOOKBEHIND_LENGTH]
      = {"lookbehind is longer than max_lookbehind_length", GRX_ERR_LIMIT},
  [GRX_DIAG_LIMIT_RECURSION_DEPTH]
      = {"recursion is deeper than max_recursion_depth", GRX_ERR_LIMIT},
  [GRX_DIAG_LIMIT_SUBJECT_LENGTH]
      = {"subject is longer than max_subject_length", GRX_ERR_LIMIT},
  [GRX_DIAG_LIMIT_STEPS]
      = {"match used more steps than max_steps", GRX_ERR_LIMIT},
  [GRX_DIAG_LIMIT_BACKTRACK]
      = {"match backtracked deeper than max_backtrack", GRX_ERR_LIMIT},
  [GRX_DIAG_LIMIT_MATCH_MEMORY]
      = {"match needs more scratch than max_match_memory", GRX_ERR_LIMIT},

  [GRX_DIAG_OUT_OF_MEMORY] = {"out of memory", GRX_ERR_OOM},
  [GRX_DIAG_INVALID_ARGUMENT] = {"invalid argument", GRX_ERR_INVALID},
  [GRX_DIAG_INVALID_TEMPLATE]
      = {"malformed replacement template", GRX_ERR_SYNTAX},
  [GRX_DIAG_TEMPLATE_UNKNOWN_GROUP]
      = {"replacement template names a group the pattern does not have",
          GRX_ERR_SYNTAX},

  [GRX_DIAG_INVALID_SUBJECT_UTF8]
      = {"subject is not valid UTF-8", GRX_ERR_INVALID},
  [GRX_DIAG_INTERNAL] = {"internal error", GRX_ERR_INTERNAL},
};

const char * grx_diag_string(GRX_Diag diag) {
  if ((unsigned)diag >= (unsigned)GRX_DIAG_COUNT || !diag_table[diag].text) {
    return "unknown diagnostic";
  }

  return diag_table[diag].text;
}

GRX_Result grx_diag_result(GRX_Diag diag) {
  if ((unsigned)diag >= (unsigned)GRX_DIAG_COUNT || !diag_table[diag].text) {
    return GRX_ERR_INTERNAL;
  }

  return diag_table[diag].code;
}

GRX_Result grx_error_set(GRX_Error * error, GRX_Result code, GRX_Diag diag,
    size_t offset, size_t length) {
  if (!error) {
    return code;
  }

  error->code = code;
  error->diag = diag;
  error->offset = offset;
  error->length = length;

  const char * text = grx_diag_string(diag);
  int written;
  if (offset == GRX_NPOS) {
    written = snprintf(error->message, sizeof(error->message), "%s", text);
  }
  else {
    written = snprintf(error->message, sizeof(error->message),
        "%s at offset %zu", text, offset);
  }

  // snprintf truncates and reports what it would have written; either way the
  // buffer is NUL-terminated, and a truncated message is not a failure worth
  // propagating past a diagnostic that already says what happened.
  if (written < 0) {
    error->message[0] = '\0';
  }

  return code;
}
