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
    readonly Pg[] _pgs; // the readers, then the writer last
    readonly Pg _writer;
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
    int _nextReader;
    /// <summary>When the oldest write in <see cref="_writes"/> was queued; 0 until the loop first sees it.</summary>
    long _writesSince;
    /// <summary>Reads or writes went back in the queue after a failure: they have waited already, so they leave this pass.</summary>
    bool _hurry;

    public Loop(int[] ports, PgSettings db, int readers)
    {
        _epfd = Native.epoll_create1(0);
        if (_epfd < 0) throw new InvalidOperationException("epoll_create1 failed");
        _listeners = new Socket[ports.Length];
        for (int i = 0; i < ports.Length; i++)
        {
            var s = new Socket(AddressFamily.InterNetwork, SocketType.Stream, ProtocolType.Tcp);
            s.SetSocketOption(SocketOptionLevel.Socket, SocketOptionName.ReuseAddress, true);
            s.SetRawSocketOption(Native.SolSocket, Native.SoReusePort, BitConverter.GetBytes(1));
            s.Bind(new IPEndPoint(IPAddress.Parse(Environment.GetEnvironmentVariable("HOST") ?? "0.0.0.0"), ports[i]));
            s.Listen(65535); // the kernel caps it at net.core.somaxconn
            s.Blocking = false;
            _listeners[i] = s;
            Native.Watch(_epfd, Native.EpollCtlAdd, (int)s.Handle, Native.EpollIn, ListenerTag | (uint)i);
        }
        _pgs = new Pg[readers + 1];
        _pgWatchingOut = new bool[_pgs.Length];
        for (int i = 0; i < _pgs.Length; i++)
        {
            _pgs[i] = new Pg(db);
            Reconnect(i);
        }
        _writer = _pgs[readers];
    }

    public void Run()
    {
        const int MaxEvents = 1024;
        EpollEvent* events = stackalloc EpollEvent[MaxEvents];
        long wait = WaitNanos(long.MaxValue, 0, AnyBroken());
        while (true)
        {
            int n = Native.EpollWait(_epfd, events, MaxEvents, wait);
            for (int i = 0; i < n; i++)
            {
                ulong data = events[i].Data;
                uint ready = events[i].Events;
                if (data >= PgTag) OnPg((int)(data - PgTag), ready);
                else if (data >= ListenerTag) OnListener((int)(data - ListenerTag));
                else OnClient((int)data, ready);
            }
            // what the events queued for Postgres waits in an open batch (and the writes in the next group) until
            // its oldest has waited Pg.Window, so the reads of a few passes share one Sync and one packet
            long now = Stopwatch.GetTimestamp();
            long due = StartWriteGroup(now);
            for (int i = 0; i < _pgs.Length - 1; i++) due = Math.Min(due, _pgs[i].Coalesce(now, _hurry));
            _hurry = false;
            for (int i = 0; i < _pgs.Length; i++) FlushPg(i);
            for (int i = 0; i < _pgs.Length; i++)
            {
                Pg pg = _pgs[i];
                if (pg.Connecting && Environment.TickCount64 >= pg.ConnectDeadline)
                {
                    Console.Error.WriteLine("postgres: connection timed out");
                    pg.Close();
                }
                else if (pg.Broken && !pg.Connecting && Environment.TickCount64 >= pg.RetryAt) Reconnect(i);
            }
            wait = WaitNanos(due, now, AnyBroken());
        }
    }

    /// <summary>How long the next epoll wait may sleep, in nanoseconds: until <paramref name="due"/> (a Stopwatch
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
