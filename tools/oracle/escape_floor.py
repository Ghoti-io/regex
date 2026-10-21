#!/usr/bin/env python3
r"""Fail a differential whose vocabulary has fallen behind its front end.

Every differential here generates patterns from a hand-written list of
atoms. A hand-written list falls behind the code silently: an escape the
front end starts accepting is compiled from that moment and asked about by
nothing, and the gate goes on reporting zero disagreements over a construct
it never spells. That has happened repeatedly - `java_syntax_diff.py` read
one area and so asked 9 of 624 rows, `java_match_diff.py` five areas and so
reached 109 of 370, and the ECMAScript and Python vocabularies between them
left twenty-three escapes with no subject-bearing case at all.

**An accept-or-reject sweep cannot stand in for this.** `\v` compiles in
every Perl-family dialect and means the vertical tab in some and five code
points of vertical space in others; .NET's was the wide reading while its
own alphabet sweep reported agreement, because both sides accepted the
letter. Only a row with a *subject* separates the two.

So the floor is: for each dialect, every escape the front end accepts must
appear in the vocabulary with a subject - bare and inside a class counted
apart, because those are different branches of the parser and `\b` is a
boundary in one and a backspace in the other.

**Accepting a letter is not the same as having a construct for it.** Asked
which letters it accepts, the ECMAScript front end answers all fifty-two,
vim fifty-one and the GNU dialects forty-eight - because in those an escape
the grammar does not know is the letter itself, which is Annex B's
IdentityEscape and vim's fall-through. `\q` meaning "q" is one rule and not
fifty-two, so a floor built on acceptance would demand a row for every
letter in half the dialects and say nothing about any of them.

What the floor wants is the letters that are *constructs*, and that is a
semantic question with a semantic answer: a letter is a construct here when
`\X` behaves differently from the bare literal `X`. That is measured by
running both and comparing, over subjects chosen to separate a class from a
character and an anchor from either. The closed-alphabet dialects come out
around twenty to thirty-nine letters and the open ones drop from fifty-two
to the handful they really define, which is the list worth insisting on.

**Why the construct list is measured rather than read.** `python_diff.py` and
`match_diff.py` parse their accept lists out of the C, which works only
where the front end happens to have a closed table to parse; perl's gate is
a large switch and vim's is a prefix grammar, and a floor that can only read
some dialects would quietly cover only those. Running the dialect instead
works for all of them and cannot drift from the source, because it *is* the
source running.

**Why that is not circular.** This asks the library what it accepts in order
to insist that everything it accepts has its *meaning* compared against a
reference. It does not use the library to decide whether accepting was
right: that question belongs to the accept-or-reject sweeps
(`syntax_diff.py` and the probes' `escape-alphabet` areas), which compare
the same list against a reference. If the front end wrongly accepts a
letter, this floor forces a semantics row for it and the reference's refusal
then shows up as a disagreement there - so a wrong accept list makes this
floor louder, never quieter.

Copyright 2026 by Corey Pennycuff
"""

import os
import subprocess
import string

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))

LETTERS = string.ascii_letters

# Escapes whose argument a bare letter cannot carry, so that the probe asks
# about the construct rather than about a missing operand. A letter absent
# here is probed as itself.
ARGUMENT = {
    "c": "A", "x": "41", "u": "0041", "p": "{L}", "P": "{L}",
    "k": "<n>", "N": "{BULLET}", "Q": "a\\E", "o": "{101}", "g": "{1}",
}
# A prefix some dialects need before the escape is legal at all.
PREFIX = {"k": "(?<n>a)"}


def find(name):
    for platform in ("linux", "mac", "win64", "win32"):
        for build in ("release", "debug"):
            path = os.path.join(ROOT, "build", platform, build, "apps",
                "tools", name)
            if os.path.exists(path):
                return path
    return None


def spelling(letter, in_class, backslash=True):
    r"""One probe pattern for a letter, with the backslash or without it.

    The pair differs by the backslash **and nothing else**, which is the
    whole of what makes the comparison mean anything. Comparing `\o{101}`
    against a bare `o` instead would differ for a structural reason - the
    braces are still there in one and not the other - and the first version
    of this did exactly that, reporting `\Q`, `\c`, `\o` and `\u` as
    constructs of dialects that have none of them.
    """
    body = ("\\" if backslash else "") + letter + ARGUMENT.get(letter, "")
    prefix = PREFIX.get(letter, "")
    return "%s[%s]" % (prefix, body) if in_class else prefix + body


# Subjects that separate a construct from a literal. The letter itself is
# first: an identity escape matches it and almost nothing else does. The rest
# divide a class from a character, an anchor from both, and the ASCII reading
# of a shorthand from the Unicode one.
PROBE_SUBJECTS = ("", "a", "A", "z", "Z", "0", "9", "_", "-", " ", "\t",
                  "\n", "\r", "\x0b", "\x0c", "\x07", "\x1b", "\x01", "\x00",
                  "ab", "a b", "é", "١", "\u0085", " ", "\b")


