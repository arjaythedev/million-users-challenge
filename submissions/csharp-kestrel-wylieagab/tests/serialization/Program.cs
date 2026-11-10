using System.Buffers;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;
using Microsoft.Extensions.ObjectPool;
using MillionUsers;
using Npgsql;

// Only parameter-only SELECT statements are issued. No application schema or seed
// is required; this runs against any existing PostgreSQL database with UTF8 support.
string connectionString = Environment.GetEnvironmentVariable("SERIALIZATION_DATABASE")
    ?? ConnectionStringFromUrl(Environment.GetEnvironmentVariable("TEST_DATABASE_URL"));
var settings = new NpgsqlConnectionStringBuilder(connectionString)
{
    ApplicationName = "million-users-serialization-tests",
    ClientEncoding = "UTF8",
    MaxPoolSize = 1,
    Enlist = false,
    GssEncryptionMode = GssEncryptionMode.Disable,
    Timeout = 5,
    CommandTimeout = 10
};
var builder = new NpgsqlSlimDataSourceBuilder(settings.ConnectionString);
builder.EnableTransportSecurity();
using var dataSource = builder.Build();
using var connection = dataSource.OpenConnection();
using var command = new NpgsqlCommand("SELECT $1::bigint, $2::text, $3::timestamptz, $4::text, $5::int", connection);
const long id = 23;
const int likes = 3;
DateTime timestamp = new DateTime(2025, 12, 31, 23, 59, 19, DateTimeKind.Utc).AddTicks(4099990);
command.Parameters.Add(new NpgsqlParameter<long> { TypedValue = id });
var bodyParameter = new NpgsqlParameter<string>(); command.Parameters.Add(bodyParameter);
command.Parameters.Add(new NpgsqlParameter<DateTime> { TypedValue = timestamp });
var authorParameter = new NpgsqlParameter<string>(); command.Parameters.Add(authorParameter);
command.Parameters.Add(new NpgsqlParameter<int> { TypedValue = likes });

var cases = new (string Name, string Body, string Author, bool MustRetain, bool MustReject)[]
{
    ("ASCII and optional HTML escapes", "plain ASCII <>&/", "user_1", true, false),
    ("empty text fields", "", "", true, false),
    ("500 ASCII scalars", new string('a', 500), "author", true, false),
    ("500 astral scalars", string.Concat(Enumerable.Repeat("😀", 500)), "日本語 😀 café", true, false),
    ("500 BMP scalars", new string('界', 500), "Δοκιμή", true, false),
    ("mixed 500-scalar boundary", new string('b', 499) + "😀", "mix", true, false),
    ("JSON quoting and controls", "quote \" slash \\ tab\t newline\n return\r backspace\b formfeed\f control\u0001", "a\"b\\c\n", true, false),
    ("Unicode separators and combining characters", "\u2028\u2029\u0085 e\u0301 café", "\u2028\u2029", true, false),
    ("author below initial scratch boundary", "short", new string('x', 2047), true, false),
    ("author at initial scratch boundary", "short", new string('x', 2048), true, false),
    ("author above initial scratch boundary", "short", new string('x', 2049), true, false),
    ("large astral author", "short", string.Concat(Enumerable.Repeat("😀", 5000)), false, false),
    ("surrogate crossing a UTF16 boundary", "short", new string('z', 1023) + "😀" + new string('z', 1024), true, false),
    ("short text following long text", "x", "q", true, false),
    ("500 two-byte scalars", new string('é', 500), "", true, false),
    ("escaped author following empty author", "final \"quoted\" body", "café\n\t\\\"", true, false),
    ("author exceeds 64 KiB retention limit", "oversized-author-check", new string('界', 22000) + "😀", false, true),
    ("new buffer after oversized rejection", "fresh", "small", true, false)
};

