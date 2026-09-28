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
 * It cannot see a *type* that is missing, because a type emits no symbol -
 * only the DWARF in a debug build records one. So the rule for types is kept
 * by hand: every name in a typedef or struct tag here, public or internal.
 *
 * See CONVENTIONS.md section 4.
 */

#ifndef GHOTI_IO_GRX_NAMESPACE_H
#define GHOTI_IO_GRX_NAMESPACE_H

#include <ghoti.io/regex/libver.h>

/// @cond HIDDEN_SYMBOLS

// Public types. Renamed as well as the functions, so that two versions whose
// structs differ in layout cannot be confused for one another.
#define GRX_Allocator GHOTIIO_REGEX(GRX_Allocator)
#define GRX_Callout GHOTIIO_REGEX(GRX_Callout)
#define GRX_CalloutFn GHOTIIO_REGEX(GRX_CalloutFn)
#define GRX_Capture GHOTIIO_REGEX(GRX_Capture)
#define GRX_Diag GHOTIIO_REGEX(GRX_Diag)
#define GRX_Engine GHOTIIO_REGEX(GRX_Engine)
#define GRX_Error GHOTIIO_REGEX(GRX_Error)
#define GRX_Facts GHOTIIO_REGEX(GRX_Facts)
#define GRX_Feature GHOTIIO_REGEX(GRX_Feature)
#define GRX_Limits GHOTIIO_REGEX(GRX_Limits)
#define GRX_Match GHOTIIO_REGEX(GRX_Match)
#define GRX_NodeKind GHOTIIO_REGEX(GRX_NodeKind)
#define GRX_Option GHOTIIO_REGEX(GRX_Option)
#define GRX_Pattern GHOTIIO_REGEX(GRX_Pattern)
#define GRX_Lint GHOTIIO_REGEX(GRX_Lint)
#define GRX_LintReport GHOTIIO_REGEX(GRX_LintReport)
#define GRX_Regex GHOTIIO_REGEX(GRX_Regex)
#define GRX_Result GHOTIIO_REGEX(GRX_Result)
#define GRX_ReplaceFlag GHOTIIO_REGEX(GRX_ReplaceFlag)
#define GRX_SearchFlag GHOTIIO_REGEX(GRX_SearchFlag)
#define GRX_SearchOptions GHOTIIO_REGEX(GRX_SearchOptions)
#define GRX_Split GHOTIIO_REGEX(GRX_Split)
#define GRX_Text GHOTIIO_REGEX(GRX_Text)
#define GRX_Syntax GHOTIIO_REGEX(GRX_Syntax)
#define GRX_SyntaxSpec GHOTIIO_REGEX(GRX_SyntaxSpec)

