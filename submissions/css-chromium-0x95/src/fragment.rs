//! The per-request HTML fragments: the input DOM the generated CSS reads. Pure byte -> number slicing, no interpretation.
//!
//!   q#q{rid} > R.. > A.. > S.. > O > B.. > V#v{rid}          (A/S/O only when the Authorization header is longer
//!                                                             than 7 bytes, B only when a body is present;
//!                                                             O is `O O0` if the last S block is S0, else `O O1`)
//! The frame pass fragments (`build_frame`, gen/http.py): Hs [> H.. ] > Z over the first bytes of a connection buffer. Besides the
//! bytes as numbers they carry three strings (`d`, `e`, `g`, and `dc` per chunk) that the CSS attribute selectors read; the strings are
//! plain per-byte table lookups (no state, no interpretation): `d` the byte as a character, `e` its character class, `g` the digit
//! folded. Everything that decides is CSS.
//! Every element carries `--rid:{rid}` in its own inline style (defeats Blink's matched-properties cache).
//! Route chunks are classed `R Rf` (first), `R R1`, `R R0`, `R R1`, ... and there are always at least two of them; the other chunk kinds `X X0`, `X X1`, `X X0`, ...
//! Attributes use single quotes so the fragments can sit inside a JSON string without escaping.
use crate::config::Consts;

pub fn push_num(o: &mut Vec<u8>, v: u64) {
    let mut t = [0u8; 20];
    let mut i = t.len();
    let mut u = v;
    loop {
        i -= 1;
        t[i] = b'0' + (u % 10) as u8;
        u /= 10;
        if u == 0 {
            break;
        }
    }
    o.extend_from_slice(&t[i..]);
}

fn prop(o: &mut Vec<u8>, name: &[u8], v: u64) {
    o.extend_from_slice(b"--");
    o.extend_from_slice(name);
    o.push(b':');
    push_num(o, v);
    o.push(b';');
}

/// `--c0:..;--c1:..;` for a slice of bytes.
fn bytes(o: &mut Vec<u8>, data: &[u8]) {
    for (i, &b) in data.iter().enumerate() {
        o.extend_from_slice(b"--c");
        push_num(o, i as u64);
        o.push(b':');
        push_num(o, b as u64);
        o.push(b';');
    }
}

/// Class attribute value of chunk `k`: `R Rf`, `R R1`, `R R0`, ... for the route, `{cls} {cls}{k%2}` otherwise.
fn class(o: &mut Vec<u8>, cls: &str, k: usize) {
    o.extend_from_slice(cls.as_bytes());
    o.push(b' ');
    o.extend_from_slice(cls.as_bytes());
    if cls == "R" {
        o.push(if k == 0 { b'f' } else { b'0' + (k % 2) as u8 }); // second chunk R1, third R0, ...
    } else {
        o.push(b'0' + (k % 2) as u8);
    }
}

/// One element per `size` bytes of `data`: class see `class`, props --rid, --n (bytes in this chunk), --off (offset of
/// the chunk in the whole value), --len (length of the whole value), --c0.. (bytes). `first` is extra props on the first chunk.
fn chunks(o: &mut Vec<u8>, rid: u32, cls: &str, data: &[u8], size: usize, first: Option<(&[u8], u64)>, closes: &mut usize) {
    for (k, ch) in data.chunks(size).enumerate() {
        o.extend_from_slice(b"<b class='");
        class(o, cls, k);
        o.extend_from_slice(b"' style='");
        prop(o, b"rid", rid as u64);
        if k == 0 {
            if let Some((n, v)) = first {
                prop(o, n, v);
            }
        }
        prop(o, b"n", ch.len() as u64);
        prop(o, b"off", (k * size) as u64);
        prop(o, b"len", data.len() as u64);
        bytes(o, ch);
        o.extend_from_slice(b"'>");
        *closes += 1;
    }
}

pub struct Req<'a> {
    /// 0 GET, 1 POST, 2 other: the frame pass (CSS) says
    pub mt: u8,
    pub path: &'a [u8],
    pub auth: Option<&'a [u8]>,
    pub body: &'a [u8],
    /// the connection is closed after the response (HTTP/1.0 without keep-alive, `Connection: close`): `--cl:1` on q
    pub close: bool,
}

