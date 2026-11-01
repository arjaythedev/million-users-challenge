using Feed.Http;
using Feed.Json;
using Feed.Pg;

namespace Feed.Api;

/// <summary>
/// One request's query: it binds one prepared statement, collects the result as its loop parses Postgres's replies,
/// and answers the client when the result is complete. Callbacks run on the loop thread and do not block.
/// </summary>
public abstract class Query : PgRequest {
    /// <summary>The client connection to answer.</summary>
    protected readonly Connection Client;
    /// <summary>The response being built; returned to the pool once sent.</summary>
    protected readonly ResponseWriter Writer = ResponseWriter.Rent();
    /// <summary>Gzip a body this big or bigger; int.MaxValue when the client does not accept gzip.</summary>
    private readonly int _gzipMin;
    /// <summary>The error Postgres reported, if any.</summary>
    protected PgError? Error { get; private set; }

    /// <summary>Starts a query that answers <paramref name="client"/>, gzipping bodies of at least <paramref name="gzipMin"/> bytes.</summary>
    protected Query(Connection client, int gzipMin, string statement) : base(statement) {
        Client = client;
        _gzipMin = gzipMin;
    }

    /// <summary>Set on a write whose group failed: it is sent again in a group of its own.</summary>
    public bool RetryAlone { get; set; }

    /// <summary>Forgets a result that was rolled back, so the query can run again. Overrides clear their own state too.</summary>
    public virtual void Reset() => Error = null;

    /// <inheritdoc />
    public override void OnRow(in PgRow row) {
    }

    /// <inheritdoc />
    public override void OnCommandComplete(ReadOnlySpan<byte> tag) {
    }

    /// <inheritdoc />
    public override void OnError(PgError error) => Error = error;

    /// <summary>Answers the client from the collected result. A failure while answering sends a plain 500.</summary>
    public override void OnDone() {
        try {
            Respond();
        } catch (Exception) {
            Client.Send(Responses.ServerError);
        } finally {
            ResponseWriter.Return(Writer);
        }
    }

    /// <summary>The query never ran or its result was lost: answer with an error instead. <paramref name="reason"/> says why.</summary>
    public void Fail(string reason) {
        try {
            Failed(reason);
        } finally {
            ResponseWriter.Return(Writer);
        }
    }

    /// <summary>Answers after the result is complete; <see cref="Error"/> is set when Postgres reported one.</summary>
    protected abstract void Respond();

    /// <summary>Answers when the query was lost. Most endpoints answer 500.</summary>
    protected virtual void Failed(string reason) => Client.Send(Responses.ServerError);

    /// <summary>Sends <see cref="Writer"/>'s body with <paramref name="status"/>, gzipped when it is big enough.</summary>
    protected void Send(int status) => Client.Send(Writer.Finish(status, Writer.BodyLength >= _gzipMin));

    /// <summary>
    /// Writes one post object from a row of <see cref="Sql.Feed"/> or <see cref="Sql.Post"/>. The keys never change, so
    /// the text between values is written as constant pieces rather than one property name at a time.
    /// </summary>
    protected static void WritePost(JsonWriter j, in PgRow row) {
        j.BeginRawValue();
        j.AppendRaw("{\"id\":"u8);
        j.AppendLong(PgBinary.ReadInt8(row[0]));
        j.AppendRaw(",\"body\":\""u8);
        j.AppendEscaped(row[1]);
        j.AppendRaw("\",\"created_at\":"u8);
        j.AppendTimestamp(PgBinary.ReadTimestamptz(row[2]));
        j.AppendRaw(",\"author\":\""u8);
        j.AppendEscaped(row[3]);
        j.AppendRaw("\",\"like_count\":"u8);
        j.AppendLong(PgBinary.ReadInt4(row[4]));
        j.AppendRaw("}"u8);
    }
}

/// <summary>GET /feed: {"posts":[...]} with the 20 newest posts.</summary>
public sealed class FeedQuery : Query {
    /// <summary>Starts the body; rows are written as they arrive.</summary>
    public FeedQuery(Connection client, int gzipMin) : base(client, gzipMin, Sql.Feed.Name) {
        Writer.Body.StartObject();
        Writer.Body.PropertyName("posts"u8);
        Writer.Body.StartArray();
    }

    /// <inheritdoc />
    public override void OnRow(in PgRow row) => WritePost(Writer.Body, row);

    /// <inheritdoc />
    protected override void Respond() {
        if (Error is not null) {
            Client.Send(Responses.ServerError);
            return;
        }

        Writer.Body.EndArray();
        Writer.Body.EndObject();
        Send(200);
    }
}

/// <summary>GET /posts/:id: {"post":{...}}, or 404 when there is no such post.</summary>
public sealed class PostQuery : Query {
    /// <summary>True once the row arrived.</summary>
    private bool _found;

    /// <summary>Looks up post <paramref name="id"/>.</summary>
    public PostQuery(Connection client, int gzipMin, long id) : base(client, gzipMin, Sql.Post.Name) => Params.AddInt8(id);

    /// <inheritdoc />
    public override void OnRow(in PgRow row) {
        Writer.Body.StartObject();
        Writer.Body.PropertyName("post"u8);
        WritePost(Writer.Body, row);
        Writer.Body.EndObject();
        _found = true;
    }

    /// <inheritdoc />
    protected override void Respond() {
        if (Error is not null) {
            Client.Send(Responses.ServerError);
        } else if (!_found) {
            Client.Send(Responses.PostNotFound);
        } else {
            Send(200);
        }
    }
}

