package feed.db;

import java.util.List;

import feed.pg.PgStatement;

/**
 * The SQL from SPEC.md, verbatim, prepared once per Postgres connection. Indexes into {@link #ALL} are the
 * statement numbers passed to {@code PgProtocol.bind}. Every result column comes back in binary format.
 */
public final class Queries {

    private static final String POST_OBJECT = """
            SELECT p.id, p.body, p.created_at, u.username,
                   (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
              FROM posts p JOIN users u ON u.id = p.user_id""";

    /** Post object columns: id int8, body text, created_at timestamptz, author text, like_count int4. */
    public static final int FEED = 0;
    public static final int POST = 1;
    /** Returns id int8, created_at timestamptz. Params: user_id, body. */
    public static final int INSERT_POST = 2;
    /** No rows; the command tag says 1 (new like) or 0 (already liked). Params: user_id, post_id. */
    public static final int INSERT_LIKE = 3;
    public static final int PING = 4;

    public static final List<PgStatement> ALL = List.of(
            new PgStatement("feed", POST_OBJECT + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20"),
            new PgStatement("post", POST_OBJECT + " WHERE p.id = $1", PgStatement.INT8),
            new PgStatement("insert_post", "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at",
                    PgStatement.INT8, PgStatement.TEXT),
            new PgStatement("insert_like",
                    "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING",
                    PgStatement.INT8, PgStatement.INT8),
            new PgStatement("ping", "SELECT 1"));

    private Queries() {
    }

    /** Writes run on their own connections, so reads never wait behind a commit. */
    public static boolean isWrite(int query) {
        return query == INSERT_POST || query == INSERT_LIKE;
    }
}
