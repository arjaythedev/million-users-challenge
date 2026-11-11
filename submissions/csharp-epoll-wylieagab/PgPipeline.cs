using System.Buffers;
using System.Buffers.Binary;
using System.Diagnostics;
using System.Globalization;
using System.Net;
using System.Net.Sockets;
using System.Runtime.InteropServices;
using System.Text;
using System.Text.Encodings.Web;
using System.Text.Json;

namespace MillionReactor;

public enum DbOperation { Feed, Post, Create, Like, Health }
public sealed record DbRequest(DbOperation Operation, long Id, long UserId, string? Body, string? Username, Action<DbResponse> Complete);
public sealed record DbResponse(int Status, byte[] Body);

// One shard is confined to one reactor thread, including its callbacks. It never
// uses Tasks, the thread pool, or blocking database operations after construction.
public sealed unsafe class PgShard : IDisposable
{
    private const int ReadConnections = 4;
    private const int WriteConnections = 2;
    private const int PipelineDepth = 32;
    private const int QueueLimit = 256;
    private static readonly long TimeoutTicks = Stopwatch.Frequency * 10L;
    private static readonly long Started = Stopwatch.GetTimestamp();
    private static readonly long EpochTicks = new DateTime(2000, 1, 1, 0, 0, 0, DateTimeKind.Utc).Ticks;
    private const string SelectSql = "SELECT p.id, p.body, p.created_at, u.username, (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count FROM posts p JOIN users u ON u.id = p.user_id";
    private static readonly string[] Sql =
    [
        SelectSql + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20",
        SelectSql + " WHERE p.id = $1",
        "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at",
        "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING",
        "SELECT 1"
    ];
    private static readonly byte[][] Names = ["feed\0"u8.ToArray(), "post\0"u8.ToArray(), "create\0"u8.ToArray(), "like\0"u8.ToArray(), "health\0"u8.ToArray()];
    private static readonly byte[][] SqlBytes = Sql.Select(sql => Encoding.UTF8.GetBytes(sql + '\0')).ToArray();
    private static readonly int[] ParameterCounts = [0, 1, 2, 2, 0];
    private static readonly DbResponse InternalError = new(500, "{\"error\":\"internal server error\"}"u8.ToArray());
    private static readonly DbResponse MissingPost = new(404, "{\"error\":\"post not found\"}"u8.ToArray());
    private static readonly DbResponse Unreachable = new(503, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"database unavailable\"}"u8.ToArray());

    private readonly Action<int, bool> watch;
    private readonly Action<int> unwatch;
    private readonly Dictionary<int, Connection> sockets = new(ReadConnections + WriteConnections);
    private readonly Connection[] reads;
    private readonly Connection[] writes;
    private readonly Connection[] all;
    private readonly Queue<Pending> readQueue = new(QueueLimit);
    private readonly Queue<Pending> writeQueue = new(QueueLimit);
    private readonly string databaseUrl;
    private readonly string hostAddress;
    private readonly string applicationName;
    private readonly ArrayBufferWriter<byte> jsonBuffer = new(8192);
    private readonly Utf8JsonWriter json;
    private bool dispatching;
    private bool disposed;
    private int nextRead;
    private int nextWrite;
    private long acceptedRequests;
    private long completedRequests;
    private long errorResponses;
    private long queueWaitTicks;
    private long inFlightTicks;
    private int inFlight;
    private int maxQueued;
    private int maxInFlight;
    private long resets;
    private long reconnects;
    private long callbackErrors;

