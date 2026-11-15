use std::cell::{Cell, RefCell};
use std::collections::VecDeque;
use std::future::poll_fn;
use std::pin::Pin;
use std::rc::Rc;
use std::task::{Context, Poll, Waker};
use std::time::Duration;
use tokio::io::{AsyncRead, ReadBuf};
use tokio::net::TcpStream;

use crate::{Config, Worker, http, json, pg, uring};

const READ_BATCH_MAX: usize = 64;
const WRITE_BATCH_MAX: usize = 256;
const MAX_TRIES: u8 = 3;

pub enum Query {
    Feed,
    Post(i64),
    Health,
    Create {
        user_id: i64,
        username: Box<str>,
        body: Box<str>,
    },
    Like {
        user_id: i64,
        post_id: i64,
    },
}

pub enum Waiter {
    Task(Rc<Reply>),
    Conn(uring::Target),
}

impl Waiter {
    fn resolve(&self, w: &'static Worker, status: u16, body: Vec<u8>) {
        match self {
            Waiter::Task(r) => r.resolve(status, body),
            Waiter::Conn(t) => uring::db_reply(w, *t, status, body),
        }
    }
}

#[derive(Default)]
pub struct Reply {
    done: Cell<bool>,
    status: Cell<u16>,
    body: Cell<Vec<u8>>,
    waker: Cell<Option<Waker>>,
}

impl Reply {
    fn resolve(&self, status: u16, body: Vec<u8>) {
        self.status.set(status);
        self.body.set(body);
        self.done.set(true);
        if let Some(w) = self.waker.take() {
            w.wake();
        }
    }
}

struct Op {
    q: Query,
    to: Waiter,
    tries: u8,
}

pub struct Db {
    reads: Vec<Lane>,
    writes: Vec<Lane>,
    read_queue: RefCell<Vec<Op>>,
    write_queue: RefCell<VecDeque<Op>>,
    flush_pending: Cell<bool>,
    flusher: Cell<Option<Waker>>,
    last_error: RefCell<String>,
}

struct Lane {
    write: bool,
    alive: Cell<bool>,
    load: Cell<usize>,
    st: RefCell<LaneState>,
    waker: Cell<Option<Waker>>,
}

#[derive(Default)]
struct LaneState {
    out: Vec<u8>,
    inflight: VecDeque<Batch>,
}

struct Batch {
    ops: Vec<Op>,
    next: usize,
    rows: usize,
    body: Option<Vec<u8>>,
    results: Vec<(u16, Vec<u8>)>,
    failed: Option<(usize, pg::Error)>,
}

impl Db {
    pub fn new(cfg: &Config) -> Db {
        let lane = |write| Lane {
            write,
            alive: Cell::new(false),
            load: Cell::new(0),
            st: RefCell::new(LaneState::default()),
            waker: Cell::new(None),
        };
        Db {
            reads: (0..cfg.read_conns).map(|_| lane(false)).collect(),
            writes: (0..cfg.write_conns).map(|_| lane(true)).collect(),
            read_queue: RefCell::new(Vec::new()),
            write_queue: RefCell::new(VecDeque::new()),
            flush_pending: Cell::new(false),
            flusher: Cell::new(None),
            last_error: RefCell::new("connecting".into()),
        }
    }

    fn schedule_flush(&self) {
        if !self.flush_pending.replace(true)
            && let Some(w) = self.flusher.take()
        {
            w.wake();
        }
    }
}

pub fn start(w: &'static Worker) {
    for lane in w.db.reads.iter().chain(&w.db.writes) {
        tokio::task::spawn_local(lane_task(w, lane));
    }
    tokio::task::spawn_local(flusher(w));
}

pub fn enqueue(w: &Worker, q: Query, to: Waiter) {
    let op = Op { q, to, tries: 0 };
    match op.q {
        Query::Create { .. } | Query::Like { .. } => w.db.write_queue.borrow_mut().push_back(op),
        _ => w.db.read_queue.borrow_mut().push(op),
    }
    w.db.schedule_flush();
}

pub async fn submit(w: &'static Worker, q: Query) -> (u16, Vec<u8>) {
    let reply = Rc::new(Reply::default());
    enqueue(w, q, Waiter::Task(reply.clone()));
    poll_fn(|cx| {
        if reply.done.get() {
            return Poll::Ready((reply.status.get(), reply.body.take()));
        }
        set_waker(&reply.waker, cx);
        Poll::Pending
    })
    .await
}

