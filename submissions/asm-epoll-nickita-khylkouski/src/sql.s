# sql.s: the five prepared statements. Names are one byte (NUL-terminated for pg_prepare, the same byte is used
# in Bind). Texts match the reference server, so the query plans and the ::int like_count type are identical.

.include "defs.inc"
.include "app.inc"

.section .rodata
.globl sql_feed, sql_feed_len, sql_post, sql_post_len, sql_ins, sql_ins_len, sql_like, sql_like_len
.globl sql_health, sql_health_len, st_feed, st_post, st_ins, st_like, st_health

DEFSTR sql_feed, "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20"
DEFSTR sql_post, "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1"
DEFSTR sql_ins, "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at"
DEFSTR sql_like, "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING"
DEFSTR sql_health, "SELECT 1"

st_feed:   .ascii "f\0"
st_post:   .ascii "p\0"
st_ins:    .ascii "i\0"
st_like:   .ascii "l\0"
st_health: .ascii "h\0"

.section .note.GNU-stack,"",@progbits
