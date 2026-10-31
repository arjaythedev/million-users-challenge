//! Experimental PostgreSQL v3 client. Own bounded transport/pipeline and result
//! ownership; uses standard protocol codecs, SQL types, config parsing and SCRAM.
use bytes::{Bytes, BytesMut};
use fallible_iterator::FallibleIterator;
use postgres_protocol::{
    authentication::{
        md5_hash,
        sasl::{ChannelBinding, ScramSha256},
    },
    message::{
        backend::{self, Message},
        frontend,
    },
};
use postgres_types::{FromSql, IsNull, ToSql, Type};
use smallvec::SmallVec;
use std::{cell::Cell, fmt, future::Future, io, rc::Rc, time::Duration};
use tokio::{
    io::{AsyncRead, AsyncReadExt, AsyncWrite, AsyncWriteExt},
    net::{TcpStream, UnixStream},
    sync::{mpsc, oneshot},
};
pub use tokio_postgres::NoTls;
pub mod error {
    pub use tokio_postgres::error::SqlState;
}

#[derive(Debug, Clone)]
pub struct Error {
    message: String,
    code: Option<error::SqlState>,
    skipped_read: bool,
}
impl Error {
    fn new(message: impl ToString) -> Self {
        Self {
            message: message.to_string(),
            code: None,
            skipped_read: false,
        }
    }
    pub fn code(&self) -> Option<&error::SqlState> {
        self.code.as_ref()
    }
}
impl fmt::Display for Error {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        f.write_str(&self.message)
    }
}
impl std::error::Error for Error {}
impl From<io::Error> for Error {
    fn from(e: io::Error) -> Self {
        Self::new(e)
    }
}
fn db_error(body: backend::ErrorResponseBody) -> Result<Error, Error> {
    let mut e = Error::new("PostgreSQL error");
    let mut fields = body.fields();
    while let Some(field) = fields.next()? {
        match field.type_() {
            b'C' => {
                e.code = Some(error::SqlState::from_code(&String::from_utf8_lossy(
                    field.value_bytes(),
                )))
            }
            b'M' => e.message = String::from_utf8_lossy(field.value_bytes()).into_owned(),
            _ => {}
        }
    }
    Ok(e)
}
#[derive(Clone)]
pub struct Statement {
    read_only: bool,
    name: Rc<str>,
    params: Rc<Vec<Type>>,
    columns: Rc<Vec<Type>>,
}
impl Statement {
    /// Only mark side-effect-free SELECTs on a connection outside an explicit
    /// transaction. A skipped execution may be retried after group recovery.
    #[cfg(feature = "grouped-read-sync")]
    pub fn read_only(mut self) -> Self {
        self.read_only = true;
        self
    }
}
pub struct Row {
    data: Bytes,
    ranges: SmallVec<[(u32, u32); 5]>,
    types: Rc<Vec<Type>>,
}
impl Row {
    pub fn get<'a, T: FromSql<'a>>(&'a self, index: usize) -> T {
        let ty = &self.types[index];
        assert!(T::accepts(ty), "incorrect SQL type");
        let (start, end) = self.ranges[index];
        let data = (start != u32::MAX).then(|| &self.data[start as usize..end as usize]);
        T::from_sql_nullable(ty, data).expect("valid SQL value")
    }
}
#[derive(Default)]
pub struct Output {
    pub rows: Vec<Row>,
    pub count: u64,
    #[cfg(feature = "grouped-write-sync")]
    pub grouped: Vec<Output>,
    #[cfg(feature = "grouped-write-sync")]
    pub group_failure: Option<(usize, Error)>,
    params: Vec<Type>,
    columns: Vec<Type>,
}
struct Job {
    #[cfg_attr(not(feature = "grouped-read-sync"), allow(dead_code))]
    read_only: bool,
    #[cfg(feature = "grouped-write-sync")]
    group: Option<Vec<Rc<Vec<Type>>>>,
    bytes: Bytes,
    columns: Rc<Vec<Type>>,
    reply: oneshot::Sender<Result<Output, Error>>,
    prepare: bool,
}
pub struct Client {
    tx: mpsc::Sender<Job>,
    next: Cell<u64>,
}
impl Client {
    async fn send(
        &self,
        bytes: BytesMut,
        columns: Rc<Vec<Type>>,
        prepare: bool,
        read_only: bool,
    ) -> Result<Output, Error> {
        let (tx, rx) = oneshot::channel();
        self.tx
            .send(Job {
                read_only,
                #[cfg(feature = "grouped-write-sync")]
                group: None,
                bytes: bytes.freeze(),
                columns,
                reply: tx,
                prepare,
            })
            .await
            .map_err(|_| Error::new("database transport closed"))?;
        rx.await
            .map_err(|_| Error::new("database transport closed"))?
    }
    pub async fn prepare(&self, query: &str) -> Result<Statement, Error> {
        let n = self.next.get();
        self.next.set(n + 1);
        let name = format!("q{n}");
        let mut bytes = BytesMut::new();
        frontend::parse(&name, query, std::iter::empty(), &mut bytes)?;
        frontend::describe(b'S', &name, &mut bytes)?;
        frontend::sync(&mut bytes);
        let output = self.send(bytes, Rc::new(Vec::new()), true, false).await?;
        Ok(Statement {
            read_only: false,
            name: name.into(),
            params: Rc::new(output.params),
            columns: Rc::new(output.columns),
        })
    }
    async fn run(
        &self,
        statement: &Statement,
        params: &[&(dyn ToSql + Sync)],
    ) -> Result<Output, Error> {
        loop {
            let mut bytes = BytesMut::with_capacity(128);
            Self::encode(statement, params, &mut bytes)?;
            frontend::sync(&mut bytes);
            let result = self
                .send(bytes, statement.columns.clone(), false, statement.read_only)
                .await;
            // Only SELECTs explicitly marked by their caller may be retried after
            // PostgreSQL skipped them following an earlier group member's error.
            if statement.read_only && result.as_ref().is_err_and(|e| e.skipped_read) {
                continue;
            }
            return result;
        }
    }
    fn encode(
        statement: &Statement,
        params: &[&(dyn ToSql + Sync)],
        bytes: &mut BytesMut,
    ) -> Result<(), Error> {
        if params.len() != statement.params.len() {
            return Err(Error::new("parameter count mismatch"));
        }
        frontend::bind(
            "",
            &statement.name,
            [1],
            params.iter().zip(statement.params.iter()),
            |(value, ty), bytes| {
                value.to_sql_checked(ty, bytes).map(|null| match null {
                    IsNull::Yes => postgres_protocol::IsNull::Yes,
                    IsNull::No => postgres_protocol::IsNull::No,
                })
            },
            [1],
            bytes,
        )
        .map_err(|e| match e {
            frontend::BindError::Conversion(e) => Error::new(e),
            frontend::BindError::Serialization(e) => Error::new(e),
        })?;
        frontend::execute("", 0, bytes)?;
        Ok(())
    }
    /// One extended-protocol error boundary around an explicit transaction.
    /// Callers include prepared BEGIN and COMMIT. No result is exposed early.
    #[cfg(feature = "grouped-write-sync")]
    pub async fn transaction_group(
        &self,
        commands: &[(&Statement, &[&(dyn ToSql + Sync)])],
    ) -> Result<Output, Error> {
        if commands.len() < 3 || commands.len() > 66 {
            return Err(Error::new("invalid transaction group size"));
        }
        let mut bytes = BytesMut::new();
        let mut columns = Vec::with_capacity(commands.len());
        for (statement, params) in commands {
            Self::encode(statement, params, &mut bytes)?;
            columns.push(statement.columns.clone());
        }
        frontend::sync(&mut bytes);
        let (reply, result) = oneshot::channel();
        self.tx
            .send(Job {
                read_only: false,
                bytes: bytes.freeze(),
                columns: Rc::new(Vec::new()),
                prepare: false,
                reply,
                group: Some(columns),
            })
            .await
            .map_err(|_| Error::new("database transport closed"))?;
        result
            .await
            .map_err(|_| Error::new("database transport closed"))?
    }
    pub async fn query(&self, s: &Statement, p: &[&(dyn ToSql + Sync)]) -> Result<Vec<Row>, Error> {
        Ok(self.run(s, p).await?.rows)
    }
    pub async fn query_opt(
        &self,
        s: &Statement,
        p: &[&(dyn ToSql + Sync)],
    ) -> Result<Option<Row>, Error> {
        let mut rows = self.query(s, p).await?;
        if rows.len() > 1 {
            return Err(Error::new("expected at most one row"));
        }
        Ok(rows.pop())
    }
    pub async fn query_one(&self, s: &Statement, p: &[&(dyn ToSql + Sync)]) -> Result<Row, Error> {
        self.query_opt(s, p)
            .await?
            .ok_or_else(|| Error::new("expected one row"))
    }
    pub async fn execute(&self, s: &Statement, p: &[&(dyn ToSql + Sync)]) -> Result<u64, Error> {
        Ok(self.run(s, p).await?.count)
    }
    pub async fn batch_execute(&self, query: &str) -> Result<(), Error> {
        let mut bytes = BytesMut::new();
        frontend::query(query, &mut bytes)?;
        self.send(bytes, Rc::new(Vec::new()), false, false).await?;
        Ok(())
    }
    pub async fn simple_query(&self, query: &str) -> Result<(), Error> {
        self.batch_execute(query).await
    }
}
trait Socket: AsyncRead + AsyncWrite + Unpin {}
impl<T: AsyncRead + AsyncWrite + Unpin> Socket for T {}
async fn message<R: AsyncRead + Unpin>(
    read: &mut R,
    buffer: &mut BytesMut,
) -> Result<Message, Error> {
    loop {
        // Bound a corrupt/unexpected peer's advertised frame size before allocating.
        if buffer.len() >= 5 {
            let n = i32::from_be_bytes(buffer[1..5].try_into().unwrap());
            if !(4..=16 * 1024 * 1024).contains(&n) {
                return Err(Error::new("invalid PostgreSQL frame length"));
            }
        }
        if let Some(message) = Message::parse(buffer)? {
            return Ok(message);
        }
        if read.read_buf(buffer).await? == 0 {
            return Err(Error::new("database EOF"));
        }
    }
}
fn sql_type(oid: u32) -> Result<Type, Error> {
    Type::from_oid(oid).ok_or_else(|| Error::new(format!("unsupported SQL type OID {oid}")))
}
async fn response<R: AsyncRead + Unpin>(
    read: &mut R,
    buffer: &mut BytesMut,
    job: &Job,
) -> Result<Result<Output, Error>, Error> {
    #[cfg(feature = "grouped-write-sync")]
    if let Some(columns) = &job.group {
        let mut out = Output::default();
        for (index, types) in columns.iter().enumerate() {
            match response_piece(read, buffer, types, false, true).await? {
                Ok(result) => out.grouped.push(result),
                Err(error) => {
                    out.group_failure = Some((index, error));
                    break;
                }
            }
        }
        // After an extended-protocol error PostgreSQL skips the remaining
        // commands (including COMMIT) until Sync. Drain it before rollback.
        loop {
            match message(read, buffer).await? {
                Message::ReadyForQuery(_) => return Ok(Ok(out)),
                Message::NoticeResponse(_) | Message::ParameterStatus(_) => {}
                _ => return Err(Error::new("unexpected transaction boundary")),
            }
        }
    }
    response_piece(read, buffer, &job.columns, job.prepare, false).await
}
async fn response_piece<R: AsyncRead + Unpin>(
    read: &mut R,
    buffer: &mut BytesMut,
    columns: &Rc<Vec<Type>>,
    prepare: bool,
    stop_on_command: bool,
) -> Result<Result<Output, Error>, Error> {
    let mut out = Output::default();
    let mut failure = None;
    loop {
        match message(read, buffer).await? {
            Message::DataRow(body) => {
                // Simple-query results (SELECT 1 / transaction control) need no rows.
                if !columns.is_empty() && failure.is_none() {
                    let mut ranges = SmallVec::new();
                    let mut fields = body.ranges();
                    while let Some(range) = fields.next()? {
                        // Frames are capped at 16 MiB, so 32-bit offsets are sufficient.
                        ranges.push(
                            range.map_or((u32::MAX, u32::MAX), |r| (r.start as u32, r.end as u32)),
                        );
                    }
                    if ranges.len() != columns.len() {
                        return Err(Error::new("row column count mismatch"));
                    }
                    out.rows.push(Row {
                        data: body.buffer_bytes().clone(),
                        ranges,
                        types: columns.clone(),
                    });
                }
            }
            Message::CommandComplete(body) => {
                out.count = body
                    .tag()?
                    .rsplit(' ')
                    .next()
                    .and_then(|n| n.parse().ok())
                    .unwrap_or(0);
                if stop_on_command {
                    return Ok(Ok(out));
                }
            }
            Message::ParameterDescription(body) if prepare => {
                let mut params = body.parameters();
                while let Some(oid) = params.next()? {
                    out.params.push(sql_type(oid)?);
                }
            }
            Message::RowDescription(body) if prepare => {
                let mut fields = body.fields();
                while let Some(field) = fields.next()? {
                    out.columns.push(sql_type(field.type_oid())?);
                }
            }
            Message::ErrorResponse(body) => {
                let error = db_error(body)?;
                if stop_on_command {
                    return Ok(Err(error));
                }
                failure = Some(error);
            }
            Message::ReadyForQuery(_) => {
                return Ok(match failure {
                    Some(e) => Err(e),
                    None => Ok(out),
                });
            }
            Message::ParseComplete
            | Message::BindComplete
            | Message::NoData
            | Message::RowDescription(_)
            | Message::NoticeResponse(_)
            | Message::ParameterStatus(_)
            | Message::EmptyQueryResponse => {}
            _ => return Err(Error::new("unexpected PostgreSQL response")),
        }
    }
}
pub async fn connect(
    url: &str,
    _: NoTls,
) -> Result<(Client, impl Future<Output = Result<(), Error>> + use<>), Error> {
    // Reuse configuration syntax only, not tokio-postgres connection/driver code.
    let cfg: tokio_postgres::Config = url.parse().map_err(Error::new)?;
    if cfg.get_hosts().len() != 1 || cfg.get_ports().len() > 1 {
        return Err(Error::new("wire client requires one database host"));
    }
    if cfg.get_ssl_mode() == tokio_postgres::config::SslMode::Require {
        return Err(Error::new(
            "TLS is unsupported; challenge PostgreSQL uses ssl=off",
        ));
    }
    let user = cfg
        .get_user()
        .ok_or_else(|| Error::new("database user required"))?;
    let database = cfg.get_dbname().unwrap_or(user);
    let port = cfg.get_ports().first().copied().unwrap_or(5432);
    let mut stream: Box<dyn Socket> = match &cfg.get_hosts()[0] {
        tokio_postgres::config::Host::Tcp(host) => {
            let s = TcpStream::connect((host.as_str(), port)).await?;
            s.set_nodelay(true)?;
            Box::new(s)
        }
        tokio_postgres::config::Host::Unix(path) => {
            Box::new(UnixStream::connect(path.join(format!(".s.PGSQL.{port}"))).await?)
        }
    };
    let mut parameters = vec![
        ("user", user),
        ("database", database),
        ("client_encoding", "UTF8"),
    ];
    if let Some(options) = cfg.get_options() {
        parameters.push(("options", options));
    }
    let mut bytes = BytesMut::new();
    frontend::startup_message(parameters, &mut bytes)?;
    stream.write_all(&bytes).await?;
    let mut buffer = BytesMut::with_capacity(8192);
    let mut scram = None;
    loop {
        bytes.clear();
        match message(&mut stream, &mut buffer).await? {
            Message::AuthenticationOk
            | Message::BackendKeyData(_)
            | Message::ParameterStatus(_)
            | Message::NoticeResponse(_) => {}
            Message::AuthenticationCleartextPassword => frontend::password_message(
                cfg.get_password()
                    .ok_or_else(|| Error::new("password required"))?,
                &mut bytes,
            )?,
            Message::AuthenticationMd5Password(body) => {
                let hash = md5_hash(
                    user.as_bytes(),
                    cfg.get_password()
                        .ok_or_else(|| Error::new("password required"))?,
                    body.salt(),
                );
                frontend::password_message(hash.as_bytes(), &mut bytes)?;
            }
            Message::AuthenticationSasl(body) => {
                let mut methods = body.mechanisms();
                let mut supported = false;
                while let Some(m) = methods.next()? {
                    supported |= m == "SCRAM-SHA-256";
                }
                if !supported {
                    return Err(Error::new("SCRAM-SHA-256 required"));
                }
                let s = ScramSha256::new(
                    cfg.get_password()
                        .ok_or_else(|| Error::new("password required"))?,
                    ChannelBinding::unsupported(),
                );
                frontend::sasl_initial_response("SCRAM-SHA-256", s.message(), &mut bytes)?;
                scram = Some(s);
            }
            Message::AuthenticationSaslContinue(body) => {
                let s = scram
                    .as_mut()
                    .ok_or_else(|| Error::new("unexpected SASL continuation"))?;
                s.update(body.data())?;
                frontend::sasl_response(s.message(), &mut bytes)?;
            }
            Message::AuthenticationSaslFinal(body) => scram
                .as_mut()
                .ok_or_else(|| Error::new("unexpected SASL final"))?
                .finish(body.data())?,
            Message::ReadyForQuery(_) => break,
            Message::ErrorResponse(body) => return Err(db_error(body)?),
            _ => {
                return Err(Error::new(
                    "unsupported PostgreSQL authentication/startup message",
                ));
            }
        }
        if !bytes.is_empty() {
            stream.write_all(&bytes).await?;
        }
    }
    let (tx, rx) = mpsc::channel::<Job>(256);
    let delay = Duration::from_micros(crate::number("PG_BATCH_US", 0).min(5000) as u64);
    let driver = async move {
        #[cfg(feature = "continuous-pg")]
        {
            return pipeline(stream, buffer, rx, delay).await;
        }
        #[cfg(not(feature = "continuous-pg"))]
        {
            let mut rx = rx;
            let (mut read, mut write) = tokio::io::split(stream);
            let mut output = BytesMut::with_capacity(8192);
            while let Some(first) = rx.recv().await {
                let mut jobs = Vec::with_capacity(64);
                jobs.push(first);
                if !delay.is_zero() {
                    tokio::time::sleep(delay).await;
                }
                while jobs.len() < 64 {
                    match rx.try_recv() {
                        Ok(job) => jobs.push(job),
                        Err(_) => break,
                    }
                }
                output.clear();
                for job in &jobs {
                    output.extend_from_slice(&job.bytes);
                }
                // Read concurrently with writing: neither side can deadlock on socket
                // buffer capacity when pipelined requests and results are both large.
                let send = async { write.write_all(&output).await.map_err(Error::from) };
                let receive = async {
                    for job in jobs {
                        let result = response(&mut read, &mut buffer, &job).await?;
                        // Even canceled HTTP requests must consume their full DB reply.
                        let _ = job.reply.send(result);
                    }
                    Ok::<_, Error>(())
                };
                tokio::try_join!(send, receive)?;
            }
            Ok(())
        }
    };
    Ok((
        Client {
            tx,
            next: Cell::new(0),
        },
        driver,
    ))
}

