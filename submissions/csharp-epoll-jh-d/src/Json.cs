// Compact JSON straight into a pooled byte buffer. Leaves Http.HeaderRoom bytes free at the front so the HTTP
// head can be written in place just before the body once its length is known (no copy).
using System.Buffers;
using System.Globalization;
using System.Numerics;
using System.Runtime.CompilerServices;
using System.Text;

namespace FeedServer;

struct JsonWriter
{
    public byte[] Buf;
    public int Pos;

    public static JsonWriter Create(int capacity = 8192) =>
        new() { Buf = ArrayPool<byte>.Shared.Rent(Http.HeaderRoom + capacity), Pos = Http.HeaderRoom };

    public readonly int BodyLength => Pos - Http.HeaderRoom;

    // At least `bytes` writable bytes at Pos; a full buffer is swapped for a pooled one of the next power of two.
    [MethodImpl(MethodImplOptions.AggressiveInlining)]
    Span<byte> Room(int bytes)
    {
        if (Buf.Length - Pos < bytes) Swap(bytes);
        return Buf.AsSpan(Pos);
    }

    void Swap(int bytes)
    {
        var next = ArrayPool<byte>.Shared.Rent((int)BitOperations.RoundUpToPowerOf2((uint)(Pos + bytes)));
        Buf.AsSpan(0, Pos).CopyTo(next);
        ArrayPool<byte>.Shared.Return(Buf);
        Buf = next;
    }

    public void Raw(ReadOnlySpan<byte> s)
    {
        s.CopyTo(Room(s.Length));
        Pos += s.Length;
    }

    public void Long(long v)
    {
        v.TryFormat(Room(20), out int written, default, CultureInfo.InvariantCulture);
        Pos += written;
    }

    public void Bool(bool v) => Raw(v ? "true"u8 : "false"u8);

    // "2025-12-31T23:59:19.409Z": UTC, milliseconds truncated (not rounded), quoted.
    public void Timestamp(DateTime utc)
    {
        var to = Room(32);
        to[0] = (byte)'"';
        utc.TryFormat(to[1..], out int n, "yyyy-MM-dd'T'HH:mm:ss.fff'Z'", CultureInfo.InvariantCulture);
        to[1 + n] = (byte)'"';
        Pos += n + 2;
    }

    // A quoted JSON string from UTF-16: transcoded to UTF-8, then escaped like Utf8String.
    public void String(string s)
    {
        int max = Encoding.UTF8.GetMaxByteCount(s.Length);
        byte[]? pooled = max > 1024 ? ArrayPool<byte>.Shared.Rent(max) : null;
        Span<byte> utf8 = pooled ?? stackalloc byte[1024];
        int n = Encoding.UTF8.GetBytes(s, utf8);
        Utf8String(utf8[..n]);
        if (pooled is not null) ArrayPool<byte>.Shared.Return(pooled);
    }

    // A quoted JSON string from UTF-8: '"', '\' and control characters escaped, everything else (including
    // multi-byte sequences) copied as is.
    public void Utf8String(ReadOnlySpan<byte> s)
    {
        var to = Room(s.Length * 6 + 2);
        int at = 0;
        to[at++] = (byte)'"';
        while (!s.IsEmpty)
        {
            int plain = s.IndexOfAny(MustEscape);
            if (plain < 0) plain = s.Length;
            s[..plain].CopyTo(to[at..]);
            at += plain;
            if (plain == s.Length) break;
            var esc = Escapes[s[plain]];
            esc.CopyTo(to[at..]);
            at += esc.Length;
            s = s[(plain + 1)..];
        }
        to[at++] = (byte)'"';
        Pos += at;
    }

    static readonly SearchValues<byte> MustEscape =
        SearchValues.Create([.. Enumerable.Range(0, 0x20).Select(i => (byte)i), (byte)'"', (byte)'\\']);

    // The escape sequence for every byte in MustEscape: the short forms where JSON has one, else \u00XX.
    static readonly byte[][] Escapes = Enumerable.Range(0, '\\' + 1).Select(i => Encoding.ASCII.GetBytes(i switch
    {
        '"' => "\\\"",
        '\\' => "\\\\",
        '\n' => "\\n",
        '\r' => "\\r",
        '\t' => "\\t",
        '\b' => "\\b",
        '\f' => "\\f",
        _ => $"\\u{i:x4}",
    })).ToArray();
}
