// feed: the challenge API in C. One event loop per core (io_uring, epoll fallback), each with its own
// SO_REUSEPORT listeners and Postgres connections. See README.md.
#include "loop.h"
#include <arpa/inet.h>
#include <linux/filter.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

config_t cfg;
struct timespec start_time;
int64_t now_sec(void) { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); return ts.tv_sec; }

static int env_int(const char *name, int def) { const char *v = getenv(name); return v && *v ? atoi(v) : def; }
static bool env_bool(const char *name, bool def) { const char *v = getenv(name); if (!v || !*v) return def; return *v == '1' || *v == 'y' || *v == 't'; }

static int listen_on(uint32_t host, int port) {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd < 0) { perror("socket"); exit(1); }
    int one = 1, defer = 10;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);          // inherited by accepted sockets
    setsockopt(fd, IPPROTO_TCP, TCP_DEFER_ACCEPT, &defer, sizeof defer); // accept once the first request is here
    struct sockaddr_in a = {.sin_family = AF_INET, .sin_port = htons((uint16_t)port), .sin_addr.s_addr = host};
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 65535)) { perror("bind/listen"); exit(1); }
    return fd;
}

// Steer each new connection to the listener whose loop runs on the CPU that is handling the packet:
// the socket index in the reuseport group is (cpu - base). Needs a contiguous CPU set and pinning.
static void attach_cbpf(int fd, int base) {
    struct sock_filter prog[] = {
        {BPF_LD | BPF_W | BPF_ABS, 0, 0, (uint32_t)(SKF_AD_OFF + SKF_AD_CPU)},
        {BPF_ALU | BPF_SUB | BPF_K, 0, 0, (uint32_t)base},
        {BPF_RET | BPF_A, 0, 0, 0},
    };
    struct sock_fprog fp = {.len = 3, .filter = prog};
    if (setsockopt(fd, SOL_SOCKET, SO_ATTACH_REUSEPORT_CBPF, &fp, sizeof fp)) perror("SO_ATTACH_REUSEPORT_CBPF");
}

static loop_t *loops;
static void *stats_thread(void *arg) {
    (void)arg;
    uint64_t pw = 0, pe = 0, pr = 0, pd = 0, pb = 0, ps = 0; double pcpu = 0;
    for (;;) {
        sleep((unsigned)cfg.stats_secs);
        uint64_t w = 0, e = 0, r = 0, d = 0, b = 0, s = 0, acc = 0, cl = 0; int conns = 0;
        for (int i = 0; i < cfg.loops; i++) {
            w += loops[i].st.wakeups; e += loops[i].st.events; r += loops[i].st.reqs; d += loops[i].st.dbreqs;
            b += loops[i].st.batches; s += loops[i].st.sleeps; acc += loops[i].st.accepts; cl += loops[i].st.closes; conns += loops[i].nconn;
        }
        FILE *f = fopen("/proc/self/stat", "r"); double cpu = 0; long rss = 0;
        if (f) { char line[1024]; if (fgets(line, sizeof line, f)) { char *p = strrchr(line, ')'); long ut = 0, st = 0; if (p) sscanf(p + 2, "%*c %*d %*d %*d %*d %*d %*u %*u %*u %*u %*u %ld %ld", &ut, &st); cpu = (double)(ut + st) / sysconf(_SC_CLK_TCK); } fclose(f); }
        f = fopen("/proc/self/statm", "r"); if (f) { long a; if (fscanf(f, "%ld %ld", &a, &rss) != 2) rss = 0; fclose(f); }
        double dt = cfg.stats_secs, dr = (double)(r - pr), dcpu = cpu - pcpu;
        fprintf(stderr, "stats: conns=%d req/s=%.0f cpu=%.2fcores us/req=%.1f wakeups/s=%.0f events/wakeup=%.1f db-req/s=%.0f queries/batch=%.1f sleeps/s=%.0f accepts=%llu closes=%llu rss=%ldMB\n",
                conns, dr / dt, dcpu / dt, dr > 0 ? dcpu * 1e6 / dr : 0, (double)(w - pw) / dt, w - pw ? (double)(e - pe) / (double)(w - pw) : 0,
                (double)(d - pd) / dt, b - pb ? (double)(d - pd) / (double)(b - pb) : 0, (double)(s - ps) / dt, (unsigned long long)acc, (unsigned long long)cl, rss * sysconf(_SC_PAGESIZE) >> 20);
        pw = w; pe = e; pr = r; pd = d; pb = b; ps = s; pcpu = cpu;
    }
    return NULL;
}

