package feed.pg;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.NoSuchAlgorithmException;
import java.util.Arrays;
import java.util.HexFormat;
import java.util.List;
import java.util.function.Supplier;

/**
 * The frontend side of one Postgres connection as a socket-free state machine. The caller owns the socket: it reads
 * into {@link #readBuffer()} and calls {@link #process()}, and writes out whatever {@link #output()} holds. Startup
 * (authentication, then preparing every statement) runs through the same calls; {@link #isReady()} says when
 * requests may be sent.
 *
 * <p>A request is {@link #bind}, its parameters, then {@link #execute}: Bind, Execute and Sync, so every request is
 * its own implicit transaction and an error in one never affects its neighbours. Requests pipeline freely; replies
 * come back in order, and each request ends at the ReadyForQuery that answers its Sync. Parameters and results are
 * all binary. Steady-state requests allocate nothing.
 *
 * <p>Writes can share a commit instead (group commit): requests ended with {@link #executeInGroup} go out with one
 * Sync at {@link #endGroup}, so they run as one transaction. Their handlers hear nothing until the group's
 * ReadyForQuery is in. If one statement fails, the whole group was rolled back, and each request is sent again in a
 * transaction of its own, so every handler still sees the outcome its request would have had alone.
 *
 * <p>Not thread-safe: one connection belongs to one event loop.
 */
public final class PgProtocol {

    private static final VarHandle INT4 = MethodHandles.byteArrayViewVarHandle(int[].class, ByteOrder.BIG_ENDIAN);
    private static final VarHandle INT8 = MethodHandles.byteArrayViewVarHandle(long[].class, ByteOrder.BIG_ENDIAN);
    private static final VarHandle INT2 = MethodHandles.byteArrayViewVarHandle(short[].class, ByteOrder.BIG_ENDIAN);

    private static final int PROTOCOL_3_0 = 196_608;
    private static final int MIN_READ = 16 * 1024;

    private enum State { NEW, AUTHENTICATING, AUTHENTICATED, PREPARING, READY, FAILED }

    private final PgUrl url;
    private final List<PgStatement> statements;
    private final byte[][] statementNames;
    private final Supplier<String> scramNonce;

    private State state = State.NEW;
    private String failure;
    private Scram scram; // set while a SCRAM exchange is under way, until the server has proved itself

    // Backend bytes: unparsed data is in[inStart, inEnd). needed = size of the message we're waiting for.
    private byte[] in = new byte[64 * 1024];
    private int inStart;
    private int inEnd;
    private int needed = 5;
    private ByteBuffer inView = ByteBuffer.wrap(in);

    // Frontend bytes waiting to go out: out[outStart, outEnd). outView's position is the caller's progress.
    private byte[] out = new byte[64 * 1024];
    private int outStart;
    private int outEnd;
    private ByteBuffer outView = ByteBuffer.wrap(out).limit(0);
    private int lengthAt = -1;      // where the open message's length goes
    private int paramCountAt = -1;  // where the open Bind's parameter count goes
    private int paramCount;

    // Requests sent and not yet answered by their ReadyForQuery, oldest first (a ring). endsGroup: a Sync follows
    // the request (false for all but the last of a group). keptLength: its Bind + Execute bytes kept for a retry.
    private PgHandler[] handlers = new PgHandler[256];
    private long[] tags = new long[256];
    private boolean[] endsGroup = new boolean[256];
    private int[] keptLength = new int[256];
    private int head;
    private int count;
    private int grouped;          // requests in the open group: Execute sent, no Sync yet
    private int requestAt = -1;   // where the open request's Bind starts in out

    // The Bind + Execute bytes of grouped requests, oldest first, until their group is done: kept[keptStart, keptEnd).
    private byte[] kept = new byte[16 * 1024];
    private int keptStart;
    private int keptEnd;

    // While a group of several waits for its ReadyForQuery: how far its replies have been looked at (past inStart),
    // and whether one of them was an error.
    private int groupScanned;
    private boolean groupFailed;

    private final PgRow row = new PgRow();
    private final PgError error = new PgError();

    public PgProtocol(PgUrl url, List<PgStatement> statements) {
        this(url, statements, Scram::randomNonce);
    }

    PgProtocol(PgUrl url, List<PgStatement> statements, Supplier<String> scramNonce) {
        this.url = url;
        this.statements = List.copyOf(statements);
        this.statementNames = new byte[statements.size()][];
        for (int i = 0; i < statementNames.length; i++) {
            statementNames[i] = statements.get(i).name().getBytes(StandardCharsets.UTF_8);
        }
        this.scramNonce = scramNonce;
    }

