# Local benchmark

The setup behind the local numbers in [../README.md](../README.md). It runs on one machine with Docker, using the
challenge's own `bench/mload` and `test/run.sh`. The server, Postgres and the load generator each get their own
container and their own cores. It ran on an Apple Silicon Mac (arm64).

Use it to compare servers on the same machine. It doesn't predict scores on the benchmark boxes.

## Containers

| Container | Image | Cores | What runs there |
|---|---|---|---|
| `bapp` | `ubuntu:24.04` | 0 (1 core) or 0–1 (2 cores) | the server under test, with these scripts in `/bench` |
| `bload` | `golang:1.27` | 4–5 | `mload`, and `idle` for idle connections |
| `bpg` | `postgres:18` | 6–9 | Postgres with the seed database `feed_seed` |

- **Networks:** `bapp` sits on six Docker networks, so it has six IPs. A client IP can open only ~64,000 connections
  to one IP and port, and six IPs times two ports gives room for ~770,000.
- **seccomp:** `bapp` runs with Docker's seccomp profile off, because the profile blocks io_uring, which some of the
  other submissions use.

## Setup

From the repo root (the commands mount `$PWD`). The networks need free subnets: change `172.19` to `172.24` if
Docker already uses them.

```bash
docker network create --subnet 172.19.0.0/16 muc-bench
for i in 2 3 4 5 6; do docker network create --subnet 172.$((18 + i)).0.0/16 muc-bench$i; done

docker run -d --name bpg --network muc-bench --cpuset-cpus 6-9 --shm-size 1g -e POSTGRES_PASSWORD=postgres \
  postgres:18 -c max_connections=600 -c shared_buffers=2GB -c effective_cache_size=4GB \
  -c maintenance_work_mem=512MB -c random_page_cost=1.1 -c effective_io_concurrency=200 -c wal_buffers=64MB \
  -c wal_compression=lz4 -c checkpoint_timeout=15min -c checkpoint_completion_target=0.9 -c max_wal_size=8GB \
  -c min_wal_size=2GB -c jit=off -c enable_memoize=off

docker run -d --name bapp --platform linux/arm64 --security-opt seccomp=unconfined --cpuset-cpus 0 \
  --ulimit nofile=1048576:1048576 --sysctl net.core.somaxconn=65535 --sysctl net.ipv4.tcp_max_syn_backlog=65535 \
  --network muc-bench --ip 172.19.0.3 -v "$PWD":/src:ro ubuntu:24.04 sleep infinity
for i in 2 3 4 5 6; do docker network connect --ip 172.$((18 + i)).0.2 muc-bench$i bapp; done

docker run -d --name bload --network muc-bench --cpuset-cpus 4-5 --ulimit nofile=2097152:2097152 \
  --sysctl "net.ipv4.ip_local_port_range=1024 65535" --sysctl net.ipv4.tcp_tw_reuse=1 \
  -v "$PWD":/src:ro golang:1.27 sleep infinity
for i in 2 3 4 5 6; do docker network connect muc-bench$i bload; done

# bapp: tools, a copy of the repo in /repo, the seed, the scripts in /bench, this submission built
docker exec -e PGHOST=bpg -e PGUSER=postgres -e PGPASSWORD=postgres bapp \
  bash /src/submissions/nelua-epoll-tbsvttr/bench/setup-app.sh
# bload: the seed's tokens, mload and idle
docker cp bapp:/bench/tokens.json /tmp/tokens.json && docker cp /tmp/tokens.json bload:/tmp/tokens.json
docker exec -w /src/bench/mload -e CGO_ENABLED=0 bload go build -o /usr/local/bin/mload .
docker exec -w /src/submissions/nelua-epoll-tbsvttr/bench/idle bload go build -o /usr/local/bin/idle .
```

`/repo` in `bapp` is a copy made at setup. After changing a submission, copy it in again (`docker cp`) and run its
`build.sh`.

## Runs

`serve.sh` starts the server fresh on a fresh copy of the seed for every run. The CPU is the `bapp` container's
(cgroup), so it includes the kernel's network work for the server's sockets.

**1 core, 20,000 users, ~10,550 req/s.** This is a worker's share of a million users on 4 cores. The 1-core tables
and the packet counts come from this run.

```bash
docker update --cpuset-cpus 0 bapp
docker exec -e PGHOST=bpg -e PGUSER=postgres -e PGPASSWORD=postgres bapp \
  bash /bench/serve.sh submissions/nelua-epoll-tbsvttr
docker exec -d bapp bash -c 'sleep 30; bash /bench/counters.sh 30 > /tmp/counters.txt'
docker exec bload bash -c 'mload -target 172.19.0.3 -ports 3000,3001 -users 20000 -ramp 15s -hold 60s -gzip \
  -think-scale 0.2 -tokens /tmp/tokens.json -out /tmp/run.json > /tmp/run.log 2>&1;
  sh /src/submissions/nelua-epoll-tbsvttr/bench/holdrps.sh'
docker exec bapp cat /tmp/counters.txt
```

