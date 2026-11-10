package feed.pg;

import java.lang.invoke.MethodHandles;
import java.lang.invoke.VarHandle;
import java.nio.ByteOrder;
import java.nio.charset.StandardCharsets;

/** A view over one {@code DataRow} message (binary format columns). Reused for every row: valid only in the callback. */
public final class PgRow {

    private static final VarHandle INT8 = MethodHandles.byteArrayViewVarHandle(long[].class, ByteOrder.BIG_ENDIAN);
    private static final VarHandle INT4 = MethodHandles.byteArrayViewVarHandle(int[].class, ByteOrder.BIG_ENDIAN);

    private byte[] buffer;
    private int columns;
    private int[] offsets = new int[8];
    private int[] lengths = new int[8];

    PgRow() {
    }

    /** Points the view at a DataRow body: int16 column count, then int32 length (-1 = null) + bytes per column. */
    void reset(byte[] buf, int off) {
        buffer = buf;
        columns = ((buf[off] & 0xff) << 8) | (buf[off + 1] & 0xff);
        if (columns > offsets.length) {
            offsets = new int[columns];
            lengths = new int[columns];
        }
        int pos = off + 2;
        for (int i = 0; i < columns; i++) {
            int length = (int) INT4.get(buf, pos);
            pos += 4;
            offsets[i] = pos;
            lengths[i] = length;
            if (length > 0) {
                pos += length;
            }
        }
    }

    public int columnCount() {
        return columns;
    }

    public boolean isNull(int column) {
        return lengths[column] < 0;
    }

    /** The buffer holding the column bytes, at {@link #offset} for {@link #length} bytes. */
    public byte[] buffer() {
        return buffer;
    }

    public int offset(int column) {
        return offsets[column];
    }

    public int length(int column) {
        return lengths[column];
    }

    public long int8(int column) {
        return (long) INT8.get(buffer, offsets[column]);
    }

    public int int4(int column) {
        return (int) INT4.get(buffer, offsets[column]);
    }

    /** A text column decoded as a String (allocates: for tests and error paths). */
    public String text(int column) {
        return new String(buffer, offsets[column], lengths[column], StandardCharsets.UTF_8);
    }
}
