"""The spec's SQL, verbatim, shared by every Postgres adapter (rule 3: one of these per request)."""

POST_SELECT = """SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
  FROM posts p JOIN users u ON u.id = p.user_id"""

FEED = POST_SELECT + " ORDER BY p.created_at DESC, p.id DESC LIMIT $1"
POST_BY_ID = POST_SELECT + " WHERE p.id = $1"
INSERT_POST = "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at"
INSERT_LIKE = "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING"
PING = "SELECT 1"

FOREIGN_KEY_VIOLATION = "23503"  # a like on a post that does not exist
