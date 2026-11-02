package feed.json;

import java.nio.charset.StandardCharsets;

/**
 * Validates a JSON document (RFC 8259) and captures chosen top-level object members, without building a tree or
 * allocating. Used for POST bodies and JWT headers/claims. Reusable, one per worker; not thread-safe.
 */
public final class JsonReader {

    public static final int INVALID = 0;
    public static final int OBJECT = 1;
    /** Valid JSON whose top-level value isn't an object. */
    public static final int NOT_OBJECT = 2;

    public static final int MISSING = 0;
    public static final int STRING = 1;
    public static final int NUMBER = 2;
    public static final int OTHER = 3;

    private static final int MAX_DEPTH = 64;
    private static final int MAX_KEY = 64;

    private final byte[][] keys;
    private final int[] types;
    private final int[] starts; // strings: inside the quotes; numbers: the literal
    private final int[] ends;
    private final byte[] scratch = new byte[MAX_KEY];
    private byte[] doc;

    public JsonReader(String... keys) {
        this.keys = new byte[keys.length][];
        for (int i = 0; i < keys.length; i++) {
            this.keys[i] = keys[i].getBytes(StandardCharsets.UTF_8);
        }
        types = new int[keys.length];
        starts = new int[keys.length];
        ends = new int[keys.length];
    }

    /** Reads the document in {@code b[from, to)}: INVALID, OBJECT or NOT_OBJECT. */
    public int read(byte[] b, int from, int to) {
        doc = b;
        java.util.Arrays.fill(types, MISSING);
        int pos = skipSpace(b, from, to);
        if (pos >= to) {
            return INVALID;
        }
        boolean object = b[pos] == '{';
        pos = value(b, pos, to, 0, true);
        if (pos < 0 || skipSpace(b, pos, to) != to) {
            java.util.Arrays.fill(types, MISSING);
            return INVALID;
        }
        return object ? OBJECT : NOT_OBJECT;
    }

    /** The type of the captured member {@code key} (index into the constructor's keys); last occurrence wins. */
    public int type(int key) {
        return types[key];
    }

    /** The decoded string's length in UTF-8 bytes, at most (for sizing {@code out}). */
    public int maxDecodedLength(int key) {
        return ends[key] - starts[key];
    }

    /** Decodes captured string member {@code key} as UTF-8 into {@code out} at {@code off}; returns the end offset. */
    public int decodeString(int key, byte[] out, int off) {
        return decode(doc, starts[key], ends[key], out, off);
    }

    public boolean stringEquals(int key, String ascii) {
        if (types[key] != STRING || maxDecodedLength(key) > MAX_KEY) {
            return false;
        }
        int len = decodeString(key, scratch, 0);
        if (len != ascii.length()) {
            return false;
        }
        for (int i = 0; i < len; i++) {
            if (scratch[i] != ascii.charAt(i)) {
                return false;
            }
        }
        return true;
    }

    public double number(int key) {
        int i = starts[key];
        int end = ends[key];
        boolean negative = doc[i] == '-';
        if (negative) {
            i++;
        }
        long value = 0;
        int digits = 0;
        while (i < end && doc[i] >= '0' && doc[i] <= '9' && digits < 18) {
            value = value * 10 + (doc[i++] - '0');
            digits++;
        }
        if (i == end) {
            return negative ? -value : value;
        }
        return Double.parseDouble(new String(doc, starts[key], end - starts[key], StandardCharsets.US_ASCII));
    }

    // ---- validation ----

    /** Validates one value at {@code pos}; returns the position after it, or -1. */
    private int value(byte[] b, int pos, int to, int depth, boolean capture) {
        if (pos >= to) {
            return -1;
        }
        return switch (b[pos]) {
            case '{' -> object(b, pos, to, depth, capture);
            case '[' -> array(b, pos, to, depth);
            case '"' -> stringEnd(b, pos, to);
            case 't' -> literal(b, pos, to, "true");
            case 'f' -> literal(b, pos, to, "false");
            case 'n' -> literal(b, pos, to, "null");
            default -> number(b, pos, to);
        };
    }

