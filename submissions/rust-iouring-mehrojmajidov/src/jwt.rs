//! HS256 bearer tokens. Every request's signature and `exp` are checked; nothing is remembered between requests.

use crate::crypto::{b64_decode, b64_decode_into, Hmac};
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

/// The members of a payload that the check looks at. A member of another type than the one wanted is `Err`.
#[derive(Default, Debug, PartialEq)]
struct Claims<'a> {
    sub: Option<Result<&'a [u8], ()>>,
    username: Option<Result<&'a [u8], ()>>,
    exp: Option<Result<f64, ()>>,
    nbf: Option<Result<f64, ()>>,
}

impl<'a> Claims<'a> {
    /// The payload as nearly every token has it: one object without spaces, its values plain strings (no
    /// escapes, printable ASCII) and whole numbers of at most 15 digits, which a double holds exactly. `None`
    /// for anything else, and the full JSON reader decides.
    fn plain(raw: &'a [u8]) -> Option<Claims<'a>> {
        let text = |at: usize| -> Option<usize> {
            let len = raw[at..].iter().position(|&b| !(0x20..0x7f).contains(&b) || b == b'"' || b == b'\\')?;
            (raw[at + len] == b'"').then_some(at + len)
        };
        let mut claims = Claims::default();
        if raw.first() != Some(&b'{') {
            return None;
        }
        let mut i = 1;
        loop {
            if raw.get(i) != Some(&b'"') {
                return None;
            }
            let key_end = text(i + 1)?;
            let key = &raw[i + 1..key_end];
            if raw.get(key_end + 1) != Some(&b':') {
                return None;
            }
            i = key_end + 2;
            let (string, number);
            if raw.get(i) == Some(&b'"') {
                let end = text(i + 1)?;
                (string, number) = (Ok(&raw[i + 1..end]), Err(()));
                i = end + 1;
            } else {
                let digits = raw[i..].iter().take_while(|b| b.is_ascii_digit()).count();
                if digits == 0 || digits > 15 || (digits > 1 && raw[i] == b'0') {
                    return None;
                }
                (string, number) = (Err(()), Ok(raw[i..i + digits].iter().fold(0u64, |a, &d| a * 10 + (d - b'0') as u64) as f64));
                i += digits;
            }
            // a name that repeats: the last one counts, as in `JSON.parse`
            match key {
                b"sub" => claims.sub = Some(string),
                b"username" => claims.username = Some(string),
                b"exp" => claims.exp = Some(number),
                b"nbf" => claims.nbf = Some(number),
                _ => {}
            }
            match raw.get(i) {
                Some(b',') => i += 1,
                Some(b'}') => return (i + 1 == raw.len()).then_some(claims),
                _ => return None,
            }
        }
    }

    fn of(found: &'a [Option<Value>]) -> Claims<'a> {
        let string = |v: &'a Option<Value>| {
            v.as_ref().map(|v| match v {
                Value::Str(s) => Ok(s.as_slice()),
                _ => Err(()),
            })
        };
        let number = |v: &Option<Value>| {
            v.as_ref().map(|v| match v {
                Value::Num(n) => Ok(*n),
                _ => Err(()),
            })
        };
        Claims { sub: string(&found[0]), username: string(&found[1]), exp: number(&found[2]), nbf: number(&found[3]) }
    }
}

impl Verifier {
    pub fn new(secret: &[u8]) -> Verifier {
        Verifier { hmac: Hmac::new(secret) }
    }

