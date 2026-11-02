package challenge;

import static java.lang.foreign.ValueLayout.JAVA_BYTE;

import java.lang.foreign.MemorySegment;
import java.net.InetAddress;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.ArrayDeque;
import java.util.Arrays;
import java.util.Base64;
import javax.crypto.Mac;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.PBEKeySpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * One non-blocking connection to Postgres on the event loop's epoll set: the v3 wire protocol with SCRAM-SHA-256,
 * prepared statements, binary results and pipelining. A reader takes many units at once (replies come back in order);
 * a writer takes one unit at a time, which is a group commit.
 */
final class Pg {
    static final int DOWN = 0, CONNECTING = 1, AUTH = 2, PREPARING = 3, READY = 4;

    static final String FEED_SQL = "SELECT p.id, p.body, p.created_at, u.username, "
            + "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count "
            + "FROM posts p JOIN users u ON u.id = p.user_id ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
    static final String POST_SQL = "SELECT p.id, p.body, p.created_at, u.username, "
            + "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count "
            + "FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = $1";
    static final String CREATE_SQL = "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at";
    static final String LIKE_SQL =
            "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING";

    private static final String[] STMT = {"h", "f", "p", "c", "l"};
    private static final SecureRandom RANDOM = new SecureRandom();

    final Loop lp;
    final Sys sys;
    final boolean writer;
    int state = DOWN;
    int fd = -1;
    long retryAt;
    final ArrayDeque<Unit> flight = new ArrayDeque<>();

    private byte[] in = new byte[1 << 16];
    private int inLen;
    private final Out out = new Out(1 << 16);
    private int outSent;
    private boolean wantOut;
    boolean dirty;
    private final Out rows = new Out(8192);
    private int nrows;
    private Scram scram;

    Pg(Loop lp, boolean writer) {
        this.lp = lp;
        this.sys = lp.sys;
        this.writer = writer;
    }

    // ---- connection ----------------------------------------------------------------------------------------

    void connect() {
        try {
            byte[] ip = InetAddress.getByName(lp.cfg.pgHost).getAddress();
            if (ip.length != 4) throw new IllegalStateException("IPv4 address needed for " + lp.cfg.pgHost);
            fd = sys.socket();
            if (fd < 0) {
                int e = fd;
                fd = -1;
                throw new IllegalStateException("socket: errno " + -e);
            }
            sys.setIntOpt(fd, Sys.IPPROTO_TCP, Sys.TCP_NODELAY, 1);
            int r = sys.connect(fd, ip, lp.cfg.pgPort);
            if (r != 0 && r != -Sys.EINPROGRESS) throw new IllegalStateException("connect: errno " + -r);
            Loop.TABLE[fd] = this;
            state = CONNECTING;
            sys.epollCtl(lp.ep, Sys.EPOLL_CTL_ADD, fd, Sys.EPOLLIN | Sys.EPOLLOUT);
            wantOut = true;
        } catch (Exception e) {
            fail(e.toString());
        }
    }

    private void connected() {
        int err = sys.soError(fd);
        if (err != 0) {
            fail("connect failed: errno " + Math.abs(err));
            return;
        }
        state = AUTH;
        inLen = 0;
        out.reset();
        outSent = 0;
        int start = out.n;
        out.n += 4;
        i32(out, 196608);
        cstr(out, "user");
        cstr(out, lp.cfg.pgUser);
        cstr(out, "database");
        cstr(out, lp.cfg.pgDb);
        cstr(out, "client_encoding");
        cstr(out, "UTF8");
        out.put(0);
        patchLen(out, start);
        flushOut();
    }

    void fail(String why) {
        if (state != DOWN || fd >= 0) System.err.println("postgres connection (" + (writer ? "writer" : "reader") + ") lost: " + why);
        if (fd >= 0) {
            Loop.TABLE[fd] = null;
            sys.close(fd);
            fd = -1;
        }
        state = DOWN;
        retryAt = System.nanoTime() + 1_000_000_000L;
        inLen = 0;
        out.reset();
        outSent = 0;
        wantOut = false;
        rows.reset();
        nrows = 0;
        scram = null;
        Unit u;
        while ((u = flight.poll()) != null) {
            for (int i = 0; i < u.n; i++) lp.fail(u.rs[i], 500);
        }
        if (writer) lp.failQueued();
    }

