package feed.http;

import java.lang.foreign.Arena;
import java.lang.foreign.FunctionDescriptor;
import java.lang.foreign.Linker;
import java.lang.foreign.MemorySegment;
import java.lang.foreign.SymbolLookup;
import java.lang.foreign.ValueLayout;
import java.lang.invoke.MethodHandle;
import java.util.zip.CRC32;
import java.util.zip.Deflater;

/**
 * Compresses a whole response body into a gzip stream in one call, straight into the caller's buffer: no stream, no
 * per-request state. libdeflate (through FFM) when the library is installed, else the JDK's zlib. One per worker,
 * made at startup; not thread-safe.
 */
public abstract sealed class Gzip implements AutoCloseable permits Gzip.Jdk, Gzip.LibDeflate {

    private final int level;

    Gzip(int level) {
        this.level = level;
    }

    /** libdeflate if it loads, else the JDK. */
    public static Gzip open(int level) {
        Gzip g = LibDeflate.tryOpen(level);
        return g != null ? g : new Jdk(level);
    }

    /**
     * Compresses {@code in[off, off+len)} into {@code out} at {@code outOff}, using at most {@code outLen} bytes.
     * Returns the compressed length, or 0 if it didn't fit.
     */
    public abstract int compress(byte[] in, int off, int len, byte[] out, int outOff, int outLen);

    public abstract String name();

    /** The level in use: the one asked for, capped at this compressor's maximum. */
    public int level() {
        return level;
    }

    /** Frees the compressor; idempotent. */
    @Override
    public abstract void close();

    /** java.util.zip: raw deflate plus the gzip header and trailer written here. */
    static final class Jdk extends Gzip {

        private static final int HEADER = 10;
        private static final int TRAILER = 8;

        private final Deflater deflater;
        private final CRC32 crc = new CRC32();

        Jdk(int level) {
            super(Math.min(level, Deflater.BEST_COMPRESSION));
            deflater = new Deflater(level(), true);
        }

        @Override
        public int compress(byte[] in, int off, int len, byte[] out, int outOff, int outLen) {
            if (outLen <= HEADER + TRAILER) {
                return 0;
            }
            deflater.reset();
            deflater.setInput(in, off, len);
            deflater.finish();
            int n = deflater.deflate(out, outOff + HEADER, outLen - HEADER - TRAILER);
            if (!deflater.finished()) {
                return 0;
            }
            // RFC 1952: magic, CM=deflate, no flags, no mtime, no extra flags, OS unknown.
            out[outOff] = 0x1f;
            out[outOff + 1] = (byte) 0x8b;
            out[outOff + 2] = 8;
            for (int i = 3; i < 9; i++) {
                out[outOff + i] = 0;
            }
            out[outOff + 9] = (byte) 0xff;
            crc.reset();
            crc.update(in, off, len);
            int t = outOff + HEADER + n;
            littleEndian(out, t, (int) crc.getValue());
            littleEndian(out, t + 4, len);
            return HEADER + n + TRAILER;
        }

        private static void littleEndian(byte[] b, int at, int v) {
            b[at] = (byte) v;
            b[at + 1] = (byte) (v >>> 8);
            b[at + 2] = (byte) (v >>> 16);
            b[at + 3] = (byte) (v >>> 24);
        }

        @Override
        public String name() {
            return "jdk";
        }

        @Override
        public void close() {
            deflater.end();
        }
    }

    /** libdeflate's one-shot gzip compressor, working on two native buffers it owns. */
    static final class LibDeflate extends Gzip {

        private static final int MAX_LEVEL = 12;
        private static final int INITIAL_BUFFER = 64 * 1024;

        private static final String[] NAMES = {"libdeflate.so.0", "libdeflate.so", "libdeflate.0.dylib",
            "libdeflate.dylib"};
        private static final MethodHandle ALLOC;
        private static final MethodHandle COMPRESS;
        private static final MethodHandle FREE;

