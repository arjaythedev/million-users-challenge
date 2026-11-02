\ ============================================================================
\ ffi.fs - shared C library bindings for the whole project (libpq + crypto +
\ sockets/epoll). This is the ONE c-library block. The Postgres layer (pg.fs)
\ and the JWT layer (jwt.fs) pull their C functions from here, and the event
\ loop (epoll.fs) uses the socket/epoll helpers.
\
\ Changed from the $12 (SQLite) entry: the sqlite3 prototypes/bindings are
\ replaced with libpq (PQ*). libcrypto (HMAC/EVP_sha256), the raw-socket
\ listener, and the epoll/non-blocking I/O helpers are unchanged, except:
\   - the listener now also sets SO_REUSEPORT (every worker process binds the
\     same port; the kernel load-balances accepted connections across workers);
\   - a fork() binding is added for the multi-process worker model (phase 2).
\ ============================================================================

c-library tdsc

\c #include <stdint.h>
\c #include <stddef.h>

\ --- libpq prototypes ---
\ Ubuntu 24.04's libpq-dev installs the header under /usr/include/postgresql/
\ (not a default C include dir), and gforth 0.7.3's `c-library` has no portable
\ word to add -I. The challenge box is Ubuntu 24.04 (rule 10), where this path
\ is fixed, so we include it absolutely. build.sh asserts the header exists.
\c #include "/usr/include/postgresql/libpq-fe.h"

\ --- libcrypto prototypes (used by jwt.fs) ---
\c extern unsigned char *HMAC(const void *evp_md, const void *key, int key_len,
\c     const unsigned char *d, size_t n, unsigned char *md, unsigned int *md_len);
\c extern const void *EVP_sha256(void);

