using System;
using System.Buffers.Binary;
using System.Collections.Generic;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;
using System.Text;

/// <summary>Where the database is: a <c>postgres://user:password@host:port/db</c> URL.</summary>
sealed record PgSettings(string Host, int Port, string User, string Password, string Database)
{
    public static PgSettings Parse(string url)
    {
        var uri = new Uri(url);
        string[] userInfo = uri.UserInfo.Split(':', 2);
        return new PgSettings(uri.Host, uri.Port > 0 ? uri.Port : 5432, Uri.UnescapeDataString(userInfo[0]),
            userInfo.Length > 1 ? Uri.UnescapeDataString(userInfo[1]) : "", Uri.UnescapeDataString(uri.AbsolutePath.TrimStart('/')));
    }
}

/// <summary>A read waiting for its reply: the connection it answers (fd and generation), what it asked, and whether
/// it is the last read before its batch's Sync.</summary>
struct Waiter
{
    public int Fd, Gen;
    public byte Kind;
    public bool Last;
    public long Id;
}

/// <summary>A write waiting for its group's commit, with what its reply needs.</summary>
sealed class WriteJob
{
    public const byte Create = 0, Like = 1;
    public int Fd, Gen;
    public byte Kind;
    public long UserId, PostId;
    public readonly byte[] Body = new byte[2048]; // 500 characters of up to 4 UTF-8 bytes each
    public int BodyLength;
    public readonly byte[] Name = new byte[Jwt.MaxNameBytes];
    public int NameLength;
    // the outcome, once its group committed
    public long NewId, CreatedAt;
    public int Inserted;
}

/// <summary>
/// One Postgres connection, owned by one loop: the spec's statements prepared once as it opens, then reads sent as
/// Bind and Execute, a batch of them under one Sync (one ReadyForQuery, one flush on the server), pipelined; and each
/// group of writes as Binds and Executes under one Sync: one transaction, one commit. Parameters are binary; a post's
/// columns come in whichever format is shorter (<see cref="PostColumnFormats"/>).
/// </summary>
sealed class Pg
{
    public const byte Health = (byte)'h', Feed = (byte)'f', Post = (byte)'p', CreatePost = (byte)'c', LikePost = (byte)'l';
    /// <summary>The most reads under one Sync. A loop pass's reads go to one reader until it holds this many.</summary>
    public const int MaxBatch = 64;
    /// <summary>How long a connection that owes replies may stay silent before it is nudged with a Sync, in
    /// milliseconds; doubled after each nudge until it answers. A reply normally takes a few milliseconds, so silence
    /// this long means a lost packet, and the retransmit timer behind it runs 200 ms or more.</summary>
    const int NudgeAfterMs = 50;
    /// <summary>How long a connect, and each answer of the login, may take.</summary>
    const int LoginTimeoutMs = 2_000;
    /// <summary>How long a dropped connection waits before it reconnects.</summary>
    const int ReconnectDelayMs = 1000;

    // the spec's SQL, as it writes it
    static readonly (byte Name, string Sql, uint[] Types)[] Statements =
    [
        (Health, "SELECT 1", []),
        (Feed, """
            SELECT p.id, p.body, p.created_at, u.username,
                   (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
              FROM posts p JOIN users u ON u.id = p.user_id
             ORDER BY p.created_at DESC, p.id DESC LIMIT 20
            """, []),
        (Post, """
            SELECT p.id, p.body, p.created_at, u.username,
                   (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
              FROM posts p JOIN users u ON u.id = p.user_id
             WHERE p.id = $1
            """, [20]),
        (CreatePost, "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at", [20, 25]),
        (LikePost, "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING", [20, 20]),
    ];

