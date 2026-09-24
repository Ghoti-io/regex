#!/usr/bin/env python3
"""Compare replacement templates against their references, on templates
nobody wrote.

`tools/oracle/sed_diff.py` does this for the POSIX and GNU rows against sed.
Nothing did it for ECMAScript, whose template grammar is the largest of the
five this library implements - `$1`, `$<name>`, `$&`, the two context forms
`` $` `` and `$'`, `$$`, and the rule that decides what every *other* `$` is.

That last rule is the reason a generator is worth more here than a list. The
recognised forms are easy to check by hand and this file's author did check
them, all twelve, and found no disagreement. What is hard to check by hand is
the boundary: `$12` is group 1 followed by "2" when the pattern has one
group and group 12 when it has twelve; `$<` with no `>` is two characters;
`$<nope>` names a group that does not exist and ECMAScript says the whole
thing is empty rather than an error. Those are decided by interactions
between the template and the *pattern*, which is exactly what a hand-written
list cannot cover and a generator covers for free.

**Each dialect has one definition.** node is ECMA-262 22.1.3.19
GetSubstitution; pcre2's `pcre2_substitute()` is the PCRE2 grammar. The perl
row has no generated comparison, for the reason `perl_diff.py` gives at
length: perl's replacement is an interpolated string rather than a grammar of
its own, so asking perl about a template means evaluating it as perl code,
and generated code is not something to run.

The two grammars disagree about the rule that decides everything else -
ECMAScript makes an unrecognised `$` ordinary text, PCRE2 refuses it - which
is why they are separate vocabularies and separate runs rather than one.

Usage:
    tools/oracle/replace_diff.py [--seed N] [--patterns N] [--examples N]
                                 [--dialect ecmascript|pcre|all]

Copyright 2026 by Corey Pennycuff
"""

import argparse
import json
import os
import random
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

sys.path.insert(0, HERE)

import match_diff
import perl_diff
import vim_diff

# The pieces a template is built from. Every recognised form, every way of
# spelling something that looks like one and is not, and plain text between
# them so that a `$` at the very end is reached as often as one in the
# middle.
# The pieces a template is built from. Every recognised form, every way of
# spelling something that looks like one and is not, and plain text between
# them so that a `$` at the very end is reached as often as one in the
# middle.
#
# Split into well-formed and not, and weighted, because the two dialects
# disagree about what "not" *means*: ECMAScript has no ill-formed template
# and PCRE2 refuses a dozen spellings. A vocabulary that was one third
# malformed put three pcre rows in five beyond comparison - pcre2 parses a
# template only when it has a match to put it in, so a bad template on a
# subject that does not match is not an answer either way - and a run that
# excludes most of its rows is measuring its own generator.
WELL_FORMED = [
    # Plain text, weighted by appearing several times.
    "X", "-", "", "ab", " ", "X", "-", " ",
    # The forms both dialects recognise.
    "$&", "$$", "$1", "$2",
    "$&", "$$", "$1",
]

# Python's, which shares only the plain text: every reference form above is
# introduced by `$`, and `$` is an ordinary character in a `re.sub` template.
PYTHON_WELL_FORMED = ["X", "-", "", "ab", " ", "$1", "$&", "X", "-", " "]

# Spellings `re` refuses. Its template alphabet is closed the way its pattern
# alphabet is, so an unknown escape is "bad escape" rather than the letter -
# the difference from sed's rule, which this library had been applying.
PYTHON_MALFORMED = [
    "\\q", "\\x41", "\\e", "\\c", "\\g1", "\\g<", "\\g<>", "\\g<-1>",
    "\\3", "\\12", "\\99", "\\g<nosuch>", "\\", "\\u00",
    # `\U` and `\N{...}` are pattern escapes in Python and errors in a
    # template. They are here, among the malformed, for exactly that reason.
    "\\U00000041", "\\N{BULLET}",
]