    private int object(byte[] b, int pos, int to, int depth, boolean capture) {
        if (depth >= MAX_DEPTH) {
            return -1;
        }
        pos = skipSpace(b, pos + 1, to);
        if (pos < to && b[pos] == '}') {
            return pos + 1;
        }
        while (true) {
            if (pos >= to || b[pos] != '"') {
                return -1;
            }
            int keyStart = pos + 1;
            pos = stringEnd(b, pos, to);
            if (pos < 0) {
                return -1;
            }
            int keyEnd = pos - 1;
            pos = skipSpace(b, pos, to);
            if (pos >= to || b[pos] != ':') {
                return -1;
            }
            pos = skipSpace(b, pos + 1, to);
            int valueStart = pos;
            pos = value(b, pos, to, depth + 1, false);
            if (pos < 0) {
                return -1;
            }
            if (capture) {
                capture(b, keyStart, keyEnd, valueStart, pos);
            }
            pos = skipSpace(b, pos, to);
            if (pos >= to) {
                return -1;
            }
            if (b[pos] == '}') {
                return pos + 1;
            }
            if (b[pos] != ',') {
                return -1;
            }
            pos = skipSpace(b, pos + 1, to);
        }
    }

    private int array(byte[] b, int pos, int to, int depth) {
        if (depth >= MAX_DEPTH) {
            return -1;
        }
        pos = skipSpace(b, pos + 1, to);
        if (pos < to && b[pos] == ']') {
            return pos + 1;
        }
        while (true) {
            pos = value(b, pos, to, depth + 1, false);
            if (pos < 0) {
                return -1;
            }
            pos = skipSpace(b, pos, to);
            if (pos >= to) {
                return -1;
            }
            if (b[pos] == ']') {
                return pos + 1;
            }
            if (b[pos] != ',') {
                return -1;
            }
            pos = skipSpace(b, pos + 1, to);
        }
    }

    /** {@code pos} is at the opening quote; returns the position after the closing quote, or -1. */
    private static int stringEnd(byte[] b, int pos, int to) {
        int i = pos + 1;
        while (i < to) {
            int c = b[i] & 0xff;
            if (c == '"') {
                return i + 1;
            }
            if (c == '\\') {
                if (i + 1 >= to) {
                    return -1;
                }
                switch (b[i + 1]) {
                    case '"', '\\', '/', 'b', 'f', 'n', 'r', 't' -> i += 2;
                    case 'u' -> {
                        if (i + 6 > to) {
                            return -1;
                        }
                        for (int k = i + 2; k < i + 6; k++) {
                            if (hex(b[k]) < 0) {
                                return -1;
                            }
                        }
                        i += 6;
                    }
                    default -> {
                        return -1;
                    }
                }
            } else if (c < 0x20) {
                return -1;
            } else {
                i++;
            }
        }
        return -1;
    }

    private static int number(byte[] b, int pos, int to) {
        int i = pos;
        if (i < to && b[i] == '-') {
            i++;
        }
        if (i >= to) {
            return -1;
        }
        if (b[i] == '0') {
            i++;
        } else if (b[i] >= '1' && b[i] <= '9') {
            while (i < to && b[i] >= '0' && b[i] <= '9') {
                i++;
            }
        } else {
            return -1;
        }
        if (i < to && b[i] == '.') {
            i = digits(b, i + 1, to);
            if (i < 0) {
                return -1;
            }
        }
        if (i < to && (b[i] == 'e' || b[i] == 'E')) {
            i++;
            if (i < to && (b[i] == '+' || b[i] == '-')) {
                i++;
            }
            i = digits(b, i, to);
        }
        return i;
    }

    /** At least one digit from {@code i}; returns the position after them, or -1. */
    private static int digits(byte[] b, int i, int to) {
        int start = i;
        while (i < to && b[i] >= '0' && b[i] <= '9') {
            i++;
        }
        return i == start ? -1 : i;
    }

