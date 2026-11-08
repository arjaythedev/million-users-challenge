package feed.api;

import feed.db.Database;
import feed.db.Queries;
import feed.http.Connections;
import feed.http.ResponseBuffer;
import feed.pg.PgError;
import feed.pg.PgHandler;
import feed.pg.PgRow;

/**
 * Turns one Postgres connection's query results into HTTP responses. Rows are rendered as they arrive into this
 * connection's own response buffer, and the response goes out at the query's end.
 */
final class QueryResults implements Database.ResultHandlers {

    private static final String FOREIGN_KEY_VIOLATION = "23503";

    private final Connections connections;
    private final RequestArgs args;
    private final long startNanos;
    private final ResponseBuffer out = new ResponseBuffer(16 * 1024);
    private final Feed feed = new Feed();
    private final Post post = new Post();
    private final NewPost newPost = new NewPost();
    private final Like like = new Like();
    private final Health health = new Health();

    QueryResults(Connections connections, RequestArgs args, long startNanos) {
        this.connections = connections;
        this.args = args;
        this.startNanos = startNanos;
    }

    @Override
    public PgHandler handler(int query) {
        return switch (query) {
            case Queries.FEED -> feed;
            case Queries.POST -> post;
            case Queries.INSERT_POST -> newPost;
            case Queries.INSERT_LIKE -> like;
            case Queries.PING -> health;
            default -> throw new IllegalArgumentException("query " + query);
        };
    }

    private void fail(long tag, int status, byte[] body) {
        Json.error(out, body);
        connections.complete(tag, status, out);
    }

    /** {"posts":[...]}: up to 20 rows. */
    private final class Feed implements PgHandler {
        private int rows;
        private boolean failed;

        @Override
        public void onRow(long tag, PgRow row) {
            if (rows == 0) {
                Json.feedStart(out);
            }
            Json.feedPost(out, row, rows == 0);
            rows++;
        }

        @Override
        public void onError(long tag, PgError error) {
            failed = true;
        }

        @Override
        public void onDone(long tag) {
            if (failed) {
                fail(tag, 500, Json.INTERNAL_ERROR);
            } else {
                if (rows == 0) {
                    Json.feedStart(out);
                }
                Json.feedEnd(out);
                connections.complete(tag, 200, out);
            }
            rows = 0;
            failed = false;
        }
    }

    /** {"post":{...}}, or 404 when no row came back. */
    private final class Post implements PgHandler {
        private boolean found;
        private boolean failed;

        @Override
        public void onRow(long tag, PgRow row) {
            found = true;
            Json.singlePost(out, row);
        }

        @Override
        public void onError(long tag, PgError error) {
            failed = true;
        }

        @Override
        public void onDone(long tag) {
            if (failed) {
                fail(tag, 500, Json.INTERNAL_ERROR);
            } else if (!found) {
                fail(tag, 404, Json.POST_NOT_FOUND);
            } else {
                connections.complete(tag, 200, out);
            }
            found = false;
            failed = false;
        }
    }

    /** 201 with the new post, its body and author taken from the request. */
    private final class NewPost implements PgHandler {
        private long id;
        private long createdAt;
        private boolean failed;

        @Override
        public void onRow(long tag, PgRow row) {
            id = row.int8(0);
            createdAt = row.int8(1);
        }

        @Override
        public void onError(long tag, PgError error) {
            failed = true;
        }

        @Override
        public void onDone(long tag) {
            if (connections.isWaiting(tag)) {
                int slot = Connections.slot(tag);
                byte[] text = args.text(slot);
                args.clear(slot); // before complete(), which may start this client's next request
                if (failed || id == 0) {
                    fail(tag, 500, Json.INTERNAL_ERROR);
                } else {
                    Json.newPost(out, id, text, args.bodyLength(slot), createdAt);
                    connections.complete(tag, 201, out);
                }
            }
            id = 0;
            failed = false;
        }
    }

    /** 201 for a new like, 200 if it already existed (no row inserted), 404 if the post doesn't exist. */
    private final class Like implements PgHandler {
        private long inserted;
        private boolean missingPost;
        private boolean failed;

        @Override
        public void onComplete(long tag, long rows) {
            inserted = rows;
        }

        @Override
        public void onError(long tag, PgError error) {
            if (error.is(FOREIGN_KEY_VIOLATION)) {
                missingPost = true;
            } else {
                failed = true;
            }
        }

        @Override
        public void onDone(long tag) {
            if (missingPost) {
                fail(tag, 404, Json.POST_NOT_FOUND);
            } else if (failed) {
                fail(tag, 500, Json.INTERNAL_ERROR);
            } else if (connections.isWaiting(tag)) {
                Json.liked(out, inserted == 0, args.postId(Connections.slot(tag)));
                connections.complete(tag, inserted == 0 ? 200 : 201, out);
            }
            inserted = 0;
            missingPost = false;
            failed = false;
        }
    }

    /** /health: 200 with the uptime, or 503 with Postgres's error. */
    private final class Health implements PgHandler {
        private String failure;

        @Override
        public void onError(long tag, PgError error) {
            failure = error.message();
        }

        @Override
        public void onDone(long tag) {
            if (failure == null) {
                Json.healthy(out, (System.nanoTime() - startNanos) / 1_000_000_000L);
                connections.complete(tag, 200, out);
            } else {
                Json.degraded(out, failure);
                connections.complete(tag, 503, out);
            }
            failure = null;
        }
    }
}