// Public functions.
#define grx_allocator_default GHOTIIO_REGEX(grx_allocator_default)
#define grx_diag_string GHOTIIO_REGEX(grx_diag_string)
#define grx_error_clear GHOTIIO_REGEX(grx_error_clear)
#define grx_facts_init GHOTIIO_REGEX(grx_facts_init)
#define grx_limits_default GHOTIIO_REGEX(grx_limits_default)
#define grx_limits_unlimited GHOTIIO_REGEX(grx_limits_unlimited)
#define grx_match_count GHOTIIO_REGEX(grx_match_count)
#define grx_match_create GHOTIIO_REGEX(grx_match_create)
#define grx_match_destroy GHOTIIO_REGEX(grx_match_destroy)
#define grx_match_dump GHOTIIO_REGEX(grx_match_dump)
#define grx_match_engine GHOTIIO_REGEX(grx_match_engine)
#define grx_match_group GHOTIIO_REGEX(grx_match_group)
#define grx_match_group_named GHOTIIO_REGEX(grx_match_group_named)
#define grx_match_mark GHOTIIO_REGEX(grx_match_mark)
#define grx_match_span GHOTIIO_REGEX(grx_match_span)
#define grx_match_steps GHOTIIO_REGEX(grx_match_steps)
#define grx_match_error GHOTIIO_REGEX(grx_match_error)
#define grx_node_kind_name GHOTIIO_REGEX(grx_node_kind_name)
#define grx_options_parse GHOTIIO_REGEX(grx_options_parse)
#define grx_pattern_capture_count GHOTIIO_REGEX(grx_pattern_capture_count)
#define grx_pattern_dump GHOTIIO_REGEX(grx_pattern_dump)
#define grx_pattern_free GHOTIIO_REGEX(grx_pattern_free)
#define grx_pattern_lint GHOTIIO_REGEX(grx_pattern_lint)
#define grx_lint_string GHOTIIO_REGEX(grx_lint_string)
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
#define grx_regex_facts GHOTIIO_REGEX(grx_regex_facts)
#define grx_regex_free GHOTIIO_REGEX(grx_regex_free)
#define grx_regex_match GHOTIIO_REGEX(grx_regex_match)
#define grx_regex_match_ex GHOTIIO_REGEX(grx_regex_match_ex)
#define grx_regex_program_size GHOTIIO_REGEX(grx_regex_program_size)
#define grx_regex_search GHOTIIO_REGEX(grx_regex_search)
#define grx_regex_search_ex GHOTIIO_REGEX(grx_regex_search_ex)
#define grx_regex_search_next GHOTIIO_REGEX(grx_regex_search_next)
#define grx_regex_replace GHOTIIO_REGEX(grx_regex_replace)
#define grx_regex_split GHOTIIO_REGEX(grx_regex_split)
#define grx_split_free GHOTIIO_REGEX(grx_split_free)
#define grx_text_free GHOTIIO_REGEX(grx_text_free)
#define grx_search_options_default GHOTIIO_REGEX(grx_search_options_default)
#define grx_regex_syntax GHOTIIO_REGEX(grx_regex_syntax)
#define grx_result_string GHOTIIO_REGEX(grx_result_string)
#define grx_syntax_from_name GHOTIIO_REGEX(grx_syntax_from_name)
#define grx_syntax_has_feature GHOTIIO_REGEX(grx_syntax_has_feature)
#define grx_syntax_name GHOTIIO_REGEX(grx_syntax_name)
#define grx_syntax_spec GHOTIIO_REGEX(grx_syntax_spec)
#define grx_unicode_version GHOTIIO_REGEX(grx_unicode_version)
#define grx_utf8_validate GHOTIIO_REGEX(grx_utf8_validate)
#define grx_version_number GHOTIIO_REGEX(grx_version_number)
#define grx_version_string GHOTIIO_REGEX(grx_version_string)

// Internal names. Hidden by -fvisibility=hidden and so unable to collide, but
// renamed anyway: one rule is easier to keep than two.

// The semantic vocabulary that survives lowering.
#define GRX_AssertKind GHOTIIO_REGEX(GRX_AssertKind)
#define GRX_BackrefUnsetMode GHOTIIO_REGEX(GRX_BackrefUnsetMode)
#define GRX_CaptureResetMode GHOTIIO_REGEX(GRX_CaptureResetMode)
#define GRX_CondKind GHOTIIO_REGEX(GRX_CondKind)
#define GRX_EmptyLoopMode GHOTIIO_REGEX(GRX_EmptyLoopMode)
#define GRX_LookKind GHOTIIO_REGEX(GRX_LookKind)
#define GRX_IterationRule GHOTIIO_REGEX(GRX_IterationRule)
#define GRX_MatchPreference GHOTIIO_REGEX(GRX_MatchPreference)
#define GRX_RepeatMode GHOTIIO_REGEX(GRX_RepeatMode)
#define GRX_VerbKind GHOTIIO_REGEX(GRX_VerbKind)

// Storage.
#define GRX_Arena GHOTIIO_REGEX(GRX_Arena)
#define grx_arena_append GHOTIIO_REGEX(grx_arena_append)
#define grx_arena_at GHOTIIO_REGEX(grx_arena_at)
#define grx_arena_clear GHOTIIO_REGEX(grx_arena_clear)
#define grx_arena_init GHOTIIO_REGEX(grx_arena_init)
#define grx_arena_reserve GHOTIIO_REGEX(grx_arena_reserve)

