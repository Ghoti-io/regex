#!/usr/bin/env python3
"""Compare the Vim front end against vim itself, on patterns nobody wrote.

documentation/work-packages.md WP-36. The dialect's definition is "what vim so
does", one oracle decides every row - the arrangement `python_diff.py` and
`perl_diff.py` use, and not the glibc-and-musl agreement the POSIX rows are
held to.

**One fresh Vim process per reference operation.**
Span and capture queries run separately: calling a second matching API can
change NFA state even within one case. Their outcomes must agree before joining.
One container supervises bounded processes;
raw process-local envelopes and the explicit aggregation map are retained.
The batch transport is available only as a diagnostic of history dependence.

**The subject is a string, not a buffer.** `matchstrpos()` is what this
drives, and over a string vim's line break is an ordinary character: `a.b`
matches "a\\nb" and `^` holds only at offset 0. That is the subject this
library has, so it is the right reference - and it is why the Vim profile row
says GRX_NEWLINES_NONE where vim's own help says the opposite. A buffer
oracle would measure a dialect this library does not offer.

**What it cannot see.** vim's `matchlist()` returns the *text* of each group
and returns "" for a group that did not participate, so an unset group and a
group that matched empty are one answer there. The comparison therefore folds
ours the same way, and the unset axis - GRX_BACKREF_UNSET_EMPTY in the
profile - is stated by tests/unit/test_vim.cpp instead, from the one probe
that can separate them: `\\(a\\)\\?\\1` matches the empty string against "b",
which a dialect that failed on an unset reference could not do.

A trap for anyone editing the vocabulary below: **Python's raw strings still
decode `\\u`**, so `r"\\u0041"` is "[A]" and not the six characters anyone
writing it meant. Every atom here is an ordinary string with the backslashes
doubled, which is why they look the way they do.

Usage:
    tools/oracle/vim_diff.py [--seed N] [--patterns N] [--examples N]
                             [--strict]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import binascii
import json
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
import unicodedata

import vim_runner
import vim_isolated
import vim_protocol
import escape_floor

PROFILES = {"vim-backtrack": 1, "vim-nfa": 2}

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# One entry per construct vim has, so that a combination exercises the
# interactions rather than one rule at a time. Grouped by what decides them,
# because the magic level is what this dialect is *for* and a vocabulary that
# only spelled magic-level patterns would test one of the four grammars.
ATOMS = [
    # Grouped optional sequences and lexical forward-reference permission
    # deliberately distinguish the two compatibility profiles.
    "[[:foo:]]", "[a[:foo:]]", "\\9[@<=]", "\\1\\(a\\)\\@<=",
    "a\\%[\\(b\\)c]", "a\\%[b\\%[cd]]", "\\v\\_^*", "\\%23l*",
    # Literals and the characters whose meaning the level decides.
    "a", "b", ".", "\\.", "*", "\\*", "^", "$", "\\^", "\\$",
    "\\n", "\\t", "\\e", "\\r", "\\b", "\\\\", "z", "q",
    # The named classes, and their complements, and the `\\_` forms that add
    # the line break.
    "\\s", "\\S", "\\d", "\\D", "\\w", "\\W", "\\h", "\\H", "\\a", "\\A",
    "\\l", "\\L", "\\u", "\\U", "\\x", "\\X", "\\o", "\\O", "\\i", "\\k",
    "\\f", "\\F", "\\I", "\\K",
    # Skips the composing characters at the position; the engines differ in
    # what it waives for the atom before it, so it is generated after atoms.
    "\\%C",
    "\\_s", "\\_d", "\\_w", "\\_.", "\\_[ab]", "\\_^", "\\_$",
    # The negated underscore forms, which are not the positive ones with a
    # member added: a negated class carries its negation over the items, so
    # `\_[^a]` has to be built as "the break, or the class" and was built
    # as "the class without the break" until a unit test asked.
    "\\_S", "\\_W", "\\_D", "\\_[^ab]", "\\_[^\\n]",
    # Collections. `[]` and a bare `[` are here because vim reads a `[` that
    # opens nothing as the character.
    "[ab]", "[^ab]", "[a-c]", "[]a]", "[a-]", "[", "[]", "[\\]]", "[\\\\]",
    "[[:alpha:]]", "[[:digit:]]", "[[:lower:]]", "[[:upper:]]", "[\\d65]",
    "[\\x41]", "[\\e]", "[\\b]", "[\\r]", "[\\t]",
    "[\\u0041]", "[a\\b]",
    # Groups and alternation, in both spellings.
    "\\(a\\)", "\\(a\\|b\\)", "\\%(ab\\)", "\\(a\\)\\1",
    "(a)", "(a|b)", "%(ab)", "(a)\\1", "%C",
    # Postfix assertions, including markers whose behavior differs between
    # the forced profiles. Both profiles receive the same vocabulary.
    "\\(a\\)\\@=", "\\(a\\)\\@!", "\\(a\\)\\@<=", "\\(a\\)\\@<!",
    "\\(a\\+\\)\\@>", "(a)@=", "(a)@!", "(a)@<=", "(a)@<!", "(a+)@>",
    # A postfix assertion inside another one: vim's spelling of the shape
    # that hid a defect in shared lowering, where a lookaround written
    # inside a lookbehind's body inherited the backwards direction of the
    # body instead of looking forwards from where it stands.
    "\\(a\\(b\\)\\@=\\)\\@<=", "\\(\\(a\\)\\@=a\\)\\@<=",
    "\\(a\\(c\\)\\@!\\)\\@<=", "\\(a\\(b\\)\\@=\\)\\@<!",
    "\\(a\\zsb\\)\\@>", "\\(a\\zeb\\)\\@>", "\\(a\\zsb\\)\\@=",
    "\\(a\\zeb\\)\\@=", "\\(a\\zeb\\)\\@<=",
    # The byte bound on a lookbehind, which is a restriction and so needs a
    # body that can overrun it as well as one that cannot.
    "\\(a\\)\\@1<=", "\\(ab\\)\\@1<=", "\\(ab\\)\\@2<=",
    "\\(\\w\\+\\)\\@2<=", "\\(ab\\)\\@1<!", "(a|ab)@2<=",
    # Repeats, in both spellings and both modes.
    "a*", "a\\+", "a\\=", "a\\?", "a\\{2}", "a\\{2,3}", "a\\{,2}",
    "a\\{2,}", "a\\{}", "a\\{-}", "a\\{-1,}", "a\\{-2,3}",
    "a+", "a=", "a?", "a{2}", "a{-}", "a{2,3}",
    # The `\\%` family, and the word boundaries.
    "\\%^", "\\%$", "\\%d65", "\\%x41", "\\%o101", "\\%u0041", "\\%[abc]",
    "a\\%[bc]", "\\<", "\\>", "<", ">",
    # `\%[...]`'s members are atoms, not characters, so the vocabulary has
    # to spell one that is not a literal: a class, a collection, a mark and
    # the seven escapes that are bare letters only in here.
    "a\\%[\\d\\w]", "a\\%[[bc]d]", "a\\%[\\zsb]", "a\\%[\\vb]",
    "a\\%[\\%d98]", "a\\%[\\_s]", "\\v%[\\db]",
    # The buffer positions. `l`, `V` and `#` never match over a string and
    # `c` is the byte column, so all four are built rather than refused.
    "\\%V", "\\%#", "\\%23l", "\\%1l", "\\%2c", "\\%<3c", "\\%>2c",
    "\\%1c", "\\v%2c", "\\%2v", "\\%<4v", "\\%>2v", "\\%9v",
    "\\%1v", "\\v%3v",
    # The two that move the reported match, and the level markers themselves.
    # `\\=` after a mark is the one multi vim allows there, and it is the
    # case that separates the spelling from the bounds: `\\zs\\{0,1}` is
    # E888 and asks for the same repeat.
    "\\zs", "\\ze", "\\zs\\=", "\\ze\\?", "\\v\\zs=",
    # `\z` is a prefix and never an atom, which this list did not ask until a
    # sweep of the whole alphabet under every magic level found a bare `\z`
    # and `\zx` compiling here and refused by vim. `\z(` and `\z1` have
    # their own refusal - they are syntax-file only - so `\zs` and `\ze`
    # above are the whole of what `\z` may be followed by.
    "\\z", "\\zx", "\\v\\z", "\\V\\z", "\\m\\z", "\\M\\z",
    # The last-substitute spellings, which both sides refuse and read alike.
    "~", "\\~", "\\M~",
    "\\v", "\\m", "\\M", "\\V", "\\c", "\\C",
    # The branch operator, which is a grammar level rather than an atom.
    "\\&", "&",
    # `\Z` turns the composing-cluster rule off for the whole pattern, and
    # it is an atom here for the same reason `\c` is - it decides the
    # pattern from wherever it stands. The clusters it decides about are
    # in COMPOSING_SUBJECTS below, where the subjects hold one.
    "\\Z",
]

# Paired composing controls exercise every atom against marked bases and
# leading marks. Multi-atom grammar below also measures competing frontiers,
# lexical repeat ownership, captures and assertion-local boundaries. Each
# profile is checked against its own forced reference engine.
COMPOSING_SUBJECTS = [
    "áb", "á̂b", "á", "xá", "áá",
    "́a", "́̂a", "a\ńb", "a ́b", "日́b",
]


def composing_cases():
    """Every atom the block asks about, with and without `\\Z`."""
    out = []
    for atom in ATOMS:
        for subject in COMPOSING_SUBJECTS:
            out.append((atom, subject))
            out.append(("\\Z" + atom, subject))
    return out


COMBINING_ATOMS = [
    "\u0301", "a\u0301", "a\u0301\u0302", ".\u0301", "[a\u0301]",
    "[ab\u0301]", "[^a\u0301]", r"\%u0301", r"\_[ab]", r"\_[^x]",
    r"\_s", r"\W",
]
COMBINING_TAILS = ["", ".", "b", r"\zs.", r"\ze."]


def combining_patterns():
    """Lexical atoms, repeats, competing paths and local assertion scopes."""
    patterns = [atom + tail for atom in COMBINING_ATOMS
                for tail in COMBINING_TAILS]
    for atom in COMBINING_ATOMS:
        patterns.extend([atom + r"\?.", atom + r"\{1}.",
                         r"\(" + atom + r"\)."])
    for branch in [r"a\|[ab]", r"[ab]\|a", r"[ab].\|abX",
                   r"a*bX\|[ab].", r"[ab].\|a*bX"]:
        body = r"\(" + branch + r"\)"
        patterns.extend([body, body + r"\@=.", body + r"\@>",
                         body + r"\@<=b"])
    return patterns


def combining_grammar_cases():
    return [(prefix + pattern, subject)
            for pattern in combining_patterns()
            for prefix in ("", r"\Z") for subject in COMPOSING_SUBJECTS]


# Constructs vim refuses or this library refuses on purpose. Kept in the
# vocabulary rather than left out, because a differential that only generates
# what both sides accept never tests a rejection - `differential-needs-
# invalid-input`, and the reason `python_diff.py` injects one row in eight.
REFUSED = [
    "\\z(a\\)", "\\z1", "\\1", "a\\{2", "\\(a", "a\\)", "a**", "\\@=",
    "\\%(a", "a\\{1}\\+", "\\v+a", "\\v?a", "\\v@a",
    "\\zs*", "\\ze\\+", "\\zs\\{0,1}", "\\v\\ze{2}",
    "a\\%[b*]", "a\\%[b\\|c]", "a\\%[]", "a\\%[b\\=]",
]

# The four levels, written as the prefix that selects one. The empty string
# is magic, which is vim's default and what a pattern with no marker gets.
LEVELS = ["", "\\v", "\\m", "\\M", "\\V"]

SUBJECTS = [
    "\b", "\r", "\t", "\x1b", "/", "-", "9", "a/b",
    "", "a", "ab", "aaab", "abc", "ABC", "a b", "a\tb", "a\nb", "\n",
    # A tab run, so that `\%23v` can be told from `\%23c`: it counts
    # display cells, and a subject of one-cell characters answers both the
    # same way.
    #
    # **A composing character is not in this list**, and the reason is not
    # that the rule is unbuilt - it is built, and COMPOSING_SUBJECTS below
    # is the block that checks it. It is that **vim's default engine shares
    # one step length between every thread alive at a position**: `clen` is
    # the length of a character with its composing characters, and a thread
    # that matches a plain literal sets it to the base alone for the whole
    # step. So `a\|` beside a cluster branch finds nothing over
    # "a" U+0301 U+0302 "b" where that branch alone finds 0-5, while
    # `b\|` beside it finds 0-5 - branch order having nothing to do with
    # it. A rule cannot depend on what a failed alternative begins with, so
    # rows like that would measure how often this generator spelled one.
    # documentation/dialects.md section 6 carries the measurement.
    #
    # Characters outside Latin's word class *are* here, because `\<` and
    # `\>` follow vim's nine character classes now rather than a word set:
    # six of the nine are represented.
    "\t\tx",
    "abcabc", "xayaz", "[a]", "a*b", "a+b", "a.c", "(a)", "a|b", "read",
    "rea", "r", "A", "0", "_", "é", "É", "aéb", "~", "^a$",
    # Six of vim's nine classes, and the boundaries between them: CJK,
    # Hiragana, Katakana, Hangul, Braille and emoji, each beside Latin and
    # beside one another. `\>` holds between U+65E5 and "x" there, both of
    # them keyword characters, which is what no word set could see.
    "日x", "x日", "日日", "aあ", "あア",
    "⠁a", "a😀", "한ㄱ", "日 x", "一あb",
    # The two code points where vim's *width* table is not East_Asian_Width,
    # added 2026-09-24. `\%v` counts screen columns, so these rows are the
    # only ones here that ask display.c a question its answer could get
    # wrong: every other subject above agrees with the property, so the
    # column arithmetic was checked and the table behind it never was. If
    # cell_widths were replaced with East_Asian_Width tomorrow the gate
    # stayed green, which is the same hole the classes had one property
    # over - see the comment above `×` below.
    #
    # They point opposite ways on purpose, because one direction does not
    # cover the other:
    #
    #   U+187F8 TANGUT IDEOGRAPH  UCD 17.0.0 says Wide, vim draws ONE cell.
    #     vim's table predates the block's extension; it stands for 145
    #     Other_Letter across Tangut and Khitan.
    #   U+23ED BLACK RIGHT-POINTING DOUBLE TRIANGLE WITH VERTICAL BAR
    #     UCD 17.0.0 says Neutral, vim draws TWO. It stands for 165
    #     Other_Symbol, and being emoji-shaped is exactly why the 😀 above
    #     does not cover it - that one agrees with the property.
    #
    # Alone and beside Latin, so `\%1v`, `\%2v` and `\%3v` separate the
    # readings rather than only the total.
    "\U000187F8", "a\U000187F8b", "⏭", "a⏭b",
    # The code points where vim's classes are not the Unicode properties,
    # added 2026-09-24 after a hand sweep found two defects this gate could
    # not see. It reported 0 disagreements over 50,980 rows while `\k` was
    # two code points narrow and `[[:lower:]]`/`[[:upper:]]` were 1,125 and
    # 608 wide: "é" and "É" were the only non-ASCII letters here, and both
    # are covered by every reading of every rule in question.
    #
    # `×` and `÷` are the only two members of 192-255 that vim's `@` does
    # not cover, so they are the whole of the difference between
    # 'iskeyword' at its Vim default and at its Vi-compatible one.
    "×", "a×b", "÷",
    # Case classes: vim asks for a case *counterpart*, not for a property.
    # A modifier letter and a small Roman numeral are Other_Lowercase with
    # no uppercase to map to, a circled capital is Other_Uppercase, a
    # titlecase letter maps both ways, "ß" has no simple uppercase and is
    # lowercase in vim anyway, and "ŉ" has only a multi-character one and is
    # neither case there.
    "ʰ", "ⅰ", "Ⓐ", "ǅ", "ß", "ŉ",
]


def make_pattern(rng):
    """Build one pattern by concatenating atoms, with a level chosen for it."""
    pieces = [rng.choice(LEVELS)] if rng.random() < 0.6 else []
    for _ in range(rng.randint(1, 4)):
        pieces.append(rng.choice(ATOMS))
        # A level change in the middle is the construct this dialect is
        # about: it moves the line between operator and literal for
        # everything after it, and nothing else here does that.
        if rng.random() < 0.12:
            pieces.append(rng.choice(LEVELS[1:]))
    if rng.random() < 0.15:
        pieces.append("\\|")
        pieces.append(rng.choice(ATOMS))
    pattern = "".join(pieces)
    if rng.random() < 0.125:
        # Splice between complete atoms so the chosen invalid construct
        # remains available as rejection coverage. Unknown POSIX names are
        # ordinary collection fallback, included in ATOMS below.
        where = rng.randint(0, len(pieces))
        pieces.insert(where, rng.choice(REFUSED))
        pattern = "".join(pieces)
    return pattern


# Separate fresh processes are required for span and group queries: even two
# calls for one input can inherit NFA capture endpoints. Placeholder fields
# let both replies use the same validated envelope; only ask_vim()
# may combine their independently validated results.
VIM_SPAN_SCRIPT = """
let lines = readfile(g:vimdiff_in)
let out = []
for l in lines
  let c = json_decode(l)
  let phase = 'pattern'
  try
    let p = matchstrpos(c[1], c[0])
    if p[1] < 0
      let answer = {'status':'nomatch'}
    else
      let fields = [printf('%d:%d', p[1], p[2])] + repeat([''], 9)
      let answer = {'status':'match', 'fields':fields}
    endif
