package feed.api;

import java.nio.charset.StandardCharsets;

import feed.http.ResponseBuffer;
import feed.pg.PgRow;

/** The API's JSON bodies (SPEC.md), as pre-encoded fragments and writers that render straight into a response. */
final class Json {

    static final byte[] NOT_FOUND = ascii("{\"error\":\"not found\"}");
    static final byte[] INVALID_ID = ascii("{\"error\":\"invalid post id\"}");
    static final byte[] POST_NOT_FOUND = ascii("{\"error\":\"post not found\"}");
    static final byte[] MISSING_TOKEN = ascii("{\"error\":\"missing bearer token\"}");
    static final byte[] INVALID_TOKEN = ascii("{\"error\":\"invalid or expired token\"}");
    static final byte[] BAD_TOKEN_PAYLOAD = ascii("{\"error\":\"invalid token payload\"}");
    static final byte[] MALFORMED_JSON = ascii("{\"error\":\"malformed JSON body\"}");
    static final byte[] BODY_REQUIRED = ascii("{\"error\":\"body is required\"}");
    static final byte[] BODY_TOO_LONG = ascii("{\"error\":\"body must be at most 500 characters\"}");
    static final byte[] INTERNAL_ERROR = ascii("{\"error\":\"internal server error\"}");
    static final byte[] OVERLOADED = ascii("{\"error\":\"overloaded\"}");

    private static final byte[] NO_DATABASE = ascii("no database connection");
    private static final byte[] HEALTH_OK = ascii("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    private static final byte[] HEALTH_DOWN = ascii("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":");
    private static final byte[] POSTS_OPEN = ascii("{\"posts\":[");
    private static final byte[] POSTS_CLOSE = ascii("]}");
    private static final byte[] POST_OPEN = ascii("{\"post\":");
    private static final byte[] ID = ascii("{\"id\":");
    private static final byte[] BODY = ascii(",\"body\":");
    private static final byte[] CREATED_AT = ascii(",\"created_at\":");
    private static final byte[] AUTHOR = ascii(",\"author\":");
    private static final byte[] LIKE_COUNT = ascii(",\"like_count\":");
    private static final byte[] NO_LIKES_YET = ascii(",\"like_count\":0}}");
    private static final byte[] LIKED = ascii("{\"liked\":true,\"already_liked\":");
    private static final byte[] POST_ID = ascii(",\"post_id\":");
    private static final byte[] TRUE = ascii("true");
    private static final byte[] FALSE = ascii("false");

    private Json() {
    }

    static void feedStart(ResponseBuffer out) {
        out.begin();
        out.raw(POSTS_OPEN);
    }

    /** One post of a feed; {@code first} leaves out the separating comma. */
    static void feedPost(ResponseBuffer out, PgRow row, boolean first) {
        if (!first) {
            out.raw((byte) ',');
        }
        post(out, row);
    }

    static void feedEnd(ResponseBuffer out) {
        out.raw(POSTS_CLOSE);
    }

    /** {"post":{...}} from a row of the post query. */
    static void singlePost(ResponseBuffer out, PgRow row) {
        out.begin();
        out.raw(POST_OPEN);
        post(out, row);
        out.raw((byte) '}');
    }

    /** {"post":{...}} for a post just inserted: nobody has liked it yet. */
    static void newPost(ResponseBuffer out, long id, byte[] text, int bodyLength, long createdAtMicros) {
        out.begin();
        out.raw(POST_OPEN);
        out.raw(ID);
        out.number(id);
        out.raw(BODY);
        out.string(text, 0, bodyLength);
        out.raw(CREATED_AT);
        out.timestamp(createdAtMicros);
        out.raw(AUTHOR);
        out.string(text, bodyLength, text.length - bodyLength);
        out.raw(NO_LIKES_YET);
    }

    static void liked(ResponseBuffer out, boolean alreadyLiked, long postId) {
        out.begin();
        out.raw(LIKED);
        out.raw(alreadyLiked ? TRUE : FALSE);
        out.raw(POST_ID);
        out.number(postId);
        out.raw((byte) '}');
    }

    static void healthy(ResponseBuffer out, long uptimeSeconds) {
        out.begin();
        out.raw(HEALTH_OK);
        out.number(uptimeSeconds);
        out.raw((byte) '}');
    }

    static void degraded(ResponseBuffer out, String reason) {
        degraded(out, reason.getBytes(StandardCharsets.UTF_8));
    }

    static void noDatabase(ResponseBuffer out) {
        degraded(out, NO_DATABASE);
    }

    static void error(ResponseBuffer out, byte[] body) {
        out.begin();
        out.raw(body);
    }

    private static void degraded(ResponseBuffer out, byte[] reason) {
        out.begin();
        out.raw(HEALTH_DOWN);
        out.string(reason, 0, reason.length);
        out.raw((byte) '}');
    }

    /** id, body, created_at, author, like_count: the post query's columns in order. */
    private static void post(ResponseBuffer out, PgRow row) {
        out.raw(ID);
        out.number(row.int8(0));
        out.raw(BODY);
        out.string(row.buffer(), row.offset(1), row.length(1));
        out.raw(CREATED_AT);
        out.timestamp(row.int8(2));
        out.raw(AUTHOR);
        out.string(row.buffer(), row.offset(3), row.length(3));
        out.raw(LIKE_COUNT);
        out.number(row.int4(4));
        out.raw((byte) '}');
    }

    private static byte[] ascii(String s) {
        return s.getBytes(StandardCharsets.UTF_8);
    }
}
