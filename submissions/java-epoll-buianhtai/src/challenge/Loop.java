package challenge;

import static java.lang.foreign.ValueLayout.JAVA_BYTE;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.charset.StandardCharsets;
import java.util.ArrayDeque;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Map;
import java.util.concurrent.locks.LockSupport;
import java.util.zip.CRC32;
import java.util.zip.Deflater;

/**
 * One event loop: its own epoll set, its own listeners on every port, its own clients and Postgres connections.
 * Nothing is shared with other loops except the fd table, whose slots are only touched by the loop that owns the fd.
 * A client connection owns no buffer while idle; reads and writes go through per-loop buffers.
 */
final class Loop implements Runnable {
    /** One slot per possible fd: the service's LimitNOFILE is 8,388,608 (1 << 23). */
    static final Object[] TABLE = new Object[1 << 23];
    static final long STARTED = System.nanoTime();
    private static final Object LISTENER = new Object();
    private static final int MAX_EVENTS = 1024;
    private static final int MAX_HEADER = 16384;
    private static final int MAX_BODY = 1 << 20;

    final Cfg cfg;
    final Sys sys = new Sys();
    int ep;
    final MemorySegment rbuf = Arena.global().allocate(1 << 16, 16);
    final MemorySegment wbuf = Arena.global().allocate(1 << 20, 16);
    private final MemorySegment evs = Arena.global().allocate((long) MAX_EVENTS * Sys.EV_SIZE, 16);
    private final byte[] in = new byte[1 << 16];
    final Out body = new Out(16384);
    private final Out errBody = new Out(256);
    private final Out res = new Out(16384);
    private final Out gz = new Out(8192);
    private final Jwt jwt;
    private final Deflater deflater;
    private final CRC32 crc = new CRC32();
    private Pg[] readers;
    private Pg writer;
    private final ArrayList<Pg> dirty = new ArrayList<>();
    private final ArrayDeque<Req> writeQueue = new ArrayDeque<>();

    Loop(Cfg cfg) {
        this.cfg = cfg;
        this.jwt = new Jwt(cfg.secret);
        this.deflater = new Deflater(Math.max(1, cfg.gzipLevel), true);
    }

    @Override
    public void run() {
        try {
            loop();
        } catch (Throwable t) {
            t.printStackTrace();
            System.exit(1);
        }
    }

    private void loop() {
        ep = sys.epollCreate();
        if (ep < 0) throw new IllegalStateException("epoll_create1: errno " + -ep);
        for (int port : cfg.ports) {
            int lfd = sys.listenTcp(cfg.ip, port, 65535);
            if (lfd < 0) throw new IllegalStateException("listen on port " + port + ": errno " + -lfd);
            TABLE[lfd] = LISTENER;
            sys.epollCtl(ep, Sys.EPOLL_CTL_ADD, lfd, Sys.EPOLLIN);
        }
        readers = new Pg[cfg.readers];
        for (int i = 0; i < readers.length; i++) readers[i] = new Pg(this, false);
        writer = new Pg(this, true);

        while (true) {
            long now = System.nanoTime();
            for (Pg p : readers) if (p.state == Pg.DOWN && now >= p.retryAt) p.connect();
            if (writer.state == Pg.DOWN && now >= writer.retryAt) writer.connect();

            int n = sys.epollWait(ep, evs, MAX_EVENTS, 1000);
            if (n < 0) {
                if (n == -Sys.EINTR) continue;
                throw new IllegalStateException("epoll_wait: errno " + -n);
            }
            for (int i = 0; i < n; i++) {
                int fd = Sys.fdAt(evs, i);
                int ev = Sys.eventsAt(evs, i);
                Object o = TABLE[fd];
                // A closed fd can be reused by another loop before this event batch is exhausted.
                if (o instanceof Conn c && c.owner == this) conn(c, ev);
                else if (o instanceof Pg p && p.lp == this) p.onEvent(ev);
                else if (o == LISTENER) accept(fd);
            }
            dispatchWrites();
            for (int i = 0; i < dirty.size(); i++) dirty.get(i).flushOut();
            dirty.clear();
        }
    }

    void dirty(Pg p) {
        dirty.add(p);
    }

    // ---- clients -------------------------------------------------------------------------------------------

    private void accept(int lfd) {
        for (int i = 0; i < 256; i++) {
            int fd = sys.accept(lfd);
            if (fd == -Sys.EMFILE || fd == -Sys.ENFILE) {
                // Out of fds: the listener stays readable, so pause briefly rather than spin on accept.
                LockSupport.parkNanos(1_000_000L);
                return;
            }
            if (fd < 0) return;
            if (fd >= TABLE.length) {
                sys.close(fd);
                continue;
            }
            TABLE[fd] = new Conn(fd, this);
            sys.epollCtl(ep, Sys.EPOLL_CTL_ADD, fd, Sys.EPOLLIN);
        }
    }

