package feed.pg;

import static feed.pg.Backend.int4;
import static feed.pg.Backend.int8;
import static feed.pg.Backend.text;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.lang.management.ManagementFactory;
import java.util.List;

import org.junit.jupiter.api.Test;

/**
 * DESIGN.md's central assumption: a steady-state request allocates nothing, so 3M connections' worth of traffic
 * never wakes the garbage collector. Measured with the thread's allocation counter after JIT warmup.
 */
class PgAllocationTest {

    private static final List<PgStatement> STATEMENTS = List.of(
            new PgStatement("a", "SELECT $1, $2", PgStatement.INT8, PgStatement.TEXT));

    /** Reads every column the server will read, the allocation-free way. */
    static final class Reader implements PgHandler {
        long sum;
        int done;
        int foreignKey;

        @Override
        public void onRow(long tag, PgRow row) {
            sum += row.int8(0) + row.int4(1) + row.length(2) + row.offset(2) + row.buffer()[row.offset(2)];
        }

        @Override
        public void onComplete(long tag, long rows) {
            sum += rows;
        }

        @Override
        public void onError(long tag, PgError error) {
            if (error.is("23503")) {
                foreignKey++;
            }
        }

        @Override
        public void onDone(long tag) {
            done++;
        }
    }

    @Test
    void steadyStateRequestsAllocateNothing() {
        PgProtocol pg = new PgProtocol(new PgUrl("h", 5432, "feed", "nick", null), STATEMENTS, () -> "n");
        pg.start();
        pg.output().position(pg.output().limit());
        byte[] startup = new Backend().trustStartup().prepared(1).bytes();
        pg.receive(startup, 0, startup.length);
        pg.output().position(pg.output().limit());
        assertTrue(pg.isReady());

        byte[] reply = new Backend()
                .bindComplete().dataRow(int8(7), int4(3), text("hello")).dataRow(int8(8), int4(4), text("world"))
                .commandComplete("SELECT 2").readyForQuery()
                .error("23503", "fk").readyForQuery()
                .bytes();
        byte[] body = text("a post body");
        Reader reader = new Reader();

        cycles(pg, reply, body, reader, 20_000);
        com.sun.management.ThreadMXBean threads = (com.sun.management.ThreadMXBean) ManagementFactory.getThreadMXBean();
        long thread = Thread.currentThread().threadId();
        long before = threads.getThreadAllocatedBytes(thread);
        cycles(pg, reply, body, reader, 10_000);
        long allocated = threads.getThreadAllocatedBytes(thread) - before;

        assertEquals(60_000, reader.done);
        assertEquals(30_000, reader.foreignKey);
        assertTrue(allocated < 1024, "allocated " + allocated + " bytes over 20,000 requests");
    }

    /** Each cycle: two requests (rows, then a foreign-key error), sent, answered, output drained. */
    private static void cycles(PgProtocol pg, byte[] reply, byte[] body, Reader reader, int n) {
        for (int i = 0; i < n; i++) {
            for (int r = 0; r < 2; r++) {
                pg.bind(0);
                pg.paramInt8(i);
                pg.paramText(body, 0, body.length);
                pg.execute(reader, i);
            }
            var out = pg.output();
            out.position(out.limit());
            pg.receive(reply, 0, reply.length);
        }
    }
}
