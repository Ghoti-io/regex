// SPDX-License-Identifier: LGPL-3.0-only
// Copyright (C) 2026 Corey Pennycuff
//
// Independent persistent oracle for System.Text.RegularExpressions.
// Input: mode TAB pattern-hex TAB flags-hex TAB subject-hex TAB argument-hex.
// Each request and response occupies one line. Text fields are strict UTF-8
// encoded as hex; response payload is hex UTF-8 JSON. Numeric offsets in the
// JSON are UTF-8 byte offsets, or an explicit offset-error is returned.
//
// This is JavaOracle.java's protocol, byte for byte, and deliberately so:
// .NET and Java are both UTF-16 engines, so every differential this tree
// already has for Java asks the same questions here, and a shared protocol
// means the diff drivers differ only where the *dialects* differ.
//
// A mode prefixed `w:` reads its text fields as WTF-8 and reports WTF-8 byte
// offsets, for the same reason it exists on the Java side: a .NET `string` is
// a UTF-16 code unit sequence that may hold an unpaired surrogate, strict
// UTF-8 cannot carry one, and without the prefix the reference cannot be
// asked about them at all. WTF-8 and not CESU-8 - a surrogate *pair* arrives
// as the four-byte scalar and a high surrogate immediately followed by a low
// one is refused, which is what keeps `.` from matching half a character.
//
// Three things here are .NET's and are reported rather than normalised:
//
//   * **Group numbering.** .NET numbers unnamed groups first and named groups
//     after them, so `(?<a>x)(y)` makes `y` group 1 and `a` group 2 - the
//     reverse of every other dialect here. `Groups` is walked in .NET's own
//     order and the spans come out in it.
//   * **Split returns captures.** `Regex.Split` interleaves the captured
//     groups of each separator match between the pieces. Java's does not.
//   * **`RegexOptions.ECMAScript`** is a mode inside the dialect rather than
//     a dialect of its own, reached by the `E` flag letter below.
//
// The driver is compiled inside the pinned image at run time, from this file
// in the mounted tree, by `dotnet_runner.py` - so the image holds a toolchain
// and knows nothing about this library. It drives Roslyn directly rather than
// going through `dotnet run`, because a file-based app runs an implicit NuGet
// restore and the container has no network: see dotnet_runner.py.
using System;
using System.Collections.Generic;
using System.Globalization;
using System.IO;
using System.Text;
using System.Text.RegularExpressions;

public static class DotNetOracle {
    /// <summary>Hex to bytes, with no opinion about what the bytes mean.</summary>
    private static byte[] Unhex(string hex) {
        if ((hex.Length & 1) != 0) throw new ArgumentException("odd hex length");
        byte[] bytes = new byte[hex.Length / 2];
        for (int i = 0; i < bytes.Length; i++) {
            int hi = HexDigit(hex[i * 2]), lo = HexDigit(hex[i * 2 + 1]);
            if (hi < 0 || lo < 0) throw new ArgumentException("non-hex input");
            bytes[i] = (byte)((hi << 4) | lo);
        }
        return bytes;
    }

    private static int HexDigit(char c) {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    }

    /// <summary>
    /// WTF-8 bytes to a string, unpaired surrogates and all.
    ///
    /// Written out rather than handed to an Encoding because no .NET encoding
    /// is WTF-8: UTF8Encoding substitutes U+FFFD for a surrogate sequence
    /// (or throws, with throwOnInvalidBytes), and there is no CESU-8 at all.
    /// </summary>
    private static string Wtf8Decode(byte[] bytes) {
        StringBuilder outText = new StringBuilder();
        int i = 0;
        bool previousWasHigh = false;
        int[] lowest = { 0, 0x00, 0x80, 0x800, 0x10000 };
        while (i < bytes.Length) {
            int b = bytes[i];
            int value, width;
            if (b < 0x80) { value = b; width = 1; }
            else if ((b & 0xE0) == 0xC0) { value = b & 0x1F; width = 2; }
            else if ((b & 0xF0) == 0xE0) { value = b & 0x0F; width = 3; }
            else if ((b & 0xF8) == 0xF0) { value = b & 0x07; width = 4; }
            else throw new ArgumentException("malformed WTF-8 lead byte");
            if (i + width > bytes.Length)
                throw new ArgumentException("truncated WTF-8 sequence");
            for (int k = 1; k < width; k++) {
                int c = bytes[i + k];
                if ((c & 0xC0) != 0x80)
                    throw new ArgumentException("malformed WTF-8 continuation");
                value = (value << 6) | (c & 0x3F);
            }
            if (value < lowest[width])
                throw new ArgumentException("overlong WTF-8 sequence");
            if (value > 0x10FFFF)
                throw new ArgumentException("WTF-8 value out of range");
            bool high = value >= 0xD800 && value <= 0xDBFF;
            bool low = value >= 0xDC00 && value <= 0xDFFF;
            // The CESU-8 refusal. A pair has to be the four-byte scalar.
            if (previousWasHigh && low)
                throw new ArgumentException(
                    "surrogate pair spelled as two sequences; WTF-8 wants the "
                    + "four-byte form");
            if (value < 0x10000) outText.Append((char)value);
            else outText.Append(char.ConvertFromUtf32(value));
            previousWasHigh = high;
            i += width;
        }
        return outText.ToString();
    }

