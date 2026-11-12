using System.Runtime.InteropServices;

/// <summary>struct epoll_event on x86-64: packed, 12 bytes, the data at 4.</summary>
[StructLayout(LayoutKind.Sequential, Pack = 4)]
struct EpollEvent
{
    public uint Events;
    public ulong Data;
}

/// <summary>The libc calls the loops make directly. recv and send skip the GC transition: the sockets are
/// non-blocking, so they return in microseconds.</summary>
static unsafe partial class Native
{
    // the values of Linux's headers on x86-64
    public const int EAgain = 11, EIntr = 4, ENoSys = 38;
    public const int EpollCtlAdd = 1, EpollCtlDel = 2, EpollCtlMod = 3;
    public const uint EpollIn = 0x001, EpollOut = 0x004, EpollErr = 0x008, EpollHup = 0x010, EpollRdHup = 0x2000;
    const int SockNonBlock = 0x800, SockCloExec = 0x80000, MsgNoSignal = 0x4000;
    public const int SolSocket = 1, SoReusePort = 15;
    public const int IpProtoTcp = 6, TcpNoDelay = 1, TcpDeferAccept = 9, TcpQuickAck = 12, TcpThinLinearTimeouts = 16;
    const int SoError = 4, AfInet = 2, AfInet6 = 10, SockStream = 1, EInProgress = 115;
    const int PollIn = 0x001, PollOut = 0x004, RlimitNoFile = 7;

    [LibraryImport("libc", SetLastError = true)]
    public static partial int epoll_create1(int flags);

    [LibraryImport("libc", SetLastError = true)]
    public static partial int epoll_ctl(int epfd, int op, int fd, EpollEvent* ev);

    [LibraryImport("libc", SetLastError = true)]
    public static partial int epoll_wait(int epfd, EpollEvent* events, int max, int timeout);

    /// <summary>epoll_wait with a nanosecond timeout (Linux 5.11, glibc 2.35); a null timeout waits for ever.</summary>
    [LibraryImport("libc", SetLastError = true)]
    private static partial int epoll_pwait2(int epfd, EpollEvent* events, int max, Timespec* timeout, void* sigmask);

    struct Timespec
    {
        public long Sec, Nsec;
    }

    static bool _noPwait2;

    /// <summary>Waits for events up to <paramref name="nanos"/> (-1: until one comes). Where the kernel has no
    /// epoll_pwait2, the wait rounds up to whole milliseconds.</summary>
    public static int EpollWait(int epfd, EpollEvent* events, int max, long nanos)
    {
        if (!_noPwait2)
        {
            Timespec t = new() { Sec = nanos / 1_000_000_000, Nsec = nanos % 1_000_000_000 };
            int n = epoll_pwait2(epfd, events, max, nanos < 0 ? null : &t, null);
            if (n >= 0 || Marshal.GetLastSystemError() != ENoSys) return n;
            _noPwait2 = true;
        }
        return epoll_wait(epfd, events, max, nanos < 0 ? -1 : (int)System.Math.Min((nanos + 999_999) / 1_000_000, int.MaxValue));
    }

    [LibraryImport("libc", SetLastError = true)]
    private static partial int accept4(int fd, void* address, void* length, int flags);

    [LibraryImport("libc", SetLastError = true)]
    private static partial int setsockopt(int fd, int level, int name, int* value, int length);

    [LibraryImport("libc")]
    public static partial int close(int fd);

    [LibraryImport("libc")]
    private static partial int getrlimit(int resource, ulong* limits);

    [LibraryImport("libc"), SuppressGCTransition]
    private static partial nint recv(int fd, byte* buffer, nint length, int flags);

    [LibraryImport("libc"), SuppressGCTransition]
    private static partial nint send(int fd, byte* buffer, nint length, int flags);

    [LibraryImport("libc", SetLastError = true)]
    private static partial int socket(int domain, int type, int protocol);

