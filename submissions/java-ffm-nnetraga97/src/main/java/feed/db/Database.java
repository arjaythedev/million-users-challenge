package feed.db;

import feed.net.FdMap;
import feed.net.Io;
import feed.pg.PgHandler;
import feed.pg.PgProtocol;
import feed.pg.PgUrl;

/**
 * A worker's Postgres connections: reads and writes on separate connections (a feed never waits behind a commit),
 * each query pipelined on the ready connection with the fewest in flight. A connection takes at most
 * {@code pipelineDepth} queries; past that, queries wait in a FIFO until one has room, and past
 * {@link #MAX_QUEUED} waiting queries new ones are turned away rather than queued without bound.
 */
public final class Database {

    /** Queries that may wait for room, per worker and kind (reads, writes). */
    public static final int MAX_QUEUED = 65_536;

    public enum Submitted {
        SENT,
        /** Every connection is at the pipeline cap, or older queries are waiting: it waits in the FIFO. */
        QUEUED,
        /** The FIFO is full. */
        REJECTED,
        /** No connection is up. */
        UNAVAILABLE
    }

    /** The result handlers of one connection (a response can span reads, so handlers aren't shared). */
    public interface ResultHandlers {
        PgHandler handler(int query);
    }

    /** What the database needs from the requests it runs. */
    public interface Requests {
        ResultHandlers newResultHandlers();

        /** Writes the Bind for {@code query} with the parameters of request {@code tag}. */
        void bind(PgProtocol protocol, int query, long tag);

        /** Whether request {@code tag} is still waiting for its result (its client may have left). */
        boolean isWaiting(long tag);

        /** A queued query found no connection up: answer its request. */
        void unavailable(long tag, int query);
    }

    private final Io io;
    private final PgUrl url;
    private final int readConnections;
    private final int writeConnections;
    private final int pipelineDepth;
    private final FdMap fds = new FdMap(64);
    private final WaitQueue readQueue = new WaitQueue();
    private final WaitQueue writeQueue = new WaitQueue();
    private Requests requests;
    private DbConnection[] all;
    private DbConnection[] reads;
    private DbConnection[] writes;
    private boolean resuming;
    private long resumingTag;

    public Database(Io io, PgUrl url, int readConnections, int writeConnections, int pipelineDepth) {
        this.io = io;
        this.url = url;
        this.readConnections = readConnections;
        this.writeConnections = writeConnections;
        this.pipelineDepth = pipelineDepth;
    }

    /** Creates the connections and starts connecting them; their startup runs in the event loop. */
    public void start(Requests requests) {
        this.requests = requests;
        all = new DbConnection[readConnections + writeConnections];
        reads = new DbConnection[readConnections];
        writes = new DbConnection[writeConnections];
        for (int i = 0; i < all.length; i++) {
            boolean forWrites = i >= readConnections;
            all[i] = new DbConnection(io, url, fds, i, forWrites, requests.newResultHandlers());
            if (forWrites) {
                writes[i - readConnections] = all[i];
            } else {
                reads[i] = all[i];
            }
            all[i].connect();
        }
    }

    /** Reconnects and times out connections; returns how long the event loop may sleep. */
    public int maintain(long now) {
        boolean attention = false;
        for (DbConnection c : all) {
            c.maintain(now);
            attention |= c.needsAttention();
        }
        return attention ? 20 : 1000;
    }

    /** Handles a read event if {@code fd} is one of these connections. */
    public boolean readable(int fd) {
        int i = fds.get(fd);
        if (i < 0) {
            return false;
        }
        try {
            all[i].readable();
        } catch (RuntimeException e) {
            System.err.println("postgres connection error: " + e);
            e.printStackTrace();
            all[i].lost(e.toString());
        }
        return true;
    }

    /** Handles a write event if {@code fd} is one of these connections. */
    public boolean writable(int fd) {
        int i = fds.get(fd);
        if (i < 0) {
            return false;
        }
        all[i].writable();
        return true;
    }

    /** Sends the query for request {@code tag}, or queues it, or says no connection is up. */
    public Submitted submit(long tag, int query) {
        boolean write = Queries.isWrite(query);
        DbConnection c = leastBusy(write ? writes : reads);
        if (c == null) {
            return Submitted.UNAVAILABLE;
        }
        WaitQueue queue = write ? writeQueue : readQueue;
        boolean olderWaiting = !queue.isEmpty() && !(resuming && resumingTag == tag);
        if (c.pending() >= pipelineDepth || olderWaiting) {
            if (queue.size() >= MAX_QUEUED) {
                return Submitted.REJECTED;
            }
            queue.add(tag, query);
            return Submitted.QUEUED;
        }
        requests.bind(c.protocol(), query, tag);
        c.protocol().execute(c.handlers().handler(query), tag);
        c.sent();
        return Submitted.SENT;
    }

    /** Sends queued queries, oldest first, while connections have room. */
    public void resumeQueued() {
        resume(readQueue, reads);
        resume(writeQueue, writes);
    }

    /** Writes every connection's queued protocol output. */
    public void flush() {
        for (DbConnection c : all) {
            if (c.hasOutput()) {
                c.flush();
            }
        }
    }

    private void resume(WaitQueue queue, DbConnection[] connections) {
        while (!queue.isEmpty()) {
            long tag = queue.headTag();
            if (!requests.isWaiting(tag)) {
                queue.removeHead(); // its client left
                continue;
            }
            DbConnection c = leastBusy(connections);
            if (c != null && c.pending() >= pipelineDepth) {
                return;
            }
            int query = queue.headQuery();
            queue.removeHead();
            resuming = true;
            resumingTag = tag;
            Submitted result = submit(tag, query);
            resuming = false;
            if (result == Submitted.UNAVAILABLE) {
                requests.unavailable(tag, query);
            }
        }
    }

    private static DbConnection leastBusy(DbConnection[] connections) {
        DbConnection best = null;
        for (DbConnection c : connections) {
            if (c.isReady() && (best == null || c.pending() < best.pending())) {
                best = c;
            }
        }
        return best;
    }
}
