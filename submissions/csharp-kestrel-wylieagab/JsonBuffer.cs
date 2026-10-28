using System.Buffers;
using System.Globalization;
using System.Text.Encodings.Web;
using System.Text.Json;
using Microsoft.Extensions.ObjectPool;
using Npgsql;

namespace MillionUsers;

// Pooled working memory, never a response cache: every lease starts empty and is populated
// from this request's database result. Nothing is held on an idle HTTP connection.
internal sealed class JsonBuffer
{
    private readonly ArrayBufferWriter<byte> buffer = new(4096);
    public Utf8JsonWriter Writer { get; }
    public ReadOnlyMemory<byte> Written => buffer.WrittenMemory;
    public JsonBuffer() => Writer = new Utf8JsonWriter(buffer, new JsonWriterOptions
    {
        Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
        SkipValidation = true // Structural writes are owned by the server.
    });
    public void Reset()
    {
        buffer.Clear();
        Writer.Reset(buffer);
    }
    public void Post(NpgsqlDataReader reader) => Post(reader.GetInt64(0), reader.GetString(1),
        reader.GetDateTime(2), reader.GetString(3), reader.GetInt32(4));

    public void Post(long id, string body, DateTime created, string author, int likes)
    {
        var json = Writer;
        json.WriteStartObject();
        json.WriteNumber("id"u8, id);
        json.WriteString("body"u8, body);
        Span<char> timestamp = stackalloc char[24];
        // timestamptz arrives in UTC; fff truncates, rather than rounding, microseconds.
        created.TryFormat(timestamp, out var length, "yyyy-MM-dd'T'HH:mm:ss.fff'Z'", CultureInfo.InvariantCulture);
        json.WriteString("created_at"u8, timestamp[..length]);
        json.WriteString("author"u8, author);
        json.WriteNumber("like_count"u8, likes);
        json.WriteEndObject();
    }
    public sealed class Policy : PooledObjectPolicy<JsonBuffer>
    {
        public override JsonBuffer Create() => new();
        public override bool Return(JsonBuffer item)
        {
            var retain = item.buffer.Capacity <= 64 * 1024;
            item.Reset();
            return retain;
        }
    }
}