""" + vim_protocol.CATCH + """endfor
call writefile(out, g:vimdiff_out)
qa!
"""

VIM_GROUP_SCRIPT = """
let lines = readfile(g:vimdiff_in)
let out = []
for l in lines
  let c = json_decode(l)
  let phase = 'pattern'
  try
    let g = matchlist(c[1], c[0])
    if empty(g)
      let answer = {'status':'nomatch'}
    else
      let answer = {'status':'match', 'fields':['0:0'] + g[1:9]}
    endif
""" + vim_protocol.CATCH + """endfor
call writefile(out, g:vimdiff_out)
qa!
"""

# Historical two-query transport is diagnostic only. It must never supply
# normative expectations, even when each distinct case has its own process.
VIM_SCRIPT = """
let lines = readfile(g:vimdiff_in)
let out = []
for l in lines
  let c = json_decode(l)
  let phase = 'pattern'
  try
    let p = matchstrpos(c[1], c[0])
    if p[1] < 0
      let answer = {'status':'nomatch'}
    else
      let g = matchlist(c[1], c[0])
      let fields = [printf('%d:%d', p[1], p[2])]
      for i in range(1, 9)
        call add(fields, g[i])
      endfor
      let answer = {'status':'match', 'fields':fields}
    endif
""" + vim_protocol.CATCH + """endfor
call writefile(out, g:vimdiff_out)
qa!
"""


# Every vim this file runs goes through here, and the `--cmd` is the reason
# the list exists rather than being spelled at the call site.
#
# vim takes `&encoding` from the locale, and `-u NONE` means no vimrc is read
# to set it. Under `LANG=C`, or with `LANG` unset as it is in a container,
# `&encoding` is latin1 and vim reads each byte of a UTF-8 subject as its own
# character: `\X` over U+00E9 then "Y" answers 0:1 where a UTF-8 vim answers
# 0:2, and `\k\+` over U+00C9 then "x" answers 0:2 against 0:3.
#
# Measured at this gate's defaults, that was **1855 of 50980 rows** - every
# one a non-ASCII subject, and none of them a defect in this library. It also
# moved two exclusion counts (113 known vim artifacts to 119, and four rows
# into "neither engine gives it"), so part of the damage was being absorbed
# by the exclusion list rather than counted as disagreement, which is why the
# figure is larger than a glance at a sample suggests.
#
# So the gate's green result was a statement about the shell `make` happened
# to be started from, and no file in either repository recorded which locale
# it had to be. Setting it here rather than inside the two scripts below puts
# it at the one place every invocation passes, so a third script cannot be
# added without it. vim_encoding() below confirms it took.
#
# 'iskeyword' is the second pin, added 2026-09-24 for the same reason and
# found the same way. `-u NONE` does not mean "vim's defaults" - it means
# **Vi-compatible** defaults, and 'iskeyword' is one of exactly two
# regex-visible options whose default differs between the two modes:
# `@,48-57,_` compatible against `@,48-57,_,192-255` not, which vim's own
# help calls the Vim default. That range is the whole reason U+00D7 and
# U+00F7 are keyword characters - they are the only two members of 192-255
# that vim's `@` does not cover - so a gate run in compatible mode reports a
# `\k` two code points narrower than any user's vim has, and this library's
# tables were enumerated from exactly such a run.
#
# The *option* rather than the mode, deliberately. `set nocompatible` would
# also flip 'cpoptions', which is the other regex-visible difference and is
# not the one in question; pinning what is actually being claimed keeps the
# blast radius measured. The other three option-backed sets - 'isident',
# 'isfname' and 'isprint' - are identical in both modes, checked rather than
# assumed, which is why only this one is set.
# The command line moved to tools/oracle/vim_runner.py, and the reason is the
# one the comment above gives applied one file further out. It said a third
# script could not be added without the setting - and `replace_diff.py`
# already had a vim command line of its own, which never got it, and was
# still reporting 67 disagreements per 9,600 rows under `LANG=C` on
# 2026-09-25. A module is what enforces "one spelling"; a comment in one file
# is not.
#
# Kept as a name so that the two call sites below read the same as they did.
VIM_COMMAND = vim_runner.BASE + vim_runner.PINS


def ask_vim(cases, engine=2, reference_max_memory=None):
    """Join fresh span/group operations, never two searches in one process."""
    if engine not in (1, 2):
        raise ValueError("a forced Vim engine (1 or 2) is required")
    settings = "set regexpengine=%d\n" % engine
    if reference_max_memory is not None:
        if type(reference_max_memory) is not int or reference_max_memory <= 0:
            raise ValueError('reference memory allowance must be a positive integer')
        settings += 'set maxmempattern=%d\n' % reference_max_memory
    pairs = list(cases)
    evidence = dict(complete=False, transport='fresh-process-per-operation-v2',
                    inputs=[[p, s] for p, s in pairs], operations={}, aggregation=[])
    try:
        spans = _ask(pairs, settings + VIM_SPAN_SCRIPT)
        evidence['operations']['span'] = spans.evidence
        if spans.evidence.get('complete') is not True:
            raise ValueError('incomplete span reference execution')
        if len(spans) != len(pairs):
            raise ValueError('short span reference population')
        for answer, (_, subject) in zip(spans, pairs):
            normalise_theirs(answer, subject)
        selected = [i for i, answer in enumerate(spans) if answer.startswith('match ')]
        if selected:
            groups = _ask([pairs[i] for i in selected], settings + VIM_GROUP_SCRIPT)
            evidence['operations']['groups'] = groups.evidence
            if groups.evidence.get('complete') is not True:
                raise ValueError('incomplete group reference execution')
            if len(groups) != len(selected):
                raise ValueError('short group reference population')
        else:
            groups = []
        answers = list(spans)
        for group_index, case_index in enumerate(selected):
            group_answer = groups[group_index]
            if not group_answer.startswith('match '):
                raise ValueError('fresh reference span/group operations disagree on outcome')
            span_fields = json.loads(spans[case_index][len('match '):])
            group_fields = json.loads(group_answer[len('match '):])
            fields = [span_fields[0]] + group_fields[1:]
            if len(fields) != 10:
                raise ValueError('reference must supply exactly nine group texts')
            vim_protocol.validate_match_fields(fields, pairs[case_index][1])
            answers[case_index] = 'match ' + json.dumps(fields)
        group_indices = {case: group for group, case in enumerate(selected)}
        evidence['aggregation'] = [dict(index=i, input=[p, s],
            span_process=i, group_process=group_indices.get(i))
            for i, (p, s) in enumerate(pairs)]
        evidence['complete'] = True
        return vim_protocol.Answers(answers, evidence)
    except BaseException as error:
        evidence['complete'] = False
        if isinstance(error, vim_protocol.Failure):
            evidence['failed_operation'] = error.evidence
        evidence['error'] = str(error) or type(error).__name__
        raise vim_protocol.Failure(evidence['error'], evidence) from error


def _ask(cases, script):
    inputs = [[pattern, subject] for pattern, subject in cases]
    answers = vim_isolated.ask(inputs, script, 'vimdiff_in', 'vimdiff_out')
    return vim_protocol.decode(answers, lambda answer: vim_protocol.match_answer(
        answer, answer['input'][1]))


def ask_vim_batch_diagnostic(cases, engine=2):
    """Diagnostic only: distinct cases share history in this transport."""
    if engine not in (1, 2):
        raise ValueError('a forced Vim engine (1 or 2) is required')
    answers = _ask_batch(cases, 'set regexpengine=%d\n' % engine + VIM_SCRIPT)
    answers.evidence['normative'] = False
    answers.evidence['transport'] = 'batch-diagnostic'
    return answers


def _ask_batch(cases, script):
    work = tempfile.mkdtemp(prefix="vim_diff.")
    in_path = os.path.join(work, "cases.jsonl")
    out_path = os.path.join(work, "answers.txt")
    script_path = os.path.join(work, "run.vim")
    with open(script_path, "w", newline="\n") as handle:
        handle.write(script)
    with open(in_path, "w", newline="\n") as handle:
        for pattern, subject in cases:
            handle.write(json.dumps([pattern, subject]) + "\n")
    command = vim_runner.command([
        "--cmd", "let g:vimdiff_in=%s" % json.dumps(in_path),
        "--cmd", "let g:vimdiff_out=%s" % json.dumps(out_path),
        "-c", "source " + script_path], scratch=work)
    inputs = [[pattern, subject] for pattern, subject in cases]
    answers = vim_protocol.run(command, out_path, inputs)
    return vim_protocol.decode(answers, lambda answer: vim_protocol.match_answer(
        answer, inputs[answer['id']][1]))


def normalise_theirs(line, subject):
    """Fold vim's answer into the shape ask_ours() is folded into."""
    if line in ('compile', 'nomatch'):
        return line
    if not isinstance(line, str) or not line.startswith('match '):
        raise ValueError('invalid Vim match response: %r' % line)
    fields = json.loads(line[len("match "):])
    vim_protocol.validate_match_fields(fields, subject)
    span = fields[0]
    groups = fields[1:]
    while groups and groups[-1] == "":
        groups.pop()
    return "match %s %s" % (span, " ".join(json.dumps(g) for g in groups))


