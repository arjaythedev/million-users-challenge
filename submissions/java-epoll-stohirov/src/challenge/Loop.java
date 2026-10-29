package challenge;

import static java.lang.foreign.ValueLayout.JAVA_BYTE;

import java.io.IOException;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.concurrent.locks.LockSupport;

final class Loop implements Runnable, Pg.Handler, Uring.Handler {
    record Config(int readers, int gzipLevel, int gzipMin, boolean uring, int batchMin, long batchNanos) {}

    private static final int READ_CAP = 64 * 1024;
    private static final int MAX_HEAD = 16 * 1024;
    private static final int MAX_BODY = 32 * 1024;
    private static final int OUT_CAP = 1 << 20;
    private static final int OUT_RESERVE = 128 * 1024;
    private static final int PG_IO = 256 * 1024;
    private static final int MAX_EVENTS = 4096, ACCEPT_BURST = 256, MAX_BATCH = 256;
    private static final int RING_ENTRIES = 4096, RECV_BUFFERS = 4096, RECV_BUFFER_SIZE = 2048;
    private static final long SEND_ARENA = 8 << 20;
    private static final long RECONNECT_NANOS = 1_000_000_000L;
    private static final int TAG_CONN = 0, TAG_LISTEN = 1, TAG_PG = 2;

    private static final int NOT_FOUND = 0, HEALTH = 1, FEED = 2, GET_POST = 3, CREATE = 4, LIKE = 5;

