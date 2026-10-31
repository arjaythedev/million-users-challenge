package challenge;

import java.net.InetAddress;
import java.net.URI;
import java.nio.charset.StandardCharsets;

/** Settings from the environment (SPEC.md runtime contract) plus optional tuning knobs. */
final class Cfg {
    final byte[] ip;
    final int[] ports;
    final byte[] secret;
    final String pgHost;
    final int pgPort;
    final String pgUser;
    final String pgPass;
    final String pgDb;
    final int loops;
    final int readers;
    final int gzipLevel;
    final int gzipMin;
    /** A wake-up that delivers fewer than batchMin events is followed by a pause of batchMicros (0 = off). */
    final int batchMin;
    final int batchMicros;

    private Cfg(byte[] ip, int[] ports, byte[] secret, URI db, int loops, int readers, int gzipLevel, int gzipMin,
            int batchMin, int batchMicros) {
        this.ip = ip;
        this.ports = ports;
        this.secret = secret;
        this.pgHost = db.getHost();
        this.pgPort = db.getPort() > 0 ? db.getPort() : 5432;
        String info = db.getUserInfo();
        int colon = info == null ? -1 : info.indexOf(':');
        this.pgUser = info == null ? "postgres" : colon < 0 ? info : info.substring(0, colon);
        this.pgPass = colon < 0 ? "" : info.substring(colon + 1);
        String path = db.getPath();
        this.pgDb = path == null || path.length() <= 1 ? pgUser : path.substring(1);
        this.loops = loops;
        this.readers = readers;
        this.gzipLevel = gzipLevel;
        this.gzipMin = gzipMin;
        this.batchMin = batchMin;
        this.batchMicros = batchMicros;
    }

    static Cfg fromEnv() throws Exception {
        int loops = intEnv("LOOPS", Runtime.getRuntime().availableProcessors());
        int pool = intEnv("DB_POOL", 64);
        int port = intEnv("PORT", 80);
        int extra = intEnv("EXTRA_PORT", 0);
        byte[] ip = InetAddress.getByName(env("HOST", "0.0.0.0")).getAddress();
        return new Cfg(ip, extra > 0 ? new int[] {port, extra} : new int[] {port},
                required("JWT_SECRET").getBytes(StandardCharsets.UTF_8), new URI(required("DATABASE_URL")), loops,
                Math.max(1, (pool + loops - 1) / loops), intEnv("GZIP_LEVEL", 1), intEnv("GZIP_MIN", 1024),
                intEnv("BATCH_MIN", 32), intEnv("BATCH_MICROS", 2000));
    }

    private static String required(String name) {
        String v = System.getenv(name);
        if (v == null || v.isEmpty()) throw new IllegalStateException(name + " is not set");
        return v;
    }

    private static String env(String name, String def) {
        String v = System.getenv(name);
        return v == null || v.isEmpty() ? def : v;
    }

    private static int intEnv(String name, int def) {
        String v = System.getenv(name);
        return v == null || v.isEmpty() ? def : Integer.parseInt(v);
    }
}
