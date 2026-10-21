#!/usr/bin/env python3
"""Condition 2: mutate each .NET profile field, rebuild, and see what fails.

A mutation nothing fails on is a rule with no test. The restore step is part
of the instrument, so every iteration ends with `git checkout --` whether it
succeeded or not.
"""
import os, re, subprocess, sys

ROOT = "/home/corey/Documents/ghoti.io/libs/regex"
PREFIX = "/home/corey/Documents/ghoti.io/.local"
SRC = os.path.join(ROOT, "src", "syntax", "syntax.c")
HDR = os.path.join(ROOT, "src", "syntax", "syntax_internal.h")
APP = os.path.join(ROOT, "build", "linux", "release", "apps")
DIFFS = ["syntax", "match", "replace", "split", "flags", "search"]
DRIVER = {"syntax": "grx_syntax", "match": "grx_match", "replace": "grx_replace",
          "split": "grx_split", "flags": "grx_match", "search": "grx_match"}


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
        if type_name == "int":
            want = "0" if current == "1" else "1"
        elif type_name == "uint32_t":
            want = "GRX_OPT_UTF" if current != "GRX_OPT_UTF" else "GRX_OPT_UCP"
        else:
            vals = enums.get(type_name)
            if not vals or len(vals) < 2:
                continue
            base = current or vals[0]
            want = vals[1] if base == vals[0] else vals[0]
        plan.append((field, current, want))

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
    print("pre-flight: building the unmutated tree", flush=True)
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

    print("%d fields to sweep" % len(plan), flush=True)
    caught, missed = [], []
    for field, current, want in plan:
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
            missed.append((field, want, "build failed"))
            print("%-36s %-28s BUILD FAILED" % (field, want), flush=True)
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
            caught.append((field, want, failures))
            print("%-36s %-28s caught by %s"
                  % (field, want, ",".join(failures)), flush=True)
        else:
            missed.append((field, want, "nothing failed"))
            print("%-36s %-28s *** NOTHING FAILED ***" % (field, want), flush=True)

    # The control: with the tree restored, everything must pass again.
    run("make tools build/linux/release/apps/testDotnet PREFIX=%s" % PREFIX)
    clean = [d for d in DIFFS
             if run("python3 tools/oracle/dotnet_%s_diff.py --driver %s/tools/%s"
                    % (d, APP, DRIVER[d])).returncode]
    print("\ncaught %d, nothing failed %d" % (len(caught), len(missed)))
    print("restored-tree control: %s"
          % ("CLEAN" if not clean else "STILL FAILING: " + ",".join(clean)))
    for field, want, why in missed:
        print("  missed %-34s %-28s %s" % (field, want, why))
    return 0


if __name__ == "__main__":
    sys.exit(main())