// Diagnostics.
#define grx_diag_result GHOTIIO_REGEX(grx_diag_result)
#define grx_error_set GHOTIIO_REGEX(grx_error_set)

// Character classes.
#define GRX_CharClass GHOTIIO_REGEX(GRX_CharClass)
#define GRX_ClassRef GHOTIIO_REGEX(GRX_ClassRef)
#define GRX_ClassTable GHOTIIO_REGEX(GRX_ClassTable)
#define grx_charclass_add_property GHOTIIO_REGEX(grx_charclass_add_property)
#define grx_charclass_add_range GHOTIIO_REGEX(grx_charclass_add_range)
#define grx_charclass_add_ranges GHOTIIO_REGEX(grx_charclass_add_ranges)
#define grx_charclass_canonicalize GHOTIIO_REGEX(grx_charclass_canonicalize)
#define grx_charclass_clear GHOTIIO_REGEX(grx_charclass_clear)
#define grx_charclass_complement GHOTIIO_REGEX(grx_charclass_complement)
#define grx_charclass_contains GHOTIIO_REGEX(grx_charclass_contains)
#define grx_charclass_copy GHOTIIO_REGEX(grx_charclass_copy)
#define grx_charclass_equals GHOTIIO_REGEX(grx_charclass_equals)
#define grx_charclass_fold_closure GHOTIIO_REGEX(grx_charclass_fold_closure)
#define grx_charclass_init GHOTIIO_REGEX(grx_charclass_init)
#define grx_charclass_intersect GHOTIIO_REGEX(grx_charclass_intersect)
#define grx_charclass_size GHOTIIO_REGEX(grx_charclass_size)
#define grx_charclass_subtract GHOTIIO_REGEX(grx_charclass_subtract)
#define grx_charclass_symdiff GHOTIIO_REGEX(grx_charclass_symdiff)
#define grx_charclass_union GHOTIIO_REGEX(grx_charclass_union)
#define grx_class_table_add GHOTIIO_REGEX(grx_class_table_add)
#define grx_class_table_add_class GHOTIIO_REGEX(grx_class_table_add_class)
#define grx_class_table_clear GHOTIIO_REGEX(grx_class_table_clear)
#define grx_class_table_contains GHOTIIO_REGEX(grx_class_table_contains)
#define grx_class_table_count GHOTIIO_REGEX(grx_class_table_count)
#define grx_class_table_get GHOTIIO_REGEX(grx_class_table_get)
#define grx_class_table_init GHOTIIO_REGEX(grx_class_table_init)

// The syntax tree.
#define GRX_AnchorKind GHOTIIO_REGEX(GRX_AnchorKind)
#define GRX_ClassItem GHOTIIO_REGEX(GRX_ClassItem)
#define GRX_ClassItemKind GHOTIIO_REGEX(GRX_ClassItemKind)
#define GRX_ClassOpKind GHOTIIO_REGEX(GRX_ClassOpKind)
#define GRX_Node GHOTIIO_REGEX(GRX_Node)
#define GRX_Parser GHOTIIO_REGEX(GRX_Parser)
#define GRX_ShorthandKind GHOTIIO_REGEX(GRX_ShorthandKind)
#define grx_parse_pattern GHOTIIO_REGEX(grx_parse_pattern)
#define grx_pattern_add_child GHOTIIO_REGEX(grx_pattern_add_child)
#define grx_pattern_add_name GHOTIIO_REGEX(grx_pattern_add_name)
#define grx_pattern_add_string GHOTIIO_REGEX(grx_pattern_add_string)
#define grx_pattern_string GHOTIIO_REGEX(grx_pattern_string)
#define grx_pattern_string_begin GHOTIIO_REGEX(grx_pattern_string_begin)
#define grx_pattern_string_push GHOTIIO_REGEX(grx_pattern_string_push)
#define SetOperand GHOTIIO_REGEX(SetOperand)
#define grx_pattern_add_node GHOTIIO_REGEX(grx_pattern_add_node)
#define grx_pattern_create GHOTIIO_REGEX(grx_pattern_create)
#define grx_pattern_name GHOTIIO_REGEX(grx_pattern_name)
#define grx_pattern_node GHOTIIO_REGEX(grx_pattern_node)

