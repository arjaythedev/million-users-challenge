//! Small HTTP/1 transport for the API. Syntax parsing uses httparse; framing,
//! limits, keep-alive, chunk decoding and partial vectored writes are ours.
use crate::{App, Reply, handle};
use bytes::{Buf, Bytes, BytesMut};
use http_body_util::{BodyExt, Full};
use hyper::{Method, Request, Uri, header::HeaderValue};
use std::{
    io::{self, IoSlice, Write},
    rc::Rc,
    time::{Duration, SystemTime},
};
use tokio::{
    io::{AsyncReadExt, AsyncWriteExt},
    net::TcpStream,
};

const HEAD_LIMIT: usize = 8192;
const BODY_LIMIT: usize = 16384;
const WIRE_LIMIT: usize = 65536;
struct Frame {
    request: Request<Full<Bytes>>,
    used: usize,
    close: bool,
    gzip: bool,
}
// Keep the parsed request inline: boxing would add an allocation per request.
#[allow(clippy::large_enum_variant)]
enum Decode {
    More(bool),
    Complete(Frame),
}
fn invalid() -> io::Error {
    io::Error::new(io::ErrorKind::InvalidData, "invalid HTTP framing")
}
fn tokens(value: &[u8], token: &[u8]) -> bool {
    value
        .split(|b| *b == b',')
        .any(|v| v.trim_ascii().eq_ignore_ascii_case(token))
}
fn chunks(input: &[u8]) -> io::Result<Option<(Bytes, usize)>> {
    let mut at = 0;
    let mut out = Vec::new();
    loop {
        let Some(end) = input[at..].windows(2).position(|s| s == b"\r\n") else {
            return Ok(None);
        };
        if end > 1024 {
            return Err(invalid());
        }
        let line = &input[at..at + end];
        let size_text = line.split(|c| *c == b';').next().unwrap();
        if size_text.is_empty() || !size_text.iter().all(u8::is_ascii_hexdigit) {
            return Err(invalid());
        }
        let n = usize::from_str_radix(std::str::from_utf8(size_text).map_err(|_| invalid())?, 16)
            .map_err(|_| invalid())?;
        at += end + 2;
        if n == 0 {
            let mut headers = [httparse::EMPTY_HEADER; 64];
            match httparse::parse_headers(&input[at..], &mut headers).map_err(|_| invalid())? {
                httparse::Status::Partial => return Ok(None),
                httparse::Status::Complete((used, trailers)) => {
                    if used > HEAD_LIMIT
                        || trailers.iter().any(|h| {
                            [
                                "content-length",
                                "transfer-encoding",
                                "host",
                                "authorization",
                            ]
                            .iter()
                            .any(|n| h.name.eq_ignore_ascii_case(n))
                        })
                    {
                        return Err(invalid());
                    }
                    return Ok(Some((out.into(), at + used)));
                }
            }
        }
        if n > BODY_LIMIT - out.len() {
            return Err(invalid());
        }
        if input.len() - at < n + 2 {
            return Ok(None);
        }
        if &input[at + n..at + n + 2] != b"\r\n" {
            return Err(invalid());
        }
        out.extend_from_slice(&input[at..at + n]);
        at += n + 2;
    }
}
fn decode(input: &[u8]) -> io::Result<Decode> {
    let mut headers = [httparse::EMPTY_HEADER; 64];
    let mut parsed = httparse::Request::new(&mut headers);
    let head = match parsed.parse(input).map_err(|_| invalid())? {
        httparse::Status::Partial => {
            if input.len() >= HEAD_LIMIT {
                return Err(invalid());
            }
            return Ok(Decode::More(false));
        }
        httparse::Status::Complete(n) => n,
    };
    if head > HEAD_LIMIT {
        return Err(invalid());
    }
    #[cfg(feature = "gzip")]
    let gzip = crate::compression::accepted(parsed.headers);
    #[cfg(not(feature = "gzip"))]
    let gzip = false;
    let mut length = None;
    let mut chunked = false;
    let mut close = parsed.version == Some(0);
    let mut connection_close = false;
    let mut expect = false;
    let mut auth = None;
    let mut hosts = 0;
    for h in parsed.headers.iter() {
        if h.name.eq_ignore_ascii_case("content-length") {
            if length.is_some() || h.value.is_empty() || !h.value.iter().all(u8::is_ascii_digit) {
                return Err(invalid());
            }
            let n = std::str::from_utf8(h.value)
                .map_err(|_| invalid())?
                .parse::<usize>()
                .map_err(|_| invalid())?;
            if n > BODY_LIMIT {
                return Err(invalid());
            }
            length = Some(n);
        } else if h.name.eq_ignore_ascii_case("transfer-encoding") {
            if chunked || !h.value.eq_ignore_ascii_case(b"chunked") {
                return Err(invalid());
            }
            chunked = true;
        } else if h.name.eq_ignore_ascii_case("connection") {
            connection_close |= tokens(h.value, b"close");
            if tokens(h.value, b"keep-alive") {
                close = false;
            }
        } else if h.name.eq_ignore_ascii_case("expect") {
            if !h.value.eq_ignore_ascii_case(b"100-continue") {
                return Err(invalid());
            }
            expect = true;
        } else if h.name.eq_ignore_ascii_case("host") {
            hosts += 1;
            if h.value.is_empty() {
                return Err(invalid());
            }
        } else if h.name.eq_ignore_ascii_case("authorization") {
            if auth.is_some() {
                return Err(invalid());
            }
            auth = Some(HeaderValue::from_bytes(h.value).map_err(|_| invalid())?);
        }
    }
    if hosts > 1 || (parsed.version == Some(1) && hosts != 1) || (chunked && length.is_some()) {
        return Err(invalid());
    }
    let (body, used) = if chunked {
        let Some((body, n)) = chunks(&input[head..])? else {
            return Ok(Decode::More(expect));
        };
        (body, head + n)
    } else {
        let n = length.unwrap_or(0);
        if input.len() - head < n {
            return Ok(Decode::More(expect));
        }
        (Bytes::copy_from_slice(&input[head..head + n]), head + n)
    };
    let method =
        Method::from_bytes(parsed.method.ok_or_else(invalid)?.as_bytes()).map_err(|_| invalid())?;
    let uri = parsed
        .path
        .ok_or_else(invalid)?
        .parse::<Uri>()
        .map_err(|_| invalid())?;
    let mut request = Request::builder()
        .method(method)
        .uri(uri)
        .body(Full::new(body))
        .map_err(|_| invalid())?;
    if let Some(auth) = auth {
        request.headers_mut().insert("authorization", auth);
    }
    Ok(Decode::Complete(Frame {
        request,
        used,
        close: close || connection_close,
        gzip,
    }))
}
async fn reply(
    stream: &mut TcpStream,
    response: Reply,
    close: bool,
    head_only: bool,
    gzip: bool,
) -> io::Result<()> {
    let status = response.status();
    let body = response.into_body().collect().await.unwrap().to_bytes();
    #[cfg(feature = "gzip")]
    let (body, encoded) = crate::compression::encode(body, gzip)?;
    #[cfg(not(feature = "gzip"))]
    let encoded = {
        let _ = gzip;
        false
    };
    let mut bytes = [0u8; 320];
    let mut header = io::Cursor::new(&mut bytes[..]);
    write!(
        header,
        "HTTP/1.1 {} {}\r\nContent-Type: application/json\r\nContent-Length: {}\r\nDate: {}\r\n{}{}{}\r\n",
        status.as_u16(),
        status.canonical_reason().unwrap_or(""),
        body.len(),
        httpdate::HttpDate::from(SystemTime::now()),
        if close { "Connection: close\r\n" } else { "" },
        if encoded {
            "Content-Encoding: gzip\r\n"
        } else {
            ""
        },
        if cfg!(feature = "gzip") {
            "Vary: Accept-Encoding\r\n"
        } else {
            ""
        }
    )?;
    let n = header.position() as usize;
    let mut h = &bytes[..n];
    let mut b = if head_only { &[][..] } else { &body[..] };
    while !h.is_empty() || !b.is_empty() {
        let written = stream
            .write_vectored(&[IoSlice::new(h), IoSlice::new(b)])
            .await?;
        if written == 0 {
            return Err(io::ErrorKind::WriteZero.into());
        }
        let used = written.min(h.len());
        h = &h[used..];
        b = &b[written - used..];
    }
    Ok(())
}
#[cfg(not(feature = "lean-http-state"))]
pub async fn serve(mut stream: TcpStream, app: Rc<App>) -> io::Result<()> {
    #[cfg(feature = "lazy-http-buffers")]
    let mut input = BytesMut::new();
    #[cfg(not(feature = "lazy-http-buffers"))]
    let mut input = BytesMut::with_capacity(1024);
    let mut deadline = None;
    let mut continued = false;
    loop {
        match decode(&input) {
            Ok(Decode::Complete(frame)) => {
                input.advance(frame.used);
                let head_only = frame.request.method() == Method::HEAD;
                // Keep the large route future out of every idle connection's state.
                // Opt-in until memory savings and per-request allocation cost are measured.
                #[cfg(feature = "compact-http")]
                let response = Box::pin(handle(frame.request, app.clone())).await.unwrap();
                #[cfg(not(feature = "compact-http"))]
                let response = handle(frame.request, app.clone()).await.unwrap();
                // A peer that stops reading must not keep an active response indefinitely.
                tokio::time::timeout(
                    Duration::from_secs(5),
                    reply(&mut stream, response, frame.close, head_only, frame.gzip),
                )
                .await??;
                if frame.close {
                    return Ok(());
                }
                continued = false;
                deadline = if input.is_empty() {
                    None
                } else {
                    Some(tokio::time::Instant::now() + Duration::from_secs(5))
                };
                if input.is_empty() {
                    #[cfg(feature = "lazy-http-buffers")]
                    {
                        input = BytesMut::new();
                    }
                    #[cfg(not(feature = "lazy-http-buffers"))]
                    if input.capacity() > 2048 {
                        input = BytesMut::with_capacity(1024);
                    }
                }
                continue;
            }
            Ok(Decode::More(expect)) => {
                if expect && !continued {
                    stream.write_all(b"HTTP/1.1 100 Continue\r\n\r\n").await?;
                    continued = true;
                }
            }
            Err(_) => {
                reply(
                    &mut stream,
                    crate::error(400, "malformed HTTP request"),
                    true,
                    false,
                    false,
                )
                .await?;
                return Ok(());
            }
        }
        if input.len() >= WIRE_LIMIT {
            return Err(invalid());
        }
        let n = read_input(&mut stream, &mut input, deadline).await?;
        if n == 0 {
            return Ok(());
        }
        deadline.get_or_insert_with(|| tokio::time::Instant::now() + Duration::from_secs(5));
    }
}
// The idle task owns only the socket, input remainder and readiness future.
// Parsing, routing and response-writing state lives in a box only while active.
#[cfg(all(feature = "lean-http-state", not(feature = "compact-idle-owner")))]
pub async fn serve(mut stream: TcpStream, app: Rc<App>) -> io::Result<()> {
    let mut input = BytesMut::new();
    loop {
        if input.is_empty() && read_idle(&mut stream, &mut input).await? == 0 {
            return Ok(());
        }
        if !Box::pin(exchange(&mut stream, &mut input, &app)).await? {
            return Ok(());
        }
        if input.is_empty() {
            input = BytesMut::new();
        }
    }
}
// Move socket ownership into the boxed active exchange only after bytes arrive.
// The idle task holds no input buffer or nested async-read state.
#[cfg(all(
    feature = "compact-idle-owner",
    not(all(feature = "shared-idle", target_os = "linux"))
))]
pub fn serve(stream: TcpStream, app: Rc<App>) -> impl std::future::Future<Output = io::Result<()>> {
    use std::{future::Future, pin::Pin, task::Poll};
    type Returned = io::Result<Option<(TcpStream, Rc<App>)>>;
    enum State {
        Idle(TcpStream, Rc<App>),
        Active(Pin<Box<dyn Future<Output = Returned>>>),
        Done,
    }
    let mut state = State::Idle(stream, app);
    std::future::poll_fn(move |cx| {
        loop {
            match &mut state {
                State::Idle(stream, _) => {
                    if let Err(e) = std::task::ready!(stream.poll_read_ready(cx)) {
                        return Poll::Ready(Err(e));
                    }
                    let mut input = BytesMut::with_capacity(1024);
                    match stream.try_read_buf(&mut input) {
                        Ok(0) => return Poll::Ready(Ok(())),
                        Ok(_) => {}
                        // try_read_buf clears spurious readiness. Repoll to register
                        // the same task's waker; the temporary allocation is dropped.
                        Err(e) if e.kind() == io::ErrorKind::WouldBlock => continue,
                        Err(e) => return Poll::Ready(Err(e)),
                    }
                    let State::Idle(stream, app) = std::mem::replace(&mut state, State::Done)
                    else {
                        unreachable!()
                    };
                    state = State::Active(Box::pin(async move {
                        let mut stream = stream;
                        loop {
                            if !exchange(&mut stream, &mut input, &app).await? {
                                return Ok(None);
                            }
                            if input.is_empty() {
                                return Ok(Some((stream, app)));
                            }
                            // Preserve and process every pipelined remainder before
                            // returning to idle ownership. Partial requests keep deadlines.
                        }
                    }));
                }
                State::Active(active) => match std::task::ready!(active.as_mut().poll(cx)) {
                    Ok(Some((stream, app))) => state = State::Idle(stream, app),
                    Ok(None) => return Poll::Ready(Ok(())),
                    Err(e) => return Poll::Ready(Err(e)),
                },
                State::Done => unreachable!(),
            }
        }
    })
}
#[cfg(all(feature = "shared-idle", target_os = "linux"))]
pub async fn serve_active(
    mut stream: TcpStream,
    mut input: BytesMut,
    app: Rc<App>,
) -> io::Result<Option<std::net::TcpStream>> {
    loop {
        if !exchange(&mut stream, &mut input, &app).await? {
            return Ok(None);
        }
        if input.is_empty() {
            return stream.into_std().map(Some);
        }
    }
}
#[cfg(feature = "lean-http-state")]
async fn exchange(stream: &mut TcpStream, input: &mut BytesMut, app: &Rc<App>) -> io::Result<bool> {
    let deadline = tokio::time::Instant::now() + Duration::from_secs(5);
    let mut continued = false;
    loop {
        match decode(input) {
            Ok(Decode::Complete(frame)) => {
                input.advance(frame.used);
                let head_only = frame.request.method() == Method::HEAD;
                let response = handle(frame.request, app.clone()).await.unwrap();
                tokio::time::timeout(
                    Duration::from_secs(5),
                    reply(stream, response, frame.close, head_only, frame.gzip),
                )
                .await??;
                return Ok(!frame.close);
            }
            Ok(Decode::More(expect)) => {
                if expect && !continued {
                    stream.write_all(b"HTTP/1.1 100 Continue\r\n\r\n").await?;
                    continued = true;
                }
            }
            Err(_) => {
                tokio::time::timeout(
                    Duration::from_secs(5),
                    reply(
                        stream,
                        crate::error(400, "malformed HTTP request"),
                        true,
                        false,
                        false,
                    ),
                )
                .await??;
                return Ok(false);
            }
        }
        if input.len() >= WIRE_LIMIT {
            return Err(invalid());
        }
        if read_input(stream, input, Some(deadline)).await? == 0 {
            return Ok(false);
        }
    }
}
#[cfg(feature = "lazy-http-buffers")]
async fn read_idle(stream: &mut TcpStream, input: &mut BytesMut) -> io::Result<usize> {
    loop {
        // Idle sockets retain no input allocation. Readiness can be spurious;
        // try_read_buf clears that readiness on WouldBlock before retrying.
        // Each HTTP socket has exactly one reader. The single-reader poll API
        // avoids retaining readable()'s multi-waiter future on every idle task.
        #[cfg(feature = "poll-http-idle")]
        std::future::poll_fn(|cx| stream.poll_read_ready(cx)).await?;
        #[cfg(not(feature = "poll-http-idle"))]
        stream.readable().await?;
        input.reserve(1024);
        match stream.try_read_buf(input) {
            Ok(n) => return Ok(n),
            Err(e) if e.kind() == io::ErrorKind::WouldBlock => *input = BytesMut::new(),
            Err(e) => return Err(e),
        }
    }
}
async fn read_input(
    stream: &mut TcpStream,
    input: &mut BytesMut,
    deadline: Option<tokio::time::Instant>,
) -> io::Result<usize> {
    #[cfg(feature = "lazy-http-buffers")]
    if input.is_empty() && deadline.is_none() {
        return read_idle(stream, input).await;
    }
    let read = stream.read_buf(input);
    if let Some(end) = deadline {
        tokio::time::timeout_at(end, read).await?
    } else {
        read.await
    }
}
#[cfg(test)]
mod tests {
    use super::*;
    #[cfg(feature = "lazy-http-buffers")]
    #[tokio::test(flavor = "current_thread")]
    async fn idle_read_allocates_only_after_data_and_keeps_partial_request() {
        let listener = tokio::net::TcpListener::bind("127.0.0.1:0").await.unwrap();
        let mut client = TcpStream::connect(listener.local_addr().unwrap())
            .await
            .unwrap();
        let (mut server, _) = listener.accept().await.unwrap();
        let mut input = BytesMut::new();
        assert!(
            tokio::time::timeout(
                Duration::from_millis(10),
                read_input(&mut server, &mut input, None)
            )
            .await
            .is_err()
        );
        assert_eq!(input.capacity(), 0);
        client.write_all(b"GET /hea").await.unwrap();
        while input.len() < 8 {
            read_input(&mut server, &mut input, None).await.unwrap();
        }
        let deadline = Some(tokio::time::Instant::now() + Duration::from_millis(10));
        assert!(read_input(&mut server, &mut input, deadline).await.is_err());
        assert_eq!(&input[..], b"GET /hea");
        client
            .write_all(b"lth HTTP/1.1\r\nHost: x\r\n\r\n")
            .await
            .unwrap();
        let deadline = Some(tokio::time::Instant::now() + Duration::from_secs(1));
        while matches!(decode(&input).unwrap(), Decode::More(_)) {
            read_input(&mut server, &mut input, deadline).await.unwrap();
        }
        assert!(matches!(decode(&input).unwrap(), Decode::Complete(_)));
    }
    #[test]
    fn rejects_ambiguous_framing() {
        for headers in [
            "Content-Length: 0\r\nContent-Length: 0\r\n",
            "Content-Length: 0\r\nTransfer-Encoding: chunked\r\n",
            "Transfer-Encoding: gzip, chunked\r\n",
            "Content-Length: -1\r\n",
        ] {
            assert!(
                decode(format!("POST /posts HTTP/1.1\r\nHost: x\r\n{headers}\r\n").as_bytes())
                    .is_err()
            );
        }
    }
    #[test]
    fn chunks_and_pipeline_boundaries() {
        let data = b"POST /posts HTTP/1.1\r\nHost: x\r\nTransfer-Encoding: chunked\r\n\r\n3;x=y\r\nabc\r\n0\r\nX-Foo: yes\r\n\r\nGET /feed HTTP/1.1\r\nHost: x\r\n\r\n";
        let Decode::Complete(frame) = decode(data).unwrap() else {
            panic!()
        };
        assert_eq!(&data[frame.used..frame.used + 3], b"GET");
        assert!(matches!(
            decode(&data[..frame.used - 1]).unwrap(),
            Decode::More(_)
        ));
    }
}