    /// <summary>The result formats of a post's columns, chosen for the fewest bytes, since Postgres's replies are most of
    /// what the box takes in and its network is what runs out first: id and like_count as text (six digits and one
    /// or two against 8 and 4 bytes), created_at binary (8 bytes against 29), body and username text (the same bytes
    /// either way). About 5 of a feed row's 135 bytes.</summary>
    static ReadOnlySpan<byte> PostColumnFormats => [0, 5, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0];

    static ReadOnlySpan<byte> ExecuteMessage => [(byte)'E', 0, 0, 0, 9, 0, 0, 0, 0, 0];
    static ReadOnlySpan<byte> SyncMessage => [(byte)'S', 0, 0, 0, 4];

    readonly PgSettings _settings;
    public int Fd = -1;
    public bool Broken = true;
    /// <summary>Between <see cref="BeginOpen"/> and <see cref="FinishOpen"/>; still <see cref="Broken"/>.</summary>
    public bool Connecting;
    public long ConnectDeadline;
    public long RetryAt;
    IPAddress? _address;

    byte[] _out = new byte[64 << 10];
    // [_outSent, _sealed) goes with the next flush; [_sealed, _outLength) is a batch still open (no Sync yet)
    int _outLength, _outSent, _sealed;
    byte[] _in = new byte[256 << 10];
    int _inStart, _inEnd;

    /// <summary>The reads sent or queued, oldest first, as a ring that doubles when full; their replies come in this
    /// order.</summary>
    Waiter[] _waiters = new Waiter[1024];
    int _waiterHead, _waiterCount;
    /// <summary>The writes of the group in flight, in the order they were sent.</summary>
    public readonly List<WriteJob> Group = [];

    /// <summary>Reads queued since the last Sync.</summary>
    public int Open;
    /// <summary>A writer's Syncs whose ReadyForQuery hasn't come back, oldest first: true for a group of writes, false
    /// for a nudge. A reader needs none: its results end at each read's own CommandComplete.</summary>
    readonly Queue<bool> _syncs = new();
    /// <summary>When the connection began waiting for what it is owed: when it last heard from Postgres, was last
    /// nudged, or sent work with nothing owed (a Stopwatch timestamp).</summary>
    long _waitingSince;
    /// <summary>How long it may stay silent before the next nudge, in Stopwatch ticks.</summary>
    long _nudgeAfter = NudgeTicks;
    static readonly long NudgeTicks = NudgeAfterMs * Stopwatch.Frequency / 1000;

    /// <summary>A writer carries groups of writes, one at a time; a reader carries batches of reads.</summary>
    readonly bool _writer;

    public Pg(PgSettings settings, bool writer)
    {
        _settings = settings;
        _writer = writer;
    }

    public int Pending => _waiterCount;
    /// <summary>Work has been sent whose reply hasn't come: reads past the open batch, or a group of writes.</summary>
    public bool Owes => _waiterCount > Open || Group.Count > 0;
    public bool WantsWrite => _outSent < _sealed;
    /// <summary>What the next flush sends.</summary>
    public ReadOnlySpan<byte> Unsent => _out.AsSpan(_outSent, _sealed - _outSent);

    /// <summary>Starts connecting without blocking: the event loop watches <see cref="Fd"/> for writable and then calls
    /// <see cref="FinishOpen"/>, or gives up at <see cref="ConnectDeadline"/>. A host that doesn't answer never holds
    /// the loop.</summary>
    public void BeginOpen()
    {
        if (_address == null)
        {
            IPAddress[] all = Dns.GetHostAddresses(_settings.Host);
            _address = Array.Find(all, a => a.AddressFamily == AddressFamily.InterNetwork) ?? all[0];
        }
        int fd = Native.Connect(_address, _settings.Port);
        if (fd < 0)
        {
            _address = null;
            throw new SocketException(-fd);
        }
        Fd = fd;
        Connecting = true;
        ConnectDeadline = Environment.TickCount64 + LoginTimeoutMs;
    }

