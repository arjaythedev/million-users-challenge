//! The statements of SPEC.md, verbatim, prepared once per connection. One statement per request.
const pg = @import("pg.zig");

pub const Stmt = enum(u16) { health, feed, post, new_post, like };

pub const Def = struct { id: u16, sql: []const u8, params: []const u32 };

const post_select =
    "SELECT p.id, p.body, p.created_at, u.username, " ++
    "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count " ++
    "FROM posts p JOIN users u ON u.id = p.user_id";

pub const prepared = [_]Def{
    .{ .id = @backingInt(Stmt.health), .sql = "SELECT 1", .params = &.{} },
    .{ .id = @backingInt(Stmt.feed), .sql = post_select ++ " ORDER BY p.created_at DESC, p.id DESC LIMIT 20", .params = &.{} },
    .{ .id = @backingInt(Stmt.post), .sql = post_select ++ " WHERE p.id = $1", .params = &.{pg.oid.int8} },
    .{ .id = @backingInt(Stmt.new_post), .sql = "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at", .params = &.{ pg.oid.int8, pg.oid.text } },
    .{ .id = @backingInt(Stmt.like), .sql = "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING", .params = &.{ pg.oid.int8, pg.oid.int8 } },
};

pub inline fn id(s: Stmt) u16 {
    return @backingInt(s);
}
