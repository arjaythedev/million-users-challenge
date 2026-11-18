use crate::json::{self, Kind, Slot};
use crate::sha256::Hmac;

pub enum Auth {
    Ok,
    Missing,
    Invalid,
    BadPayload,
}

const B64URL: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

static B64_INDEX: [u8; 256] = {
    let mut t = [255u8; 256];
    let mut i = 0;
    while i < 64 {
        t[B64URL[i] as usize] = i as u8;
        i += 1;
    }
    t
};

const USUAL_HEADER: &[u8] = b"eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";

pub struct Jwt {
    hmac: Hmac,
    raw: Vec<u8>,
    key: Vec<u8>,
    slots: [Slot; 5],
    pub user_id: i64,
    pub username: Vec<u8>,
}

fn decode(s: &[u8], out: &mut Vec<u8>) -> bool {
    out.clear();
    if s.len() % 4 == 1 {
        return false;
    }
    let mut acc: u32 = 0;
    let mut n = 0;
    for &c in s {
        let v = B64_INDEX[c as usize];
        if v == 255 {
            return false;
        }
        acc = acc << 6 | v as u32;
        n += 1;
        if n == 4 {
            out.extend_from_slice(&[(acc >> 16) as u8, (acc >> 8) as u8, acc as u8]);
            acc = 0;
            n = 0;
        }
    }
    match n {
        2 => out.push((acc >> 4) as u8),
        3 => out.extend_from_slice(&[(acc >> 10) as u8, (acc >> 2) as u8]),
        _ => {}
    }
    true
}

pub fn b64_std_encode(data: &[u8], out: &mut String) {
    const STD: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for chunk in data.chunks(3) {
        let b = [chunk[0], *chunk.get(1).unwrap_or(&0), *chunk.get(2).unwrap_or(&0)];
        let v = (b[0] as u32) << 16 | (b[1] as u32) << 8 | b[2] as u32;
        out.push(STD[(v >> 18) as usize] as char);
        out.push(STD[(v >> 12 & 63) as usize] as char);
        out.push(if chunk.len() > 1 { STD[(v >> 6 & 63) as usize] as char } else { '=' });
        out.push(if chunk.len() > 2 { STD[(v & 63) as usize] as char } else { '=' });
    }
}

pub fn b64_std_decode(s: &[u8]) -> Option<Vec<u8>> {
    let mut out = Vec::with_capacity(s.len() * 3 / 4);
    let mut acc = 0u32;
    let mut n = 0;
    for &c in s {
        let v = match c {
            b'A'..=b'Z' => c - b'A',
            b'a'..=b'z' => c - b'a' + 26,
            b'0'..=b'9' => c - b'0' + 52,
            b'+' => 62,
            b'/' => 63,
            b'=' => break,
            _ => return None,
        };
        acc = acc << 6 | v as u32;
        n += 1;
        if n == 4 {
            out.extend_from_slice(&[(acc >> 16) as u8, (acc >> 8) as u8, acc as u8]);
            acc = 0;
            n = 0;
        }
    }
    match n {
        2 => out.push((acc >> 4) as u8),
        3 => out.extend_from_slice(&[(acc >> 10) as u8, (acc >> 2) as u8]),
        1 => return None,
        _ => {}
    }
    Some(out)
}

fn positive_int(s: &[u8]) -> i64 {
    if s.is_empty() || s.len() > 19 {
        return -1;
    }
    let mut v: i64 = 0;
    for &c in s {
        if !c.is_ascii_digit() {
            return -1;
        }
        v = match v.checked_mul(10).and_then(|v| v.checked_add((c - b'0') as i64)) {
            Some(v) => v,
            None => return -1,
        };
    }
    if v > 0 { v } else { -1 }
}

impl Jwt {
    pub fn new(secret: &[u8]) -> Self {
        Jwt {
            hmac: Hmac::new(secret),
            raw: Vec::with_capacity(512),
            key: Vec::with_capacity(64),
            slots: Default::default(),
            user_id: 0,
            username: Vec::with_capacity(64),
        }
    }

