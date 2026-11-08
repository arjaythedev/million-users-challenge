package challenge;

import io.micronaut.core.type.Argument;
import io.micronaut.core.type.MutableHeaders;
import io.micronaut.http.HttpHeaders;
import io.micronaut.http.HttpRequest;
import io.micronaut.http.HttpResponse;
import io.micronaut.http.MediaType;
import io.micronaut.http.MutableHttpResponse;
import io.micronaut.http.body.ByteBodyFactory;
import io.micronaut.http.body.CloseableByteBody;
import io.micronaut.http.body.ResponseBodyWriter;
import io.micronaut.http.codec.CodecException;
import io.netty.util.concurrent.FastThreadLocal;
import io.netty.util.concurrent.FastThreadLocalThread;
import java.io.IOException;
import java.io.OutputStream;
import java.util.Arrays;
import java.util.Objects;
import java.util.concurrent.ArrayBlockingQueue;
import java.util.zip.CRC32;
import java.util.zip.Deflater;
import tools.jackson.core.JacksonException;
import tools.jackson.core.JsonGenerator;
import tools.jackson.core.json.JsonFactory;
import tools.jackson.core.util.JsonRecyclerPools;

final class DirectBodyWriter implements ResponseBodyWriter<Object> {
    static final DirectBodyWriter INSTANCE = new DirectBodyWriter();
    private static final int GZIP_LEVEL = Integer.parseInt(System.getProperty("mu.gzip-level", "0"));
    private static final boolean GZIP_LOOP_MODE = switch (System.getProperty("mu.gzip-mode", "loop")) {
        case "loop" -> true;
        case "pool" -> false;
        default -> throw new IllegalArgumentException("mu.gzip-mode must be loop or pool");
    };
    private static final int GZIP_MIN = 1024;
    private static final byte[] GZIP_HEADER = {31, (byte) 139, 8, 0, 0, 0, 0, 0, 0, (byte) 255};
    private static final ArrayBlockingQueue<GzipBuffer> GZIP_POOL = new ArrayBlockingQueue<>(64);
    private static final FastThreadLocal<GzipBuffer> GZIP_LOOP = new FastThreadLocal<>() {
        @Override protected GzipBuffer initialValue() { return new GzipBuffer(); }
        @Override protected void onRemoval(GzipBuffer buffer) {
            if (buffer.deflater != null) buffer.deflater.end();
        }
    };
    // Recycle encoding buffers across short-lived virtual threads with bounded retention.
    private static final JsonFactory JSON = JsonFactory.builder()
            .recyclerPool(JsonRecyclerPools.newBoundedPool(64)).build();

    private DirectBodyWriter() {}

    @Override
    public void writeTo(Argument<Object> type, MediaType mediaType, Object object,
                        MutableHeaders headers, OutputStream output) throws CodecException {
        try (JsonGenerator json = JSON.createGenerator(output)) {
            json.writeStartObject();
            if (object instanceof Responses.Feed feed) {
                json.writeArrayPropertyStart("posts");
                for (Responses.Post post : feed.posts()) writePost(json, post);
                json.writeEndArray();
            } else if (object instanceof Responses.Single single) {
                json.writeName("post");
                writePost(json, single.post());
            } else {
                throw new CodecException("Unsupported response body");
            }
            json.writeEndObject();
        } catch (JacksonException e) {
            throw new CodecException("Error encoding response JSON", e);
        }
    }

    private static void writePost(JsonGenerator json, Responses.Post post) {
        json.writeStartObject();
        json.writeNumberProperty("id", post.id());
        json.writeStringProperty("body", post.body());
        json.writeStringProperty("created_at", post.created_at());
        json.writeStringProperty("author", post.author());
        json.writeNumberProperty("like_count", post.like_count());
        json.writeEndObject();
    }