pub fn set_waker(slot: &Cell<Option<Waker>>, cx: &Context) {
    match slot.take() {
        Some(w) if w.will_wake(cx.waker()) => slot.set(Some(w)),
        _ => slot.set(Some(cx.waker().clone())),
    }
}

async fn flusher(w: &'static Worker) {
    loop {
        poll_fn(|cx| {
            if w.db.flush_pending.get() {
                return Poll::Ready(());
            }
            set_waker(&w.db.flusher, cx);
            Poll::Pending
        })
        .await;
        w.db.flush_pending.set(false);
        flush(w);
    }
}

fn flush(w: &'static Worker) {
    let db = &w.db;
    let reads = std::mem::take(&mut *db.read_queue.borrow_mut());
    let mut reads = reads.into_iter();
    while reads.len() > 0 {
        let Some(lane) = db
            .reads
            .iter()
            .filter(|l| l.alive.get())
            .min_by_key(|l| l.load.get())
        else {
            let err = db.last_error.borrow().clone();
            reads.for_each(|op| fail(w, op, &err));
            break;
        };
        lane.send(reads.by_ref().take(READ_BATCH_MAX).collect());
    }

    if db.write_queue.borrow().is_empty() {
        return;
    }
    let mut alive = false;
    for lane in db.writes.iter().filter(|l| l.alive.get()) {
        alive = true;
        let mut q = db.write_queue.borrow_mut();
        if lane.load.get() == 0 && !q.is_empty() {
            let n = q.len().min(WRITE_BATCH_MAX);
            lane.send(q.drain(..n).collect());
        }
    }
    if !alive {
        let err = db.last_error.borrow().clone();
        let ops: Vec<Op> = db.write_queue.borrow_mut().drain(..).collect();
        ops.into_iter().for_each(|op| fail(w, op, &err));
    }
}

impl Lane {
    fn send(&self, ops: Vec<Op>) {
        let mut st = self.st.borrow_mut();
        for op in &ops {
            let out = &mut st.out;
            match &op.q {
                Query::Feed => pg::bind(out, pg::FEED, &[]),
                Query::Post(id) => pg::bind(out, pg::POST, &[&id.to_be_bytes()]),
                Query::Health => pg::bind(out, pg::HEALTH, &[]),
                Query::Create { user_id, body, .. } => {
                    pg::bind(out, pg::CREATE, &[&user_id.to_be_bytes(), body.as_bytes()])
                }
                Query::Like { user_id, post_id } => pg::bind(
                    out,
                    pg::LIKE,
                    &[&user_id.to_be_bytes(), &post_id.to_be_bytes()],
                ),
            }
            pg::execute(out);
        }
        pg::sync(&mut st.out);
        self.load.set(self.load.get() + ops.len());
        st.inflight.push_back(Batch {
            ops,
            next: 0,
            rows: 0,
            body: None,
            results: Vec::new(),
            failed: None,
        });
        drop(st);
        if let Some(w) = self.waker.take() {
            w.wake();
        }
    }
}

async fn lane_task(w: &'static Worker, lane: &'static Lane) {
    let mut logged = String::new();
    loop {
        match pg::connect(&w.cfg.pg).await {
            Ok(stream) => {
                lane.alive.set(true);
                logged.clear();
                let fd = std::os::fd::AsRawFd::as_raw_fd(&stream);
                if let Some(r) = w.ring {
                    r.watch(fd);
                }
                w.db.schedule_flush();
                let e = run(w, lane, stream).await;
                if let Some(r) = w.ring {
                    r.unwatch(fd);
                }
                lane.alive.set(false);
                eprintln!("postgres connection lost: {e}");
                lane_lost(w, lane);
                *w.db.last_error.borrow_mut() = e;
            }
            Err(e) => {
                if e != logged {
                    eprintln!("postgres: {e}");
                    logged = e.clone();
                }
                *w.db.last_error.borrow_mut() = e;
            }
        }
        tokio::time::sleep(Duration::from_secs(1)).await;
    }
}

