using Npgsql;

namespace MillionUsers;

internal static class Database
{
    // Exactly the challenge SQL. No JSON, result caches, schema changes, or extra request queries.
    public const string Select = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id";
    public const string Feed = Select + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
    public const string Post = Select + " WHERE p.id = $1";
    public const string Insert = "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at";
    public const string Like = "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING";
    public static int PoolSize(int cores) => Math.Clamp(cores * 16, 32, 256);

    public static NpgsqlDataSource Create(string url, int cores)
    {
        var uri = new Uri(url);
        if (uri.Scheme is not ("postgres" or "postgresql"))
            throw new ArgumentException("DATABASE_URL must use postgres:// or postgresql://");
        var credentials = uri.UserInfo.Split(':', 2);
        var settings = new NpgsqlConnectionStringBuilder
        {
            Host = uri.Host,
            Port = uri.Port > 0 ? uri.Port : 5432,
            Username = Uri.UnescapeDataString(credentials[0]),
            Password = credentials.Length == 2 ? Uri.UnescapeDataString(credentials[1]) : null,
            Database = Uri.UnescapeDataString(uri.AbsolutePath.TrimStart('/')),
            ClientEncoding = "UTF8", // GetBytes forwards text directly to the UTF-8 JSON writer.
            MaxPoolSize = PoolSize(cores),
            MaxAutoPrepare = 8,
            AutoPrepareMinUsages = 2,
            Enlist = false,
            NoResetOnClose = true, // The app never changes session state.
            Timeout = 5,
            CommandTimeout = 10,
            ConnectionLifetime = 0,
            GssEncryptionMode = GssEncryptionMode.Disable,
            ApplicationName = "csharp-kestrel-wylieagab"
        };
        var builder = new NpgsqlSlimDataSourceBuilder(settings.ConnectionString);
        builder.EnableTransportSecurity();
        return builder.Build();
    }
}