    /// <summary>A string to WTF-8: a pair is four bytes, a lone surrogate three.</summary>
    private static int Wtf8Length(string s, int end) {
        int bytes = 0;
        for (int i = 0; i < end; i++) {
            char c = s[i];
            if (char.IsHighSurrogate(c) && i + 1 < s.Length
                && char.IsLowSurrogate(s[i + 1])) {
                bytes += 4;
                i++;
                if (i >= end)
                    // The caller asked for an offset inside a pair, which is
                    // not a position in WTF-8 any more than it is in UTF-8.
                    throw new ArgumentException("split-pair offset " + end);
                continue;
            }
            int cp = c;
            bytes += cp < 0x80 ? 1 : cp < 0x800 ? 2 : 3;
        }
        return bytes;
    }

    private static string Decode(string hex) {
        byte[] bytes = Unhex(hex);
        try {
            return new UTF8Encoding(false, true).GetString(bytes);
        } catch (DecoderFallbackException) {
            throw new ArgumentException("malformed UTF-8");
        }
    }

    /// <summary>
    /// Which encoding the offsets of the request in flight are measured in.
    ///
    /// A static field rather than an argument because ByteIndex() is reached
    /// from Spans(), Split() and the replace modes, and threading a flag
    /// through all of them invites exactly one site being missed - which
    /// would report UTF-8 offsets for a WTF-8 subject and look like an
    /// off-by-one. The loop is single-threaded; this is not shared state.
    /// </summary>
    private static bool wide;

    private static int ByteIndex(string s, int index) {
        if (index < 0) return -1;
        if (index > s.Length) throw new ArgumentException("offset beyond string");
        if (wide) return Wtf8Length(s, index);
        if (index > 0 && index < s.Length
            && char.IsHighSurrogate(s[index - 1]) && char.IsLowSurrogate(s[index]))
            throw new ArgumentException("split-pair offset " + index);
        for (int i = 0; i < s.Length; i++) {
            char c = s[i];
            if (char.IsHighSurrogate(c)) {
                if (++i >= s.Length || !char.IsLowSurrogate(s[i]))
                    throw new ArgumentException("unpaired surrogate");
            } else if (char.IsLowSurrogate(c)) {
                throw new ArgumentException("unpaired surrogate");
            }
        }
        return Encoding.UTF8.GetByteCount(s.Substring(0, index));
    }

    private static string Hex(string s) {
        ByteIndex(s, s.Length);
        byte[] bytes = wide ? Wtf8Encode(s) : Encoding.UTF8.GetBytes(s);
        StringBuilder outText = new StringBuilder(bytes.Length * 2);
        foreach (byte b in bytes) outText.Append(b.ToString("x2", CultureInfo.InvariantCulture));
        return outText.ToString();
    }

