//! The hub: one `Worker` per Chromium page, in two pools. The only scheduling input is structural: a request that
//! carries an `Authorization` header is "heavy" (its chain contains the SHA-256 blocks), every other one is "light".
//! Nothing here looks at what the CSS decides.
//!
//!  * Each class has ONE shared FIFO queue (work conserving: an idle worker of the class takes the next entry; there
//!    is no per-worker backlog to get stuck behind). A full queue answers 503 at once instead of growing.
//!  * At most one WS message is in flight per worker (until the page's ack). A light worker takes everything queued (up to
//!    `light_batch`); a heavy worker takes at most `heavy_batch` heavy requests per message (a heavy request is ~200 ms
//!    of the page's single thread, so a bigger batch only delays the first answers).
//!  * The page talks to Postgres itself (web/pg.js, through the byte relay in relay.rs). A request that needs the database
//!    is acked as "pending" and finishes later with its own message. While a heavy worker has pending requests it is
//!    held back for up to `hold` so that their database round trip + stage 3 does not queue behind the next ~250 ms heavy request.
//!  * A heavy worker with nothing heavy to do takes light requests from the light queue (work stealing, small batches).
//!
//! front -> page (JSON text):
//!   {"init":{"style":"<root inline style>","auth_chunk":N,"body_chunk":N,"gc_every":N,"gc_light_every":N,"pg":{..}}}  once, after the handshake
//!   {"add":"<html of new requests and frame chains>","new":[rid,..],"frames":[rid,..],"gc":N,"w":N}   every later message
//!        (new = request chains, frames = frame passes (gen/http.py: Hs..Z), N = heavy requests in `add`: GC hint, w = garbage weight)
//! page -> front:
//!   text   {"ready":1}                                          reply to init (the first message)
//!   binary kind:u8 | ndone:u32le | ndone x (rid:u32le len:u32le bytes) | npend:u32le | npend x rid:u32le | nfr:u32le | nfr x (rid:u32le 13 x u32le)
//!          (a frame result is 13 little endian u32: ok bad hl cl used close cont mt ps pe as ae slow)
//!          kind 0 = ack of an `add` message (done = answered by CSS alone, pend = waiting for Postgres), kind 1 = later completions.
//!          `bytes` is the complete raw HTTP response the page built.
use std::collections::{HashMap, VecDeque};
use std::sync::atomic::{AtomicBool, AtomicU32, AtomicU64, AtomicUsize, Ordering::Relaxed};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};
use tokio::sync::{mpsc, oneshot, Notify};

use crate::config::Cfg;
use crate::fragment::push_num;
use crate::ws::HEADROOM;

pub enum Out {
    /// text message; the buffer starts with ws::HEADROOM reserved bytes
    Text(Vec<u8>),
    Pong(Vec<u8>),
}

/// Scheduling class, decided from the request's framing only (an `Authorization` header is present).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Class {
    Light = 0,
    Heavy = 1,
}

/// What an entry of a class queue is: a request chain, a frame pass without chunks (tier S) or with them (tier E).
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Kind {
    Request = 0,
    Frame1 = 1,
    Frame2 = 2,
}

/// Garbage weight (in light requests) of a frame pass of the given kind, for the light pages' GC policy.
fn weight(k: Kind) -> usize {
    match k {
        Kind::Request | Kind::Frame1 => 1,
        Kind::Frame2 => FRAME2_WEIGHT,
    }
}
const FRAME2_WEIGHT: usize = 200;

/// A request waiting in its class queue for a worker.
struct Entry {
    rid: u32,
    kind: Kind,
    html: Vec<u8>,
    tx: oneshot::Sender<Vec<u8>>,
    tk: Arc<TState>,
}

/// Where a request ended up (filled in when a worker takes it from the queue).
struct TState {
    rid: u32,
    at: Mutex<Option<(Arc<Worker>, u64)>>,
}

