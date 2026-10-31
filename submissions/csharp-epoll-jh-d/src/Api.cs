// Routes and handlers. Routing, auth and input validation run on the loop thread; a request that passes them
// becomes exactly one SPEC.md statement on one of the loop's Postgres connections, and the OnRow / OnComplete /
// OnError callbacks below turn its results into the response body.
using System.Buffers.Text;
using System.Diagnostics;
using System.Text.Json;

namespace FeedServer;

static class Api
{
    static readonly Stopwatch Uptime = Stopwatch.StartNew();

    public static void Dispatch(EventLoop loop, int fd, uint gen, in Request req)
    {
        var path = req.Path;
        if (req.Method == Method.Get)
        {
            if (path.SequenceEqual("/feed"u8)) { loop.Submit(loop.Rent(Stmt.Feed, fd, gen, req)); return; }
            if (path.StartsWith("/posts/"u8) && path[7..].IndexOf((byte)'/') < 0)
            {
                if (!TryParseId(path[7..], out long id)) { Error(loop, fd, req, 400, "invalid post id"u8); return; }
                var e = loop.Rent(Stmt.PostById, fd, gen, req);
                e.A = id;
                loop.Submit(e);
                return;
            }
            if (path.SequenceEqual("/health"u8)) { loop.Submit(loop.Rent(Stmt.Health, fd, gen, req)); return; }
        }
        else if (req.Method == Method.Post)
        {
            if (path.SequenceEqual("/posts"u8)) { CreatePost(loop, fd, gen, req); return; }
            if (path.StartsWith("/posts/"u8) && path.EndsWith("/like"u8) && path.Length > 12
                && path[7..^5].IndexOf((byte)'/') < 0)
            {
                var auth = Jwt.Verify(loop.Hmac, req, out var user);
                if (auth != AuthResult.Ok) { Error(loop, fd, req, 401, Jwt.Message(auth)); return; }
                if (!TryParseId(path[7..^5], out long id)) { Error(loop, fd, req, 400, "invalid post id"u8); return; }
                var e = loop.Rent(Stmt.Like, fd, gen, req);
                e.A = user.Id;
                e.B = id;
                loop.Submit(e);
                return;
            }
        }
        Error(loop, fd, req, 404, "not found"u8);
    }

    static void Error(EventLoop loop, int fd, in Request req, int status, ReadOnlySpan<byte> message) =>
        loop.RespondError(fd, status, message, req.KeepAlive);

    // A post id in a path: 1 to 18 ASCII digits, not 0 ("0", "-1", "abc", "1.5" are 400).
    static bool TryParseId(ReadOnlySpan<byte> digits, out long id)
    {
        id = 0;
        if (digits.Length is 0 or > 18) return false;
        foreach (byte d in digits)
        {
            if (d - (uint)'0' > 9) { id = 0; return false; }
            id = id * 10 + (d - '0');
        }
        return id > 0;
    }

    static void CreatePost(EventLoop loop, int fd, uint gen, in Request req)
    {
        var auth = Jwt.Verify(loop.Hmac, req, out var user);
        if (auth != AuthResult.Ok) { Error(loop, fd, req, 401, Jwt.Message(auth)); return; }
        if (!TryReadPostBody(req.Body, out string body)) { Error(loop, fd, req, 400, "malformed JSON body"u8); return; }
        if (body.Length == 0) { Error(loop, fd, req, 400, "body is required"u8); return; }
        if (CodePoints(body) > 500) { Error(loop, fd, req, 400, "body must be at most 500 characters"u8); return; }
        var e = loop.Rent(Stmt.InsertPost, fd, gen, req);
        e.A = user.Id;
        e.Text = body;
        e.Username = user.Username;
        loop.Submit(e);
    }

    // Reads the whole request body as JSON (false: it isn't valid JSON) and returns the top-level "body" member,
    // trimmed, or "" when there is none or it isn't a string.
    static bool TryReadPostBody(ReadOnlySpan<byte> json, out string body)
    {
        body = "";
        var reader = new Utf8JsonReader(json);
        bool sawToken = false, inObject = false;
        try
        {
            while (reader.Read())
            {
                sawToken = true;
                if (reader.CurrentDepth == 0) { inObject |= reader.TokenType == JsonTokenType.StartObject; continue; }
                if (!inObject || reader.CurrentDepth != 1 || reader.TokenType != JsonTokenType.PropertyName
                    || !reader.ValueTextEquals("body"u8)) continue;
                reader.Read();
                body = reader.TokenType == JsonTokenType.String ? reader.GetString()!.Trim() : "";
                if (reader.TokenType is JsonTokenType.StartObject or JsonTokenType.StartArray) reader.Skip();
            }
        }
        catch (Exception ex) when (ex is JsonException or InvalidOperationException) { return false; }
        return sawToken;
    }

    // Postgres's char_length counts code points, not UTF-16 units.
    static int CodePoints(string s)
    {
        int n = s.Length;
        foreach (char c in s) if (char.IsLowSurrogate(c)) n--;
        return n;
    }

