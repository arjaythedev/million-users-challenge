using System;
using System.Buffers.Binary;
using System.IO.Hashing;
using System.Numerics;
using System.Runtime.CompilerServices;

/// <summary>
/// A gzip writer for responses, one per loop, that allocates nothing per call: one deflate block with the fixed
/// Huffman codes and a greedy LZ77 match search over a hash table that is never cleared (each call's positions are
/// offset past the last call's, so stale entries fall out of range on their own). A feed is about 3.5 KB of JSON that
/// repeats itself 20 times, so the matches carry it and building Huffman codes per response would cost more than it
/// saves: mload's feed goes from three TCP segments to one either way.
/// </summary>
sealed unsafe class Gzip
{
    const int HashBits = 13, MinMatch = 4, MaxMatch = 258, MaxDistance = 32768;

    /// <summary>Last position (plus <see cref="_base"/> of its call) each 4-byte hash was seen at.</summary>
    readonly int[] _head = new int[1 << HashBits];
    /// <summary>Added to this call's positions; every entry below it belongs to an earlier call.</summary>
    int _base = MaxDistance + 1;

    /// <summary>The most a gzip of <paramref name="length"/> bytes can take here: every byte a 9-bit literal, plus the
    /// header, the trailer and the bit writer's 8 bytes of slack.</summary>
    public static int Bound(int length) => length + (length >> 3) + 64;

    /// <summary>Writes <paramref name="input"/> as one gzip member into <paramref name="output"/>; its length, or 0
    /// when <paramref name="output"/> is shorter than <see cref="Bound"/>.</summary>
    public int Compress(ReadOnlySpan<byte> input, Span<byte> output)
    {
        int n = input.Length;
        if (output.Length < Bound(n)) return 0;
        if (_base > int.MaxValue - n - MaxDistance - 1)
        {
            Array.Clear(_head);
            _base = MaxDistance + 1;
        }
        int bas = _base;
        _base += n + MaxDistance + 1;

        fixed (byte* src = input)
        fixed (byte* dst = output)
        fixed (int* head = _head)
        fixed (uint* lit = s_literal)
        fixed (uint* len = s_length)
        {
            // header: magic, deflate, no flags, no mtime, no extra flags, OS unknown
            *(ulong*)dst = 0x0000_0000_0008_8B1FUL;
            *(ushort*)(dst + 8) = 0xFF00;
            byte* op = dst + 10;
            ulong bits = 0b011; // BFINAL = 1, BTYPE = 01 (fixed codes)
            int count = 3;

            int i = 0, literals = 0, anchor = n - MinMatch; // positions past anchor can't start a match
            while (i <= anchor)
            {
                uint v = *(uint*)(src + i);
                uint h = (v * 2654435761u) >> (32 - HashBits);
                int candidate = head[h] - bas;
                head[h] = i + bas;
                // an entry from an earlier call (or never set) is at least MaxDistance + 1 behind: one range check
                int distance = i - candidate;
                if ((uint)(distance - 1) >= MaxDistance || *(uint*)(src + candidate) != v)
                {
                    i++;
                    continue;
                }
                int m = MinMatch, limit = Math.Min(MaxMatch, n - i);
                while (m + 8 <= limit)
                {
                    ulong x = *(ulong*)(src + i + m) ^ *(ulong*)(src + candidate + m);
                    if (x != 0)
                    {
                        m += BitOperations.TrailingZeroCount(x) >> 3;
                        goto found;
                    }
                    m += 8;
                }
                while (m < limit && src[i + m] == src[candidate + m]) m++;
            found:
                op = Literals(src + literals, i - literals, lit, op, ref bits, ref count);
                uint l = len[m];
                bits |= (ulong)(l & 0xFFFFFF) << count;
                count += (int)(l >> 24);
                uint d = Distance(distance);
                bits |= (ulong)(d & 0xFFFFFF) << count;
                count += (int)(d >> 24);
                *(ulong*)op = bits;
                op += count >> 3;
                bits >>= count & ~7;
                count &= 7;
                i += m;
                literals = i;
                // the match's last position goes in too, so the next post's copy of it finds this one
                if (i <= anchor)
                {
                    uint e = *(uint*)(src + i - 1);
                    head[(e * 2654435761u) >> (32 - HashBits)] = i - 1 + bas;
                }
            }
            op = Literals(src + literals, n - literals, lit, op, ref bits, ref count);
            // end of block (symbol 256: seven 0 bits), then pad to a byte
            count += 7;
            *(ulong*)op = bits;
            op += (count + 7) >> 3;
            *(uint*)op = Crc32.HashToUInt32(input);
            *(uint*)(op + 4) = (uint)n;
            if (!BitConverter.IsLittleEndian)
            {
                *(uint*)op = BinaryPrimitives.ReverseEndianness(*(uint*)op);
                *(uint*)(op + 4) = BinaryPrimitives.ReverseEndianness(*(uint*)(op + 4));
            }
            return (int)(op + 8 - dst);
        }
    }

