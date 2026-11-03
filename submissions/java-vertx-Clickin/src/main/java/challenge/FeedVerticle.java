package challenge;

import io.netty.handler.flush.FlushConsolidationHandler;
import io.vertx.core.Completable;
import io.vertx.core.Future;
import io.vertx.core.VerticleBase;
import io.vertx.core.buffer.Buffer;
import io.vertx.core.http.HttpHeaders;
import io.vertx.core.http.HttpMethod;
import io.vertx.core.http.HttpServerOptions;
import io.vertx.core.http.HttpServerRequest;
import io.vertx.core.net.NetClientOptions;
import io.vertx.pgclient.PgBuilder;
import io.vertx.pgclient.PgConnectOptions;
import io.vertx.pgclient.PgException;
import io.vertx.sqlclient.PoolOptions;
import io.vertx.sqlclient.PreparedQuery;
import io.vertx.sqlclient.Row;
import io.vertx.sqlclient.RowSet;
import io.vertx.sqlclient.SqlClient;
import io.vertx.sqlclient.Tuple;
import io.vertx.sqlclient.codec.SocketConnectionBase;
import io.vertx.sqlclient.internal.SqlConnectionInternal;
import java.util.concurrent.TimeUnit;
import java.util.function.Consumer;

final class FeedVerticle extends VerticleBase {
    private static final String POST_QUERY = """
            SELECT p.id, p.body, p.created_at, u.username,
                   (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
              FROM posts p JOIN users u ON u.id = p.user_id
            """;
    private static final String FEED = POST_QUERY + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
    private static final String FIND = POST_QUERY + " WHERE p.id = $1";
    private static final String CREATE = "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at";
    private static final String LIKE = "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING";
    private static final int MAX_BODY = 16 * 1024;
    private static final CharSequence CONTENT_TYPE = HttpHeaders.createOptimized("Content-Type");
    private static final CharSequence JSON = HttpHeaders.createOptimized("application/json");
    private static final CharSequence VARY = HttpHeaders.createOptimized("Vary");
    private static final CharSequence ACCEPT_ENCODING = HttpHeaders.createOptimized("Accept-Encoding");
    private static final CharSequence CONTENT_ENCODING = HttpHeaders.createOptimized("Content-Encoding");
    private static final CharSequence GZIP = HttpHeaders.createOptimized("gzip");
    private static final CharSequence CONTENT_LENGTH = HttpHeaders.createOptimized("Content-Length");
    private static final CharSequence[] LENGTHS = new CharSequence[16384];
    static {
        for (int i = 0; i < LENGTHS.length; i++) LENGTHS[i] = HttpHeaders.createOptimized(Integer.toString(i));
    }

    private final int poolSize;
    private final long started;
    // The event loop owns the scratch array; responses copy it before reuse.
    private final Json json = new Json();
    private SqlClient db;
    private PreparedQuery<RowSet<Row>> feedQuery, findQuery, createQuery, likeQuery, healthQuery;
    private JwtVerifier jwt;
    private Gzip gzip;

    FeedVerticle(int poolSize, long started) {
        this.poolSize = poolSize;
        this.started = started;
    }

