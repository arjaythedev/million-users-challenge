use crate::crypto::{base64url_decode, HmacSha256Key};
use crate::json::{
    jwt_header_is_hs256, parse_jwt_claims, parse_positive_id, parse_post_body, JwtClaimsResult,
    PostBodyParse,
};
use crate::sys::{igzip_compress_gzip, IgzipCtx};

pub const HDR_HEADROOM: usize = 160;

pub const RESP_404_NOT_FOUND: &[u8] = b"HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 21\r\n\r\n{\"error\":\"not found\"}";
pub const RESP_404_POST_NOT_FOUND: &[u8] = b"HTTP/1.1 404 Not Found\r\nContent-Type: application/json\r\nContent-Length: 26\r\n\r\n{\"error\":\"post not found\"}";
pub const RESP_400_INVALID_POST_ID: &[u8] = b"HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 27\r\n\r\n{\"error\":\"invalid post id\"}";
pub const RESP_400_MALFORMED_JSON: &[u8] = b"HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 31\r\n\r\n{\"error\":\"malformed JSON body\"}";
pub const RESP_400_BODY_REQUIRED: &[u8] = b"HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 28\r\n\r\n{\"error\":\"body is required\"}";
pub const RESP_400_BODY_TOO_LONG: &[u8] = b"HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 47\r\n\r\n{\"error\":\"body must be at most 500 characters\"}";
pub const RESP_401_MISSING_BEARER: &[u8] = b"HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 32\r\n\r\n{\"error\":\"missing bearer token\"}";
pub const RESP_401_INVALID_TOKEN: &[u8] = b"HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 36\r\n\r\n{\"error\":\"invalid or expired token\"}";
pub const RESP_401_INVALID_PAYLOAD: &[u8] = b"HTTP/1.1 401 Unauthorized\r\nContent-Type: application/json\r\nContent-Length: 33\r\n\r\n{\"error\":\"invalid token payload\"}";
pub const RESP_500_INTERNAL: &[u8] = b"HTTP/1.1 500 Internal Server Error\r\nContent-Type: application/json\r\nContent-Length: 33\r\n\r\n{\"error\":\"internal server error\"}";
pub const RESP_400_BAD_HTTP: &[u8] = b"HTTP/1.1 400 Bad Request\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: 23\r\n\r\n{\"error\":\"bad request\"}";

pub enum ParsedAction<'a> {
    NeedMoreData,
    BadHttp,
    Immediate {
        consumed: usize,
        close_after: bool,
        resp: &'static [u8],
    },
    Feed {
        consumed: usize,
        close_after: bool,
        accept_gzip: bool,
    },
    GetPost {
        consumed: usize,
        close_after: bool,
        accept_gzip: bool,
        post_id: i64,
    },
    Health {
        consumed: usize,
        close_after: bool,
        accept_gzip: bool,
    },
    CreatePost {
        consumed: usize,
        close_after: bool,
        accept_gzip: bool,
        user_id: i64,
        username: &'a [u8],
        body: &'a [u8],
    },
    LikePost {
        consumed: usize,
        close_after: bool,
        accept_gzip: bool,
        user_id: i64,
        post_id: i64,
    },
}

#[inline(always)]
fn find_crlf(buf: &[u8], mut i: usize) -> Option<usize> {
    let len = buf.len();
    #[cfg(all(target_arch = "x86_64", target_feature = "avx2"))]
    unsafe {
        use std::arch::x86_64::*;
        let cr = _mm256_set1_epi8(b'\r' as i8);
        let lf = _mm256_set1_epi8(b'\n' as i8);
        let ptr = buf.as_ptr();
        while i + 33 <= len {
            let v0 = _mm256_loadu_si256(ptr.add(i) as *const __m256i);
            let v1 = _mm256_loadu_si256(ptr.add(i + 1) as *const __m256i);
            let m = _mm256_movemask_epi8(_mm256_and_si256(
                _mm256_cmpeq_epi8(v0, cr),
                _mm256_cmpeq_epi8(v1, lf),
            )) as u32;
            if m != 0 {
                return Some(i + m.trailing_zeros() as usize);
            }
            i += 32;
        }
    }
    #[cfg(target_arch = "x86_64")]
    unsafe {
        use std::arch::x86_64::*;
        let cr = _mm_set1_epi8(b'\r' as i8);
        let lf = _mm_set1_epi8(b'\n' as i8);
        let ptr = buf.as_ptr();
        while i + 17 <= len {
            let v0 = _mm_loadu_si128(ptr.add(i) as *const __m128i);
            let v1 = _mm_loadu_si128(ptr.add(i + 1) as *const __m128i);
            let m = _mm_movemask_epi8(_mm_and_si128(_mm_cmpeq_epi8(v0, cr), _mm_cmpeq_epi8(v1, lf))) as u32;
            if m != 0 {
                return Some(i + m.trailing_zeros() as usize);
            }
            i += 16;
        }
    }
    while i + 1 < len {
        if buf[i + 1] == b'\n' && buf[i] == b'\r' {
            return Some(i);
        }
        i += 1;
    }
    None
}

