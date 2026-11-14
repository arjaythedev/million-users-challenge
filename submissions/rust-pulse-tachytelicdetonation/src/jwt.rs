use crate::json::{self, Val};
use crate::sha256::HmacKey;

const fn b64_table(url: bool) -> [u8; 256] {
    let mut t = [255u8; 256];
    let mut i = 0;
    while i < 26 {
        t[b'A' as usize + i] = i as u8;
        t[b'a' as usize + i] = 26 + i as u8;
        i += 1;
    }
    let mut d = 0;
    while d < 10 {
        t[b'0' as usize + d] = 52 + d as u8;
        d += 1;
    }
    if url {
        t[b'-' as usize] = 62;
        t[b'_' as usize] = 63;
    } else {
        t[b'+' as usize] = 62;
        t[b'/' as usize] = 63;
    }
    t
}

static URL: [u8; 256] = b64_table(true);
static STD: [u8; 256] = b64_table(false);

fn decode_with(t: &[u8; 256], s: &[u8], out: &mut [u8]) -> Option<usize> {
    if s.len() % 4 == 1 || s.len() / 4 * 3 + 3 > out.len() + 3 {
        return None;
    }
    let mut acc = 0u32;
    let mut bits = 0;
    let mut n = 0;
    for &c in s {
        let v = t[c as usize];
        if v == 255 {
            return None;
        }
        acc = (acc << 6) | v as u32;
        bits += 6;
        if bits >= 8 {
            bits -= 8;
            if n >= out.len() {
                return None;
            }
            out[n] = (acc >> bits) as u8;
            n += 1;
        }
    }
    Some(n)
}

pub fn b64url_decode(s: &[u8], out: &mut [u8]) -> Option<usize> {
    decode_with(&URL, s, out)
}

pub fn b64_decode(s: &[u8], out: &mut [u8]) -> Option<usize> {
    let s = s.strip_suffix(b"==").or_else(|| s.strip_suffix(b"=")).unwrap_or(s);
    decode_with(&STD, s, out)
}

pub fn b64_encode(data: &[u8], out: &mut Vec<u8>) {
    const A: &[u8; 64] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    for ch in data.chunks(3) {
        let b = [ch[0], *ch.get(1).unwrap_or(&0), *ch.get(2).unwrap_or(&0)];
        let v = (b[0] as u32) << 16 | (b[1] as u32) << 8 | b[2] as u32;
        out.push(A[(v >> 18) as usize & 63]);
        out.push(A[(v >> 12) as usize & 63]);
        out.push(if ch.len() > 1 { A[(v >> 6) as usize & 63] } else { b'=' });
        out.push(if ch.len() > 2 { A[v as usize & 63] } else { b'=' });
    }
}

#[derive(Debug, PartialEq)]
pub enum AuthErr {
    Missing,
    Invalid,
    Payload,
}

pub struct Jwt {
    key: HmacKey,
}

fn parse_sub(s: &[u8]) -> Option<i64> {
    if s.is_empty() || s[0] == b'0' || s.len() > 19 {
        return None;
    }
    let mut v: i64 = 0;
    for &c in s {
        if !c.is_ascii_digit() {
            return None;
        }
        v = v.checked_mul(10)?.checked_add((c - b'0') as i64)?;
    }
    Some(v)
}

impl Jwt {
    pub fn new(secret: &[u8]) -> Self {
        Jwt { key: HmacKey::new(secret) }
    }

