// The five statements route A′'s host runs, in one place
// (docs/specs/S08-pg-effect-and-interpreter.md, docs/specs/S10-two-batches.md).
// host/pg.c prepares each once per connection, at connect, under the name in
// pg_sql[kind].name, and runs them with binary int8 parameters ($1, $2 as the
// table says), the post body as text, and the result formats below. The index
// is the Queue kind of host/pg.bend. They are SPEC.md:55-60 as written: the post
// object query (with the feed's ORDER BY and LIMIT, or the post's WHERE), the
// insert and the like. Nothing else is ever sent, and no statement text
// depends on a request.
#ifndef BM_SQL_H
#define BM_SQL_H

#include <stdint.h>

#define PG_POST_OBJECT \
  "SELECT p.id, p.body, p.created_at, u.username," \
  " (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count" \
  " FROM posts p JOIN users u ON u.id = p.user_id"

typedef struct {
  const char* name;
  const char* text;
  int         nparams;  // each an int8, but $2 of "ins" is text
} PgSql;

#define PG_KINDS 5

static const PgSql pg_sql[PG_KINDS] = {
  // 0 SelectOne -> one row: Ping
  { "s_one", "SELECT 1", 0 },
  // 1 FeedQuery -> 0 to 20 rows: Posts
  { "s_feed", PG_POST_OBJECT " ORDER BY p.created_at DESC, p.id DESC LIMIT 20", 0 },
  // 2 PostQuery{pid}: $1 pid -> one row: Post; none: NotFound
  { "s_post", PG_POST_OBJECT " WHERE p.id = $1", 1 },
  // 3 InsertPost{uid, name, text}: $1 uid, $2 text -> one row (id, created_at): Committed;
  //   an integrity error (class 23: a token whose user has no row) -> Unknown
  { "s_ins", "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at", 2 },
  // 4 InsertLike{uid, pid}: $1 uid, $2 pid -> CommandComplete "INSERT 0 1" (pg_inserted reads the count): Liked{False},
  //   "INSERT 0 0": Liked{True}; an integrity error (class 23, SPEC.md:61: 23503 on a missing post) -> NotFound
  { "s_like", "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING", 2 },
};

#define PG_OID_INT8 20
#define PG_OID_TEXT 25

// The result-format codes of a Bind (0 text, 1 binary; docs/specs/S50-the-mixed-result-format.md).
// pg_fmt_binary is one code for every column of the statement. Every statement is sent with
// it but one: with --result-format mixed the feed statement is sent with pg_fmt_feed_mixed,
// one code per column of the post object. Its id and like_count then come as decimal text,
// which is shorter than their 8 and 4 binary bytes (an id of the seed is 6 digits at the
// most, a like count is mostly one), and body, created_at and username stay binary. Five
// codes where one stood make the Bind 8 bytes longer. A feed of 20 rows repays that and a
// statement of one row would not, so the post statement stays binary. host/pg.c's pg_mixed
// picks the list for the Bind and the reader for the rows.
static const uint16_t pg_fmt_binary[1]     = { 1 };
static const uint16_t pg_fmt_feed_mixed[5] = { 0, 1, 1, 1, 0 };

#endif
