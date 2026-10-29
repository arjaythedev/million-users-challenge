// HTTP/1.1: a request-head parser over raw bytes (only the headers the API needs are looked at) and a response
// head written in place in front of the body.
using System.Buffers;
using System.Buffers.Text;
using System.Text;

namespace FeedServer;

enum Method : byte { Other, Get, Post }

ref struct Request
{
    public Method Method;
    public ReadOnlySpan<byte> Path;          // without the query string
    public ReadOnlySpan<byte> Authorization; // raw header value
    public bool HasAuthorization;
    public bool AcceptsGzip;
    public bool KeepAlive;
    public ReadOnlySpan<byte> Body;
}

static class Http
{
    public const int HeaderRoom = 256;        // bytes kept free in front of every response body for the head
    public const int MaxHead = 16 * 1024;
    public const int MaxBody = 1024 * 1024;
    public const int ParseIncomplete = 0, ParseBad = -1, ParseTooLarge = -2;

    // Returns the request's total length (head + body) when complete, ParseIncomplete when more bytes are needed,
    // or ParseBad / ParseTooLarge. needContinue: the head is complete, the body isn't, and the client sent
    // "Expect: 100-continue".
    public static int Parse(ReadOnlySpan<byte> data, out Request req, out bool needContinue)
    {
        req = default;
        needContinue = false;
        int blank = data.IndexOf("\r\n\r\n"u8);
        if (blank < 0) return data.Length > MaxHead ? ParseTooLarge : ParseIncomplete;
        var head = data[..blank];

        long length = 0;
        bool sawRequestLine = false, wantsContinue = false;
        foreach (Range r in head.Split("\r\n"u8))
        {
            var line = head[r];
            if (!sawRequestLine)
            {
                // METHOD SP request-target SP HTTP/1.x
                sawRequestLine = true;
                int a = line.IndexOf((byte)' '), b = line.LastIndexOf((byte)' ');
                if (a <= 0 || b <= a + 1) return ParseBad;
                var proto = line[(b + 1)..];
                if (!proto.StartsWith("HTTP/1."u8)) return ParseBad;
                req.KeepAlive = !proto.SequenceEqual("HTTP/1.0"u8);
                var verb = line[..a];
                req.Method = verb.SequenceEqual("GET"u8) ? Method.Get : verb.SequenceEqual("POST"u8) ? Method.Post : Method.Other;
                var target = line[(a + 1)..b];
                int query = target.IndexOf((byte)'?');
                req.Path = query < 0 ? target : target[..query];
                continue;
            }
            int colon = line.IndexOf((byte)':');
            if (colon <= 0) continue;
            var name = line[..colon];
            var value = line[(colon + 1)..].Trim(" \t"u8);
            switch (name[0] | 0x20)
            {
                case 'a':
                    if (Ascii.EqualsIgnoreCase(name, "authorization"u8)) { req.Authorization = value; req.HasAuthorization = true; }
                    else if (Ascii.EqualsIgnoreCase(name, "accept-encoding"u8)) req.AcceptsGzip = HasToken(value, "gzip"u8);
                    break;
                case 'c':
                    if (Ascii.EqualsIgnoreCase(name, "content-length"u8)) { if (!TryDecimal(value, out length)) return ParseBad; }
                    else if (Ascii.EqualsIgnoreCase(name, "connection"u8))
                        req.KeepAlive = HasToken(value, "close"u8) ? false : HasToken(value, "keep-alive"u8) || req.KeepAlive;
                    break;
                case 'e':
                    if (Ascii.EqualsIgnoreCase(name, "expect"u8)) wantsContinue = HasToken(value, "100-continue"u8);
                    break;
                case 't':
                    if (Ascii.EqualsIgnoreCase(name, "transfer-encoding"u8)) return ParseBad; // chunked bodies are out of scope
                    break;
            }
        }
        if (!sawRequestLine) return ParseBad;

        if (length > MaxBody) return ParseTooLarge;
        int bodyAt = blank + 4, end = bodyAt + (int)length;
        if (data.Length < end)
        {
            needContinue = wantsContinue;
            return ParseIncomplete;
        }
        req.Body = data[bodyAt..end];
        return end;
    }

    // A comma-separated header value contains this token (case-insensitive; parameters after ';' ignored).
    static bool HasToken(ReadOnlySpan<byte> value, ReadOnlySpan<byte> token)
    {
        foreach (Range r in value.Split((byte)','))
        {
            var item = value[r];
            int semi = item.IndexOf((byte)';');
            if (semi >= 0) item = item[..semi];
            if (Ascii.EqualsIgnoreCase(item.Trim(" \t"u8), token)) return true;
        }
        return false;
    }

