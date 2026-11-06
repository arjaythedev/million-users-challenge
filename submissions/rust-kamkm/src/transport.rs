//! HTTP/1.1 connections retain no request/response buffer while idle.
use super::{fast, AppState, MAX_REQUEST_BODY};
use std::{io, sync::Arc};
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt},
    net::TcpStream,
    time::{timeout, Duration},
};

const MAX_HEADERS: usize = 16 * 1024;
const MALFORMED: &[u8] = br#"{"error":"malformed JSON body"}"#;

pub(super) enum ReqMethod {
    Get,
    Post,
    Other,
}

/// What the handlers need from a request, copied out of the read buffer.
pub(super) struct Req {
    pub method: ReqMethod,
    /// The target's path, without any query.
    pub path: String,
    /// The first Authorization header, when the method is POST and the value
    /// is visible ASCII.
    pub auth: Option<String>,
    pub body: Vec<u8>,
}

pub(super) async fn serve(mut stream: TcpStream, state: Arc<AppState>) -> io::Result<()> {
    let mut pending = Vec::new();
    loop {
        if pending.is_empty() {
            // Wait for readiness without holding a buffer, then read the
            // first bytes; the request timeout starts once they arrive.
            // Readiness may be cached from the previous request, so a read
            // that would block waits again.
            let first = timeout(Duration::from_secs(180), async {
                loop {
                    stream.readable().await?;
                    let mut buffer = Vec::with_capacity(2048);
                    match stream.try_read_buf(&mut buffer) {
                        Ok(0) => return Ok(None),
                        Ok(_) => return Ok(Some(buffer)),
                        Err(error) if error.kind() == io::ErrorKind::WouldBlock => continue,
                        Err(error) => return Err(error),
                    }
                }
            })
            .await
            .map_err(io::Error::other)??;
            match first {
                Some(buffer) => pending = buffer,
                None => return Ok(()),
            }
        }
        // These futures include parsing/handling scratch state. Box them only
        // while active rather than embedding that state in every idle task.
        let parsed = timeout(
            Duration::from_secs(15),
            Box::pin(read_request(&mut stream, &mut pending)),
        )
        .await;
        let (request, gzip, close) = match parsed {
            Ok(Ok(request)) => request,
            Ok(Err(error)) if error.kind() == io::ErrorKind::UnexpectedEof => return Ok(()),
            _ => {
                stream
                    .write_all(&fast::response(400, MALFORMED, false, true))
                    .await?;
                return Ok(());
            }
        };
        let (status, body) = Box::pin(state.handle(request)).await;
        let wire = fast::response(status, &body, gzip, close);
        drop(body);
        stream.write_all(&wire).await?;
        if close {
            return Ok(());
        }
    }
}

fn invalid() -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, "invalid HTTP request")
}

/// Visible ASCII or tab: what `HeaderValue::to_str` accepts.
fn header_str(value: &[u8]) -> Option<&str> {
    value
        .iter()
        .all(|&b| b == b'\t' || (32..127).contains(&b))
        .then(|| std::str::from_utf8(value).ok())
        .flatten()
}

/// The path of a request target, as `Uri::path` gives it. Plain origin-form
/// targets are used as they are; anything else goes through `Uri`.
fn request_path(target: &str) -> io::Result<String> {
    if target.starts_with('/')
        && target
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || b"/-._~".contains(&b))
    {
        return Ok(target.to_owned());
    }
    let uri: http::Uri = target.parse().map_err(|_| invalid())?;
    Ok(uri.path().to_owned())
}