#[cfg(feature = "grouped-read-sync")]
async fn receive_read_group<R: AsyncRead + Unpin>(
    read: &mut R,
    buffer: &mut BytesMut,
    jobs: Vec<Job>,
) -> Result<(), Error> {
    let mut results = Vec::with_capacity(jobs.len());
    for job in &jobs {
        let result = response_piece(read, buffer, &job.columns, false, true).await?;
        let failed = result.is_err();
        results.push(result);
        if failed {
            break;
        }
    }
    loop {
        match message(read, buffer).await? {
            Message::ReadyForQuery(body) if body.status() == b'I' => break,
            Message::NoticeResponse(_) | Message::ParameterStatus(_) => {}
            _ => return Err(Error::new("unexpected read group boundary")),
        }
    }
    let mut results = results.into_iter();
    for job in jobs {
        let result = results.next().unwrap_or_else(|| {
            let mut e = Error::new("read skipped by previous query error");
            e.skipped_read = true;
            Err(e)
        });
        let _ = job.reply.send(result);
    }
    Ok(())
}

// Keep reading ordered results while the sender supplies new bounded batches.
// A batch's metadata is queued before its bytes, so every response has an owner.
// The metadata channel is in batches, not individual jobs: otherwise filling it
// halfway through a batch could make the reader wait for bytes not yet written.
#[cfg(feature = "continuous-pg")]
async fn pipeline<S: AsyncRead + AsyncWrite + Unpin>(
    stream: S,
    mut buffer: BytesMut,
    mut rx: mpsc::Receiver<Job>,
    delay: Duration,
) -> Result<(), Error> {
    let (mut read, mut write) = tokio::io::split(stream);
    let (pending_tx, mut pending_rx) = mpsc::channel::<Vec<Job>>(4);
    let send = async move {
        let mut output = BytesMut::with_capacity(8192);
        while let Some(first) = rx.recv().await {
            let mut jobs = Vec::with_capacity(64);
            jobs.push(first);
            if !delay.is_zero() {
                tokio::time::sleep(delay).await;
            }
            while jobs.len() < 64 {
                match rx.try_recv() {
                    Ok(job) => jobs.push(job),
                    Err(_) => break,
                }
            }
            output.clear();
            let combine = cfg!(feature = "grouped-read-sync")
                && jobs.len() > 1
                && jobs.iter().all(|j| j.read_only);
            for job in &jobs {
                if combine {
                    if !job.bytes.ends_with(&[b'S', 0, 0, 0, 4]) {
                        return Err(Error::new("missing read Sync"));
                    }
                    output.extend_from_slice(&job.bytes[..job.bytes.len() - 5]);
                } else {
                    output.extend_from_slice(&job.bytes);
                }
            }
            if combine {
                frontend::sync(&mut output);
            }
            pending_tx
                .send(jobs)
                .await
                .map_err(|_| Error::new("database reader closed"))?;
            write.write_all(&output).await?;
        }
        Ok::<_, Error>(())
    };
    let receive = async move {
        while let Some(jobs) = pending_rx.recv().await {
            #[cfg(feature = "grouped-read-sync")]
            if jobs.len() > 1 && jobs.iter().all(|j| j.read_only) {
                receive_read_group(&mut read, &mut buffer, jobs).await?;
                continue;
            }
            for job in jobs {
                let result = response(&mut read, &mut buffer, &job).await?;
                let _ = job.reply.send(result);
            }
        }
        Ok::<_, Error>(())
    };
    tokio::try_join!(send, receive)?;
    Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    fn frame(tag: u8, body: &[u8], out: &mut Vec<u8>) {
        out.push(tag);
        out.extend_from_slice(&((body.len() + 4) as u32).to_be_bytes());
        out.extend_from_slice(body);
    }
    #[cfg(feature = "continuous-pg")]
    #[tokio::test(flavor = "current_thread")]
    async fn continuous_pipeline_sends_next_job_before_previous_reply() {
        let (client, mut peer) = tokio::io::duplex(32);
        let (tx, rx) = mpsc::channel(256);
        let (first_seen, first_seen_rx) = oneshot::channel();
        let driver = pipeline(client, BytesMut::new(), rx, Duration::ZERO);
        let producer = async {
            let (reply, result) = oneshot::channel();
            tx.send(Job {
                read_only: false,
                #[cfg(feature = "grouped-write-sync")]
                group: None,
                bytes: Bytes::from_static(b"a"),
                columns: Rc::new(Vec::new()),
                reply,
                prepare: false,
            })
            .await
            .unwrap();
            drop(result); // Cancellation must still drain this first reply.
            first_seen_rx.await.unwrap();
            let (reply, result) = oneshot::channel();
            tx.send(Job {
                read_only: false,
                #[cfg(feature = "grouped-write-sync")]
                group: None,
                bytes: Bytes::from_static(b"b"),
                columns: Rc::new(Vec::new()),
                reply,
                prepare: false,
            })
            .await
            .unwrap();
            assert_eq!(result.await.unwrap().unwrap().count, 1);
            // Cross all bounded channel capacities with a small duplex buffer.
            let mut replies = Vec::new();
            for _ in 0..1000 {
                let (reply, result) = oneshot::channel();
                tx.send(Job {
                    read_only: false,
                    #[cfg(feature = "grouped-write-sync")]
                    group: None,
                    bytes: Bytes::from_static(b"c"),
                    columns: Rc::new(Vec::new()),
                    reply,
                    prepare: false,
                })
                .await
                .unwrap();
                replies.push(result);
            }
            drop(tx);
            for reply in replies {
                assert_eq!(reply.await.unwrap().unwrap().count, 1);
            }
        };
        let server = async {
            let mut b = [0];
            peer.read_exact(&mut b).await.unwrap();
            assert_eq!(b, *b"a");
            first_seen.send(()).unwrap();
            // Deliberately withhold the first response until the second request.
            peer.read_exact(&mut b).await.unwrap();
            assert_eq!(b, *b"b");
            let mut error = Vec::new();
            frame(b'E', b"SERROR\0C23503\0Mmissing post\0\0", &mut error);
            frame(b'Z', b"I", &mut error);
            peer.write_all(&error).await.unwrap();
            let mut success = Vec::new();
            frame(b'C', b"INSERT 0 1\0", &mut success);
            frame(b'Z', b"I", &mut success);
            peer.write_all(&success).await.unwrap();
            for _ in 0..1000 {
                peer.read_exact(&mut b).await.unwrap();
                assert_eq!(b, *b"c");
                peer.write_all(&success).await.unwrap();
            }
        };
        tokio::time::timeout(Duration::from_secs(5), async {
            let (result, _, _) = tokio::join!(driver, producer, server);
            result.unwrap();
        })
        .await
        .unwrap();
    }
    #[cfg(feature = "grouped-write-sync")]
    #[tokio::test(flavor = "current_thread")]
    async fn transaction_group_error_drains_sync_without_exposing_partial_success() {
        for fail in [false, true] {
            let mut bytes = Vec::new();
            frame(b'C', b"BEGIN\0", &mut bytes);
            frame(b'C', b"INSERT 0 1\0", &mut bytes);
            if fail {
                frame(b'E', b"SERROR\0C23503\0Mmissing post\0\0", &mut bytes);
            } else {
                frame(b'C', b"INSERT 0 1\0", &mut bytes);
                frame(b'C', b"COMMIT\0", &mut bytes);
            }
            frame(b'Z', if fail { b"E" } else { b"I" }, &mut bytes);
            frame(b'C', b"SELECT 1\0", &mut bytes);
            frame(b'Z', b"I", &mut bytes);
            let (reply, _rx) = oneshot::channel();
            let job = Job {
                read_only: false,
                bytes: Bytes::new(),
                columns: Rc::new(Vec::new()),
                prepare: false,
                reply,
                group: Some((0..4).map(|_| Rc::new(Vec::new())).collect()),
            };
            let mut read = &bytes[..];
            let mut buffer = BytesMut::new();
            let out = response(&mut read, &mut buffer, &job)
                .await
                .unwrap()
                .unwrap();
            if fail {
                assert_eq!(out.group_failure.unwrap().0, 2);
                assert_eq!(out.grouped.len(), 2);
            } else {
                assert!(out.group_failure.is_none());
                assert_eq!(out.grouped.len(), 4);
            }
            let (reply, _rx) = oneshot::channel();
            let next = Job {
                read_only: false,
                bytes: Bytes::new(),
                columns: Rc::new(Vec::new()),
                prepare: false,
                reply,
                group: None,
            };
            assert_eq!(
                response(&mut read, &mut buffer, &next)
                    .await
                    .unwrap()
                    .unwrap()
                    .count,
                1
            );
            assert!(buffer.is_empty());
        }
    }
    #[cfg(feature = "grouped-read-sync")]
    #[tokio::test(flavor = "current_thread")]
    async fn read_group_error_marks_only_unexecuted_reads_for_retry() {
        let mut bytes = Vec::new();
        frame(b'C', b"SELECT 1\0", &mut bytes);
        frame(b'E', b"SERROR\0C22012\0Mdivision by zero\0\0", &mut bytes);
        frame(b'Z', b"I", &mut bytes);
        let mut jobs = Vec::new();
        let mut replies = Vec::new();
        for _ in 0..3 {
            let (reply, rx) = oneshot::channel();
            replies.push(rx);
            jobs.push(Job {
                read_only: true,
                bytes: Bytes::new(),
                columns: Rc::new(Vec::new()),
                prepare: false,
                reply,
                #[cfg(feature = "grouped-write-sync")]
                group: None,
            });
        }
        receive_read_group(&mut &bytes[..], &mut BytesMut::new(), jobs)
            .await
            .unwrap();
        assert_eq!(replies.remove(0).await.unwrap().unwrap().count, 1);
        let error = replies.remove(0).await.unwrap().err().unwrap();
        assert!(!error.skipped_read);
        assert_eq!(error.code(), Some(&error::SqlState::DIVISION_BY_ZERO));
        assert!(replies.remove(0).await.unwrap().err().unwrap().skipped_read);
    }
    #[cfg(feature = "grouped-read-sync")]
    #[tokio::test(flavor = "current_thread")]
    async fn skipped_retry_is_read_only() {
        for read_only in [true, false] {
            let (tx, mut rx) = mpsc::channel::<Job>(256);
            let client = Client {
                tx,
                next: Cell::new(0),
            };
            let statement = Statement {
                read_only,
                name: "q0".into(),
                params: Rc::new(Vec::new()),
                columns: Rc::new(Vec::new()),
            };
            let server = async {
                let job = rx.recv().await.unwrap();
                let mut error = Error::new("skipped");
                error.skipped_read = true;
                assert!(job.reply.send(Err(error)).is_ok());
                if read_only {
                    let job = rx.recv().await.unwrap();
                    assert!(job.read_only);
                    assert!(
                        job.reply
                            .send(Ok(Output {
                                count: 7,
                                ..Output::default()
                            }))
                            .is_ok()
                    );
                }
            };
            let (result, _) = tokio::join!(client.run(&statement, &[]), server);
            if read_only {
                assert_eq!(result.unwrap().count, 7);
            } else {
                assert!(result.is_err());
            }
        }
    }
    #[cfg(feature = "grouped-read-sync")]
    #[tokio::test(flavor = "current_thread")]
    async fn read_pipeline_emits_one_sync_and_preserves_order() {
        let (client, mut peer) = tokio::io::duplex(32);
        let (tx, rx) = mpsc::channel(256);
        let producer = async {
            let mut replies = Vec::new();
            for b in b"abc" {
                let (reply, result) = oneshot::channel();
                replies.push(result);
                tx.send(Job {
                    read_only: true,
                    bytes: Bytes::from(vec![*b, b'S', 0, 0, 0, 4]),
                    columns: Rc::new(Vec::new()),
                    prepare: false,
                    reply,
                    #[cfg(feature = "grouped-write-sync")]
                    group: None,
                })
                .await
                .unwrap();
            }
            drop(tx);
            for (i, reply) in replies.into_iter().enumerate() {
                assert_eq!(reply.await.unwrap().unwrap().count, (i + 1) as u64);
            }
        };
        let server = async {
            let mut bytes = [0; 8];
            peer.read_exact(&mut bytes).await.unwrap();
            assert_eq!(&bytes, b"abcS\0\0\0\x04");
            let mut out = Vec::new();
            for n in 1..=3 {
                frame(b'C', format!("SELECT {n}\0").as_bytes(), &mut out);
            }
            frame(b'Z', b"I", &mut out);
            peer.write_all(&out).await.unwrap();
        };
        tokio::time::timeout(Duration::from_secs(5), async {
            let (_, result, _) = tokio::join!(
                producer,
                pipeline(client, BytesMut::new(), rx, Duration::ZERO),
                server
            );
            result.unwrap();
        })
        .await
        .unwrap();
    }
    #[cfg(feature = "grouped-read-sync")]
    #[tokio::test(flavor = "current_thread")]
    #[ignore = "requires DATABASE_URL; explicit live protocol validation"]
    async fn live_read_group_recovers_skipped_query() {
        tokio::task::LocalSet::new()
            .run_until(async {
                let (client, driver) = connect(&std::env::var("DATABASE_URL").unwrap(), NoTls)
                    .await
                    .unwrap();
                tokio::task::spawn_local(async move {
                    driver.await.unwrap();
                });
                let good = client.prepare("SELECT $1::int4").await.unwrap().read_only();
                let bad = client
                    .prepare("SELECT 1 / $1::int4")
                    .await
                    .unwrap()
                    .read_only();
                let a = 111i32;
                let zero = 0i32;
                let b = 222i32;
                let (x, y, z) = tokio::join!(
                    async { client.query_one(&good, &[&a]).await },
                    async { client.query_one(&bad, &[&zero]).await },
                    async { client.query_one(&good, &[&b]).await }
                );
                assert_eq!(x.unwrap().get::<i32>(0), 111);
                assert_eq!(
                    y.err().unwrap().code(),
                    Some(&error::SqlState::DIVISION_BY_ZERO)
                );
                assert_eq!(z.unwrap().get::<i32>(0), 222);
                assert_eq!(
                    client
                        .query_one(&good, &[&333i32])
                        .await
                        .unwrap()
                        .get::<i32>(0),
                    333
                );
            })
            .await;
    }
    #[tokio::test(flavor = "current_thread")]
    async fn error_and_canceled_receiver_do_not_consume_next_result() {
        let mut bytes = Vec::new();
        frame(b'E', b"SERROR\0C23503\0Mmissing post\0\0", &mut bytes);
        frame(b'Z', b"I", &mut bytes);
        frame(b'C', b"INSERT 0 1\0", &mut bytes);
        frame(b'Z', b"I", &mut bytes);
        let mut read = &bytes[..];
        let mut buffer = BytesMut::new();
        let (tx, rx) = oneshot::channel();
        drop(rx);
        let first = Job {
            read_only: false,
            #[cfg(feature = "grouped-write-sync")]
            group: None,
            bytes: Bytes::new(),
            columns: Rc::new(Vec::new()),
            reply: tx,
            prepare: false,
        };
        let result = response(&mut read, &mut buffer, &first).await.unwrap();
        assert_eq!(
            result.err().unwrap().code(),
            Some(&error::SqlState::FOREIGN_KEY_VIOLATION)
        );
        let (tx, _rx) = oneshot::channel();
        let second = Job {
            read_only: false,
            #[cfg(feature = "grouped-write-sync")]
            group: None,
            bytes: Bytes::new(),
            columns: Rc::new(Vec::new()),
            reply: tx,
            prepare: false,
        };
        let result = response(&mut read, &mut buffer, &second)
            .await
            .unwrap()
            .ok()
            .unwrap();
        assert_eq!(result.count, 1);
        assert!(buffer.is_empty());
    }
    #[tokio::test(flavor = "current_thread")]
    async fn invalid_length_and_truncated_frames_fail() {
        for bytes in [vec![b'D', 0x7f, 0xff, 0xff, 0xff], vec![b'Z', 0, 0, 0, 5]] {
            assert!(
                message(&mut &bytes[..], &mut BytesMut::new())
                    .await
                    .is_err()
            );
        }
    }
}
