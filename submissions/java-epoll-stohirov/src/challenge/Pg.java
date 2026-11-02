package challenge;

import static java.lang.foreign.ValueLayout.JAVA_BYTE;

import java.io.IOException;
import java.lang.foreign.MemorySegment;
import java.net.InetAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Arrays;
import java.util.Base64;
import java.util.HexFormat;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

final class Pg {
    static final byte FEED = 0, POST = 1, CREATE = 2, LIKE = 3, PING = 4;

    private static final String POST_SQL = "SELECT p.id, p.body, p.created_at, u.username,"
            + " (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count"
            + " FROM posts p JOIN users u ON u.id = p.user_id";
    private static final String[] SQL = {
        POST_SQL + " ORDER BY p.created_at DESC, p.id DESC LIMIT 20",
        POST_SQL + " WHERE p.id = $1",
        "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at",
        "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING",
        "SELECT 1",
    };
    private static final int INT8 = 20, TEXT = 25;
    private static final int[][] PARAM_TYPES = {{}, {INT8}, {INT8, TEXT}, {INT8, INT8}, {}};
    private static final int CONNECT_TIMEOUT_MILLIS = 3000;

    record Config(String host, int port, String user, String password, String database) {
        static Config parse(String url) {
            URI u = URI.create(url);
            if (!"postgres".equals(u.getScheme()) && !"postgresql".equals(u.getScheme())) {
                throw new IllegalArgumentException("DATABASE_URL must be postgres://user:password@host:port/database");
            }
            String user = System.getProperty("user.name"), password = "";
            String info = u.getRawUserInfo();
            if (info != null) {
                int c = info.indexOf(':');
                user = decode(c < 0 ? info : info.substring(0, c));
                if (c >= 0) password = decode(info.substring(c + 1));
            }
            String host = u.getHost() == null ? "localhost" : u.getHost();
            if (host.startsWith("[")) host = host.substring(1, host.length() - 1);
            String path = u.getRawPath();
            String db = path == null || path.length() <= 1 ? user : decode(path.substring(1));
            return new Config(host, u.getPort() < 0 ? 5432 : u.getPort(), user, password, db);
        }

        private static String decode(String s) {
            byte[] out = new byte[s.length()];
            int n = 0;
            for (int k = 0; k < s.length(); k++) {
                char c = s.charAt(k);
                if (c == '%' && k + 2 < s.length()) {
                    out[n++] = (byte) Integer.parseInt(s, k + 1, k + 3, 16);
                    k += 2;
                } else {
                    out[n++] = (byte) c;
                }
            }
            return new String(out, 0, n, StandardCharsets.UTF_8);
        }
    }

    interface Handler {
        void row(Pg pg, Conn c, byte[] b, int at);

        void command(Conn c, byte[] b, int at, int len);

        void done(Pg pg, Conn[] unit, int n, String code, String message);
    }

    final int tag;
    private final Config cfg;
    private final Sys sys;
    int fd = -1;
    final Buf in = new Buf(1 << 16), out = new Buf(1 << 16);
    private int outFrom;
    boolean dirty, wantWrite, recvArmed;
    final Buf body = new Buf(1 << 16);
    String lastError = "not connected";

    private Conn[] ring = new Conn[64];
    private int head, size, cur;
    private Conn[] unit = new Conn[64];
    private String code, message;

    Pg(int tag, Config cfg, Sys sys) {
        this.tag = tag;
        this.cfg = cfg;
        this.sys = sys;
    }

    int inFlight() { return size; }

    boolean alive() { return fd >= 0; }


    void query(Conn c) {
        bind(c);
        sync();
        c.last = true;
        push(c);
    }

    void batch(Conn[] cs, int n) {
        for (int i = 0; i < n; i++) {
            bind(cs[i]);
            cs[i].last = i == n - 1;
            push(cs[i]);
        }
        sync();
    }

    private void bind(Conn c) {
        Buf o = out;
        o.put('B');
        int lenAt = o.len;
        o.i32(0);
        o.put(0);
        o.put('0' + c.op);
        o.put(0);
        o.i16(1);
        o.i16(1);
        switch (c.op) {
            case POST -> { o.i16(1); o.i32(8); o.i64(c.id); }
            case CREATE -> { o.i16(2); o.i32(8); o.i64(c.userId); o.i32(c.text.length); o.put(c.text); }
            case LIKE -> { o.i16(2); o.i32(8); o.i64(c.userId); o.i32(8); o.i64(c.id); }
            default -> o.i16(0);
        }
        o.i16(1);
        o.i16(1);
        o.setI32(lenAt, o.len - lenAt);
        o.put('E');
        o.i32(9);
        o.put(0);
        o.i32(0);
    }

