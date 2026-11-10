using System.Diagnostics;
using System.Globalization;
using System.Text;
using System.Text.Json;
using Microsoft.Extensions.ObjectPool;
using Npgsql;

namespace MillionUsers;

internal sealed class Api(NpgsqlDataSource db, byte[] key, long started, int cores)
{
    private readonly ObjectPool<JsonBuffer> buffers = new DefaultObjectPool<JsonBuffer>(new JsonBuffer.Policy(), Math.Clamp(cores * 16, 32, 256));
    // Bound requests holding buffers/commands. Waiting sockets hold no database connection.
    private readonly SemaphoreSlim active = new(Math.Clamp(cores * 64, 128, 1024));

    public async Task Handle(HttpContext context)
    {
        context.Response.ContentType = "application/json";
        context.Response.Headers.Vary = "Accept-Encoding";
        JsonBuffer? buffer = null;
        var entered = false;
        try
        {
            await active.WaitAsync(context.RequestAborted);
            entered = true;
            buffer = buffers.Get();
            var request = context.Request;
            var path = request.Path.Value ?? "/";
            var status = 200;
            if (HttpMethods.IsGet(request.Method))
            {
                if (path == "/feed") await Feed(buffer, context.RequestAborted);
                else if (path == "/health") status = await Health(buffer, context.RequestAborted);
                else if (path.StartsWith("/posts/", StringComparison.Ordinal) && path.AsSpan(7).IndexOf('/') < 0)
                    status = TryId(path.AsSpan(7), out var id) ? await GetPost(buffer, id, context.RequestAborted) : Error(buffer, 400, "invalid post id");
                else status = Error(buffer, 404, "not found");
            }
            else if (HttpMethods.IsPost(request.Method) && (path == "/posts" || IsLike(path)))
            {
                var error = Auth.Validate(request.Headers.Authorization.ToString(), key, out var userId, out var username);
                if (error is not null) status = Error(buffer, 401, error);
                else if (path == "/posts") status = await CreatePost(context, buffer, userId, username!);
                else status = TryId(path.AsSpan(7, path.Length - 12), out var id)
                    ? await Like(buffer, userId, id, context.RequestAborted) : Error(buffer, 400, "invalid post id");
            }
            else status = Error(buffer, 404, "not found");

            buffer.Writer.Flush();
            context.Response.StatusCode = status;
            context.Response.ContentLength = buffer.Written.Length;
            await context.Response.Body.WriteAsync(buffer.Written, context.RequestAborted);
        }
        catch (OperationCanceledException) when (context.RequestAborted.IsCancellationRequested) { }
        catch (Exception) when (!context.Response.HasStarted)
        {
            context.Response.StatusCode = 500;
            ReadOnlyMemory<byte> body = "{\"error\":\"internal server error\"}"u8.ToArray();
            context.Response.ContentLength = body.Length;
            await context.Response.Body.WriteAsync(body, context.RequestAborted);
        }
        finally
        {
            if (buffer is not null) buffers.Return(buffer);
            if (entered) active.Release();
        }
    }

    private static bool IsLike(string path) => path.StartsWith("/posts/", StringComparison.Ordinal)
        && path.EndsWith("/like", StringComparison.Ordinal) && path.Length > 12
        && path.AsSpan(7, path.Length - 12).IndexOf('/') < 0;

    internal static bool TryId(ReadOnlySpan<char> text, out long id) =>
        long.TryParse(text, NumberStyles.None, CultureInfo.InvariantCulture, out id) && id > 0;

    private static int Error(JsonBuffer buffer, int status, string error)
    {
        buffer.Reset();
        buffer.Writer.WriteStartObject();
        buffer.Writer.WriteString("error"u8, error);
        buffer.Writer.WriteEndObject();
        return status;
    }

    private async Task Feed(JsonBuffer buffer, CancellationToken ct)
    {
        await using var command = db.CreateCommand(Database.Feed);
        await using var reader = await command.ExecuteReaderAsync(ct);
        buffer.Writer.WriteStartObject();
        buffer.Writer.WriteStartArray("posts"u8);
        while (await reader.ReadAsync(ct)) buffer.Post(reader);
        buffer.Writer.WriteEndArray();
        buffer.Writer.WriteEndObject();
    }

