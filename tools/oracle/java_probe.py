#!/usr/bin/env python3
"""Reproducible JDK 25/27 semantic matrix; never consults libregex.

Use --record only when intentionally refreshing a pin's answers. The normal
strict gate compares every answer with the committed version-specific file.
"""
import argparse
import json
import os
import subprocess
import sys

import oracle_env
from probe import CASES as SHARED

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
DATA = os.path.join(ROOT, "tests", "data", "probe")


def case(area, pattern, subject="", flags="", mode="first", argument=""):
    return dict(area=area, pattern=pattern, subject=subject, flags=flags,
                mode=mode, argument=argument)


CASES = [case("shared", p, s, f) for p, f, s, _ in SHARED] + [
    case("control", "a", "a"), case("control", "a", "b"),
    case("control", "(", ""), case("control", "(a)?b", "b"),
    case("control", ".", "😀"),
    case("control", "a", "😀a", mode="find-from", argument="4"),
    case("flags", "a", "A", "i"), case("flags", "é", "É", "iu"),
    case("flags", "(?U)\\w", "é"), case("flags", "\\w", "é"),
    case("flags", "(?d).", "\r"), case("flags", "(?s).", "\n"),
    case("flags", "(?m)^b", "a\nb"), case("flags", "(?x)a # hi\n b", "ab"),
    case("flags", "(?i:a)b", "Ab"), case("flags", "(?i:a)b", "AB"),
    case("flags", "a", "A", "l"), case("flags", "é", "e\u0301", "c"),
    case("classes", "[a&&[a-z]]", "a"), case("classes", "[a&&[^a]]", "a"),
    case("classes", "[[a-z][0-9]]", "5"), case("classes", "[a-z&&[^aeiou]]", "b"),
    case("classes", "[a-z&&[^aeiou]]", "a"), case("classes", "[[:alpha:]]", "a"),
    case("classes", "[a-\\d]", "a"), case("classes", "[]]", "]"),
    case("classes", "[-a]", "-"), case("classes", "[a-]", "-"),
    case("lookbehind", "(?<=ab)c", "abc"),
    case("lookbehind", "(?<=a|ab)c", "abc"),
    case("lookbehind", "(?<=a{1,3})b", "aaab"),
    case("lookbehind", "(?<=a+)b", "aab"),
    case("lookbehind", "(?<=(a)\\1)b", "aab"),
    case("captures", "((a)|b)+", "ab"),
    case("captures", "(a)?b\\1", "b"),
    case("captures", "(?=(a))a", "a"),
    case("captures", "\\2(a)(b)", "ab"),
    case("captures", "(?<n>a)\\k<n>", "aa"),
    case("iteration", "", "ab", mode="all"),
    case("iteration", "a*", "ba", mode="all"),
    case("iteration", "\\G.", "abc", mode="all"),
    case("iteration", "a", "ba", mode="find-from", argument="1"),
    case("iteration", "a", "a", mode="matches"),
    case("iteration", "a", "ab", mode="looking-at"),
    case("escapes", "\\Q[a]+\\E", "[a]+"),
    case("escapes", "\\x{1F600}", "😀"),
    case("escapes", "\\u00E9", "é"),
    case("escapes", "\\0101", "A"),
    case("escapes", "\\cA", "\u0001"),
    case("escapes", "\\h", "\u00a0"),
    case("escapes", "\\v", "\n"),
    case("escapes", "\\R", "\r\n"),
    case("escapes", "\\X", "e\u0301"),
    case("escapes", "\\b{g}", "e\u0301"),
    case("unicode-sensitive", "\\p{IsGreek}", "Ω"),
    case("unicode", "\\p{InGreek}", "Ω"),
    case("unicode", "\\p{javaLowerCase}", "é"),
    case("unicode", "\\p{NotAProperty}", "a"),
    case("unicode", "\\d", "١"),
    case("unicode", "(?U)\\d", "١"),
    case("unicode", "(?iu)k", "K"),
    case("replacement", "(a)", "a", mode="replace-all", argument="$1$1"),
    case("replacement", "(?<x>a)", "a", mode="replace-first", argument="${x}"),
    case("replacement", "(a)?b", "b", mode="replace-all", argument="<$1>"),
    case("replacement", "(a)", "a", mode="replace-all", argument="$2"),
    case("replacement", "a", "a", mode="replace-all", argument="\\$"),
    case("replacement", "a", "a", mode="replace-all", argument="$"),
    case("replacement", "", "é", mode="replace-all", argument="X"),
    case("split", ",", "a,b,", mode="split", argument="0"),
    case("split", ",", "a,b,", mode="split", argument="-1"),
    case("split", ",", "a,b,", mode="split", argument="2"),
    case("split", "(?=a)", "abc", mode="split", argument="0"),
    case("split", "(,)", "a,b", mode="split", argument="0"),
    case("split", ",", "", mode="split", argument="-1"),
    case("refusal", "(?R)", "a"),
    case("refusal", "(?(1)a|b)", "a"),
    case("refusal", "(*SKIP)", "a"),
    case("refusal", "\\g{1}", "a"),
    case("refusal", "(?P<n>a)", "a"),
    case("lines", ".", "\n"), case("lines", ".", "\u0085"),
    case("lines", ".", "\u2028"), case("lines", ".", "\u2029"),
    case("lines", "\\R", "\r\n"), case("lines", "\\R", "\u0085"),
    case("lines", "(?d).", "\u0085"),
    case("lines", "(?m)^b", "a\rb"),
    case("lines", "(?dm)^b", "a\rb"),
    case("flags", "(?x)[a b]", " "),
    case("flags", "(?x)[a b]", "b"),
    case("flags", "(?-i:A)", "a", "i"),
    case("replacement", "(a)(b)", "ab", mode="replace-all", argument="$12"),
    case("replacement", "(a)(b)", "ab", mode="replace-all", argument="${missing}"),
    case("split", "", "ab", mode="split", argument="-1"),
    case("split", "$", "ab", mode="split", argument="-1"),
    case("split", ",", ",a,", mode="split", argument="0"),
    case("split", ",", ",a,", mode="split", argument="-1"),
    case("control", "a+", mode="compile"),
    case("classes", "[[:alpha:]]", "Z"),
    case("flags", "a+", "a+", "l"),
    case("iteration", "\\Ga", "aba", mode="all"),
    case("replacement", "a", "aa", mode="replace-first", argument="<$0>"),
    case("replacement", "a", "a", mode="replace-all", argument="<$0>"),
    case("flags", ".", "\r", "d"),
    case("flags", "^b", "a\nb", "m"),
    case("flags", ".", "\n", "s"),
    case("flags", "a # hi\n b", "ab", "x"),
    case("flags", "\\w", "é", "U"),
    case("control", "😀(", mode="compile"),
    case("replacement", "a", "a", mode="replace-first", argument="$"),
    # Flag interactions and Character predicates, with separate JDK 25/27
    # answers in the two committed records.
    case("flags", "k", "K", "i"),
    case("flags", "k", "K", "iu"),
    case("flags", "k", "K", "iU"),
    case("flags", "k", "K", "U"),
    case("flags", "(?-u:k)", "K", "iu"),
    case("flags", "(?u:k)", "K", "i"),
    case("flags", "\\d", "١"),
    case("flags", "\\d", "١", "U"),
    case("flags", "[\\w]", "é"),
    case("flags", "[\\w]", "é", "U"),
    case("flags", "\\bé", "é"),
    case("flags", "\\Bé", "é"),
    case("flags", "\\bé", "é", "U"),
    case("flags", "\\Bé", "é", "U"),
    case("lines", "a.b", "a\rb", "d"),
    case("lines", "^b", "a\rb", "md"),
    case("lines", "^\\n", "\r\n", "m"),
    case("lines", "(?d:a.\\nb)", "a\r\nb"),
    case("lines", "(?-d:a.\\nb)", "a\r\nb", "d"),
    case("flags", "a b", "ab", "x"),
    case("flags", "[a b]", "b", "x"),
    case("unicode-sensitive", "\\p{IsGreek}", "ʹ"),
    case("unicode-sensitive", "\\p{sc=Greek}", "ʹ"),
    case("unicode-sensitive", "\\p{sc=Greek}", "ἀ"),
    case("unicode-sensitive", "\\p{InGreek}", "ʹ"),
    case("unicode-sensitive", "\\p{javaWhitespace}", "\u001c"),
    case("unicode-sensitive", "\\p{javaWhitespace}", "\u00a0"),
    case("unicode-sensitive", "\\p{javaWhitespace}", "\u0085"),
    case("unicode-sensitive", "\\p{javaWhitespace}", "\u2003"),
    case("unicode-sensitive", "\\p{javaWhitespace}", "\u2007"),
    case("unicode-sensitive", "\\p{javaWhitespace}", "\u202f"),
    case("flags", "(k)", "K", "iu"),
    case("flags", "(a).(b)", "a\rb", "d"),
    case("unicode-sensitive", "(\\p{javaWhitespace})", "\u2003"),
    case("unicode-sensitive", "(?U)(\\w+)", "é"),
    case("unicode-sensitive", "\\p{javaLowerCase}", "ʕ"),
    case("flags", "𐐀", "𐐨", "i"),
    case("flags", "𐐀", "𐐨", "iu"),
    case("flags", "\\w", "𐐨"),
    case("flags", "\\w", "𐐨", "U"),
    case("unicode-sensitive", "\\p{javaLowerCase}", "𐐨"),
    case("unicode-sensitive", "\\p{javaDigit}", "𝟎"),
    # Search-state matrix. Arguments and all answer spans are UTF-8 bytes.
    case("search", "", "ab", mode="all"),
    case("search", "a*", "baac", mode="all"),
    case("search", "a*", "a", mode="all"),
    case("search", "\\Ga*", "baac", mode="all"),
    case("search", "\\Ga*", "aab", mode="all"),
    case("search", "(?:\\Ga|b)", "cba", mode="all"),
    case("search", "(?:\\Ga|b)", "aba", mode="all"),
    case("search", "(?=(a))a|(b)", "ab", mode="all"),
    case("search", "((a)|b)+", "ab", mode="all"),
    case("search", "(a*)*", "bc", mode="all"),
    case("search", "(?<=(a+))b", "aaabaaab", mode="all"),
    case("search", "(?<=a{1,3})(b)", "aabaaab", mode="all"),
    case("search", "(?=(a))(a)", "aa", mode="all"),
    case("search", "(?<=a(?=(b)))b", "abab", mode="all"),
    case("search", "(?<!a{2,})b", "ab aab aaab b", mode="all"),
    case("search", "\\Ga*", "cbaa", mode="all-from", argument="1"),
    case("search", "", "😀é", mode="all"),
    case("search", ".", "😀é", mode="all"),
    case("search", "(?<=😀)(é)", "😀é", mode="first"),
    case("search", "\\G.", "abc", mode="find-from", argument="1"),
    case("search", "(?<=😀)a", "😀a", mode="find-from", argument="4"),
    case("search", "a", "ba", mode="find-from", argument="2"),
    case("search", "a+", "aa", mode="matches"),
    case("search", "a", "aa", mode="matches"),
    case("search", "a", "ab", mode="looking-at"),
    case("search", "a", "ba", mode="looking-at"),
    case("search", "(?=a)(a*)", "a", mode="matches"),
    case("search", "(?:)", "", mode="matches"),
    case("search", "(?=a)a", "a", mode="matches"),
    case("search", "(?=a)(a*)", "ab", mode="looking-at"),
    case("search", "(?=a)", "abc", mode="looking-at"),
    case("search", "(?<=a)b", "ab", mode="looking-at"),
    # Region and bounds are measured, then classified separately from the
    # current C search window (which has intentionally different semantics).
    case("region", "^a", "ba", mode="region-first", argument="1,2,0,1"),
    case("region", "^a", "ba", mode="region-first", argument="1,2,0,0"),
    case("region", "(?<=b)a", "ba", mode="region-first", argument="1,2,0,1"),
    case("region", "(?<=b)a", "ba", mode="region-first", argument="1,2,1,1"),
    case("region", "a$", "ab", mode="region-first", argument="0,1,0,1"),
    case("region", "a", "ba", mode="region-matches", argument="1,2,0,1"),
    case("region", "a", "ba", mode="region-looking-at", argument="1,2,0,1"),
    # Replacement grammar and progression, with the JDK 25 record fixed as
    # the baseline and JDK 27 held in a separate file.
    case("replacement", "(a)", "a", mode="replace-all", argument="$0/$1/$12"),
    case("replacement", "(a)(b)", "ab", mode="replace-all", argument="$12"),
    case("replacement", "(a)", "a", mode="replace-all", argument="$01"),
    case("replacement", "(a)", "a", mode="replace-all", argument="$00"),
    case("replacement", "(a)", "a", mode="replace-all", argument="$123"),
    case("replacement", "(a)(b)(c)(d)(e)(f)(g)(h)(i)(j)(k)(l)",
         "abcdefghijkl", mode="replace-all", argument="$123"),
    case("replacement", "(?<x>a)", "a", mode="replace-all", argument="${x}$1"),
    case("replacement", "(a)?b", "b", mode="replace-all", argument="<$1>"),
    case("replacement", "a", "a", mode="replace-all", argument="\\$\\\\\\q"),
    case("replacement", "a", "a", mode="replace-all", argument="\\"),
    case("replacement", "a", "a", mode="replace-all", argument="$"),
    case("replacement", "a", "a", mode="replace-all", argument="$x"),
    case("replacement", "(a)", "a", mode="replace-all", argument="$2"),
    case("replacement", "(a)", "a", mode="replace-all", argument="${missing}"),
    case("replacement", "(a)", "a", mode="replace-all", argument="${}"),
    case("replacement", "(a)", "a", mode="replace-all", argument="${1}"),
    case("replacement", "(?<x>a)", "a", mode="replace-all", argument="${x\0}"),
    case("replacement", "(a)", "a", mode="replace-all", argument="${x"),
    case("replacement", "a", "bbb", mode="replace-all", argument="$"),
    case("replacement", "a", "bbb", mode="replace-first", argument="$"),
    case("replacement", "a", "bbb", mode="replace-all", argument="\\"),
    case("replacement", "a", "bbb", mode="replace-first", argument="\\"),
    case("replacement", "a", "bbb", mode="replace-all", argument="${x"),
    case("replacement", "a", "bbb", mode="replace-first", argument="${}"),
    case("replacement", "a", "aa", mode="replace-first", argument="<$0>"),
    case("replacement", "a", "aa", mode="replace-all", argument="<$0>"),
    case("replacement", "", "é", mode="replace-all", argument="X"),
    case("replacement", "(é)", "é", mode="replace-all", argument="<$1>"),
    case("replacement", "(😀)", "😀😀", mode="replace-first", argument="<$1>"),
    case("replacement", "(😀)", "😀😀", mode="replace-all", argument="<$1>"),
    # Java finds an empty match between the UTF-16 halves of this scalar and
    # writes an unpaired-surrogate result; the byte API cannot represent it.
    case("replacement", "", "😀", mode="replace-all", argument="X"),
    case("replacement", "😀", "😀", mode="replace-all", argument="\\😀"),
    case("replacement", "a", "a\0a", mode="replace-all", argument="X"),
    case("replacement", "a", "a", mode="replace-all-literal", argument="$0\\q"),
    case("replacement", "a", "aa", mode="replace-first-literal", argument="$0\\q"),
]


