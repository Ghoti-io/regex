#!/usr/bin/env python3
r"""How this tree reaches .NET's `System.Text.RegularExpressions`.

The shape `pcre2_runner.py`, `go_runner.py` and `rust_runner.py` have: the
driver's source is `tools/oracle/DotNetOracle.cs`, in this tree, and is
compiled inside the pinned image at run time so the image stays ignorant of
this library. What is different is the compiler it reaches for, and that is
worth writing down because the obvious spelling does not work.

**`dotnet run DotNetOracle.cs` cannot be used, and the reason is the network
namespace.** .NET 10 runs a single source file as a "file-based app", which
reads exactly like `java tools/oracle/JavaOracle.java` on the Java side and
was the first thing tried here. It synthesises a project and runs an implicit
restore, so inside `--network none` it fails with four lines of NU1301 against
api.nuget.org and no mention of the real problem. Measured in this image:

    error NU1301: Unable to load the service index for source
                  https://api.nuget.org/v3/index.json

Pre-seeding a NuGet cache would make the gate depend on a package feed to ask
a question about the standard library, which is the dependency the container
exists to remove.

**So it drives Roslyn directly.** `csc.dll` ships in the SDK, compiles against
the reference assemblies already in the image, and touches no package feed at
all: `-nostdlib` plus every `.dll` in the Microsoft.NETCore.App.Ref pack, out
to an assembly in the container's own `/tmp`, run with `dotnet exec`. There is
no project file, no restore, and nothing to resolve.

**The compile happens in the container's filesystem, not in the tree.** The
repository is mounted read-only - which is the rule, so that a corpus cannot
be edited by the thing being compared against it - and `/tmp` inside the
container is the container's own and dies with it. That also means no scratch
mount is needed: unlike vim, this reference answers through a pipe.

**The runtimeconfig.json is written rather than generated.** `dotnet exec`
needs one to know which shared framework to load, and the only generator for
it is the project system this file exists to avoid. It is three keys, and the
framework version is read from the image rather than written in, so raising
the pin does not silently leave it naming a framework the image no longer has.

Three versions decide an answer here and the pin in `containers/IMAGES` names
the first two: the **runtime**, because `Regex` and the Unicode tables
`\p{...}` reads are part of it; the **SDK**, because it carries the Roslyn
that compiles this driver; and the **Unicode data** the runtime was generated
from, which .NET does not state anywhere a probe can read. That third one is
measured rather than asked - `check-oracle-dotnet-properties` compares whole
property sets through the driver's `ranges` mode - and until it has been, the
IMAGES comment says so instead of naming a version nobody checked.
"""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import oracle_env

SOURCE = os.path.join(oracle_env.ROOT, "tools", "oracle", "DotNetOracle.cs")

# The container's own filesystem. Not under the mounted tree, which is
# read-only, and not a scratch mount, which this reference does not need.
WORK = "/tmp/ghoti-regex-dotnet"

# One `sh -c`, because every path here has to be discovered inside the image:
# the SDK directory, the shared framework and the reference pack all carry
# their versions in their names, and hard-coding any of them would make the
# runner break on a pin raise in a way that reads as a broken image.
#
# `exec` on the last line matters: without it the driver runs as a child of
# the shell and the shell owns the pipe, so a differential that closes stdin
# to end the batch waits for the wrong process.
BUILD = r"""
set -e
mkdir -p %(work)s
cd %(work)s
sdk=$(ls -d /usr/share/dotnet/sdk/*/ | head -1)
framework=$(ls -d /usr/share/dotnet/shared/Microsoft.NETCore.App/*/ | head -1)
reference=$(ls -d /usr/share/dotnet/packs/Microsoft.NETCore.App.Ref/*/ref/net*/ | head -1)
if [ ! -f oracle.dll ] || [ %(source)s -nt oracle.dll ]; then
  : > refs.rsp
  for dll in "$reference"*.dll; do echo "-reference:$dll" >> refs.rsp; done
  dotnet "${sdk}Roslyn/bincore/csc.dll" -nologo -nostdlib -optimize \
      -warnaserror @refs.rsp -out:oracle.dll %(source)s >&2
  printf '%%s' "{\"runtimeOptions\":{\"tfm\":\"net10.0\",\"framework\":{\"name\":\"Microsoft.NETCore.App\",\"version\":\"$(basename $framework)\"}}}" \
      > oracle.runtimeconfig.json
fi
exec dotnet exec oracle.dll
""" % {"work": WORK, "source": SOURCE}


def command():
    """`DotNetOracle`, compiled against the pinned SDK and run."""
    return oracle_env.command(
        "dotnet", ["sh", "-c", BUILD],
        # Telemetry would be a network call from a namespace with no network,
        # and the banner would land on stdout where the answers are.
        env={"DOTNET_NOLOGO": "1", "DOTNET_CLI_TELEMETRY_OPTOUT": "1"})


def version():
    """What the reference says it is, so a run names what answered."""
    return oracle_env.version("dotnet")