    // ---- events --------------------------------------------------------------------------------------------

    void onEvent(int ev) {
        if (state == CONNECTING) {
            if ((ev & (Sys.EPOLLOUT | Sys.EPOLLERR | Sys.EPOLLHUP)) != 0) connected();
            return;
        }
        if ((ev & (Sys.EPOLLIN | Sys.EPOLLERR | Sys.EPOLLHUP)) != 0) readAll();
        if (fd >= 0 && (ev & Sys.EPOLLOUT) != 0) flushOut();
    }

    private void readAll() {
        MemorySegment rbuf = lp.rbuf;
        if (inLen == in.length) in = Arrays.copyOf(in, in.length * 2);
        long want = Math.min(rbuf.byteSize(), in.length - inLen);
        long n = sys.read(fd, rbuf, want);
        if (n == -Sys.EAGAIN || n == -Sys.EINTR) return;
        if (n <= 0) {
            fail(n == 0 ? "closed by server" : "read: errno " + -n);
            return;
        }
        MemorySegment.copy(rbuf, JAVA_BYTE, 0, in, inLen, (int) n);
        inLen += (int) n;
        parse();
    }

    private int i32(int p) {
        return ((in[p] & 255) << 24) | ((in[p + 1] & 255) << 16) | ((in[p + 2] & 255) << 8) | (in[p + 3] & 255);
    }

    private long i64(int p) {
        return ((long) i32(p) << 32) | (i32(p + 4) & 0xFFFFFFFFL);
    }

    private void parse() {
        int p = 0;
        while (inLen - p >= 5) {
            int len = i32(p + 1);
            if (len < 4) {
                fail("bad message length");
                return;
            }
            if (inLen - p < 1 + len) {
                if (1 + len > in.length) in = Arrays.copyOf(in, Math.max(in.length * 2, 1 + len));
                break;
            }
            handle(in[p], p + 5, len - 4);
            if (state == DOWN) return;
            p += 1 + len;
        }
        if (p > 0) {
            System.arraycopy(in, p, in, 0, inLen - p);
            inLen -= p;
        }
    }

    private void handle(byte t, int off, int len) {
        switch (t) {
            case 'R' -> auth(off, len);
            case 'Z' -> ready();
            case 'D' -> dataRow(off);
            case 'C' -> commandComplete(off, len);
            case 'E' -> errorResponse(off, len);
            default -> { /* ParseComplete, BindComplete, ParameterStatus, BackendKeyData, NoData, notices */ }
        }
    }

    // ---- startup and authentication ------------------------------------------------------------------------

    private void auth(int off, int len) {
        int code = i32(off);
        try {
            switch (code) {
                case 0 -> { /* AuthenticationOk */ }
                case 3 -> {
                    int s = out.n;
                    out.put('p');
                    out.n += 4;
                    cstr(out, lp.cfg.pgPass);
                    patchLen(out, s + 1);
                    flushOut();
                }
                case 10 -> {
                    scram = new Scram(lp.cfg.pgPass);
                    byte[] first = scram.clientFirst();
                    int s = out.n;
                    out.put('p');
                    out.n += 4;
                    cstr(out, "SCRAM-SHA-256");
                    i32(out, first.length);
                    out.bytes(first, 0, first.length);
                    patchLen(out, s + 1);
                    flushOut();
                }
                case 11 -> {
                    byte[] fin = scram.clientFinal(new String(in, off + 4, len - 4, StandardCharsets.UTF_8));
                    int s = out.n;
                    out.put('p');
                    out.n += 4;
                    out.bytes(fin, 0, fin.length);
                    patchLen(out, s + 1);
                    flushOut();
                }
                case 12 -> scram.verify(new String(in, off + 4, len - 4, StandardCharsets.UTF_8));
                default -> fail("unsupported authentication method " + code);
            }
        } catch (GeneralSecurityException | IllegalStateException e) {
            fail("authentication: " + e.getMessage());
        }
    }

