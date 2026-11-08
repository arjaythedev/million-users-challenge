// A Postgres wire-protocol (v3) client that lives inside the epoll loops.
//
// Pg.Connect does the blocking part on a background thread: TCP connect, startup, authentication (SCRAM-SHA-256,
// MD5 or cleartext), and a Parse of each SPEC.md statement. The finished socket is then handed to a loop, which
// owns it from there: requests become Bind+Execute (binary parameters and results) appended to the connection's
// output buffer, and at the end of every loop pass one Sync closes the batch and one send() writes it.
//
// Postgres runs a batch as one implicit transaction. Reads are answered as soon as their CommandComplete arrives.
// Writes go to their own connections, and their answers wait for the batch's ReadyForQuery, i.e. its commit
// (group commit: one commit per batch, every response after it). After an error Postgres skips the rest of the
// batch: the failed request gets its error, and the others (skipped, or rolled back) are submitted again.
using System.Buffers.Binary;
using System.Net;
using System.Net.Sockets;
using System.Security.Cryptography;
using System.Text;
using static FeedServer.Native;

namespace FeedServer;

enum Stmt : byte { Feed, PostById, InsertPost, Like, Health }

// One request waiting on Postgres. Pooled per loop.
sealed class PgEntry
{
    public Stmt Stmt;
    public int Fd;
    public uint Gen;
    public bool KeepAlive, AcceptsGzip;
    public long A, B;          // int8 parameters
    public string? Text;       // POST /posts body (trimmed)
    public byte[]? Username;   // from the token, UTF-8
    public JsonWriter W;       // the response body being built
    public int Status, Rows;
    public bool EndsBatch;     // the last request before a Sync
    public long WaitingSince;  // when it first found no live connection (0: never)

    public bool IsWrite => Stmt is Stmt.InsertPost or Stmt.Like;

    public void ResetResult()
    {
        Db.Return(ref W);
        Status = Rows = 0;
        EndsBatch = false;
    }

    public void Clear()
    {
        ResetResult();
        Text = null;
        Username = null;
        WaitingSince = 0;
    }
}

// Columns of a binary DataRow, read in order.
ref struct PgRow(ReadOnlySpan<byte> row)
{
    readonly ReadOnlySpan<byte> d = row;
    int pos = 2; // skip the column count

    public ReadOnlySpan<byte> Bytes()
    {
        int len = BinaryPrimitives.ReadInt32BigEndian(d[pos..]);
        pos += 4;
        if (len < 0) return default;
        var s = d.Slice(pos, len);
        pos += len;
        return s;
    }

    public long Int64() => BinaryPrimitives.ReadInt64BigEndian(Bytes());
    public int Int32() => BinaryPrimitives.ReadInt32BigEndian(Bytes());

    // timestamptz: microseconds since 2000-01-01 UTC
    public DateTime Timestamp() => new(630822816000000000L + Int64() * 10, DateTimeKind.Utc);
}

static class Pg
{
    public static string Host = "", User = "", Password = "", Database = "";
    public static int Port = 5432;
    public static volatile string LastError = "connecting";

    // name, SQL, parameter type OIDs (20 = int8, 25 = text)
    static readonly (byte Name, string Sql, int[] Oids)[] Statements =
    [
        ((byte)'f', Db.FeedSql, []),
        ((byte)'p', Db.PostByIdSql, [20]),
        ((byte)'i', Db.InsertPostSql, [20, 25]),
        ((byte)'l', Db.LikeSql, [20, 20]),
        ((byte)'h', "SELECT 1", []),
    ];

    public static byte Name(Stmt s) => Statements[(int)s].Name;

    // DATABASE_URL: postgres://user[:password]@host[:port]/database[?options]
    public static void Configure(string databaseUrl)
    {
        string s = databaseUrl[(databaseUrl.IndexOf("://", StringComparison.Ordinal) + 3)..];
        int q = s.IndexOf('?');
        if (q >= 0) s = s[..q];
        int at = s.LastIndexOf('@');
        string credentials = at < 0 ? "" : s[..at], location = s[(at + 1)..];
        int colon = credentials.IndexOf(':');
        User = Uri.UnescapeDataString(colon < 0 ? credentials : credentials[..colon]);
        Password = colon < 0 ? "" : Uri.UnescapeDataString(credentials[(colon + 1)..]);
        int slash = location.IndexOf('/');
        Database = slash < 0 ? User : Uri.UnescapeDataString(location[(slash + 1)..]);
        string hostPort = slash < 0 ? location : location[..slash];
        int portAt = hostPort.LastIndexOf(':');
        Host = portAt < 0 ? hostPort : hostPort[..portAt];
        Port = portAt < 0 ? 5432 : int.Parse(hostPort[(portAt + 1)..]);
    }

