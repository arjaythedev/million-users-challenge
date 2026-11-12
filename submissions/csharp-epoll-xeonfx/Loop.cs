using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Net;
using System.Net.Sockets;

/// <summary>A client connection, indexed by its fd. <see cref="Gen"/> changes whenever the fd closes, so a database
/// reply for a connection that has gone (and maybe come back as another client) is dropped.</summary>
struct Conn
{
    public int Gen;
    /// <summary>A request is waiting on the database; anything that arrives meanwhile waits in <see cref="In"/>.</summary>
    public bool Busy;
    /// <summary>Close once <see cref="Out"/> is sent.</summary>
    public bool CloseAfterSend;
    /// <summary>The client sent all it will (it shut down its side): answer what it asked, then close.</summary>
    public bool PeerDone;
    /// <summary>Reading is paused: <see cref="In"/> is full of requests not answered yet.</summary>
    public bool Paused;
    /// <summary>Since when the request being answered has waited for its ACK to ride on a response, in milliseconds
    /// modulo 65,536 (<see cref="Loop.NowMs"/>): two bytes, so the struct stays 40.</summary>
    public ushort ReceivedMs;
    /// <summary>The request being answered takes gzip (its Accept-Encoding says so).</summary>
    public bool Gzip;
    public byte[]? In;
    public int InLength;
    public byte[]? Out;
    public int OutStart, OutEnd;
}

/// <summary>
/// One thread's event loop: its own listeners on both ports (SO_REUSEPORT spreads the connections over the loops),
/// its clients, and its own Postgres connections. Nothing is shared between loops, so nothing is locked.
/// Loop.Http.cs serves the clients, Loop.Routes.cs answers the routes, Loop.Postgres.cs talks to the database.
/// </summary>
sealed unsafe partial class Loop
{
    public static Conn[] Conns = [];
    public static Jwt Jwt = null!;
    public static long StartedAt;

    // an epoll event's data is the client's fd, or one of these tags plus the listener's or Postgres connection's index
    const ulong ListenerTag = 1UL << 40, PgTag = 2UL << 40;
    const int MaxRequest = 64 << 10, MaxGroup = 512;
    const uint ClientEvents = Native.EpollIn | Native.EpollRdHup;
    // the response's body is written here; its head goes just before it, so both leave in one send
    const int HeadRoom = 256;
    /// <summary>Connections taken per listener wake-up, so one busy port can't starve the rest of the loop.</summary>
    const int MaxAccepts = 256;
    /// <summary>Bodies longer than this are gzipped for clients that take it: a shorter one already leaves in one TCP
    /// segment with its head, so compressing it saves nothing. In practice that is the feed (about 3.6 KB, three
    /// segments, one gzipped). Bodies past <see cref="GzipMax"/> (no response the routes write) go plain.</summary>
    const int GzipFrom = 1200, GzipMax = 256 << 10;

    readonly int _epfd;
    readonly Socket[] _listeners;
    readonly Pg[] _pgs; // the readers, then the writers
    readonly int _readers;
    readonly bool[] _pgWatchingOut;
    readonly byte[] _receive = new byte[MaxRequest];
    byte[] _response = new byte[1 << 20];
    /// <summary>A gzipped body goes here, at HeadRoom, with its head written in front as in <see cref="_response"/>.</summary>
    readonly byte[] _gzipped = new byte[HeadRoom + Gzip.Bound(GzipMax)];
    readonly Gzip _gzip = new();
    /// <summary>A new post's body, unescaped, before it is copied into its <see cref="WriteJob"/>.</summary>
    readonly byte[] _postBody = new byte[MaxRequest];
    /// <summary>The username of the request's token, before it is copied into its <see cref="WriteJob"/>.</summary>
    readonly byte[] _userName = new byte[Jwt.MaxNameBytes];
    readonly List<WriteJob> _writes = [];
    readonly Stack<WriteJob> _spareJobs = new();
    /// <summary>The reader this pass's reads go to; null until the pass has one.</summary>
    Pg? _passReader;
    /// <summary>When this pass began (a Stopwatch timestamp).</summary>
    long _now;
    /// <summary>When this pass began, in milliseconds modulo 65,536; differences of these wrap correctly.</summary>
    ushort NowMs => (ushort)(_now / (Stopwatch.Frequency / 1000));
    /// <summary>A response sent this long after its request was too late for the request's ACK to ride on it: the
    /// kernel's delayed-ACK timer (40 ms at least) has sent that ACK alone and left delayed-ACK mode, so the
    /// connection's next request would get an ACK of its own too. Such a response puts it back in the mode.</summary>
    internal const int LateAckMs = 25;

