package feed.api;

import feed.auth.JwtVerifier;
import feed.db.Database;
import feed.db.Queries;
import feed.http.Bytes;
import feed.http.Connections;
import feed.http.HttpParser;
import feed.http.ResponseBuffer;
import feed.pg.PgProtocol;

/**
 * The feed API (SPEC.md): routes each request, authenticates and validates it, and either answers at once or runs
 * its query. Query results come back through {@link QueryResults}.
 */
public final class FeedApi implements Connections.RequestHandler, Database.Requests {

    private final Connections connections;
    private final Database database;
    private final JwtVerifier jwt;
    private final long startNanos;
    private final RequestArgs args = new RequestArgs();
    private final NewPostBody newPostBody = new NewPostBody();
    private final ResponseBuffer out = new ResponseBuffer(1024);

    public FeedApi(Connections connections, Database database, JwtVerifier jwt, long startNanos) {
        this.connections = connections;
        this.database = database;
        this.jwt = jwt;
        this.startNanos = startNanos;
    }

    // ---- routing ----

    @Override
    public boolean handle(int slot, HttpParser request, byte[] b) {
        int path = request.pathStart();
        int end = request.pathEnd();
        if (request.method() == HttpParser.GET) {
            if (Bytes.equals(b, path, end, "/feed")) {
                return query(slot, Queries.FEED);
            }
            if (Bytes.equals(b, path, end, "/health")) {
                return query(slot, Queries.PING);
            }
            if (startsWith(b, path, end, "/posts/")) {
                return getPost(slot, Bytes.positiveId(b, path + 7, end));
            }
        } else if (request.method() == HttpParser.POST) {
            if (Bytes.equals(b, path, end, "/posts")) {
                return authenticated(slot, request, b) && createPost(slot, request, b);
            }
            if (startsWith(b, path, end, "/posts/") && endsWith(b, path, end, "/like") && end - path > 12) {
                return authenticated(slot, request, b) && like(slot, Bytes.positiveId(b, path + 7, end - 5));
            }
        }
        connections.reply(slot, 404, Json.NOT_FOUND);
        return false;
    }

    @Override
    public void closed(int slot) {
        args.clear(slot);
    }

    // ---- requests ----

    private boolean getPost(int slot, long postId) {
        if (postId <= 0) {
            connections.reply(slot, 400, Json.INVALID_ID);
            return false;
        }
        args.setPost(slot, postId);
        return query(slot, Queries.POST);
    }

    private boolean createPost(int slot, HttpParser request, byte[] b) {
        switch (newPostBody.read(b, request.bodyStart(), request.bodyEnd())) {
            case MALFORMED -> connections.reply(slot, 400, Json.MALFORMED_JSON);
            case MISSING -> connections.reply(slot, 400, Json.BODY_REQUIRED);
            case TOO_LONG -> connections.reply(slot, 400, Json.BODY_TOO_LONG);
            case OK -> {
                args.setNewPost(slot, jwt.userId(), newPostBody.text(), newPostBody.start(), newPostBody.length(),
                        jwt.username(), jwt.usernameLength());
                return query(slot, Queries.INSERT_POST);
            }
        }
        return false;
    }

    private boolean like(int slot, long postId) {
        if (postId <= 0) {
            connections.reply(slot, 400, Json.INVALID_ID);
            return false;
        }
        args.setLike(slot, jwt.userId(), postId);
        return query(slot, Queries.INSERT_LIKE);
    }

    /** Checks the bearer token (answering 401 if it fails); auth comes before any other check. */
    private boolean authenticated(int slot, HttpParser request, byte[] b) {
        switch (jwt.verify(b, request.authorizationStart(), request.authorizationLength(),
                System.currentTimeMillis() / 1000)) {
            case VALID -> {
                return true;
            }
            case MISSING -> connections.reply(slot, 401, Json.MISSING_TOKEN);
            case BAD_PAYLOAD -> connections.reply(slot, 401, Json.BAD_TOKEN_PAYLOAD);
            case INVALID -> connections.reply(slot, 401, Json.INVALID_TOKEN);
        }
        return false;
    }

    /** Runs {@code query} for the request; true if it is now in flight, false if it was answered at once. */
    private boolean query(int slot, int query) {
        switch (database.submit(connections.tag(slot), query)) {
            case SENT, QUEUED -> {
                return true;
            }
            case REJECTED -> connections.reply(slot, 503, Json.OVERLOADED);
            case UNAVAILABLE -> connections.reply(slot, writeUnavailable(query), out);
        }
        args.clear(slot);
        return false;
    }

    // ---- Database.Requests ----

    @Override
    public Database.ResultHandlers newResultHandlers() {
        return new QueryResults(connections, args, startNanos);
    }

    @Override
    public void bind(PgProtocol protocol, int query, long tag) {
        int slot = Connections.slot(tag);
        protocol.bind(query);
        switch (query) {
            case Queries.POST -> protocol.paramInt8(args.postId(slot));
            case Queries.INSERT_POST -> {
                protocol.paramInt8(args.userId(slot));
                protocol.paramText(args.text(slot), 0, args.bodyLength(slot));
            }
            case Queries.INSERT_LIKE -> {
                protocol.paramInt8(args.userId(slot));
                protocol.paramInt8(args.postId(slot));
            }
            default -> {
                // FEED and PING take no parameters
            }
        }
    }

    @Override
    public boolean isWaiting(long tag) {
        return connections.isWaiting(tag);
    }

    @Override
    public void unavailable(long tag, int query) {
        args.clear(Connections.slot(tag));
        int status = writeUnavailable(query);
        connections.complete(tag, status, out);
    }

    /** No database connection is up: 503 with a degraded body for /health, 500 for everything else. */
    private int writeUnavailable(int query) {
        if (query == Queries.PING) {
            Json.noDatabase(out);
            return 503;
        }
        Json.error(out, Json.INTERNAL_ERROR);
        return 500;
    }

    private static boolean startsWith(byte[] b, int from, int to, String prefix) {
        return to - from > prefix.length() && Bytes.equals(b, from, from + prefix.length(), prefix);
    }

    private static boolean endsWith(byte[] b, int from, int to, String suffix) {
        return to - from > suffix.length() && Bytes.equals(b, to - suffix.length(), to, suffix);
    }
}
