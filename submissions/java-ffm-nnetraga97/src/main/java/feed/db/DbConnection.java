package feed.db;

import java.io.IOException;
import java.nio.ByteBuffer;

import feed.net.FdMap;
import feed.net.Io;
import feed.pg.PgProtocol;
import feed.pg.PgUrl;

/**
 * One Postgres connection of a worker: a non-blocking socket driving a {@link PgProtocol}. When the socket fails, or
 * the connection doesn't become ready in time, or a query gets no response for {@link #STALL_NANOS}, it fails its
 * pending queries (writes are never retried) and reconnects after a backoff (50 ms, doubling to 1 s).
 */
final class DbConnection {

    private static final long STARTUP_TIMEOUT_NANOS = 5_000_000_000L; // connect, authenticate, prepare
    private static final long STALL_NANOS = 30_000_000_000L;
    private static final long FIRST_BACKOFF_NANOS = 50_000_000L;
    private static final long MAX_BACKOFF_NANOS = 1_000_000_000L;

    private final Io io;
    private final PgUrl url;
    private final FdMap fds;
    private final int index;
    private final boolean forWrites;
    private final Database.ResultHandlers handlers;

    private PgProtocol protocol;
    private int fd = -1;
    private boolean connecting;
    private long startupDeadline;
    private long lastProgress; // last read, or when a query was sent to an idle connection
    private boolean watchingWrites;
    private long retryAt;
    private long backoffNanos = FIRST_BACKOFF_NANOS;

    /** {@code fds} maps this connection's socket to {@code index} while it is open. */
    DbConnection(Io io, PgUrl url, FdMap fds, int index, boolean forWrites, Database.ResultHandlers handlers) {
        this.io = io;
        this.url = url;
        this.fds = fds;
        this.index = index;
        this.forWrites = forWrites;
        this.handlers = handlers;
    }

    PgProtocol protocol() {
        return protocol;
    }

    Database.ResultHandlers handlers() {
        return handlers;
    }

    /** Ready for queries: connected, authenticated and its statements prepared. */
    boolean isReady() {
        return fd >= 0 && protocol.isReady();
    }

    int pending() {
        return protocol.pending();
    }

    /** Writes in the open group (sent with the next {@link #endGroup}). */
    int grouped() {
        return fd >= 0 ? protocol.grouped() : 0;
    }

    void endGroup() {
        if (fd >= 0) {
            protocol.endGroup();
        }
    }

    /** Down or not ready yet: the event loop should wake up soon to retry or time it out. */
    boolean needsAttention() {
        return fd < 0 || !protocol.isReady();
    }

    /** Reconnects once the backoff has passed; drops a connection that won't start up or stopped answering. */
    void maintain(long now) {
        if (fd < 0) {
            if (now - retryAt >= 0) {
                connect();
            }
        } else if (!protocol.isReady()) {
            if (now - startupDeadline >= 0) {
                lost("startup timed out");
            }
        } else if (protocol.pending() > 0 && now - lastProgress >= STALL_NANOS) {
            lost("no response for " + STALL_NANOS / 1_000_000_000L + " s");
        }
    }

    /** Call after queueing a query: a connection that was idle starts its stall clock now. */
    void sent() {
        if (protocol.pending() == 1) {
            lastProgress = System.nanoTime();
        }
    }

    /** Starts a non-blocking connect; the socket's first event finishes it. */
    void connect() {
        try {
            fd = io.connect(url.host(), url.port());
        } catch (IOException e) {
            retryLater("connect: " + e.getMessage());
            return;
        }
        fds.put(fd, index);
        connecting = true;
        watchingWrites = false;
        startupDeadline = System.nanoTime() + STARTUP_TIMEOUT_NANOS;
        protocol = new PgProtocol(url, Queries.ALL); // not ready (so never chosen) until its startup completes
    }

    void readable() {
        if (connecting) {
            connected(); // a failed connect is reported as readable (EPOLLERR / EPOLLHUP)
            return;
        }
        ByteBuffer buffer = protocol.readBuffer();
        int n = io.read(fd, buffer.array(), buffer.arrayOffset() + buffer.position(), buffer.remaining());
        if (n < 0) {
            lost("server closed the connection");
            return;
        }
        if (n == 0) {
            return;
        }
        lastProgress = System.nanoTime();
        buffer.position(buffer.position() + n);
        boolean wasReady = protocol.isReady();
        protocol.process();
        if (protocol.isFailed()) {
            lost(protocol.failure());
        } else if (!wasReady && protocol.isReady()) {
            backoffNanos = FIRST_BACKOFF_NANOS;
        }
    }

    void writable() {
        if (connecting) {
            connected();
        } else {
            flush();
        }
    }

    /** Writes whatever the protocol has queued, watching for writability while the socket is full. */
    void flush() {
        while (fd >= 0 && protocol.hasOutput()) {
            ByteBuffer out = protocol.output();
            int n = io.write(fd, out.array(), out.arrayOffset() + out.position(), out.remaining());
            if (n < 0) {
                lost("write failed");
                return;
            }
            if (n == 0) {
                watchWrites(true);
                return;
            }
            out.position(out.position() + n);
        }
        if (fd >= 0) {
            watchWrites(false);
        }
    }

    boolean hasOutput() {
        return fd >= 0 && protocol.hasOutput();
    }

    /** The connection is gone: its pending queries fail (their clients get a 500) and it reconnects later. */
    void lost(String reason) {
        if (fd >= 0) {
            io.close(fd);
            fds.remove(fd);
            fd = -1;
        }
        connecting = false;
        protocol.connectionLost(reason);
        retryLater(reason);
    }

    private void connected() {
        try {
            io.finishConnect(fd);
        } catch (IOException e) {
            lost("connect: " + e.getMessage());
            return;
        }
        connecting = false;
        protocol.start();
        flush();
    }

    private void watchWrites(boolean on) {
        if (watchingWrites != on) {
            io.watchWrites(fd, on);
            watchingWrites = on;
        }
    }

    private void retryLater(String reason) {
        System.err.println("postgres connection (" + (forWrites ? "writes" : "reads") + ") down: " + reason
                + "; retrying in " + backoffNanos / 1_000_000 + " ms");
        retryAt = System.nanoTime() + backoffNanos;
        backoffNanos = Math.min(backoffNanos * 2, MAX_BACKOFF_NANOS);
    }
}
