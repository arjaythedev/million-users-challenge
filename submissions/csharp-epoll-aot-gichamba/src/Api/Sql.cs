using Feed.Pg;

namespace Feed.Api;

/// <summary>The prepared statements: SPEC.md's queries, one per request, as rule 3 requires.</summary>
public static class Sql {
    /// <summary>The post object query that the feed and single-post statements share.</summary>
    private const string PostSelect = """
        SELECT p.id, p.body, p.created_at, u.username,
               (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
          FROM posts p JOIN users u ON u.id = p.user_id
        """;

    /// <summary>The 20 newest posts.</summary>
    public static readonly Statement Feed = new("feed", PostSelect + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20", []);
    /// <summary>One post by id ($1).</summary>
    public static readonly Statement Post = new("post", PostSelect + " WHERE p.id = $1", [PgOid.Int8]);
    /// <summary>A health check.</summary>
    public static readonly Statement Ping = new("ping", "SELECT 1", []);
    /// <summary>A new post by user $1 with body $2; returns its id and created_at.</summary>
    public static readonly Statement Create = new("create", "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at", [PgOid.Int8, PgOid.Text]);
    /// <summary>A like by user $1 on post $2; inserts nothing on a repeat.</summary>
    public static readonly Statement Like = new("like", "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING", [PgOid.Int8, PgOid.Int8]);

    /// <summary>What the read connections prepare.</summary>
    public static readonly Statement[] Reads = [Feed, Post, Ping];
    /// <summary>What the write connections prepare.</summary>
    public static readonly Statement[] Writes = [Create, Like];

    /// <summary>Session settings of every connection. A generic plan skips re-planning the post query for each id.</summary>
    public static readonly KeyValuePair<string, string>[] Session =
    [
        new("plan_cache_mode", "force_generic_plan"),
        new("max_parallel_workers_per_gather", "0"),
    ];
}
