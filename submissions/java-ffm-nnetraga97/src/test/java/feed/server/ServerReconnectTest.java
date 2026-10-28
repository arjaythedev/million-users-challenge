package feed.server;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import java.io.IOException;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.util.List;

import org.junit.jupiter.api.Test;

import feed.Config;
import feed.pg.BlockingPg;
import feed.pg.PgHandler;
import feed.pg.PgRow;
import feed.pg.PgStatement;
import feed.pg.PgUrl;
import feed.testing.HttpClient;

/** Postgres drops every one of the server's connections; the server answers throughout and reconnects by itself. */
class ServerReconnectTest {

    @Test
    void recoversWhenPostgresDropsItsConnections() throws Exception {
        String url = System.getenv("PG_TEST_SERVER_URL");
        assumeTrue(url != null, "PG_TEST_SERVER_URL not set: run test-pg.sh");
        Config config = new Config(PgUrl.parse(url),
                "million-users-challenge".getBytes(StandardCharsets.UTF_8), "127.0.0.1", 0, -1, 2, 4, 4, 64, 1, 512);
        try (Server server = Server.start(config)) {
            awaitStatus(server, "/posts/1", 200);

            assertTrue(terminateServerBackends(url) >= 8, "expected to drop all 8 server connections");

            // Right after: each request gets an answer (200 from a reconnected backend, or 500), none hangs.
            for (int i = 0; i < 20; i++) {
                try (Socket s = connect(server)) {
                    int status = HttpClient.roundTrip(s, HttpClient.get("/posts/1")).status();
                    assertTrue(status == 200 || status == 500, "status " + status);
                }
            }
            // And it comes back: health, then the golden post.
            awaitStatus(server, "/health", 200);
            try (Socket s = connect(server)) {
                assertEquals(HttpClient.golden("post-1.json"),
                        HttpClient.roundTrip(s, HttpClient.get("/posts/1")).body());
            }
        }
    }

    private static Socket connect(Server server) throws IOException {
        Socket s = new Socket("127.0.0.1", server.port());
        s.setSoTimeout(10_000);
        return s;
    }

    private static void awaitStatus(Server server, String path, int status) throws Exception {
        long deadline = System.currentTimeMillis() + 15_000;
        while (true) {
            try (Socket s = connect(server)) {
                if (HttpClient.roundTrip(s, HttpClient.get(path)).status() == status) {
                    return;
                }
            }
            assertTrue(System.currentTimeMillis() < deadline, path + " never returned " + status);
            Thread.sleep(50);
        }
    }

    /** Terminates the server's backends in this database (application_name 'feed'), not this one; returns how many. */
    private static int terminateServerBackends(String url) throws IOException {
        String sql = "SELECT count(*) FILTER (WHERE pg_terminate_backend(pid))::int FROM pg_stat_activity"
                + " WHERE application_name = 'feed' AND pid <> pg_backend_pid() AND datname = current_database()";
        int[] killed = new int[1];
        try (BlockingPg admin = BlockingPg.connect(PgUrl.parse(url), List.of(new PgStatement("kill", sql)),
                Integer.MAX_VALUE)) {
            admin.pg.bind(0);
            admin.pg.execute(new PgHandler() {
                @Override
                public void onRow(long tag, PgRow row) {
                    killed[0] = row.int4(0);
                }

                @Override
                public void onDone(long tag) {
                }
            }, 0);
            admin.drain();
        }
        return killed[0];
    }
}
