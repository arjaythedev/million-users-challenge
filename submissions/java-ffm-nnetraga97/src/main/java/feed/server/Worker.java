package feed.server;

import java.io.IOException;
import java.util.concurrent.locks.LockSupport;

import feed.Config;
import feed.api.FeedApi;
import feed.auth.JwtVerifier;
import feed.db.Database;
import feed.http.Connections;
import feed.http.Gzip;
import feed.net.Io;

/**
 * One event loop thread: its own listeners, client connections and Postgres connections, shared with no other
 * thread, so the request path needs no locks.
 */
final class Worker implements Runnable, Io.Handler {

    private final Io io;
    private final Gzip gzip; // null: gzip off
    private final Connections connections;
    private final Database database;
    private final int batchMin;
    private final long batchNanos;
    private volatile boolean running = true;

    Worker(Io io, Config config, long startNanos, int readConnections, int writeConnections) {
        this.io = io;
        this.gzip = config.gzipLevel() > 0 ? Gzip.open(config.gzipLevel()) : null;
        this.connections = new Connections(io, gzip, config.gzipMin());
        this.database = new Database(io, config.db(), readConnections, writeConnections, config.pipelineDepth());
        this.batchMin = config.batchMin();
        this.batchNanos = config.batchMicros() * 1000L;
        FeedApi api = new FeedApi(connections, database, new JwtVerifier(config.jwtSecret()), startNanos);
        connections.start(api);
        database.start(api);
    }

    /** Asks the loop to end (from another thread). The sockets stay open until {@link #closeIo}. */
    void stop() {
        running = false;
        io.wakeup();
    }

    /** Closes the sockets, once the worker's thread has ended (so no wakeup can race the close). */
    void closeIo() {
        io.close();
        if (gzip != null) {
            gzip.close();
        }
    }

    /** The compressor in use, as "libdeflate level 1", or "off". */
    String gzipDescription() {
        return gzip == null ? "off" : gzip.name() + " level " + gzip.level();
    }

    @Override
    public void run() {
        try {
            while (running) {
                int sockets = io.poll(database.maintain(System.nanoTime()), this);
                database.resumeQueued();
                database.flush(); // one write per Postgres connection per loop: pipelining
                // A quiet pass: responses and queries are out, so pause before the next poll and let more arrive.
                // Fewer, fuller wake-ups cost less kernel time per request; only reading waits.
                if (sockets > 0 && sockets < batchMin && batchNanos > 0) {
                    LockSupport.parkNanos(batchNanos);
                }
            }
        } catch (IOException | RuntimeException e) {
            System.err.println("worker stopped: " + e);
            e.printStackTrace();
            // Release the listeners so the kernel stops sending this worker's share of connections here.
            closeIo();
        }
    }

    @Override
    public void accepted(int fd) {
        connections.accepted(fd);
    }

    @Override
    public void readable(int fd) {
        if (!database.readable(fd)) {
            connections.readable(fd);
        }
    }

    @Override
    public void writable(int fd) {
        if (!database.writable(fd)) {
            connections.writable(fd);
        }
    }
}
