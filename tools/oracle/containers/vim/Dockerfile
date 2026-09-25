# vim, the reference for the Vim dialect.
#
# There is no official vim image, which makes this the *other* kind of pin
# and the difference is worth stating. A stock image is pinned by digest and
# the digest is the whole guarantee: the same bytes every time, forever. An
# image built here cannot be, because two builds of one Dockerfile are not
# two copies of one image. So what is pinned instead is everything the build
# reads:
#
#   - the base, by digest, so the C library and headers cannot move
#   - vim's source, by tag, so the interpreter cannot move
#
# and the version is then *checked at run time* against tools/oracle/
# containers/IMAGES rather than assumed. For a stock image that check is
# belt and braces; here it is the actual guarantee.
#
# Why not `apt-get install vim`. Debian's archive keeps one version of a
# package and drops it when a security update lands, so `vim=2:9.1.0861-1`
# builds today and fails to resolve in some number of weeks - and it fails
# by *not finding* the version, which a careless Dockerfile turns into
# "install whatever is there now". The pinned tag has no expiry.
#
# Tiny build, --depth 1 on one tag: the clone is a few seconds and the
# compile about a minute.

FROM docker.io/library/debian@sha256:7792b1f7702a86946cd518db72b6a407302c3e9bc1635634368b878189e8221c AS build

ARG VIM_TAG=v9.1.1244

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      ca-certificates git gcc make libc6-dev libncurses-dev \
 && rm -rf /var/lib/apt/lists/*

RUN git clone --depth 1 --branch "${VIM_TAG}" https://github.com/vim/vim /src

# --with-features=huge because the Vim dialect's questions reach constructs
# a "normal" build does not compile in, and a missing feature would answer
# "no such construct" in a way that reads exactly like a disagreement.
# No GUI, no interpreters: none of them is asked anything here.
WORKDIR /src
RUN ./configure \
      --with-features=huge \
      --enable-multibyte \
      --disable-gui \
      --without-x \
      --disable-netbeans \
      --disable-nls \
      --prefix=/usr/local \
 && make -j"$(nproc)" \
 && make install

FROM docker.io/library/debian@sha256:7792b1f7702a86946cd518db72b6a407302c3e9bc1635634368b878189e8221c

RUN apt-get update \
 && apt-get install -y --no-install-recommends libncurses6 \
 && rm -rf /var/lib/apt/lists/*

COPY --from=build /usr/local/bin/vim /usr/local/bin/vim
COPY --from=build /usr/local/share/vim /usr/local/share/vim

# The locale, pinned, and this is the entry that earned the whole exercise.
#
# vim takes `&encoding` from the locale, and the first container run of
# vim_diff.py reported fifty disagreements that the host reported none of -
# all of them on non-ASCII subjects, `\X` and `\k` and `É`. The container
# had no LANG, so its vim was latin1 and read one character as two.
#
# The host was not *right*; it was configured, by a developer's shell, in a
# way nothing in this repository records. `LANG=C` on the host reproduces
# every one of those fifty. So the oracle's answers depended on an
# environment variable no file here mentions, and the differential had been
# quietly measuring the shell it was started from.
#
# Pinning it here fixes the container. tools/oracle/vim_diff.py's script
# also sets `encoding` outright, which fixes it everywhere and is the real
# repair - this line is so that the image is not one more thing that has to
# be right by accident.
#
# It was still needed after that repair landed, and not only as belt and
# braces: `tools/oracle/replace_diff.py` drives vim through a command line of
# its own and never got the setting, so on 2026-09-25 its gate still reported
# **67 disagreements over 9,600 rows under `LANG=C` against 0 under UTF-8**,
# with 3,691 rows "the reference declined" and three absorbed into the
# known-defect bucket. One repair in one file is not a repair of the rule;
# `tools/oracle/vim_runner.py` is, and this image is the floor under both.
ENV LANG=C.UTF-8 LC_ALL=C.UTF-8

# vim writes viminfo, swap files and its undo tree, and the repository is
# mounted read-only. HOME is somewhere writable inside the image, which is not
# the scratch directory a caller declares: a tool that needs vim to write an
# *answer* says where, so one that forgets fails on a missing path rather than
# writing into the image's own /tmp where nobody looks.
ENV HOME=/tmp VIMINIT="set nocompatible"

ENTRYPOINT []
CMD ["vim", "--version"]
