using System.Buffers;
using System.Globalization;
using System.Text;

namespace MillionReactor;

internal sealed record HttpRequest(string Method, string Path, string? Authorization, bool Gzip, bool Close, byte[] Body);

internal static class HttpParser
{
    // Positive bytes consumed, zero incomplete, -1 invalid. Body ownership leaves the input buffer free for reuse.
    public static int Parse(ReadOnlySpan<byte> data, out HttpRequest? request)
    {
        request = null;
        int end = data.IndexOf("\r\n\r\n"u8);
        if (end < 0) return data.Length > 16384 ? -1 : 0;
        if (end > 16384) return -1;
        var headers = data[..end];
        int first = headers.IndexOf("\r\n"u8);
        var line = first < 0 ? headers : headers[..first];
        int space = line.IndexOf((byte)' '), last = line.LastIndexOf((byte)' ');
        if (space <= 0 || last <= space + 1) return -1;
        foreach (byte c in line[..space]) if (!Token(c)) return -1;
        foreach (byte c in line[(space + 1)..last]) if (c <= 32 || c == 127) return -1;
        string method = Encoding.ASCII.GetString(line[..space]);
        string path = Encoding.ASCII.GetString(line[(space + 1)..last]);
        var version = line[(last + 1)..];
        bool close = version.SequenceEqual("HTTP/1.0"u8);
        if (!close && !version.SequenceEqual("HTTP/1.1"u8)) return -1;
        string? authorization = null;
        bool gzip = false, chunked = false, hasLength = false, hasTransfer = false;
        int contentLength = 0;
        var rest = first < 0 ? ReadOnlySpan<byte>.Empty : headers[(first + 2)..];
        while (!rest.IsEmpty)
        {
            int crlf = rest.IndexOf("\r\n"u8);
            var header = crlf < 0 ? rest : rest[..crlf];
            rest = crlf < 0 ? [] : rest[(crlf + 2)..];
            int colon = header.IndexOf((byte)':');
            if (colon <= 0) return -1;
            foreach (byte c in header[..colon]) if (!Token(c)) return -1;
            foreach (byte c in header[(colon + 1)..]) if ((c < 32 && c != 9) || c == 127) return -1;
            string name = Encoding.ASCII.GetString(header[..colon]);
            string value = Encoding.ASCII.GetString(header[(colon + 1)..]).Trim(' ', '\t');
            if (name.Equals("Content-Length", StringComparison.OrdinalIgnoreCase))
            {
                if (hasLength || !int.TryParse(value, NumberStyles.None, CultureInfo.InvariantCulture, out contentLength) || contentLength > 16384) return -1;
                hasLength = true;
            }
            else if (name.Equals("Transfer-Encoding", StringComparison.OrdinalIgnoreCase))
            {
                if (hasTransfer || !value.Equals("chunked", StringComparison.OrdinalIgnoreCase)) return -1;
                hasTransfer = chunked = true;
            }
            else if (name.Equals("Authorization", StringComparison.OrdinalIgnoreCase))
            {
                if (authorization is not null) return -1;
                authorization = value;
            }
            else if (name.Equals("Accept-Encoding", StringComparison.OrdinalIgnoreCase)) gzip = AcceptsGzip(value);
            else if (name.Equals("Connection", StringComparison.OrdinalIgnoreCase))
            {
                foreach (string token in value.Split(','))
                {
                    if (token.Trim().Equals("close", StringComparison.OrdinalIgnoreCase)) close = true;
                    else if (token.Trim().Equals("keep-alive", StringComparison.OrdinalIgnoreCase)) close = false;
                }
            }
        }
        if (chunked && hasLength) return -1;
        int offset = end + 4;
        byte[] body;
        if (!chunked)
        {
            if (data.Length - offset < contentLength) return 0;
            body = contentLength == 0 ? [] : data.Slice(offset, contentLength).ToArray();
            offset += contentLength;
        }
        else
        {
            var chunks = new ArrayBufferWriter<byte>();
            while (true)
            {
                int crlf = data[offset..].IndexOf("\r\n"u8);
                if (crlf < 0) return data.Length - offset > 128 ? -1 : 0;
                var sizeText = data.Slice(offset, crlf);
                int semi = sizeText.IndexOf((byte)';');
                if (semi >= 0) sizeText = sizeText[..semi];
                if (!int.TryParse(Encoding.ASCII.GetString(sizeText), NumberStyles.AllowHexSpecifier, CultureInfo.InvariantCulture, out int size)
                    || size < 0 || size > 16384 - chunks.WrittenCount) return -1;
                offset += crlf + 2;
                if (size == 0)
                {
                    if (data.Length - offset < 2) return 0;
                    if (data.Slice(offset, 2).SequenceEqual("\r\n"u8)) offset += 2;
                    else
                    {
                        int trailers = data[offset..].IndexOf("\r\n\r\n"u8);
                        if (trailers < 0) return data.Length - offset > 16384 ? -1 : 0;
                        offset += trailers + 4;
                    }
                    break;
                }
                if (data.Length - offset < size + 2) return 0;
                if (!data.Slice(offset + size, 2).SequenceEqual("\r\n"u8)) return -1;
                chunks.Write(data.Slice(offset, size));
                offset += size + 2;
            }
            body = chunks.WrittenSpan.ToArray();
        }
        request = new(method, path, authorization, gzip, close, body);
        return offset;
    }
    private static bool Token(byte c) => c is >= (byte)'a' and <= (byte)'z' or >= (byte)'A' and <= (byte)'Z'
        or >= (byte)'0' and <= (byte)'9' || "!#$%&'*+-.^_`|~"u8.Contains(c);
    internal static bool AcceptsGzip(string value)
    {
        if (value == "gzip") return true;
        bool wildcard = false;
        foreach (string item in value.Split(','))
        {
            string[] parts = item.Trim().Split(';');
            double quality = 1;
            foreach (string parameter in parts.Skip(1))
            {
                var text = parameter.Trim();
                if (text.StartsWith("q=", StringComparison.OrdinalIgnoreCase)
                    && (!double.TryParse(text.AsSpan(2), NumberStyles.AllowDecimalPoint, CultureInfo.InvariantCulture, out quality)
                        || quality is < 0 or > 1)) quality = 0;
            }
            if (parts[0].Trim().Equals("gzip", StringComparison.OrdinalIgnoreCase)) return quality > 0;
            if (parts[0].Trim() == "*") wildcard = quality > 0;
        }
        return wildcard;
    }
}