    private static int literal(byte[] b, int pos, int to, String word) {
        if (pos + word.length() > to) {
            return -1;
        }
        for (int i = 0; i < word.length(); i++) {
            if (b[pos + i] != word.charAt(i)) {
                return -1;
            }
        }
        return pos + word.length();
    }

    private static int skipSpace(byte[] b, int pos, int to) {
        while (pos < to && (b[pos] == ' ' || b[pos] == '\t' || b[pos] == '\n' || b[pos] == '\r')) {
            pos++;
        }
        return pos;
    }

    // ---- capture ----

    private void capture(byte[] b, int keyStart, int keyEnd, int valueStart, int valueEnd) {
        if (keys.length == 0 || keyEnd - keyStart > MAX_KEY) {
            return;
        }
        int keyLen = decode(b, keyStart, keyEnd, scratch, 0);
        for (int k = 0; k < keys.length; k++) {
            if (java.util.Arrays.equals(scratch, 0, keyLen, keys[k], 0, keys[k].length)) {
                byte first = b[valueStart];
                if (first == '"') {
                    types[k] = STRING;
                    starts[k] = valueStart + 1;
                    ends[k] = valueEnd - 1;
                } else {
                    types[k] = first == '-' || (first >= '0' && first <= '9') ? NUMBER : OTHER;
                    starts[k] = valueStart;
                    ends[k] = valueEnd;
                }
            }
        }
    }

    /** Decodes a validated string body (between the quotes) to UTF-8; returns the end offset in {@code out}. */
    private static int decode(byte[] b, int from, int to, byte[] out, int off) {
        int i = from;
        while (i < to) {
            byte c = b[i];
            if (c != '\\') {
                out[off++] = c;
                i++;
                continue;
            }
            byte e = b[i + 1];
            if (e != 'u') {
                out[off++] = switch (e) {
                    case 'b' -> '\b';
                    case 'f' -> '\f';
                    case 'n' -> '\n';
                    case 'r' -> '\r';
                    case 't' -> '\t';
                    default -> e; // " \ /
                };
                i += 2;
                continue;
            }
            int cp = hex4(b, i + 2);
            i += 6;
            if (cp >= 0xD800 && cp <= 0xDBFF && i + 6 <= to && b[i] == '\\' && b[i + 1] == 'u') {
                int low = hex4(b, i + 2);
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                    i += 6;
                }
            }
            off = utf8(cp, out, off);
        }
        return off;
    }

    private static int utf8(int cp, byte[] out, int off) {
        if (cp < 0x80) {
            out[off++] = (byte) cp;
        } else if (cp < 0x800) {
            out[off++] = (byte) (0xC0 | (cp >> 6));
            out[off++] = (byte) (0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out[off++] = (byte) (0xE0 | (cp >> 12));
            out[off++] = (byte) (0x80 | ((cp >> 6) & 0x3F));
            out[off++] = (byte) (0x80 | (cp & 0x3F));
        } else {
            out[off++] = (byte) (0xF0 | (cp >> 18));
            out[off++] = (byte) (0x80 | ((cp >> 12) & 0x3F));
            out[off++] = (byte) (0x80 | ((cp >> 6) & 0x3F));
            out[off++] = (byte) (0x80 | (cp & 0x3F));
        }
        return off;
    }

    private static int hex4(byte[] b, int at) {
        return (hex(b[at]) << 12) | (hex(b[at + 1]) << 8) | (hex(b[at + 2]) << 4) | hex(b[at + 3]);
    }

    private static int hex(byte c) {
        if (c >= '0' && c <= '9') {
            return c - '0';
        }
        if (c >= 'a' && c <= 'f') {
            return c - 'a' + 10;
        }
        if (c >= 'A' && c <= 'F') {
            return c - 'A' + 10;
        }
        return -1;
    }
}
