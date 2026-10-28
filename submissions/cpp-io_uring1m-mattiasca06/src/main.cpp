#include <libpq-fe.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <sys/resource.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#ifdef USE_MIMALLOC
#include <mimalloc.h>
#endif

#include "auth.h"
#include "egress.h"
#include "worker.h"

static const char *env(const char *k, const char *def) {
    const char *v = getenv(k);
    return v && *v ? v : def;
}

// True if every host of the URL is this machine: a unix socket directory, localhost or a loopback address.
static bool pg_is_local(const char *url) {
    PQconninfoOption *o = PQconninfoParse(url, nullptr);
    if (!o) return false;
    std::string hosts;
    for (PQconninfoOption *p = o; p->keyword; p++)
        if (p->val && (!strcmp(p->keyword, "host") || !strcmp(p->keyword, "hostaddr"))) hosts += std::string(p->val) + ",";
    PQconninfoFree(o);
    if (hosts.empty()) {
        const char *e = getenv("PGHOST");
        hosts = e ? std::string(e) + "," : "";
    }
    if (hosts.empty()) return true;  // libpq's default: the local unix socket
    size_t pos = 0;
    while (pos < hosts.size()) {
        size_t end = hosts.find(',', pos);
        std::string h = hosts.substr(pos, end - pos);
        pos = end + 1;
        if (h.empty()) continue;
        if (!(h[0] == '/' || h == "localhost" || h == "::1" || h.rfind("127.", 0) == 0)) return false;
    }
    return true;
}

int main() {
#ifdef USE_MIMALLOC
    // Defaults only: MIMALLOC_* environment variables still win (used to A/B these).
    mi_option_set_default(mi_option_purge_delay, -1);            // never madvise freed pages back; steady-state RSS is bounded
    mi_option_set_default(mi_option_allow_large_os_pages, 1);    // 2 MB pages (THP) for arenas -> fewer TLB misses
    mi_option_set_default(mi_option_arena_eager_commit, 1);
#endif
    signal(SIGPIPE, SIG_IGN);
    rlimit rl;
    if (getrlimit(RLIMIT_NOFILE, &rl) == 0) {
        rl.rlim_cur = rl.rlim_max;
        setrlimit(RLIMIT_NOFILE, &rl);
    }

    Config cfg;
    cfg.host = env("HOST", "0.0.0.0");
    cfg.port = atoi(env("PORT", "80"));
    cfg.extra_port = atoi(env("EXTRA_PORT", "0"));
    cfg.database_url = env("DATABASE_URL", "postgres://postgres:postgres@127.0.0.1:5432/feed_seed");
    cfg.jwt_secret = env("JWT_SECRET", "million-users-challenge");
    {  // one worker per CPU this process may run on (the box is 16, 8 or 4 cores)
        cpu_set_t set;
        int n = sched_getaffinity(0, sizeof set, &set) == 0 ? CPU_COUNT(&set) : int(std::thread::hardware_concurrency());
        cfg.workers = atoi(env("WORKERS", std::to_string(std::max(1, n)).c_str()));
    }
    cfg.read_conns = atoi(env("DB_CONNS", "1"));  // read connections per worker at start
    // The pool grows on demand (queue delay, see Worker::control) up to this many; DB_CONNS_MAX=DB_CONNS freezes it.
    cfg.read_conns_max = std::max(cfg.read_conns, atoi(env("DB_CONNS_MAX", "4")));
    cfg.pool_wait_us = std::max(0, atoi(env("POOL_WAIT_US", "0")));  // 0 = automatic
    cfg.read_group_max = std::max(1, atoi(env("READ_GROUP_MAX", "64")));
    cfg.gzip_min = atoi(env("GZIP_MIN", "512"));  // responses at least this long are compressed for clients that accept gzip; 0 = never
    g_jwt.init(cfg.jwt_secret);
    // Same-box Postgres: skip libpq default TLS negotiation (an explicit sslmode in the URL still wins).
    if (pg_is_local(cfg.database_url.c_str())) setenv("PGSSLMODE", "disable", 0);

    // The rules cap the app at 512 Postgres connections.
    constexpr int kWriteConnsPerWorker = 1, kMaxConns = 512;
    if (cfg.workers < 1 || cfg.read_conns < 1 || cfg.workers * (cfg.read_conns_max + kWriteConnsPerWorker) > kMaxConns) {
        fprintf(stderr, "WORKERS x (DB_CONNS_MAX + %d) must be in 1..%d\n", kWriteConnsPerWorker, kMaxConns);
        return 1;
    }

    std::vector<std::unique_ptr<Worker>> workers;
    std::vector<std::thread> threads;
    try {
        for (int i = 0; i < cfg.workers; i++) workers.push_back(std::make_unique<Worker>(cfg, i));
    } catch (const std::exception &e) {
        fprintf(stderr, "startup failed: %s\n", e.what());
        return 1;
    }
    fprintf(stderr, "listening on %s:%d and :%d with %d workers\n", cfg.host.c_str(), cfg.port, cfg.extra_port, cfg.workers);
    egress::start();
    for (auto &w : workers) threads.emplace_back([&w] { w->run(); });
    for (auto &t : threads) t.join();
    return 0;
}