    private void sync() {
        out.put('S');
        out.i32(4);
    }

    private void push(Conn c) {
        if (size == ring.length) {
            Conn[] r = new Conn[size * 2];
            for (int i = 0; i < size; i++) r[i] = ring[(head + i) & (ring.length - 1)];
            ring = r;
            head = 0;
        }
        ring[(head + size++) & (ring.length - 1)] = c;
    }


    boolean read(MemorySegment io) {
        long r = sys.read(fd, io, io.byteSize());
        if (r == 0) {
            lastError = "connection closed by the database";
            return false;
        }
        if (r < 0) {
            int e = sys.errno();
            if (e == Sys.EAGAIN || e == Sys.EINTR) return true;
            lastError = "read: errno " + e;
            return false;
        }
        in.ensure((int) r);
        MemorySegment.copy(io, JAVA_BYTE, 0, in.a, in.len, (int) r);
        in.len += (int) r;
        return true;
    }

    void received(MemorySegment data, int n) {
        in.ensure(n);
        MemorySegment.copy(data, JAVA_BYTE, 0, in.a, in.len, n);
        in.len += n;
    }

    boolean flush(MemorySegment io) {
        while (outFrom < out.len) {
            int n = (int) Math.min(out.len - outFrom, io.byteSize());
            MemorySegment.copy(out.a, outFrom, io, JAVA_BYTE, 0, n);
            long w = sys.write(fd, io, n);
            if (w < 0) {
                int e = sys.errno();
                if (e == Sys.EINTR) continue;
                if (e == Sys.EAGAIN) break;
                lastError = "write: errno " + e;
                return false;
            }
            outFrom += (int) w;
            if (w < n) break;
        }
        if (outFrom == out.len) out.len = outFrom = 0;
        return true;
    }

    boolean flushed() { return out.len == 0; }

    void parse(Handler h) {
        byte[] b = in.a;
        int p = 0, end = in.len;
        while (end - p >= 5) {
            int n = Buf.i32(b, p + 1);
            if (end - p < 1 + n) break;
            byte type = b[p];
            int at = p + 5, len = n - 4;
            p += 1 + n;
            switch (type) {
                case 'D' -> { if (cur < size) h.row(this, ring[(head + cur) & (ring.length - 1)], b, at); }
                case 'C' -> { if (cur < size) h.command(ring[(head + cur++) & (ring.length - 1)], b, at, len - 1); }
                case 'E' -> {
                    String[] f = errorFields(b, at, at + len);
                    if (size == 0) {
                        System.err.println("postgres: " + f[1] + " (" + f[0] + ")");
                    } else if (code == null) {
                        code = f[0];
                        message = f[1];
                    }
                }
                case 'Z' -> { if (size > 0) done(h); }
                default -> { }
            }
            if (fd < 0) return;
        }
        System.arraycopy(b, p, b, 0, end - p);
        in.len = end - p;
    }

    private void done(Handler h) {
        int n = 0;
        while (size > 0) {
            Conn c = ring[head];
            ring[head] = null;
            head = (head + 1) & (ring.length - 1);
            size--;
            if (n == unit.length) unit = Arrays.copyOf(unit, n * 2);
            unit[n++] = c;
            if (c.last) break;
        }
        String code = this.code, message = this.message;
        this.code = this.message = null;
        cur = 0;
        h.done(this, unit, n, code, message);
        Arrays.fill(unit, 0, n, null);
    }

    Conn[] close(boolean closeFd) {
        if (fd >= 0 && closeFd) Sys.close(fd);
        fd = -1;
        Conn[] lost = new Conn[size];
        for (int i = 0; i < size; i++) {
            int k = (head + i) & (ring.length - 1);
            lost[i] = ring[k];
            ring[k] = null;
        }
        head = size = cur = 0;
        code = message = null;
        in.len = out.len = outFrom = body.len = 0;
        wantWrite = recvArmed = false;
        return lost;
    }


