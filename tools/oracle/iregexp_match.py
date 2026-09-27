#!/usr/bin/env python3
r"""The two I-Regexp references, behind the batch protocol the others speak.

RFC 9485 has no implementation to name the way `GRX_SYNTAX_PERL` names perl:
it is a *format* defined by an ABNF, so what answers for it is two references
that answer different halves.

    check   iregexp-check: is this string an I-Regexp?
    xsd     libxml2: does this pattern match this string?

**Why two.** A checking implementation is defined by what it refuses (RFC 9485
section 3.1), so the accept-or-refuse half needs a second *reading of Figure
1* - iregexp-check is jg-rp's Rust parser, written independently against the
same grammar, and two readings that agree are evidence where one is not. The
matching half needs something that runs a pattern, and section 5.2 supplies
it: every I-Regexp is an XSD regexp under the identity mapping, so any XSD
engine answers what a pattern matches. libxml2's pattern facet is that engine,
reached through lxml.

**The XSD half asks the whole-string question**, which is the one section 4
borrows from XSD and the one JSONPath's `match()` needs. A pattern facet is
anchored by definition - there is no unanchored form of it - so `search()` has
no reference here and `iregexp_diff.py` says so rather than inventing one.

**The file is named `iregexp_match.py` and not after the package it imports.**
A script named `iregexp_check.py` puts its own directory first on `sys.path`
and then `from iregexp_check import check` imports *itself*, which fails with
an ImportError naming the driver as the module that has no `check` - a
confusing way to find out that two files are one name.

Input is one case per line, hex-encoded, the same as every other driver here:

    check   <pattern>
    xsd     <pattern>\t<subject>

and the answers are one word per line:

    check   ok | no
    xsd     true | false | badpattern | badsubject

`badpattern` is libxml2 refusing to compile the facet, which is a third answer
and not a `false`: it means the pattern was never run. `badsubject` is a
subject XML cannot carry - a C0 control other than tab, LF or CR is not a
character any XML 1.0 document may hold, by any spelling - which is the
oracle declining rather than answering.

Copyright 2026 by Corey Pennycuff
"""

import binascii
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))


def command(mode=None):
    """How a caller runs this driver against the pinned image.

    The pin name differs by mode, because the two references are two pins that
    happen to share an image: a run that names `libxml2` prints libxml2's
    version in its provenance line, and one that names `iregexp` prints the
    checker's. One image, two references, and the line says which answered.
    """
    import oracle_env
    argv = ["python3", os.path.join(HERE, "iregexp_match.py")]
    if mode:
        argv.append(mode)
    return oracle_env.command("libxml2" if mode == "xsd" else "iregexp", argv)


def unhex(text):
    return binascii.unhexlify(text).decode("utf-8")


# Characters no XML 1.0 document may contain, in any spelling: a numeric
# reference to one is as ill-formed as the character itself. Tab, LF and CR are
# the three C0 controls that are allowed.
XML_FORBIDDEN = set(range(0x00, 0x20)) - {0x09, 0x0A, 0x0D}
XML_FORBIDDEN |= set(range(0xD800, 0xE000))  # lone surrogates, unreachable from UTF-8
XML_FORBIDDEN |= {0xFFFE, 0xFFFF}


def references(text):
    """Every character as a numeric character reference.

    Not `xml.sax.saxutils.escape`, which escapes the three markup characters
    and leaves the rest literal. Two reasons a literal will not do here:

    - **Attribute-value normalisation.** A literal tab, LF or CR inside an
      attribute is replaced by a space before the schema processor ever sees
      it, so a pattern containing `\t` would silently become one containing a
      space. A numeric reference survives it.
    - The pattern is the thing under test, so the fewer of its characters this
      driver decides about, the better.
    """
    return "".join("&#%d;" % ord(character) for character in text)


def xml_can_carry(text):
    return not any(ord(character) in XML_FORBIDDEN for character in text)


def do_check(fields, state):
    pattern = unhex(fields[0])
    return "ok" if state["check"](pattern) else "no"


def do_xsd(fields, state):
    pattern = unhex(fields[0])
    subject = unhex(fields[1]) if len(fields) > 1 else ""
    if not xml_can_carry(pattern) or not xml_can_carry(subject):
        return "badsubject"

    etree = state["etree"]
    schema = state["schemas"].get(pattern)
    if schema is None:
        text = (
            '<xs:schema xmlns:xs="http://www.w3.org/2001/XMLSchema">'
            '<xs:element name="v"><xs:simpleType>'
            '<xs:restriction base="xs:string">'
            '<xs:pattern value="%s"/>'
            '</xs:restriction></xs:simpleType></xs:element></xs:schema>'
            % references(pattern))
        try:
            schema = etree.XMLSchema(etree.fromstring(text.encode("utf-8")))
        except Exception:
            state["schemas"][pattern] = False
            return "badpattern"
        state["schemas"][pattern] = schema
    elif schema is False:
        return "badpattern"

    try:
        document = etree.fromstring(
            ("<v>%s</v>" % references(subject)).encode("utf-8"))
    except Exception:
        return "badsubject"
    return "true" if schema.validate(document) else "false"


MODES = {None: do_check, "check": do_check, "xsd": do_xsd}


def main(argv):
    mode = argv[1] if len(argv) > 1 else None
    if mode == "--version":
        import importlib.metadata as metadata
        import lxml.etree
        sys.stdout.write(
            "iregexp-check %s, libxml2 %s, lxml %s\n"
            % (metadata.version("iregexp-check"),
               ".".join(str(part) for part in lxml.etree.LIBXML_VERSION),
               metadata.version("lxml")))
        return 0
    if mode not in MODES:
        sys.stderr.write("iregexp_match.py [check|xsd]\n")
        return 2

    state = {"schemas": {}}
    if mode == "xsd":
        import lxml.etree
        state["etree"] = lxml.etree
    else:
        from iregexp_check import check
        state["check"] = check

    answer = MODES[mode]
    out = []
    for line in sys.stdin:
        # **A blank line is a record here, and the other drivers skip one.**
        # The empty pattern is a legal I-Regexp - `i-regexp = branch *( "|"
        # branch )` and `branch = *piece`, so it matches only the empty string
        # - and its hex encoding is the empty string, which makes its record a
        # line with nothing on it. Skipping blank lines lost exactly that row
        # and the differential then compared 37,247 answers against 37,248
        # questions, which it reported as a driver fault rather than silently
        # sliding every answer by one. One record per line, no exceptions.
        out.append(answer(line.rstrip("\n").split("\t"), state))
    sys.stdout.write("\n".join(out) + ("\n" if out else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
