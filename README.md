# The Million Users Challenge

Can one **16-core server** hold **a million users**? **[See the leaderboard](https://benchmarks.arjaythedev.com/million-users)**

I benchmarked C#, Node, Java, Rust, Python and Go, each with its most common framework set up the normal way, on
1 to 16 cores, with Postgres on its own machine. The best of them held 665,000 users on 16 cores. None reached a million.

**Now it's your turn: implement the same API in any language and framework, make it as fast as you can, and I'll
benchmark it on the same boxes. Get to a million on 16 cores, then try it on 8, and if you can, on 4.**

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


## All in one script setup

**setup:**  

```
nano setup.sh 
```

**Paste in then ctrl+o then enter then ctrl+x**

```
#!/usr/bin/env bash
set -euo pipefail

REPO="https://github.com/arjaythedev/million-users-challenge"
WORKDIR="${1:-./million-users-challenge}"

# ─── 1. Clone ────────────────────────────────────────────────────────────────
if [[ ! -d "$WORKDIR" ]]; then
  git clone "$REPO" "$WORKDIR"
fi
cd "$WORKDIR"

# ─── 2. Pick a language ─────────────────────────────────────────────────────
echo "Available examples:"
ls examples/
echo ""
read -rp "Language to set up [csharp|node|java|rust|python|go]: " LANG

case "$LANG" in
  csharp|node|java|rust|python|go) ;;
  *) echo "Unknown: $LANG"; exit 1 ;;
esac

# ─── 3. Common prerequisites (all languages) ────────────────────────────────
echo "=== Installing common prerequisites ==="
apt-get update
apt-get install -y \
  git curl jq openssl \
  build-essential \
  postgresql-client \
  libpq-dev

# ─── 4. Language-specific toolchain ─────────────────────────────────────────
case "$LANG" in

  csharp)
    echo "=== Installing .NET SDK 10 ==="
    DOTNET_VER="10.0.402"
    DOTNET_DIR="/usr/local/dotnet"
    curl -fsSL "https://builds.dotnet.microsoft.com/dotnet/Sdk/${DOTNET_VER}/dotnet-sdk-${DOTNET_VER}-linux-x64.tar.gz" \
      | tar -xz -C "$DOTNET_DIR" --strip-components=0
    ln -sf "$DOTNET_DIR/dotnet" /usr/local/bin/dotnet
    ;;

  node)
    echo "=== Installing Node.js 22 LTS ==="
    NODE_VER="22.11.0"
    curl -fsSL "https://nodejs.org/dist/v${NODE_VER}/node-v${NODE_VER}-linux-x64.tar.xz" \
      | tar -xJ -C /usr/local --strip-components=1
    ;;

  java)
    echo "=== Installing OpenJDK 21 ==="
    apt-get install -y openjdk-21-jdk
    ;;

  rust)
    echo "=== Installing Rust 1.99.0 via rustup ==="
    curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs \
      | sh -s -- -y --default-toolchain 1.99.0 --profile minimal
    # Make cargo/rustc available system-wide
    ln -sf /root/.cargo/bin/cargo   /usr/local/bin/cargo
    ln -sf /root/.cargo/bin/rustc   /usr/local/bin/rustc
    ln -sf /root/.cargo/bin/rustup  /usr/local/bin/rustup
    ;;

  python)
    echo "=== Installing Python 3.12 + pip ==="
    apt-get install -y python3 python3-pip python3-venv
    ;;

  go)
    echo "=== Installing Go 1.27 ==="
    GO_VER="1.27.0"
    curl -fsSL "https://go.dev/dl/go${GO_VER}.linux-amd64.tar.gz" \
      | tar -C /usr/local -xz
    ln -sf /usr/local/go/bin/go /usr/local/bin/go
    ln -sf /usr/local/go/bin/gofmt /usr/local/bin/gofmt
    ;;

esac

# ─── 5. Build the example ───────────────────────────────────────────────────
EXAMPLE_DIR="examples/${LANG}"
if [[ -f "$EXAMPLE_DIR/build.sh" ]]; then
  echo "=== Building $LANG example ==="
  bash "$EXAMPLE_DIR/build.sh"
fi

# ─── 6. Run tests (optional) ────────────────────────────────────────────────
read -rp "Run conformance tests? [y/N]: " RUN_TESTS
if [[ "${RUN_TESTS,,}" == "y" ]]; then
  read -rp "DATABASE_URL (e.g. postgres://user:pass@host:5432/conduit): " DB_URL
  export DATABASE_URL="$DB_URL"
  bash test/run.sh "$EXAMPLE_DIR"
fi

echo ""
echo "✅ Done. Start the server with:"
echo "   DATABASE_URL=... JWT_SECRET=... bash $EXAMPLE_DIR/start.sh"
```
**Usage:**


```
chmod +x setup.sh
sudo bash setup.sh          # run as root (install.sh requires it)
```


**Notes:**

Must run on a clean Ubuntu 24.04 x86_64 box (that's the challenge's target).
DATABASE_URL must point to a PostgreSQL 18 instance on a separate machine — the script won't install Postgres for you since the rules require it on its own box. 
The versions above (Go 1.27, Rust 1.99.0, .NET 10.0.402, Node 22) are pinned to match what the examples' install.sh files use; adjust if the repo updates them.
For your own submission, replace the build.sh step with your own submission folder path.



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
    [SPEC.md](SPEC.md#runtime-contract)). At a million users that's a million open keep-alive connections.

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

1. **Warm-up**: 3 minutes at a low load (not scored).

2. **Find the limit**: 2-minute runs, doubling the users until one fails, then narrowing down to within 5%.

3. **Confirm it**: one 5-minute hold at that number. If it fails, step down 5% and try again.

**Your score is the most users that pass a 5-minute hold on 16 cores**, with p95 under 500 ms, p99 under 1 s and
under 1% errors. **If you hold a million**, I run you again on 8 cores, then on 4: your server, the kernel's network
processing and all, gets only those cores. Fewest cores to hold a million wins.

A run where a load generator or the database box, not your server, is the limit doesn't count: I add capacity and
run it again.

**After your pull request is merged**, it waits for the next benchmark batch. I run merged submissions in batches,
not one at a time, on no fixed schedule, and share the results once a batch is done.
