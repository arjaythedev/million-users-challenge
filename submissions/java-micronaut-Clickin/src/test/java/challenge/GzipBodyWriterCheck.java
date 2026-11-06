package challenge;

import io.micronaut.core.type.Argument;
import io.micronaut.core.util.functional.ThrowingConsumer;
import io.micronaut.http.HttpHeaders;
import io.micronaut.http.HttpRequest;
import io.micronaut.http.HttpResponse;
import io.micronaut.http.MediaType;
import io.micronaut.http.body.AvailableByteBody;
import io.micronaut.http.body.ByteBodyFactory;
import io.micronaut.http.body.CloseableAvailableByteBody;
import io.micronaut.http.codec.CodecException;
import io.micronaut.http.netty.body.NettyByteBodyFactory;
import io.netty.channel.DefaultEventLoop;
import io.netty.channel.embedded.EmbeddedChannel;
import io.netty.util.concurrent.FastThreadLocal;
import io.netty.util.concurrent.FastThreadLocalThread;
import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.io.FilterOutputStream;
import java.io.IOException;
import java.io.OutputStream;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.List;
import java.util.Queue;
import java.util.Random;
import java.util.concurrent.CountDownLatch;
import java.util.concurrent.Executors;
import java.util.concurrent.TimeUnit;
import java.util.zip.GZIPInputStream;

public final class GzipBodyWriterCheck {
    public static void main(String[] args) throws Exception {
        boolean enabled = Integer.parseInt(System.getProperty("mu.gzip-level", "0")) > 0;
        var channel = new EmbeddedChannel();
        var factory = new NettyByteBodyFactory(channel);
        try {
            var feed = feed(0);
            var single = new Responses.Single(new Responses.Post(1, "small", "2026-10-28T01:02:03.004Z", "a", 0));
            if (enabled) {
                check(factory, feed, true, "gzip");
                placements(factory);
                releaseOrdering(factory);
            }
            check(factory, feed, enabled, "gzip");
            check(factory, feed, false);
            check(factory, single, false, "gzip");
            check(factory, feed, enabled, "br, GZip;q=0.5");
            check(factory, feed, enabled, "br", "gzip");
            check(factory, feed, false, "gzip;q=0.000, br");
            check(factory, feed, false, "notgzip");
            var largeSingle = new Responses.Single(new Responses.Post(2, "한".repeat(400), "2026-10-28T01:02:03.004Z", "a", 0));
            check(factory, largeSingle, enabled, "gzip");
            check(factory, sizedSingle(1023), false, "gzip");
            check(factory, sizedSingle(1024), enabled, "gzip");
            for (int i = 1; i <= 20; i++) check(factory, feed(i), enabled, "gzip");
            if (enabled) concurrent(factory);
            System.out.println("Gzip body checks passed at level " + System.getProperty("mu.gzip-level", "0") +
                    " and mode " + System.getProperty("mu.gzip-mode", "loop"));
        } finally {
            channel.finishAndReleaseAll();
        }
    }

    private static Responses.Single sizedSingle(int size) {
        String body = "한".repeat(300);
        var post = new Responses.Post(1, body, "2026-10-28T01:02:03.004Z", "a", 0);
        int padding = size - json(new Responses.Single(post)).length;
        var result = new Responses.Single(new Responses.Post(post.id(), body + "x".repeat(padding), post.created_at(), post.author(), 0));
        if (json(result).length != size) throw new AssertionError("incorrect test body size");
        return result;
    }

    private static byte[] json(Object value) {
        var out = new ByteArrayOutputStream();
        DirectBodyWriter.INSTANCE.writeTo(Argument.of(Object.class), MediaType.APPLICATION_JSON_TYPE, value, null, out);
        return out.toByteArray();
    }

    private static Responses.Feed feed(int seed) {
        var random = new Random(seed);
        var posts = new ArrayList<Responses.Post>();
        for (int i = 0; i < 20; i++) {
            var body = new StringBuilder();
            for (int j = 0; j < 300 + (seed + i) % 201; j++) body.append((char) (' ' + random.nextInt(95)));
            posts.add(new Responses.Post(i + seed * 20L, body.toString(), "2026-10-28T01:02:03.004Z", "user" + i, i));
        }
        return new Responses.Feed(posts);
    }

