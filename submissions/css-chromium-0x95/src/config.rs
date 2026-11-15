//! Environment + the shared chunk-size constants (gen/constants.json, also read by the CSS generator).
use std::path::PathBuf;
use std::time::Duration;

const CONSTANTS_JSON: &str = include_str!("../gen/constants.json");

/// Tiny lookup of `"name": <number>` in the constants file (no JSON dependency).
fn constant(name: &str) -> usize {
    let key = format!("\"{name}\"");
    let at = CONSTANTS_JSON.find(&key).unwrap_or_else(|| panic!("gen/constants.json: missing {name}"));
    let rest = CONSTANTS_JSON[at + key.len()..].trim_start().trim_start_matches(':').trim_start();
    let end = rest.find(|c: char| !c.is_ascii_digit()).unwrap_or(rest.len());
    rest[..end].parse().unwrap_or_else(|_| panic!("gen/constants.json: bad {name}"))
}

pub struct Consts {
    pub path_chunk: usize,
    pub auth_chunk: usize,
    pub body_chunk: usize,
    pub sha_block: usize,
    /// frame pass (gen/http.py): bytes per chunk element, window length, head length from which tier S is skipped
    pub raw_chunk: usize,
    pub head_max: usize,
    pub frame_big: usize,
}

/// The parts of DATABASE_URL: host/port are all the Rust relay needs; user/password/database go to the page, which speaks
/// the Postgres protocol itself.
#[derive(Debug, Clone, PartialEq)]
pub struct DbUrl {
    pub host: String,
    pub port: u16,
    pub user: String,
    pub password: String,
    pub database: String,
}

fn pct_decode(s: &str) -> String {
    let b = s.as_bytes();
    let hex = |c: u8| (c as char).to_digit(16);
    let mut o = Vec::with_capacity(b.len());
    let mut i = 0;
    while i < b.len() {
        if b[i] == b'%' && i + 3 <= b.len() {
            if let (Some(h), Some(l)) = (hex(b[i + 1]), hex(b[i + 2])) {
                o.push((h * 16 + l) as u8);
                i += 3;
                continue;
            }
        }
        o.push(b[i]);
        i += 1;
    }
    String::from_utf8_lossy(&o).into_owned()
}

/// `postgres://[user[:password]@]host[:port][/database][?ignored]` (also `postgresql://`; `[v6]` hosts; percent-encoding).
pub fn parse_db_url(url: &str) -> Result<DbUrl, String> {
    let rest = url
        .strip_prefix("postgres://")
        .or_else(|| url.strip_prefix("postgresql://"))
        .ok_or("DATABASE_URL must start with postgres://")?;
    let rest = rest.split(['?', '#']).next().unwrap_or("");
    let (auth, hostpart) = match rest.rsplit_once('@') {
        Some((a, h)) => (Some(a), h),
        None => (None, rest),
    };
    let (hostport, database) = match hostpart.split_once('/') {
        Some((h, d)) => (h, d),
        None => (hostpart, ""),
    };
    let (host, port) = if let Some(v6) = hostport.strip_prefix('[') {
        let (h, r) = v6.split_once(']').ok_or("bad IPv6 host in DATABASE_URL")?;
        (h.to_string(), r.strip_prefix(':').filter(|p| !p.is_empty()))
    } else {
        match hostport.rsplit_once(':') {
            Some((h, p)) => (h.to_string(), Some(p).filter(|p| !p.is_empty())),
            None => (hostport.to_string(), None),
        }
    };
    if host.is_empty() {
        return Err("DATABASE_URL has no host (unix sockets are not supported)".into());
    }
    let port = match port {
        Some(p) => p.parse().map_err(|_| "bad port in DATABASE_URL".to_string())?,
        None => 5432,
    };
    let (user, password) = match auth {
        Some(a) => match a.split_once(':') {
            Some((u, p)) => (pct_decode(u), pct_decode(p)),
            None => (pct_decode(a), String::new()),
        },
        None => (String::new(), String::new()),
    };
    let user = if user.is_empty() { env("PGUSER").unwrap_or_else(|| "postgres".into()) } else { user };
    let database = if database.is_empty() { user.clone() } else { pct_decode(database) };
    Ok(DbUrl { host: pct_decode(&host), port, user, password, database })
}

pub struct Cfg {
    pub host: String,
    pub ports: Vec<u16>,
    pub jwt_key: Vec<u8>,
    pub db: DbUrl,
    /// total Chromium pages = heavy + light (indices 0..heavy are the heavy pool)
    pub workers: usize,
    pub heavy: usize,
    pub light: usize,
    /// heavy (Authorization header present) requests per WS message to a heavy worker
    pub heavy_batch: usize,
    pub light_batch: usize,
    /// light requests an otherwise idle heavy worker takes at a time
    pub steal_batch: usize,
    /// how long a heavy worker waits for the database round trip of the request it just finished stage 1 of
    pub hold: Duration,
    /// queue limits (requests waiting for a worker); beyond them requests get 503 at once
    pub queue_heavy: usize,
    pub queue_light: usize,
    /// glue: run gc() after this many heavy requests (0: never), and at least every `gc_idle_ms` when idle
    pub gc_every: usize,
    pub gc_light_every: usize,
    /// nice value of heavy Chromiums (0: leave alone)
    pub heavy_nice: i32,
    pub stats: bool,
    pub ws_port: u16,
    pub chrome_bin: String,
    /// Postgres connections of each page (the page opens them through the relay): heavy pages 1, light pages the rest
    pub pg_heavy: usize,
    pub pg_light: usize,
    pub front_threads: usize,
    pub web_dir: PathBuf,
    pub start_ms: u64,
    pub k: Consts,
}