    [LibraryImport("libc", SetLastError = true)]
    private static partial int connect(int fd, byte* address, int length);

    [LibraryImport("libc", SetLastError = true)]
    private static partial int getsockopt(int fd, int level, int name, int* value, int* length);

    [LibraryImport("libc", SetLastError = true)]
    private static partial int poll(PollFd* fds, nuint count, int timeout);

    struct PollFd
    {
        public int Fd;
        public short Events, Revents;
    }

    /// <summary>A new connection, non-blocking; -1 when there is none waiting (or accept failed). It has the
    /// listener's TCP options (<see cref="TuneListener"/>), so no call sets them per connection.</summary>
    public static int Accept(int listener) => accept4(listener, null, null, SockNonBlock | SockCloExec);

    /// <summary>The TCP options a listener's connections inherit: Nagle off, so a reply's last segment never waits for
    /// an ack, and <see cref="Thin"/>.</summary>
    public static void TuneListener(int fd)
    {
        int one = 1;
        setsockopt(fd, IpProtoTcp, TcpNoDelay, &one, sizeof(int));
        Thin(fd);
    }

    /// <summary>The options that only hold once the socket listens (listen() resets its ACK state), which accepted
    /// connections inherit:
    /// <list type="bullet">
    /// <item><c>TCP_DEFER_ACCEPT</c>: a connection is handed over once its first request is in, so it costs one
    /// wake-up instead of two, and the accept reads that request at once.</item>
    /// <item>Delayed-ACK mode (<see cref="DelayAcks"/>).</item>
    /// </list></summary>
    public static void TuneListening(int fd)
    {
        int seconds = 1, zero = 0;
        // neither is needed to serve, so a kernel that refuses one only costs packets: say so and go on
        if (setsockopt(fd, IpProtoTcp, TcpDeferAccept, &seconds, sizeof(int)) < 0)
            System.Console.Error.WriteLine($"TCP_DEFER_ACCEPT refused (errno {Marshal.GetLastSystemError()})");
        if (setsockopt(fd, IpProtoTcp, TcpQuickAck, &zero, sizeof(int)) < 0)
            System.Console.Error.WriteLine($"TCP_QUICKACK refused (errno {Marshal.GetLastSystemError()})");
    }

    /// <summary>Clears <c>TCP_QUICKACK</c>: the connection holds back the ACK of a request so it rides on the
    /// response. Without it the kernel, seeing a request after the user's idle seconds, acks it at once in a packet
    /// of its own. This is not sticky: the kernel leaves the mode when its delayed-ACK timer fires before a response
    /// goes out, so a late response sets it again (<see cref="Loop.LateAckMs"/>).</summary>
    public static void DelayAcks(int fd)
    {
        int zero = 0;
        setsockopt(fd, IpProtoTcp, TcpQuickAck, &zero, sizeof(int));
    }

    /// <summary>Linear retransmit timeouts while the connection has fewer than four segments in flight.
    /// This limits exponential timeout backoff for small responses on quiet connections.</summary>
    static void Thin(int fd)
    {
        int one = 1;
        setsockopt(fd, IpProtoTcp, TcpThinLinearTimeouts, &one, sizeof(int));
    }

    [LibraryImport("libc")]
    private static partial int nanosleep(Timespec* request, Timespec* remaining);

    /// <summary>Sleeps the thread for <paramref name="nanos"/>.</summary>
    public static void Sleep(long nanos)
    {
        if (nanos <= 0) return;
        Timespec t = new() { Sec = nanos / 1_000_000_000, Nsec = nanos % 1_000_000_000 };
        nanosleep(&t, null);
    }

