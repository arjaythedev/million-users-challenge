# The Million Users Challenge

Can one **16-core server** hold **a million users**, and how far past it can you go? **[See the leaderboard](https://benchmarks.arjaythedev.com/million-users)**

I benchmarked C#, Node, Java, Rust, Python and Go, each with its most common framework set up the normal way, on
1 to 16 cores, with Postgres on its own machine. The best of them held 665,000 users on 16 cores. None reached a million.

**Now it's your turn: implement the same API in any language and framework, make it as fast as you can, and I'll
benchmark it on the same boxes: how many users it holds on 16 cores, on 8 and on 4.**

The boxes are [Synteq](https://synteq.com/arjay) VPSs. To test on the same hardware, sign up through
[synteq.com/arjay](https://synteq.com/arjay) and you get **$20 of free credit**.

## What's here

| | |
|---|---|
| [`SPEC.md`](SPEC.md) | The API you implement: 5 endpoints, exact JSON and errors, and the SQL you run |
| [`schema.sql`](schema.sql) | The Postgres schema (fixed) |
| [`seed/make-seed.sh`](seed/make-seed.sh) | Builds the seed database `feed_seed` (50k users, 500k posts, 2M likes) and `seed/tokens.json` |
| [`test/test.sh`](test/test.sh) | The test suite (42 checks). Your server must pass all of them |
| [`test/run.sh`](test/run.sh) | Builds your submission, starts it on a fresh database copy and runs the tests (what CI runs) |
| [`bench/`](bench) | The load generator I score with, and how to load test your own server |
| [`box/`](box) | How both boxes are set up: kernel limits, the systemd unit your server runs in, the Postgres config |
| [`examples/`](examples) | The six servers from the video (C#, Node, Java, Rust, Python, Go), with their 16-core scores. Copy one as a starting point |

## Quick start

```bash
bash seed/make-seed.sh                          # needs node >= 18, psql and a Postgres >= 16; about 30 seconds
mkdir submissions/rust-axum-yourname            # your code + install.sh, build.sh, start.sh, README.md
bash test/run.sh submissions/rust-axum-yourname # needs curl, jq, openssl
```

To load test it yourself, see [`bench/README.md`](bench/README.md#load-test-your-own-server).

## Rules

**The stack**

1. Any language, framework, runtime and Postgres driver. Any number of processes or threads.

2. **The database is PostgreSQL 18 on its own machine**, at `$DATABASE_URL`. No other database, cache server or
   external service (no Redis, Memcached, SQLite). Postgres's config is fixed ([`box/postgresql.conf`](box/postgresql.conf)).
   Session-level `SET` on your own connections is fine; server-wide changes (`ALTER SYSTEM`,
   `ALTER DATABASE/ROLE ... SET`) are not.

3. **Treat the database machine as never the bottleneck.** It has 16 cores to itself, and if a run is limited by
   the database box rather than your server, I give it more resources and run again. In return, the work stays in
   your server: you run the queries in [SPEC.md](SPEC.md#endpoints), one per request. Preparing them, pipelining
   them, and batching several requests' writes into one statement or transaction are fine. Moving work into
   Postgres is not (building the JSON in SQL, extra queries, functions).

4. The schema and data are fixed. Don't create, alter or drop anything in the database: no tables, columns,
   indexes, views, functions, triggers or extensions.

5. At most **512 open connections** to Postgres in total.

**Correctness**

6. Pass the test suite 100% (`test/run.sh`).

7. **No caching across requests.** Every request reads its data from Postgres while it is being served: no
   response caches, query-result caches, in-memory copies of tables, and no remembered JWT verifications. Connection
   pools and prepared statements are fine: they cache connections and query plans, not data.

8. **Writes are committed before you respond.** No `synchronous_commit = off`, no unlogged tables. Group commit is
   allowed if each response waits for its commit.

9. Every token's HS256 signature and `exp` are checked on every request. No hard-coded responses, no detecting the
   load test, nothing that only works because it's a benchmark.

**The box**

10. Your server runs on Ubuntu 24.04 x86_64 with **16 vCPUs and 32 GB RAM**, alone (Postgres is on the other box).
    If you run out of memory, you lose. If your server crashes, it isn't restarted.

11. **No Nginx: you serve the internet directly**, on ports 80 and 81 at the same time (see
    [SPEC.md](SPEC.md#runtime-contract)). At a million users that's a million open keep-alive connections, and the search goes past that.

12. It is built from source on the box with your `install.sh` and `build.sh`. Pin your dependency versions,
    and don't ship prebuilt binaries. It must answer `GET /health` within 60 seconds of `start.sh`.

13. Nothing on the box gets tuned for you beyond [`box/setup-app-box.sh`](box/setup-app-box.sh): no kernel
    parameters, and no pinning of CPU or other processes. Settings inside your own process (GC, thread counts,
    allocator, pool sizes) are fair game.

## Submitting

Open a pull request that adds one folder, `submissions/<language>-<framework>-<github-username>/`, containing:

| File | What it does |
|---|---|
| `install.sh` | Runs once as root on a clean Ubuntu 24.04: installs your toolchain/runtime (apt, official tarballs) |
| `build.sh` | Builds your app as a normal user (may download pinned dependencies) |
| `start.sh` | Runs the server **in the foreground**, configured only by the env vars in [SPEC.md](SPEC.md#runtime-contract) |
| `README.md` | Language, framework, driver and versions; the optimizations you made and why |
| your code | Source only: don't commit build output (`bin/`, `target/`, `node_modules/`, …). Add a `.gitignore` |

The [examples](examples) show all three scripts for six languages.

CI runs `install.sh` and `test/run.sh` on every pull request, and it must be green. Don't touch files outside your folder.
By submitting, you license your code under the repo's [MIT license](LICENSE).

## Scoring

I benchmark each passing submission like the video: your server on the 16-core app box, Postgres on the database
box (a fresh copy of the seed for every run), and the load generator ([`bench/`](bench)) on separate machines. A
simulated user browses the feed with think time: about one page every 24 seconds, so a million users is about
42,000 requests per second.

1. **Find the limit**: 2-minute runs. After each passing run, the next one goes to where your server's CPU says it
   would be at about 85% (at least 25% more users, at most 4×), until one fails; then narrowing down to within 5%.

2. **Confirm it**: one 5-minute hold at that number. If it fails, step down 5% and try again.

Every run starts your server fresh on a fresh copy of the database. A run passes with p95 under 500 ms, p99 under
1 s and under 1% errors.

**You get three scores: the most users held for 5 minutes on 16 cores, on 8 cores and on 4 cores.** On 8 and 4 cores
your server, the kernel's network processing and all, gets only those cores. The leaderboard ranks by 16 cores.

**16 cores is capped at 1,000,000 users**: past that, the one Postgres box (not your server) becomes the limit, so
holding a million on 16 cores is the top 16-core score, shown as `1,000,000 (max)`. **8 and 4 cores are searched up to
3,000,000 users**, with your server talking to the same single Postgres box. A score that reaches the search's
ceiling shows `(max)`.

A run where a load generator or the database box, not your server, is the limit doesn't count: I add capacity and
run it again.

**Pull requests are benchmarked as they come in**, oldest first, and the result goes on the
[leaderboard](LEADERBOARD.md) and as a comment on your PR.
