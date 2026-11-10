//! Postgres: each worker thread has a few long-lived tokio-postgres connections with SPEC.md's statements
//! prepared on each.
//!
//! No pool checkout: a request takes the first idle connection (or the least busy) and sends its query right away;
//! tokio-postgres pipelines concurrent queries on one connection back to back. Reads and writes use separate
//! connections, so a read never queues behind a commit waiting for its WAL flush. A connection that drops is reopened
//! by its own task.

use std::{
    cell::{Cell, RefCell},
    ops::Deref,
    rc::Rc,
    time::{Duration, Instant},
};

use tokio::task::JoinHandle;
use tokio_postgres::{
    Client, NoTls, Statement,
    types::{FromSql, Type},
};

use crate::Config;

// SPEC.md "Endpoints": the queries, one per request.
const FEED_SQL: &str = "SELECT p.id, p.body, p.created_at, u.username, \
    (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count \
    FROM posts p JOIN users u ON u.id = p.user_id \
    ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
const POST_SQL: &str = "SELECT p.id, p.body, p.created_at, u.username, \
    (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count \
    FROM posts p JOIN users u ON u.id = p.user_id \
    WHERE p.id = $1";
const CREATE_SQL: &str = "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at";
const LIKE_SQL: &str =
    "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING";
const PING_SQL: &str = "SELECT 1";

const RECONNECT_MIN: Duration = Duration::from_millis(100);
const RECONNECT_MAX: Duration = Duration::from_secs(5);
/// How long a worker waits for its connections before it starts listening anyway (/health then says 503).
const OPEN_TIMEOUT: Duration = Duration::from_secs(30);

pub struct Statements {
    pub feed: Statement,
    pub post: Statement,
    pub create: Statement,
    pub like: Statement,
    pub ping: Statement,
}

pub struct Conn {
    pub client: Client,
    pub st: Statements,
    inflight: Cell<u32>,
}

type Slot = RefCell<Option<Rc<Conn>>>;

pub struct Pool {
    read: Vec<Slot>,
    write: Vec<Slot>,
    /// Slots that have made their first connection attempt.
    tried: Cell<usize>,
}

/// A connection taken for one query; counts as in flight until dropped.
pub struct Lease(Rc<Conn>);

impl Deref for Lease {
    type Target = Conn;
    fn deref(&self) -> &Conn {
        &self.0
    }
}

impl Drop for Lease {
    fn drop(&mut self) {
        self.0.inflight.set(self.0.inflight.get() - 1);
    }
}

impl Pool {
    pub fn new(read: usize, write: usize) -> Pool {
        let slots = |n| (0..n).map(|_| RefCell::new(None)).collect();
        Pool { read: slots(read), write: slots(write), tried: Cell::new(0) }
    }

    pub fn read(&self) -> Option<Lease> {
        lease(&self.read)
    }

    /// Falls back to a read connection while every write connection is down.
    pub fn write(&self) -> Option<Lease> {
        lease(&self.write).or_else(|| lease(&self.read))
    }

    /// Starts every connection's task and returns once each has made its first attempt (or after 30 s).
    pub async fn open(&'static self, cfg: &'static Config) {
        for slot in self.read.iter().chain(&self.write) {
            tokio::task::spawn_local(self.keep_open(slot, cfg));
        }
        let deadline = Instant::now() + OPEN_TIMEOUT;
        while self.tried.get() < self.read.len() + self.write.len() && Instant::now() < deadline {
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
    }

    /// Keeps one slot connected for the life of the process.
    async fn keep_open(&'static self, slot: &'static Slot, cfg: &'static Config) {
        let mut first = true;
        let mut backoff = RECONNECT_MIN;
        loop {
            let connected = connect(cfg).await;
            let driver = match connected {
                Ok((conn, driver)) => {
                    *slot.borrow_mut() = Some(Rc::new(conn));
                    Some(driver)
                }
                Err(e) => {
                    eprintln!("[db] connect: {}", describe(&e));
                    None
                }
            };
            if first {
                first = false;
                self.tried.set(self.tried.get() + 1);
            }
            if let Some(driver) = driver {
                backoff = RECONNECT_MIN;
                match driver.await {
                    Ok(Ok(())) => eprintln!("[db] connection closed"),
                    Ok(Err(e)) => eprintln!("[db] connection lost: {}", describe(&e)),
                    Err(e) => eprintln!("[db] connection task failed: {e}"),
                }
                *slot.borrow_mut() = None;
            }
            tokio::time::sleep(backoff).await;
            backoff = (backoff * 2).min(RECONNECT_MAX);
        }
    }
}

/// An error with its cause: tokio-postgres's own message for a server error is just "db error", and Postgres's message
/// (e.g. "ERROR: permission denied for table posts") is in the source.
pub fn describe(e: &tokio_postgres::Error) -> String {
    match std::error::Error::source(e) {
        Some(cause) => format!("{e}: {cause}"),
        None => e.to_string(),
    }
}

/// The live connection with the fewest queries in flight (the first idle one if there is one).
fn lease(slots: &[Slot]) -> Option<Lease> {
    let mut best: Option<Rc<Conn>> = None;
    for slot in slots {
        let slot = slot.borrow();
        let Some(conn) = slot.as_ref() else { continue };
        if conn.client.is_closed() {
            continue;
        }
        if best.as_ref().is_none_or(|b| conn.inflight.get() < b.inflight.get()) {
            best = Some(conn.clone());
            if conn.inflight.get() == 0 {
                break;
            }
        }
    }
    best.map(|conn| {
        conn.inflight.set(conn.inflight.get() + 1);
        Lease(conn)
    })
}

type Driver = JoinHandle<Result<(), tokio_postgres::Error>>;

async fn connect(cfg: &Config) -> Result<(Conn, Driver), tokio_postgres::Error> {
    let (client, connection) = cfg.pg.connect(NoTls).await?;
    // The connection future does the socket I/O for every query on this client.
    let driver = tokio::task::spawn_local(connection);
    let st = Statements {
        feed: client.prepare(FEED_SQL).await?,
        post: client.prepare(POST_SQL).await?,
        create: client.prepare(CREATE_SQL).await?,
        like: client.prepare(LIKE_SQL).await?,
        ping: client.prepare(PING_SQL).await?,
    };
    Ok((Conn { client, st, inflight: Cell::new(0) }, driver))
}

/// TIMESTAMPTZ as Postgres sends it in binary: microseconds since 2000-01-01 UTC.
pub struct Ts(pub i64);

impl<'a> FromSql<'a> for Ts {
    fn from_sql(_: &Type, raw: &'a [u8]) -> Result<Self, Box<dyn std::error::Error + Sync + Send>> {
        Ok(Ts(i64::from_be_bytes(raw.try_into()?)))
    }

    fn accepts(ty: &Type) -> bool {
        *ty == Type::TIMESTAMPTZ
    }
}