// The intermediate representation, and lowering into it.
#define GRX_NamedSet GHOTIIO_REGEX(GRX_NamedSet)
#define grx_analyze_ir GHOTIIO_REGEX(grx_analyze_ir)
#define grx_ir_can_match_empty GHOTIIO_REGEX(grx_ir_can_match_empty)
#define grx_lower_pattern GHOTIIO_REGEX(grx_lower_pattern)
#define grx_named_set GHOTIIO_REGEX(grx_named_set)
#define grx_newline_set GHOTIIO_REGEX(grx_newline_set)
#define grx_shorthand_set GHOTIIO_REGEX(grx_shorthand_set)
#define GRX_IR GHOTIIO_REGEX(GRX_IR)
#define GRX_IRKind GHOTIIO_REGEX(GRX_IRKind)
#define GRX_IRNode GHOTIIO_REGEX(GRX_IRNode)
#define grx_ir_add_child GHOTIIO_REGEX(grx_ir_add_child)
#define grx_ir_add_name GHOTIIO_REGEX(grx_ir_add_name)
#define grx_ir_add_node GHOTIIO_REGEX(grx_ir_add_node)
#define grx_ir_create GHOTIIO_REGEX(grx_ir_create)
#define grx_ir_dump GHOTIIO_REGEX(grx_ir_dump)
#define grx_ir_free GHOTIIO_REGEX(grx_ir_free)
#define grx_ir_name GHOTIIO_REGEX(grx_ir_name)
#define grx_ir_node GHOTIIO_REGEX(grx_ir_node)

// The compiled program.
#define GRX_Inst GHOTIIO_REGEX(GRX_Inst)
#define GRX_Opcode GHOTIIO_REGEX(GRX_Opcode)
#define GRX_Program GHOTIIO_REGEX(GRX_Program)
#define grx_codegen_program GHOTIIO_REGEX(grx_codegen_program)
#define grx_compile_program GHOTIIO_REGEX(grx_compile_program)
#define grx_opcode_name GHOTIIO_REGEX(grx_opcode_name)
#define grx_program_add GHOTIIO_REGEX(grx_program_add)
#define grx_program_at GHOTIIO_REGEX(grx_program_at)
#define grx_program_clear GHOTIIO_REGEX(grx_program_clear)
#define grx_program_dump GHOTIIO_REGEX(grx_program_dump)
#define grx_program_init GHOTIIO_REGEX(grx_program_init)

// Execution.
#define GRX_ExecRequest GHOTIIO_REGEX(GRX_ExecRequest)
#define GRX_EmptyMatchRule GHOTIIO_REGEX(GRX_EmptyMatchRule)
#define grx_exec_accepts GHOTIIO_REGEX(grx_exec_accepts)
#define grx_exec_backtrack GHOTIIO_REGEX(grx_exec_backtrack)
#define grx_exec_pike GHOTIIO_REGEX(grx_exec_pike)
#define grx_dfa_create GHOTIIO_REGEX(grx_dfa_create)
#define grx_dfa_eligible GHOTIIO_REGEX(grx_dfa_eligible)
#define grx_dfa_exists GHOTIIO_REGEX(grx_dfa_exists)
#define grx_dfa_flushes GHOTIIO_REGEX(grx_dfa_flushes)
#define grx_dfa_free GHOTIIO_REGEX(grx_dfa_free)
#define grx_dfa_search GHOTIIO_REGEX(grx_dfa_search)
#define grx_dfa_search_skipping GHOTIIO_REGEX(grx_dfa_search_skipping)
#define grx_dfa_states GHOTIIO_REGEX(grx_dfa_states)
#define grx_onepass_create GHOTIIO_REGEX(grx_onepass_create)
#define grx_onepass_eligible GHOTIIO_REGEX(grx_onepass_eligible)
#define grx_onepass_free GHOTIIO_REGEX(grx_onepass_free)
#define grx_onepass_run GHOTIIO_REGEX(grx_onepass_run)
#define grx_onepass_states GHOTIIO_REGEX(grx_onepass_states)

