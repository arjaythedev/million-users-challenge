using System.Net;
using System.Runtime.InteropServices;

namespace MillionReactor;

internal static unsafe partial class Linux
{
    internal const uint Read = 1, Write = 4, Error = 8, Hangup = 16, ReadHangup = 8192;
    [StructLayout(LayoutKind.Sequential, Pack = 1)]
    internal struct EpollEvent { public uint Events; public ulong Data; }
    [StructLayout(LayoutKind.Sequential)]
    private struct Address { public ushort Family; public ushort Port; public uint Host; public ulong Zero; }
    [LibraryImport("libc.so.6", SetLastError = true)] internal static partial int epoll_create1(int flags);
    [LibraryImport("libc.so.6", SetLastError = true)] internal static partial int epoll_ctl(int epfd, int op, int fd, EpollEvent* ev);
    [LibraryImport("libc.so.6", SetLastError = true)] internal static partial int epoll_wait(int epfd, EpollEvent* ev, int count, int timeout);
    [LibraryImport("libc.so.6", SetLastError = true)] internal static partial int socket(int domain, int type, int protocol);
    [LibraryImport("libc.so.6", SetLastError = true)] private static partial int setsockopt(int fd, int level, int name, int* value, uint length);
    [LibraryImport("libc.so.6", SetLastError = true)] private static partial int bind(int fd, Address* address, uint length);
    [LibraryImport("libc.so.6", SetLastError = true)] private static partial int listen(int fd, int backlog);
    [LibraryImport("libc.so.6", SetLastError = true)] internal static partial int accept4(int fd, nint address, nint length, int flags);
    [LibraryImport("libc.so.6", SetLastError = true)] internal static partial nint recv(int fd, byte* buffer, nuint length, int flags);
    [LibraryImport("libc.so.6", SetLastError = true)] internal static partial nint send(int fd, byte* buffer, nuint length, int flags);
    [LibraryImport("libc.so.6", SetLastError = true)] internal static partial int close(int fd);
    internal static int Check(int value, string operation)
    {
        if (value < 0) throw new IOException($"{operation}: errno={Marshal.GetLastPInvokeError()}");
        return value;
    }
    internal static void NoDelay(int fd)
    {
        int enabled = 1;
        Check(setsockopt(fd, 6, 1, &enabled, 4), "TCP_NODELAY");
    }
    internal static int Listener(string host, int port)
    {
        int fd = Check(socket(2, 1 | 2048 | 0x80000, 0), "socket");
        try
        {
            int enabled = 1;
            Check(setsockopt(fd, 1, 2, &enabled, 4), "SO_REUSEADDR");
            Check(setsockopt(fd, 1, 15, &enabled, 4), "SO_REUSEPORT");
            var bytes = IPAddress.Parse(host is "*" ? "0.0.0.0" : host == "localhost" ? "127.0.0.1" : host).GetAddressBytes();
            if (bytes.Length != 4) throw new ArgumentException("This Linux prototype currently binds IPv4 addresses.");
            var address = new Address { Family = 2, Port = (ushort)IPAddress.HostToNetworkOrder((short)port), Host = BitConverter.ToUInt32(bytes) };
            Check(bind(fd, &address, 16), "bind");
            Check(listen(fd, 65535), "listen");
            return fd;
        }
        catch { close(fd); throw; }
    }
}