    public PgShard(string databaseUrl, int workerIndex, int workerCount, Action<int, bool> watch, Action<int> unwatch)
    {
        if (workerCount is < 1 or > 64 || workerIndex < 0 || workerIndex >= workerCount)
            throw new ArgumentOutOfRangeException(nameof(workerCount), "Use 1..64 workers: six connections each, at most 384 total.");
        this.databaseUrl = databaseUrl;
        this.watch = watch;
        this.unwatch = unwatch;
        applicationName = $"csharp-epoll-{workerIndex}";
        var uri = new Uri(databaseUrl);
        if (uri.Scheme is not ("postgres" or "postgresql"))
            throw new ArgumentException("DATABASE_URL must use postgres:// or postgresql://", nameof(databaseUrl));
        // Resolve once before serving HTTP. Passing hostaddr on every later connect
        // prevents libpq's hostname resolution from blocking the reactor on reconnect.
        string hostname = uri.DnsSafeHost;
        hostAddress = IPAddress.TryParse(hostname, out var address) ? address.ToString() :
            Dns.GetHostAddresses(hostname).OrderBy(ip => ip.AddressFamily == AddressFamily.InterNetwork ? 0 : 1).First().ToString();
        json = new Utf8JsonWriter(jsonBuffer, new JsonWriterOptions
        {
            Encoder = JavaScriptEncoder.UnsafeRelaxedJsonEscaping,
            SkipValidation = true
        });
        reads = Enumerable.Range(0, ReadConnections).Select(_ => new Connection(this, false)).ToArray();
        writes = Enumerable.Range(0, WriteConnections).Select(_ => new Connection(this, true)).ToArray();
        all = [.. reads, .. writes];
        try
        {
            foreach (var connection in all) connection.StartConnect();
            WaitForInitialConnections();
        }
        catch
        {
            Dispose();
            throw;
        }
    }

    public bool Owns(int fd) => sockets.ContainsKey(fd);

    public void OnReady(int fd)
    {
        if (disposed || !sockets.TryGetValue(fd, out var connection)) return;
        connection.OnReady();
        Dispatch();
    }

    // False means admission backpressure: the caller still owns the request and no
    // callback will occur. True guarantees exactly one completion, even on failure.
    public bool Submit(DbRequest request)
    {
        if (disposed) return false;
        if (request.Operation is < DbOperation.Feed or > DbOperation.Health)
        {
            acceptedRequests++;
            Complete(new Pending(request, Stopwatch.GetTimestamp()), InternalError);
            return true;
        }
        var queue = request.Operation is DbOperation.Create or DbOperation.Like ? writeQueue : readQueue;
        if (queue.Count >= QueueLimit) return false;
        acceptedRequests++;
        queue.Enqueue(new Pending(request, Stopwatch.GetTimestamp()));
        maxQueued = Math.Max(maxQueued, readQueue.Count + writeQueue.Count);
        Dispatch();
        return true;
    }

    // Call periodically even when epoll has no events (e.g. every 50-100 ms).
    public void Tick()
    {
        if (disposed) return;
        long now = Stopwatch.GetTimestamp();
        foreach (var connection in all) connection.Tick(now);
        ExpireQueue(readQueue, now);
        ExpireQueue(writeQueue, now);
        Dispatch();
    }

    private void ExpireQueue(Queue<Pending> queue, long now)
    {
        while (queue.TryPeek(out var pending) && now - pending.Accepted >= TimeoutTicks)
        {
            queue.Dequeue();
            Complete(pending, Failure(pending.Request));
        }
    }

    private void Dispatch()
    {
        if (disposed || dispatching) return;
        dispatching = true;
        try
        {
            DispatchQueue(readQueue, reads, ref nextRead);
            DispatchQueue(writeQueue, writes, ref nextWrite);
        }
        finally { dispatching = false; }
    }

    private void DispatchQueue(Queue<Pending> queue, Connection[] connections, ref int next)
    {
        while (queue.Count > 0 && !disposed)
        {
            Connection? chosen = null;
            int chosenIndex = 0;
            for (int offset = 0; offset < connections.Length; offset++)
            {
                int index = (next + offset) % connections.Length;
                var candidate = connections[index];
                if (candidate.Ready && candidate.Count < PipelineDepth &&
                    (chosen is null || candidate.Count < chosen.Count))
                {
                    chosen = candidate;
                    chosenIndex = index;
                }
            }
            if (chosen is null) return;
            var pending = queue.Dequeue();
            next = (chosenIndex + 1) % connections.Length;
            if (Stopwatch.GetTimestamp() - pending.Accepted >= TimeoutTicks)
                Complete(pending, Failure(pending.Request));
            else chosen.Send(pending);
        }
    }