    private async Task<int> GetPost(JsonBuffer buffer, long id, CancellationToken ct)
    {
        await using var command = db.CreateCommand(Database.Post);
        command.Parameters.Add(new NpgsqlParameter<long> { TypedValue = id });
        await using var reader = await command.ExecuteReaderAsync(ct);
        if (!await reader.ReadAsync(ct)) return Error(buffer, 404, "post not found");
        buffer.Writer.WriteStartObject();
        buffer.Writer.WritePropertyName("post"u8);
        buffer.Post(reader);
        buffer.Writer.WriteEndObject();
        return 200;
    }

    private async Task<int> Health(JsonBuffer buffer, CancellationToken ct)
    {
        try
        {
            await using var command = db.CreateCommand("SELECT 1");
            await command.ExecuteScalarAsync(ct);
            buffer.Writer.WriteStartObject();
            buffer.Writer.WriteString("status"u8, "ok"u8);
            buffer.Writer.WriteString("db"u8, "ok"u8);
            buffer.Writer.WriteNumber("uptime_s"u8, (long)Stopwatch.GetElapsedTime(started).TotalSeconds);
            buffer.Writer.WriteEndObject();
            return 200;
        }
        catch (Exception) when (!ct.IsCancellationRequested)
        {
            buffer.Reset();
            buffer.Writer.WriteStartObject();
            buffer.Writer.WriteString("status"u8, "degraded"u8);
            buffer.Writer.WriteString("db"u8, "unreachable"u8);
            buffer.Writer.WriteString("error"u8, "database unavailable"u8);
            buffer.Writer.WriteEndObject();
            return 503;
        }
    }

    private async Task<int> CreatePost(HttpContext context, JsonBuffer buffer, long userId, string username)
    {
        string body;
        try
        {
            using var document = await JsonDocument.ParseAsync(context.Request.Body,
                new JsonDocumentOptions { MaxDepth = 32 }, context.RequestAborted);
            body = document.RootElement.ValueKind == JsonValueKind.Object
                && document.RootElement.TryGetProperty("body", out var value) && value.ValueKind == JsonValueKind.String
                ? value.GetString()!.Trim() : "";
        }
        catch (JsonException) { return Error(buffer, 400, "malformed JSON body"); }
        if (body.Length == 0) return Error(buffer, 400, "body is required");
        // PostgreSQL char_length counts Unicode scalar values, not UTF-16 code units.
        if (body.EnumerateRunes().Count() > 500) return Error(buffer, 400, "body must be at most 500 characters");

        long id;
        DateTime created;
        await using (var command = db.CreateCommand(Database.Insert))
        {
            command.Parameters.Add(new NpgsqlParameter<long> { TypedValue = userId });
            command.Parameters.Add(new NpgsqlParameter<string> { TypedValue = body });
            await using var reader = await command.ExecuteReaderAsync(context.RequestAborted);
            if (!await reader.ReadAsync(context.RequestAborted)) throw new InvalidOperationException("Missing INSERT result");
            id = reader.GetInt64(0);
            created = reader.GetDateTime(1);
            // Disposal drains CommandComplete/ReadyForQuery: RETURNING alone does not prove commit.
        }
        buffer.Writer.WriteStartObject();
        buffer.Writer.WritePropertyName("post"u8);
        buffer.Post(id, body, created, username, 0);
        buffer.Writer.WriteEndObject();
        return 201;
    }

    private async Task<int> Like(JsonBuffer buffer, long userId, long postId, CancellationToken ct)
    {
        await using var command = db.CreateCommand(Database.Like);
        command.Parameters.Add(new NpgsqlParameter<long> { TypedValue = userId });
        command.Parameters.Add(new NpgsqlParameter<long> { TypedValue = postId });
        int inserted;
        try { inserted = await command.ExecuteNonQueryAsync(ct); }
        catch (PostgresException ex) when (ex.SqlState == PostgresErrorCodes.ForeignKeyViolation)
        { return Error(buffer, 404, "post not found"); }
        buffer.Writer.WriteStartObject();
        buffer.Writer.WriteBoolean("liked"u8, true);
        buffer.Writer.WriteBoolean("already_liked"u8, inserted == 0);
        buffer.Writer.WriteNumber("post_id"u8, postId);
        buffer.Writer.WriteEndObject();
        return inserted > 0 ? 201 : 200;
    }
}