    pub fn verify(&self, auth: Option<&[u8]>, now: i64, name: &mut Vec<u8>) -> Result<i64, AuthErr> {
        let tok = match auth {
            Some(a) if a.starts_with(b"Bearer ") => &a[7..],
            _ => return Err(AuthErr::Missing),
        };
        let d1 = tok.iter().position(|&c| c == b'.').ok_or(AuthErr::Invalid)?;
        let d2 = d1 + 1 + tok[d1 + 1..].iter().position(|&c| c == b'.').ok_or(AuthErr::Invalid)?;
        let sig_s = &tok[d2 + 1..];
        if sig_s.len() != 43 {
            return Err(AuthErr::Invalid);
        }
        let mut sig = [0u8; 33];
        if b64url_decode(sig_s, &mut sig) != Some(32) {
            return Err(AuthErr::Invalid);
        }
        let mac = self.key.mac(&tok[..d2]);
        let mut diff = 0u8;
        for i in 0..32 {
            diff |= mac[i] ^ sig[i];
        }
        if diff != 0 {
            return Err(AuthErr::Invalid);
        }
        let mut hbuf = [0u8; 512];
        let hn = b64url_decode(&tok[..d1], &mut hbuf).ok_or(AuthErr::Invalid)?;
        let mut alg_ok = false;
        let mut tmp = Vec::new();
        let is_obj = json::fields(&hbuf[..hn], &mut |k, v| {
            if k == b"alg" {
                alg_ok = match v {
                    Val::Str { raw, esc } => {
                        if esc {
                            tmp.clear();
                            json::decode(raw, &mut tmp).is_ok() && tmp == b"HS256"
                        } else {
                            raw == b"HS256"
                        }
                    }
                    _ => false,
                }
            }
        })
        .map_err(|_| AuthErr::Invalid)?;
        if !is_obj || !alg_ok {
            return Err(AuthErr::Invalid);
        }
        let mut pbuf = [0u8; 4096];
        let pn = b64url_decode(&tok[d1 + 1..d2], &mut pbuf).ok_or(AuthErr::Invalid)?;
        let payload = &pbuf[..pn];
        if std::str::from_utf8(payload).is_err() {
            return Err(AuthErr::Invalid);
        }
        let mut exp: Option<f64> = None;
        let mut sub: Option<Option<i64>> = None;
        let mut uname: Option<bool> = None;
        name.clear();
        let is_obj = json::fields(payload, &mut |k, v| match k {
            b"exp" => {
                exp = match v {
                    Val::Num(n) => std::str::from_utf8(n).ok().and_then(|s| s.parse::<f64>().ok()),
                    _ => None,
                }
            }
            b"sub" => {
                sub = Some(match v {
                    Val::Str { raw, esc } => {
                        if esc {
                            tmp.clear();
                            if json::decode(raw, &mut tmp).is_ok() {
                                parse_sub(&tmp)
                            } else {
                                None
                            }
                        } else {
                            parse_sub(raw)
                        }
                    }
                    _ => None,
                })
            }
            b"username" => match v {
                Val::Str { raw, esc } => {
                    name.clear();
                    if esc {
                        let _ = json::decode(raw, name);
                    } else {
                        name.extend_from_slice(raw);
                    }
                    uname = Some(true);
                }
                _ => uname = Some(false),
            },
            _ => {}
        })
        .map_err(|_| AuthErr::Invalid)?;
        if !is_obj {
            return Err(AuthErr::Invalid);
        }
        match exp {
            Some(e) if e > now as f64 => {}
            _ => return Err(AuthErr::Invalid),
        }
        let sub = match sub {
            Some(Some(s)) => s,
            _ => return Err(AuthErr::Payload),
        };
        if uname != Some(true) {
            return Err(AuthErr::Payload);
        }
        Ok(sub)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn enc(s: &[u8]) -> String {
        let mut v = Vec::new();
        b64_encode(s, &mut v);
        String::from_utf8(v).unwrap().replace('+', "-").replace('/', "_").replace('=', "")
    }

    fn sign(secret: &[u8], header: &str, payload: &str) -> String {
        let hp = format!("{}.{}", enc(header.as_bytes()), enc(payload.as_bytes()));
        let mac = HmacKey::new(secret).mac(hp.as_bytes());
        format!("Bearer {}.{}", hp, enc(&mac))
    }

    #[test]
    fn verify() {
        let j = Jwt::new(b"million-users-challenge");
        let h = r#"{"alg":"HS256","typ":"JWT"}"#;
        let mut name = Vec::new();
        let t = sign(b"million-users-challenge", h, r#"{"sub":"42","username":"bob","iat":1,"exp":2000000000}"#);
        assert_eq!(j.verify(Some(t.as_bytes()), 1_700_000_000, &mut name), Ok(42));
        assert_eq!(name, b"bob");
        assert_eq!(j.verify(Some(t.as_bytes()), 2_000_000_000, &mut name), Err(AuthErr::Invalid));
        assert_eq!(j.verify(None, 0, &mut name), Err(AuthErr::Missing));
        assert_eq!(j.verify(Some(b"Basic abc"), 0, &mut name), Err(AuthErr::Missing));
        assert_eq!(j.verify(Some(b"Bearer abc"), 0, &mut name), Err(AuthErr::Invalid));
        let bad = sign(b"other", h, r#"{"sub":"42","username":"bob","exp":2000000000}"#);
        assert_eq!(j.verify(Some(bad.as_bytes()), 0, &mut name), Err(AuthErr::Invalid));
        let none = sign(b"million-users-challenge", r#"{"alg":"none"}"#, r#"{"sub":"42","username":"bob","exp":2000000000}"#);
        assert_eq!(j.verify(Some(none.as_bytes()), 0, &mut name), Err(AuthErr::Invalid));
        for p in [
            r#"{"sub":"abc","username":"bob","exp":2000000000}"#,
            r#"{"sub":"0","username":"bob","exp":2000000000}"#,
            r#"{"sub":"01","username":"bob","exp":2000000000}"#,
            r#"{"sub":"+1","username":"bob","exp":2000000000}"#,
            r#"{"sub":42,"username":"bob","exp":2000000000}"#,
            r#"{"sub":"9223372036854775808","username":"bob","exp":2000000000}"#,
            r#"{"sub":"1","exp":2000000000}"#,
            r#"{"sub":"1","username":5,"exp":2000000000}"#,
        ] {
            let t = sign(b"million-users-challenge", h, p);
            assert_eq!(j.verify(Some(t.as_bytes()), 0, &mut name), Err(AuthErr::Payload), "{}", p);
        }
        let t = sign(b"million-users-challenge", h, r#"{"sub":"9223372036854775807","username":"a\"b","exp":2000000000}"#);
        assert_eq!(j.verify(Some(t.as_bytes()), 0, &mut name), Ok(i64::MAX));
        assert_eq!(name, b"a\"b");
        let t = sign(b"million-users-challenge", h, r#"{"sub":"1","username":"x"}"#);
        assert_eq!(j.verify(Some(t.as_bytes()), 0, &mut name), Err(AuthErr::Invalid));
    }
}
