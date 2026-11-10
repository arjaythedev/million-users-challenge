package feed.pg;

/**
 * Receives the results of one pipelined request, identified by the caller's {@code tag}. Handlers are long-lived
 * objects (one per request kind), so nothing is allocated per request. For every request, {@link #onDone} is called
 * exactly once and last: after it, the request's implicit transaction has committed (or rolled back, if
 * {@link #onError} was called).
 */
public interface PgHandler {

    /** One result row. {@code row} is only valid during the call. */
    default void onRow(long tag, PgRow row) {
    }

    /** The statement finished; {@code rows} is the count from the command tag (e.g. 1 for {@code INSERT 0 1}). */
    default void onComplete(long tag, long rows) {
    }

    /** The statement failed. {@code error} is only valid during the call. */
    default void onError(long tag, PgError error) {
    }

    /** The request's {@code Sync} was answered with {@code ReadyForQuery}. */
    void onDone(long tag);
}
