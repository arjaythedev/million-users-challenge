//! The Postgres relay: `ws://127.0.0.1:$CSS_WS_PORT/pg?w=K&i=J`. A page connects, we open one TCP connection to the host:port
//! of DATABASE_URL and copy bytes both ways: binary WebSocket messages -> TCP, TCP reads -> binary WebSocket messages.
//! Nothing here looks at the bytes (the page speaks the Postgres v3 protocol itself: web/pg.js). Either side closing
//! closes the other. At most `MAX_PG` relays exist at a time (rule 5: 512 connections in total).
use std::sync::Arc;
use std::sync::atomic::{AtomicUsize, Ordering::Relaxed};
use std::time::Duration;
use tokio::io::{AsyncReadExt, AsyncWriteExt};
use tokio::net::TcpStream;

use crate::hub::Hub;
use crate::ws::{self, Ev, HEADROOM};

pub const MAX_PG: usize = 512;
static OPEN: AtomicUsize = AtomicUsize::new(0);

struct Slot;
impl Drop for Slot {
    fn drop(&mut self) {
        OPEN.fetch_sub(1, Relaxed);
    }
}

/// `s` is a page's WebSocket (handshake done, `extra` = bytes already read behind it).
pub async fn run(s: TcpStream, extra: Vec<u8>, hub: Arc<Hub>) {
    if OPEN.fetch_add(1, Relaxed) >= MAX_PG {
        OPEN.fetch_sub(1, Relaxed);
        return;
    }
    let _slot = Slot;
    let db = &hub.cfg.db;
    let (ws_rd, mut ws_wr) = s.into_split();
    let up = match tokio::time::timeout(Duration::from_secs(5), TcpStream::connect((db.host.as_str(), db.port))).await {
        Ok(Ok(u)) => u,
        r => {
            // the page sees the close (with the reason, for /health's error text) and retries with backoff
            let why = match r {
                Ok(Err(e)) => e.to_string(),
                _ => "timed out".to_string(),
            };
            let _ = ws_wr.write_all(&ws::close_frame(1011, &format!("connect {}:{}: {why}", db.host, db.port))).await;
            return;
        }
    };
    let _ = up.set_nodelay(true);
    let (mut db_rd, mut db_wr) = up.into_split();
    let mut rd = ws::Reader::new(ws_rd, extra);
    let to_db = async {
        loop {
            match rd.next().await {
                Ok(Ev::Msg(m)) => {
                    if db_wr.write_all(&m).await.is_err() {
                        return;
                    }
                }
                Ok(Ev::Ping(_)) => {}
                Ok(Ev::Close) | Err(_) => return,
            }
        }
    };
    let to_ws = async {
        let mut buf = vec![0u8; HEADROOM + 32 * 1024];
        loop {
            let n = match db_rd.read(&mut buf[HEADROOM..]).await {
                Ok(0) | Err(_) => return,
                Ok(n) => n,
            };
            if ws::write_frame(&mut ws_wr, &mut buf[..HEADROOM + n], 2).await.is_err() {
                return;
            }
        }
    };
    tokio::select! {
        _ = to_db => {}
        _ = to_ws => {}
    }
    let _ = ws_wr.shutdown().await;
    let _ = db_wr.shutdown().await;
}
