SUITE := ghoti.io
PROJECT := regex

BUILD ?= release
# The version of this library. MINOR_VERSION carries the minor and the patch as
# one dotted string; the two are split out below for the places that need three
# separate integers. See CONVENTIONS.md section 4.
MAJOR_VERSION := 0
MINOR_VERSION := 0.0
VERSION_MINOR_ONLY := $(word 1,$(subst ., ,$(MINOR_VERSION)))
VERSION_PATCH_ONLY := $(or $(word 2,$(subst ., ,$(MINOR_VERSION))),0)
# Substituted into the .pc file; an empty Version: field makes every
# pkg-config version constraint fail.
VERSION := $(MAJOR_VERSION).$(MINOR_VERSION)

# Names this build everywhere: the .pc file, the install directory, the soname
# and the symbol token. It defaults to the major version, so an ordinary build
# of 1.x is "-1" and two majors cannot be loaded into one process by mistake.
# Override it for a build that wants its own identity:  make BRANCH=-dev
BRANCH ?= -$(MAJOR_VERSION)

# What the library reports as its version. The branch is appended only when it
# is not the default, so an ordinary build says "1.2.3" and an overridden one
# says "1.2.3-dev". Computed before BUILD=debug rewrites BRANCH below.
ifeq ($(BRANCH),-$(MAJOR_VERSION))
VERSION_STRING := $(VERSION)
else
VERSION_STRING := $(VERSION)$(BRANCH)
endif

# If BUILD is debug, append -debug.
#
# "override" because BRANCH may have come from the command line, and a
# command-line variable otherwise wins over a plain assignment here: without it
# `make BRANCH=-dev BUILD=debug` produced a debug build carrying the release
# token, whose symbols collide with the release build's.
ifeq ($(BUILD),debug)
    override BRANCH := $(BRANCH)-debug
    override VERSION_STRING := $(VERSION_STRING)-debug
    OPT_CFLAGS := -O0
else
    OPT_CFLAGS := -O2
endif

BASE_NAME := lib$(SUITE)-$(PROJECT)$(BRANCH).so
# The symbol namespace token, from BRANCH, so that the token inside every
# exported symbol is the same one that names the .pc file, the install directory
# and the shared library. See CONVENTIONS.md section 4.
LIBVER_SYMBOL := $(shell echo "ghotiio_$(PROJECT)$(BRANCH)" | sed 's/[.-]/_/g')

BASE_NAME_PREFIX := lib$(SUITE)-$(PROJECT)$(BRANCH)
SO_NAME := $(BASE_NAME).$(MAJOR_VERSION)
STATIC_TARGET := $(BASE_NAME_PREFIX).a
ENV_VARS :=

# PC_INSTALL_PATH names where this project's own .pc file is installed.
# PKG_CONFIG_PATH is the environment's and is never assigned here: make exports
# an inherited variable with whatever value the makefile last gave it, so
# overwriting it handed every sub-make a different PKG_CONFIG_PATH from the
# parent's. The sub-make then derived different flags, found the flag stamp
# changed, and rebuilt everything - which check-rebuild reports as a settled
# tree that will not settle. It showed first under MSYS2, whose login shell
# exports PKG_CONFIG_PATH, and happens on Linux whenever the exported value is
# not exactly the install location. cutil made the same change.
PKG_CONFIG_PATH_ENV := $(PKG_CONFIG_PATH)

# `override` on each of those: BUILD may arrive on the command line, and a
# command-line variable beats a plain makefile assignment, so without it
# `make BUILD=debug` skips the rewrite and builds into ./build/debug --
# outside the platform tree, and a different tree from the one plain `make`
# uses. The platform segment exists to keep linux/mac/win builds apart.

# Detect OS
UNAME_S := $(shell uname -s)

ifeq ($(UNAME_S), Linux)
	OS_NAME := Linux
	LIB_EXTENSION := so
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG := -Wl,-soname,$(SO_NAME)
	TARGET := $(SO_NAME).$(MINOR_VERSION)
	EXE_EXTENSION :=
	# Additional Linux-specific variables
	PC_INSTALL_PATH := /usr/local/share/pkgconfig
	INCLUDE_INSTALL_PATH := /usr/local/include
	LIB_INSTALL_PATH := /usr/local/lib
	PC_INCLUDE_DIR := $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	PC_LIB_DIR := $(LIB_INSTALL_PATH)/$(SUITE)
	override BUILD := linux/$(BUILD)

else ifeq ($(UNAME_S), Darwin)
	OS_NAME := Mac
	LIB_EXTENSION := dylib
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG := -Wl,-install_name,$(BASE_NAME_PREFIX).dylib
	TARGET := $(BASE_NAME_PREFIX).dylib
	EXE_EXTENSION :=
	# Additional macOS-specific variables
	PC_INSTALL_PATH := /usr/local/share/pkgconfig
	INCLUDE_INSTALL_PATH := /usr/local/include
	LIB_INSTALL_PATH := /usr/local/lib
	PC_INCLUDE_DIR := $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	PC_LIB_DIR := $(LIB_INSTALL_PATH)/$(SUITE)
	override BUILD := mac/$(BUILD)

else ifeq ($(findstring MINGW32_NT,$(UNAME_S)),MINGW32_NT)  # 32-bit Windows
	OS_NAME := Windows
	LIB_EXTENSION := dll
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG = -Wl,--out-implib,$(APP_DIR)/$(BASE_NAME_PREFIX).dll.a
	TARGET := $(BASE_NAME_PREFIX).dll
	EXE_EXTENSION := .exe
	# Additional Windows-specific variables
	# This is the path to the pkg-config files on MSYS2
	PC_INSTALL_PATH := /mingw32/lib/pkgconfig
	INCLUDE_INSTALL_PATH := /mingw32/include
	LIB_INSTALL_PATH := /mingw32/lib
	BIN_INSTALL_PATH := /mingw32/bin
	# Windows paths for .pc so gcc invoked by mingw can resolve -I/-L (cygpath for MSYS2)
	PC_INCLUDE_DIR = $(shell cygpath -m $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH))
	PC_LIB_DIR = $(shell cygpath -m $(LIB_INSTALL_PATH)/$(SUITE))
	override BUILD := win32/$(BUILD)

# TODO(windows): the Windows branches in this file were adapted from model's,
# which were adapted from image's, and have never been run, nor has GRX_API's
# dllexport/dllimport switching. See WINDOWS-TODO.md.
else ifeq ($(findstring MINGW64_NT,$(UNAME_S)),MINGW64_NT)  # 64-bit Windows
	OS_NAME := Windows
	LIB_EXTENSION := dll
	OS_SPECIFIC_CXX_FLAGS := -shared
	OS_SPECIFIC_LIBRARY_NAME_FLAG = -Wl,--out-implib,$(APP_DIR)/$(BASE_NAME_PREFIX).dll.a
	TARGET := $(BASE_NAME_PREFIX).dll
	EXE_EXTENSION := .exe
	# Additional Windows-specific variables
	# This is the path to the pkg-config files on MSYS2
	PC_INSTALL_PATH := /mingw64/lib/pkgconfig
	INCLUDE_INSTALL_PATH := /mingw64/include
	LIB_INSTALL_PATH := /mingw64/lib
	BIN_INSTALL_PATH := /mingw64/bin
	# Windows paths for .pc so gcc invoked by mingw can resolve -I/-L (cygpath for MSYS2)
	PC_INCLUDE_DIR = $(shell cygpath -m $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH))
	PC_LIB_DIR = $(shell cygpath -m $(LIB_INSTALL_PATH)/$(SUITE))
	override BUILD := win64/$(BUILD)

else
    $(error Unsupported OS: $(UNAME_S))

endif

# ---------------------------------------------------------------------------
# Installation prefix
#
# Defaults to the system location chosen above. Override it to install
# somewhere else - the suite's bootstrap installs every library into a local
# prefix so that each build resolves its dependencies through pkg-config,
# exactly as a consumer would, rather than through a second code path that
# only in-tree builds exercise. See CONVENTIONS.md section 1.
#
#     make install PREFIX=/path/to/prefix
# ---------------------------------------------------------------------------
ifdef PREFIX
INCLUDE_INSTALL_PATH := $(PREFIX)/include
LIB_INSTALL_PATH := $(PREFIX)/lib
BIN_INSTALL_PATH := $(PREFIX)/bin
PC_INSTALL_PATH := $(PREFIX)/share/pkgconfig
ifeq ($(OS_NAME), Windows)
PC_INCLUDE_DIR = $(shell cygpath -m $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH))
PC_LIB_DIR = $(shell cygpath -m $(LIB_INSTALL_PATH)/$(SUITE))
else
PC_INCLUDE_DIR := $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
PC_LIB_DIR := $(LIB_INSTALL_PATH)/$(SUITE)
endif
# A non-system prefix has no /etc/ld.so.conf.d, and writing to it would need
# root anyway. Everything built here carries an rpath to the prefix instead.
LDCONF_INSTALL_PATH :=
endif

# Dependencies are looked up along the inherited PKG_CONFIG_PATH as well as the
# install location chosen above, so that exporting PKG_CONFIG_PATH works as the
# errors below say it does. The inherited value comes first: it is an explicit
# request for this build, where the install location may be only a default.
PKG_CONFIG_LOOKUP_PATH := $(if $(PKG_CONFIG_PATH_ENV),$(PKG_CONFIG_PATH_ENV):)$(PC_INSTALL_PATH)


CXX := g++
# The C++ side is the tests and the two link steps, and it does *not* follow
# OPT_CFLAGS: it stays at -O1 whatever BUILD says. That is deliberate only in
# the sense that it is not this library's call - every library in the suite
# compiles its C++ tests at a fixed -O1, so `make BUILD=debug` gives a
# steppable library and tests that are still optimised. Left as it is, and
# written down, rather than settled here alone.
CXXFLAGS := -pedantic-errors -Wall -Wextra -Werror -Wno-error=unused-function -Wfatal-errors -std=c++20 -O1 -g $(EXTRA_CXXFLAGS)
CC := cc
# $(OPT_CFLAGS), set beside the BUILD=debug rename above: -O2 for a release
# build and -O0 for a debug one. It is set there rather than here because
# `BUILD` still holds the caller's raw value at that point; by this line the
# platform prefix has been added and it reads `linux/debug`, so a test for
# `debug` here would be false.
#
# It used also to be true that a command-line `BUILD` escaped that prefix
# entirely, because a plain assignment loses to a command-line variable. The
# four platform assignments carry `override` now, so `make BUILD=release` and
# a bare `make` are the same tree - they were not, and that is worth knowing
# when reading anything in this file written before that change.
#
# This was a literal -O0 for both builds until 2026-09-23, and nobody chose
# it: early repos in this suite were written when the production build doubled
# as the debugging build, and newer ones copied what already existed. The
# release build was therefore unoptimised and the debug build was correct by
# accident. Measured cost of the -O0: 2.32x-2.53x on tools/bench, median
# 2.38x, over regex's own code - the timed loop allocates nothing, so no part
# of that figure is cutil's.
#
# Strict aliasing, in two halves, because they travel to different places.
#
# This is the one undefined-behaviour class in the suite with no runtime gate
# at all, so `make test-asan` and every fuzzer are blind to it however they
# are compiled. Measured here rather than assumed, with a program that writes
# 7 through an int32_t *, writes 1.0f through a float * aliasing the same
# object, and reads the int back:
#
#   -O0, no sanitizers                        prints 1065353216
#   -O2, no sanitizers                        prints 7
#   -O1 and -O2, ASan+UBSan, gcc and clang    prints 7, exits 0, says nothing
#
# So the optimiser's answer already differs from the unoptimised one - the
# violation is live at the level this library ships at - and four sanitizer
# runs report no error at all. text measured the same blindness against
# checks its sanitizers DO catch (heap-use-after-free, stack overflow, signed
# overflow, float-cast overflow, all caught at -O1 and -O2), which is what
# makes it a gap in the instrument rather than a quiet run.
#
# A static warning is therefore the only instrument there is, and it is
# partial: it does not follow a violation laundered through a function
# boundary, and no level catches punning through a void *.
#
# The ASSUME half names the optimiser assumption. It belongs in every tree
# that compiles the library, including the ones where it is already on,
# because the default differs by compiler and optimisation level and appeared
# on no command line. Measured here by diffing emitted code against
# -fno-strict-aliasing for a minimal pair, not read off a manual page:
#
#              -O0   -O1   -O2
#   gcc 14.2   off   off   ON
#   clang 19.1 off   ON    ON
#
# So the assumption this library ships under is one gcc picked at -O2 and
# nobody wrote down, and the fuzz tree's clang had it from -O1 for a
# different reason. Naming it changes nothing here today:
# compiling all 38 library TUs with and without it, at -O2 and at -O0,
# with -g0 so that the flag string in DW_AT_producer cannot masquerade as
# codegen, gives 38 identical objects at both levels. At -O2 because the flag
# was already on; at -O0 because -O0 does no alias-based optimisation to
# change. So this is a statement of intent that the -O0 debug build compiles
# under the same assumption as the shipped one, not a change to either.
ALIASING_ASSUME_CFLAGS := -fstrict-aliasing
#
# The WARN half is the guard, and its LEVEL is named because -Wall already
# sets one. `gcc -Q --help=warnings -Wall` reports -Wstrict-aliasing=3, and
# level 3 is silent on shapes level 1 rejects - so -Wall at -O2 gives
# -fstrict-aliasing the optimisation with no warning behind it: the
# assumption armed and the guard absent. That was this library's state until
# now.
#
# Precedence is not positional against -Wall. An explicit level beats -Wall's
# implicit 3 from either side, so where $(ALIASING_CFLAGS) sits in the line
# does not matter; "last one wins" holds only between two EXPLICIT levels.
# The disarm vector is therefore a later explicit level, and CFLAGS ends with
# $(EXTRA_CFLAGS): `make EXTRA_CFLAGS=-Wstrict-aliasing=3` builds at level 3
# with every flag still present and every sentence here still true.
# check-aliasing catches exactly that, because it compiles its control with
# the real $(CFLAGS) and so sees the resolved level rather than the spelling.
#
# Level 1 rather than 3 costs this library nothing: all 38 TUs compile clean
# at level 1 under -Werror, measured, 0 diagnostics. That is a property of
# the code rather than a general truth. Measured next door for contrast:
# libs/ctang's 61 source TUs give 588 diagnostics across 47 of them at the
# same level, every one a downcast to a struct's initial member that C17
# 6.7.2.1p15 makes well defined. Where that is the architecture this gate
# cannot be coverage, only a statement that gcc's level 1 still works.
ALIASING_WARN_CFLAGS := -Wstrict-aliasing=1
ALIASING_CFLAGS := $(ALIASING_ASSUME_CFLAGS) $(ALIASING_WARN_CFLAGS)
CFLAGS := -pedantic-errors -Wall -Wextra -Werror -Wno-error=unused-function -Wfatal-errors -std=c17 $(OPT_CFLAGS) $(ALIASING_CFLAGS) -g $(EXTRA_CFLAGS)
# Library-specific compile flags (export symbols on Windows, PIC on Linux)
# GRX_BUILD enables DLL export on Windows (checked by GRX_API macro)
# GRX_TEST_BUILD enables export of internal functions for testing (checked by GRX_INTERNAL_API macro)
# No -DGRX_TEST_BUILD: the shipped library exports its public API and nothing
# else. Tests reach the internals by linking the static archive, which a static
# link can do even for hidden symbols.
ifeq ($(OS_NAME), Windows)
# Everything built here but the library itself links the static archive, so
# the headers must not say dllimport to it: an archive has no __imp_ thunks.
# The library's own objects also get GRX_BUILD, which the header tests first.
# See GRX_API in macros.h.
CFLAGS += -DGRX_STATIC
CXXFLAGS += -DGRX_STATIC
endif
LIB_CFLAGS := $(CFLAGS) -fvisibility=hidden -DGRX_BUILD $(EXTRA_CFLAGS)
LDFLAGS := -L /usr/lib -lstdc++ -lm $(EXTRA_LDFLAGS)
ifdef PREFIX
# So that a library, a test or an example finds its Ghoti.io dependencies in the
# prefix at run time without LD_LIBRARY_PATH.
LDFLAGS += -Wl,-rpath,$(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Windows)
# Windows has no rpath: a program finds its DLLs through PATH. Putting the
# prefix's bin/ on it for everything make runs is the equivalent, so that a
# test, a tool or a Python oracle driving one finds cutil without the caller
# arranging it. Without this they die before main() with 0xC0000135.
export PATH := $(BIN_INSTALL_PATH):$(PATH)
endif
endif

