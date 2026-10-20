#!/usr/bin/env python3
"""WTF-8, in one place, because two spellings of an encoding is one bug.

A UTF-16 reference - the JDK, V8 - hands out strings that may hold an
unpaired surrogate, and a differential has to carry one to the other side
without changing what it is. WTF-8 is the encoding that can: a surrogate
*pair* is the four-byte scalar it stands for, and only a genuinely unpaired
surrogate takes its own three bytes.

**Not CESU-8**, which spells a pair as two three-byte sequences. This
library refuses that, because accepting it would let `.` match half a
character and put every byte offset at odds with the reference. So the pair
rule is not a detail of the encoder, it is the reason the encoder exists,
and Python's own `surrogatepass` is not enough on its own: it encodes each
surrogate it finds, pair or not, which is CESU-8 exactly. The pairs have to
be joined first.

This module was written after the second place that joining was needed. The
first - `tools/corpus/import_jdk_tests.py` - got it wrong twice before it
was right, each time as 19 to 159 false disagreements rather than as a
crash, and the second - `tools/oracle/match_diff.py` - reproduced the CESU-8
mistake on its first run: two adjacent surrogate pieces are a *pair* to
`JSON.parse` and two lone surrogates to `str.encode`, so the two sides were
being asked different questions. One definition, imported by both.

Copyright 2026 by Corey Pennycuff
"""


def scalars(text):
    """Join every surrogate pair; leave an unpaired surrogate alone.

    A reference that spells a supplementary character as two `\\uXXXX`
    escapes produces two code units, and they have to be joined before the
    string means what the reference meant. Walking the units is what makes
    this total: a whole-string round trip through UTF-16 raises when a lone
    surrogate is present anywhere, and a fallback that returns the string
    untouched then joins nothing at all.
    """
    return "".join(chr(point) for point in _code_points(text))


def encode(text):
    """WTF-8 bytes for `text`, which may hold unpaired surrogates."""
    out = bytearray()
    for point in _code_points(text):
        if point < 0x80:
            out.append(point)
        elif point < 0x800:
            out += bytes((0xC0 | point >> 6, 0x80 | point & 0x3F))
        elif point < 0x10000:
            out += bytes((0xE0 | point >> 12, 0x80 | (point >> 6) & 0x3F,
                          0x80 | point & 0x3F))
        else:
            out += bytes((0xF0 | point >> 18, 0x80 | (point >> 12) & 0x3F,
                          0x80 | (point >> 6) & 0x3F, 0x80 | point & 0x3F))
    return bytes(out)


def decode(data):
    """The inverse: WTF-8 bytes to a `str` that may hold a lone surrogate.

    `surrogatepass` is the right decoder here and the wrong encoder, which
    is worth the asymmetry being explicit: reading back three bytes that
    spell a surrogate is what the reference meant, while writing a pair that
    way would be CESU-8.
    """
    return data.decode("utf-8", "surrogatepass")


def _code_points(text):
    """The code points of `text`, with surrogate pairs joined."""
    units = text.encode("utf-16-le", "surrogatepass")
    i = 0
    while i < len(units):
        unit = units[i] | (units[i + 1] << 8)
        i += 2
        if 0xD800 <= unit <= 0xDBFF and i + 1 < len(units):
            low = units[i] | (units[i + 1] << 8)
            if 0xDC00 <= low <= 0xDFFF:
                i += 2
                unit = 0x10000 + ((unit - 0xD800) << 10) + (low - 0xDC00)
        yield unit
