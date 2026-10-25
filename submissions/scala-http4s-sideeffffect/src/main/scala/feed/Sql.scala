package feed

import skunk.*
import skunk.codec.all.*
import skunk.implicits.*

/** The SQL from SPEC.md, verbatim, as skunk statements. Skunk prepares a statement the first time a session runs it and
  * after that sends only Bind/Execute. That caches the plan, not data (rule 7).
  */
object Sql:

  private val select: Fragment[Void] =
    sql"""SELECT p.id, p.body, p.created_at, u.username,
                 (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
            FROM posts p JOIN users u ON u.id = p.user_id"""

  private val post: Decoder[Post] =
    (int8 *: text *: timestamptz *: text *: int4).map { case (id, body, createdAt, author, likes) =>
      Post(id, body, Timestamp(createdAt.toInstant), author, likes)
    }

  /** The 20 newest posts. */
  val feed: Query[Void, Post] =
    sql"$select ORDER BY p.created_at DESC, p.id DESC LIMIT 20".query(post)

  val postById: Query[Long, Post] =
    sql"$select WHERE p.id = $int8".query(post)

  /** `(user_id, body)` to the new post's `(id, created_at)`. */
  val insertPost: Query[(Long, String), (Long, Timestamp)] =
    sql"INSERT INTO posts (user_id, body) VALUES ($int8, $text) RETURNING id, created_at"
      .query(int8 *: timestamptz)
      .map((id, createdAt) => (id, Timestamp(createdAt.toInstant)))

  /** `(user_id, post_id)`; inserts one row, or none if the like exists. A missing post fails with SQLSTATE 23503. */
  val insertLike: Command[(Long, Long)] =
    sql"INSERT INTO likes (user_id, post_id) VALUES ($int8, $int8) ON CONFLICT (user_id, post_id) DO NOTHING".command

  val ping: Query[Void, Int] = sql"SELECT 1".query(int4)