    private void WaitForInitialConnections()
    {
        long deadline = Stopwatch.GetTimestamp() + TimeoutTicks;
        Span<PgNative.PollFd> fds = stackalloc PgNative.PollFd[ReadConnections + WriteConnections];
        while (Stopwatch.GetTimestamp() < deadline)
        {
            int count = 0;
            foreach (var connection in all)
                if (connection.Initializing && connection.Fd >= 0)
                    fds[count++] = new PgNative.PollFd { Fd = connection.Fd, Events = (short)(1 | (connection.WantsWrite ? 4 : 0)) };
            if (count == 0) return;
            int remaining = Math.Max(1, (int)((deadline - Stopwatch.GetTimestamp()) * 1000 / Stopwatch.Frequency));
            int result;
            fixed (PgNative.PollFd* ptr = fds)
                result = PgNative.poll(ptr, (nuint)count, Math.Min(100, remaining));
            if (result < 0)
            {
                if (Marshal.GetLastPInvokeError() == 4) continue; // EINTR
                break;
            }
            for (int i = 0; i < count; i++)
                if (fds[i].ReturnedEvents != 0 && sockets.TryGetValue(fds[i].Fd, out var connection)) connection.OnReady();
        }
        foreach (var connection in all)
            if (connection.Initializing) connection.Fail();
    }

    private static DbResponse Failure(DbRequest request) => request.Operation == DbOperation.Health ? Unreachable : InternalError;

    private void Complete(Pending pending, DbResponse response)
    {
        if (pending.Completed) return;
        pending.Completed = true;
        completedRequests++;
        if (response.Status >= 400) errorResponses++;
        long now = Stopwatch.GetTimestamp();
        if (pending.SentAt != 0)
        {
            inFlight--;
            inFlightTicks += now - pending.SentAt;
        }
        else queueWaitTicks += now - pending.Accepted;
        try { pending.Request.Complete(response); }
        catch (Exception)
        {
            // A callback must not strand other entries; aggregate diagnostics only.
            callbackErrors++;
        }
    }

