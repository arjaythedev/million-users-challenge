package feed.pg;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.time.Duration;
import java.time.Instant;
import java.util.ArrayList;
import java.util.List;
import java.util.regex.Matcher;
import java.util.regex.Pattern;

import org.junit.jupiter.api.AfterAll;
import org.junit.jupiter.api.BeforeAll;
import org.junit.jupiter.api.MethodOrderer;
import org.junit.jupiter.api.Order;
import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.TestMethodOrder;

import feed.db.Queries;

/**
 * The driver against a real Postgres, on a fresh copy of feed_seed. Run through {@code test-pg.sh}, which makes the
 * copy and sets PG_TEST_URL; skipped otherwise. The golden reads run first, before any test writes a post.
 */
@TestMethodOrder(MethodOrderer.OrderAnnotation.class)
class PgIntegrationTest {

    private static final Path GOLDEN = Path.of("../../test/golden");
    private static final Pattern POST = Pattern.compile(
            "\\{\"id\":(\\d+),\"body\":\"([^\"\\\\]*)\",\"created_at\":\"([^\"]+)\",\"author\":\"([^\"]+)\",\"like_count\":(\\d+)}");

    private static PgUrl url;
    private static BlockingPg conn;

    record Post(long id, String body, String createdAt, String author, int likeCount) {
    }

    /** Collects post-object rows (id, body, created_at, author, like_count). */
    static final class Posts implements PgHandler {
        final List<Post> rows = new ArrayList<>();
        final List<Long> done = new ArrayList<>();

        @Override
        public void onRow(long tag, PgRow row) {
            byte[] created = new byte[PgTime.LENGTH];
            PgTime.format(row.int8(2), created, 0);
            rows.add(new Post(row.int8(0), row.text(1), new String(created, StandardCharsets.US_ASCII), row.text(3),
                    row.int4(4)));
        }

        @Override
        public void onDone(long tag) {
            done.add(tag);
        }
    }

    @BeforeAll
    static void connect() throws IOException {
        String env = System.getenv("PG_TEST_URL");
        assumeTrue(env != null, "PG_TEST_URL not set: run test-pg.sh");
        url = PgUrl.parse(env);
        conn = BlockingPg.connect(url, Queries.ALL, Integer.MAX_VALUE);
        assertTrue(conn.pg.isReady(), conn.pg.failure());
    }

    @AfterAll
    static void close() throws IOException {
        if (conn != null) {
            conn.close();
        }
    }

    private static List<Post> golden(String file) throws IOException {
        Matcher m = POST.matcher(Files.readString(GOLDEN.resolve(file)));
        List<Post> posts = new ArrayList<>();
        while (m.find()) {
            posts.add(new Post(Long.parseLong(m.group(1)), m.group(2), m.group(3), m.group(4),
                    Integer.parseInt(m.group(5))));
        }
        return posts;
    }

    private static List<Post> feed(BlockingPg c) throws IOException {
        Posts posts = new Posts();
        c.pg.bind(Queries.FEED);
        c.pg.execute(posts, 1);
        c.drain();
        return posts.rows;
    }

    private static List<Post> post(long id) throws IOException {
        Posts posts = new Posts();
        conn.pg.bind(Queries.POST);
        conn.pg.paramInt8(id);
        conn.pg.execute(posts, id);
        conn.drain();
        return posts.rows;
    }

    private static long[] insertPost(long userId, String body) throws IOException {
        long[] result = new long[2];
        byte[] utf8 = body.getBytes(StandardCharsets.UTF_8);
        conn.pg.bind(Queries.INSERT_POST);
        conn.pg.paramInt8(userId);
        conn.pg.paramText(utf8, 0, utf8.length);
        conn.pg.execute(new PgHandler() {
            @Override
            public void onRow(long tag, PgRow row) {
                result[0] = row.int8(0);
                result[1] = row.int8(1);
            }

            @Override
            public void onDone(long tag) {
            }
        }, 0);
        conn.drain();
        return result;
    }

    private static void like(long userId, long postId, EventLog log, long tag) {
        conn.pg.bind(Queries.INSERT_LIKE);
        conn.pg.paramInt8(userId);
        conn.pg.paramInt8(postId);
        conn.pg.execute(log, tag);
    }

    // ---- reads, on the untouched seed ----

    @Test
    @Order(1)
    void feedMatchesTheGoldenFile() throws IOException {
        List<Post> expected = golden("feed.json");
        assertEquals(20, expected.size());
        assertEquals(expected, feed(conn));
    }

    @Test
    @Order(1)
    void postsMatchTheGoldenFiles() throws IOException {
        for (long id : new long[] {1, 250_000, 500_000}) {
            assertEquals(golden("post-" + id + ".json"), post(id), "post " + id);
        }
    }