    private void prepare() {
        String[] sql = {"SELECT 1", FEED_SQL, POST_SQL, CREATE_SQL, LIKE_SQL};
        int[][] oids = {{}, {}, {20}, {20, 25}, {20, 20}};
        for (int i = 0; i < sql.length; i++) {
            int s = out.n;
            out.put('P');
            out.n += 4;
            cstr(out, STMT[i]);
            cstr(out, sql[i]);
            i16(out, oids[i].length);
            for (int oid : oids[i]) i32(out, oid);
            patchLen(out, s + 1);
        }
        sync();
        flushOut();
    }

    private void ready() {
        switch (state) {
            case AUTH -> {
                state = PREPARING;
                prepare();
            }
            case PREPARING -> {
                state = READY;
                flushOut();
                if (writer) lp.dispatchWrites();
            }
            case READY -> {
                Unit u = flight.poll();
                if (u != null) finish(u);
            }
            default -> { }
        }
    }

    // ---- results ---------------------------------------------------------------------------------------------

    private void dataRow(int off) {
        Unit u = flight.peek();
        if (u == null) return;
        Req r = u.rs[Math.min(u.cmd, u.n - 1)];
        int p = off + 2;
        switch (r.kind) {
            case Req.FEED, Req.POST -> {
                if (nrows++ > 0) rows.put(',');
                long id = 0, created = 0, likes = 0;
                int bodyOff = 0, bodyLen = 0, nameOff = 0, nameLen = 0;
                for (int c = 0; c < 5; c++) {
                    int l = i32(p);
                    p += 4;
                    switch (c) {
                        case 0 -> id = i64(p);
                        case 1 -> {
                            bodyOff = p;
                            bodyLen = l;
                        }
                        case 2 -> created = i64(p);
                        case 3 -> {
                            nameOff = p;
                            nameLen = l;
                        }
                        default -> likes = i32(p);
                    }
                    if (l > 0) p += l;
                }
                rows.ascii("{\"id\":").dec(id).ascii(",\"body\":").jstr(in, bodyOff, bodyLen).ascii(",\"created_at\":\"")
                        .timestamp(created).ascii("\",\"author\":").jstr(in, nameOff, nameLen).ascii(",\"like_count\":")
                        .dec(likes).put('}');
            }
            case Req.CREATE -> {
                p += 4;
                r.resId = i64(p);
                p += 8 + 4;
                r.resMicros = i64(p);
            }
            default -> { }
        }
    }

    private void commandComplete(int off, int len) {
        Unit u = flight.peek();
        if (u == null) return;
        if (u.cmd < u.n && u.rs[u.cmd].kind == Req.LIKE) {
            // "INSERT 0 1" when the like was new, "INSERT 0 0" when it already existed
            u.rs[u.cmd].liked = len >= 2 && in[off + len - 2] == '1';
        }
        u.cmd++;
    }

    private void errorResponse(int off, int len) {
        Unit u = flight.peek();
        int p = off, end = off + len;
        String state = "";
        String msg = "";
        while (p < end && in[p] != 0) {
            byte f = in[p++];
            int s = p;
            while (p < end && in[p] != 0) p++;
            String v = new String(in, s, p - s, StandardCharsets.UTF_8);
            p++;
            if (f == 'C') state = v;
            else if (f == 'M') msg = v;
        }
        if (u == null) {
            fail("startup/preparation error " + state + ": " + msg);
            return;
        }
        u.err = true;
        u.sqlState = state;
        if (!"23503".equals(state)) System.err.println("postgres error " + state + ": " + msg);
    }

