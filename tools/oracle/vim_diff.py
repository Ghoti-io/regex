#!/usr/bin/env python3
"""Compare the Vim front end against vim itself, on patterns nobody wrote.

documentation/plan.md WP-36. The dialect's definition is "what vim does", so
one oracle decides every row - the arrangement `python_diff.py` and
`perl_diff.py` use, and not the glibc-and-musl agreement the POSIX rows are
held to.

**One vim process for the whole run.** `probe.py`'s vim driver starts one per
case, which is fine for the eighty rows that page fills and hopeless for a
differential. vim reads a file of cases and writes a file of answers, so a
hundred thousand rows cost one fork - the same order of magnitude as
`python_diff.py`'s in-process `re`, for a reference that cannot be imported.

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
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

# One entry per construct vim has, so that a combination exercises the
# interactions rather than one rule at a time. Grouped by what decides them,
# because the magic level is what this dialect is *for* and a vocabulary that
# only spelled magic-level patterns would test one of the four grammars.
ATOMS = [
    # Literals and the characters whose meaning the level decides.
    "a", "b", ".", "\\.", "*", "\\*", "^", "$", "\\^", "\\$",
    "\\n", "\\t", "\\e", "\\r", "\\b", "\\\\", "z", "q",
    # The named classes, and their complements, and the `\\_` forms that add
    # the line break.
    "\\s", "\\S", "\\d", "\\D", "\\w", "\\W", "\\h", "\\H", "\\a", "\\A",
    "\\l", "\\L", "\\u", "\\U", "\\x", "\\X", "\\o", "\\O", "\\i", "\\k",
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
    "[\\x41]", "[\\e]",
    # Groups and alternation, in both spellings.
    "\\(a\\)", "\\(a\\|b\\)", "\\%(ab\\)", "\\(a\\)\\1",
    "(a)", "(a|b)", "%(ab)", "(a)\\1",
    # The postfix assertions. The four that carry a mark are here because a
    # marker inside an assertion or an atomic group is inert, and a
    # vocabulary whose group bodies were all plain atoms could not spell the
    # question: `\\(a\\zsb\\)\\@>c` was answered 1-3 here and 0-3 by vim
    # for as long as this file had no way to generate it.
    "\\(a\\)\\@=", "\\(a\\)\\@!", "\\(a\\)\\@<=", "\\(a\\)\\@<!",
    "\\(a\\+\\)\\@>", "(a)@=", "(a)@!", "(a)@<=", "(a)@<!", "(a+)@>",
    "\\(a\\zsb\\)\\@>", "\\(a\\zeb\\)\\@>", "\\(a\\zsb\\)\\@=",
    "\\(a\\zeb\\)\\@=", "\\(a\\zeb\\)\\@<=",
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
    "\\%1c", "\\v%2c",
    # The two that move the reported match, and the level markers themselves.
    # `\\=` after a mark is the one multi vim allows there, and it is the
    # case that separates the spelling from the bounds: `\\zs\\{0,1}` is
    # E888 and asks for the same repeat.
    "\\zs", "\\ze", "\\zs\\=", "\\ze\\?", "\\v\\zs=",
    "\\v", "\\m", "\\M", "\\V", "\\c", "\\C",
    # The branch operator, which is a grammar level rather than an atom.
    "\\&", "&",
]

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

# Constructs vim *accepts* and this library refuses, listed here and
# deliberately not generated. A differential is a comparison, and a row whose
# two answers are known to differ by design measures nothing except how many
# times the generator happened to spell it - it would put a floor under the
# disagreement count and hide the next real one under it.
#
# Each is a documented deviation in documentation/dialects.md section 6, and
# each has a test in tests/unit/test_vim.cpp asserting the refusal, which is
# where a regression that started accepting one would be caught:
#
#   \Z           ignore Unicode combining characters
#   \%23v        the *screen* column: a tabstop and a cell-width table
#   ~ and \~     the text of the last `:s` replacement
#   \%V \%#      the Visual area and the cursor: not in a string
#   \%23l \%23c  a buffer line and a byte column: no assertion kind for them
NOT_IMPLEMENTED = [
    "\\Z", "~", "\\~", "\\%23v", "\\%<4v",
]

# The four levels, written as the prefix that selects one. The empty string
# is magic, which is vim's default and what a pattern with no marker gets.
LEVELS = ["", "\\v", "\\m", "\\M", "\\V"]

SUBJECTS = [
    "", "a", "ab", "aaab", "abc", "ABC", "a b", "a\tb", "a\nb", "\n",
    "abcabc", "xayaz", "[a]", "a*b", "a+b", "a.c", "(a)", "a|b", "read",
    "rea", "r", "A", "0", "_", "é", "É", "aéb", "~", "^a$",
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
        # Spliced between pieces rather than at a random byte. A byte offset
        # lands inside a construct as often as not, and the one construct
        # that matters is `[[:name:]]`: vim compiles a collection with an
        # unknown class name into a pattern that can never match *anything*
        # - `[[:foo:]]*a` does not match "a" - which is a degenerate answer
        # rather than a rule, and generating it by the thousand would put a
        # floor under the disagreement count and hide the next real one.
        # The invalid constructs are still generated; they are the entries
        # of REFUSED, each of which vim refuses outright.
        where = rng.randint(0, len(pieces))
        pieces.insert(where, rng.choice(REFUSED))
        pattern = "".join(pieces)
    return pattern


VIM_SCRIPT = """
let lines = readfile(g:vimdiff_in)
let out = []
for l in lines
  let c = json_decode(l)
  try
    let p = matchstrpos(c[1], c[0])
    if p[1] < 0
      call add(out, 'nomatch')
    else
      let g = matchlist(c[1], c[0])
      let fields = [printf('%d:%d', p[1], p[2])]
      for i in range(1, 9)
        call add(fields, g[i])
      endfor
      call add(out, 'match ' . json_encode(fields))
    endif
  catch
    call add(out, 'compile')
  endtry
