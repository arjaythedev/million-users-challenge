package feed.net;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.io.InputStream;
import java.net.ServerSocket;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.ArrayList;
import java.util.List;
import java.util.function.BooleanSupplier;

import org.junit.jupiter.api.AfterEach;
import org.junit.jupiter.api.BeforeEach;
import org.junit.jupiter.api.Test;

/** What every Io backend must do. Subclasses say which backend. */
abstract class IoContract {

    abstract Io open() throws IOException;

    Io io;
    final List<Integer> accepted = new ArrayList<>();
    private final List<Integer> readable = new ArrayList<>();
    private final List<Integer> writable = new ArrayList<>();

    final Io.Handler recorder = new Io.Handler() {
        @Override
        public void accepted(int id) {
            accepted.add(id);
        }

        @Override
        public void readable(int id) {
            readable.add(id);
        }

        @Override
        public void writable(int id) {
            writable.add(id);
        }
    };

    @BeforeEach
    void setUp() throws IOException {
        io = open();
    }

    @AfterEach
    void tearDown() {
        io.close();
    }

    void pollUntil(BooleanSupplier done) throws IOException {
        long deadline = System.currentTimeMillis() + 5000;
        while (!done.getAsBoolean()) {
            assertTrue(System.currentTimeMillis() < deadline, "timed out");
            io.poll(50, recorder);
        }
    }

    /**
     * A listener may report a client only once it has sent something (TCP_DEFER_ACCEPT), so the client sends a byte;
     * it is read here, so the socket starts with nothing unread.
     */
    private int acceptAfterOneByte(Socket client) throws IOException {
        client.getOutputStream().write('!');
        pollUntil(() -> !accepted.isEmpty());
        int id = accepted.get(0);
        pollUntil(() -> readable.contains(id));
        assertEquals(1, io.read(id, new byte[1], 0, 1));
        readable.clear();
        return id;
    }

    private static String readExactly(InputStream in, int n) throws IOException {
        return new String(in.readNBytes(n), StandardCharsets.UTF_8);
    }

    @Test
    void acceptsReadsAndWrites() throws IOException {
        int port = io.listen("127.0.0.1", 0);
        assertTrue(port > 0);
        try (Socket client = new Socket("127.0.0.1", port)) {
            client.getOutputStream().write("hello".getBytes(StandardCharsets.UTF_8));
            pollUntil(() -> !accepted.isEmpty());
            int id = accepted.get(0);
            pollUntil(() -> readable.contains(id));
            byte[] buf = new byte[16];
            int n = io.read(id, buf, 3, 13);
            assertEquals("hello", new String(buf, 3, n, StandardCharsets.UTF_8));
            assertEquals(0, io.read(id, buf, 0, 16));

            byte[] reply = "world".getBytes(StandardCharsets.UTF_8);
            assertEquals(5, io.write(id, reply, 0, 5));
            assertEquals("world", readExactly(client.getInputStream(), 5));
        }
    }

    @Test
    void pollReturnsHowManySocketsItReported() throws IOException {
        int port = io.listen("127.0.0.1", 0);
        assertEquals(0, io.poll(10, recorder));
        try (Socket client = new Socket("127.0.0.1", port)) {
            int id = acceptAfterOneByte(client);
            client.getOutputStream().write('x');
            long deadline = System.currentTimeMillis() + 5000;
            int n = 0;
            while (n == 0) {
                assertTrue(System.currentTimeMillis() < deadline, "timed out");
                n = io.poll(50, recorder);
            }
            assertEquals(1, n);
            assertEquals(List.of(id), readable);
        }
    }

    @Test
    void reportsEndOfStream() throws IOException {
        int port = io.listen("127.0.0.1", 0);
        Socket client = new Socket("127.0.0.1", port);
        int id = acceptAfterOneByte(client);
        client.close();
        pollUntil(() -> readable.contains(id));
        assertEquals(-1, io.read(id, new byte[8], 0, 8));
        io.close(id);
    }

    @Test
    void closingASocketEndsTheClientsStream() throws IOException {
        int port = io.listen("127.0.0.1", 0);
        try (Socket client = new Socket("127.0.0.1", port)) {
            io.close(acceptAfterOneByte(client));
            assertEquals(-1, client.getInputStream().read());
        }
    }

    @Test
    void connectsOutWithoutBlocking() throws IOException {
        try (ServerSocket server = new ServerSocket(0)) {
            int id = io.connect("127.0.0.1", server.getLocalPort());
            pollUntil(() -> writable.contains(id));
            io.finishConnect(id);
            try (Socket peer = server.accept()) {
                peer.getOutputStream().write('x');
                pollUntil(() -> readable.contains(id));
                byte[] buf = new byte[4];
                assertEquals(1, io.read(id, buf, 0, 4));
                assertEquals('x', buf[0]);
                assertEquals(2, io.write(id, new byte[] {'o', 'k'}, 0, 2));
                assertEquals("ok", readExactly(peer.getInputStream(), 2));
            }
        }
    }

    @Test
    void connectFailsFastWhenNobodyListens() throws IOException {
        int port;
        try (ServerSocket s = new ServerSocket(0)) {
            port = s.getLocalPort();
        }
        int p = port;
        // Refused either at once or when the connect completes.
        org.junit.jupiter.api.Assertions.assertThrows(IOException.class, () -> {
            int id = io.connect("127.0.0.1", p);
            pollUntil(() -> writable.contains(id) || readable.contains(id));
            io.finishConnect(id);
        });
    }

    @Test
    void pausedReadsReportNoMoreData() throws IOException {
        int port = io.listen("127.0.0.1", 0);
        try (Socket client = new Socket("127.0.0.1", port)) {
            int id = acceptAfterOneByte(client);
            io.pauseReads(id);
            client.getOutputStream().write('x');
            readable.clear();
            io.poll(200, recorder);
            assertTrue(readable.isEmpty(), "paused socket reported readable");
            assertEquals(1, io.write(id, new byte[] {'y'}, 0, 1)); // writes still work
            assertEquals('y', client.getInputStream().read());
        }
    }

    @Test
    void closeAndWakeupAreSafeAfterClose() throws IOException {
        Io other = open();
        other.close();
        other.wakeup();
        other.close();
    }

    @Test
    void reportsWritabilityWhileWatched() throws IOException {
        int port = io.listen("127.0.0.1", 0);
        try (Socket client = new Socket("127.0.0.1", port)) {
            int id = acceptAfterOneByte(client);
            io.watchWrites(id, true);
            pollUntil(() -> writable.contains(id));
            io.watchWrites(id, false);
            writable.clear();
            io.poll(100, recorder);
            assertTrue(writable.isEmpty());
        }
    }

    @Test
    void wakeupEndsAPoll() throws Exception {
        Thread waker = new Thread(() -> {
            try {
                Thread.sleep(100);
            } catch (InterruptedException e) {
                return;
            }
            io.wakeup();
        });
        long start = System.nanoTime();
        waker.start();
        io.poll(10_000, recorder);
        waker.join();
        assertTrue(System.nanoTime() - start < 5_000_000_000L);
    }

    @Test
    void twoListenersCanShareAPort() throws IOException {
        int port = io.listen("127.0.0.1", 0);
        try (Io other = open()) {
            assertEquals(port, other.listen("127.0.0.1", port));
        }
    }
}