    @Override public Future<?> start() throws Exception {
        jwt = new JwtVerifier(System.getenv().getOrDefault("JWT_SECRET", "million-users-challenge"));
        int level = Integer.getInteger("mu.gzip-level", 0);
        if (level > 0) gzip = new Gzip(level);
        var connect = PgConnectOptions.fromUri(System.getenv("DATABASE_URL"))
                .setCachePreparedStatements(true)
                .setPipeliningLimit(Integer.getInteger("mu.db-pipeline", 256));
        var builder = PgBuilder.client().using(vertx).connectingTo(connect)
                .with(new PoolOptions().setMaxSize(poolSize).setEventLoopSize(0))
                .with(new NetClientOptions().setConnectTimeout(5_000));
        if (Boolean.parseBoolean(System.getProperty("mu.db-flush-batch", "true"))) {
            builder.withConnectHandler(conn -> {
                var socket = (SocketConnectionBase) ((SqlConnectionInternal) conn).unwrap().unwrap();
                socket.socket().channelHandlerContext().pipeline().addFirst(new FlushConsolidationHandler(256, true));
                conn.close();
            });
        }
        db = builder.build();
        feedQuery = db.preparedQuery(FEED);
        findQuery = db.preparedQuery(FIND);
        createQuery = db.preparedQuery(CREATE);
        likeQuery = db.preparedQuery(LIKE);
        healthQuery = db.preparedQuery("SELECT 1");
        var options = new HttpServerOptions()
                .setStrictThreadMode(true)
                .setIdleTimeout(0)
                .setCompressionSupported(false)
                .setHttp2ClearTextEnabled(false)
                .setPerFrameWebSocketCompressionSupported(false)
                .setPerMessageWebSocketCompressionSupported(false);
        String host = System.getenv().getOrDefault("HOST", "0.0.0.0");
        int port = Integer.parseInt(System.getenv().getOrDefault("PORT", "3000"));
        int extraPort = Integer.parseInt(System.getenv().getOrDefault("EXTRA_PORT", "3001"));
        var main = vertx.createHttpServer(options).requestHandler(this::route).listen(port, host);
        var extra = vertx.createHttpServer(options).requestHandler(this::route).listen(extraPort, host);
        return Future.all(main, extra);
    }

    @Override public Future<?> stop() {
        if (gzip != null) gzip.close();
        return db.close();
    }

    private final class Request implements Completable<RowSet<Row>> {
        final HttpServerRequest request;
        boolean feed;

        Request(HttpServerRequest request) {
            this.request = request;
        }

        void send(int status, boolean isFeed) {
            var response = request.response();
            if (status != 200) response.setStatusCode(status);
            response.putHeader(CONTENT_TYPE, JSON);
            Buffer body;
            if (isFeed && gzip != null) {
                response.putHeader(VARY, ACCEPT_ENCODING);
                if (json.size() >= 1024 && Gzip.accepts(request.headers())) {
                    body = gzip.compress(json.bytes(), 0, json.size());
                    response.putHeader(CONTENT_ENCODING, GZIP);
                } else body = json.buffer();
            } else body = json.buffer();
            int length = body.length();
            response.putHeader(CONTENT_LENGTH, length < LENGTHS.length ? LENGTHS[length] : Integer.toString(length)).end(body);
        }

        void error(Throwable error) {
            if (error instanceof ApiException api) {
                json.error(api.getMessage());
                send(api.status, false);
            } else {
                json.error("internal server error");
                send(500, false);
            }
        }

        void get(boolean isFeed, long id) {
            feed = isFeed;
            (isFeed ? feedQuery.execute() : findQuery.execute(Tuple.of(id))).onComplete(this);
        }

        @Override public void complete(RowSet<Row> rows, Throwable cause) {
            try {
                if (cause != null) throw new IllegalStateException(cause);
                if (feed) json.feed(rows);
                else {
                    var iterator = rows.iterator();
                    if (!iterator.hasNext()) throw new ApiException(404, "post not found");
                    Row row = iterator.next();
                    json.single(row, row.getString(1), row.getString(3), row.getInteger(4), 2);
                }
                send(200, feed);
            } catch (Exception e) {
                error(e);
            }
        }
    }