    private static void concurrent(NettyByteBodyFactory factory) throws Exception {
        var ready = new CountDownLatch(1);
        try (var executor = Executors.newVirtualThreadPerTaskExecutor()) {
            var tasks = new ArrayList<java.util.concurrent.Future<?>>();
            for (int i = 0; i < 32; i++) {
                int seed = i + 100;
                tasks.add(executor.submit(() -> {
                    ready.await();
                    check(factory, feed(seed), true, "gzip");
                    return null;
                }));
            }
            ready.countDown();
            for (var task : tasks) task.get();
        }
    }

    private static Object field(Object target, String name) throws Exception {
        var type = target instanceof Class<?> c ? c : target.getClass();
        var field = type.getDeclaredField(name);
        field.setAccessible(true);
        return field.get(target instanceof Class<?> ? null : target);
    }

    private static void placements(NettyByteBodyFactory factory) throws Exception {
        boolean loopMode = System.getProperty("mu.gzip-mode", "loop").equals("loop");
        var local = (FastThreadLocal<?>) field(DirectBodyWriter.class, "GZIP_LOOP");
        var pool = (Queue<?>) field(DirectBodyWriter.class, "GZIP_POOL");
        if (pool.size() != 1) throw new AssertionError("expected one warmed pooled buffer");
        Object pooled = pool.peek();
        int pooledCount = (int) field(pooled, "count");
        var loop = new DefaultEventLoop();
        var otherLoop = new DefaultEventLoop();
        try {
            Object first = loop.submit(() -> {
                if (!(Thread.currentThread() instanceof FastThreadLocalThread) || Thread.currentThread().isVirtual())
                    throw new AssertionError("expected a Netty platform thread");
                check(factory, feed(51), true, "gzip");
                Object buffer = local.getIfExists();
                if ((buffer != null) != loopMode) throw new AssertionError("wrong event-loop placement");
                Object deflater = buffer == null ? null : field(buffer, "deflater");
                Object crc = buffer == null ? null : field(buffer, "crc");
                Object scratch = buffer == null ? null : field(buffer, "compressed");
                failedWrite(new InterceptingFactory(factory, null, null, true));
                check(factory, feed(52), true, "gzip");
                check(factory, new Responses.Single(new Responses.Post(7, "한😀\"\\\n".repeat(10000), "date", "a", 0)), true, "gzip");
                if (buffer != local.getIfExists() || (buffer != null &&
                        (deflater == null || deflater != field(buffer, "deflater") ||
                         crc != field(buffer, "crc") || scratch != field(buffer, "compressed"))))
                    throw new AssertionError("event-loop compressor was not reused");
                return buffer;
            }).get(10, TimeUnit.SECONDS);
            Object second = otherLoop.submit(() -> {
                check(factory, feed(53), true, "gzip");
                return local.getIfExists();
            }).get(10, TimeUnit.SECONDS);
            if (loopMode && (first == null || second == null || first == second))
                throw new AssertionError("event loops did not get separate compressors");
            if (!loopMode && second != null) throw new AssertionError("pool mode used a loop compressor");
            if (pool.size() != 1 || pool.peek() != pooled ||
                    (loopMode ? (int) field(pooled, "count") != pooledCount :
                            (int) field(pooled, "count") != json(feed(53)).length))
                throw new AssertionError("event loop used the wrong pool");

            var value = sizedSingle(1024);
            try (var executor = Executors.newVirtualThreadPerTaskExecutor()) {
                executor.submit(() -> {
                    if (!Thread.currentThread().isVirtual()) throw new AssertionError("expected a virtual thread");
                    check(factory, value, true, "gzip");
                    return null;
                }).get(10, TimeUnit.SECONDS);
            }
            if (pool.peek() != pooled || (int) field(pooled, "count") != 1024)
                throw new AssertionError("virtual thread did not reuse the pooled buffer");
        } finally {
            loop.shutdownGracefully(0, 1, TimeUnit.SECONDS).syncUninterruptibly();
            otherLoop.shutdownGracefully(0, 1, TimeUnit.SECONDS).syncUninterruptibly();
        }
    }

