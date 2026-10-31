using System.ComponentModel;
using System.Runtime.InteropServices;

namespace MillionReactor;

// A controlled readiness-only comparison with EpollPoller. Socket reads/writes and
// libpq are unchanged; this does not implement io_uring RECV/SEND or fixed files.
internal sealed unsafe partial class UringPoller : IEventPoller
{
    private const string Library = "liburing-ffi.so.2";
    private const int QueueEntries = 4096;
    private const uint SupportedEvents = 1 | 4 | 8 | 16 | 8192;
    private const int Interrupted = 4, Busy = 16, TimedOut = 62;
    private const uint NoDrop = 1 << 1;
    private nint ring;
    private bool initialized;
    private uint generation;
    private readonly Dictionary<int, Registration> watched = new();
    private readonly Queue<Ready> ready = new();
    private readonly List<ulong> rearm = new(512);

    private struct Registration
    {
        internal uint Mask;
        internal ulong Token;
        internal bool Armed;
    }

    private readonly record struct Ready(ulong Token, uint Events);

    // Pinned liburing 2.5, Linux x86-64 ABI. install-uring.sh verifies these sizes
    // and offsets with the C compiler before installing the library. All ring
    // operations use liburing; managed code does not manipulate ring indexes.
    [StructLayout(LayoutKind.Explicit, Size = 216)]
    private struct NativeRing
    {
        [FieldOffset(200)] internal uint Features;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct Completion
    {
        internal ulong Token;
        internal int Result;
        internal uint Flags;
    }

    [StructLayout(LayoutKind.Sequential)]
    private struct KernelTimespec
    {
        internal long Seconds;
        internal long Nanoseconds;
    }

    public UringPoller()
    {
        if (!OperatingSystem.IsLinux() || RuntimeInformation.ProcessArchitecture != Architecture.X64)
            throw new UringUnavailableException("io_uring comparison requires Linux x86-64.");
        try
        {
            if (MajorVersion() != 2 || MinorVersion() != 5)
                throw new UringUnavailableException("io_uring comparison requires the pinned liburing-ffi 2.5 library.");
            ring = (nint)NativeMemory.AllocZeroed((nuint)sizeof(NativeRing));
            int result = QueueInit(QueueEntries, ring, 0);
            if (result < 0)
            {
                if (result is -1 or -13 or -22 or -38 or -95)
                    throw new UringUnavailableException($"io_uring_setup is unavailable or restricted: {ErrorText(result)}");
                throw Failure("io_uring_queue_init", result);
            }
            initialized = true;
            if ((((NativeRing*)ring)->Features & NoDrop) == 0)
                throw new UringUnavailableException("io_uring IORING_FEAT_NODROP is required to preserve readiness completions.");
            nint probe = GetProbeRing(ring);
            if (probe == 0) throw new IOException("io_uring opcode probing failed; no capability verdict is available.");
            try
            {
                if (OpcodeSupported(probe, 6) == 0 || OpcodeSupported(probe, 7) == 0)
                    throw new UringUnavailableException("io_uring POLL_ADD and POLL_REMOVE are required.");
            }
            finally { FreeProbe(probe); }
        }
        catch (DllNotFoundException error)
        {
            Dispose();
            throw new UringUnavailableException("Pinned liburing-ffi.so.2 is missing; run install-uring.sh and copy its library beside the app.", error);
        }
        catch (EntryPointNotFoundException error)
        {
            Dispose();
            throw new UringUnavailableException("The loaded library does not expose liburing 2.5 FFI helpers.", error);
        }
        catch
        {
            Dispose();
            throw;
        }
    }

    public void Watch(int fd, uint events)
    {
        ObjectDisposedException.ThrowIf(ring == 0, this);
        if (fd < 0) throw new ArgumentOutOfRangeException(nameof(fd));
        if ((events & ~SupportedEvents) != 0)
            throw new ArgumentOutOfRangeException(nameof(events), "Only IN, OUT, ERR, HUP and RDHUP readiness is supported.");
        bool exists = watched.TryGetValue(fd, out Registration previous);
        if (exists && previous.Mask == events) return;
        if (generation == uint.MaxValue)
            throw new InvalidOperationException("io_uring registration generation exhausted; refusing token reuse.");
        ulong token = ((ulong)++generation << 32) | (uint)fd;
        if (exists && previous.Armed) Cancel(previous.Token);
        var registration = new Registration { Mask = events, Token = token, Armed = true };
        // Change the generation before any CQ drain, so late completions from
        // a previous registration or a reused file descriptor are discarded.
        watched[fd] = registration;
        AddPoll(fd, registration);
    }

