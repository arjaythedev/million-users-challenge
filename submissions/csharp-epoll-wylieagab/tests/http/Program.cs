using System.Text;
using MillionReactor;

int checks = 0;
void Check(bool ok, string message) { checks++; if (!ok) throw new Exception(message); }
void Complete(string text, string body, bool gzip = false, bool close = false)
{
    byte[] data = Encoding.UTF8.GetBytes(text);
    for (int split = 0; split < data.Length; split++)
        Check(HttpParser.Parse(data.AsSpan(0, split), out _) == 0, $"Incomplete split {split}: {text}");
    Check(HttpParser.Parse(data, out var request) == data.Length, "Consumed exactly one request");
    Check(Encoding.UTF8.GetString(request!.Body) == body, "Body bytes");
    Check(request.Gzip == gzip && request.Close == close, "Negotiation/connection");
    byte[] pipelined = [..data, .."GET /feed HTTP/1.1\r\nHost: test\r\n\r\n"u8];
    Check(HttpParser.Parse(pipelined, out _) == data.Length, "Do not consume next pipelined request");
}
Complete("GET /feed HTTP/1.1\r\nHost: example\r\nAccept-Encoding: gzip\r\n\r\n", "", gzip:true);
Complete("GET /feed HTTP/1.0\r\n\r\n", "", close:true);
Complete("POST /posts HTTP/1.1\r\nContent-Length: 14\r\n\r\n{\"body\":\"hey\"}", "{\"body\":\"hey\"}");
Complete("POST /posts HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n7\r\n{\"body\"\r\n7;ext=yes\r\n:\"hey\"}\r\n0\r\n\r\n", "{\"body\":\"hey\"}");
Complete("POST /posts HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\nTrailer: ignored\r\n\r\n", "");
foreach (string headers in new[]{"Content-Length: -1", "Content-Length: +1", "Content-Length: 1\r\nContent-Length: 1",
    "Transfer-Encoding: chunked\r\nContent-Length: 0", "Transfer-Encoding: gzip", "Content-Length : 10",
    "Transfer-Encoding: chunked\r\nTransfer-Encoding: chunked", "Authorization: a\r\nAuthorization: b", "bad header: x"})
    Check(HttpParser.Parse(Encoding.ASCII.GetBytes($"POST /posts HTTP/1.1\r\n{headers}\r\n\r\n"), out _) == -1, headers);
foreach (var test in new[]{("gzip",true), ("GZip",true), ("gzip;q=0",false), ("br, gzip;q=0.1",true), ("*;q=1,gzip;q=0",false),
    ("*;q=0",false),("*;q=0.1",true),("gzip;q=2",false),("gzip;q=bad",false),("br",false)})
    Check(HttpParser.AcceptsGzip(test.Item1)==test.Item2, test.Item1);
Console.WriteLine($"HTTP parser: {checks} checks passed");
