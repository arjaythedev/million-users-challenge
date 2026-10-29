package challenge;

final class Conn {
    final int fd;
    byte[] left;
    byte[] pending;
    int pendingFrom;
    boolean closeAfterWrite, closed;
    boolean gzip;

    boolean recvArmed, sending, pollArmed, released;
    long sendAt;
    int sendLen;

    boolean waiting;
    byte op;
    boolean last;
    int rows;
    long id;
    long userId, ts;
    byte[] text, user;

    Conn(int fd) { this.fd = fd; }
}