int main(void) {
    clock_gettime(CLOCK_REALTIME, &start_time);
    setvbuf(stderr, NULL, _IOLBF, 0);
    signal(SIGPIPE, SIG_IGN);
    const char *url = getenv("DATABASE_URL");
    if (!url || !*url) { fprintf(stderr, "DATABASE_URL is required\n"); return 1; }
    if (pg_parse_url(&cfg.pg, url)) { fprintf(stderr, "DATABASE_URL: cannot parse or resolve\n"); return 1; }
    const char *secret = getenv("JWT_SECRET");
    if (!secret || !*secret) { fprintf(stderr, "JWT_SECRET is required\n"); return 1; }
    cfg.secret_len = strlen(secret); if (cfg.secret_len > sizeof cfg.secret) cfg.secret_len = sizeof cfg.secret; memcpy(cfg.secret, secret, cfg.secret_len);
    const char *host = getenv("HOST"); if (!host || !*host) host = "0.0.0.0";
    if (inet_pton(AF_INET, host, &cfg.host) != 1) { fprintf(stderr, "HOST %s: only IPv4 is supported\n", host); return 1; }
    cfg.ports[0] = env_int("PORT", 3000); cfg.nports = 1;
    // EXTRA_PORT: one port, or a comma list (load generators with one IP run out of source ports)
    for (const char *e = getenv("EXTRA_PORT"); e && *e && cfg.nports < 8; ) { int p = atoi(e); if (p > 0) cfg.ports[cfg.nports++] = p; e = strchr(e, ','); if (e) e++; }
    // the loop count and CPU list follow the affinity mask (taskset / systemd CPUAffinity)
    cpu_set_t set; sched_getaffinity(0, sizeof set, &set);
    int cpus[1024], ncpu = 0;
    for (int i = 0; i < 1024 && ncpu < 1024; i++) if (CPU_ISSET(i, &set)) cpus[ncpu++] = i;
    cfg.loops = env_int("LOOPS", ncpu); if (cfg.loops < 1) cfg.loops = 1;
    cfg.db_read = env_int("DB_READ", 96); cfg.db_write = env_int("DB_WRITE", 32);
    cfg.batch_max = env_int("BATCH_MAX", 64); cfg.batch_min = env_int("BATCH_MIN", 32); cfg.batch_micros = env_int("BATCH_MICROS", 8000);
    cfg.gzip_min = env_int("GZIP_MIN", 1024); cfg.stats_secs = env_int("STATS_SECS", 0);
    cfg.pin = env_bool("PIN", true); cfg.cbpf = env_bool("CBPF", true); cfg.uring = env_bool("URING", true);
    cfg.pg_sqe = cfg.uring && env_bool("PG_SQE", false);
    bool contiguous = cfg.loops <= ncpu; for (int i = 1; i < cfg.loops && contiguous; i++) if (cpus[i] != cpus[0] + i) contiguous = false;
    if (!contiguous) { cfg.cbpf = false; if (cfg.loops > ncpu) cfg.pin = false; }
    if (!cfg.pin) cfg.cbpf = false;
    struct rlimit rl;
    if (!getrlimit(RLIMIT_NOFILE, &rl)) { rl.rlim_cur = rl.rlim_max; setrlimit(RLIMIT_NOFILE, &rl); }
    // gzip code trained on a sample feed body
    static uint8_t sample[65536]; size_t sample_len = 0;
    FILE *sf = fopen("feed_sample.json", "rb");
    if (sf) { sample_len = fread(sample, 1, sizeof sample, sf); fclose(sf); }
    if (!sample_len) { const char *s = "{\"posts\":[{\"id\":1,\"body\":\"hello world\",\"created_at\":\"2025-01-01T00:00:00.000Z\",\"author\":\"user\",\"like_count\":0}]}"; sample_len = strlen(s); memcpy(sample, s, sample_len); }
    // listeners: loop i gets socket index i of each port's reuseport group (bind order)
    int nports = cfg.nports;
    int (*lfds)[8] = calloc((size_t)cfg.loops, sizeof *lfds);
    for (int p = 0; p < nports; p++) {
        for (int i = 0; i < cfg.loops; i++) lfds[i][p] = listen_on(cfg.host, cfg.ports[p]);
        if (cfg.cbpf) attach_cbpf(lfds[0][p], cpus[0]);
    }
    loops = calloc((size_t)cfg.loops, sizeof *loops);
    for (int i = 0; i < cfg.loops; i++) loop_init(&loops[i], i, cfg.pin ? cpus[i % ncpu] : -1, lfds[i], nports, sample, sample_len);
    pthread_t *th = calloc((size_t)cfg.loops, sizeof *th);
    for (int i = 0; i < cfg.loops; i++) pthread_create(&th[i], NULL, loop_run, &loops[i]);
    char portlist[64] = {0}; for (int p = 0, n = 0; p < nports; p++) n += snprintf(portlist + n, sizeof portlist - (size_t)n, "%s%d", p ? "," : "", cfg.ports[p]);
    fprintf(stderr, "feed: %d loops (%d cpus%s%s), ports %s, postgres %d read + %d write connections, batch window %d events / %d us, gzip from %d bytes\n",
            cfg.loops, ncpu, cfg.pin ? ", pinned" : "", cfg.cbpf ? ", cpu-steered accepts" : "", portlist,
            loops[0].nr * cfg.loops, loops[0].nw * cfg.loops, cfg.batch_min, cfg.batch_micros, cfg.gzip_min);
    if (cfg.stats_secs > 0) { pthread_t st; pthread_create(&st, NULL, stats_thread, NULL); }
    sigset_t ss; sigemptyset(&ss); sigaddset(&ss, SIGTERM); sigaddset(&ss, SIGINT);
    pthread_sigmask(SIG_BLOCK, &ss, NULL);
    int sig; sigwait(&ss, &sig);
    fprintf(stderr, "feed: exiting\n");
    return 0;
}
