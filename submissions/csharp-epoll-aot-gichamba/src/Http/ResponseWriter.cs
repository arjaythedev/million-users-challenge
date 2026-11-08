using Feed.Json;

namespace Feed.Http;

/// <summary>
/// Builds one HTTP response in a single buffer. The JSON body is written after a reserved
/// headroom, and <see cref="Finish"/> writes the headers into the space just in front of it,
/// so the whole response is contiguous and goes out in one send. A gzipped response is built the same way in a second
/// buffer.
/// </summary>
public sealed class ResponseWriter {
    /// <summary>Bytes reserved in front of the body; larger than the longest header block.</summary>
    public const int Headroom = 192;

    /// <summary>Writers whose buffer has grown past this many bytes are dropped, not pooled.</summary>
    private const int MaxPooledBytes = 1 << 20;
    /// <summary>Most idle writers kept in each thread's pool.</summary>
    private const int MaxPooled = 1024;
    /// <summary>
    /// This thread's idle writers. A request rents and returns its writer on its loop's thread, so a pool per thread
    /// needs no lock.
    /// </summary>
    [ThreadStatic]
    private static Stack<ResponseWriter>? t_pool;

    /// <summary>Holds the headers and gzipped body of a compressed response; grown on demand.</summary>
    private byte[] _zip = [];

    /// <summary>Creates a writer with a body ready to write.</summary>
    public ResponseWriter() {
        Body = new JsonWriter(4096);
        Body.Reset(Headroom);
    }

    /// <summary>The JSON body writer. Write the body here before calling <see cref="Finish"/>.</summary>
    public JsonWriter Body { get; }

    /// <summary>Bytes of JSON body written so far.</summary>
    public int BodyLength => Body.Length - Headroom;

    /// <summary>Takes a reset writer from this thread's pool, or makes one.</summary>
    public static ResponseWriter Rent() {
        if (t_pool is { } pool && pool.TryPop(out ResponseWriter? w)) {
            w.Body.Reset(Headroom);
            return w;
        }
        return new ResponseWriter();
    }

    /// <summary>Returns a writer to this thread's pool. Do not touch it or its Finish result afterwards.</summary>
    public static void Return(ResponseWriter w) {
        if (w.Body.Buffer.Length > MaxPooledBytes || w._zip.Length > MaxPooledBytes) {
            return;
        }

        Stack<ResponseWriter> pool = t_pool ??= new Stack<ResponseWriter>();
        if (pool.Count < MaxPooled) {
            pool.Push(w);
        }
    }

    /// <summary>Replaces the body with {"error":"message"}.</summary>
    public void WriteError(ReadOnlySpan<byte> message) {
        Body.Reset(Headroom);
        Body.StartObject();
        Body.PropertyName("error"u8);
        Body.WriteString(message);
        Body.EndObject();
    }

    /// <summary>
    /// Writes the status line and headers in front of the body and returns the whole response, with
    /// Content-Type: application/json and an exact Content-Length. With <paramref name="gzip"/> the body is gzipped and
    /// marked with Content-Encoding. The span is valid until the writer is reset or returned.
    /// </summary>
    public ReadOnlySpan<byte> Finish(int status, bool gzip = false) {
        byte[] buf = Body.Buffer;
        int bodyLen = BodyLength;
        if (gzip) {
            int need = Headroom + Gzip.Bound(bodyLen);
            if (_zip.Length < need) {
                _zip = new byte[need];
            }

            bodyLen = Gzip.Compress(buf.AsSpan(Headroom, bodyLen), _zip.AsSpan(Headroom));
            buf = _zip;
        }

        Span<byte> head = stackalloc byte[Headroom];
        int n = 0;
        Append(head, ref n, StatusLine(status));
        Append(head, ref n, gzip
            ? "Content-Type: application/json\r\nContent-Encoding: gzip\r\nContent-Length: "u8
            : "Content-Type: application/json\r\nContent-Length: "u8);
        bodyLen.TryFormat(head[n..], out int w, default, System.Globalization.CultureInfo.InvariantCulture);
        n += w;
        Append(head, ref n, "\r\n\r\n"u8);

        int start = Headroom - n;
        head[..n].CopyTo(buf.AsSpan(start));
        return new ReadOnlySpan<byte>(buf, start, n + bodyLen);
    }

    /// <summary>Copies <paramref name="src"/> into <paramref name="dst"/> at offset <paramref name="n"/> and advances <paramref name="n"/>.</summary>
    private static void Append(Span<byte> dst, ref int n, ReadOnlySpan<byte> src) {
        src.CopyTo(dst[n..]);
        n += src.Length;
    }

    /// <summary>The HTTP/1.1 status line for a code; a code not listed gets the 500 line.</summary>
    private static ReadOnlySpan<byte> StatusLine(int status) => status switch {
        200 => "HTTP/1.1 200 OK\r\n"u8,
        201 => "HTTP/1.1 201 Created\r\n"u8,
        400 => "HTTP/1.1 400 Bad Request\r\n"u8,
        401 => "HTTP/1.1 401 Unauthorized\r\n"u8,
        404 => "HTTP/1.1 404 Not Found\r\n"u8,
        503 => "HTTP/1.1 503 Service Unavailable\r\n"u8,
        _ => "HTTP/1.1 500 Internal Server Error\r\n"u8,
    };
}