endfor
call writefile(out, g:vimdiff_out)
qa!
"""


def ask_vim(cases):
    """The reference: one process for every case in the run."""
    return _ask(cases, VIM_SCRIPT)


def _ask(cases, script):
    work = tempfile.mkdtemp(prefix="vim_diff.")
    in_path = os.path.join(work, "cases.jsonl")
    out_path = os.path.join(work, "answers.txt")
    script_path = os.path.join(work, "run.vim")
    with open(script_path, "w") as handle:
        handle.write(script)
    with open(in_path, "w") as handle:
        for pattern, subject in cases:
            handle.write(json.dumps([pattern, subject]) + "\n")
    command = ["vim", "-es", "-u", "NONE", "-i", "NONE",
        "--cmd", "let g:vimdiff_in=%s" % json.dumps(in_path),
        "--cmd", "let g:vimdiff_out=%s" % json.dumps(out_path),
        "-c", "source " + script_path]
    try:
        subprocess.run(command, stdin=subprocess.DEVNULL,
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1800)
    except subprocess.TimeoutExpired:
        sys.stderr.write("vim did not finish within 1800s\n")
        return []
    if not os.path.exists(out_path):
        sys.stderr.write("vim wrote no answers\n")
        return []
    with open(out_path) as handle:
        return [line.rstrip("\n") for line in handle]


def normalise_theirs(line, subject):
    """Fold vim's answer into the shape ask_ours() is folded into."""
    if not line.startswith("match "):
        return line
    fields = json.loads(line[len("match "):])
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
    try:
        finished = subprocess.run(command, input="\n".join(lines) + "\n",
            capture_output=True, text=True, timeout=900)
    except subprocess.TimeoutExpired:
        sys.stderr.write("%s did not finish within 900s\n" % command[0])
        return []
    return finished.stdout.splitlines()


def normalise_ours(line, subject):
    """A grx_match line in the shape vim answers in.

    grx_match reports byte spans and vim reports the text of each group, so
    the spans are cut out of the subject here. The whole match keeps its
    span, which is the half vim states exactly.
    """
    if line.startswith("compile") or line.startswith("error"):
        return "compile"
    if not line.startswith("match "):
        return line
    fields = line.split()[2:]
    raw = subject.encode()
    whole = fields[0] if fields else "0:0"
    groups = []
    for field in fields[1:]:
        if field == "-":
            groups.append("")
            continue
        start, end = field.split(":")
        groups.append(raw[int(start):int(end)].decode("utf-8", "replace"))
    while groups and groups[-1] == "":
        groups.pop()
    return "match %s %s" % (whole, " ".join(json.dumps(g) for g in groups))