    /// <summary>Bytes read; 0 at the peer's close; -1 when nothing is waiting (EAGAIN); -2 on an error.</summary>
    public static int Receive(int fd, byte[] buffer, int at, int max)
    {
        fixed (byte* p = buffer)
            while (true)
            {
                nint n = recv(fd, p + at, max, 0);
                if (n >= 0) return (int)n;
                int errno = Marshal.GetLastSystemError();
                if (errno == EIntr) continue;
                return errno == EAgain ? -1 : -2;
            }
    }

    /// <summary>Bytes sent (0 when the socket is full); -1 on an error.</summary>
    public static int Send(int fd, byte[] buffer, int at, int count)
    {
        fixed (byte* p = buffer)
            while (true)
            {
                nint n = send(fd, p + at, count, MsgNoSignal);
                if (n >= 0) return (int)n;
                int errno = Marshal.GetLastSystemError();
                if (errno == EIntr) continue;
                return errno == EAgain ? 0 : -1;
            }
    }

    /// <summary>Starts a non-blocking TCP connect with Nagle off and <see cref="Thin"/>. Plain libc, not <c>Socket</c>: a .NET socket that has
    /// been non-blocking runs its blocking calls through the runtime's socket engine, whose thread then wakes a
    /// thread-pool worker on every reply. The fd, or minus the errno.</summary>
    public static int Connect(System.Net.IPAddress address, int port)
    {
        bool v6 = address.AddressFamily == System.Net.Sockets.AddressFamily.InterNetworkV6;
        int fd = socket(v6 ? AfInet6 : AfInet, SockStream | SockNonBlock | SockCloExec, 0);
        if (fd < 0) return -Marshal.GetLastSystemError();
        int one = 1;
        setsockopt(fd, IpProtoTcp, TcpNoDelay, &one, sizeof(int));
        Thin(fd);
        byte* sa = stackalloc byte[28]; // sockaddr_in (16 bytes) or sockaddr_in6 (28)
        new System.Span<byte>(sa, 28).Clear();
        sa[0] = (byte)(v6 ? AfInet6 : AfInet);
        sa[2] = (byte)(port >> 8);
        sa[3] = (byte)port;
        address.TryWriteBytes(new System.Span<byte>(sa + (v6 ? 8 : 4), v6 ? 16 : 4), out _);
        if (v6) *(uint*)(sa + 24) = (uint)address.ScopeId;
        if (connect(fd, sa, v6 ? 28 : 16) == 0) return fd;
        int errno = Marshal.GetLastSystemError();
        if (errno == EInProgress) return fd;
        close(fd);
        return -errno;
    }

    /// <summary>SO_ERROR: 0 once a connect has succeeded.</summary>
    public static int SocketError(int fd)
    {
        int error = 0, length = sizeof(int);
        return getsockopt(fd, SolSocket, SoError, &error, &length) < 0 ? Marshal.GetLastSystemError() : error;
    }

    /// <summary>Waits up to <paramref name="ms"/> for the fd to turn readable (or writable); false on a timeout.</summary>
    public static bool Wait(int fd, bool write, int ms)
    {
        PollFd p = new() { Fd = fd, Events = (short)(write ? PollOut : PollIn) };
        while (true)
        {
            int n = poll(&p, 1, ms);
            if (n >= 0) return n > 0;
            if (Marshal.GetLastSystemError() != EIntr) return false;
        }
    }

    /// <summary>The soft RLIMIT_NOFILE: the highest fd this process can get, plus one.</summary>
    public static long MaxFiles()
    {
        ulong* limits = stackalloc ulong[2];
        return getrlimit(RlimitNoFile, limits) == 0 ? (long)System.Math.Min(limits[0], 1UL << 24) : 1 << 20;
    }

    public static void Watch(int epfd, int op, int fd, uint events, ulong data)
    {
        EpollEvent ev = new() { Events = events, Data = data };
        if (epoll_ctl(epfd, op, fd, &ev) < 0 && op != EpollCtlDel)
            throw new System.IO.IOException($"epoll_ctl failed (errno {Marshal.GetLastPInvokeError()})");
    }
}