BUILD_DIR := ./build/$(BUILD)
OBJ_DIR := $(BUILD_DIR)/objects
FLAGS_STAMP := $(OBJ_DIR)/.flags
GEN_DIR := $(BUILD_DIR)/generated
APP_DIR := $(BUILD_DIR)/apps


# Add OS-specific flags
ifeq ($(UNAME_S), Linux)
	LIB_CFLAGS += -fPIC

else ifeq ($(UNAME_S), Darwin)

else ifeq ($(findstring MINGW32_NT,$(UNAME_S)),MINGW32_NT)  # 32-bit Windows

else ifeq ($(findstring MINGW64_NT,$(UNAME_S)),MINGW64_NT)  # 64-bit Windows

else
	$(error Unsupported OS: $(UNAME_S))

endif

# The standard include directories for the project.
INCLUDE := -I include/ -I $(GEN_DIR)/

# Goals that compile and link nothing.  A missing sibling library must not stop
# them: `make docs` needs doxygen and the tracked sources, not cutil, and it
# was failing at parse time - before doxygen was ever reached - on any machine
# where the suite is not installed.  Every other goal still gets the hard
# error below, which is the point of having no fallback.
DEPLESS_GOALS := docs docs-pdf clean fuzz-clean cloc help
ifeq ($(filter-out $(DEPLESS_GOALS),$(or $(MAKECMDGOALS),all)),)
SKIP_DEP_CHECK := 1
endif

# ghoti.io-cutil, for the allocator vtable, and - once the parser exists - the
# growable array and the overflow-checked size math. Prefer pkg-config; fall back to a sibling
# checkout. The name must carry $(BRANCH): cutil installs its .pc as
# ghoti.io-cutil-dev.pc, so asking for "ghoti.io-cutil" never matches.
CUTIL_PC ?= ghoti.io-cutil$(BRANCH)
CUTIL_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(CUTIL_PC) 2>/dev/null)
CUTIL_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(CUTIL_PC) 2>/dev/null)
# Use the sibling path when pkg-config failed (empty) or returned an
# unsubstituted placeholder from the .pc template.
ifeq ($(strip $(CUTIL_CFLAGS)),)
ifndef SKIP_DEP_CHECK
$(error ghoti.io-cutil was not found by pkg-config. Run ./bootstrap.sh in the parent folder to build and install the suite into a local prefix, then pass the same PREFIX here - or point PKG_CONFIG_PATH at the directory holding its .pc file. There is deliberately no sibling-checkout fallback: a second resolution path that only in-tree builds exercise is one that silently rots.)
endif
endif
INCLUDE += $(CUTIL_CFLAGS)

# ghoti.io-text, for WP-11's JSON Schema seam. Nothing that ships links it:
# it is needed by examples/json_schema_provider.c, which is the adapter, and
# by tools/jsonschema, which runs JSON-Schema-Test-Suite through that
# adapter.
#
# So `make all` and `make install` do not need it, and `make examples`
# without it builds the rest and names the one it left out. `make test` does
# need it, because check-json-schema-suite is one of TEST_GATES and a gate
# that can decline to run is not a gate - this seam went unverified for as
# long as it did precisely because its absence printed "skipped" and exited
# 0. workspace.txt still marks text optional for regex and that stays true:
# bootstrap.sh builds and installs, it does not test.
TEXT_PC ?= ghoti.io-text$(BRANCH)
TEXT_CFLAGS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --cflags $(TEXT_PC) 2>/dev/null)
TEXT_LIBS := $(shell PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs $(TEXT_PC) 2>/dev/null)
HAVE_TEXT := $(if $(strip $(TEXT_LIBS)),1,)

# Automatically collect all .c source files under the src directory.
SOURCES := $(shell find src -type f -name '*.c')

# Convert each source file path to an object file path.
LIBOBJECTS := $(patsubst src/%.c,$(OBJ_DIR)/%.o,$(SOURCES))

TESTFLAGS := `PKG_CONFIG_PATH=$(PKG_CONFIG_LOOKUP_PATH) pkg-config --libs --cflags gtest`

# The checks `make test` runs besides the tests themselves. Named in a
# variable so that a build which cannot satisfy them can clear it: the
# coverage target does, because --coverage links the gcov runtime, whose
# mangle_path check-symbols is right to reject in a shipping library and
# wrong to reject in an instrumented one. Spelled as text's TEST_GATES is.
#
# check-engine-equivalence is here rather than only in check-oracles because
# it is not an oracle check. It compares this library against itself - the
# same rows through every engine that can run them - which is design.md
# section 9 invariant 2, and nothing else enforces it. It was reachable only
# through check-oracles, a target a routine build does not type, which is the
# shape of hole a gate goes unnoticed in. It needs python3 and this library's
# own driver, no reference implementation, and costs about a second and a
# half.
# check-json-schema-suite is here for the same reason: it is the only thing
# that asks whether WP-11's seam is right - this library's matcher driving
# `text`'s JSON Schema engine over the corpus every other implementation is
# measured with - and it was reachable only by typing its name. Both of its
# preconditions are hard failures rather than skips, and the runner is given
# the count it must reach, because all three of "text is not installed", "the
# suite was never fetched" and "the corpus is there but answered nothing"
# used to exit 0.
# check-aliasing is here because strict aliasing is the one undefined-
# behaviour class nothing else in this library can see: the sanitizers do
# not detect it at any optimisation level, so `make test-asan` and the
# fuzzers are not covering it and never were. The static warning is the
# only instrument, it now rides every C compile line, and this gate is what
# says it is still armed. It costs one -fsyntax-only invocation.
# Spelled as two variables so that dropping one gate is a thing you can say
# on a command line. `TEST_GATES='$$(filter-out <gate>,$$(TEST_GATES))'` is
# not: a command-line assignment is recursively expanded, so a TEST_GATES
# that names itself is a recursion error rather than a subtraction.
ALL_TEST_GATES := check-symbols check-layering check-aliasing \
	check-unicode-tables check-dump-names check-readme-example \
	check-diagnostics check-engine-equivalence check-json-schema-suite
TEST_GATES ?= $(ALL_TEST_GATES)

# Every target that runs the suites carries them - `test`, `test-quiet`,
# `test-valgrind`, `test-valgrind-quiet` - because a gate that hangs off one
# spelling of "run the tests" is a gate for whoever types that spelling. It
# was on `test` alone, and `test-quiet` is the readable one; `image` had the
# same hole and this is that fix swept here. `test-debug` and
# `test-valgrind-debug` re-enter through those, so they inherit it.
#
# Two are exempt, for the same reason and deliberately: `test-asan` and
# `coverage` build a *different* library - the sanitizer runtime and gcov's
# `mangle_path` are symbols check-symbols is right to reject in a shipping
# library and wrong to reject there. coverage clears TEST_GATES on its
# sub-make; test-asan never named them.

# How much of the pattern space `make check-oracle-syntax` walks. The default
# is a few seconds; a soak before a milestone raises the count and varies the
# seed (documentation/testing.md).
ORACLE_SEED ?= 1
ORACLE_COUNT ?= 120000
ORACLE_PATTERNS ?= 600

# Valgrind flags (exclude "still reachable" as it's not a leak)
VALGRIND_FLAGS := --leak-check=full --show-leak-kinds=definite,indirect,possible --track-origins=yes --error-exitcode=1

####################################################################
# Test discovery
####################################################################