def is_forward_reference_artifact(pattern, them, us):
    """vim accepts a forward backreference when a lookbehind follows it.

    Every other spelling is "E65: Illegal back reference", in both of vim's
    engines: `\\1\\(a\\)`, `\\(a\\1\\)`, `\\1\\(a\\)\\@=`,
    `\\1\\(a\\)\\@>` and `\\1\\%(a\\)` are all refused, and
    `\\1\\(a\\)\\@<!` and `\\1\\(a\\)\\@<=` are accepted with the
    reference matching empty. The rule vim documents is the first one, and a
    construct that is legal only when a *later* part of the pattern takes a
    particular shape is an artifact of how vim compiles a lookbehind rather
    than a rule a second implementation could follow - the same category as
    the two `reference-defect` records in tests/data/vectors/known-gaps.txt.

    Counted and reported rather than dropped silently, so that the number
    moving is visible.
    """
    if not us.startswith("compile") or them.startswith("compile"):
        return False
    return "@<" in pattern and any(
        "\\%d" % n in pattern for n in range(1, 10))


def is_postfix_capture_artifact(pattern, them, us):
    """vim mislays a capture around one of its postfix assertions.

    Two shapes, both of them vim disagreeing with pcre2test as well as with
    this library, and both of them only about *groups* - the whole match is
    the same either way, which is the first thing this checks:

    - **A capture that outlived its branch.** `\\(a\\)\\@>x\\|\\A`
      against "a b" reports group one as "a" in vim, where the branch that
      set it failed and the match came from the other one. Without the
      `\\@>` vim reports it unset, so it is the atomic group committing its
      writes; pcre2test answers `(?>(a))x|[^[:alpha:]]` with group one unset.
    - **A capture that vanished.** `\\(a\\)\\(a\\)\\@=a\\{2,}`
      against "aaab" reports group one as empty in vim and "a" in pcre2test;
      drop the trailing repeat and vim reports "a" too.

    The two directions are not treated alike. vim *losing* a capture is
    allowed for any of the `\\@` operators, because a defect of this
    library's would be the same loss and would still be reported - the
    comparison only stops seeing vim's. vim *gaining* one is allowed only
    where the atomic operator is written, which is the one shape it was
    measured in.
    """
    if not (them.startswith("match ") and us.startswith("match ")):
        return False
    if "@" not in pattern:
        return False
    their_fields = them.split()
    our_fields = us.split()
    if their_fields[1] != our_fields[1]:
        return False
    theirs = their_fields[2:]
    ours = our_fields[2:]
    while len(theirs) < len(ours):
        theirs.append('""')
    while len(ours) < len(theirs):
        ours.append('""')
    for their_group, our_group in zip(theirs, ours):
        if their_group == our_group:
            continue
        if their_group == '""':
            continue        # vim lost one: allowed for any `\@` operator.
        if our_group == '""' and "@>" in pattern:
            continue        # vim kept one an atomic group had written.
        return False
    return True


ENGINE_SCRIPT = """
let lines = readfile(g:vimdiff_in)
let out = []
set re=1
for l in lines
  let c = json_decode(l)
  try
    let p = matchstrpos(c[1], c[0])
    if p[1] < 0
      call add(out, 'nomatch')
    else
      let g = matchlist(c[1], c[0])
      let fields = [printf('%d:%d', p[1], p[2])]
      for i in range(1, 9)
        call add(fields, g[i])
      endfor
      call add(out, 'match ' . json_encode(fields))
    endif
  catch
    call add(out, 'compile')
  endtry
endfor
call writefile(out, g:vimdiff_out)
qa!
"""


def ask_old_engine(cases):
    """The same questions, put to vim's *other* regexp engine.

    vim ships two, chosen by 'regexpengine', and they do not always agree:
    `\\%^\\|a\\?` against "a" is the empty match at 0 under `set re=1`
    and "a" under the default `re=2`. Where they disagree, one of them is
    wrong and the dialect is whichever this library can also justify from
    somewhere else - which so far has been the old engine every time.
    ask_vim() drives the default, so this is the second opinion, asked only
    about the rows that came back different.
    """
    return _ask(cases, ENGINE_SCRIPT)