# Forms one dialect has and the other does not, or that mean different
# things. Kept apart so a run can say which side it is asking about.
DIALECT_FORMS = {
    "ecmascript": ["$`", "$'", "$<n>", "$<m>", "$3", "$9"],
    "pcre": ["$`", "$'", "$_", "$<n>", "${n}", "$n", "${1}", "$0", "${0}"],
    # Python's sigil is a backslash and its alphabet is closed, so its forms
    # share nothing with the other two: `$1` is two literal characters here
    # and `\\1` is the group. The escapes are in the list because they are
    # the half of this grammar that *decodes* - `\\n` is a newline, not the
    # letter - which no other dialect's template does.
    "python": ["\\1", "\\2", "\\g<1>", "\\g<n>", "\\g<0>", "\\0", "\\\\",
               "\\n", "\\t", "\\101", "\\g<01>"],
    # Vim's. `&` is the whole match and `\&` a literal one, `\0` to `\9`
    # name groups a digit at a time, `\n`, `\r`, `\t` and `\b` decode,
    # and the six case markers are the half of this grammar no other
    # dialect here has: they emit nothing and change what follows.
    "vim": ["&", "\\&", "\\0", "\\1", "\\2", "\\9", "~", "\\~",
            "\\n", "\\r", "\\t", "\\b", "\\\\", "\\q",
            "\\u", "\\l", "\\U", "\\L", "\\E", "\\e",
            "\\u\\1", "\\U\\1\\E\\2", "\\Uab\\lcd", "\\u&"],
}

# Spellings that begin like a form and are not one, or name something that is
# not there. A minority of the vocabulary, not a third of it.
MALFORMED = [
    "$", "$<", "$<>", "$<nope>", "$<n", "$}", "$-",
    "$10", "$12", "$01", "$0",
    "$$1", "$&$&", "$1$", "$$<n>",
]

# Patterns whose group count and names the numbered and named forms can be
# tested against. Generated patterns come from match_diff, which knows how to
# build something node will accept; these are added so that `$<n>` and the
# two-digit forms have something to refer to in at least some rows.
NAMED_PATTERNS = [
    "(?<n>a)", "(?<n>a)(?<m>b)", "(?<n>a)(b)", "(a)(?<n>b)",
    "(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)",
    "(a)", "(a)(b)", "(a)(b)(c)", "(?<n>a)|(?<n>b)",
]

# Each dialect's own alphabet. `u` and `v` are ECMAScript's letters and the
# pcre row does not have them - PCRE2's UTF mode is an option rather than a
# pattern flag - so a shared list would ask this library for something it
# rightly refuses and count the refusal as a disagreement. Which is what the
# first draft did, 1,283 times.
#
# Both sides of the pcre run are therefore byte-oriented, which is pcre2's
# default and this library's for that row, so the two agree about what a
# position is and the surrogate question never arises.
FLAG_SETS = {
    "ecmascript": ("", "u", "i", "m", "iu", "s"),
    "pcre": ("", "i", "m", "s", "x", "im"),
    "python": ("", "i", "m", "s", "im"),
    # Vim has no flag string at all: `\c`, `\v` and the rest are pattern
    # syntax, so the alphabet is empty and the only valid value is "".
    "vim": ("",),
}


# The same list in Python's spelling: `(?<n>...)` is "unknown extension"
# there, and a reference to `\g<n>` needs a group actually named `n`.
PYTHON_NAMED_PATTERNS = [
    "(?P<n>a)", "(?P<n>a)(?P<m>b)", "(?P<n>a)(b)", "(a)(?P<n>b)",
    "(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)",
    "(a)", "(a)(b)", "(a)(b)(c)",
]

# And in vim's, where a group is `\(...\)` and there are no names at all.
VIM_NAMED_PATTERNS = [
    "\\(a\\)", "\\(a\\)\\(b\\)", "\\(a\\)\\(b\\)\\(c\\)",
    "\\(.\\)", "\\(\\w\\)\\(\\w\\)", "\\(a\\|b\\)",
    "\\(a\\)\\(b\\)\\(c\\)\\(d\\)\\(e\\)\\(f\\)",
]


