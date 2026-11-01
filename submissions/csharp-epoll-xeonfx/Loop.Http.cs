using System;
using System.Buffers;
using System.Buffers.Text;
using System.Text;

/// <summary>What the routes need from a request, sliced from the bytes it arrived in.</summary>
ref struct HttpRequest
{
    public ReadOnlySpan<byte> Method, Path, Authorization, Body;
    public bool HasAuthorization, Close, Gzip;
}

/// <summary>The clients: accepting, reading and parsing requests, and sending responses.</summary>
sealed partial class Loop
{
    void OnListener(int i)
    {
        int listener = (int)_listeners[i].Handle;
        for (int k = 0; k < MaxAccepts; k++)
        {
            int fd = Native.Accept(listener);
            if (fd < 0) return;
            if (fd >= Conns.Length)
            {
                Native.close(fd);
                continue;
            }
            ref Conn c = ref Conns[fd];
            c.Busy = c.CloseAfterSend = c.PeerDone = c.Paused = c.Gzip = false;
            c.InLength = c.OutStart = c.OutEnd = 0;
            Native.Watch(_epfd, Native.EpollCtlAdd, fd, ClientEvents, (ulong)fd);
        }
    }

    void OnClient(int fd, uint events)
    {
        ref Conn c = ref Conns[fd];
        if ((events & Native.EpollErr) != 0)
        {
            Close(fd);
            return;
        }
        if ((events & Native.EpollOut) != 0 && !SendPending(fd)) return;
        if ((events & (Native.EpollIn | Native.EpollRdHup | Native.EpollHup)) == 0) return;

        if (c.Busy || c.OutEnd > 0)
        {
            // keep what arrives until the answer before it is out; a client this far ahead waits
            if (c.InLength == MaxRequest)
            {
                c.Paused = true;
                Arm(fd);
                return;
            }
            c.In ??= ArrayPool<byte>.Shared.Rent(MaxRequest);
            int got = Native.Receive(fd, c.In, c.InLength, MaxRequest - c.InLength);
            if (got == -2) Close(fd);
            else if (got > 0) c.InLength += got;
            else if (got == 0)
            {
                // stop reading; what it asked is still answered
                c.PeerDone = true;
                Arm(fd);
            }
            return;
        }

        // the usual case: the loop's receive buffer, behind what was held back from the last read
        int held = c.InLength;
        if (held > 0) c.In.AsSpan(0, held).CopyTo(_receive);
        int n = Native.Receive(fd, _receive, held, MaxRequest - held);
        if (n == 0 || n == -2 || (n == -1 && held == MaxRequest))
        {
            Close(fd);
            return;
        }
        if (n > 0) held += n;
        Serve(fd, _receive, held);
    }

    /// <summary>Answers the requests in <paramref name="buffer"/> until one waits on the database or one is incomplete;
    /// what is left waits in the connection's <see cref="Conn.In"/>.</summary>
    void Serve(int fd, byte[] buffer, int length)
    {
        int at = 0;
        int gen = Conns[fd].Gen;
        while (at < length)
        {
            int used = ServeOne(fd, buffer.AsSpan(at, length - at));
            if (used == 0) break;
            if (used < 0 || Conns[fd].Gen != gen) return; // closed
            at += used;
            if (Conns[fd].Busy || Conns[fd].OutEnd > 0) break;
        }
        ref Conn c = ref Conns[fd];
        int left = length - at;
        if (left > 0)
        {
            if (buffer != c.In)
            {
                c.In ??= ArrayPool<byte>.Shared.Rent(MaxRequest);
                buffer.AsSpan(at, left).CopyTo(c.In);
            }
            else if (at > 0) buffer.AsSpan(at, left).CopyTo(c.In);
            c.InLength = left;
        }
        else
        {
            c.InLength = 0;
            if (c.In != null)
            {
                ArrayPool<byte>.Shared.Return(c.In);
                c.In = null;
            }
        }
        if (c.PeerDone && !c.Busy && c.OutEnd == 0) Close(fd);
        else if (c.Paused && c.InLength < MaxRequest)
        {
            c.Paused = false;
            Arm(fd);
        }
    }

