package challenge;

import io.netty.channel.EventLoop;
import io.netty.handler.flush.FlushConsolidationHandler;
import io.netty.util.concurrent.FastThreadLocal;
import io.vertx.core.Future;
import io.vertx.core.Vertx;
import io.vertx.core.VertxOptions;
import io.vertx.core.internal.ContextInternal;
import io.vertx.core.internal.VertxInternal;
import io.vertx.core.net.NetClientOptions;
import io.vertx.pgclient.PgBuilder;
import io.vertx.pgclient.PgConnectOptions;
import io.vertx.pgclient.PgException;
import io.vertx.sqlclient.PoolOptions;
import io.vertx.sqlclient.Row;
import io.vertx.sqlclient.RowSet;
import io.vertx.sqlclient.SqlClient;
import io.vertx.sqlclient.Tuple;
import io.vertx.sqlclient.codec.SocketConnectionBase;
import io.vertx.sqlclient.internal.SqlConnectionInternal;
import jakarta.annotation.PreDestroy;
import jakarta.inject.Singleton;
import java.time.OffsetDateTime;
import java.time.format.DateTimeFormatter;
import java.time.format.DateTimeFormatterBuilder;
import java.time.temporal.ChronoUnit;
import java.util.ArrayList;
import java.util.List;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.CompletionException;
import java.util.concurrent.ConcurrentLinkedQueue;
import java.util.concurrent.TimeUnit;
import java.util.function.Function;

/**
 * One Postgres pool per Micronaut (Netty) event loop. Each pool's connections are registered on that same loop
 * through a Vert.x context built on it, so a request, its query and its response all run on the channel's own
 * thread: no hop to a separate database loop and back.
 */
@Singleton
public final class Database implements AutoCloseable {
    private static final String POST_QUERY = """
            SELECT p.id, p.body, p.created_at, u.username,
                   (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
              FROM posts p JOIN users u ON u.id = p.user_id
            """;
    private static final String FEED = POST_QUERY + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
    private static final String FIND = POST_QUERY + " WHERE p.id = $1";
    private static final String CREATE = "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at";
    private static final String LIKE = "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING";
    private static final boolean FLUSH_BATCH = Boolean.parseBoolean(System.getProperty("mu.db-flush-batch", "true"));
    private static final DateTimeFormatter TIMESTAMP = new DateTimeFormatterBuilder().appendInstant(3).toFormatter();

    private record Local(ContextInternal context, SqlClient client) {}

    private final VertxInternal vertx;
    private final PgConnectOptions connect;
    private final int poolPerLoop;
    private final ConcurrentLinkedQueue<SqlClient> clients = new ConcurrentLinkedQueue<>();
    private final FastThreadLocal<Local> local = new FastThreadLocal<>();

    public Database() {
        connect = PgConnectOptions.fromUri(System.getenv("DATABASE_URL"))
                .setCachePreparedStatements(true)
                .setPipeliningLimit(Integer.getInteger("mu.db-pipeline", 256));
        int loops = Math.max(1, Integer.getInteger("mu.event-loops", Runtime.getRuntime().availableProcessors()));
        int total = Math.clamp(Integer.getInteger("mu.db-pool", 64), 1, 512);
        poolPerLoop = Math.max(1, Math.min((total + loops - 1) / loops, 512 / loops));
        // Vert.x needs its own loop only as a fallback; the pools run on Micronaut's loops. Both sides pick
        // epoll when Netty's native epoll is available, so Vert.x's channels fit Micronaut's loops.
        vertx = (VertxInternal) Vertx.vertx(new VertxOptions().setEventLoopPoolSize(1)
                .setPreferNativeTransport(System.getProperty("os.name", "").startsWith("Linux")));
    }

    int poolPerLoop() { return poolPerLoop; }