#[inline(always)]
fn trim_ows(mut s: &[u8]) -> &[u8] {
    while let Some(&b) = s.first() {
        if b == b' ' || b == b'\t' {
            s = &s[1..];
        } else {
            break;
        }
    }
    while let Some(&b) = s.last() {
        if b == b' ' || b == b'\t' {
            s = &s[..s.len() - 1];
        } else {
            break;
        }
    }
    s
}

#[inline(always)]
fn accepts_gzip(val: &[u8]) -> bool {
    if val == b"gzip" {
        return true;
    }
    for part in val.split(|&b| b == b',') {
        let p = trim_ows(part);
        let (coding, params) = match p.iter().position(|&b| b == b';') {
            Some(idx) => (trim_ows(&p[..idx]), Some(&p[idx + 1..])),
            None => (p, None),
        };
        if coding.eq_ignore_ascii_case(b"gzip") {
            if let Some(prm) = params {
                for seg in prm.split(|&b| b == b';') {
                    let kv = trim_ows(seg);
                    if kv.len() >= 3 && (kv[0] == b'q' || kv[0] == b'Q') && kv[1] == b'=' {
                        let qv = trim_ows(&kv[2..]);
                        if !qv.is_empty() && qv.iter().all(|&c| c == b'0' || c == b'.') {
                            return false;
                        }
                    }
                }
            }
            return true;
        }
    }
    false
}

#[inline(always)]
fn connection_has_token(val: &[u8], needle: &[u8]) -> bool {
    for part in val.split(|&b| b == b',') {
        if trim_ows(part).eq_ignore_ascii_case(needle) {
            return true;
        }
    }
    false
}

#[inline(always)]
fn parse_content_length(val: &[u8]) -> Option<usize> {
    let s = trim_ows(val);
    if s.is_empty() || s.len() > 8 {
        return None;
    }
    let mut n: usize = 0;
    for &b in s {
        if !b.is_ascii_digit() {
            return None;
        }
        n = n * 10 + (b - b'0') as usize;
    }
    Some(n)
}

