#!/usr/bin/env python3
# SPDX-License-Identifier: LGPL-3.0-only
#
# Fail if a fuzz harness's corpus directory cannot hold a committed seed.
#
# tests/fuzz/corpus/.gitignore ignores `*`, because the corpus grows to tens of
# thousands of files as the fuzzer runs and none of those belong in the
# repository. The hand-written seeds are re-included by `!*.seed`. The subtlety
# is that `*` excludes the per-harness DIRECTORIES too, and git does not look
# inside an excluded directory - so a negation on a file under one never gets
# the chance to match. A `.seed` placed there is invisible: `git add` on it is
# refused, `git status` stays clean, and the fixture is simply absent.
#
# That is not hypothetical. The rule re-included `pattern/` by name, which was
# the only harness with seeds when it was written, so `crossengine/` and
# `subject/` silently dropped anything put in them. The fix is `!*/`, and this
# gate is what keeps it true.
#
# The harness list comes from tests/fuzz/fuzz_*.cpp rather than from the corpus
# directories on disk, because the case this is guarding against is a NEW
# harness: its directory may not exist yet, and if the check enumerated
# directories it would have nothing to say about the one that matters.
#
# Both directions are asserted. Un-ignoring everything would satisfy "a seed is
# visible" while committing 122 Mb of grown corpus, so the gate also requires
# that a non-seed file stays ignored.

import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FUZZ_DIR = ROOT / "tests" / "fuzz"
CORPUS = FUZZ_DIR / "corpus"


def is_ignored(path):
    """True if git would ignore `path`. The path need not exist.

    `git check-ignore -q` exits 0 when the path is ignored and 1 when it is
    not. Do NOT use -v here: -v exits 0 whenever any pattern MATCHED, and a
    negation such as `!*.seed` is a match, so -v reports 0 for both answers.
    """
    r = subprocess.run(
        ["git", "check-ignore", "-q", str(path.relative_to(ROOT))],
        cwd=ROOT,
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    return r.returncode == 0


def main():
    if not (ROOT / ".git").exists():
        print("check-corpus-seeds: skipped (not a git checkout)")
        return 0

    try:
        subprocess.run(
            ["git", "rev-parse", "--is-inside-work-tree"],
            cwd=ROOT,
            stdout=subprocess.DEVNULL,
            stderr=subprocess.DEVNULL,
            check=True,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        print("check-corpus-seeds: skipped (no usable git)")
        return 0

    harnesses = sorted(p.name[len("fuzz_"):-len(".cpp")]
                       for p in FUZZ_DIR.glob("fuzz_*.cpp"))
    if not harnesses:
        print("check-corpus-seeds: no fuzz harnesses found in %s" % FUZZ_DIR,
              file=sys.stderr)
        return 1

    problems = []
    for name in harnesses:
        seed = CORPUS / name / "check-corpus-seeds-probe.seed"
        grown = CORPUS / name / "check-corpus-seeds-probe.bin"
        if is_ignored(seed):
            problems.append(
                "%s/: a .seed file here is IGNORED, so it cannot be committed"
                % name)
        if not is_ignored(grown):
            problems.append(
                "%s/: a grown-corpus file here is NOT ignored, so a fuzzer "
                "run would offer tens of thousands of files to commit" % name)

    if problems:
        sys.stderr.write("\n\033[0;31m### A fuzz corpus directory is wrong ###\033[0m\n\n")
        for p in problems:
            sys.stderr.write("  %s\n" % p)
        sys.stderr.write(
            "\ntests/fuzz/corpus/.gitignore must ignore the grown corpus and\n"
            "re-include the seeds. `*` excludes the per-harness directories\n"
            "themselves and git will not descend into an excluded directory,\n"
            "so the rules need `!*/` before `!*.seed` can ever match:\n\n"
            "    *\n"
            "    !.gitignore\n"
            "    !*/\n"
            "    !*.seed\n\n")
        return 1

    print("\033[0;32mAll %d fuzz corpus directories admit a seed and ignore "
          "the grown corpus.\033[0m" % len(harnesses))
    return 0


if __name__ == "__main__":
    sys.exit(main())