    private void conn(Conn c, int ev) {
        if ((ev & Sys.EPOLLOUT) != 0) flushRest(c);
        if (!c.closed && !c.closeAfter && (ev & (Sys.EPOLLIN | Sys.EPOLLERR | Sys.EPOLLHUP)) != 0) {
            long n = sys.read(c.fd, rbuf, in.length);
            if (n == -Sys.EAGAIN || n == -Sys.EINTR) return;
            if (n <= 0) {
                close(c);
                return;
            }
            MemorySegment.copy(rbuf, JAVA_BYTE, 0, in, 0, (int) n);
            if (c.pend == null && !c.waiting) {
                process(c, in, 0, (int) n);
            } else {
                appendPend(c, in, (int) n);
                if (!c.waiting) resume(c);
            }
        }
    }

    void close(Conn c) {
        if (c.closed) return;
        c.closed = true;
        TABLE[c.fd] = null;
        sys.close(c.fd);
        c.pend = null;
        c.rest = null;
    }

    private void appendPend(Conn c, byte[] b, int n) {
        if (c.pend == null) {
            c.pend = Arrays.copyOf(b, Math.max(n, 256));
            c.pendLen = n;
            return;
        }
        if (c.pendLen + n > MAX_BODY + MAX_HEADER) {
            close(c);
            return;
        }
        if (c.pendLen + n > c.pend.length) c.pend = Arrays.copyOf(c.pend, Math.max(c.pend.length * 2, c.pendLen + n));
        System.arraycopy(b, 0, c.pend, c.pendLen, n);
        c.pendLen += n;
    }

    /** Continues with the bytes that waited behind a request, once that request has been answered. */
    private void resume(Conn c) {
        if (c.pend == null || c.waiting || c.closed || c.closeAfter || c.rest != null) return;
        byte[] p = c.pend;
        int len = c.pendLen;
        c.pend = null;
        c.pendLen = 0;
        process(c, p, 0, len);
    }

    private void process(Conn c, byte[] b, int off, int end) {
        int pos = off;
        while (pos < end && !c.waiting && !c.closed && !c.closeAfter && c.rest == null) {
            int used = one(c, b, pos, end);
            if (used < 0) {
                if (c.rest == null) close(c);
                return;
            }
            if (used == 0) break;
            pos += used;
        }
        if (!c.closed && !c.closeAfter && pos < end) {
            c.pend = Arrays.copyOfRange(b, pos, Math.max(end, pos + 256));
            c.pendLen = end - pos;
        }
    }

    // ---- HTTP -------------------------------------------------------------------------------------------------

    private static boolean ieq(byte[] b, int s, int len, String lower) {
        if (len != lower.length()) return false;
        for (int i = 0; i < len; i++) {
            int ch = b[s + i];
            if (ch >= 'A' && ch <= 'Z') ch += 32;
            if (ch != lower.charAt(i)) return false;
        }
        return true;
    }

    private static boolean contains(byte[] b, int s, int e, String lower) {
        int n = lower.length();
        outer:
        for (int i = s; i + n <= e; i++) {
            for (int j = 0; j < n; j++) {
                int ch = b[i + j];
                if (ch >= 'A' && ch <= 'Z') ch += 32;
                if (ch != lower.charAt(j)) continue outer;
            }
            return true;
        }
        return false;
    }

    private static boolean pathIs(byte[] b, int s, int e, String path) {
        if (e - s != path.length()) return false;
        for (int i = 0; i < path.length(); i++) if (b[s + i] != path.charAt(i)) return false;
        return true;
    }

    private static boolean startsAt(byte[] b, int s, int e, String prefix) {
        if (e - s < prefix.length()) return false;
        for (int i = 0; i < prefix.length(); i++) if (b[s + i] != prefix.charAt(i)) return false;
        return true;
    }

    /** A post id: positive digits. -1 = not a valid id, -2 = a number too large to exist. */
    private static long parseId(byte[] b, int s, int e) {
        if (e <= s) return -1;
        for (int i = s; i < e; i++) if (b[i] < '0' || b[i] > '9') return -1;
        if (e - s > 18) return -2;
        long v = 0;
        for (int i = s; i < e; i++) v = v * 10 + (b[i] - '0');
        return v > 0 ? v : -1;
    }