    // Connects in the background and hands the connection to the loop; retries every second until it works.
    public static void ConnectLater(EventLoop loop, int slot, bool forWrites, int delayMs)
    {
        Task.Run(async () =>
        {
            if (delayMs > 0) await Task.Delay(delayMs);
            try
            {
                int fd = Connect();
                loop.OnPgConnected(new PgConn(loop, slot, fd, forWrites));
            }
            catch (Exception ex)
            {
                LastError = ex.Message;
                Log.Error(ex);
                ConnectLater(loop, slot, forWrites, 1000);
            }
        });
    }

    // ---- the blocking handshake ----

    static unsafe int Connect()
    {
        var ip = IPAddress.TryParse(Host, out var parsed) ? parsed
            : Dns.GetHostAddresses(Host).First(a => a.AddressFamily == AddressFamily.InterNetwork);
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) throw new IOException($"socket: errno {Errno()}");
        try
        {
            SetTimeouts(fd, 5);
            var sa = new SockAddrIn { Family = AF_INET, Port = Htons(Port), Addr = BitConverter.ToUInt32(ip.GetAddressBytes()) };
            if (connect(fd, &sa, (uint)sizeof(SockAddrIn)) != 0)
                throw new IOException($"connect to {Host}:{Port} failed: errno {Errno()}");
            int one = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, 4);

            var q = new ByteFifo(4096);
            Message(q, '\0', 256 + 3 * (User.Length + Database.Length), (ref BeWriter w) =>
            {
                w.U32(3 << 16); // protocol 3.0
                w.Z("user"); w.Z(User);
                w.Z("database"); w.Z(Database);
                w.Z("application_name"); w.Z("csharp-epoll");
                w.U8(0);
            });
            Transmit(fd, q);
            Authenticate(fd, q);

            // Prepare every statement (named by one letter), then one Sync for the lot.
            foreach (var (name, sql, oids) in Statements)
                Message(q, 'P', 16 + 3 * sql.Length + 4 * oids.Length, (ref BeWriter w) =>
                {
                    w.U8(name); w.U8(0);
                    w.Z(sql);
                    w.U16(oids.Length);
                    foreach (int oid in oids) w.U32(oid);
                });
            Message(q, 'S', 0, (ref BeWriter w) => { });
            Transmit(fd, q);
            for (var (type, body) = Read(fd); type != 'Z'; (type, body) = Read(fd))
                if (type == 'E') throw new IOException("prepare: " + ErrorMessage(body));