# Shared test code that is not itself a test: the helper, and the conformance
# vector reader, which every conformance binary links and which has its own
# tests rather than being trusted.
# The exclusion is by *basename*, because `filter-out` allows one `%` per
# pattern and `%/test_%.cpp` has two - make reads the second one literally and
# the filter silently matches nothing, which linked every conformance test
# into every other one.
TEST_HELPER_CANDIDATES := $(wildcard tests/test_helpers.cpp) \
	$(wildcard tests/conformance/*.cpp)
TEST_HELPER_SRC := $(foreach candidate,$(TEST_HELPER_CANDIDATES),\
	$(if $(filter test_%,$(notdir $(candidate))),,$(candidate)))
TEST_HELPER_OBJ := $(patsubst tests/%.cpp,$(OBJ_DIR)/tests/%.o,$(TEST_HELPER_SRC))

# The static archive, not -l: a static link resolves hidden symbols, so the
# tests can exercise internals the shared library does not export.
# --whole-archive because anything registering itself from a constructor is
# otherwise dropped - a plain archive link only pulls in object files that
# something references by name.
REGEXLIBRARY := -Wl,--whole-archive $(APP_DIR)/$(STATIC_TARGET) -Wl,--no-whole-archive $(CUTIL_LIBS)

# Discover test sources and compute an executable name for each, as
# "path|name" pairs. test_foo.cpp -> testFoo.
TEST_PAIRS := $(shell find tests -type f -name 'test_*.cpp' 2>/dev/null | sort | grep -v test_helpers | while read f; do \
	echo "$$f|$$(basename "$$f" .cpp | sed 's/test_/test/; s/^test\([a-z]\)/test\U\1/')"; done)
TEST_SOURCES := $(foreach pair,$(TEST_PAIRS),$(word 1,$(subst |, ,$(pair))))
TEST_NAMES := $(foreach pair,$(TEST_PAIRS),$(word 2,$(subst |, ,$(pair))))
TEST_EXECUTABLES := $(addprefix $(APP_DIR)/,$(addsuffix $(EXE_EXTENSION),$(TEST_NAMES)))

# Automatically collect all example .c files. The one that links `text` is
# held out of the glob and added back only when pkg-config found it, so that
# `make examples` on a machine without `text` builds the rest instead of
# stopping at the first missing header.
TEXT_EXAMPLE_SOURCES := examples/json_schema_provider.c
EXAMPLE_SOURCES := $(filter-out $(TEXT_EXAMPLE_SOURCES),\
	$(shell find examples -type f -name '*.c' 2>/dev/null))
EXAMPLES := $(patsubst examples/%.c,$(APP_DIR)/examples/%$(EXE_EXTENSION),$(EXAMPLE_SOURCES))
ifdef HAVE_TEXT
EXAMPLES += $(patsubst examples/%.c,$(APP_DIR)/examples/%$(EXE_EXTENSION),$(TEXT_EXAMPLE_SOURCES))
endif

# The musl oracle. Its two translation units are fetched rather than
# committed, so it is not in TOOL_SOURCES and is built only when they are
# there - `tools/corpus/fetch.sh musl`. tools/corpus/VERSIONS says what a
# second POSIX implementation is for and what this hosted build of it can be
# asked; the ref is read from the same file so there is one place to raise.
MUSL_REF := $(shell awk '$$1 == "musl" { print $$2; exit }' tools/corpus/VERSIONS)
MUSL_SRC := third_party/musl/$(MUSL_REF)/src/regex
MUSL_UNITS := $(MUSL_SRC)/regcomp.c $(MUSL_SRC)/regexec.c $(MUSL_SRC)/tre-mem.c
MUSL_MATCH := $(APP_DIR)/tools/musl_match$(EXE_EXTENSION)

# Joined to the tool list only when the fetch has happened, so that `make
# tools` on a fresh clone builds what it can rather than failing on what it
# has not got. The checks that want it say so and skip when it is absent.
ifneq ($(wildcard $(MUSL_SRC)/regcomp.c),)
MUSL_AVAILABLE := 1
endif

# Strict ISO C, because under a GNU dialect glibc's <limits.h> would define
# RE_DUP_MAX as 0x7fff over musl's 255; tools/oracle/musl-include/regex.h
# refuses to compile without it rather than let that pass silently. -w
# because this is somebody else's code and its warnings are not ours to fix.
# The renames keep musl's entry points from colliding with glibc's, and the
# two limits are musl's own, from its include/limits.h.
MUSL_CFLAGS := -std=c11 -O2 -w -Itools/oracle/musl-include \
	-Dhidden='__attribute__((__visibility__("hidden")))' \
	-DCHARCLASS_NAME_MAX=14 -DRE_DUP_MAX=255 \
	-Dregcomp=musl_regcomp -Dregexec=musl_regexec -Dregfree=musl_regfree

# pcre2, linked rather than driven through pcre2test. Debian ships
# libpcre2-8.so.0 without the -dev package's pcre2.h, so the header comes
# from the release pinned in tools/corpus/VERSIONS, fetched with the corpus.
# It is a configure template whose only open substitutions are four version
# macros, which is what the rule below fills in - so the pinned corpus, the
# header and the installed library are one version or the build says so.
PCRE2_REF := $(shell awk '$$1 == "pcre2" { print $$2; exit }' tools/corpus/VERSIONS)
PCRE2_SRC := third_party/pcre2/$(PCRE2_REF)
PCRE2_HEADER_IN := $(PCRE2_SRC)/pcre2.h.in
PCRE2_HEADER := $(GEN_DIR)/pcre2.h
PCRE2_MATCH := $(APP_DIR)/tools/pcre2_match$(EXE_EXTENSION)

# Where the shared library actually is. `-lpcre2-8` needs the `.so` symlink
# that only the -dev package installs, so the versioned file is found instead
# - through ldconfig where there is one, and by looking where a Debian
# multiarch install puts it otherwise.
PCRE2_LIB := $(shell { /sbin/ldconfig -p 2>/dev/null || ldconfig -p 2>/dev/null; } \
	| awk '/libpcre2-8\.so/ { print $$NF; exit }')
ifeq ($(PCRE2_LIB),)
PCRE2_LIB := $(firstword $(wildcard /usr/lib/*/libpcre2-8.so.0 \
	/usr/lib/libpcre2-8.so.0 /lib/*/libpcre2-8.so.0))
endif

# Joined to the tool list only when both halves are present, the way the musl
# oracle is: a fresh clone that has not fetched, or a machine with no pcre2,
# builds what it can and the checks that want this say they skipped.
ifneq ($(wildcard $(PCRE2_HEADER_IN)),)
ifneq ($(PCRE2_LIB),)
PCRE2_AVAILABLE := 1
endif
endif

# The oracle drivers: this library wrapped so that a conformance harness can
# ask it the same question it asks a reference implementation. Built on
# demand rather than by `all`, because they are development tools and are not
# installed.
TOOL_SOURCES := $(shell find tools -type f -name '*.c' -not -path 'tools/jsonschema/*' \
	-not -name 'musl_match.c' -not -name 'pcre2_match.c' 2>/dev/null)
TOOLS := $(patsubst tools/%.c,$(APP_DIR)/tools/%$(EXE_EXTENSION),$(notdir $(TOOL_SOURCES)))
TOOLS := $(patsubst tools/oracle/%.c,$(APP_DIR)/tools/%$(EXE_EXTENSION),$(TOOL_SOURCES))
TOOLS := $(patsubst tools/limits/%.c,$(APP_DIR)/tools/%$(EXE_EXTENSION),$(TOOLS))
ifdef MUSL_AVAILABLE
TOOLS += $(MUSL_MATCH)
endif
ifdef PCRE2_AVAILABLE
TOOLS += $(PCRE2_MATCH)
endif

# The JSON Schema suite runner links `text`, so it joins the list only when
# pkg-config found it.
JSONSCHEMA_TOOL_SOURCES := $(shell find tools/jsonschema -type f -name '*.c' 2>/dev/null)
ifdef HAVE_TEXT
TOOLS += $(patsubst tools/jsonschema/%.c,$(APP_DIR)/tools/%$(EXE_EXTENSION),$(JSONSCHEMA_TOOL_SOURCES))
endif

# Where the test fixtures live. Tests run from build/.../apps, so the path is
# baked in at compile time.
REGEX_ROOT := $(CURDIR)
TEST_DATA := $(CURDIR)/tests/data

all: $(APP_DIR)/$(TARGET) $(APP_DIR)/$(STATIC_TARGET) ## Build shared + static libraries

####################################################################
# Dependency Inclusion
####################################################################

TEST_DEPFILES := $(foreach pair,$(TEST_PAIRS),$(OBJ_DIR)/tests/$(basename $(notdir $(word 1,$(subst |, ,$(pair))))).d)
DEPFILES := $(LIBOBJECTS:.o=.d) $(TEST_HELPER_OBJ:.o=.d) $(TEST_DEPFILES)
-include $(DEPFILES)

# The sanitizer and fuzz trees get theirs at the end of this file, where the
# variables naming their object directories have been defined. They were
# getting none at all, which meant a header change did not rebuild them and
# `make test-asan` reported on whatever had been compiled last.

####################################################################
# Object Files
####################################################################

####################################################################
# Generated version header
####################################################################

LIBVER_GEN := $(GEN_DIR)/ghoti.io/$(PROJECT)/libver_gen.h

# libver_gen.h is regenerated on every build and rewritten only when its content
# changes, so a variable given on the command line - make MAJOR_VERSION=2, or
# make BRANCH=-dev - takes effect. Keying the rule on the Makefile's timestamp
# alone left the previous token and version baked into the build, and nothing
# said so.
.PHONY: force-libver
force-libver:

$(LIBVER_GEN): force-libver
	@if [ -z "$(LIBVER_SYMBOL)" ]; then \
		printf "### LIBVER_SYMBOL is empty ###\n" >&2; \
		printf "Every exported symbol would lose its version namespace.\n" >&2; \
		exit 1; \
	fi
	@mkdir -p $(@D)
	@printf '%s\n' \
		'// Generated by the Makefile. Do not edit; see CONVENTIONS.md section 4.' \
		'#ifndef GHOTI_IO_GRX_LIBVER_GEN_H' \
		'#define GHOTI_IO_GRX_LIBVER_GEN_H' \
		'' \
		'/** The symbol namespace for this build, from the Makefile'"'"'s BRANCH. */' \
		'#define GHOTIIO_REGEX_NAME $(LIBVER_SYMBOL)' \
		'' \
		'/** Human-readable version of this build. */' \
		'#define GHOTIIO_REGEX_VERSION "$(VERSION_STRING)"' \
		'' \
		'/** The same version as three integers. */' \
		'#define GHOTIIO_REGEX_VERSION_MAJOR $(MAJOR_VERSION)' \
		'#define GHOTIIO_REGEX_VERSION_MINOR $(VERSION_MINOR_ONLY)' \
		'#define GHOTIIO_REGEX_VERSION_PATCH $(VERSION_PATCH_ONLY)' \
		'' \
		'#endif // GHOTI_IO_GRX_LIBVER_GEN_H' > $@.tmp
	@if cmp -s $@.tmp $@; then rm -f $@.tmp; else mv $@.tmp $@; fi

# `Makefile` is a prerequisite of every object rule in this file, all nine of
# them. make does not record the flags an object was built with, so editing
# CFLAGS or CXXFLAGS otherwise recompiles nothing and the next link mixes
# objects from two flag sets - which looks exactly like a successful build.
#
# All nine or none. Covering only the `src/%.c` rules would leave a CXXFLAGS
# edit rebuilding the library and not the tests, so the suite would run C++
# objects from the old flags against a library from the new ones. A partial
# rebuild is indistinguishable from a complete one, which makes it worse
# than no coverage at all. The invariant, which is worth re-running after
# any rule is added:
#
#     grep -cE '^\$\([A-Z_]*DIR\)[^:]*%\.[o]:' Makefile           # rules
#     grep -cE '^\$\([A-Z_]*DIR\)[^:]*%\.[o]:.*Makefile' Makefile # covered
#
# What it does *not* cover is a flag given on the command line -
# `make EXTRA_CFLAGS=-O3` changes no file's timestamp, so nothing rebuilds.
# That half has no structural fix and needs a BUILD tree of its own; see the
# note above the `bench` target.
$(OBJ_DIR)/%.o: src/%.c $(FLAGS_STAMP) | $(LIBVER_GEN)
	@printf "\n### Compiling $@ ###\n"
	@mkdir -p $(@D)
	$(CC) $(LIB_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

####################################################################
# Shared Library
####################################################################

$(APP_DIR)/$(TARGET): $(LIBOBJECTS)
	@printf "\n### Compiling Regex Library ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) -shared -o $@ $^ $(LDFLAGS) $(CUTIL_LIBS) $(OS_SPECIFIC_LIBRARY_NAME_FLAG)

ifeq ($(OS_NAME), Linux)
	@ln -f -s $(TARGET) $(APP_DIR)/$(SO_NAME)
	@ln -f -s $(SO_NAME) $(APP_DIR)/$(BASE_NAME)
endif

####################################################################
# Static Library
####################################################################

$(APP_DIR)/$(STATIC_TARGET): $(LIBOBJECTS)
	@printf "\n### Archiving Static Regex Library ###\n"
	@mkdir -p $(@D)
	@rm -f $@
	ar rcs $@ $^

####################################################################
# Unit Tests
####################################################################

ifneq ($(TEST_HELPER_SRC),)
$(TEST_HELPER_OBJ): $(TEST_HELPER_SRC)
	@printf "\n### Compiling Test Helper ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@
endif

# Test sources live in tests/ and tests/unit/; the object name comes from the
# basename either way, so the executable name matches.
$(OBJ_DIR)/tests/%.o: tests/%.cpp $(FLAGS_STAMP)
	@printf "\n### Compiling Test: $* ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -Itests -DGRX_TEST_DATA=\"$(TEST_DATA)\" -DGRX_REPO_ROOT=\"$(REGEX_ROOT)\" -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(OBJ_DIR)/tests/%.o: tests/conformance/%.cpp $(FLAGS_STAMP)
	@printf "\n### Compiling Test: $* ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -Itests -DGRX_TEST_DATA=\"$(TEST_DATA)\" -DGRX_REPO_ROOT=\"$(REGEX_ROOT)\" -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(OBJ_DIR)/tests/%.o: tests/unit/%.cpp $(FLAGS_STAMP)
	@printf "\n### Compiling Test: $* ###\n"
	@mkdir -p $(@D)
	$(CXX) $(CXXFLAGS) $(INCLUDE) -Itests -DGRX_TEST_DATA=\"$(TEST_DATA)\" -DGRX_REPO_ROOT=\"$(REGEX_ROOT)\" -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# Build rule for one test executable. $1 = source path, $2 = executable name.
# Tests compile to .o first and link separately, so a library change relinks
# without recompiling every test.
#
# The prerequisite is the static archive, because that is what the link line
# uses. Naming the shared library here instead - which is what this rule was
# copied from - makes `make test` fail on a clean tree, since nothing in that
# chain builds the archive, and makes a parallel build race when something
# does.
define test-executable-rule
TEST_OBJ_$1 := $(OBJ_DIR)/tests/$(basename $(notdir $1)).o

$(APP_DIR)/$2$(EXE_EXTENSION): $$(TEST_OBJ_$1) $(TEST_HELPER_OBJ) \
		$(APP_DIR)/$(STATIC_TARGET) | $(APP_DIR)/$(TARGET)
	@printf "\n### Linking Test: $2 ###\n"
	@mkdir -p $$(@D)
	$(CXX) $(CXXFLAGS) -o $$@ $$(TEST_OBJ_$1) $(TEST_HELPER_OBJ) $(LDFLAGS) $(REGEXLIBRARY) $(CUTIL_LIBS) $(TESTFLAGS)
endef

$(foreach pair,$(TEST_PAIRS),\
	$(eval $(call test-executable-rule,$(word 1,$(subst |, ,$(pair))),$(word 2,$(subst |, ,$(pair))))))

####################################################################
# Examples
####################################################################

# Links the archive, so it depends on the archive; see test-executable-rule.
$(APP_DIR)/examples/%$(EXE_EXTENSION): examples/%.c $(APP_DIR)/$(STATIC_TARGET) \
		| $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Example: $* ###\n"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $< $(LDFLAGS) $(REGEXLIBRARY) $(CUTIL_LIBS)

$(APP_DIR)/tools/%$(EXE_EXTENSION): tools/oracle/%.c $(APP_DIR)/$(STATIC_TARGET) \
		| $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Tool: $* ###\n"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $< $(LDFLAGS) $(REGEXLIBRARY) $(CUTIL_LIBS)

# musl's regex compiled straight into the driver: three of its translation
# units and ours, no library of ours linked, because an oracle answers for
# somebody else's implementation and must not be able to reach this one.
$(MUSL_MATCH): tools/oracle/musl_match.c tools/oracle/musl-include/regex.h \
		$(MUSL_UNITS)
	@printf "\n### Compiling Tool: musl_match ###\n"
	@mkdir -p $(@D)
	$(CC) $(MUSL_CFLAGS) -DGRX_MUSL_REF='"$(MUSL_REF)"' -o $@ \
		tools/oracle/musl_match.c $(MUSL_UNITS)

# pcre2's public header, from the pinned release's configure template. The
# four substitutions are the whole of what configure does to it, and they are
# version macros the driver prints so that a mismatch with the installed
# library is visible in the run rather than inferred from a crash.
$(PCRE2_HEADER): $(PCRE2_HEADER_IN)
	@mkdir -p $(@D)
	@sed -e 's/@PCRE2_MAJOR@/$(word 1,$(subst ., ,$(patsubst pcre2-%,%,$(PCRE2_REF))))/' \
	     -e 's/@PCRE2_MINOR@/$(word 2,$(subst ., ,$(patsubst pcre2-%,%,$(PCRE2_REF))))/' \
	     -e 's/@PCRE2_PRERELEASE@//' \
	     -e 's/@PCRE2_DATE@/$(PCRE2_REF)/' $< > $@

# Nothing of this library linked, for the reason musl_match links none: an
# oracle answers for somebody else's implementation and must not be able to
# reach this one. The shared library is named by path rather than by -l,
# because the `.so` symlink -l needs belongs to a -dev package that is not
# installed here.
$(PCRE2_MATCH): tools/oracle/pcre2_match.c $(PCRE2_HEADER)
	@printf "\n### Compiling Tool: pcre2_match ###\n"
	@mkdir -p $(@D)
	$(CC) -std=c17 -O2 -Wall -Wextra -I$(GEN_DIR) -o $@ \
		tools/oracle/pcre2_match.c $(PCRE2_LIB)

$(APP_DIR)/tools/%$(EXE_EXTENSION): tools/limits/%.c $(APP_DIR)/$(STATIC_TARGET) \
		| $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Tool: $* ###\n"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) -o $@ $< $(LDFLAGS) $(REGEXLIBRARY) $(CUTIL_LIBS)

# The two targets that link `text`. Specific rules, so they take precedence
# over the pattern rules above and add the flags pkg-config gave for it.
$(APP_DIR)/examples/json_schema_provider$(EXE_EXTENSION): \
		examples/json_schema_provider.c $(APP_DIR)/$(STATIC_TARGET) \
		| $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Example: json_schema_provider ###\n"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) $(TEXT_CFLAGS) -o $@ $< $(LDFLAGS) \
		$(REGEXLIBRARY) $(CUTIL_LIBS) $(TEXT_LIBS)

$(APP_DIR)/tools/%$(EXE_EXTENSION): tools/jsonschema/%.c \
		$(APP_DIR)/$(STATIC_TARGET) | $(APP_DIR)/$(TARGET)
	@printf "\n### Compiling Tool: $* ###\n"
	@mkdir -p $(@D)
	$(CC) $(CFLAGS) $(INCLUDE) $(TEXT_CFLAGS) -o $@ $< $(LDFLAGS) \
		$(REGEXLIBRARY) $(CUTIL_LIBS) $(TEXT_LIBS)

####################################################################
# Commands
####################################################################

# General commands
.PHONY: clean cloc docs docs-pdf examples tools coverage check-symbols check-layering check-aliasing check-diagnostics check-unicode-tables check-dump-names check-readme-example check-oracle-syntax check-oracle-match check-engine-equivalence check-oracle-perl check-oracle-vim check-oracle-perl-syntax check-oracle-script-runs check-oracle-newlines check-oracle-replace check-oracle-split check-oracle-window check-oracle-iterate \
	check-oracle-properties check-oracle-numeric-properties \
	check-oracle-string-properties check-oracle-posix check-oracle-sed \
	check-oracles \
	check-limits check-json-schema-suite vectors vectors-ecmascript \
	vectors-pcre vectors-perl vectors-posix
# Release build commands
.PHONY: all install test test-quiet test-asan test-valgrind test-valgrind-quiet test-watch uninstall watch
# Debug build commands
.PHONY: all-debug install-debug test-debug test-valgrind-debug test-watch-debug uninstall-debug watch-debug
# Fuzz commands
.PHONY: fuzz fuzz-long fuzz-clean

watch: ## Watch the file directory for changes and compile the target
	@while true; do \
		make --no-print-directory all; \
		printf "\033[0;32m\n"; \
		printf "#########################\n"; \
		printf "# Waiting for changes.. #\n"; \
		printf "#########################\n"; \
		printf "\033[0m\n"; \
		inotifywait -qr -e modify -e create -e delete -e move src include tests Makefile --exclude '/\.'; \
		done

test-watch: ## Watch the file directory for changes and run the unit tests
	@while true; do \
		make --no-print-directory all; \
		make --no-print-directory test; \
		printf "\033[0;32m\n"; \
		printf "#########################\n"; \
		printf "# Waiting for changes.. #\n"; \
		printf "#########################\n"; \
		printf "\033[0m\n"; \
		inotifywait -qr -e modify -e create -e delete -e move src include tests Makefile --exclude '/\.'; \
		done

tools: ## Build the oracle drivers used by the conformance harnesses
tools: $(APP_DIR)/$(TARGET) $(TOOLS)
	@printf "\nOracle drivers are in: $(APP_DIR)/tools/\n"

BENCH_DIR := $(APP_DIR)/bench
ifneq ($(wildcard $(MUSL_SRC)/regcomp.c),)
BENCH_MUSL_BIN := $(BENCH_DIR)/musl
$(BENCH_DIR)/musl: tools/bench/regex_bench.c $(MUSL_UNITS)
	@printf "\n### Compiling Benchmark: musl ###\n"
	@mkdir -p $(BENCH_DIR)
	$(CC) $(MUSL_CFLAGS) -DBENCH_MUSL -o $@ $< $(MUSL_UNITS)
else
BENCH_MUSL_BIN :=
endif

BENCH_BINS := $(BENCH_DIR)/ours $(BENCH_DIR)/glibc

bench: ## Time this library against glibc and musl on the same workload
# A number about this library needs something beside it, and the one this
# replaced was whole-process wall clock over a CLI run - so start-up, stdin
# and compiling a thousand patterns were all in the denominator, and it read
# as a statement about matching. This compiles once and times the searches.
#
# Built at whatever CFLAGS say, which today is -O0 for the C sources, so the
# absolute figures are not this library's and only the ratios are worth
# reading. For what an optimised build does, build into a directory of its
# own rather than over the release one:
#
#     make bench BUILD=o3 EXTRA_CFLAGS=-O3 CUTIL_PC=ghoti.io-cutil-0
#
# BUILD, because make does not track the flags an object was built with: -O3
# objects left in the release tree are invisible to a later `make test`,
# which will link them and report on a library nobody asked for. CUTIL_PC,
# because a non-default BUILD renames the .pc this looks for.
bench: $(BENCH_BINS) $(BENCH_MUSL_BIN)
	@printf "\n"
	@for load in a p n; do \
		$(BENCH_DIR)/ours 400 $$load posix-ere; \
		$(BENCH_DIR)/ours 400 $$load gnu-ere; \
		$(BENCH_DIR)/glibc 400 $$load; \
		if [ -x $(BENCH_DIR)/musl ]; then \
			$(BENCH_DIR)/musl 400 $$load; \
		fi; \
		printf "\n"; \
	done

$(BENCH_DIR)/ours: tools/bench/regex_bench.c $(APP_DIR)/$(STATIC_TARGET)
	@printf "\n### Compiling Benchmark: ours ###\n"
	@mkdir -p $(BENCH_DIR)
	$(CC) $(CFLAGS) -DBENCH_OURS $(INCLUDE) -o $@ $< $(LDFLAGS) $(REGEXLIBRARY)

$(BENCH_DIR)/glibc: tools/bench/regex_bench.c
	@printf "\n### Compiling Benchmark: glibc ###\n"
	@mkdir -p $(BENCH_DIR)
	$(CC) $(CFLAGS) -DBENCH_GLIBC -o $@ $<


check-oracles: ## Run every differential check against the reference implementation
check-oracles: check-oracle-syntax check-oracle-match check-oracle-properties \
	check-oracle-numeric-properties check-oracle-string-properties \
	check-oracle-posix check-oracle-submatch \
	check-oracle-perl check-oracle-perl-syntax check-oracle-python \
	check-oracle-vim \
	check-oracle-script-runs check-oracle-newlines check-oracle-callouts \
	check-oracle-sed \
	check-oracle-replace check-oracle-split check-oracle-window check-oracle-iterate \
	check-engine-equivalence

check-oracle-properties: ## Compare every Unicode property table against the reference
check-oracle-properties: $(TOOLS)
	@if ! command -v node >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then \
		printf "check-oracle-properties: skipped (no node or no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/property_diff.py \
		--driver $(APP_DIR)/tools/grx_properties$(EXE_EXTENSION)

check-oracle-numeric-properties: ## Compare the Numeric_Value tables against perl
check-oracle-numeric-properties: $(TOOLS)
	@if ! command -v perl >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then \
		printf "check-oracle-numeric-properties: skipped (no perl or no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/numeric_property_diff.py \
		--driver $(APP_DIR)/tools/grx_properties$(EXE_EXTENSION)

check-oracle-perl: ## Compare the Perl-family front ends against perl and pcre2
# WP-20's missing half. The rates were measured against two imported corpora;
# this generates the patterns instead, which is what found `(?^i:...)`
# dropping the letter after the reset, a backreference to a duplicated name
# resolving to a group that was never set, and perl's rule for quantifying a
# control verb.
check-oracle-perl: $(TOOLS)
	@if ! command -v perl >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then \
		printf "check-oracle-perl: skipped (no perl or no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/perl_diff.py --seed $(ORACLE_SEED) \
		--patterns $(ORACLE_PATTERNS)

check-oracle-perl-syntax: ## Ask perl and this library whether each Perl-family construct compiles
# A list rather than a generator, and that is the point: perl_diff.py
# generates from the grammar *this library* implements, so a construct this
# library has and perl has not is never generated as a disagreement. Four
# families were accepted here and "not recognized" in perl until this
# existed, each of them silently.
check-oracle-perl-syntax: $(TOOLS)
	@if ! command -v perl >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then \
		printf "check-oracle-perl-syntax: skipped (no perl or no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/perl_syntax_diff.py

check-oracle-script-runs: ## Compare `(*script_run:...)` against pcre2 and perl
# A table rule needs a table of cases: every pair and triple over an
# alphabet chosen to hit each clause of UTS #39 section 5.1, plus random
# longer strings. Both references must agree before either decides, which
# is what turned pcre2 10.46's Han defect from a disagreement into a
# finding - pcre2 accepts the three-way mixture its own manual names as
# not a script run, and perl refuses it.
check-oracle-script-runs: $(TOOLS)
	@if ! command -v perl >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then \
		printf "check-oracle-script-runs: skipped (no perl or no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/script_run_diff.py --seed $(ORACLE_SEED)

check-oracle-newlines: ## Compare PCRE2's newline conventions against pcre2
# Six conventions, each deciding four things at once - what `.` refuses,
# where `^` and `$` hold, where they hold *between* the two characters of a
# CR LF pair, and where an unanchored search may begin. pcre2 alone
# decides: Perl has no newline conventions, so there is no second opinion
# and none is pretended.
check-oracle-newlines: $(TOOLS)
	@if ! command -v python3 >/dev/null 2>&1; then \
		printf "check-oracle-newlines: skipped (no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/newline_diff.py

check-oracle-callouts: ## Compare the `(?C...)` trace against pcre2
# The one construct whose answer is not "did it match" but "where were you,
# and when" - so every other gate here is blind to it, and `a(?C1)b` and
# `ab` would report agreement about a feature that had not been built.
# pcre2 is compiled with NO_START_OPTIMIZE and NO_AUTO_POSSESS for this,
# because both change which paths it takes and this library has neither.
check-oracle-callouts: $(TOOLS)
	@if ! command -v python3 >/dev/null 2>&1; then \
		printf "check-oracle-callouts: skipped (no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/callout_diff.py

check-oracle-posix: ## Compare the POSIX and GNU front ends against glibc
check-oracle-posix: $(TOOLS)
	@if ! command -v python3 >/dev/null 2>&1; then \
		printf "check-oracle-posix: skipped (no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/posix_diff.py

check-oracle-submatch: ## Compare which spans the groups get, against glibc and musl
# posix_diff.py asks whether the same text matched; this asks which group got
# which part of it when more than one answer fits. Nothing asked that before:
# Spencer's vectors contain no case where two assignments share one extent,
# and posix_diff's patterns reach one only by accident. Built on purpose out
# of ambiguous pieces it found 1,050 cases across three dialects at once, all
# of one rule, and it is now WP-26's gate as well - including the six rows
# where both references agree and POSIX says otherwise. See
# documentation/dialects.md sections 5.1 and 6.
check-oracle-submatch: $(TOOLS)
	@if ! command -v python3 >/dev/null 2>&1; then \
		printf "check-oracle-submatch: skipped (no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/submatch_diff.py --strict

check-oracle-python: ## Compare the Python front end against CPython's `re`
# The only oracle here that runs in-process: `re` is importable by the tool
# that generates the cases, so a run costs a function call per row instead of
# a fork per batch. That is worth a note because it changed what the gate
# could find - 600,000 rows in under four seconds, against the tens of
# thousands the subprocess differentials manage in the same time - and every
# defect WP-30 found after the first build came out of scaling it up rather
# than out of reading the `re` documentation more carefully.
#
# Two of those defects were in code this dialect does not own: the prescan
# lost its group count after any class containing an escape, which made
# `[\d](a)\1` an invalid backreference in perl and pcre as well, and
# GRX_LOOKBEHIND_FIXED was in the profile and read by nothing.
check-oracle-python: $(TOOLS)
	@if ! command -v python3 >/dev/null 2>&1; then \
		printf "check-oracle-python: skipped (no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/python_diff.py --strict

check-oracle-vim: ## Compare the Vim front end against vim itself
# One vim process for the whole run, not one per case: vim reads a file of
# cases and writes a file of answers, which is what makes a differential
# possible against a reference that cannot be imported. `probe.py`'s vim
# driver starts a process per case and is fine for the eighty rows that page
# fills; this asks tens of thousands.
#
# It found a defect in code this dialect does not own, which is the second
# time a new front end has: a lookaround restored the live capture slots when
# its body's path was abandoned and left the *shadow* spans - the ones a
# backreference reads - where the body had written them. `(?=(a))$|(a)\1`
# matched "aa" here and "a" in node, and pcre2test refused it outright.
check-oracle-vim: $(TOOLS)
	@if ! command -v vim >/dev/null 2>&1; then \
		printf "check-oracle-vim: skipped (no vim)\n"; \
		exit 0; \
	fi; \
	if ! command -v python3 >/dev/null 2>&1; then \
		printf "check-oracle-vim: skipped (no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/vim_diff.py --strict

check-oracle-replace: ## Compare every built template grammar against its reference
# WP-16 and WP-22's missing generator. sed_diff.py did this for the POSIX and
# GNU rows; nothing did it for the two largest template grammars, and it
# found six defects in the pcre row alone.
# Four dialects now: `--dialect all` covers ecmascript, pcre, python and vim.
# Python's arm earned its place at once. Its template grammar had twelve
# hand-written tests that all passed, and the generator found six rules they
# had missed - `\u`, `\U`, `\N{...}` and `\x` are pattern escapes there and
# errors in a template, a backslash before a non-alphanumeric keeps *both*
# characters, three octal digits outrank a group reference, and `\12` against
# two groups is an error rather than group 1 followed by "2".
#
# Vim's arm earned its place the same way. It is the only grammar here whose
# template can emit *nothing* and still change the answer - `\u`, `\U` and
# their four relatives - and asking it over generated rows found two rules
# that are not the template's at all: `\<` and `\>` are defined from
# 'iskeyword' rather than from `\w`, and vim's search-all loop advances
# after an empty match and stops at a match reaching the end.
#
# It drives `substitute()` rather than `:s`, for the reason vim_diff.py
# drives `matchstrpos()`: the subject here is a string. Rows where vim's two
# engines disagree are put to `set re=1`, as that tool does.
check-oracle-replace: $(TOOLS)
	@if ! command -v node >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then \
		printf "check-oracle-replace: skipped (no node or no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/replace_diff.py --seed $(ORACLE_SEED)

check-oracle-split: ## Compare grx_regex_split() against ECMAScript's and perl's
# The last documented surface of the substitution API with no generator
# behind it. Its six hand-written tests each assert a rule the author had
# already decided was right; deleting any one of ECMA-262 22.2.6.14's four
# rules from src/subst/subst.c makes this report hundreds of disagreements
# and makes none of those six fail.
#
# Both dialects, since splitting became a profile axis. The perl side earned
# its place immediately: twenty-four hand-written probe cases agreed with
# perl exactly and the generator still found two rules they had missed - a
# zero-width match at the very end of the subject is a separator in perl and
# not in ECMAScript, and perl's trailing-empty drop removes *elements* rather
# than fields, an unset capture among them.
check-oracle-split: $(TOOLS)
	@if ! command -v node >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then \
		printf "check-oracle-split: skipped (no node or no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/split_diff.py --seed $(ORACLE_SEED)
	@if ! command -v perl >/dev/null 2>&1; then \
		printf "check-oracle-split: perl half skipped (no perl)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/split_diff.py --dialect perl --seed $(ORACLE_SEED)
	@# Python's is a third rule and not a blend of the other two, so it needs
	@# its own arm: the empty-subject and trailing-field halves are
	@# ECMAScript's, `maxsplit` is perl's, and every match separates - which
	@# neither of the others does. Six hand-written cases in tests/unit
	@# agreed with `re` while 1,848 generated rows did not.
	python3 tools/oracle/split_diff.py --dialect python --seed $(ORACLE_SEED)

check-oracle-window: ## Compare the search window and its flags against pcre2
# The six fields of GRX_SearchOptions that decide an answer - begin, end and
# the four subject-side flags - sat at one value in every other differential
# this suite runs. Varying them found that NOTBOL and NOTEOL suppressed the
# subject anchors as well as the line anchors, which neither PCRE2 nor glibc
# does.
check-oracle-window: $(TOOLS)
	@if ! command -v python3 >/dev/null 2>&1; then \
		printf "check-oracle-window: skipped (no python3)\n"; \
		exit 0; \
	fi; \
	if [ ! -x "$(PCRE2_MATCH)" ]; then \
		printf "check-oracle-window: skipped (pcre2 is not available)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/window_diff.py --seed $(ORACLE_SEED)

check-oracle-iterate: ## Compare the search-all loop against node and perl
# grx_regex_search_next() is the one entry point whose answer is a sequence,
# and nothing generated asked it anything: match_diff.py stops at the first
# match, and replacement and splitting report text and pieces, so a different
# set of matches producing the same output is invisible to them. Both oracles
# run their own loop - matchAll and `while (/$re/g)` - which is why not pcre2,
# whose loop its caller writes.
check-oracle-iterate: $(TOOLS)
	@if ! command -v node >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then \
		printf "check-oracle-iterate: skipped (no node or no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/iterate_diff.py --seed $(ORACLE_SEED)

check-oracle-sed: ## Compare the POSIX and GNU replacement templates against sed
check-oracle-sed: $(TOOLS)
	@if ! command -v sed >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then \
		printf "check-oracle-sed: skipped (no sed or no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/sed_diff.py

check-oracle-string-properties: ## Compare the properties of strings against the reference
check-oracle-string-properties: $(TOOLS)
	@if ! command -v node >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then \
		printf "check-oracle-string-properties: skipped (no node or no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/string_property_diff.py

vectors: ## Regenerate every dialect's conformance vectors
vectors: vectors-ecmascript vectors-pcre vectors-perl vectors-posix

vectors-ecmascript: ## Regenerate the ECMAScript vectors (needs node)
	@if ! command -v node >/dev/null 2>&1; then \
		printf "vectors-ecmascript: skipped (no node)\n"; exit 0; \
	fi; \
	python3 tools/oracle/make_vectors.py; \
	python3 tools/oracle/make_long_vectors.py; \
	if [ -d third_party/test262 ]; then \
		python3 tools/corpus/import_test262.py; \
	else \
		printf "test262 not fetched: run tools/corpus/fetch.sh test262\n"; \
	fi

vectors-pcre: ## Re-import PCRE2's testinput corpus (needs pcre2test)
	@if ! command -v pcre2test >/dev/null 2>&1; then \
		printf "vectors-pcre: skipped (no pcre2test)\n"; exit 0; \
	fi; \
	if [ ! -d third_party/pcre2 ]; then \
		printf "vectors-pcre: skipped (run tools/corpus/fetch.sh pcre2)\n"; \
		exit 0; \
	fi; \
	python3 tools/corpus/import_pcre2test.py

vectors-posix: ## Re-import Spencer's test set, answered by glibc and by musl
vectors-posix: $(TOOLS)
	@if [ ! -d third_party/glibc ]; then \
		printf "vectors-posix: skipped (run tools/corpus/fetch.sh glibc)\n"; \
		exit 0; \
	fi; \
	python3 tools/corpus/import_rxspencer.py \
		--driver $(APP_DIR)/tools/posix_match$(EXE_EXTENSION)

vectors-perl: ## Re-import Perl's re_tests corpus (needs perl)
	@if ! command -v perl >/dev/null 2>&1; then \
		printf "vectors-perl: skipped (no perl)\n"; exit 0; \
	fi; \
	if [ ! -d third_party/perl ]; then \
		printf "vectors-perl: skipped (run tools/corpus/fetch.sh perl)\n"; \
		exit 0; \
	fi; \
	python3 tools/corpus/import_re_tests.py

# Only the one tool, not $(TOOLS): this is a gate, and a gate that first
# builds every oracle in the tree is one people learn to skip. It is also a
# conditional prerequisite, because the runner links `text` and a build
# without `text` cannot make it - the recipe is then reached with nothing
# built, and says why instead of letting a link error explain it.
ifdef HAVE_TEXT
check-json-schema-suite: $(APP_DIR)/tools/grx_json_schema$(EXE_EXTENSION)
endif

check-json-schema-suite: ## Run JSON-Schema-Test-Suite's pattern files through `text`
check-json-schema-suite:
	@if [ -z "$(HAVE_TEXT)" ]; then \
		printf "\033[0;31m\n### check-json-schema-suite: ghoti.io-text is not installed ###\033[0m\n" >&2; \
		printf "\npkg-config cannot find $(TEXT_PC), so the adapter WP-11 is about\n" >&2; \
		printf "cannot be built, and nothing checks that this library's matcher\n" >&2; \
		printf "answers correctly through it. This used to print \"skipped\" and\n" >&2; \
		printf "exit 0, which is how the seam stayed unverified.\n\n" >&2; \
		printf "  ./bootstrap.sh                        # installs text into .local\n" >&2; \
		printf "  make test PREFIX=<prefix>             # ...then test against it\n\n" >&2; \
		printf "If this machine genuinely has no \`text\`, clear the gate for the\n" >&2; \
		printf "run, so that the choice is visible in the command rather than\n" >&2; \
		printf "in the output of one that looked like it passed:\n\n" >&2; \
		printf "  make test TEST_GATES='\$$(filter-out check-json-schema-suite,\$$(ALL_TEST_GATES))'\n\n" >&2; \
		exit 1; \
	fi; \
	dir="$(JSON_SCHEMA_SUITE)/tests/$(JSON_SCHEMA_DRAFT)"; \
	if [ ! -d "$$dir" ]; then \
		printf "\033[0;31m\n### check-json-schema-suite: the suite is not here ###\033[0m\n" >&2; \
		printf "\n%s\n" "$$dir" >&2; \
		printf "\nThe corpus is not committed - it is somebody else's, and a copy\n" >&2; \
		printf "here would stop being what everyone else is measured against.\n" >&2; \
		printf "The commit is pinned instead, so fetching is reproducible:\n\n" >&2; \
		printf "  tools/jsonschema/fetch.sh\n\n" >&2; \
		exit 1; \
	fi; \
	files=""; \
	for f in $(JSON_SCHEMA_FILES); do files="$$files $$dir/$$f.json"; done; \
	$(APP_DIR)/tools/grx_json_schema$(EXE_EXTENSION) --expect-passed $(JSON_SCHEMA_EXPECT) $$files

# Where tools/jsonschema/fetch.sh puts the corpus, and which draft's files are
# run. The suite is not committed - it is somebody else's, and a copy here
# would be a snapshot that stops being what everyone else is measured against -
# so the commit is pinned instead, and a published pass rate names it.
# 2020-12 is the draft `text`'s keyword set is written against; draft7 is
# fetched too and differs here only in `items`.
JSON_SCHEMA_COMMIT := $(shell cat tools/jsonschema/SUITE_COMMIT 2>/dev/null)
JSON_SCHEMA_SUITE ?= third_party/json-schema-test-suite/$(JSON_SCHEMA_COMMIT)
JSON_SCHEMA_DRAFT ?= draft2020-12

# Which of the suite's files to run. The two WP-11 is about, plus the two
# other keywords that measure a string: the first version of this check ran
# only the pattern files, and so could not have caught a defect in the pair it
# exists to validate - which is exactly what maxLength.json then found.
JSON_SCHEMA_FILES ?= pattern patternProperties maxLength minLength

# How many assertions those files make. Named because the exit status alone
# only says that nothing answered *wrongly*, and an empty file, an unfetched
# corpus and a group skipped for a keyword `text` stopped implementing all
# satisfy that. The suite commit is pinned, so this is a constant; it moves
# in the same commit that moves tools/jsonschema/SUITE_COMMIT. It counts the
# files above, and on the draft - `JSON_SCHEMA_DRAFT=draft7` is a documented
# way to run this, so both counts are here and neither goes stale unnoticed.
# Overriding JSON_SCHEMA_FILES means overriding this too, and the check says
# so rather than quietly measuring something else.
JSON_SCHEMA_EXPECT ?= $(if $(filter draft7,$(JSON_SCHEMA_DRAFT)),46,51)

check-limits: ## Report what real patterns cost against grx_limits_default()
check-limits: $(TOOLS)
	@if ! command -v python3 >/dev/null 2>&1; then \
		printf "check-limits: skipped (no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/limits/measure.py \
		--driver $(APP_DIR)/tools/grx_limits$(EXE_EXTENSION) \
		--matcher $(APP_DIR)/tools/grx_match$(EXE_EXTENSION)

check-engine-equivalence: ## Fail if two engines disagree about one program
# Only the one driver, not $(TOOLS): this check consults no reference
# implementation, and `make test` should not have to build the tools that do.
check-engine-equivalence: $(APP_DIR)/tools/grx_match$(EXE_EXTENSION)
	@if ! command -v python3 >/dev/null 2>&1; then \
		printf "check-engine-equivalence: skipped (no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/engine_diff.py --seed $(ORACLE_SEED) \
		--patterns $(ORACLE_PATTERNS) --driver $(APP_DIR)/tools/grx_match$(EXE_EXTENSION)

check-oracle-match: ## Compare what patterns match against the reference implementation
check-oracle-match: $(TOOLS)
	@if ! command -v node >/dev/null 2>&1 || ! command -v python3 >/dev/null 2>&1; then \
		printf "check-oracle-match: skipped (no node or no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/match_diff.py --seed $(ORACLE_SEED) \
		--patterns $(ORACLE_PATTERNS) --driver $(APP_DIR)/tools/grx_match$(EXE_EXTENSION)

check-oracle-syntax: ## Compare accept/reject against the reference implementation
check-oracle-syntax: $(TOOLS)
	@if ! command -v node >/dev/null 2>&1; then \
		printf "check-oracle-syntax: skipped (no node)\n"; \
		exit 0; \
	fi; \
	if ! command -v python3 >/dev/null 2>&1; then \
		printf "check-oracle-syntax: skipped (no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/oracle/syntax_diff.py --seed $(ORACLE_SEED) \
		--count $(ORACLE_COUNT) --driver $(APP_DIR)/tools/grx_syntax$(EXE_EXTENSION)

examples: ## Build all examples
examples: $(APP_DIR)/$(TARGET) $(EXAMPLES)
	@printf "\033[0;32m\n"
	@printf "############################\n"
	@printf "### Examples built       ###\n"
	@printf "############################\n"
	@printf "\033[0m\n"
	@printf "Examples are available in: $(APP_DIR)/examples/\n"
ifndef HAVE_TEXT
	@printf "\n"
	@printf "  json_schema_provider was skipped: ghoti.io-text is not\n"
	@printf "  installed, and it is the library that example plugs into.\n"
endif
	@printf "\n"
	@printf "\033[0;33mTo run examples:\033[0m\n"
ifeq ($(OS_NAME), Linux)
	@printf "  Linux: Set LD_LIBRARY_PATH to include the library directory:\n"
	@printf "    export LD_LIBRARY_PATH=\"$(TEST_LD_PATH)\"\n"
	@printf "    $(APP_DIR)/examples/<example>\n"
else ifeq ($(OS_NAME), Mac)
	@printf "  macOS: Set DYLD_LIBRARY_PATH to include the library directory:\n"
	@printf "    export DYLD_LIBRARY_PATH=\"$(TEST_LD_PATH)\"\n"
	@printf "    $(APP_DIR)/examples/<example>\n"
else ifeq ($(OS_NAME), Windows)
	@printf "  Windows (MSYS2): The DLL must be in the same directory or in PATH.\n"
	@printf "    cd $(APP_DIR) && ./examples/<example>$(EXE_EXTENSION)\n"
endif
	@printf "\n"

# So the tests can load the regex library and its dependency on cutil. cutil's
# build tree has no release/debug component, so only the leading OS component
# of BUILD applies to it.
TEST_LD_PATH := $(APP_DIR):$(LIB_INSTALL_PATH)/$(SUITE)

####################################################################
# Symbol namespace check
####################################################################

# The files that are *below* the IR, and so may not consult a dialect. Named
# one by one rather than globbed, because three files in these directories
# are legitimately above the line:
#
#   src/compile/compile.c holds the public accessors, and
#   grx_regex_syntax() reporting which dialect a regex came from is
#   reporting rather than branching.
#
#   src/compile/compile_internal.h declares `GRX_Regex::syntax`, the field
#   that reporting reads. Declaring it is not consulting it - which is why
#   the pattern below also catches `->syntax`, so that a file that *is* on
#   this list cannot read the field through the header either.
#
#   src/ir/lower.c is where the dialect is spent, and consulting the profile
#   is its whole job.
BELOW_THE_IR := src/exec/*.c src/exec/*.h src/compile/codegen.c \
	src/compile/program.c

check-diagnostics: ## Fail if a diagnostic exists that no code path produces
	@if ! command -v python3 >/dev/null 2>&1; then \
		printf "check-diagnostics: skipped (no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/check_diagnostics.py

check-layering: ## Fail if an engine knows which dialect it is running
	@leaked=$$(grep -lnE 'GRX_SYNTAX_|GRX_Syntax|grx_syntax_|->syntax|regex/syntax\.h|syntax_internal\.h' \
		$(BELOW_THE_IR) 2>/dev/null || true); \
	if [ -n "$$leaked" ]; then \
		printf "\033[0;31m\n### An engine names a dialect ###\033[0m\n" >&2; \
		printf "%s\n" "$$leaked" >&2; \
		printf "\nThe dialect is resolved away by lowering: every dialect-dependent\n" >&2; \
		printf "decision reaches an engine as an explicit opcode, mode or class index.\n" >&2; \
		printf "An engine that consults GRX_Syntax has moved a dialect decision below\n" >&2; \
		printf "the IR, where every dialect shares it. The construct it needs is\n" >&2; \
		printf "missing from the IR; add it there.\n" >&2; \
		printf "See documentation/design.md section 3 and section 9 invariant 1.\n" >&2; \
		exit 1; \
	fi
	@printf "\033[0;32mNo engine names a dialect.\033[0m\n"

####################################################################
# Unicode table check
####################################################################

# Where the UCD lands and where the generator's output is committed. Both are
# named here rather than inside the recipe so that a reader can see what the
# check compares without reading the shell.
UCD_VERSION := $(shell cat tools/unicode/UCD_VERSION 2>/dev/null)
UCD_DIR := third_party/ucd/$(UCD_VERSION)
UNICODE_TABLES := src/unicode/tables

check-dump-names: ## Fail if a dump's name table is shorter than its enum
# Every dump here turns an enumerator into a word through a positional
# table, and a name left out does not leave a hole at the end - it shifts
# every name after it onto its neighbour. The compiler cannot see it (the
# array is sized by the COUNT and the tail is NULL) and the lookups all
# guard and return "?", so nothing warns. Seven of twenty-eight tables were
# short when this was written, one of them for long enough that every IR
# assertion dumped under the wrong name.
	@if ! command -v python3 >/dev/null 2>&1; then \
		printf "check-dump-names: skipped (no python3)\n"; \
		exit 0; \
	fi; \
	python3 tools/check_dump_names.py

check-readme-example: ## Fail if the README's example does not compile and run
# tests/unit/test_docs.cpp asks whether the example's pattern and subject
# still answer what the prose says, and it cannot ask whether the code
# *compiles*: a test binary has no compiler in it. That was the half of the
# check that was missing, and it is the half an API change breaks - a
# renamed field or a changed signature leaves the page saying something
# that has not built for months.
#
# Compiled against the tree rather than an installed prefix, which is the
# one difference from what a reader would do: they would ask pkg-config,
# and pkg-config would hand them these same headers.
check-readme-example: $(APP_DIR)/$(STATIC_TARGET) | $(APP_DIR)/$(TARGET)
	@mkdir -p $(BUILD_DIR)/docs
	@awk '/^## Example/{found=1} found && /^```c$$/{copy=1; next} \
		copy && /^```$$/{exit} copy' README.md > $(BUILD_DIR)/docs/readme_example.c
	@if [ ! -s $(BUILD_DIR)/docs/readme_example.c ]; then \
		printf "\033[0;31m\n### The README has no example to compile ###\033[0m\n" >&2; \
		exit 1; \
	fi
	@$(CC) $(CFLAGS) $(INCLUDE) -o $(BUILD_DIR)/docs/readme_example \
		$(BUILD_DIR)/docs/readme_example.c $(LDFLAGS) $(REGEXLIBRARY) \
		$(CUTIL_LIBS) || { \
		printf "\033[0;31m\n### The README's example does not compile ###\033[0m\n" >&2; \
		exit 1; \
	}
	@out=$$(LD_LIBRARY_PATH="$(TEST_LD_PATH)" $(BUILD_DIR)/docs/readme_example); \
	if [ "$$out" != "Corey" ]; then \
		printf "\033[0;31m\n### The README's example printed %s, not Corey ###\033[0m\n" "$$out" >&2; \
		exit 1; \
	fi; \
	printf "The README example compiles, runs and prints Corey.\n"

check-unicode-tables: ## Fail if the committed Unicode tables are not what the generator produces
	@if ! command -v python3 >/dev/null 2>&1; then \
		printf "check-unicode-tables: skipped (no python3)\n"; \
		exit 0; \
	fi; \
	if ! python3 tools/unicode/test_gen.py >/dev/null 2>&1; then \
		printf "\033[0;31m\n### The Unicode generator's own tests fail ###\033[0m\n" >&2; \
		python3 tools/unicode/test_gen.py >&2 || true; \
		exit 1; \
	fi; \
	if [ ! -d "$(UCD_DIR)" ]; then \
		printf "check-unicode-tables: generator tests pass; table diff skipped (no $(UCD_DIR); run tools/unicode/fetch.sh)\n"; \
		exit 0; \
	fi; \
	tmp=$$(mktemp -d) || exit 1; \
	trap 'rm -rf "$$tmp"' EXIT; \
	mkdir -p "$$tmp/out"; \
	if ! python3 tools/unicode/gen_tables.py --out "$$tmp/out" >/dev/null 2>"$$tmp/err"; then \
		printf "\033[0;31m\n### The Unicode generator failed ###\033[0m\n" >&2; \
		cat "$$tmp/err" >&2; \
		exit 1; \
	fi; \
	if ! diff -ru $(UNICODE_TABLES) "$$tmp/out" >"$$tmp/diff" 2>&1; then \
		printf "\033[0;31m\n### The committed Unicode tables are stale ###\033[0m\n" >&2; \
		head -40 "$$tmp/diff" >&2; \
		printf "\nThe tables under $(UNICODE_TABLES) are committed so that a build needs\n" >&2; \
		printf "neither the network nor Python, which means they can drift from the\n" >&2; \
		printf "generator that is supposed to produce them. Regenerate with:\n" >&2; \
		printf "  tools/unicode/gen_tables.py\n" >&2; \
		printf "See documentation/unicode.md section 4.\n" >&2; \
		exit 1; \
	fi; \
	printf "\033[0;32mUnicode tables are byte-identical to the generator's output (UCD $(UCD_VERSION)).\033[0m\n"

check-symbols: ## Fail if any exported symbol lacks the version namespace
check-symbols: $(APP_DIR)/$(TARGET)
ifeq ($(OS_NAME), Linux)
	@leaked=$$(nm -D --defined-only $(APP_DIR)/$(TARGET) \
		| awk '$$2 ~ /^[TDBR]$$/ {print $$3}' \
		| grep -v '^$(LIBVER_SYMBOL)_' | grep -v '^_' || true); \
	if [ -n "$$leaked" ]; then \
		printf "\033[0;31m\n### Exported symbols missing the $(LIBVER_SYMBOL)_ namespace ###\033[0m\n" >&2; \
		printf "%s\n" "$$leaked" >&2; \
		printf "\nEach needs a '#define <name> GHOTIIO_REGEX(<name>)' line in namespace.h.\n" >&2; \
		printf "See CONVENTIONS.md section 4.\n" >&2; \
		exit 1; \
	fi
	@unexported=$$(find include -name '*.h' -exec awk '/^#if DOXYGEN/{d=1} d==0 && /^[a-z_][A-Za-z0-9_ ]*\**[[:space:]]*grx_[a-z0-9_]+[[:space:]]*\(/{print FILENAME": "$$0} /^#endif/{d=0}' {} + \
		| grep -vE 'typedef|static inline' || true); \
	if [ -n "$$unexported" ]; then \
		printf "\033[0;31m\n### Public declarations without GRX_API ###\033[0m\n" >&2; \
		printf "%s\n" "$$unexported" >&2; \
		printf "\nThese are hidden in the shared library. The tests link the archive and\n" >&2; \
		printf "would not notice; a consumer gets an undefined reference.\n" >&2; \
		exit 1; \
	fi
	@split=$$(nm -D --undefined-only $(APP_DIR)/$(TARGET) \
		| awk '{print $$2}' | grep '^$(LIBVER_SYMBOL)_' || true); \
	if [ -n "$$split" ]; then \
		printf "\033[0;31m\n### Renamed but undefined - a split symbol ###\033[0m\n" >&2; \
		printf "%s\n" "$$split" >&2; \
		printf "\nA translation unit referenced the namespaced name while the one that\n" >&2; \
		printf "defines it did not see the rename - usually an internal header that\n" >&2; \
		printf "declares or defines something without including macros.h first.\n" >&2; \
		exit 1; \
	fi
	@nomacros=$$(find include src -name '*.h' \
		! -name 'libver.h' ! -name 'libver_gen.h' ! -name 'namespace.h' ! -name 'macros.h' \
		-exec grep -L '#include <ghoti.io/regex/macros.h>' {} + || true); \
	if [ -n "$$nomacros" ]; then \
		printf "\033[0;31m\n### Headers that do not include macros.h ###\033[0m\n" >&2; \
		printf "%s\n" "$$nomacros" >&2; \
		printf "\nEvery header must include <ghoti.io/regex/macros.h> before it declares\n" >&2; \
		printf "anything, so that the renames in namespace.h are already in effect. A\n" >&2; \
		printf "header that skips it can name a type before that type has been renamed,\n" >&2; \
		printf "producing two different types under one spelling.\n" >&2; \
		printf "See CONVENTIONS.md section 4.\n" >&2; \
		exit 1; \
	fi
	@badguards=$$(find include src -name '*.h' -exec awk 'FNR==1{d=0} !d && /^#ifndef/{print $$2; d=1}' {} + \
		| awk '$$1 !~ /^GHOTI_IO_GRX_/ {print $$1}' || true); \
	if [ -n "$$badguards" ]; then \
		printf "\033[0;31m\n### Include guards with the wrong prefix ###\033[0m\n" >&2; \
		printf "%s\n" "$$badguards" >&2; \
		printf "\nGuards mirror the path: GHOTI_IO_GRX_<PATH>_H. A guard without the\n" >&2; \
		printf "library token is one rename away from colliding with another library's.\n" >&2; \
		exit 1; \
	fi
	@dupguards=$$(find include src -name '*.h' -exec awk 'FNR==1{d=0} !d && /^#ifndef/{print $$2; d=1}' {} + \
		| sort | uniq -d || true); \
	if [ -n "$$dupguards" ]; then \
		printf "\033[0;31m\n### Headers sharing an include guard ###\033[0m\n" >&2; \
		printf "%s\n" "$$dupguards" >&2; \
		printf "\nTwo headers with one guard means whichever is included second is\n" >&2; \
		printf "silently empty. Guards mirror the path: GHOTI_IO_GRX_<PATH>_H.\n" >&2; \
		exit 1; \
	fi
	@printf "\033[0;32mEvery exported symbol carries the $(LIBVER_SYMBOL)_ namespace.\033[0m\n"
	@printf "\033[0;32mEvery public declaration carries GRX_API.\033[0m\n"
	@printf "\033[0;32mEvery header includes macros.h.\033[0m\n"
	@printf "\033[0;32mEvery include guard is unique and correctly prefixed.\033[0m\n"
else
	@printf "check-symbols: skipped (Linux only)\n"
endif

check-aliasing: ## Fail if the strict-aliasing warning is no longer armed
# $(ALIASING_CFLAGS) detects the violations; this proves it can still detect
# one. The flags live in CFLAGS under -Werror, so a real violation fails the
# build and no sweep is needed - but a DISARMED warning fails nothing and
# looks exactly like a clean library. This library compiles clean at level 1
# today, which means every single thing the gate has to say is said by
# whether it can still refuse a violation it plants itself.
#
# Deliberately the real $(CFLAGS), not a copy. A control compiled with flags
# written out beside it proves those flags work, which is not the question.
#
# THE SHAPE OF THE CONTROL IS LOAD-BEARING. What each level diagnoses depends
# on the violation, and only some shapes separate level 1 from the rest.
# Measured here with this tree's cc (Debian gcc 14.2.0) at -O2, counts of the
# diagnostic:
#
#                                              L0  L1  L2  L3
#   *(int *)&obj      known object, in place    0   1   1   1
#   int *p = (int *)&obj; *p                    0   1   1   0
#   int *p = (int *)f; *p   f a PARAMETER       0   1   0   0   <- this one
#   punning through a void *                    0   0   0   0
#
# Two axes: taking the address of an object the compiler can see is what
# level 2 needs, and routing the cast through a separate pointer variable is
# what defeats level 3.
#
# THE REQUIREMENT, for anyone changing the level or the control: a control
# for a gate at level N must be caught at N and MISSED at N+1. A control that
# survives into the weaker level still passes after the gate has silently
# fallen back to it, which is indistinguishable from working. This control is
# a parameter cast through a variable - caught at 1, missed at 2 and 3 - so
# it certifies level 1 specifically. Raise the level and it must be
# respelled, or the gate passes green while asserting nothing. The row to
# respell it to is the second: the known object through a pointer variable is
# the only shape that certifies "2 and not 3". The first row is useless as a
# probe at any level, because it fires from 1 upward and so distinguishes
# nothing - which is the trap, since it is also the most natural way to write
# a type pun. The last row is the standing limit: no level catches punning
# through a void *, so a clean build is not evidence about that class at all.
#
# The warning is a gcc diagnostic. clang accepts -Wstrict-aliasing=0, =1 and
# =2 in silence and implements nothing behind them - measured here: the
# control passes clang at every level it accepts, and =3 it rejects outright
# as an unknown option. So `make CC=clang` reaches this gate with the
# aliasing flags on every compile line and no aliasing coverage behind them.
# That is a true failure and the gate reports it, but the cause is the
# compiler rather than the flags, so the message separates the two.
check-aliasing: $(LIBVER_GEN)
	@mkdir -p $(BUILD_DIR)
	@printf '%s\n' \
		'#include <stdint.h>' \
		'int32_t grx_alias_control(float * f) {' \
		'  int32_t * p = (int32_t *)f;' \
		'  *f = 1.0f;' \
		'  return *p;' \
		'}' > $(BUILD_DIR)/alias_control.c
# qrc below is read on the same line the compiler runs on, and must stay
# there. Any $(...) evaluated in between - including one building the very
# message that reports the status - replaces $? with the subshell's, and the
# clang branch stops being selected. Adding a substitution to the lines above
# it looks like editing prose.
	@if $(CC) $(CFLAGS) $(INCLUDE) -fsyntax-only \
			$(BUILD_DIR)/alias_control.c 2> $(BUILD_DIR)/alias_control.log; then \
		qout=$$($(CC) -Q --help=warnings $(CFLAGS) 2>/dev/null); qrc=$$?; \
		lvl=$$(printf '%s\n' "$$qout" \
			| awk '/-Wstrict-aliasing=<0,3>/ { print $$2 }'); \
		printf "\033[0;31mcheck-aliasing: %s accepted a planted type-punning violation, so this build has no aliasing coverage.\033[0m\n" "$$($(CC) --version 2>/dev/null | head -1)" >&2; \
		if [ -z "$$lvl" ] && [ "$$qrc" = "0" ]; then \
			printf '%s\n' \
				'  -Q --help=warnings succeeded and named no -Wstrict-aliasing level at all, which is' \
				'  neither compiler behaviour seen here. Do NOT read this as the clang case: check what' \
				'  CFLAGS was actually passed before concluding anything about the warning.' >&2; \
		elif [ -z "$$lvl" ]; then \
			printf '%s\n' \
				'  This compiler would not report an effective -Wstrict-aliasing level, which gcc gives' \
				'  through -Q --help=warnings. Expect clang: it accepts -fstrict-aliasing' \
				'  -Wstrict-aliasing=1 in silence and implements no such diagnostic, so the flags ride' \
				'  every compile line of a clang build while detecting nothing. regex aliasing coverage' \
				'  is gcc-only, and a clang run does not have it.' >&2; \
		elif [ "$$lvl" = "1" ]; then \
			printf '%s\n' \
				'  The effective level is 1, which is the level that catches this violation. So the' \
				'  flags are right and the compiler is not implementing them - that is clang, which' \
				'  accepts -Wstrict-aliasing=1 in silence. regex aliasing coverage is gcc-only.' >&2; \
		else \
			printf '  The effective -Wstrict-aliasing level is %s, and only level 1 diagnoses this control.\n' "$$lvl" >&2; \
			printf '%s\n' \
				'  Levels 0, 2 and 3 are all silent on it, measured against this same file - so the' \
				'  warning is at the WRONG LEVEL rather than missing, and ALIASING_CFLAGS is likely' \
				'  untouched. What overrides it is a later EXPLICIT level, since an explicit level beats' \
				'  the 3 that -Wall implies from either side. CFLAGS ends with EXTRA_CFLAGS, so' \
				'  EXTRA_CFLAGS=-Wstrict-aliasing=3 does exactly this. Note that 3 is also what -Wall' \
				'  implies on its own, so a level of 3 is equally what removing ALIASING_CFLAGS looks' \
				'  like; 0 and 2 can only have been asked for.' >&2; \
		fi; \
		exit 1; \
	fi
# A compile that failed for some other reason - a missing header, a typo in
# the printf above - would otherwise read as the gate passing, since the
# `if` only asks whether the compiler was happy.
	@if ! grep -q 'strict-aliasing' $(BUILD_DIR)/alias_control.log; then \
		printf "\033[0;31mcheck-aliasing: the control failed to compile, but not for aliasing - so this says nothing about whether the warning is armed:\033[0m\n" >&2; \
		cat $(BUILD_DIR)/alias_control.log >&2; \
		exit 1; \
	fi
	@printf "\033[0;32mA planted type-punning violation is still refused by the library's own flags.\033[0m\n"

test: ## Make and run the unit tests
# The loop used to end with the test run itself, so the recipe exited with the
# status of the LAST binary and every failure before it printed and was
# discarded - `make test` reported success with a failing suite. Verified by
# breaking an early test deliberately: the suite exited 0. compress carries
# the same comment, having been fixed first; this is that fix swept here.
#
# Failures are collected rather than stopping at the first, so one run names
# every suite that failed.
test: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES) $(TEST_GATES)
	@failed=""; \
	for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		printf "\033[0;30;43m\n"; \
		printf "############################\n"; \
		printf "### Running %s tests ###\n" "$$test_name"; \
		printf "############################"; \
		printf "\033[0m\n\n"; \
		if ! LD_LIBRARY_PATH="$(TEST_LD_PATH)" $$test_exe --gtest_brief=1; then \
			failed="$$failed $$test_name"; \
		fi; \
	done; \
	if [ -n "$$failed" ]; then \
		printf "\033[0;31m\n### Failing suites:%s ###\033[0m\n" "$$failed" >&2; \
		exit 1; \
	fi

test-quiet: ## Run tests with minimal output (one line per test suite)
test-quiet: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES) $(TEST_GATES)
	@total_tests=0; total_passed=0; total_failed=0; total_time=0; failed_suites=""; \
	failed_count=0; \
	printf "\n\033[1;36m%-30s %8s %10s %s\033[0m\n" "Test Suite" "Tests" "Time" "Status"; \
	printf "\033[1;36m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		output=$$(LD_LIBRARY_PATH="$(TEST_LD_PATH)" $$test_exe --gtest_brief=1 2>&1); \
		exit_code=$$?; \
		num_tests=$$(echo "$$output" | grep -oP '\[\s*=+\s*\]\s*\K\d+(?=\s+tests?)' | head -1); \
		time_ms=$$(echo "$$output" | grep -oP '\(\K\d+(?=\s*ms\s*total\))' | head -1); \
		[ -z "$$num_tests" ] && num_tests=0; \
		[ -z "$$time_ms" ] && time_ms=0; \
		total_tests=$$((total_tests + num_tests)); \
		total_time=$$((total_time + time_ms)); \
		if [ $$exit_code -eq 0 ]; then \
			total_passed=$$((total_passed + num_tests)); \
			printf "%-30s %8d %8dms \033[0;32mPASS\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
		else \
			failures=$$(echo "$$output" | grep -oP '\[\s*FAILED\s*\]\s*\K\d+' | head -1); \
			[ -z "$$failures" ] && failures=$$num_tests; \
			total_failed=$$((total_failed + failures)); \
			failed_count=$$((failed_count + 1)); \
			total_passed=$$((total_passed + num_tests - failures)); \
			printf "%-30s %8d %8dms \033[0;31mFAIL\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
			failed_suites="$$failed_suites\n\033[0;31m=== $$test_name FAILURES ===\033[0m\n$$output\n"; \
		fi; \
	done; \
	printf "\033[1;36m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	if [ $$failed_count -eq 0 ]; then \
		printf "\033[0;32m%-30s %8d %6dms PASS\033[0m\n\n" "TOTAL" "$$total_tests" "$$total_time"; \
	else \
		printf "\033[0;31m%-30s %8d %6dms FAIL (%d failed in %d suites)\033[0m\n" "TOTAL" "$$total_tests" "$$total_time" "$$total_failed" "$$failed_count"; \
		printf "$$failed_suites\n"; \
		exit 1; \
	fi

# The verdict is `failed_count`, which counts *suites that exited non-zero*,
# and not `total_failed`, which counts the failing assertions the output
# reported. A suite that segfaults reports neither a test count nor a
# `[  FAILED  ]` line, so `total_failed` gained nothing from it and the
# whole run printed PASS and exited 0 - with the crashed suite's own line
# right there saying FAIL. Found by a NULL in a name table, which crashed
# testIr and left `make test-quiet` green; `make test` exited 2 throughout,
# so the two spellings disagreed about whether the suite passed. The count
# is a report and the exit status is the verdict, and only one of them can
# be the gate.

test-valgrind: ## Run all tests under valgrind (Linux only)
test-valgrind: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES) $(TEST_GATES)
ifeq ($(OS_NAME), Linux)
	@failed=""; \
	for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		printf "\033[0;30;43m\n"; \
		printf "############################\n"; \
		printf "### Running %s tests under Valgrind ###\n" "$$test_name"; \
		printf "############################"; \
		printf "\033[0m\n\n"; \
		if ! LD_LIBRARY_PATH="$(TEST_LD_PATH)" valgrind $(VALGRIND_FLAGS) $$test_exe --gtest_brief=1; then \
			failed="$$failed $$test_name"; \
		fi; \
	done; \
	if [ -n "$$failed" ]; then \
		printf "\033[0;31m\n### Valgrind found errors in:%s ###\033[0m\n" "$$failed" >&2; \
		exit 1; \
	fi
else
	@printf "\033[0;31m\nValgrind is only available on Linux\n\033[0m\n"
	@exit 1
endif

# test-valgrind-quiet passes only when both the tests pass and Valgrind is
# clean, so a FAIL here can mean an assertion failure even with no leaks.
test-valgrind-quiet: ## Run tests under valgrind with minimal output (Linux only)
test-valgrind-quiet: $(APP_DIR)/$(TARGET) $(TEST_EXECUTABLES) $(TEST_GATES)
ifeq ($(OS_NAME), Linux)
	@total_tests=0; total_failed=0; total_time=0; failed_suites=""; \
	printf "\n\033[1;35m%-30s %8s %10s %s\033[0m\n" "Test Suite (Valgrind)" "Tests" "Time" "Status"; \
	printf "\033[1;35m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	for test_exe in $(TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		output=$$(LD_LIBRARY_PATH="$(TEST_LD_PATH)" valgrind $(VALGRIND_FLAGS) $$test_exe --gtest_brief=1 2>&1); \
		exit_code=$$?; \
		num_tests=$$(echo "$$output" | grep -oP '\[\s*=+\s*\]\s*\K\d+(?=\s+tests?)' | head -1); \
		time_ms=$$(echo "$$output" | grep -oP '\(\K\d+(?=\s*ms\s*total\))' | head -1); \
		[ -z "$$num_tests" ] && num_tests=0; \
		[ -z "$$time_ms" ] && time_ms=0; \
		total_tests=$$((total_tests + num_tests)); \
		total_time=$$((total_time + time_ms)); \
		has_leak=$$(echo "$$output" | grep -c "are definitely lost\|are indirectly lost\|are possibly lost" || true); \
		if [ $$exit_code -eq 0 ] && [ $$has_leak -eq 0 ]; then \
			printf "%-30s %8d %8dms \033[0;32mPASS\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms"; \
		else \
			if [ $$has_leak -gt 0 ]; then status_msg="LEAK"; else status_msg="FAIL"; fi; \
			total_failed=$$((total_failed + 1)); \
			printf "%-30s %8d %8dms \033[0;31m%s\033[0m\n" "$$test_name" "$$num_tests" "$$time_ms" "$$status_msg"; \
			failed_suites="$$failed_suites\n\033[0;31m=== $$test_name FAILURES ===\033[0m\n$$output\n"; \
		fi; \
	done; \
	printf "\033[1;35m%-30s %8s %10s %s\033[0m\n" "------------------------------" "--------" "----------" "------"; \
	if [ $$total_failed -eq 0 ]; then \
		printf "\033[0;32m%-30s %8d %6dms PASS\033[0m\n\n" "TOTAL" "$$total_tests" "$$total_time"; \
	else \
		printf "\033[0;31m%-30s %8d %6dms FAIL (%d suites)\033[0m\n" "TOTAL" "$$total_tests" "$$total_time" "$$total_failed"; \
		printf "$$failed_suites\n"; \
		exit 1; \
	fi
else
	@printf "\033[0;31m\nValgrind is only available on Linux\n\033[0m\n"
	@exit 1
endif

####################################################################
# Sanitizer build (ASan + UBSan): separate build dir, run the test suite
####################################################################
# -fno-sanitize-recover=undefined because UBSan is *recoverable* by default:
# without it a violation prints "runtime error: ..." and the process exits 0,
# so the suite reports clean with undefined behaviour in its own log. Verified
# by injecting a signed overflow: it printed and the build passed. The runtime
# options below say the same thing a second way, for a binary run by hand.
# GRX_SANITIZERS is for the tests that measure *time*. A sanitized build is
# about four times slower, so a wall clock bound that is right for a release
# build fails here for a reason that is not a defect. The tests that care
# scale their budget by it rather than being loosened for everybody.
#
# UBSAN_CHECKS is one list feeding both `-fsanitize=` and
# `-fno-sanitize-recover=`, because those two are the halves of one decision
# and writing them separately is how they drift: a check that is *on* and
# still recoverable prints its diagnostic and exits 0, which is the failure
# this block already exists to prevent, wearing a different name.
#
# `float-cast-overflow` is named because gcc's `undefined` group does not
# contain it and clang's does. Converting a float that does not fit an
# integer is undefined behaviour, and under gcc - which is what builds
# `make test-asan` here - nothing was watching for it. Measured rather than
# read: `(int)1e30` with the old flags printed -2147483648 and exited 0;
# with the check on but only `-fno-sanitize-recover=undefined` it printed
# the diagnostic and *still* exited 0; with both halves naming it, it exits
# 1. `bounds-strict` and `pointer-overflow` are already inside gcc 14.2's
# `undefined` and add nothing. `float-divide-by-zero` is deliberately left
# out: IEEE defines it, and it would fire on correct code that records an
# infinity.
UBSAN_CHECKS := undefined,float-cast-overflow
ASAN_UBSAN_FLAGS := -fsanitize=address,$(UBSAN_CHECKS) -fno-sanitize-recover=$(UBSAN_CHECKS) -fno-omit-frame-pointer -g -DGRX_SANITIZERS=1
ASAN_BUILD_DIR := ./build/$(BUILD)-asan
ASAN_OBJ_DIR := $(ASAN_BUILD_DIR)/objects
ASAN_FLAGS_STAMP := $(ASAN_OBJ_DIR)/.flags
ASAN_APP_DIR := $(ASAN_BUILD_DIR)/apps

ASAN_LIBOBJECTS := $(patsubst src/%.c,$(ASAN_OBJ_DIR)/%.o,$(SOURCES))
ASAN_TEST_HELPER_OBJ := $(patsubst tests/%.cpp,$(ASAN_OBJ_DIR)/tests/%.o,$(TEST_HELPER_SRC))
ASAN_TARGET := $(BASE_NAME_PREFIX)-asan.$(LIB_EXTENSION)
ASAN_REGEXLIBRARY := -L $(ASAN_APP_DIR) -l$(SUITE)-$(PROJECT)$(BRANCH)-asan

# The sanitizer gate *inherits* $(CFLAGS), so it moved from -O0 to -O2 with
# the release build on 2026-09-23 rather than by anyone deciding it should.
# Measured after the move: 38 C translation units at -O2, 32 C++ at -O1, 70
# compile lines, counts summing to the line count.
#
# Left inherited rather than pinned, because the change asked for was the
# release level and pinning would be a second, unasked-for decision. Two
# things a later reader should weigh before leaving it that way:
#
#   - The fuzz build pins its own -O1 (see FUZZ_SAN), so after this change
#     the sanitizer gate and the fuzzers no longer run on the same codegen.
#     A finding in one need not reproduce in the other.
#   - Inheriting was argued for on the ground that latent UB is inert at -O0
#     and live at -O2, so the gate should run at what ships. That is true of
#     the *optimizer* and says nothing about what the sanitizer detects:
#     text measured gcc 14.2 catching heap-use-after-free, stack overflow,
#     signed overflow and float-cast overflow at both -O1 and -O2, and
#     strict-aliasing violations at neither. Aliasing was the named hazard,
#     and no level of this toolchain's sanitizers sees it.
#
# If this is ever pinned, remove $(ASAN_BUILD_DIR) by hand in the same
# change: nothing in the sanitizer flags is a prerequisite of these objects,
# so editing ASAN_UBSAN_FLAGS alone would relink objects built at the old
# level. Editing *this file* is covered - every object rule depends on it.
ASAN_CFLAGS := $(CFLAGS) $(ASAN_UBSAN_FLAGS) -DGRX_BUILD -DGRX_TEST_BUILD
ASAN_CXXFLAGS := $(CXXFLAGS) $(ASAN_UBSAN_FLAGS)
ASAN_LDFLAGS := $(LDFLAGS) $(ASAN_UBSAN_FLAGS)
ifeq ($(UNAME_S), Linux)
	ASAN_CFLAGS += -fPIC
endif

# -MMD -MP here for the same reason as the release build, and it was missing:
# without it a change to a header did not rebuild these objects, so
# `make test-asan` ran the previous build's code. Found when two diagnostics
# were added to an enum and the sanitizer suite failed against a table it had
# compiled before they existed. A stale sanitizer build is worse than no
# sanitizer build, because it reports on something other than the tree.
$(ASAN_OBJ_DIR)/%.o: src/%.c $(ASAN_FLAGS_STAMP)
	@printf "\n### Compiling (ASan+UBSan): $< ###\n"
	@mkdir -p $(@D)
	$(CC) $(ASAN_CFLAGS) $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(ASAN_APP_DIR)/$(ASAN_TARGET): $(ASAN_LIBOBJECTS)
	@printf "\n### Linking ASan+UBSan Regex Library ###\n"
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) -shared -o $@ $^ $(ASAN_LDFLAGS) $(CUTIL_LIBS)

$(ASAN_OBJ_DIR)/tests/%.o: tests/%.cpp $(ASAN_FLAGS_STAMP)
	@printf "\n### Compiling ASan Test: $* ###\n"
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) $(INCLUDE) -Itests -DGRX_TEST_DATA=\"$(TEST_DATA)\" -DGRX_REPO_ROOT=\"$(REGEX_ROOT)\" -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(ASAN_OBJ_DIR)/tests/%.o: tests/conformance/%.cpp $(ASAN_FLAGS_STAMP)
	@printf "\n### Compiling ASan Test: $* ###\n"
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) $(INCLUDE) -Itests -DGRX_TEST_DATA=\"$(TEST_DATA)\" -DGRX_REPO_ROOT=\"$(REGEX_ROOT)\" -c $< -MMD -MP -MF $(@:.o=.d) -o $@

$(ASAN_OBJ_DIR)/tests/%.o: tests/unit/%.cpp $(ASAN_FLAGS_STAMP)
	@printf "\n### Compiling ASan Test: $* ###\n"
	@mkdir -p $(@D)
	$(CXX) $(ASAN_CXXFLAGS) $(INCLUDE) -Itests -DGRX_TEST_DATA=\"$(TEST_DATA)\" -DGRX_REPO_ROOT=\"$(REGEX_ROOT)\" -c $< -MMD -MP -MF $(@:.o=.d) -o $@

define asan-test-executable-rule
ASAN_TEST_OBJ_$1 := $(ASAN_OBJ_DIR)/tests/$(basename $(notdir $1)).o

$(ASAN_APP_DIR)/$2$(EXE_EXTENSION): $$(ASAN_TEST_OBJ_$1) $(ASAN_TEST_HELPER_OBJ) \
		$(ASAN_APP_DIR)/$(ASAN_TARGET)
	@printf "\n### Linking ASan Test: $2 ###\n"
	@mkdir -p $$(@D)
	$(CXX) $(ASAN_CXXFLAGS) -o $$@ $$(ASAN_TEST_OBJ_$1) $(ASAN_TEST_HELPER_OBJ) $(ASAN_LDFLAGS) $(ASAN_REGEXLIBRARY) $(CUTIL_LIBS) $(TESTFLAGS)
endef

$(foreach pair,$(TEST_PAIRS),\
	$(eval $(call asan-test-executable-rule,$(word 1,$(subst |, ,$(pair))),$(word 2,$(subst |, ,$(pair))))))

ASAN_TEST_EXECUTABLES := $(addprefix $(ASAN_APP_DIR)/,$(addsuffix $(EXE_EXTENSION),$(TEST_NAMES)))

# ASan insists on being the first library loaded. A desktop session that sets
# LD_PRELOAD for its own reasons (libgtk3-nocsd, for instance) puts something
# ahead of it and every sanitized binary refuses to start, so put the runtime
# back in front rather than discarding whatever the user had set.
ASAN_RUNTIME := $(shell $(CC) -print-file-name=libasan.so 2>/dev/null)

test-asan: ## Build with ASan+UBSan and run the test suite
test-asan: $(ASAN_TEST_EXECUTABLES)
	@for test_exe in $(ASAN_TEST_EXECUTABLES); do \
		test_name=$$(basename $$test_exe $(EXE_EXTENSION)); \
		printf "\033[0;30;43m\n### Running %s (ASan+UBSan) ###\033[0m\n\n" "$$test_name"; \
		LD_PRELOAD="$(ASAN_RUNTIME)$${LD_PRELOAD:+:$$LD_PRELOAD}" \
		LD_LIBRARY_PATH="$(ASAN_APP_DIR):$(LIB_INSTALL_PATH)/$(SUITE)" \
		ASAN_OPTIONS=detect_leaks=1:halt_on_error=1 \
		UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1 \
			$$test_exe --gtest_brief=1 || exit 1; \
	done
	@printf "\033[0;32m\nASan+UBSan suite clean.\033[0m\n"

####################################################################
# Fuzzing (libFuzzer)
####################################################################
#
# The library is rebuilt with -fsanitize=fuzzer-no-link rather than linking the
# ordinary shared library. That matters: libFuzzer steers its mutations by the
# coverage it observes, and a harness linked against an uninstrumented library
# sees none of the parser's branches, which leaves it generating random input
# rather than exploring the format.
FUZZ_CC ?= clang
FUZZ_CXX ?= clang++
FUZZ_CC_OK := $(shell which $(FUZZ_CC) 2>/dev/null)
# -fno-sanitize-recover=undefined for the same reason the ASan build has it,
# and it matters more here: a recoverable UBSan diagnostic is not a crash, so
# libFuzzer prints it once, keeps going, writes no artifact and exits 0. A
# fuzzer that finds undefined behaviour and discards it is worse than no
# fuzzer. Verified by injecting a signed overflow on a reachable input.
# The same list, so the fuzzer and the suite cannot disagree about what
# counts as undefined. clang already has `float-cast-overflow` inside
# `undefined`, so naming it changes nothing here and keeps one definition.
# $(ALIASING_ASSUME_CFLAGS) so that the fuzz tree compiles the library under
# the same optimiser assumption the shipped one does. Only the assume half:
# the fuzz compile line carries -w, so it is not a warning gate and the
# -Wstrict-aliasing level would be discarded. FUZZ_CC is overridable and
# clang defaults the assumption on from -O1, so naming it changes nothing
# for the default clang tree and everything for a fuzz tree someone points
# at gcc, which is off at -O1. The name is what makes that not matter.
FUZZ_SAN := -fsanitize=address,$(UBSAN_CHECKS) -fno-sanitize-recover=$(UBSAN_CHECKS) -fno-omit-frame-pointer -g -O1 $(ALIASING_ASSUME_CFLAGS)
FUZZ_LIB_FLAGS := $(FUZZ_SAN) -fsanitize=fuzzer-no-link
FUZZ_BIN_FLAGS := $(FUZZ_SAN) -fsanitize=fuzzer
FUZZ_DIR := $(BUILD_DIR)/fuzz
FUZZ_OBJ_DIR := $(FUZZ_DIR)/objects
FUZZ_FLAGS_STAMP := $(FUZZ_OBJ_DIR)/.flags
FUZZ_APP_DIR := $(FUZZ_DIR)/apps
FUZZ_OBJECTS := $(patsubst src/%.c,$(FUZZ_OBJ_DIR)/%.o,$(SOURCES))
FUZZ_CORPUS := tests/fuzz/corpus

# A smoke-test length by default; for a real campaign: make fuzz FUZZ_TIME=3600
FUZZ_TIME ?= 60

# The longest input libFuzzer may build.
#
# It was libFuzzer's own default of 4096 until it was checked, which meant no
# pattern over 4 KB and no subject over 4 KB had ever been fuzzed, against a
# max_pattern_length of 65,536. 64 KB is that limit, so the harness can reach
# the refusal as well as everything under it. It costs less throughput than
# it looks: every harness caps max_steps well below the default, so a long
# subject ends at the step limit rather than scanning to the end.
FUZZ_MAX_LEN ?= 65536

# libFuzzer grows its inputs from short to long over the course of a run, so
# a run that ends in minutes never reaches FUZZ_MAX_LEN. `fuzz-long-<h>`
# turns that off and generates at the full length from the first input. The
# two are complementary and neither replaces the other: length control is
# what makes a short run find shallow bugs quickly, and turning it off is the
# only way the long end is ever reached.
FUZZ_LEN_CONTROL ?= 1

$(FUZZ_OBJ_DIR)/%.o: src/%.c $(FUZZ_FLAGS_STAMP)
	@mkdir -p $(@D)
	@$(FUZZ_CC) $(FUZZ_LIB_FLAGS) -std=c17 -w $(INCLUDE) -c $< -MMD -MP -MF $(@:.o=.d) -o $@

# $1 = harness basename (fuzz_obj), $2 = target suffix (obj)
define fuzz-rule
fuzz-$2: ## Build the $2 fuzz harness (requires clang)
fuzz-$2: $$(FUZZ_APP_DIR)/$1

$$(FUZZ_APP_DIR)/$1: tests/fuzz/$1.cpp $$(FUZZ_OBJECTS)
	@if [ -z "$$(FUZZ_CC_OK)" ]; then \
		echo "fuzzing requires $$(FUZZ_CXX); install clang or set FUZZ_CC/FUZZ_CXX"; \
		exit 1; \
	fi
	@mkdir -p $$(@D) $$(FUZZ_CORPUS)/$2
	@printf "\n### Building fuzz harness: $1 ###\n"
	$$(FUZZ_CXX) $$(FUZZ_BIN_FLAGS) -std=c++20 -w $$(INCLUDE) \
		-o $$@ $$< $$(FUZZ_OBJECTS) $(CUTIL_LIBS)

fuzz-run-$2: ## Run the $2 fuzzer for $$(FUZZ_TIME) seconds
fuzz-run-$2: $$(FUZZ_APP_DIR)/$1
	@mkdir -p $$(FUZZ_CORPUS)/$2
	@printf "\n### Fuzzing $2 for $$(FUZZ_TIME)s ###\n"
	@LD_LIBRARY_PATH="$(TEST_LD_PATH)" $$(FUZZ_APP_DIR)/$1 $$(FUZZ_CORPUS)/$2 \
		-max_total_time=$$(FUZZ_TIME) -max_len=$$(FUZZ_MAX_LEN) \
		-len_control=$$(FUZZ_LEN_CONTROL) -print_final_stats=1

fuzz-long-$2: ## Run the $2 fuzzer at FUZZ_MAX_LEN from the first input
fuzz-long-$2: $$(FUZZ_APP_DIR)/$1
	@mkdir -p $$(FUZZ_CORPUS)/$2
	@printf "\n### Fuzzing $2 for $$(FUZZ_TIME)s at up to $$(FUZZ_MAX_LEN) bytes ###\n"
	@LD_LIBRARY_PATH="$(TEST_LD_PATH)" $$(FUZZ_APP_DIR)/$1 $$(FUZZ_CORPUS)/$2 \
		-max_total_time=$$(FUZZ_TIME) -max_len=$$(FUZZ_MAX_LEN) \
		-len_control=0 -print_final_stats=1
endef

$(eval $(call fuzz-rule,fuzz_pattern,pattern))
$(eval $(call fuzz-rule,fuzz_subject,subject))
$(eval $(call fuzz-rule,fuzz_crossengine,crossengine))

fuzz: ## Build and run every fuzzer for $(FUZZ_TIME) seconds each
fuzz: fuzz-run-pattern fuzz-run-subject fuzz-run-crossengine

fuzz-long: ## Run every fuzzer at full length for $(FUZZ_TIME) seconds each
fuzz-long: fuzz-long-pattern fuzz-long-subject fuzz-long-crossengine

fuzz-clean: ## Remove the fuzz build (keeps the corpus)
	-@rm -rf $(FUZZ_DIR)

####################################################################
# Install / uninstall
####################################################################

# Where the loader configuration fragment is written. Kept overridable so a
# staged or user-prefix install has somewhere to write it.
LDCONF_INSTALL_PATH ?= /etc/ld.so.conf.d

# What goes in the .pc Requires: field. Built from the same variables the
# compile uses, so a dependency on another branch cannot be named one way for
# the build and another way for consumers.
PC_REQUIRES := $(CUTIL_PC)

# Where this project's own .pc file is installed.
PKGCONFIG_INSTALL_PATH ?= $(PC_INSTALL_PATH)


install: ## Install the library globally, requires sudo
# Depends on all: install used to copy whatever happened to be in the build
# directory, so it could install a stale artifact or fail outright on a clean
# tree.
install: all
	# Installing the shared library.
	@mkdir -p $(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Linux)
	@cp $(APP_DIR)/$(TARGET) $(LIB_INSTALL_PATH)/$(SUITE)/
	@ln -f -s $(TARGET) $(LIB_INSTALL_PATH)/$(SUITE)/$(SO_NAME)
	@ln -f -s $(SO_NAME) $(LIB_INSTALL_PATH)/$(SUITE)/$(BASE_NAME)
	# Installing the ld configuration file.
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then mkdir -p $(LDCONF_INSTALL_PATH); fi
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then echo "$(LIB_INSTALL_PATH)/$(SUITE)" > $(LDCONF_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).conf; fi
endif
ifeq ($(OS_NAME), Windows)
# The .dll goes in bin/, where the loader finds it once that directory is on
# PATH - Windows has no rpath. The import library goes where the .pc's -L
# points, lib/$(SUITE)/, as the .so does on Linux; in lib/ no -L named it.
	@mkdir -p $(BIN_INSTALL_PATH) $(LIB_INSTALL_PATH)/$(SUITE)
	@cp $(APP_DIR)/$(TARGET).a $(LIB_INSTALL_PATH)/$(SUITE)/
	@cp $(APP_DIR)/$(TARGET) $(BIN_INSTALL_PATH)/
endif
	# Installing the headers.
	# Removed first: this directory is owned entirely by this project and
	# branch, and copying over the top of it would leave headers behind that
	# have since been renamed or deleted.
	@rm -rf $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	@mkdir -p $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	@if [ -d include/ghoti.io ]; then \
		cp -r include/ghoti.io $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ ; \
	fi
	@if [ -d $(GEN_DIR)/ghoti.io ]; then \
		cp -r $(GEN_DIR)/ghoti.io $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)/ ; \
	fi
	# Installing the pkg-config files.
	@mkdir -p $(PKGCONFIG_INSTALL_PATH)
	@cat pkgconfig/$(SUITE)-$(PROJECT).pc | sed 's/(SUITE)/$(SUITE)/g; s/(PROJECT)/$(PROJECT)/g; s/(BRANCH)/$(BRANCH)/g; s/(VERSION)/$(VERSION)/g; s|(PC_LIB_DIR)|$(PC_LIB_DIR)|g; s|(PC_INCLUDE_DIR)|$(PC_INCLUDE_DIR)|g; s|(REQUIRES)|$(PC_REQUIRES)|g' > $(PKGCONFIG_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).pc
ifeq ($(OS_NAME), Linux)
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then ldconfig >> /dev/null 2>&1; fi
endif
	@echo "Ghoti.io $(PROJECT)$(BRANCH) installed"

uninstall: ## Delete the globally-installed files.  Requires sudo.
ifeq ($(OS_NAME), Linux)
	@rm -f $(LIB_INSTALL_PATH)/$(SUITE)/$(BASE_NAME)*
	@rm -f $(LDCONF_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).conf
endif
ifeq ($(OS_NAME), Windows)
	@rm -f $(LIB_INSTALL_PATH)/$(SUITE)/$(TARGET).a
	@rm -f $(BIN_INSTALL_PATH)/$(TARGET)
endif
	@rm -rf $(INCLUDE_INSTALL_PATH)/$(SUITE)/$(PROJECT)$(BRANCH)
	@rm -f $(PKGCONFIG_INSTALL_PATH)/$(SUITE)-$(PROJECT)$(BRANCH).pc
	@rmdir --ignore-fail-on-non-empty $(INCLUDE_INSTALL_PATH)/$(SUITE)
	@rmdir --ignore-fail-on-non-empty $(LIB_INSTALL_PATH)/$(SUITE)
ifeq ($(OS_NAME), Linux)
	@if [ -n "$(LDCONF_INSTALL_PATH)" ]; then ldconfig >> /dev/null 2>&1; fi
endif
	@echo "Ghoti.io $(PROJECT)$(BRANCH) has been uninstalled"

debug: ## Build the project in DEBUG mode
	make all BUILD=debug

install-debug: ## Install the DEBUG library globally, requires sudo
	make install BUILD=debug

uninstall-debug: ## Delete the DEBUG globally-installed files.  Requires sudo.
	make uninstall BUILD=debug

test-debug: ## Make and run the Unit tests in DEBUG mode
	make test BUILD=debug

test-valgrind-debug: ## Run all tests under valgrind in DEBUG mode (Linux only)
	make test-valgrind BUILD=debug

watch-debug: ## Watch the file directory for changes and compile the target in DEBUG mode
	make watch BUILD=debug

test-watch-debug: ## Watch the file directory for changes and run the unit tests in DEBUG mode
	make test-watch BUILD=debug

docs: ## Generate the documentation in the ./docs subdirectory
	doxygen

docs-pdf: docs ## Generate the documentation as a pdf, at ./docs/(SUITE)-(PROJECT)(BRANCH).pdf
	cd ./docs/latex/ && make
	mv -f ./docs/latex/refman.pdf ./docs/$(SUITE)-$(PROJECT)$(BRANCH)-docs.pdf

cloc: ## Count the lines of code used in the project
	cloc src include tests Makefile

coverage: ## Build instrumented, run the tests, and report line coverage
# Cleans first because the object files would otherwise be reused without the
# instrumentation, then cleans and rebuilds at the end: leaving the
# instrumented objects behind would have a later `make` silently link them,
# and leaving the tree cleaned would break any sibling project that links this
# one. The cost is one extra build; coverage is not run often.
	@$(MAKE) --no-print-directory clean > /dev/null
# The instrumented build, the report and the restoration of the tree are one
# shell command so that the cleanup runs whatever fails. Letting a failure
# stop the recipe leaves the --coverage objects in build/, and the next
# ordinary `make` links them into a library that needs the gcov runtime; every
# later build then fails with undefined references to __gcov_init until
# somebody works out why.
#
# TEST_GATES is cleared because --coverage links the gcov runtime, which
# exports mangle_path. check-symbols is right to reject that in a shipping
# build and wrong to reject it here, and it made this target fail before it
# ever produced a report.
	@status=0; \
	$(MAKE) --no-print-directory test TEST_GATES= \
		EXTRA_CFLAGS="--coverage -O0" \
		EXTRA_LDFLAGS="--coverage" > /dev/null || status=$$?; \
	if [ $$status -eq 0 ]; then \
		tools/coverage.sh $(OBJ_DIR) || status=$$?; \
	else \
		printf "coverage: the instrumented test run failed; no report\n" >&2; \
	fi; \
	$(MAKE) --no-print-directory clean > /dev/null; \
	$(MAKE) --no-print-directory all > /dev/null; \
	exit $$status

clean: ## Remove all contents of the build directories.
	-@rm -rvf $(OBJ_DIR)/*
	-@rm -rvf $(APP_DIR)/*
	-@rm -rvf $(GEN_DIR)/*
	-@rm -rvf $(ASAN_BUILD_DIR)
# The fuzz tree too, which it did not remove for as long as the tree existed.
# ASan's was here and fuzz's was not - a sibling directory under the same
# BUILD_DIR, missed when the second of the two was added - so `make clean`
# left objects compiled against the *previous* prefix's headers, and the next
# `fuzz-run-*` linked them against the new .so. Header-resident inline code of
# one vintage against compiled functions of another, arrived at by way of the
# command whose entire job is to prevent it, and a fuzz soak is the worst
# place to find out: a crash would be attributable to nothing.
#
# Safe for the corpus, which is what fuzz-clean's "keeps the corpus" is about:
# FUZZ_CORPUS is tests/fuzz/corpus, outside BUILD_DIR entirely, so no rm here
# can reach the 22,584 accumulated inputs. Checked before this line was
# written rather than after.
	-@rm -rvf $(FUZZ_DIR)
# The debug tree, which this target cannot reach on its own: the four globs
# above name only the *current* BUILD's directories, and a debug build is a
# different one. After a `make BUILD=debug`, a plain `make clean` left 38
# objects and a complete linkable library behind, at -O0 where everything
# else in the tree is now -O2 - read out of the objects' own DW_AT_producer
# rather than inferred.
#
# Harmless until 2026-09-23, when the two build modes stopped sharing an
# optimisation level and the leftovers stopped being indistinguishable from
# what belonged there.
#
# Derived from BUILD_DIR rather than spelled out. This line named
# `./build/debug` when it was written, which was correct then and silently
# wrong ten commits later: `override` on the platform assignments made a
# command-line `BUILD=debug` become `linux/debug`, so the tree moved to
# ./build/linux/debug and a hard-coded path went on naming a directory
# nothing creates. A dead `rm` looks exactly like a live one.
	-@rm -rvf $(dir $(BUILD_DIR))debug

help: ## Display this help
	@grep -E '^[ a-zA-Z_-]+:.*?## .*$$' Makefile | sort | sed 's/\\([^:]*\\):.*## \\(.*\\)/\\1:\\2/' | awk -F: '{printf "%-20s %s\n", $$1, $$2}' | sed "s/(SUITE)/$(SUITE)/g; s/(PROJECT)/$(PROJECT)/g; s/(BRANCH)/$(BRANCH)/g"

####################################################################
# Dependency inclusion for the sanitizer and fuzz builds
#
# Here rather than beside the release build's, because both trees' object
# directories are defined further down this file and `:=` is expanded where
# it is written.
####################################################################

ASAN_DEPFILES := $(ASAN_LIBOBJECTS:.o=.d) \
	$(foreach pair,$(TEST_PAIRS),$(ASAN_OBJ_DIR)/tests/$(basename $(notdir $(word 1,$(subst |, ,$(pair))))).d)
FUZZ_DEPFILES := $(FUZZ_OBJECTS:.o=.d)
-include $(ASAN_DEPFILES) $(FUZZ_DEPFILES)


####################################################################
# Flag stamps
####################################################################
# Each build tree carries the flag string it was built with. The stamp is
# rewritten only when that string differs -- written to a scratch file,
# compared, moved into place only on a difference -- so its mtime moves on a
# flag change and on nothing else. The object rules above depend on it.
#
# This replaces listing `Makefile` as a prerequisite, which was too broad (a
# comment-only edit recompiled everything) and too narrow (a command-line
# override such as `make EXTRA_CFLAGS=-O2` changes no file's mtime and so was
# invisible).
#
# These rules sit at the end of the file for two reasons. A rule's target
# expands when make reads the line, so a stamp rule above its own OBJ_DIR
# definition has an empty target: not an error, just a rule that silently does
# not exist. And the first target in a makefile is the default goal, so a stamp
# rule above `all:` makes a bare `make` build the stamp and nothing else.
.PHONY: force-flags

$(FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(CFLAGS) $(CXXFLAGS) $(LDFLAGS) $(INCLUDE)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

$(ASAN_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(ASAN_CFLAGS) $(ASAN_CXXFLAGS) $(ASAN_LDFLAGS) $(INCLUDE)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@

$(FUZZ_FLAGS_STAMP): force-flags
	@mkdir -p $(@D)
	@printf '%s\n' '$(FUZZ_SAN) $(FUZZ_LIB_FLAGS) $(FUZZ_BIN_FLAGS) $(INCLUDE)' > $@.new
	@cmp -s $@.new $@ 2>/dev/null && rm -f $@.new || mv -f $@.new $@