    /** Handles one request at `off`: bytes consumed, 0 if it is incomplete, -1 to close the connection. */
    private int one(Conn c, byte[] b, int off, int end) {
        int he = -1;
        for (int i = off; i + 3 < end; i++) {
            if (b[i] == '\r' && b[i + 1] == '\n' && b[i + 2] == '\r' && b[i + 3] == '\n') {
                he = i;
                break;
            }
        }
        if (he < 0) {
            if (end - off > MAX_HEADER) {
                replyErr(c, 431, "request headers too large", true);
                return -1;
            }
            return 0;
        }
        int bodyStart = he + 4;

        int sp1 = off;
        while (sp1 < he && b[sp1] != ' ') sp1++;
        int sp2 = sp1 + 1;
        while (sp2 < he && b[sp2] != ' ') sp2++;
        int le = sp2;
        while (le < he && b[le] != '\r') le++;
        if (sp1 >= he || sp2 >= he) {
            replyErr(c, 400, "malformed request", true);
            return -1;
        }
        boolean http10 = le - sp2 == 9 && b[le - 1] == '0';
        int ps = sp1 + 1, pe = ps;
        while (pe < sp2 && b[pe] != '?') pe++;

        int cl = 0, authOff = -1, authLen = 0;
        boolean gzip = false, close = http10, chunked = false, expect = false;
        int p = le + 2;
        while (p < he) {
            int e = p;
            while (e < he && b[e] != '\r') e++;
            int colon = p;
            while (colon < e && b[colon] != ':') colon++;
            if (colon < e) {
                int vs = colon + 1;
                while (vs < e && (b[vs] == ' ' || b[vs] == '\t')) vs++;
                int ve = e;
                while (ve > vs && (b[ve - 1] == ' ' || b[ve - 1] == '\t')) ve--;
                int nl = colon - p;
                switch (nl) {
                    case 14 -> {
                        if (ieq(b, p, nl, "content-length")) {
                            long v = 0;
                            for (int i = vs; i < ve; i++) {
                                if (b[i] < '0' || b[i] > '9') {
                                    replyErr(c, 400, "malformed request", true);
                                    return -1;
                                }
                                v = v * 10 + (b[i] - '0');
                                if (v > MAX_BODY) {
                                    replyErr(c, 413, "request body too large", true);
                                    return -1;
                                }
                            }
                            cl = (int) v;
                        }
                    }
                    case 13 -> {
                        if (ieq(b, p, nl, "authorization")) {
                            authOff = vs;
                            authLen = ve - vs;
                        }
                    }
                    case 15 -> {
                        if (ieq(b, p, nl, "accept-encoding") && contains(b, vs, ve, "gzip")) gzip = true;
                    }
                    case 10 -> {
                        if (ieq(b, p, nl, "connection")) {
                            if (contains(b, vs, ve, "close")) close = true;
                            else if (contains(b, vs, ve, "keep-alive")) close = false;
                        }
                    }
                    case 17 -> {
                        if (ieq(b, p, nl, "transfer-encoding") && contains(b, vs, ve, "chunked")) chunked = true;
                    }
                    case 6 -> {
                        if (ieq(b, p, nl, "expect") && contains(b, vs, ve, "100-continue")) expect = true;
                    }
                    default -> { }
                }
            }
            p = e + 2;
        }
        if (chunked) {
            replyErr(c, 400, "chunked request bodies are not supported", true);
            return -1;
        }
        if (bodyStart + cl > end) {
            if (expect && !c.sent100) {
                c.sent100 = true;
                byte[] cont = "HTTP/1.1 100 Continue\r\n\r\n".getBytes(StandardCharsets.US_ASCII);
                write(c, cont, 0, cont.length);
            }
            return 0;
        }
        int used = bodyStart + cl - off;
        c.sent100 = false;

        boolean get = sp1 - off == 3 && b[off] == 'G' && b[off + 1] == 'E' && b[off + 2] == 'T';
        boolean post = sp1 - off == 4 && b[off] == 'P' && b[off + 1] == 'O' && b[off + 2] == 'S' && b[off + 3] == 'T';
        if (get && pathIs(b, ps, pe, "/health")) {
            c.waiting = true;
            submit(new Req(c, Req.HEALTH, false, close));
        } else if (get && pathIs(b, ps, pe, "/feed")) {
            c.waiting = true;
            submit(new Req(c, Req.FEED, gzip, close));
        } else if (get && startsAt(b, ps, pe, "/posts/")) {
            int s = ps + 7;
            boolean slash = false;
            for (int i = s; i < pe; i++) if (b[i] == '/') slash = true;
            long id = slash ? -3 : parseId(b, s, pe);
            if (id == -3) {
                replyErr(c, 404, "not found", close);
            } else if (id == -1) {
                replyErr(c, 400, "invalid post id", close);
            } else if (id == -2) {
                replyErr(c, 404, "post not found", close);
            } else {
                Req r = new Req(c, Req.POST, gzip, close);
                r.id = id;
                c.waiting = true;
                submit(r);
            }
        } else if (post && pathIs(b, ps, pe, "/posts")) {
            createPost(c, b, authOff, authLen, bodyStart, cl, gzip, close);
        } else if (post && startsAt(b, ps, pe, "/posts/") && pe - ps > 12 && pathIs(b, pe - 5, pe, "/like")) {
            likePost(c, b, authOff, authLen, ps + 7, pe - 5, gzip, close);
        } else {
            replyErr(c, 404, "not found", close);
        }
        return c.closed ? -1 : used;
    }

