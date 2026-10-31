package challenge;

/** Starts one event loop per core. */
public final class Main {
    private Main() {}

    public static void main(String[] args) throws Exception {
        Cfg cfg = Cfg.fromEnv();
        Thread[] threads = new Thread[cfg.loops];
        for (int i = 0; i < cfg.loops; i++) {
            threads[i] = new Thread(new Loop(cfg), "loop-" + i);
            threads[i].start();
        }
        System.err.println("server up: " + cfg.loops + " loops, " + cfg.readers + " readers each");
        for (Thread t : threads) t.join();
    }
}
