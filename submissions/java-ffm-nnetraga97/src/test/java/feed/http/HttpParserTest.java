package feed.http;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.charset.StandardCharsets;

import org.junit.jupiter.api.Test;

class HttpParserTest {

    private final HttpParser p = new HttpParser();
    private byte[] buf;

    private int parse(String request) {
        buf = ("##" + request).getBytes(StandardCharsets.UTF_8);
        return p.parse(buf, 2, buf.length - 2);
    }

    private String path() {
        return new String(buf, p.pathStart(), p.pathEnd() - p.pathStart(), StandardCharsets.UTF_8);
    }

    private String body() {
        return new String(buf, p.bodyStart(), p.bodyEnd() - p.bodyStart(), StandardCharsets.UTF_8);
    }

    private String auth() {
        if (p.authorizationLength() < 0) {
            return null;
        }
        return new String(buf, p.authorizationStart(), p.authorizationLength(), StandardCharsets.UTF_8);
    }

    @Test
    void parsesASimpleGet() {
        String req = "GET /feed HTTP/1.1\r\nHost: x\r\nAccept-Encoding: gzip, deflate\r\n\r\n";
        assertEquals(req.length(), parse(req));
        assertEquals(HttpParser.GET, p.method());
        assertEquals("/feed", path());
        assertTrue(p.keepAlive());
        assertFalse(p.http10());
        assertTrue(p.acceptsGzip());
        assertEquals(0, (p.bodyEnd() - p.bodyStart()));
        assertEquals(null, auth());
    }

    @Test
    void parsesAPostWithBodyAndAuthorization() {
        String req = "POST /posts HTTP/1.1\r\nauthorization:   Bearer abc.def.ghi  \r\nContent-Type: application/json\r\n"
                + "CONTENT-LENGTH: 13\r\n\r\n{\"body\":\"hi\"}";
        assertEquals(req.length(), parse(req));
        assertEquals(HttpParser.POST, p.method());
        assertEquals("/posts", path());
        assertEquals("Bearer abc.def.ghi", auth());
        assertEquals("{\"body\":\"hi\"}", body());
        assertFalse(p.acceptsGzip());
    }

    private boolean acceptsGzip(String... acceptEncoding) {
        StringBuilder req = new StringBuilder("GET /feed HTTP/1.1\r\n");
        for (String value : acceptEncoding) {
            req.append("Accept-Encoding: ").append(value).append("\r\n");
        }
        parse(req.append("\r\n").toString());
        return p.acceptsGzip();
    }

    @Test
    void acceptsGzipOnlyWhenItsQualityIsAboveZero() {
        assertTrue(acceptsGzip("gzip"));
        assertTrue(acceptsGzip("br, GZIP;q=0.5"));
        assertTrue(acceptsGzip("x-gzip"));
        assertTrue(acceptsGzip("*"));
        assertFalse(acceptsGzip("gzip;q=0, identity"));
        assertFalse(acceptsGzip("gzip ; q=0.000"));
        assertFalse(acceptsGzip("*;q=0"));
        assertFalse(acceptsGzip("gzip;q=0, *"), "an explicit refusal beats the wildcard");
        assertFalse(acceptsGzip("identity"));
        assertFalse(acceptsGzip("gzipped"));
    }

    @Test
    void combinesRepeatedAcceptEncodingHeaders() {
        assertTrue(acceptsGzip("gzip", "br"));
        assertTrue(acceptsGzip("br", "gzip"));
        assertFalse(acceptsGzip("br", "deflate"));
    }

    @Test
    void stripsTheQueryString() {
        parse("GET /posts/12?x=1 HTTP/1.1\r\n\r\n");
        assertEquals("/posts/12", path());
    }