\ --- sockets / epoll / process (used by http.fs, epoll.fs, server.fs) ---
\c #include <sys/types.h>
\c #include <sys/socket.h>
\c #include <netinet/in.h>
\c #include <netinet/tcp.h>
\c #include <arpa/inet.h>
\c #include <unistd.h>
\c #include <errno.h>
\c #include <signal.h>
\c #include <string.h>
\c
\c /* Create a listening TCP socket bound to host:port with SO_REUSEADDR and
\c    SO_REUSEPORT. SO_REUSEPORT lets every worker process bind the SAME
\c    host:port; the kernel then spreads accepted connections across the
\c    workers' listen sockets. host is a dotted-quad string (e.g. "127.0.0.1")
\c    or "0.0.0.0"/NULL for INADDR_ANY. Returns the listening fd, or -1. */
\c static int tdsc_listen(const char *host, int port, int backlog) {
\c   int fd = socket(AF_INET, SOCK_STREAM, 0);
\c   if (fd < 0) return -1;
\c   int one = 1;
\c   setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
\c   setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof(one));
\c   struct sockaddr_in a;
\c   memset(&a, 0, sizeof(a));
\c   a.sin_family = AF_INET;
\c   a.sin_port = htons((unsigned short)port);
\c   if (host == 0 || host[0] == 0) a.sin_addr.s_addr = htonl(INADDR_ANY);
\c   else if (inet_pton(AF_INET, host, &a.sin_addr) != 1) { close(fd); return -1; }
\c   if (bind(fd, (struct sockaddr*)&a, sizeof(a)) != 0) { close(fd); return -1; }
\c   if (listen(fd, backlog) != 0) { close(fd); return -1; }
\c   return fd;
\c }
\c static int tdsc_close(int fd) { return close(fd); }
\c static int tdsc_set_nodelay(int fd) {
\c   int one = 1; return setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
\c }
\c static void tdsc_ignore_sigpipe(void) { signal(SIGPIPE, SIG_IGN); }
\c static int tdsc_fork(void) { return (int)fork(); }
\c static int tdsc_getpid(void) { return (int)getpid(); }
\c #include <sys/wait.h>
\c /* block until any child exits; returns that child's pid (or -1). */
\c static int tdsc_wait(void) { int st; return (int)wait(&st); }
\c /* SIGTERM a single pid. The supervisor signals each worker it forked, rather
\c    than kill(0,...) which would hit the whole process group -- under test/run.sh
\c    that group also contains the test harness, so kill(0) made the harness kill
\c    itself (exit 143). Signaling explicit worker pids is correct under both
\c    run.sh and systemd. Returns 0 on success, -1 on error (e.g. already gone). */
\c static int tdsc_kill(int pid) { return kill((pid_t)pid, SIGTERM); }
\c
\c /* ---- epoll event loop support ---- */
\c #include <sys/epoll.h>
\c #include <fcntl.h>
\c #define TDSC_MAXEV 4096
\c static struct epoll_event tdsc_evbuf[TDSC_MAXEV];
\c static int tdsc_epoll_create(void) { return epoll_create1(0); }
\c /* set O_NONBLOCK on fd */
\c static int tdsc_set_nonblock(int fd) {
\c   int fl = fcntl(fd, F_GETFL, 0); if (fl < 0) return -1;
\c   return fcntl(fd, F_SETFL, fl | O_NONBLOCK);
\c }
\c /* non-blocking accept: returns fd, or -1 (check errno EAGAIN) */
\c static int tdsc_accept_nb(int lfd) {
\c   int fd = accept(lfd, 0, 0);
\c   if (fd >= 0) { int fl = fcntl(fd, F_GETFL, 0); if (fl >= 0) fcntl(fd, F_SETFL, fl | O_NONBLOCK); }
\c   return fd;
\c }
\c /* epoll_ctl add/mod/del. op: 1=ADD 2=DEL 3=MOD. events bitmask (EPOLLIN=1,EPOLLOUT=4). */
\c static int tdsc_epoll_ctl(int epfd, int op, int fd, unsigned int events) {
\c   int realop = (op==1)?EPOLL_CTL_ADD:(op==2)?EPOLL_CTL_DEL:EPOLL_CTL_MOD;
\c   struct epoll_event ev; ev.events = events; ev.data.fd = fd;
\c   return epoll_ctl(epfd, realop, fd, &ev);
\c }
\c /* wait; fills internal tdsc_evbuf. returns number of ready events (or -1). */
\c static int tdsc_epoll_wait(int epfd, int timeout_ms) {
\c   int n; do { n = epoll_wait(epfd, tdsc_evbuf, TDSC_MAXEV, timeout_ms); }
\c   while (n < 0 && errno == EINTR); return n;
\c }
\c static int tdsc_ev_fd(int i)     { return tdsc_evbuf[i].data.fd; }
\c static unsigned int tdsc_ev_ev(int i) { return tdsc_evbuf[i].events; }
\c /* non-blocking recv/send: return bytes, 0 on EOF, -1 on EAGAIN/error (check errno) */
\c static long tdsc_recv_nb(int fd, char *buf, long n) {
\c   long r = recv(fd, buf, (size_t)n, 0); return r;
\c }
\c static long tdsc_send_nb(int fd, const char *buf, long n) {
\c   long w = send(fd, buf, (size_t)n, MSG_NOSIGNAL); return w;
\c }
\c static int tdsc_errno(void) { return errno; }
\c
\c /* ---- libpq parameter-array helpers ----
\c    PQexecPrepared wants a `const char * const *paramValues`. Building a C
\c    pointer array from Forth is awkward, so we keep a small fixed-size array
\c    of char* here and let Forth fill slots by index. Max 2 params (our widest
\c    statement binds $1,$2). Values must stay valid until PQexecPrepared runs;
\c    Forth passes pointers to NUL-terminated scratch it owns. */
\c #define TDSC_MAXPARAM 2
\c static const char *tdsc_params[TDSC_MAXPARAM];
\c static void tdsc_param_set(int i, const char *p) {
\c   if (i >= 0 && i < TDSC_MAXPARAM) tdsc_params[i] = p;
\c }
\c static const char *const *tdsc_params_ptr(void) { return tdsc_params; }

s" pq"     add-lib
s" crypto" add-lib