    /// <summary>
    /// A string to WTF-8 bytes. Encoding.UTF8 cannot do this: it substitutes
    /// U+FFFD for an unpaired surrogate, which would silently change the
    /// answer rather than report it.
    /// </summary>
    private static byte[] Wtf8Encode(string s) {
        List<byte> outBytes = new List<byte>(s.Length + 8);
        for (int i = 0; i < s.Length; i++) {
            int cp = s[i];
            if (char.IsHighSurrogate(s[i]) && i + 1 < s.Length
                && char.IsLowSurrogate(s[i + 1])) {
                cp = char.ConvertToUtf32(s[i], s[i + 1]);
                i++;
            }
            if (cp < 0x80) outBytes.Add((byte)cp);
            else if (cp < 0x800) {
                outBytes.Add((byte)(0xC0 | (cp >> 6)));
                outBytes.Add((byte)(0x80 | (cp & 0x3F)));
            } else if (cp < 0x10000) {
                outBytes.Add((byte)(0xE0 | (cp >> 12)));
                outBytes.Add((byte)(0x80 | ((cp >> 6) & 0x3F)));
                outBytes.Add((byte)(0x80 | (cp & 0x3F)));
            } else {
                outBytes.Add((byte)(0xF0 | (cp >> 18)));
                outBytes.Add((byte)(0x80 | ((cp >> 12) & 0x3F)));
                outBytes.Add((byte)(0x80 | ((cp >> 6) & 0x3F)));
                outBytes.Add((byte)(0x80 | (cp & 0x3F)));
            }
        }
        return outBytes.ToArray();
    }

    private static string Quoted(string s) {
        StringBuilder outText = new StringBuilder("\"");
        foreach (char c in s) {
            if (c == '"' || c == '\\') outText.Append('\\').Append(c);
            else if (c < 32) outText.Append("\\u").Append(((int)c).ToString("x4", CultureInfo.InvariantCulture));
            else outText.Append(c);
        }
        return outText.Append('"').ToString();
    }

    /// <summary>
    /// The flag alphabet. `imnsx` are the five .NET spells inline as well as
    /// in RegexOptions, and are what grx_options_parse() will have to accept.
    /// The three capitals are options with no inline spelling, given letters
    /// here so a differential can reach them: `E` is RegexOptions.ECMAScript,
    /// which narrows the classes and shorthands to ASCII and is a mode inside
    /// the dialect; `C` is CultureInvariant, which this library implements as
    /// SIMPLE_FOLD and documents as a deviation; `R` is RightToLeft.
    ///
    /// Compiled and NonBacktracking are deliberately absent. Both are
    /// documented as not changing an answer, so a letter for them would be a
    /// letter whose only effect is to make two runs of the same question
    /// disagree or not - and if they ever *do* change an answer that is a
    /// finding about .NET, which belongs in a determinism gate on the model
    /// of check-oracle-determinism, not in this alphabet.
    /// </summary>
    private static RegexOptions Options(string letters) {
        RegexOptions outOptions = RegexOptions.None;
        foreach (char c in letters) {
            switch (c) {
                case 'i': outOptions |= RegexOptions.IgnoreCase; break;
                case 'm': outOptions |= RegexOptions.Multiline; break;
                case 'n': outOptions |= RegexOptions.ExplicitCapture; break;
                case 's': outOptions |= RegexOptions.Singleline; break;
                case 'x': outOptions |= RegexOptions.IgnorePatternWhitespace; break;
                case 'E': outOptions |= RegexOptions.ECMAScript; break;
                case 'C': outOptions |= RegexOptions.CultureInvariant; break;
                case 'R': outOptions |= RegexOptions.RightToLeft; break;
                default: throw new ArgumentException("unknown flag " + c);
            }
        }
        return outOptions;
    }

    /// <summary>
    /// Every group's span, in .NET's own numbering.
    ///
    /// `Groups` is walked by its own index rather than by a 0..GroupCount
    /// loop, because .NET's numbering is not the declaration order: unnamed
    /// groups are numbered first and named ones after them. Reporting them in
    /// .NET's order is the point - that renumbering is a dialect rule this
    /// library has to implement, so the oracle must not hide it.
    /// </summary>
    private static string Spans(Match m, string subject) {
        StringBuilder outText = new StringBuilder("[");
        for (int i = 0; i < m.Groups.Count; i++) {
            if (i != 0) outText.Append(',');
            Group g = m.Groups[i];
            if (!g.Success) outText.Append("null");
            else outText.Append('[').Append(ByteIndex(subject, g.Index))
                .Append(',').Append(ByteIndex(subject, g.Index + g.Length)).Append(']');
        }
        return outText.Append(']').ToString();
    }

    /// <summary>The group names, so a differential can see the renumbering.</summary>
    private static string Names(Regex compiled) {
        StringBuilder outText = new StringBuilder("[");
        string[] names = compiled.GetGroupNames();
        int[] numbers = compiled.GetGroupNumbers();
        for (int i = 0; i < numbers.Length; i++) {
            if (i != 0) outText.Append(',');
            outText.Append('[').Append(numbers[i]).Append(',')
                .Append(Quoted(compiled.GroupNameFromNumber(numbers[i]))).Append(']');
        }
        return outText.Append(']').ToString();
    }

