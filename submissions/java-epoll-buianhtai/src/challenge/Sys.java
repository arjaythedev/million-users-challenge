package challenge;

import static java.lang.foreign.ValueLayout.ADDRESS;
import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT;
import static java.lang.foreign.ValueLayout.JAVA_LONG;
import static java.lang.foreign.ValueLayout.JAVA_LONG_UNALIGNED;
import static java.lang.foreign.ValueLayout.JAVA_SHORT;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemoryLayout;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.StructLayout;
import java.lang.invoke.MethodHandle;
import java.lang.invoke.VarHandle;

/**
 * Linux system calls through the foreign function API. One instance per event loop: it owns the errno capture
 * area and the scratch memory the calls need. Failing calls return -errno.
 */
final class Sys {
    static final int SOCK_STREAM = 1, SOCK_NONBLOCK = 0x800, SOCK_CLOEXEC = 0x80000;
    static final int SOL_SOCKET = 1, SO_REUSEADDR = 2, SO_ERROR = 4, SO_REUSEPORT = 15;
    static final int IPPROTO_TCP = 6, TCP_NODELAY = 1, TCP_DEFER_ACCEPT = 9;
    static final int EPOLLIN = 1, EPOLLOUT = 4, EPOLLERR = 8, EPOLLHUP = 0x10;
    static final int EPOLL_CTL_ADD = 1, EPOLL_CTL_MOD = 3;
    static final int EINTR = 4, EAGAIN = 11, EINPROGRESS = 115;
    static final int MSG_NOSIGNAL = 0x4000;

    /** epoll_event is packed on x86-64 (12 bytes, data at 4) and padded elsewhere (16 bytes, data at 8). */
    private static final boolean X86 = switch (System.getProperty("os.arch")) {
        case "amd64", "x86_64" -> true;
        case "aarch64" -> false;
        default -> throw new IllegalStateException("unsupported architecture " + System.getProperty("os.arch"));
    };
    static final int EV_SIZE = X86 ? 12 : 16, EV_DATA = X86 ? 4 : 8;

    private static final Linker LINKER = Linker.nativeLinker();
    private static final Linker.Option ERRNO = Linker.Option.captureCallState("errno");
    private static final StructLayout STATE = Linker.Option.captureStateLayout();
    private static final VarHandle ERRNO_VH = STATE.varHandle(MemoryLayout.PathElement.groupElement("errno"));

    private static MethodHandle handle(String name, FunctionDescriptor fd, boolean errno) {
        MemorySegment sym = LINKER.defaultLookup().find(name).orElseThrow();
        return errno ? LINKER.downcallHandle(sym, fd, ERRNO) : LINKER.downcallHandle(sym, fd);
    }