    /// <summary>Handles the request at the start of <paramref name="bytes"/>: the bytes it took, 0 when it isn't all
    /// here yet, -1 when the connection closed.</summary>
    int ServeOne(int fd, ReadOnlySpan<byte> bytes)
    {
        int gen = Conns[fd].Gen;
        int total = Parse(bytes, out HttpRequest request);
        if (total == 0) return bytes.Length >= MaxRequest ? BadRequest(fd) : 0;
        if (total < 0) return BadRequest(fd);
        if (request.Close) Conns[fd].CloseAfterSend = true;
        Conns[fd].Gzip = request.Gzip;
        Route(fd, request.Method, request.Path, request.HasAuthorization, request.Authorization, request.Body);
        return Conns[fd].Gen == gen ? total : -1;
    }

    /// <summary>Reads the request at the start of <paramref name="bytes"/>: the bytes it takes, 0 when it isn't all
    /// here yet, -1 when it is malformed.</summary>
    public static int Parse(ReadOnlySpan<byte> bytes, out HttpRequest request)
    {
        request = default;
        int headEnd = bytes.IndexOf("\r\n\r\n"u8);
        if (headEnd < 0) return 0;
        ReadOnlySpan<byte> head = bytes[..headEnd];

        int lineEnd = head.IndexOf("\r\n"u8);
        ReadOnlySpan<byte> line = lineEnd < 0 ? head : head[..lineEnd];
        int firstSpace = line.IndexOf((byte)' ');
        int lastSpace = line.LastIndexOf((byte)' ');
        if (firstSpace <= 0 || lastSpace <= firstSpace) return -1;
        ReadOnlySpan<byte> target = line[(firstSpace + 1)..lastSpace];
        request.Method = line[..firstSpace];
        request.Close = !line[(lastSpace + 1)..].SequenceEqual("HTTP/1.1"u8); // HTTP/1.0 closes unless asked not to

        int contentLength = 0;
        ReadOnlySpan<byte> headers = lineEnd < 0 ? default : head[(lineEnd + 2)..];
        while (!headers.IsEmpty)
        {
            int end = headers.IndexOf("\r\n"u8);
            ReadOnlySpan<byte> header = end < 0 ? headers : headers[..end];
            headers = end < 0 ? default : headers[(end + 2)..];
            int colon = header.IndexOf((byte)':');
            if (colon <= 0) continue;
            ReadOnlySpan<byte> name = header[..colon], value = header[(colon + 1)..].Trim(" \t"u8);
            if (Ascii.EqualsIgnoreCase(name, "authorization"u8))
            {
                request.Authorization = value;
                request.HasAuthorization = true;
            }
            else if (Ascii.EqualsIgnoreCase(name, "content-length"u8))
            {
                if (!Utf8Parser.TryParse(value, out contentLength, out int used) || used != value.Length || contentLength < 0) return -1;
            }
            else if (Ascii.EqualsIgnoreCase(name, "connection"u8))
            {
                if (Ascii.EqualsIgnoreCase(value, "close"u8)) request.Close = true;
                else if (Ascii.EqualsIgnoreCase(value, "keep-alive"u8)) request.Close = false;
            }
            else if (Ascii.EqualsIgnoreCase(name, "accept-encoding"u8)) request.Gzip |= AcceptsGzip(value);
            // a chunked body isn't read: the request counts as malformed
            else if (Ascii.EqualsIgnoreCase(name, "transfer-encoding"u8)) return -1;
        }
        int total = headEnd + 4 + contentLength;
        if (total > MaxRequest) return -1;
        if (bytes.Length < total) return 0;
        request.Body = bytes.Slice(headEnd + 4, contentLength);

        int query = target.IndexOf((byte)'?');
        request.Path = query >= 0 ? target[..query] : target;
        return total;
    }