// Substitution and splitting.
#define GRX_Template GHOTIIO_REGEX(GRX_Template)
#define GRX_TemplateMissing GHOTIIO_REGEX(GRX_TemplateMissing)
#define GRX_TemplateOp GHOTIIO_REGEX(GRX_TemplateOp)
#define GRX_TemplateOpKind GHOTIIO_REGEX(GRX_TemplateOpKind)
#define GRX_TemplateSpec GHOTIIO_REGEX(GRX_TemplateSpec)
#define grx_template_clear GHOTIIO_REGEX(grx_template_clear)
#define grx_template_parse GHOTIIO_REGEX(grx_template_parse)
#define grx_exec_program_needs_backtracking                                    \
  GHOTIIO_REGEX(grx_exec_program_needs_backtracking)
#define grx_exec_program_is_memoizable                                         \
  GHOTIIO_REGEX(grx_exec_program_is_memoizable)
#define grx_exec_bitmap_bytes GHOTIIO_REGEX(grx_exec_bitmap_bytes)

// Code-point ranges, the shape every set in the library has.
#define GRX_CharRange GHOTIIO_REGEX(GRX_CharRange)

// Dialects.
#define GRX_Frontend GHOTIIO_REGEX(GRX_Frontend)
#define GRX_GroupOpen GHOTIIO_REGEX(GRX_GroupOpen)
#define GRX_Parser GHOTIIO_REGEX(GRX_Parser)
#define GRX_Quantifier GHOTIIO_REGEX(GRX_Quantifier)
#define grx_frontend_ecmascript GHOTIIO_REGEX(grx_frontend_ecmascript)
#define grx_frontend_for GHOTIIO_REGEX(grx_frontend_for)
#define grx_parse_alternation GHOTIIO_REGEX(grx_parse_alternation)
#define grx_parse_at_end GHOTIIO_REGEX(grx_parse_at_end)
#define grx_parse_class_add GHOTIIO_REGEX(grx_parse_class_add)
#define grx_parse_class_node GHOTIIO_REGEX(grx_parse_class_node)
#define grx_parse_eat GHOTIIO_REGEX(grx_parse_eat)
#define grx_parse_fail GHOTIIO_REGEX(grx_parse_fail)
#define grx_parse_literal_node GHOTIIO_REGEX(grx_parse_literal_node)
#define grx_parse_literal_extend GHOTIIO_REGEX(grx_parse_literal_extend)
#define grx_parse_peek GHOTIIO_REGEX(grx_parse_peek)
#define grx_parse_shorthand_node GHOTIIO_REGEX(grx_parse_shorthand_node)
#define grx_parse_take GHOTIIO_REGEX(grx_parse_take)
#define GRX_DollarRule GHOTIIO_REGEX(GRX_DollarRule)
#define GRX_LookbehindLimit GHOTIIO_REGEX(GRX_LookbehindLimit)
#define GRX_NewlineSet GHOTIIO_REGEX(GRX_NewlineSet)
#define GRX_Profile GHOTIIO_REGEX(GRX_Profile)
#define GRX_ShorthandSet GHOTIIO_REGEX(GRX_ShorthandSet)
#define grx_syntax_profile GHOTIIO_REGEX(grx_syntax_profile)
#define grx_syntax_spec_table GHOTIIO_REGEX(grx_syntax_spec_table)

