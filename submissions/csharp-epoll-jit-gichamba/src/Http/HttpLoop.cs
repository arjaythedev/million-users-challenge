using System.Collections.Concurrent;

namespace Feed.Http;

/// <summary>
/// Receives each parsed request on the loop thread. The handler must not wait for a result: it queues the work and
/// returns. It must copy whatever it needs from the request before returning, because the receive buffer is reused.
/// It answers by calling <see cref="Connection.Send"/> now or later from any thread.
/// </summary>
public interface IRequestHandler {
    /// <summary>Handles one request. The next pipelined request on this connection waits for <see cref="Connection.Send"/>.</summary>
    void Handle(Connection conn, in HttpRequest request);
}

/// <summary>A descriptor the loop watches for someone else, such as a Postgres connection the loop drives.</summary>
public interface IFdHandler {
    /// <summary>Called on the loop thread with the epoll flags of an event on the descriptor.</summary>
    void OnEvent(uint flags);
}

/// <summary>
/// One epoll event loop on its own thread: it accepts from its own listeners (if any), reads,
/// parses and dispatches requests, and writes queued output. Sockets are edge-triggered.
/// It also watches descriptors for an <see cref="IFdHandler"/>, runs work posted with <see cref="Run(Action)"/>, and
/// runs the actions given to <see cref="OnPassEnd"/> after every pass over a batch of events.
/// </summary>
public sealed unsafe class HttpLoop : IDisposable {
    /// <summary>Size of the receive buffer and the most one recv call reads.</summary>
    private const int RecvBytes = 64 * 1024;
    /// <summary>Most events taken from one epoll_wait call.</summary>
    private const int MaxEvents = 256;
    /// <summary>How long accepting stops after the process runs out of descriptors.</summary>
    private const int AcceptPauseMs = 100;

    /// <summary>Reply to a malformed request; the connection closes after it.</summary>
    private static readonly byte[] BadRequest =
        "HTTP/1.1 400 Bad Request\r\nContent-Length: 0\r\nConnection: close\r\n\r\n"u8.ToArray();
    /// <summary>Reply when the handler throws, with the error body SPEC.md gives for a 500.</summary>
    private static readonly byte[] ServerError =
        "HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 33\r\n\r\n{\"error\":\"internal server error\"}"u8.ToArray();

    /// <summary>Receives each parsed request.</summary>
    private readonly IRequestHandler _handler;
    /// <summary>Handlers of watched descriptors, indexed by descriptor number.</summary>
    private IFdHandler?[] _foreign = new IFdHandler?[1024];
    /// <summary>Work posted from other threads with <see cref="Run(Action)"/>.</summary>
    private readonly ConcurrentQueue<Action> _tasks = new();
    /// <summary>Actions run after every pass, on the loop thread.</summary>
    private readonly List<Action> _passEnd = [];
    /// <summary>A pass with fewer events than this is followed by a wait of <see cref="_coalesceUs"/>.</summary>
    private readonly int _coalesceMin;
    /// <summary>How long a lightly loaded loop waits before its next epoll_wait, in microseconds; 0 never waits.</summary>
    private readonly int _coalesceUs;
    /// <summary>Counters for accept errors and descriptor exhaustion.</summary>
    private readonly Stats _stats;
    /// <summary>The listening sockets this loop owns, one per port; empty for none.</summary>
    private readonly int[] _listenFds;
    /// <summary>The epoll descriptor.</summary>
    private readonly int _ep;
    /// <summary>Eventfd that wakes the loop for <see cref="Adopt"/>, <see cref="Post"/> and <see cref="Dispose"/>.</summary>
    private readonly int _wakeFd;
    /// <summary>Receive buffer shared by this loop's connections. Handlers copy what they keep.</summary>
    private readonly byte[] _recv = new byte[RecvBytes];
    /// <summary>Connections with unparsed input to process, posted by <see cref="Post"/>.</summary>
    private readonly ConcurrentQueue<Connection> _resume = new();
    /// <summary>Descriptors given to <see cref="Adopt"/> and not yet registered.</summary>
    private readonly ConcurrentQueue<int> _adopt = new();
    /// <summary>Open connections indexed by descriptor number.</summary>
    private Connection?[] _conns = new Connection?[1024];
    /// <summary>The loop thread, set by <see cref="Start"/>.</summary>
    private Thread? _thread;
    /// <summary>Set by <see cref="Dispose"/> to end the loop.</summary>
    private volatile bool _stop;
    /// <summary>1 while a wake-up is pending, so repeated <c>WakeUp</c> calls write the eventfd once.</summary>
    private int _wakeFlag;
    /// <summary>Tracks the pause after descriptor exhaustion.</summary>
    private readonly AcceptPause _pause = new();
    /// <summary>An accepted socket numbered this or above is closed at once, so those numbers stay free for the process.</summary>
    private readonly int _fdCeiling;

