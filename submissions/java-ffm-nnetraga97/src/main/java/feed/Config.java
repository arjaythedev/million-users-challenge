package feed;

import java.nio.charset.StandardCharsets;
import java.util.Map;

import feed.pg.PgUrl;

/**
 * Server settings, from environment variables (SPEC.md's runtime contract plus optional tuning).
 *
 * @param extraPort      a second port to serve on, or -1 for none
 * @param threads        worker threads: one event loop each
 * @param readConns      Postgres connections for reads, over all workers
 * @param writeConns     Postgres connections for writes, over all workers
 * @param pipelineDepth  queries in flight per Postgres connection before more wait in the worker
 * @param gzipLevel      compression level for large responses; 0 turns gzip off
 * @param gzipMin        smallest response body that is compressed, in bytes
 * @param batchMin       a poll that reports fewer sockets than this is followed by a pause of {@code batchMicros}
 * @param batchMicros    that pause, in microseconds; 0 turns it off
 */
public record Config(PgUrl db, byte[] jwtSecret, String host, int port, int extraPort, int threads, int readConns,
        int writeConns, int pipelineDepth, int gzipLevel, int gzipMin, int batchMin, int batchMicros) {

    /** The challenge's limit on Postgres connections. */
    public static final int MAX_DB_CONNECTIONS = 512;

    public Config {
        if (threads < 1 || readConns < threads || writeConns < threads) {
            throw new IllegalArgumentException("need a thread, and a read and a write connection per thread");
        }
        if (readConns + writeConns > MAX_DB_CONNECTIONS) {
            throw new IllegalArgumentException("at most " + MAX_DB_CONNECTIONS + " Postgres connections in total");
        }
        if (pipelineDepth < 1 || gzipLevel < 0 || gzipMin < 0) {
            throw new IllegalArgumentException("PG_PIPELINE must be at least 1; GZIP_LEVEL and GZIP_MIN at least 0");
        }
        if (batchMin < 0 || batchMicros < 0) {
            throw new IllegalArgumentException("BATCH_MIN and BATCH_MICROS must be at least 0");
        }
    }

    /**
     * Reads DATABASE_URL and JWT_SECRET (required), HOST, PORT, EXTRA_PORT, THREADS (default: the cores this process
     * may use), DB_READ_CONNS and DB_WRITE_CONNS (64 each, at least one per thread), PG_PIPELINE (64), GZIP_LEVEL (1),
     * GZIP_MIN (512), BATCH_MIN (32) and BATCH_MICROS (2000).
     */
    public static Config fromEnv(Map<String, String> env) {
        int threads = integer(env, "THREADS", Runtime.getRuntime().availableProcessors());
        return new Config(
                PgUrl.parse(required(env, "DATABASE_URL")),
                required(env, "JWT_SECRET").getBytes(StandardCharsets.UTF_8),
                env.getOrDefault("HOST", "0.0.0.0"),
                integer(env, "PORT", 3000),
                integer(env, "EXTRA_PORT", -1),
                threads,
                Math.max(threads, integer(env, "DB_READ_CONNS", 64)),
                Math.max(threads, integer(env, "DB_WRITE_CONNS", 64)),
                integer(env, "PG_PIPELINE", 64),
                integer(env, "GZIP_LEVEL", 1),
                integer(env, "GZIP_MIN", 512),
                integer(env, "BATCH_MIN", 32),
                integer(env, "BATCH_MICROS", 2000));
    }

    private static String required(Map<String, String> env, String name) {
        String value = env.get(name);
        if (value == null || value.isEmpty()) {
            throw new IllegalArgumentException(name + " is not set");
        }
        return value;
    }

    private static int integer(Map<String, String> env, String name, int fallback) {
        String value = env.get(name);
        if (value == null || value.isBlank()) {
            return fallback;
        }
        try {
            return Integer.parseInt(value.trim());
        } catch (NumberFormatException e) {
            throw new IllegalArgumentException(name + " must be an integer, not '" + value + "'");
        }
    }
}
