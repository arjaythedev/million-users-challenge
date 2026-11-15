// Strict RFC 8259 validation and string decoding. Positions are indexes into the slice being parsed.
namespace jsn;
import std;

inline fn ws(s: u8[:], p: i64&) {
    while p < s.len && (s[p] == ' ' || s[p] == '\t' || s[p] == '\n' || s[p] == '\r') { p++; }
}

fn hexv(c: u8) -> i64 {
    if c >= '0' && c <= '9' { return c - '0'; }
    if c >= 'a' && c <= 'f' { return (c - 'a') + 10; }
    if c >= 'A' && c <= 'F' { return (c - 'A') + 10; }
    return -1;
}

// Length of the one valid UTF-8 sequence at s[p], 0 if invalid.
fn utf8_len(s: u8[:], p: i64) -> i64 {
    let c = s[p];
    let left = s.len - p;
    if c < 0x80 { return 1; }
    if c >= 0xc2 && c <= 0xdf { return if left >= 2 && (s[p + 1] & 0xc0) == 0x80 { 2 } else { 0 }; }
    if c >= 0xe0 && c <= 0xef {
        if left < 3 || (s[p + 1] & 0xc0) != 0x80 || (s[p + 2] & 0xc0) != 0x80 { return 0; }
        if (c == 0xe0 && s[p + 1] < 0xa0) || (c == 0xed && s[p + 1] > 0x9f) { return 0; }
        return 3;
    }
    if c >= 0xf0 && c <= 0xf4 {
        if left < 4 || (s[p + 1] & 0xc0) != 0x80 || (s[p + 2] & 0xc0) != 0x80 || (s[p + 3] & 0xc0) != 0x80 { return 0; }
        if (c == 0xf0 && s[p + 1] < 0x90) || (c == 0xf4 && s[p + 1] > 0x8f) { return 0; }
        return 4;
    }
    return 0;
}

// What ends a run of plain ASCII inside a string: the quote, the backslash, a control character, or a byte that starts
// a multi-byte UTF-8 sequence (which has to be validated).
let STRING_STOP = string_stop_set();

fn string_stop_set() -> ByteSet {
    var members: bool[256] = [false; 256];
    for b in 256 { members[b] = b < 0x20 || b >= 0x80 || b == '"' || b == '\\'; }
    return byte_set(members);
}

// Appends bytes to out as long as the decoded text stays within cap, and counts them in n either way.
inline fn add(out: u8[>..]&, n: i64&, cap: i64, bytes: u8[:]) {
    if n + bytes.len <= cap { out.append(bytes); }
    n += bytes.len;
}

// Parses the string at s[p] (its opening quote) and appends its decoded UTF-8 to out, as long as that stays within cap
// bytes. Returns the decoded length (more than cap if it did not fit, and then out holds only a prefix) and whether
// the string is well-formed. p ends after the closing quote. With cap 0 nothing is appended: the string is only checked.
fn jstr(s: u8[:], p: i64&, out: u8[>..]&, cap: i64) -> i64, bool {
    var i = p + 1;
    var n: i64 = 0;
    loop {
        let plain = find_any(s[i..], STRING_STOP);
        if plain < 0 { return n, false; }                          // no closing quote
        add(out, n, cap, s[i..i + plain]);
        i += plain;
        let c = s[i];
        if c == '"' { p = i + 1; return n, true; }
        if c < 0x20 { return n, false; }
        if c != '\\' {                                              // the start of a multi-byte sequence
            let l = utf8_len(s, i);
            if l == 0 { return n, false; }
            add(out, n, cap, s[i..i + l]);
            i += l;
            continue;
        }
        i++;                                                        // an escape
        if i >= s.len { return n, false; }
        var ch: u8 = 0;
        match s[i] {
            '"' => { ch = '"'; },
            '\\' => { ch = '\\'; },
            '/' => { ch = '/'; },
            'b' => { ch = 8; },
            'f' => { ch = 12; },
            'n' => { ch = '\n'; },
            'r' => { ch = '\r'; },
            't' => { ch = '\t'; },
            'u' => {
                if s.len - i < 5 { return n, false; }
                let a = hexv(s[i + 1]); let b = hexv(s[i + 2]); let c2 = hexv(s[i + 3]); let d = hexv(s[i + 4]);
                if (a | b | c2 | d) < 0 { return n, false; }
                var cp: i64 = a << 12 | b << 8 | c2 << 4 | d;
                i += 4;
                if cp >= 0xd800 && cp <= 0xdbff && s.len - i >= 7 && s[i + 1] == '\\' && s[i + 2] == 'u' {   // a surrogate pair
                    let a2 = hexv(s[i + 3]); let b2 = hexv(s[i + 4]); let c3 = hexv(s[i + 5]); let d2 = hexv(s[i + 6]);
                    if (a2 | b2 | c3 | d2) < 0 { return n, false; }
                    let low = a2 << 12 | b2 << 8 | c3 << 4 | d2;
                    if low >= 0xdc00 && low <= 0xdfff { cp = 0x10000 + ((cp - 0xd800) << 10) + (low - 0xdc00); i += 6; }
                }
                if cp >= 0xd800 && cp <= 0xdfff { cp = 0xfffd; }   // a lone surrogate: U+FFFD, as JS TextEncoder does
                let size: i64 = if cp < 0x80 { 1 } else if cp < 0x800 { 2 } else if cp < 0x10000 { 3 } else { 4 };
                if n + size <= cap { push_utf8(out, cp); }
                n += size;
                i++;
                continue;
            },
            _ => { return n, false; },
        }
        if n < cap { out.push(ch); }
        n++;
        i++;
    }
}