    /// <summary>
    /// Creates the loop. <paramref name="listenFds"/> are non-blocking listening sockets this loop owns and closes;
    /// with none, the loop only serves sockets given to <see cref="Adopt"/>. An accepted socket whose descriptor is
    /// <paramref name="fdCeiling"/> or above is closed unanswered.
    /// </summary>
    public HttpLoop(IRequestHandler handler, Stats stats, int[] listenFds, int fdCeiling = int.MaxValue)
        : this(_ => handler, stats, listenFds, fdCeiling, 0, 0) {
    }

    /// <summary>
    /// Creates the loop with a handler made for it by <paramref name="handlerFor"/>, which runs once, inside the
    /// constructor. After a pass of fewer than <paramref name="coalesceMin"/> events the loop waits
    /// <paramref name="coalesceUs"/> microseconds, so its next wake-up serves what arrived meanwhile.
    /// </summary>
    public HttpLoop(Func<HttpLoop, IRequestHandler> handlerFor, Stats stats, int[] listenFds, int fdCeiling, int coalesceMin, int coalesceUs) {
        _stats = stats;
        _coalesceMin = coalesceMin;
        _coalesceUs = coalesceUs;
        _listenFds = listenFds;
        _fdCeiling = fdCeiling;
        _ep = Libc.epoll_create1(Libc.SOCK_CLOEXEC);
        if (_ep < 0) {
            throw new InvalidOperationException($"epoll_create1 failed, errno {Libc.Errno}");
        }

        _wakeFd = Libc.eventfd(0, Libc.SOCK_NONBLOCK | Libc.SOCK_CLOEXEC);
        if (_wakeFd < 0) {
            throw new InvalidOperationException($"eventfd failed, errno {Libc.Errno}");
        }

        Libc.EpollAdd(_ep, _wakeFd, Libc.EPOLLIN | Libc.EPOLLET, (ulong)_wakeFd);
        foreach (int fd in listenFds) {
            Libc.EpollAdd(_ep, fd, Libc.EPOLLIN | Libc.EPOLLET, (ulong)fd);
        }

        _handler = handlerFor(this);
    }

    /// <summary>Runs <paramref name="work"/> on the loop thread soon. Safe from any thread. A failure is counted.</summary>
    public void Run(Action work) {
        _tasks.Enqueue(work);
        WakeUp();
    }

    /// <summary>Adds an action to run on the loop thread after every pass. Call it before <see cref="Start"/>.</summary>
    public void OnPassEnd(Action action) => _passEnd.Add(action);

    /// <summary>
    /// Watches a non-blocking descriptor, edge-triggered for reading and writing, and sends its events to
    /// <paramref name="handler"/>. Call it on the loop thread. Throws if epoll refuses the descriptor.
    /// </summary>
    public void Watch(int fd, IFdHandler handler) {
        if (fd >= _foreign.Length) {
            Array.Resize(ref _foreign, Math.Max(fd + 1, _foreign.Length * 2));
        }

        _foreign[fd] = handler;
        if (Libc.EpollAdd(_ep, fd, Libc.EPOLLIN | Libc.EPOLLOUT | Libc.EPOLLET | Libc.EPOLLRDHUP, (ulong)fd) != 0) {
            _foreign[fd] = null;
            throw new InvalidOperationException($"epoll_ctl add {fd} failed, errno {Libc.Errno}");
        }
    }

    /// <summary>Stops watching a descriptor. Call it on the loop thread before the descriptor is closed.</summary>
    public void Unwatch(int fd) {
        if ((uint)fd < (uint)_foreign.Length) {
            _foreign[fd] = null;
        }

        Libc.EpollDel(_ep, fd);
    }