    private DbResponse Serialize(DbRequest request, nint result)
    {
        int status = PgNative.PQresultStatus(result);
        if (status is 5 or 6 or 7 or 11) // BAD_RESPONSE, NONFATAL/FATAL_ERROR, PIPELINE_ABORTED
        {
            if (request.Operation == DbOperation.Like &&
                Marshal.PtrToStringUTF8(PgNative.PQresultErrorField(result, 'C')) == "23503") return MissingPost;
            return Failure(request);
        }
        jsonBuffer.Clear();
        json.Reset(jsonBuffer);
        try
        {
            int responseStatus = 200;
            int rows = PgNative.PQntuples(result);
            switch (request.Operation)
            {
                case DbOperation.Feed:
                    Require(status == 2 && PgNative.PQnfields(result) == 5);
                    json.WriteStartObject();
                    json.WriteStartArray("posts"u8);
                    for (int row = 0; row < rows; row++) WritePost(result, row);
                    json.WriteEndArray();
                    json.WriteEndObject();
                    break;
                case DbOperation.Post:
                    Require(status == 2 && PgNative.PQnfields(result) == 5);
                    if (rows == 0) return MissingPost;
                    Require(rows == 1);
                    json.WriteStartObject();
                    json.WritePropertyName("post"u8);
                    WritePost(result, 0);
                    json.WriteEndObject();
                    break;
                case DbOperation.Create:
                    Require(status == 2 && rows == 1 && PgNative.PQnfields(result) == 2 && request.Body is not null && request.Username is not null);
                    json.WriteStartObject();
                    json.WriteStartObject("post"u8);
                    json.WriteNumber("id"u8, ReadInt64(result, 0, 0));
                    json.WriteString("body"u8, request.Body);
                    WriteTimestamp(ReadTimestamp(result, 0, 1));
                    json.WriteString("author"u8, request.Username);
                    json.WriteNumber("like_count"u8, 0);
                    json.WriteEndObject();
                    json.WriteEndObject();
                    responseStatus = 201;
                    break;
                case DbOperation.Like:
                    Require(status == 1);
                    byte* affected = PgNative.PQcmdTuples(result);
                    Require(affected != null && (affected[0] == (byte)'0' || affected[0] == (byte)'1') && affected[1] == 0);
                    bool inserted = affected[0] == (byte)'1';
                    json.WriteStartObject();
                    json.WriteBoolean("liked"u8, true);
                    json.WriteBoolean("already_liked"u8, !inserted);
                    json.WriteNumber("post_id"u8, request.Id);
                    json.WriteEndObject();
                    responseStatus = inserted ? 201 : 200;
                    break;
                case DbOperation.Health:
                    Require(status == 2 && rows == 1 && PgNative.PQnfields(result) == 1 && ReadInt32(result, 0, 0) == 1);
                    json.WriteStartObject();
                    json.WriteString("status"u8, "ok"u8);
                    json.WriteString("db"u8, "ok"u8);
                    json.WriteNumber("uptime_s"u8, (long)Stopwatch.GetElapsedTime(Started).TotalSeconds);
                    json.WriteEndObject();
                    break;
                default: return InternalError;
            }
            json.Flush();
            return new DbResponse(responseStatus, jsonBuffer.WrittenSpan.ToArray());
        }
        catch (Exception) { return Failure(request); }
        finally
        {
            jsonBuffer.Clear();
            json.Reset(jsonBuffer);
        }
    }

    private void WritePost(nint result, int row)
    {
        json.WriteStartObject();
        json.WriteNumber("id"u8, ReadInt64(result, row, 0));
        json.WriteString("body"u8, Field(result, row, 1));
        WriteTimestamp(ReadTimestamp(result, row, 2));
        json.WriteString("author"u8, Field(result, row, 3));
        json.WriteNumber("like_count"u8, ReadInt32(result, row, 4));
        json.WriteEndObject();
    }
    private void WriteTimestamp(DateTime created)
    {
        Span<char> timestamp = stackalloc char[24];
        Require(created.TryFormat(timestamp, out int length, "yyyy-MM-dd'T'HH:mm:ss.fff'Z'", CultureInfo.InvariantCulture));
        json.WriteString("created_at"u8, timestamp[..length]);
    }
    private static ReadOnlySpan<byte> Field(nint result, int row, int column)
    {
        Require(PgNative.PQgetisnull(result, row, column) == 0);
        return new ReadOnlySpan<byte>(PgNative.PQgetvalue(result, row, column), PgNative.PQgetlength(result, row, column));
    }
    private static long ReadInt64(nint result, int row, int column)
    {
        var value = Field(result, row, column);
        Require(value.Length == 8);
        return BinaryPrimitives.ReadInt64BigEndian(value);
    }
    private static int ReadInt32(nint result, int row, int column)
    {
        var value = Field(result, row, column);
        Require(value.Length == 4);
        return BinaryPrimitives.ReadInt32BigEndian(value);
    }
    private static DateTime ReadTimestamp(nint result, int row, int column) =>
        new(checked(EpochTicks + checked(ReadInt64(result, row, column) * 10)), DateTimeKind.Utc);
    private static void Require(bool condition)
    {
        if (!condition) throw new InvalidDataException("Unexpected PostgreSQL result.");
    }