\ --- libpq: connection ---
\ PGconn *PQconnectdb(const char *conninfo)
c-function PQconnectdb    PQconnectdb    a -- a
\ ConnStatusType PQstatus(const PGconn*)   (CONNECTION_OK == 0)
c-function PQstatus       PQstatus       a -- n
\ char *PQerrorMessage(const PGconn*)
c-function PQerrorMessage PQerrorMessage a -- a
\ void PQfinish(PGconn*)
c-function PQfinish       PQfinish       a -- void
\ int PQsetnonblocking(PGconn*, int)
c-function PQsetnonblocking PQsetnonblocking a n -- n
\ int PQsocket(const PGconn*)
c-function PQsocket       PQsocket       a -- n

\ --- libpq: prepared statements + execution ---
\ PGresult *PQprepare(conn, stmtName, query, nParams, paramTypes)
c-function PQprepare      PQprepare      a a a n a -- a
\ PGresult *PQexecPrepared(conn, stmtName, nParams, paramValues, paramLengths, paramFormats, resultFormat)
c-function PQexecPrepared PQexecPrepared a a n a a a n -- a
\ PGresult *PQexec(conn, query)   (used for session SET and SELECT 1)
c-function PQexec         PQexec         a a -- a

\ --- libpq: result inspection ---
\ ExecStatusType PQresultStatus(const PGresult*)
c-function PQresultStatus PQresultStatus a -- n
\ int PQntuples(const PGresult*)
c-function PQntuples      PQntuples      a -- n
\ char *PQgetvalue(const PGresult*, int row, int col)
c-function PQgetvalue     PQgetvalue     a n n -- a
\ int PQgetisnull(const PGresult*, int row, int col)
c-function PQgetisnull    PQgetisnull    a n n -- n
\ int PQgetlength(const PGresult*, int row, int col)
c-function PQgetlength    PQgetlength    a n n -- n
\ char *PQcmdTuples(PGresult*)   ("1"/"0"/"" as a C string)
c-function PQcmdTuples    PQcmdTuples    a -- a
\ char *PQresultErrorField(const PGresult*, int fieldcode)  ('C' = SQLSTATE)
c-function PQresultErrorField PQresultErrorField a n -- a
\ void PQclear(PGresult*)
c-function PQclear        PQclear        a -- void

\ --- crypto (jwt.fs) ---
c-function HMAC        HMAC        a a n a n a a -- a
c-function EVP_sha256  EVP_sha256  -- a

\ --- sockets (custom C helpers above) ---
c-function tdsc_listen         tdsc_listen        a n n -- n
c-function tdsc_close          tdsc_close         n -- n
c-function tdsc_set_nodelay    tdsc_set_nodelay   n -- n
c-function tdsc_ignore_sigpipe tdsc_ignore_sigpipe -- void
c-function tdsc_fork           tdsc_fork          -- n
c-function tdsc_getpid         tdsc_getpid        -- n
c-function tdsc_wait           tdsc_wait          -- n
c-function tdsc_kill           tdsc_kill          n -- n

\ --- epoll + non-blocking I/O ---
c-function tdsc_epoll_create   tdsc_epoll_create   -- n
c-function tdsc_set_nonblock   tdsc_set_nonblock   n -- n
c-function tdsc_accept_nb      tdsc_accept_nb      n -- n
c-function tdsc_epoll_ctl      tdsc_epoll_ctl      n n n n -- n
c-function tdsc_epoll_wait     tdsc_epoll_wait     n n -- n
c-function tdsc_ev_fd          tdsc_ev_fd          n -- n
c-function tdsc_ev_ev          tdsc_ev_ev          n -- n
c-function tdsc_recv_nb        tdsc_recv_nb        n a n -- n
c-function tdsc_send_nb        tdsc_send_nb        n a n -- n
c-function tdsc_errno          tdsc_errno          -- n

\ --- libpq param-array helpers ---
c-function tdsc_param_set      tdsc_param_set      n a -- void
c-function tdsc_params_ptr     tdsc_params_ptr     -- a

end-c-library