def find(name):
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(ROOT, "build", platform, build, "apps",
                "tools", name)
            if os.path.exists(path):
                return path
    return None


def make_template(dialect, rng):
    """A template, mostly well formed.

    One piece in six is a spelling that begins like a form and is not, which
    is enough for every run to ask the accept-or-refuse question many times
    over without spending the budget on it.
    """
    if dialect == "python":
        pieces = PYTHON_WELL_FORMED + DIALECT_FORMS[dialect]
        malformed = PYTHON_MALFORMED
    elif dialect == "vim":
        # No malformed list: vim's template alphabet is open the way sed's
        # is - every escape it does not know is the bare character - so
        # there is no spelling to refuse and nothing for one to measure.
        pieces = ["X", "-", "", "ab", " "] + DIALECT_FORMS[dialect]
        malformed = pieces
    else:
        pieces = WELL_FORMED + DIALECT_FORMS[dialect]
        malformed = MALFORMED
    out = []
    for _ in range(rng.randint(1, 4)):
        source = malformed if rng.randrange(6) == 0 else pieces
        out.append(rng.choice(source))
    return "".join(out)


def make_pattern(dialect, rng):
    """A pattern in the dialect's own grammar.

    Not match_diff's for both. That generator builds ECMAScript, and an
    ECMAScript pattern is not a PCRE2 pattern - the two grammars overlap and
    do not coincide - so half the pcre rows came back as "this library
    refuses the pattern, pcre2 does not" and buried the template question the
    run exists to ask. perl_diff's per-dialect atoms are the right
    vocabulary, and they are already written.
    """
    if dialect == "ecmascript":
        return match_diff.make_pattern(rng)
    if dialect == "vim":
        # vim_diff's own generator, refused constructs and all: this run has
        # to ask the accept-or-refuse question too, and vim's template
        # alphabet has no ill-formed spelling to ask it with.
        return vim_diff.make_pattern(rng)
    if dialect == "python":
        # python_diff's vocabulary, which is the one `re` accepts. Its named
        # groups use the `(?P<n>...)` spelling, so NAMED_PATTERNS below is
        # replaced for this dialect rather than shared.
        import python_diff
        return "".join(rng.choice(python_diff.ATOMS)
                       for _ in range(rng.randint(1, 3)))
    atoms = perl_diff.ATOMS["pcre"]
    return "".join(rng.choice(atoms) for _ in range(rng.randint(1, 3)))


def ask_pcre2(driver, rows):
    """pcre2's answers, each paired with how many substitutions it made."""
    lines = "".join("%s\t%s\t%s\t%s\n" % (
        flags, pattern.encode("utf-8").hex(), subject.encode("utf-8").hex(),
        template.encode("utf-8").hex())
        for flags, pattern, subject, template in rows)
    finished = subprocess.run([driver, "replace"], input=lines,
        capture_output=True, text=True, check=True)
    out = []
    for line in finished.stdout.splitlines():
        if line.startswith("ok "):
            count, _, body = line[3:].partition(" ")
            out.append((bytes.fromhex(body).decode("utf-8", "replace"),
                int(count)))
        else:
            out.append((parse_driver(line), 0))
    return out


def parse_driver(line):
    """A grx_replace line, as the shape node's answers have."""
    if line.startswith("ok "):
        return bytes.fromhex(line[3:]).decode("utf-8", "replace")
    if line == "ok":
        return ""
    if line.startswith("compile"):
        return "syntax"
    if line.startswith("skip"):
        return "error"
    return line.split()[0]