    // ---- results (binary columns, in SELECT order) ----

    public static void OnRow(PgEntry e, ReadOnlySpan<byte> row)
    {
        var r = new PgRow(row);
        switch (e.Stmt)
        {
            case Stmt.Feed:
                if (e.Rows++ == 0) { e.W = JsonWriter.Create(8192); e.W.Raw("{\"posts\":["u8); }
                else e.W.Raw(","u8);
                WritePost(ref e.W, ref r);
                break;
            case Stmt.PostById:
                e.Rows++;
                e.W = JsonWriter.Create(2048);
                e.W.Raw("{\"post\":"u8);
                WritePost(ref e.W, ref r);
                e.W.Raw("}"u8);
                break;
            case Stmt.InsertPost: // RETURNING id, created_at
                e.Rows++;
                e.W = JsonWriter.Create(2048);
                e.W.Raw("{\"post\":{\"id\":"u8); e.W.Long(r.Int64());
                e.W.Raw(",\"body\":"u8); e.W.String(e.Text!);
                e.W.Raw(",\"created_at\":"u8); e.W.Timestamp(r.Timestamp());
                e.W.Raw(",\"author\":"u8); e.W.Utf8String(e.Username);
                e.W.Raw(",\"like_count\":0}}"u8);
                break;
        }
    }

    static void WritePost(ref JsonWriter w, ref PgRow r)
    {
        w.Raw("{\"id\":"u8); w.Long(r.Int64());
        w.Raw(",\"body\":"u8); w.Utf8String(r.Bytes());
        w.Raw(",\"created_at\":"u8); w.Timestamp(r.Timestamp());
        w.Raw(",\"author\":"u8); w.Utf8String(r.Bytes());
        w.Raw(",\"like_count\":"u8); w.Long(r.Int32());
        w.Raw("}"u8);
    }

    // CommandComplete: tag is e.g. "SELECT 20" or "INSERT 0 1".
    public static void OnComplete(PgEntry e, ReadOnlySpan<byte> tag)
    {
        switch (e.Stmt)
        {
            case Stmt.Feed:
                if (e.Rows == 0) { e.W = JsonWriter.Create(64); e.W.Raw("{\"posts\":["u8); }
                e.W.Raw("]}"u8);
                e.Status = 200;
                break;
            case Stmt.PostById:
                if (e.Rows == 0) SetError(e, 404, "post not found"u8);
                else e.Status = 200;
                break;
            case Stmt.InsertPost:
                if (e.Rows == 0) SetError(e, 500, "internal server error"u8);
                else e.Status = 201;
                break;
            case Stmt.Like:
            {
                tag = tag.TrimEnd((byte)0);
                bool inserted = tag.Length > 0 && tag[^1] != '0';
                e.W = JsonWriter.Create(256);
                e.W.Raw("{\"liked\":true,\"already_liked\":"u8); e.W.Bool(!inserted);
                e.W.Raw(",\"post_id\":"u8); e.W.Long(e.B);
                e.W.Raw("}"u8);
                e.Status = inserted ? 201 : 200;
                break;
            }
            case Stmt.Health:
                e.W = JsonWriter.Create(128);
                e.W.Raw("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":"u8);
                e.W.Long((long)Uptime.Elapsed.TotalSeconds);
                e.W.Raw("}"u8);
                e.Status = 200;
                break;
        }
    }

    public static void OnError(PgEntry e, ReadOnlySpan<byte> sqlState, ReadOnlySpan<byte> message)
    {
        e.ResetResult();
        if (e.Stmt == Stmt.Like && sqlState.SequenceEqual("23503"u8)) // foreign key: no such post
        {
            SetError(e, 404, "post not found"u8);
            return;
        }
        if (e.Stmt == Stmt.Health) { Degraded(e, System.Text.Encoding.UTF8.GetString(message)); return; }
        Log.Error(new IOException($"Postgres {System.Text.Encoding.ASCII.GetString(sqlState)}: {System.Text.Encoding.UTF8.GetString(message)}"));
        SetError(e, 500, "internal server error"u8);
    }

    // No live Postgres connection for this request, or it was lost with the request in flight.
    public static void OnUnavailable(PgEntry e)
    {
        e.ResetResult();
        if (e.Stmt == Stmt.Health) Degraded(e, Pg.LastError);
        else SetError(e, 500, "internal server error"u8);
    }

    static void Degraded(PgEntry e, string error)
    {
        e.W = JsonWriter.Create(1024);
        e.W.Raw("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":"u8);
        e.W.String(error);
        e.W.Raw("}"u8);
        e.Status = 503;
    }

    static void SetError(PgEntry e, int status, ReadOnlySpan<byte> message)
    {
        Db.Return(ref e.W);
        e.W = JsonWriter.Create(256);
        e.W.Raw("{\"error\":"u8);
        e.W.Utf8String(message);
        e.W.Raw("}"u8);
        e.Status = status;
    }
}