    /// <summary>Once connected: logs in and prepares the statements, waiting two seconds at most for each answer.</summary>
    public void FinishOpen()
    {
        Connecting = false;
        int error = Native.SocketError(Fd);
        if (error != 0)
        {
            _address = null;
            throw new SocketException(error);
        }
        Startup(Fd);
        _outLength = _outSent = 0;
        foreach (var (name, sql, types) in Statements)
        {
            int start = StartMessage((byte)'P');
            PutByte(name);
            PutByte(0);
            PutBytes(Encoding.UTF8.GetBytes(sql));
            PutByte(0);
            PutInt16((short)types.Length);
            foreach (uint t in types) PutInt32((int)t);
            EndMessage(start);
        }
        PutBytes(SyncMessage);
        SendAll(Fd, _out, _outLength);
        _outLength = _sealed = 0;
        while (true)
        {
            byte type = ReadBlocking(Fd, out byte[] body);
            if (type == 'E') throw new InvalidOperationException("Postgres: " + ErrorText(body));
            if (type == 'Z') break;
        }
        _inStart = _inEnd = 0;
        Broken = false;
    }

    void Startup(int fd)
    {
        var m = new List<byte>();
        void Text(string v)
        {
            m.AddRange(Encoding.UTF8.GetBytes(v));
            m.Add(0);
        }
        m.AddRange([0, 0, 0, 0, 0, 3, 0, 0]); // length, then protocol 3.0
        Text("user"); Text(_settings.User);
        Text("database"); Text(_settings.Database);
        Text("client_encoding"); Text("UTF8");
        m.Add(0);
        byte[] startup = [.. m];
        BinaryPrimitives.WriteInt32BigEndian(startup, startup.Length);
        SendAll(fd, startup, startup.Length);
        Scram? scram = null;
        while (true)
        {
            byte type = ReadBlocking(fd, out byte[] body);
            switch (type)
            {
                case (byte)'E':
                    throw new InvalidOperationException("Postgres: " + ErrorText(body));
                case (byte)'Z':
                    return;
                case (byte)'R':
                    int code = BinaryPrimitives.ReadInt32BigEndian(body);
                    switch (code)
                    {
                        case 0: break; // AuthenticationOk
                        case 3: // cleartext password
                            SendBlocking(fd, (byte)'p', [.. Encoding.UTF8.GetBytes(_settings.Password), 0]);
                            break;
                        case 10: // SASL: SCRAM-SHA-256
                            scram = new Scram(_settings.Password);
                            byte[] first = scram.ClientFirst();
                            byte[] initial = [.. "SCRAM-SHA-256"u8, 0, 0, 0, 0, 0, .. first];
                            BinaryPrimitives.WriteInt32BigEndian(initial.AsSpan(14), first.Length);
                            SendBlocking(fd, (byte)'p', initial);
                            break;
                        case 11: // SASLContinue
                            SendBlocking(fd, (byte)'p', scram!.ClientFinal(body.AsSpan(4)));
                            break;
                        case 12: // SASLFinal
                            scram!.VerifyServerFinal(body.AsSpan(4));
                            break;
                        default:
                            throw new InvalidOperationException($"Postgres asks for authentication method {code}, which this client doesn't speak");
                    }
                    break;
            }
        }
    }

    static void SendBlocking(int fd, byte type, byte[] body)
    {
        byte[] m = new byte[5 + body.Length];
        m[0] = type;
        BinaryPrimitives.WriteInt32BigEndian(m.AsSpan(1), 4 + body.Length);
        body.CopyTo(m, 5);
        SendAll(fd, m, m.Length);
    }

    static void SendAll(int fd, byte[] bytes, int count)
    {
        for (int at = 0; at < count;)
        {
            int n = Native.Send(fd, bytes, at, count - at);
            if (n < 0) throw new InvalidOperationException("Postgres: the connection failed");
            if (n == 0 && !Native.Wait(fd, write: true, LoginTimeoutMs)) throw new TimeoutException("Postgres didn't take the login in time");
            at += n;
        }
    }

