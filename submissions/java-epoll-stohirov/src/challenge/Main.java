package challenge;

import java.net.InetAddress;
import java.util.Arrays;

public final class Main {
    private static final int MAX_DB_CONNECTIONS = 512;

    private Main() {}

    public static void main(String[] args) throws Exception {
        long start = System.nanoTime();
        String secret = required("JWT_SECRET");
        Pg.Config db = Pg.Config.parse(required("DATABASE_URL"));
        InetAddress host = InetAddress.getByName(env("HOST", "0.0.0.0"));
        String extra = env("EXTRA_PORT", "");
        int[] ports = extra.isEmpty()
                ? new int[] {Integer.parseInt(env("PORT", "3000"))}
                : new int[] {Integer.parseInt(env("PORT", "3000")), Integer.parseInt(extra)};
        int loops = Integer.parseInt(env("LOOPS", String.valueOf(Runtime.getRuntime().availableProcessors())));
        int pool = Integer.parseInt(env("DB_POOL", "64"));
        int readers = Math.max(1, Math.min((pool + loops - 1) / loops, MAX_DB_CONNECTIONS / loops - 1));
        if (loops * (readers + 1) > MAX_DB_CONNECTIONS) throw new IllegalStateException("too many loops for 512 connections");

        int gzipLevel = Integer.parseInt(env("GZIP_LEVEL", "1"));
        int gzipMin = Integer.parseInt(env("GZIP_MIN", "1024"));

        boolean uring = "uring".equals(env("IO", "epoll"));
        Loop.Config cfg = new Loop.Config(readers, gzipLevel, gzipMin, uring,
                Integer.parseInt(env("BATCH_MIN", "32")), Long.parseLong(env("BATCH_MICROS", "2000")) * 1000);

        Loop[] ls = new Loop[loops];
        for (int i = 0; i < loops; i++) ls[i] = new Loop(i, db, cfg, new Jwt(secret), host, ports, start);
        System.err.println("listening on " + host.getHostAddress() + " ports " + Arrays.toString(ports) + ": " + loops
                + " loops, each with " + readers + " read and 1 write connection to " + db.host() + ":" + db.port()
                + "; gzip " + (gzipLevel > 0 ? "from " + gzipMin + " bytes" : "off")
                + "; batching " + (cfg.batchNanos() > 0 ? "under " + cfg.batchMin() + " events, " + cfg.batchNanos() / 1000 + " us" : "off"));
        Thread[] threads = new Thread[loops];
        for (int i = 0; i < loops; i++) {
            threads[i] = new Thread(ls[i], "loop-" + i);
            threads[i].setUncaughtExceptionHandler((t, e) -> {
                System.err.println(t.getName() + " died: " + e);
                e.printStackTrace();
                Runtime.getRuntime().halt(1);
            });
            threads[i].start();
        }
        for (Thread t : threads) t.join();
    }

    private static String env(String name, String fallback) {
        String v = System.getenv(name);
        return v == null || v.isEmpty() ? fallback : v;
    }

    private static String required(String name) {
        String v = System.getenv(name);
        if (v == null || v.isEmpty()) throw new IllegalStateException(name + " is not set");
        return v;
    }
}