// Unicode: the codec, the foldings, and property resolution.
#define GRX_FoldKind GHOTIIO_REGEX(GRX_FoldKind)
#define GRX_PropertyMatch GHOTIIO_REGEX(GRX_PropertyMatch)
#define grx_unicode_orbit GHOTIIO_REGEX(grx_unicode_orbit)
#define grx_unicode_orbit_table_at GHOTIIO_REGEX(grx_unicode_orbit_table_at)
#define grx_unicode_orbit_table_size                                          \
  GHOTIIO_REGEX(grx_unicode_orbit_table_size)
#define grx_unicode_es_legacy_canonicalize                                    \
  GHOTIIO_REGEX(grx_unicode_es_legacy_canonicalize)
#define grx_unicode_es_legacy_orbit GHOTIIO_REGEX(grx_unicode_es_legacy_orbit)
#define grx_unicode_fold_orbit GHOTIIO_REGEX(grx_unicode_fold_orbit)
#define grx_unicode_fold_simple GHOTIIO_REGEX(grx_unicode_fold_simple)
#define grx_display_cell_width GHOTIIO_REGEX(grx_display_cell_width)
#define grx_display_column_after GHOTIIO_REGEX(grx_display_column_after)
#define grx_display_composing_range                                          \
  GHOTIIO_REGEX(grx_display_composing_range)
#define grx_cluster_base_before GHOTIIO_REGEX(grx_cluster_base_before)
#define grx_vim_char_class GHOTIIO_REGEX(grx_vim_char_class)
#define grx_vim_word_start GHOTIIO_REGEX(grx_vim_word_start)
#define grx_vim_word_end GHOTIIO_REGEX(grx_vim_word_end)
#define grx_unicode_upper_simple GHOTIIO_REGEX(grx_unicode_upper_simple)
#define grx_unicode_lower_simple GHOTIIO_REGEX(grx_unicode_lower_simple)
#define grx_unicode_property_lookup GHOTIIO_REGEX(grx_unicode_property_lookup)
#define grx_unicode_property_name GHOTIIO_REGEX(grx_unicode_property_name)
#define grx_unicode_property_contains                                         \
  GHOTIIO_REGEX(grx_unicode_property_contains)
#define grx_unicode_property_digest GHOTIIO_REGEX(grx_unicode_property_digest)
#define grx_unicode_property_ranges GHOTIIO_REGEX(grx_unicode_property_ranges)
#define grx_unicode_property_total GHOTIIO_REGEX(grx_unicode_property_total)
#define grx_unicode_range_digest GHOTIIO_REGEX(grx_unicode_range_digest)
#define grx_unicode_string_set_lookup GHOTIIO_REGEX(grx_unicode_string_set_lookup)
#define grx_unicode_string_set_size GHOTIIO_REGEX(grx_unicode_string_set_size)
#define grx_unicode_string_set_at GHOTIIO_REGEX(grx_unicode_string_set_at)
#define grx_unicode_utf8_decode GHOTIIO_REGEX(grx_unicode_utf8_decode)
#define grx_unicode_utf8_decode_prev                                          \
  GHOTIIO_REGEX(grx_unicode_utf8_decode_prev)
#define grx_unicode_utf8_encode GHOTIIO_REGEX(grx_unicode_utf8_encode)

