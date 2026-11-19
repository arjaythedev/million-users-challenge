using System.Buffers.Binary;
using System.Runtime.CompilerServices;
using System.Runtime.InteropServices;

namespace Feed.Http;

/// <summary>
/// A fast gzip encoder: one deflate block with the fixed Huffman codes of RFC 1951 and greedy matches found through a
/// hash table. It builds no Huffman tree and clears no table per call, which dominates the cost of a general encoder on
/// a 5 KB response. Its output is somewhat larger than libdeflate's at level 1 (a feed takes about 2.3 KB against 1.9 KB),
/// which still fits the same two TCP segments. The CRC-32 comes from libdeflate (libdeflate.so.0).
/// </summary>
internal static unsafe partial class Gzip {
    /// <summary>Bits of the match-finder hash.</summary>
    private const int HashBits = 13;
    /// <summary>Shortest match used. Deflate allows 3; matching 4 bytes at once is cheaper and loses little.</summary>
    private const int MinMatch = 4;
    /// <summary>Longest match deflate can express.</summary>
    private const int MaxMatch = 258;
    /// <summary>Farthest back a match may point.</summary>
    private const int MaxDistance = 32768;
    /// <summary>Positions stored in the hash table are offset by a base that grows each call; past this it starts over.</summary>
    private const int BaseLimit = 1 << 30;

    /// <summary>
    /// Fixed literal/length codes (RFC 1951 3.2.6), bit-reversed for LSB-first output, with the code length in the top
    /// byte. In native memory, so the per-byte lookup has no bounds check.
    /// </summary>
    private static readonly uint* Lit = (uint*)NativeMemory.Alloc(288 * sizeof(uint));
    /// <summary>For a match length less 3: its length symbol (257 to 285), extra bit count and extra value.</summary>
    private static readonly ushort[] LenSymbol = new ushort[256];
    /// <summary>Extra bits of each match length less 3.</summary>
    private static readonly byte[] LenExtraBits = new byte[256];
    /// <summary>Extra value of each match length less 3.</summary>
    private static readonly ushort[] LenExtra = new ushort[256];
    /// <summary>Base distance of each distance code.</summary>
    private static readonly int[] DistBase = new int[30];
    /// <summary>Extra bits of each distance code.</summary>
    private static readonly byte[] DistExtraBits = new byte[30];

    /// <summary>This thread's hash table of positions; allocated on first use.</summary>
    [ThreadStatic]
    private static int[]? t_table;
    /// <summary>This thread's position offset for <see cref="t_table"/>; entries below it belong to earlier inputs.</summary>
    [ThreadStatic]
    private static int t_base;

    /// <summary>Builds the code tables.</summary>
    static Gzip() {
        for (int s = 0; s < 288; s++) {
            (uint code, int bits) = s switch {
                < 144 => ((uint)(0x30 + s), 8),
                < 256 => ((uint)(0x190 + s - 144), 9),
                < 280 => ((uint)(s - 256), 7),
                _ => ((uint)(0xC0 + s - 280), 8),
            };
            Lit[s] = Reverse(code, bits) | (uint)bits << 24;
        }

        ReadOnlySpan<int> lenBase = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
        ReadOnlySpan<byte> lenExtra = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
        for (int code = 0; code < 29; code++) {
            int end = code == 28 ? 259 : lenBase[code + 1];
            for (int len = lenBase[code]; len < end; len++) {
                // 258 has its own code (285) though 227 + 31 would also reach it; the fixed table above already gives 285 to it.
                if (len == 258 && code != 28) {
                    continue;
                }

                LenSymbol[len - 3] = (ushort)(257 + code);
                LenExtraBits[len - 3] = lenExtra[code];
                LenExtra[len - 3] = (ushort)(len - lenBase[code]);
            }
        }

        int d = 1;
        for (int code = 0; code < 30; code++) {
            int extra = code < 4 ? 0 : (code - 2) / 2;
            DistBase[code] = d;
            DistExtraBits[code] = (byte)extra;
            d += 1 << extra;
        }
    }

    /// <summary>The most bytes <see cref="Compress"/> can write for <paramref name="length"/> input bytes.</summary>
    public static int Bound(int length) => length + (length >> 3) + 64;