    static byte ReadBlocking(int fd, out byte[] body)
    {
        byte[] head = new byte[5];
        Fill(fd, head);
        body = new byte[BinaryPrimitives.ReadInt32BigEndian(head.AsSpan(1)) - 4];
        Fill(fd, body);
        return head[0];
    }

    static void Fill(int fd, byte[] into)
    {
        for (int at = 0; at < into.Length;)
        {
            int n = Native.Receive(fd, into, at, into.Length - at);
            if (n == 0) throw new InvalidOperationException("Postgres closed the connection");
            if (n == -2) throw new InvalidOperationException("Postgres: the connection failed");
            if (n == -1)
            {
                if (!Native.Wait(fd, write: false, LoginTimeoutMs)) throw new TimeoutException("Postgres didn't answer the login in time");
                continue;
            }
            at += n;
        }
    }

    /// <summary>The M (message) field of an ErrorResponse.</summary>
    public static string ErrorText(ReadOnlySpan<byte> body) => Encoding.UTF8.GetString(ErrorField(body, (byte)'M'));

    /// <summary>One field of an ErrorResponse (C is the SQLSTATE, M the message); empty when it has none.</summary>
    public static ReadOnlySpan<byte> ErrorField(ReadOnlySpan<byte> body, byte field)
    {
        while (body.Length > 1 && body[0] != 0)
        {
            int end = body[1..].IndexOf((byte)0) + 1;
            if (end <= 0) break;
            if (body[0] == field) return body[1..end];
            body = body[(end + 1)..];
        }
        return default;
    }

    /// <summary>Closes the socket; the replies it owed are the caller's to fail first (<see cref="TakeWaiter"/>, <see cref="Group"/>).</summary>
    public void Close()
    {
        Broken = true;
        Connecting = false;
        RetryAt = Environment.TickCount64 + ReconnectDelayMs;
        if (Fd >= 0) Native.close(Fd);
        Fd = -1;
        _outLength = _outSent = _sealed = _inStart = _inEnd = 0;
        Open = 0;
        _syncs.Clear();
        _nudgeAfter = NudgeTicks;
    }

    // ---- reads

    /// <summary>Queues a read's Bind and Execute in the open batch; <see cref="Seal"/> ends the batch.</summary>
    public void Read(byte statement, long id, int fd, int gen)
    {
        int start = StartMessage((byte)'B');
        PutByte(0); // unnamed portal
        PutByte(statement);
        PutByte(0);
        PutInt16(1); PutInt16(1); // every parameter binary
        if (statement == Post)
        {
            PutInt16(1);
            PutInt32(8); PutInt64(id);
        }
        else PutInt16(0);
        if (statement == Health)
        {
            PutInt16(1); PutInt16(1); // the one column binary
        }
        else PutBytes(PostColumnFormats);
        EndMessage(start);
        PutBytes(ExecuteMessage);
        Open++;
        if (_waiterCount == _waiters.Length)
        {
            var grown = new Waiter[_waiterCount * 2];
            for (int i = 0; i < _waiterCount; i++) grown[i] = _waiters[(_waiterHead + i) % _waiterCount];
            _waiters = grown;
            _waiterHead = 0;
        }
        _waiters[(_waiterHead + _waiterCount++) % _waiters.Length] = new Waiter { Fd = fd, Gen = gen, Kind = statement, Id = id };
    }

    /// <summary>Ends the open batch with one Sync; it goes with the next flush. Its reads share one implicit
    /// transaction (each still takes its own snapshot), and an error skips the reads after it up to the Sync.</summary>
    public void Seal(long now)
    {
        if (Open == 0) return;
        if (!Owes) _waitingSince = now;
        PutBytes(SyncMessage);
        _waiters[(_waiterHead + _waiterCount - 1) % _waiters.Length].Last = true;
        Open = 0;
        _sealed = _outLength;
    }