fn env(k: &str) -> Option<String> {
    std::env::var(k).ok().filter(|v| !v.is_empty())
}

fn die(m: &str) -> ! {
    eprintln!("css-front: {m}");
    std::process::exit(1)
}

impl Cfg {
    pub fn from_env() -> Cfg {
        let n = std::thread::available_parallelism().map_or(1, |n| n.get());
        let num = |k: &str, d: usize| env(k).map_or(d, |v| v.parse().unwrap_or_else(|_| die(&format!("bad {k}"))));
        let mut ports = vec![num("PORT", 3000) as u16];
        if let Some(p) = env("EXTRA_PORT") {
            ports.push(p.parse().unwrap_or_else(|_| die("bad EXTRA_PORT")));
        }
        let jwt_key = env("JWT_SECRET").unwrap_or_else(|| die("JWT_SECRET must be set")).into_bytes();
        if jwt_key.len() > 64 {
            die("JWT_SECRET longer than 64 bytes is not supported (the CSS HMAC takes a key of at most one block)");
        }
        // web/ lives next to target/: <submission>/target/release/css-front -> <submission>/web
        let web_dir = env("CSS_WEB_DIR").map(PathBuf::from).unwrap_or_else(|| {
            let exe = std::env::current_exe().unwrap_or_default();
            let sub = exe.ancestors().nth(3).map(|p| p.to_path_buf()).unwrap_or_default();
            sub.join("web")
        });
        // Pools. Heavy requests (~200 ms of one page's thread each) set the capacity; light ones (<1 ms) need one page
        // (two on big boxes). The front itself and the Postgres client work need a core or so as well.
        let light = num("CSS_LIGHT", if n >= 12 { 2 } else { 1 });
        let heavy = match env("CSS_HEAVY") {
            Some(_) => num("CSS_HEAVY", 1),
            None => match env("CSS_WORKERS") {
                Some(_) => num("CSS_WORKERS", 2).saturating_sub(light),
                None => n.saturating_sub(2),
            },
        }
        .max(1);
        let db = parse_db_url(&env("DATABASE_URL").unwrap_or_else(|| die("DATABASE_URL must be set"))).unwrap_or_else(|e| die(&e));
        // PG_CONNS (default 32) is spread over the pages: heavy pages (a few authenticated requests per second) get one connection
        // each, the light pages share the rest. The total never exceeds 512 (rule 5).
        let pg_total = num("PG_CONNS", 32).clamp(1, 512);
        if heavy + light > 512 {
            die("more than 512 pages would need more than 512 Postgres connections");
        }
        let pg_heavy = 1;
        let pg_light = (pg_total.saturating_sub(heavy) / light).clamp(1, (512 - heavy) / light);
        Cfg {
            host: env("HOST").unwrap_or_else(|| "127.0.0.1".into()),
            ports,
            jwt_key,
            db,
            workers: heavy + light,
            heavy,
            light,
            heavy_batch: num("CSS_HEAVY_BATCH", 1).max(1),
            light_batch: num("CSS_LIGHT_BATCH", 256).max(1),
            steal_batch: num("CSS_STEAL_BATCH", 8),
            hold: Duration::from_micros((num("CSS_HOLD_MS", 8) * 1000) as u64),
            queue_heavy: num("CSS_QUEUE_HEAVY", (heavy * 5).div_ceil(2)).max(1),
            queue_light: num("CSS_QUEUE_LIGHT", 20000).max(1),
            gc_every: num("CSS_GC_EVERY", 1),
            gc_light_every: num("CSS_GC_LIGHT_EVERY", 20000),
            heavy_nice: num("CSS_HEAVY_NICE", 10) as i32,
            stats: env("CSS_STATS").is_some(),
            ws_port: num("CSS_WS_PORT", 9900) as u16,
            chrome_bin: env("CHROME_BIN").unwrap_or_else(|| "/opt/css-chromium/chrome-headless-shell".into()),
            pg_heavy,
            pg_light,
            front_threads: num("FRONT_THREADS", 2).max(1),
            web_dir,
            start_ms: std::time::SystemTime::now().duration_since(std::time::UNIX_EPOCH).unwrap().as_millis() as u64,
            k: Consts {
                path_chunk: constant("path_chunk"),
                auth_chunk: constant("auth_chunk"),
                body_chunk: constant("body_chunk"),
                sha_block: constant("sha_block"),
                raw_chunk: constant("raw_chunk"),
                head_max: constant("head_max"),
                frame_big: constant("frame_big"),
            },
        }
    }
}

impl Cfg {
    /// Postgres connections the page of worker `idx` opens.
    pub fn pg_conns_of(&self, idx: usize) -> usize {
        if idx < self.heavy { self.pg_heavy } else { self.pg_light }
    }
    pub fn pg_total(&self) -> usize {
        self.heavy * self.pg_heavy + self.light * self.pg_light
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn db_urls() {
        let d = parse_db_url("postgres://feed:p%40ss%3Aw@db.example:6543/feed_bench?sslmode=disable").unwrap();
        assert_eq!((d.host.as_str(), d.port, d.user.as_str(), d.password.as_str(), d.database.as_str()), ("db.example", 6543, "feed", "p@ss:w", "feed_bench"));
        let d = parse_db_url("postgresql://u@[::1]/x").unwrap();
        assert_eq!((d.host.as_str(), d.port, d.password.as_str()), ("::1", 5432, ""));
        let d = parse_db_url("postgres://feed:pw@pg:5432/feed").unwrap();
        assert_eq!(d.database, "feed");
        assert!(parse_db_url("mysql://x").is_err());
        assert!(parse_db_url("postgres:///db").is_err());
    }
}
