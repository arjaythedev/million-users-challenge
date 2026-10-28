package feed.net;

import java.io.IOException;

/**
 * One worker's sockets and readiness events. A socket is an int id (on Linux, the fd itself), with no object per
 * connection. Level-triggered: a socket with unread data is reported again on the next poll. Not thread-safe: one Io
 * per worker thread.
 */
public interface Io extends AutoCloseable {

    interface Handler {
        /**
         * A new client socket was accepted (already non-blocking, TCP_NODELAY, watched for reads). The epoll backend
         * accepts a client only once its first bytes are in, or after a few silent seconds (TCP_DEFER_ACCEPT).
         */
        void accepted(int id);

        /** Data, end of stream or an error is waiting: call {@link #read}. */
        void readable(int id);

        /** The socket can take more bytes (only while {@link #watchWrites} is on). */
        void writable(int id);
    }

    /** Listens on host:port with SO_REUSEPORT; accepted sockets are reported to poll's handler. Returns the port. */
    int listen(String host, int port) throws IOException;

    /**
     * Starts a non-blocking connect and returns at once. The socket is reported writable (or readable, on failure)
     * when the attempt ends; then call {@link #finishConnect}. A failure seen immediately throws here.
     */
    int connect(String host, int port) throws IOException;

    /** Completes a connect after its readiness event: throws if it failed, else watches the socket for reads. */
    void finishConnect(int id) throws IOException;

    /** Stops read events for {@code id} (until it is closed); writes still work. For a client that half-closed. */
    void pauseReads(int id);

    /**
     * Waits up to {@code timeoutMillis} for events and reports them. Returns how many sockets had events (0 when it
     * timed out or was woken up).
     */
    int poll(int timeoutMillis, Handler handler) throws IOException;

    /** Reads; returns the byte count, 0 when nothing is available, -1 at end of stream or on error. */
    int read(int id, byte[] b, int off, int len);

    /** Writes; returns the byte count (0 when the socket buffer is full), or -1 on error. */
    int write(int id, byte[] b, int off, int len);

    /** Turns write-readiness events for {@code id} on or off. */
    void watchWrites(int id, boolean on);

    void close(int id);

    /** Interrupts a poll in progress from another thread. Harmless after close. */
    void wakeup();

    /** Closes every socket. Idempotent. */
    @Override
    void close();

    /** The best Io for this platform: epoll on Linux, java.nio elsewhere (or when IO=nio). */
    static Io open() throws IOException {
        return backend().equals("epoll") ? EpollIo.open() : new NioIo();
    }

    /** "epoll" or "nio": what {@link #open()} returns. */
    static String backend() {
        String choice = System.getenv().getOrDefault("IO", "");
        return !choice.equals("nio") && System.getProperty("os.name").equals("Linux") ? "epoll" : "nio";
    }
}