    private void route(HttpServerRequest incoming) {
        var request = new Request(incoming);
        try {
            String uri = incoming.uri();
            int start = 0;
            if (!uri.isEmpty() && uri.charAt(0) != '/') {
                int scheme = uri.indexOf("://");
                if (scheme >= 0) {
                    start = uri.indexOf('/', scheme + 3);
                    if (start < 0) throw new ApiException(404, "not found");
                }
            }
            int end = uri.indexOf('?', start);
            if (end < 0) end = uri.length();
            int length = end - start;
            if (incoming.method() == HttpMethod.GET) {
                if (length == 7 && uri.regionMatches(start, "/health", 0, 7)) afterBody(request, () -> health(request));
                else if (length == 5 && uri.regionMatches(start, "/feed", 0, 5)) get(request, true, 0);
                else if (length > 7 && uri.startsWith("/posts/", start) && noSlash(uri, start + 7, end))
                    get(request, false, id(uri, start + 7, end));
                else throw new ApiException(404, "not found");
            } else if (incoming.method() == HttpMethod.POST && length == 6 && uri.regionMatches(start, "/posts", 0, 6)) {
                var user = jwt.verify(incoming.getHeader(HttpHeaders.AUTHORIZATION));
                create(request, user);
            } else if (incoming.method() == HttpMethod.POST && length >= 11 && uri.startsWith("/posts/", start)
                    && uri.regionMatches(end - 5, "/like", 0, 5)) {
                int idStart = start + 7, idEnd = end - 5;
                // The old route returned an internal error when the prefix and suffix overlap.
                if (idStart > idEnd) throw new IllegalStateException();
                if (idStart == idEnd || !noSlash(uri, idStart, idEnd)) throw new ApiException(404, "not found");
                var user = jwt.verify(incoming.getHeader(HttpHeaders.AUTHORIZATION));
                long postId = id(uri, idStart, idEnd);
                afterBody(request, () -> query(request, likeQuery.execute(Tuple.of(user.id(), postId)), rows -> {
                    boolean already = rows.rowCount() == 0;
                    json.like(postId, already);
                    request.send(already ? 200 : 201, false);
                }, true));
            } else throw new ApiException(404, "not found");
        } catch (Exception e) {
            request.error(e);
        }
    }

    private void health(Request request) {
        healthQuery.execute().timeout(5, TimeUnit.SECONDS).onComplete((rows, cause) -> {
            boolean ok = cause == null;
            json.health(ok, (System.nanoTime() - started) / 1_000_000_000);
            request.send(ok ? 200 : 503, false);
        });
    }

    private void query(Request request, Future<RowSet<Row>> future, Consumer<RowSet<Row>> success, boolean like) {
        future.onComplete((rows, cause) -> {
            try {
                if (cause != null) {
                    if (like && cause instanceof PgException pg && "23503".equals(pg.getSqlState()))
                        throw new ApiException(404, "post not found");
                    throw new IllegalStateException(cause);
                }
                success.accept(rows);
            } catch (Exception e) {
                request.error(e);
            }
        });
    }

    private boolean noBody(Request request) {
        String length = request.request.getHeader(HttpHeaders.CONTENT_LENGTH);
        return (length == null || length.equals("0")) && request.request.getHeader(HttpHeaders.TRANSFER_ENCODING) == null;
    }

    private void get(Request request, boolean feed, long id) {
        if (noBody(request)) request.get(feed, id);
        else receiveBody(request, false, ignored -> request.get(feed, id));
    }

    private void afterBody(Request request, Runnable action) {
        if (noBody(request)) action.run();
        else receiveBody(request, false, ignored -> action.run());
    }

    private void create(Request request, JwtVerifier.User user) {
        receiveBody(request, true, body -> {
            String text = Json.body(body);
            query(request, createQuery.execute(Tuple.of(user.id(), text)), rows -> {
                json.single(rows.iterator().next(), text, user.username(), 0, 1);
                request.send(201, false);
            }, false);
        });
    }

    private void receiveBody(Request request, boolean collect, Consumer<Buffer> action) {
        Buffer body = collect ? Buffer.buffer() : null;
        int[] size = {0};
        var tooLarge = new boolean[1];
        request.request.handler(chunk -> {
            if (tooLarge[0]) return;
            if (chunk.length() > MAX_BODY - size[0]) {
                tooLarge[0] = true;
                json.error("request body too large");
                request.send(413, false);
            } else {
                size[0] += chunk.length();
                if (body != null) body.appendBuffer(chunk);
            }
        });
        request.request.endHandler(ignored -> {
            if (tooLarge[0]) return;
            try {
                action.accept(body);
            } catch (Exception e) {
                request.error(e);
            }
        });
    }

    private static boolean noSlash(String value, int start, int end) {
        int slash = value.indexOf('/', start);
        return slash < 0 || slash >= end;
    }

    private static long id(String value, int start, int end) {
        long id = 0;
        for (int i = start; i < end; i++) {
            int digit = value.charAt(i) - '0';
            if (digit < 0 || digit > 9 || id > (Long.MAX_VALUE - digit) / 10)
                throw new ApiException(400, "invalid post id");
            id = id * 10 + digit;
        }
        if (id > 0) return id;
        throw new ApiException(400, "invalid post id");
    }
}
