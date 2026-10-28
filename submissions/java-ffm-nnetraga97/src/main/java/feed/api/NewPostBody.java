package feed.api;

import feed.http.Bytes;
import feed.http.HttpParser;
import feed.json.JsonReader;

/**
 * Validates a {@code POST /posts} body: a JSON object whose "body" is a string that, once trimmed like JavaScript's
 * {@code String.prototype.trim}, holds 1 to 500 characters (code points). Reusable, one per worker.
 */
final class NewPostBody {

    enum Result { OK, MALFORMED, MISSING, TOO_LONG }

    private static final int MAX_CHARS = 500;

    private final JsonReader json = new JsonReader("body");
    private final byte[] text = new byte[HttpParser.MAX_BODY];
    private int start;
    private int end;

    /** Checks the JSON in {@code b[from, to)}. When OK, the trimmed text is {@link #text()}[start, start + length). */
    Result read(byte[] b, int from, int to) {
        if (!Bytes.isValidUtf8(b, from, to)) {
            return Result.MALFORMED; // JSON text is UTF-8 (RFC 8259), ignored members included
        }
        int parsed = json.read(b, from, to);
        if (parsed == JsonReader.INVALID) {
            return Result.MALFORMED;
        }
        if (parsed != JsonReader.OBJECT || json.type(0) != JsonReader.STRING) {
            return Result.MISSING;
        }
        end = json.decodeString(0, text, 0);
        if (!Bytes.isValidUtf8(text, 0, end)) {
            return Result.MALFORMED; // an escape decoded to a lone surrogate; also keeps the trimming in bounds
        }
        start = 0;
        trim();
        if (start == end) {
            return Result.MISSING;
        }
        return codePoints(start, end) > MAX_CHARS ? Result.TOO_LONG : Result.OK;
    }

    byte[] text() {
        return text;
    }

    int start() {
        return start;
    }

    int length() {
        return end - start;
    }

    private void trim() {
        while (start < end && isJsWhitespace(codePointAt(start))) {
            start += utf8Length(text[start]);
        }
        while (end > start) {
            int last = end - 1;
            while (last > start && (text[last] & 0xC0) == 0x80) {
                last--; // back to the first byte of the last character
            }
            if (!isJsWhitespace(codePointAt(last))) {
                break;
            }
            end = last;
        }
    }

    private int codePoints(int from, int to) {
        int n = 0;
        for (int i = from; i < to; i++) {
            if ((text[i] & 0xC0) != 0x80) {
                n++;
            }
        }
        return n;
    }

    private int codePointAt(int i) {
        int c = text[i] & 0xff;
        if (c < 0x80) {
            return c;
        }
        if (c < 0xE0) {
            return ((c & 0x1F) << 6) | (text[i + 1] & 0x3F);
        }
        if (c < 0xF0) {
            return ((c & 0x0F) << 12) | ((text[i + 1] & 0x3F) << 6) | (text[i + 2] & 0x3F);
        }
        return ((c & 0x07) << 18) | ((text[i + 1] & 0x3F) << 12) | ((text[i + 2] & 0x3F) << 6) | (text[i + 3] & 0x3F);
    }

    private static int utf8Length(byte lead) {
        int b = lead & 0xff;
        return b < 0x80 ? 1 : b < 0xE0 ? 2 : b < 0xF0 ? 3 : 4;
    }

    /** What JavaScript's trim() removes, which is what the reference servers do. */
    private static boolean isJsWhitespace(int cp) {
        return (cp >= 9 && cp <= 13) || cp == 0xFEFF || Character.isSpaceChar(cp);
    }
}
