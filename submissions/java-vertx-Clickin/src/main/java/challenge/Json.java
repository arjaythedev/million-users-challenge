package challenge;

import io.vertx.core.buffer.Buffer;
import io.vertx.sqlclient.Row;
import io.vertx.sqlclient.RowSet;
import java.time.OffsetDateTime;
import java.util.Arrays;
import tools.jackson.core.JsonToken;
import tools.jackson.core.json.JsonFactory;

final class Json {
    private static final JsonFactory FACTORY = JsonFactory.builder().build();
    private static final byte[] HEX = "0123456789ABCDEF".getBytes(java.nio.charset.StandardCharsets.US_ASCII);
    private static final byte[] DIGITS = new byte[200];
    static {
        for (int i = 0; i < 100; i++) { DIGITS[2 * i] = (byte) ('0' + i / 10); DIGITS[2 * i + 1] = (byte) ('0' + i % 10); }
    }
    private byte[] bytes = new byte[8192];
    private int size;

    static final class Fields {
        String body, alg, sub, username;
        Double exp;
    }

    static Fields read(byte[] bytes) {
        var fields = new Fields();
        try (var json = FACTORY.createParser(bytes)) {
            var first = json.nextToken();
            if (first == null) throw new IllegalArgumentException("empty JSON");
            if (first == JsonToken.START_OBJECT) {
                while (json.nextToken() != JsonToken.END_OBJECT) {
                    String name = json.currentName();
                    JsonToken value = json.nextToken();
                    if (value == null) throw new IllegalArgumentException("missing value");
                    switch (name) {
                        case "body" -> fields.body = value == JsonToken.VALUE_STRING ? json.getString() : null;
                        case "alg" -> fields.alg = value == JsonToken.VALUE_STRING ? json.getString() : null;
                        case "sub" -> fields.sub = value == JsonToken.VALUE_STRING ? json.getString() : null;
                        case "username" -> fields.username = value == JsonToken.VALUE_STRING ? json.getString() : null;
                        case "exp" -> fields.exp = value.isNumeric() ? json.getDoubleValue() : null;
                        default -> { }
                    }
                    json.skipChildren();
                }
            } else {
                json.skipChildren();
            }
            if (json.nextToken() != null) throw new IllegalArgumentException("trailing JSON");
        }
        return fields;
    }

    static String body(Buffer raw) {
        String value;
        try {
            value = read(raw.getBytes()).body;
        } catch (Exception e) {
            throw new ApiException(400, "malformed JSON body");
        }
        if (value == null || (value = value.strip()).isEmpty()) throw new ApiException(400, "body is required");
        if (value.codePointCount(0, value.length()) > 500) throw new ApiException(400, "body must be at most 500 characters");
        return value;
    }

    byte[] bytes() { return bytes; }
    int size() { return size; }
    Buffer buffer() { return Buffer.buffer(size).appendBytes(bytes, 0, size); }

    void error(String message) {
        size = 0;
        literal("{\"error\":");
        string(message);
        literal("}");
    }

