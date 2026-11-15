//! HS256 JWT verification, done in full on every request: header `alg`, signature, `exp`, then the payload.

use crate::json::{self, Val};
use hmac::{Hmac, Mac};
use sha2::Sha256;

pub enum Auth {
    Ok { user_id: i64, username: String },
    Missing,
    Invalid,
    BadPayload,
}

pub struct Verifier {
    /// HMAC keyed once (inner and outer pads absorbed); cloned per token.
    key: Hmac<Sha256>,
}

fn b64url_val(c: u8) -> Option<u32> {
    Some(match c {
        b'A'..=b'Z' => c - b'A',
        b'a'..=b'z' => c - b'a' + 26,
        b'0'..=b'9' => c - b'0' + 52,
        b'-' => 62,
        b'_' => 63,
        _ => return None,
    } as u32)
}

/// Unpadded base64url (a trailing `=` padding is tolerated).
pub fn b64url_decode(mut s: &[u8], out: &mut Vec<u8>) -> Result<(), ()> {
    while let [rest @ .., b'='] = s {
        s = rest;
    }
    if s.len() % 4 == 1 {
        return Err(());
    }
    out.reserve(s.len() * 3 / 4);
    for ch in s.chunks(4) {
        let mut acc = 0u32;
        for &c in ch {
            acc = acc << 6 | b64url_val(c).ok_or(())?;
        }
        match ch.len() {
            4 => out.extend_from_slice(&[(acc >> 16) as u8, (acc >> 8) as u8, acc as u8]),
            3 => out.extend_from_slice(&[(acc >> 10) as u8, (acc >> 2) as u8]),
            2 => out.push((acc >> 4) as u8),
            _ => unreachable!(),
        }
    }
    Ok(())
}

impl Verifier {
    pub fn new(secret: &[u8]) -> Self {
        Verifier { key: Hmac::<Sha256>::new_from_slice(secret).expect("hmac key") }
    }

    /// `header` is the raw `Authorization` value. `now` is unix seconds.
    pub fn check(&self, header: Option<&[u8]>, now: f64) -> Auth {
        let Some(h) = header else { return Auth::Missing };
        let Some(tok) = h.strip_prefix(b"Bearer ") else { return Auth::Missing };
        let tok = tok.trim_ascii();
        let mut parts = tok.splitn(3, |&c| c == b'.');
        let (Some(h64), Some(p64), Some(s64)) = (parts.next(), parts.next(), parts.next()) else {
            return Auth::Invalid;
        };
        if s64.contains(&b'.') {
            return Auth::Invalid;
        }
        let mut buf = Vec::with_capacity(256);
        // header: {"alg":"HS256",...}
        if b64url_decode(h64, &mut buf).is_err() {
            return Auth::Invalid;
        }
        let mut alg_ok = false;
        match json::parse_object(&buf, |k, v| {
            if k == b"alg" {
                alg_ok = matches!(v, Val::Str(ref s) if s == "HS256");
            }
        }) {
            Ok(true) if alg_ok => {}
            _ => return Auth::Invalid,
        }
        // signature over "<header>.<payload>"
        buf.clear();
        if b64url_decode(s64, &mut buf).is_err() || buf.len() != 32 {
            return Auth::Invalid;
        }
        let mut mac = self.key.clone();
        mac.update(&tok[..h64.len() + 1 + p64.len()]);
        if mac.verify_slice(&buf).is_err() {
            return Auth::Invalid;
        }
        // payload
        buf.clear();
        if b64url_decode(p64, &mut buf).is_err() {
            return Auth::Invalid;
        }
        let mut exp: Option<f64> = None;
        let mut sub: Option<Val> = None;
        let mut name: Option<Val> = None;
        match json::parse_object(&buf, |k, v| match k {
            b"exp" => {
                exp = match v {
                    Val::Num(n) => std::str::from_utf8(n).ok().and_then(|s| s.parse().ok()),
                    _ => None,
                }
            }
            b"sub" => sub = Some(v),
            b"username" => name = Some(v),
            _ => {}
        }) {
            Ok(true) => {}
            _ => return Auth::Invalid,
        }
        match exp {
            Some(e) if e > now => {}
            _ => return Auth::Invalid,
        }
        let user_id = match sub {
            Some(Val::Str(s)) => match parse_pos_int(s.as_bytes()) {
                Some(v) => v,
                None => return Auth::BadPayload,
            },
            _ => return Auth::BadPayload,
        };
        match name {
            Some(Val::Str(username)) => Auth::Ok { user_id, username },
            _ => Auth::BadPayload,
        }
    }
}

/// A positive decimal integer that fits in i64 (digits only).
pub fn parse_pos_int(s: &[u8]) -> Option<i64> {
    if s.is_empty() || s.len() > 19 {
        return None;
    }
    let mut v: i64 = 0;
    for &c in s {
        if !c.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((c - b'0') as i64)?;
    }
    if v > 0 { Some(v) } else { None }
}