/// State shared by all workers: configuration, the two class queues and counters.
struct Shared {
    cfg: Arc<Cfg>,
    q: [Mutex<VecDeque<Entry>>; 2],
    batches: [AtomicU64; 2],
    reqs: [AtomicU64; 2],
    stolen: AtomicU64,
    rejected: AtomicU64,
    gcs: AtomicU64,
    /// total page time (µs) of batches that carried heavy requests, and how many such batches
    heavy_us: AtomicU64,
    heavy_n: AtomicU64,
}

impl Shared {
    /// Takes up to `max` live entries (those whose client is still waiting) from the front of a class queue.
    fn pull(&self, class: Class, max: usize, out: &mut Vec<Entry>) {
        let mut q = self.q[class as usize].lock().unwrap();
        while out.len() < max {
            match q.pop_front() {
                Some(e) if e.tx.is_closed() => drop(e), // abandoned while queued
                Some(e) => out.push(e),
                None => break,
            }
        }
    }
    fn queued(&self, class: Class) -> usize {
        self.q[class as usize].lock().unwrap().len()
    }
}

#[derive(Default)]
struct WState {
    /// bumped when the worker dies: tickets of earlier epochs are void
    epoch: u64,
    /// bumped for every page connection that completes init
    conn: u64,
    ready: bool,
    busy: bool,
    busy_since: Option<Instant>,
    /// the batch in flight carries heavy requests
    busy_heavy: bool,
    tx: Option<mpsc::UnboundedSender<Out>>,
    pending: HashMap<u32, oneshot::Sender<Vec<u8>>>,
    /// requests the page acked as waiting for Postgres and has not completed yet
    awaiting: u32,
    hold_until: Option<Instant>,
    timer: bool,
}

pub struct Worker {
    pub class: Class,
    inflight: AtomicUsize,
    ready: AtomicBool,
    busy: AtomicBool,
    /// notified when the supervisor should kill and restart this worker's Chromium
    pub kill: Notify,
    st: Mutex<WState>,
    sh: Arc<Shared>,
}

pub struct Hub {
    pub cfg: Arc<Cfg>,
    pub workers: Vec<Arc<Worker>>,
    sh: Arc<Shared>,
    next_rid: AtomicU32,
}

/// A request owned by a worker; dropping it (the client went away, or it finished) releases the worker's slot.
pub struct Ticket {
    tk: Arc<TState>,
}

impl Drop for Ticket {
    fn drop(&mut self) {
        let at = self.tk.at.lock().unwrap().take();
        if let Some((w, gn)) = at {
            w.inflight.fetch_sub(1, Relaxed);
            // if the request was abandoned, forget its slot
            let mut st = w.st.lock().unwrap();
            if st.epoch == gn {
                st.pending.remove(&self.tk.rid);
            }
        }
    }
}

impl Hub {
    pub fn new(cfg: Arc<Cfg>) -> Arc<Hub> {
        let sh = Arc::new(Shared {
            cfg: cfg.clone(),
            q: [Mutex::new(VecDeque::new()), Mutex::new(VecDeque::new())],
            batches: Default::default(),
            reqs: Default::default(),
            stolen: AtomicU64::new(0),
            rejected: AtomicU64::new(0),
            gcs: AtomicU64::new(0),
            heavy_us: AtomicU64::new(0),
            heavy_n: AtomicU64::new(0),
        });
        // workers 0..heavy are the heavy pool, the rest the light pool
        let workers = (0..cfg.workers)
            .map(|idx| {
                Arc::new(Worker {
                    class: if idx < cfg.heavy { Class::Heavy } else { Class::Light },
                    inflight: AtomicUsize::new(0),
                    ready: AtomicBool::new(false),
                    busy: AtomicBool::new(false),
                    kill: Notify::new(),
                    st: Mutex::new(WState::default()),
                    sh: sh.clone(),
                })
            })
            .collect();
        Arc::new(Hub { cfg, workers, sh, next_rid: AtomicU32::new(1) })
    }

    /// Request ids stay below 2^30 (registered `<integer>`); they only need to be unique among in-flight requests.
    pub fn alloc_rid(&self) -> u32 {
        match self.next_rid.fetch_add(1, Relaxed) & 0x3fff_ffff {
            0 => 1,
            n => n,
        }
    }

