// The SQL (SPEC.md's statements, verbatim) and small shared helpers.
using System.Buffers;

namespace FeedServer;

static class Db
{
    const string PostSelect = """
        SELECT p.id, p.body, p.created_at, u.username,
               (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
          FROM posts p JOIN users u ON u.id = p.user_id
        """;
    public const string FeedSql = PostSelect + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
    public const string PostByIdSql = PostSelect + " WHERE p.id = $1";
    public const string InsertPostSql = "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at";
    public const string LikeSql = "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING";

    public static void Return(ref JsonWriter w)
    {
        if (w.Buf != null) ArrayPool<byte>.Shared.Return(w.Buf);
        w.Buf = null!;
    }
}

static class Log
{
    static long lastError;
    static int suppressed;

    public static void Info(string message) => Console.WriteLine(message);

    // At most one error line a second, so a database outage can't flood the journal.
    public static void Error(Exception ex)
    {
        long now = Environment.TickCount64;
        long last = Volatile.Read(ref lastError);
        if (now - last < 1000 || Interlocked.CompareExchange(ref lastError, now, last) != last)
        {
            Interlocked.Increment(ref suppressed);
            return;
        }
        int dropped = Interlocked.Exchange(ref suppressed, 0);
        Console.Error.WriteLine($"error: {ex.GetType().Name}: {ex.Message}{(dropped > 0 ? $" (+{dropped} suppressed)" : "")}");
    }
}