            SetTimeouts(fd, 0);
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
            return fd;
        }
        catch
        {
            close(fd);
            throw;
        }
    }

    // Logs in with whatever the server asks for: trust, cleartext, MD5 or SCRAM-SHA-256.
    static void Authenticate(int fd, ByteFifo q)
    {
        Scram? scram = null;
        while (true)
        {
            var (type, body) = Read(fd);
            if (type == 'E') throw new IOException(ErrorMessage(body));
            if (type == 'Z') return;   // ReadyForQuery: logged in
            if (type != 'R') continue; // parameter values, the cancel key, notices
            int method = BinaryPrimitives.ReadInt32BigEndian(body);
            string text = Encoding.UTF8.GetString(body, 4, body.Length - 4);
            if (method == 0) continue; // AuthenticationOk
            if (method == 3) PasswordMessage(q, Encoding.UTF8.GetBytes(Password), true);
            else if (method == 5)
            {
                // "md5" + hex(md5(hex(md5(password + user)) + 4-byte salt))
                byte[] inner = Encoding.ASCII.GetBytes(Convert.ToHexStringLower(MD5.HashData(Encoding.UTF8.GetBytes(Password + User))));
                byte[] outer = MD5.HashData([.. inner, .. body.AsSpan(4, 4)]);
                PasswordMessage(q, Encoding.ASCII.GetBytes("md5" + Convert.ToHexStringLower(outer)), true);
            }
            else if (method == 10)
            {
                if (Array.IndexOf(text.Split('\0'), "SCRAM-SHA-256") < 0) throw new IOException("server offers no SCRAM-SHA-256");
                scram = new Scram(Password);
                byte[] first = Encoding.UTF8.GetBytes(scram.ClientFirst);
                Message(q, 'p', 32 + first.Length, (ref BeWriter w) => { w.Z("SCRAM-SHA-256"); w.U32(first.Length); w.Raw(first); });
            }
            else if (method == 11 && scram is not null) PasswordMessage(q, Encoding.UTF8.GetBytes(scram.ClientFinal(text)), false);
            else if (method == 12 && scram is not null) { scram.CheckServerFinal(text); continue; }
            else throw new IOException($"unsupported authentication method {method}");
            Transmit(fd, q);
        }
    }

    static void PasswordMessage(ByteFifo q, byte[] data, bool terminate) =>
        Message(q, 'p', data.Length + 1, (ref BeWriter w) => { w.Raw(data); if (terminate) w.U8(0); });

    delegate void Fill(ref BeWriter w);

    // Appends one message: the type byte (the startup message has none), the length, then what fill writes.
    static void Message(ByteFifo q, char type, int room, Fill fill)
    {
        var w = new BeWriter(q.Reserve(room + 5));
        if (type != '\0') w.U8(type);
        int length = w.Mark();
        fill(ref w);
        w.SetLength(length);
        q.Commit(w.Pos);
    }

    static unsafe void Transmit(int fd, ByteFifo q)
    {
        while (q.Count > 0)
        {
            nint n;
            fixed (byte* p = q.Data) n = send_blocking(fd, p + q.Head, (nuint)q.Count, MSG_NOSIGNAL);
            if (n <= 0) throw new IOException($"send to Postgres failed: errno {Errno()}");
            q.Consume((int)n);
        }
    }

    static unsafe void Receive(int fd, Span<byte> into)
    {
        fixed (byte* p = into)
            for (int got = 0; got < into.Length;)
            {
                nint n = recv_blocking(fd, p + got, (nuint)(into.Length - got), 0);
                if (n == 0) throw new IOException("Postgres closed the connection");
                if (n < 0) throw new IOException($"recv from Postgres failed: errno {Errno()}");
                got += (int)n;
            }
    }

    static (byte Type, byte[] Body) Read(int fd)
    {
        Span<byte> head = stackalloc byte[5];
        Receive(fd, head);
        int length = BinaryPrimitives.ReadInt32BigEndian(head[1..]);
        if (length is < 4 or > 1 << 20) throw new IOException("bad message from Postgres");
        var body = new byte[length - 4];
        Receive(fd, body);
        return (head[0], body);
    }

    // ErrorResponse: fields of (type byte, NUL-terminated value), ended by a 0 byte. C = SQLSTATE, M = message.
    public static void ParseError(ReadOnlySpan<byte> body, out ReadOnlySpan<byte> sqlState, out ReadOnlySpan<byte> message)
    {
        sqlState = message = default;
        for (int i = 0; i < body.Length && body[i] != 0;)
        {
            byte field = body[i];
            int len = body[(i + 1)..].IndexOf((byte)0);
            if (len < 0) return;
            if (field == 'C') sqlState = body.Slice(i + 1, len);
            if (field == 'M') message = body.Slice(i + 1, len);
            i += len + 2;
        }
    }

    static string ErrorMessage(byte[] body)
    {
        ParseError(body, out var state, out var message);
        return $"{Encoding.UTF8.GetString(message)} (SQLSTATE {Encoding.ASCII.GetString(state)})";
    }
}

// SCRAM-SHA-256 (RFC 5802, RFC 7677) the way Postgres uses it: no channel binding, and an empty user name in the
// messages because the startup message already named the user.
sealed class Scram(string password)
{
    readonly string clientNonce = Convert.ToBase64String(RandomNumberGenerator.GetBytes(24));
    byte[] serverSignature = [];

    string FirstBare => "n=,r=" + clientNonce;
    public string ClientFirst => "n,," + FirstBare;

