package com.example.feed;

import java.io.IOException;
import java.io.OutputStream;
import java.io.UncheckedIOException;
import java.time.OffsetDateTime;
import java.time.ZoneOffset;

import com.fasterxml.jackson.core.JsonFactory;
import com.fasterxml.jackson.core.JsonGenerator;

import io.vertx.core.buffer.Buffer;
import io.vertx.core.json.DecodeException;
import io.vertx.core.json.JsonObject;
import io.vertx.sqlclient.Row;
import io.vertx.sqlclient.RowSet;

final class Json {
    private static final JsonFactory FACTORY = new JsonFactory();

    @FunctionalInterface
    private interface Body {
        void write(JsonGenerator g) throws IOException;
    }

    private static Buffer write(int sizeHint, Body body) {
        Buffer out = Buffer.buffer(sizeHint);
        try (JsonGenerator g = FACTORY.createGenerator(new BufferOutputStream(out))) {
            body.write(g);
        } catch (IOException e) {
            throw new UncheckedIOException(e);
        }
        return out;
    }

    private static final class BufferOutputStream extends OutputStream {
        private final Buffer buffer;

        BufferOutputStream(Buffer buffer) {
            this.buffer = buffer;
        }

        @Override
        public void write(int b) {
            buffer.appendByte((byte) b);
        }

        @Override
        public void write(byte[] b, int off, int len) {
            buffer.appendBytes(b, off, len);
        }
    }

    static Buffer feed(RowSet<Row> rows) {
        return write(6144, g -> {
            g.writeStartObject();
            g.writeArrayFieldStart("posts");
            for (Row r : rows) {
                post(g, r.getLong(0), r.getString(1), r.getOffsetDateTime(2), r.getString(3), r.getInteger(4));
            }
            g.writeEndArray();
            g.writeEndObject();
        });
    }

    static Buffer post(Row r) {
        return post(r.getLong(0), r.getString(1), r.getOffsetDateTime(2), r.getString(3), r.getInteger(4));
    }

    static Buffer post(long id, String body, OffsetDateTime createdAt, String author, int likeCount) {
        return write(768, g -> {
            g.writeStartObject();
            g.writeFieldName("post");
            post(g, id, body, createdAt, author, likeCount);
            g.writeEndObject();
        });
    }

    private static void post(JsonGenerator g, long id, String body, OffsetDateTime createdAt, String author,
            int likeCount) throws IOException {
        g.writeStartObject();
        g.writeNumberField("id", id);
        g.writeStringField("body", body);
        g.writeStringField("created_at", isoMillis(createdAt));
        g.writeStringField("author", author);
        g.writeNumberField("like_count", likeCount);
        g.writeEndObject();
    }

    static Buffer like(boolean alreadyLiked, long postId) {
        return Buffer.buffer("{\"liked\":true,\"already_liked\":" + alreadyLiked + ",\"post_id\":" + postId + "}");
    }

    static Buffer health(long uptimeSeconds) {
        return Buffer.buffer("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":" + uptimeSeconds + "}");
    }

    static Buffer degraded(String error) {
        return write(256, g -> {
            g.writeStartObject();
            g.writeStringField("status", "degraded");
            g.writeStringField("db", "unreachable");
            g.writeStringField("error", error);
            g.writeEndObject();
        });
    }

    static String postBody(Buffer requestBody) {
        Object parsed;
        try {
            parsed = io.vertx.core.json.Json.decodeValue(requestBody == null ? Buffer.buffer() : requestBody);
        } catch (DecodeException e) {
            throw ApiError.MALFORMED_JSON;
        }
        if (!(parsed instanceof JsonObject obj) || !(obj.getValue("body") instanceof String raw)) {
            throw ApiError.BODY_REQUIRED;
        }
        String body = raw.strip();
        if (body.isEmpty()) {
            throw ApiError.BODY_REQUIRED;
        }
        if (body.length() > 500 && body.codePointCount(0, body.length()) > 500) {
            throw ApiError.BODY_TOO_LONG;
        }
        return body;
    }

    static String isoMillis(OffsetDateTime t) {
        OffsetDateTime u = ZoneOffset.UTC.equals(t.getOffset()) ? t : t.withOffsetSameInstant(ZoneOffset.UTC);
        char[] c = new char[24];
        digits(c, 0, u.getYear(), 4);
        c[4] = '-';
        digits(c, 5, u.getMonthValue(), 2);
        c[7] = '-';
        digits(c, 8, u.getDayOfMonth(), 2);
        c[10] = 'T';
        digits(c, 11, u.getHour(), 2);
        c[13] = ':';
        digits(c, 14, u.getMinute(), 2);
        c[16] = ':';
        digits(c, 17, u.getSecond(), 2);
        c[19] = '.';
        digits(c, 20, u.getNano() / 1_000_000, 3);
        c[23] = 'Z';
        return new String(c);
    }

    private static void digits(char[] c, int at, int value, int width) {
        for (int i = at + width - 1; i >= at; i--) {
            c[i] = (char) ('0' + value % 10);
            value /= 10;
        }
    }

    private Json() {
    }
}