def ask_ours(command, cases):
    lines = []
    for pattern, subject in cases:
        lines.append("\t%s\t%s" % (
            binascii.hexlify(pattern.encode()).decode(),
            binascii.hexlify(subject.encode()).decode()))
    finished = vim_protocol.run_driver(command, "\n".join(lines) + "\n")
    return finished.stdout.splitlines()


def normalise_ours(line, subject):
    """A grx_match line in the shape vim answers in.

    grx_match reports byte spans and vim reports the text of each group, so
    the spans are cut out of the subject here. The whole match keeps its
    span, which is the half vim states exactly.
    """
    # Typed unavailable context is deliberately refused in the fresh Vim API.
    if re.fullmatch(r"(?:compile|unavailable) [0-9]+", line):
        return "compile"
    if line == "nomatch":
        return line
    fields = line.split()
    if (len(fields) < 3 or len(fields) > 12 or fields[0] != "match"
            or fields[1] not in ("pike", "backtrack", "bitstate", "dfa")):
        raise ValueError("invalid grx_match response: %r" % line)
    raw = subject.encode("utf-8")
    spans = []
    for index, field in enumerate(fields[2:]):
        if field == "-" and index > 0:
            spans.append("")
            continue
        if re.fullmatch(r"[0-9]+:[0-9]+", field) is None:
            raise ValueError("invalid grx_match span: %r" % field)
        start, end = map(int, field.split(":"))
        if not 0 <= start <= end <= len(raw):
            raise ValueError("grx_match span outside subject: %r" % field)
        try:
            raw[:start].decode("utf-8")
            raw[:end].decode("utf-8")
            spans.append(raw[start:end].decode("utf-8"))
        except UnicodeDecodeError as error:
            raise ValueError("grx_match span splits UTF-8: %r" % field) from error
    groups = spans[1:]
    while groups and groups[-1] == "":
        groups.pop()
    return "match %s %s" % (fields[2], " ".join(json.dumps(g) for g in groups))