    pub fn verify(&mut self, header: Option<&[u8]>, now: i64) -> Auth {
        let h = match header {
            Some(h) if h.len() >= 7 && &h[..7] == b"Bearer " => &h[7..],
            _ => return Auth::Missing,
        };
        let d1 = match h.iter().position(|&c| c == b'.') {
            Some(d) => d,
            None => return Auth::Invalid,
        };
        let d2 = match h[d1 + 1..].iter().position(|&c| c == b'.') {
            Some(d) => d1 + 1 + d,
            None => return Auth::Invalid,
        };
        if h[d2 + 1..].contains(&b'.') {
            return Auth::Invalid;
        }
        let (head, payload, sig) = (&h[..d1], &h[d1 + 1..d2], &h[d2 + 1..]);

        if head != USUAL_HEADER {
            if !decode(head, &mut self.raw) || std::str::from_utf8(&self.raw).is_err() {
                return Auth::Invalid;
            }
            let raw = std::mem::take(&mut self.raw);
            let r = json::parse_top(&raw, &[b"alg"], &mut self.slots[..1], &mut self.key);
            self.raw = raw;
            match r {
                Ok(true) if self.slots[0].kind == Kind::Str && self.slots[0].str == b"HS256" => {}
                _ => return Auth::Invalid,
            }
        }

        if sig.len() != 43 {
            return Auth::Invalid;
        }
        let d = self.hmac.mac(&h[..d2]);
        let mut enc = [0u8; 43];
        let mut o = 0;
        for k in (0..30).step_by(3) {
            let v = (d[k] as u32) << 16 | (d[k + 1] as u32) << 8 | d[k + 2] as u32;
            enc[o] = B64URL[(v >> 18) as usize];
            enc[o + 1] = B64URL[(v >> 12 & 63) as usize];
            enc[o + 2] = B64URL[(v >> 6 & 63) as usize];
            enc[o + 3] = B64URL[(v & 63) as usize];
            o += 4;
        }
        let v = (d[30] as u32) << 8 | d[31] as u32;
        enc[40] = B64URL[(v >> 10) as usize];
        enc[41] = B64URL[(v >> 4 & 63) as usize];
        enc[42] = B64URL[(v << 2 & 63) as usize];
        let mut diff = 0u8;
        for k in 0..43 {
            diff |= enc[k] ^ sig[k];
        }
        if diff != 0 {
            return Auth::Invalid;
        }

        if !decode(payload, &mut self.raw) || std::str::from_utf8(&self.raw).is_err() {
            return Auth::Invalid;
        }
        let raw = std::mem::take(&mut self.raw);
        let r = json::parse_top(&raw, &[b"exp", b"nbf", b"sub", b"username"], &mut self.slots[..4], &mut self.key);
        self.raw = raw;
        match r {
            Ok(true) => {}
            _ => return Auth::Invalid,
        }
        let s = &self.slots;
        if !(s[0].kind == Kind::Num && (now as f64) < s[0].num) {
            return Auth::Invalid;
        }
        if !(s[1].kind == Kind::Absent || s[1].kind == Kind::Null || (s[1].kind == Kind::Num && now as f64 >= s[1].num)) {
            return Auth::Invalid;
        }
        let id = if s[2].kind == Kind::Str { positive_int(&s[2].str) } else { -1 };
        if id <= 0 || s[3].kind != Kind::Str {
            return Auth::BadPayload;
        }
        self.user_id = id;
        self.username.clear();
        self.username.extend_from_slice(&s[3].str);
        Auth::Ok
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn b64url(d: &[u8]) -> String {
        let mut s = String::new();
        b64_std_encode(d, &mut s);
        s.trim_end_matches('=').replace('+', "-").replace('/', "_")
    }

    fn sign(secret: &[u8], header: &str, payload: &str) -> String {
        let hp = format!("{}.{}", b64url(header.as_bytes()), b64url(payload.as_bytes()));
        let sig = Hmac::new(secret).mac(hp.as_bytes());
        format!("{hp}.{}", b64url(&sig))
    }

    #[test]
    fn tokens() {
        let mut j = Jwt::new(b"million-users-challenge");
        let now = 1_800_000_000;
        let ok = sign(b"million-users-challenge", r#"{"alg":"HS256","typ":"JWT"}"#,
            r#"{"sub":"1","username":"golden_ember_1","iat":1,"exp":1900000000}"#);
        assert!(ok.starts_with("eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9."));
        let h = format!("Bearer {ok}");
        assert!(matches!(j.verify(Some(h.as_bytes()), now), Auth::Ok));
        assert_eq!(j.user_id, 1);
        assert_eq!(j.username, b"golden_ember_1");
        let other = sign(b"million-users-challenge", r#"{"typ":"JWT","alg":"HS256"}"#,
            r#"{"sub":"7","username":"x","exp":1900000000}"#);
        assert!(matches!(j.verify(Some(format!("Bearer {other}").as_bytes()), now), Auth::Ok));
        assert!(matches!(j.verify(Some(format!("Bearer {ok}").as_bytes()), 1_900_000_000), Auth::Invalid));
        assert!(matches!(j.verify(None, now), Auth::Missing));
        assert!(matches!(j.verify(Some(b"Basic abc"), now), Auth::Missing));
        assert!(matches!(j.verify(Some(b"Bearer abc"), now), Auth::Invalid));
        let none = sign(b"million-users-challenge", r#"{"alg":"none","typ":"JWT"}"#, r#"{"sub":"1","username":"a","exp":1900000000}"#);
        assert!(matches!(j.verify(Some(format!("Bearer {none}").as_bytes()), now), Auth::Invalid));
        let bad_sub = sign(b"million-users-challenge", r#"{"alg":"HS256","typ":"JWT"}"#, r#"{"sub":"abc","username":"a","exp":1900000000}"#);
        assert!(matches!(j.verify(Some(format!("Bearer {bad_sub}").as_bytes()), now), Auth::BadPayload));
        let no_name = sign(b"million-users-challenge", r#"{"alg":"HS256","typ":"JWT"}"#, r#"{"sub":"1","exp":1900000000}"#);
        assert!(matches!(j.verify(Some(format!("Bearer {no_name}").as_bytes()), now), Auth::BadPayload));
        let wrong = sign(b"other", r#"{"alg":"HS256","typ":"JWT"}"#, r#"{"sub":"1","username":"a","exp":1900000000}"#);
        assert!(matches!(j.verify(Some(format!("Bearer {wrong}").as_bytes()), now), Auth::Invalid));
    }
}