    // Plain decimal digits only (no sign, no spaces), at most 10 of them.
    static bool TryDecimal(ReadOnlySpan<byte> s, out long v)
    {
        v = 0;
        if (s.IsEmpty || s.Length > 10) return false;
        foreach (byte c in s)
        {
            uint d = (uint)(c - '0');
            if (d > 9) return false;
            v = v * 10 + d;
        }
        return true;
    }

    // ---- responses ----

    // Status line plus the Content-Type every response has, one byte array per status.
    static readonly byte[]?[] heads = BuildHeads();

    static byte[]?[] BuildHeads()
    {
        var table = new byte[]?[600];
        foreach (var (code, reason) in new[] { (100, "Continue"), (200, "OK"), (201, "Created"), (400, "Bad Request"),
                     (401, "Unauthorized"), (404, "Not Found"), (413, "Payload Too Large"), (500, "Internal Server Error"),
                     (503, "Service Unavailable") })
            table[code] = Encoding.ASCII.GetBytes($"HTTP/1.1 {code} {reason}\r\n" + (code == 100 ? "" : "Content-Type: application/json\r\n"));
        return table;
    }

    public static ReadOnlySpan<byte> Continue => "HTTP/1.1 100 Continue\r\n\r\n"u8;

    // The Date header, rebuilt by whichever loop thread first notices a new second.
    [ThreadStatic] static long dateSecond;
    [ThreadStatic] static byte[]? dateHeader;

    static byte[] DateHeader()
    {
        long now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
        if (now != dateSecond || dateHeader is null)
        {
            dateHeader = Encoding.ASCII.GetBytes($"Date: {DateTimeOffset.FromUnixTimeSeconds(now):r}\r\n");
            dateSecond = now;
        }
        return dateHeader;
    }

    // Writes the response head into buf so that it ends exactly at bodyStart; returns where it starts.
    public static int WriteHead(byte[] buf, int bodyStart, int bodyLength, int status, bool gzip, bool close)
    {
        var first = (uint)status < (uint)heads.Length && heads[status] is byte[] known ? known : heads[500]!;
        var date = DateHeader();
        ReadOnlySpan<byte> encoding = gzip ? "Content-Encoding: gzip\r\nVary: Accept-Encoding\r\n"u8 : default;
        ReadOnlySpan<byte> connection = close ? "Connection: close\r\n"u8 : default;
        int digits = 1;
        for (int v = bodyLength; v >= 10; v /= 10) digits++;
        int size = first.Length + date.Length + encoding.Length + connection.Length + "Content-Length: "u8.Length + digits + 4;
        int start = bodyStart - size;
        var to = buf.AsSpan(start, size);
        int at = 0;
        Append(to, ref at, first);
        Append(to, ref at, date);
        Append(to, ref at, encoding);
        Append(to, ref at, connection);
        Append(to, ref at, "Content-Length: "u8);
        for (int v = bodyLength, i = at + digits - 1; i >= at; i--, v /= 10) to[i] = (byte)('0' + v % 10);
        "\r\n\r\n"u8.CopyTo(to[(at + digits)..]);
        return start;

        static void Append(Span<byte> to, ref int at, ReadOnlySpan<byte> part)
        {
            part.CopyTo(to[at..]);
            at += part.Length;
        }
    }

    // Turns a finished JSON body into a full response (gzip when asked and worth it). Returns the buffer to send
    // (rented from ArrayPool; the sender returns it) and the response's offset and length in it.
    public static (byte[] Buf, int Offset, int Length) Finish(ref JsonWriter w, int status, bool acceptsGzip, bool keepAlive)
    {
        byte[] buf = w.Buf;
        int bodyLength = w.BodyLength;
        bool gzipped = false;
        if (acceptsGzip && bodyLength >= Gzip.MinSize)
        {
            var zbuf = ArrayPool<byte>.Shared.Rent(HeaderRoom + Gzip.Bound(bodyLength));
            int zn = Gzip.Compress(buf.AsSpan(HeaderRoom, bodyLength), zbuf, HeaderRoom);
            if (zn > 0)
            {
                ArrayPool<byte>.Shared.Return(buf);
                buf = zbuf;
                bodyLength = zn;
                gzipped = true;
            }
            else ArrayPool<byte>.Shared.Return(zbuf);
        }
        w.Buf = null!;
        int start = WriteHead(buf, HeaderRoom, bodyLength, status, gzipped, !keepAlive);
        return (buf, start, HeaderRoom + bodyLength - start);
    }
}
