package com.example.feed.post.persistence;

import com.example.feed.post.model.Post;
import com.example.feed.auth.AuthUser;
import jakarta.enterprise.context.ApplicationScoped;
import io.smallrye.mutiny.Uni;
import io.vertx.mutiny.sqlclient.SqlClient;
import io.vertx.mutiny.sqlclient.Row;
import io.vertx.mutiny.sqlclient.Tuple;
import java.util.ArrayList;
import java.util.List;

/** Specification queries, reactive PostgreSQL autocommit, no cached application data. */
@ApplicationScoped
public class PostRepository {
    private static final String SELECT = """
            SELECT p.id, p.body, p.created_at, u.username,
                   (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
              FROM posts p JOIN users u ON u.id = p.user_id
            """;
    private static final String FEED_SQL = SELECT + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
    private static final String FIND_SQL = SELECT + " WHERE p.id = $1";
    private static final String CREATE_SQL = """
            INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at
            """;
    private static final String LIKE_SQL = """
            INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING
            """;

    private final SqlClient pool;

    public PostRepository(SqlClient pool) {
        this.pool = pool;
    }

    public Uni<List<Post>> feed() {
        return pool.preparedQuery(FEED_SQL).execute().map(rows -> {
            List<Post> posts = new ArrayList<>(20);
            for (Row row : rows) {
                posts.add(read(row));
            }
            return posts;
        });
    }

    public Uni<Post> get(long id) {
        return pool.preparedQuery(FIND_SQL).execute(Tuple.of(id)).map(rows -> {
            var iterator = rows.iterator();
            return iterator.hasNext() ? read(iterator.next()) : null;
        });
    }

    private static Post read(Row row) {
        return new Post(row.getLong(0), row.getString(1), row.getOffsetDateTime(2).toInstant(),
                row.getString(3), row.getInteger(4));
    }

    public Uni<Post> create(AuthUser user, String body) {
        // Query completion follows ReadyForQuery, after the autocommit transaction finishes.
        // Never replay a failed write: the server may have committed before disconnecting.
        return pool.preparedQuery(CREATE_SQL).execute(Tuple.of(user.id(), body)).map(rows -> {
            var row = rows.iterator().next();
            return new Post(row.getLong(0), body, row.getOffsetDateTime(1).toInstant(), user.username(), 0);
        });
    }

    public Uni<Boolean> like(long userId, long postId) {
        return pool.preparedQuery(LIKE_SQL).execute(Tuple.of(userId, postId))
                .map(rows -> rows.rowCount() == 1);
    }
}