    public void Dispose()
    {
        if (disposed) return;
        disposed = true;
        foreach (var connection in all) connection.Close();
        while (readQueue.TryDequeue(out var pending)) Complete(pending, Failure(pending.Request));
        while (writeQueue.TryDequeue(out var pending)) Complete(pending, Failure(pending.Request));
        json.Dispose();
        Console.WriteLine($"pg_shard={applicationName} accepted={acceptedRequests} completed={completedRequests} errors={errorResponses} maxqueued={maxQueued} maxinflight={maxInFlight} queue_wait_ticks={queueWaitTicks} inflight_ticks={inFlightTicks} resets={resets} reconnects={reconnects} callback_errors={callbackErrors} stopwatch_frequency={Stopwatch.Frequency}");
    }

    private sealed class Pending(DbRequest request, long accepted)
    {
        internal readonly DbRequest Request = request;
        internal readonly long Accepted = accepted;
        internal long SentAt;
        internal DbResponse? Response;
        internal bool SawBoundary;
        internal bool Completed;
    }

    private enum ConnectionState { Disconnected, Connecting, Preparing, Ready, Closed }

    private sealed class Connection(PgShard owner, bool isWrite)
    {
        private readonly Queue<Pending> pending = new(PipelineDepth);
        private readonly int[] preparation = isWrite ? [2, 3] : [0, 1, 4];
        private nint handle;
        private ConnectionState state;
        private int prepareIndex;
        private bool prepareResult;
        private long connectStarted;
        private long retryAt;
        private bool everStarted;
        internal int Fd { get; private set; } = -1;
        internal bool WantsWrite { get; private set; }
        internal bool Ready => state == ConnectionState.Ready;
        internal bool Initializing => state is ConnectionState.Connecting or ConnectionState.Preparing;
        internal int Count => pending.Count;

        internal void StartConnect()
        {
            if (state is not ConnectionState.Disconnected || owner.disposed) return;
            if (everStarted) owner.reconnects++;
            everStarted = true;
            connectStarted = Stopwatch.GetTimestamp();
            // libpq copies all connection parameters before returning. Reconnect uses
            // the resolved hostaddr, so there is no blocking DNS on the request path.
            string[] keys = ["dbname", "connect_timeout", "client_encoding", "application_name", "options", "hostaddr"];
            string[] values = [owner.databaseUrl, "10", "UTF8", owner.applicationName, "-c statement_timeout=10000", owner.hostAddress];
            byte** keyPointers = stackalloc byte*[keys.Length + 1];
            byte** valuePointers = stackalloc byte*[values.Length + 1];
            for (int i = 0; i <= keys.Length; i++) { keyPointers[i] = null; valuePointers[i] = null; }
            try
            {
                for (int i = 0; i < keys.Length; i++)
                {
                    keyPointers[i] = (byte*)Marshal.StringToCoTaskMemUTF8(keys[i]);
                    valuePointers[i] = (byte*)Marshal.StringToCoTaskMemUTF8(values[i]);
                }
                handle = PgNative.PQconnectStartParams(keyPointers, valuePointers, 1);
            }
            finally
            {
                for (int i = 0; i < keys.Length; i++)
                {
                    Marshal.FreeCoTaskMem((nint)keyPointers[i]);
                    Marshal.FreeCoTaskMem((nint)valuePointers[i]);
                }
            }
            if (handle == 0 || PgNative.PQstatus(handle) == 1) { Fail(); return; }
            state = ConnectionState.Connecting;
            PollConnect();
        }

        private void PollConnect()
        {
            for (int attempt = 0; attempt < 8; attempt++)
            {
                int poll = PgNative.PQconnectPoll(handle);
                if (poll == 0) { Fail(); return; }
                if (poll == 3)
                {
                    if (PgNative.PQsetnonblocking(handle, 1) != 0) { Fail(); return; }
                    state = ConnectionState.Preparing;
                    prepareIndex = 0;
                    RefreshSocket(false, force: true);
                    SendPrepare();
                    return;
                }
                RefreshSocket(poll != 1, force: true);
                if (poll != 4) return; // READING/WRITING wait for epoll; ACTIVE polls again.
            }
        }