def ask_python(rows):
    """CPython's `re.sub`, in the driver's output shape.

    In-process, for the same reason python_diff.py is. `re` parses the
    template up front, the way this library does, so a bad template is
    "syntax" whether or not the pattern matched - which is the one thing
    that made the pcre arm of this file so hard to compare.
    """
    import re as _re
    out = []
    for flags, pattern, subject, template in rows:
        bits = 0
        for letter in flags:
            bits |= {"i": _re.IGNORECASE, "m": _re.MULTILINE,
                     "s": _re.DOTALL}.get(letter, 0)
        try:
            compiled = _re.compile(pattern, bits)
        except Exception:
            # "syntax" is what parse_driver() calls a refused *pattern*, so
            # that the harness's `rejected` counter sees it. A refused
            # *template* is "template" below. `re` raises the same exception
            # type for both, which is why the two have to be told apart here
            # rather than from the message.
            out.append("syntax")
            continue
        try:
            out.append(compiled.sub(template, subject))
        except Exception:
            # The driver spells its template refusal "template"; `re` has one
            # exception type for all of them. Folded to the driver's word so
            # that a refusal can be compared as agreement rather than read as
            # a disagreement about wording - the mistake posix_diff.py made
            # with `compile 42` against `compile`.
            out.append("template")
    return out


def ask_node(rows):
    payload = json.dumps([[f, p, s, t] for f, p, s, t in rows])
    finished = subprocess.run(["node", os.path.join(HERE, "node_replace.mjs")],
        input=payload, capture_output=True, text=True, check=True)
    return json.loads(finished.stdout)


VIM_SCRIPT = """
let lines = readfile(g:in)
let out = []
for l in lines
  let c = json_decode(l)
  try
    let r = substitute(c[1], c[0], c[2], 'g')
    let hex = ''
    for i in range(strlen(r))
      let hex .= printf('%02x', char2nr(r[i]))
    endfor
    call add(out, 'ok ' . hex)
  catch
    call add(out, 'syntax')
  endtry
endfor
call writefile(out, g:o)
qa!
"""


def ask_vim(rows, engine=0):
    """vim's answers: one process for the whole run, as vim_diff.py does.

    `substitute()` and not `:s`, for the reason vim_diff.py drives
    `matchstrpos()`: the subject here is a string, and `:s` works on a
    buffer. Over a string vim's `\r` is U+000D and its `\n` is U+000A,
    where in a buffer the first splits the line and the second writes a NUL
    - the same two characters seen through a different container.
    """
    work = tempfile.mkdtemp(prefix="replace_diff.")
    in_path = os.path.join(work, "cases.jsonl")
    out_path = os.path.join(work, "answers.txt")
    script_path = os.path.join(work, "run.vim")
    with open(script_path, "w", newline="\n") as handle:
        handle.write(VIM_SCRIPT)
    with open(in_path, "w", newline="\n") as handle:
        for _, pattern, subject, template in rows:
            handle.write(json.dumps([pattern, subject, template]) + "\n")
    command = ["vim", "-es", "-u", "NONE", "-i", "NONE",
        "--cmd", "let g:in=%s" % json.dumps(in_path),
        "--cmd", "let g:o=%s" % json.dumps(out_path)]
    if engine:
        # The old engine, for the reason vim_diff.py asks it: vim ships two
        # and they disagree, and a row where `set re=1` gives this
        # library's answer is one where this library picked one of vim's
        # answers rather than one where it is wrong.
        command += ["--cmd", "set re=%d" % engine]
    subprocess.run(command + ["-c", "source " + script_path],
        stdin=subprocess.DEVNULL,
        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=1800)
    if not os.path.exists(out_path):
        return []
    answers = []
    for line in open(out_path):
        line = line.rstrip("\n")
        if line.startswith("ok "):
            try:
                answers.append(bytes.fromhex(line[3:]).decode("utf-8"))
            except (ValueError, UnicodeDecodeError):
                answers.append("error")
        else:
            answers.append(line)
    return answers


