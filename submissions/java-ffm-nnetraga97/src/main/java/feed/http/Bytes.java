package feed.http;

/** Allocation-free helpers on byte ranges. */
public final class Bytes {

    private Bytes() {
    }

    /** True if {@code b[from, to)} holds exactly the ASCII string {@code s}. */
    public static boolean equals(byte[] b, int from, int to, String s) {
        if (to - from != s.length()) {
            return false;
        }
        for (int i = 0; i < s.length(); i++) {
            if (b[from + i] != s.charAt(i)) {
                return false;
            }
        }
        return true;
    }

    public static int indexOf(byte[] b, int from, int to, byte c) {
        for (int i = from; i < to; i++) {
            if (b[i] == c) {
                return i;
            }
        }
        return -1;
    }

    /** A positive decimal of at most 18 digits (so it fits a long), or 0. */
    public static long positiveId(byte[] b, int from, int to) {
        if (from >= to || to - from > 18) {
            return 0;
        }
        long v = 0;
        for (int i = from; i < to; i++) {
            if (b[i] < '0' || b[i] > '9') {
                return 0;
            }
            v = v * 10 + (b[i] - '0');
        }
        return v;
    }

    /** Well-formed UTF-8 (RFC 3629): no truncated, overlong or surrogate sequences, nothing above U+10FFFF. */
    public static boolean isValidUtf8(byte[] b, int from, int to) {
        int i = from;
        while (i < to) {
            int c = b[i] & 0xff;
            if (c < 0x80) {
                i++;
                continue;
            }
            int n;
            int min;
            if (c >= 0xC2 && c <= 0xDF) {
                n = 1;
                min = 0x80;
            } else if (c >= 0xE0 && c <= 0xEF) {
                n = 2;
                min = 0x800;
            } else if (c >= 0xF0 && c <= 0xF4) {
                n = 3;
                min = 0x10000;
            } else {
                return false;
            }
            if (i + n >= to) {
                return false; // truncated
            }
            int cp = c & (0x3F >> n);
            for (int k = 1; k <= n; k++) {
                int cc = b[i + k] & 0xff;
                if ((cc & 0xC0) != 0x80) {
                    return false;
                }
                cp = (cp << 6) | (cc & 0x3F);
            }
            if (cp < min || cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
                return false;
            }
            i += n + 1;
        }
        return true;
    }
}