var policy = new JsonBuffer.Policy();
var pool = new DefaultObjectPool<JsonBuffer>(policy, maximumRetained: 1);
var buffer = pool.Get();
int passed = 0;
int reused = 0;
try
{
    foreach (var example in cases)
    {
        bodyParameter.TypedValue = example.Body;
        authorParameter.TypedValue = example.Author;
        // No explicit reset: a new or returned instance must be ready for a document.
        buffer.Writer.WriteStartObject();
        buffer.Writer.WritePropertyName("post"u8);
        using (var reader = command.ExecuteReader())
        {
            if (!reader.Read()) throw new InvalidOperationException("Missing parameter-only SELECT row");
            buffer.Post(reader);
            if (reader.Read()) throw new InvalidOperationException("Unexpected extra SELECT row");
        }
        buffer.Writer.WriteEndObject();
        buffer.Writer.Flush();

        byte[] expected = ExpectedJson(example.Body, example.Author);
        if (!buffer.Written.Span.SequenceEqual(expected))
            throw new InvalidOperationException($"Exact JSON mismatch: {example.Name}");
        using (var document = JsonDocument.Parse(buffer.Written))
        {
            var post = document.RootElement.GetProperty("post");
            string[] names = post.EnumerateObject().Select(property => property.Name).ToArray();
            if (!names.SequenceEqual(new[] { "id", "body", "created_at", "author", "like_count" }) ||
                post.GetProperty("id").GetInt64() != id ||
                post.GetProperty("body").GetString() != example.Body ||
                post.GetProperty("created_at").GetString() != "2025-12-31T23:59:19.409Z" ||
                post.GetProperty("author").GetString() != example.Author ||
                post.GetProperty("like_count").GetInt32() != likes)
                throw new InvalidOperationException($"Round-trip, field-order, or timestamp mismatch: {example.Name}");
        }

        ReadOnlyMemory<byte> previous = buffer.Written;
        if (example.MustReject)
        {
            if (Encoding.UTF8.GetByteCount(example.Author) <= 64 * 1024)
                throw new InvalidOperationException("Oversized fixture does not cross the 64 KiB boundary");
            if (policy.Return(buffer))
                throw new InvalidOperationException("Policy.Return retained an oversized instance");
            AssertCleared(example.Name, previous, buffer.Written);
        }
        pool.Return(buffer);
        AssertCleared(example.Name, previous, buffer.Written);
        var next = pool.Get();
        bool same = ReferenceEquals(buffer, next);
        if ((example.MustRetain && !same) || (example.MustReject && same))
            throw new InvalidOperationException($"Unexpected pool retention: {example.Name}");
        if (same) reused++;
        else buffer.Writer.Dispose();
        buffer = next;
        passed++;
    }
}
finally { buffer.Writer.Dispose(); }
Console.WriteLine($"Serialization checks passed: {passed} parameter-only SELECT cases; {reused} same-instance pool reuse cycles; oversized return policy and cleared JSON memory verified.");

static byte[] ExpectedJson(string body, string author)
{
    // Independent expected representation: original strings and an explicit timestamp,
    // without the production reader/serializer or a duplicate implementation.
    var output = new ArrayBufferWriter<byte>();
    using var writer = new Utf8JsonWriter(output, new JsonWriterOptions
    {
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping
    });
    writer.WriteStartObject();
    writer.WriteStartObject("post");
    writer.WriteNumber("id", 23);
    writer.WriteString("body", body);
    writer.WriteString("created_at", "2025-12-31T23:59:19.409Z");
    writer.WriteString("author", author);
    writer.WriteNumber("like_count", 3);
    writer.WriteEndObject();
    writer.WriteEndObject();
    writer.Flush();
    return output.WrittenSpan.ToArray();
}

static void AssertCleared(string label, ReadOnlyMemory<byte> previous, ReadOnlyMemory<byte> current)
{
    if (!current.IsEmpty || previous.Span.IndexOfAnyExcept((byte)0) >= 0)
        throw new InvalidOperationException($"Return did not clear Written and previously used JSON bytes: {label}");
}

static string ConnectionStringFromUrl(string? url)
{
    if (string.IsNullOrEmpty(url))
        return "Host=127.0.0.1;Port=55432;Username=postgres;Database=feed_perf_20261029";
    var uri = new Uri(url);
    if (uri.Scheme is not ("postgres" or "postgresql"))
        throw new ArgumentException("TEST_DATABASE_URL must use postgres:// or postgresql://; SERIALIZATION_DATABASE accepts a full Npgsql connection string.");
    if (uri.Query.Length != 0)
        throw new ArgumentException("Use SERIALIZATION_DATABASE for connection options; TEST_DATABASE_URL supports user, password, host, port, and database only.");
    var credentials = uri.UserInfo.Split(':', 2);
    return new NpgsqlConnectionStringBuilder
    {
        Host = uri.Host,
        Port = uri.Port > 0 ? uri.Port : 5432,
        Username = Uri.UnescapeDataString(credentials[0]),
        Password = credentials.Length == 2 ? Uri.UnescapeDataString(credentials[1]) : null,
        Database = Uri.UnescapeDataString(uri.AbsolutePath.TrimStart('/'))
    }.ConnectionString;
}
