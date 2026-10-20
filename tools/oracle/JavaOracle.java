// Independent persistent oracle for java.util.regex.Pattern and Matcher.
// Input: mode TAB pattern-hex TAB flags-hex TAB subject-hex TAB argument-hex.
// Each request and response occupies one line. Text fields are strict UTF-8
// encoded as hex; response payload is hex UTF-8 JSON. Numeric offsets in the
// JSON are UTF-8 byte offsets, or an explicit offset-error is returned.
//
// A mode prefixed `w:` reads its text fields as WTF-8 instead and reports
// WTF-8 byte offsets. That exists because a Java String may hold an unpaired
// surrogate and strict UTF-8 cannot carry one, so the reference could not be
// asked about them at all: `Matcher` answers 62 of the 101 such cases in the
// JDK's own corpus with a match, and this driver had no way to pass them.
//
// WTF-8 and not CESU-8: a surrogate *pair* must arrive as the four-byte
// scalar, and a high surrogate immediately followed by a low one is refused.
// That rule is what keeps `.` from matching half a character and keeps every
// byte offset aligned with the reference. It is a rule the strict decoder
// never needed, because it rejected all surrogates and so never had to tell
// the two spellings apart - accepting WTF-8 is *more* validation, not less.
//
// The prefix leaves every existing mode byte-identical, which matters: the
// other Java differentials are measured through this driver.
import java.io.BufferedReader;
import java.io.InputStreamReader;
import java.nio.ByteBuffer;
import java.nio.CharBuffer;
import java.nio.charset.CharacterCodingException;
import java.nio.charset.CodingErrorAction;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;
import java.util.regex.PatternSyntaxException;

public final class JavaOracle {
    /** Hex to bytes, with no opinion about what the bytes mean. */
    private static byte[] unhex(String hex) {
        if ((hex.length() & 1) != 0) throw new IllegalArgumentException("odd hex length");
        byte[] bytes = new byte[hex.length() / 2];
        for (int i = 0; i < bytes.length; i++) {
            int hi = Character.digit(hex.charAt(i * 2), 16);
            int lo = Character.digit(hex.charAt(i * 2 + 1), 16);
            if (hi < 0 || lo < 0) throw new IllegalArgumentException("non-hex input");
            bytes[i] = (byte) ((hi << 4) | lo);
        }
        return bytes;
    }

    /**
     * WTF-8 bytes to a String, unpaired surrogates and all.
     *
     * Written out rather than handed to a Charset because no JDK charset is
     * WTF-8: UTF_8 reports a surrogate sequence as malformed and CESU-8 is a
     * different format with the opposite rule about pairs.
     */
    private static String wtf8Decode(byte[] bytes) {
        StringBuilder out = new StringBuilder();
        int i = 0;
        boolean previousWasHigh = false;
        while (i < bytes.length) {
            int b = bytes[i] & 0xFF;
            int value;
            int width;
            if (b < 0x80) { value = b; width = 1; }
            else if ((b & 0xE0) == 0xC0) { value = b & 0x1F; width = 2; }
            else if ((b & 0xF0) == 0xE0) { value = b & 0x0F; width = 3; }
            else if ((b & 0xF8) == 0xF0) { value = b & 0x07; width = 4; }
            else throw new IllegalArgumentException("malformed WTF-8 lead byte");
            if (i + width > bytes.length)
                throw new IllegalArgumentException("truncated WTF-8 sequence");
            for (int k = 1; k < width; k++) {
                int c = bytes[i + k] & 0xFF;
                if ((c & 0xC0) != 0x80)
                    throw new IllegalArgumentException("malformed WTF-8 continuation");
                value = (value << 6) | (c & 0x3F);
            }
            int[] lowest = {0, 0x00, 0x80, 0x800, 0x10000};
            if (value < lowest[width])
                throw new IllegalArgumentException("overlong WTF-8 sequence");
            if (value > 0x10FFFF)
                throw new IllegalArgumentException("WTF-8 value out of range");
            boolean high = value >= 0xD800 && value <= 0xDBFF;
            boolean low = value >= 0xDC00 && value <= 0xDFFF;
            // The CESU-8 refusal. A pair has to be the four-byte scalar.
            if (previousWasHigh && low)
                throw new IllegalArgumentException(
                    "surrogate pair spelled as two sequences; WTF-8 wants the "
                    + "four-byte form");
            if (value < 0x10000) out.append((char) value);
            else out.append(Character.toChars(value));
            previousWasHigh = high;
            i += width;
        }
        return out.toString();
    }

