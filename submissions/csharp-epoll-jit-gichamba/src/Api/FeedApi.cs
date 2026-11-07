using System.Diagnostics;
using System.Text;
using Feed.Auth;
using Feed.Http;

namespace Feed.Api;

/// <summary>
/// The five endpoints of SPEC.md, one instance per loop. Each request is checked on the loop thread, then its one query
/// goes to one of that loop's Postgres connections, and the same thread answers when the result arrives. Reads and
/// health checks use one set of connections, inserts the other, so a read never waits behind a commit.
/// </summary>
public sealed class FeedApi : IRequestHandler {
    /// <summary>Endpoint indexes for <see cref="Stats"/>, in <see cref="Endpoints"/> order.</summary>
    private const int FeedIndex = 0, PostIndex = 1, CreateIndex = 2, LikeIndex = 3, HealthIndex = 4, OtherIndex = 5;

    /// <summary>Endpoint names for the stats line.</summary>
    public static readonly string[] Endpoints = ["feed", "post", "create", "like", "health", "other"];

    /// <summary>Feed, post and health queries go here.</summary>
    private readonly IQuerySender _reads;
    /// <summary>Inserts go here.</summary>
    private readonly IQuerySender _writes;
    /// <summary>The JWT signing key.</summary>
    private readonly byte[] _key;
    /// <summary>Smallest body gzipped for a client that accepts gzip.</summary>
    private readonly int _gzipMin;
    /// <summary>Request counters.</summary>
    private readonly Stats _stats;
    /// <summary>The current Unix time in seconds; replaceable for tests.</summary>
    private readonly Func<long> _now;
    /// <summary>Runs since the server started, for uptime_s.</summary>
    private readonly Stopwatch _uptime = Stopwatch.StartNew();

    /// <summary>Creates the handler over the two senders.</summary>
    public FeedApi(Settings settings, IQuerySender reads, IQuerySender writes, Stats stats, Func<long>? now = null) {
        _reads = reads;
        _writes = writes;
        _key = Encoding.UTF8.GetBytes(settings.JwtSecret);
        _gzipMin = settings.GzipMin;
        _stats = stats;
        _now = now ?? (() => DateTimeOffset.UtcNow.ToUnixTimeSeconds());
    }

    /// <inheritdoc />
    public void Handle(Connection conn, in HttpRequest request) {
        ReadOnlySpan<byte> path = request.Path;
        bool get = request.Method.SequenceEqual("GET"u8);
        bool post = !get && request.Method.SequenceEqual("POST"u8);
        int gzipMin = GzipThreshold(request.AcceptsGzip, _gzipMin);

        if (get && path.SequenceEqual("/feed"u8)) {
            _stats.RecordRequest(FeedIndex);
            Send(_reads, new FeedQuery(conn, gzipMin));
        } else if (post && path.SequenceEqual("/posts"u8)) {
            _stats.RecordRequest(CreateIndex);
            Create(conn, request, gzipMin);
        } else if (path.StartsWith("/posts/"u8)) {
            ReadOnlySpan<byte> rest = path["/posts/".Length..];
            if (post && rest.EndsWith("/like"u8)) {
                _stats.RecordRequest(LikeIndex);
                Like(conn, request, rest[..^"/like".Length], gzipMin);
            } else if (get && rest.IndexOf((byte)'/') < 0) {
                _stats.RecordRequest(PostIndex);
                if (TryParseId(rest, out long id)) {
                    Send(_reads, new PostQuery(conn, gzipMin, id));
                } else {
                    conn.Send(Responses.InvalidPostId);
                }
            } else {
                _stats.RecordRequest(OtherIndex);
                conn.Send(Responses.NotFound);
            }
        } else if (get && path.SequenceEqual("/health"u8)) {
            _stats.RecordRequest(HealthIndex);
            Send(_reads, new HealthQuery(conn, gzipMin, (long)_uptime.Elapsed.TotalSeconds));
        } else {
            _stats.RecordRequest(OtherIndex);
            conn.Send(Responses.NotFound);
        }
    }

    /// <summary>POST /posts: auth, then the body, then the insert.</summary>
    private void Create(Connection conn, in HttpRequest request, int gzipMin) {
        if (!Authorize(conn, request.Authorization, out long userId, out string? username)) {
            return;
        }

        switch (PostBody.Read(request.Body, out string? body)) {
            case PostBodyStatus.Ok:
                Send(_writes, new CreateQuery(conn, gzipMin, userId, username!, body!));
                break;
            case PostBodyStatus.Malformed:
                conn.Send(Responses.MalformedJson);
                break;
            case PostBodyStatus.TooLong:
                conn.Send(Responses.BodyTooLong);
                break;
            default:
                conn.Send(Responses.BodyRequired);
                break;
        }
    }

    /// <summary>POST /posts/:id/like: auth first, then the id, then the insert.</summary>
    private void Like(Connection conn, in HttpRequest request, ReadOnlySpan<byte> idText, int gzipMin) {
        if (!Authorize(conn, request.Authorization, out long userId, out _)) {
            return;
        }

        if (!TryParseId(idText, out long postId)) {
            conn.Send(Responses.InvalidPostId);
            return;
        }

        Send(_writes, new LikeQuery(conn, gzipMin, userId, postId));
    }

    /// <summary>
    /// Checks the Authorization header. On failure it sends the 401 SPEC.md names and returns false; on success the
    /// token's user id and username come out.
    /// </summary>
    private bool Authorize(Connection conn, ReadOnlySpan<byte> header, out long userId, out string? username) {
        userId = 0;
        username = null;
        if (!header.StartsWith("Bearer "u8)) {
            conn.Send(Responses.MissingBearer);
            return false;
        }

        switch (Jwt.Verify(header["Bearer ".Length..], _key, _now(), out userId, out username)) {
            case JwtStatus.Ok:
                return true;
            case JwtStatus.BadPayload:
                conn.Send(Responses.InvalidPayload);
                return false;
            default:
                conn.Send(Responses.InvalidToken);
                return false;
        }
    }

    /// <summary>
    /// The smallest body gzipped for this request: <paramref name="gzipMin"/> (the GZIP_MIN setting) when the client
    /// accepts gzip, and int.MaxValue, so nothing is, when it does not or when the setting is 0 (off).
    /// </summary>
    internal static int GzipThreshold(bool acceptsGzip, int gzipMin) => acceptsGzip && gzipMin > 0 ? gzipMin : int.MaxValue;

    /// <summary>Pipelines <paramref name="query"/>; with no open connection it answers at once instead.</summary>
    private static void Send(IQuerySender sender, Query query) {
        if (!sender.TrySend(query)) {
            query.Fail("no database connection");
        }
    }

    /// <summary>
    /// Reads a post id: 1 to 18 plain digits, above zero. Signs, decimals and anything else are refused; SPEC.md leaves
    /// ids beyond 2^53 out of scope.
    /// </summary>
    internal static bool TryParseId(ReadOnlySpan<byte> text, out long id) {
        id = 0;
        if (text.Length is 0 or > 18) {
            return false;
        }

        foreach (byte b in text) {
            if (b < '0' || b > '9') {
                return false;
            }

            id = id * 10 + (b - '0');
        }
        return id > 0;
    }
}
