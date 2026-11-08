//! `Authorization: Bearer <jwt>`: HS256 signature and `exp` checked on every request, nothing remembered.

use base64::{
    alphabet,
    engine::{DecodePaddingMode, GeneralPurpose, GeneralPurposeConfig},
    Engine,
};
use hmac::{Hmac, Mac};
use serde_json::Value;
use sha2::Sha256;

pub const MISSING: &str = "missing bearer token";
pub const INVALID: &str = "invalid or expired token";
pub const PAYLOAD: &str = "invalid token payload";

const B64: GeneralPurpose = GeneralPurpose::new(
    &alphabet::URL_SAFE,
    GeneralPurposeConfig::new().with_decode_padding_mode(DecodePaddingMode::Indifferent),
);

pub struct User {
    pub id: i64,
    pub username: Box<str>,
}

/// `mac` is HMAC-SHA256 already keyed with JWT_SECRET (cloning it skips re-hashing the key). `now` is unix seconds.
pub fn verify(authorization: Option<&[u8]>, mac: &Hmac<Sha256>, now: f64) -> Result<User, &'static str> {
    let token = authorization.and_then(|h| h.strip_prefix(b"Bearer ")).ok_or(MISSING)?;
    let mut parts = token.split(|&b| b == b'.');
    let (Some(header), Some(payload), Some(sig), None) = (parts.next(), parts.next(), parts.next(), parts.next()) else {
        return Err(INVALID);
    };

    let header: Value = decode_json(header).ok_or(INVALID)?;
    if header.get("alg").and_then(Value::as_str) != Some("HS256") {
        return Err(INVALID);
    }
    let sig = B64.decode(sig).map_err(|_| INVALID)?;
    let mut mac = mac.clone();
    mac.update(&token[..token.len() - sig_len(token)]);
    mac.verify_slice(&sig).map_err(|_| INVALID)?;

    let claims: Value = decode_json(payload).ok_or(INVALID)?;
    if !claims.is_object() {
        return Err(INVALID);
    }
    match claims.get("exp").and_then(Value::as_f64) {
        Some(exp) if exp > now => {}
        _ => return Err(INVALID),
    }
    if let Some(nbf) = claims.get("nbf") {
        if nbf.as_f64().map_or(true, |nbf| nbf > now) {
            return Err(INVALID);
        }
    }

    let id = claims.get("sub").and_then(Value::as_str).and_then(positive_int).ok_or(PAYLOAD)?;
    let username = claims.get("username").and_then(Value::as_str).ok_or(PAYLOAD)?;
    Ok(User { id, username: username.into() })
}

/// Length of `.<signature>` at the end of the token: the signed part is everything before it.
fn sig_len(token: &[u8]) -> usize {
    token.len() - token.iter().rposition(|&b| b == b'.').unwrap()
}

fn decode_json(part: &[u8]) -> Option<Value> {
    serde_json::from_slice(&B64.decode(part).ok()?).ok()
}

/// A positive integer written in ASCII digits only (`0`, `-1`, `1.5`, `abc`, `""` are not).
pub fn positive_int(s: &str) -> Option<i64> {
    if s.is_empty() || !s.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    s.parse::<i64>().ok().filter(|&n| n > 0)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn b64(s: &str) -> String {
        base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(s)
    }

    fn sign(header: &str, payload: &str) -> String {
        let signed = format!("{}.{}", b64(header), b64(payload));
        let mut mac = key();
        mac.update(signed.as_bytes());
        let sig = base64::engine::general_purpose::URL_SAFE_NO_PAD.encode(mac.finalize().into_bytes());
        format!("Bearer {signed}.{sig}")
    }

    fn key() -> Hmac<Sha256> {
        Hmac::new_from_slice(b"million-users-challenge").unwrap()
    }

    fn check(h: &str) -> Result<User, &'static str> {
        verify(Some(h.as_bytes()), &key(), 1_000.0)
    }

    const HS: &str = r#"{"alg":"HS256","typ":"JWT"}"#;

    #[test]
    fn valid() {
        let u = check(&sign(HS, r#"{"sub":"7","username":"x","exp":2000}"#)).unwrap();
        assert_eq!((u.id, &*u.username), (7, "x"));
    }

    #[test]
    fn rejects() {
        assert_eq!(verify(None, &key(), 0.0).err(), Some(MISSING));
        assert_eq!(check("Basic abc").err(), Some(MISSING));
        assert_eq!(check("Bearer abc").err(), Some(INVALID));
        assert_eq!(check(&sign(HS, r#"{"sub":"7","username":"x","exp":999}"#)).err(), Some(INVALID));
        assert_eq!(check(&sign(HS, r#"{"sub":"7","username":"x"}"#)).err(), Some(INVALID));
        assert_eq!(check(&sign(r#"{"alg":"HS384"}"#, r#"{"sub":"7","username":"x","exp":2000}"#)).err(), Some(INVALID));
        assert_eq!(check(&sign(HS, r#"{"sub":"abc","username":"x","exp":2000}"#)).err(), Some(PAYLOAD));
        assert_eq!(check(&sign(HS, r#"{"sub":7,"username":"x","exp":2000}"#)).err(), Some(PAYLOAD));
        assert_eq!(check(&sign(HS, r#"{"sub":"0","username":"x","exp":2000}"#)).err(), Some(PAYLOAD));
        assert_eq!(check(&sign(HS, r#"{"sub":"7","exp":2000}"#)).err(), Some(PAYLOAD));
        let good = sign(HS, r#"{"sub":"7","username":"x","exp":2000}"#);
        let i = good.len() - 10;
        let flipped = if &good[i..i + 1] == "A" { "B" } else { "A" };
        let tampered = format!("{}{flipped}{}", &good[..i], &good[i + 1..]);
        assert_eq!(check(&tampered).err(), Some(INVALID));
    }
}