    private Jwt.User authenticate(Conn c, byte[] b, int authOff, int authLen, boolean close) {
        try {
            return jwt.verify(b, authOff, authLen, System.currentTimeMillis() / 1000.0);
        } catch (Jwt.Rejected e) {
            replyErr(c, 401, e.getMessage(), close);
            return null;
        }
    }

    private void createPost(Conn c, byte[] b, int authOff, int authLen, int bodyStart, int cl, boolean gzip, boolean close) {
        Jwt.User user = authenticate(c, b, authOff, authLen, close);
        if (user == null) return;
        Object v;
        try {
            v = Jp.parse(b, bodyStart, cl);
        } catch (Jp.Bad e) {
            replyErr(c, 400, "malformed JSON body", close);
            return;
        }
        if (!(v instanceof Map<?, ?> m)) {
            replyErr(c, 400, "malformed JSON body", close);
            return;
        }
        if (!(m.get("body") instanceof String raw) || raw.strip().isEmpty()) {
            replyErr(c, 400, "body is required", close);
            return;
        }
        String text = raw.strip();
        if (text.codePointCount(0, text.length()) > 500) {
            replyErr(c, 400, "body must be at most 500 characters", close);
            return;
        }
        Req r = new Req(c, Req.CREATE, gzip, close);
        r.uid = user.id();
        r.user = user.name();
        r.body = text.getBytes(StandardCharsets.UTF_8);
        c.waiting = true;
        submit(r);
    }

    private void likePost(Conn c, byte[] b, int authOff, int authLen, int idStart, int idEnd, boolean gzip, boolean close) {
        Jwt.User user = authenticate(c, b, authOff, authLen, close);
        if (user == null) return;
        long id = parseId(b, idStart, idEnd);
        if (id == -1) {
            replyErr(c, 400, "invalid post id", close);
        } else if (id == -2) {
            replyErr(c, 404, "post not found", close);
        } else {
            Req r = new Req(c, Req.LIKE, gzip, close);
            r.uid = user.id();
            r.id = id;
            c.waiting = true;
            submit(r);
        }
    }

    // ---- Postgres work --------------------------------------------------------------------------------------------

    private void submit(Req r) {
        if (r.kind >= Req.CREATE) {
            if (writer.state != Pg.READY) {
                fail(r, 500);
                return;
            }
            writeQueue.add(r);
            return;
        }
        Pg best = null;
        for (Pg p : readers) {
            if (p.state == Pg.READY && (best == null || p.flight.size() < best.flight.size())) best = p;
        }
        if (best == null) {
            fail(r, 500);
            return;
        }
        best.send(Unit.of(r));
    }

    /** Sends the queued writes as one group: a single transaction, so one commit for all of them. */
    void dispatchWrites() {
        if (writer == null || writer.state != Pg.READY || !writer.flight.isEmpty() || writeQueue.isEmpty()) return;
        int n = Math.min(writeQueue.size(), 512);
        Req[] rs = new Req[n];
        for (int i = 0; i < n; i++) rs[i] = writeQueue.poll();
        writer.send(new Unit(rs, n));
    }

    void failQueued() {
        Req r;
        while ((r = writeQueue.poll()) != null) fail(r, 500);
    }

    void fail(Req r, int status) {
        if (r.kind == Req.HEALTH) {
            body.reset();
            body.ascii("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"no database connection\"}");
            reply(r, 503, body);
        } else {
            reply(r, status, errorBody("internal server error"));
        }
    }

    Out errorBody(String message) {
        errBody.reset();
        return errBody.ascii("{\"error\":").jstr(message).put('}');
    }

    // ---- responses ------------------------------------------------------------------------------------------------

