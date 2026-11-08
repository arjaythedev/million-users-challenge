// libc system calls (x86_64 Linux). Client sockets are raw file descriptors: no System.Net.Sockets.Socket, no
// SafeHandle, no managed object per connection. Short non-blocking calls skip the GC transition; epoll_wait,
// which blocks, does not.
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace FeedServer;

[StructLayout(LayoutKind.Sequential, Pack = 4)] // struct epoll_event is packed on x86_64: 12 bytes
struct EpollEvent
{
    public uint Events;
    public ulong Data;
}

[StructLayout(LayoutKind.Sequential)]
struct SockAddrIn
{
    public ushort Family;
    public ushort Port;   // network byte order
    public uint Addr;     // network byte order
    public ulong Zero;
}

[StructLayout(LayoutKind.Sequential)]
struct RLimit
{
    public ulong Cur;
    public ulong Max;
}

static unsafe partial class Native
{
    const string Libc = "libc.so.6";

    public const int AF_INET = 2, SOCK_STREAM = 1, SOCK_NONBLOCK = 0x800, SOCK_CLOEXEC = 0x80000;
    public const int SOL_SOCKET = 1, SO_REUSEADDR = 2, SO_REUSEPORT = 15, IPPROTO_TCP = 6, TCP_NODELAY = 1;
    public const int SO_ERROR = 4;
    public const int EPOLL_CLOEXEC = 0x80000, EPOLL_CTL_ADD = 1, EPOLL_CTL_DEL = 2, EPOLL_CTL_MOD = 3;
    public const uint EPOLLIN = 0x1, EPOLLOUT = 0x4, EPOLLERR = 0x8, EPOLLHUP = 0x10, EPOLLRDHUP = 0x2000;
    public const int EFD_NONBLOCK = 0x800, EFD_CLOEXEC = 0x80000;
    public const int MSG_NOSIGNAL = 0x4000;
    public const int EAGAIN = 11, EINTR = 4;
    public const int RLIMIT_NOFILE = 7;

    [LibraryImport(Libc, SetLastError = true)]
    public static partial int socket(int domain, int type, int protocol);

    [LibraryImport(Libc, SetLastError = true)]
    public static partial int setsockopt(int fd, int level, int optname, void* optval, uint optlen);

    [LibraryImport(Libc, SetLastError = true)]
    public static partial int getsockopt(int fd, int level, int optname, void* optval, uint* optlen);

    [LibraryImport(Libc, SetLastError = true)]
    public static partial int bind(int fd, SockAddrIn* addr, uint addrlen);

    [LibraryImport(Libc, SetLastError = true)]
    public static partial int listen(int fd, int backlog);

    [LibraryImport(Libc, SetLastError = true), SuppressGCTransition]
    public static partial int accept4(int fd, void* addr, void* addrlen, int flags);

    [LibraryImport(Libc, SetLastError = true)]
    public static partial int epoll_create1(int flags);

    [LibraryImport(Libc, SetLastError = true), SuppressGCTransition]
    public static partial int epoll_ctl(int epfd, int op, int fd, EpollEvent* ev);

    [LibraryImport(Libc, SetLastError = true)]
    public static partial int epoll_wait(int epfd, EpollEvent* events, int maxevents, int timeout);

    [LibraryImport(Libc, SetLastError = true), SuppressGCTransition]
    public static partial nint recv(int fd, byte* buf, nuint len, int flags);

    [LibraryImport(Libc, SetLastError = true), SuppressGCTransition]
    public static partial nint send(int fd, byte* buf, nuint len, int flags);

    [LibraryImport(Libc), SuppressGCTransition]
    public static partial int close(int fd);

    [LibraryImport(Libc, SetLastError = true)]
    public static partial int eventfd(uint initval, int flags);

    [LibraryImport(Libc), SuppressGCTransition]
    public static partial nint read(int fd, void* buf, nuint count);

    [LibraryImport(Libc), SuppressGCTransition]
    public static partial nint write(int fd, void* buf, nuint count);

    [LibraryImport(Libc)]
    public static partial int usleep(uint usec);

    // Blocking variants (no SuppressGCTransition): used only by the Postgres handshake on a background thread.
    [LibraryImport(Libc, EntryPoint = "recv", SetLastError = true)]
    public static partial nint recv_blocking(int fd, byte* buf, nuint len, int flags);

    [LibraryImport(Libc, EntryPoint = "send", SetLastError = true)]
    public static partial nint send_blocking(int fd, byte* buf, nuint len, int flags);

    [LibraryImport(Libc, SetLastError = true)]
    public static partial int connect(int fd, SockAddrIn* addr, uint addrlen);

    [LibraryImport(Libc, SetLastError = true)]
    public static partial int fcntl(int fd, int cmd, int arg);

    public const int SO_RCVTIMEO = 20, SO_SNDTIMEO = 21, F_GETFL = 3, F_SETFL = 4, O_NONBLOCK = 0x800;

    public static void SetTimeouts(int fd, int seconds)
    {
        long* tv = stackalloc long[2] { seconds, 0 }; // struct timeval
        setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, tv, 16);
        setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, tv, 16);
    }

    [LibraryImport(Libc, SetLastError = true)]
    public static partial int getrlimit(int resource, RLimit* rlim);

    [LibraryImport(Libc, SetLastError = true)]
    public static partial int setrlimit(int resource, RLimit* rlim);

    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    public static int Errno() => Marshal.GetLastPInvokeError();

    public static ushort Htons(int port) => (ushort)(((port & 0xff) << 8) | ((port >> 8) & 0xff));

    // The open-files limit we may use: the soft limit lifted to the hard one (the box's unit sets LimitNOFILE),
    // capped at 16M descriptors.
    public static int RaiseFdLimit()
    {
        RLimit lim;
        if (getrlimit(RLIMIT_NOFILE, &lim) != 0) return 1024;
        var wanted = new RLimit { Cur = lim.Max, Max = lim.Max };
        if (lim.Cur != lim.Max && setrlimit(RLIMIT_NOFILE, &wanted) == 0) lim = wanted;
        return (int)ulong.Min(lim.Cur, 1UL << 24);
    }

    // A non-blocking listener for one loop. SO_REUSEPORT gives every loop its own accept queue on the same port and
    // lets the kernel spread connections over them; accepted sockets inherit TCP_NODELAY.
    public static int Listen(uint addrNetworkOrder, int port)
    {
        int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) throw new IOException($"socket: errno {Errno()}");
        foreach (var (level, option) in (ReadOnlySpan<(int, int)>)[(SOL_SOCKET, SO_REUSEADDR), (SOL_SOCKET, SO_REUSEPORT), (IPPROTO_TCP, TCP_NODELAY)])
        {
            int on = 1;
            setsockopt(fd, level, option, &on, sizeof(int));
        }
        var where = new SockAddrIn { Family = AF_INET, Port = Htons(port), Addr = addrNetworkOrder };
        if (bind(fd, &where, (uint)sizeof(SockAddrIn)) != 0) throw new IOException($"bind port {port}: errno {Errno()}");
        if (listen(fd, ushort.MaxValue) != 0) throw new IOException($"listen port {port}: errno {Errno()}");
        return fd;
    }
}
