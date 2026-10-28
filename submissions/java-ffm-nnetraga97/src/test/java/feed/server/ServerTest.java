package feed.server;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assumptions.assumeTrue;
import static feed.testing.HttpClient.get;
import static feed.testing.HttpClient.golden;
import static feed.testing.HttpClient.read;
import static feed.testing.HttpClient.roundTrip;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.io.OutputStream;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.zip.GZIPInputStream;

import org.junit.jupiter.api.AfterAll;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.Test;

import feed.Config;
import feed.testing.HttpClient;
import feed.testing.HttpClient.Response;
import feed.testing.Tokens;
import feed.pg.PgUrl;

/**
 * The server over real sockets: transport behaviour test/run.sh doesn't reach (pipelining, split requests,
 * Expect: 100-continue, Connection: close, HTTP/1.0, vanishing clients). Needs PG_TEST_SERVER_URL (test-pg.sh).
 */
class ServerTest {

    private static Server server;
    private static String token;

    @BeforeAll
    static void start() throws Exception {
        String url = System.getenv("PG_TEST_SERVER_URL");
        assumeTrue(url != null, "PG_TEST_SERVER_URL not set: run test-pg.sh");
        server = Server.start(new Config(PgUrl.parse(url),
                "million-users-challenge".getBytes(StandardCharsets.UTF_8), "127.0.0.1", 0, 0, 2, 4, 4, 64, 1, 512));
        // Like test/run.sh: wait until /health says the database is reachable.
        long deadline = System.currentTimeMillis() + 10_000;
        while (true) {
            try (Socket s = connect()) {
                if (roundTrip(s, get("/health")).status() == 200) {
                    break;
                }
            }
            if (System.currentTimeMillis() > deadline) {
                throw new AssertionError("server never became healthy");
            }
            Thread.sleep(50);
        }
        long now = System.currentTimeMillis() / 1000;
        token = Tokens.token("1", "golden_ember_1", now + 3600);
    }

    @AfterAll
    static void stop() {
        if (server != null) {
            server.close();
        }
    }

    static String post(String path, String body) {
        return "POST " + path + " HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer " + token
                + "\r\nContent-Type: application/json\r\nContent-Length: "
                + body.getBytes(StandardCharsets.UTF_8).length + "\r\n\r\n" + body;
    }

    static Socket connect() throws IOException {
        return HttpClient.connect(server.port());
    }