- **The window:** `counters.sh` measures seconds 30 to 60 of the run, which is inside the hold.
- **Per request:** divide its numbers by the hold's req/s from `holdrps.sh`.
- **The split:** run `segsplit.sh 30` in place of `counters.sh 30` to split the segments into Postgres and clients.

**2 cores, 125,000 users.** This is the box's million on 16 cores, scaled down.

```bash
docker update --cpuset-cpus 0-1 bapp
docker exec -e PGHOST=bpg -e PGUSER=postgres -e PGPASSWORD=postgres bapp \
  bash /bench/serve.sh submissions/nelua-epoll-tbsvttr
docker exec bload bash -c 'mload -target 172.19.0.3,172.20.0.2,172.21.0.2,172.22.0.2,172.23.0.2,172.24.0.2 \
  -ports 3000,3001 -users 125000 -ramp 40s -hold 180s -gzip -think-scale 2.5 -tokens /tmp/tokens.json \
  -out /tmp/run.json > /tmp/run.log 2>&1; sh /src/submissions/nelua-epoll-tbsvttr/bench/holdrps.sh'
docker exec -e PGHOST=bpg -e PGUSER=postgres -e PGPASSWORD=postgres bapp bash /bench/report.sh
```

`mload` needs several GB for this run, and its garbage collection shows up in the p99.

**1 core with 250,000 idle connections, plus the 1-core load.** This is a worker's connections at a million users on
4 cores. Docker's VM tracks every connection, and its default table is too small for this, so the first command
raises it.

```bash
docker run --rm --privileged --net=host alpine:3.22 sysctl -w net.netfilter.nf_conntrack_max=1048576
docker update --cpuset-cpus 0 bapp
docker exec -e PGHOST=bpg -e PGUSER=postgres -e PGPASSWORD=postgres bapp \
  bash /bench/serve.sh submissions/nelua-epoll-tbsvttr
docker exec -d bload bash -c 'for ip in 172.19.0.3 172.20.0.2 172.21.0.2 172.22.0.2 172.23.0.2 172.24.0.2; do
  for p in 3000 3001; do idle 20834 $ip:$p > /tmp/idle-$ip-$p.log 2>&1 & done; done; wait'
docker exec bload bash /src/submissions/nelua-epoll-tbsvttr/bench/idle/wait.sh
# then the counters.sh and mload commands of the 1-core run; afterwards:
docker exec bload pkill idle
```

**Other submissions.** Install and build them in `bapp`, check them with the test suite, then serve them.

```bash
docker exec bapp bash -c 'cd /repo/submissions/<folder> && bash install.sh && bash build.sh'
docker exec -e PGHOST=bpg -e PGUSER=postgres -e PGPASSWORD=postgres bapp bash /bench/serve.sh stop
docker exec -e PGHOST=bpg -e PGPORT=5432 -e PGUSER=postgres -e PGPASSWORD=postgres -w /repo bapp \
  bash test/run.sh submissions/<folder>
docker exec -e PGHOST=bpg -e PGUSER=postgres -e PGPASSWORD=postgres bapp bash /bench/serve.sh submissions/<folder>
```

- **Stop the benchmark server before the tests.** A server still on port 3000 shares the port (`SO_REUSEPORT`),
  answers some of the test's requests and serves them from the wrong database.
- **arm64 changes:** some submissions needed small changes to build or run here. The list is in
  [../README.md](../README.md#compared-with-other-submissions-same-local-setup).

## Scripts

| Script | Runs in | What it does |
|---|---|---|
| `setup-app.sh` | `bapp` | tools, the repo copy, the seed, the scripts in `/bench`, this submission built |
| `serve.sh FOLDER [VAR=value ...]` | `bapp` | stops the last server, starts `FOLDER` on a fresh copy of the seed, starts `sample.sh`. `serve.sh stop` only stops |
| `sample.sh` | `bapp` | every 5 s: CPU, the server's RSS (all processes of its session), TCP sockets |
| `report.sh` | `bapp` | after a run: peak and hold CPU and RSS from the samples, Postgres backends |
| `counters.sh SECONDS` | `bapp` | per second: the server's wake-ups, TCP segments in, out and retransmitted, CPU user and system |
| `segsplit.sh SECONDS` | `bapp` | TCP segments per second, split into Postgres and client sockets |
| `holdrps.sh [LOG]` | `bload` | mean req/s and the worst 5-second p95 and p99 over the hold, from `mload`'s output |
| `idle/` (`idle N HOST:PORT`) | `bload` | opens N connections, sends one `/health` on each, then holds them without reading |
| `idle/wait.sh [HOLDERS]` | `bload` | waits until the `idle` processes hold their connections |

## Limits

- **arm64, not the box's x86.** Some submissions have x86 fast paths that don't run here.
- **Docker's networking** costs more kernel time than a real NIC.
- **The Mac's cores vary.** The VM's cores land on faster or slower cores of the host, and other work on the Mac
  shows. Runs of the same server varied by up to ~30% on a busy Mac and by ~10% on a quiet one. Run servers in
  alternating order, more than once.