    /// <summary>
    /// Writes <paramref name="source"/> as one gzip member into <paramref name="destination"/> and returns the number of
    /// bytes written. The destination must hold <see cref="Bound"/> bytes.
    /// </summary>
    public static int Compress(ReadOnlySpan<byte> source, Span<byte> destination) {
        if (destination.Length < Bound(source.Length)) {
            throw new ArgumentException("destination is smaller than Bound(source.Length)", nameof(destination));
        }

        int[] table = t_table ??= new int[1 << HashBits];
        int bas = t_base;
        if (bas > BaseLimit) {
            Array.Clear(table);
            bas = 0;
        }

        // Entries from earlier calls hold positions below this call's base, so they never match.
        t_base = bas + source.Length + MaxDistance + 1;
        int n = source.Length;
        fixed (byte* src = source)
        fixed (byte* dst = destination)
        fixed (int* tab = table) {
            ReadOnlySpan<byte> header = [0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 0xff];
            header.CopyTo(destination);
            BitWriter w = new(dst + header.Length);
            w.Put(3, 3); // BFINAL = 1, BTYPE = 01 (fixed Huffman codes)

            int i = 0;
            while (i <= n - MinMatch) {
                uint v = *(uint*)(src + i);
                int h = (int)((v * 2654435761u) >> (32 - HashBits));
                int cand = tab[h] - bas;
                tab[h] = bas + i;
                // One test for a distance from 1 to MaxDistance. It also refuses entries from earlier calls (cand < 0)
                // and a never-written entry read as position 0 while i is 0.
                if ((uint)(i - cand - 1) < MaxDistance && *(uint*)(src + cand) == v) {
                    int len = MatchLength(src + cand, src + i, Math.Min(MaxMatch, n - i));
                    PutMatch(ref w, len, i - cand);
                    i += len;
                } else {
                    w.Put(Lit[src[i]]);
                    i++;
                }
            }

            for (; i < n; i++) {
                w.Put(Lit[src[i]]);
            }

            w.Put(Lit[256]);
            byte* end = w.Finish();
            BinaryPrimitives.WriteUInt32LittleEndian(new Span<byte>(end, 4), libdeflate_crc32(0, src, (nuint)n));
            BinaryPrimitives.WriteUInt32LittleEndian(new Span<byte>(end + 4, 4), (uint)n);
            return (int)(end + 8 - dst);
        }
    }

    /// <summary>
    /// How far <paramref name="a"/> and <paramref name="b"/> agree, from <see cref="MinMatch"/> (already known equal) up
    /// to <paramref name="max"/>, compared 8 bytes at a time.
    /// </summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static int MatchLength(byte* a, byte* b, int max) {
        int len = MinMatch;
        while (len + 8 <= max) {
            ulong diff = *(ulong*)(a + len) ^ *(ulong*)(b + len);
            if (diff != 0) {
                return len + (System.Numerics.BitOperations.TrailingZeroCount(diff) >> 3);
            }

            len += 8;
        }

        while (len < max && a[len] == b[len]) {
            len++;
        }

        return len;
    }

    /// <summary>Writes the length and distance codes of one match.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static void PutMatch(ref BitWriter w, int len, int dist) {
        int l = len - 3;
        w.Put(Lit[LenSymbol[l]]);
        if (LenExtraBits[l] != 0) {
            w.Put(LenExtra[l], LenExtraBits[l]);
        }

        int code = DistCode(dist);
        w.Put(Reverse((uint)code, 5), 5);
        if (DistExtraBits[code] != 0) {
            w.Put((uint)(dist - DistBase[code]), DistExtraBits[code]);
        }
    }

    /// <summary>The distance code (0 to 29) of a distance from 1 to 32768.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    private static int DistCode(int dist) {
        if (dist <= 4) {
            return dist - 1;
        }

        // Codes 4 and up come in pairs per power of two: the top bit's position and the bit below it.
        int top = 31 - System.Numerics.BitOperations.LeadingZeroCount((uint)(dist - 1));
        return 2 * top + ((dist - 1) >> (top - 1) & 1);
    }

    /// <summary>Reverses the low <paramref name="bits"/> bits of <paramref name="code"/>.</summary>
    private static uint Reverse(uint code, int bits) {
        uint r = 0;
        for (int b = 0; b < bits; b++) {
            r = (r << 1) | ((code >> b) & 1);
        }

        return r;
    }

    /// <summary>Writes bits least-significant first, four bytes at a time.</summary>
    private ref struct BitWriter {
        /// <summary>Bits not yet written.</summary>
        private ulong _acc;
        /// <summary>Number of bits in <see cref="_acc"/>.</summary>
        private int _count;
        /// <summary>Next output byte.</summary>
        private byte* _out;

        /// <summary>Writes from <paramref name="out"/> on.</summary>
        public BitWriter(byte* @out) {
            _acc = 0;
            _count = 0;
            _out = @out;
        }

        /// <summary>Appends a <see cref="Lit"/> entry: its code, whose length is in the top byte.</summary>
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public void Put(uint entry) => Put(entry & 0xFFFF, (int)(entry >> 24));

        /// <summary>Appends the low <paramref name="bits"/> bits of <paramref name="value"/> (at most 32 bits).</summary>
        [MethodImpl(MethodImplOptions.AggressiveInlining)]
        public void Put(uint value, int bits) {
            _acc |= (ulong)value << _count;
            _count += bits;
            if (_count >= 32) {
                *(uint*)_out = (uint)_acc;
                _out += 4;
                _acc >>= 32;
                _count -= 32;
            }
        }

        /// <summary>Writes the remaining bits, padded to a whole byte, and returns the next free output byte.</summary>
        public byte* Finish() {
            while (_count > 0) {
                *_out++ = (byte)_acc;
                _acc >>= 8;
                _count -= 8;
            }

            return _out;
        }
    }

    /// <summary>libdeflate's CRC-32 (the gzip one): updates <paramref name="crc"/> with <paramref name="length"/> bytes.</summary>
    [LibraryImport("libdeflate.so.0")]
    private static partial uint libdeflate_crc32(uint crc, byte* buffer, nuint length);
}
