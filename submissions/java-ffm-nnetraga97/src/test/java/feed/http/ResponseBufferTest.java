package feed.http;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.nio.charset.StandardCharsets;

import org.junit.jupiter.api.Test;

class ResponseBufferTest {

    private static byte[] ascii(String s) {
        return s.getBytes(StandardCharsets.UTF_8);
    }

    private static String body(ResponseBuffer out) {
        int start = out.finish(200, ResponseBuffer.NO_CONNECTION_HEADER);
        String all = new String(out.bytes(), start, out.end() - start, StandardCharsets.UTF_8);
        return all.substring(all.indexOf("\r\n\r\n") + 4);
    }

    private static String text(ResponseBuffer out, int start) {
        return new String(out.bytes(), start, out.end() - start, StandardCharsets.UTF_8);
    }

    @Test
    void anEmptyBodyIsSentAsItIsWhenGzipIsAsked() {
        try (Gzip gzip = Gzip.open(1)) {
            ResponseBuffer out = new ResponseBuffer(64);
            out.begin();
            int start = out.finish(200, ResponseBuffer.NO_CONNECTION_HEADER, gzip);
            assertEquals("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 0\r\n\r\n",
                    new String(out.bytes(), start, out.end() - start, StandardCharsets.UTF_8));
        }
    }

    @Test
    void writesAResponseWithContentLength() {
        ResponseBuffer out = new ResponseBuffer(64);
        out.begin();
        out.raw(ascii("{\"liked\":true,\"post_id\":"));
        out.number(42);
        out.raw(ascii("}"));
        int start = out.finish(201, ResponseBuffer.NO_CONNECTION_HEADER);
        assertEquals("HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n"
                + "{\"liked\":true,\"post_id\":42}", text(out, start));
    }

    @Test
    void addsConnectionHeadersWhenAsked() {
        ResponseBuffer out = new ResponseBuffer(64);
        out.begin();
        out.raw(ascii("{}"));
        int start = out.finish(404, ResponseBuffer.CLOSE);
        assertEquals("HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 2\r\n"
                + "Connection: close\r\n\r\n{}", text(out, start));
        out.begin();
        out.raw(ascii("{}"));
        start = out.finish(200, ResponseBuffer.KEEP_ALIVE);
        assertEquals("HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: 2\r\n"
                + "Connection: keep-alive\r\n\r\n{}", text(out, start));
    }

    @Test
    void reasonPhrases() {
        for (int[] s : new int[][] {{200}, {201}, {400}, {401}, {404}, {413}, {431}, {500}, {503}}) {
            ResponseBuffer out = new ResponseBuffer(8);
            out.begin();
            int start = out.finish(s[0], ResponseBuffer.NO_CONNECTION_HEADER);
            String line = new String(out.bytes(), start, 13, StandardCharsets.US_ASCII);
            assertEquals("HTTP/1.1 " + s[0] + " ", line);
        }
    }

    @Test
    void writesNumbers() {
        ResponseBuffer out = new ResponseBuffer(8);
        out.begin();
        for (long v : new long[] {0, 7, -3, 1234567890123L, Long.MAX_VALUE}) {
            out.number(v);
            out.raw(ascii(","));
        }
        assertEquals("0,7,-3,1234567890123," + Long.MAX_VALUE + ",", body(out));
    }

    @Test
    void escapesStrings() {
        ResponseBuffer out = new ResponseBuffer(8);
        out.begin();
        byte[] s = ascii("a\"b\\c/d\ne\tf\u0001 café ✓ <b>&amp;</b>");
        out.string(s, 0, s.length);
        assertEquals("\"a\\\"b\\\\c/d\\ne\\tf\\u0001 café ✓ <b>&amp;</b>\"", body(out));
    }

    @Test
    void writesTimestamps() {
        ResponseBuffer out = new ResponseBuffer(8);
        out.begin();
        out.timestamp(0);
        assertEquals("\"2000-01-01T00:00:00.000Z\"", body(out));
    }

    @Test
    void growsForLargeBodies() {
        ResponseBuffer out = new ResponseBuffer(16);
        out.begin();
        byte[] big = ascii("x".repeat(100_000));
        out.string(big, 0, big.length);
        assertEquals(100_002, body(out).length());
    }
}
