package feed.net;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_INT_UNALIGNED;
import static java.lang.foreign.ValueLayout.JAVA_LONG;
import static java.lang.foreign.ValueLayout.JAVA_LONG_UNALIGNED;
import static java.lang.foreign.ValueLayout.JAVA_SHORT;

import java.io.IOException;
import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.StructLayout;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.VarHandle;
import java.net.Inet4Address;
import java.net.InetAddress;
import java.util.BitSet;

/**
 * epoll, called directly through the Foreign Function & Memory API (Linux, x86_64 or aarch64). A connection is its
 * fd and nothing else: no SocketChannel, no SelectionKey, no buffer while idle. Level-triggered, one epoll instance
 * per worker. Bytes pass through one native buffer per Io and are copied to and from the caller's arrays (requests
 * and responses are a few hundred bytes, so a copy costs far less than the syscall around it).
 */
public final class EpollIo implements Io {

    // Linux constants (the same on x86_64 and aarch64).
    private static final int AF_INET = 2;
    private static final int SOCK_STREAM = 1;
    private static final int SOCK_NONBLOCK = 0x800;
    private static final int SOCK_CLOEXEC = 0x80000;
    private static final int SOL_SOCKET = 1;
    private static final int SO_REUSEADDR = 2;
    private static final int SO_ERROR = 4;
    private static final int SO_REUSEPORT = 15;
    private static final int IPPROTO_TCP = 6;
    private static final int TCP_NODELAY = 1;
    private static final int TCP_DEFER_ACCEPT = 9;
    private static final int EPOLLIN = 0x1;
    private static final int EPOLLOUT = 0x4;
    private static final int EPOLLERR = 0x8;
    private static final int EPOLLHUP = 0x10;
    private static final int EPOLLRDHUP = 0x2000;
    private static final int EPOLL_CTL_ADD = 1;
    private static final int EPOLL_CTL_MOD = 3;
    private static final int EPOLL_CLOEXEC = 0x80000;
    private static final int MSG_NOSIGNAL = 0x4000;
    private static final int EINTR = 4;
    private static final int EAGAIN = 11;
    private static final int ENFILE = 23;
    private static final int EMFILE = 24;
    private static final int EINPROGRESS = 115;

    private static final int READ_EVENTS = EPOLLIN | EPOLLRDHUP;
    /** Seconds a connection may stay silent before it is accepted anyway. */
    private static final int DEFER_ACCEPT_SECONDS = 5;
    private static final int MAX_EVENTS = 1024;
    private static final int BUFFER = 256 * 1024;

    /** struct epoll_event is packed on x86_64 (12 bytes), naturally aligned elsewhere (16 bytes). */
    private static final boolean PACKED = switch (System.getProperty("os.arch")) {
        case "amd64", "x86_64" -> true;
        default -> false;
    };
    private static final int EVENT_SIZE = PACKED ? 12 : 16;
    private static final int EVENT_DATA = PACKED ? 4 : 8;

    private static final Linker LINKER = Linker.nativeLinker();
    private static final StructLayout CALL_STATE = Linker.Option.captureStateLayout();
    private static final VarHandle ERRNO =
            CALL_STATE.varHandle(MemoryLayout.PathElement.groupElement("errno")).withInvokeExactBehavior();
    private static final Linker.Option ERRNO_OPTION = Linker.Option.captureCallState("errno");

