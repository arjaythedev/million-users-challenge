package feed.db;

import feed.net.FdMap;
import feed.net.Io;
import feed.pg.PgHandler;
import feed.pg.PgProtocol;
import feed.pg.PgUrl;

/**
 * A worker's Postgres connections: reads and writes on separate connections (a feed never waits behind a commit).
 * A read is pipelined on the ready connection with the fewest in flight, at most {@code pipelineDepth} per
 * connection. Writes use group commit: a write connection has at most one transaction in flight, and the writes that
 * arrive meanwhile wait, then go out together as its next transaction (one Sync, one commit). Queries with no room
 * wait in a FIFO, oldest first; past {@link #MAX_QUEUED} waiting, new ones are turned away rather than queued
 * without bound.
 */
public final class Database {

    /** Queries that may wait for room, per worker and kind (reads, writes). */
    public static final int MAX_QUEUED = 65_536;

    /** Most writes in one transaction. */
    static final int MAX_GROUP = 256;

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
        if (!anyReady(write ? writes : reads)) {
            return Submitted.UNAVAILABLE;
        }
        WaitQueue queue = write ? writeQueue : readQueue;
        DbConnection c = queue.isEmpty() ? withRoom(write) : null; // older queries go first
        if (c == null) {
            if (queue.size() >= MAX_QUEUED) {
                return Submitted.REJECTED;
            }
            queue.add(tag, query);
            return Submitted.QUEUED;
        }
        send(c, tag, query, write);
        return Submitted.SENT;
    }

    /** Sends queued queries, oldest first, while connections have room. */
    public void resumeQueued() {
        resume(readQueue, false);
        resume(writeQueue, true);
    }

    /** Ends the open write groups and writes every connection's queued protocol output. */
    public void flush() {
        for (DbConnection c : all) {
            if (c.grouped() > 0) {
                c.endGroup();
            }
            if (c.hasOutput()) {
                c.flush();
            }
        }
    }

    private void resume(WaitQueue queue, boolean write) {
        while (!queue.isEmpty()) {
            long tag = queue.headTag();
            int query = queue.headQuery();
            if (!requests.isWaiting(tag)) {
                queue.removeHead(); // its client left
                continue;
            }
            if (!anyReady(write ? writes : reads)) {
                queue.removeHead();
                requests.unavailable(tag, query);
                continue;
            }
            DbConnection c = withRoom(write);
            if (c == null) {
                return;
            }
            queue.removeHead();
            send(c, tag, query, write);
        }
    }

    private void send(DbConnection c, long tag, int query, boolean write) {
        requests.bind(c.protocol(), query, tag);
        PgHandler handler = c.handlers().handler(query);
        if (write) {
            c.protocol().executeInGroup(handler, tag);
        } else {
            c.protocol().execute(handler, tag);
        }
        c.sent();
    }

    /** The connection a query can go on now, or null if it must wait. */
    private DbConnection withRoom(boolean write) {
        return write ? groupable() : leastBusyRead();
    }

    private DbConnection leastBusyRead() {
        DbConnection best = null;
        for (DbConnection c : reads) {
            if (c.isReady() && c.pending() < pipelineDepth && (best == null || c.pending() < best.pending())) {
                best = c;
            }
        }
        return best;
    }

    /**
     * A write connection with no transaction in flight, preferring one already gathering a group, so writes that
     * arrive together share a commit. Null while every one is waiting for a commit.
     */
    private DbConnection groupable() {
        DbConnection best = null;
        for (DbConnection c : writes) {
            if (c.isReady() && c.pending() == c.grouped() && c.grouped() < MAX_GROUP
                    && (best == null || c.grouped() > best.grouped())) {
                best = c;
            }
        }
        return best;
    }

    private static boolean anyReady(DbConnection[] connections) {
        for (DbConnection c : connections) {
            if (c.isReady()) {
                return true;
            }
        }
        return false;
    }
}
