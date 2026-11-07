// Linux/libc declarations. Everything the server needs from the outside world is on this page.
module server;

alias ThreadFn = fn void*(void*);

struct EpollEvent @packed @feat(X86_64) { uint events; ulong u64; }  // 12 bytes on x86-64
struct EpollEvent @feat(!X86_64) { uint events; ulong u64; }        // 16 bytes elsewhere (aarch64: local testing)

struct SockAddrIn
{
	ushort family;
	ushort port;
	uint addr;
	char[8] zero;
}

struct TimeSpec { long s; long ns; }
struct TimeVal { long s; long us; }
struct Rlimit { ulong cur; ulong max; }

struct AddrInfo
{
	int flags;
	int family;
	int socktype;
	int protocol;
	uint addrlen;
	void* addr;
	char* canonname;
	AddrInfo* next;
}

extern fn void* malloc(usz n);
extern fn void* calloc(usz n, usz size);
extern fn void* realloc(void* p, usz n);
extern fn void free(void* p);
extern fn void* memcpy(void* d, void* s, usz n);
extern fn void* memset(void* d, int c, usz n);
extern fn void* memmove(void* d, void* s, usz n);
extern fn void* memchr(void* s, int c, usz n);
extern fn void* memmem(void* h, usz hl, void* nd, usz nl);
extern fn int memcmp(void* a, void* b, usz n);
extern fn usz strlen(char* s);
extern fn int strncasecmp(char* a, char* b, usz n);
extern fn double strtod(char* s, char** end);
extern fn int atoi(char* s);
extern fn char* getenv(char* name);
extern fn void exit(int code);
extern fn long time(long* t);
extern fn int close(int fd);
extern fn sz read(int fd, void* buf, usz n);
extern fn sz write(int fd, void* buf, usz n);
extern fn int socket(int domain, int type, int protocol);
extern fn int setsockopt(int fd, int level, int opt, void* val, uint len);
extern fn int bind(int fd, void* addr, uint len);
extern fn int listen(int fd, int backlog);
extern fn int connect(int fd, void* addr, uint len);
extern fn int accept4(int fd, void* addr, uint* len, int flags);
extern fn int fcntl(int fd, int cmd, int arg);
extern fn int inet_pton(int af, char* src, void* dst);
extern fn ushort htons(ushort v);
extern fn int getaddrinfo(char* node, char* service, AddrInfo* hints, AddrInfo** res);
extern fn void freeaddrinfo(AddrInfo* res);
extern fn sz getrandom(void* buf, usz n, uint flags);
extern fn int epoll_create1(int flags);
extern fn int epoll_ctl(int ep, int op, int fd, EpollEvent* ev);
extern fn int epoll_wait(int ep, EpollEvent* evs, int max, int timeout);
extern fn int nanosleep(TimeSpec* req, TimeSpec* rem);
extern fn int getrlimit(int res, Rlimit* r);
extern fn int setrlimit(int res, Rlimit* r);
extern fn int sched_getaffinity(int pid, usz size, void* mask);
extern fn int pthread_create(void** th, void* attr, ThreadFn f, void* arg);
extern fn void* signal(int sig, void* handler);
extern fn int* __errno_location();

fn int errno() @inline => *__errno_location();

const int EAGAIN = 11;
const int EINTR = 4;
const int ENFILE = 23;
const int EMFILE = 24;
const int AF_INET = 2;
const int SOCK_STREAM = 1;
const int SOCK_NONBLOCK = 0x800;
const int SOCK_CLOEXEC = 0x80000;
const int SOL_SOCKET = 1;
const int SO_REUSEADDR = 2;
const int SO_REUSEPORT = 15;
const int SO_RCVTIMEO = 20;
const int SO_SNDTIMEO = 21;
const int IPPROTO_TCP = 6;
const int TCP_NODELAY = 1;
const int TCP_THIN_LINEAR_TIMEOUTS = 16;
const int F_GETFL = 3;
const int F_SETFL = 4;
const int O_NONBLOCK = 0x800;
const uint EPOLLIN = 1;
const uint EPOLLOUT = 4;
const uint EPOLLERR = 8;
const uint EPOLLHUP = 16;
const int EPOLL_CTL_ADD = 1;
const int EPOLL_CTL_DEL = 2;
const int EPOLL_CTL_MOD = 3;
const int EPOLL_CLOEXEC = 0x80000;
const int RLIMIT_NOFILE = 7;
const int SIGPIPE = 13;

fn void ep_set(int ep, int op, int fd, ulong tag, uint ev) @inline
{
	EpollEvent e = { ev, tag };
	epoll_ctl(ep, op, fd, &e);
}

fn void set_nonblock(int fd)
{
	fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
}