    /// Queues a new request's HTML in its class queue and wakes an idle worker. None: the queue is full (overload).
    pub fn submit(&self, rid: u32, html: Vec<u8>, class: Class, kind: Kind) -> Option<(Ticket, oneshot::Receiver<Vec<u8>>)> {
        let (tx, rx) = oneshot::channel();
        let tk = Arc::new(TState { rid, at: Mutex::new(None) });
        {
            let mut q = self.sh.q[class as usize].lock().unwrap();
            let max = if class == Class::Heavy { self.cfg.queue_heavy } else { self.cfg.queue_light };
            if q.len() >= max {
                self.sh.rejected.fetch_add(1, Relaxed);
                return None;
            }
            q.push_back(Entry { rid, kind, html, tx, tk: tk.clone() });
        }
        self.kick(class);
        Some((Ticket { tk }, rx))
    }

    /// Offers the queued work of `class` to idle workers: first the class's own pool, then (light work only)
    /// idle heavy workers, which steal it when they have nothing heavy to do.
    fn kick(&self, class: Class) {
        let mut own = false;
        for w in self.workers.iter().filter(|w| w.class == class) {
            own = true;
            if w.ready.load(Relaxed) && !w.busy.load(Relaxed) {
                w.flush();
                if self.sh.queued(class) == 0 {
                    return;
                }
            }
        }
        if class == Class::Light || !own {
            for w in self.workers.iter().filter(|w| w.class == Class::Heavy) {
                if w.ready.load(Relaxed) && !w.busy.load(Relaxed) {
                    w.flush();
                    if self.sh.queued(Class::Light) == 0 {
                        return;
                    }
                }
            }
        }
    }

    /// The page connected and answered `init`: `w` is usable. Returns the new generation.
    pub fn worker_up(&self, idx: usize, tx: mpsc::UnboundedSender<Out>) -> u64 {
        let w = &self.workers[idx];
        let gn = {
            let mut st = w.st.lock().unwrap();
            st.conn += 1;
            st.tx = Some(tx);
            st.ready = true;
            st.busy = false;
            st.busy_since = None;
            st.awaiting = 0;
            st.hold_until = None;
            w.busy.store(false, Relaxed);
            w.ready.store(true, Relaxed);
            st.conn
        };
        w.flush();
        gn
    }

    /// The page / its Chromium is gone (or a newer generation replaced it): fail everything in flight.
    pub fn worker_down(&self, idx: usize, gn: Option<u64>) {
        let w = &self.workers[idx];
        let dead = {
            let mut st = w.st.lock().unwrap();
            if gn.is_some_and(|g| g != st.conn) {
                return;
            }
            st.epoch += 1; // voids the tickets of everything in flight
            st.ready = false;
            st.busy = false;
            st.busy_since = None;
            st.tx = None;
            st.awaiting = 0;
            st.hold_until = None;
            w.ready.store(false, Relaxed);
            w.busy.store(false, Relaxed);
            std::mem::take(&mut st.pending)
        };
        drop(dead); // dropping the senders fails the waiting requests (500)
    }

    /// A batch has been in flight for too long.
    pub fn stuck(&self, idx: usize, limit: Duration) -> bool {
        let st = self.workers[idx].st.lock().unwrap();
        st.busy && st.busy_since.is_some_and(|t| t.elapsed() > limit)
    }

    /// One line of scheduler counters (CSS_STATS=1 prints it every 10 s).
    pub fn stats_line(&self) -> String {
        let s = &self.sh;
        let busy = |c: Class| self.workers.iter().filter(|w| w.class == c && w.busy.load(Relaxed)).count();
        let n = |c: Class| self.workers.iter().filter(|w| w.class == c).count();
        let g = |a: &AtomicU64| a.load(Relaxed);
        format!(
            "stats: heavy q={} busy={}/{} reqs={} batches={} | light q={} busy={}/{} reqs={} batches={} | stolen={} rejected={} gc-hints={} heavy-batch-ms={}",
            s.queued(Class::Heavy),
            busy(Class::Heavy),
            n(Class::Heavy),
            g(&s.reqs[1]),
            g(&s.batches[1]),
            s.queued(Class::Light),
            busy(Class::Light),
            n(Class::Light),
            g(&s.reqs[0]),
            g(&s.batches[0]),
            g(&s.stolen),
            g(&s.rejected),
            g(&s.gcs),
            g(&s.heavy_us) / 1000 / g(&s.heavy_n).max(1),
        )
    }