/// Returns the request, whether the client accepts gzip, and whether the
/// connection closes after the response.
async fn read_request(
    stream: &mut TcpStream,
    pending: &mut Vec<u8>,
) -> io::Result<(Req, bool, bool)> {
    let mut continued = false;
    loop {
        let mut headers = [httparse::EMPTY_HEADER; 64];
        let mut parsed = httparse::Request::new(&mut headers);
        match parsed.parse(pending).map_err(|_| invalid())? {
            httparse::Status::Complete(header_len) => {
                if header_len > MAX_HEADERS {
                    return Err(invalid());
                }
                let mut length = None;
                let mut chunked = false;
                let mut close = parsed.version == Some(0);
                let mut expect_continue = false;
                let mut host_seen = false;
                for h in parsed.headers.iter() {
                    if h.name.eq_ignore_ascii_case("host") {
                        if host_seen {
                            return Err(invalid());
                        }
                        host_seen = true;
                    } else if h.name.eq_ignore_ascii_case("content-length") {
                        if h.value.is_empty() || !h.value.iter().all(u8::is_ascii_digit) {
                            return Err(invalid());
                        }
                        let value = std::str::from_utf8(h.value)
                            .map_err(|_| invalid())?
                            .parse::<usize>()
                            .map_err(|_| invalid())?;
                        if length.is_some_and(|prior| prior != value) {
                            return Err(invalid());
                        }
                        length = Some(value);
                    } else if h.name.eq_ignore_ascii_case("transfer-encoding") {
                        if chunked || !h.value.eq_ignore_ascii_case(b"chunked") {
                            return Err(invalid());
                        }
                        chunked = true;
                    } else if h.name.eq_ignore_ascii_case("connection") {
                        for token in h.value.split(|b| *b == b',') {
                            let token = std::str::from_utf8(token).map_err(|_| invalid())?.trim();
                            if token.eq_ignore_ascii_case("close") {
                                close = true;
                            } else if token.eq_ignore_ascii_case("keep-alive")
                                && parsed.version == Some(0)
                            {
                                close = false;
                            }
                        }
                    } else if h.name.eq_ignore_ascii_case("expect") {
                        expect_continue = h.value.eq_ignore_ascii_case(b"100-continue");
                    }
                }
                if parsed.version == Some(1) && !host_seen
                    || chunked && length.is_some()
                    || length.is_some_and(|n| n > MAX_REQUEST_BODY)
                {
                    return Err(invalid());
                }
                let body = if chunked {
                    decode_chunks(&pending[header_len..])?
                        .map(|(body, consumed)| (body, header_len + consumed))
                } else {
                    let used = header_len + length.unwrap_or(0);
                    (pending.len() >= used).then(|| (pending[header_len..used].to_vec(), used))
                };
                if let Some((body, used)) = body {
                    let method = match parsed.method.ok_or_else(invalid)? {
                        "GET" => ReqMethod::Get,
                        "POST" => ReqMethod::Post,
                        _ => ReqMethod::Other,
                    };
                    let path = request_path(parsed.path.ok_or_else(invalid)?)?;
                    // Like `HeaderMap::get`, the first header of each name decides.
                    let first = |name: &str| {
                        parsed
                            .headers
                            .iter()
                            .find(|h| h.name.eq_ignore_ascii_case(name))
                            .and_then(|h| header_str(h.value))
                    };
                    let gzip = first("accept-encoding").is_some_and(accepts_gzip);
                    let auth = match method {
                        ReqMethod::Post => first("authorization").map(str::to_owned),
                        _ => None,
                    };
                    if used == pending.len() {
                        *pending = Vec::new();
                    } else {
                        pending.drain(..used);
                    }
                    return Ok((
                        Req {
                            method,
                            path,
                            auth,
                            body,
                        },
                        gzip,
                        close,
                    ));
                }
                if expect_continue && !continued {
                    stream.write_all(b"HTTP/1.1 100 Continue\r\n\r\n").await?;
                    continued = true;
                }
            }
            httparse::Status::Partial => {
                if pending.len() >= MAX_HEADERS {
                    return Err(invalid());
                }
            }
        }
        if pending.len() >= MAX_HEADERS + MAX_REQUEST_BODY + 4096 {
            return Err(invalid());
        }
        if pending.capacity() == pending.len() {
            pending.reserve(1024);
        }
        if stream.read_buf(pending).await? == 0 {
            return Err(io::ErrorKind::UnexpectedEof.into());
        }
    }
}

fn accepts_gzip(value: &str) -> bool {
    value.split(',').any(|part| {
        let mut segments = part.split(';');
        let coding = segments.next().map(str::trim).unwrap_or_default();
        if !coding.eq_ignore_ascii_case("gzip") {
            return false;
        }
        !segments.any(|parameter| {
            let mut kv = parameter.trim().splitn(2, '=');
            kv.next()
                .map(str::trim)
                .unwrap_or_default()
                .eq_ignore_ascii_case("q")
                && kv
                    .next()
                    .map(str::trim)
                    .is_some_and(|v| v == "0" || v == "0.0" || v == "0.00" || v == "0.000")
        })
    })
}

fn decode_chunks(input: &[u8]) -> io::Result<Option<(Vec<u8>, usize)>> {
    let mut offset = 0;
    let mut body = Vec::new();
    loop {
        let Some(end) = input[offset..].windows(2).position(|w| w == b"\r\n") else {
            return Ok(None);
        };
        let line = &input[offset..offset + end];
        let size_text = line.split(|b| *b == b';').next().ok_or_else(invalid)?;
        if size_text.is_empty() || !size_text.iter().all(u8::is_ascii_hexdigit) {
            return Err(invalid());
        }
        let size =
            usize::from_str_radix(std::str::from_utf8(size_text).map_err(|_| invalid())?, 16)
                .map_err(|_| invalid())?;
        offset += end + 2;
        if size == 0 {
            if input.get(offset..offset + 2) == Some(b"\r\n") {
                return Ok(Some((body, offset + 2)));
            }
            return Ok(input[offset..]
                .windows(4)
                .position(|w| w == b"\r\n\r\n")
                .map(|n| (body, offset + n + 4)));
        }
        if size > MAX_REQUEST_BODY - body.len() {
            return Err(invalid());
        }
        if input.len() - offset < size + 2 {
            return Ok(None);
        }
        body.extend_from_slice(&input[offset..offset + size]);
        offset += size;
        if &input[offset..offset + 2] != b"\r\n" {
            return Err(invalid());
        }
        offset += 2;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn paths_match_uri() {
        for target in [
            "/feed",
            "/posts/12",
            "/posts/12/like",
            "/feed?x=1",
            "/a%20b",
            "/posts/1#frag",
            "http://h/feed",
            "*",
            "/~user/a.b-c_d",
        ] {
            let expected = target
                .parse::<http::Uri>()
                .map(|u| u.path().to_owned())
                .ok();
            assert_eq!(request_path(target).ok(), expected, "{target}");
        }
        assert!(request_path("/bad path").is_err());
    }

    #[test]
    fn header_values_follow_to_str() {
        for value in [&b"gzip"[..], b"a\tb", b"\x7f", b"caf\xc3\xa9", b"x\x01"] {
            let expected = http::HeaderValue::from_bytes(value)
                .ok()
                .and_then(|v| v.to_str().ok().map(str::to_owned));
            assert_eq!(header_str(value).map(str::to_owned), expected, "{value:?}");
        }
    }
}