// The generated Unicode tables. Data rather than functions, and hidden by
// -fvisibility=hidden like everything else here, but renamed under the same
// rule: two versions of this library in one process must not share a table
// whose layout their headers disagree about.
#define GRX_UnicodeCaseMap GHOTIIO_REGEX(GRX_UnicodeCaseMap)
#define GRX_UnicodeName GHOTIIO_REGEX(GRX_UnicodeName)
#define GRX_UnicodeOrbit GHOTIIO_REGEX(GRX_UnicodeOrbit)
#define GRX_UnicodeString GHOTIIO_REGEX(GRX_UnicodeString)
#define GRX_UnicodeStringSet GHOTIIO_REGEX(GRX_UnicodeStringSet)
#define grx_unicode_string_points GHOTIIO_REGEX(grx_unicode_string_points)
#define grx_unicode_string_point_count GHOTIIO_REGEX(grx_unicode_string_point_count)
#define grx_unicode_strings GHOTIIO_REGEX(grx_unicode_strings)
#define grx_unicode_string_count GHOTIIO_REGEX(grx_unicode_string_count)
#define grx_unicode_string_sets GHOTIIO_REGEX(grx_unicode_string_sets)
#define grx_unicode_string_set_count GHOTIIO_REGEX(grx_unicode_string_set_count)
#define GRX_UnicodeProperty GHOTIIO_REGEX(GRX_UnicodeProperty)
#define GRX_UPropKind GHOTIIO_REGEX(GRX_UPropKind)
#define grx_unicode_es_legacy_map GHOTIIO_REGEX(grx_unicode_es_legacy_map)
#define grx_unicode_es_legacy_map_count                                       \
  GHOTIIO_REGEX(grx_unicode_es_legacy_map_count)
#define grx_unicode_simple_upper_map                                          \
  GHOTIIO_REGEX(grx_unicode_simple_upper_map)
#define grx_unicode_simple_upper_map_count                                    \
  GHOTIIO_REGEX(grx_unicode_simple_upper_map_count)
#define grx_unicode_simple_lower_map                                          \
  GHOTIIO_REGEX(grx_unicode_simple_lower_map)
#define grx_unicode_simple_lower_map_count                                    \
  GHOTIIO_REGEX(grx_unicode_simple_lower_map_count)
#define grx_unicode_es_legacy_orbit_member_count                              \
  GHOTIIO_REGEX(grx_unicode_es_legacy_orbit_member_count)
#define grx_unicode_es_legacy_orbit_members                                   \
  GHOTIIO_REGEX(grx_unicode_es_legacy_orbit_members)
#define grx_unicode_es_legacy_orbit_count                                     \
  GHOTIIO_REGEX(grx_unicode_es_legacy_orbit_count)
#define grx_unicode_es_legacy_orbits                                          \
  GHOTIIO_REGEX(grx_unicode_es_legacy_orbits)
#define grx_unicode_fold_map GHOTIIO_REGEX(grx_unicode_fold_map)
#define grx_unicode_fold_map_count GHOTIIO_REGEX(grx_unicode_fold_map_count)
#define grx_unicode_fold_orbit_count                                          \
  GHOTIIO_REGEX(grx_unicode_fold_orbit_count)
#define grx_unicode_fold_orbit_member_count                                   \
  GHOTIIO_REGEX(grx_unicode_fold_orbit_member_count)
#define grx_unicode_fold_orbit_members                                        \
  GHOTIIO_REGEX(grx_unicode_fold_orbit_members)
#define grx_unicode_fold_orbits GHOTIIO_REGEX(grx_unicode_fold_orbits)
#define grx_unicode_loose_name_count                                          \
  GHOTIIO_REGEX(grx_unicode_loose_name_count)
#define grx_unicode_loose_names GHOTIIO_REGEX(grx_unicode_loose_names)
#define grx_unicode_loose_prop_name_count                                     \
  GHOTIIO_REGEX(grx_unicode_loose_prop_name_count)
#define grx_unicode_loose_prop_names                                          \
  GHOTIIO_REGEX(grx_unicode_loose_prop_names)
#define grx_unicode_prop_name_count                                           \
  GHOTIIO_REGEX(grx_unicode_prop_name_count)
#define grx_unicode_prop_names GHOTIIO_REGEX(grx_unicode_prop_names)
#define grx_unicode_properties GHOTIIO_REGEX(grx_unicode_properties)
#define grx_unicode_property_count                                            \
  GHOTIIO_REGEX(grx_unicode_property_count)
#define grx_unicode_strict_name_count                                         \
  GHOTIIO_REGEX(grx_unicode_strict_name_count)
#define grx_unicode_strict_names GHOTIIO_REGEX(grx_unicode_strict_names)

/// @endcond

#endif // GHOTI_IO_GRX_NAMESPACE_H
