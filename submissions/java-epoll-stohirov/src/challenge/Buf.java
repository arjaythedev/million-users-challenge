package challenge;

import java.util.Arrays;

final class Buf {
    private static final byte[] HEX = Out.ascii("0123456789abcdef");
    private static final long PG_EPOCH_MILLIS = 946_684_800_000L, DAY_MILLIS = 86_400_000L;

    byte[] a;
    int len;

    Buf(int capacity) { a = new byte[capacity]; }

    void ensure(int n) {
        if (len + n > a.length) a = Arrays.copyOf(a, Math.max(a.length * 2, len + n));
    }

    void put(int b) {
        ensure(1);
        a[len++] = (byte) b;
    }

    void put(byte[] s) { put(s, 0, s.length); }

    void put(byte[] s, int off, int n) {
        ensure(n);
        System.arraycopy(s, off, a, len, n);
        len += n;
    }

    void i16(int v) {
        ensure(2);
        a[len++] = (byte) (v >>> 8);
        a[len++] = (byte) v;
    }

    void i32(int v) {
        ensure(4);
        setI32(len, v);
        len += 4;
    }

    void setI32(int at, int v) {
        a[at] = (byte) (v >>> 24);
        a[at + 1] = (byte) (v >>> 16);
        a[at + 2] = (byte) (v >>> 8);
        a[at + 3] = (byte) v;
    }

    void i64(long v) {
        i32((int) (v >>> 32));
        i32((int) v);
    }

    void cstr(String s) {
        ensure(s.length() + 1);
        for (int k = 0; k < s.length(); k++) a[len++] = (byte) s.charAt(k);
        a[len++] = 0;
    }

    static int i32(byte[] b, int at) {
        return (b[at] & 0xff) << 24 | (b[at + 1] & 0xff) << 16 | (b[at + 2] & 0xff) << 8 | (b[at + 3] & 0xff);
    }

    static long i64(byte[] b, int at) {
        return (long) i32(b, at) << 32 | (i32(b, at + 4) & 0xffffffffL);
    }

    void num(long v) {
        if (v < 0) {
            put('-');
            v = -v;
        }
        int n = Out.digits(v);
        ensure(n);
        int p = len + n;
        do {
            a[--p] = (byte) ('0' + (v % 10));
            v /= 10;
        } while (v != 0);
        len += n;
    }

    void escaped(byte[] s, int off, int n) {
        ensure(n);
        int end = off + n, run = off;
        for (int k = off; k < end; k++) {
            int c = s[k];
            if (c >= 0x20 && c != '"' && c != '\\' || c < 0) continue;
            put(s, run, k - run);
            run = k + 1;
            switch (c) {
                case '"' -> { put('\\'); put('"'); }
                case '\\' -> { put('\\'); put('\\'); }
                case '\n' -> { put('\\'); put('n'); }
                case '\r' -> { put('\\'); put('r'); }
                case '\t' -> { put('\\'); put('t'); }
                case '\b' -> { put('\\'); put('b'); }
                case '\f' -> { put('\\'); put('f'); }
                default -> {
                    ensure(6);
                    a[len++] = '\\';
                    a[len++] = 'u';
                    a[len++] = '0';
                    a[len++] = '0';
                    a[len++] = HEX[c >> 4];
                    a[len++] = HEX[c & 15];
                }
            }
        }
        put(s, run, end - run);
    }

    void timestamp(long pgMicros) {
        long millis = Math.floorDiv(pgMicros, 1000) + PG_EPOCH_MILLIS;
        long days = Math.floorDiv(millis, DAY_MILLIS);
        int ms = (int) Math.floorMod(millis, DAY_MILLIS);
        long z = days + 719_468;
        long era = Math.floorDiv(z, 146_097);
        int doe = (int) (z - era * 146_097);
        int yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
        int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        int mp = (5 * doy + 2) / 153;
        int day = doy - (153 * mp + 2) / 5 + 1;
        int month = mp < 10 ? mp + 3 : mp - 9;
        long year = yoe + era * 400 + (month <= 2 ? 1 : 0);
        ensure(24);
        digits((int) year, 4);
        a[len++] = '-';
        digits(month, 2);
        a[len++] = '-';
        digits(day, 2);
        a[len++] = 'T';
        digits(ms / 3_600_000, 2);
        a[len++] = ':';
        digits(ms / 60_000 % 60, 2);
        a[len++] = ':';
        digits(ms / 1000 % 60, 2);
        a[len++] = '.';
        digits(ms % 1000, 3);
        a[len++] = 'Z';
    }

    private void digits(int v, int width) {
        for (int k = width - 1; k >= 0; k--) {
            a[len + k] = (byte) ('0' + v % 10);
            v /= 10;
        }
        len += width;
    }
}