    void connect(MemorySegment io) throws IOException {
        int f = -1;
        InetAddress[] addresses;
        try {
            addresses = InetAddress.getAllByName(cfg.host());
        } catch (java.net.UnknownHostException e) {
            throw new IOException("unknown host " + cfg.host(), e);
        }
        for (InetAddress a : addresses) {
            f = sys.connect(a, cfg.port(), CONNECT_TIMEOUT_MILLIS);
            if (f >= 0) break;
        }
        if (f < 0) throw new IOException("connect " + cfg.host() + ":" + cfg.port() + ": errno " + -f);
        fd = f;
        try {
            startup(io);
            prepare(io);
            sys.setNonBlocking(fd);
        } catch (IOException | GeneralSecurityException | RuntimeException e) {
            close(true);
            throw e instanceof IOException io2 ? io2 : new IOException(e.toString(), e);
        }
    }

    private void startup(MemorySegment io) throws IOException, GeneralSecurityException {
        out.i32(0);
        out.i32(196608);
        out.cstr("user");
        out.put(utf8z(cfg.user()));
        out.cstr("database");
        out.put(utf8z(cfg.database()));
        out.cstr("client_encoding");
        out.cstr("UTF8");
        out.put(0);
        out.setI32(0, out.len);
        sendBlocking(io);
        Scram scram = null;
        int p = 0;
        while (true) {
            p = nextBlocking(io, p);
            byte type = in.a[msgAt - 5];
            if (type == 'E') throw new IOException(errorText(msgAt, msgEnd));
            if (type == 'Z') break;
            if (type != 'R') continue;
            int auth = Buf.i32(in.a, msgAt);
            String data = new String(in.a, msgAt + 4, msgEnd - msgAt - 4, StandardCharsets.UTF_8);
            switch (auth) {
                case 0 -> { }
                case 3 -> password(io, utf8z(cfg.password()));
                case 5 -> {
                    MessageDigest md5 = MessageDigest.getInstance("MD5");
                    HexFormat hex = HexFormat.of();
                    String inner = hex.formatHex(md5.digest((cfg.password() + cfg.user()).getBytes(StandardCharsets.UTF_8)));
                    md5.update(inner.getBytes(StandardCharsets.US_ASCII));
                    md5.update(in.a, msgAt + 4, 4);
                    password(io, utf8z("md5" + hex.formatHex(md5.digest())));
                }
                case 10 -> {
                    if (!Arrays.asList(data.split("\0")).contains("SCRAM-SHA-256")) {
                        throw new IOException("the database offers no supported SASL mechanism: " + data);
                    }
                    scram = new Scram(cfg.password());
                    byte[] first = scram.first();
                    out.put('p');
                    out.i32(4 + 14 + 4 + first.length);
                    out.cstr("SCRAM-SHA-256");
                    out.i32(first.length);
                    out.put(first);
                    sendBlocking(io);
                }
                case 11 -> {
                    if (scram == null) throw new IOException("SASL continue without SASL");
                    byte[] last = scram.next(data);
                    out.put('p');
                    out.i32(4 + last.length);
                    out.put(last);
                    sendBlocking(io);
                }
                case 12 -> { if (scram == null || !scram.verify(data)) throw new IOException("bad SCRAM server signature"); }
                default -> throw new IOException("unsupported authentication method " + auth);
            }
        }
        in.len = 0;
    }

    private void password(MemorySegment io, byte[] z) throws IOException {
        out.put('p');
        out.i32(4 + z.length);
        out.put(z);
        sendBlocking(io);
    }

    private void prepare(MemorySegment io) throws IOException {
        for (int s = 0; s < SQL.length; s++) {
            out.put('P');
            int lenAt = out.len;
            out.i32(0);
            out.cstr(String.valueOf(s));
            out.cstr(SQL[s]);
            out.i16(PARAM_TYPES[s].length);
            for (int t : PARAM_TYPES[s]) out.i32(t);
            out.setI32(lenAt, out.len - lenAt);
        }
        sync();
        sendBlocking(io);
        String error = null;
        int p = 0;
        while (true) {
            p = nextBlocking(io, p);
            byte type = in.a[msgAt - 5];
            if (type == 'E' && error == null) error = errorText(msgAt, msgEnd);
            if (type == 'Z') break;
        }
        in.len = 0;
        if (error != null) throw new IOException("prepare: " + error);
    }

    private int msgAt, msgEnd;

    private int nextBlocking(MemorySegment io, int p) throws IOException {
        while (in.len - p < 5 || in.len - p < 1 + Buf.i32(in.a, p + 1)) {
            long r = sys.readBlocking(fd, io, io.byteSize());
            if (r <= 0) {
                throw new IOException(r == 0 ? "connection closed by the database"
                        : sys.errno() == Sys.EAGAIN ? "timed out" : "read: errno " + sys.errno());
            }
            in.ensure((int) r);
            MemorySegment.copy(io, JAVA_BYTE, 0, in.a, in.len, (int) r);
            in.len += (int) r;
        }
        msgAt = p + 5;
        msgEnd = p + 1 + Buf.i32(in.a, p + 1);
        return msgEnd;
    }