async fn run(w: &'static Worker, lane: &'static Lane, mut stream: TcpStream) -> String {
    let mut rbuf: Vec<u8> = Vec::with_capacity(256 * 1024);
    let mut pos = 0;
    poll_fn(move |cx| {
        {
            let mut st = lane.st.borrow_mut();
            if !st.out.is_empty() {
                match http::write_some(&mut stream, cx, &st.out) {
                    Ok(n) => drop(st.out.drain(..n)),
                    Err(e) => return Poll::Ready(e.to_string()),
                }
            }
        }
        loop {
            if rbuf.capacity() - rbuf.len() < 64 * 1024 {
                rbuf.drain(..pos);
                pos = 0;
                rbuf.reserve(64 * 1024);
            }
            let mut rb = ReadBuf::uninit(rbuf.spare_capacity_mut());
            match Pin::new(&mut stream).poll_read(cx, &mut rb) {
                Poll::Pending => break,
                Poll::Ready(Err(e)) => return Poll::Ready(e.to_string()),
                Poll::Ready(Ok(())) => {
                    let n = rb.filled().len();
                    if n == 0 {
                        return Poll::Ready("server closed the connection".into());
                    }
                    unsafe { rbuf.set_len(rbuf.len() + n) };
                    match dispatch_all(w, lane, &rbuf[pos..]) {
                        Ok(used) => pos += used,
                        Err(e) => return Poll::Ready(e),
                    }
                    if pos == rbuf.len() {
                        rbuf.clear();
                        pos = 0;
                    }
                }
            }
        }
        set_waker(&lane.waker, cx);
        Poll::Pending
    })
    .await
}

fn dispatch_all(w: &'static Worker, lane: &Lane, buf: &[u8]) -> Result<usize, String> {
    let mut at = 0;
    let mut st = lane.st.borrow_mut();
    while buf.len() - at >= 5 {
        let len = u32::from_be_bytes(buf[at + 1..at + 5].try_into().unwrap()) as usize;
        if len < 4 {
            return Err("bad message length".into());
        }
        if buf.len() - at < 1 + len {
            break;
        }
        dispatch(w, lane, &mut st, buf[at], &buf[at + 5..at + 1 + len])?;
        at += 1 + len;
    }
    Ok(at)
}

fn dispatch(
    w: &'static Worker,
    lane: &Lane,
    st: &mut LaneState,
    ty: u8,
    msg: &[u8],
) -> Result<(), String> {
    match ty {
        b'D' => row(w, st.inflight.front_mut().ok_or("unexpected DataRow")?, msg),
        b'C' => complete(
            w,
            lane,
            st.inflight
                .front_mut()
                .ok_or("unexpected CommandComplete")?,
            msg,
        ),
        b'E' => match st.inflight.front_mut() {
            Some(b) => {
                b.failed.get_or_insert((b.next, pg::Error::parse(msg)));
                Ok(())
            }
            None => Err(pg::Error::parse(msg).message),
        },
        b'Z' => {
            let b = st.inflight.pop_front().ok_or("unexpected ReadyForQuery")?;
            finish(w, lane, b);
            Ok(())
        }
        b'2' | b'N' | b'S' | b'A' => Ok(()),
        _ => Err(format!("unexpected message '{}'", ty as char)),
    }
}

fn row(w: &Worker, b: &mut Batch, msg: &[u8]) -> Result<(), String> {
    let op = b.ops.get(b.next).ok_or("DataRow past the batch")?;
    let bad = || "bad row".to_string();
    match &op.q {
        Query::Feed | Query::Post(_) => {
            let [id, text, created_at, author, likes] = pg::columns::<5>(msg).ok_or_else(bad)?;
            let (id, created_at) = (
                pg::int8(id).ok_or_else(bad)?,
                pg::int8(created_at).ok_or_else(bad)?,
            );
            let likes = pg::int4(likes).ok_or_else(bad)?;
            let out = b.body.get_or_insert_with(|| w.take_buf());
            let feed = matches!(op.q, Query::Feed);
            out.extend_from_slice(match (feed, b.rows) {
                (true, 0) => b"{\"posts\":[",
                (true, _) => b",",
                (false, _) => b"{\"post\":",
            });
            json::post(out, id, text, created_at, author, likes as i64);
            if !feed {
                out.push(b'}');
            }
        }
        Query::Create { username, body, .. } => {
            let [id, created_at] = pg::columns::<2>(msg).ok_or_else(bad)?;
            let mut out = w.take_buf();
            out.extend_from_slice(b"{\"post\":");
            let (id, created_at) = (
                pg::int8(id).ok_or_else(bad)?,
                pg::int8(created_at).ok_or_else(bad)?,
            );
            json::post(
                &mut out,
                id,
                body.as_bytes(),
                created_at,
                username.as_bytes(),
                0,
            );
            out.push(b'}');
            b.body = Some(out);
        }
        Query::Health => {}
        Query::Like { .. } => return Err("unexpected row".into()),
    }
    b.rows += 1;
    Ok(())
}