    public string ClientFinal(string serverFirst)
    {
        string? nonce = null, salt = null;
        int rounds = 0;
        foreach (var attr in serverFirst.Split(','))
        {
            if (attr.StartsWith("r=")) nonce = attr[2..];
            else if (attr.StartsWith("s=")) salt = attr[2..];
            else if (attr.StartsWith("i=")) rounds = int.Parse(attr[2..]);
        }
        if (nonce is null || salt is null || rounds <= 0 || !nonce.StartsWith(clientNonce, StringComparison.Ordinal))
            throw new IOException("bad SCRAM server-first message");
        byte[] salted = Rfc2898DeriveBytes.Pbkdf2(Encoding.UTF8.GetBytes(password), Convert.FromBase64String(salt), rounds,
            HashAlgorithmName.SHA256, 32);
        string withoutProof = "c=biws,r=" + nonce;
        byte[] authMessage = Encoding.UTF8.GetBytes(FirstBare + "," + serverFirst + "," + withoutProof);
        byte[] clientKey = HMACSHA256.HashData(salted, "Client Key"u8);
        byte[] proof = HMACSHA256.HashData(SHA256.HashData(clientKey), authMessage);
        for (int i = 0; i < proof.Length; i++) proof[i] ^= clientKey[i];
        serverSignature = HMACSHA256.HashData(HMACSHA256.HashData(salted, "Server Key"u8), authMessage);
        return withoutProof + ",p=" + Convert.ToBase64String(proof);
    }

    public void CheckServerFinal(string serverFinal)
    {
        string v = serverFinal.Split(',')[0];
        if (!v.StartsWith("v=") || !CryptographicOperations.FixedTimeEquals(Convert.FromBase64String(v[2..]), serverSignature))
            throw new IOException("SCRAM server signature mismatch");
    }
}

// A growable byte queue: append at Tail (Reserve, then Commit), take from Head (Consume). Live bytes move back to
// the start of the array only when the tail runs out of room.
sealed class ByteFifo(int capacity)
{
    public byte[] Data = new byte[capacity];
    public int Head, Tail;
    public int Count => Tail - Head;

    public Span<byte> Reserve(int bytes)
    {
        if (Data.Length - Tail < bytes)
        {
            int live = Count;
            var to = live + bytes <= Data.Length ? Data : new byte[Math.Max(Data.Length * 2, live + bytes)];
            Data.AsSpan(Head, live).CopyTo(to);
            Data = to;
            Head = 0;
            Tail = live;
        }
        return Data.AsSpan(Tail);
    }

    public void Commit(int bytes) => Tail += bytes;

    public void Consume(int bytes)
    {
        Head += bytes;
        if (Head == Tail) Head = Tail = 0;
    }
}

// Writes Postgres's big-endian integers and NUL-terminated strings into a span; Pos is the next free byte.
ref struct BeWriter(Span<byte> to)
{
    readonly Span<byte> s = to;
    public int Pos;

    public void U8(int v) => s[Pos++] = (byte)v;
    public void U16(int v) { BinaryPrimitives.WriteUInt16BigEndian(s[Pos..], (ushort)v); Pos += 2; }
    public void U32(int v) { BinaryPrimitives.WriteInt32BigEndian(s[Pos..], v); Pos += 4; }
    public void U64(long v) { BinaryPrimitives.WriteInt64BigEndian(s[Pos..], v); Pos += 8; }
    public void Raw(ReadOnlySpan<byte> v) { v.CopyTo(s[Pos..]); Pos += v.Length; }
    public void Utf8(string v) => Pos += Encoding.UTF8.GetBytes(v, s[Pos..]);
    public void Z(string v) { Utf8(v); U8(0); }

    // A 4-byte length to fill in later: SetLength counts itself (message lengths), SetSize doesn't (values).
    public int Mark() { Pos += 4; return Pos - 4; }
    public readonly void SetLength(int mark) => BinaryPrimitives.WriteInt32BigEndian(s[mark..], Pos - mark);
    public readonly void SetSize(int mark) => BinaryPrimitives.WriteInt32BigEndian(s[mark..], Pos - mark - 4);
}

// One Postgres connection owned by one loop (only that loop's thread touches it after the handshake).
sealed unsafe class PgConn(EventLoop loop, int slot, int fd, bool forWrites)
{
    public readonly int Slot = slot, Fd = fd;
    public readonly bool ForWrites = forWrites;
    readonly EventLoop loop = loop;