def run(pin, rows):
    command = oracle_env.command(pin, ["java", "tools/oracle/JavaOracle.java"])
    lines = []
    for row in rows:
        values = [row["mode"]] + [row[k].encode("utf-8").hex()
            for k in ("pattern", "flags", "subject", "argument")]
        lines.append("\t".join(values))
    finished = subprocess.run(command, input="\n".join(lines) + "\n",
                              capture_output=True, text=True, timeout=180)
    if finished.returncode:
        raise RuntimeError(f"driver failed: {finished.stderr}")
    answers = []
    for line in finished.stdout.splitlines():
        state, sep, payload = line.partition("\t")
        if not sep or state not in ("ok", "error"):
            raise RuntimeError(f"bad driver response: {line!r}")
        answer = json.loads(bytes.fromhex(payload).decode("utf-8"))
        if state == "error":
            answer["driver_error"] = True
        answers.append(answer)
    if len(answers) != len(rows):
        raise RuntimeError(f"{len(answers)} answers for {len(rows)} cases")
    return answers


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--pin", choices=("java25", "java27"), required=True)
    parser.add_argument("--record", action="store_true")
    parser.add_argument("--wrong-expected-control", action="store_true")
    args = parser.parse_args()
    if args.record and (oracle_env.MODE != "container"
                        or os.environ.get("GHOTI_ORACLE_ALIAS", "").strip()):
        raise RuntimeError("recording requires pinned container mode without aliases")
    print(oracle_env.provenance([args.pin]), flush=True)
    answers = run(args.pin, CASES)
    path = os.path.join(DATA, args.pin + ".json")
    payload = {"cases": CASES, "answers": answers}
    if args.record:
        os.makedirs(DATA, exist_ok=True)
        with open(path, "w", encoding="utf-8") as out:
            json.dump(payload, out, ensure_ascii=False, indent=2)
            out.write("\n")
        print(f"recorded {len(answers)} answers in {path}")
        return 0
    with open(path, encoding="utf-8") as source:
        expected = json.load(source)
    if args.wrong_expected_control:
        expected["answers"][0] = {"kind": "planted-wrong-answer"}
    if expected["cases"] != CASES:
        raise RuntimeError("case inventory changed; record and review both pins")
    wrong = [(i, actual, wanted) for i, (actual, wanted) in enumerate(
        zip(answers, expected["answers"])) if actual != wanted]
    if len(expected["answers"]) != len(answers):
        raise RuntimeError("answer count changed")
    for i, actual, wanted in wrong[:10]:
        print(f"case {i} {CASES[i]}: {actual} != {wanted}", file=sys.stderr)
    print(f"{len(answers)} cases, {len(answers)-len(wrong)} agree, {len(wrong)} differ")
    return bool(wrong)


if __name__ == "__main__":
    sys.exit(main())
