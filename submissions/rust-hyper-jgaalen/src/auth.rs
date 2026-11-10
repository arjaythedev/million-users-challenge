use base64::{Engine, engine::general_purpose::URL_SAFE_NO_PAD};
use hmac::{Hmac, Mac};
use serde_json::Value;
use sha2::Sha256;
use std::time::{SystemTime, UNIX_EPOCH};

pub struct User {
    pub id: i64,
    pub name: String,
}

pub fn positive_id(value: &str) -> Option<i64> {
    if value.is_empty() || !value.bytes().all(|c| c.is_ascii_digit()) {
        return None;
    }
    value.parse::<i64>().ok().filter(|v| *v > 0)
}

pub fn verify(header: Option<&str>, secret: &[u8]) -> Result<User, &'static str> {
    let token = header
        .and_then(|h| h.strip_prefix("Bearer "))
        .ok_or("missing bearer token")?;
    let invalid = "invalid or expired token";
    let parts: Vec<_> = token.split('.').collect();
    if parts.len() != 3 {
        return Err(invalid);
    }
    let signature = URL_SAFE_NO_PAD.decode(parts[2]).map_err(|_| invalid)?;
    let mut mac = Hmac::<Sha256>::new_from_slice(secret).map_err(|_| invalid)?;
    mac.update(parts[0].as_bytes());
    mac.update(b".");
    mac.update(parts[1].as_bytes());
    mac.verify_slice(&signature).map_err(|_| invalid)?;
    let decode = |s: &str| -> Result<Value, &'static str> {
        serde_json::from_slice(&URL_SAFE_NO_PAD.decode(s).map_err(|_| invalid)?)
            .map_err(|_| invalid)
    };
    let header = decode(parts[0])?;
    if header["alg"].as_str() != Some("HS256") {
        return Err(invalid);
    }
    let payload = decode(parts[1])?;
    let now = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map_err(|_| invalid)?
        .as_secs_f64();
    let exp = payload["exp"]
        .as_f64()
        .filter(|v| v.is_finite())
        .ok_or(invalid)?;
    if exp <= now {
        return Err(invalid);
    }
    if let Some(nbf) = payload.get("nbf")
        && nbf
            .as_f64()
            .filter(|v| v.is_finite() && *v <= now)
            .is_none()
    {
        return Err(invalid);
    }
    let bad_payload = "invalid token payload";
    let id = payload["sub"]
        .as_str()
        .and_then(positive_id)
        .ok_or(bad_payload)?;
    let name = payload["username"].as_str().ok_or(bad_payload)?.to_owned();
    Ok(User { id, name })
}

#[cfg(test)]
mod tests {
    use super::*;
    fn token(payload: Value, alg: &str) -> String {
        let h =
            URL_SAFE_NO_PAD.encode(serde_json::to_vec(&serde_json::json!({"alg":alg})).unwrap());
        let p = URL_SAFE_NO_PAD.encode(serde_json::to_vec(&payload).unwrap());
        let data = format!("{h}.{p}");
        let mut mac = Hmac::<Sha256>::new_from_slice(b"secret").unwrap();
        mac.update(data.as_bytes());
        format!(
            "Bearer {data}.{}",
            URL_SAFE_NO_PAD.encode(mac.finalize().into_bytes())
        )
    }
    #[test]
    fn validates_every_token() {
        let future = SystemTime::now()
            .duration_since(UNIX_EPOCH)
            .unwrap()
            .as_secs()
            + 3600;
        let valid = serde_json::json!({"sub":"1","username":"u","exp":future});
        assert_eq!(
            verify(Some(&token(valid.clone(), "HS256")), b"secret")
                .unwrap()
                .id,
            1
        );
        assert!(verify(Some(&token(valid.clone(), "none")), b"secret").is_err());
        assert!(verify(Some(&token(valid.clone(), "HS256")), b"wrong").is_err());
        for patch in [
            serde_json::json!({"exp":0}),
            serde_json::json!({"exp":null}),
            serde_json::json!({"sub":1}),
            serde_json::json!({"sub":" 1"}),
            serde_json::json!({"username":null}),
        ] {
            let mut p = valid.clone();
            for (k, v) in patch.as_object().unwrap() {
                p[k] = v.clone();
            }
            assert!(verify(Some(&token(p, "HS256")), b"secret").is_err());
        }
    }
    #[test]
    fn strict_ids() {
        for s in ["", "0", "-1", "+1", "1.5", " 1", "abc"] {
            assert!(positive_id(s).is_none());
        }
        assert_eq!(positive_id("123"), Some(123));
    }
}
