// Run: goose tests/wire_test.goose   (from the goose-epoll folder; modules are found next to the test's parent)
import wire;
import http;
import jsn;

// Parses a copy of text, as the server parses its read buffer.
fn parse_and_show(text: const u8[:]) {
    var buf: u8[>..] = [];
    buf.append(text);
    let r, parsed = http::parse(buf[..]);
    print(parsed, ": ", r.method, "|", r.target, "|", r.authorization, "|", r.body, "|", r.size, " close=", r.close, " gzip=", r.gzip);
}

fn main() {
    var o: u8[>..] = [];
    wire::putjs(o, "a\"b\\c\n\x01z"); print(o);
    o.clear(); wire::putjs(o, "plain text, long enough to span more than one sixteen byte block, no escapes"); print(o);
    print(http::parse_id("123"), " ", http::parse_id("99999999999999999999"), " ", http::parse_id("0"), " ", http::parse_id("1a"));
    print(wire::jstrim("   hi  ").len, " ", http::accepts_gzip("gzip, deflate"), http::accepts_gzip("gzip;q=0"), http::accepts_gzip("br"));

    // http::parse
    parse_and_show("GET /feed?x=1 HTTP/1.1\r\nHost: h\r\nAccept-Encoding: gzip\r\nAuthorization:  Bearer t \r\n\r\nNEXT");
    parse_and_show("POST /posts HTTP/1.0\r\ncontent-length: 5\r\nConnection: keep-alive\r\n\r\nhello!");
    parse_and_show("POST /posts HTTP/1.1\r\nContent-Length: 5\r\n\r\nhel");
    parse_and_show("POST /posts HTTP/1.1\r\nContent-Length: 5\r\nExpect: 100-continue\r\n\r\n");
    parse_and_show("GET /feed HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n");
    parse_and_show("GET /feed\r\n\r\n");
    parse_and_show("GET /feed HTTP/1.1\r\nHost");

    // jsn::jstr: length, well-formedness, cap
    var s: u8[>..] = [];
    var at: i64 = 0;
    let n, ok = jsn::jstr("\"a\\u00e9\\n\\ud83d\\ude00z\" tail", at, s, 100);
    print(n, " ", ok, " ", at, " ", s);
    s.clear(); at = 0;
    let n2, ok2 = jsn::jstr("\"abcdefghij\"", at, s, 4);
    print(n2, " ", ok2, " [", s, "]");
    at = 0;
    let n3, ok3 = jsn::jstr("\"bad \\q\"", at, s, 100);
    print(ok3);
}