def _pinned_ccc(cp):
    """Canonical_Combining_Class for one code point, from the pinned UCD.

    Consulted only for characters the host CPython does not know, so the
    common path never opens a file and the tool keeps working in a tree
    where the UCD has not been fetched unless a subject actually needs it.
    """
    if _pinned_ccc.table is None:
        table, pending = {}, None
        path = os.path.join(HERE, "..", "..", "third_party", "ucd",
            open(os.path.join(HERE, "..", "unicode", "UCD_VERSION"),
                encoding="utf-8").read().strip(), "UnicodeData.txt")
        for line in open(path, encoding="utf-8"):
            f = line.split(";")
            if len(f) < 4:
                continue
            c, name, ccc = int(f[0], 16), f[1], int(f[3])
            if name.endswith(", First>"):
                pending = (c, ccc)
            elif name.endswith(", Last>"):
                for x in range(pending[0], c + 1):
                    table[x] = pending[1]
                pending = None
            else:
                table[c] = ccc
        _pinned_ccc.table = table
    return _pinned_ccc.table.get(cp, 0)


_pinned_ccc.table = None


def _combining(c):
    """combining() for one character, correct for a character CPython lacks.

    unicodedata.combining() returns 0 for an unassigned code point, and
    "unassigned" here means unassigned in the HOST's UCD - 15.1.0 against
    our pinned 17.0.0. 46 code points are assigned at 17.0.0, unknown to
    15.1.0 and carry a nonzero combining class (U+0897, U+1ACF..U+1AEB,
    U+10D69.., U+113CE.., U+1E5EE..), and for those the host answers 0
    because it has never heard of them rather than because they do not
    compose.

    That direction of error is the silent one. holds_composing() deciding
    False sends a row to `candidates`, where vim's old engine may excuse it
    - so a mark the host does not know would let a genuine disagreement be
    absorbed by an exclusion. Deciding True merely counts a row that must
    then be explained.

    Answering "unknown means composing" would be conservative and wrong
    often: 9,988 code points are new since 15.1.0 and only 46 of them
    compose. U+187F8, a subject in this file, is one of the other 9,942.
    So the pin is consulted for exactly the characters the host cannot
    answer for, which is the smallest set that is also correct.
    """
    if unicodedata.category(c) == "Cn":
        return _pinned_ccc(ord(c))
    return unicodedata.combining(c)