    @Test
    void healthSaysOk() throws IOException {
        try (Socket s = connect()) {
            Response r = roundTrip(s, get("/health"));
            assertEquals(200, r.status());
            assertTrue(r.body().matches("\\{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":\\d+}"), r.body());
            assertTrue(r.head().contains("Content-Type: application/json"));
        }
    }

    @Test
    void servesAGoldenPost() throws IOException {
        try (Socket s = connect()) {
            assertEquals(golden("post-1.json"), roundTrip(s, get("/posts/1")).body());
        }
    }

    @Test
    void theExtraPortServesToo() throws IOException {
        try (Socket s = new Socket("127.0.0.1", server.extraPort())) {
            assertEquals(golden("post-250000.json"), roundTrip(s, get("/posts/250000")).body());
        }
    }

    @Test
    void theFeedIsGzippedWhenTheClientAcceptsIt() throws IOException {
        try (Socket s = connect()) {
            Response zipped = roundTrip(s, "GET /feed HTTP/1.1\r\nHost: t\r\nAccept-Encoding: gzip, deflate\r\n\r\n");
            Response plain = roundTrip(s, get("/feed"));
            assertEquals(200, zipped.status());
            assertTrue(zipped.head().contains("\r\nContent-Encoding: gzip\r\n"), zipped.head());
            assertTrue(zipped.head().contains("Content-Type: application/json"), zipped.head());
            assertFalse(plain.head().contains("Content-Encoding"), plain.head());
            assertTrue(zipped.bytes().length < plain.bytes().length / 2);
            try (GZIPInputStream z = new GZIPInputStream(new ByteArrayInputStream(zipped.bytes()))) {
                assertEquals(plain.body(), new String(z.readAllBytes(), StandardCharsets.UTF_8));
            }
        }
    }

    @Test
    void aSmallBodyIsNotGzipped() throws IOException {
        try (Socket s = connect()) {
            Response r = roundTrip(s, "GET /posts/1 HTTP/1.1\r\nHost: t\r\nAccept-Encoding: gzip\r\n\r\n");
            assertFalse(r.head().contains("Content-Encoding"), r.head());
            assertEquals(golden("post-1.json"), r.body());
        }
    }

    @Test
    void pipelinedRequestsAreAnsweredInOrder() throws IOException {
        try (Socket s = connect()) {
            s.getOutputStream().write((get("/posts/1") + get("/nope") + get("/posts/500000") + get("/posts/0"))
                    .getBytes(StandardCharsets.UTF_8));
            InputStream in = s.getInputStream();
            assertEquals(golden("post-1.json"), read(in).body());
            assertEquals("{\"error\":\"not found\"}", read(in).body());
            assertEquals(golden("post-500000.json"), read(in).body());
            Response last = read(in);
            assertEquals(400, last.status());
            assertEquals("{\"error\":\"invalid post id\"}", last.body());
        }
    }

    @Test
    void aRequestArrivingByteByByteWorks() throws Exception {
        try (Socket s = connect()) {
            s.setTcpNoDelay(true);
            OutputStream out = s.getOutputStream();
            for (byte b : get("/posts/1").getBytes(StandardCharsets.UTF_8)) {
                out.write(b);
                out.flush();
                Thread.sleep(1);
            }
            assertEquals(golden("post-1.json"), read(s.getInputStream()).body());
        }
    }

    @Test
    void keepsAConnectionOpenForManyRequests() throws IOException {
        try (Socket s = connect()) {
            for (int i = 1; i <= 200; i++) {
                Response r = roundTrip(s, get("/posts/" + i));
                assertEquals(200, r.status());
                assertTrue(r.body().startsWith("{\"post\":{\"id\":" + i + ","), r.body());
            }
        }
    }

    @Test
    void answersExpectContinue() throws IOException {
        try (Socket s = connect()) {
            String body = "{\"body\":\"expect test\"}";
            s.getOutputStream().write(("POST /posts HTTP/1.1\r\nHost: t\r\nAuthorization: Bearer " + token
                    + "\r\nExpect: 100-continue\r\nContent-Length: " + body.length() + "\r\n\r\n")
                    .getBytes(StandardCharsets.UTF_8));
            InputStream in = s.getInputStream();
            byte[] interim = in.readNBytes("HTTP/1.1 100 Continue\r\n\r\n".length());
            assertEquals("HTTP/1.1 100 Continue\r\n\r\n", new String(interim, StandardCharsets.US_ASCII));
            s.getOutputStream().write(body.getBytes(StandardCharsets.UTF_8));
            Response r = read(in);
            assertEquals(201, r.status());
            assertTrue(r.body().contains("\"body\":\"expect test\""), r.body());
        }
    }

    @Test
    void connectionCloseClosesAfterTheResponse() throws IOException {
        try (Socket s = connect()) {
            Response r = roundTrip(s, "GET /posts/1 HTTP/1.1\r\nConnection: close\r\n\r\n");
            assertTrue(r.head().contains("Connection: close"));
            assertEquals(-1, s.getInputStream().read());
        }
    }

    @Test
    void http10ClosesUnlessKeepAlive() throws IOException {
        try (Socket s = connect()) {
            Response r = roundTrip(s, "GET /posts/1 HTTP/1.0\r\n\r\n");
            assertEquals(200, r.status());
            assertEquals(-1, s.getInputStream().read());
        }
        try (Socket s = connect()) {
            Response r = roundTrip(s, "GET /posts/1 HTTP/1.0\r\nConnection: keep-alive\r\n\r\n");
            assertTrue(r.head().contains("Connection: keep-alive"));
            assertEquals(200, roundTrip(s, "GET /posts/2 HTTP/1.0\r\nConnection: keep-alive\r\n\r\n").status());
        }
    }

    @Test
    void aMalformedRequestGets400AndIsClosed() throws IOException {
        try (Socket s = connect()) {
            Response r = roundTrip(s, "NONSENSE\r\n\r\n");
            assertEquals(400, r.status());
            assertEquals(-1, s.getInputStream().read());
        }
    }

    @Test
    void aWriteIsVisibleToTheNextRead() throws IOException {
        try (Socket s = connect()) {
            Response created = roundTrip(s, post("/posts", "{\"body\":\"  ordering test  \"}"));
            assertEquals(201, created.status());
            String id = created.body().replaceAll(".*\"id\":(\\d+).*", "$1");
            assertEquals(201, roundTrip(s, post("/posts/" + id + "/like", "")).status());
            Response again = roundTrip(s, post("/posts/" + id + "/like", ""));
            assertEquals(200, again.status());
            assertEquals("{\"liked\":true,\"already_liked\":true,\"post_id\":" + id + "}", again.body());
            Response read = roundTrip(s, get("/posts/" + id));
            assertTrue(read.body().contains("\"body\":\"ordering test\""), read.body());
            assertTrue(read.body().endsWith("\"like_count\":1}}"), read.body());
        }
    }

    @Test
    void aHalfClosedClientStillGetsItsAnswer() throws IOException {
        try (Socket s = connect()) {
            s.getOutputStream().write(get("/posts/1").getBytes(StandardCharsets.UTF_8));
            s.shutdownOutput(); // "that's all I'm sending", while the query is in flight
            assertEquals(golden("post-1.json"), read(s.getInputStream()).body());
            assertEquals(-1, s.getInputStream().read());
        }
    }

    @Test
    void aBodyThatIsNotUtf8IsMalformedJson() throws IOException {
        try (Socket s = connect()) {
            byte[] body = {'{', '"', 'b', 'o', 'd', 'y', '"', ':', '"', (byte) 0xF0, 'a', '"', '}'};
            String head = "POST /posts HTTP/1.1\r\nAuthorization: Bearer " + token + "\r\nContent-Length: "
                    + body.length + "\r\n\r\n";
            s.getOutputStream().write(head.getBytes(StandardCharsets.UTF_8));
            s.getOutputStream().write(body);
            Response r = read(s.getInputStream());
            assertEquals(400, r.status());
            assertEquals("{\"error\":\"malformed JSON body\"}", r.body());
            assertEquals(200, roundTrip(s, get("/posts/1")).status()); // and the connection lives on
        }
    }

    /** Responses pile up behind a client that isn't reading: they queue, go out in order, then the close. */
    @Test
    void aSlowReaderGetsEveryResponseInOrder() throws Exception {
        try (Socket s = new Socket()) {
            s.setReceiveBufferSize(4096);
            s.connect(new java.net.InetSocketAddress("127.0.0.1", server.port()));
            s.setSoTimeout(20_000);
            StringBuilder requests = new StringBuilder();
            int n = 300; // ~6 KB of feed each: far more than the socket buffers hold
            for (int i = 0; i < n - 1; i++) {
                requests.append(get("/feed"));
            }
            requests.append("GET /posts/1 HTTP/1.1\r\nConnection: close\r\n\r\n");
            s.getOutputStream().write(requests.toString().getBytes(StandardCharsets.UTF_8));
            Thread.sleep(500); // let the server hit a full socket and queue the rest
            InputStream in = s.getInputStream();
            for (int i = 0; i < n - 1; i++) {
                Response r = read(in);
                assertEquals(200, r.status(), "response " + i);
                assertEquals(20, r.body().split("\"id\":").length - 1, "response " + i);
            }
            Response last = read(in);
            assertEquals(golden("post-1.json"), last.body());
            assertTrue(last.head().contains("Connection: close"));
            assertEquals(-1, in.read());
        }
    }

    /** More than 64 KB of requests waiting behind one in flight is more than any client of this API sends. */
    @Test
    void tooMuchUnansweredInputClosesTheConnection() throws IOException {
        try (Socket s = connect()) {
            String request = get("/posts/1");
            int n = 100 * 1024 / request.length();
            s.getOutputStream().write(request.repeat(n).getBytes(StandardCharsets.UTF_8));
            InputStream in = s.getInputStream();
            int answered = 0;
            try {
                while (true) {
                    read(in);
                    answered++;
                }
            } catch (IOException closed) {
                // the server closed the connection
            }
            assertTrue(answered < n, "answered all " + n + " requests instead of closing");
        }
    }

    @Test
    void aClientThatVanishesMidRequestHurtsNobody() throws IOException {
        for (int i = 0; i < 50; i++) {
            Socket s = connect();
            s.getOutputStream().write(get("/feed").getBytes(StandardCharsets.UTF_8));
            s.close();
        }
        try (Socket s = connect()) {
            assertEquals(200, roundTrip(s, get("/health")).status());
            assertEquals(20, roundTrip(s, get("/feed")).body().split("\"id\":").length - 1);
        }
    }
}