    private static final MethodHandle EPOLL_CREATE1 = handle("epoll_create1", FunctionDescriptor.of(JAVA_INT, JAVA_INT), true);
    private static final MethodHandle EPOLL_CTL =
            handle("epoll_ctl", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS), true);
    private static final MethodHandle EPOLL_WAIT =
            handle("epoll_wait", FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT, JAVA_INT), true);
    private static final MethodHandle SOCKET =
            handle("socket", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT), true);
    private static final MethodHandle SETSOCKOPT = handle("setsockopt",
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT), true);
    private static final MethodHandle GETSOCKOPT = handle("getsockopt",
            FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT, JAVA_INT, ADDRESS, ADDRESS), true);
    private static final MethodHandle BIND =
            handle("bind", FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT), true);
    private static final MethodHandle LISTEN = handle("listen", FunctionDescriptor.of(JAVA_INT, JAVA_INT, JAVA_INT), true);
    private static final MethodHandle ACCEPT4 =
            handle("accept4", FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, ADDRESS, JAVA_INT), true);
    private static final MethodHandle CONNECT =
            handle("connect", FunctionDescriptor.of(JAVA_INT, JAVA_INT, ADDRESS, JAVA_INT), true);
    private static final MethodHandle READ =
            handle("read", FunctionDescriptor.of(JAVA_LONG, JAVA_INT, ADDRESS, JAVA_LONG), true);
    private static final MethodHandle SEND =
            handle("send", FunctionDescriptor.of(JAVA_LONG, JAVA_INT, ADDRESS, JAVA_LONG, JAVA_INT), true);
    private static final MethodHandle CLOSE = handle("close", FunctionDescriptor.of(JAVA_INT, JAVA_INT), false);

    private final MemorySegment cap = Arena.global().allocate(STATE);
    private final MemorySegment ev = Arena.global().allocate(16, 8);
    private final MemorySegment tmp = Arena.global().allocate(16, 8);
    private final MemorySegment sa = Arena.global().allocate(16, 8);

    private int errno() {
        return (int) ERRNO_VH.get(cap, 0L);
    }

    private static AssertionError bug(Throwable t) {
        return new AssertionError(t);
    }

    int epollCreate() {
        try {
            int r = (int) EPOLL_CREATE1.invokeExact(cap, 0);
            return r >= 0 ? r : -errno();
        } catch (Throwable t) {
            throw bug(t);
        }
    }

    /** The registered data is the fd itself. */
    int epollCtl(int ep, int op, int fd, int events) {
        ev.set(JAVA_INT, 0, events);
        ev.set(JAVA_LONG_UNALIGNED, EV_DATA, fd);
        try {
            int r = (int) EPOLL_CTL.invokeExact(cap, ep, op, fd, ev);
            return r >= 0 ? r : -errno();
        } catch (Throwable t) {
            throw bug(t);
        }
    }

    int epollWait(int ep, MemorySegment events, int max, int timeoutMs) {
        try {
            int r = (int) EPOLL_WAIT.invokeExact(cap, ep, events, max, timeoutMs);
            return r >= 0 ? r : -errno();
        } catch (Throwable t) {
            throw bug(t);
        }
    }

    static int eventsAt(MemorySegment events, int i) {
        return events.get(JAVA_INT, (long) i * EV_SIZE);
    }

    static int fdAt(MemorySegment events, int i) {
        return (int) events.get(JAVA_LONG_UNALIGNED, (long) i * EV_SIZE + EV_DATA);
    }

    int socket() {
        try {
            int r = (int) SOCKET.invokeExact(cap, 2, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
            return r >= 0 ? r : -errno();
        } catch (Throwable t) {
            throw bug(t);
        }
    }

    int setIntOpt(int fd, int level, int opt, int value) {
        tmp.set(JAVA_INT, 0, value);
        try {
            int r = (int) SETSOCKOPT.invokeExact(cap, fd, level, opt, tmp, 4);
            return r >= 0 ? r : -errno();
        } catch (Throwable t) {
            throw bug(t);
        }
    }

    /** SO_ERROR of a socket, or -errno if the call itself failed. */
    int soError(int fd) {
        tmp.set(JAVA_INT, 0, 0);
        tmp.set(JAVA_INT, 8, 4);
        try {
            int r = (int) GETSOCKOPT.invokeExact(cap, fd, SOL_SOCKET, SO_ERROR, tmp, tmp.asSlice(8));
            return r >= 0 ? tmp.get(JAVA_INT, 0) : -errno();
        } catch (Throwable t) {
            throw bug(t);
        }
    }

    private MemorySegment sockaddr(byte[] ip, int port) {
        sa.fill((byte) 0);
        sa.set(JAVA_SHORT, 0, (short) 2);
        sa.set(JAVA_BYTE, 2, (byte) (port >> 8));
        sa.set(JAVA_BYTE, 3, (byte) port);
        for (int i = 0; i < 4; i++) sa.set(JAVA_BYTE, 4 + i, ip[i]);
        return sa;
    }

    /** A non-blocking listening socket with SO_REUSEPORT, TCP_NODELAY and TCP_DEFER_ACCEPT; fd or -errno. */
    int listenTcp(byte[] ip, int port, int backlog) {
        int fd = socket();
        if (fd < 0) return fd;
        setIntOpt(fd, SOL_SOCKET, SO_REUSEADDR, 1);
        setIntOpt(fd, SOL_SOCKET, SO_REUSEPORT, 1);
        setIntOpt(fd, IPPROTO_TCP, TCP_NODELAY, 1);
        setIntOpt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, 5);
        try {
            int r = (int) BIND.invokeExact(cap, fd, sockaddr(ip, port), 16);
            if (r < 0) {
                int e = errno();
                close(fd);
                return -e;
            }
            r = (int) LISTEN.invokeExact(cap, fd, backlog);
            if (r < 0) {
                int e = errno();
                close(fd);
                return -e;
            }
            return fd;
        } catch (Throwable t) {
            throw bug(t);
        }
    }

    /** accept4 with SOCK_NONBLOCK | SOCK_CLOEXEC; fd or -errno. */
    int accept(int lfd) {
        try {
            int r = (int) ACCEPT4.invokeExact(cap, lfd, MemorySegment.NULL, MemorySegment.NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);
            return r >= 0 ? r : -errno();
        } catch (Throwable t) {
            throw bug(t);
        }
    }

    /** Starts a non-blocking connect: 0 or -EINPROGRESS mean it is under way. */
    int connect(int fd, byte[] ip, int port) {
        try {
            int r = (int) CONNECT.invokeExact(cap, fd, sockaddr(ip, port), 16);
            return r >= 0 ? 0 : -errno();
        } catch (Throwable t) {
            throw bug(t);
        }
    }

    long read(int fd, MemorySegment buf, long n) {
        try {
            long r = (long) READ.invokeExact(cap, fd, buf, n);
            return r >= 0 ? r : -errno();
        } catch (Throwable t) {
            throw bug(t);
        }
    }

    long send(int fd, MemorySegment buf, long n) {
        try {
            long r = (long) SEND.invokeExact(cap, fd, buf, n, MSG_NOSIGNAL);
            return r >= 0 ? r : -errno();
        } catch (Throwable t) {
            throw bug(t);
        }
    }

    void close(int fd) {
        try {
            int r = (int) CLOSE.invokeExact(fd);
        } catch (Throwable t) {
            throw bug(t);
        }
    }
}
