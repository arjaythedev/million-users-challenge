package challenge;

/** A request that needs Postgres, and the unit of work (one Sync) it travels in. */
final class Req {
    static final int HEALTH = 0, FEED = 1, POST = 2, CREATE = 3, LIKE = 4;

    final Conn conn;
    final int kind;
    final boolean gzip;
    final boolean close;
    long id;
    long uid;
    byte[] body;
    String user;
    // results of a write
    long resId;
    long resMicros;
    boolean liked;

    Req(Conn conn, int kind, boolean gzip, boolean close) {
        this.conn = conn;
        this.kind = kind;
        this.gzip = gzip;
        this.close = close;
    }
}