    @Test
    void reportsIncompleteHeadersAndBodies() {
        assertEquals(HttpParser.INCOMPLETE, parse("GET /feed HTTP/1.1\r\nHost: x\r\n"));
        assertFalse(p.headersComplete());
        assertEquals(HttpParser.INCOMPLETE, parse("POST /posts HTTP/1.1\r\nContent-Length: 10\r\n\r\n{\"bo"));
        assertTrue(p.headersComplete());
    }

    @Test
    void returnsOnlyTheFirstOfPipelinedRequests() {
        String first = "GET /feed HTTP/1.1\r\n\r\n";
        assertEquals(first.length(), parse(first + "GET /posts/1 HTTP/1.1\r\n\r\n"));
    }

    @Test
    void notesExpectContinue() {
        assertEquals(HttpParser.INCOMPLETE,
                parse("POST /posts HTTP/1.1\r\nExpect: 100-Continue\r\nContent-Length: 5\r\n\r\n"));
        assertTrue(p.expectContinue());
        parse("POST /posts HTTP/1.1\r\nContent-Length: 5\r\n\r\n");
        assertFalse(p.expectContinue());
    }

    @Test
    void honoursConnectionClose() {
        parse("GET /feed HTTP/1.1\r\nConnection: close\r\n\r\n");
        assertFalse(p.keepAlive());
    }

    @Test
    void http10ClosesUnlessKeepAlive() {
        parse("GET /feed HTTP/1.0\r\n\r\n");
        assertTrue(p.http10());
        assertFalse(p.keepAlive());
        parse("GET /feed HTTP/1.0\r\nConnection: Keep-Alive\r\n\r\n");
        assertTrue(p.keepAlive());
    }

    @Test
    void acceptsBareLineFeeds() {
        String req = "GET /feed HTTP/1.1\nHost: x\n\n";
        assertEquals(req.length(), parse(req));
        assertEquals("/feed", path());
    }

    @Test
    void otherMethodsAreOther() {
        parse("DELETE /posts/1 HTTP/1.1\r\n\r\n");
        assertEquals(HttpParser.OTHER, p.method());
    }

    @Test
    void rejectsMalformedRequests() {
        assertEquals(-400, parse("GARBAGE\r\n\r\n"));
        assertEquals(-400, parse("GET feed HTTP/1.1\r\n\r\n"));
        assertEquals(-400, parse("GET /feed HTTP/2.0\r\n\r\n"));
        assertEquals(-400, parse("POST /posts HTTP/1.1\r\nContent-Length: abc\r\n\r\n"));
        assertEquals(-400, parse("POST /posts HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n"));
        assertEquals(-400, parse("GET /feed HTTP/1.1\r\nNoColonHere\r\n\r\n"));
    }

    @Test
    void headerNamesMustBeTokensThatMatchExactly() {
        assertEquals(-400, parse("POST /posts HTTP/1.1\r\nContent\rLength: 5\r\n\r\nhello"));
        assertEquals(-400, parse("POST /posts HTTP/1.1\r\nContent-Length : 5\r\n\r\nhello"));
        assertEquals(-400, parse("GET /feed HTTP/1.1\r\nX Y: z\r\n\r\n"));
        // Only letters fold: "Content-Length" with '-' replaced by '\r' (0x0D | 0x20 == '-') isn't Content-Length.
        String req = "GET /feed HTTP/1.1\r\nCONTENT-length: 0\r\n\r\n";
        assertEquals(req.length(), parse(req));
    }

    @Test
    void duplicateContentLengthsMustAgree() {
        String same = "POST /posts HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 5\r\n\r\nhello";
        assertEquals(same.length(), parse(same));
        assertEquals(-400, parse("POST /posts HTTP/1.1\r\nContent-Length: 5\r\nContent-Length: 50\r\n\r\nhello"));
    }

    @Test
    void rejectsOversizedBodiesAndHeaders() {
        assertEquals(-413, parse("POST /posts HTTP/1.1\r\nContent-Length: 1000000\r\n\r\n"));
        assertEquals(-431, parse("GET /feed HTTP/1.1\r\nX: " + "a".repeat(20_000)));
    }
}