    readonly ByteFifo output = new(16 * 1024), input = new(64 * 1024);

    readonly List<PgEntry> unsent = [];         // encoded, not yet closed by a Sync
    readonly Queue<PgEntry> inflight = new();    // sent, in order
    readonly List<PgEntry> committing = [];      // finished writes of the current batch, waiting for its commit
    bool batchFailed, batchEndSeen;
    public bool Dirty, WantWrite;
    public string? FailReason;   // why a call returned false (the connection is broken), for the log
    long lastProgress;   // when Postgres last sent us something, or when it was last given work while idle

    public int Load => unsent.Count + inflight.Count;
    public int Unsent => unsent.Count;
    public int Inflight => inflight.Count;

    // Postgres owes us answers but has sent nothing for timeoutMs: the backend or the database box is stuck.
    public bool Stalled(long now, long timeoutMs) =>
        (inflight.Count > 0 || committing.Count > 0) && now - lastProgress > timeoutMs;

    // Bind (unnamed portal, prepared statement, binary parameters and results) + Execute (all rows).
    public void Encode(PgEntry e)
    {
        int room = 64 + (e.Text is null ? 0 : Encoding.UTF8.GetMaxByteCount(e.Text.Length));
        var w = new BeWriter(output.Reserve(room));
        w.U8('B');
        int bind = w.Mark();
        w.U8(0);
        w.U8(Pg.Name(e.Stmt)); w.U8(0);
        switch (e.Stmt)
        {
            case Stmt.PostById:
                w.U16(1); w.U16(1); w.U16(1);    // one format for all: binary; one parameter
                Int8(ref w, e.A);
                break;
            case Stmt.Like:
                w.U16(1); w.U16(1); w.U16(2);
                Int8(ref w, e.A);
                Int8(ref w, e.B);
                break;
            case Stmt.InsertPost:
                w.U16(1); w.U16(1); w.U16(2);
                Int8(ref w, e.A);
                int text = w.Mark();
                w.Utf8(e.Text!);
                w.SetSize(text);
                break;
            default:
                w.U16(0); w.U16(0);              // no parameters
                break;
        }
        w.U16(1); w.U16(1);                      // results: binary
        w.SetLength(bind);
        w.U8('E'); w.U32(9); w.U8(0); w.U32(0);
        output.Commit(w.Pos);
        unsent.Add(e);

        static void Int8(ref BeWriter w, long v) { w.U32(8); w.U64(v); }
    }

    // Closes the batch with a Sync and writes everything out. False: the connection is broken.
    public bool Flush()
    {
        if (unsent.Count > 0)
        {
            var w = new BeWriter(output.Reserve(5));
            w.U8('S'); w.U32(4);
            output.Commit(w.Pos);
            unsent[^1].EndsBatch = true;
            if (inflight.Count == 0) lastProgress = Environment.TickCount64;
            foreach (var e in unsent) inflight.Enqueue(e);
            unsent.Clear();
        }
        return SendOut();
    }

    public bool SendOut()
    {
        while (output.Count > 0)
        {
            nint n;
            fixed (byte* p = output.Data) n = send(Fd, p + output.Head, (nuint)output.Count, MSG_NOSIGNAL);
            if (n > 0) { output.Consume((int)n); continue; }
            int err = n < 0 ? Errno() : 0;
            if (err == EAGAIN)
            {
                if (!WantWrite) { WantWrite = true; loop.SetPgEvents(this, EPOLLIN | EPOLLOUT | EPOLLRDHUP); }
                return true;
            }
            return Fail($"send: errno {err}");
        }
        if (WantWrite) { WantWrite = false; loop.SetPgEvents(this, EPOLLIN | EPOLLRDHUP); }
        return true;
    }

    // Reads what Postgres sent and handles every complete message. False: the connection is broken.
    public bool OnReadable()
    {
        while (true)
        {
            var room = input.Reserve(32 * 1024);
            nint n;
            fixed (byte* p = room) n = recv(Fd, p, (nuint)room.Length, 0);
            if (n == 0) return Fail("closed by Postgres");
            if (n < 0)
            {
                int err = Errno();
                return err == EAGAIN || Fail($"recv: errno {err}");
            }
            input.Commit((int)n);
            lastProgress = Environment.TickCount64;
            if (!HandleBuffered()) return false;
            if (n < room.Length) return true;
        }
    }

