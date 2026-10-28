// Entry point: read the challenge environment, raise fd limits, spawn
// worker threads.

#include "server.h"

const char* g_dburl;
const char* g_secret;
size_t g_secret_len;
const char* g_host = "0.0.0.0";
int g_port = 80;
int g_xport = 81;
int g_threads;
int g_pg_per = 8;
int g_uring = 0;
uint64_t g_idle_ms = 90000;
time_t g_start;

int main() {
  signal(SIGPIPE, SIG_IGN);
  g_dburl = getenv("DATABASE_URL");
  g_secret = getenv("JWT_SECRET");
  g_host = getenv("HOST");
  const char* ports = getenv("PORT");
  const char* xports = getenv("EXTRA_PORT");
  if (!g_dburl || !g_secret) die("DATABASE_URL and JWT_SECRET are required");
  if (!g_host) g_host = "0.0.0.0";
  g_port = ports ? atoi(ports) : 80;
  g_xport = xports ? atoi(xports) : 81;
  g_secret_len = strlen(g_secret);
  const char* t = getenv("THREADS");
  g_threads = t ? atoi(t) : (int)std::thread::hardware_concurrency();
  if (g_threads < 1) g_threads = 1;
  t = getenv("PG_CONNS");
  if (t) g_pg_per = atoi(t);
  if (g_pg_per < 1) g_pg_per = 1;
  if ((int64_t)g_threads * g_pg_per > 500)
    g_pg_per = std::max(1, 500 / g_threads);
  t = getenv("IDLE_MS");
  if (t) g_idle_ms = strtoull(t, nullptr, 10);
  t = getenv("URING");
  g_uring = t && atoi(t) == 1;
  g_start = time(nullptr);

  // connect_timeout so a dead db can't hang a worker forever;
  // ssl/gss probes off (the challenge db has neither), keepalives +
  // tcp_user_timeout so a silently dead db fails fast instead of
  // hanging pipelines into client timeouts
  static char dburl[4096];
  const bool kw = strchr(g_dburl, '=') &&
                  strncmp(g_dburl, "postgres://", 11) &&
                  strncmp(g_dburl, "postgresql://", 14);
  const bool has_ssl = strstr(g_dburl, "sslmode") != nullptr;
  if (kw)
    snprintf(dburl, sizeof(dburl), "%s connect_timeout=5%s", g_dburl,
             has_ssl ? "" : " sslmode=disable gssencmode=disable"
                            " tcp_user_timeout=10000 keepalives_idle=30"
                            " keepalives_interval=10 keepalives_count=3");
  else
    snprintf(dburl, sizeof(dburl), "%s%sconnect_timeout=5%s", g_dburl,
             strchr(g_dburl, '?') ? "&" : "?",
             has_ssl ? "" : "&sslmode=disable&gssencmode=disable"
                            "&tcp_user_timeout=10000&keepalives_idle=30"
                            "&keepalives_interval=10&keepalives_count=3");
  g_dburl = dburl;

  // take the kernel's own hard fd limit: connections are bounded by
  // box RAM and /proc/sys/fs/nr_open, not by any cap of ours
  rlimit rl;
  if (!getrlimit(RLIMIT_NOFILE, &rl) && rl.rlim_cur < rl.rlim_max) {
    rl.rlim_cur = rl.rlim_max;
    setrlimit(RLIMIT_NOFILE, &rl);
  }

  std::vector<std::thread> ts;
  for (int i = 1; i < g_threads; i++) ts.emplace_back(worker_run, i);
  worker_run(0);
  for (auto& x : ts) x.join();
  return 0;
}
