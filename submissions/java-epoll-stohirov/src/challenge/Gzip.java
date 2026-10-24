package challenge;

import java.lang.foreign.MemorySegment;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.zip.CRC32;
import java.util.zip.Deflater;

final class Gzip {
    private static final byte[] HEADER = {0x1f, (byte) 0x8b, 8, 0, 0, 0, 0, 0, 0, (byte) 0xff};

    private final Deflater deflater;
    private final CRC32 crc = new CRC32();
    final ByteBuffer buf;
    final MemorySegment seg;
    private int size;

    Gzip(int level, int capacity) {
        deflater = new Deflater(level, true);
        buf = ByteBuffer.allocateDirect(capacity).order(ByteOrder.LITTLE_ENDIAN);
        seg = MemorySegment.ofBuffer(buf);
    }

    void begin() {
        deflater.reset();
        crc.reset();
        size = 0;
        buf.clear();
        buf.put(HEADER);
    }

    void add(byte[] a, int off, int n) {
        crc.update(a, off, n);
        size += n;
        deflater.setInput(a, off, n);
        while (!deflater.needsInput()) deflate(Deflater.NO_FLUSH);
    }

    int end() {
        deflater.finish();
        while (!deflater.finished()) deflate(Deflater.NO_FLUSH);
        buf.putInt((int) crc.getValue());
        buf.putInt(size);
        return buf.position();
    }

    private void deflate(int flush) {
        if (!buf.hasRemaining()) throw new IllegalStateException("gzip buffer full");
        deflater.deflate(buf, flush);
    }
}
