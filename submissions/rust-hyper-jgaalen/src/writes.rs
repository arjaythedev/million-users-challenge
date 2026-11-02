use crate::pg::{Client, Row, Statement};
#[cfg(not(feature = "grouped-write-sync"))]
use futures_util::future::join_all;
use std::{
    cell::Cell,
    rc::Rc,
    time::{Duration, Instant},
};
use tokio::sync::{mpsc, oneshot};
pub enum Input {
    Post(i64, String),
    Like(i64, i64),
}
pub enum Output {
    Post(Row),
    Like(u64),
}
type Result = std::result::Result<Output, crate::pg::Error>;
struct Job {
    input: Input,
    reply: oneshot::Sender<Result>,
}
#[derive(Default)]
struct Metrics {
    groups: Cell<u64>,
    jobs: Cell<u64>,
    max_batch: Cell<usize>,
    rollbacks: Cell<u64>,
    transactions: Cell<u64>,
    begin_us: Cell<u64>,
    statements_us: Cell<u64>,
    commit_us: Cell<u64>,
}
pub struct Writer {
    tx: mpsc::Sender<Job>,
    metrics: Rc<Metrics>,
}
impl Writer {
    pub fn stage_metrics(&self) -> (u64, u64, u64, u64) {
        let m = &self.metrics;
        (
            m.transactions.get(),
            m.begin_us.get(),
            m.statements_us.get(),
            m.commit_us.get(),
        )
    }
    pub fn metrics(&self) -> (u64, u64, usize, u64) {
        (
            self.metrics.groups.get(),
            self.metrics.jobs.get(),
            self.metrics.max_batch.get(),
            self.metrics.rollbacks.get(),
        )
    }
    pub async fn send(&self, input: Input) -> Result {
        let (tx, rx) = oneshot::channel();
        self.tx
            .send(Job { input, reply: tx })
            .await
            .expect("writer running");
        rx.await.expect("writer response")
    }
    pub fn start(
        client: Client,
        post: Statement,
        like: Statement,
        size: usize,
        delay: Duration,
    ) -> Self {
        let client = std::rc::Rc::new(client);
        let (tx, mut rx) = mpsc::channel::<Job>(256);
        let metrics = Rc::new(Metrics::default());
        let stats = metrics.clone();
        tokio::task::spawn_local(async move {
            #[cfg(feature = "grouped-write-sync")]
            let (begin, commit) = (
                client.prepare("BEGIN").await.expect("prepare BEGIN"),
                client.prepare("COMMIT").await.expect("prepare COMMIT"),
            );
            while let Some(first) = rx.recv().await {
                if size == 1 {
                    stats.groups.set(stats.groups.get() + 1);
                    stats.jobs.set(stats.jobs.get() + 1);
                    stats.max_batch.set(stats.max_batch.get().max(1));
                    let c = client.clone();
                    let p = post.clone();
                    let l = like.clone();
                    tokio::task::spawn_local(async move {
                        let result = execute(&c, &p, &l, &first.input).await;
                        let _ = first.reply.send(result);
                    });
                    continue;
                }
                let mut jobs = vec![first];
                let deadline = tokio::time::Instant::now() + delay;
                while jobs.len() < size {
                    match rx.try_recv() {
                        Ok(j) => jobs.push(j),
                        Err(_) if delay.is_zero() => break,
                        Err(_) => match tokio::time::timeout_at(deadline, rx.recv()).await {
                            Ok(Some(j)) => jobs.push(j),
                            _ => break,
                        },
                    }
                }
                stats.groups.set(stats.groups.get() + 1);
                stats.jobs.set(stats.jobs.get() + jobs.len() as u64);
                stats.max_batch.set(stats.max_batch.get().max(jobs.len()));
                if jobs.len() == 1 {
                    let j = jobs.pop().unwrap();
                    let result = execute(&client, &post, &like, &j.input).await;
                    let _ = j.reply.send(result);
                    continue;
                }
                #[cfg(feature = "grouped-write-sync")]
                {
                    let params: Vec<Vec<&(dyn postgres_types::ToSql + Sync)>> = jobs
                        .iter()
                        .map(|j| match &j.input {
                            Input::Post(user, body) => vec![
                                user as &(dyn postgres_types::ToSql + Sync),
                                body as &(dyn postgres_types::ToSql + Sync),
                            ],
                            Input::Like(user, id) => vec![
                                user as &(dyn postgres_types::ToSql + Sync),
                                id as &(dyn postgres_types::ToSql + Sync),
                            ],
                        })
                        .collect();
                    let mut commands: Vec<(&Statement, &[&(dyn postgres_types::ToSql + Sync)])> =
                        vec![(&begin, &[])];
                    for (j, p) in jobs.iter().zip(&params) {
                        commands.push((
                            match j.input {
                                Input::Post(..) => &post,
                                Input::Like(..) => &like,
                            },
                            p,
                        ));
                    }
                    commands.push((&commit, &[]));
                    let started = Instant::now();
                    let result = match client.transaction_group(&commands).await {
                        Ok(result) => result,
                        Err(e) => {
                            eprintln!("group commit outcome unknown: {e}");
                            std::process::exit(1);
                        }
                    };
                    stats.transactions.set(stats.transactions.get() + 1);
                    stats
                        .statements_us
                        .set(stats.statements_us.get() + started.elapsed().as_micros() as u64);
                    if let Some((index, e)) = result.group_failure {
                        if index == 0 || index == jobs.len() + 1 {
                            eprintln!("transaction control failed at {index}: {e}");
                            std::process::exit(1);
                        }
                        stats.rollbacks.set(stats.rollbacks.get() + 1);
                        // Extended-protocol error skipped COMMIT. Replay only
                        // after confirmed rollback; never after transport failure.
                        if let Err(e) = client.batch_execute("ROLLBACK").await {
                            eprintln!("group rollback uncertain: {e}");
                            std::process::exit(1);
                        }
                        for j in jobs {
                            let result = execute(&client, &post, &like, &j.input).await;
                            let _ = j.reply.send(result);
                        }
                    } else {
                        assert_eq!(result.grouped.len(), jobs.len() + 2);
                        for (j, mut out) in jobs.into_iter().zip(result.grouped.into_iter().skip(1))
                        {
                            let value = match j.input {
                                Input::Post(..) => {
                                    assert_eq!(out.rows.len(), 1);
                                    Output::Post(out.rows.pop().unwrap())
                                }
                                Input::Like(..) => Output::Like(out.count),
                            };
                            let _ = j.reply.send(Ok(value));
                        }
                    }
                }
                #[cfg(not(feature = "grouped-write-sync"))]
                {
                    // All results stay private until COMMIT completes. Never replay an uncertain commit.
                    let began = Instant::now();
                    if let Err(e) = client.batch_execute("BEGIN").await {
                        eprintln!("batch begin: {e}");
                        std::process::exit(1);
                    }
                    let begun = Instant::now();
                    stats
                        .begin_us
                        .set(stats.begin_us.get() + (begun - began).as_micros() as u64);
                    stats.transactions.set(stats.transactions.get() + 1);
                    // Poll all statements together so tokio-postgres pipelines them. The
                    // connection is exclusive to this actor until COMMIT or ROLLBACK.
                    // join_all preserves input/result order and drains every queued query,
                    // including transaction-aborted errors, before rollback/replay.
                    let outputs = join_all(
                        jobs.iter()
                            .map(|j| execute(&client, &post, &like, &j.input)),
                    )
                    .await;
                    stats
                        .statements_us
                        .set(stats.statements_us.get() + begun.elapsed().as_micros() as u64);
                    let failed = outputs.iter().any(std::result::Result::is_err);
                    if failed {
                        stats.rollbacks.set(stats.rollbacks.get() + 1);
                        // Only replay after a confirmed rollback; each individual request gets its own error.
                        if let Err(e) = client.batch_execute("ROLLBACK").await {
                            eprintln!("batch rollback uncertain: {e}");
                            std::process::exit(1);
                        }
                        for j in jobs {
                            let result = execute(&client, &post, &like, &j.input).await;
                            let _ = j.reply.send(result);
                        }
                    } else {
                        let committing = Instant::now();
                        if let Err(e) = client.batch_execute("COMMIT").await {
                            eprintln!("batch commit uncertain: {e}");
                            std::process::exit(1);
                        }
                        stats
                            .commit_us
                            .set(stats.commit_us.get() + committing.elapsed().as_micros() as u64);
                        for (j, output) in jobs.into_iter().zip(outputs) {
                            let _ = j.reply.send(output);
                        }
                    }
                }
            }
        });
        Self { tx, metrics }
    }
}
async fn execute(c: &Client, post: &Statement, like: &Statement, input: &Input) -> Result {
    match input {
        Input::Post(id, body) => c.query_one(post, &[id, body]).await.map(Output::Post),
        Input::Like(user, id) => c.execute(like, &[user, id]).await.map(Output::Like),
    }
}
