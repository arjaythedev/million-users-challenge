package feed.server;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.ByteArrayOutputStream;
import java.io.DataInputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.net.InetAddress;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.atomic.AtomicInteger;

import org.junit.jupiter.api.Test;

import feed.Config;
import feed.pg.PgUrl;
import feed.testing.HttpClient;
import feed.testing.Tokens;

/**
 * The Postgres pipeline-depth cap and group commit, against a fake Postgres that answers only when told to: past the
 * cap, requests wait in the worker (not on the Postgres socket) and go out, oldest first, as earlier ones are answered;
 * writes that wait for a commit go out together as the next one. One worker, so a probe round trip (see
 * {@link #settle}) proves the worker has handled everything sent before it. No real database.
 */
class ServerPipelineTest {

    private static final byte[] SECRET = "million-users-challenge".getBytes(StandardCharsets.UTF_8);

    private static Config config(FakePostgres pg, int depth) {
        return new Config(PgUrl.parse("postgres://u@127.0.0.1:" + pg.port() + "/db"), SECRET, "127.0.0.1", 0,
                -1, 1, 1, 1, depth, 0, 512, 32, 2000);
    }

    @Test
    void requestsPastTheCapWaitAndAreAllAnswered() throws Exception {
        try (FakePostgres pg = new FakePostgres(); Server server = Server.start(config(pg, 2))) {
            pg.awaitReady(2);
            List<Socket> clients = new ArrayList<>();
            try {
                for (int i = 0; i < 5; i++) {
                    clients.add(send(server, HttpClient.get("/feed")));
                }
                settle(server);
                assertEquals(2, pg.requests(), "requests on the Postgres connection before any answer");

                pg.answer(2);
                settle(server);
                assertEquals(4, pg.requests());

                pg.answer(2);
                settle(server);
                assertEquals(5, pg.requests());
                pg.answer(1);
                for (Socket s : clients) {
                    HttpClient.Response r = HttpClient.read(s.getInputStream());
                    assertEquals(200, r.status());
                    assertEquals("{\"posts\":[]}", r.body());
                }
            } finally {
                for (Socket s : clients) {
                    s.close();
                }
            }
        }
    }

    @Test
    void aWaitingRequestGoesBeforeTheNextPipelinedOne() throws Exception {
        try (FakePostgres pg = new FakePostgres(); Server server = Server.start(config(pg, 1))) {
            pg.awaitReady(2);
            // A pipelines two requests; B's arrives while A's first is in flight, so B waits for room.
            try (Socket a = send(server, HttpClient.get("/posts/1") + HttpClient.get("/posts/2"))) {
                settle(server);
                try (Socket b = send(server, HttpClient.get("/feed"))) {
                    settle(server);
                    pg.answer(1); // A's first: room again. B was waiting before A's second was read.
                    settle(server);
                    assertEquals(List.of("post", "feed"), pg.statements());
                    pg.answer(1);
                    settle(server);
                    pg.answer(1);
                    assertEquals(404, HttpClient.read(a.getInputStream()).status());
                    assertEquals(200, HttpClient.read(b.getInputStream()).status());
                    assertEquals(404, HttpClient.read(a.getInputStream()).status());
                    assertEquals(List.of("post", "feed", "post"), pg.statements());
                }
            }
        }
    }

    @Test
    void aWaitingRequestIsNotAuthenticatedAgain() throws Exception {
        try (FakePostgres pg = new FakePostgres(); Server server = Server.start(config(pg, 1))) {
            pg.awaitReady(2);
            long exp = System.currentTimeMillis() / 1000 + 2;
            String token = Tokens.sign("{\"alg\":\"HS256\",\"typ\":\"JWT\"}",
                    "{\"sub\":\"7\",\"username\":\"u\",\"iat\":" + (exp - 60) + ",\"exp\":" + exp + "}");
            String like = "POST /posts/5/like HTTP/1.1\r\nAuthorization: Bearer " + token + "\r\n\r\n";
            Socket first = send(server, like);
            settle(server); // in flight first: a like read in the same pass would share its transaction instead
            try (first; Socket second = send(server, like)) {
                settle(server);
                assertEquals(List.of(1), pg.bindsPerSync()); // the second waits for the first's commit
                while (System.currentTimeMillis() / 1000 <= exp) {
                    Thread.sleep(50); // the token has now expired
                }
                pg.answer(1, "INSERT 0 1");
                settle(server);
                pg.answer(1, "INSERT 0 1");
                assertEquals(201, HttpClient.read(first.getInputStream()).status());
                assertEquals(201, HttpClient.read(second.getInputStream()).status(),
                        "valid when it arrived: waiting must not turn it into a 401");
            }
        }
    }

