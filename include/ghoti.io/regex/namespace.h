/**
 * @file namespace.h
 *
 * Maps every public name of this library into its version namespace.
 *
 * Kept in one file rather than beside each declaration: a type rename has to
 * be in effect before any struct tag that uses the name, and an internal
 * header may define such a tag without including the public header that
 * declares the typedef.
 *
 * `make check-symbols` fails if an exported symbol is missing from this list.
 *
 * See CONVENTIONS.md section 4.
 *
 * Copyright 2026 by Corey Pennycuff
 */

#ifndef GHOTI_IO_GRX_NAMESPACE_H
#define GHOTI_IO_GRX_NAMESPACE_H

#include <ghoti.io/regex/libver.h>

/// @cond HIDDEN_SYMBOLS

// Public types. Renamed as well as the functions, so that two versions whose
// structs differ in layout cannot be confused for one another.
#define GRX_Allocator GHOTIIO_REGEX(GRX_Allocator)
#define GRX_Capture GHOTIIO_REGEX(GRX_Capture)
#define GRX_Engine GHOTIIO_REGEX(GRX_Engine)
#define GRX_Error GHOTIIO_REGEX(GRX_Error)
#define GRX_Feature GHOTIIO_REGEX(GRX_Feature)
#define GRX_Limits GHOTIIO_REGEX(GRX_Limits)
#define GRX_Match GHOTIIO_REGEX(GRX_Match)
#define GRX_NodeKind GHOTIIO_REGEX(GRX_NodeKind)
#define GRX_Option GHOTIIO_REGEX(GRX_Option)
#define GRX_Pattern GHOTIIO_REGEX(GRX_Pattern)
#define GRX_Regex GHOTIIO_REGEX(GRX_Regex)
#define GRX_Result GHOTIIO_REGEX(GRX_Result)
#define GRX_Syntax GHOTIIO_REGEX(GRX_Syntax)
#define GRX_SyntaxSpec GHOTIIO_REGEX(GRX_SyntaxSpec)

// Public functions.
#define grx_allocator_default GHOTIIO_REGEX(grx_allocator_default)
#define grx_error_clear GHOTIIO_REGEX(grx_error_clear)
#define grx_limits_default GHOTIIO_REGEX(grx_limits_default)
#define grx_match_count GHOTIIO_REGEX(grx_match_count)
#define grx_match_create GHOTIIO_REGEX(grx_match_create)
#define grx_match_destroy GHOTIIO_REGEX(grx_match_destroy)
#define grx_match_dump GHOTIIO_REGEX(grx_match_dump)
#define grx_match_engine GHOTIIO_REGEX(grx_match_engine)
#define grx_match_group GHOTIIO_REGEX(grx_match_group)
#define grx_match_group_named GHOTIIO_REGEX(grx_match_group_named)
#define grx_pattern_capture_count GHOTIIO_REGEX(grx_pattern_capture_count)
#define grx_pattern_dump GHOTIIO_REGEX(grx_pattern_dump)
#define grx_pattern_free GHOTIIO_REGEX(grx_pattern_free)
#define grx_pattern_node_count GHOTIIO_REGEX(grx_pattern_node_count)
#define grx_pattern_parse GHOTIIO_REGEX(grx_pattern_parse)
#define grx_pattern_parse_with_allocator                                       \
  GHOTIIO_REGEX(grx_pattern_parse_with_allocator)
#define grx_pattern_syntax GHOTIIO_REGEX(grx_pattern_syntax)
#define grx_regex_capture_count GHOTIIO_REGEX(grx_regex_capture_count)
#define grx_regex_capture_index GHOTIIO_REGEX(grx_regex_capture_index)
#define grx_regex_capture_name GHOTIIO_REGEX(grx_regex_capture_name)
#define grx_regex_compile GHOTIIO_REGEX(grx_regex_compile)
#define grx_regex_compile_pattern GHOTIIO_REGEX(grx_regex_compile_pattern)
#define grx_regex_compile_with_allocator                                       \
  GHOTIIO_REGEX(grx_regex_compile_with_allocator)
#define grx_regex_dump GHOTIIO_REGEX(grx_regex_dump)
#define grx_regex_free GHOTIIO_REGEX(grx_regex_free)
#define grx_regex_match GHOTIIO_REGEX(grx_regex_match)
#define grx_regex_program_size GHOTIIO_REGEX(grx_regex_program_size)
#define grx_regex_search GHOTIIO_REGEX(grx_regex_search)
#define grx_regex_syntax GHOTIIO_REGEX(grx_regex_syntax)
#define grx_result_string GHOTIIO_REGEX(grx_result_string)
#define grx_syntax_from_name GHOTIIO_REGEX(grx_syntax_from_name)
#define grx_syntax_has_feature GHOTIIO_REGEX(grx_syntax_has_feature)
#define grx_syntax_name GHOTIIO_REGEX(grx_syntax_name)
#define grx_syntax_spec GHOTIIO_REGEX(grx_syntax_spec)
#define grx_version_number GHOTIIO_REGEX(grx_version_number)
#define grx_version_string GHOTIIO_REGEX(grx_version_string)

// Internal names. Hidden by -fvisibility=hidden and so unable to collide, but
// renamed anyway: one rule is easier to keep than two.
#define GRX_CharClass GHOTIIO_REGEX(GRX_CharClass)
#define GRX_Node GHOTIIO_REGEX(GRX_Node)
#define GRX_Parser GHOTIIO_REGEX(GRX_Parser)
#define GRX_Program GHOTIIO_REGEX(GRX_Program)
#define GRX_Inst GHOTIIO_REGEX(GRX_Inst)
#define GRX_Opcode GHOTIIO_REGEX(GRX_Opcode)
#define grx_charclass_add_range GHOTIIO_REGEX(grx_charclass_add_range)
#define grx_charclass_clear GHOTIIO_REGEX(grx_charclass_clear)
#define grx_charclass_contains GHOTIIO_REGEX(grx_charclass_contains)
#define grx_compile_program GHOTIIO_REGEX(grx_compile_program)
#define grx_exec_backtrack GHOTIIO_REGEX(grx_exec_backtrack)
#define grx_exec_pike GHOTIIO_REGEX(grx_exec_pike)
#define grx_exec_program_needs_backtracking                                    \
  GHOTIIO_REGEX(grx_exec_program_needs_backtracking)
#define grx_parse_pattern GHOTIIO_REGEX(grx_parse_pattern)
#define grx_syntax_spec_table GHOTIIO_REGEX(grx_syntax_spec_table)
#define grx_unicode_fold_simple GHOTIIO_REGEX(grx_unicode_fold_simple)
#define grx_unicode_utf8_decode GHOTIIO_REGEX(grx_unicode_utf8_decode)
#define grx_unicode_utf8_encode GHOTIIO_REGEX(grx_unicode_utf8_encode)

/// @endcond

#endif // GHOTI_IO_GRX_NAMESPACE_H
