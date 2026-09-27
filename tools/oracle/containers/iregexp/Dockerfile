# The two references for I-Regexp, RFC 9485, and there are two because the
# dialect's two halves have no single implementation to answer both.
#
# **Built here, which makes it the other kind of pin.** A stock image is pinned
# by digest and the digest is the whole guarantee; an image built here cannot
# be, because two builds of one Dockerfile are not two copies of one image. So
# what is pinned instead is everything the build reads - the base by digest,
# each package by an exact version, and `--only-binary` so that a source build
# (which would compile against whatever this machine's libraries are) fails
# rather than quietly producing a different reference. The run-time version
# check against tools/oracle/containers/IMAGES is the real guarantee.
#
# **Why a new image rather than the pinned python.** `python:3.14.7-slim` is
# the base here and its digest is the same one IMAGES pins for the `python`
# reference - deliberately, so that the two CPythons cannot drift apart - but
# the packages cannot be added to a stock image without building one, and
# adding them to the *posix* image would change what four other references run.
#
# **Why two references in one image.** They are not two faces of one thing the
# way glibc, musl, sed and grep are faces of one Debian userland; they are two
# pip wheels that happen to be reached through the same interpreter. What
# argues for one image is that neither is a program in its own right - both are
# libraries called from tools/oracle/iregexp_check.py - so a second image would
# be a second copy of CPython to hold one more wheel.

FROM docker.io/library/python:3.14.7-slim@sha256:7bf6c3111fe094f8ee1a1cbcdc63c4cfb345b0e3df42d5aa9a90b3b4b022ab6d

# iregexp-check, the syntax half: a *checking* implementation of RFC 9485,
# which is what this library claims to be and therefore the only kind of
# reference that can contradict it. It is jg-rp's Rust crate behind Python
# bindings (github.com/jg-rp/rust-iregexp), so the answer comes from a parser
# written independently of this one against the same ABNF - which is the whole
# value: two readings of Figure 1 that agree are evidence, and one reading is
# not.
#
# lxml, the semantic half, and what is pinned is really the libxml2 inside it.
# RFC 9485 section 5.2 says every I-Regexp *is* an XSD regexp and the mapping
# is the identity, so libxml2's XSD pattern facet answers what a pattern
# matches - the whole-string Boolean that section 4 borrows from XSD. The wheel
# statically links its own libxml2, which is why the version field names both
# and why `--only-binary` matters here more than anywhere: a source build would
# link this machine's libxml2 and the reference would be something else.
RUN pip install --no-cache-dir --only-binary :all: \
      iregexp-check==0.1.4 \
      lxml==6.1.3

# Nothing here knows about this library. The drivers live in the tree and are
# bind-mounted read-only at their own path, the way pcre2's compiled driver and
# python_match.py are - an image that held them would be an oracle that could
# reach the implementation it answers for.
ENV LANG=C.UTF-8 LC_ALL=C.UTF-8
ENV PYTHONDONTWRITEBYTECODE=1

ENTRYPOINT []

# The same line tools/oracle/iregexp_match.py --version prints, so that a bare
# `docker run` of the image and the differential's own provenance line cannot
# disagree about what is inside it.
CMD ["python3", "-c", "import importlib.metadata as m, lxml.etree as e; print('iregexp-check %s, libxml2 %s, lxml %s' % (m.version('iregexp-check'), '.'.join(str(n) for n in e.LIBXML_VERSION), m.version('lxml')))"]
