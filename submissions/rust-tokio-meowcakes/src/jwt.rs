use base64::Engine;
use base64::engine::general_purpose::URL_SAFE_NO_PAD;
use hmac::{Hmac, KeyInit, Mac};
use sha2::Sha256;

const USUAL_HEADER: &[u8] = b"eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";

pub struct Key(Hmac<Sha256>);

impl Key {
    pub fn new(secret: &[u8]) -> Self {
        Key(Hmac::new_from_slice(secret).expect("HMAC takes any key length"))
    }
}

#[derive(Debug, PartialEq)]
pub enum AuthError {
    Missing,
    Invalid,
    Payload,
}

pub struct Claims {
    pub user_id: i64,
    pub username: Box<str>,
}

pub fn verify(key: &Key, authorization: Option<&[u8]>, now_s: f64) -> Result<Claims, AuthError> {
    let token = authorization
        .and_then(|a| a.strip_prefix(b"Bearer "))
        .ok_or(AuthError::Missing)?;
    let token = token.trim_ascii();
    let mut parts = token.split(|&b| b == b'.');
    let (Some(header), Some(payload), Some(sig), None) =
        (parts.next(), parts.next(), parts.next(), parts.next())
    else {
        return Err(AuthError::Invalid);
    };

    let mut sig_bytes = [0u8; 32];
    match URL_SAFE_NO_PAD.decode_slice(sig, &mut sig_bytes) {
        Ok(32) => {}
        _ => return Err(AuthError::Invalid),
    }
    let mut mac = key.0.clone();
    mac.update(&token[..header.len() + 1 + payload.len()]);
    mac.verify_slice(&sig_bytes)
        .map_err(|_| AuthError::Invalid)?;

    if header != USUAL_HEADER {
        let h: serde_json::Value = decode_json(header)?;
        if h.get("alg").and_then(|a| a.as_str()) != Some("HS256") {
            return Err(AuthError::Invalid);
        }
    }

    let claims: serde_json::Value = decode_json(payload)?;
    let claims = claims.as_object().ok_or(AuthError::Invalid)?;
    let exp = claims
        .get("exp")
        .and_then(|v| v.as_f64())
        .ok_or(AuthError::Invalid)?;
    if now_s >= exp {
        return Err(AuthError::Invalid);
    }
    if let Some(nbf) = claims.get("nbf")
        && nbf.as_f64().is_none_or(|nbf| nbf > now_s)
    {
        return Err(AuthError::Invalid);
    }

    let user_id = claims
        .get("sub")
        .and_then(|s| s.as_str())
        .and_then(positive_int)
        .ok_or(AuthError::Payload)?;
    let username = claims
        .get("username")
        .and_then(|u| u.as_str())
        .ok_or(AuthError::Payload)?;
    Ok(Claims {
        user_id,
        username: username.into(),
    })
}

fn decode_json(part: &[u8]) -> Result<serde_json::Value, AuthError> {
    let bytes = URL_SAFE_NO_PAD
        .decode(part)
        .map_err(|_| AuthError::Invalid)?;
    serde_json::from_slice(&bytes).map_err(|_| AuthError::Invalid)
}

pub fn positive_int(s: &str) -> Option<i64> {
    if s.is_empty() || !s.bytes().all(|b| b.is_ascii_digit()) {
        return None;
    }
    s.parse().ok().filter(|&n| n > 0)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sign(secret: &[u8], header: &str, payload: &str) -> String {
        let h = URL_SAFE_NO_PAD.encode(header);
        let p = URL_SAFE_NO_PAD.encode(payload);
        let mut mac = Hmac::<Sha256>::new_from_slice(secret).unwrap();
        mac.update(format!("{h}.{p}").as_bytes());
        format!(
            "Bearer {h}.{p}.{}",
            URL_SAFE_NO_PAD.encode(mac.finalize().into_bytes())
        )
    }

    const H: &str = r#"{"alg":"HS256","typ":"JWT"}"#;

    #[test]
    fn tokens() {
        let key = Key::new(b"s");
        let check =
            |t: &str| verify(&key, Some(t.as_bytes()), 1000.0).map(|c| (c.user_id, c.username));
        assert_eq!(
            check(&sign(b"s", H, r#"{"sub":"7","username":"u","exp":2000}"#)),
            Ok((7, "u".into()))
        );
        assert_eq!(
            check(&sign(
                b"s",
                r#"{"typ":"JWT","alg":"HS256"}"#,
                r#"{"sub":"7","username":"u","exp":2000}"#
            )),
            Ok((7, "u".into()))
        );
        assert_eq!(
            check(&sign(b"x", H, r#"{"sub":"7","username":"u","exp":2000}"#)),
            Err(AuthError::Invalid)
        );
        assert_eq!(
            check(&sign(
                b"s",
                r#"{"alg":"HS384"}"#,
                r#"{"sub":"7","username":"u","exp":2000}"#
            )),
            Err(AuthError::Invalid)
        );
        assert_eq!(
            check(&sign(b"s", H, r#"{"sub":"7","username":"u","exp":999}"#)),
            Err(AuthError::Invalid)
        );
        assert_eq!(
            check(&sign(b"s", H, r#"{"sub":"7","username":"u"}"#)),
            Err(AuthError::Invalid)
        );
        assert_eq!(
            check(&sign(b"s", H, r#"{"sub":"abc","username":"u","exp":2000}"#)),
            Err(AuthError::Payload)
        );
        assert_eq!(
            check(&sign(b"s", H, r#"{"sub":7,"username":"u","exp":2000}"#)),
            Err(AuthError::Payload)
        );
        assert_eq!(
            check(&sign(b"s", H, r#"{"sub":"7","exp":2000}"#)),
            Err(AuthError::Payload)
        );
        assert_eq!(check("Bearer abc"), Err(AuthError::Invalid));
        assert_eq!(check("Basic abc"), Err(AuthError::Missing));
        assert!(verify(&key, None, 0.0).is_err_and(|e| e == AuthError::Missing));
    }
}
