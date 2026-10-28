package feed.http;

import java.nio.charset.StandardCharsets;

import feed.pg.PgTime;

/**
 * Builds one HTTP response: the JSON body is written first, after room reserved for the head, and {@link #finish}
 * then writes the status line and headers right in front of it, so nothing is copied. Reusable; grows if a body
 * needs it (rarely: a feed of twenty 500-character posts is ~15 KB). A gzipped body is compressed into a second
 * buffer, and the two swap roles, so that copies nothing either.
 */
public final class ResponseBuffer {

    public static final int NO_CONNECTION_HEADER = 0;
    public static final int CLOSE = 1;
    public static final int KEEP_ALIVE = 2;

    /** Room for the longest head finish() writes. */
    private static final int HEAD_ROOM = 192;

    private static final byte[] HEX = "0123456789abcdef".getBytes(StandardCharsets.US_ASCII);
    private static final byte[][] STATUS_LINES = new byte[600][];
    private static final byte[] CONTENT_ENCODING = ascii("Content-Encoding: gzip\r\n");
    private static final byte[] CONTENT_LENGTH =
            ascii("Content-Type: application/json\r\nContent-Length: ");
    private static final byte[] END_CLOSE = ascii("\r\nConnection: close\r\n\r\n");
    private static final byte[] END_KEEP_ALIVE = ascii("\r\nConnection: keep-alive\r\n\r\n");
    private static final byte[] END = ascii("\r\n\r\n");

    static {
        String[][] reasons = {
            {"100", "Continue"}, {"200", "OK"}, {"201", "Created"}, {"400", "Bad Request"},
            {"401", "Unauthorized"}, {"404", "Not Found"}, {"413", "Payload Too Large"},
            {"431", "Request Header Fields Too Large"}, {"500", "Internal Server Error"},
            {"503", "Service Unavailable"},
        };
        for (String[] r : reasons) {
            STATUS_LINES[Integer.parseInt(r[0])] = ascii("HTTP/1.1 " + r[0] + " " + r[1] + "\r\n");
        }
    }

    private byte[] buf;
    private int pos;
    private byte[] spare;
    private final byte[] digits = new byte[20];

    public ResponseBuffer(int capacity) {
        buf = new byte[HEAD_ROOM + capacity];
    }

    static byte[] ascii(String s) {
        return s.getBytes(StandardCharsets.UTF_8);
    }

    /** Starts a new body. */
    public void begin() {
        pos = HEAD_ROOM;
    }

    public void raw(byte b) {
        ensure(1);
        buf[pos++] = b;
    }

    public void raw(byte[] bytes) {
        raw(bytes, 0, bytes.length);
    }

    public void raw(byte[] bytes, int off, int len) {
        ensure(len);
        System.arraycopy(bytes, off, buf, pos, len);
        pos += len;
    }

    public void number(long value) {
        ensure(20);
        if (value < 0) {
            if (value == Long.MIN_VALUE) {
                raw(ascii(Long.toString(value)));
                return;
            }
            buf[pos++] = '-';
            value = -value;
        }
        int n = 0;
        do {
            digits[n++] = (byte) ('0' + value % 10);
            value /= 10;
        } while (value > 0);
        while (n > 0) {
            buf[pos++] = digits[--n];
        }
    }

    /** A JSON string (with quotes) from UTF-8 bytes: escapes '"', '\' and control characters. */
    public void string(byte[] utf8, int off, int len) {
        ensure(len * 6 + 2);
        byte[] b = buf;
        int p = pos;
        b[p++] = '"';
        for (int i = off, end = off + len; i < end; i++) {
            byte c = utf8[i];
            if (c == '"' || c == '\\') {
                b[p++] = '\\';
                b[p++] = c;
            } else if (c >= 0 && c < 0x20) {
                switch (c) {
                    case '\n' -> { b[p++] = '\\'; b[p++] = 'n'; }
                    case '\r' -> { b[p++] = '\\'; b[p++] = 'r'; }
                    case '\t' -> { b[p++] = '\\'; b[p++] = 't'; }
                    case '\b' -> { b[p++] = '\\'; b[p++] = 'b'; }
                    case '\f' -> { b[p++] = '\\'; b[p++] = 'f'; }
                    default -> {
                        b[p++] = '\\';
                        b[p++] = 'u';
                        b[p++] = '0';
                        b[p++] = '0';
                        b[p++] = HEX[c >> 4];
                        b[p++] = HEX[c & 0xF];
                    }
                }
            } else {
                b[p++] = c;
            }
        }
        b[p++] = '"';
        pos = p;
    }

    /** A JSON string holding a TIMESTAMPTZ as YYYY-MM-DDTHH:MM:SS.mmmZ. */
    public void timestamp(long pgMicros) {
        ensure(PgTime.LENGTH + 2);
        buf[pos++] = '"';
        pos = PgTime.format(pgMicros, buf, pos);
        buf[pos++] = '"';
    }

    /** The response is {@code bytes()[start, end())}, {@code start} being what {@link #finish} returned. */
    public byte[] bytes() {
        return buf;
    }

    public int end() {
        return pos;
    }

    public int bodyLength() {
        return pos - HEAD_ROOM;
    }

    /** Writes the head in front of the body; returns the response's start offset (it ends at {@link #pos}). */
    public int finish(int status, int connectionHeader) {
        return finish(status, connectionHeader, null);
    }

    /**
     * As {@link #finish(int, int)}, but first gzips the body with {@code gzip} (if not null) when that makes it
     * smaller. Call {@link #bytes()} after this: it may return the other buffer.
     */
    public int finish(int status, int connectionHeader, Gzip gzip) {
        boolean gzipped = false;
        if (gzip != null && pos > HEAD_ROOM) { // an empty body can't get smaller
            if (spare == null || spare.length < buf.length) {
                spare = new byte[buf.length];
            }
            int n = gzip.compress(buf, HEAD_ROOM, pos - HEAD_ROOM, spare, HEAD_ROOM, pos - HEAD_ROOM - 1);
            if (n > 0) {
                byte[] body = spare;
                spare = buf;
                buf = body;
                pos = HEAD_ROOM + n;
                gzipped = true;
            }
        }
        int bodyLength = pos - HEAD_ROOM;
        byte[] end = connectionHeader == CLOSE ? END_CLOSE : connectionHeader == KEEP_ALIVE ? END_KEEP_ALIVE : END;
        int n = 0;
        int v = bodyLength;
        do {
            digits[n++] = (byte) ('0' + v % 10);
            v /= 10;
        } while (v > 0);
        byte[] statusLine = STATUS_LINES[status];
        int start = HEAD_ROOM - end.length - n - CONTENT_LENGTH.length - statusLine.length
                - (gzipped ? CONTENT_ENCODING.length : 0);
        int p = start;
        System.arraycopy(statusLine, 0, buf, p, statusLine.length);
        p += statusLine.length;
        if (gzipped) {
            System.arraycopy(CONTENT_ENCODING, 0, buf, p, CONTENT_ENCODING.length);
            p += CONTENT_ENCODING.length;
        }
        System.arraycopy(CONTENT_LENGTH, 0, buf, p, CONTENT_LENGTH.length);
        p += CONTENT_LENGTH.length;
        while (n > 0) {
            buf[p++] = digits[--n];
        }
        System.arraycopy(end, 0, buf, p, end.length);
        return start;
    }

    private void ensure(int n) {
        if (buf.length - pos < n) {
            buf = java.util.Arrays.copyOf(buf, Math.max(buf.length * 2, pos + n));
        }
    }
}