fn complete(w: &'static Worker, lane: &Lane, b: &mut Batch, tag: &[u8]) -> Result<(), String> {
    let op = b.ops.get(b.next).ok_or("CommandComplete past the batch")?;
    let answer = match &op.q {
        Query::Feed => {
            let mut out = b.body.take().unwrap_or_else(|| w.take_buf());
            if b.rows == 0 {
                out.extend_from_slice(b"{\"posts\":[");
            }
            out.extend_from_slice(b"]}");
            (200, out)
        }
        Query::Post(_) => match b.body.take() {
            Some(out) => (200, out),
            None => (404, static_body(w, json::POST_NOT_FOUND)),
        },
        Query::Health => {
            let mut out = w.take_buf();
            json::health(&mut out, w.cfg.started.elapsed().as_secs());
            (200, out)
        }
        Query::Create { .. } => (201, b.body.take().ok_or("INSERT returned no row")?),
        Query::Like { post_id, .. } => {
            let already = tag.strip_suffix(b"\0").unwrap_or(tag).ends_with(b" 0");
            let mut out = w.take_buf();
            json::like(&mut out, already, *post_id);
            (if already { 200 } else { 201 }, out)
        }
    };
    b.next += 1;
    b.rows = 0;
    if lane.write {
        b.results.push(answer);
    } else {
        op.to.resolve(w, answer.0, answer.1);
        lane.load.set(lane.load.get() - 1);
    }
    Ok(())
}

fn finish(w: &'static Worker, lane: &Lane, b: Batch) {
    let Batch {
        ops,
        next,
        body,
        results,
        failed,
        ..
    } = b;
    if let Some(body) = body {
        w.give_buf(body);
    }
    if !lane.write {
        lane.load.set(lane.load.get() - (ops.len() - next));
        if let Some((k, err)) = failed {
            let mut ops = ops.into_iter().skip(k);
            if let Some(op) = ops.next() {
                eprintln!("postgres read failed: {}", err.message);
                fail(w, op, &err.message);
            }
            let mut q = w.db.read_queue.borrow_mut();
            q.extend(ops);
            if !q.is_empty() {
                w.db.schedule_flush();
            }
        }
        return;
    }

    lane.load.set(lane.load.get() - ops.len());
    match failed {
        None => {
            for (op, (status, body)) in ops.into_iter().zip(results) {
                op.to.resolve(w, status, body);
            }
        }
        Some((k, err)) => {
            results.into_iter().for_each(|(_, body)| w.give_buf(body));
            let mut retry = Vec::new();
            for (i, mut op) in ops.into_iter().enumerate() {
                if i == k {
                    if &err.code == b"23503" && matches!(op.q, Query::Like { .. }) {
                        op.to.resolve(w, 404, static_body(w, json::POST_NOT_FOUND));
                        continue;
                    }
                    if !matches!(&err.code, b"40001" | b"40P01") {
                        eprintln!("postgres write failed: {}", err.message);
                        fail(w, op, &err.message);
                        continue;
                    }
                }
                op.tries += 1;
                if op.tries > MAX_TRIES {
                    fail(w, op, &err.message);
                } else {
                    retry.push(op);
                }
            }
            let mut q = w.db.write_queue.borrow_mut();
            for op in retry.into_iter().rev() {
                q.push_front(op);
            }
        }
    }
    w.db.schedule_flush();
}

fn lane_lost(w: &'static Worker, lane: &Lane) {
    let mut st = lane.st.borrow_mut();
    st.out.clear();
    let batches = std::mem::take(&mut st.inflight);
    drop(st);
    lane.load.set(0);
    for b in batches {
        if let Some(body) = b.body {
            w.give_buf(body);
        }
        b.results.into_iter().for_each(|(_, body)| w.give_buf(body));
        if lane.write {
            b.ops
                .into_iter()
                .for_each(|op| fail(w, op, "connection lost"));
        } else {
            w.db.read_queue
                .borrow_mut()
                .extend(b.ops.into_iter().skip(b.next));
        }
    }
    w.db.schedule_flush();
}

fn fail(w: &'static Worker, op: Op, err: &str) {
    let mut out = w.take_buf();
    if matches!(op.q, Query::Health) {
        json::degraded(&mut out, err);
        op.to.resolve(w, 503, out);
    } else {
        out.extend_from_slice(json::INTERNAL);
        op.to.resolve(w, 500, out);
    }
}

fn static_body(w: &Worker, body: &[u8]) -> Vec<u8> {
    let mut out = w.take_buf();
    out.extend_from_slice(body);
    out
}