    /// <summary>Whether an Accept-Encoding value takes gzip: <c>gzip</c> or <c>x-gzip</c> among its codings, without
    /// <c>q=0</c>. A bare <c>*</c> isn't taken as gzip.</summary>
    internal static bool AcceptsGzip(ReadOnlySpan<byte> value)
    {
        while (!value.IsEmpty)
        {
            int comma = value.IndexOf((byte)',');
            ReadOnlySpan<byte> item = comma < 0 ? value : value[..comma];
            value = comma < 0 ? default : value[(comma + 1)..];
            int semicolon = item.IndexOf((byte)';');
            ReadOnlySpan<byte> coding = (semicolon < 0 ? item : item[..semicolon]).Trim(" \t"u8);
            if (!Ascii.EqualsIgnoreCase(coding, "gzip"u8) && !Ascii.EqualsIgnoreCase(coding, "x-gzip"u8)) continue;
            if (semicolon < 0 || !QualityZero(item[(semicolon + 1)..])) return true;
        }
        return false;
    }

    /// <summary>Whether a coding's parameters hold <c>q=0</c> (or 0., 0.0, 0.00, 0.000).</summary>
    static bool QualityZero(ReadOnlySpan<byte> parameters)
    {
        while (!parameters.IsEmpty)
        {
            int semicolon = parameters.IndexOf((byte)';');
            ReadOnlySpan<byte> p = (semicolon < 0 ? parameters : parameters[..semicolon]).Trim(" \t"u8);
            parameters = semicolon < 0 ? default : parameters[(semicolon + 1)..];
            if (p.Length < 3 || (p[0] | 0x20) != 'q' || p[1] != '=') continue;
            ReadOnlySpan<byte> q = p[2..];
            if (q[0] != '0') return false;
            return q.Length == 1 || (q[1] == '.' && q.Length <= 5 && q[2..].IndexOfAnyExcept((byte)'0') < 0);
        }
        return false;
    }

    /// <summary>Answers 400 and closes after it; -1, as <see cref="ServeOne"/> returns for a closed connection.</summary>
    int BadRequest(int fd)
    {
        Conns[fd].CloseAfterSend = true;
        Error(fd, 400, "bad request"u8);
        return -1;
    }

    /// <summary>Watches a client for what it can do now: read unless paused or done, write while output waits.</summary>
    void Arm(int fd)
    {
        ref Conn c = ref Conns[fd];
        uint events = (c.PeerDone || c.Paused ? 0 : ClientEvents) | (c.OutEnd > 0 ? Native.EpollOut : 0);
        Native.Watch(_epfd, Native.EpollCtlMod, fd, events, (ulong)fd);
    }

    /// <summary>After an answer to a request that waited, serves what the client sent meanwhile.</summary>
    void Resume(int fd, int gen)
    {
        ref Conn c = ref Conns[fd];
        if (c.Gen != gen || c.Busy || c.OutEnd > 0) return;
        if (c.InLength > 0) Serve(fd, c.In!, c.InLength);
        else if (c.PeerDone) Close(fd);
    }

    void Error(int fd, int status, ReadOnlySpan<byte> message)
    {
        int at = HeadRoom;
        Json.Raw(_response, ref at, "{\"error\":"u8);
        Json.String(_response, ref at, message);
        _response[at++] = (byte)'}';
        Respond(fd, status, at);
    }