    private static final MethodHandle SOCKET = errno("socket", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT));
    private static final MethodHandle SETSOCKOPT = errno("setsockopt",
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT));
    private static final MethodHandle GETSOCKOPT = errno("getsockopt",
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS, ADDRESS));
    private static final MethodHandle BIND = errno("bind", FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT));
    private static final MethodHandle LISTEN = errno("listen", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT));
    private static final MethodHandle GETSOCKNAME = errno("getsockname",
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, ADDRESS));
    private static final MethodHandle CONNECT = errno("connect", FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT));
    private static final MethodHandle ACCEPT4 = errno("accept4",
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, ADDRESS, JAVA_INT));
    private static final MethodHandle RECV = errno("recv",
            FunctionDescriptor.of(JAVA_LONG, JAVA_INT, ADDRESS, JAVA_LONG, JAVA_INT));
    private static final MethodHandle SEND = errno("send",
            FunctionDescriptor.of(JAVA_LONG, JAVA_INT, ADDRESS, JAVA_LONG, JAVA_INT));
    private static final MethodHandle READ = errno("read", FunctionDescriptor.of(JAVA_LONG, JAVA_INT, ADDRESS, JAVA_LONG));
    private static final MethodHandle WRITE = errno("write", FunctionDescriptor.of(JAVA_LONG, JAVA_INT, ADDRESS, JAVA_LONG));
    private static final MethodHandle CLOSE = errno("close", FunctionDescriptor.of(JAVA_INT, JAVA_INT));
    private static final MethodHandle EPOLL_CREATE1 = errno("epoll_create1", FunctionDescriptor.of(JAVA_INT, JAVA_INT));
    private static final MethodHandle EPOLL_CTL = errno("epoll_ctl",
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS));
    private static final MethodHandle EPOLL_WAIT = errno("epoll_wait",
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT, JAVA_INT));
    private static final MethodHandle EVENTFD = errno("eventfd", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT));

    private static MethodHandle errno(String name, FunctionDescriptor descriptor) {
        MemorySegment symbol = LINKER.defaultLookup().find(name)
                .orElseThrow(() -> new UnsatisfiedLinkError("libc has no " + name));
        return LINKER.downcallHandle(symbol, descriptor, ERRNO_OPTION);
    }

    private final Arena arena = Arena.ofShared();
    private final MemorySegment state = arena.allocate(CALL_STATE);
    private final MemorySegment events = arena.allocate((long) EVENT_SIZE * MAX_EVENTS, 8);
    private final MemorySegment buffer = arena.allocate(BUFFER, 8);
    private final MemorySegment ctlEvent = arena.allocate(16, 8);
    private final MemorySegment intValue = arena.allocate(JAVA_INT);
    private final MemorySegment lengthValue = arena.allocate(JAVA_INT);
    private final MemorySegment address = arena.allocate(16, 4);
    private final MemorySegment wakeState = arena.allocate(CALL_STATE);
    private final MemorySegment wakeValue = arena.allocate(JAVA_LONG);
    private final int epfd;
    private final int wakeFd;
    private final BitSet open = new BitSet();
    private int[] listeners = new int[0];
    private final BitSet writing = new BitSet(); // watched for EPOLLOUT
    private final BitSet paused = new BitSet();  // no longer watched for reads
    private boolean acceptsPaused;
    private long acceptResumeAt;
    private boolean loggedAcceptLimit;
    private int noDelayInherited = -1; // whether accepted sockets get the listener's TCP_NODELAY; -1: not checked yet
    private static volatile boolean loggedNoDelay;
    private volatile boolean closed;

    private EpollIo() throws IOException {
        epfd = check(epollCreate(), "epoll_create1");
        wakeFd = check(eventfd(), "eventfd");
        ctl(EPOLL_CTL_ADD, wakeFd, EPOLLIN);
        wakeValue.set(JAVA_LONG, 0, 1L);
    }

    static Io open() throws IOException {
        return new EpollIo();
    }

    // ---- Io ----

    @Override
    public int listen(String host, int port) throws IOException {
        int fd = check(socket(SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC), "socket");
        try {
            setInt(fd, SOL_SOCKET, SO_REUSEADDR, 1);
            setInt(fd, SOL_SOCKET, SO_REUSEPORT, 1);
            // Linux copies TCP_NODELAY to accepted sockets, which saves a setsockopt per connection (accept checks).
            setInt(fd, IPPROTO_TCP, TCP_NODELAY, 1);
            // A connection is accepted once its first request is in: one wake-up per new client instead of two.
            setInt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, DEFER_ACCEPT_SECONDS);
            sockaddr(host, port);
            check(call(BIND, fd, address, 16), "bind " + host + ":" + port);
            check(call(LISTEN, fd, 65_535), "listen");
            lengthValue.set(JAVA_INT, 0, 16);
            check(call(GETSOCKNAME, fd, address, lengthValue), "getsockname");
            int bound = ((address.get(JAVA_BYTE, 2) & 0xff) << 8) | (address.get(JAVA_BYTE, 3) & 0xff);
            ctl(EPOLL_CTL_ADD, fd, EPOLLIN);
            listeners = java.util.Arrays.copyOf(listeners, listeners.length + 1);
            listeners[listeners.length - 1] = fd;
            open.set(fd);
            return bound;
        } catch (IOException e) {
            closeFd(fd);
            throw e;
        }
    }

    @Override
    public int connect(String host, int port) throws IOException {
        int fd = check(socket(SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC), "socket");
        try {
            sockaddr(host, port);
            if (call(CONNECT, fd, address, 16) < 0 && errno() != EINPROGRESS) {
                throw new IOException("connect " + host + ":" + port + ": errno " + errno());
            }
            setInt(fd, IPPROTO_TCP, TCP_NODELAY, 1);
            ctl(EPOLL_CTL_ADD, fd, READ_EVENTS | EPOLLOUT); // EPOLLOUT (or ERR/HUP) when the attempt ends
            open.set(fd);
            writing.set(fd);
            return fd;
        } catch (IOException e) {
            closeFd(fd);
            throw e;
        }
    }

    @Override
    public void finishConnect(int id) throws IOException {
        lengthValue.set(JAVA_INT, 0, 4);
        check(call(GETSOCKOPT, id, SOL_SOCKET, SO_ERROR, intValue, lengthValue), "getsockopt");
        int soError = intValue.get(JAVA_INT, 0);
        if (soError != 0) {
            throw new IOException("connect: errno " + soError);
        }
        watchWrites(id, false);
    }

    @Override
    public void pauseReads(int id) {
        paused.set(id);
        watchWrites(id, writing.get(id));
    }

    @Override
    public int poll(int timeoutMillis, Handler handler) throws IOException {
        if (acceptsPaused) {
            long wait = acceptResumeAt - System.nanoTime();
            if (wait <= 0) {
                for (int l : listeners) {
                    ctl(EPOLL_CTL_MOD, l, EPOLLIN);
                }
                acceptsPaused = false;
            } else {
                timeoutMillis = (int) Math.min(timeoutMillis, wait / 1_000_000 + 1);
            }
        }
        int n;
        try {
            n = (int) EPOLL_WAIT.invokeExact(state, epfd, events, MAX_EVENTS, timeoutMillis);
        } catch (Throwable t) {
            throw rethrow(t);
        }
        if (n < 0) {
            if (errno() == EINTR) {
                return 0;
            }
            throw new IOException("epoll_wait: errno " + errno());
        }
        int sockets = n;
        for (int i = 0; i < n; i++) {
            long at = (long) i * EVENT_SIZE;
            int flags = events.get(JAVA_INT_UNALIGNED, at);
            int fd = (int) events.get(JAVA_LONG_UNALIGNED, at + EVENT_DATA);
            if (fd == wakeFd) {
                sockets--;
                try {
                    long ignored = (long) READ.invokeExact(state, wakeFd, buffer, 8L);
                } catch (Throwable t) {
                    throw rethrow(t);
                }
            } else if (isListener(fd)) {
                accept(fd, handler);
            } else {
                if ((flags & (EPOLLIN | EPOLLERR | EPOLLHUP | EPOLLRDHUP)) != 0) {
                    handler.readable(fd);
                }
                if ((flags & EPOLLOUT) != 0 && open.get(fd)) {
                    handler.writable(fd);
                }
            }
        }
        return sockets;
    }

    private void accept(int listener, Handler handler) {
        for (int i = 0; i < 256; i++) {
            int fd;
            try {
                fd = (int) ACCEPT4.invokeExact(state, listener, MemorySegment.NULL, MemorySegment.NULL,
                        SOCK_NONBLOCK | SOCK_CLOEXEC);
            } catch (Throwable t) {
                throw rethrow(t);
            }
            if (fd < 0) {
                int err = errno();
                if (err == EINTR) {
                    continue;
                }
                if (err == EMFILE || err == ENFILE) {
                    pauseAccepts(err);
                } else if (err != EAGAIN) {
                    System.err.println("accept4: errno " + err);
                }
                return;
            }
            try {
                if (noDelayInherited < 0) {
                    checkNoDelayInherited(fd);
                }
                if (noDelayInherited == 0) {
                    setInt(fd, IPPROTO_TCP, TCP_NODELAY, 1);
                }
                ctl(EPOLL_CTL_ADD, fd, READ_EVENTS);
            } catch (IOException e) {
                closeFd(fd);
                continue;
            }
            open.set(fd);
            handler.accepted(fd);
        }
    }

    /** Looks at the first accepted socket: if it has no TCP_NODELAY of its own, every socket gets it set. */
    private void checkNoDelayInherited(int fd) throws IOException {
        lengthValue.set(JAVA_INT, 0, 4);
        check(call(GETSOCKOPT, fd, IPPROTO_TCP, TCP_NODELAY, intValue, lengthValue), "getsockopt");
        noDelayInherited = intValue.get(JAVA_INT, 0) != 0 ? 1 : 0;
        if (!loggedNoDelay) {
            loggedNoDelay = true;
            System.err.println("TCP_NODELAY: " + (noDelayInherited == 1 ? "inherited from the listening socket"
                    : "not inherited, set on each socket"));
        }
    }

    /**
     * Out of fds: the listener stays readable (level-triggered), so stop watching it for 100 ms instead of spinning
     * on accept4 failures; poll() turns it back on. Logged once.
     */
    private void pauseAccepts(int err) {
        if (!loggedAcceptLimit) {
            loggedAcceptLimit = true;
            System.err.println("accept4: errno " + err + " (out of file descriptors); pausing accepts for 100 ms at a time");
        }
        try {
            for (int l : listeners) {
                ctl(EPOLL_CTL_MOD, l, 0);
            }
        } catch (IOException e) {
            return;
        }
        acceptsPaused = true;
        acceptResumeAt = System.nanoTime() + 100_000_000L;
    }

    @Override
    public int read(int id, byte[] b, int off, int len) {
        long n;
        try {
            n = (long) RECV.invokeExact(state, id, buffer, (long) Math.min(len, BUFFER), 0);
        } catch (Throwable t) {
            throw rethrow(t);
        }
        if (n > 0) {
            MemorySegment.copy(buffer, JAVA_BYTE, 0, b, off, (int) n);
            return (int) n;
        }
        if (n == 0) {
            return -1;
        }
        int err = errno();
        return err == EAGAIN || err == EINTR ? 0 : -1;
    }

    @Override
    public int write(int id, byte[] b, int off, int len) {
        int written = 0;
        while (written < len) {
            int chunk = Math.min(len - written, BUFFER);
            MemorySegment.copy(b, off + written, buffer, JAVA_BYTE, 0, chunk);
            long n;
            try {
                n = (long) SEND.invokeExact(state, id, buffer, (long) chunk, MSG_NOSIGNAL);
            } catch (Throwable t) {
                throw rethrow(t);
            }
            if (n < 0) {
                int err = errno();
                if (err == EINTR) {
                    continue;
                }
                return err == EAGAIN ? written : -1;
            }
            written += (int) n;
            if (n < chunk) {
                break;
            }
        }
        return written;
    }

    @Override
    public void watchWrites(int id, boolean on) {
        writing.set(id, on);
        try {
            ctl(EPOLL_CTL_MOD, id, (paused.get(id) ? 0 : READ_EVENTS) | (on ? EPOLLOUT : 0));
        } catch (IOException e) {
            // the socket is going away; its next read reports that
        }
    }

    @Override
    public void close(int id) {
        if (open.get(id)) {
            open.clear(id);
            writing.clear(id);
            paused.clear(id);
            closeFd(id);
        }
    }

    @Override
    public void wakeup() {
        if (closed) {
            return;
        }
        try {
            long ignored = (long) WRITE.invokeExact(wakeState, wakeFd, wakeValue, 8L);
        } catch (IllegalStateException e) {
            // closed while we were here: nothing left to wake
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    @Override
    public void close() {
        if (closed) {
            return;
        }
        closed = true;
        for (int fd = open.nextSetBit(0); fd >= 0; fd = open.nextSetBit(fd + 1)) {
            closeFd(fd);
        }
        open.clear();
        closeFd(wakeFd);
        closeFd(epfd);
        arena.close();
    }

    // ---- helpers ----

    private boolean isListener(int fd) {
        for (int l : listeners) {
            if (l == fd) {
                return true;
            }
        }
        return false;
    }

    private void ctl(int op, int fd, int flags) throws IOException {
        ctlEvent.set(JAVA_INT_UNALIGNED, 0, flags);
        ctlEvent.set(JAVA_LONG_UNALIGNED, EVENT_DATA, fd);
        int result;
        try {
            result = (int) EPOLL_CTL.invokeExact(state, epfd, op, fd, ctlEvent);
        } catch (Throwable t) {
            throw rethrow(t);
        }
        check(result, "epoll_ctl");
    }

    private void setInt(int fd, int level, int option, int value) throws IOException {
        intValue.set(JAVA_INT, 0, value);
        int result;
        try {
            result = (int) SETSOCKOPT.invokeExact(state, fd, level, option, intValue, 4);
        } catch (Throwable t) {
            throw rethrow(t);
        }
        check(result, "setsockopt");
    }

    /** Fills {@link #address} with a sockaddr_in for host:port (IPv4). */
    private void sockaddr(String host, int port) throws IOException {
        byte[] ip = null;
        for (InetAddress a : InetAddress.getAllByName(host)) {
            if (a instanceof Inet4Address) {
                ip = a.getAddress();
                break;
            }
        }
        if (ip == null) {
            throw new IOException("no IPv4 address for " + host);
        }
        address.fill((byte) 0);
        address.set(JAVA_SHORT, 0, (short) AF_INET); // native byte order
        address.set(JAVA_BYTE, 2, (byte) (port >> 8)); // network byte order
        address.set(JAVA_BYTE, 3, (byte) port);
        for (int i = 0; i < 4; i++) {
            address.set(JAVA_BYTE, 4 + i, ip[i]);
        }
    }

    private int socket(int type) {
        return call(SOCKET, AF_INET, type, 0);
    }

    private int epollCreate() {
        try {
            return (int) EPOLL_CREATE1.invokeExact(state, EPOLL_CLOEXEC);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    private int eventfd() {
        try {
            return (int) EVENTFD.invokeExact(state, 0, SOCK_NONBLOCK | SOCK_CLOEXEC);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    private void closeFd(int fd) {
        try {
            int ignored = (int) CLOSE.invokeExact(state, fd);
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    private int errno() {
        return (int) ERRNO.get(state, 0L);
    }

    private int check(int result, String what) throws IOException {
        if (result < 0) {
            throw new IOException(what + ": errno " + errno());
        }
        return result;
    }

    /** For the setup calls that aren't on the request path (argument types vary, so invokeWithArguments). */
    private int call(MethodHandle handle, Object... args) {
        Object[] all = new Object[args.length + 1];
        all[0] = state;
        System.arraycopy(args, 0, all, 1, args.length);
        try {
            Object result = handle.invokeWithArguments(all);
            return result instanceof Long l ? (int) (long) l : (Integer) result;
        } catch (Throwable t) {
            throw rethrow(t);
        }
    }

    private static RuntimeException rethrow(Throwable t) {
        if (t instanceof RuntimeException r) {
            return r;
        }
        if (t instanceof Error e) {
            throw e;
        }
        return new IllegalStateException(t);
    }
}
