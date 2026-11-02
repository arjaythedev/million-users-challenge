//! HS256 JWT verification (SPEC.md "Auth"): the signature and `exp` are checked on every request.

use std::time::{SystemTime, UNIX_EPOCH};

use base64::{
    Engine, alphabet,
    engine::{DecodePaddingMode, GeneralPurpose, GeneralPurposeConfig},
};
use hmac::{Hmac, KeyInit, Mac};
use serde_json::Value;
use sha2::Sha256;

use crate::api::{self, Reply};

/// base64url. JWTs carry no padding, but don't reject it.
const B64: GeneralPurpose = GeneralPurpose::new(
    &alphabet::URL_SAFE,
    GeneralPurposeConfig::new().with_decode_padding_mode(DecodePaddingMode::Indifferent),
);

pub struct Jwt {
    /// HMAC-SHA256 keyed with JWT_SECRET once, cloned for each verification.
    key: Hmac<Sha256>,
}

pub struct User {
    pub id: i64,
    pub name: String,
}

impl Jwt {
    pub fn new(secret: &[u8]) -> Jwt {
        Jwt { key: Hmac::new_from_slice(secret).expect("HMAC takes a key of any length") }
    }

    /// The user from an `Authorization: Bearer <jwt>` header, or the 401 to answer with.
    pub fn verify(&self, authorization: Option<&[u8]>) -> Result<User, Reply> {
        let token = authorization.and_then(|v| v.strip_prefix(b"Bearer ")).ok_or(api::MISSING_TOKEN)?;
        let claims = self.claims(token.trim_ascii()).ok_or(api::INVALID_TOKEN)?;
        let id = claims.get("sub").and_then(Value::as_str).and_then(api::positive_int);
        match (id, claims.get("username")) {
            (Some(id), Some(Value::String(name))) => Ok(User { id, name: name.clone() }),
            _ => Err(api::INVALID_PAYLOAD),
        }
    }

    /// The payload of a token with a valid HS256 signature that hasn't expired.
    fn claims(&self, token: &[u8]) -> Option<Value> {
        let mut parts = token.split(|&b| b == b'.');
        let (header, payload, signature) = (parts.next()?, parts.next()?, parts.next()?);
        if parts.next().is_some() {
            return None;
        }
        let mut sig = [0u8; 64];
        let sig_len = B64.decode_slice(signature, &mut sig).ok()?;
        let mut mac = self.key.clone();
        mac.update(&token[..header.len() + 1 + payload.len()]);
        mac.verify_slice(&sig[..sig_len]).ok()?; // constant time; also rejects a wrong length

        let header: Value = serde_json::from_slice(&B64.decode(header).ok()?).ok()?;
        if header.get("alg")?.as_str()? != "HS256" {
            return None;
        }
        let claims: Value = serde_json::from_slice(&B64.decode(payload).ok()?).ok()?;
        let exp = claims.get("exp")?.as_f64()?;
        let now = SystemTime::now().duration_since(UNIX_EPOCH).ok()?.as_secs_f64();
        (now < exp).then_some(claims)
    }
}