    private void sendBlocking(MemorySegment io) throws IOException {
        int from = 0;
        while (from < out.len) {
            int n = (int) Math.min(out.len - from, io.byteSize());
            MemorySegment.copy(out.a, from, io, JAVA_BYTE, 0, n);
            long w = sys.writeBlocking(fd, io, n);
            if (w <= 0) throw new IOException("write: errno " + sys.errno());
            from += (int) w;
        }
        out.len = 0;
    }

    private String errorText(int from, int to) {
        String[] f = errorFields(in.a, from, to);
        return f[1] + " (" + f[0] + ")";
    }

    private static String[] errorFields(byte[] b, int from, int to) {
        String sqlState = "", msg = "";
        int k = from;
        while (k < to && b[k] != 0) {
            byte field = b[k++];
            int z = k;
            while (z < to && b[z] != 0) z++;
            if (field == 'C') sqlState = new String(b, k, z - k, StandardCharsets.UTF_8);
            else if (field == 'M') msg = new String(b, k, z - k, StandardCharsets.UTF_8);
            k = z + 1;
        }
        return new String[] {sqlState, msg};
    }

    private static byte[] utf8z(String s) {
        byte[] b = s.getBytes(StandardCharsets.UTF_8);
        return Arrays.copyOf(b, b.length + 1);
    }

    private static final class Scram {
        private final byte[] password;
        private final String nonce, clientFirstBare;
        private byte[] serverSignature;

        Scram(String password) {
            this.password = password.getBytes(StandardCharsets.UTF_8);
            byte[] r = new byte[18];
            new SecureRandom().nextBytes(r);
            nonce = Base64.getEncoder().encodeToString(r);
            clientFirstBare = "n=,r=" + nonce;
        }

        byte[] first() { return ("n,," + clientFirstBare).getBytes(StandardCharsets.US_ASCII); }

        byte[] next(String serverFirst) throws IOException, GeneralSecurityException {
            String r = null, s = null;
            int i = 0;
            for (String attr : serverFirst.split(",")) {
                if (attr.startsWith("r=")) r = attr.substring(2);
                else if (attr.startsWith("s=")) s = attr.substring(2);
                else if (attr.startsWith("i=")) i = Integer.parseInt(attr.substring(2));
            }
            if (r == null || s == null || i <= 0 || !r.startsWith(nonce)) throw new IOException("bad SCRAM server-first-message");
            byte[] salted = pbkdf2(password, Base64.getDecoder().decode(s), i);
            byte[] clientKey = hmac(salted, "Client Key".getBytes(StandardCharsets.US_ASCII));
            byte[] storedKey = MessageDigest.getInstance("SHA-256").digest(clientKey);
            String withoutProof = "c=biws,r=" + r;
            byte[] auth = (clientFirstBare + "," + serverFirst + "," + withoutProof).getBytes(StandardCharsets.UTF_8);
            byte[] proof = hmac(storedKey, auth);
            for (int k = 0; k < proof.length; k++) proof[k] ^= clientKey[k];
            serverSignature = hmac(hmac(salted, "Server Key".getBytes(StandardCharsets.US_ASCII)), auth);
            return (withoutProof + ",p=" + Base64.getEncoder().encodeToString(proof)).getBytes(StandardCharsets.US_ASCII);
        }

        boolean verify(String serverFinal) {
            return serverFinal.startsWith("v=")
                    && MessageDigest.isEqual(Base64.getDecoder().decode(serverFinal.substring(2).trim()), serverSignature);
        }

        private static byte[] hmac(byte[] key, byte[] data) throws GeneralSecurityException {
            Mac mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(key, "HmacSHA256"));
            return mac.doFinal(data);
        }

        private static byte[] pbkdf2(byte[] password, byte[] salt, int iterations) throws GeneralSecurityException {
            Mac mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec(password, "HmacSHA256"));
            mac.update(salt);
            byte[] u = mac.doFinal(new byte[] {0, 0, 0, 1});
            byte[] result = u.clone();
            for (int k = 1; k < iterations; k++) {
                u = mac.doFinal(u);
                for (int j = 0; j < result.length; j++) result[j] ^= u[j];
            }
            return result;
        }
    }
}