    /** Must run on {@code loop}: the pool is created lazily, once per loop thread. */
    private Local local(EventLoop loop) {
        Local value = local.get();
        if (value != null) return value;
        ContextInternal context = vertx.contextBuilder().withEventLoop(loop).build();
        var builder = PgBuilder.client().using(vertx).connectingTo(connect)
                .with(new PoolOptions().setMaxSize(poolPerLoop).setEventLoopSize(0))
                .with(new NetClientOptions().setConnectTimeout(5_000));
        if (FLUSH_BATCH) {
            // The pg client flushes after every query. Queries are issued while this loop handles HTTP reads,
            // so deferring those flushes to the end of the loop pass sends them to Postgres in one write.
            builder.withConnectHandler(conn -> {
                var socket = (SocketConnectionBase) ((SqlConnectionInternal) conn).unwrap().unwrap();
                socket.socket().channelHandlerContext().pipeline().addFirst(new FlushConsolidationHandler(256, true));
                conn.close();
            });
        }
        SqlClient client = builder.build();
        clients.add(client);
        value = new Local(context, client);
        local.set(value);
        return value;
    }

    /** Runs a query with the loop's Vert.x context current, so the pool acquires and completes on that loop. */
    private <T> CompletableFuture<T> run(EventLoop loop, Function<SqlClient, Future<T>> query) {
        var result = new CompletableFuture<T>();
        Runnable task = () -> {
            try {
                Local l = local(loop);
                l.context().dispatch(() -> query.apply(l.client()).onComplete((value, error) -> {
                    if (error != null) result.completeExceptionally(error);
                    else result.complete(value);
                }));
            } catch (Throwable error) {
                result.completeExceptionally(error);
            }
        };
        if (loop.inEventLoop()) task.run();
        else loop.execute(task);
        return result;
    }

    CompletableFuture<Boolean> health(EventLoop loop) {
        return run(loop, c -> c.preparedQuery("SELECT 1").execute()
                .timeout(5, TimeUnit.SECONDS).map(true).otherwise(false));
    }

    CompletableFuture<List<Responses.Post>> feed(EventLoop loop) {
        return run(loop, c -> c.preparedQuery(FEED).execute()).thenApply(rows -> {
            List<Responses.Post> posts = new ArrayList<>(20);
            for (Row row : rows) posts.add(post(row));
            return posts;
        });
    }

    CompletableFuture<Responses.Post> find(EventLoop loop, long id) {
        return run(loop, c -> c.preparedQuery(FIND).execute(Tuple.of(id))).thenApply(rows -> {
            var iterator = rows.iterator();
            if (!iterator.hasNext()) throw new ApiException(404, "post not found");
            return post(iterator.next());
        });
    }

    CompletableFuture<Responses.Post> create(EventLoop loop, JwtVerifier.User user, String body) {
        return run(loop, c -> c.preparedQuery(CREATE).execute(Tuple.of(user.id(), body))).thenApply(rows -> {
            Row row = rows.iterator().next();
            return new Responses.Post(row.getLong("id"), body,
                    timestamp(row.getOffsetDateTime("created_at")), user.username(), 0);
        });
    }

    CompletableFuture<Boolean> like(EventLoop loop, long userId, long postId) {
        return run(loop, c -> c.preparedQuery(LIKE).execute(Tuple.of(userId, postId)))
                .handle((RowSet<Row> rows, Throwable error) -> {
                    Throwable cause = error instanceof CompletionException && error.getCause() != null ? error.getCause() : error;
                    if (cause instanceof PgException pg && "23503".equals(pg.getSqlState()))
                        throw new ApiException(404, "post not found");
                    if (cause != null) throw new CompletionException(cause);
                    return rows.rowCount() == 0;
                });
    }

    private static Responses.Post post(Row row) {
        return new Responses.Post(row.getLong("id"), row.getString("body"),
                timestamp(row.getOffsetDateTime("created_at")), row.getString("username"),
                row.getInteger("like_count"));
    }

    private static String timestamp(OffsetDateTime value) {
        return TIMESTAMP.format(value.toInstant().truncatedTo(ChronoUnit.MILLIS));
    }

    @PreDestroy
    @Override
    public void close() {
        List<Future<Void>> closing = new ArrayList<>();
        for (SqlClient client : clients) closing.add(client.close());
        Future.join(closing).eventually(vertx::close);
    }
}