    @Test
    void aQueuedClientThatDisconnectsDoesNotHoldUpOthers() throws Exception {
        try (FakePostgres pg = new FakePostgres(); Server server = Server.start(config(pg, 1))) {
            pg.awaitReady(2);
            try (Socket first = send(server, HttpClient.get("/feed"))) {
                Socket second = send(server, HttpClient.get("/posts/2"));
                settle(server);
                assertEquals(1, pg.requests()); // the second waits
                second.close(); // read as a half-close: its query still runs, its answer has nowhere to go
                settle(server);
                pg.answer(1);
                assertEquals(200, HttpClient.read(first.getInputStream()).status());
                settle(server);
                assertEquals(List.of("feed", "post"), pg.statements());
                pg.answer(1);

                try (Socket third = send(server, HttpClient.get("/feed"))) {
                    settle(server);
                    assertEquals(List.of("feed", "post", "feed"), pg.statements());
                    pg.answer(1);
                    assertEquals(200, HttpClient.read(third.getInputStream()).status());
                }
            }
        }
    }

    @Test
    void aPostgresThatNeverFinishesStartupIsDroppedAndRetried() throws Exception {
        try (FakePostgres pg = new FakePostgres(true); Server server = Server.start(config(pg, 64))) {
            long deadline = System.currentTimeMillis() + 15_000;
            while (pg.accepted() < 3) { // the first two, then at least one retry after the 5 s startup timeout
                assertTrue(System.currentTimeMillis() < deadline, "no reconnect after a silent startup");
                Thread.sleep(100);
            }
        }
    }

    @Test
    void writesThatWaitForACommitShareTheNextOne() throws Exception {
        try (FakePostgres pg = new FakePostgres(); Server server = Server.start(config(pg, 64))) {
            pg.awaitReady(2);
            try (Socket a = send(server, like(5))) {
                settle(server);
                Socket b = send(server, like(6));
                settle(server); // b queues before c
                try (b; Socket c = send(server, like(7))) {
                    settle(server);
                    assertEquals(List.of(1), pg.bindsPerSync(), "b and c wait while a's commit is in flight");
                    pg.answer(1, "INSERT 0 1");
                    settle(server);
                    assertEquals(List.of(1, 2), pg.bindsPerSync(), "b and c share one transaction");
                    pg.reply(complete("INSERT 0 1"), complete("INSERT 0 0"), ready());
                    assertEquals(201, HttpClient.read(a.getInputStream()).status());
                    assertEquals(201, HttpClient.read(b.getInputStream()).status());
                    assertEquals(200, HttpClient.read(c.getInputStream()).status());
                }
            }
        }
    }

    @Test
    void aGroupThatFailsIsRetriedOneWriteAtATime() throws Exception {
        try (FakePostgres pg = new FakePostgres(); Server server = Server.start(config(pg, 64))) {
            pg.awaitReady(2);
            try (Socket a = send(server, like(5))) {
                settle(server);
                Socket b = send(server, like(6));
                settle(server); // b queues before c
                try (b; Socket c = send(server, like(999_999))) {
                    settle(server);
                    pg.answer(1, "INSERT 0 1");
                    settle(server);
                    assertEquals(List.of(1, 2), pg.bindsPerSync());
                    pg.reply(complete("INSERT 0 1"), error("23503"), ready()); // c's post doesn't exist: all undone
                    settle(server);
                    assertEquals(List.of(1, 2, 1, 1), pg.bindsPerSync(), "b and c again, each on its own");
                    pg.reply(complete("INSERT 0 1"), ready(), error("23503"), ready());
                    assertEquals(201, HttpClient.read(a.getInputStream()).status());
                    assertEquals(201, HttpClient.read(b.getInputStream()).status());
                    assertEquals(404, HttpClient.read(c.getInputStream()).status());
                }
            }
        }
    }

    /** A like by user 7, with a token that stays valid through the test. */
    private static String like(long postId) {
        long exp = System.currentTimeMillis() / 1000 + 3600;
        String token = Tokens.sign("{\"alg\":\"HS256\",\"typ\":\"JWT\"}",
                "{\"sub\":\"7\",\"username\":\"u\",\"iat\":" + (exp - 60) + ",\"exp\":" + exp + "}");
        return "POST /posts/" + postId + "/like HTTP/1.1\r\nAuthorization: Bearer " + token + "\r\n\r\n";
    }

    private static byte[] complete(String commandTag) {
        return FakePostgres.message('C', (commandTag + "\0").getBytes(StandardCharsets.US_ASCII));
    }

    private static byte[] error(String sqlState) {
        return FakePostgres.message('E', ("SERROR\0C" + sqlState + "\0Mfake\0\0").getBytes(StandardCharsets.US_ASCII));
    }

    private static byte[] ready() {
        return FakePostgres.message('Z', new byte[] {'I'});
    }

    private static Socket send(Server server, String request) throws IOException {
        Socket s = new Socket("127.0.0.1", server.port());
        s.setSoTimeout(10_000);
        s.getOutputStream().write(request.getBytes(StandardCharsets.UTF_8));
        return s;
    }

    /**
     * Returns once the worker has handled everything already sent to it: two round trips on a new connection (no
     * database involved). The second needs a later event-loop pass than the one that read the first.
     */
    private static void settle(Server server) throws IOException {
        try (Socket probe = new Socket("127.0.0.1", server.port())) {
            probe.setSoTimeout(10_000);
            for (int i = 0; i < 2; i++) {
                assertEquals(404, HttpClient.roundTrip(probe, HttpClient.get("/nope")).status());
            }
        }
    }

