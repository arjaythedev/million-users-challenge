// System headers and small wrappers for macros, errno and socket setup.
#ifndef SV_SHIM_H
#define SV_SHIM_H
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <signal.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static inline int sv_errno(void) { return errno; }

static inline int64_t sv_now_ms(void) {
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    return (int64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}

static inline int64_t sv_monotonic_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec;
}

static inline const char *sv_getenv(const char *name) { return getenv(name); }

int sv_listen(const char *host, int port, int backlog, int reuseport);
/// A blocking TCP connection with TCP_NODELAY and `timeout_ms` send and receive timeouts, or -1.
int sv_connect(const char *host, int port, int timeout_ms);
/// Drops the socket timeouts and makes it nonblocking, for handing it to a ring.
void sv_set_nonblocking(int fd);
/// Fills `out` with random bytes.
void sv_random(void *out, unsigned n);
/// The CPUs this process may run on (taskset, CPUAffinity).
int sv_cpu_count(void);
int sv_spawn(void *(*body)(void *), void *arg);
int sv_raise_nofile(void);
void sv_ignore_sigpipe(void);
/// Compresses `n` bytes to gzip with ISA-L at level 0. Returns the size, or 0 if it doesn't fit.
size_t sv_gzip(const void *in, size_t n, void *out, size_t cap);
#endif
