// gzip for large bodies (the feed). libdeflate level 1 with one compressor per thread: one call per body, no
// stream state to set up. Falls back to System.IO.Compression (zlib-ng, Fastest) if libdeflate.so.0 is missing.
using System.Buffers;
using System.IO.Compression;
using System.Runtime.InteropServices;

namespace FeedServer;

static unsafe partial class Gzip
{
    const string Lib = "libdeflate.so.0";
    public const int MinSize = 1024; // smaller bodies fit in one TCP segment anyway

    [LibraryImport(Lib)] private static partial nint libdeflate_alloc_compressor(int level);
    [LibraryImport(Lib)] private static partial nuint libdeflate_gzip_compress(nint c, byte* input, nuint inLen, byte* output, nuint outAvail);

    public static readonly bool HasLibdeflate = NativeLibrary.TryLoad(Lib, out _);
    public const int Level = 1;

    [ThreadStatic] static nint compressor;

    // Compresses src into dst (starting at dstOffset); returns the compressed length, or -1 if it didn't fit.
    public static int Compress(ReadOnlySpan<byte> src, byte[] dst, int dstOffset)
    {
        if (HasLibdeflate)
        {
            if (compressor == 0) compressor = libdeflate_alloc_compressor(Level);
            fixed (byte* s = src)
            fixed (byte* d = &dst[dstOffset])
            {
                var n = libdeflate_gzip_compress(compressor, s, (nuint)src.Length, d, (nuint)(dst.Length - dstOffset));
                return n == 0 ? -1 : (int)n;
            }
        }
        var ms = new MemoryStream(dst, dstOffset, dst.Length - dstOffset, writable: true);
        try
        {
            using (var gz = new GZipStream(ms, CompressionLevel.Fastest, leaveOpen: true)) gz.Write(src);
            return (int)ms.Position;
        }
        catch (NotSupportedException) { return -1; } // MemoryStream full
    }

    public static int Bound(int len) => len + len / 8 + 128;
}
