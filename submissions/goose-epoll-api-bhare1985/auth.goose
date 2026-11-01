// HS256 JWT check: the signature, the algorithm and a required exp on every request, nothing remembered between requests.
// The claims a token must carry are read by the server (the OpenAPI file says which); this checks the token itself.
namespace auth;
import std;
import crypto;
import jsn;

var key: crypto::Hmac;

let DOT = byte_set(".");
let HEADER_KEYS: const u8[:][1] = ["alg"];
let TIME_KEYS: const u8[:][2] = ["exp", "nbf"];
let EXP = 0;
let NBF = 1;

let MISSING: const u8[:] = "missing bearer token";
let INVALID: const u8[:] = "invalid or expired token";
let BAD_PAYLOAD: const u8[:] = "invalid token payload";

fn set_secret(secret: const u8[:]) { key = crypto::hmac_key(secret); }

// Checks the Authorization header value: the signature, HS256 and exp (and nbf). Returns the token's payload JSON (a slice
// of decoded, which receives the pieces) and "", or an empty payload and the message for the 401.
fn verify(header: u8[:], now: i64, decoded: u8[>..]&) -> u8[:], const u8[:] {
    if !starts_with(header, "Bearer ") { return decoded[0..0], MISSING; }
    let token = header[7..];
    let d1 = find_any(token, DOT);
    if d1 < 0 { return decoded[0..0], INVALID; }
    let d2 = d1 + 1 + find_any(token[d1 + 1..], DOT);
    if d2 <= d1 || find_any(token[d2 + 1..], DOT) >= 0 { return decoded[0..0], INVALID; }
    if token.len - d2 - 1 != 43 { return decoded[0..0], INVALID; }
    if !crypto::b64url_decode(token[d2 + 1..], decoded) || decoded.len != 32 { return decoded[0..0], INVALID; }
    var mac: u8[32] = [0; 32];
    crypto::hmac(key, token[..d2], mac);
    var diff: u8 = 0;
    for i in 32 { diff |= decoded[i] ^ mac[i]; }
    if diff != 0 { return decoded[0..0], INVALID; }
    if d1 > 680 || d2 - d1 - 1 > 2700 { return decoded[0..0], INVALID; }
    if !crypto::b64url_decode(token[..d1], decoded) { return decoded[0..0], INVALID; }
    let payload_at = decoded.len;
    if !crypto::b64url_decode(token[d1 + 1..d2], decoded) { return decoded[0..0], INVALID; }
    if !is_hs256(decoded[32..payload_at]) || !times_ok(decoded[payload_at..], now) { return decoded[0..0], INVALID; }
    return decoded[payload_at..], "";
}

fn is_hs256(json: u8[:]) -> bool {
    var found: jsn::Want[1];
    if !jsn::jdoc(json, HEADER_KEYS[..], found[..]) || found[0].start < 0 || json[found[0].start] != '"' { return false; }
    var alg: u8[>..] = [];
    var at = found[0].start;
    let n, ok = jsn::jstr(json[..found[0].end], at, alg, 8);
    return ok && n == 5 && alg[..] == "HS256";
}

// exp (required) is in the future and nbf (when present) in the past.
fn times_ok(json: u8[:], now: i64) -> bool {
    var found: jsn::Want[2];
    if !jsn::jdoc(json, TIME_KEYS[..], found[..]) || found[EXP].start < 0 { return false; }
    for i in 2 {                                                // exp and nbf: numbers, compared with now
        let span = found[i];
        if span.start < 0 { continue; }
        let first = json[span.start];
        if first != '-' && (first < '0' || first > '9') { return false; }
        if span.end - span.start >= 64 { return false; }
        let limit, ok = parse_flt(json[span.start..span.end]);
        let t = now as f64;
        if i == EXP { if t >= limit { return false; } } else { if t < limit { return false; } }
    }
    return true;
}