def is_very_magic_line_start_repeat(pattern, them, us):
    """`\\v\\_^*` matches nothing at all in vim, and there is no rule in it.

    `\\m\\_^*` matches the empty string, `\\v\\_$*` does,
    `\\v\\_^{0,1}` does and `\\v(\\_^)*` does - it is only the very
    magic level, only `\\_^`, and only the `*` spelling, and both of vim's
    engines answer alike. A construct that means nothing when written one
    way and the obvious thing when written three others is an accident of
    vim's parser rather than a rule a second implementation could follow.
    """
    if not them.startswith("nomatch") or not us.startswith("match "):
        return False
    where = pattern.find("\\_^*")
    return where > 0 and "\\v" in pattern[:where]


def is_line_number_star(pattern, them, us):
    """`\\%23l*` matches nothing at all in vim, and there is no rule in it.

    `\\%23l` names a buffer line and never matches over a string, so a `*`
    on it should leave the empty match a zero-iteration repeat always has -
    and vim agrees five ways: `\\%23l\\{}`, `\\%23l\\{-}`,
    `\\%23l\\{0,1}`, `\\(\\%23l\\)*` and the very magic `%23l*` all
    match the empty string there. Only the bare `*`, only outside very
    magic, only after the `l` forms - `\\%V*` and `\\%2c*` match the empty
    string - and both engines alike. The same shape as
    is_very_magic_line_start_repeat() above, and excluded for the same
    reason.
    """
    if not them.startswith("nomatch") or not us.startswith("match "):
        return False
    for form in ("l*", "l\\{1}"):
        where = pattern.find(form)
        while where > 0:
            head = pattern[:where]
            mark = head.rfind("\\%")
            if mark >= 0 and head[mark + 2:].lstrip("<>").isdigit():
                return True
            where = pattern.find(form, where + 1)
    return False


def is_lookbehind_backreference_artifact(pattern, them, us):
    """vim mis-accounts a postfix lookbehind when a backreference follows.

    Two measured instances, and pcre2test answers this library's way in
    both:

    - `\\(a\\)\\@<=\\(a\\)\\1\\l` against "aaab" is "aa" at 1:3
      in both of vim's engines - text with nothing in it for the trailing
      `\\l` to have matched. `(?<=(a))(a)\\1[a-z]` is 1:4 in pcre2test,
      as here.
    - `\\v\\D(a)@<=\\m\\(a\\)\\1\\(a\\+\\)\\@>` against
      "aaab" is 0:3 in both engines with a third group that consumed nothing
      the match has room for; `[^0-9](?<=(a))(a)\\1(?>(a+))` is "No match"
      in pcre2test, as here. Drop the atomic group and vim's two engines
      stop agreeing with each other: 0:2 under `re=1` and 0:3 under `re=2`.

    So the class is "a postfix lookbehind with a backreference after it",
    and it is excluded whatever the disagreement looks like - which is a
    real narrowing of this gate and is the reason it is written out here
    rather than folded into one of the others. A regression of this
    library's inside that shape would not be reported.
    """
    if "@<" not in pattern:
        return False
    return any("\\%d" % n in pattern for n in range(1, 10))


