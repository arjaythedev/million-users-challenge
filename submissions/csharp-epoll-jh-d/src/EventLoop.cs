// One epoll loop per core, sharing nothing: each has its own SO_REUSEPORT listeners on both ports, its own epoll
// instance, its own receive buffer, its own client connections and its own Postgres connections. A client
// connection is a 40-byte struct in a native table indexed by fd; an idle one owns no buffer and no managed object.
//
// Requests that need Postgres are collected for the whole pass. At its end they are dealt out in contiguous runs to
// as few of this loop's Postgres connections as keeps each run near DealSize (the least loaded ones first), and
// every connection that got a run gets one Sync and one send(): a busy pass becomes a few large round trips.
// Results are decoded on this thread, straight into the response, which is sent from here too: no thread pool, no locks, no hand-offs. A generation
// number, bumped on close, drops answers for client connections that closed (and whose fd may be reused) meanwhile.
using System.Buffers;
using System.Collections.Concurrent;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Security.Cryptography;
using static FeedServer.Native;

namespace FeedServer;

[StructLayout(LayoutKind.Sequential)]
unsafe struct Conn
{
    public byte* In;        // bytes of a partial or pipelined request, or null
    public byte* Out;       // unsent tail of a response, or null
    public int InLen, InCap;
    public int OutOff, OutLen;
    public uint Gen;        // bumped on close
    public byte Busy;       // waiting for Postgres
    public byte CloseAfterWrite;
    public byte ContinueSent;
    public byte Dead;       // closed this pass; the fd itself is closed at the end of the pass
}

sealed unsafe class EventLoop
{
    const int MaxEvents = 512, RecvSize = 64 * 1024, MaxBuffered = Http.MaxBody + Http.MaxHead;
    // epoll user data: what the fd is (high 32 bits) and its index (low 32: the fd itself, or a Postgres slot)
    const uint ForClient = 0, ForListener = 1, ForWake = 2, ForPostgres = 3;
    static ulong Tag(uint what, int index) => (ulong)what << 32 | (uint)index;

    static Conn* conns;
    static int maxFd;
    public const int BatchMicros = 2000;        // batching window, see Run()
    public const int DealSize = 16;             // target requests per Postgres round trip
    public const int PgTimeoutMs = 10_000;      // a Postgres connection owing answers that is silent this long is dropped
    public const int StatsSeconds = 10;         // a stats line per loop this often

    public readonly int Id;
    readonly int epfd, wakeFd;
    readonly byte* recvBuf;
    public readonly IncrementalHash Hmac;
    int processingFd = -1;

    // Postgres: slots [0, readSlots) serve reads, the rest writes; null while (re)connecting
    readonly PgConn?[] pg;
    readonly int readSlots;
    readonly ConcurrentQueue<PgConn> connected = new();
    readonly List<PgConn> dirty = [];
    readonly Stack<PgEntry> entries = new();
    readonly Queue<PgEntry> waiting = new(); // no live connection of the right kind yet; retried on (re)connect
    const int MaxWaitMs = 5000;
    int wakePending;
    readonly List<PgEntry> passReads = [], passWrites = []; // this pass's requests, sent at its end
    readonly PgConn[] byLoad;
    // Client fds closed this pass. close() waits for the end of the pass: until then the kernel can't hand the fd
    // number to another loop's accept4() while this pass still holds an event for it.
    readonly List<int> toClose = [];

    // counters for the stats line
    long open, accepted, closed, acceptErrors, answered, passes, syncs, lastHousekeeping, lastStats;
    long prevAccepted, prevClosed, prevAcceptErrors, prevAnswered, prevPasses, prevSyncs, pgDropped;

    public static void Init(int fdLimit)
    {
        maxFd = fdLimit;
        conns = (Conn*)NativeMemory.AllocZeroed((nuint)fdLimit, (nuint)sizeof(Conn)); // untouched pages cost nothing
    }

