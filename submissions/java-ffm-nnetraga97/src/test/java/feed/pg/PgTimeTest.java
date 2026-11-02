package feed.pg;

import static org.junit.jupiter.api.Assertions.assertEquals;

import java.nio.charset.StandardCharsets;
import java.time.Instant;
import java.time.ZoneOffset;
import java.time.format.DateTimeFormatter;
import java.time.temporal.ChronoUnit;
import java.util.SplittableRandom;

import org.junit.jupiter.api.Test;

class PgTimeTest {

    private static final long EPOCH_2000_MICROS = 946_684_800_000_000L;
    private static final DateTimeFormatter ISO_MILLIS =
            DateTimeFormatter.ofPattern("uuuu-MM-dd'T'HH:mm:ss.SSS'Z'").withZone(ZoneOffset.UTC);

    private static String format(long micros) {
        byte[] out = new byte[PgTime.LENGTH + 4];
        int end = PgTime.format(micros, out, 2);
        assertEquals(2 + PgTime.LENGTH, end);
        return new String(out, 2, PgTime.LENGTH, StandardCharsets.US_ASCII);
    }

    /** The reference: java.time, truncated (not rounded) to milliseconds. */
    private static String expected(long pgMicros) {
        Instant t = Instant.EPOCH.plus(pgMicros + EPOCH_2000_MICROS, ChronoUnit.MICROS).truncatedTo(ChronoUnit.MILLIS);
        return ISO_MILLIS.format(t);
    }

    @Test
    void formatsThePostgresEpoch() {
        assertEquals("2000-01-01T00:00:00.000Z", format(0));
    }

    /** Only this test can catch rounding: every seed timestamp is a whole millisecond, so the golden files can't. */
    @Test
    void truncatesMicrosecondsInsteadOfRounding() {
        assertEquals("2000-01-01T00:00:00.000Z", format(999));
        assertEquals("2000-01-01T00:00:00.001Z", format(1_999));
    }

    @Test
    void truncatesTowardTheEarlierMillisecondBeforeTheEpoch() {
        assertEquals("1999-12-31T23:59:59.999Z", format(-1));
    }

    @Test
    void handlesLeapDays() {
        long micros = (Instant.parse("2024-02-29T13:45:07.123456Z").toEpochMilli() * 1000 + 456) - EPOCH_2000_MICROS;
        assertEquals("2024-02-29T13:45:07.123Z", format(micros));
    }

    @Test
    void matchesJavaTimeOverACentury() {
        SplittableRandom random = new SplittableRandom(42);
        long hundredYears = 100L * 366 * 86_400 * 1_000_000;
        for (int i = 0; i < 200_000; i++) {
            long micros = random.nextLong(-hundredYears, hundredYears);
            assertEquals(expected(micros), format(micros), "micros=" + micros);
        }
    }
}
