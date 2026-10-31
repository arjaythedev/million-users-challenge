//! HTTP/1.1 connections retain no request/response buffer while idle.
use super::{AppState, HttpBody, MAX_REQUEST_BODY};
use bytes::Bytes;
use http_body_util::{BodyExt, Full};
use hyper::{header, Request, Response, StatusCode, Version};
use std::{io, sync::Arc};
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt},
    net::TcpStream,
    time::{timeout, Duration},
};

const MAX_HEADERS: usize = 16 * 1024;

pub(super) async fn serve(mut stream: TcpStream, state: Arc<AppState>) -> io::Result<()> {
    let mut pending = Vec::new();
    loop {
        if pending.is_empty() {
            // Readiness may remain cached after the previous request. Peek
            // waits for an actual byte before the request timeout starts.
            let mut first = [0u8; 1];
            let count = timeout(Duration::from_secs(180), stream.peek(&mut first))
                .await
                .map_err(io::Error::other)??;
            if count == 0 {
                return Ok(());
            }
        }
        // These futures include parsing/handling scratch state. Box them only
        // while active rather than embedding that state in every idle task.
        let parsed = timeout(
            Duration::from_secs(15),
            Box::pin(read_request(&mut stream, &mut pending)),
        )
        .await;
        let (request, close) = match parsed {
            Ok(Ok(request)) => request,
            Ok(Err(error)) if error.kind() == io::ErrorKind::UnexpectedEof => return Ok(()),
            _ => {
                let mut response = Response::new(Full::new(Bytes::from_static(
                    br#"{"error":"malformed JSON body"}"#,
                )));
                *response.status_mut() = StatusCode::BAD_REQUEST;
                write_response(&mut stream, response, true).await?;
                return Ok(());
            }
        };
        let response = Box::pin(state.handle(request)).await;
        Box::pin(write_response(&mut stream, response, close)).await?;
        if close {
            return Ok(());
        }
    }
}

fn invalid() -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, "invalid HTTP request")
}

async fn read_request(
    stream: &mut TcpStream,
    pending: &mut Vec<u8>,
) -> io::Result<(Request<Full<Bytes>>, bool)> {
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
                for h in parsed.headers.iter() {
                    if h.name.eq_ignore_ascii_case("content-length") {
                        let value = std::str::from_utf8(h.value)
                            .map_err(|_| invalid())?
                            .trim()
                            .parse::<usize>()
                            .map_err(|_| invalid())?;
                        if length.is_some_and(|prior| prior != value) {
                            return Err(invalid());
                        }
                        length = Some(value);
                    } else if h.name.eq_ignore_ascii_case("transfer-encoding") {
                        if !h.value.eq_ignore_ascii_case(b"chunked") {
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
                if chunked && length.is_some() || length.is_some_and(|n| n > MAX_REQUEST_BODY) {
                    return Err(invalid());
                }
                let body = if chunked {
                    decode_chunks(&pending[header_len..])?
                        .map(|(body, consumed)| (body, header_len + consumed))
                } else {
                    let used = header_len + length.unwrap_or(0);
                    (pending.len() >= used)
                        .then(|| (Bytes::copy_from_slice(&pending[header_len..used]), used))
                };
                if let Some((body, used)) = body {
                    let mut request = Request::builder()
                        .method(parsed.method.ok_or_else(invalid)?)
                        .uri(parsed.path.ok_or_else(invalid)?)
                        .version(if parsed.version == Some(0) {
                            Version::HTTP_10
                        } else {
                            Version::HTTP_11
                        });
                    for h in parsed.headers.iter() {
                        if !h.name.eq_ignore_ascii_case("transfer-encoding")
                            && !h.name.eq_ignore_ascii_case("content-length")
                        {
                            request = request.header(h.name, h.value);
                        }
                    }
                    let request = request
                        .header(header::CONTENT_LENGTH, body.len())
                        .body(Full::new(body))
                        .map_err(|_| invalid())?;
                    if used == pending.len() {
                        *pending = Vec::new();
                    } else {
                        pending.drain(..used);
                    }
                    return Ok((request, close));
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

fn decode_chunks(input: &[u8]) -> io::Result<Option<(Bytes, usize)>> {
    let mut offset = 0;
    let mut body = Vec::new();
    loop {
        let Some(end) = input[offset..].windows(2).position(|w| w == b"\r\n") else {
            return Ok(None);
        };
        let line = &input[offset..offset + end];
        let size_text = line.split(|b| *b == b';').next().ok_or_else(invalid)?;
        let size = usize::from_str_radix(
            std::str::from_utf8(size_text)
                .map_err(|_| invalid())?
                .trim(),
            16,
        )
        .map_err(|_| invalid())?;
        offset += end + 2;
        if size == 0 {
            if input.get(offset..offset + 2) == Some(b"\r\n") {
                return Ok(Some((Bytes::from(body), offset + 2)));
            }
            return Ok(input[offset..]
                .windows(4)
                .position(|w| w == b"\r\n\r\n")
                .map(|n| (Bytes::from(body), offset + n + 4)));
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

async fn write_response(
    stream: &mut TcpStream,
    response: Response<HttpBody>,
    close: bool,
) -> io::Result<()> {
    let (parts, body) = response.into_parts();
    let body = body.collect().await.map_err(io::Error::other)?.to_bytes();
    let mut output = Vec::with_capacity(256 + body.len());
    output.extend_from_slice(
        format!(
            "HTTP/1.1 {} {}\r\n",
            parts.status.as_u16(),
            parts.status.canonical_reason().unwrap_or("")
        )
        .as_bytes(),
    );
    for (name, value) in &parts.headers {
        if name == header::CONNECTION || name == header::CONTENT_LENGTH {
            continue;
        }
        output.extend_from_slice(name.as_str().as_bytes());
        output.extend_from_slice(b": ");
        output.extend_from_slice(value.as_bytes());
        output.extend_from_slice(b"\r\n");
    }
    output.extend_from_slice(
        format!(
            "Content-Length: {}\r\nConnection: {}\r\n\r\n",
            body.len(),
            if close { "close" } else { "keep-alive" }
        )
        .as_bytes(),
    );
    output.extend_from_slice(&body);
    stream.write_all(&output).await
}