    @Override
    public CloseableByteBody writePiece(ByteBodyFactory factory, HttpRequest<?> request,
                                        HttpResponse<?> response, Argument<Object> type,
                                        MediaType mediaType, Object object) throws CodecException {
        if (GZIP_LEVEL <= 0 || !(response instanceof MutableHttpResponse<?> mutable))
            return factory.buffer(8192, out -> writeTo(type, mediaType, object, null, out));
        mutable.getHeaders().add(HttpHeaders.VARY, HttpHeaders.ACCEPT_ENCODING);
        if (!acceptsGzip(request))
            return factory.buffer(8192, out -> writeTo(type, mediaType, object, null, out));

        boolean loop = GZIP_LOOP_MODE && Thread.currentThread() instanceof FastThreadLocalThread;
        GzipBuffer buffer = loop ? GZIP_LOOP.get() : GZIP_POOL.poll();
        if (buffer == null) buffer = new GzipBuffer();
        try {
            buffer.count = 0;
            writeTo(type, mediaType, object, null, buffer);
            boolean gzip = buffer.count >= GZIP_MIN;
            GzipBuffer data = buffer;
            // ReadBufferFactory.buffer completes the writer before returning the body.
            var body = factory.buffer(gzip ? 4096 : buffer.count, out -> {
                if (gzip) data.gzip(out);
                else out.write(data.buf, 0, data.count);
            });
            if (gzip) mutable.getHeaders().set(HttpHeaders.CONTENT_ENCODING, "gzip");
            return body;
        } catch (IOException e) {
            throw new CodecException("Error encoding gzip response", e);
        } finally {
            if (!loop && !GZIP_POOL.offer(buffer) && buffer.deflater != null) buffer.deflater.end();
        }
    }

    private static boolean acceptsGzip(HttpRequest<?> request) {
        String first = request.getHeaders().get(HttpHeaders.ACCEPT_ENCODING);
        if (first == null) return false;
        if (first.equalsIgnoreCase("gzip")) return true;
        for (String header : request.getHeaders().getAll(HttpHeaders.ACCEPT_ENCODING)) {
            for (String entry : header.split(",")) {
                String[] parts = entry.trim().split(";", 2);
                if (!parts[0].trim().equalsIgnoreCase("gzip")) continue;
                if (parts.length == 1) return true;
                String[] parameter = parts[1].trim().split("=", 2);
                if (parameter.length == 2 && parameter[0].trim().equalsIgnoreCase("q")) {
                    try {
                        float quality = Float.parseFloat(parameter[1].trim());
                        if (quality > 0 && quality <= 1) return true;
                    } catch (NumberFormatException ignored) {}
                }
            }
        }
        return false;
    }

    private static final class GzipBuffer extends OutputStream {
        private byte[] buf = new byte[8192];
        private int count;
        private Deflater deflater;
        private CRC32 crc;
        private byte[] compressed;

        @Override public void write(int value) {
            grow(1);
            buf[count++] = (byte) value;
        }

        @Override public void write(byte[] bytes, int offset, int length) {
            Objects.checkFromIndexSize(offset, length, bytes.length);
            grow(length);
            System.arraycopy(bytes, offset, buf, count, length);
            count += length;
        }

        private void grow(int length) {
            int needed = Math.addExact(count, length);
            if (needed > buf.length)
                buf = Arrays.copyOf(buf, (int) Math.min(Integer.MAX_VALUE, Math.max((long) buf.length * 2, needed)));
        }

        private void gzip(OutputStream out) throws IOException {
            if (deflater == null) {
                deflater = new Deflater(GZIP_LEVEL, true);
                crc = new CRC32();
                compressed = new byte[8192];
            }
            deflater.reset();
            crc.reset();
            crc.update(buf, 0, count);
            deflater.setInput(buf, 0, count);
            deflater.finish();
            out.write(GZIP_HEADER);
            while (!deflater.finished()) out.write(compressed, 0, deflater.deflate(compressed));
            writeInt(out, crc.getValue());
            writeInt(out, count);
        }

        private static void writeInt(OutputStream out, long value) throws IOException {
            for (int shift = 0; shift < 32; shift += 8) out.write((int) (value >>> shift) & 255);
        }
    }
}
