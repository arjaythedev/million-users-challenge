package feed.http;

import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;
import static org.junit.jupiter.api.Assumptions.assumeTrue;

import java.io.ByteArrayInputStream;
import java.io.IOException;
import java.lang.management.ManagementFactory;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import java.util.zip.GZIPInputStream;

import org.junit.jupiter.api.Test;

/** Both compressors: the output is a gzip stream any client can read, it never overflows, it allocates nothing. */
class GzipTest {

    /** A feed-shaped body: twenty posts of short generated text, like mload's. */
    private static final byte[] FEED = feed();

    private static byte[] feed() {
        StringBuilder s = new StringBuilder("{\"posts\":[");
        for (int i = 0; i < 20; i++) {
            s.append(i == 0 ? "" : ",").append("{\"id\":").append(500_000 + i)
                    .append(",\"body\":\"load test post ").append(i * 7919).append(" from user ").append(i * 31)
                    .append("\",\"created_at\":\"2026-01-01T00:0").append(i % 10).append(":00.000Z\",\"author\":\"")
                    .append("swift_falcon_").append(i * 13).append("\",\"like_count\":").append(i).append('}');
        }
        return s.append("]}").toString().getBytes(StandardCharsets.UTF_8);
    }

    private static Gzip jdk() {
        return new Gzip.Jdk(1);
    }

    private static Gzip libdeflate() {
        Gzip g = Gzip.LibDeflate.tryOpen(1);
        assumeTrue(g != null, "libdeflate not installed");
        return g;
    }

    @Test
    void jdkOutputIsReadableGzip() throws IOException {
        roundTrips(jdk());
    }

    @Test
    void libdeflateOutputIsReadableGzip() throws IOException {
        roundTrips(libdeflate());
    }

    @Test
    void jdkReturnsZeroWhenTheOutputDoesNotFit() {
        tooSmall(jdk());
    }

    @Test
    void libdeflateReturnsZeroWhenTheOutputDoesNotFit() {
        tooSmall(libdeflate());
    }

    @Test
    void jdkAllocatesNothing() {
        allocatesNothing(jdk());
    }

    @Test
    void libdeflateAllocatesNothing() {
        allocatesNothing(libdeflate());
    }

    @Test
    void levelsPastTheMaximumAreClampedAndReported() {
        try (Gzip g = new Gzip.Jdk(12)) {
            assertEquals(9, g.level());
        }
        try (Gzip g = Gzip.open(99)) {
            assertEquals(g.name().equals("libdeflate") ? 12 : 9, g.level());
            assertTrue(g.compress(FEED, 0, FEED.length, new byte[FEED.length], 0, FEED.length) > 0);
        }
    }

    @Test
    void openPrefersLibdeflateAndFallsBackToTheJdk() {
        try (Gzip g = Gzip.open(1)) {
            try (Gzip lib = Gzip.LibDeflate.tryOpen(1)) {
                assertEquals(lib == null ? "jdk" : "libdeflate", g.name());
            }
        }
    }

    private static void roundTrips(Gzip g) throws IOException {
        try (g) {
            byte[] in = new byte[7 + FEED.length];
            System.arraycopy(FEED, 0, in, 7, FEED.length); // not at offset 0, as in ResponseBuffer's buffers
            byte[] out = new byte[11 + FEED.length];
            for (int round = 0; round < 3; round++) { // the compressor is reused
                int n = g.compress(in, 7, FEED.length, out, 11, FEED.length);
                assertTrue(n > 0 && n < FEED.length / 2, "compressed to " + n + " of " + FEED.length);
                try (GZIPInputStream z = new GZIPInputStream(new ByteArrayInputStream(out, 11, n))) {
                    assertArrayEquals(FEED, z.readAllBytes());
                }
            }
        }
    }

    private static void tooSmall(Gzip g) {
        try (g) {
            byte[] out = new byte[FEED.length];
            assertEquals(0, g.compress(FEED, 0, FEED.length, out, 0, 40));
            // ...and it still works after that.
            assertTrue(g.compress(FEED, 0, FEED.length, out, 0, out.length) > 0);
        }
    }

    private static void allocatesNothing(Gzip g) {
        try (g) {
            // The response buffer swaps its two buffers after each compression: the arrays alternate, the offsets
            // stay the same.
            byte[][] in = {Arrays.copyOf(FEED, FEED.length), Arrays.copyOf(FEED, FEED.length)};
            byte[][] out = {new byte[32 + FEED.length], new byte[32 + FEED.length]};
            run(g, in, out, 20_000);
            com.sun.management.ThreadMXBean threads =
                    (com.sun.management.ThreadMXBean) ManagementFactory.getThreadMXBean();
            long thread = Thread.currentThread().threadId();
            long before = threads.getThreadAllocatedBytes(thread);
            run(g, in, out, 10_000);
            long allocated = threads.getThreadAllocatedBytes(thread) - before;
            assertTrue(allocated < 1024, "allocated " + allocated + " bytes over 10,000 compressions");
        }
    }

    private static void run(Gzip g, byte[][] in, byte[][] out, int n) {
        for (int i = 0; i < n; i++) {
            if (g.compress(in[i & 1], 0, FEED.length, out[i & 1], 32, FEED.length) == 0) {
                throw new AssertionError("did not fit");
            }
        }
    }
}
