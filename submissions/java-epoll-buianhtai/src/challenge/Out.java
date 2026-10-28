package challenge;

import java.nio.charset.StandardCharsets;
import java.util.Arrays;

/** A growable byte buffer with the few encoders the server needs. */
final class Out {
    byte[] b;
    int n;

    Out(int cap) {
        b = new byte[cap];
    }

    void reset() {
        n = 0;
    }

    void ensure(int more) {
        if (n + more > b.length) b = Arrays.copyOf(b, Math.max(b.length * 2, n + more));
    }

    Out put(int c) {
        ensure(1);
        b[n++] = (byte) c;
        return this;
    }

    Out ascii(String s) {
        int len = s.length();
        ensure(len);
        for (int i = 0; i < len; i++) b[n++] = (byte) s.charAt(i);
        return this;
    }

    Out bytes(byte[] s, int off, int len) {
        ensure(len);
        System.arraycopy(s, off, b, n, len);
        n += len;
        return this;
    }

    Out dec(long v) {
        ensure(20);
        if (v < 0) {
            b[n++] = '-';
            v = -v;
        }
        int start = n;
        do {
            b[n++] = (byte) ('0' + v % 10);
            v /= 10;
        } while (v != 0);
        for (int i = start, j = n - 1; i < j; i++, j--) {
            byte t = b[i];
            b[i] = b[j];
            b[j] = t;
        }
        return this;
    }

    private Out pad(int v, int width) {
        ensure(width);
        for (int i = width - 1; i >= 0; i--) {
            b[n + i] = (byte) ('0' + v % 10);
            v /= 10;
        }
        n += width;
        return this;
    }

    /** A JSON string literal from UTF-8 bytes; non-ASCII bytes are copied as they are. */
    Out jstr(byte[] s, int off, int len) {
        ensure(len + 2);
        b[n++] = '"';
        for (int i = off, end = off + len; i < end; i++) {
            byte c = s[i];
            if (c == '"' || c == '\\') {
                ensure(end - i + 2);
                b[n++] = '\\';
                b[n++] = c;
            } else if (c >= 0x20 || c < 0) {
                ensure(1);
                b[n++] = c;
            } else {
                ensure(end - i + 6);
                switch (c) {
                    case '\n' -> {
                        b[n++] = '\\';
                        b[n++] = 'n';
                    }
                    case '\r' -> {
                        b[n++] = '\\';
                        b[n++] = 'r';
                    }
                    case '\t' -> {
                        b[n++] = '\\';
                        b[n++] = 't';
                    }
                    default -> {
                        b[n++] = '\\';
                        b[n++] = 'u';
                        b[n++] = '0';
                        b[n++] = '0';
                        b[n++] = (byte) Character.forDigit(c >> 4, 16);
                        b[n++] = (byte) Character.forDigit(c & 15, 16);
                    }
                }
            }
        }
        ensure(1);
        b[n++] = '"';
        return this;
    }

    Out jstr(String s) {
        byte[] u = s.getBytes(StandardCharsets.UTF_8);
        return jstr(u, 0, u.length);
    }

    /** A TIMESTAMPTZ (microseconds since 2000-01-01 UTC) as `2025-12-31T23:59:19.409Z`, truncated to milliseconds. */
    Out timestamp(long micros2000) {
        long ms = Math.floorDiv(micros2000, 1000L) + 946_684_800_000L;
        long days = Math.floorDiv(ms, 86_400_000L);
        int rem = (int) Math.floorMod(ms, 86_400_000L);
        long z = days + 719_468;
        long era = Math.floorDiv(z, 146_097);
        long doe = z - era * 146_097;
        long yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
        long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        long mp = (5 * doy + 2) / 153;
        int d = (int) (doy - (153 * mp + 2) / 5 + 1);
        int m = (int) (mp < 10 ? mp + 3 : mp - 9);
        int y = (int) (yoe + era * 400 + (m <= 2 ? 1 : 0));
        pad(y, 4).put('-');
        pad(m, 2).put('-');
        pad(d, 2).put('T');
        pad(rem / 3_600_000, 2).put(':');
        pad(rem / 60_000 % 60, 2).put(':');
        pad(rem / 1000 % 60, 2).put('.');
        pad(rem % 1000, 3).put('Z');
        return this;
    }
}
