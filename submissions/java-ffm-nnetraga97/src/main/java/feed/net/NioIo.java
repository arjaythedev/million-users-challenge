package feed.net;

import java.io.IOException;
import java.net.InetSocketAddress;
import java.net.StandardSocketOptions;
import java.nio.ByteBuffer;
import java.nio.channels.SelectableChannel;
import java.nio.channels.SelectionKey;
import java.nio.channels.Selector;
import java.nio.channels.ServerSocketChannel;
import java.nio.channels.SocketChannel;
import java.util.Arrays;
import java.util.Iterator;

/**
 * java.nio Selector backend: portable, for development on macOS and as a fallback. It keeps a SocketChannel and a
 * SelectionKey per connection, so it is not the one to hold millions of connections; {@link EpollIo} is.
 */
public final class NioIo implements Io {

    private static final int LISTENER = -1;

    private final Selector selector;
    private SelectableChannel[] channels = new SelectableChannel[1024];
    private SelectionKey[] keys = new SelectionKey[1024];
    private int[] free = new int[1024];
    private int freeCount;
    private int nextId;

    public NioIo() throws IOException {
        selector = Selector.open();
    }

    @Override
    public int listen(String host, int port) throws IOException {
        ServerSocketChannel server = ServerSocketChannel.open();
        server.setOption(StandardSocketOptions.SO_REUSEADDR, true);
        if (server.supportedOptions().contains(StandardSocketOptions.SO_REUSEPORT)) {
            server.setOption(StandardSocketOptions.SO_REUSEPORT, true);
        }
        server.bind(new InetSocketAddress(host, port), 65_535);
        server.configureBlocking(false);
        server.register(selector, SelectionKey.OP_ACCEPT, LISTENER);
        return ((InetSocketAddress) server.getLocalAddress()).getPort();
    }

    @Override
    public int connect(String host, int port) throws IOException {
        SocketChannel channel = SocketChannel.open();
        boolean done;
        try {
            channel.configureBlocking(false);
            channel.setOption(StandardSocketOptions.TCP_NODELAY, true);
            done = channel.connect(new InetSocketAddress(host, port));
        } catch (IOException | RuntimeException e) { // UnresolvedAddressException is a RuntimeException
            channel.close();
            throw e instanceof IOException io ? io : new IOException("connect " + host + ":" + port + ": " + e, e);
        }
        // Done or not, report the outcome as a writable event, as epoll does.
        return register(channel, done ? SelectionKey.OP_READ | SelectionKey.OP_WRITE : SelectionKey.OP_CONNECT);
    }

    @Override
    public void finishConnect(int id) throws IOException {
        SocketChannel channel = (SocketChannel) channels[id];
        if (!channel.finishConnect()) {
            throw new IOException("connect not finished");
        }
        keys[id].interestOps(SelectionKey.OP_READ);
    }

    @Override
    public void pauseReads(int id) {
        SelectionKey key = keys[id];
        if (key != null && key.isValid()) {
            key.interestOps(key.interestOps() & ~SelectionKey.OP_READ);
        }
    }

    @Override
    public int poll(int timeoutMillis, Handler handler) throws IOException {
        selector.select(Math.max(1, timeoutMillis));
        int sockets = selector.selectedKeys().size();
        Iterator<SelectionKey> it = selector.selectedKeys().iterator();
        while (it.hasNext()) {
            SelectionKey key = it.next();
            it.remove();
            int id = (Integer) key.attachment();
            if (id == LISTENER) {
                accept((ServerSocketChannel) key.channel(), handler);
                continue;
            }
            if (key.isValid() && key.isReadable()) {
                handler.readable(id);
            }
            if (key.isValid() && (key.isWritable() || key.isConnectable()) && keys[id] == key) {
                handler.writable(id);
            }
        }
        return sockets;
    }

    private void accept(ServerSocketChannel server, Handler handler) throws IOException {
        for (int i = 0; i < 256; i++) {
            SocketChannel channel = server.accept();
            if (channel == null) {
                return;
            }
            channel.configureBlocking(false);
            channel.setOption(StandardSocketOptions.TCP_NODELAY, true);
            handler.accepted(register(channel, SelectionKey.OP_READ));
        }
    }

    private int register(SocketChannel channel, int ops) throws IOException {
        int id = freeCount > 0 ? free[--freeCount] : nextId++;
        if (id >= channels.length) {
            channels = Arrays.copyOf(channels, id * 2);
            keys = Arrays.copyOf(keys, id * 2);
        }
        channels[id] = channel;
        keys[id] = channel.register(selector, ops, id);
        return id;
    }

    @Override
    public int read(int id, byte[] b, int off, int len) {
        try {
            int n = ((SocketChannel) channels[id]).read(ByteBuffer.wrap(b, off, len));
            return n;
        } catch (IOException e) {
            return -1;
        }
    }

    @Override
    public int write(int id, byte[] b, int off, int len) {
        try {
            return ((SocketChannel) channels[id]).write(ByteBuffer.wrap(b, off, len));
        } catch (IOException e) {
            return -1;
        }
    }

    @Override
    public void watchWrites(int id, boolean on) {
        SelectionKey key = keys[id];
        if (key != null && key.isValid()) {
            key.interestOps((key.interestOps() & SelectionKey.OP_READ) | (on ? SelectionKey.OP_WRITE : 0));
        }
    }

    @Override
    public void close(int id) {
        SelectableChannel channel = channels[id];
        if (channel == null) {
            return;
        }
        keys[id].cancel();
        try {
            channel.close();
        } catch (IOException e) {
            // already gone
        }
        channels[id] = null;
        keys[id] = null;
        if (freeCount == free.length) {
            free = Arrays.copyOf(free, free.length * 2);
        }
        free[freeCount++] = id;
    }

    @Override
    public void wakeup() {
        selector.wakeup();
    }

    @Override
    public void close() {
        if (!selector.isOpen()) {
            return;
        }
        for (int id = 0; id < nextId; id++) {
            close(id);
        }
        for (SelectionKey key : selector.keys()) {
            try {
                key.channel().close();
            } catch (IOException e) {
                // closing anyway
            }
        }
        try {
            selector.close();
        } catch (IOException e) {
            // closing anyway
        }
    }
}
