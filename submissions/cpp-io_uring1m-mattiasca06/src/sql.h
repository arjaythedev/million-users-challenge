// Statements, prepared once per connection. SQL follows schema.sql and SPEC.md.
#pragma once

struct Stmt {
    const char *name, *sql;
};

// One query per request. Result columns are fixed per statement (binary format): the post statements all return
// id (int8), body (text), created_at (timestamptz), author (text), like_count (int8) in that order.
#define POST_COLS "p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)"

inline constexpr Stmt kStatements[] = {
    {"ping", "SELECT 1"},
    {"feed",
     "SELECT " POST_COLS " FROM posts p JOIN users u ON u.id = p.user_id "
     "ORDER BY p.created_at DESC, p.id DESC LIMIT 20"},
    {"post", "SELECT " POST_COLS " FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1::bigint"},
    {"post_insert", "INSERT INTO posts (user_id, body) VALUES ($1::bigint, $2::text) RETURNING id, created_at"},
    // One row (the 1) when the like is new, none when it already existed. A missing post is a foreign key error (23503).
    {"like_insert",
     "INSERT INTO likes (user_id, post_id) VALUES ($1::bigint, $2::bigint) ON CONFLICT (user_id, post_id) DO NOTHING "
     "RETURNING 1"},
};