    /// <summary>Test hook: runs before each connection event, so a test can make dispatch fail. Null in production.</summary>
    internal Action<int>? BeforeConnectionEvent { get; set; }

    /// <summary>
    /// Accepts nothing for up to <paramref name="ms"/> milliseconds, or until <see cref="ResumeAccepts"/>. Clients wait in
    /// the listen backlog meanwhile. Call it before <see cref="Start"/>.
    /// </summary>
    public void HoldAccepts(int ms) => _pause.Start(Environment.TickCount64, ms);

    /// <summary>Ends a hold on the loop's next pass, which accepts what queued up during it. Safe from any thread.</summary>
    public void ResumeAccepts() => Run(() => _pause.Start(Environment.TickCount64, 0));

    /// <summary>Starts the loop thread.</summary>
    public void Start() {
        _thread = new Thread(Run) { IsBackground = true, Name = "http-loop" };
        _thread.Start();
    }

    /// <summary>Hands a connected, non-blocking socket to this loop. Safe from any thread. The loop owns and closes it.</summary>
    public void Adopt(int fd) {
        _adopt.Enqueue(fd);
        WakeUp();
    }

    /// <summary>Queues a connection whose unparsed input can now be processed, from any thread.</summary>
    internal void Post(Connection c) {
        _resume.Enqueue(c);
        WakeUp();
    }

    /// <summary>Stops the loop and closes its descriptors, including open connections.</summary>
    public void Dispose() {
        _stop = true;
        WakeUp();
        _thread?.Join(5000);
        for (int i = 0; i < _conns.Length; i++) {
            _conns[i]?.Release();
        }

        foreach (int fd in _listenFds) {
            Libc.close(fd);
        }

        Libc.close(_wakeFd);
        Libc.close(_ep);
    }

    /// <summary>Wakes the loop thread by writing the eventfd, unless a wake-up is already pending.</summary>
    private void WakeUp() {
        if (Interlocked.Exchange(ref _wakeFlag, 1) != 0) {
            return;
        }

        long one = 1;
        Libc.write(_wakeFd, (byte*)&one, 8);
    }

    /// <summary>
    /// The loop thread body. Waits on epoll and handles wake, accept and connection events until stopped.
    /// It also ends, with a line on stderr, if epoll_wait fails with anything but EINTR: that means a bad descriptor or
    /// buffer, which retrying cannot fix.
    /// </summary>
    private void Run() {
        byte* events = stackalloc byte[MaxEvents * 16];
        while (!_stop) {
            // The listeners are edge-triggered, so when a pause ends, accept what queued up during it.
            if (_pause.TakeExpired(Environment.TickCount64)) {
                foreach (int listener in _listenFds) {
                    GuardedAccept(listener);
                }
            }

            int timeout = _pause.TimeoutMs(Environment.TickCount64);

            int n = Libc.epoll_wait(_ep, events, MaxEvents, timeout);
            if (n < 0) {
                int err = Libc.Errno;
                if (err == Libc.EINTR) {
                    continue;
                }

                Console.Error.WriteLine($"http loop stopped: epoll_wait failed, errno {err}");
                return;
            }

            for (int i = 0; i < n; i++) {
                int fd = (int)Libc.EventData(events, i);
                uint flags = Libc.EventFlags(events, i);
                if (fd == _wakeFd) {
                    GuardedWake();
                } else if (Array.IndexOf(_listenFds, fd) >= 0) {
                    if (!_pause.IsPaused(Environment.TickCount64)) {
                        GuardedAccept(fd);
                    }
                } else if ((uint)fd < (uint)_foreign.Length && _foreign[fd] is { } h) {
                    Guarded(h, flags);
                } else {
                    DispatchConnectionEvent(fd, flags);
                }
            }

            foreach (Action a in _passEnd) {
                Guarded(a);
            }

            // A light pass means a wake-up per request or two. Waiting a little lets the next one serve many.
            if (n < _coalesceMin && _coalesceUs > 0) {
                Libc.usleep((uint)_coalesceUs);
            }
        }
    }

    /// <summary>Runs one event of a watched descriptor. A failure is counted and the loop carries on.</summary>
    private void Guarded(IFdHandler h, uint flags) {
        try {
            h.OnEvent(flags);
        } catch (Exception) {
            _stats.RecordAcceptError();
        }
    }

