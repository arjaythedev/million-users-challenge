package challenge;

/** Requests sent to Postgres together, ending in one Sync (so in one transaction when they are writes). */
final class Unit {
    final Req[] rs;
    final int n;
    /** Index of the request whose CommandComplete is next. */
    int cmd;
    boolean err;
    String sqlState;

    Unit(Req[] rs, int n) {
        this.rs = rs;
        this.n = n;
    }

    static Unit of(Req r) {
        return new Unit(new Req[] {r}, 1);
    }
}