    private static void releaseOrdering(NettyByteBodyFactory factory) throws Exception {
        var pool = (Queue<?>) field(DirectBodyWriter.class, "GZIP_POOL");
        for (boolean gzip : new boolean[] {true, false}) {
            Object held = pool.peek();
            var entered = new CountDownLatch(1);
            var resume = new CountDownLatch(1);
            var blockedFactory = new InterceptingFactory(factory, entered, resume, false);
            try (var executor = Executors.newVirtualThreadPerTaskExecutor()) {
                var blocked = executor.submit(() -> {
                    check(blockedFactory, gzip ? feed(61) : sizedSingle(1023), gzip, "gzip");
                    return null;
                });
                try {
                    if (!entered.await(10, TimeUnit.SECONDS)) throw new AssertionError("write did not start");
                    if (pool.contains(held)) throw new AssertionError("buffer released before the write finished");
                    check(factory, feed(62), true, "gzip");
                    if (pool.contains(held)) throw new AssertionError("buffer reused during the write");
                } finally {
                    resume.countDown();
                }
                blocked.get(10, TimeUnit.SECONDS);
            }
            if (!pool.contains(held)) throw new AssertionError("buffer was not returned after the write");
        }
        Object held = pool.peek();
        failedWrite(new InterceptingFactory(factory, null, null, true));
        if (!pool.contains(held)) throw new AssertionError("buffer was not returned after a failed write");
    }

    private static void failedWrite(ByteBodyFactory factory) throws Exception {
        try {
            check(factory, feed(63), true, "gzip");
            throw new AssertionError("write failure was not propagated");
        } catch (CodecException expected) {
            if (!(expected.getCause() instanceof IOException)) throw expected;
        }
    }

    private static final class InterceptingFactory extends ByteBodyFactory {
        private final NettyByteBodyFactory delegate;
        private final CountDownLatch entered, resume;
        private final boolean fail;

        @SuppressWarnings("deprecation")
        private InterceptingFactory(NettyByteBodyFactory delegate, CountDownLatch entered, CountDownLatch resume, boolean fail) {
            super(delegate.byteBufferFactory(), delegate.readBufferFactory());
            this.delegate = delegate;
            this.entered = entered;
            this.resume = resume;
            this.fail = fail;
        }

        @Override
        public <T extends Throwable> CloseableAvailableByteBody buffer(int size, ThrowingConsumer<? super OutputStream, T> writer) throws T {
            return delegate.buffer(size, out -> writer.accept(new FilterOutputStream(out) {
                private boolean first = true;
                private void intercept() throws IOException {
                    if (!first) return;
                    first = false;
                    if (fail) throw new IOException("forced write failure");
                    entered.countDown();
                    try {
                        if (!resume.await(10, TimeUnit.SECONDS)) throw new IOException("write timed out");
                    } catch (InterruptedException e) {
                        Thread.currentThread().interrupt();
                        throw new IOException(e);
                    }
                }
                @Override public void write(int value) throws IOException { intercept(); out.write(value); }
                @Override public void write(byte[] bytes, int offset, int length) throws IOException {
                    intercept();
                    out.write(bytes, offset, length);
                }
            }));
        }
    }

    private static void check(ByteBodyFactory factory, Object value, boolean gzip, String... encodings) throws Exception {
        var request = HttpRequest.GET("/check");
        for (String encoding : encodings) request.getHeaders().add(HttpHeaders.ACCEPT_ENCODING, encoding);
        var response = HttpResponse.ok(value);
        response.getHeaders().add(HttpHeaders.VARY, "Origin");
        byte[] expected = json(value);
        try (var body = DirectBodyWriter.INSTANCE.writePiece(factory, request, response,
                Argument.of(Object.class), MediaType.APPLICATION_JSON_TYPE, value)) {
            var available = (AvailableByteBody) body;
            long length = available.length();
            byte[] actual = available.toByteArray();
            if (length != actual.length) throw new AssertionError("incorrect available body length");
            if (!response.getHeaders().getAll(HttpHeaders.VARY).contains("Origin")) throw new AssertionError("lost Vary header");
            if (Integer.parseInt(System.getProperty("mu.gzip-level", "0")) > 0 &&
                    !response.getHeaders().getAll(HttpHeaders.VARY).contains(HttpHeaders.ACCEPT_ENCODING))
                throw new AssertionError("missing Vary: Accept-Encoding");
            String encoding = response.header(HttpHeaders.CONTENT_ENCODING);
            if (gzip ? !"gzip".equals(encoding) : encoding != null)
                throw new AssertionError("incorrect Content-Encoding for " + List.of(encodings));
            if (gzip) {
                try (var input = new GZIPInputStream(new ByteArrayInputStream(actual))) { actual = input.readAllBytes(); }
            }
            if (!Arrays.equals(expected, actual)) throw new AssertionError("JSON changed during encoding");
        }
    }
}