    @Test
    @Order(1)
    void aMissingPostHasNoRows() throws IOException {
        assertEquals(List.of(), post(999_999_999));
    }

    @Test
    @Order(1)
    void pingReturnsOne() throws IOException {
        int[] value = new int[1];
        conn.pg.bind(Queries.PING);
        conn.pg.execute(new PgHandler() {
            @Override
            public void onRow(long tag, PgRow row) {
                value[0] = row.int4(0);
            }

            @Override
            public void onDone(long tag) {
            }
        }, 0);
        conn.drain();
        assertEquals(1, value[0]);
    }

    @Test
    @Order(1)
    void aThousandPipelinedReadsAnswerInOrder() throws IOException {
        Posts posts = new Posts();
        for (long id = 1; id <= 1000; id++) {
            conn.pg.bind(Queries.POST);
            conn.pg.paramInt8(id);
            conn.pg.execute(posts, id);
        }
        assertEquals(1000, conn.pg.pending());
        conn.drain();
        assertEquals(1000, posts.rows.size());
        for (int i = 0; i < 1000; i++) {
            assertEquals(i + 1, posts.rows.get(i).id());
            assertEquals(i + 1, posts.done.get(i));
        }
    }

    @Test
    @Order(1)
    void readsSurviveMessagesArrivingThreeBytesAtATime() throws IOException {
        try (BlockingPg slow = BlockingPg.connect(url, Queries.ALL, 3)) {
            assertTrue(slow.pg.isReady(), slow.pg.failure());
            assertEquals(golden("feed.json"), feed(slow));
        }
    }

    // ---- writes, on posts made here ----

    @Test
    @Order(2)
    void insertPostReturnsANewIdAndNow() throws IOException {
        long[] created = insertPost(1, "driver test: héllo");
        assertTrue(created[0] > 500_000, "id " + created[0]);
        byte[] text = new byte[PgTime.LENGTH];
        PgTime.format(created[1], text, 0);
        Instant at = Instant.parse(new String(text, StandardCharsets.US_ASCII));
        assertTrue(Duration.between(at, Instant.now()).abs().toSeconds() < 60, "created_at " + at);

        List<Post> back = post(created[0]);
        assertEquals("driver test: héllo", back.get(0).body());
        assertEquals(0, back.get(0).likeCount());
    }

    @Test
    @Order(2)
    void aRepeatLikeInsertsNothing() throws IOException {
        long postId = insertPost(1, "like me")[0];
        EventLog log = new EventLog();
        like(2, postId, log, 1);
        like(2, postId, log, 2);
        conn.drain();
        assertEquals(List.of("complete 1 1", "done 1", "complete 2 0", "done 2"), log.events);
        assertEquals(1, post(postId).get(0).likeCount());
    }

    @Test
    @Order(2)
    void aForeignKeyErrorMidPipelineLeavesItsNeighboursCommitted() throws IOException {
        long postId = insertPost(1, "neighbours")[0];
        EventLog log = new EventLog();
        like(4, postId, log, 1);
        like(4, 999_999_999, log, 2);
        like(5, postId, log, 3);
        conn.drain();
        assertEquals("complete 1 1", log.events.get(0));
        assertEquals("done 1", log.events.get(1));
        assertTrue(log.events.get(2).startsWith("error 2 23503 "), log.events.get(2));
        assertEquals(List.of("done 2", "complete 3 1", "done 3"), log.events.subList(3, 6));
        assertEquals(2, post(postId).get(0).likeCount());
    }

    // ---- SCRAM, when test-pg.sh found a password-protected role ----

    @Test
    @Order(3)
    void scramLoginWorks() throws IOException {
        String scramUrl = System.getenv("PG_TEST_SCRAM_URL");
        assumeTrue(scramUrl != null, "PG_TEST_SCRAM_URL not set");
        try (BlockingPg c = BlockingPg.connect(PgUrl.parse(scramUrl), Queries.ALL, Integer.MAX_VALUE)) {
            assertTrue(c.pg.isReady(), c.pg.failure());
            assertEquals(20, feed(c).size());
        }
    }

    @Test
    @Order(3)
    void scramWithAWrongPasswordFails() throws IOException {
        String scramUrl = System.getenv("PG_TEST_SCRAM_URL");
        assumeTrue(scramUrl != null, "PG_TEST_SCRAM_URL not set");
        PgUrl good = PgUrl.parse(scramUrl);
        PgUrl bad = new PgUrl(good.host(), good.port(), good.database(), good.user(), good.password() + "x");
        try (BlockingPg c = BlockingPg.connect(bad, Queries.ALL, Integer.MAX_VALUE)) {
            assertFalse(c.pg.isReady());
            assertTrue(c.pg.failure().startsWith("28P01"), c.pg.failure());
        }
    }
}