/// <summary>POST /posts: 201 with the new post, its author the token's username and no likes.</summary>
public sealed class CreateQuery : Query {
    /// <summary>The trimmed body as stored.</summary>
    private readonly string _body;
    /// <summary>The token's username.</summary>
    private readonly string _author;
    /// <summary>The new post's id and created_at, from RETURNING.</summary>
    private long _id, _createdAt;
    /// <summary>True once the RETURNING row arrived.</summary>
    private bool _returned;

    /// <summary>Inserts <paramref name="body"/> by user <paramref name="userId"/>, named <paramref name="author"/>.</summary>
    public CreateQuery(Connection client, int gzipMin, long userId, string author, string body) : base(client, gzipMin, Sql.Create.Name) {
        _body = body;
        _author = author;
        Params.AddInt8(userId);
        Params.AddText(body);
    }

    /// <inheritdoc />
    public override void Reset() {
        base.Reset();
        _returned = false;
    }

    /// <inheritdoc />
    public override void OnRow(in PgRow row) {
        _id = PgBinary.ReadInt8(row[0]);
        _createdAt = PgBinary.ReadTimestamptz(row[1]);
        _returned = true;
    }

    /// <inheritdoc />
    protected override void Respond() {
        if (Error is not null || !_returned) {
            Client.Send(Responses.ServerError);
            return;
        }

        JsonWriter j = Writer.Body;
        j.StartObject();
        j.PropertyName("post"u8);
        j.StartObject();
        j.PropertyName("id"u8);
        j.WriteLong(_id);
        j.PropertyName("body"u8);
        j.WriteString(_body);
        j.PropertyName("created_at"u8);
        j.WriteTimestamp(_createdAt);
        j.PropertyName("author"u8);
        j.WriteString(_author);
        j.PropertyName("like_count"u8);
        j.WriteLong(0);
        j.EndObject();
        j.EndObject();
        Send(201);
    }
}

/// <summary>POST /posts/:id/like: 201 for a new like, 200 for a repeat, 404 when the post does not exist.</summary>
public sealed class LikeQuery : Query {
    /// <summary>SQLSTATE foreign_key_violation: the post does not exist.</summary>
    private const string ForeignKeyViolation = "23503";
    /// <summary>The liked post.</summary>
    private readonly long _postId;
    /// <summary>Rows inserted, from the command tag; -1 until it arrives.</summary>
    private long _inserted = -1;

    /// <summary>Inserts a like by user <paramref name="userId"/> on post <paramref name="postId"/>.</summary>
    public LikeQuery(Connection client, int gzipMin, long userId, long postId) : base(client, gzipMin, Sql.Like.Name) {
        _postId = postId;
        Params.AddInt8(userId);
        Params.AddInt8(postId);
    }

    /// <inheritdoc />
    public override void Reset() {
        base.Reset();
        _inserted = -1;
    }

    /// <inheritdoc />
    public override void OnCommandComplete(ReadOnlySpan<byte> tag) => _inserted = InsertedRows(tag);

    /// <inheritdoc />
    protected override void Respond() {
        if (Error is { SqlState: ForeignKeyViolation }) {
            Client.Send(Responses.PostNotFound);
            return;
        }

        if (Error is not null || _inserted < 0) {
            Client.Send(Responses.ServerError);
            return;
        }

        JsonWriter j = Writer.Body;
        j.StartObject();
        j.PropertyName("liked"u8);
        j.WriteBool(true);
        j.PropertyName("already_liked"u8);
        j.WriteBool(_inserted == 0);
        j.PropertyName("post_id"u8);
        j.WriteLong(_postId);
        j.EndObject();
        Send(_inserted == 0 ? 200 : 201);
    }

    /// <summary>The row count of an INSERT command tag ("INSERT 0 1"), or -1 if the tag is something else.</summary>
    internal static long InsertedRows(ReadOnlySpan<byte> tag) {
        int space = tag.LastIndexOf((byte)' ');
        if (!tag.StartsWith("INSERT "u8) || space < 0) {
            return -1;
        }

        long n = 0;
        foreach (byte b in tag[(space + 1)..]) {
            if (b < '0' || b > '9') {
                return -1;
            }

            n = n * 10 + (b - '0');
        }
        return n;
    }
}

/// <summary>GET /health: 200 once SELECT 1 succeeds, else 503 with the reason.</summary>
public sealed class HealthQuery : Query {
    /// <summary>Whole seconds since the process started.</summary>
    private readonly long _uptime;

    /// <summary>Runs SELECT 1 and reports <paramref name="uptimeSeconds"/> on success.</summary>
    public HealthQuery(Connection client, int gzipMin, long uptimeSeconds) : base(client, gzipMin, Sql.Ping.Name) => _uptime = uptimeSeconds;

    /// <inheritdoc />
    protected override void Respond() {
        if (Error is { } e) {
            Failed(e.Message);
            return;
        }

        JsonWriter j = Writer.Body;
        j.StartObject();
        j.PropertyName("status"u8);
        j.WriteString("ok"u8);
        j.PropertyName("db"u8);
        j.WriteString("ok"u8);
        j.PropertyName("uptime_s"u8);
        j.WriteLong(_uptime);
        j.EndObject();
        Send(200);
    }

    /// <inheritdoc />
    protected override void Failed(string reason) {
        Writer.Body.Reset(ResponseWriter.Headroom);
        JsonWriter j = Writer.Body;
        j.StartObject();
        j.PropertyName("status"u8);
        j.WriteString("degraded"u8);
        j.PropertyName("db"u8);
        j.WriteString("unreachable"u8);
        j.PropertyName("error"u8);
        j.WriteString(reason);
        j.EndObject();
        Send(503);
    }
}