    /** Queues the StartupMessage. */
    public void start() {
        if (state != State.NEW) {
            throw new IllegalStateException("already started");
        }
        lengthAt = reserve(4);
        putInt(PROTOCOL_3_0);
        putCString("user");
        putCString(url.user());
        putCString("database");
        putCString(url.database());
        putCString("client_encoding");
        putCString("UTF8");
        putCString("application_name");
        putCString("feed");
        putByte(0);
        endMessage();
        state = State.AUTHENTICATING;
    }

    public boolean isReady() {
        return state == State.READY;
    }

    public boolean isFailed() {
        return state == State.FAILED;
    }

    /** Why the connection failed, or null. */
    public String failure() {
        return failure;
    }

    /** Requests sent and not yet done, the open group's included. */
    public int pending() {
        return count;
    }

    /** Requests in the open group, not yet ended by {@link #endGroup}. */
    public int grouped() {
        return grouped;
    }

    // ---- input ----

    /** A buffer to read backend bytes into (position = where to write); call {@link #process()} after. */
    public ByteBuffer readBuffer() {
        if (inStart == inEnd) {
            inStart = 0;
            inEnd = 0;
        }
        if (in.length - inEnd < MIN_READ || in.length - inStart < needed) {
            System.arraycopy(in, inStart, in, 0, inEnd - inStart);
            inEnd -= inStart;
            inStart = 0;
            if (in.length - inEnd < MIN_READ || in.length < needed) {
                in = Arrays.copyOf(in, Math.max(in.length * 2, needed + MIN_READ));
                inView = ByteBuffer.wrap(in);
            }
        }
        inView.limit(in.length).position(inEnd);
        return inView;
    }

    /** Handles every complete backend message read so far. */
    public void process() {
        inEnd = inView.position();
        while (state != State.FAILED && inEnd - inStart >= 5) {
            if (state == State.READY && count > 0 && !endsGroup[head]) {
                if (!deliverGroup()) {
                    return;
                }
                continue;
            }
            int total = 1 + (int) INT4.get(in, inStart + 1);
            if (inEnd - inStart < total) {
                needed = total;
                return;
            }
            int at = inStart;
            inStart += total;
            handle((char) in[at], at + 5, total - 5);
        }
        needed = 5;
    }

    /** Copies bytes in and processes them (for callers that read elsewhere, and tests). */
    public void receive(byte[] bytes, int off, int len) {
        while (len > 0) {
            ByteBuffer buffer = readBuffer();
            int n = Math.min(len, buffer.remaining());
            buffer.put(bytes, off, n);
            process();
            off += n;
            len -= n;
        }
    }

    private void handle(char type, int body, int length) {
        switch (type) {
            case 'D' -> {
                if (state == State.READY && count > 0) {
                    row.reset(in, body);
                    handlers[head].onRow(tags[head], row);
                }
            }
            case 'C' -> {
                if (state == State.READY && count > 0) {
                    handlers[head].onComplete(tags[head], commandRows(body, length));
                }
            }
            case 'Z' -> readyForQuery();
            case 'E' -> {
                if (state == State.READY && count > 0) {
                    error.reset(in, body);
                    handlers[head].onError(tags[head], error);
                } else {
                    error.reset(in, body);
                    fail(error.sqlState() + ": " + error.message());
                }
            }
            case 'R' -> authentication(body, length);
            default -> {
                // ParseComplete, BindComplete, ParameterStatus, BackendKeyData, NoticeResponse,
                // NotificationResponse, NegotiateProtocolVersion: nothing to do.
            }
        }
    }

    private void readyForQuery() {
        switch (state) {
            case AUTHENTICATED -> {
                for (int i = 0; i < statements.size(); i++) {
                    PgStatement s = statements.get(i);
                    beginMessage('P');
                    putCString(statementNames[i]);
                    putCString(s.sql());
                    putShort(s.paramTypes().length);
                    for (int type : s.paramTypes()) {
                        putInt(type);
                    }
                    endMessage();
                }
                beginMessage('S');
                endMessage();
                state = State.PREPARING;
            }
            case PREPARING -> state = State.READY;
            case READY -> {
                if (count == 0) {
                    fail("ReadyForQuery with no request pending");
                    return;
                }
                PgHandler handler = handlers[head];
                long tag = tags[head];
                pop();
                handler.onDone(tag);
            }
            default -> fail("unexpected ReadyForQuery during " + state);
        }
    }