    /// A result message from the page of worker `idx` (generation `gn`): see the module doc for the binary format.
    pub fn on_result(&self, idx: usize, gn: u64, msg: &[u8]) {
        let Some((kind, done, pend)) = parse_result(msg) else {
            eprintln!("worker {idx}: unparsable message ({} bytes)", msg.len());
            self.workers[idx].kill.notify_one();
            return;
        };
        let w = &self.workers[idx];
        let mut senders: Vec<(oneshot::Sender<Vec<u8>>, &[u8])> = Vec::with_capacity(done.len());
        {
            let mut st = w.st.lock().unwrap();
            if st.conn != gn {
                return;
            }
            for (rid, bytes) in done.iter() {
                if let Some(tx) = st.pending.remove(rid) {
                    senders.push((tx, bytes));
                }
            }
            if kind == 0 {
                if pend > 0 && w.class == Class::Heavy && !self.cfg.hold.is_zero() {
                    // their database round trip is about to come back: do not start the next heavy batch yet
                    st.awaiting += pend;
                    st.hold_until = Some(Instant::now() + self.cfg.hold);
                }
                if st.busy_heavy {
                    if let Some(t) = st.busy_since {
                        self.sh.heavy_us.fetch_add(t.elapsed().as_micros() as u64, Relaxed);
                        self.sh.heavy_n.fetch_add(1, Relaxed);
                    }
                }
                st.busy = false;
                st.busy_since = None;
                w.busy.store(false, Relaxed);
            } else {
                st.awaiting = st.awaiting.saturating_sub(done.len() as u32);
            }
        }
        for (tx, bytes) in senders {
            let _ = tx.send(bytes.to_vec());
        }
        w.flush();
    }
}

/// kind | ndone | ndone x (rid len bytes) | npend | npend x rid | nfr | nfr x (rid, 13 x u32)   ->  (kind, [(rid, bytes)], npend);
/// the frame results are appended to the list of answers (their bytes are the 52 bytes of the 13 u32)
pub const FRAME_BYTES: usize = 13 * 4;

fn parse_result(m: &[u8]) -> Option<(u8, Vec<(u32, &[u8])>, u32)> {
    let u32at = |p: usize| m.get(p..p + 4).map(|b| u32::from_le_bytes(b.try_into().unwrap()));
    let kind = *m.first()?;
    let n = u32at(1)? as usize;
    let mut p = 5;
    let mut done = Vec::with_capacity(n.min(4096));
    for _ in 0..n {
        let rid = u32at(p)?;
        let len = u32at(p + 4)? as usize;
        p += 8;
        done.push((rid, m.get(p..p.checked_add(len)?)?));
        p += len;
    }
    let npend = u32at(p)?;
    p = p.checked_add(4)?.checked_add((npend as usize).checked_mul(4)?)?;
    let nfr = u32at(p)? as usize;
    p += 4;
    for _ in 0..nfr {
        let rid = u32at(p)?;
        done.push((rid, m.get(p + 4..p + 4 + FRAME_BYTES)?));
        p += 4 + FRAME_BYTES;
    }
    Some((kind, done, npend))
}