    public void Remove(int fd)
    {
        ObjectDisposedException.ThrowIf(ring == 0, this);
        if (watched.Remove(fd, out Registration registration) && registration.Armed)
            Cancel(registration.Token);
    }

    public bool IsCurrent(PollEvent ev) => watched.TryGetValue(ev.FileDescriptor, out Registration current) && current.Token == ev.Token;

    public int Wait(Span<PollEvent> events, int timeoutMs)
    {
        ObjectDisposedException.ThrowIf(ring == 0, this);
        if (events.IsEmpty) throw new ArgumentException("An event destination is required.", nameof(events));
        if (timeoutMs < -1) throw new ArgumentOutOfRangeException(nameof(timeoutMs));
        long deadline = timeoutMs < 0 ? long.MaxValue : Environment.TickCount64 + timeoutMs;

        // One-shot polls are rearmed only after the caller has handled the prior
        // batch. Watch with an unchanged mask is therefore still a no-op.
        foreach (ulong token in rearm)
        {
            int fd = (int)(uint)token;
            if (!watched.TryGetValue(fd, out Registration registration) ||
                registration.Token != token || registration.Armed) continue;
            registration.Armed = true;
            watched[fd] = registration;
            AddPoll(fd, registration);
        }
        rearm.Clear();
        Submit();

        while (true)
        {
            DrainCompletions();
            int count = 0;
            while (count < events.Length && ready.TryDequeue(out Ready completed))
            {
                int fd = (int)(uint)completed.Token;
                if (!watched.TryGetValue(fd, out Registration registration) || registration.Token != completed.Token)
                    continue;
                events[count++] = new(fd, completed.Events, completed.Token);
                rearm.Add(completed.Token);
            }
            if (count != 0) return count;
            long remaining = deadline - Environment.TickCount64;
            if (timeoutMs == 0 || (timeoutMs > 0 && remaining <= 0)) return 0;
            nint completion;
            int result;
            if (timeoutMs < 0) result = WaitCqe(ring, &completion);
            else
            {
                var timeout = new KernelTimespec { Seconds = remaining / 1000, Nanoseconds = remaining % 1000 * 1_000_000 };
                result = WaitCqeTimeout(ring, &completion, &timeout);
            }
            if (result is -TimedOut or -Interrupted) return 0;
            if (result < 0) throw Failure("io_uring_wait_cqe", result);
        }
    }

    private void AddPoll(int fd, Registration registration)
    {
        nint sqe = AcquireSqe();
        PrepPollAdd(sqe, fd, registration.Mask);
        SetData64(sqe, registration.Token);
    }

    private void Cancel(ulong token)
    {
        nint sqe = AcquireSqe();
        PrepPollRemove(sqe, token);
        SetData64(sqe, 0); // Cancellation acknowledgements are not socket events.
    }

    private nint AcquireSqe()
    {
        nint sqe = GetSqe(ring);
        if (sqe != 0) return sqe;
        Submit();
        sqe = GetSqe(ring);
        return sqe != 0 ? sqe : throw new IOException("io_uring submission queue made no progress.");
    }

    private void Submit()
    {
        while (true)
        {
            int result = SubmitNative(ring);
            if (result >= 0) return;
            if (result == -Interrupted) continue;
            // NODROP kernels can backpressure submissions while the CQ overflow
            // list is full. Preserve those readiness events before retrying.
            if (result == -Busy && DrainCompletions() != 0) continue;
            throw Failure("io_uring_submit", result);
        }
    }