        internal void OnReady()
        {
            if (handle == 0 || state is ConnectionState.Disconnected or ConnectionState.Closed) return;
            if (state == ConnectionState.Connecting) { PollConnect(); return; }
            if (PgNative.PQconsumeInput(handle) != 1) { Fail(); return; }
            if (state == ConnectionState.Preparing) DrainPreparation();
            else DrainResults();
            if (handle != 0 && state is ConnectionState.Preparing or ConnectionState.Ready) Flush();
        }

        private void SendPrepare()
        {
            int operation = preparation[prepareIndex];
            prepareResult = false;
            uint* types = stackalloc uint[2];
            types[0] = 20; // int8
            types[1] = operation == 2 ? 25u : 20u; // text for post body, otherwise int8
            int sent;
            fixed (byte* name = Names[operation])
            fixed (byte* sql = SqlBytes[operation])
                sent = PgNative.PQsendPrepare(handle, name, sql, ParameterCounts[operation], types);
            if (sent != 1) { Fail(); return; }
            Flush();
        }

        private void DrainPreparation()
        {
            while (state == ConnectionState.Preparing && PgNative.PQisBusy(handle) == 0)
            {
                nint result = PgNative.PQgetResult(handle);
                if (result == 0)
                {
                    if (!prepareResult) { Fail(); return; }
                    if (++prepareIndex == preparation.Length)
                    {
                        if (PgNative.PQenterPipelineMode(handle) != 1) { Fail(); return; }
                        state = ConnectionState.Ready;
                        return;
                    }
                    SendPrepare();
                }
                else
                {
                    bool success = PgNative.PQresultStatus(result) == 1;
                    PgNative.PQclear(result);
                    if (!success || prepareResult) { Fail(); return; }
                    prepareResult = true;
                }
            }
        }

        internal void Send(Pending request)
        {
            request.SentAt = Stopwatch.GetTimestamp();
            owner.queueWaitTicks += request.SentAt - request.Accepted;
            owner.maxInFlight = Math.Max(owner.maxInFlight, ++owner.inFlight);
            pending.Enqueue(request);
            try
            {
                if (!SendParameters(request.Request) || PgNative.PQpipelineSync(handle) != 1)
                {
                    Fail();
                    return;
                }
                // PQpipelineSync flushes, but a nonblocking connection can still
                // have queued output. EPOLLOUT is requested only while needed.
                Flush();
            }
            catch (Exception) { Fail(); }
        }

        private bool SendParameters(DbRequest request)
        {
            int operation = (int)request.Operation;
            byte* first = stackalloc byte[8];
            byte* second = stackalloc byte[8];
            BinaryPrimitives.WriteInt64BigEndian(new Span<byte>(first, 8), request.Operation == DbOperation.Post ? request.Id : request.UserId);
            BinaryPrimitives.WriteInt64BigEndian(new Span<byte>(second, 8), request.Id);
            byte** values = stackalloc byte*[2];
            int* lengths = stackalloc int[2];
            int* formats = stackalloc int[2];
            values[0] = first; values[1] = second;
            lengths[0] = lengths[1] = 8;
            formats[0] = formats[1] = 1;
            byte[]? rented = null;
            int bodyLength = request.Operation == DbOperation.Create ? Encoding.UTF8.GetByteCount(request.Body ?? throw new InvalidDataException("Missing body")) : 0;
            int capacity = Math.Max(1, bodyLength);
            Span<byte> body = capacity <= 4096 ? stackalloc byte[capacity] : (rented = ArrayPool<byte>.Shared.Rent(capacity));
            try
            {
                if (request.Operation == DbOperation.Create) Encoding.UTF8.GetBytes(request.Body!, body);
                fixed (byte* name = Names[operation])
                fixed (byte* text = body)
                {
                    if (request.Operation == DbOperation.Create) { values[1] = text; lengths[1] = bodyLength; }
                    return PgNative.PQsendQueryPrepared(handle, name, ParameterCounts[operation], values, lengths, formats, 1) == 1;
                }
            }
            finally
            {
                if (rented is not null) ArrayPool<byte>.Shared.Return(rented, clearArray: true);
            }
        }