impl Worker {
    /// Sends the next message, unless one is already in flight. What goes in it:
    ///  * a light worker: up to `light_batch` light requests,
    ///  * a heavy worker not holding for the database round trip of its pending requests: up to `heavy_batch` heavy
    ///    requests; with no heavy work available (or no light pool at all) it takes up to `steal_batch` light requests instead.
    pub fn flush(self: &Arc<Self>) {
        let cfg = &self.sh.cfg;
        let mut adds: Vec<Entry> = Vec::new();
        let (tx, heavy_n, reqs) = {
            let mut st = self.st.lock().unwrap();
            if !st.ready || st.busy {
                return;
            }
            let mut heavy_n = 0;
            match self.class {
                Class::Light => self.sh.pull(Class::Light, cfg.light_batch, &mut adds),
                Class::Heavy => {
                    let holding = st.awaiting > 0 && st.hold_until.is_some_and(|t| Instant::now() < t);
                    if st.awaiting > 0 && !holding {
                        st.awaiting = 0;
                    }
                    if !holding {
                        self.sh.pull(Class::Heavy, cfg.heavy_batch, &mut adds);
                        heavy_n = adds.len();
                    }
                    if adds.is_empty() || cfg.light == 0 {
                        let before = adds.len();
                        let room = if cfg.light == 0 { cfg.light_batch } else { cfg.steal_batch };
                        self.sh.pull(Class::Light, before + room, &mut adds);
                        if adds.len() > before {
                            self.sh.stolen.fetch_add((adds.len() - before) as u64, Relaxed);
                        }
                    }
                    if holding && adds.is_empty() && !st.timer {
                        // nothing to send now; come back when the hold ends
                        st.timer = true;
                        let w = self.clone();
                        let d = st.hold_until.unwrap().saturating_duration_since(Instant::now());
                        tokio::spawn(async move {
                            tokio::time::sleep(d).await;
                            {
                                let mut st = w.st.lock().unwrap();
                                st.timer = false;
                                st.awaiting = 0;
                            }
                            w.flush();
                        });
                    }
                }
            }
            if adds.is_empty() {
                return;
            }
            st.busy = true;
            st.busy_since = Some(Instant::now());
            st.busy_heavy = heavy_n > 0;
            self.busy.store(true, Relaxed);
            // claim the requests for this worker and register their slots (under its lock, so a restart voids them cleanly)
            for e in adds.iter() {
                *e.tk.at.lock().unwrap() = Some((self.clone(), st.epoch));
            }
            self.inflight.fetch_add(adds.len(), Relaxed);
            let mut reqs = Vec::with_capacity(adds.len());
            for e in adds {
                st.pending.insert(e.rid, e.tx);
                reqs.push((e.rid, e.kind, e.html));
            }
            (st.tx.clone().unwrap(), heavy_n, reqs)
        };
        let mut m = Vec::with_capacity(HEADROOM + reqs.iter().map(|e| e.2.len() + 11).sum::<usize>() + 96);
        m.resize(HEADROOM, 0);
        m.extend_from_slice(b"{\"add\":\"");
        for (_, _, h) in reqs.iter() {
            m.extend_from_slice(h);
        }
        let list = |m: &mut Vec<u8>, frames: bool| {
            let mut first = true;
            for (r, _, _) in reqs.iter().filter(|e| (e.1 != Kind::Request) == frames) {
                if !first {
                    m.push(b',');
                }
                first = false;
                push_num(m, *r as u64);
            }
        };
        m.extend_from_slice(b"\",\"new\":[");
        list(&mut m, false);
        m.extend_from_slice(b"],\"frames\":[");
        list(&mut m, true);
        m.extend_from_slice(b"],\"gc\":");
        push_num(&mut m, heavy_n as u64);
        m.extend_from_slice(b",\"w\":");
        push_num(&mut m, reqs.iter().map(|e| weight(e.1)).sum::<usize>() as u64);
        m.push(b'}');
        let s = &self.sh;
        s.batches[self.class as usize].fetch_add(1, Relaxed);
        s.reqs[self.class as usize].fetch_add(reqs.len() as u64, Relaxed);
        if heavy_n > 0 {
            s.gcs.fetch_add(1, Relaxed);
        }
        let _ = tx.send(Out::Text(m));
    }
}

/// Appends `s` as a JSON string literal (config values only: user, password, database).
fn json_str(o: &mut Vec<u8>, s: &str) {
    o.push(b'"');
    for c in s.chars() {
        match c {
            '"' => o.extend_from_slice(b"\\\""),
            '\\' => o.extend_from_slice(b"\\\\"),
            c if (c as u32) < 0x20 => o.extend_from_slice(format!("\\u{:04x}", c as u32).as_bytes()),
            c => {
                let mut t = [0u8; 4];
                o.extend_from_slice(c.encode_utf8(&mut t).as_bytes());
            }
        }
    }
    o.push(b'"');
}