    /// `with_name`: whether the caller uses the username. It is checked either way; only a create keeps it.
    pub fn verify(&self, authorization: Option<&[u8]>, now_secs: i64, with_name: bool) -> Result<User, AuthError> {
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

        // 33 bytes of room: a signature of another length decodes (or fails to) and cannot be equal
        let signed = &token[..head.len() + 1 + payload.len()];
        let mut given = [0u8; 33];
        let given_len = b64_decode_into(sig, &mut given).ok_or(AuthError::Invalid)?;
        if given[..given_len] != self.hmac.mac(&[signed]) {
            return Err(AuthError::Invalid);
        }

        // decoded on the stack unless it is unusually long
        let (mut short, long);
        let raw: &[u8] = if payload.len() <= 680 {
            short = [0u8; 510];
            let n = b64_decode_into(payload, &mut short).ok_or(AuthError::Invalid)?;
            &short[..n]
        } else {
            long = b64_decode(payload).ok_or(AuthError::Invalid)?;
            &long
        };
        let found;
        let claims = match Claims::plain(raw) {
            Some(claims) => claims,
            None => {
                let Ok(Doc::Object(f)) = parse_object(raw, &[b"sub", b"username", b"exp", b"nbf"]) else {
                    return Err(AuthError::Invalid);
                };
                found = f;
                Claims::of(&found)
            }
        };
        match claims.exp {
            Some(Ok(exp)) if (now_secs as f64) < exp => {}
            _ => return Err(AuthError::Invalid),
        }
        match claims.nbf {
            None => {}
            Some(Ok(nbf)) if (now_secs as f64) >= nbf => {}
            _ => return Err(AuthError::Invalid),
        }

        let id = match claims.sub {
            Some(Ok(s)) if !s.is_empty() && s.len() <= 18 && s.iter().all(u8::is_ascii_digit) => {
                s.iter().fold(0u64, |a, &d| a * 10 + (d - b'0') as u64)
            }
            _ => return Err(AuthError::Payload),
        };
        if id == 0 {
            return Err(AuthError::Payload);
        }
        match claims.username {
            Some(Ok(name)) => Ok(User { id, name: if with_name { name.to_vec() } else { Vec::new() } }),
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
        Verifier::new(SECRET).verify(Some(&h), NOW, true)
    }

    const HS256: &str = r#"{"alg":"HS256","typ":"JWT"}"#;

    #[test]
    #[ignore = "a timing, not a check: cargo test --release -- --ignored --nocapture timing"]
    fn timing() {
        // tokens as the load test signs them, a different one each time
        let tokens: Vec<Vec<u8>> = (0..2000)
            .map(|k| {
                let mut h = b"Bearer ".to_vec();
                let user = 1 + k * 7919 % 1_480_000;
                h.extend_from_slice(&sign_with(HS256, &format!(r#"{{"sub":"{user}","username":"clever_nebula_{user}","iat":1794633757,"exp":2109993757}}"#), SECRET));
                h
            })
            .collect();
        let v = Verifier::new(SECRET);
        let (rounds, mut sum) = (200, 0);
        let t = std::time::Instant::now();
        for _ in 0..rounds {
            for token in &tokens {
                sum += v.verify(Some(std::hint::black_box(token)), NOW, true).unwrap().id;
            }
        }
        let each = t.elapsed().as_nanos() as f64 / (rounds * tokens.len()) as f64;
        println!("checking a token of {} bytes: {each:.0} ns ({sum})", tokens[0].len());
        let signed = &tokens[0][7..tokens[0].len() - 44];
        let t = std::time::Instant::now();
        for _ in 0..400_000 {
            std::hint::black_box(v.hmac.mac(&[std::hint::black_box(signed)]));
        }
        println!("  of it the signature over {} bytes: {:.0} ns", signed.len(), t.elapsed().as_nanos() as f64 / 400_000.0);
    }

    #[test]
    fn plain_payloads_read_as_the_json_reader_reads_them() {
        let keys: [&[u8]; 4] = [b"sub", b"username", b"exp", b"nbf"];
        let general = |raw: &str| match parse_object(raw.as_bytes(), &keys) {
            Ok(Doc::Object(f)) => Some(format!("{:?}", Claims::of(&f))),
            _ => None,
        };
        // taken by the short way, and read the same
        for raw in [
            r#"{"sub":"7","username":"golden_ember_1","iat":1,"exp":1800000001}"#,
            r#"{"exp":1800000001,"nbf":0,"username":"","sub":""}"#,
            r#"{"sub":"1","sub":"2","exp":5,"exp":"5","username":7,"nbf":"x"}"#,
            r#"{"sub":"a b/c <&> ~","exp":999999999999999,"other":"x","n":0}"#,
        ] {
            let short = Claims::plain(raw.as_bytes()).unwrap_or_else(|| panic!("not plain: {raw}"));
            assert_eq!(Some(format!("{short:?}")), general(raw), "{raw}");
        }
        // left to the JSON reader, whatever it then says
        for raw in [
            "{}",
            r#"{"exp":1800000001} "#,
            r#"{ "exp":1800000001}"#,
            r#"{"exp": 1800000001}"#,
            r#"{"exp":1800000001,}"#,
            r#"{"exp":01}"#,
            r#"{"exp":1.5}"#,
            r#"{"exp":1e3}"#,
            r#"{"exp":-1}"#,
            r#"{"exp":1234567890123456}"#,
            r#"{"exp":true}"#,
            r#"{"exp":null}"#,
            r#"{"exp":{"a":1}}"#,
            r#"{"exp":[1]}"#,
            r#"{"sub":"a\"b"}"#,
            r#"{"s\u0075b":"7"}"#,
            "{\"sub\":\"caf\u{e9}\"}",
            "{\"sub\":\"a\u{7f}\"}",
            "{\"sub\":\"a\tb\"}",
            r#"{"sub":"7""#,
            r#"{"sub":"7"}x"#,
            r#"{"sub""#,
            r#"{"sub":"#,
            r#"["sub"]"#,
            "",
        ] {
            assert_eq!(Claims::plain(raw.as_bytes()), None, "{raw}");
        }
    }

    #[test]
    fn plain_payloads_against_the_json_reader_at_random() {
        // payloads put together from the pieces that matter: whatever the short way takes, the JSON reader
        // must read as the same members
        let pieces: [&str; 28] = [
            "{", "}", "\"sub\"", "\"exp\"", "\"nbf\"", "\"username\"", "\"x\"", ":", ",", "\"7\"", "\"\"", "\"a b\"", "0", "7", "42",
            "1800000001", "999999999999999", "1234567890123456", "01", "-1", "1.5", "1e2", "true", "null", " ", "\"a\\\"b\"",
            "[1]", "{\"sub\":\"1\"}",
        ];
        let keys: [&[u8]; 4] = [b"sub", b"username", b"exp", b"nbf"];
        let (mut x, mut taken) = (0x9e37_79b9_7f4a_7c15u64, 0);
        for round in 0..400_000 {
            let mut next = |n: u64| {
                x ^= x << 13;
                x ^= x >> 7;
                x ^= x << 17;
                (x >> 11) % n
            };
            let mut raw = String::new();
            if round % 4 != 0 {
                // mostly well-formed: an object of members, now and then with a slip
                raw.push('{');
                for k in 0..next(5) {
                    if k > 0 {
                        raw.push(',');
                    }
                    raw += pieces[2 + next(5) as usize];
                    raw.push(':');
                    raw += pieces[if next(8) == 0 { next(28) } else { 9 + next(9) } as usize];
                }
                raw.push('}');
            } else {
                for _ in 0..next(9) {
                    raw += pieces[next(28) as usize];
                }
            }
            if let Some(short) = Claims::plain(raw.as_bytes()) {
                taken += 1;
                match parse_object(raw.as_bytes(), &keys) {
                    Ok(Doc::Object(f)) => assert_eq!(short, Claims::of(&f), "{raw}"),
                    _ => panic!("taken the short way but not an object: {raw}"),
                }
            }
        }
        assert!(taken > 20_000, "only {taken} payloads went the short way");
    }

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
        assert_eq!(v.verify(None, NOW, true).err(), Some(AuthError::Missing));
        assert_eq!(v.verify(Some(b"Basic abc"), NOW, true).err(), Some(AuthError::Missing));
        assert_eq!(v.verify(Some(b"bearer abc"), NOW, true).err(), Some(AuthError::Missing));
    }

    #[test]
    fn invalid_tokens() {
        let v = Verifier::new(SECRET);
        let ok = r#"{"sub":"7","username":"a","exp":1800000001}"#;
        assert_eq!(v.verify(Some(b"Bearer abc"), NOW, true).err(), Some(AuthError::Invalid));
        assert_eq!(v.verify(Some(b"Bearer "), NOW, true).err(), Some(AuthError::Invalid));
        assert_eq!(v.verify(Some(b"Bearer a.b.c.d"), NOW, true).err(), Some(AuthError::Invalid));

        // signed with another secret
        let mut h = b"Bearer ".to_vec();
        h.extend_from_slice(&sign_with(HS256, ok, b"other"));
        assert_eq!(v.verify(Some(&h), NOW, true).err(), Some(AuthError::Invalid));

        // signature cut off, and alg=none with an empty signature
        let mut t = sign_with(HS256, ok, SECRET);
        t.truncate(t.len() - 4);
        let mut h = b"Bearer ".to_vec();
        h.extend_from_slice(&t);
        assert_eq!(v.verify(Some(&h), NOW, true).err(), Some(AuthError::Invalid));
        let mut none = b"Bearer ".to_vec();
        none.extend_from_slice(&b64url(br#"{"alg":"none","typ":"JWT"}"#));
        none.push(b'.');
        none.extend_from_slice(&b64url(ok.as_bytes()));
        none.push(b'.');
        assert_eq!(v.verify(Some(&none), NOW, true).err(), Some(AuthError::Invalid));

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