    /**
     * The head request is the first of a group of several. Its replies stay unread until the group's ReadyForQuery is
     * in, since only then is it known whether the group committed. Returns false while that is still to come.
     */
    private boolean deliverGroup() {
        int at = inStart + groupScanned;
        while (true) {
            int total = inEnd - at >= 5 ? 1 + (int) INT4.get(in, at + 1) : 5;
            if (inEnd - at < total) {
                groupScanned = at - inStart;
                needed = at + total - inStart;
                return false;
            }
            char type = (char) in[at];
            at += total;
            if (type == 'E') {
                groupFailed = true;
            } else if (type == 'Z') {
                break;
            }
        }
        int from = inStart;
        inStart = at;
        boolean failed = groupFailed;
        groupScanned = 0;
        groupFailed = false;
        int size = groupSize();
        if (size < 0) {
            fail("ReadyForQuery for a group that was never sent");
        } else if (failed) {
            retryGroup(size);
        } else {
            deliverCommitted(from, at, size);
        }
        return true;
    }

    /** Requests in the head's group, or -1 if the group is still open. */
    private int groupSize() {
        for (int i = 0; i < count; i++) {
            if (endsGroup[(head + i) & (handlers.length - 1)]) {
                return i + 1;
            }
        }
        return -1;
    }

    /** The group committed: hands each request its rows and row count, then its done, in order. */
    private void deliverCommitted(int from, int to, int size) {
        int left = size;
        for (int at = from; at < to; ) {
            char type = (char) in[at];
            int body = at + 5;
            int length = (int) INT4.get(in, at + 1) - 4;
            at = body + length;
            if (type != 'D' && type != 'C') {
                continue; // BindComplete, notices, the closing ReadyForQuery
            }
            if (left == 0) {
                fail("more results than requests in a group");
                return;
            }
            if (type == 'D') {
                row.reset(in, body);
                handlers[head].onRow(tags[head], row);
            } else {
                PgHandler handler = handlers[head];
                long tag = tags[head];
                handler.onComplete(tag, commandRows(body, length));
                pop();
                left--;
                handler.onDone(tag); // may send a request: it goes behind everything in the ring
            }
        }
        if (left > 0) {
            fail("ReadyForQuery before every request in a group finished");
        }
    }

    /** The group rolled back: each of its requests goes out again as a transaction of its own. */
    private void retryGroup(int size) {
        endGroup(); // a newer group still open goes out as it is, ahead of the retries
        for (int i = 0; i < size; i++) {
            PgHandler handler = handlers[head];
            long tag = tags[head];
            putBytes(kept, keptStart, keptLength[head]);
            sync();
            pop();
            enqueue(handler, tag, true, 0);
        }
    }

    private void authentication(int body, int length) {
        int code = (int) INT4.get(in, body);
        switch (code) {
            case 0 -> {
                if (scram != null) {
                    fail("the server ended SCRAM without proving it knows the password");
                } else {
                    state = State.AUTHENTICATED;
                }
            }
            case 3 -> {
                if (requirePassword()) {
                    beginMessage('p');
                    putCString(url.password());
                    endMessage();
                }
            }
            case 5 -> {
                if (requirePassword()) {
                    byte[] salt = Arrays.copyOfRange(in, body + 4, body + 8);
                    beginMessage('p');
                    putCString(md5Password(url.user(), url.password(), salt));
                    endMessage();
                }
            }
            case 10 -> {
                if (!saslMechanisms(body + 4, body + length).contains(",SCRAM-SHA-256,")) {
                    fail("the server offers no SCRAM-SHA-256");
                } else if (requirePassword()) {
                    scram = new Scram("", url.password(), scramNonce.get());
                    byte[] first = scram.clientFirstMessage().getBytes(StandardCharsets.UTF_8);
                    beginMessage('p');
                    putCString("SCRAM-SHA-256");
                    putInt(first.length);
                    putBytes(first, 0, first.length);
                    endMessage();
                }
            }
            case 11 -> {
                if (scram == null) {
                    fail("SASLContinue outside a SCRAM exchange");
                    return;
                }
                String serverFirst = new String(in, body + 4, length - 4, StandardCharsets.UTF_8);
                try {
                    byte[] reply = scram.clientFinalMessage(serverFirst).getBytes(StandardCharsets.UTF_8);
                    beginMessage('p');
                    putBytes(reply, 0, reply.length);
                    endMessage();
                } catch (RuntimeException e) {
                    fail(e.getMessage());
                }
            }
            case 12 -> {
                if (scram == null) {
                    fail("SASLFinal outside a SCRAM exchange");
                    return;
                }
                try {
                    scram.verifyServerFinal(new String(in, body + 4, length - 4, StandardCharsets.UTF_8));
                    scram = null;
                } catch (RuntimeException e) {
                    fail(e.getMessage());
                }
            }
            default -> fail("unsupported authentication method " + code);
        }
    }