    void health(boolean ok, long uptime) {
        size = 0;
        if (ok) {
            literal("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
            number(uptime);
            literal("}");
        } else literal("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"internal server error\"}");
    }

    void feed(RowSet<Row> rows) {
        size = 0;
        literal("{\"posts\":[");
        boolean first = true;
        for (Row row : rows) {
            if (!first) literal(",");
            first = false;
            post(row, row.getString(1), row.getString(3), row.getInteger(4), 2);
        }
        literal("]}");
    }

    void single(Row row, String body, String author, int likes, int createdAtColumn) {
        size = 0;
        literal("{\"post\":");
        post(row, body, author, likes, createdAtColumn);
        literal("}");
    }

    private void post(Row row, String body, String author, int likes, int createdAtColumn) {
        literal("{\"id\":");
        number(row.getLong(0));
        literal(",\"body\":");
        string(body);
        literal(",\"created_at\":\"");
        timestamp(row.getOffsetDateTime(createdAtColumn));
        literal("\",\"author\":");
        string(author);
        literal(",\"like_count\":");
        number(likes);
        literal("}");
    }

    void like(long id, boolean already) {
        size = 0;
        literal(already ? "{\"liked\":true,\"already_liked\":true,\"post_id\":" : "{\"liked\":true,\"already_liked\":false,\"post_id\":");
        number(id);
        literal("}");
    }

    private void ensure(int count) {
        int needed = size + count;
        if (needed > bytes.length) bytes = Arrays.copyOf(bytes, Math.max(needed, bytes.length * 2));
    }

    @SuppressWarnings("deprecation")
    private void literal(String value) {
        ensure(value.length());
        value.getBytes(0, value.length(), bytes, size);
        size += value.length();
    }

    private void number(long value) {
        ensure(20);
        int end = size + 20, pos = end;
        boolean negative = value < 0;
        if (!negative) value = -value;
        while (value <= -100) {
            int pair = (int) -(value % 100) * 2;
            bytes[--pos] = DIGITS[pair + 1];
            bytes[--pos] = DIGITS[pair];
            value /= 100;
        }
        int pair = (int) -value;
        if (pair >= 10) { bytes[--pos] = DIGITS[pair * 2 + 1]; bytes[--pos] = DIGITS[pair * 2]; }
        else bytes[--pos] = (byte) ('0' + pair);
        if (negative) bytes[--pos] = '-';
        System.arraycopy(bytes, pos, bytes, size, end - pos);
        size += end - pos;
    }

    @SuppressWarnings("deprecation")
    private void string(String value) {
        if (value == null) { literal("null"); return; }
        int length = value.length();
        ensure(length + 2);
        bytes[size++] = '"';
        int start = size, i = 0;
        value.getBytes(0, length, bytes, start);
        while (i < length) {
            byte b = bytes[start + i];
            // getBytes truncates chars above 0xFF, so the byte alone cannot prove ASCII.
            if (b < 0x20 || b == '"' || b == '\\' || value.charAt(i) != b) break;
            i++;
        }
        size += i;
        for (; i < length; i++) {
            int c = value.charAt(i);
            ensure(6);
            if (c == '"' || c == '\\') {
                bytes[size++] = '\\'; bytes[size++] = (byte) c;
            } else if (c < 0x20) {
                int escape = switch (c) { case '\b' -> 'b'; case '\t' -> 't'; case '\n' -> 'n'; case '\f' -> 'f'; case '\r' -> 'r'; default -> 0; };
                if (escape != 0) { bytes[size++] = '\\'; bytes[size++] = (byte) escape; }
                else unicodeEscape(c);
            } else if (c < 0x80) bytes[size++] = (byte) c;
            else if (c < 0x800) {
                bytes[size++] = (byte) (0xC0 | c >> 6);
                bytes[size++] = (byte) (0x80 | c & 0x3F);
            } else if (c >= 0xD800 && c <= 0xDFFF) {
                if (c <= 0xDBFF && i + 1 < value.length() && Character.isLowSurrogate(value.charAt(i + 1))) {
                    c = Character.toCodePoint((char) c, value.charAt(++i));
                    bytes[size++] = (byte) (0xF0 | c >> 18);
                    bytes[size++] = (byte) (0x80 | c >> 12 & 0x3F);
                    bytes[size++] = (byte) (0x80 | c >> 6 & 0x3F);
                    bytes[size++] = (byte) (0x80 | c & 0x3F);
                } else unicodeEscape(c);
            } else {
                bytes[size++] = (byte) (0xE0 | c >> 12);
                bytes[size++] = (byte) (0x80 | c >> 6 & 0x3F);
                bytes[size++] = (byte) (0x80 | c & 0x3F);
            }
        }
        ensure(1);
        bytes[size++] = '"';
    }

    private void unicodeEscape(int c) {
        bytes[size++] = '\\'; bytes[size++] = 'u';
        bytes[size++] = HEX[c >> 12]; bytes[size++] = HEX[c >> 8 & 15];
        bytes[size++] = HEX[c >> 4 & 15]; bytes[size++] = HEX[c & 15];
    }

    private void timestamp(OffsetDateTime date) {
        long seconds = date.toEpochSecond();
        // Convert epoch days to a Gregorian date in 400-year eras.
        long days = Math.floorDiv(seconds, 86400) + 719468;
        int time = (int) Math.floorMod(seconds, 86400);
        long era = Math.floorDiv(days, 146097);
        long dayOfEra = days - era * 146097;
        long yearOfEra = (dayOfEra - dayOfEra / 1460 + dayOfEra / 36524 - dayOfEra / 146096) / 365;
        long year = yearOfEra + era * 400;
        int dayOfYear = (int) (dayOfEra - (365 * yearOfEra + yearOfEra / 4 - yearOfEra / 100));
        int monthPart = (5 * dayOfYear + 2) / 153;
        int day = dayOfYear - (153 * monthPart + 2) / 5 + 1;
        int month = monthPart + (monthPart < 10 ? 3 : -9);
        if (month <= 2) year++;
        if (year < 0) { literal("-"); year = -year; }
        else if (year > 9999) literal("+");
        if (year < 1000) literal("0");
        if (year < 100) literal("0");
        if (year < 10) literal("0");
        number(year);
        literal("-"); digits2(month); literal("-"); digits2(day);
        literal("T"); digits2(time / 3600); literal(":"); digits2(time / 60 % 60); literal(":"); digits2(time % 60);
        int millis = date.getNano() / 1_000_000;
        literal("."); digits2(millis / 10); ensure(1); bytes[size++] = (byte) ('0' + millis % 10); literal("Z");
    }

    private void digits2(int value) {
        ensure(2);
        bytes[size++] = DIGITS[value * 2];
        bytes[size++] = DIGITS[value * 2 + 1];
    }
}