    /// <summary>Runs posted or end-of-pass work. A failure is counted and the loop carries on.</summary>
    private void Guarded(Action work) {
        try {
            work();
        } catch (Exception) {
            _stats.RecordAcceptError();
        }
    }

    /// <summary>Wakes the loop: see <see cref="OnWake"/>. A failure is counted and the loop carries on.</summary>
    private void GuardedWake() {
        try {
            OnWake();
        } catch (Exception) {
            _stats.RecordAcceptError();
        }
    }

    /// <summary>Accepts from <paramref name="listener"/>: see <see cref="AcceptAll"/>. A failure is counted and the loop carries on.</summary>
    private void GuardedAccept(int listener) {
        try {
            AcceptAll(listener);
        } catch (Exception) {
            _stats.RecordAcceptError();
        }
    }

    /// <summary>Handles one connection event. A failure closes that connection and the loop carries on.</summary>
    private void DispatchConnectionEvent(int fd, uint flags) {
        try {
            BeforeConnectionEvent?.Invoke(fd);
            OnConnectionEvent(fd, flags);
        } catch (Exception) {
            if ((uint)fd < (uint)_conns.Length && _conns[fd] is { } c) {
                CloseQuietly(c);
            }
        }
    }

    /// <summary>Clears the wake flag, registers adopted sockets and processes connections posted by <see cref="Post"/>.</summary>
    private void OnWake() {
        long counter;
        Libc.read(_wakeFd, (byte*)&counter, 8);
        Interlocked.Exchange(ref _wakeFlag, 0);

        while (_adopt.TryDequeue(out int fd)) {
            RegisterOrClose(fd);
        }

        while (_tasks.TryDequeue(out Action? work)) {
            Guarded(work);
        }

        while (_resume.TryDequeue(out Connection? c)) {
            try {
                if (!c.IsClosed && c.PendingLength > 0) {
                    Process(c, c.PendingSpan);
                }
            } catch (Exception) {
                CloseQuietly(c);
            }
        }
    }

    /// <summary>
    /// Accepts from <paramref name="listener"/> until none are pending (it is edge-triggered) and registers each socket.
    /// On EMFILE or ENFILE, or a descriptor past the ceiling, it counts the event and starts an accept pause.
    /// </summary>
    private void AcceptAll(int listener) {
        while (true) {
            int fd = Libc.accept4(listener, null, null, Libc.SOCK_NONBLOCK | Libc.SOCK_CLOEXEC);
            if (fd >= 0) {
                if (fd >= _fdCeiling) {
                    // Clients never get the last descriptors: the runtime, Postgres reconnects and the console may
                    // need one, and failing there can end the process.
                    Libc.close(fd);
                    _stats.RecordEmfile();
                    _pause.Start(Environment.TickCount64, AcceptPauseMs);
                    return;
                }

                // TCP_NODELAY is only a latency tweak: a failure is counted and the connection is kept.
                if (!Libc.TrySetOption(fd, Libc.IPPROTO_TCP, Libc.TCP_NODELAY, 1)) {
                    _stats.RecordAcceptError();
                }

                RegisterOrClose(fd);
                continue;
            }
            int err = Libc.Errno;
            if (err == Libc.EINTR || err == Libc.ECONNABORTED) {
                continue;
            }

            if (err == Libc.EMFILE || err == Libc.ENFILE) {
                // Out of descriptors: stop accepting for a moment, keep serving, count it.
                _stats.RecordEmfile();
                _pause.Start(Environment.TickCount64, AcceptPauseMs);
            }
            return;
        }
    }

    /// <summary>Registers an accepted or adopted socket. On any failure the descriptor is closed, never leaked.</summary>
    private void RegisterOrClose(int fd) {
        try {
            Register(fd);
        } catch (Exception) {
            _stats.RecordAcceptError();
            if ((uint)fd < (uint)_conns.Length && _conns[fd] is { } c) {
                _conns[fd] = null;
                c.Release();
            } else {
                Libc.close(fd);
            }
        }
    }