enum AuthCheck {
    Ok { user_id: i64 },
    Err(&'static [u8]),
}

fn verify_bearer_jwt(
    auth_hdr: Option<&[u8]>,
    hmac_key: &HmacSha256Key,
    now_sec: i64,
    scratch: &mut HttpScratch,
) -> AuthCheck {
    let Some(raw_auth) = auth_hdr else {
        return AuthCheck::Err(RESP_401_MISSING_BEARER);
    };
    if raw_auth.len() < 7 || !raw_auth.starts_with(b"Bearer ") {
        return AuthCheck::Err(RESP_401_MISSING_BEARER);
    }
    let token = trim_ows(&raw_auth[7..]);
    if token.is_empty() || token.len() > 1200 {
        return AuthCheck::Err(RESP_401_INVALID_TOKEN);
    }

    let Some(dot1) = token.iter().position(|&b| b == b'.') else {
        return AuthCheck::Err(RESP_401_INVALID_TOKEN);
    };
    let rest = &token[dot1 + 1..];
    let Some(dot2_rel) = rest.iter().position(|&b| b == b'.') else {
        return AuthCheck::Err(RESP_401_INVALID_TOKEN);
    };
    let dot2 = dot1 + 1 + dot2_rel;
    let sig_b64 = &token[dot2 + 1..];
    if dot1 == 0 || dot2_rel == 0 || sig_b64.is_empty() || sig_b64.contains(&b'.') {
        return AuthCheck::Err(RESP_401_INVALID_TOKEN);
    }

    let hdr_b64 = &token[..dot1];
    const HDR_HS256_STD: &[u8] = b"eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
    const HDR_HS256_ALT: &[u8] = b"eyJ0eXAiOiJKV1QiLCJhbGciOiJIUzI1NiJ9";
    let hdr_ok = if hdr_b64 == HDR_HS256_STD || hdr_b64 == HDR_HS256_ALT {
        true
    } else if let Some(hdr_json) = base64url_decode(hdr_b64, &mut scratch.hdr_dec) {
        jwt_header_is_hs256(hdr_json, &mut scratch.key_buf, &mut scratch.val_buf)
    } else {
        false
    };
    if !hdr_ok {
        return AuthCheck::Err(RESP_401_INVALID_TOKEN);
    }

    let mut sig_buf = [0u8; 64];
    let Some(sig_bytes) = base64url_decode(sig_b64, &mut sig_buf) else {
        return AuthCheck::Err(RESP_401_INVALID_TOKEN);
    };
    let Ok(expected_sig): Result<&[u8; 32], _> = sig_bytes.try_into() else {
        return AuthCheck::Err(RESP_401_INVALID_TOKEN);
    };

    if !hmac_key.verify_ct(&token[..dot2], expected_sig) {
        return AuthCheck::Err(RESP_401_INVALID_TOKEN);
    }

    let pay_b64 = &token[dot1 + 1..dot2];
    let Some(pay_json) = base64url_decode(pay_b64, &mut scratch.pay_dec) else {
        return AuthCheck::Err(RESP_401_INVALID_TOKEN);
    };

    match parse_jwt_claims(
        pay_json,
        now_sec,
        &mut scratch.username,
        &mut scratch.key_buf,
        &mut scratch.val_buf,
    ) {
        JwtClaimsResult::Ok { user_id } => AuthCheck::Ok { user_id },
        JwtClaimsResult::InvalidOrExpired => AuthCheck::Err(RESP_401_INVALID_TOKEN),
        JwtClaimsResult::InvalidPayload => AuthCheck::Err(RESP_401_INVALID_PAYLOAD),
    }
}

pub struct HttpScratch {
    pub hdr_dec: [u8; 256],
    pub pay_dec: [u8; 1024],
    pub username: Vec<u8>,
    pub body: Vec<u8>,
    pub key_buf: Vec<u8>,
    pub val_buf: Vec<u8>,
}

impl HttpScratch {
    pub fn new() -> Self {
        Self {
            hdr_dec: [0u8; 256],
            pay_dec: [0u8; 1024],
            username: Vec::with_capacity(64),
            body: Vec::with_capacity(2048),
            key_buf: Vec::with_capacity(32),
            val_buf: Vec::with_capacity(32),
        }
    }
}

pub fn parse_http_request<'a>(
    buf: &[u8],
    hmac_key: &HmacSha256Key,
    now_sec: i64,
    scratch: &'a mut HttpScratch,
) -> ParsedAction<'a> {
    let Some(req_line_end) = find_crlf(buf, 0) else {
        return if buf.len() > 8192 {
            ParsedAction::BadHttp
        } else {
            ParsedAction::NeedMoreData
        };
    };

    let req_line = &buf[..req_line_end];
    let (method, raw_target, is_http10): (&[u8], &[u8], bool) = if req_line == b"GET /feed HTTP/1.1"
    {
        (b"GET", b"/feed", false)
    } else {
        let Some(sp1) = req_line.iter().position(|&b| b == b' ') else {
            std::hint::cold_path();
            return ParsedAction::BadHttp;
        };
        let m = &req_line[..sp1];
        let after_m = &req_line[sp1 + 1..];
        let Some(sp2) = after_m.iter().position(|&b| b == b' ') else {
            std::hint::cold_path();
            return ParsedAction::BadHttp;
        };
        let rt = &after_m[..sp2];
        let ver = &after_m[sp2 + 1..];
        if rt.is_empty() || !ver.starts_with(b"HTTP/1.") {
            std::hint::cold_path();
            return ParsedAction::BadHttp;
        }
        (m, rt, ver == b"HTTP/1.0")
    };

    let mut pos = req_line_end + 2;
    let mut content_len: usize = 0;
    let mut accept_gzip = false;
    let mut conn_close = is_http10;
    let mut auth_range: Option<(usize, usize)> = None;

    loop {
        let Some(line_end) = find_crlf(buf, pos) else {
            return if buf.len() > 32768 {
                std::hint::cold_path();
                ParsedAction::BadHttp
            } else {
                ParsedAction::NeedMoreData
            };
        };
        if line_end == pos {
            pos = line_end + 2;
            break;
        }
        let line = &buf[pos..line_end];
        let c0 = line[0] | 0x20;
        if c0 != b'a' && c0 != b'c' {
            pos = line_end + 2;
            continue;
        }
        if line == b"Accept-Encoding: gzip" {
            accept_gzip = true;
            pos = line_end + 2;
            continue;
        }
        if line == b"Content-Length: 0" {
            content_len = 0;
            pos = line_end + 2;
            continue;
        }
        if line == b"Content-Type: application/json" {
            pos = line_end + 2;
            continue;
        }
        if line.len() > 22 && line.starts_with(b"Authorization: ") {
            let val = trim_ows(&line[15..]);
            let v_start = (val.as_ptr() as usize) - (buf.as_ptr() as usize);
            auth_range = Some((v_start, v_start + val.len()));
            pos = line_end + 2;
            continue;
        }
        if line.len() > 16 && line.starts_with(b"Content-Length: ") {
            match parse_content_length(&line[16..]) {
                Some(n) if n <= 32768 => {
                    content_len = n;
                    pos = line_end + 2;
                    continue;
                }
                _ => {
                    std::hint::cold_path();
                    return ParsedAction::BadHttp;
                }
            }
        }
        if let Some(colon) = line.iter().position(|&b| b == b':') {
            let name = &line[..colon];
            let val = trim_ows(&line[colon + 1..]);
            match name.len() {
                10 if name.eq_ignore_ascii_case(b"connection") => {
                    if connection_has_token(val, b"close") {
                        conn_close = true;
                    } else if is_http10 && connection_has_token(val, b"keep-alive") {
                        conn_close = false;
                    }
                }
                13 if name.eq_ignore_ascii_case(b"authorization") => {
                    let v_start = (val.as_ptr() as usize) - (buf.as_ptr() as usize);
                    auth_range = Some((v_start, v_start + val.len()));
                }
                14 if name.eq_ignore_ascii_case(b"content-length") => {
                    match parse_content_length(val) {
                        Some(n) if n <= 32768 => content_len = n,
                        _ => return ParsedAction::BadHttp,
                    }
                }
                15 if name.eq_ignore_ascii_case(b"accept-encoding") => {
                    if accepts_gzip(val) {
                        accept_gzip = true;
                    }
                }
                _ => {}
            }
        }
        pos = line_end + 2;
    }

    let total_len = pos + content_len;
    if buf.len() < total_len {
        return ParsedAction::NeedMoreData;
    }

    let path = match raw_target.iter().position(|&b| b == b'?') {
        Some(q) => &raw_target[..q],
        None => raw_target,
    };

    if method == b"GET" {
        if path == b"/feed" {
            return ParsedAction::Feed {
                consumed: total_len,
                close_after: conn_close,
                accept_gzip,
            };
        }
        if path.starts_with(b"/posts/") {
            let id_seg = &path[7..];
            if !id_seg.is_empty() && !id_seg.contains(&b'/') {
                return match parse_positive_id(id_seg) {
                    Some(post_id) => ParsedAction::GetPost {
                        consumed: total_len,
                        close_after: conn_close,
                        accept_gzip,
                        post_id,
                    },
                    None => ParsedAction::Immediate {
                        consumed: total_len,
                        close_after: conn_close,
                        resp: RESP_400_INVALID_POST_ID,
                    },
                };
            }
        }
        if path == b"/health" {
            return ParsedAction::Health {
                consumed: total_len,
                close_after: conn_close,
                accept_gzip,
            };
        }
        return ParsedAction::Immediate {
            consumed: total_len,
            close_after: conn_close,
            resp: RESP_404_NOT_FOUND,
        };
    }

    if method == b"POST" {
        if path == b"/posts" {
            let auth_slice = auth_range.map(|(a, b)| &buf[a..b]);
            let user_id = match verify_bearer_jwt(auth_slice, hmac_key, now_sec, scratch) {
                AuthCheck::Ok { user_id } => user_id,
                AuthCheck::Err(resp) => {
                    return ParsedAction::Immediate {
                        consumed: total_len,
                        close_after: conn_close,
                        resp,
                    };
                }
            };

            let req_body = &buf[pos..total_len];
            let trimmed_body =
                match parse_post_body(req_body, &mut scratch.body, &mut scratch.key_buf) {
                PostBodyParse::Ok(s) => s,
                PostBodyParse::MalformedJson => {
                    return ParsedAction::Immediate {
                        consumed: total_len,
                        close_after: conn_close,
                        resp: RESP_400_MALFORMED_JSON,
                    };
                }
                PostBodyParse::BodyRequired => {
                    return ParsedAction::Immediate {
                        consumed: total_len,
                        close_after: conn_close,
                        resp: RESP_400_BODY_REQUIRED,
                    };
                }
                PostBodyParse::BodyTooLong => {
                    return ParsedAction::Immediate {
                        consumed: total_len,
                        close_after: conn_close,
                        resp: RESP_400_BODY_TOO_LONG,
                    };
                }
            };

            return ParsedAction::CreatePost {
                consumed: total_len,
                close_after: conn_close,
                accept_gzip,
                user_id,
                username: scratch.username.as_slice(),
                body: trimmed_body,
            };
        }

        if path.starts_with(b"/posts/") && path.ends_with(b"/like") && path.len() > 12 {
            let id_seg = &path[7..path.len() - 5];
            if !id_seg.contains(&b'/') {
                let auth_slice = auth_range.map(|(a, b)| &buf[a..b]);
                let user_id = match verify_bearer_jwt(auth_slice, hmac_key, now_sec, scratch) {
                    AuthCheck::Ok { user_id } => user_id,
                    AuthCheck::Err(resp) => {
                        return ParsedAction::Immediate {
                            consumed: total_len,
                            close_after: conn_close,
                            resp,
                        };
                    }
                };

                return match parse_positive_id(id_seg) {
                    Some(post_id) => ParsedAction::LikePost {
                        consumed: total_len,
                        close_after: conn_close,
                        accept_gzip,
                        user_id,
                        post_id,
                    },
                    None => ParsedAction::Immediate {
                        consumed: total_len,
                        close_after: conn_close,
                        resp: RESP_400_INVALID_POST_ID,
                    },
                };
            }
        }

        return ParsedAction::Immediate {
            consumed: total_len,
            close_after: conn_close,
            resp: RESP_404_NOT_FOUND,
        };
    }

    ParsedAction::Immediate {
        consumed: total_len,
        close_after: conn_close,
        resp: RESP_404_NOT_FOUND,
    }
}