def holds_composing(subject):
    """Whether the subject carries a composing character after something.

    unicodedata.combining() is not vim's table - vim's is the one in
    src/unicode/display.c, measured - but the two agree on everything a
    subject here is built from, and what this needs is "might the cluster
    rules be in play", not the set itself.

    That agreement is now checked rather than asserted, because CPython
    carries its own UCD and it is behind ours: unicodedata 15.1.0 against
    the pinned 17.0.0, two releases. Every distinct character in this
    file's literals - 127 of them - was compared against field 3 of
    third_party/ucd/17.0.0/UnicodeData.txt on the boolean this function
    actually uses, and none disagrees.

    Two failure modes, and the wider measurement closes only one of them.
    A comparison of the two releases over every code point assigned in both,
    289,394 of them, found the combining class **changed for none**. So no
    value CPython holds is stale - the "it
    changed under us" mode is ruled out as a standing property rather than
    as a fact about the current subject list.

    What that cannot cover is the other mode, because those code points are
    excluded from it by construction: **46 code points are assigned at
    17.0.0 and unknown to 15.1.0 while carrying a nonzero combining class**
    (U+0897, U+1ACF..U+1AFF and others). For one of those, combining()
    returns 0 because CPython has never heard of it, not because it
    disagrees - and holds_composing() would answer "nothing composing here"
    when there is.

    None of this file's 127 characters is one of the 46; that was checked,
    not assumed. So the condition is now a specific set rather than a
    vague one: do not build a subject from a character assigned since
    15.1.0 without re-running the comparison. This is a filter and not a
    verdict, so the cost of being wrong is a case moving into or out of an
    exclusion rather than a wrong answer - which is why it still reads
    unicodedata rather than taking a hard UCD dependency.
    """
    return any(_combining(c) for c in subject[1:])