pub fn build(o: &mut Vec<u8>, rid: u32, r: &Req, now_ms: u64, k: &Consts) {
    let mut closes = 0usize;
    o.extend_from_slice(b"<b class='q' id='q");
    push_num(o, rid as u64);
    o.extend_from_slice(b"' style='");
    prop(o, b"rid", rid as u64);
    prop(o, b"now", now_ms / 1000);
    if r.close {
        prop(o, b"cl", 1);
    }
    o.extend_from_slice(b"'>");
    closes += 1;

    // route: path chunks (--mt on the first: 0 GET, 1 POST, 2 other)
    let mt = r.mt as u64;
    if r.path.is_empty() {
        // cannot happen with a parsed request line; keep the chain well-formed anyway
        o.extend_from_slice(b"<b class='R Rf' style='");
        prop(o, b"rid", rid as u64);
        prop(o, b"mt", mt);
        o.extend_from_slice(b"--n:0;--off:0;--len:0'>");
        closes += 1;
    } else {
        chunks(o, rid, "R", r.path, k.path_chunk, Some((b"mt", mt)), &mut closes);
    }
    if r.path.len() <= k.path_chunk {
        // always at least two route chunks (the second may be empty): the first only decides the literal prefix
        o.extend_from_slice(b"<b class='R R1' style='");
        prop(o, b"rid", rid as u64);
        prop(o, b"n", 0);
        prop(o, b"off", k.path_chunk as u64);
        prop(o, b"len", r.path.len() as u64);
        o.extend_from_slice(b"'>");
        closes += 1;
    }

    // authorization value, then the inner SHA-256 blocks over its bytes from offset 7, then the outer block
    if let Some(a) = r.auth.filter(|a| !a.is_empty()) {
        chunks(o, rid, "A", a, k.auth_chunk, None, &mut closes);
        if a.len() > 7 {
            let m = &a[7..];
            let blocks = (m.len() + 9).div_ceil(k.sha_block);
            for b in 0..blocks {
                let s = (b * k.sha_block).min(m.len());
                let blk = &m[s..(s + k.sha_block).min(m.len())];
                o.extend_from_slice(b"<b class='S S");
                o.push(b'0' + (b % 2) as u8);
                o.extend_from_slice(b"' style='");
                prop(o, b"rid", rid as u64);
                prop(o, b"n", blk.len() as u64);
                prop(o, b"off", s as u64);
                prop(o, b"len", m.len() as u64);
                bytes(o, blk);
                o.extend_from_slice(b"'>");
                closes += 1;
            }
            o.extend_from_slice(b"<b class='O O"); // O0 if the last S block is S0, else O1
            o.push(b'0' + ((blocks - 1) % 2) as u8);
            o.extend_from_slice(b"' style='");
            prop(o, b"rid", rid as u64);
            o.extend_from_slice(b"'>");
            closes += 1;
        }
    }

    if !r.body.is_empty() {
        chunks(o, rid, "B", r.body, k.body_chunk, None, &mut closes);
    }

    o.extend_from_slice(b"<b class='V' id='v");
    push_num(o, rid as u64);
    o.extend_from_slice(b"' style='");
    prop(o, b"rid", rid as u64);
    o.extend_from_slice(b"'></b>");
    for _ in 0..closes {
        o.extend_from_slice(b"</b>");
    }
}

// ---- frame pass (gen/http.py) ------------------------------------------------------------------------------------------------

/// Character class of every byte (the `e` / `dc` strings): t token char, u other printable except ':', ':', s blank, r CR, n LF, ! the rest.
const fn class_table() -> [u8; 256] {
    let mut t = [b'!'; 256];
    let mut i = 33;
    while i < 127 {
        let c = i as u8;
        let tchar = c.is_ascii_alphanumeric() || matches!(c, b'!' | b'#' | b'$' | b'%' | b'&' | b'\'' | b'*' | b'+' | b'-' | b'.' | b'^' | b'_' | b'`' | b'|' | b'~');
        t[i] = if tchar { b't' } else if c == b':' { b':' } else { b'u' };
        i += 1;
    }
    t[32] = b's';
    t[13] = b'r';
    t[10] = b'n';
    t
}
static CLASS: [u8; 256] = class_table();