    /// <summary>
    /// Tracks <paramref name="fd"/> as a connection and adds it to the epoll set (edge-triggered, read and write).
    /// If epoll refuses it, the connection is released.
    /// </summary>
    private void Register(int fd) {
        if (fd >= _conns.Length) {
            Array.Resize(ref _conns, Math.Max(fd + 1, _conns.Length * 2));
        }

        Connection c = new(this, fd);
        _conns[fd] = c;
        if (Libc.EpollAdd(_ep, fd, Libc.EPOLLIN | Libc.EPOLLOUT | Libc.EPOLLET, (ulong)fd) != 0) {
            _conns[fd] = null;
            c.Release();
        }
    }

    /// <summary>Handles one epoll event: reads on readable, hang-up or error, flushes queued output on writable, and closes on hang-up or error.</summary>
    private void OnConnectionEvent(int fd, uint flags) {
        if ((uint)fd >= (uint)_conns.Length) {
            return;
        }

        Connection? c = _conns[fd];
        if (c is null) {
            return;
        }

        if ((flags & (Libc.EPOLLIN | Libc.EPOLLHUP | Libc.EPOLLERR)) != 0 && !OnReadable(c)) {
            Close(c);
            return;
        }
        if ((flags & Libc.EPOLLOUT) != 0 && !c.IsClosed) {
            if (c.FlushOutput()) {
                Process(c, c.PendingSpan);
            }
        }
        if ((flags & (Libc.EPOLLHUP | Libc.EPOLLERR)) != 0) {
            Close(c);
        }
    }

    /// <summary>
    /// Reads and feeds the parser until the socket is drained; a short read counts as drained. Returns false when
    /// the client is gone, a read fails, or its unparsed input is too large.
    /// </summary>
    private bool OnReadable(Connection c) {
        while (true) {
            long n;
            fixed (byte* p = _recv) {
                n = Libc.recv(c.Fd, p, RecvBytes, 0);
            }

            if (n > 0) {
                if (!Feed(c, new ReadOnlySpan<byte>(_recv, 0, (int)n))) {
                    return false;
                }

                if (c.IsClosed) {
                    return false;
                }

                if (n < RecvBytes) {
                    return true;
                }

                continue;
            }
            if (n == 0) {
                return false;
            }

            int err = Libc.Errno;
            if (err == Libc.EINTR) {
                continue;
            }

            return err == Libc.EAGAIN;
        }
    }

    /// <summary>
    /// Parses fresh bytes straight from the receive buffer when nothing is pending, otherwise appends them to the
    /// pending input first. Returns false when the pending input would grow too large.
    /// </summary>
    private bool Feed(Connection c, ReadOnlySpan<byte> fresh) {
        if (c.PendingLength == 0) {
            Process(c, fresh);
            return true;
        }
        if (!c.Append(fresh)) {
            return false;
        }

        Process(c, c.PendingSpan);
        return true;
    }

    /// <summary>
    /// Parses and dispatches as many whole requests as the connection can take now, one
    /// outstanding at a time, and keeps what is left for later.
    /// </summary>
    private void Process(Connection c, ReadOnlySpan<byte> data) {
        while (true) {
            int off = 0;
            bool needMore = false;
            while (off < data.Length) {
                if (!c.TryBegin()) {
                    break;
                }

                ParseStatus status = HttpParser.TryParse(data[off..], out HttpRequest request, out int used);
                if (status == ParseStatus.NeedMore) {
                    c.CancelBegin();
                    needMore = true;
                    break;
                }
                if (status == ParseStatus.Bad) {
                    c.SendAndClose(BadRequest);
                    return;
                }
                off += used;
                c.SetKeepAlive(request.KeepAlive);
                try {
                    _handler.Handle(c, in request);
                } catch (Exception) {
                    c.Send(ServerError);
                }
            }

            if (!c.StoreLeftover(data[off..], needMore)) {
                return;
            }

            data = c.PendingSpan;
        }
    }

    /// <summary>Closes a connection whose handling failed; a second failure while closing is ignored, as the loop must carry on.</summary>
    private void CloseQuietly(Connection c) {
        try {
            Close(c);
        } catch (Exception) {
            // The descriptor may already be gone; there is nothing more to release.
        }
    }

    /// <summary>Releases the connection and clears its slot.</summary>
    private void Close(Connection c) {
        int fd = c.Fd;
        c.Release();
        if ((uint)fd < (uint)_conns.Length && ReferenceEquals(_conns[fd], c)) {
            _conns[fd] = null;
        }
    }
}