def ask_old_engine(cases):
    """The forced backtracking reference, retained for deterministic probes."""
    return ask_vim(cases, engine=1)


def find(name):
    for base in ("build/linux/release/apps/tools", "build/release/apps/tools"):
        candidate = os.path.join(ROOT, base, name)
        if os.path.exists(candidate):
            return candidate
    return None


def vim_encoding():
    """The encoding vim actually answers in, or None when it cannot be asked.

    Both halves live in vim_runner now. The argument is unchanged: `which vim`
    asks whether something called vim is on `PATH`, which is not the question.
    The question is whether the vim about to answer fifty thousand rows will
    read them the way they were written, and the only honest way to answer it
    is to reach for that vim, through the same command line, first.
    """
    work = tempfile.mkdtemp(prefix="vim_diff.enc.")
    try:
        return vim_runner.encoding(work)
    finally:
        shutil.rmtree(work, ignore_errors=True)


def vim_version():
    """Which vim answered, by its patch level rather than its release."""
    return vim_runner.version()


def failure_report(kind):
    """Version one: exact rows plus explicit coverage status, even on failure."""
    return dict(schema_version=1, kind=kind, complete=False,
        completed_profiles=[], failures=[], error=None)


def run_reporting(path, report, run):
    def save():
        if path:
            with open(path, "w", encoding="utf-8") as output:
                json.dump(report, output, ensure_ascii=False, indent=2)
                output.write("\n")
    # Invalidate a previous result before invoking any driver.
    save()
    try:
        result = run()
        if result not in (0, 1):
            report['complete'] = False
        if not report['complete']:
            report['error'] = 'comparison incomplete'
            return 2
        return result
    except BaseException as error:
        report['complete'] = False
        report["error"] = str(error) or type(error).__name__
        if isinstance(error, vim_protocol.Failure):
            report['execution_evidence'] = error.evidence
        raise
    finally:
        save()


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--syntax", choices=tuple(PROFILES),
        default="vim-backtrack")
    parser.add_argument("--seed", type=int, default=20260923)
    parser.add_argument("--patterns", type=int, default=1200)
    parser.add_argument("--subjects", type=int, default=0,
        help="subjects per pattern; 0 means all of them")
    parser.add_argument("--examples", type=int, default=12)
    parser.add_argument("--failures", help="write all exact differences as JSON")
    parser.add_argument("--strict", action="store_true",
        help="exit 1 if the two disagree anywhere")
    args = parser.parse_args()

    report = failure_report("match")
    return run_reporting(args.failures, report, lambda: run_comparison(args, report))