    private int DrainCompletions()
    {
        nint* completions = stackalloc nint[512];
        uint count = PeekBatch(ring, completions, 512);
        try
        {
            for (int i = 0; i < count; i++)
            {
                Completion completion = *(Completion*)completions[i];
                if (completion.Token == 0)
                {
                    // Completion can win the race with removal. Both results
                    // leave the old generation invalid and are expected.
                    if (completion.Result < 0 && completion.Result is not (-2 or -114 or -125))
                        throw Failure("io_uring_poll_remove", completion.Result);
                    continue;
                }
                int fd = (int)(uint)completion.Token;
                if (!watched.TryGetValue(fd, out Registration registration) || registration.Token != completion.Token)
                    continue;
                if (completion.Result < 0) throw Failure("io_uring_poll_add", completion.Result);
                if ((completion.Flags & 2) != 0)
                    throw new IOException("Unexpected multishot completion for a one-shot readiness poll.");
                registration.Armed = false;
                watched[fd] = registration;
                ready.Enqueue(new(completion.Token, (uint)completion.Result));
            }
        }
        finally { CqAdvance(ring, count); }
        return (int)count;
    }

    // Optional CI capability probe: no database or listener, just a local pair
    // of sockets, readiness, mask replacement and cancellation.
    internal static string Probe()
    {
        using var poller = new UringPoller();
        PollerProbe.Check(poller);
        return "liburing-ffi 2.5; POLL_ADD/POLL_REMOVE; NODROP; readiness, stale registration rejection and replacement passed";
    }

    public void Dispose()
    {
        if (ring == 0) return;
        if (initialized) QueueExit(ring);
        NativeMemory.Free((void*)ring);
        ring = 0;
        initialized = false;
    }

    private static string ErrorText(int negativeErrno) => $"{new Win32Exception(-negativeErrno).Message} (errno {-negativeErrno})";
    private static IOException Failure(string operation, int result) => new($"{operation}: {ErrorText(result)}");

    [LibraryImport(Library, EntryPoint = "io_uring_major_version")] private static partial int MajorVersion();
    [LibraryImport(Library, EntryPoint = "io_uring_minor_version")] private static partial int MinorVersion();
    [LibraryImport(Library, EntryPoint = "io_uring_queue_init")] private static partial int QueueInit(uint entries, nint ring, uint flags);
    [LibraryImport(Library, EntryPoint = "io_uring_queue_exit")] private static partial void QueueExit(nint ring);
    [LibraryImport(Library, EntryPoint = "io_uring_get_probe_ring")] private static partial nint GetProbeRing(nint ring);
    [LibraryImport(Library, EntryPoint = "io_uring_free_probe")] private static partial void FreeProbe(nint probe);
    [LibraryImport(Library, EntryPoint = "io_uring_opcode_supported")] private static partial int OpcodeSupported(nint probe, int opcode);
    [LibraryImport(Library, EntryPoint = "io_uring_get_sqe")] private static partial nint GetSqe(nint ring);
    [LibraryImport(Library, EntryPoint = "io_uring_prep_poll_add")] private static partial void PrepPollAdd(nint sqe, int fd, uint mask);
    [LibraryImport(Library, EntryPoint = "io_uring_prep_poll_remove")] private static partial void PrepPollRemove(nint sqe, ulong token);
    [LibraryImport(Library, EntryPoint = "io_uring_sqe_set_data64")] private static partial void SetData64(nint sqe, ulong token);
    [LibraryImport(Library, EntryPoint = "io_uring_submit")] private static partial int SubmitNative(nint ring);
    [LibraryImport(Library, EntryPoint = "io_uring_peek_batch_cqe")] private static partial uint PeekBatch(nint ring, nint* completions, uint count);
    [LibraryImport(Library, EntryPoint = "io_uring_cq_advance")] private static partial void CqAdvance(nint ring, uint count);
    [LibraryImport(Library, EntryPoint = "io_uring_wait_cqe")] private static partial int WaitCqe(nint ring, nint* completion);
    [LibraryImport(Library, EntryPoint = "io_uring_wait_cqe_timeout")] private static partial int WaitCqeTimeout(nint ring, nint* completion, KernelTimespec* timeout);
}

internal sealed class UringUnavailableException(string message, Exception? inner = null) : NotSupportedException(message, inner);