    private void finish(Unit u) {
        Out body = lp.body;
        if (u.err) {
            if (u.n > 1) {
                // a failed group: run each request in its own transaction so each gets its own answer
                for (int i = 0; i < u.n; i++) send(Unit.of(u.rs[i]));
            } else if ("23503".equals(u.sqlState)) {
                lp.reply(u.rs[0], 404, lp.errorBody("post not found"));
            } else {
                lp.fail(u.rs[0], 500);
            }
        } else {
            for (int i = 0; i < u.n; i++) {
                Req r = u.rs[i];
                body.reset();
                switch (r.kind) {
                    case Req.HEALTH -> {
                        long up = (System.nanoTime() - Loop.STARTED) / 1_000_000_000L;
                        body.ascii("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":").dec(up).put('}');
                        lp.reply(r, 200, body);
                    }
                    case Req.FEED -> {
                        body.ascii("{\"posts\":[").bytes(rows.b, 0, rows.n).ascii("]}");
                        lp.reply(r, 200, body);
                    }
                    case Req.POST -> {
                        if (nrows == 0) {
                            lp.reply(r, 404, lp.errorBody("post not found"));
                        } else {
                            body.ascii("{\"post\":").bytes(rows.b, 0, rows.n).put('}');
                            lp.reply(r, 200, body);
                        }
                    }
                    case Req.CREATE -> {
                        body.ascii("{\"post\":{\"id\":").dec(r.resId).ascii(",\"body\":").jstr(r.body, 0, r.body.length)
                                .ascii(",\"created_at\":\"").timestamp(r.resMicros).ascii("\",\"author\":").jstr(r.user)
                                .ascii(",\"like_count\":0}}");
                        lp.reply(r, 201, body);
                    }
                    default -> {
                        body.ascii("{\"liked\":true,\"already_liked\":").ascii(r.liked ? "false" : "true")
                                .ascii(",\"post_id\":").dec(r.id).put('}');
                        lp.reply(r, r.liked ? 201 : 200, body);
                    }
                }
            }
        }
        rows.reset();
        nrows = 0;
        if (writer) lp.dispatchWrites();
    }

    // ---- sending ----------------------------------------------------------------------------------------------

    /** Queues a unit: Bind and Execute for each request, then one Sync. */
    void send(Unit u) {
        for (int i = 0; i < u.n; i++) {
            Req r = u.rs[i];
            int s = out.n;
            out.put('B');
            out.n += 4;
            out.put(0);
            cstr(out, STMT[r.kind]);
            i16(out, 1);
            i16(out, 1);
            switch (r.kind) {
                case Req.POST -> {
                    i16(out, 1);
                    i32(out, 8);
                    i64(out, r.id);
                }
                case Req.CREATE -> {
                    i16(out, 2);
                    i32(out, 8);
                    i64(out, r.uid);
                    i32(out, r.body.length);
                    out.bytes(r.body, 0, r.body.length);
                }
                case Req.LIKE -> {
                    i16(out, 2);
                    i32(out, 8);
                    i64(out, r.uid);
                    i32(out, 8);
                    i64(out, r.id);
                }
                default -> i16(out, 0);
            }
            i16(out, 1);
            i16(out, 1);
            patchLen(out, s + 1);
            s = out.n;
            out.put('E');
            out.n += 4;
            out.put(0);
            i32(out, 0);
            patchLen(out, s + 1);
        }
        sync();
        flight.add(u);
        markDirty();
    }

    private void sync() {
        out.put('S');
        i32(out, 4);
    }

    private void markDirty() {
        if (!dirty) {
            dirty = true;
            lp.dirty(this);
        }
    }

    void flushOut() {
        dirty = false;
        if (fd < 0) return;
        MemorySegment wbuf = lp.wbuf;
        while (outSent < out.n) {
            int chunk = (int) Math.min(out.n - outSent, wbuf.byteSize());
            MemorySegment.copy(out.b, outSent, wbuf, JAVA_BYTE, 0, chunk);
            long r = sys.send(fd, wbuf, chunk);
            if (r > 0) outSent += (int) r;
            if (r == -Sys.EAGAIN || (r >= 0 && r < chunk)) {
                if (!wantOut) {
                    wantOut = true;
                    sys.epollCtl(lp.ep, Sys.EPOLL_CTL_MOD, fd, Sys.EPOLLIN | Sys.EPOLLOUT);
                }
                return;
            }
            if (r < 0 && r != -Sys.EINTR) {
                fail("send: errno " + -r);
                return;
            }
        }
        out.reset();
        outSent = 0;
        if (wantOut && state != CONNECTING) {
            wantOut = false;
            sys.epollCtl(lp.ep, Sys.EPOLL_CTL_MOD, fd, Sys.EPOLLIN);
        }
    }

    // ---- encoding helpers ----------------------------------------------------------------------------------------

    private static void i16(Out o, int v) {
        o.put(v >> 8).put(v);
    }

    private static void i32(Out o, int v) {
        o.put(v >> 24).put(v >> 16).put(v >> 8).put(v);
    }

    private static void i64(Out o, long v) {
        i32(o, (int) (v >> 32));
        i32(o, (int) v);
    }

    private static void cstr(Out o, String s) {
        byte[] u = s.getBytes(StandardCharsets.UTF_8);
        o.bytes(u, 0, u.length).put(0);
    }

    /** Writes the length (counting itself, up to the end of the buffer) at `at`. */
    private static void patchLen(Out o, int at) {
        int len = o.n - at;
        o.b[at] = (byte) (len >> 24);
        o.b[at + 1] = (byte) (len >> 16);
        o.b[at + 2] = (byte) (len >> 8);
        o.b[at + 3] = (byte) len;
    }

    // ---- SCRAM-SHA-256 ---------------------------------------------------------------------------------------------

    private static final class Scram {
        private final String password;
        private final String nonce;
        private String bare;
        private byte[] serverSignature;

        Scram(String password) {
            this.password = password;
            byte[] n = new byte[18];
            RANDOM.nextBytes(n);
            this.nonce = Base64.getEncoder().encodeToString(n);
        }

        byte[] clientFirst() {
            bare = "n=,r=" + nonce;
            return ("n,," + bare).getBytes(StandardCharsets.UTF_8);
        }

        byte[] clientFinal(String serverFirst) throws GeneralSecurityException {
            String r = null, s = null;
            int iter = 0;
            for (String part : serverFirst.split(",")) {
                if (part.startsWith("r=")) r = part.substring(2);
                else if (part.startsWith("s=")) s = part.substring(2);
                else if (part.startsWith("i=")) iter = Integer.parseInt(part.substring(2));
            }
            if (r == null || s == null || iter <= 0 || !r.startsWith(nonce)) throw new IllegalStateException("bad server-first message");
            byte[] salted = SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256")
                    .generateSecret(new PBEKeySpec(password.toCharArray(), Base64.getDecoder().decode(s), iter, 256)).getEncoded();
            byte[] clientKey = hmac(salted, "Client Key");
            byte[] storedKey = MessageDigest.getInstance("SHA-256").digest(clientKey);
            String finalNoProof = "c=biws,r=" + r;
            byte[] authMessage = (bare + "," + serverFirst + "," + finalNoProof).getBytes(StandardCharsets.UTF_8);
            byte[] clientSig = hmac(storedKey, authMessage);
            byte[] proof = new byte[clientKey.length];
            for (int i = 0; i < proof.length; i++) proof[i] = (byte) (clientKey[i] ^ clientSig[i]);
            serverSignature = hmac(hmac(salted, "Server Key"), authMessage);
            return (finalNoProof + ",p=" + Base64.getEncoder().encodeToString(proof)).getBytes(StandardCharsets.UTF_8);
        }

        void verify(String serverFinal) {
            if (!serverFinal.startsWith("v=")
                    || !MessageDigest.isEqual(Base64.getDecoder().decode(serverFinal.substring(2).trim()), serverSignature)) {
                throw new IllegalStateException("server signature mismatch");
            }
        }

        private static byte[] hmac(byte[] key, String msg) throws GeneralSecurityException {
            return hmac(key, msg.getBytes(StandardCharsets.UTF_8));
        }

        private static byte[] hmac(byte[] key, byte[] msg) throws GeneralSecurityException {
            Mac m = Mac.getInstance("HmacSHA256");
            m.init(new SecretKeySpec(key, "HmacSHA256"));
            return m.doFinal(msg);
        }
    }
}