def run_comparison(args, report):
    encoding = vim_encoding()
    if encoding is None:
        print("vim_diff: reference encoding probe unavailable")
        return 2
    if encoding != "utf-8":
        sys.stderr.write("vim answers in %s, not utf-8; the subjects here are "
            "UTF-8 and every non-ASCII row would disagree for that reason "
            "alone. VIM_COMMAND sets it, so this means the setting did not "
            "take.\n" % encoding)
        return 2
    # Not "oracle(host)", which is what this line said and what it stopped
    # being true of the moment the reference moved into an image. Where the
    # reference ran is oracle_run.py's line to print; this one says which vim
    # and which encoding, which is the part only this tool can answer.
    print("vim_diff: %s, encoding %s" % (vim_version(), encoding))
    ours = find("grx_match")
    if not ours:
        sys.stderr.write("grx_match not built; run `make tools`\n")
        return 2

    rng = random.Random(args.seed)
    cases = []
    for _ in range(args.patterns):
        pattern = make_pattern(rng)
        subjects = (SUBJECTS if args.subjects <= 0
            else rng.sample(SUBJECTS, min(args.subjects, len(SUBJECTS))))
        for subject in subjects:
            cases.append((pattern, subject))
    composing = composing_cases()
    print("%s composing population: %d atom rows, %d grammar rows, no skips"
        % (args.syntax, len(composing), len(combining_grammar_cases())))
    cases.extend(composing)
    cases.extend(combining_grammar_cases())

    bare, classed = escape_floor.check(args.syntax, ATOMS, driver=ours)
    print("%s vocabulary: %d bare and %d in-class constructs, all spelled"
        % (args.syntax, bare, classed))
    theirs_raw = ask_vim(cases, engine=PROFILES[args.syntax])
    if len(theirs_raw) != len(cases):
        sys.stderr.write("vim answered %d of %d rows\n"
            % (len(theirs_raw), len(cases)))
        return 2
    if hasattr(theirs_raw, 'evidence'):
        report['reference_evidence'] = theirs_raw.evidence
    mine_raw = ask_ours([ours, args.syntax], cases)
    if len(mine_raw) != len(cases):
        sys.stderr.write("grx_match answered %d of %d rows\n"
            % (len(mine_raw), len(cases)))
        return 2

    theirs = [normalise_theirs(line, case[1])
        for line, case in zip(theirs_raw, cases)]
    mine = [normalise_ours(line, case[1])
        for line, case in zip(mine_raw, cases)]

    # A pattern resembling a known gap cannot justify a different answer.
    # Keep every complete normalized result against this profile's engine.
    disagreements = [(case, them, us)
        for case, them, us in zip(cases, theirs, mine) if them != us]

    # What the reference actually answered, because "0 disagreements" over
    # rows the oracle refused outright would be a gate agreeing about
    # nothing.
    kinds = {}
    for line in theirs:
        head = line.split()[0] if line else "empty"
        kinds[head] = kinds.get(head, 0) + 1
    shape = ", ".join("%d %s" % (kinds[k], k) for k in sorted(kinds))
    print("%s: forced regexpengine=%d, %d/%d answers (%s), "
        "%d disagreements, 0 excluded"
        % (args.syntax, PROFILES[args.syntax], len(theirs_raw), len(cases),
            shape, len(disagreements)))
    report["failures"] = [dict(syntax=args.syntax, pattern=case[0],
        subject=case[1], reference=them, actual=us)
        for case, them, us in disagreements]
    report["completed_profiles"].append(args.syntax)
    report["complete"] = True
    for (pattern, subject), them, us in disagreements[:args.examples]:
        print("  /%s/ on %r" % (pattern, subject))
        print("     vim: %s" % them)
        print("    ours: %s" % us)
    if len(disagreements) > args.examples:
        print("  ... and %d more" % (len(disagreements) - args.examples))
    if args.strict and disagreements:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
