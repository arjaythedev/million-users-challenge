#include "net.h"
#include "jwt.h"
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

char g_host[256] = "0.0.0.0";
int g_port = 0;
int g_extra_port = 0;
char g_db_url[2048] = "";
static char g_jwt_secret[1024] = "";
#ifdef BUILD_PGO_GEN
/* Pass 12: PGO profile flush. The instrumented (-fprofile-generate) build
 * runs a finite mload profile pass and is then SIGTERM'd; a normal signal
 * death would discard all counters, so dump them explicitly before exit.
 * Production builds (no BUILD_PGO_GEN) do not install this handler: their
 * shutdown behavior is unchanged.
 * The profile-use rebuild keeps -DBUILD_PGO_GEN so main keeps an identical CFG (else its arc counts mismatch); there __gcov_dump is a NULL weak ref and the handler just exits.
 */
void __gcov_dump(void) __attribute__((weak));
static void pgo_term_handler(int sig) {
    (void)sig;
    if (__gcov_dump) __gcov_dump();
    _exit(0);
}
#endif
struct timespec g_start_mono;

static const char *req_env(const char *k) {
    const char *v = getenv(k);
    return (v && *v) ? v : NULL;
}

int main(void) {
    /* Block SIGPIPE process-wide. */
    signal(SIGPIPE, SIG_IGN);
    sigset_t ss; sigemptyset(&ss); sigaddset(&ss, SIGPIPE);
    sigprocmask(SIG_BLOCK, &ss, NULL);
#ifdef BUILD_PGO_GEN
    { struct sigaction sa; memset(&sa, 0, sizeof sa); sa.sa_handler = pgo_term_handler;
      sigemptyset(&sa.sa_mask); sa.sa_flags = 0;
      sigaction(SIGTERM, &sa, NULL); sigaction(SIGINT, &sa, NULL); }
#endif

    clock_gettime(CLOCK_MONOTONIC, &g_start_mono);

    const char *host = req_env("HOST");
    const char *port = req_env("PORT");
    const char *xport = req_env("EXTRA_PORT");
    const char *db = req_env("DATABASE_URL");
    const char *jwt = req_env("JWT_SECRET");
    if (host) { strncpy(g_host, host, sizeof g_host - 1); g_host[sizeof g_host-1] = '\0'; }
    if (!port || !xport) {
        fprintf(stderr, "PORT and EXTRA_PORT are required\n");
        return 1;
    }
    g_port = atoi(port); g_extra_port = atoi(xport);
    if (g_port <= 0 || g_extra_port <= 0) { fprintf(stderr, "bad ports\n"); return 1; }
    if (!jwt || !*jwt) { fprintf(stderr, "JWT_SECRET is required\n"); return 1; }
    if (db) { strncpy(g_db_url, db, sizeof g_db_url - 1); g_db_url[sizeof g_db_url - 1] = '\0'; }
    { strncpy(g_jwt_secret, jwt, sizeof g_jwt_secret - 1); g_jwt_secret[sizeof g_jwt_secret - 1] = '\0'; }

    long n = sysconf(_SC_NPROCESSORS_ONLN);
    if (n < 1) n = 1;
    if (n > 16) n = 16;
    /* Overload hardening: sysconf ignores taskset/cpuset/container quotas,
     * so a constrained box would overspawn workers (16 workers x 128k cap =
     * 2M fds on a 4-core box, and 512 PG conns). Clamp to the calling
     * thread's affinity when it is narrower; a no-op on unconstrained rigs
     * (affinity == online count). */
    {
        cpu_set_t set;
        CPU_ZERO(&set);
        if (sched_getaffinity(0, sizeof set, &set) == 0) {
            int k = CPU_COUNT(&set);
            if (k >= 1 && (long)k < n) n = k;
        }
    }
    fprintf(stderr, "server: %ld workers on %s:%d+%d (liburing probe, epoll fallback)\n",
        n, g_host, g_port, g_extra_port);

    pthread_t *th = calloc((size_t)n, sizeof *th);
    if (!th) return 1;
    for (long i = 0; i < n; i++) {
        if (pthread_create(&th[i], NULL, worker_main, (void *)i) != 0) {
            fprintf(stderr, "pthread_create %ld: %s\n", i, strerror(errno));
            return 1;
        }
    }
    int rc = 0;
    for (long i = 0; i < n; i++) {
        void *rv = NULL;
        pthread_join(th[i], &rv);
        /* A worker returns non-NULL only on fatal startup failure (bind /
         * epoll / pbuf ring); it never calls exit() itself, so one worker's
         * bad config can no longer SIGKILL the healthy ones mid-overload.
         * The process still reports failure via the exit code. */
        if (rv != NULL) rc = 1;
    }
    free(th);
    return rc;
}

const char *jwt_secret(void) {
    return g_jwt_secret;
}