    /** Answers a request that was waiting on Postgres, then continues with whatever queued up behind it. */
    void reply(Req r, int status, Out json) {
        Conn c = r.conn;
        c.waiting = false;
        if (c.closed) return;
        send(c, status, json, r.gzip, r.close);
        resume(c);
    }

    private void replyErr(Conn c, int status, String message, boolean close) {
        send(c, status, errorBody(message), false, close);
    }

    private static String statusLine(int status) {
        return switch (status) {
            case 100 -> "HTTP/1.1 100 Continue\r\n";
            case 200 -> "HTTP/1.1 200 OK\r\n";
            case 201 -> "HTTP/1.1 201 Created\r\n";
            case 400 -> "HTTP/1.1 400 Bad Request\r\n";
            case 401 -> "HTTP/1.1 401 Unauthorized\r\n";
            case 404 -> "HTTP/1.1 404 Not Found\r\n";
            case 413 -> "HTTP/1.1 413 Content Too Large\r\n";
            case 431 -> "HTTP/1.1 431 Request Header Fields Too Large\r\n";
            case 503 -> "HTTP/1.1 503 Service Unavailable\r\n";
            default -> "HTTP/1.1 500 Internal Server Error\r\n";
        };
    }

    private void send(Conn c, int status, Out json, boolean gzipOk, boolean close) {
        Out src = json;
        boolean z = false;
        if (gzipOk && cfg.gzipLevel > 0 && json.n >= cfg.gzipMin) {
            gzip(json);
            src = gz;
            z = true;
        }
        res.reset();
        res.ascii(statusLine(status)).ascii("content-type: application/json\r\n");
        if (z) res.ascii("content-encoding: gzip\r\n");
        if (close) res.ascii("connection: close\r\n");
        res.ascii("content-length: ").dec(src.n).ascii("\r\n\r\n").bytes(src.b, 0, src.n);
        write(c, res.b, 0, res.n);
        if (close && !c.closed) {
            if (c.rest == null) close(c);
            else c.closeAfter = true;
        }
    }

    private void gzip(Out src) {
        gz.reset();
        gz.ensure(src.n + 64);
        gz.put(0x1f).put(0x8b).put(8).put(0).put(0).put(0).put(0).put(0).put(0).put(3);
        deflater.reset();
        deflater.setInput(src.b, 0, src.n);
        deflater.finish();
        while (!deflater.finished()) {
            gz.ensure(4096);
            gz.n += deflater.deflate(gz.b, gz.n, gz.b.length - gz.n);
        }
        crc.reset();
        crc.update(src.b, 0, src.n);
        int v = (int) crc.getValue();
        gz.put(v).put(v >> 8).put(v >> 16).put(v >> 24);
        gz.put(src.n).put(src.n >> 8).put(src.n >> 16).put(src.n >> 24);
    }

    private void write(Conn c, byte[] a, int off, int len) {
        if (c.closed) return;
        if (c.rest != null) {
            int have = c.rest.length - c.restOff;
            byte[] n = new byte[have + len];
            System.arraycopy(c.rest, c.restOff, n, 0, have);
            System.arraycopy(a, off, n, have, len);
            c.rest = n;
            c.restOff = 0;
            return;
        }
        MemorySegment.copy(a, off, wbuf, JAVA_BYTE, 0, len);
        long r = sys.send(c.fd, wbuf, len);
        if (r == len) return;
        if (r < 0 && r != -Sys.EAGAIN && r != -Sys.EINTR) {
            close(c);
            return;
        }
        int sent = r < 0 ? 0 : (int) r;
        c.rest = Arrays.copyOfRange(a, off + sent, off + len);
        c.restOff = 0;
        // Stop reading further requests until this response is drained.
        sys.epollCtl(ep, Sys.EPOLL_CTL_MOD, c.fd, Sys.EPOLLOUT);
    }

    private void flushRest(Conn c) {
        while (c.rest != null && !c.closed) {
            int have = c.rest.length - c.restOff;
            int chunk = Math.min(have, (int) wbuf.byteSize());
            MemorySegment.copy(c.rest, c.restOff, wbuf, JAVA_BYTE, 0, chunk);
            long r = sys.send(c.fd, wbuf, chunk);
            if (r < 0) {
                if (r == -Sys.EAGAIN || r == -Sys.EINTR) return;
                close(c);
                return;
            }
            c.restOff += (int) r;
            if (r < chunk) return;
            if (c.restOff == c.rest.length) {
                c.rest = null;
                sys.epollCtl(ep, Sys.EPOLL_CTL_MOD, c.fd, Sys.EPOLLIN);
                if (c.closeAfter) close(c);
                else resume(c);
            }
        }
    }
}
