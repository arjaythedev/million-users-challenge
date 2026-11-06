package challenge;

import java.util.ArrayList;
import java.util.Collections;
import java.util.List;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.ForkJoinPool;
import java.util.concurrent.ScheduledFuture;
import java.util.concurrent.TimeUnit;

/** JDK-8370887: cancellation must not leave an earlier timer behind later deadlines. */
public final class DelayedTaskOrderCheck {
    public static void main(String[] args) throws Exception {
        long[] delays = {400, 900, 800, 700, 600, 430, 420, 310, 500, 200};
        var actual = Collections.synchronizedList(new ArrayList<Long>());
        var done = new CountDownLatch(9);
        var tasks = new ArrayList<ScheduledFuture<?>>();
        var pool = new ForkJoinPool(1);
        // Reflection keeps this JDK 25 check compilable with the application's Java 21 target.
        var schedule = ForkJoinPool.class.getMethod("schedule", Runnable.class, long.class, TimeUnit.class);
        try {
            for (long delay : delays) {
                Runnable task = () -> { actual.add(delay); done.countDown(); };
                tasks.add((ScheduledFuture<?>) schedule.invoke(pool, task, 500 + delay * 10, TimeUnit.MILLISECONDS));
            }
            Thread.sleep(50);
            if (!tasks.get(4).cancel(false)) throw new AssertionError("timer was not cancelled");
            if (!done.await(12, TimeUnit.SECONDS)) throw new AssertionError("delayed work did not finish");
            List<Long> expected = List.of(200L, 310L, 400L, 420L, 430L, 500L, 700L, 800L, 900L);
            if (!actual.equals(expected)) throw new AssertionError("out-of-order timer wakeups: " + actual);
            System.out.println("Cancelled timer preserved deadline order: " + actual);
        } finally {
            pool.shutdownNow();
        }
    }
}
