package feed.server;

import java.io.IOException;

import feed.Config;
import feed.net.Io;

/**
 * Runs one {@link Worker} per thread. Each worker has its own SO_REUSEPORT listener on each port (the kernel spreads
 * new connections over them) and its own share of the Postgres connections, so workers share nothing.
 */
public final class Server implements AutoCloseable {

    private static final long JOIN_MILLIS = 5_000;

    private final Worker[] workers;
    private final Thread[] threads;
    private final int port;
    private final int extraPort;

    private Server(Worker[] workers, Thread[] threads, int port, int extraPort) {
        this.workers = workers;
        this.threads = threads;
        this.port = port;
        this.extraPort = extraPort;
    }

    /** Opens the listeners and Postgres connections and starts the worker threads. */
    public static Server start(Config config) throws IOException {
        long startNanos = System.nanoTime();
        int n = config.threads();
        Worker[] workers = new Worker[n];
        Thread[] threads = new Thread[n];
        int port = config.port();
        int extraPort = config.extraPort();
        for (int i = 0; i < n; i++) {
            Io io = Io.open();
            workers[i] = new Worker(io, config, startNanos, share(config.readConns(), n, i),
                    share(config.writeConns(), n, i));
            port = io.listen(config.host(), port); // port 0 picks a free port, which the other workers then share
            if (extraPort >= 0) {
                extraPort = io.listen(config.host(), extraPort);
            }
        }
        for (int i = 0; i < n; i++) {
            threads[i] = new Thread(workers[i], "worker-" + i);
            threads[i].start();
        }
        return new Server(workers, threads, port, extraPort);
    }

    /** Worker {@code i}'s share of {@code total} connections. */
    private static int share(int total, int workers, int i) {
        return Math.max(1, total / workers + (i < total % workers ? 1 : 0));
    }

    public int port() {
        return port;
    }

    /** The second port, or -1 if there is none. */
    public int extraPort() {
        return extraPort;
    }

    /** The gzip compressor in use (every worker has the same kind), or "off". */
    public String gzip() {
        return workers[0].gzipDescription();
    }

    /** Waits until every worker has stopped. */
    public void awaitTermination() throws InterruptedException {
        for (Thread t : threads) {
            t.join();
        }
    }

    /** Stops the workers and closes their sockets. */
    @Override
    public void close() {
        for (Worker w : workers) {
            w.stop();
        }
        for (int i = 0; i < workers.length; i++) {
            try {
                threads[i].join(JOIN_MILLIS);
            } catch (InterruptedException e) {
                Thread.currentThread().interrupt();
                return;
            }
            if (!threads[i].isAlive()) {
                workers[i].closeIo();
            }
        }
    }
}