    private static int CharIndex(string subject, int byteOffset) {
        for (int i = 0; i <= subject.Length; i++) {
            if (i > 0 && i < subject.Length
                && char.IsHighSurrogate(subject[i - 1])
                && char.IsLowSurrogate(subject[i])) continue;
            if (ByteIndex(subject, i) == byteOffset) return i;
        }
        throw new ArgumentException("invalid byte offset");
    }

    private static string Answer(string line) {
        string[] fields = line.Split('\t');
        if (fields.Length != 5) throw new ArgumentException("expected five fields");
        string mode = fields[0];
        // `w:` switches the text fields to WTF-8 and the reported offsets with
        // them. One switch for both, because a subject decoded one way and
        // measured the other is the shape that produces an accurate answer to
        // a different question.
        bool wtf8 = mode.StartsWith("w:", StringComparison.Ordinal);
        if (wtf8) mode = mode.Substring(2);
        wide = wtf8;
        string pattern = wtf8 ? Wtf8Decode(Unhex(fields[1])) : Decode(fields[1]);
        string flagText = Decode(fields[2]);
        string subject = wtf8 ? Wtf8Decode(Unhex(fields[3])) : Decode(fields[3]);
        string argument = wtf8 ? Wtf8Decode(Unhex(fields[4])) : Decode(fields[4]);
        switch (mode) {
            case "compile": case "names": case "first": case "all":
            case "all-from": case "find-from": case "ranges":
            case "replace-all": case "replace-first":
            case "replace-all-literal": case "replace-first-literal":
            case "split": case "split-count":
                break;
            default: throw new ArgumentException("unknown mode " + mode);
        }
        Regex compiled;
        try {
            compiled = new Regex(pattern, Options(flagText));
        } catch (RegexParseException e) {
            // .NET reports the offset in UTF-16 code units, like Java's
            // PatternSyntaxException.getIndex(), so it converts the same way.
            // `Error` is the machine-readable kind and is reported beside the
            // prose, because the prose is localised and the kind is not.
            return "{\"kind\":\"syntax\",\"index\":" + ByteIndex(pattern, e.Offset)
                + ",\"error\":" + Quoted(e.Error.ToString())
                + ",\"description\":" + Quoted(e.Message) + "}";
        }
        switch (mode) {
            case "compile": return "{\"kind\":\"compile\"}";
            case "names":
                return "{\"kind\":\"names\",\"names\":" + Names(compiled) + "}";
            case "ranges": {
                // Whole-string membership, which is what Java's
                // Matcher.matches() answers. .NET has no such method - IsMatch
                // is a search - so the search's result is checked for covering
                // the subject rather than the pattern being wrapped in \A..\z,
                // because wrapping it would be asking about a different
                // pattern.
                StringBuilder outText = new StringBuilder("{\"kind\":\"ranges\",\"ranges\":[");
                int start = -1;
                bool comma = false;
                for (int cp = 0; cp <= 0x110000; cp++) {
                    bool member = false;
                    if (cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF)) {
                        string one = char.ConvertFromUtf32(cp);
                        Match m = compiled.Match(one);
                        member = m.Success && m.Index == 0 && m.Length == one.Length;
                    }
                    if (member && start < 0) start = cp;
                    else if (!member && start >= 0) {
                        if (comma) outText.Append(',');
                        comma = true;
                        outText.Append('[').Append(start).Append(',').Append(cp - 1).Append(']');
                        start = -1;
                    }
                }
                return outText.Append("]}").ToString();
            }
            case "first": {
                Match m = compiled.Match(subject);
                return m.Success ? "{\"kind\":\"match\",\"groups\":"
                    + Spans(m, subject) + "}" : "{\"kind\":\"nomatch\"}";
            }
            case "find-from": {
                Match m = compiled.Match(subject, CharIndex(subject, int.Parse(argument, CultureInfo.InvariantCulture)));
                return m.Success ? "{\"kind\":\"match\",\"groups\":"
                    + Spans(m, subject) + "}" : "{\"kind\":\"nomatch\"}";
            }
            case "all": case "all-from": {
                StringBuilder outText = new StringBuilder("{\"kind\":\"all\",\"matches\":[");
                Match m = mode == "all-from"
                    ? compiled.Match(subject, CharIndex(subject, int.Parse(argument, CultureInfo.InvariantCulture)))
                    : compiled.Match(subject);
                bool comma = false;
                while (m.Success) {
                    if (comma) outText.Append(',');
                    comma = true;
                    outText.Append(Spans(m, subject));
                    m = m.NextMatch();
                }
                return outText.Append("]}").ToString();
            }
            case "replace-all":
                return "{\"kind\":\"replace\",\"text\":"
                    + Quoted(compiled.Replace(subject, argument)) + "}";
            case "replace-first":
                return "{\"kind\":\"replace\",\"text\":"
                    + Quoted(compiled.Replace(subject, argument, 1)) + "}";
            // .NET has no Matcher.quoteReplacement. `$` is the only character
            // the template grammar gives meaning to, and `$$` is its literal,
            // so that substitution *is* the quoting - and saying so here is
            // better than shipping a helper that hides which rule was applied.
            case "replace-all-literal":
                return "{\"kind\":\"replace\",\"text\":"
                    + Quoted(compiled.Replace(subject, argument.Replace("$", "$$"))) + "}";
            case "replace-first-literal":
                return "{\"kind\":\"replace\",\"text\":"
                    + Quoted(compiled.Replace(subject, argument.Replace("$", "$$"), 1)) + "}";
            // Two split modes because .NET's two overloads are two rules, not
            // one with a default: Split(input) splits everywhere, and
            // Split(input, count) treats count as a maximum number of pieces
            // and *throws* on a negative - where Java reads a negative limit
            // as "no limit, and keep the trailing empties". A single mode with
            // a sentinel would have to pick one of those readings.
            case "split": case "split-count": {
                string[] pieces = mode == "split"
                    ? compiled.Split(subject)
                    : compiled.Split(subject, int.Parse(argument, CultureInfo.InvariantCulture));
                StringBuilder outText = new StringBuilder("{\"kind\":\"split\",\"pieces\":[");
                for (int i = 0; i < pieces.Length; i++) {
                    if (i != 0) outText.Append(',');
                    outText.Append(Quoted(pieces[i]));
                }
                return outText.Append("]}").ToString();
            }
            default: throw new ArgumentException("unknown mode " + mode);
        }
    }

    public static int Main() {
        // US-ASCII in, because the wire is hex: a byte above 0x7F on the
        // request stream is a bug in the caller, not a character.
        using (StreamReader input = new StreamReader(Console.OpenStandardInput(), Encoding.ASCII))
        using (StreamWriter output = new StreamWriter(Console.OpenStandardOutput(), Encoding.ASCII)) {
            output.AutoFlush = true;
            string line;
            while ((line = input.ReadLine()) != null) {
                try {
                    output.WriteLine("ok\t" + Hex(Answer(line)));
                } catch (Exception e) {
                    string message = e.Message ?? "null";
                    string kind =
                        message.StartsWith("split-pair offset", StringComparison.Ordinal)
                        || message.StartsWith("unpaired surrogate", StringComparison.Ordinal)
                        || message.StartsWith("offset beyond string", StringComparison.Ordinal) ? "offset"
                        : message.StartsWith("odd hex length", StringComparison.Ordinal)
                        || message.StartsWith("non-hex input", StringComparison.Ordinal)
                        || message.StartsWith("malformed UTF-8", StringComparison.Ordinal)
                        || message.StartsWith("expected five fields", StringComparison.Ordinal)
                        || message.StartsWith("unknown flag", StringComparison.Ordinal)
                        || message.StartsWith("unknown mode", StringComparison.Ordinal)
                        || message.StartsWith("invalid byte offset", StringComparison.Ordinal)
                        || ((line.StartsWith("split-count\t", StringComparison.Ordinal)
                             || line.StartsWith("find-from\t", StringComparison.Ordinal)
                             || line.StartsWith("all-from\t", StringComparison.Ordinal))
                            && e is FormatException) ? "input"
                        : line.StartsWith("replace-", StringComparison.Ordinal) ? "template"
                        : "runtime";
                    // The split overload's negative-count refusal is .NET's
                    // own rule and an answer, not a driver fault: it arrives
                    // as ArgumentOutOfRangeException from Split and is
                    // reported as `runtime` so the differential scores it.
                    output.WriteLine("error\t" + Hex("{\"kind\":" + Quoted(kind)
                        + ",\"class\":" + Quoted(e.GetType().Name)
                        + ",\"message\":" + Quoted(message) + "}"));
                }
            }
        }
        return 0;
    }
}
