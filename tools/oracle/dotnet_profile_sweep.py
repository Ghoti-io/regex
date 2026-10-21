#!/usr/bin/env python3
"""Condition 2: mutate each .NET profile field, rebuild, and see what fails.

A mutation nothing fails on is a rule with no test. The restore step is part
of the instrument, so every iteration ends with `git checkout --` whether it
succeeded or not.
"""
import os, re, subprocess, sys

# The repository root is this file's own location, three directories up, so the
# sweep runs from any checkout rather than from one. The install prefix is the
# caller's: $PREFIX, or --prefix. There is no default for it, because a wrong
# guess here does not fail - it builds against somebody else's headers and
# reports a clean sweep over the wrong library.
ROOT = os.path.dirname(
    os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PREFIX = os.environ.get("PREFIX", "")
for _index, _argument in enumerate(sys.argv[1:]):
    if _argument.startswith("--prefix="):
        PREFIX = _argument.split("=", 1)[1]
if not PREFIX:
    sys.stderr.write(
        "dotnet_profile_sweep: no install prefix.\n"
        "  PREFIX=<prefix> %s\n"
        "  %s --prefix=<prefix>\n"
        "The prefix holds the built dependencies this sweep compiles\n"
        "against; guessing it would measure whatever was installed.\n"
        % (sys.argv[0], sys.argv[0]))
    raise SystemExit(2)
SRC = os.path.join(ROOT, "src", "syntax", "syntax.c")
HDR = os.path.join(ROOT, "src", "syntax", "syntax_internal.h")
APP = os.path.join(ROOT, "build", "linux", "release", "apps")
DIFFS = ["syntax", "match", "replace", "split", "flags", "search", "names"]
DRIVER = {"syntax": "grx_syntax", "match": "grx_match", "replace": "grx_replace",
          "split": "grx_split", "flags": "grx_match", "search": "grx_match",
          "names": "grx_match"}


def enumerators():
    out = {}
    for base, _, names in os.walk(os.path.join(ROOT, "src")):
        for name in names:
            if not name.endswith(".h"):
                continue
            text = open(os.path.join(base, name), encoding="utf-8").read()
            for body, tag in re.findall(
                    r"typedef enum \{(.*?)\} (GRX_[A-Za-z]+);", text, re.S):
                vals = [v for v in re.findall(r"^\s*(GRX_[A-Z0-9_]+)", body, re.M)
                        if not v.endswith("COUNT")]
                out[tag] = vals
    return out


def profile_fields():
    src = open(HDR, encoding="utf-8").read()
    body = src[src.index("typedef struct GRX_Profile {"):src.index("} GRX_Profile;")]
    return re.findall(r"^  ([A-Za-z_][A-Za-z_0-9]*) ([a-z_][a-z_0-9]*);", body, re.M)


def dotnet_row(text):
    start = text.index("  [GRX_SYNTAX_DOTNET] = {", text.index("GRX_Profile kProfiles") if "GRX_Profile kProfiles" in text else 0)
    # The profile table is the second occurrence in the file.
    occurrences = [m.start() for m in re.finditer(r"  \[GRX_SYNTAX_DOTNET\] = \{", text)]
    start = occurrences[-1]
    end = text.index("\n  },", start) + len("\n  },")
    return start, end


def run(cmd, cwd=ROOT):
    return subprocess.run(cmd, cwd=cwd, shell=True, capture_output=True, text=True)


def main():
    enums = enumerators()
    fields = profile_fields()
    original = open(SRC, encoding="utf-8").read()
    start, end = dotnet_row(original)
    row = original[start:end]

    plan = []
    for type_name, field in fields:
        if type_name in ("GRX_TemplateSpec",):
            continue
        current = None
        found = re.search(r"\.%s = ([A-Za-z0-9_]+)" % field, row)
        if found:
            current = found.group(1)
        # **Every other legal value, not one of them.** The first version of
        # this picked enumerators[1] (or [0]), and for `lookbehind` that is
        # GRX_LOOKBEHIND_NONE against .NET's UNBOUNDED - which lower.c maps
        # to the same GRX_NPOS, since only BOUNDED and FIXED get a number.
        # The mutation compiled, linked and changed nothing, and the sweep
        # recorded "nothing failed": a reading that says no test covers the
        # field when what happened is that the value chosen was a synonym of
        # the one already there.
        #
        # A field is uncaught only when *no* alternative fails anything, and
        # the report names which values caught it, because a field caught by
        # one of six alternatives is weaker coverage than one caught by all
        # six.
        if type_name == "int":
            wants = ["0" if current == "1" else "1"]
        elif type_name == "uint32_t":
            wants = ["GRX_OPT_UTF", "GRX_OPT_UCP", "GRX_OPT_CASELESS"]
            wants = [w for w in wants if w != current]
        else:
            vals = enums.get(type_name)
            if not vals or len(vals) < 2:
                continue
            base = current or vals[0]
            wants = [v for v in vals if v != base]
        plan.append((field, current, wants))

    # **Pre-flight, and it is the control the first run of this did not
    # have.** That run reported "caught 0, nothing failed 40" with every
    # line reading BUILD FAILED: the tree still held objects built by the
    # container's GCC 16, which the host's GCC 14 cannot link, so no
    # mutation was ever compiled and the sweep measured nothing. The
    # end-of-run control passed, because it only proved the restore worked.
    #
    # A mutation harness has to know that an *unmutated* build succeeds and
    # that the gates pass on it, before any result from a mutated one means
    # anything.
    # **`make clean` first, and that is the second correction to this
    # control.** The first version checked nothing at all and every line
    # read BUILD FAILED. The second built the unmutated tree and passed -
    # and every mutated build still failed, because an unmutated tree built
    # by the container's GCC 16 *links* on the host without recompiling
    # anything, while touching syntax.c rebuilds one object with the host's
    # GCC 14 and then cannot link it against the rest.
    #
    # So a pre-flight has to exercise the path the iterations exercise -
    # compile and link after a source change - not merely ask whether the
    # tree as it stands links. Cleaning first is the cheapest way to
    # guarantee every object in the tree came from the compiler this sweep
    # will be using.
    print("pre-flight: cleaning, then building the unmutated tree",
          flush=True)
    run("make clean")
    build = run("make tools build/linux/release/apps/testDotnet "
                "PREFIX=%s" % PREFIX)
    if build.returncode:
        print("pre-flight build FAILED; the sweep would measure nothing:\n%s"
              % build.stdout[-2000:] + build.stderr[-2000:])
        return 2
    broken = [d for d in DIFFS
              if run("python3 tools/oracle/dotnet_%s_diff.py --driver "
                     "%s/tools/%s" % (d, APP, DRIVER[d])).returncode]
    if broken or run("%s/testDotnet" % APP).returncode:
        print("pre-flight gates FAILED (%s); fix those first"
              % ",".join(broken + (["testDotnet"] if not broken else [])))
        return 2
    print("pre-flight clean", flush=True)

    print("%d fields to sweep, %d mutations in all"
          % (len(plan), sum(len(w) for _, _, w in plan)), flush=True)
    caught, missed = [], []
    for field, current, wants in plan:
      caught_by = []
      build_failures = []
      for want in wants:
        text = open(SRC, encoding="utf-8").read()
        s, e = dotnet_row(text)
        body = text[s:e]
        if current is not None:
            body = re.sub(r"\.%s = [A-Za-z0-9_]+" % field,
                          ".%s = %s" % (field, want), body, count=1)
        else:
            body = body.replace("\n  },", "\n    .%s = %s,\n  }," % (field, want))
        open(SRC, "w", encoding="utf-8").write(text[:s] + body + text[e:])

        build = run("make tools build/linux/release/apps/testDotnet "
                    "PREFIX=%s" % PREFIX)
        if build.returncode:
            run("git checkout -- src/syntax/syntax.c")
            build_failures.append(want)
            print("%-34s %-30s BUILD FAILED" % (field, want), flush=True)
            continue

        failures = []
        unit = run("%s/testDotnet" % APP)
        if unit.returncode:
            failures.append("testDotnet")
        for diff in DIFFS:
            got = run("python3 tools/oracle/dotnet_%s_diff.py --driver "
                      "%s/tools/%s" % (diff, APP, DRIVER[diff]))
            if got.returncode:
                failures.append(diff)
        run("git checkout -- src/syntax/syntax.c")
        if failures:
            caught_by.append((want, failures))
            print("%-34s %-30s caught by %s"
                  % (field, want, ",".join(failures)), flush=True)
        else:
            print("%-34s %-30s no failure" % (field, want), flush=True)

      if caught_by:
          caught.append((field, caught_by, len(wants)))
          print("  => %s caught by %d of %d values"
                % (field, len(caught_by), len(wants)), flush=True)
      else:
          why = ("every alternative failed to build" if build_failures
                 else "no alternative value failed anything")
          missed.append((field, wants, why))
          print("  => %s *** %s ***" % (field, why), flush=True)

    # The control: with the tree restored, everything must pass again.
    run("make tools build/linux/release/apps/testDotnet PREFIX=%s" % PREFIX)
    clean = [d for d in DIFFS
             if run("python3 tools/oracle/dotnet_%s_diff.py --driver %s/tools/%s"
                    % (d, APP, DRIVER[d])).returncode]
    print("\ncaught %d, nothing failed %d" % (len(caught), len(missed)))
    print("restored-tree control: %s"
          % ("CLEAN" if not clean else "STILL FAILING: " + ",".join(clean)))
    for field, wants, why in missed:
        print("  missed %-32s %s (tried %s)"
              % (field, why, ",".join(wants)))
    for field, caught_by, total in caught:
        if len(caught_by) < total:
            print("  partial %-31s caught by %d of %d: %s"
                  % (field, len(caught_by), total,
                     ",".join(w for w, _ in caught_by)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
