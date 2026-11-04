// Feed API on Postgres in C3: one epoll loop per CPU, each with its own SO_REUSEPORT listeners on PORT and
// EXTRA_PORT and its own pipelined Postgres connections. See README.md.
// Config comes only from DATABASE_URL, JWT_SECRET, HOST, PORT, EXTRA_PORT.
module server;
import std::io;

const int BATCH_MIN = 32;       // an iteration with fewer events than this ...
const long BATCH_NS = 2000000;  // ... is followed by a 2 ms pause (see README.md, "Batching window")
const ulong TAG_LISTEN = 1ul << 32;
const ulong TAG_PG = 2ul << 32;

struct Th
{
	int ep;
	int[2] lfd;
	int nl;
	int npg;
	PgConn* pg;
	long retry_at;
	long paused_until;
}

char* g_host;
int[2] g_ports;
int g_nports;

fn int listen_on(int port)
{
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
	int one = 1;
	setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, 4);
	setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, 4);
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, 4);  // inherited by accepted sockets
	SockAddrIn a = { .family = AF_INET, .port = htons((ushort)port) };
	if (inet_pton(AF_INET, g_host, &a.addr) != 1)
	{
		io::eprintfn("bad HOST %s", (ZString)g_host);
		exit(1);
	}
	if (bind(fd, &a, SockAddrIn::size) != 0 || listen(fd, 65535) != 0)
	{
		io::eprintfn("bind/listen failed, errno %d", errno());
		exit(1);
	}
	return fd;
}

fn void listeners(Th* t, int op)
{
	for (int i = 0; i < t.nl; i++) ep_set(t.ep, op, t.lfd[i], TAG_LISTEN | (ulong)i, EPOLLIN);
}

fn void accept_conns(Th* t, int lfd)
{
	for (int k = 0; k < 256; k++)
	{
		int c = accept4(lfd, null, null, SOCK_NONBLOCK | SOCK_CLOEXEC);
		if (c < 0)
		{
			int e = errno();
			if (e == EMFILE || e == ENFILE)  // out of descriptors: retry in a second
			{
				listeners(t, EPOLL_CTL_DEL);
				t.paused_until = now_s + 1;
			}
			return;
		}
		if (c >= maxfd)
		{
			close(c);
			continue;
		}
		conns[c].open = true;
		ep_set(t.ep, EPOLL_CTL_ADD, c, (ulong)c, EPOLLIN);
	}
}

fn void* run(void* arg)
{
	Th* t = arg;
	arena = malloc(OBUF);
	zbuf = malloc(OBUF);
	rbuf = malloc(RBUF);
	txt = malloc(MAXREQ);
	now_s = time(null);
	for (int i = 0; i < t.npg; i++) pg_connect(t, i);
	for (int i = 0; i < g_nports; i++) t.lfd[t.nl++] = listen_on(g_ports[i]);
	listeners(t, EPOLL_CTL_ADD);
	EpollEvent[512] evs;
	while (true)
	{
		int n = epoll_wait(t.ep, &evs, 512, 1000);
		if (n < 0)
		{
			if (errno() == EINTR) continue;
			io::eprintfn("epoll_wait failed, errno %d", errno());
			exit(1);
		}
		now_s = time(null);
		for (int i = 0; i < n; i++)
		{
			ulong tag = evs[i].u64;
			uint e = evs[i].events;
			int id = (int)tag;
			if (tag >= TAG_PG)
			{
				PgConn* p = &t.pg[id];
				if (!p.up) continue;
				if ((e & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0) pg_read(t, p);
				if (p.up && (e & EPOLLOUT) != 0) p.dirty = true;
			}
			else if (tag >= TAG_LISTEN)
			{
				accept_conns(t, t.lfd[id]);
			}
			else if (conns[id].open)
			{
				if ((e & EPOLLOUT) != 0)
				{
					on_writable(t, id);
				}
				else if ((e & (EPOLLIN | EPOLLHUP | EPOLLERR)) != 0)
				{
					on_readable(t, id);
				}
			}
		}
		for (int i = 0; i < t.npg; i++)  // one Sync for this iteration's reads, then send everything
		{
			PgConn* p = &t.pg[i];
			if (!p.up || !p.dirty) continue;
			if (p.unsynced) pg_sync(p);
			p.dirty = false;
			p.inbatch = 0;
			pg_flush(t, p);
		}
		if (t.paused_until != 0 && now_s >= t.paused_until)
		{
			t.paused_until = 0;
			listeners(t, EPOLL_CTL_ADD);
		}
		// A light iteration: let events pile up for a moment, so the next one serves more per wake-up and
		// per Sync.
		if (n != 0 && n < BATCH_MIN)
		{
			TimeSpec ts = { 0, BATCH_NS };
			nanosleep(&ts, null);
		}
	}
}

fn int main()
{
	char* secret = getenv("JWT_SECRET");
	char* port = getenv("PORT");
	char* extra = getenv("EXTRA_PORT");
	char* dburl = getenv("DATABASE_URL");
	if (dburl == null || secret == null)
	{
		io::eprintn("DATABASE_URL and JWT_SECRET are required");
		return 1;
	}
	if (!parse_dburl((String)dburl[:strlen(dburl)]))
	{
		io::eprintn("DATABASE_URL must be postgres://user:pass@host:port/db");
		return 1;
	}
	g_host = getenv("HOST") != null ? getenv("HOST") : "0.0.0.0";
	g_ports[g_nports++] = port != null ? atoi(port) : 3000;
	if (extra != null && *extra != 0) g_ports[g_nports++] = atoi(extra);
	signal(SIGPIPE, (void*)1);  // SIG_IGN

	Rlimit rl;
	getrlimit(RLIMIT_NOFILE, &rl);
	if (rl.cur < rl.max)
	{
		rl.cur = rl.max;
		setrlimit(RLIMIT_NOFILE, &rl);
	}
	maxfd = rl.cur > (16 << 20) ? (16 << 20) : (long)rl.cur;  // LimitNOFILE=8388608 on the box: 8.4M connections
	conns = calloc((usz)maxfd, Conn::size);  // untouched pages cost nothing

	foreach (&x : b64t) *x = -1;
	foreach (i, ch : "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_") b64t[ch] = (ichar)i;
	hm_base.init(secret[:strlen(secret)]);
	gz_init();
	started = time(null);

	char[128] cs = {};
	sched_getaffinity(0, 128, &cs);
	int nthreads = 0;
	foreach (b : cs) nthreads += (int)b.popcount();
	if (nthreads < 1) nthreads = 1;
	int npg_per = 64 / nthreads > 2 ? 64 / nthreads : 2;  // ~64 in all; reads and writes need one each
	io::eprintfn("%d threads, %d Postgres connections each, ports %d%s",
		nthreads, npg_per, g_ports[0], g_nports > 1 ? " + extra" : "");

	Th* ts = calloc((usz)nthreads, Th::size);
	for (int i = 0; i < nthreads; i++)
	{
		ts[i].ep = epoll_create1(EPOLL_CLOEXEC);
		ts[i].npg = npg_per;
		ts[i].pg = calloc((usz)npg_per, PgConn::size);
		if (i != 0)
		{
			void* th;
			pthread_create(&th, null, &run, &ts[i]);
		}
	}
	run(&ts[0]);
	return 0;
}
