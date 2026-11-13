//! HS256 bearer tokens. Every request's signature and `exp` are checked; nothing is remembered between requests.

use crate::crypto::{b64_decode, Hmac};
use crate::json::{parse_object, Doc, Value};

#[derive(Debug, PartialEq)]
pub enum AuthError {
    /// No `Authorization` header, or it does not start with `Bearer `.
    Missing,
    /// Bad signature, expired, not HS256, or not a JWT at all.
    Invalid,
    /// A valid token whose `sub` or `username` is unusable.
    Payload,
}

pub struct User {
    pub id: u64,
    pub name: Vec<u8>,
}

pub struct Verifier {
    hmac: Hmac,
}

/// base64url of `{"alg":"HS256","typ":"JWT"}`, the header almost every token carries.
const USUAL_HEADER: &[u8] = b"eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";

impl Verifier {
    pub fn new(secret: &[u8]) -> Verifier {
        Verifier { hmac: Hmac::new(secret) }
    }

    pub fn verify(&self, authorization: Option<&[u8]>, now_secs: i64) -> Result<User, AuthError> {
        let token = authorization.and_then(|h| h.strip_prefix(b"Bearer ")).ok_or(AuthError::Missing)?;

        let mut parts = token.split(|&b| b == b'.');
        let (Some(head), Some(payload), Some(sig), None) = (parts.next(), parts.next(), parts.next(), parts.next())
        else {
            return Err(AuthError::Invalid);
        };

        if head != USUAL_HEADER {
            let raw = b64_decode(head).ok_or(AuthError::Invalid)?;
            match parse_object(&raw, &[b"alg"]) {
                Ok(Doc::Object(f)) if matches!(&f[0], Some(Value::Str(a)) if a == b"HS256") => {}
                _ => return Err(AuthError::Invalid),
            }
        }

        let signed = &token[..head.len() + 1 + payload.len()];
        let given = b64_decode(sig).ok_or(AuthError::Invalid)?;
        if given.as_slice() != self.hmac.mac(&[signed]).as_slice() {
            return Err(AuthError::Invalid);
        }

        let raw = b64_decode(payload).ok_or(AuthError::Invalid)?;
        let Ok(Doc::Object(mut f)) = parse_object(&raw, &[b"sub", b"username", b"exp", b"nbf"]) else {
            return Err(AuthError::Invalid);
        };
        match f[2] {
            Some(Value::Num(exp)) if (now_secs as f64) < exp => {}
            _ => return Err(AuthError::Invalid),
        }
        match f[3] {
            None => {}
            Some(Value::Num(nbf)) if (now_secs as f64) >= nbf => {}
            _ => return Err(AuthError::Invalid),
        }

        let id = match &f[0] {
            Some(Value::Str(s)) if !s.is_empty() && s.len() <= 18 && s.iter().all(u8::is_ascii_digit) => {
                s.iter().fold(0u64, |a, &d| a * 10 + (d - b'0') as u64)
            }
            _ => return Err(AuthError::Payload),
        };
        if id == 0 {
            return Err(AuthError::Payload);
        }
        match f.swap_remove(1) {
            Some(Value::Str(name)) => Ok(User { id, name }),
            _ => Err(AuthError::Payload),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::crypto::b64_encode;

    const SECRET: &[u8] = b"million-users-challenge";
    const NOW: i64 = 1_800_000_000;

    fn b64url(data: &[u8]) -> Vec<u8> {
        let mut v = b64_encode(data);
        v.retain(|&c| c != b'=');
        for c in v.iter_mut() {
            *c = match *c {
                b'+' => b'-',
                b'/' => b'_',
                x => x,
            };
        }
        v
    }

    fn sign_with(header: &str, payload: &str, secret: &[u8]) -> Vec<u8> {
        let mut t = b64url(header.as_bytes());
        t.push(b'.');
        t.extend_from_slice(&b64url(payload.as_bytes()));
        let sig = Hmac::new(secret).mac(&[&t]);
        t.push(b'.');
        t.extend_from_slice(&b64url(&sig));
        t
    }

    fn check(header: &str, payload: &str) -> Result<User, AuthError> {
        let mut h = b"Bearer ".to_vec();
        h.extend_from_slice(&sign_with(header, payload, SECRET));
        Verifier::new(SECRET).verify(Some(&h), NOW)
    }

    const HS256: &str = r#"{"alg":"HS256","typ":"JWT"}"#;

    #[test]
    fn accepts_a_good_token() {
        let u = check(HS256, r#"{"sub":"7","username":"golden_ember_1","iat":1,"exp":1800000001}"#).unwrap();
        assert_eq!((u.id, u.name.as_slice()), (7, &b"golden_ember_1"[..]));
        // another key order and spacing in the header takes the slow path
        assert!(check(r#"{ "typ":"JWT", "alg":"HS256" }"#, r#"{"sub":"7","username":"a","exp":1800000001}"#).is_ok());
    }

    #[test]
    fn missing_bearer() {
        let v = Verifier::new(SECRET);
        assert_eq!(v.verify(None, NOW).err(), Some(AuthError::Missing));
        assert_eq!(v.verify(Some(b"Basic abc"), NOW).err(), Some(AuthError::Missing));
        assert_eq!(v.verify(Some(b"bearer abc"), NOW).err(), Some(AuthError::Missing));
    }

    #[test]
    fn invalid_tokens() {
        let v = Verifier::new(SECRET);
        let ok = r#"{"sub":"7","username":"a","exp":1800000001}"#;
        assert_eq!(v.verify(Some(b"Bearer abc"), NOW).err(), Some(AuthError::Invalid));
        assert_eq!(v.verify(Some(b"Bearer "), NOW).err(), Some(AuthError::Invalid));
        assert_eq!(v.verify(Some(b"Bearer a.b.c.d"), NOW).err(), Some(AuthError::Invalid));

        // signed with another secret
        let mut h = b"Bearer ".to_vec();
        h.extend_from_slice(&sign_with(HS256, ok, b"other"));
        assert_eq!(v.verify(Some(&h), NOW).err(), Some(AuthError::Invalid));

        // signature cut off, and alg=none with an empty signature
        let mut t = sign_with(HS256, ok, SECRET);
        t.truncate(t.len() - 4);
        let mut h = b"Bearer ".to_vec();
        h.extend_from_slice(&t);
        assert_eq!(v.verify(Some(&h), NOW).err(), Some(AuthError::Invalid));
        let mut none = b"Bearer ".to_vec();
        none.extend_from_slice(&b64url(br#"{"alg":"none","typ":"JWT"}"#));
        none.push(b'.');
        none.extend_from_slice(&b64url(ok.as_bytes()));
        none.push(b'.');
        assert_eq!(v.verify(Some(&none), NOW).err(), Some(AuthError::Invalid));

        assert_eq!(check(r#"{"alg":"HS512","typ":"JWT"}"#, ok).err(), Some(AuthError::Invalid));
        assert_eq!(check(HS256, r#"{"sub":"7","username":"a","exp":1800000000}"#).err(), Some(AuthError::Invalid), "expires now");
        assert_eq!(check(HS256, r#"{"sub":"7","username":"a","exp":1}"#).err(), Some(AuthError::Invalid));
        assert_eq!(check(HS256, r#"{"sub":"7","username":"a"}"#).err(), Some(AuthError::Invalid), "exp is required");
        assert_eq!(check(HS256, r#"{"sub":"7","username":"a","exp":"1800000001"}"#).err(), Some(AuthError::Invalid));
        assert_eq!(check(HS256, r#"{"sub":"7","username":"a","exp":1800000001,"nbf":1800000001}"#).err(), Some(AuthError::Invalid));
        assert_eq!(check(HS256, r#"["sub"]"#).err(), Some(AuthError::Invalid));
        assert_eq!(check(HS256, "not json").err(), Some(AuthError::Invalid));
    }

    #[test]
    fn bad_payloads() {
        let p = |sub: &str, name: &str| format!(r#"{{"sub":{sub},"username":{name},"exp":1800000001}}"#);
        for (sub, name) in [
            ("\"abc\"", "\"a\""), ("\"0\"", "\"a\""), ("\"-1\"", "\"a\""), ("\"1.5\"", "\"a\""), ("\"\"", "\"a\""),
            ("7", "\"a\""), ("\"7\"", "7"), ("\"7\"", "null"), ("\"1234567890123456789\"", "\"a\""),
        ] {
            assert_eq!(check(HS256, &p(sub, name)).err(), Some(AuthError::Payload), "{sub} {name}");
        }
        assert_eq!(check(HS256, r#"{"sub":"7","exp":1800000001}"#).err(), Some(AuthError::Payload));
        assert_eq!(check(HS256, r#"{"username":"a","exp":1800000001}"#).err(), Some(AuthError::Payload));
    }
}