        private void DrainResults(int budget = PipelineDepth * 4)
        {
            while (budget-- > 0 && state == ConnectionState.Ready && pending.Count > 0 && PgNative.PQisBusy(handle) == 0)
            {
                nint result = PgNative.PQgetResult(handle);
                var current = pending.Peek();
                if (result == 0)
                {
                    // NULL separates query results; it does NOT mean the pipeline
                    // is empty. Continue to the Sync result, without blocking.
                    if (current.Response is null || current.SawBoundary) { Fail(); return; }
                    current.SawBoundary = true;
                    continue;
                }
                bool sync = false;
                try
                {
                    int status = PgNative.PQresultStatus(result);
                    if (status == 10)
                    {
                        sync = true;
                        pending.Dequeue();
                    }
                    else if (current.Response is not null)
                        current.Response = Failure(current.Request);
                    else
                        current.Response = owner.Serialize(current.Request, result);
                }
                finally { PgNative.PQclear(result); }
                if (sync)
                {
                    // PGRES_PIPELINE_SYNC is the server's ReadyForQuery. Each
                    // request has its own implicit transaction and Sync. A returned
                    // INSERT row / CommandComplete alone never triggers success.
                    owner.Complete(current, current.Response ?? Failure(current.Request));
                    if (owner.disposed) return;
                }
            }
        }

        private void Flush()
        {
            int result = PgNative.PQflush(handle);
            if (result < 0) { Fail(); return; }
            RefreshSocket(result == 1);
        }

        private void RefreshSocket(bool wantsWrite, bool force = false)
        {
            int fd = PgNative.PQsocket(handle);
            if (fd < 0) { Fail(); return; }
            if (Fd != fd)
            {
                if (Fd >= 0)
                {
                    owner.sockets.Remove(Fd);
                    owner.unwatch(Fd);
                }
                Fd = fd;
                owner.sockets.Add(fd, this);
                force = true;
            }
            // libpq may replace a connection-attempt socket and get the same fd
            // number back. Force a fresh registration during connect polling;
            // otherwise a poller's fd/interest cache could retain a dead watch.
            else if (force) owner.unwatch(fd);
            if (force || WantsWrite != wantsWrite)
            {
                WantsWrite = wantsWrite;
                owner.watch(fd, wantsWrite);
            }
        }

        internal void Tick(long now)
        {
            if (state == ConnectionState.Disconnected)
            {
                if (now >= retryAt) StartConnect();
            }
            else if (Initializing && now - connectStarted >= TimeoutTicks) Fail();
            else if (Ready && pending.TryPeek(out var first))
            {
                if (now - first.Accepted >= TimeoutTicks) Fail();
                else DrainResults(); // Results already in libpq need not create another socket event.
            }
        }

        internal void Fail()
        {
            if (state == ConnectionState.Closed) return;
            owner.resets++;
            Detach();
            state = ConnectionState.Disconnected;
            retryAt = Stopwatch.GetTimestamp() + Stopwatch.Frequency;
            // No retries: an interrupted write may have committed even when its
            // response/Sync was lost. The caller receives failure, never false success.
            while (pending.TryDequeue(out var item)) owner.Complete(item, Failure(item.Request));
        }

        internal void Close()
        {
            if (state == ConnectionState.Closed) return;
            Detach();
            state = ConnectionState.Closed;
            while (pending.TryDequeue(out var item)) owner.Complete(item, Failure(item.Request));
        }

        private void Detach()
        {
            if (Fd >= 0)
            {
                owner.sockets.Remove(Fd);
                owner.unwatch(Fd);
                Fd = -1;
            }
            if (handle != 0)
            {
                PgNative.PQfinish(handle);
                handle = 0;
            }
            WantsWrite = false;
        }
    }
}