#[inline(always)]
fn status_line(status: u16) -> &'static [u8] {
    match status {
        200 => b"HTTP/1.1 200 OK\r\n",
        201 => b"HTTP/1.1 201 Created\r\n",
        400 => b"HTTP/1.1 400 Bad Request\r\n",
        401 => b"HTTP/1.1 401 Unauthorized\r\n",
        404 => b"HTTP/1.1 404 Not Found\r\n",
        503 => b"HTTP/1.1 503 Service Unavailable\r\n",
        _ => b"HTTP/1.1 500 Internal Server Error\r\n",
    }
}

#[inline(always)]
unsafe fn prepend_http_headers_ptr(
    dst_base: *mut u8,
    body_start: usize,
    body_len: usize,
    status: u16,
    gzipped: bool,
    close_after: bool,
) -> usize {
    let mut cur = body_start;
    cur -= 4;
    std::ptr::copy_nonoverlapping(b"\r\n\r\n".as_ptr(), dst_base.add(cur), 4);

    let mut v = body_len;
    if v == 0 {
        cur -= 1;
        *dst_base.add(cur) = b'0';
    } else {
        while v > 0 {
            cur -= 1;
            *dst_base.add(cur) = b'0' + (v % 10) as u8;
            v /= 10;
        }
    }

    if status == 200 && !close_after {
        if gzipped {
            const PFX_200_GZ: &[u8] = b"HTTP/1.1 200 OK\r\nContent-Encoding: gzip\r\nContent-Type: application/json\r\nContent-Length: ";
            cur -= PFX_200_GZ.len();
            std::ptr::copy_nonoverlapping(PFX_200_GZ.as_ptr(), dst_base.add(cur), PFX_200_GZ.len());
            return cur;
        } else {
            const PFX_200_RAW: &[u8] =
                b"HTTP/1.1 200 OK\r\nContent-Type: application/json\r\nContent-Length: ";
            cur -= PFX_200_RAW.len();
            std::ptr::copy_nonoverlapping(
                PFX_200_RAW.as_ptr(),
                dst_base.add(cur),
                PFX_200_RAW.len(),
            );
            return cur;
        }
    }

    const CT_CL: &[u8] = b"Content-Type: application/json\r\nContent-Length: ";
    cur -= CT_CL.len();
    std::ptr::copy_nonoverlapping(CT_CL.as_ptr(), dst_base.add(cur), CT_CL.len());

    if gzipped {
        const CE_GZ: &[u8] = b"Content-Encoding: gzip\r\n";
        cur -= CE_GZ.len();
        std::ptr::copy_nonoverlapping(CE_GZ.as_ptr(), dst_base.add(cur), CE_GZ.len());
    }
    if close_after {
        const C_CL: &[u8] = b"Connection: close\r\n";
        cur -= C_CL.len();
        std::ptr::copy_nonoverlapping(C_CL.as_ptr(), dst_base.add(cur), C_CL.len());
    }
    let st = status_line(status);
    cur -= st.len();
    std::ptr::copy_nonoverlapping(st.as_ptr(), dst_base.add(cur), st.len());
    cur
}

