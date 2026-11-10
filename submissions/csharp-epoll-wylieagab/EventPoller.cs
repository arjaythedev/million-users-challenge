using System.Runtime.InteropServices;

namespace MillionReactor;

public readonly record struct PollEvent(int FileDescriptor, uint Events, ulong Token);
public interface IEventPoller : IDisposable
{
    void Watch(int fd, uint events);
    void Remove(int fd);
    int Wait(Span<PollEvent> events, int timeoutMs);
    bool IsCurrent(PollEvent ev);
}

internal sealed unsafe class EpollPoller : IEventPoller
{
    private readonly int handle = Linux.Check(Linux.epoll_create1(0x80000), "epoll_create1");
    private readonly Dictionary<int, Registration> watched = new();
    private uint generation;
    private readonly record struct Registration(uint Events, ulong Token);
    public void Watch(int fd, uint events)
    {
        var exists = watched.TryGetValue(fd, out Registration previous);
        if (exists && previous.Events == events) return;
        if (!exists && generation == uint.MaxValue)
            throw new InvalidOperationException("epoll registration generation exhausted; refusing token reuse.");
        ulong token = exists ? previous.Token : ((ulong)++generation << 32) | (uint)fd;
        var ev = new Linux.EpollEvent { Events = events, Data = token };
        Linux.Check(Linux.epoll_ctl(handle, exists ? 3 : 1, fd, &ev), "epoll_ctl");
        watched[fd] = new(events, token);
    }
    public void Remove(int fd)
    {
        if (watched.Remove(fd)) Linux.epoll_ctl(handle, 2, fd, null);
    }
    public int Wait(Span<PollEvent> events, int timeoutMs)
    {
        Span<Linux.EpollEvent> native = stackalloc Linux.EpollEvent[Math.Min(events.Length, 512)];
        int count;
        fixed (Linux.EpollEvent* p = native) count = Linux.epoll_wait(handle, p, native.Length, timeoutMs);
        if (count < 0 && Marshal.GetLastPInvokeError() == 4) return 0;
        Linux.Check(count, "epoll_wait");
        for (int i = 0; i < count; i++) events[i] = new((int)(uint)native[i].Data, native[i].Events, native[i].Data);
        return count;
    }
    public bool IsCurrent(PollEvent ev) => watched.TryGetValue(ev.FileDescriptor, out Registration current) && current.Token == ev.Token;
    internal static string Probe()
    {
        using var poller = new EpollPoller();
        PollerProbe.Check(poller);
        return "epoll readiness, unchanged interest, stale registration rejection and replacement passed";
    }
    public void Dispose() => Linux.close(handle);
}

// Shared, local-only transport probe. Keeping a returned event across Remove /
// Watch reproduces the stale-batch identity issue without touching unrelated fds.
internal static unsafe partial class PollerProbe
{
    internal static void Check(IEventPoller poller)
    {
        int* sockets = stackalloc int[2];
        Linux.Check(SocketPair(1, 1 | 0x800 | 0x80000, 0, sockets), "socketpair probe");
        try
        {
            Span<PollEvent> events = stackalloc PollEvent[4];
            poller.Watch(sockets[0], Linux.Read | Linux.ReadHangup);
            byte value = 37;
            if (Write(sockets[1], &value, 1) != 1) throw new IOException("Poller probe socket write failed.");
            int count = poller.Wait(events, 1000);
            if (count != 1 || events[0].FileDescriptor != sockets[0] || (events[0].Events & Linux.Read) == 0 || !poller.IsCurrent(events[0]))
                throw new IOException("Poller probe did not return current input readiness.");
            PollEvent stale = events[0];
            poller.Watch(sockets[0], Linux.Read | Linux.ReadHangup);
            if (!poller.IsCurrent(stale)) throw new IOException("Unchanged interest invalidated a current event.");
            if (Read(sockets[0], &value, 1) != 1 || value != 37) throw new IOException("Poller probe socket read failed.");
            poller.Remove(sockets[0]);
            if (poller.IsCurrent(stale)) throw new IOException("Removed event remained current.");
            poller.Watch(sockets[0], Linux.Read | Linux.ReadHangup);
            if (poller.IsCurrent(stale)) throw new IOException("Stale batch event matched a replacement registration.");
            value = 38;
            if (Write(sockets[1], &value, 1) != 1) throw new IOException("Replacement probe socket write failed.");
            count = poller.Wait(events, 1000);
            if (count != 1 || events[0].FileDescriptor != sockets[0] || (events[0].Events & Linux.Read) == 0 ||
                !poller.IsCurrent(events[0]) || events[0].Token == stale.Token)
                throw new IOException("Replacement registration did not produce a fresh current event.");
            if (Read(sockets[0], &value, 1) != 1 || value != 38) throw new IOException("Replacement probe socket read failed.");
            // Rearm the single-shot backend while the socket is quiet, then
            // replace an outstanding poll rather than only a completed one.
            if (poller.Wait(events, 0) != 0) throw new IOException("Drained probe socket remained readable.");
            poller.Watch(sockets[0], Linux.Read | Linux.Write);
            count = poller.Wait(events, 1000);
            if (count != 1 || events[0].FileDescriptor != sockets[0] || (events[0].Events & Linux.Write) == 0 || !poller.IsCurrent(events[0]))
                throw new IOException("Poller probe did not preserve replacement interest readiness.");
            stale = events[0];
            poller.Remove(sockets[0]);
            if (poller.IsCurrent(stale) || poller.Wait(events, 0) != 0)
                throw new IOException("Poller probe returned removed readiness.");
        }
        finally
        {
            poller.Remove(sockets[0]);
            Linux.close(sockets[0]);
            Linux.close(sockets[1]);
        }
    }

    [LibraryImport("libc.so.6", EntryPoint = "socketpair", SetLastError = true)] private static partial int SocketPair(int domain, int type, int protocol, int* sockets);
    [LibraryImport("libc.so.6", EntryPoint = "write", SetLastError = true)] private static partial nint Write(int fd, byte* data, nuint length);
    [LibraryImport("libc.so.6", EntryPoint = "read", SetLastError = true)] private static partial nint Read(int fd, byte* data, nuint length);
}