    // Every complete message in the input: type byte, then a length that counts itself but not the type.
    bool HandleBuffered()
    {
        while (input.Count >= 5)
        {
            int length = BinaryPrimitives.ReadInt32BigEndian(input.Data.AsSpan(input.Head + 1, 4));
            byte type = input.Data[input.Head];
            if (length is < 4 or > 64 << 20) return Fail($"bad message length {length} (type {(char)type})");
            if (input.Count <= length) return true;
            bool ok = Handle(type, input.Data.AsSpan(input.Head + 5, length - 4));
            input.Consume(length + 1);
            if (!ok) return false;
        }
        return true;
    }

    bool Handle(byte type, ReadOnlySpan<byte> body)
    {
        PgEntry? e;
        switch (type)
        {
            case (byte)'D': // DataRow
                if (!inflight.TryPeek(out e)) return Fail("DataRow with nothing in flight");
                Api.OnRow(e, body);
                return true;
            case (byte)'C': // CommandComplete
            {
                if (!inflight.TryDequeue(out e)) return Fail("CommandComplete with nothing in flight");
                if (e.EndsBatch) batchEndSeen = true;
                Api.OnComplete(e, body);
                if (e.IsWrite) committing.Add(e);
                else loop.Deliver(e);
                return true;
            }
            case (byte)'E': // ErrorResponse: this request failed; Postgres skips the rest of the batch
            {
                Pg.ParseError(body, out var state, out var message);
                // SQLSTATE class 08 (connection exception) and 57P (server shutting down, backend terminated) end
                // the session, not just this request: reconnect, and the requests in flight go out again
                if (state.StartsWith("08"u8) || state.StartsWith("57P"u8))
                    return Fail("Postgres " + Encoding.ASCII.GetString(state) + ": " + Encoding.UTF8.GetString(message));
                // Every request of this batch has its answer, so the error is the batch's commit at the Sync, not
                // a request's (the next request in flight belongs to the next batch): the batch rolled back, and
                // ReadyForQuery submits its writes again.
                if (batchEndSeen)
                {
                    batchFailed = true;
                    Log.Error(new IOException("Postgres commit: " + Encoding.UTF8.GetString(message)));
                    return true;
                }
                if (!inflight.TryDequeue(out e)) return Fail("ErrorResponse with nothing in flight: " + Encoding.UTF8.GetString(message));
                if (e.EndsBatch) batchEndSeen = true;
                batchFailed = true;
                Api.OnError(e, state, message);
                loop.Deliver(e);
                return true;
            }
            case (byte)'Z': // ReadyForQuery: the batch is over (committed, or rolled back after an error)
            {
                if (!batchFailed)
                {
                    foreach (var w in committing) loop.Deliver(w);
                    committing.Clear();
                }
                else
                {
                    foreach (var w in committing) loop.Requeue(w); // rolled back with the batch
                    committing.Clear();
                    while (!batchEndSeen && inflight.TryDequeue(out e)) // skipped after the error
                    {
                        batchEndSeen = e.EndsBatch;
                        loop.Requeue(e);
                    }
                }
                batchFailed = batchEndSeen = false;
                return true;
            }
            default: // BindComplete, ParseComplete, NoData, ParameterStatus, NoticeResponse, ...
                return true;
        }
    }

    bool Fail(string why)
    {
        FailReason = why;
        return false;
    }

    // The socket's pending error (SO_ERROR), after EPOLLERR.
    public string SocketError()
    {
        int err = 0;
        uint len = 4;
        return getsockopt(Fd, SOL_SOCKET, SO_ERROR, &err, &len) == 0 ? $"errno {err}" : $"(getsockopt: errno {Errno()})";
    }

    // The connection is gone: writes whose fate is unknown get a 500, everything else is submitted again.
    public void FailAll()
    {
        foreach (var e in committing) { Api.OnUnavailable(e); loop.Deliver(e); }
        committing.Clear();
        while (inflight.TryDequeue(out var e))
        {
            if (e.IsWrite) { Api.OnUnavailable(e); loop.Deliver(e); }
            else loop.Requeue(e);
        }
        var pending = unsent.ToArray();
        unsent.Clear();
        foreach (var e in pending) loop.Requeue(e);
    }
}