    /// <summary>Writes <paramref name="length"/> literals four at a time: their codes are looked up independently and
    /// joined (at most 36 bits, 43 with what waits), then flushed once.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal static byte* Literals(byte* p, int length, uint* lit, byte* op, ref ulong bits, ref int count)
    {
        ulong b = bits;
        int c = count;
        byte* end = p + length;
        for (; p + 4 <= end; p += 4)
        {
            uint c0 = lit[p[0]], c1 = lit[p[1]], c2 = lit[p[2]], c3 = lit[p[3]];
            int l0 = (int)(c0 >> 24), l1 = (int)(c1 >> 24), l2 = (int)(c2 >> 24), l3 = (int)(c3 >> 24);
            ulong joined = (c0 & 0xFFFFFF) | (ulong)(c1 & 0xFFFFFF) << l0 | (ulong)(c2 & 0xFFFFFF) << (l0 + l1) | (ulong)(c3 & 0xFFFFFF) << (l0 + l1 + l2);
            b |= joined << c;
            c += l0 + l1 + l2 + l3;
            *(ulong*)op = b;
            op += c >> 3;
            b >>= c & ~7;
            c &= 7;
        }
        for (; p < end; p++)
        {
            uint c0 = lit[*p];
            b |= (ulong)(c0 & 0xFFFFFF) << c;
            c += (int)(c0 >> 24);
            *(ulong*)op = b;
            op += c >> 3;
            b >>= c & ~7;
            c &= 7;
        }
        bits = b;
        count = c;
        return op;
    }

    /// <summary>A distance's fixed code and extra bits, LSB first, with their count in the top byte.</summary>
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    internal static uint Distance(int distance)
    {
        int d = distance - 1;
        int code = d < 4 ? d : (BitOperations.Log2((uint)d) << 1) | ((d >> (BitOperations.Log2((uint)d) - 1)) & 1);
        int extraBits = code < 4 ? 0 : (code >> 1) - 1;
        int extra = d - (code < 4 ? code : ((2 | (code & 1)) << extraBits));
        return (uint)(s_distanceCode[code] | (extra << 5)) | (uint)((5 + extraBits) << 24);
    }

    // fixed Huffman codes, bit-reversed for an LSB-first writer, with their length in the top byte
    internal static readonly uint[] s_literal = Literals();
    internal static readonly uint[] s_length = Lengths();
    static readonly byte[] s_distanceCode = DistanceCodes();

    static uint Reverse(uint code, int length)
    {
        uint r = 0;
        for (int k = 0; k < length; k++) r |= ((code >> k) & 1) << (length - 1 - k);
        return r;
    }

    /// <summary>The fixed code of a literal/length symbol (RFC 1951 3.2.6).</summary>
    static uint Symbol(int s)
    {
        if (s < 144) return Reverse((uint)(0x30 + s), 8) | 8u << 24;
        if (s < 256) return Reverse((uint)(0x190 + s - 144), 9) | 9u << 24;
        if (s < 280) return Reverse((uint)(s - 256), 7) | 7u << 24;
        return Reverse((uint)(0xC0 + s - 280), 8) | 8u << 24;
    }

    static uint[] Literals()
    {
        var t = new uint[256];
        for (int s = 0; s < 256; s++) t[s] = Symbol(s);
        return t;
    }

    /// <summary>For each match length 3..258: its length symbol's code and extra bits in one.</summary>
    static uint[] Lengths()
    {
        ReadOnlySpan<int> bases = [3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31, 35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258];
        ReadOnlySpan<int> extras = [0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2, 3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0];
        var t = new uint[MaxMatch + 1];
        for (int l = 3; l <= MaxMatch; l++)
        {
            int k = bases.Length - 1;
            while (bases[k] > l) k--;
            if (l == 258) k = 28; // 258 has its own symbol, 285, not 284 with extra bits 31
            uint c = Symbol(257 + k);
            int codeLength = (int)(c >> 24);
            t[l] = (c & 0xFFFFFF) | (uint)((l - bases[k]) << codeLength) | (uint)((codeLength + extras[k]) << 24);
        }
        return t;
    }

    static byte[] DistanceCodes()
    {
        var t = new byte[30];
        for (int c = 0; c < 30; c++) t[c] = (byte)Reverse((uint)c, 5);
        return t;
    }
}