    private boolean requirePassword() {
        if (url.password() == null) {
            fail("the server wants a password and DATABASE_URL has none");
            return false;
        }
        return true;
    }

    /** The SASL mechanism names, as ",A,B," for a simple contains() check. */
    private String saslMechanisms(int from, int to) {
        StringBuilder names = new StringBuilder(",");
        int start = from;
        for (int i = from; i < to; i++) {
            if (in[i] == 0) {
                if (i == start) {
                    break;
                }
                names.append(new String(in, start, i - start, StandardCharsets.UTF_8)).append(',');
                start = i + 1;
            }
        }
        return names.toString();
    }

    private static String md5Password(String user, String password, byte[] salt) {
        try {
            MessageDigest md5 = MessageDigest.getInstance("MD5");
            String inner = HexFormat.of().formatHex(md5.digest((password + user).getBytes(StandardCharsets.UTF_8)));
            md5.update(inner.getBytes(StandardCharsets.US_ASCII));
            md5.update(salt);
            return "md5" + HexFormat.of().formatHex(md5.digest());
        } catch (NoSuchAlgorithmException e) {
            throw new IllegalStateException(e);
        }
    }

    /** The row count at the end of a command tag: "INSERT 0 1" -> 1, "SELECT 20" -> 20. */
    private long commandRows(int body, int length) {
        int end = body + length - 1; // the tag's terminating 0
        long rows = 0;
        long scale = 1;
        for (int i = end - 1; i >= body && in[i] >= '0' && in[i] <= '9'; i--) {
            rows += (in[i] - '0') * scale;
            scale *= 10;
        }
        return rows;
    }

    // ---- requests ----

    /** Starts a request on the statement at {@code statement} in the list given to the constructor. */
    public void bind(int statement) {
        if (state != State.READY) {
            throw new IllegalStateException("connection not ready: " + state);
        }
        if (paramCountAt >= 0) {
            throw new IllegalStateException("bind() while another request is open");
        }
        beginMessage('B');
        requestAt = lengthAt - 1; // the type byte
        putByte(0); // unnamed portal
        putCString(statementNames[statement]);
        putShort(1); // one format code for every parameter:
        putShort(1); // binary
        paramCountAt = reserve(2);
        paramCount = 0;
    }

    public void paramInt8(long value) {
        ensureOut(12);
        INT4.set(out, outEnd, 8);
        INT8.set(out, outEnd + 4, value);
        outEnd += 12;
        paramCount++;
    }

    /** A text parameter: UTF-8 bytes (a text value's binary format is its UTF-8 bytes). */
    public void paramText(byte[] utf8, int off, int len) {
        putInt(len);
        putBytes(utf8, off, len);
        paramCount++;
    }

    /** Ends the request (Execute + Sync), its own transaction, and queues {@code handler} for its results. */
    public void execute(PgHandler handler, long tag) {
        if (grouped > 0) {
            throw new IllegalStateException("execute() while a group is open: endGroup() first");
        }
        endRequest();
        requestAt = -1;
        sync();
        enqueue(handler, tag, true, 0);
    }

    /** Ends the request with Execute alone: it joins the open group, which {@link #endGroup} commits. */
    public void executeInGroup(PgHandler handler, long tag) {
        endRequest();
        int length = outEnd - requestAt;
        keep(requestAt, length);
        requestAt = -1;
        enqueue(handler, tag, false, length);
        grouped++;
    }

    /** Ends the open group with a Sync: its requests commit together, or not at all. Nothing if none is grouped. */
    public void endGroup() {
        if (grouped > 0) {
            sync();
            endsGroup[(head + count - 1) & (handlers.length - 1)] = true;
            grouped = 0;
        }
    }

    /** Finishes the Bind and adds the Execute. */
    private void endRequest() {
        if (paramCountAt < 0) {
            throw new IllegalStateException("execute() without bind()");
        }
        INT2.set(out, paramCountAt, (short) paramCount);
        paramCountAt = -1;
        putShort(1); // one format code for every result column:
        putShort(1); // binary
        endMessage();
        beginMessage('E');
        putByte(0); // unnamed portal
        putInt(0);  // no row limit
        endMessage();
    }

    private void sync() {
        beginMessage('S');
        endMessage();
    }