pub enum ReplyPayload<'a> {
    Static(&'static [u8]),
    OwnedSlice {
        buf: &'a mut Vec<u8>,
        start: usize,
        end: usize,
    },
}

pub fn finalize_json_reply<'a>(
    raw_buf: &'a mut Vec<u8>,
    zbuf: &'a mut Vec<u8>,
    status: u16,
    accept_gzip: bool,
    close_after: bool,
    gz_min_bytes: usize,
    igzip: *mut IgzipCtx,
    now_ms: u64,
) -> ReplyPayload<'a> {
    let body_len = raw_buf.len() - HDR_HEADROOM;
    if accept_gzip && body_len >= gz_min_bytes && !igzip.is_null() {
        let max_dst = body_len + 64;
        let needed = HDR_HEADROOM + max_dst;
        if zbuf.capacity() < needed {
            zbuf.reserve(needed - zbuf.len());
        }
        let zlen = unsafe {
            igzip_compress_gzip(
                igzip,
                raw_buf.as_ptr().add(HDR_HEADROOM),
                body_len,
                zbuf.as_mut_ptr().add(HDR_HEADROOM),
                max_dst,
                now_ms,
            )
        };
        if zlen > 0 && zlen + 26 < body_len {
            let end = HDR_HEADROOM + zlen;
            let start = unsafe {
                let s = prepend_http_headers_ptr(
                    zbuf.as_mut_ptr(),
                    HDR_HEADROOM,
                    zlen,
                    status,
                    true,
                    close_after,
                );
                if s > 0 && zbuf.len() < s {
                    std::ptr::write_bytes(zbuf.as_mut_ptr(), 0, s);
                }
                zbuf.set_len(end);
                s
            };
            return ReplyPayload::OwnedSlice {
                buf: zbuf,
                start,
                end,
            };
        }
    }
    let end = raw_buf.len();
    let start = unsafe {
        prepend_http_headers_ptr(
            raw_buf.as_mut_ptr(),
            HDR_HEADROOM,
            body_len,
            status,
            false,
            close_after,
        )
    };
    ReplyPayload::OwnedSlice {
        buf: raw_buf,
        start,
        end,
    }
}