/// `d` / `g` attribute text of one byte: printable ASCII as it is, except the characters that would end the single-quoted attribute or
/// break the JSON string the fragment travels in (`&`, `'`, `"`, `<`, `>`, `\`), which and every other byte are numeric references.
#[inline]
fn push_attr(o: &mut Vec<u8>, b: u8) {
    if (32..127).contains(&b) && !matches!(b, b'&' | b'\'' | b'"' | b'<' | b'>' | b'\\') {
        o.push(b);
    } else {
        o.extend_from_slice(b"&#");
        push_num(o, b as u64);
        o.push(b';');
    }
}

/// The frame fragment of one request head: `Hs` (+ chunk elements when `chunks`) + `Z`. `window` = the first bytes of the connection
/// buffer (at most `head_max`, not empty). The classes of the chunk elements alternate `H0` / `H1`, the last one is also `Hl`, and
/// `Z` continues the alternation (`Z0` after an `H1`, ...); without chunks it is `Z0`.
pub fn build_frame(o: &mut Vec<u8>, rid: u32, window: &[u8], chunks: bool, k: &Consts) {
    let n = window.len().div_ceil(k.raw_chunk).max(1);
    o.extend_from_slice(b"<b class='Hs' id='h");
    push_num(o, rid as u64);
    o.extend_from_slice(b"' style='");
    prop(o, b"rid", rid as u64);
    prop(o, b"len", window.len() as u64);
    o.extend_from_slice(b"' d='");
    for &b in window {
        push_attr(o, b);
    }
    o.extend_from_slice(b"' e='^");
    for &b in window {
        o.push(CLASS[b as usize]);
    }
    o.extend_from_slice(b"$' g='");
    for (i, &b) in window.iter().take(64).enumerate() {
        let dot = |j: Option<&u8>| j == Some(&b'.');
        let keep = (i > 0 && dot(window.get(i - 1))) || dot(window.get(i + 1));
        push_attr(o, if b.is_ascii_digit() && !keep { b'9' } else { b });
    }
    o.extend_from_slice(b"'>");
    if chunks {
        for (c, ch) in window.chunks(k.raw_chunk).enumerate() {
            o.extend_from_slice(b"<b class='H H");
            o.push(b'0' + (c % 2) as u8);
            if c == n - 1 {
                o.extend_from_slice(b" Hl");
            }
            o.extend_from_slice(b"' dc='");
            for &b in ch {
                o.push(CLASS[b as usize]);
            }
            o.extend_from_slice(b"' style='");
            prop(o, b"rid", rid as u64);
            prop(o, b"off", (c * k.raw_chunk) as u64);
            for (i, &b) in ch.iter().enumerate() {
                o.extend_from_slice(b"--h");
                push_num(o, i as u64);
                o.push(b':');
                push_num(o, b as u64);
                o.push(b';');
            }
            o.extend_from_slice(b"'>");
        }
    }
    o.extend_from_slice(b"<b class='Z Z");
    o.push(if chunks { b'0' + (n % 2) as u8 } else { b'0' });
    o.extend_from_slice(b"' id='z");
    push_num(o, rid as u64);
    o.extend_from_slice(b"' style='");
    prop(o, b"rid", rid as u64);
    prop(o, b"off", (n * k.raw_chunk) as u64);
    o.extend_from_slice(b"'></b>");
    for _ in 0..(if chunks { n } else { 0 }) + 1 {
        o.extend_from_slice(b"</b>");
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn frame() {
        let k = Consts { path_chunk: 8, auth_chunk: 32, body_chunk: 16, sha_block: 64, raw_chunk: 16, head_max: 2048, frame_big: 208 };
        let w = b"GET /posts/12 HTTP/1.1\r\nX: a\\b'\"&<>\x00\xff\r\n\r\n";
        let mut o = Vec::new();
        build_frame(&mut o, 5, w, false, &k);
        let s = String::from_utf8(o).unwrap();
        let want_d = "GET /posts/12 HTTP/1.1&#13;&#10;X: a&#92;b&#39;&#34;&#38;&#60;&#62;&#0;&#255;&#13;&#10;&#13;&#10;";
        assert!(s.starts_with(&format!("<b class='Hs' id='h5' style='--rid:5;--len:{};' d='{}' e='^tttsutttttuttsttttutttrnt:stuttutuu!!rnrn$'", w.len(), want_d)), "{s}");
        assert!(s.contains("g='GET /posts/99 HTTP/1.1&#13;"), "{s}");
        assert!(s.ends_with("<b class='Z Z0' id='z5' style='--rid:5;--off:48;'></b></b>"), "{s}");
        assert!(!s.contains('"') && !s.contains('\\'));
        let mut o = Vec::new();
        build_frame(&mut o, 6, w, true, &k);
        let s = String::from_utf8(o).unwrap();
        assert_eq!(s.matches("class='H ").count(), 3);
        assert!(s.contains("<b class='H H0' dc='tttsutttttuttstt' style='--rid:6;--off:0;--h0:71;"), "{s}");
        assert!(s.contains("<b class='H H0 Hl' dc="), "{s}");
        assert!(s.ends_with("<b class='Z Z1' id='z6' style='--rid:6;--off:48;'></b></b></b></b></b>"), "{s}");
    }
    #[test]
    fn health() {
        let k = Consts { path_chunk: 4, auth_chunk: 64, body_chunk: 64, sha_block: 64, raw_chunk: 16, head_max: 2048, frame_big: 208 };
        let mut o = Vec::new();
        build(&mut o, 7, &Req { mt: 0, path: b"/health", auth: None, body: b"", close: false }, 1_700_000_123_456, &k);
        let s = String::from_utf8(o).unwrap();
        assert!(s.starts_with("<b class='q' id='q7' style='--rid:7;--now:1700000123;'>"));
        assert!(s.contains("<b class='R Rf' style='--rid:7;--mt:0;--n:4;--off:0;--len:7;--c0:47;--c1:104;--c2:101;--c3:97;'>"));
        assert!(s.contains("<b class='R R1' style='--rid:7;--n:3;--off:4;--len:7;--c0:108;--c1:116;--c2:104;'>"));
        assert!(s.ends_with("<b class='V' id='v7' style='--rid:7;'></b></b></b></b>"));
    }
    #[test]
    fn route_parity() {
        let k = Consts { path_chunk: 2, auth_chunk: 64, body_chunk: 64, sha_block: 64, raw_chunk: 16, head_max: 2048, frame_big: 208 };
        let mut o = Vec::new();
        build(&mut o, 1, &Req { mt: 0, path: b"/abcdefg", auth: None, body: b"", close: false }, 0, &k);
        let s = String::from_utf8(o).unwrap();
        assert!(s.contains("class='R Rf'") && s.contains("class='R R1'") && s.contains("class='R R0'"));
        assert_eq!(s.matches("class='R ").count(), 4);
        let mut o = Vec::new();
        build(&mut o, 1, &Req { mt: 0, path: b"/", auth: None, body: b"", close: false }, 0, &k);
        let s = String::from_utf8(o).unwrap();
        assert!(s.contains("class='R Rf'") && s.contains("class='R R1'") && s.matches("class='R ").count() == 2);
    }
    #[test]
    fn auth_blocks() {
        let k = Consts { path_chunk: 64, auth_chunk: 64, body_chunk: 64, sha_block: 64, raw_chunk: 16, head_max: 2048, frame_big: 208 };
        let mut o = Vec::new();
        let tok = [b'a'; 7 + 55]; // 55 message bytes: 55 + 9 = 64 -> one block
        build(&mut o, 1, &Req { mt: 1, path: b"/posts", auth: Some(&tok), body: b"{}", close: false }, 0, &k);
        let s = String::from_utf8(o).unwrap();
        assert_eq!(s.matches("class='S ").count(), 1);
        assert_eq!(s.matches("class='O O0'").count(), 1);
        assert_eq!(s.matches("--rid:1;").count(), s.matches("<b ").count());
        let tok = [b'a'; 7 + 56];
        let mut o = Vec::new();
        build(&mut o, 1, &Req { mt: 1, path: b"/posts", auth: Some(&tok), body: b"", close: false }, 0, &k);
        assert_eq!(String::from_utf8(o).unwrap().matches("class='S ").count(), 2);
    }
}
