package feed.pg;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.nio.ByteBuffer;
import java.nio.channels.SocketChannel;
import java.util.List;
import java.util.function.BooleanSupplier;

/**
 * Drives a PgProtocol over a plain blocking socket, for tests. The server's event loop (build step 3) does the same
 * with non-blocking sockets; PgProtocol doesn't know the difference.
 */
public final class BlockingPg implements AutoCloseable {

    public final PgProtocol pg;
    private final SocketChannel channel;
    private final int maxRead;

    private BlockingPg(PgProtocol pg, SocketChannel channel, int maxRead) {
        this.pg = pg;
        this.channel = channel;
        this.maxRead = maxRead;
    }

    /** Connects and runs startup. {@code maxRead} caps each socket read, to force messages to arrive in pieces. */
    public static BlockingPg connect(PgUrl url, List<PgStatement> statements, int maxRead) throws IOException {
        SocketChannel channel = SocketChannel.open(new InetSocketAddress(url.host(), url.port()));
        BlockingPg conn = new BlockingPg(new PgProtocol(url, statements), channel, maxRead);
        conn.pg.start();
        conn.pumpUntil(() -> conn.pg.isReady() || conn.pg.isFailed());
        return conn;
    }

    /** Sends everything queued, then reads and processes until {@code done}. */
    void pumpUntil(BooleanSupplier done) throws IOException {
        flush();
        while (!done.getAsBoolean()) {
            ByteBuffer buffer = pg.readBuffer();
            if (buffer.remaining() > maxRead) {
                buffer.limit(buffer.position() + maxRead);
            }
            if (channel.read(buffer) < 0) {
                pg.connectionLost("server closed the connection");
                return;
            }
            pg.process();
            flush();
        }
    }

    /** Waits until every pending request is done. */
    public void drain() throws IOException {
        pumpUntil(() -> pg.pending() == 0);
    }

    private void flush() throws IOException {
        while (pg.hasOutput()) {
            channel.write(pg.output());
        }
    }

    @Override
    public void close() throws IOException {
        channel.close();
    }
}