def constructs(dialect, driver=None, flags=""):
    """The letters this dialect gives a meaning other than the letter itself.

    A letter is a construct when `\\X` answers some subject differently from
    the bare literal `X`, bare and inside a class asked apart. A letter whose
    two spellings agree everywhere is an identity escape - Annex B's rule,
    and vim's fall-through - and needs no row of its own.

    A letter neither spelling compiles is not a construct either: it is a
    refusal, which is the accept-or-reject sweeps' business and not this
    floor's.
    """
    driver = driver or find("grx_match")
    if not driver:
        raise SystemExit("escape_floor: grx_match not built; run `make tools`")
    rows = []
    for in_class in (0, 1):
        for letter in LETTERS:
            escaped = spelling(letter, in_class)
            plain = spelling(letter, in_class, backslash=False)
            for subject in PROBE_SUBJECTS:
                rows.append((letter, in_class, escaped, subject))
                rows.append((letter, in_class, plain, subject))
    payload = "".join("%s\t%s\t%s\n" % (flags, p.encode().hex(),
                                        s.encode().hex())
                      for _, _, p, s in rows)
    result = subprocess.run([driver, dialect], input=payload, text=True,
                            capture_output=True, check=True)
    answers = result.stdout.strip().split("\n")
    if len(answers) != len(rows):
        raise SystemExit("escape_floor: %s answered %d of %d rows"
                         % (dialect, len(answers), len(rows)))
    # The engine name is in a match line and is not part of the answer.
    def verdict(line):
        parts = line.split()
        return parts[:1] + parts[2:] if parts[:1] == ["match"] else parts

    bare, in_class_set = set(), set()
    compiled_any = False
    for i in range(0, len(rows), 2):
        letter, inside, _, _ = rows[i]
        escaped_answer = verdict(answers[i])
        plain_answer = verdict(answers[i + 1])
        if escaped_answer[:1] == ["compile"]:
            continue              # a refusal, not a construct
        compiled_any = True
        if escaped_answer != plain_answer:
            (in_class_set if inside else bare).add(letter)
    # **No constructs is a legitimate answer**, and strict POSIX gives it:
    # `\w` there is the letter, `\<` and `\>` are GNU's, and the
    # backreferences are digits. So the guard is that the probe reached the
    # parser at all - some spelling compiled - rather than that it found
    # something. A floor that insisted on a construct would have to be
    # switched off for the two dialects whose answer is honestly zero, and a
    # check with an exception for the awkward cases is not a check.
    if not compiled_any:
        raise SystemExit("escape_floor: %s compiled none of the %d probe "
                         "patterns, so the floor never reached the parser"
                         % (dialect, len(rows)))
    return bare, in_class_set


def spelled(*lists):
    """The escape letters a vocabulary spells, bare and inside a class.

    A class atom does not answer the bare question: a `[\\v]` satisfying it
    is the defect this separation exists to catch, and writing it the other
    way round left a removed bare `\\v` undetected.

    Takes several lists because a vocabulary is not always one: the
    ECMAScript one keeps `\\b` and `\\B` in its anchor list rather than its
    atoms, and a floor reading only the atoms reported both as missing.
    """
    import re
    bare, classed = set(), set()
    for atom in [atom for atoms in lists for atom in atoms]:
        for letter in re.findall(r"\\([A-Za-z])",
                                 re.sub(r"\[[^]]*\]", "", atom)):
            bare.add(letter)
        for letter in re.findall(r"\[[^]]*?\\([A-Za-z])", atom):
            classed.add(letter)
    return bare, classed


def check(dialect, *lists, driver=None, flags="", exempt=()):
    """Raise unless the vocabulary spells every construct `dialect` has.

    `exempt` is for a letter whose meaning genuinely cannot be asked through
    this harness. Each one needs a reason beside it where it is passed: an
    exemption with no reason is how a floor stops holding.
    """
    want_bare, want_class = constructs(dialect, driver, flags)
    have_bare, have_class = spelled(*lists)
    exempt = set(exempt)
    problems = []
    for where, want, have in (("", want_bare, have_bare),
                              (" inside a class", want_class, have_class)):
        missing = sorted((want - have) - exempt)
        if missing:
            problems.append("%s spells no subject-bearing case%s for %s"
                            % (dialect, where,
                               " ".join("\\" + c for c in missing)))
    if problems:
        raise SystemExit(
            "escape_floor: " + "; ".join(problems)
            + ". Each is a letter this front end gives a meaning other than "
              "itself and no generated pattern asks the meaning of. Add an "
              "atom for it, or exempt it with a reason - do not widen the "
              "exemption silently.")
    return len(want_bare), len(want_class)
