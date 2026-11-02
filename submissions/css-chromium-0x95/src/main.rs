//! css-front: the I/O shell of the CSS-evaluated feed API. HTTP in, WS to Chromium pages, a byte relay to Postgres.
//! No application logic here: the page decides everything (CSS) and speaks the Postgres protocol itself (JS); this process
//! only moves bytes (and queues, schedules and respawns the browsers).
mod chrome;
mod config;
mod fragment;
mod http;
mod hub;
mod relay;
mod ws;
mod wsrv;

use std::net::ToSocketAddrs;
use std::sync::Arc;
use std::time::Duration;
use tokio::net::{TcpListener, TcpSocket};

async fn listen(addr: std::net::SocketAddr, hub: Arc<hub::Hub>) {
    let sock = if addr.is_ipv4() { TcpSocket::new_v4() } else { TcpSocket::new_v6() }.expect("socket");
    sock.set_reuseaddr(true).unwrap();
    if let Err(e) = sock.bind(addr) {
        eprintln!("css-front: cannot bind {addr}: {e}");
        std::process::exit(1);
    }
    let l: TcpListener = sock.listen(65535).expect("listen");
    loop {
        match l.accept().await {
            Ok((s, _)) => {
                let _ = s.set_nodelay(true);
                tokio::spawn(http::conn(s, hub.clone()));
            }
            Err(_) => tokio::time::sleep(Duration::from_millis(5)).await, // EMFILE and friends: back off
        }
    }
}

fn main() {
    let cfg = Arc::new(config::Cfg::from_env());
    eprintln!(
        "css-front: {} chromium pages ({} heavy, {} light), {} postgres connections (opened by the pages, relayed to {}:{}), {} front threads, ports {:?}, web {}",
        cfg.workers, cfg.heavy, cfg.light, cfg.pg_total(), cfg.db.host, cfg.db.port, cfg.front_threads, cfg.ports, cfg.web_dir.display()
    );
    let rt = tokio::runtime::Builder::new_multi_thread().worker_threads(cfg.front_threads).enable_all().build().unwrap();
    rt.block_on(async move {
        let hub = hub::Hub::new(cfg.clone());
        tokio::spawn(wsrv::serve(hub.clone()));
        for i in 0..cfg.workers {
            tokio::spawn(chrome::supervise(hub.clone(), i));
        }
        if cfg.stats {
            let h = hub.clone();
            tokio::spawn(async move {
                loop {
                    tokio::time::sleep(Duration::from_secs(10)).await;
                    eprintln!("css-front: {}", h.stats_line());
                }
            });
        }
        for p in &cfg.ports {
            let addr = (cfg.host.as_str(), *p).to_socket_addrs().expect("bad HOST").next().expect("bad HOST");
            tokio::spawn(listen(addr, hub.clone()));
        }
        // run until SIGTERM / SIGINT, then take the browsers down with us
        let mut term = tokio::signal::unix::signal(tokio::signal::unix::SignalKind::terminate()).unwrap();
        tokio::select! {
            _ = term.recv() => {}
            _ = tokio::signal::ctrl_c() => {}
        }
        chrome::kill_all(&hub);
    });
    std::process::exit(0);
}