    /// <summary>The /health answer while the database can't be reached.</summary>
    void Unhealthy(int fd, ReadOnlySpan<byte> message)
    {
        int at = HeadRoom;
        Json.Raw(_response, ref at, "{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":"u8);
        Json.String(_response, ref at, message);
        _response[at++] = (byte)'}';
        Respond(fd, 503, at);
    }

    /// <summary>Sends the body at [HeadRoom, end) of the response buffer with its head, gzipped when the client takes
    /// it and the body is past <see cref="GzipFrom"/>; what the socket won't take waits in the connection's Out.</summary>
    void Respond(int fd, int status, int end)
    {
        ref Conn c = ref Conns[fd];
        byte[] b = _response;
        bool gzip = false;
        int length = end - HeadRoom;
        if (c.Gzip && length > GzipFrom && length <= GzipMax)
        {
            int packed = _gzip.Compress(_response.AsSpan(HeadRoom, length), _gzipped.AsSpan(HeadRoom));
            if (packed > 0 && packed < length)
            {
                b = _gzipped;
                end = HeadRoom + packed;
                gzip = true;
            }
        }
        int start = Head(b, status, end - HeadRoom, c.CloseAfterSend, gzip);
        int sent = Native.Send(fd, b, start, end - start);
        if (sent < 0)
        {
            Close(fd);
            return;
        }
        if (sent == end - start)
        {
            if (c.CloseAfterSend) Close(fd);
            return;
        }
        // the socket is full: keep the rest and wait until it drains
        int rest = end - start - sent;
        c.Out = ArrayPool<byte>.Shared.Rent(rest);
        b.AsSpan(start + sent, rest).CopyTo(c.Out);
        c.OutStart = 0;
        c.OutEnd = rest;
        Arm(fd);
    }

    /// <summary>Writes the head of a response whose body of <paramref name="length"/> bytes starts at HeadRoom right in
    /// front of it, back to front; returns where it starts.</summary>
    internal static int Head(byte[] b, int status, int length, bool close, bool gzip = false)
    {
        ReadOnlySpan<byte> tail = close ? "\r\nConnection: close\r\n\r\n"u8 : "\r\n\r\n"u8;
        int start = HeadRoom - tail.Length;
        tail.CopyTo(b.AsSpan(start));
        do
        {
            b[--start] = (byte)('0' + length % 10);
            length /= 10;
        } while (length > 0);
        ReadOnlySpan<byte> type = gzip
            ? "Content-Type: application/json\r\nContent-Encoding: gzip\r\nContent-Length: "u8
            : "Content-Type: application/json\r\nContent-Length: "u8;
        start -= type.Length;
        type.CopyTo(b.AsSpan(start));
        ReadOnlySpan<byte> line = status switch
        {
            200 => "HTTP/1.1 200 OK\r\n"u8,
            201 => "HTTP/1.1 201 Created\r\n"u8,
            400 => "HTTP/1.1 400 Bad Request\r\n"u8,
            401 => "HTTP/1.1 401 Unauthorized\r\n"u8,
            404 => "HTTP/1.1 404 Not Found\r\n"u8,
            503 => "HTTP/1.1 503 Service Unavailable\r\n"u8,
            _ => "HTTP/1.1 500 Internal Server Error\r\n"u8,
        };
        start -= line.Length;
        line.CopyTo(b.AsSpan(start));
        return start;
    }

    /// <summary>Sends what waits in Out. False when the connection closed.</summary>
    bool SendPending(int fd)
    {
        ref Conn c = ref Conns[fd];
        if (c.OutEnd == 0) return true;
        int sent = Native.Send(fd, c.Out!, c.OutStart, c.OutEnd - c.OutStart);
        if (sent < 0)
        {
            Close(fd);
            return false;
        }
        c.OutStart += sent;
        if (c.OutStart < c.OutEnd) return true;
        ArrayPool<byte>.Shared.Return(c.Out!);
        c.Out = null;
        c.OutStart = c.OutEnd = 0;
        if (c.CloseAfterSend)
        {
            Close(fd);
            return false;
        }
        Arm(fd);
        Resume(fd, c.Gen);
        return Conns[fd].Gen == c.Gen;
    }

    void Close(int fd)
    {
        ref Conn c = ref Conns[fd];
        c.Gen++;
        c.Busy = c.CloseAfterSend = c.PeerDone = c.Paused = c.Gzip = false;
        if (c.In != null) ArrayPool<byte>.Shared.Return(c.In);
        if (c.Out != null) ArrayPool<byte>.Shared.Return(c.Out);
        c.In = c.Out = null;
        c.InLength = c.OutStart = c.OutEnd = 0;
        Native.close(fd); // leaves the epoll set with it
    }
}
