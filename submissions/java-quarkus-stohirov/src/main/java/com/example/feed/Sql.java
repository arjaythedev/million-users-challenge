package com.example.feed;

final class Sql {
    private static final String POST = """
            SELECT p.id, p.body, p.created_at, u.username,
                   (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
              FROM posts p JOIN users u ON u.id = p.user_id""";

    static final String FEED = POST + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
    static final String POST_BY_ID = POST + " WHERE p.id = $1";
    static final String INSERT_POST = "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at";
    static final String INSERT_LIKE = "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING";
    static final String HEALTH = "SELECT 1";

    private Sql() {
    }
}
