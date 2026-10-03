#!/usr/bin/env python3
"""Fail if documentation/development.md's layout block disagrees with the tree.

That page's whole job is to say where the code is, and a directory that
arrives after it was written does not disturb it: the block still renders, and
every path in it still resolves. So the page goes stale in the one way that
reads as correct, and it had - `examples/`, `pkgconfig/`, four directories
under `tools/` and two under `tests/data/` were all missing, along with twelve
gate scripts, by the time anybody compared the two.

Both directions, because each catches a different mistake. A directory in the
tree that the block does not reach is work the page has stopped describing; a
path in the block that is not in the tree is a page describing a layout that
no longer exists, which is worse, since a reader follows it.

Coverage is by prefix: `tools/oracle/` in the block covers
`tools/oracle/containers/` under it, because the page is a map and not an
inventory. A directory is in the denominator when git tracks a file somewhere
inside it, which is what makes generated output and __pycache__ invisible here
without a list of exceptions to maintain.
"""

import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PAGE = "documentation/development.md"

# Where the code is. A directory outside these is not the layout page's
# business - the root files are named one by one in the block, and build
# output is not tracked.
ROOTS = ("include", "src", "tests", "tools", "examples", "pkgconfig")


def block_entries():
    """The left column of the fenced block under `## Layout`."""
    text = open(os.path.join(ROOT, PAGE), encoding="utf-8").read()
    heading = re.search(r"^## Layout\s*$", text, re.MULTILINE)
    if not heading:
        return None
    fence = re.search(r"^```\s*$(.*?)^```\s*$", text[heading.end():],
        re.MULTILINE | re.DOTALL)
    if not fence:
        return None
    entries = []
    for line in fence.group(1).split("\n"):
        # A continuation line is indented past the path column; a path is the
        # first field of a line that starts in column zero.
        if not line or line.startswith(" "):
            continue
        entries.append(line.split()[0].rstrip(","))
    return entries


def tracked_files_by_directory():
    """Tracked files grouped by the directory they sit directly in."""
    listing = subprocess.run(["git", "-C", ROOT, "ls-files"],
        capture_output=True, text=True, check=True).stdout.split("\n")
    found = {}
    for path in listing:
        if not path or "/" not in path:
            continue
        if path.split("/")[0] not in ROOTS:
            continue
        found.setdefault(os.path.dirname(path), set()).add(path)
    return found


NAMED = set()


def directory_files():
    """Every tracked path a file or glob entry in the block names."""
    return NAMED


def main():
    entries = block_entries()
    if entries is None:
        print("check-layout-page: no fenced block under `## Layout` in "
              f"{PAGE}; the heading or the fence moved", file=sys.stderr)
        return 1

    problems = []
    directories = [e.rstrip("/") for e in entries if e.endswith("/")]
    globs = [e for e in entries if "*" in e]
    files = [e for e in entries
        if not e.endswith("/") and "*" not in e]

    # Every path the page names has to be there.
    for entry in directories:
        if not os.path.isdir(os.path.join(ROOT, entry)):
            problems.append(f"{PAGE} names `{entry}/`, which is not a "
                            "directory in the tree")
    for entry in files:
        if not os.path.exists(os.path.join(ROOT, entry)):
            problems.append(f"{PAGE} names `{entry}`, which is not in the "
                            "tree")
        else:
            NAMED.add(entry)
    for entry in globs:
        base = os.path.dirname(entry) or "."
        pattern = os.path.basename(entry).replace(".", r"\.").replace(
            "*", ".*")
        try:
            names = os.listdir(os.path.join(ROOT, base))
        except OSError:
            problems.append(f"{PAGE} names `{entry}`, whose directory is not "
                            "in the tree")
            continue
        matched = [n for n in names if re.fullmatch(pattern, n)]
        if not matched:
            problems.append(f"{PAGE} names `{entry}`, which matches nothing")
        for name in matched:
            NAMED.add(os.path.join(base, name) if base != "." else name)

    # And every directory with tracked code in it has to be reachable: either
    # a directory entry names it or an ancestor, or every tracked file sitting
    # directly in it is named one by one.
    #
    # A *file* entry does not cover its directory for this purpose, and that
    # distinction is the whole of what makes this arm work. `src/regex.c` is
    # in the block, and granting `src/` prefix coverage because of it made
    # every future directory under `src/` invisible here - which is the
    # failure this gate exists to catch, so the first version of it passed a
    # new `src/` directory without a word.
    named = directory_files()
    for directory, held in sorted(tracked_files_by_directory().items()):
        if any(directory == c or directory.startswith(c + "/")
                for c in directories):
            continue
        unnamed = sorted(f for f in held if f not in named)
        if unnamed:
            problems.append(f"`{directory}/` holds tracked files that no "
                            f"entry in {PAGE}'s layout block reaches: "
                            + ", ".join(unnamed))

    # Nothing found is the one green result this must not give: it is what a
    # renamed heading, a changed fence or a parser that stopped matching the
    # path column would all produce, and each reads as a page that agrees.
    if not directories:
        problems.append(f"no directory entries parsed out of {PAGE}'s layout "
                        "block, so nothing was compared")

    if problems:
        for problem in problems:
            print(f"check-layout-page: {problem}", file=sys.stderr)
        return 1

    print(f"layout page: {len(directories)} directories named, "
          f"{len(tracked_files_by_directory())} tracked directories reached.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
