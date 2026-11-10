package challenge;

import io.vertx.core.MultiMap;
import io.vertx.core.buffer.Buffer;
import io.vertx.core.http.HttpHeaders;
import java.util.Arrays;
import java.util.zip.CRC32;
import java.util.zip.Deflater;

final class Gzip implements AutoCloseable {
    private static final byte[] HEADER = {31, (byte) 139, 8, 0, 0, 0, 0, 0, 0, (byte) 255};
    private final Deflater deflater;
    private final CRC32 crc = new CRC32();
    private byte[] scratch = new byte[8192];

    Gzip(int level) { deflater = new Deflater(level, true); }

    Buffer compress(byte[] input, int offset, int length) {
        deflater.reset();
        crc.reset();
        crc.update(input, offset, length);
        deflater.setInput(input, offset, length);
        deflater.finish();
        System.arraycopy(HEADER, 0, scratch, 0, HEADER.length);
        int size = HEADER.length;
        while (!deflater.finished()) {
            if (size == scratch.length) scratch = Arrays.copyOf(scratch, scratch.length * 2);
            size += deflater.deflate(scratch, size, scratch.length - size);
        }
        if (scratch.length - size < 8) scratch = Arrays.copyOf(scratch, scratch.length * 2);
        int checksum = (int) crc.getValue();
        for (int i = 0; i < 4; i++) scratch[size++] = (byte) (checksum >>> (8 * i));
        for (int i = 0; i < 4; i++) scratch[size++] = (byte) (length >>> (8 * i));
        return Buffer.buffer(size).appendBytes(scratch, 0, size);
    }

    static boolean accepts(MultiMap headers) {
        String first = headers.get(HttpHeaders.ACCEPT_ENCODING);
        if (first == null) return false;
        if (first.equalsIgnoreCase("gzip")) return true;
        for (String header : headers.getAll(HttpHeaders.ACCEPT_ENCODING)) {
            if (header.equalsIgnoreCase("gzip")) return true;
            for (String entry : header.split(",")) {
                String[] parts = entry.trim().split(";", 2);
                if (!parts[0].trim().equalsIgnoreCase("gzip")) continue;
                if (parts.length == 1) return true;
                String[] parameter = parts[1].trim().split("=", 2);
                if (parameter.length == 2 && parameter[0].trim().equalsIgnoreCase("q")) {
                    try {
                        float quality = Float.parseFloat(parameter[1].trim());
                        if (quality > 0 && quality <= 1) return true;
                    } catch (NumberFormatException ignored) { }
                }
            }
        }
        return false;
    }

    @Override public void close() { deflater.end(); }
}