fn jnumber(s: u8[:], p: i64&) -> bool {
    var i = p;
    if i < s.len && s[i] == '-' { i++; }
    if i >= s.len { return false; }
    if s[i] == '0' { i++; }
    else if s[i] >= '1' && s[i] <= '9' { while i < s.len && s[i] >= '0' && s[i] <= '9' { i++; } }
    else { return false; }
    if i < s.len && s[i] == '.' {
        i++;
        if i >= s.len || s[i] < '0' || s[i] > '9' { return false; }
        while i < s.len && s[i] >= '0' && s[i] <= '9' { i++; }
    }
    if i < s.len && (s[i] == 'e' || s[i] == 'E') {
        i++;
        if i < s.len && (s[i] == '+' || s[i] == '-') { i++; }
        if i >= s.len || s[i] < '0' || s[i] > '9' { return false; }
        while i < s.len && s[i] >= '0' && s[i] <= '9' { i++; }
    }
    p = i;
    return true;
}

fn jlit(s: u8[:], p: i64&, lit: u8[:]) -> bool {
    if s.len - p < lit.len { return false; }
    for k in lit.len { if s[p + k] != lit[k] { return false; } }
    p += lit.len;
    return true;
}

var scratch: u8[>..] = [];   // jstr with cap 0 never appends to it; it only has to be passed

// Validates and skips one value.
recursive fn jvalue(s: u8[:], p: i64&, depth: i64) -> bool {
    if depth > 64 { return false; }
    ws(s, p);
    if p >= s.len { return false; }
    let c = s[p];
    if c == '"' { let n, ok = jstr(s, p, scratch, 0); return ok; }
    if c == '{' {
        p++; ws(s, p);
        if p < s.len && s[p] == '}' { p++; return true; }
        loop {
            ws(s, p);
            if p >= s.len || s[p] != '"' { return false; }
            let n, ok = jstr(s, p, scratch, 0);
            if !ok { return false; }
            ws(s, p);
            if p >= s.len || s[p] != ':' { return false; }
            p++;
            if !jvalue(s, p, depth + 1) { return false; }
            ws(s, p);
            if p >= s.len { return false; }
            if s[p] == ',' { p++; continue; }
            if s[p] == '}' { p++; return true; }
            return false;
        }
    }
    if c == '[' {
        p++; ws(s, p);
        if p < s.len && s[p] == ']' { p++; return true; }
        loop {
            if !jvalue(s, p, depth + 1) { return false; }
            ws(s, p);
            if p >= s.len { return false; }
            if s[p] == ',' { p++; continue; }
            if s[p] == ']' { p++; return true; }
            return false;
        }
    }
    if c == 't' { return jlit(s, p, "true"); }
    if c == 'f' { return jlit(s, p, "false"); }
    if c == 'n' { return jlit(s, p, "null"); }
    return jnumber(s, p);
}

// The raw span of the last value of a wanted top-level key.
struct Want { start: i64, end: i64 }   // start < 0: key absent

// Validates a whole document. For a top-level object it records the raw span of the last value of each wanted
// key (the last duplicate wins, as in JSON.parse). Returns false if the document is invalid.
fn jdoc(s: u8[:], keys: const u8[:][:], found: Want[:]) -> bool {
    assert(found.len == keys.len);
    for i in found.len { found[i] = Want { start: -1, end: -1 }; }
    var p: i64 = 0;
    var key: u8[>..] = [];
    ws(s, p);
    if p < s.len && s[p] == '{' {
        p++; ws(s, p);
        if p < s.len && s[p] == '}' { p++; }
        else {
            loop {
                ws(s, p);
                if p >= s.len || s[p] != '"' { return false; }
                key.clear();
                let key_len, ok = jstr(s, p, key, 32);       // a longer key is not any of the wanted ones
                if !ok { return false; }
                ws(s, p);
                if p >= s.len || s[p] != ':' { return false; }
                p++;
                ws(s, p);
                let value_at = p;
                if !jvalue(s, p, 1) { return false; }
                for i in keys.len { if keys[i].len == key_len && key[..] == keys[i] { found[i] = Want { start: value_at, end: p }; } }
                ws(s, p);
                if p >= s.len { return false; }
                if s[p] == ',' { p++; continue; }
                if s[p] == '}' { p++; break; }
                return false;
            }
        }
    } else if !jvalue(s, p, 0) { return false; }
    ws(s, p);
    return p == s.len;
}
