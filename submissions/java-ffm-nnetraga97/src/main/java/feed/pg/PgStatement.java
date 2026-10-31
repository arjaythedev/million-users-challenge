package feed.pg;

/** A statement prepared once per connection. Parameter types are Postgres type OIDs (e.g. 20 = int8, 25 = text). */
public record PgStatement(String name, String sql, int... paramTypes) {

    public static final int INT8 = 20;
    public static final int TEXT = 25;
}
