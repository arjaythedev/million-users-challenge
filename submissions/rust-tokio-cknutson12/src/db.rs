//! Each worker thread owns a few Postgres connections. Concurrent queries on one connection are pipelined by
//! tokio-postgres, so a handful per thread is enough. Reads and writes use separate connections, so a read
//! never waits in line behind an INSERT's commit (WAL flush).

use std::{cell::{Cell, RefCell}, error::Error as StdError, rc::Rc, time::Duration};

use tokio_postgres::{
    types::{FromSql, Type},
    Client, NoTls, Row, Statement,
};

pub use tokio_postgres::error::SqlState;

const POST_SELECT: &str = "SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count
  FROM posts p JOIN users u ON u.id = p.user_id";

pub struct Conn {
    client: Client,
    inflight: Cell<u32>,
    health: Statement,
    feed: Statement,
    post: Statement,
    insert_post: Statement,
    insert_like: Statement,
}

impl Conn {
    async fn open(url: &str) -> Result<(Client, tokio_postgres::Connection<tokio_postgres::Socket, tokio_postgres::tls::NoTlsStream>), tokio_postgres::Error> {
        tokio_postgres::connect(url, NoTls).await
    }

    async fn prepare(client: Client) -> Result<Conn, tokio_postgres::Error> {
        Ok(Conn {
            health: client.prepare("SELECT 1").await?,
            feed: client.prepare(&format!("{POST_SELECT} ORDER BY p.created_at DESC, p.id DESC LIMIT 20")).await?,
            post: client.prepare(&format!("{POST_SELECT} WHERE p.id = $1")).await?,
            insert_post: client.prepare("INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at").await?,
            insert_like: client
                .prepare("INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING")
                .await?,
            inflight: Cell::new(0),
            client,
        })
    }
}

/// One connection slot: (re)connects in the background whenever its connection drops.
struct Slot {
    conn: RefCell<Option<Rc<Conn>>>,
}

pub struct Pool {
    read: Vec<Rc<Slot>>,
    write: Vec<Rc<Slot>>,
}

#[derive(Debug)]
pub enum DbError {
    Unavailable,
    Pg(tokio_postgres::Error),
}

impl std::fmt::Display for DbError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            DbError::Unavailable => f.write_str("no database connection"),
            DbError::Pg(e) => match e.as_db_error() {
                Some(db) => write!(f, "{}: {}", db.code().code(), db.message()),
                None => write!(f, "{e}"),
            },
        }
    }
}

impl From<tokio_postgres::Error> for DbError {
    fn from(e: tokio_postgres::Error) -> Self {
        DbError::Pg(e)
    }
}

impl DbError {
    pub fn code(&self) -> Option<&SqlState> {
        match self {
            DbError::Pg(e) => e.code(),
            DbError::Unavailable => None,
        }
    }
}

/// Decrements the connection's in-flight count when the query finishes (or is dropped).
struct InFlight(Rc<Conn>);

impl Drop for InFlight {
    fn drop(&mut self) {
        self.0.inflight.set(self.0.inflight.get() - 1);
    }
}

impl Pool {
    /// Opens `reads` + `writes` connections and keeps them open; returns once all are connected.
    pub async fn start(url: Rc<str>, reads: usize, writes: usize) -> Pool {
        let slot = || Rc::new(Slot { conn: RefCell::new(None) });
        let pool = Pool { read: (0..reads).map(|_| slot()).collect(), write: (0..writes).map(|_| slot()).collect() };
        for s in pool.read.iter().chain(&pool.write) {
            tokio::task::spawn_local(keep_connected(s.clone(), url.clone()));
        }
        while pool.read.iter().chain(&pool.write).any(|s| s.conn.borrow().is_none()) {
            tokio::time::sleep(Duration::from_millis(20)).await;
        }
        pool
    }

    /// The open connection with the fewest queries in flight.
    fn pick(slots: &[Rc<Slot>]) -> Result<InFlight, DbError> {
        let mut best: Option<Rc<Conn>> = None;
        for s in slots {
            if let Some(c) = &*s.conn.borrow() {
                if best.as_ref().map_or(true, |b| c.inflight.get() < b.inflight.get()) {
                    best = Some(c.clone());
                }
            }
        }
        let c = best.ok_or(DbError::Unavailable)?;
        c.inflight.set(c.inflight.get() + 1);
        Ok(InFlight(c))
    }

    pub async fn health(&self) -> Result<(), DbError> {
        let c = Self::pick(&self.read)?;
        c.0.client.query_one(&c.0.health, &[]).await?;
        Ok(())
    }

    pub async fn feed(&self) -> Result<Vec<Row>, DbError> {
        let c = Self::pick(&self.read)?;
        Ok(c.0.client.query(&c.0.feed, &[]).await?)
    }

    pub async fn post(&self, id: i64) -> Result<Option<Row>, DbError> {
        let c = Self::pick(&self.read)?;
        Ok(c.0.client.query_opt(&c.0.post, &[&id]).await?)
    }

    /// Returns (id, created_at in Postgres microseconds).
    pub async fn insert_post(&self, user_id: i64, body: &str) -> Result<(i64, i64), DbError> {
        let c = Self::pick(&self.write)?;
        let row = c.0.client.query_one(&c.0.insert_post, &[&user_id, &body]).await?;
        Ok((row.try_get(0)?, row.try_get::<_, Timestamp>(1)?.0))
    }

    /// Returns true if the like was new.
    pub async fn insert_like(&self, user_id: i64, post_id: i64) -> Result<bool, DbError> {
        let c = Self::pick(&self.write)?;
        Ok(c.0.client.execute(&c.0.insert_like, &[&user_id, &post_id]).await? == 1)
    }
}

async fn keep_connected(slot: Rc<Slot>, url: Rc<str>) {
    let mut backoff = Duration::from_millis(50);
    loop {
        match Conn::open(&url).await {
            Ok((client, connection)) => {
                // The connection future does the socket I/O; it has to run while we prepare.
                let io = tokio::task::spawn_local(connection);
                match Conn::prepare(client).await {
                    Ok(conn) => {
                        slot.conn.replace(Some(Rc::new(conn)));
                        backoff = Duration::from_millis(50);
                        if let Ok(Err(e)) = io.await {
                            eprintln!("postgres connection lost: {e}");
                        }
                        slot.conn.replace(None);
                    }
                    Err(e) => {
                        eprintln!("postgres prepare failed: {e}");
                        io.abort();
                    }
                }
            }
            Err(e) => eprintln!("postgres connect failed: {e}"),
        }
        tokio::time::sleep(backoff).await;
        backoff = (backoff * 2).min(Duration::from_secs(1));
    }
}

/// TIMESTAMPTZ in its binary wire form: microseconds since 2000-01-01 UTC.
pub struct Timestamp(pub i64);

impl<'a> FromSql<'a> for Timestamp {
    fn from_sql(_: &Type, raw: &'a [u8]) -> Result<Self, Box<dyn StdError + Sync + Send>> {
        Ok(Timestamp(i64::from_be_bytes(raw.try_into()?)))
    }

    fn accepts(ty: &Type) -> bool {
        *ty == Type::TIMESTAMPTZ
    }
}