def ask_library(driver, dialect, rows):
    lines = "".join("%s\t%s\t%s\t%s\n" % (
        flags, pattern.encode("utf-8").hex(), subject.encode("utf-8").hex(),
        template.encode("utf-8").hex())
        for flags, pattern, subject, template in rows)
    finished = subprocess.run([driver, dialect], input=lines,
        capture_output=True, text=True, check=True)
    return [parse_driver(line) for line in finished.stdout.splitlines()]


# "template" has no counterpart on the node side: ECMAScript has no
# ill-formed template, so a row where this library says `template` and node
# returns text is a real disagreement about the grammar. Against pcre2 both
# sides can say it and it compares as itself.
parse_ours = parse_driver


def splits_a_surrogate_pair(text):
    """Whether node's answer put a replacement inside a surrogate pair.

    The documented deviation, dialects.md section 6: ECMA-262 matches over
    UTF-16 code units and a zero-width assertion may therefore match between
    the halves of a surrogate pair, where this library's subject is code
    points and a match can begin and end only on a character boundary. A
    global replace visits every position, so every astral subject reaches it.

    Detected rather than assumed. When node replaces at such a position the
    two halves end up separated, and the result is a string holding unpaired
    surrogates - which is precisely a string that cannot be encoded as UTF-8.
    Nothing else produces one, so this cannot quietly cover anything but the
    deviation it names.
    """
    try:
        text.encode("utf-8")
    except UnicodeEncodeError:
        return True
    return False


# A template with no `$` in it and no surrogate in it either, so that a
# replacement node writes between the halves of a pair cannot be healed by
# the text the template reinserts. See heal_split_pairs below.
SURROGATE_PROBE = "\u00b7"


def heal_split_pairs(text, marker):
    """Take the marker back out from between the halves of every split pair.

    A high surrogate, the marker, and a low surrogate can only have come
    from a replacement written at a position inside a character, because
    nothing else in this file's alphabet produces an unpaired surrogate.

    The halves are then put back together, which is not cosmetic: node
    reports them as two lone surrogates and this library reports one
    character, and the two print the same because `json.dumps` spells an
    astral character as a surrogate pair either way. Re-encoding through
    UTF-16 is what makes the comparison ask about the text.
    """
    out = []
    index = 0
    while index < len(text):
        after = index + 1 + len(marker)
        if (0xD800 <= ord(text[index]) <= 0xDBFF
                and text[index + 1:after] == marker
                and after < len(text)
                and 0xDC00 <= ord(text[after]) <= 0xDFFF):
            out.append(text[index])
            index = after
            continue
        out.append(text[index])
        index += 1
    joined = "".join(out)
    try:
        return joined.encode("utf-16", "surrogatepass").decode("utf-16")
    except UnicodeDecodeError:
        return joined


TEMPLATE_GROUP = re.compile(r"\\[1-9]")


def is_abandoned_path_artifact(pattern, template):
    r"""vim keeps what a path it abandoned wrote; this library discards it.

    documentation/dialects.md section 6, item 2, and the two predicates in
    tools/oracle/vim_diff.py that count it there. A group written inside one
    of vim's postfix operators survives the branch that set it -
    `\(a\)\@>x\|\A` reports group one as "a" in vim where the branch died,
    and pcre2test and node both report it unset - and so do `\zs` and `\ze`:
    `\(a\zeb\)\@>\d\|\&` against "ab" is 0-2 there, wearing the end a branch
    that cannot match left behind.

    A replacement is where both of those become text. The group arrives
    through a template that names it, and the mark arrives through the span
    the substitution covers, so either is enough on its own - which is why
    this asks for the postfix operator and a path that can be abandoned
    first, and only then for one of the two ways the difference can be
    seen. The row this was written for, from seed 901 of the soak, is the
    negative-lookbehind spelling:
    `\%>2v\(a\(b\)\@=\)\@<!\(a\zsb\)\@=\V\m` with `\n\2\U\&`, where vim's
    `\2` holds the "b" its failed lookbehind captured and this library's is
    empty.

    The narrowing is real and worth saying: a defect of this library's in
    exactly that shape would not be reported here. The match differential
    still compares these patterns, against spans rather than text, with
    predicates of its own that are narrower again.
    """
    if "@" not in pattern:
        return False
    if "|" not in pattern and "@!" not in pattern and "@<!" not in pattern:
        return False
    return (TEMPLATE_GROUP.search(template) is not None
        or "\\zs" in pattern or "\\ze" in pattern)