        static {
            MethodHandle alloc = null;
            MethodHandle compress = null;
            MethodHandle free = null;
            SymbolLookup lib = null;
            for (String name : NAMES) {
                try {
                    lib = SymbolLookup.libraryLookup(name, Arena.global());
                    break;
                } catch (IllegalArgumentException notFound) {
                    // try the next name
                } catch (RuntimeException | LinkageError e) {
                    System.err.println("gzip: can't load libdeflate (" + e + "); using the JDK");
                    break;
                }
            }
            if (lib != null) {
                try {
                    Linker linker = Linker.nativeLinker();
                    MethodHandle a = linker.downcallHandle(lib.find("libdeflate_alloc_compressor").orElseThrow(),
                            FunctionDescriptor.of(ValueLayout.ADDRESS, ValueLayout.JAVA_INT));
                    MethodHandle c = linker.downcallHandle(lib.find("libdeflate_gzip_compress").orElseThrow(),
                            FunctionDescriptor.of(ValueLayout.JAVA_LONG, ValueLayout.ADDRESS, ValueLayout.ADDRESS,
                                    ValueLayout.JAVA_LONG, ValueLayout.ADDRESS, ValueLayout.JAVA_LONG),
                            Linker.Option.critical(false));
                    MethodHandle f = linker.downcallHandle(lib.find("libdeflate_free_compressor").orElseThrow(),
                            FunctionDescriptor.ofVoid(ValueLayout.ADDRESS));
                    alloc = a;
                    compress = c;
                    free = f;
                } catch (RuntimeException | LinkageError e) { // a symbol missing, native access refused, ...
                    System.err.println("gzip: can't use libdeflate (" + e + "); using the JDK");
                }
            }
            ALLOC = alloc;
            COMPRESS = compress;
            FREE = free;
        }

        private final Arena arena = Arena.ofShared();
        private MemorySegment compressor;
        private MemorySegment input = arena.allocate(INITIAL_BUFFER);
        private MemorySegment output = arena.allocate(INITIAL_BUFFER);

        private LibDeflate(MemorySegment compressor, int level) {
            super(level);
            this.compressor = compressor;
        }

        /** A compressor, or null when libdeflate isn't installed. */
        static LibDeflate tryOpen(int level) {
            if (ALLOC == null) {
                return null;
            }
            int capped = Math.min(level, MAX_LEVEL);
            try {
                MemorySegment c = (MemorySegment) ALLOC.invokeExact(capped);
                if (c.equals(MemorySegment.NULL)) {
                    throw new IllegalArgumentException("libdeflate: no compressor for level " + capped);
                }
                return new LibDeflate(c, capped);
            } catch (RuntimeException | Error e) {
                throw e;
            } catch (Throwable t) {
                throw new IllegalStateException(t);
            }
        }

        /** Copies the body into native memory, compresses it there and copies the result back (a few KB each way). */
        @Override
        public int compress(byte[] in, int off, int len, byte[] out, int outOff, int outLen) {
            if (input.byteSize() < len) {
                input = arena.allocate(Math.max(len, input.byteSize() * 2)); // rare: the old one is freed at close
            }
            if (output.byteSize() < outLen) {
                output = arena.allocate(Math.max(outLen, output.byteSize() * 2));
            }
            MemorySegment.copy(in, off, input, ValueLayout.JAVA_BYTE, 0, len);
            long n;
            try {
                n = (long) COMPRESS.invokeExact(compressor, input, (long) len, output, (long) outLen);
            } catch (RuntimeException | Error e) {
                throw e;
            } catch (Throwable t) {
                throw new IllegalStateException(t);
            }
            if (n > 0) {
                MemorySegment.copy(output, ValueLayout.JAVA_BYTE, 0, out, outOff, (int) n);
            }
            return (int) n;
        }

        @Override
        public String name() {
            return "libdeflate";
        }

        @Override
        public void close() {
            if (compressor != null) {
                try {
                    FREE.invokeExact(compressor);
                } catch (Throwable t) {
                    throw new IllegalStateException(t);
                }
                compressor = null;
                arena.close();
            }
        }
    }
}
