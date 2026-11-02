//! The internal WebSocket server the Chromium pages connect to (127.0.0.1:CSS_WS_PORT, `?w=K` names the worker).
use std::sync::Arc;
use std::time::Duration;
use tokio::io::AsyncWriteExt;
use tokio::net::{TcpListener, TcpStream};
use tokio::sync::mpsc;

use crate::hub::{init_message, Hub, Out};
use crate::ws::{self, Ev};

pub async fn serve(hub: Arc<Hub>) {
    let l = loop {
        match TcpListener::bind(("127.0.0.1", hub.cfg.ws_port)).await {
            Ok(l) => break l,
            Err(e) => {
                eprintln!("css-front: cannot bind 127.0.0.1:{}: {e}", hub.cfg.ws_port);
                tokio::time::sleep(Duration::from_secs(1)).await;
            }
        }
    };
    loop {
        if let Ok((s, _)) = l.accept().await {
            let _ = s.set_nodelay(true);
            tokio::spawn(page(s, hub.clone()));
        }
    }
}

fn worker_of(path: &str) -> Option<usize> {
    let q = path.split_once('?')?.1;
    q.split('&').find_map(|kv| kv.strip_prefix("w=")?.parse().ok())
}

async fn page(mut s: TcpStream, hub: Arc<Hub>) {
    let Ok((path, extra)) = ws::handshake(&mut s).await else { return };
    let Some(idx) = worker_of(&path).filter(|i| *i < hub.workers.len()) else { return };
    if path.starts_with("/pg") {
        return crate::relay::run(s, extra, hub).await; // a Postgres connection of the page: a byte relay, nothing else
    }
    let (rd, mut wr) = s.into_split();
    let mut rd = ws::Reader::new(rd, extra);
    let (tx, mut rx) = mpsc::unbounded_channel::<Out>();

    // writer: serialises everything sent to this page
    let writer = tokio::spawn(async move {
        while let Some(m) = rx.recv().await {
            let r = match m {
                Out::Text(mut b) => ws::write_frame(&mut wr, &mut b, 1).await,
                Out::Pong(p) => {
                    let mut b = vec![0u8; ws::HEADROOM];
                    b.extend_from_slice(&p);
                    ws::write_frame(&mut wr, &mut b, 0xA).await
                }
            };
            if r.is_err() {
                break;
            }
        }
        let _ = wr.shutdown().await;
    });

    let _ = tx.send(Out::Text(init_message(&hub.cfg, idx)));
    let mut gn = None;
    loop {
        match rd.next().await {
            Ok(Ev::Msg(m)) => match gn {
                None => {
                    // the first message is the text {"ready":1}
                    if m.starts_with(b"{\"ready\"") {
                        gn = Some(hub.worker_up(idx, tx.clone()));
                    } else {
                        break;
                    }
                }
                Some(g) => hub.on_result(idx, g, &m),
            },
            Ok(Ev::Ping(p)) => {
                let _ = tx.send(Out::Pong(p));
            }
            Ok(Ev::Close) | Err(_) => break,
        }
    }
    if let Some(g) = gn {
        hub.worker_down(idx, Some(g));
    }
    hub.workers[idx].kill.notify_one(); // the page is gone: restart its Chromium
    writer.abort();
}