    /// <summary>Sends a lone Sync when the connection owes replies and has been silent since
    /// <see cref="NudgeAfterMs"/> (twice as long after each nudge). A request or a reply that was lost on the way
    /// would otherwise wait for its sender's retransmit timer, 200 ms at least: nothing else is in flight on the
    /// connection to tell either side of the gap. The Sync does: it reaches Postgres past the hole, or its
    /// ReadyForQuery reaches us past the hole, and can trigger loss detection before the retransmit timer expires. Returns when the next nudge is due (long.MaxValue when nothing is owed).</summary>
    public long Nudge(long now)
    {
        if (Broken || !Owes) return long.MaxValue;
        long due = _waitingSince + _nudgeAfter;
        if (now < due) return due;
        PutBytes(SyncMessage);
        _sealed = _outLength;
        if (_writer) _syncs.Enqueue(false);
        _waitingSince = now;
        _nudgeAfter *= 2;
        return now + _nudgeAfter;
    }

    public Waiter TakeWaiter()
    {
        Waiter w = _waiters[_waiterHead];
        _waiterHead = (_waiterHead + 1) % _waiters.Length;
        _waiterCount--;
        return w;
    }

    // ---- writes

    /// <summary>Sends <see cref="Group"/> as one transaction: each write's Bind and Execute, then one Sync.</summary>
    public void SendGroup(long now)
    {
        _waitingSince = now;
        _syncs.Enqueue(true);
        foreach (WriteJob job in Group)
        {
            int start = StartMessage((byte)'B');
            PutByte(0); // unnamed portal
            PutByte(job.Kind == WriteJob.Create ? CreatePost : LikePost);
            PutByte(0);
            PutInt16(1); PutInt16(1); // every parameter binary
            PutInt16(2);
            PutInt32(8); PutInt64(job.UserId);
            if (job.Kind == WriteJob.Create)
            {
                PutInt32(job.BodyLength);
                PutBytes(job.Body.AsSpan(0, job.BodyLength));
            }
            else
            {
                PutInt32(8); PutInt64(job.PostId);
            }
            PutInt16(1); PutInt16(1); // every column binary
            EndMessage(start);
            PutBytes(ExecuteMessage);
        }
        PutBytes(SyncMessage);
        _sealed = _outLength;
    }

    // ---- the socket

    /// <summary>Sends what is queued. False on an error; <see cref="WantsWrite"/> says whether some is left.</summary>
    public bool Flush()
    {
        while (_outSent < _sealed)
        {
            int n = Native.Send(Fd, _out, _outSent, _sealed - _outSent);
            if (n < 0) return false;
            if (n == 0) return true;
            _outSent += n;
        }
        if (_outSent == _outLength) _outLength = _outSent = _sealed = 0;
        else if (_outSent > 0)
        {
            // keep the open batch, a few hundred bytes at most, at the start
            _out.AsSpan(_outSent, _outLength - _outSent).CopyTo(_out);
            _outLength -= _outSent;
            _sealed = _outSent = 0;
        }
        return true;
    }

    /// <summary>Reads what arrived. False when the connection is gone; <paramref name="more"/> when the buffer
    /// filled, so more may be waiting.</summary>
    public bool Receive(long now, out bool more)
    {
        more = false;
        if (_inEnd == _in.Length)
        {
            if (_inStart > 0)
            {
                _in.AsSpan(_inStart, _inEnd - _inStart).CopyTo(_in);
                _inEnd -= _inStart;
                _inStart = 0;
            }
            else Array.Resize(ref _in, _in.Length * 2);
        }
        int room = _in.Length - _inEnd;
        int n = Native.Receive(Fd, _in, _inEnd, room);
        if (n == -1) return true;
        if (n <= 0) return false;
        _inEnd += n;
        _waitingSince = now;
        _nudgeAfter = NudgeTicks;
        more = n == room;
        return true;
    }

