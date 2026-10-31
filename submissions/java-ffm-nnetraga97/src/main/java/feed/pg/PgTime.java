package feed.pg;

/**
 * Formats a binary TIMESTAMPTZ (microseconds since 2000-01-01 UTC) as {@code YYYY-MM-DDTHH:MM:SS.mmmZ}, truncated
 * (never rounded) to milliseconds, as SPEC.md requires. No allocation: it writes straight into the response bytes.
 */
public final class PgTime {

    /** Length of the formatted timestamp. */
    public static final int LENGTH = 24;

    private static final long POSTGRES_EPOCH_UNIX_MILLIS = 946_684_800_000L;
    private static final long MILLIS_PER_DAY = 86_400_000L;

    private PgTime() {
    }

    /** Writes the timestamp at {@code off}; returns the offset after it. Years 0-9999. */
    public static int format(long micros, byte[] out, int off) {
        long unixMillis = Math.floorDiv(micros, 1000) + POSTGRES_EPOCH_UNIX_MILLIS;
        long days = Math.floorDiv(unixMillis, MILLIS_PER_DAY);
        int millisOfDay = (int) Math.floorMod(unixMillis, MILLIS_PER_DAY);

        // Days since 1970-01-01 to year/month/day (Howard Hinnant's civil_from_days).
        long z = days + 719_468;
        long era = Math.floorDiv(z, 146_097);
        int dayOfEra = (int) (z - era * 146_097);
        int yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36_524 - dayOfEra / 146_096) / 365;
        int dayOfYear = dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100);
        int mp = (5 * dayOfYear + 2) / 153;
        int day = dayOfYear - (153 * mp + 2) / 5 + 1;
        int month = mp < 10 ? mp + 3 : mp - 9;
        int year = (int) (yearOfEra + era * 400) + (month <= 2 ? 1 : 0);

        int seconds = millisOfDay / 1000;
        off = digits(year, 4, out, off);
        out[off++] = '-';
        off = digits(month, 2, out, off);
        out[off++] = '-';
        off = digits(day, 2, out, off);
        out[off++] = 'T';
        off = digits(seconds / 3600, 2, out, off);
        out[off++] = ':';
        off = digits(seconds / 60 % 60, 2, out, off);
        out[off++] = ':';
        off = digits(seconds % 60, 2, out, off);
        out[off++] = '.';
        off = digits(millisOfDay % 1000, 3, out, off);
        out[off++] = 'Z';
        return off;
    }

    private static int digits(int value, int width, byte[] out, int off) {
        for (int i = off + width - 1; i >= off; i--) {
            out[i] = (byte) ('0' + value % 10);
            value /= 10;
        }
        return off + width;
    }
}
