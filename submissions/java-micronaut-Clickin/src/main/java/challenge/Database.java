package challenge;

import io.vertx.core.Vertx;
import io.vertx.core.VertxOptions;
import io.vertx.core.net.NetClientOptions;
import io.vertx.pgclient.PgBuilder;
import io.vertx.pgclient.PgConnectOptions;
import io.vertx.pgclient.PgException;
import io.vertx.sqlclient.PoolOptions;
import io.vertx.sqlclient.Row;
import io.vertx.sqlclient.SqlClient;
import io.vertx.sqlclient.Tuple;
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
import java.util.concurrent.TimeUnit;

@Singleton
public final class Database implements AutoCloseable {
    private static final String POST_QUERY = """
            SELECT p.id, p.body, p.created_at, u.username,
                   (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
              FROM posts p JOIN users u ON u.id = p.user_id
            """;
    private static final DateTimeFormatter TIMESTAMP = new DateTimeFormatterBuilder().appendInstant(3).toFormatter();
    private final Vertx vertx;
    private final SqlClient client;

    public Database() {
        var options = PgConnectOptions.fromUri(System.getenv("DATABASE_URL"))
                .setCachePreparedStatements(true)
                .setPipeliningLimit(Integer.getInteger("mu.db-pipeline", 256));
        var pool = new PoolOptions().setMaxSize(Math.clamp(Integer.getInteger("mu.db-pool", 64), 1, 512));
        vertx = Vertx.vertx(new VertxOptions().setPreferNativeTransport(true)
                .setEventLoopPoolSize(Integer.getInteger("mu.db-loops", Math.max(1, Runtime.getRuntime().availableProcessors() / 4))));
        client = PgBuilder.client().using(vertx).connectingTo(options).with(pool)
                .with(new NetClientOptions().setConnectTimeout(5_000)).build();
    }

    CompletableFuture<Boolean> health() {
        return client.preparedQuery("SELECT 1").execute()
                .timeout(5, TimeUnit.SECONDS).map(true).otherwise(false).toCompletionStage().toCompletableFuture();
    }

    CompletableFuture<List<Responses.Post>> feed() {
        return client.preparedQuery(POST_QUERY + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20").execute()
                .toCompletionStage().toCompletableFuture().thenApply(rows -> {
                    List<Responses.Post> posts = new ArrayList<>(20);
                    for (Row row : rows) posts.add(post(row));
                    return posts;
                });
    }

    CompletableFuture<Responses.Post> find(long id) {
        return client.preparedQuery(POST_QUERY + " WHERE p.id = $1").execute(Tuple.of(id))
                .toCompletionStage().toCompletableFuture().thenApply(rows -> {
                    var iterator = rows.iterator();
                    if (!iterator.hasNext()) throw new ApiException(404, "post not found");
                    return post(iterator.next());
                });
    }

    CompletableFuture<Responses.Post> create(JwtVerifier.User user, String body) {
        return client.preparedQuery("INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at")
                .execute(Tuple.of(user.id(), body)).toCompletionStage().toCompletableFuture().thenApply(rows -> {
                    Row row = rows.iterator().next();
                    return new Responses.Post(row.getLong("id"), body,
                            timestamp(row.getOffsetDateTime("created_at")), user.username(), 0);
                });
    }

    CompletableFuture<Boolean> like(long userId, long postId) {
        return client.preparedQuery("INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING")
                .execute(Tuple.of(userId, postId)).toCompletionStage().toCompletableFuture().handle((rows, error) -> {
                    if (error instanceof PgException pg && "23503".equals(pg.getSqlState()))
                        throw new ApiException(404, "post not found");
                    if (error != null) throw new CompletionException(error);
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
        client.close().eventually(vertx::close);
    }
}