    /// <summary>A pass serves what is ready, then the loop sleeps out the rest of this time before the next one. At a
    /// million users a loop otherwise wakes for nearly every request and every Postgres reply; this way one wake-up
    /// serves all that arrived meanwhile, its reads leave under one Sync and its writes as one group. A request waits
    /// a pass or two, so a shorter pass keeps it far from the delayed-ACK timer (<see cref="LateAckMs"/>) and keeps
    /// each batch's reply, the burst the box's network takes in at once, small.</summary>
    const long PassNanos = 2_000_000;

    public Loop(int[] ports, PgSettings db, int readers, int writers)
    {
        _epfd = Native.epoll_create1(0);
        if (_epfd < 0) throw new InvalidOperationException("epoll_create1 failed");
        _listeners = new Socket[ports.Length];
        for (int i = 0; i < ports.Length; i++)
        {
            var s = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
            s.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            s.SetRawSocketOption(Native.SolSocket, Native.SoReusePort, BitConverter.GetBytes(1));
            Native.TuneListener((int)s.Handle);
            s.Bind(new IPEndPoint(IPAddress.Parse(Environment.GetEnvironmentVariable("HOST") ?? "0.0.0.0"), ports[i]));
            s.Listen(65535); // the kernel caps it at net.core.somaxconn
            Native.TuneListening((int)s.Handle);
            s.Blocking = false;
            _listeners[i] = s;
            Native.Watch(_epfd, Native.EpollCtlAdd, (int)s.Handle, Native.EpollIn, ListenerTag | (uint)i);
        }
        _readers = readers;
        _pgs = new Pg[readers + writers];
        _pgWatchingOut = new bool[_pgs.Length];
        for (int i = 0; i < _pgs.Length; i++)
        {
            _pgs[i] = new Pg(db, writer: i >= readers);
            Reconnect(i);
        }
    }

    public void Run()
    {
        const int MaxEvents = 1024;
        EpollEvent* events = stackalloc EpollEvent[MaxEvents];
        long wait = WaitNanos(long.MaxValue, 0, AnyBroken());
        while (true)
        {
            int n = Native.EpollWait(_epfd, events, MaxEvents, wait);
            _now = Stopwatch.GetTimestamp();
            for (int i = 0; i < n; i++)
            {
                ulong data = events[i].Data;
                uint ready = events[i].Events;
                if (data >= PgTag) OnPg((int)(data - PgTag), ready);
                else if (data >= ListenerTag) OnListener((int)(data - ListenerTag));
                else OnClient((int)data, ready);
            }
            wait = EndPass();
            if (n == MaxEvents) wait = 0; // more is ready: straight on
            else if (n > 0) Native.Sleep(PassNanos - Stopwatch.GetElapsedTime(_now).Ticks * 100);
        }
    }

    /// <summary>Ends a pass: what it queued for Postgres leaves (its reads under one Sync, its writes as one group),
    /// silent connections are nudged and dropped ones reconnect. Returns how long the loop may wait for the next
    /// event, as <see cref="WaitNanos"/>.</summary>
    long EndPass()
    {
        _passReader?.Seal(_now);
        _passReader = null;
        StartWriteGroups();
        Judge();
        long due = long.MaxValue;
        bool broken = false;
        for (int i = 0; i < _pgs.Length; i++)
        {
            Pg pg = _pgs[i];
            if (!pg.Broken)
            {
                // most of a loop's connections are idle on a quiet network: nothing to nudge, nothing to send
                if (!pg.Owes && !pg.WantsWrite && !_pgWatchingOut[i]) continue;
                due = Math.Min(due, pg.Nudge(_now));
                FlushPg(i);
            }
            else if (pg.Connecting)
            {
                if (Environment.TickCount64 >= pg.ConnectDeadline)
                {
                    Console.Error.WriteLine("postgres: connection timed out");
                    pg.Close();
                }
            }
            else if (Environment.TickCount64 >= pg.RetryAt) Reconnect(i);
            broken |= pg.Broken;
        }
        return WaitNanos(due, _now, broken);
    }

    /// <summary>How long the next epoll wait may last, in nanoseconds: until <paramref name="due"/> (a Stopwatch
    /// timestamp; long.MaxValue for no deadline), and a second at most while a Postgres connection is down so it
    /// reconnects. -1 waits for events only.</summary>
    internal static long WaitNanos(long due, long now, bool broken)
    {
        long nanos = -1;
        if (due != long.MaxValue)
        {
            long ticks = Math.Clamp(due - now, 0, Stopwatch.Frequency);
            nanos = (ticks * 1_000_000_000 + Stopwatch.Frequency - 1) / Stopwatch.Frequency;
        }
        if (broken && (nanos < 0 || nanos > 1_000_000_000)) nanos = 1_000_000_000;
        return nanos;
    }
}