/// The `init` message: root inline style (JWT key bytes, start time), chunk sizes and GC policy for the glue, and the
/// Postgres settings (taken from DATABASE_URL) for the page's own client.
pub fn init_message(cfg: &Cfg, idx: usize) -> Vec<u8> {
    let mut m = Vec::new();
    m.resize(HEADROOM, 0);
    m.extend_from_slice(b"{\"init\":{\"style\":\"--klen:");
    push_num(&mut m, cfg.jwt_key.len() as u64);
    m.push(b';');
    for (i, b) in cfg.jwt_key.iter().enumerate() {
        m.extend_from_slice(b"--k");
        push_num(&mut m, i as u64);
        m.push(b':');
        push_num(&mut m, *b as u64);
        m.push(b';');
    }
    m.extend_from_slice(b"--start:"); // unix seconds; uptime = now - start (both exact integer literals)
    push_num(&mut m, cfg.start_ms / 1000);
    m.push(b';');
    m.extend_from_slice(b"\",\"auth_chunk\":");
    push_num(&mut m, cfg.k.auth_chunk as u64);
    m.extend_from_slice(b",\"body_chunk\":");
    push_num(&mut m, cfg.k.body_chunk as u64);
    m.extend_from_slice(b",\"gc_every\":");
    push_num(&mut m, cfg.gc_every as u64);
    m.extend_from_slice(b",\"gc_light_every\":");
    push_num(&mut m, cfg.gc_light_every as u64);
    m.extend_from_slice(b",\"pg\":{\"user\":");
    json_str(&mut m, &cfg.db.user);
    m.extend_from_slice(b",\"password\":");
    json_str(&mut m, &cfg.db.password);
    m.extend_from_slice(b",\"database\":");
    json_str(&mut m, &cfg.db.database);
    m.extend_from_slice(b",\"conns\":");
    push_num(&mut m, cfg.pg_conns_of(idx) as u64);
    m.extend_from_slice(b"}}}");
    m
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn page_messages() {
        // kind 0, two done (rid 7: "hi", rid 9: ""), two pending (11, 12)
        let mut m = vec![0u8];
        m.extend_from_slice(&2u32.to_le_bytes());
        m.extend_from_slice(&7u32.to_le_bytes());
        m.extend_from_slice(&2u32.to_le_bytes());
        m.extend_from_slice(b"hi");
        m.extend_from_slice(&9u32.to_le_bytes());
        m.extend_from_slice(&0u32.to_le_bytes());
        m.extend_from_slice(&2u32.to_le_bytes());
        m.extend_from_slice(&11u32.to_le_bytes());
        m.extend_from_slice(&12u32.to_le_bytes());
        // one frame result (rid 13, 13 x u32 = 1, 0, 2, ...)
        m.extend_from_slice(&1u32.to_le_bytes());
        m.extend_from_slice(&13u32.to_le_bytes());
        for i in 0..13u32 {
            m.extend_from_slice(&i.to_le_bytes());
        }
        let (kind, done, pend) = parse_result(&m).unwrap();
        assert_eq!((kind, pend), (0, 2));
        assert_eq!(done.len(), 3);
        assert_eq!(done[..2], vec![(7, &b"hi"[..]), (9, &b""[..])]);
        assert_eq!((done[2].0, done[2].1.len(), done[2].1[4..8].to_vec()), (13, FRAME_BYTES, 1u32.to_le_bytes().to_vec()));
        assert!(parse_result(&m[..m.len() - 9]).is_none()); // truncated
        assert!(parse_result(&[]).is_none());
        // a length that points past the end must not panic or overflow
        let bad = [1u8, 1, 0, 0, 0, 1, 0, 0, 0, 255, 255, 255, 255];
        assert!(parse_result(&bad).is_none());
    }
    #[test]
    fn init_json_strings() {
        let mut o = Vec::new();
        json_str(&mut o, "a\"b\\c\u{1}\u{e9}");
        assert_eq!(String::from_utf8(o).unwrap(), "\"a\\\"b\\\\c\\u0001\u{e9}\"");
    }
}