def find(name):
    for base in ("build/linux/release/apps/tools", "build/release/apps/tools"):
        candidate = os.path.join(ROOT, base, name)
        if os.path.exists(candidate):
            return candidate
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--seed", type=int, default=20260923)
    parser.add_argument("--patterns", type=int, default=1200)
    parser.add_argument("--subjects", type=int, default=0,
        help="subjects per pattern; 0 means all of them")
    parser.add_argument("--examples", type=int, default=12)
    parser.add_argument("--strict", action="store_true",
        help="exit 1 if the two disagree anywhere")
    args = parser.parse_args()

    if subprocess.run(["which", "vim"], capture_output=True).returncode != 0:
        print("vim_diff: skipped (vim is not installed)")
        return 0
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

    theirs_raw = ask_vim(cases)
    if len(theirs_raw) != len(cases):
        sys.stderr.write("vim answered %d of %d rows\n"
            % (len(theirs_raw), len(cases)))
        return 2
    mine_raw = ask_ours([ours, "vim"], cases)
    if len(mine_raw) != len(cases):
        sys.stderr.write("grx_match answered %d of %d rows\n"
            % (len(mine_raw), len(cases)))
        return 2

    theirs = [normalise_theirs(line, case[1])
        for line, case in zip(theirs_raw, cases)]
    mine = [normalise_ours(line, case[1])
        for line, case in zip(mine_raw, cases)]

    disagreements = []
    artifacts = 0
    candidates = []
    for case, them, us in zip(cases, theirs, mine):
        if them == us:
            continue
        if (is_forward_reference_artifact(case[0], them, us)
                or is_postfix_capture_artifact(case[0], them, us)
                or is_lookbehind_backreference_artifact(case[0], them, us)
                or is_very_magic_line_start_repeat(case[0], them, us)
                or is_line_number_star(case[0], them, us)):
            artifacts += 1
            continue
        candidates.append((case, them, us))

    # Ask vim's other engine about what is left, rather than guessing from
    # the shape of the pattern which disagreements are vim arguing with
    # itself. A row where the two engines disagree is a row vim has no one
    # answer for; a row where they agree with each other and not with us is
    # ours to fix. Only the rows that came back different are asked, so the
    # second process costs nothing on a clean run.
    #
    # Two counts, because they are not equally informative. Most split rows
    # turn on one axis and `set re=1` gives exactly this library's answer.
    # The rest turn on *two*: this library follows the old engine where a
    # mark sits inside an assertion and the new one where `\c` meets
    # `[[:lower:]]`, each because that engine is the one whose answers can
    # be stated as a rule, so a pattern holding both agrees with neither.
    # Those are printed rather than swallowed - a defect of this library's
    # could hide among them, and the only defence is that a person can see
    # them.
    engine_split = 0
    both_axes = []
    if candidates:
        second = ask_old_engine([case for case, _, _ in candidates])
        if len(second) != len(candidates):
            sys.stderr.write("vim's old engine answered %d of %d rows\n"
                % (len(second), len(candidates)))
            return 2
        for (case, them, us), old in zip(candidates, second):
            older = normalise_theirs(old, case[1])
            if older == us:
                engine_split += 1
                continue
            if older != them:
                both_axes.append((case, them, us, older))
                continue
            disagreements.append((case, them, us))

    # What the reference actually answered, because "0 disagreements" over
    # rows the oracle refused outright would be a gate agreeing about
    # nothing.
    kinds = {}
    for line in theirs:
        head = line.split()[0] if line else "empty"
        kinds[head] = kinds.get(head, 0) + 1
    shape = ", ".join("%d %s" % (kinds[k], k) for k in sorted(kinds))
    notes = []
    if artifacts:
        notes.append("%d excluded (known vim artifacts)" % artifacts)
    if engine_split:
        notes.append("%d excluded (vim's two engines disagree; `set re=1` "
            "gives this library's answer)" % engine_split)
    if both_axes:
        notes.append("%d excluded (vim's two engines disagree and neither "
            "gives it; listed below)" % len(both_axes))
    print("vim_diff: %d rows (%s), %d disagreements%s"
        % (len(cases), shape, len(disagreements),
            "".join(", " + note for note in notes)))
    for (pattern, subject), them, us in disagreements[:args.examples]:
        print("  /%s/ on %r" % (pattern, subject))
        print("     vim: %s" % them)
        print("    ours: %s" % us)
    if len(disagreements) > args.examples:
        print("  ... and %d more" % (len(disagreements) - args.examples))
    for (pattern, subject), them, us, older in both_axes[:args.examples]:
        print("  [both axes] /%s/ on %r" % (pattern, subject))
        print("     re=2: %s" % them)
        print("     re=1: %s" % older)
        print("     ours: %s" % us)
    if len(both_axes) > args.examples:
        print("  ... and %d more two-axis rows"
            % (len(both_axes) - args.examples))
    if args.strict and disagreements:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