    /**
     * Speaks just enough of the backend protocol: trust login, a ReadyForQuery for the statement-preparing Sync, then
     * records each request's statement name and answers it only when {@link #answer} says so.
     */
    private static final class FakePostgres implements AutoCloseable {

        private final AtomicInteger ready = new AtomicInteger();
        private final AtomicInteger accepted = new AtomicInteger();
        private final boolean silent;
        private final List<String> statements = new ArrayList<>();
        private final List<Integer> bindsPerSync = new ArrayList<>();
        private final ServerSocket listener;
        private final List<Socket> connections = new ArrayList<>();
        private volatile OutputStream busy; // the connection the requests arrive on

        FakePostgres() throws IOException {
            this(false);
        }

        /** {@code silent}: accept connections but never answer their startup. */
        FakePostgres(boolean silent) throws IOException {
            this.silent = silent;
            listener = new ServerSocket(0, 50, InetAddress.getLoopbackAddress());
            Thread.ofPlatform().daemon().start(() -> {
                try {
                    while (true) {
                        Socket s = listener.accept();
                        s.setTcpNoDelay(true);
                        accepted.incrementAndGet();
                        synchronized (connections) {
                            connections.add(s);
                        }
                        Thread.ofPlatform().daemon().start(() -> serve(s));
                    }
                } catch (IOException closed) {
                    // done
                }
            });
        }

        int accepted() {
            return accepted.get();
        }

        int port() {
            return listener.getLocalPort();
        }

        int requests() throws InterruptedException {
            return statements().size();
        }

        /** Statement names of the requests received so far (waits briefly for bytes still on the wire). */
        List<String> statements() throws InterruptedException {
            Thread.sleep(20);
            synchronized (statements) {
                return List.copyOf(statements);
            }
        }

        void awaitReady(int n) throws InterruptedException {
            long deadline = System.currentTimeMillis() + 5_000;
            while (ready.get() < n) {
                assertTrue(System.currentTimeMillis() < deadline, "the server never finished connecting");
                Thread.sleep(10);
            }
        }

        /** How many Binds came before each Sync, once the statements were prepared. */
        List<Integer> bindsPerSync() throws InterruptedException {
            Thread.sleep(20);
            synchronized (statements) {
                return List.copyOf(bindsPerSync);
            }
        }

        /** Sends these backend messages in one write. */
        void reply(byte[]... messages) throws IOException {
            ByteArrayOutputStream bytes = new ByteArrayOutputStream();
            for (byte[] m : messages) {
                bytes.writeBytes(m);
            }
            busy.write(bytes.toByteArray());
        }

        void answer(int n) throws IOException {
            answer(n, "SELECT 0");
        }

        void answer(int n, String commandTag) throws IOException {
            // One write, sent at once (TCP_NODELAY): a reply split across segments would arrive after settle().
            ByteArrayOutputStream replies = new ByteArrayOutputStream();
            for (int i = 0; i < n; i++) {
                replies.writeBytes(message('C', (commandTag + "\0").getBytes(StandardCharsets.US_ASCII)));
                replies.writeBytes(message('Z', new byte[] {'I'}));
            }
            busy.write(replies.toByteArray());
        }

        private void serve(Socket s) {
            try {
                DataInputStream in = new DataInputStream(s.getInputStream());
                OutputStream out = s.getOutputStream();
                in.readFully(new byte[in.readInt() - 4]); // StartupMessage
                if (silent) {
                    in.read(); // until the server gives up and closes
                    return;
                }
                out.write(message('R', new byte[4])); // AuthenticationOk
                out.write(message('Z', new byte[] {'I'}));
                boolean prepared = false;
                String statement = null;
                int binds = 0;
                while (true) {
                    int type = in.read();
                    if (type < 0) {
                        return;
                    }
                    byte[] body = new byte[in.readInt() - 4];
                    in.readFully(body);
                    if (type == 'B') {
                        binds++;
                        int portalEnd = indexOf(body, 0);
                        statement = new String(body, portalEnd + 1, indexOf(body, portalEnd + 1) - portalEnd - 1,
                                StandardCharsets.UTF_8);
                    } else if (type == 'S' && !prepared) {
                        prepared = true;
                        out.write(message('Z', new byte[] {'I'}));
                        ready.incrementAndGet();
                    } else if (type == 'S') {
                        busy = out;
                        synchronized (statements) {
                            statements.add(statement);
                            bindsPerSync.add(binds);
                        }
                        binds = 0;
                    }
                }
            } catch (IOException closed) {
                // done
            }
        }

        private static int indexOf(byte[] b, int from) {
            for (int i = from; i < b.length; i++) {
                if (b[i] == 0) {
                    return i;
                }
            }
            return b.length;
        }

        static byte[] message(char type, byte[] body) {
            return ByteBuffer.allocate(5 + body.length).put((byte) type).putInt(4 + body.length).put(body).array();
        }

        @Override
        public void close() throws IOException {
            listener.close();
            synchronized (connections) {
                for (Socket s : connections) {
                    s.close();
                }
            }
        }
    }
}