    /** A String to WTF-8: a pair is four bytes, an unpaired surrogate three. */
    private static int wtf8Length(String s, int end) {
        int bytes = 0;
        for (int i = 0; i < end; i++) {
            char c = s.charAt(i);
            if (Character.isHighSurrogate(c) && i + 1 < s.length()
                && Character.isLowSurrogate(s.charAt(i + 1))) {
                bytes += 4;
                i++;
                if (i >= end) {
                    // The caller asked for an offset inside a pair, which is
                    // not a position in WTF-8 any more than it is in UTF-8.
                    throw new IllegalArgumentException("split-pair offset " + end);
                }
                continue;
            }
            int cp = c;
            bytes += cp < 0x80 ? 1 : cp < 0x800 ? 2 : 3;
        }
        return bytes;
    }

    private static String decode(String hex) throws Exception {
        if ((hex.length() & 1) != 0) throw new IllegalArgumentException("odd hex length");
        byte[] bytes = new byte[hex.length() / 2];
        for (int i = 0; i < bytes.length; i++) {
            int hi = Character.digit(hex.charAt(i * 2), 16);
            int lo = Character.digit(hex.charAt(i * 2 + 1), 16);
            if (hi < 0 || lo < 0) throw new IllegalArgumentException("non-hex input");
            bytes[i] = (byte) ((hi << 4) | lo);
        }
        try {
            CharBuffer chars = StandardCharsets.UTF_8.newDecoder()
                .onMalformedInput(CodingErrorAction.REPORT)
                .onUnmappableCharacter(CodingErrorAction.REPORT)
                .decode(ByteBuffer.wrap(bytes));
            return chars.toString();
        } catch (CharacterCodingException e) {
            throw new IllegalArgumentException("malformed UTF-8");
        }
    }

    private static String hex(String s) {
        byteIndex(s, s.length());
        byte[] bytes = s.getBytes(StandardCharsets.UTF_8);
        StringBuilder out = new StringBuilder(bytes.length * 2);
        for (byte b : bytes) out.append(String.format("%02x", b & 255));
        return out.toString();
    }