    private static final byte[] E_NOT_FOUND = err("not found"), E_INVALID_ID = err("invalid post id"),
            E_NO_POST = err("post not found"), E_MISSING_TOKEN = err("missing bearer token"),
            E_INVALID_TOKEN = err("invalid or expired token"), E_BAD_PAYLOAD = err("invalid token payload"),
            E_MALFORMED = err("malformed JSON body"), E_BODY_REQUIRED = err("body is required"),
            E_BODY_TOO_LONG = err("body must be at most 500 characters"), E_INTERNAL = err("internal server error"),
            E_BAD_REQUEST = err("bad request"), E_TOO_LARGE = err("request too large"),
            E_NOT_IMPLEMENTED = err("not implemented");
    private static final byte[] LIKED_NEW = Out.ascii("{\"liked\":true,\"already_liked\":false,\"post_id\":");
    private static final byte[] LIKED_AGAIN = Out.ascii("{\"liked\":true,\"already_liked\":true,\"post_id\":");
    private static final byte[] HEALTH_OK = Out.ascii("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
    private static final byte[] FEED_OPEN = Out.ascii("{\"posts\":["), FEED_CLOSE = Out.ascii("]}");
    private static final byte[] POST_OPEN = Out.ascii("{\"post\":"), POST_CLOSE = Out.ascii("}");
    private static final byte[] P_ID = Out.ascii("{\"id\":"), P_BODY = Out.ascii(",\"body\":\""),
            P_CREATED = Out.ascii("\",\"created_at\":\""), P_AUTHOR = Out.ascii("\",\"author\":\""),
            P_LIKES = Out.ascii("\",\"like_count\":");
    private static final byte[] CLOSE_BRACE = {'}'};

    private static byte[] err(String message) { return Out.ascii("{\"error\":\"" + message + "\"}"); }

    private final int id;
    private final Sys sys = new Sys();
    private final Jwt jwt;
    private final long startNanos;
    private final Config cfg;
    private final int ep;
    private final int[] listenFds;
    private Uring ring;
    private boolean nodelayChecked, nodelayInherited;
    private static volatile boolean nodelayLogged;
    private final Pg[] pgs;
    private final int readers;
    private final Pg writer;

    private final MemorySegment events = Arena.global().allocate((long) MAX_EVENTS * Sys.EVENT_SIZE, 8);
    private final MemorySegment in = Arena.global().allocate(READ_CAP);
    private final MemorySegment wbuf = Arena.global().allocate(OUT_CAP);
    private final MemorySegment pgIo = Arena.global().allocate(PG_IO);
    private final byte[] rb = new byte[READ_CAP + MAX_HEAD + MAX_BODY];
    private final Out out = new Out(OUT_CAP);
    private final Buf scratch = new Buf(4096);
    private final Gzip gzip;
    private final int gzipMin;
    private Conn[] byFd = new Conn[1024];

    private final ArrayDeque<Conn> writes = new ArrayDeque<>(), solo = new ArrayDeque<>();
    private Conn[] batch = new Conn[MAX_BATCH];
    private final ArrayList<Pg> dirty = new ArrayList<>();
    private long lastAcceptError, lastConnectFailure;
    private boolean connectFailed;

    Loop(int id, Pg.Config db, Config cfg, Jwt jwt, InetAddress host, int[] ports, long startNanos) {
        this.id = id;
        this.cfg = cfg;
        this.gzip = cfg.gzipLevel() > 0 ? new Gzip(2 * OUT_RESERVE) : null;
        this.gzipMin = cfg.gzipMin();
        this.jwt = jwt;
        this.startNanos = startNanos;
        this.readers = cfg.readers();
        pgs = new Pg[readers + 1];
        for (int i = 0; i < pgs.length; i++) pgs[i] = new Pg(i, db, sys);
        writer = pgs[readers];
        ep = sys.epollCreate();
        listenFds = new int[ports.length];
        for (int i = 0; i < ports.length; i++) listenFds[i] = sys.listen(host, ports[i], 65535);
    }

    String backend() { return ring != null ? "io_uring" : "epoll"; }

    @Override
    public void run() {
        if (cfg.uring()) ring = Uring.open(sys, RING_ENTRIES, RECV_BUFFERS, RECV_BUFFER_SIZE, SEND_ARENA);
        if (id == 0) System.err.println("loop 0: " + backend());
        for (int fd : listenFds) {
            if (ring != null) ring.acceptMultishot(fd, Uring.userData(Uring.ACCEPT, 0, fd));
            else sys.epollCtl(ep, Sys.EPOLL_CTL_ADD, fd, Sys.EPOLLIN, TAG_LISTEN);
        }
        for (Pg pg : pgs) reconnect(pg);
        if (ring != null) runUring();
        else runEpoll();
    }


    private void runEpoll() {
        while (true) {
            int n = sys.epollWait(ep, events, MAX_EVENTS, 1000);
            if (n < 0) {
                if (sys.errno() != Sys.EINTR) throw new IllegalStateException("epoll_wait: errno " + sys.errno());
                n = 0;
            }
            for (int i = 0; i < n; i++) {
                int fd = Sys.eventFd(events, i), tag = Sys.eventTag(events, i), flags = Sys.eventFlags(events, i);
                if (tag == TAG_CONN) onConn(fd, flags);
                else if (tag == TAG_LISTEN) accept(fd);
                else onPg(pgs[tag - TAG_PG], fd, flags);
            }
            pumpWrites();
            flushPgs();
            if (cfg.batchNanos() > 0 && n > 0 && n < cfg.batchMin()) LockSupport.parkNanos(cfg.batchNanos());
        }
    }

    private void runUring() {
        boolean small = false;
        while (true) {
            if (small) ring.submit(cfg.batchMin(), cfg.batchNanos());
            else ring.submit(1, 1_000_000_000L);
            int n = ring.reap(this);
            pumpWrites();
            flushPgs();
            small = cfg.batchNanos() > 0 && n > 0 && n < cfg.batchMin();
        }
    }

    @Override
    public void completion(long ud, int res, int flags) {
        int fd = Uring.fd(ud);
        try {
            dispatch(ud, fd, res, flags);
        } catch (Throwable t) {
            System.err.println("loop " + id + ": completion " + Uring.kind(ud) + " failed: " + t);
            if (Uring.kind(ud) == Uring.PG_RECV || Uring.kind(ud) == Uring.PG_POLLOUT) {
                Pg pg = pgs[Uring.tag(ud)];
                if (pg.fd == fd) {
                    pg.lastError = t.toString();
                    fail(pg);
                }
            } else {
                Conn c = fd < byFd.length ? byFd[fd] : null;
                if (c != null) close(c);
            }
        }
    }

    private void dispatch(long ud, int fd, int res, int flags) {
        switch (Uring.kind(ud)) {
            case Uring.ACCEPT -> {
                if (res >= 0) accepted(res);
                else acceptError(-res);
                if ((flags & Uring.CQE_F_MORE) == 0) ring.acceptMultishot(fd, ud);
            }
            case Uring.RECV -> received(fd, res, flags);
            case Uring.SEND -> sent(fd, res);
            case Uring.POLLOUT -> {
                Conn c = fd < byFd.length ? byFd[fd] : null;
                if (c == null) return;
                c.pollArmed = false;
                if (c.closed) release(c);
                else if (c.pending != null) onWritable(c);
            }
            case Uring.PG_RECV -> pgReceived(pgs[Uring.tag(ud)], fd, res, flags);
            case Uring.PG_POLLOUT -> {
                Pg pg = pgs[Uring.tag(ud)];
                if (pg.fd != fd) return;
                pg.wantWrite = false;
                flushPg(pg);
            }
            default -> { }
        }
    }


    private void accept(int listenFd) {
        for (int k = 0; k < ACCEPT_BURST; k++) {
            int fd = sys.accept(listenFd);
            if (fd < 0) {
                int e = sys.errno();
                if (e == Sys.EINTR || e == Sys.ECONNABORTED) continue;
                if (e != Sys.EAGAIN) acceptError(e);
                return;
            }
            accepted(fd);
        }
    }

    private void acceptError(int errno) {
        if (errno == Sys.ECONNABORTED || System.nanoTime() - lastAcceptError < 1_000_000_000L) return;
        lastAcceptError = System.nanoTime();
        System.err.println("loop " + id + ": accept: errno " + errno);
    }

    private void accepted(int fd) {
        if (!nodelayChecked) {
            nodelayChecked = true;
            nodelayInherited = sys.getInt(fd, Sys.IPPROTO_TCP, Sys.TCP_NODELAY) > 0;
            if (!nodelayLogged) {
                nodelayLogged = true;
                System.err.println("TCP_NODELAY " + (nodelayInherited ? "inherited from the listening socket" : "set per socket"));
            }
        }
        if (!nodelayInherited) sys.setInt(fd, Sys.IPPROTO_TCP, Sys.TCP_NODELAY, 1);
        if (ring == null && sys.epollCtl(ep, Sys.EPOLL_CTL_ADD, fd, Sys.EPOLLIN, TAG_CONN) != 0) {
            Sys.close(fd);
            return;
        }
        if (fd >= byFd.length) byFd = Arrays.copyOf(byFd, Math.max(fd + 1, byFd.length * 2));
        Conn c = new Conn(fd);
        byFd[fd] = c;
        if (ring != null) {
            ring.recvMultishot(fd, Uring.userData(Uring.RECV, 0, fd));
            c.recvArmed = true;
        }
    }


    private void received(int fd, int res, int flags) {
        Conn c = fd < byFd.length ? byFd[fd] : null;
        if (c == null) {
            ring.recycle(flags);
            return;
        }
        try {
            if (res > 0 && !c.closed) onData(c, ring.buffer(flags, res), res);
        } catch (Throwable t) {
            System.err.println("loop " + id + ": connection error: " + t);
            close(c);
        } finally {
            ring.recycle(flags);
        }
        if ((flags & Uring.CQE_F_MORE) != 0) return;
        c.recvArmed = false;
        if (!c.closed && (res > 0 || res == -Sys.ENOBUFS)) {
            ring.recvMultishot(fd, Uring.userData(Uring.RECV, 0, fd));
            c.recvArmed = true;
        } else {
            close(c);
        }
    }

    private void sent(int fd, int res) {
        ring.sendsInFlight--;
        Conn c = fd < byFd.length ? byFd[fd] : null;
        if (c == null) return;
        c.sending = false;
        if (c.closed) {
            release(c);
            return;
        }
        if (res == c.sendLen) {
            if (c.pending != null) onWritable(c);
            else if (c.closeAfterWrite && !c.waiting) close(c);
            else resume(c);
            return;
        }
        if (res < 0 && res != -Sys.EAGAIN) {
            close(c);
            return;
        }
        int done = Math.max(res, 0), rest = c.sendLen - done;
        int behind = c.pending == null ? 0 : c.pending.length - c.pendingFrom;
        byte[] p = new byte[rest + behind];
        MemorySegment.copy(ring.arena, JAVA_BYTE, c.sendAt + done, p, 0, rest);
        if (behind > 0) System.arraycopy(c.pending, c.pendingFrom, p, rest, behind);
        c.pending = p;
        c.pendingFrom = 0;
        armWritable(c);
    }

    private void armWritable(Conn c) {
        if (ring == null) {
            sys.epollCtl(ep, Sys.EPOLL_CTL_MOD, c.fd, Sys.EPOLLOUT, TAG_CONN);
        } else if (!c.pollArmed) {
            ring.pollOut(c.fd, Uring.userData(Uring.POLLOUT, 0, c.fd));
            c.pollArmed = true;
        }
    }

    private void onConn(int fd, int flags) {
        Conn c = fd < byFd.length ? byFd[fd] : null;
        if (c == null) return;
        try {
            if (c.pending != null) {
                if ((flags & (Sys.EPOLLOUT | Sys.EPOLLERR | Sys.EPOLLHUP)) != 0) onWritable(c);
            } else if ((flags & (Sys.EPOLLIN | Sys.EPOLLERR | Sys.EPOLLHUP)) != 0) {
                onReadable(c);
            }
        } catch (Throwable t) {
            System.err.println("loop " + id + ": connection error: " + t);
            close(c);
        }
    }

    private void close(Conn c) {
        if (!c.closed) {
            c.closed = true;
            c.left = null;
            c.pending = null;
            if (ring == null) {
                byFd[c.fd] = null;
                Sys.close(c.fd);
                return;
            }
            if (c.recvArmed) ring.cancel(Uring.userData(Uring.RECV, 0, c.fd));
            if (c.pollArmed) ring.cancel(Uring.userData(Uring.POLLOUT, 0, c.fd));
        }
        release(c);
    }

    private void release(Conn c) {
        if (ring == null || !c.closed || c.released || c.recvArmed || c.sending || c.pollArmed) return;
        c.released = true;
        byFd[c.fd] = null;
        Sys.close(c.fd);
    }

    private boolean retryable(int errno) { return errno == Sys.EAGAIN || errno == Sys.EINTR; }

    private void onReadable(Conn c) {
        int have = c.left == null ? 0 : c.left.length;
        int room = Math.min(READ_CAP, rb.length - have);
        if (room <= 0) {
            close(c);
            return;
        }
        long r = sys.read(c.fd, in, room);
        if (r <= 0) {
            if (r == 0 || !retryable(sys.errno())) close(c);
            return;
        }
        onData(c, in, (int) r);
    }

    private void onData(Conn c, MemorySegment data, int n) {
        int have = c.left == null ? 0 : c.left.length;
        if (have + n > rb.length) {
            close(c);
            return;
        }
        if (have > 0) System.arraycopy(c.left, 0, rb, 0, have);
        MemorySegment.copy(data, JAVA_BYTE, 0, rb, have, n);
        c.left = null;
        if (c.waiting || c.pending != null || c.sending) {
            c.left = Arrays.copyOf(rb, have + n);
            return;
        }
        process(c, have + n);
    }

    private void onWritable(Conn c) {
        byte[] p = c.pending;
        int len = Math.min(p.length - c.pendingFrom, OUT_CAP);
        MemorySegment.copy(p, c.pendingFrom, wbuf, JAVA_BYTE, 0, len);
        long w = sys.write(c.fd, wbuf, len);
        if (w < 0) {
            if (!retryable(sys.errno())) close(c);
            else if (ring != null) armWritable(c);
            return;
        }
        c.pendingFrom += (int) w;
        if (c.pendingFrom < p.length) {
            if (ring != null) armWritable(c);
            return;
        }
        c.pending = null;
        c.pendingFrom = 0;
        if (c.closeAfterWrite && !c.waiting) {
            close(c);
            return;
        }
        if (ring == null) sys.epollCtl(ep, Sys.EPOLL_CTL_MOD, c.fd, Sys.EPOLLIN, TAG_CONN);
        resume(c);
    }

    private void resume(Conn c) {
        if (c.closed || c.waiting || c.pending != null || c.sending || c.left == null) return;
        byte[] l = c.left;
        c.left = null;
        System.arraycopy(l, 0, rb, 0, l.length);
        process(c, l.length);
    }

    private void send(Conn c, int from, int to) {
        if (c.closed || from == to) return;
        int len = to - from;
        if (c.pending != null || c.sending) {
            int rest = c.pending == null ? 0 : c.pending.length - c.pendingFrom;
            byte[] p = new byte[rest + len];
            if (rest > 0) System.arraycopy(c.pending, c.pendingFrom, p, 0, rest);
            MemorySegment.copy(out.seg, JAVA_BYTE, from, p, rest, len);
            c.pending = p;
            c.pendingFrom = 0;
            return;
        }
        if (ring != null) {
            long at = ring.alloc(len);
            if (at >= 0) {
                MemorySegment.copy(out.seg, from, ring.arena, at, len);
                ring.send(c.fd, at, len, Uring.userData(Uring.SEND, 0, c.fd));
                c.sending = true;
                c.sendAt = at;
                c.sendLen = len;
                return;
            }
        }
        long w = sys.write(c.fd, out.seg.asSlice(from, len), len);
        if (w < 0) {
            if (!retryable(sys.errno())) {
                close(c);
                return;
            }
            w = 0;
        }
        if (w < len) {
            c.pending = new byte[len - (int) w];
            MemorySegment.copy(out.seg, JAVA_BYTE, from + w, c.pending, 0, c.pending.length);
            c.pendingFrom = 0;
            armWritable(c);
        } else if (c.closeAfterWrite && !c.waiting) {
            close(c);
        }
    }

    private void flush(Conn c) {
        int n = out.pos;
        out.pos = 0;
        send(c, 0, n);
    }

    private void process(Conn c, int end) {
        byte[] b = rb;
        int p = 0;
        while (p < end && !c.closed) {
            if (out.free() < OUT_RESERVE) {
                flush(c);
                if (c.pending != null || c.sending || c.closed) break;
            }
            int used = request(c, b, p, end);
            if (used == 0) break;
            p += used;
            if (c.closeAfterWrite) p = end;
            if (c.waiting) break;
        }
        c.left = !c.closed && p < end ? Arrays.copyOfRange(b, p, end) : null;
        flush(c);
    }


    private int pathFrom, pathTo, authFrom, authTo, bodyFrom, bodyLen;

    private int request(Conn c, byte[] b, int p, int end) {
        int he = headerEnd(b, p, end);
        if (he < 0) {
            if (end - p > MAX_HEAD) return reject(c, Out.S431, E_TOO_LARGE, end - p);
            return 0;
        }
        int sp1 = indexOf(b, (byte) ' ', p, he);
        int sp2 = sp1 < 0 ? -1 : indexOf(b, (byte) ' ', sp1 + 1, he);
        int eol = sp2 < 0 ? -1 : indexOf(b, (byte) '\r', sp2 + 1, he);
        if (eol < 0 || eol - sp2 - 1 != 8 || !startsWith(b, sp2 + 1, "HTTP/1.")) {
            return reject(c, Out.S400, E_BAD_REQUEST, end - p);
        }
        boolean http11 = b[sp2 + 8] == '1';
        boolean isGet = sp1 - p == 3 && b[p] == 'G' && b[p + 1] == 'E' && b[p + 2] == 'T';
        boolean isPost = sp1 - p == 4 && b[p] == 'P' && b[p + 1] == 'O' && b[p + 2] == 'S' && b[p + 3] == 'T';
        pathFrom = sp1 + 1;
        pathTo = sp2;
        int q = indexOf(b, (byte) '?', pathFrom, pathTo);
        if (q >= 0) pathTo = q;

        authFrom = authTo = -1;
        long contentLength = 0;
        boolean connClose = false, connKeepAlive = false, hasLength = false, acceptGzip = false;
        int teFrom = -1, teTo = -1;
        int line = eol + 2;
        while (line < he - 2) {
            int le = indexOf(b, (byte) '\r', line, he);
            int colon = indexOf(b, (byte) ':', line, le);
            if (colon > line) {
                int vs = colon + 1, ve = le;
                while (vs < ve && (b[vs] == ' ' || b[vs] == '\t')) vs++;
                while (ve > vs && (b[ve - 1] == ' ' || b[ve - 1] == '\t')) ve--;
                int nl = colon - line;
                if (nl == 13 && nameIs(b, line, "authorization")) {
                    authFrom = vs;
                    authTo = ve;
                } else if (nl == 14 && nameIs(b, line, "content-length")) {
                    contentLength = 0;
                    hasLength = true;
                    if (vs == ve) return reject(c, Out.S400, E_BAD_REQUEST, end - p);
                    for (int k = vs; k < ve; k++) {
                        if (b[k] < '0' || b[k] > '9' || contentLength > MAX_BODY) {
                            return reject(c, b[k] < '0' || b[k] > '9' ? Out.S400 : Out.S413,
                                    b[k] < '0' || b[k] > '9' ? E_BAD_REQUEST : E_TOO_LARGE, end - p);
                        }
                        contentLength = contentLength * 10 + (b[k] - '0');
                    }
                } else if (nl == 10 && nameIs(b, line, "connection")) {
                    if (containsIgnoreCase(b, vs, ve, "close")) connClose = true;
                    if (containsIgnoreCase(b, vs, ve, "keep-alive")) connKeepAlive = true;
                } else if (nl == 15 && nameIs(b, line, "accept-encoding")) {
                    acceptGzip = containsIgnoreCase(b, vs, ve, "gzip");
                } else if (nl == 17 && nameIs(b, line, "transfer-encoding")) {
                    if (teFrom >= 0) return reject(c, Out.S400, E_BAD_REQUEST, end - p);
                    teFrom = vs;
                    teTo = ve;
                }
            }
            line = le + 2;
        }
        int msgEnd;
        boolean chunked = teFrom >= 0;
        if (chunked) {
            if (!http11 || hasLength) return reject(c, Out.S400, E_BAD_REQUEST, end - p);
            if (teTo - teFrom != 7 || !nameIs(b, teFrom, "chunked")) {
                boolean last = teTo - teFrom > 7 && nameIs(b, teTo - 7, "chunked");
                return reject(c, last ? Out.S501 : Out.S400, last ? E_NOT_IMPLEMENTED : E_BAD_REQUEST, end - p);
            }
            msgEnd = chunked(b, he, end, false);
            if (msgEnd == MALFORMED) return reject(c, Out.S400, E_BAD_REQUEST, end - p);
            if (msgEnd == TOO_LARGE) return reject(c, Out.S413, E_TOO_LARGE, end - p);
            if (msgEnd == INCOMPLETE) return 0;
            bodyLen = chunkedLen;
            chunked(b, he, msgEnd, true);
        } else {
            if (contentLength > MAX_BODY) return reject(c, Out.S413, E_TOO_LARGE, end - p);
            msgEnd = he + (int) contentLength;
            if (end < msgEnd) return 0;
            bodyLen = (int) contentLength;
        }
        bodyFrom = he;

        c.closeAfterWrite = http11 ? connClose : !connKeepAlive;
        c.gzip = acceptGzip;
        out.close = c.closeAfterWrite;
        int mark = out.pos;
        try {
            handle(c, route(b, isGet, isPost), b);
        } catch (Throwable t) {
            System.err.println("loop " + id + ": request failed: " + t);
            out.pos = mark;
            out.respond(Out.S500, E_INTERNAL);
        }
        return msgEnd - p;
    }

    private static final int INCOMPLETE = 0, MALFORMED = -1, TOO_LARGE = -2;
    private int chunkedLen;

    private int chunked(byte[] b, int from, int end, boolean compact) {
        int k = from, len = 0;
        while (true) {
            int size = 0, start = k, d;
            while (k < end && (d = Character.digit(b[k], 16)) >= 0) {
                size = size * 16 + d;
                if (len + size > MAX_BODY) return TOO_LARGE;
                k++;
            }
            if (k == end) return INCOMPLETE;
            if (k == start || b[k] != '\r' && b[k] != ';' && b[k] != ' ' && b[k] != '\t') return MALFORMED;
            int eol = indexOf(b, (byte) '\r', k, end);
            if (eol < 0 || eol + 1 == end) return INCOMPLETE;
            if (b[eol + 1] != '\n') return MALFORMED;
            k = eol + 2;
            if (size == 0) break;
            if (end - k < size + 2) return INCOMPLETE;
            if (b[k + size] != '\r' || b[k + size + 1] != '\n') return MALFORMED;
            if (compact) System.arraycopy(b, k, b, from + len, size);
            len += size;
            k += size + 2;
        }
        while (true) {
            int eol = indexOf(b, (byte) '\r', k, end);
            if (eol < 0 || eol + 1 == end) return INCOMPLETE;
            if (b[eol + 1] != '\n') return MALFORMED;
            if (eol == k) {
                chunkedLen = len;
                return eol + 2;
            }
            k = eol + 2;
        }
    }

    private int reject(Conn c, byte[] status, byte[] body, int consumed) {
        c.closeAfterWrite = true;
        out.close = true;
        out.respond(status, body);
        return Math.max(consumed, 1);
    }

    private int route(byte[] b, boolean isGet, boolean isPost) {
        int len = pathTo - pathFrom;
        if (isGet) {
            if (len == 7 && startsWith(b, pathFrom, "/health")) return HEALTH;
            if (len == 5 && startsWith(b, pathFrom, "/feed")) return FEED;
            if (len > 7 && startsWith(b, pathFrom, "/posts/") && indexOf(b, (byte) '/', pathFrom + 7, pathTo) < 0) {
                return GET_POST;
            }
            return NOT_FOUND;
        }
        if (isPost) {
            if (len == 6 && startsWith(b, pathFrom, "/posts")) return CREATE;
            if (len > 12 && startsWith(b, pathFrom, "/posts/") && endsWith(b, pathTo, "/like")
                    && indexOf(b, (byte) '/', pathFrom + 7, pathTo - 5) < 0) {
                return LIKE;
            }
        }
        return NOT_FOUND;
    }

    private void handle(Conn c, int route, byte[] b) {
        switch (route) {
            case HEALTH -> read(c, Pg.PING);
            case FEED -> read(c, Pg.FEED);
            case GET_POST -> {
                long id = parseId(b, pathFrom + 7, pathTo);
                if (id == -1) out.respond(Out.S400, E_INVALID_ID);
                else if (id == -2) out.respond(Out.S404, E_NO_POST);
                else {
                    c.id = id;
                    read(c, Pg.POST);
                }
            }
            case CREATE -> create(c, b);
            case LIKE -> like(c, b);
            default -> out.respond(Out.S404, E_NOT_FOUND);
        }
    }

    private boolean auth(byte[] b) {
        int r = jwt.verify(b, authFrom, authTo);
        if (r == Jwt.OK) return true;
        out.respond(Out.S401, r == Jwt.MISSING ? E_MISSING_TOKEN : r == Jwt.INVALID ? E_INVALID_TOKEN : E_BAD_PAYLOAD);
        return false;
    }

    private void create(Conn c, byte[] b) {
        if (!auth(b)) return;
        Object doc;
        try {
            doc = Json.parse(Json.utf8(b, bodyFrom, bodyLen));
        } catch (Json.Malformed e) {
            out.respond(Out.S400, E_MALFORMED);
            return;
        }
        if (!(Json.get(doc, "body") instanceof String raw)) {
            out.respond(Out.S400, E_BODY_REQUIRED);
            return;
        }
        String body = trim(raw);
        if (body.isEmpty()) {
            out.respond(Out.S400, E_BODY_REQUIRED);
            return;
        }
        if (body.codePointCount(0, body.length()) > 500) {
            out.respond(Out.S400, E_BODY_TOO_LONG);
            return;
        }
        c.userId = jwt.userId;
        c.user = jwt.username;
        c.text = body.getBytes(StandardCharsets.UTF_8);
        write(c, Pg.CREATE);
    }

    private void like(Conn c, byte[] b) {
        if (!auth(b)) return;
        long id = parseId(b, pathFrom + 7, pathTo - 5);
        if (id == -1) {
            out.respond(Out.S400, E_INVALID_ID);
            return;
        }
        if (id == -2) {
            out.respond(Out.S404, E_NO_POST);
            return;
        }
        c.userId = jwt.userId;
        c.id = id;
        write(c, Pg.LIKE);
    }


    private void read(Conn c, byte op) {
        Pg pg = pickReader();
        if (pg == null) {
            if (op == Pg.PING) degraded(pgs[0].lastError);
            else out.respond(Out.S500, E_INTERNAL);
            return;
        }
        c.op = op;
        c.rows = 0;
        c.waiting = true;
        pg.query(c);
        markDirty(pg);
    }

    private void write(Conn c, byte op) {
        c.op = op;
        c.rows = 0;
        c.waiting = true;
        writes.add(c);
    }

    private Pg pickReader() {
        Pg best = null;
        for (int i = 0; i < readers; i++) {
            Pg pg = pgs[i];
            if (pg.alive() && (best == null || pg.inFlight() < best.inFlight())) best = pg;
        }
        if (best != null) return best;
        for (int i = 0; i < readers; i++) if (reconnect(pgs[i])) return pgs[i];
        return null;
    }

    private void pumpWrites() {
        if (writer.inFlight() > 0 || writes.isEmpty() && solo.isEmpty()) return;
        if (!reconnect(writer)) {
            ArrayList<Conn> failed = new ArrayList<>(solo);
            failed.addAll(writes);
            solo.clear();
            writes.clear();
            for (Conn c : failed) finish(writer, c, "08006", writer.lastError);
            return;
        }
        if (!solo.isEmpty()) {
            while (!solo.isEmpty()) {
                Conn c = solo.poll();
                if (!c.closed) writer.query(c);
            }
        } else {
            int n = 0;
            while (n < MAX_BATCH && !writes.isEmpty()) {
                Conn c = writes.poll();
                if (!c.closed) batch[n++] = c;
            }
            if (n > 0) writer.batch(batch, n);
            Arrays.fill(batch, 0, n, null);
        }
        markDirty(writer);
    }

    private void markDirty(Pg pg) {
        if (pg.dirty) return;
        pg.dirty = true;
        dirty.add(pg);
    }

    private void flushPgs() {
        for (int i = 0; i < dirty.size(); i++) {
            Pg pg = dirty.get(i);
            pg.dirty = false;
            if (pg.alive()) flushPg(pg);
        }
        dirty.clear();
    }

    private void flushPg(Pg pg) {
        if (!pg.flush(pgIo)) {
            fail(pg);
            return;
        }
        boolean want = !pg.flushed();
        if (ring != null) {
            if (want && !pg.wantWrite) {
                pg.wantWrite = true;
                ring.pollOut(pg.fd, Uring.userData(Uring.PG_POLLOUT, pg.tag, pg.fd));
            }
        } else if (want != pg.wantWrite) {
            pg.wantWrite = want;
            sys.epollCtl(ep, Sys.EPOLL_CTL_MOD, pg.fd, Sys.EPOLLIN | (want ? Sys.EPOLLOUT : 0), TAG_PG + pg.tag);
        }
    }

    private void pgReceived(Pg pg, int fd, int res, int flags) {
        if (pg.fd != fd) {
            ring.recycle(flags);
            if ((flags & Uring.CQE_F_MORE) == 0) Sys.close(fd);
            return;
        }
        if (res > 0) {
            try {
                pg.received(ring.buffer(flags, res), res);
            } finally {
                ring.recycle(flags);
            }
            pg.parse(this);
            if (pg.fd != fd) return;
        }
        if ((flags & Uring.CQE_F_MORE) != 0) return;
        pg.recvArmed = false;
        if (res > 0 || res == -Sys.ENOBUFS) {
            ring.recvMultishot(fd, Uring.userData(Uring.PG_RECV, pg.tag, fd));
            pg.recvArmed = true;
        } else {
            pg.lastError = res == 0 ? "connection closed by the database" : "recv: errno " + -res;
            fail(pg);
        }
    }

    private void onPg(Pg pg, int fd, int flags) {
        if (pg.fd != fd) return;
        try {
            if ((flags & Sys.EPOLLOUT) != 0) flushPg(pg);
            if (!pg.alive() || (flags & (Sys.EPOLLIN | Sys.EPOLLERR | Sys.EPOLLHUP)) == 0) return;
            if (!pg.read(pgIo)) {
                fail(pg);
                return;
            }
            pg.parse(this);
        } catch (Throwable t) {
            System.err.println("loop " + id + ": postgres connection error: " + t);
            if (pg.alive()) {
                pg.lastError = t.toString();
                fail(pg);
            }
        }
    }

    private boolean reconnect(Pg pg) {
        if (pg.alive()) return true;
        if (connectFailed && System.nanoTime() - lastConnectFailure < RECONNECT_NANOS) return false;
        try {
            pg.connect(pgIo);
        } catch (IOException e) {
            pg.lastError = e.getMessage();
            connectFailed = true;
            lastConnectFailure = System.nanoTime();
            System.err.println("loop " + id + ": postgres: " + e.getMessage());
            return false;
        }
        connectFailed = false;
        if (ring != null) {
            ring.recvMultishot(pg.fd, Uring.userData(Uring.PG_RECV, pg.tag, pg.fd));
            pg.recvArmed = true;
        } else {
            sys.epollCtl(ep, Sys.EPOLL_CTL_ADD, pg.fd, Sys.EPOLLIN, TAG_PG + pg.tag);
        }
        return true;
    }

    private void fail(Pg pg) {
        System.err.println("loop " + id + ": postgres connection lost: " + pg.lastError);
        int fd = pg.fd;
        boolean keep = ring != null && pg.recvArmed;
        if (keep) ring.cancel(Uring.userData(Uring.PG_RECV, pg.tag, fd));
        if (ring != null && pg.wantWrite) ring.cancel(Uring.userData(Uring.PG_POLLOUT, pg.tag, fd));
        Conn[] lost = pg.close(!keep);
        for (Conn c : lost) finish(pg, c, "08006", pg.lastError);
    }

    @Override
    public void row(Pg pg, Conn c, byte[] b, int at) {
        switch (c.op) {
            case Pg.FEED, Pg.POST -> {
                if (c.rows++ > 0) pg.body.put(',');
                post(pg.body, b, at + 2);
            }
            case Pg.CREATE -> {
                c.id = Buf.i64(b, at + 6);
                c.ts = Buf.i64(b, at + 18);
                c.rows = 1;
            }
            default -> { }
        }
    }

    @Override
    public void command(Conn c, byte[] b, int at, int len) {
        if (c.op != Pg.LIKE) return;
        int v = 0, m = 1, k = at + len;
        while (k > at && b[k - 1] >= '0' && b[k - 1] <= '9') {
            v += (b[--k] - '0') * m;
            m *= 10;
        }
        c.rows = v;
    }

    @Override
    public void done(Pg pg, Conn[] unit, int n, String code, String message) {
        if (code != null && n > 1) {
            for (int i = 0; i < n; i++) solo.add(unit[i]);
            return;
        }
        for (int i = 0; i < n; i++) finish(pg, unit[i], code, message);
        pg.body.len = 0;
    }

    private void finish(Pg pg, Conn c, String code, String message) {
        c.waiting = false;
        if (c.closed) {
            c.text = c.user = null;
            return;
        }
        out.pos = 0;
        out.close = c.closeAfterWrite;
        byte op = c.op;
        if (op == Pg.PING) {
            if (code == null) healthy();
            else degraded(message);
        } else if (op == Pg.LIKE && "23503".equals(code)) {
            out.respond(Out.S404, E_NO_POST);
        } else if (code != null || op == Pg.CREATE && c.rows == 0) {
            System.err.println("loop " + id + ": postgres: " + message + " (" + code + ")");
            out.respond(Out.S500, E_INTERNAL);
        } else {
            switch (op) {
                case Pg.FEED -> respond(c, Out.S200, FEED_OPEN, pg.body, FEED_CLOSE);
                case Pg.POST -> {
                    if (c.rows == 0) out.respond(Out.S404, E_NO_POST);
                    else respond(c, Out.S200, POST_OPEN, pg.body, POST_CLOSE);
                }
                case Pg.CREATE -> {
                    Buf s = scratch;
                    s.len = 0;
                    s.put(P_ID);
                    s.num(c.id);
                    s.put(P_BODY);
                    s.escaped(c.text, 0, c.text.length);
                    s.put(P_CREATED);
                    s.timestamp(c.ts);
                    s.put(P_AUTHOR);
                    s.escaped(c.user, 0, c.user.length);
                    s.put(P_LIKES);
                    s.put('0');
                    s.put('}');
                    respond(c, Out.S201, POST_OPEN, s, POST_CLOSE);
                }
                case Pg.LIKE -> {
                    byte[] prefix = c.rows > 0 ? LIKED_NEW : LIKED_AGAIN;
                    out.head(c.rows > 0 ? Out.S201 : Out.S200, prefix.length + Out.digits(c.id) + 1);
                    out.put(prefix);
                    out.putLong(c.id);
                    out.put(CLOSE_BRACE);
                }
                default -> out.respond(Out.S500, E_INTERNAL);
            }
        }
        c.text = c.user = null;
        flush(c);
        resume(c);
    }

    private void respond(Conn c, byte[] status, byte[] open, Buf body, byte[] close) {
        int len = open.length + body.len + close.length;
        if (gzip != null && c.gzip && len >= gzipMin) {
            gzip.begin();
            gzip.add(open, 0, open.length);
            gzip.add(body.a, 0, body.len);
            gzip.add(close, 0, close.length);
            int n = gzip.end();
            out.head(status, n, true);
            out.put(gzip.buf, 0, n);
            return;
        }
        out.head(status, len);
        out.put(open);
        out.put(body.a, 0, body.len);
        out.put(close);
    }

    private static void post(Buf o, byte[] b, int p) {
        long id = Buf.i64(b, p + 4);
        p += 4 + Buf.i32(b, p);
        int bodyLen = Buf.i32(b, p), bodyAt = p + 4;
        p = bodyAt + bodyLen;
        long ts = Buf.i64(b, p + 4);
        p += 4 + Buf.i32(b, p);
        int userLen = Buf.i32(b, p), userAt = p + 4;
        p = userAt + userLen;
        int likes = Buf.i32(b, p + 4);
        o.put(P_ID);
        o.num(id);
        o.put(P_BODY);
        o.escaped(b, bodyAt, bodyLen);
        o.put(P_CREATED);
        o.timestamp(ts);
        o.put(P_AUTHOR);
        o.escaped(b, userAt, userLen);
        o.put(P_LIKES);
        o.num(likes);
        o.put('}');
    }

    private void healthy() {
        long up = (System.nanoTime() - startNanos) / 1_000_000_000L;
        out.head(Out.S200, HEALTH_OK.length + Out.digits(up) + 1);
        out.put(HEALTH_OK);
        out.putLong(up);
        out.put(CLOSE_BRACE);
    }

    private void degraded(String error) {
        byte[] body = ("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":" + quote(String.valueOf(error)) + "}")
                .getBytes(StandardCharsets.UTF_8);
        out.respond(Out.S503, body);
    }


    private static long parseId(byte[] b, int from, int to) {
        if (from >= to) return -1;
        long v = 0;
        boolean overflow = false;
        for (int k = from; k < to; k++) {
            int d = b[k] - '0';
            if (d < 0 || d > 9) return -1;
            if (!overflow) {
                if (v > (Long.MAX_VALUE - d) / 10) overflow = true;
                else v = v * 10 + d;
            }
        }
        if (overflow) return -2;
        return v > 0 ? v : -1;
    }

    private static boolean isJsSpace(char c) {
        return c == ' ' || (c >= '\t' && c <= '\r') || c == ' ' || c == ' '
                || (c >= ' ' && c <= ' ') || c == ' ' || c == ' ' || c == ' '
                || c == ' ' || c == '　' || c == '﻿';
    }

    private static String trim(String s) {
        int a = 0, z = s.length();
        while (a < z && isJsSpace(s.charAt(a))) a++;
        while (z > a && isJsSpace(s.charAt(z - 1))) z--;
        return s.substring(a, z);
    }

    private static String quote(String s) {
        StringBuilder sb = new StringBuilder(s.length() + 2).append('"');
        for (int k = 0; k < s.length(); k++) {
            char c = s.charAt(k);
            if (c == '"' || c == '\\') sb.append('\\').append(c);
            else if (c < 0x20) sb.append(String.format("\\u%04x", (int) c));
            else sb.append(c);
        }
        return sb.append('"').toString();
    }

    private static int headerEnd(byte[] b, int p, int end) {
        for (int k = p + 3; k < end; k++) {
            if (b[k] == '\n' && b[k - 1] == '\r' && b[k - 2] == '\n' && b[k - 3] == '\r') return k + 1;
        }
        return -1;
    }

    private static int indexOf(byte[] b, byte x, int from, int to) {
        for (int k = from; k < to; k++) if (b[k] == x) return k;
        return -1;
    }

    private static boolean startsWith(byte[] b, int at, String s) {
        for (int k = 0; k < s.length(); k++) if (b[at + k] != s.charAt(k)) return false;
        return true;
    }

    private static boolean endsWith(byte[] b, int to, String s) {
        return startsWith(b, to - s.length(), s);
    }

    private static boolean nameIs(byte[] b, int at, String lower) {
        for (int k = 0; k < lower.length(); k++) if ((b[at + k] | 0x20) != lower.charAt(k)) return false;
        return true;
    }

    private static boolean containsIgnoreCase(byte[] b, int from, int to, String lower) {
        int n = lower.length();
        for (int k = from; k + n <= to; k++) if (nameIs(b, k, lower)) return true;
        return false;
    }
}
