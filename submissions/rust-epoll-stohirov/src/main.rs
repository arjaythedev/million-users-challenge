mod gzip;
mod http;
mod json;
mod jwt;
mod pg;
mod server;
mod sha256;
mod sys;

use std::net::{IpAddr, ToSocketAddrs};

const MAX_DB_CONNECTIONS: usize = 480;

fn env(name: &str) -> Option<String> {
    std::env::var(name).ok().filter(|v| !v.is_empty())
}

fn env_num<T: std::str::FromStr>(name: &str, fallback: T) -> T {
    match env(name) {
        Some(v) => v.parse().unwrap_or_else(|_| die(&format!("{name} is not a number: {v}"))),
        None => fallback,
    }
}

fn die(msg: &str) -> ! {
    eprintln!("{msg}");
    std::process::exit(1);
}

fn main() {
    let start_ns = sys::now_ns();
    let secret = env("JWT_SECRET").unwrap_or_else(|| die("JWT_SECRET is not set"));
    let db = pg::DbConfig::parse(&env("DATABASE_URL").unwrap_or_else(|| die("DATABASE_URL is not set")))
        .unwrap_or_else(|e| die(&e));
    let host_s = env("HOST").unwrap_or_else(|| "0.0.0.0".into());
    let host: IpAddr = host_s.parse().unwrap_or_else(|_| {
        (host_s.as_str(), 0)
            .to_socket_addrs()
            .ok()
            .and_then(|mut a| a.next())
            .map(|a| a.ip())
            .unwrap_or_else(|| die(&format!("bad HOST {host_s}")))
    });
    let mut ports = vec![env_num::<u16>("PORT", 3000)];
    if let Some(p) = env("EXTRA_PORT") {
        ports.push(p.parse().unwrap_or_else(|_| die("bad EXTRA_PORT")));
    }

    let cpus = sys::allowed_cpus();
    let cores = if cpus.is_empty() { std::thread::available_parallelism().map_or(1, |n| n.get()) } else { cpus.len() };
    let loops: usize = env_num("LOOPS", cores).max(1);
    let readers = env_num::<usize>("DB_POOL", 48).div_ceil(loops).max(1);
    let writers = env_num::<usize>("DB_WRITERS", 32).div_ceil(loops).max(1);
    if loops * (readers + writers) > MAX_DB_CONNECTIONS {
        die(&format!("{loops} loops x ({readers} read + {writers} write) connections is over {MAX_DB_CONNECTIONS}"));
    }
    let hold_us: u64 = env_num("HOLD_MICROS", 1000);
    let gzip_on = env_num::<u32>("GZIP", 1) != 0;
    let cfg: &'static server::Config = Box::leak(Box::new(server::Config {
        readers,
        writers,
        pack_max: env_num::<usize>("PACK_MAX", 32).max(1),
        hold_ns: hold_us * 1000,
        gzip_min: if gzip_on { env_num("GZIP_MIN", 1024) } else { usize::MAX },
        start_ns,
        pass_ns: env_num::<u64>("PASS_MICROS", 4000) * 1000,
        edge: env_num::<u32>("EPOLL_ET", 1) != 0,
        thin: env_num::<u32>("THIN_TIMEOUTS", 1) != 0,
        sched_idle: env_num::<u32>("SCHED_IDLE", 0) != 0,
        timer_slack_ns: env_num::<u64>("TIMER_SLACK_MICROS", 250) * 1000,
        probe_ns: env_num::<u64>("PG_PROBE_MICROS", 50_000) * 1000,
    }));

    unsafe { libc::signal(libc::SIGPIPE, libc::SIG_IGN) };
    let nofile = sys::raise_nofile().clamp(1024, 1 << 24);
    let table = server::Table::new(nofile);

    let defer: i32 = env_num("DEFER_ACCEPT", 5);
    let rto_min_us: i32 = env_num("RTO_MIN_MICROS", 60_000);
    let steer = env_num::<u32>("INCOMING_CPU", 1) != 0 && loops == cpus.len();
    let mut listeners = Vec::new();
    for k in 0..loops {
        let fds: Vec<i32> = ports
            .iter()
            .map(|&p| {
                let fd = sys::listen(host, p, 65535, defer).unwrap_or_else(|e| die(&e));
                sys::tune_listener(fd, cfg.thin, rto_min_us, if steer { cpus[k] as i32 } else { -1 }, k == 0 && p == ports[0]);
                fd
            })
            .collect();
        listeners.push(fds);
    }
    eprintln!(
        "listening on {host} ports {ports:?}: {loops} loops on CPUs {cpus:?}, each with {readers} read and {writers} \
         write connections to {}:{}; {} us passes, reads and writes held up to {hold_us} us otherwise, {} reads per \
         batch; epoll {}; connections steered by CPU: {steer}; gzip {}; fd table {nofile}",
        db.host,
        db.port,
        cfg.pass_ns / 1000,
        cfg.pack_max,
        if cfg.edge { "edge-triggered" } else { "level-triggered" },
        if gzip_on { format!("from {} bytes", cfg.gzip_min) } else { "off".into() },
    );

    let mut threads = Vec::new();
    for (id, fds) in listeners.into_iter().enumerate() {
        let db = db.clone();
        let secret = secret.clone();
        threads.push(
            std::thread::Builder::new()
                .name(format!("loop-{id}"))
                .stack_size(4 << 20)
                .spawn(move || {
                    server::Loop::new(id, cfg, db, secret.as_bytes(), table, fds).run();
                })
                .unwrap(),
        );
    }
    for t in threads {
        let _ = t.join();
    }
}
