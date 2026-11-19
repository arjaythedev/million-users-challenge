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
    private byte[] text = new byte[2048];
    private int textUsed;
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
        text.AsSpan(0, textUsed).Clear();
        textUsed = 0;
        Writer.Reset(buffer);
    }
    public void Post(NpgsqlDataReader reader)
    {
        var json = Writer;
        json.WriteStartObject();
        json.WriteNumber("id"u8, reader.GetInt64(0));
        WriteText(reader, 1, "body"u8);
        WriteTimestamp(reader.GetDateTime(2));
        WriteText(reader, 3, "author"u8);
        json.WriteNumber("like_count"u8, reader.GetInt32(4));
        json.WriteEndObject();
    }

    public void Post(long id, string body, DateTime created, string author, int likes)
    {
        var json = Writer;
        json.WriteStartObject();
        json.WriteNumber("id"u8, id);
        json.WriteString("body"u8, body);
        WriteTimestamp(created);
        json.WriteString("author"u8, author);
        json.WriteNumber("like_count"u8, likes);
        json.WriteEndObject();
    }

    private void WriteText(NpgsqlDataReader reader, int ordinal, ReadOnlySpan<byte> name)
    {
        // GetBytes exposes the TEXT field's wire bytes; the connection must use UTF8.
        var length = checked((int)reader.GetBytes(ordinal, 0, null, 0, 0));
        if (text.Length < length)
        {
            text.AsSpan(0, textUsed).Clear();
            textUsed = 0;
            text = new byte[length];
        }
        // Record the full destination before reading so Reset also clears a failed read.
        textUsed = Math.Max(textUsed, length);
        var read = reader.GetBytes(ordinal, 0, text, 0, length);
        if (read != length)
            throw new InvalidDataException("PostgreSQL text field was not read completely.");
        Writer.WriteString(name, text.AsSpan(0, length));
    }

    private void WriteTimestamp(DateTime created)
    {
        Span<char> timestamp = stackalloc char[24];
        // timestamptz arrives in UTC; fff truncates, rather than rounding, microseconds.
        created.TryFormat(timestamp, out var length, "yyyy-MM-dd'T'HH:mm:ss.fff'Z'", CultureInfo.InvariantCulture);
        Writer.WriteString("created_at"u8, timestamp[..length]);
    }

    public sealed class Policy : PooledObjectPolicy<JsonBuffer>
    {
        public override JsonBuffer Create() => new();
        public override bool Return(JsonBuffer item)
        {
            var retain = item.buffer.Capacity <= 64 * 1024 && item.text.Length <= 64 * 1024;
            item.Reset();
            return retain;
        }
    }
}