    private void enqueue(PgHandler handler, long tag, boolean ends, int kept) {
        if (count == handlers.length) {
            PgHandler[] h = new PgHandler[count * 2];
            long[] t = new long[count * 2];
            boolean[] e = new boolean[count * 2];
            int[] k = new int[count * 2];
            for (int i = 0; i < count; i++) {
                int from = (head + i) & (handlers.length - 1);
                h[i] = handlers[from];
                t[i] = tags[from];
                e[i] = endsGroup[from];
                k[i] = keptLength[from];
            }
            handlers = h;
            tags = t;
            endsGroup = e;
            keptLength = k;
            head = 0;
        }
        int slot = (head + count) & (handlers.length - 1);
        handlers[slot] = handler;
        tags[slot] = tag;
        endsGroup[slot] = ends;
        keptLength[slot] = kept;
        count++;
    }

    /** Removes the head request, and the bytes kept for it. */
    private void pop() {
        handlers[head] = null;
        keptStart += keptLength[head];
        head = (head + 1) & (handlers.length - 1);
        count--;
        if (count == 0) {
            keptStart = 0;
            keptEnd = 0;
        }
    }

    /** Keeps out[from, from + length) for a retry. */
    private void keep(int from, int length) {
        if (kept.length - keptEnd < length) {
            System.arraycopy(kept, keptStart, kept, 0, keptEnd - keptStart);
            keptEnd -= keptStart;
            keptStart = 0;
            if (kept.length - keptEnd < length) {
                kept = Arrays.copyOf(kept, Math.max(kept.length * 2, keptEnd + length));
            }
        }
        System.arraycopy(out, from, kept, keptEnd, length);
        keptEnd += length;
    }

    /** The socket is gone: every pending request gets an error (SQLSTATE 08006) and then onDone. */
    public void connectionLost(String reason) {
        fail(reason);
    }

    private void fail(String reason) {
        if (state != State.FAILED) {
            state = State.FAILED;
            failure = reason;
        }
        grouped = 0;
        groupScanned = 0;
        groupFailed = false;
        while (count > 0) {
            PgHandler handler = handlers[head];
            long tag = tags[head];
            pop();
            error.synthetic("08006", reason);
            handler.onError(tag, error);
            handler.onDone(tag);
        }
    }

    // ---- output ----

    /**
     * Bytes waiting to be sent, from position to limit. Advance the position as they are written. Fetch a fresh view
     * before every socket write: bind/execute may replace it when the buffer grows.
     */
    public ByteBuffer output() {
        syncOut();
        outView.limit(outEnd).position(outStart);
        return outView;
    }

    public boolean hasOutput() {
        syncOut();
        return outStart < outEnd;
    }

    /** Takes in how far the caller got through outView. */
    private void syncOut() {
        int sent = outView.position();
        if (sent > outStart && sent <= outEnd) {
            outStart = sent;
        }
        if (outStart == outEnd && lengthAt < 0 && paramCountAt < 0) {
            outStart = 0;
            outEnd = 0;
            outView.limit(0);
        }
    }

    private void ensureOut(int n) {
        if (out.length - outEnd >= n) {
            return;
        }
        syncOut();
        if (outStart > 0) {
            int shift = outStart;
            System.arraycopy(out, outStart, out, 0, outEnd - outStart);
            outEnd -= shift;
            outStart = 0;
            if (lengthAt >= 0) {
                lengthAt -= shift;
            }
            if (paramCountAt >= 0) {
                paramCountAt -= shift;
            }
            if (requestAt >= 0) {
                requestAt -= shift;
            }
        }
        if (out.length - outEnd < n) {
            out = Arrays.copyOf(out, Math.max(out.length * 2, outEnd + n));
            outView = ByteBuffer.wrap(out);
        }
        outView.limit(outEnd).position(outStart);
    }

    private void beginMessage(char type) {
        putByte(type);
        lengthAt = reserve(4);
    }

    private void endMessage() {
        INT4.set(out, lengthAt, outEnd - lengthAt);
        lengthAt = -1;
    }

    private int reserve(int n) {
        ensureOut(n);
        int at = outEnd;
        outEnd += n;
        return at;
    }

    private void putByte(int b) {
        ensureOut(1);
        out[outEnd++] = (byte) b;
    }

    private void putShort(int v) {
        ensureOut(2);
        INT2.set(out, outEnd, (short) v);
        outEnd += 2;
    }

    private void putInt(int v) {
        ensureOut(4);
        INT4.set(out, outEnd, v);
        outEnd += 4;
    }

    private void putBytes(byte[] bytes, int off, int len) {
        ensureOut(len);
        System.arraycopy(bytes, off, out, outEnd, len);
        outEnd += len;
    }

    private void putCString(byte[] bytes) {
        putBytes(bytes, 0, bytes.length);
        putByte(0);
    }

    private void putCString(String s) {
        putCString(s.getBytes(StandardCharsets.UTF_8));
    }
}