    private static String quoted(String s) {
        StringBuilder out = new StringBuilder("\"");
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (c == '"' || c == '\\') out.append('\\').append(c);
            else if (c < 32) out.append(String.format("\\u%04x", (int)c));
            else out.append(c);
        }
        return out.append('"').toString();
    }

    /**
     * Which encoding the offsets of the request in flight are measured in.
     *
     * A field rather than an argument because byteIndex() is reached from
     * spans(), split() and the replace modes, and threading a flag through
     * all of them invites exactly one site being missed - which would report
     * UTF-8 offsets for a WTF-8 subject and look like an off-by-one.
     */
    private static final ThreadLocal<Boolean> WIDE
        = ThreadLocal.withInitial(() -> Boolean.FALSE);

    private static int byteIndex(String s, int index) {
        if (index < 0) return -1;
        if (index > s.length()) throw new IllegalArgumentException("offset beyond string");
        if (WIDE.get()) return wtf8Length(s, index);
        if (index > 0 && index < s.length()
            && Character.isHighSurrogate(s.charAt(index - 1))
            && Character.isLowSurrogate(s.charAt(index)))
            throw new IllegalArgumentException("split-pair offset " + index);
        for (int i = 0; i < s.length(); i++) {
            char c = s.charAt(i);
            if (Character.isHighSurrogate(c)) {
                if (++i >= s.length() || !Character.isLowSurrogate(s.charAt(i)))
                    throw new IllegalArgumentException("unpaired surrogate");
            } else if (Character.isLowSurrogate(c)) {
                throw new IllegalArgumentException("unpaired surrogate");
            }
        }
        return s.substring(0, index).getBytes(StandardCharsets.UTF_8).length;
    }

    private static String spans(Matcher m, String subject) {
        StringBuilder out = new StringBuilder("[");
        for (int i = 0; i <= m.groupCount(); i++) {
            if (i != 0) out.append(',');
            if (m.start(i) < 0) out.append("null");
            else out.append('[').append(byteIndex(subject, m.start(i)))
                .append(',').append(byteIndex(subject, m.end(i))).append(']');
        }
        return out.append(']').toString();
    }

    private static int charIndex(String subject, int byteOffset) {
        for (int i = 0; i <= subject.length(); i++) {
            if (i > 0 && i < subject.length()
                && Character.isHighSurrogate(subject.charAt(i - 1))
                && Character.isLowSurrogate(subject.charAt(i))) continue;
            if (byteIndex(subject, i) == byteOffset) return i;
        }
        throw new IllegalArgumentException("invalid byte offset");
    }

    private static int flags(String letters) {
        int out = 0;
        for (int i = 0; i < letters.length(); i++) {
            out |= switch (letters.charAt(i)) {
                case 'i' -> Pattern.CASE_INSENSITIVE;
                case 'd' -> Pattern.UNIX_LINES;
                case 'm' -> Pattern.MULTILINE;
                case 's' -> Pattern.DOTALL;
                case 'u' -> Pattern.UNICODE_CASE;
                case 'x' -> Pattern.COMMENTS;
                case 'U' -> Pattern.UNICODE_CHARACTER_CLASS;
                case 'l' -> Pattern.LITERAL;
                case 'c' -> Pattern.CANON_EQ;
                default -> throw new IllegalArgumentException("unknown flag " + letters.charAt(i));
            };
        }
        return out;
    }

    private static String answer(String line) throws Exception {
        String[] fields = line.split("\\t", -1);
        if (fields.length != 5) throw new IllegalArgumentException("expected five fields");
        String mode = fields[0];
        // `w:` switches the text fields to WTF-8 and the reported offsets
        // with them. One switch for both, because a subject decoded one way
        // and measured the other is the shape that produces an accurate
        // answer to a different question.
        boolean wtf8 = mode.startsWith("w:");
        if (wtf8) mode = mode.substring(2);
        WIDE.set(wtf8);
        String pattern = wtf8 ? wtf8Decode(unhex(fields[1])) : decode(fields[1]);
        String flagText = decode(fields[2]);
        String subject = wtf8 ? wtf8Decode(unhex(fields[3])) : decode(fields[3]);
        String argument = wtf8 ? wtf8Decode(unhex(fields[4])) : decode(fields[4]);
        switch (mode) {
            case "compile", "first", "all", "all-from", "find-from", "matches", "ranges",
                 "looking-at", "region-first", "region-all", "region-matches",
                 "region-looking-at", "replace-all", "replace-first",
                 "replace-all-literal", "replace-first-literal", "split" -> { }
            default -> throw new IllegalArgumentException("unknown mode " + mode);
        }
        Pattern compiled;
        try { compiled = Pattern.compile(pattern, flags(flagText)); }
        catch (PatternSyntaxException e) {
            int index = byteIndex(pattern, e.getIndex());
            return "{\"kind\":\"syntax\",\"index\":" + index
                + ",\"description\":" + quoted(e.getDescription()) + "}";
        }
        Matcher matcher = compiled.matcher(subject);
        if (mode.startsWith("region-")) {
            String[] controls = argument.split(",", -1);
            if (controls.length != 4) throw new IllegalArgumentException("invalid region controls");
            int begin = charIndex(subject, Integer.parseInt(controls[0]));
            int end = charIndex(subject, Integer.parseInt(controls[1]));
            matcher.region(begin, end);
            matcher.useTransparentBounds(controls[2].equals("1"));
            matcher.useAnchoringBounds(controls[3].equals("1"));
        }
        switch (mode) {
            case "compile": return "{\"kind\":\"compile\"}";
            case "ranges": {
                StringBuilder out = new StringBuilder("{\"kind\":\"ranges\",\"ranges\":[");
                int start = -1;
                boolean comma = false;
                for (int cp = 0; cp <= 0x110000; cp++) {
                    boolean member = cp <= 0x10FFFF && (cp < 0xD800 || cp > 0xDFFF)
                        && matcher.reset(new String(Character.toChars(cp))).matches();
                    if (member && start < 0) start = cp;
                    else if (!member && start >= 0) {
                        if (comma) out.append(',');
                        comma = true;
                        out.append('[').append(start).append(',').append(cp - 1).append(']');
                        start = -1;
                    }
                }
                return out.append("]}").toString();
            }
            case "first", "region-first":
                return matcher.find() ? "{\"kind\":\"match\",\"groups\":"
                    + spans(matcher, subject) + "}" : "{\"kind\":\"nomatch\"}";
            case "all", "all-from", "region-all": {
                StringBuilder out = new StringBuilder("{\"kind\":\"all\",\"matches\":[");
                boolean comma = false;
                boolean found = mode.equals("all-from")
                    ? matcher.find(charIndex(subject, Integer.parseInt(argument)))
                    : matcher.find();
                while (found) {
                    if (comma) out.append(',');
                    comma = true;
                    out.append(spans(matcher, subject));
                    found = matcher.find();
                }
                return out.append("]}").toString();
            }
            case "find-from": {
                int offset = charIndex(subject, Integer.parseInt(argument));
                return matcher.find(offset) ? "{\"kind\":\"match\",\"groups\":"
                    + spans(matcher, subject) + "}" : "{\"kind\":\"nomatch\"}";
            }
            case "matches":
            case "looking-at":
            case "region-matches":
            case "region-looking-at": {
                boolean found = mode.endsWith("matches") ? matcher.matches() : matcher.lookingAt();
                return found ? "{\"kind\":\"match\",\"groups\":"
                    + spans(matcher, subject) + "}" : "{\"kind\":\"nomatch\"}";
            }
            case "replace-all": return "{\"kind\":\"replace\",\"text\":"
                + quoted(matcher.replaceAll(argument)) + "}";
            case "replace-first": return "{\"kind\":\"replace\",\"text\":"
                + quoted(matcher.replaceFirst(argument)) + "}";
            case "replace-all-literal": return "{\"kind\":\"replace\",\"text\":"
                + quoted(matcher.replaceAll(Matcher.quoteReplacement(argument))) + "}";
            case "replace-first-literal": return "{\"kind\":\"replace\",\"text\":"
                + quoted(matcher.replaceFirst(Matcher.quoteReplacement(argument))) + "}";
            case "split": {
                int limit = Integer.parseInt(argument);
                String[] pieces = compiled.split(subject, limit);
                StringBuilder out = new StringBuilder("{\"kind\":\"split\",\"pieces\":[");
                for (int i = 0; i < pieces.length; i++) {
                    if (i != 0) out.append(',');
                    out.append(quoted(pieces[i]));
                }
                return out.append("]}").toString();
            }
            default: throw new IllegalArgumentException("unknown mode " + mode);
        }
    }

    public static void main(String[] args) throws Exception {
        BufferedReader input = new BufferedReader(new InputStreamReader(System.in, StandardCharsets.US_ASCII));
        String line;
        while ((line = input.readLine()) != null) {
            try { System.out.println("ok\t" + hex(answer(line))); }
            catch (Exception e) {
                String message = String.valueOf(e.getMessage());
                String kind = message.startsWith("split-pair offset")
                    || message.startsWith("unpaired surrogate")
                    || message.startsWith("offset beyond string") ? "offset"
                    : message.startsWith("odd hex length")
                    || message.startsWith("non-hex input")
                    || message.startsWith("malformed UTF-8")
                    || message.startsWith("expected five fields")
                    || message.startsWith("unknown flag")
                    || message.startsWith("unknown mode")
                    || message.startsWith("invalid region controls")
                    || message.startsWith("invalid byte offset")
                    || (line.startsWith("split\t")
                        && e instanceof NumberFormatException) ? "input"
                    : line.startsWith("replace-all\t")
                    || line.startsWith("replace-first\t")
                    || line.startsWith("replace-all-literal\t")
                    || line.startsWith("replace-first-literal\t") ? "template" : "runtime";
                System.out.println("error\t" + hex("{\"kind\":" + quoted(kind)
                    + ",\"class\":" + quoted(e.getClass().getSimpleName())
                    + ",\"message\":" + quoted(message) + "}"));
            }
        }
    }
}