    /// <summary>A writer's next whole reply: its messages up to and with the ReadyForQuery that ends it.
    /// <paramref name="group"/> is false for the answer to a nudge, which carries nothing.</summary>
    public bool NextReply(out ReadOnlySpan<byte> reply, out bool group)
    {
        group = false;
        int at = _inStart;
        while (_inEnd - at >= 5)
        {
            int length = BinaryPrimitives.ReadInt32BigEndian(_in.AsSpan(at + 1));
            if (_inEnd - at < 1 + length) break;
            byte type = _in[at];
            at += 1 + length;
            if (type == 'Z')
            {
                reply = _in.AsSpan(_inStart, at - _inStart);
                _inStart = at;
                if (_inStart == _inEnd) _inStart = _inEnd = 0;
                group = _syncs.Count == 0 || _syncs.Dequeue();
                return true;
            }
        }
        reply = default;
        return false;
    }

    /// <summary>The next result of a read batch: one read's messages up to its CommandComplete or ErrorResponse, or
    /// the ReadyForQuery that ends the batch (<paramref name="end"/> says which).</summary>
    public bool NextResult(out ReadOnlySpan<byte> result, out byte end)
    {
        int n = ResultLength(_in.AsSpan(_inStart, _inEnd - _inStart), out end);
        if (n == 0)
        {
            result = default;
            return false;
        }
        result = _in.AsSpan(_inStart, n);
        _inStart += n;
        if (_inStart == _inEnd) _inStart = _inEnd = 0;
        return true;
    }

    /// <summary>The length of the whole messages up to and with the first C, E or Z; 0 when that isn't all here.</summary>
    public static int ResultLength(ReadOnlySpan<byte> data, out byte end)
    {
        int at = 0;
        while (data.Length - at >= 5)
        {
            int length = BinaryPrimitives.ReadInt32BigEndian(data[(at + 1)..]);
            if (data.Length - at < 1 + length) break;
            byte type = data[at];
            at += 1 + length;
            if (type is (byte)'C' or (byte)'E' or (byte)'Z')
            {
                end = type;
                return at;
            }
        }
        end = 0;
        return 0;
    }

    /// <summary>The next message of a reply.</summary>
    public static bool NextMessage(ref ReadOnlySpan<byte> reply, out byte type, out ReadOnlySpan<byte> body)
    {
        if (reply.Length < 5)
        {
            type = 0;
            body = default;
            return false;
        }
        type = reply[0];
        int length = BinaryPrimitives.ReadInt32BigEndian(reply[1..]);
        body = reply.Slice(5, length - 4);
        reply = reply[(1 + length)..];
        return true;
    }

    // ---- message building: a type byte and a length that EndMessage fills in once the body is written

    int StartMessage(byte type)
    {
        PutByte(type);
        int start = _outLength;
        PutInt32(0);
        return start;
    }

    void EndMessage(int start) => BinaryPrimitives.WriteInt32BigEndian(_out.AsSpan(start), _outLength - start);

    /// <summary>The next <paramref name="n"/> bytes of the output buffer, which grows when they don't fit.</summary>
    Span<byte> Reserve(int n)
    {
        if (_out.Length - _outLength < n) Array.Resize(ref _out, Math.Max(_out.Length * 2, _outLength + n));
        var s = _out.AsSpan(_outLength, n);
        _outLength += n;
        return s;
    }

    void PutByte(byte b) => Reserve(1)[0] = b;
    void PutBytes(ReadOnlySpan<byte> b) => b.CopyTo(Reserve(b.Length));
    void PutInt16(short v) => BinaryPrimitives.WriteInt16BigEndian(Reserve(2), v);
    void PutInt32(int v) => BinaryPrimitives.WriteInt32BigEndian(Reserve(4), v);
    void PutInt64(long v) => BinaryPrimitives.WriteInt64BigEndian(Reserve(8), v);
}
