// Response text: JSON string escaping and the trim JavaScript's String.trim does.
namespace wire;
import std;

// The bytes a JSON string must escape: the quote, the backslash and the control characters.
let ESCAPED = escaped_set();

fn escaped_set() -> ByteSet {
    var members: bool[256] = [false; 256];
    for b in 32 { members[b] = true; }
    members['"'] = true;
    members['\\'] = true;
    return byte_set(members);
}

// Appends s as JSON string contents. find_any scans 16 bytes at a time, and the runs between escapes are copied whole.
fn putjs(out: u8[>..]&, s: u8[:]) {
    var rest = s;
    loop {
        let k = find_any(rest, ESCAPED);
        if k < 0 { out.append(rest); return; }
        out.append(rest[..k]);
        put_escape(out, rest[k]);
        rest = rest[k + 1..];
    }
}

fn put_escape(out: u8[>..]&, c: u8) {
    match c {
        '"' => { out.append("\\\""); },
        '\\' => { out.append("\\\\"); },
        '\n' => { out.append("\\n"); },
        '\r' => { out.append("\\r"); },
        '\t' => { out.append("\\t"); },
        _ => {
            out.append("\\u00");
            out.push("0123456789abcdef"[(c >> 4) as i64]);
            out.push("0123456789abcdef"[(c & 15) as i64]);
        },
    }
}

// Byte length of the JS String.trim whitespace at s[p], or 0.
fn ws_at(s: u8[:], p: i64) -> i64 {
    let c = s[p];
    if c == ' ' || (c >= 9 && c <= 13) { return 1; }
    let left = s.len - p;
    if left >= 2 && c == 0xc2 && s[p + 1] == 0xa0 { return 2; }
    if left >= 3 {
        let u = (c as u32) << 16 | (s[p + 1] as u32) << 8 | s[p + 2];
        if u == 0xe19a80 || (u >= 0xe28080 && u <= 0xe2808a) || u == 0xe280a8 || u == 0xe280a9 || u == 0xe280af
            || u == 0xe2819f || u == 0xe38080 || u == 0xefbbbf { return 3; }
    }
    return 0;
}

// Byte length of the JS String.trim whitespace ending at s[e] (exclusive), or 0.
fn ws_before(s: u8[:], e: i64) -> i64 {
    let c = s[e - 1];
    if c == ' ' || (c >= 9 && c <= 13) { return 1; }
    if e >= 2 && s[e - 2] == 0xc2 && c == 0xa0 { return 2; }
    if e >= 3 && ws_at(s[..e], e - 3) == 3 { return 3; }
    return 0;
}

// The sub-slice of s without JS-trim whitespace at both ends.
fn jstrim(s: u8[:]) -> u8[:] {
    var a: i64 = 0;
    var e = s.len;
    loop { if a >= e { break; } let l = ws_at(s[..e], a); if l == 0 { break; } a += l; }
    loop { if e <= a { break; } let l = ws_before(s[a..e], e - a); if l == 0 { break; } e -= l; }
    return s[a..e];
}