    public EventLoop(int id, uint addr, int[] ports, int readConns, int writeConns)
    {
        Id = id;
        Hmac = IncrementalHash.CreateHMAC(HashAlgorithmName.SHA256, Jwt.Key);
        recvBuf = (byte*)NativeMemory.Alloc(RecvSize);
        epfd = epoll_create1(EPOLL_CLOEXEC);
        if (epfd < 0) throw new IOException($"epoll_create1: errno {Errno()}");
        wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        Add(wakeFd, EPOLLIN, ForWake);
        foreach (int port in ports) Add(Listen(addr, port), EPOLLIN, ForListener);
        readSlots = readConns;
        pg = new PgConn?[readConns + writeConns];
        byLoad = new PgConn[pg.Length];
    }

    void Add(int fd, uint events, uint what)
    {
        var ev = new EpollEvent { Events = events, Data = Tag(what, fd) };
        if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) != 0) throw new IOException($"epoll_ctl add: errno {Errno()}");
    }

    public void Start()
    {
        for (int slot = 0; slot < pg.Length; slot++) Pg.ConnectLater(this, slot, slot >= readSlots, 0);
        new Thread(Run) { Name = $"loop-{Id}", IsBackground = true }.Start();
    }

    void Run()
    {
        var events = (EpollEvent*)NativeMemory.Alloc(MaxEvents, (nuint)sizeof(EpollEvent));
        long batchTicks = BatchMicros * Stopwatch.Frequency / 1_000_000;
        while (true)
        {
            int n = epoll_wait(epfd, events, MaxEvents, passReads.Count + passWrites.Count > 0 ? 0 : waiting.Count > 0 ? 100 : 1000);
            if (n < 0)
            {
                if (Errno() == EINTR) continue;
                throw new IOException($"epoll_wait: errno {Errno()}");
            }
            long passStart = Stopwatch.GetTimestamp();
            // A bug must not take the server down (it isn't restarted). The handlers below contain their own
            // failures; this catch is the last resort for anything that slips past them, and keeps the loop going.
            try { Pass(events, n); }
            catch (Exception ex) { Log.Error(ex); }
            for (int i = 0; i < toClose.Count; i++) close(toClose[i]); // also removes it from the epoll set
            toClose.Clear();
            passes++;
            // Batching window: a pass starts at most every BatchMicros, so more requests and Postgres results pile
            // up and share each pass's one Sync and one send() per connection. The pass's own work counts towards
            // the window, so a loaded loop sleeps less, and one that couldn't fetch every ready event doesn't sleep.
            if (n == MaxEvents) continue;
            long left = batchTicks - (Stopwatch.GetTimestamp() - passStart);
            if (left > 0) usleep((uint)(left * 1_000_000 / Stopwatch.Frequency));
        }
    }

    void Pass(EpollEvent* events, int n)
    {
        foreach (var ev in new ReadOnlySpan<EpollEvent>(events, n))
        {
            int id = unchecked((int)ev.Data);
            switch ((uint)(ev.Data >> 32))
            {
                // A bug in one request drops that client connection.
                case ForClient:
                    if (conns[id].Dead != 0) break; // closed earlier in this pass
                    try { OnConn(id, ev.Events); }
                    catch (Exception ex) { Log.Error(ex); if (conns[id].Dead == 0) Close(id); }
                    break;
                case ForListener: AcceptAll(id); break;
                case ForPostgres:
                    try { OnPg(id, ev.Events); }
                    catch (Exception ex) { Log.Error(ex); if (pg[id] is PgConn broken) PgFail(broken, ex.GetType().Name); }
                    break;
                case ForWake:
                    ulong v;
                    read(wakeFd, &v, 8);
                    Interlocked.Exchange(ref wakePending, 0);
                    while (connected.TryDequeue(out var c))
                    {
                        pg[c.Slot] = c;
                        var add = new EpollEvent { Events = EPOLLIN | EPOLLRDHUP, Data = Tag(ForPostgres, c.Slot) };
                        epoll_ctl(epfd, EPOLL_CTL_ADD, c.Fd, &add);
                    }
                    for (int k = waiting.Count; k > 0; k--) Submit(waiting.Dequeue());
                    break;
            }
        }
        if (waiting.Count > 0) ExpireWaiting();
        long now = Environment.TickCount64;
        if (now - lastHousekeeping >= 1000) Housekeeping(now);
        FlushPg();
    }

    // ---- clients ----

    void AcceptAll(int lfd)
    {
        while (true)
        {
            int fd = accept4(lfd, null, null, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd < 0)
            {
                // EAGAIN, or out of descriptors or memory: try again on the next readiness event
                int err = Errno();
                if (err != EAGAIN) { acceptErrors++; Log.Error(new IOException($"loop {Id}: accept4: errno {err}")); }
                return;
            }
            if (fd >= maxFd) { acceptErrors++; close(fd); continue; }
            ref Conn c = ref conns[fd];
            c = new Conn { Gen = c.Gen }; // the generation survives fd reuse
            var ev = new EpollEvent { Events = EPOLLIN | EPOLLRDHUP, Data = Tag(ForClient, fd) };
            if (epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &ev) != 0)
            {
                acceptErrors++;
                Log.Error(new IOException($"loop {Id}: epoll_ctl add client: errno {Errno()}"));
                c.Gen++;
                close(fd);
                continue;
            }
            accepted++;
            open++;
        }
    }

    void OnConn(int fd, uint events)
    {
        if ((events & (EPOLLERR | EPOLLHUP)) != 0) { Close(fd); return; }
        uint gen = conns[fd].Gen;
        if ((events & EPOLLOUT) != 0)
        {
            FlushOut(fd);
            if (conns[fd].Gen != gen) return;
        }
        if ((events & (EPOLLIN | EPOLLRDHUP)) != 0) OnReadable(fd);
    }

    void OnReadable(int fd)
    {
        nint n = recv(fd, recvBuf, RecvSize, 0);
        if (n <= 0)
        {
            if (n < 0 && Errno() == EAGAIN) return;
            Close(fd);
            return;
        }
        ref Conn c = ref conns[fd];
        if (c.InLen == 0 && c.Busy == 0 && c.OutLen == 0)
        {
            int used = Process(fd, new Span<byte>(recvBuf, (int)n));
            if (used >= 0 && used < n) Append(fd, recvBuf + used, (int)n - used);
            return;
        }
        uint gen = c.Gen;
        Append(fd, recvBuf, (int)n);
        if (c.Gen == gen && c.Busy == 0 && c.OutLen == 0) ProcessBuffered(fd);
    }

    void Append(int fd, byte* data, int len)
    {
        ref Conn c = ref conns[fd];
        if (c.InLen + len > MaxBuffered) { Close(fd); return; }
        if (c.InLen + len > c.InCap)
        {
            int cap = Math.Max(c.InCap * 2, Math.Max(c.InLen + len, 1024));
            c.In = (byte*)NativeMemory.Realloc(c.In, (nuint)cap);
            c.InCap = cap;
        }
        Buffer.MemoryCopy(data, c.In + c.InLen, c.InCap - c.InLen, len);
        c.InLen += len;
    }

    void ProcessBuffered(int fd)
    {
        ref Conn c = ref conns[fd];
        int used = Process(fd, new Span<byte>(c.In, c.InLen));
        if (used <= 0) return;
        int left = c.InLen - used;
        if (left > 0)
        {
            new Span<byte>(c.In + used, left).CopyTo(new Span<byte>(c.In, left)); // overlapping: CopyTo handles it
            c.InLen = left;
            return;
        }
        NativeMemory.Free(c.In); // all consumed: an idle connection keeps no buffer
        c.In = null;
        c.InLen = c.InCap = 0;
    }

    // Handles every complete request in data, in order, until one has to wait for Postgres. Returns the bytes
    // consumed, or -1 if the connection was closed.
    int Process(int fd, Span<byte> data)
    {
        int off = 0;
        uint gen = conns[fd].Gen;
        int outer = processingFd;
        processingFd = fd; // an answer delivered while we're in here must not re-enter this connection's buffer
        try
        {
            while (off < data.Length)
            {
                int r = Http.Parse(data[off..], out var req, out bool needContinue);
                if (r == Http.ParseIncomplete)
                {
                    if (needContinue && conns[fd].ContinueSent == 0)
                    {
                        conns[fd].ContinueSent = 1;
                        SendRaw(fd, Http.Continue);
                    }
                    break;
                }
                if (r < 0)
                {
                    if (r == Http.ParseTooLarge) RespondError(fd, 413, "payload too large"u8, keepAlive: false);
                    else RespondError(fd, 400, "bad request"u8, keepAlive: false);
                    return -1;
                }
                conns[fd].ContinueSent = 0;
                off += r;
                Api.Dispatch(this, fd, gen, in req);
                if (conns[fd].Gen != gen) return -1;
                if (conns[fd].Busy != 0 || conns[fd].OutLen != 0) break;
            }
            return off;
        }
        finally { processingFd = outer; }
    }

    // ---- requests waiting on Postgres ----

    public PgEntry Rent(Stmt stmt, int fd, uint gen, in Request req)
    {
        if (!entries.TryPop(out var e)) e = new PgEntry();
        (e.Stmt, e.Fd, e.Gen) = (stmt, fd, gen);
        (e.KeepAlive, e.AcceptsGzip) = (req.KeepAlive, req.AcceptsGzip);
        conns[fd].Busy = 1; // answers its later pipelined requests only after this one
        return e;
    }

    // Holds the request for the end of the pass (FlushPg).
    public void Submit(PgEntry e) => (e.IsWrite ? passWrites : passReads).Add(e);

    // No live connection of this kind: wait up to MaxWaitMs, like a pool waits for a free connection. /health
    // answers at once, so it reports the outage.
    void NoConnection(PgEntry e)
    {
        if (e.Stmt == Stmt.Health)
        {
            Api.OnUnavailable(e);
            Deliver(e);
            return;
        }
        if (e.WaitingSince == 0) e.WaitingSince = Environment.TickCount64;
        waiting.Enqueue(e);
    }

    // Splits the pass's requests of one kind into ceil(count / DealSize) contiguous runs of near-equal length, one
    // per connection, given to the least loaded live connections of slots [from, to).
    void Deal(List<PgEntry> list, int from, int to)
    {
        int count = list.Count;
        if (count == 0) return;
        int live = 0;
        for (int i = from; i < to; i++)
            if (pg[i] is PgConn c) byLoad[live++] = c;
        if (live == 0)
        {
            // answering /health can start a pipelined request, which lands in the list: empty it first
            var stuck = list.ToArray();
            list.Clear();
            foreach (var e in stuck) NoConnection(e);
            return;
        }
        int runs = Math.Min(live, (count + DealSize - 1) / DealSize);
        for (int r = 0; r < runs; r++) // selection: the r-th least loaded into place r
        {
            int min = r;
            for (int j = r + 1; j < live; j++)
                if (byLoad[j].Load < byLoad[min].Load) min = j;
            (byLoad[r], byLoad[min]) = (byLoad[min], byLoad[r]);
            var conn = byLoad[r];
            for (int i = count * r / runs, end = count * (r + 1) / runs; i < end; i++) conn.Encode(list[i]);
            if (!conn.Dirty) { conn.Dirty = true; dirty.Add(conn); }
        }
        Array.Clear(byLoad, 0, live);
        list.Clear();
    }

    void ExpireWaiting()
    {
        long now = Environment.TickCount64;
        for (int k = waiting.Count; k > 0; k--)
        {
            var e = waiting.Dequeue();
            if (now - e.WaitingSince < MaxWaitMs) { waiting.Enqueue(e); continue; }
            Api.OnUnavailable(e);
            Deliver(e);
        }
    }

    public void Requeue(PgEntry e)
    {
        e.ResetResult();
        Submit(e);
    }

    // Sends the finished response (Api has set e.Status and e.W) and recycles the entry.
    public void Deliver(PgEntry e)
    {
        int fd = e.Fd;
        uint gen = e.Gen;
        bool keepAlive = e.KeepAlive, gzip = e.AcceptsGzip;
        int status = e.Status;
        if (conns[fd].Gen != gen)
        {
            e.Clear();
            entries.Push(e);
            return;
        }
        var (buf, off, len) = Http.Finish(ref e.W, status, gzip, keepAlive);
        e.Clear();
        entries.Push(e);
        answered++;
        conns[fd].Busy = 0;
        Send(fd, buf, off, len, !keepAlive);
        ref Conn c = ref conns[fd];
        if (c.Gen != gen || c.OutLen != 0 || c.InLen == 0 || fd == processingFd) return;
        // The next pipelined request is this client's, not the caller's: a bug in it drops this client only, not
        // the Postgres connection (or pass) that delivered the answer.
        try { ProcessBuffered(fd); }
        catch (Exception ex) { Log.Error(ex); if (c.Gen == gen) Close(fd); }
    }

    void FlushPg()
    {
        // Answering a request can start the next pipelined one, and a failed send hands its requests back
        // (Requeue), so deal and send again until the pass has nothing left; anything still left after a few
        // rounds goes out at the end of the next pass, which then starts without waiting.
        for (int round = 0; round < 4 && passReads.Count + passWrites.Count > 0; round++)
        {
            Deal(passReads, 0, readSlots);
            Deal(passWrites, readSlots, pg.Length);
            for (int i = 0; i < dirty.Count; i++)
            {
                var c = dirty[i];
                c.Dirty = false;
                if (pg[c.Slot] != c) continue;
                if (c.Unsent > 0) syncs++;
                if (!c.Flush()) PgFail(c, c.FailReason);
            }
            dirty.Clear();
        }
    }

    // Once a second: drop Postgres connections that stopped answering, and print the stats line when it's due.
    void Housekeeping(long now)
    {
        lastHousekeeping = now;
        foreach (var c in pg)
            if (c is not null && c.Stalled(now, PgTimeoutMs))
            {
                pgDropped++;
                PgFail(c, $"silent for {PgTimeoutMs} ms with {c.Inflight} requests in flight");
            }
        if (now - lastStats < StatsSeconds * 1000L) return;
        double secs = lastStats == 0 ? StatsSeconds : (now - lastStats) / 1000.0;
        lastStats = now;
        int live = 0, inflight = 0, maxLoad = 0;
        foreach (var c in pg)
        {
            if (c is null) continue;
            live++;
            inflight += c.Load;
            maxLoad = Math.Max(maxLoad, c.Load);
        }
        long dPasses = passes - prevPasses, dAnswered = answered - prevAnswered, dSyncs = syncs - prevSyncs;
        Log.Info($"stats loop={Id} open={open} accepted/s={(accepted - prevAccepted) / secs:F0} closed/s={(closed - prevClosed) / secs:F0} " +
                 $"accept_errors={acceptErrors - prevAcceptErrors} pg_answers/s={dAnswered / secs:F0} passes/s={dPasses / secs:F0} " +
                 $"pg_batch={(dSyncs > 0 ? (double)dAnswered / dSyncs : 0):F1} pg_live={live}/{pg.Length} pg_queued={inflight} " +
                 $"pg_max_queue={maxLoad} waiting={waiting.Count} pg_dropped={pgDropped}");
        prevAccepted = accepted; prevClosed = closed; prevAcceptErrors = acceptErrors; prevAnswered = answered;
        prevPasses = passes; prevSyncs = syncs;
    }

    void OnPg(int slot, uint events)
    {
        if (pg[slot] is not PgConn c) return;
        if ((events & EPOLLERR) != 0) { PgFail(c, $"socket error {c.SocketError()}"); return; }
        bool ok = true;
        if ((events & EPOLLOUT) != 0) ok = c.SendOut();
        if (ok && (events & (EPOLLIN | EPOLLRDHUP | EPOLLHUP)) != 0) ok = c.OnReadable();
        if (!ok) PgFail(c, c.FailReason);
    }

    public void SetPgEvents(PgConn c, uint events)
    {
        var ev = new EpollEvent { Events = events, Data = Tag(ForPostgres, c.Slot) };
        epoll_ctl(epfd, EPOLL_CTL_MOD, c.Fd, &ev);
    }

    void PgFail(PgConn c, string? why)
    {
        if (pg[c.Slot] != c) return;
        pg[c.Slot] = null;
        close(c.Fd);
        Pg.LastError = "connection to Postgres lost";
        Log.Error(new IOException($"loop {Id}: Postgres connection {c.Slot} lost ({why ?? "no reason recorded"}), reconnecting"));
        c.FailAll();
        Pg.ConnectLater(this, c.Slot, c.ForWrites, 0); // then every second while it fails
    }

    // Called from a background thread once a new connection is ready.
    public void OnPgConnected(PgConn c)
    {
        connected.Enqueue(c);
        if (Interlocked.Exchange(ref wakePending, 1) == 0)
        {
            ulong one = 1;
            write(wakeFd, &one, 8);
        }
    }

    // ---- responses ----

    // {"error":"<message>"}, built and sent on the loop thread (no database involved).
    public void RespondError(int fd, int status, ReadOnlySpan<byte> message, bool keepAlive)
    {
        var w = JsonWriter.Create(512);
        w.Raw("{\"error\":"u8);
        w.Utf8String(message);
        w.Raw("}"u8);
        var (buf, off, len) = Http.Finish(ref w, status, false, keepAlive);
        Send(fd, buf, off, len, !keepAlive);
    }

    void SendRaw(int fd, ReadOnlySpan<byte> bytes)
    {
        var buf = ArrayPool<byte>.Shared.Rent(bytes.Length);
        bytes.CopyTo(buf);
        Send(fd, buf, 0, bytes.Length, false);
    }

    // Sends a whole response (returning buf to the pool). What the socket doesn't take now is kept and sent when
    // it becomes writable.
    void Send(int fd, byte[] buf, int off, int len, bool closeAfter)
    {
        nint n;
        fixed (byte* p = buf) n = send(fd, p + off, (nuint)len, MSG_NOSIGNAL);
        if (n == len)
        {
            ArrayPool<byte>.Shared.Return(buf);
            if (closeAfter) Close(fd);
            return;
        }
        if (n < 0 && Errno() != EAGAIN)
        {
            ArrayPool<byte>.Shared.Return(buf);
            Close(fd);
            return;
        }
        int sent = (int)Math.Max(n, 0);
        ref Conn c = ref conns[fd];
        int rest = len - sent;
        c.Out = (byte*)NativeMemory.Alloc((nuint)rest);
        fixed (byte* p = buf) Buffer.MemoryCopy(p + off + sent, c.Out, rest, rest);
        ArrayPool<byte>.Shared.Return(buf);
        c.OutOff = 0;
        c.OutLen = rest;
        c.CloseAfterWrite = closeAfter ? (byte)1 : (byte)0;
        var ev = new EpollEvent { Events = EPOLLIN | EPOLLOUT | EPOLLRDHUP, Data = Tag(ForClient, fd) };
        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
    }

    void FlushOut(int fd)
    {
        ref Conn c = ref conns[fd];
        if (c.OutLen == 0) return;
        nint n = send(fd, c.Out + c.OutOff, (nuint)c.OutLen, MSG_NOSIGNAL);
        if (n < 0)
        {
            if (Errno() != EAGAIN) Close(fd);
            return;
        }
        c.OutOff += (int)n;
        c.OutLen -= (int)n;
        if (c.OutLen > 0) return;
        NativeMemory.Free(c.Out);
        c.Out = null;
        c.OutOff = 0;
        if (c.CloseAfterWrite != 0) { Close(fd); return; }
        var ev = new EpollEvent { Events = EPOLLIN | EPOLLRDHUP, Data = Tag(ForClient, fd) };
        epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &ev);
        if (c.InLen > 0 && c.Busy == 0) ProcessBuffered(fd);
    }

    void Close(int fd)
    {
        ref Conn c = ref conns[fd];
        if (c.Dead != 0) return;
        c.Gen++;
        closed++;
        open--;
        NativeMemory.Free(c.In);  // both may be null
        NativeMemory.Free(c.Out);
        c = new Conn { Gen = c.Gen, Dead = 1 };
        toClose.Add(fd); // closed at the end of the pass
    }
}