def compare(dialect, driver, seed, patterns, templates, subjects, examples):
    """One dialect against its reference. None means the run was not made."""
    reference = None
    if dialect == "vim":
        if subprocess.run(["which", "vim"],
                capture_output=True).returncode != 0:
            print("vim: skipped (vim is not installed)")
            return 0
    if dialect == "pcre":
        pcre2 = find("pcre2_match")
        if not pcre2:
            print("pcre: skipped (no pcre2_match; run tools/corpus/fetch.sh "
                  "pcre2 and `make tools`)")
            return 0
        reference = pcre2

    rng = random.Random(seed)
    rows = []
    for i in range(patterns):
        # Half from the generator, half from the named list, so that the
        # named and high-numbered forms have something to refer to and the
        # rest still explores shapes nobody chose.
        if i % 2:
            pattern = make_pattern(dialect, rng)
        elif dialect == "python":
            pattern = rng.choice(PYTHON_NAMED_PATTERNS)
        elif dialect == "vim":
            pattern = rng.choice(VIM_NAMED_PATTERNS)
        else:
            pattern = rng.choice(NAMED_PATTERNS)
        flags = rng.choice(FLAG_SETS[dialect])
        for _ in range(templates):
            template = make_template(dialect, rng)
            for _ in range(subjects):
                subject = match_diff.make_subject(rng, "u" in flags)
                rows.append((flags, pattern, subject, template))

    if dialect == "ecmascript":
        # node never substitutes nothing and then reports a template error,
        # because ECMAScript has no template error to report; the count is
        # only needed where one side parses the template lazily.
        theirs = [(answer, 1) for answer in ask_node(rows)]
    elif dialect == "python":
        theirs = [(answer, 1) for answer in ask_python(rows)]
    elif dialect == "vim":
        theirs = [(answer, 1) for answer in ask_vim(rows)]
    else:
        theirs = ask_pcre2(reference, rows)
    mine = ask_library(driver, dialect, rows)
    if len(theirs) != len(rows) or len(mine) != len(rows):
        sys.stderr.write("%s: the drivers answered %d and %d of %d requests\n"
            % (dialect, len(theirs), len(mine), len(rows)))
        return None

    disagreements = []
    compared = 0
    rejected = 0
    declined = 0
    deviation = 0
    refused_here = 0
    lazy = 0
    defect = 0

    for (flags, pattern, subject, template), (them, count), us in \
            zip(rows, theirs, mine):
        if them == "error":
            declined += 1
            continue
        if them == "syntax":
            if dialect == "pcre" and perl_diff.reference_defect(
                    "pcre", pattern, "compile"):
                # pcre2 10.46 fails to compile a pattern holding both a
                # lookbehind and an extended class whose body uses an
                # operator. Found and characterised by perl_diff.py, recorded
                # in tools/corpus/VERSIONS, and excluded by the same
                # predicate here rather than by a second copy of it.
                defect += 1
                continue
            # Both must refuse the pattern. Counted separately so that a run
            # where nothing was ever rejected is visible: that is the half of
            # the comparison sed_diff.py and posix_diff.py were each blind to
            # in turn.
            rejected += 1
            compared += 1
            if us != "syntax":
                disagreements.append(
                    (flags, pattern, subject, template, them, us))
            continue
        if dialect == "vim" and us == "syntax" \
                and vim_diff.is_forward_reference_artifact(
                    pattern, "match", "compile"):
            # vim accepts a forward backreference when a lookbehind follows
            # it and refuses every other spelling of one, in both of its
            # engines - documentation/dialects.md section 6, item 1, and
            # `vim_diff.py`'s predicate rather than a second copy of it.
            # The rows reach here rather than the refusal arm above because
            # vim *compiled* the pattern: what it returns is the subject
            # unchanged.
            defect += 1
            continue
        if splits_a_surrogate_pair(them):
            deviation += 1
            continue
        if count == 0 and us == "template":
            # pcre2 parses a replacement template *lazily* - only when it has
            # a match to substitute into - so a malformed template against a
            # subject that does not match comes back as the subject
            # unchanged. This library parses it up front and reports the
            # error whether or not anything would have been replaced.
            #
            # A deliberate difference rather than a defect, and the count
            # from pcre2_substitute() is what makes it safe to say so: the
            # exclusion fires only where pcre2 made *no* substitution at all,
            # so a template this library wrongly rejects on a subject that
            # does match is still a disagreement. Recorded in dialects.md
            # section 5.11 and counted here.
            lazy += 1
            continue
        if us == "syntax" and perl_diff.library_deviation(
                pattern, "ok", "compile"):
            # A subroutine call to a group defined inside a non-atomic
            # lookbehind, which this library refuses rather than answering
            # backwards. perl_diff.py's predicate rather than a second copy
            # of it; documentation/dialects.md section 6.
            refused_here += 1
            continue
        compared += 1
        if us != them:
            disagreements.append((flags, pattern, subject, template, them, us))

    if dialect == "ecmascript" and disagreements:
        # The surrogate-pair deviation again, for the rows the check inside
        # the loop cannot see. That one reads node's answer and asks whether
        # it holds an unpaired surrogate, which is what a replacement
        # written between the halves of a pair leaves behind - but only
        # while the template writes something that keeps them apart. A
        # template that reinserts the text around the match puts the two
        # halves back together across the join: `\B.??` over "aab" U+1F600
        # "Abx" with `$'ab$`$&` heals into a string that encodes as UTF-8
        # like any other, and the witness for the deviation is gone.
        #
        # So ask again with a template that cannot heal - one character,
        # no `$`, no surrogate - and put the same rows to this library.
        # Take node's extra marker back out from between the halves it
        # split, and the two answers have to be equal. That is the
        # deviation stated as a property rather than guessed from a
        # spelling: the match sets agree except for the positions inside a
        # character, and a replacement is a function of the match set and
        # the template, so the row this exclusion covers is the row where
        # nothing else differs. Anything left over is a disagreement.
        astral = [index for index, row in enumerate(disagreements)
            if any(ord(character) > 0xFFFF for character in row[2])]
        if astral:
            probe = [(disagreements[index][0], disagreements[index][1],
                disagreements[index][2], SURROGATE_PROBE)
                for index in astral]
            theirs_probe = ask_node(probe)
            ours_probe = ask_library(driver, dialect, probe)
            healed = set()
            if len(theirs_probe) == len(probe) \
                    and len(ours_probe) == len(probe):
                for index, theirs_one, ours_one in zip(
                        astral, theirs_probe, ours_probe):
                    if heal_split_pairs(theirs_one, SURROGATE_PROBE) \
                            == ours_one:
                        healed.add(index)
            if healed:
                deviation += len(healed)
                compared -= len(healed)
                disagreements = [row for index, row
                    in enumerate(disagreements) if index not in healed]

    split = 0
    both_axes = []
    if dialect == "vim" and disagreements:
        # Every row left, put to vim's other engine. The two disagree about
        # where a `\zs` or a `\ze` inside an assertion counts, and this
        # library follows the one whose answers can be stated as a rule -
        # so a replacement built on those spans differs from the default
        # engine's and agrees with the old one's. Only the rows that came
        # back different are asked.
        older = ask_vim([(f, p, s, t) for f, p, s, t, _, _ in disagreements],
            engine=1)
        kept = []
        for row, old_answer in zip(disagreements, older):
            if old_answer == row[5]:
                split += 1
                compared -= 1
                continue
            if vim_diff.lookbehind_with_backreference(row[1]):
                # vim loses the capture a *successful* postfix lookbehind
                # made, so a `\\1` after it matches nothing there and the
                # substitution covers a shorter span. Section 6, item 4,
                # and the predicate is vim_diff.py's so that the two gates
                # ask one question.
                defect += 1
                compared -= 1
                continue
            if is_abandoned_path_artifact(row[1], row[3]):
                # vim keeps the captures and the marks a path it abandoned
                # wrote, and this library does not, so the group a template
                # names or the span it covers differs. Section 6, item 2.
                defect += 1
                compared -= 1
                continue
            if old_answer != row[4]:
                # Two axes at once, which is the third category vim_diff.py
                # keeps and this gate did not. Most split rows turn on one
                # axis and `set re=1` gives exactly this library's answer;
                # a pattern touching two of them agrees with neither
                # engine, because this library follows the old one where a
                # mark sits inside an assertion or an atomic group and the
                # new one elsewhere - each because that engine's answer is
                # the one that can be stated as a rule.
                # `\(\w\+\)\@2<=\(a\zsb\)\@>\v` over "BbBab0A" is
                # both: the `\zs` inside `\@>` counts here and under
                # `re=1` and is inert under `re=2`, and the bounded
                # lookbehind reaches two characters back here and under
                # `re=2` and one under `re=1`. Printed rather than
                # swallowed - a defect of this library's could hide among
                # them, and the only defence is that a person can see them.
                both_axes.append(row + (old_answer,))
                compared -= 1
                continue
            kept.append(row)
        disagreements = kept

    for flags, pattern, subject, template, them, us in \
            disagreements[:examples]:
        print("  /%s/%s  %s on %s" % (pattern, flags, json.dumps(template),
            json.dumps(subject)))
        print("      reference: %s" % json.dumps(them))
        print("      ours:      %s" % json.dumps(us))
    for flags, pattern, subject, template, them, us, older in \
            both_axes[:examples]:
        print("  [both axes] /%s/%s  %s on %s"
            % (pattern, flags, json.dumps(template), json.dumps(subject)))
        print("      re=2: %s" % json.dumps(them))
        print("      re=1: %s" % json.dumps(older))
        print("      ours: %s" % json.dumps(us))

    print("%-11s %d rows, %d compared, %d the pattern was rejected, "
          "%d the reference declined, %d the surrogate-pair deviation, "
          "%d the template parsed up front, %d a known reference defect, "
          "%d this library refuses on purpose, "
          "%d vim's two engines disagree, "
          "%d vim's two engines disagree and neither gives ours, "
          "%d disagreements"
          % (dialect + ":", len(rows), compared, rejected, declined,
             deviation, lazy, defect, refused_here, split, len(both_axes),
             len(disagreements)))
    if not rejected:
        sys.stderr.write(
            "%s: no generated pattern was rejected, so the accept/reject "
            "half was never asked\n" % dialect)
        return None
    return len(disagreements)


def main(argv):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--seed", type=int, default=1)
    parser.add_argument("--patterns", type=int, default=300)
    parser.add_argument("--templates", type=int, default=8)
    parser.add_argument("--subjects", type=int, default=4)
    parser.add_argument("--examples", type=int, default=10)
    parser.add_argument("--dialect", default="all",
        help="ecmascript, pcre, or all")
    args = parser.parse_args(argv[1:])

    driver = find("grx_replace")
    if not driver:
        sys.stderr.write("run `make tools` first\n")
        return 2

    dialects = ("ecmascript", "pcre", "python", "vim") \
        if args.dialect == "all" else (args.dialect,)
    total = 0
    for dialect in dialects:
        found = compare(dialect, driver, args.seed, args.patterns,
            args.templates, args.subjects, args.examples)
        if found is None:
            return 2
        total += found
    return 1 if total else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
