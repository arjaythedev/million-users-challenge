package challenge;

/** One client connection. An idle one owns no buffers: `pend` and `rest` exist only while something is partial. */
final class Conn {
    final int fd;
    final Loop owner;
    boolean closed;
    /** A request of this connection is waiting on Postgres; later pipelined bytes wait in `pend`. */
    boolean waiting;
    boolean closeAfter;
    boolean sent100;
    /** Unprocessed request bytes: an incomplete request, or pipelined ones behind a waiting request. */
    byte[] pend;
    int pendLen;
    /** Response bytes the socket did not take yet. */
    byte[] rest;
    int restOff;

    Conn(int fd, Loop owner) {
        this.fd = fd;
        this.owner = owner;
    }
}
