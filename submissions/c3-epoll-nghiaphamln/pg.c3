// A Postgres client for exactly this server: the extended protocol over a plain TCP socket, five prepared
// statements, binary parameters and results, pipelining with explicit Syncs, SCRAM-SHA-256 / MD5 / password
// auth from the standard library hashes. Results are never copied into row objects: a DataRow is rendered
// to JSON straight from the socket buffer.
//
// Every request runs one prepared statement. Reads queue up behind each other on a connection and share
// one Sync at the end of the loop iteration; each write gets its own Sync right after it, so it commits
// (or fails, e.g. a like's foreign key) on its own, and its response is sent only once that Sync's
// ReadyForQuery arrives, i.e. after the commit.
module server;
import std::io, std::bits, std::hash::sha256, std::hash::md5, std::encoding::base64;

const char Q_FEED = 0;
const char Q_POST = 1;
const char Q_INS = 2;
const char Q_LIKE = 3;
const char Q_HEALTH = 4;
const char Q_SYNC = 5;
const uint PACK = 64;  // reads per batch on one connection

// A statement in flight (or a Sync), in the order it was sent.
struct Qe
{
	int fd;
	uint gen;
	char kind;
	bool close;
	bool gz;
	long id;
	char* extra;  // POST /posts: the body and the username, echoed in the response
	uint blen;
	uint ulen;
}

// What a statement came back with: rows still sitting in the receive buffer, or an error.
struct PgRes
{
	char status;     // 'T' rows, 'C' done, 'E' error, 'F' connection lost
	char* rows;
	uint nrows;
	char[5] sqlstate;
	char* msg;
	usz msglen;
	long id;         // INSERT ... RETURNING id, created_at
	long ts;
	bool created;    // a like that inserted a row (vs. one that was already there)
}

struct PgConn
{
	int fd;
	bool up;
	char* rb;        // received bytes ...
	usz rlen;
	usz rcap;
	usz rpos;        // ... and the parse position
	char* wb;        // bytes to send ...
	usz wlen;
	usz wcap;
	usz woff;        // ... and the send position
	Qe* q;           // ring of sent statements and Syncs
	uint head;
	uint n;
	uint cap;
	bool dirty;      // something to flush
	bool unsynced;   // reads sent without a Sync yet
	bool wout;       // waiting for EPOLLOUT
	ushort inbatch;  // statements since the last flush
	usz rowstart;    // DataRows of the statement in progress, as an offset into rb ...
	uint nrows;
	bool in_rows;    // ... kept across reads
	bool has_held;   // a write's result, waiting for its commit
	Qe held;
	long held_a;
	long held_b;
	bool held_created;
}

const String COLS = "SELECT p.id, p.body, p.created_at, u.username, "
	+++ "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count "
	+++ "FROM posts p JOIN users u ON u.id = p.user_id";
const String SQL_FEED = COLS +++ " ORDER BY p.created_at DESC, p.id DESC LIMIT 20";
const String SQL_POST = COLS +++ " WHERE p.id = $1";
const String SQL_INS = "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at";
const String SQL_LIKE = "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING";
const uint OID_INT8 = 20;
const uint OID_TEXT = 25;

String pg_user;
String pg_pass;
String pg_host;
String pg_port;
String pg_db;
char* pg_host_z;  // C strings for getaddrinfo
char* pg_port_z;

fn long be64(char* p) @inline
{
	ulong v;
	memcpy(&v, p, 8);
	return (long)bswap(v);
}

fn int be32(char* p) @inline
{
	uint v;
	memcpy(&v, p, 4);
	return (int)bswap(v);
}

// ---------- outgoing messages ----------
fn void PgConn.reserve(&self, usz n)
{
	if (self.wcap - self.wlen >= n) return;
	usz nc = self.wcap != 0 ? self.wcap : 64 * 1024;
	while (nc - self.wlen < n) nc *= 2;
	self.wb = realloc(self.wb, nc);
	self.wcap = nc;
}

fn void PgConn.w8(&self, char c) @inline
{
	self.reserve(1);
	self.wb[self.wlen++] = c;
}

fn void PgConn.w16(&self, ushort v)
{
	self.reserve(2);
	v = bswap(v);
	memcpy(self.wb + self.wlen, &v, 2);
	self.wlen += 2;
}

fn void PgConn.w32(&self, uint v)
{
	self.reserve(4);
	v = bswap(v);
	memcpy(self.wb + self.wlen, &v, 4);
	self.wlen += 4;
}

fn void PgConn.w64(&self, ulong v)
{
	self.reserve(8);
	v = bswap(v);
	memcpy(self.wb + self.wlen, &v, 8);
	self.wlen += 8;
}

fn void PgConn.wbytes(&self, char* p, usz n)
{
	self.reserve(n);
	memcpy(self.wb + self.wlen, p, n);
	self.wlen += n;
}

fn void PgConn.wstr(&self, String s)
{
	self.wbytes(s.ptr, s.len);
	self.w8(0);
}

// Starts a message (type 0: the untyped StartupMessage). Returns where the length goes.
fn usz PgConn.msg(&self, char type)
{
	if (type != 0) self.w8(type);
	usz at = self.wlen;
	self.w32(0);
	return at;
}

fn void PgConn.end(&self, usz at)
{
	uint len = bswap((uint)(self.wlen - at));
	memcpy(self.wb + at, &len, 4);
}

// Bind (named statement, binary params and results) + Execute, no Sync.
fn void PgConn.exec(&self, String stmt, long* params, usz np, char* text, usz tl)
{
	usz at = self.msg('B');
	self.w8(0);
	self.wstr(stmt);
	self.w16(1);  // one parameter format code: binary, for all parameters
	self.w16(1);
	self.w16((ushort)(np + (usz)(text != null ? 1 : 0)));
	for (usz i = 0; i < np; i++)
	{
		self.w32(8);
		self.w64((ulong)params[i]);
	}
	if (text != null)
	{
		self.w32((uint)tl);
		self.wbytes(text, tl);
	}
	self.w16(1);  // one result format code: binary
	self.w16(1);
	self.end(at);
	at = self.msg('E');
	self.w8(0);
	self.w32(0);
	self.end(at);
}

fn void PgConn.sync(&self)
{
	usz at = self.msg('S');
	self.end(at);
}

// ---------- incoming messages ----------
fn void PgConn.grow_rb(&self)
{
	usz nc = self.rcap != 0 ? self.rcap * 2 : 256 * 1024;
	self.rb = realloc(self.rb, nc);
	self.rcap = nc;
}

// The next complete message at rpos, if any. Garbage comes back as type 0.
fn bool PgConn.peek(&self, char* type, char** body, usz* blen)
{
	if (self.rlen - self.rpos < 5) return false;
	usz len = (usz)be32(self.rb + self.rpos + 1);
	if (len < 4 || len > 64 * 1024 * 1024)
	{
		self.rlen = self.rpos;
		*type = 0;
		return true;
	}
	if (self.rlen - self.rpos < 1 + len) return false;
	*type = self.rb[self.rpos];
	*body = self.rb + self.rpos + 5;
	*blen = len - 4;
	return true;
}

fn void PgConn.advance(&self) @inline
{
	self.rpos += 1 + (usz)be32(self.rb + self.rpos + 1);
}

// Drops parsed bytes, but never the rows of a statement still in progress.
fn void PgConn.compact(&self)
{
	usz drop = self.in_rows && self.rowstart < self.rpos ? self.rowstart : self.rpos;
	if (drop == 0) return;
	if (drop == self.rlen)
	{
		self.rlen = self.rpos = 0;
		self.rowstart = 0;
		return;
	}
	memmove(self.rb, self.rb + drop, self.rlen - drop);
	self.rlen -= drop;
	self.rpos -= drop;
	self.rowstart -= drop;
}

// Blocking read of one message during setup (the socket has a 3 s timeout then).
fn bool PgConn.wait_msg(&self, char* type, char** body, usz* blen)
{
	while (!self.peek(type, body, blen))
	{
		if (self.rcap - self.rlen < 16 * 1024) self.grow_rb();
		sz r = read(self.fd, self.rb + self.rlen, self.rcap - self.rlen);
		if (r <= 0) return false;
		self.rlen += (usz)r;
	}
	return *type != 0;
}

fn bool PgConn.send_all(&self)
{
	while (self.woff < self.wlen)
	{
		sz w = write(self.fd, self.wb + self.woff, self.wlen - self.woff);
		if (w <= 0) return false;
		self.woff += (usz)w;
	}
	self.wlen = self.woff = 0;
	return true;
}

// ErrorResponse fields: a code byte and a C string each; 'C' is the SQLSTATE, 'M' the message.
fn void parse_error(char* body, usz n, PgRes* r)
{
	r.sqlstate = {};
	r.msg = "?";
	r.msglen = 1;
	char* p = body;
	char* e = body + n;
	while (p < e && *p != 0)
	{
		char code = *p++;
		char* s = p;
		while (p < e && *p != 0) p++;
		if (code == 'C' && p - s == 5) memcpy(&r.sqlstate, s, 5);
		if (code == 'M')
		{
			r.msg = s;
			r.msglen = (usz)(p - s);
		}
		p++;
	}
}

fn String err_msg(char* body, usz n)
{
	PgRes r;
	parse_error(body, n, &r);
	return (String)r.msg[:r.msglen];
}

// ---------- connecting ----------
fn String urldec(String s)
{
	usz o = 0;
	char* d = malloc(s.len + 1);
	for (usz i = 0; i < s.len; i++)
	{
		if (s[i] == '%' && i + 2 < s.len && hexv(s[i + 1]) >= 0 && hexv(s[i + 2]) >= 0)
		{
			d[o++] = (char)(hexv(s[i + 1]) << 4 | hexv(s[i + 2]));
			i += 2;
		}
		else
		{
			d[o++] = s[i];
		}
	}
	d[o] = 0;
	return (String)d[:o];
}

fn char* zstr(String s)
{
	char* z = malloc(s.len + 1);
	memcpy(z, s.ptr, s.len);
	z[s.len] = 0;
	return z;
}

// postgres://user:pass@host:port/db?... (user and password percent-decoded)
fn bool parse_dburl(String u)
{
	if (u.len > 11 && u[:11] == "postgres://")
	{
		u = u[11..];
	}
	else if (u.len > 13 && u[:13] == "postgresql://")
	{
		u = u[13..];
	}
	else
	{
		return false;
	}
	usz q = u.len;
	foreach (i, c : u)
	{
		if (c == '?')
		{
			q = (usz)i;
			break;
		}
	}
	u = u[:q];
	usz at = u.len;
	for (usz i = u.len; i > 0; i--)
	{
		if (u[i - 1] == '@')
		{
			at = i - 1;
			break;
		}
	}
	String cred = at < u.len ? u[:at] : "";
	String rest = at < u.len ? u[at + 1..] : u;
	usz colon = cred.len;
	foreach (i, c : cred)
	{
		if (c == ':')
		{
			colon = (usz)i;
			break;
		}
	}
	pg_user = urldec(cred[:colon]);
	pg_pass = colon < cred.len ? urldec(cred[colon + 1..]) : "";
	usz slash = rest.len;
	foreach (i, c : rest)
	{
		if (c == '/')
		{
			slash = (usz)i;
			break;
		}
	}
	String hp = rest[:slash];
	pg_db = slash < rest.len ? rest[slash + 1..] : "";
	usz pc = hp.len;
	foreach (i, c : hp)
	{
		if (c == ':')
		{
			pc = (usz)i;
			break;
		}
	}
	pg_host = hp[:pc];
	pg_port = pc < hp.len ? hp[pc + 1..] : "5432";
	if (pg_host.len == 0) pg_host = "localhost";
	if (pg_db.len == 0) pg_db = pg_user;
	pg_host_z = zstr(pg_host);
	pg_port_z = zstr(pg_port);
	return pg_user.len > 0;
}

fn int pg_socket()
{
	AddrInfo hints = { .family = AF_INET, .socktype = SOCK_STREAM };
	AddrInfo* res;
	if (getaddrinfo(pg_host_z, pg_port_z, &hints, &res) != 0)
	{
		io::eprintfn("connect: cannot resolve %s", pg_host);
		return -1;
	}
	int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
	TimeVal tv = { 3, 0 };  // a down database stalls this loop for at most 3 s a try
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, TimeVal::size);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, TimeVal::size);
	int one = 1;
	setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, 4);
	int rc = connect(fd, res.addr, res.addrlen);
	freeaddrinfo(res);
	if (rc != 0)
	{
		io::eprintfn("connect: %s:%s errno %d", pg_host, pg_port, errno());
		close(fd);
		return -1;
	}
	return fd;
}

// SCRAM-SHA-256 (RFC 7677), no channel binding: the box runs ssl = off. Called on AuthenticationSASL;
// consumes the AuthenticationSASLContinue too.
fn bool PgConn.scram(&self)
{
	char[18] rnd;
	getrandom(&rnd, 18, 0);
	char[24] nonce;
	base64::encode_into(nonce[..], rnd[..]);

	// client-first-message-bare = n=<user>,r=<nonce>
	char[256] cfb;
	usz cfl = 0;
	if (pg_user.len > 200) return false;
	memcpy(&cfb[cfl], "n=".ptr, 2);
	cfl += 2;
	memcpy(&cfb[cfl], pg_user.ptr, pg_user.len);
	cfl += pg_user.len;
	memcpy(&cfb[cfl], ",r=".ptr, 3);
	cfl += 3;
	memcpy(&cfb[cfl], &nonce, 24);
	cfl += 24;
	usz at = self.msg('p');
	self.wstr("SCRAM-SHA-256");
	self.w32((uint)(cfl + 3));
	self.wbytes("n,,".ptr, 3);
	self.wbytes(&cfb, cfl);
	self.end(at);
	if (!self.send_all()) return false;

	// server-first-message: r=...,s=...,i=...
	char type;
	char* body;
	usz blen;
	if (!self.wait_msg(&type, &body, &blen) || type != 'R' || blen < 4 || be32(body) != 11) return false;
	String sf = (String)body[4:blen - 4];
	String rn;
	String salt;
	int iters = 0;
	usz i = 0;
	while (i < sf.len)
	{
		usz j = i;
		while (j < sf.len && sf[j] != ',') j++;
		String f = sf[i..j - 1];
		if (f.len > 2 && f[0] == 'r') rn = f[2..];
		if (f.len > 2 && f[0] == 's') salt = f[2..];
		if (f.len > 2 && f[0] == 'i')
		{
			iters = 0;
			foreach (c : f[2..]) iters = c >= '0' && c <= '9' ? iters * 10 + (c - '0') : -1;
		}
		i = j + 1;
	}
	if (rn.len < 24 || memcmp(rn.ptr, &nonce, 24) != 0 || salt.len == 0 || iters <= 0) return false;
	char[64] saltbuf;
	char[]? sb = base64::decode_into(saltbuf[..], salt);
	if (catch sb) return false;
	char[32] salted;
	sha256::pbkdf2(pg_pass, sb, iters, salted[..]);
	char[32] client_key = sha256::hmac(salted[..], "Client Key");
	char[32] stored_key = sha256::hash(client_key[..]);

	// client-final-message-without-proof = c=biws,r=<server nonce>
	char[512] cfw;
	usz cwl = 0;
	if (rn.len > 400) return false;
	memcpy(&cfw[cwl], "c=biws,r=".ptr, 9);
	cwl += 9;
	memcpy(&cfw[cwl], rn.ptr, rn.len);
	cwl += rn.len;

	// auth-message = client-first-bare , server-first , client-final-without-proof
	HmacSha256 h;
	h.init(stored_key[..]);
	h.update(cfb[:cfl]);
	h.update(",");
	h.update(sf);
	h.update(",");
	h.update(cfw[:cwl]);
	char[32] sig = h.final();
	char[32] proof;
	for (usz k = 0; k < 32; k++) proof[k] = client_key[k] ^ sig[k];
	char[44] pb64;
	base64::encode_into(pb64[..], proof[..]);
	at = self.msg('p');
	self.wbytes(&cfw, cwl);
	self.wbytes(",p=".ptr, 3);
	self.wbytes(&pb64, 44);
	self.end(at);
	if (!self.send_all()) return false;
	self.advance();
	return true;
}

fn void hex16(char[16]* d, char[32]* out)
{
	for (usz i = 0; i < 16; i++)
	{
		(*out)[2 * i] = HEXD[(*d)[i] >> 4];
		(*out)[2 * i + 1] = HEXD[(*d)[i] & 15];
	}
}

// "md5" + hex(md5(hex(md5(password + user)) + salt))
fn bool PgConn.md5auth(&self, char* salt)
{
	Md5 m;
	m.init();
	m.update(pg_pass);
	m.update(pg_user);
	char[16] a = m.final();
	char[32] ah;
	hex16(&a, &ah);
	char[36] b;
	memcpy(&b, &ah, 32);
	memcpy(&b[32], salt, 4);
	char[16] c = md5::hash(b[..]);
	char[32] ch;
	hex16(&c, &ch);
	usz at = self.msg('p');
	self.wbytes("md5".ptr, 3);
	self.wbytes(&ch, 32);
	self.w8(0);
	self.end(at);
	return self.send_all();
}

// StartupMessage, authentication, then wait for ReadyForQuery.
fn bool PgConn.setup(&self)
{
	usz at = self.msg(0);
	self.w32(196608);
	self.wstr("user");
	self.wstr(pg_user);
	self.wstr("database");
	self.wstr(pg_db);
	self.w8(0);
	self.end(at);
	if (!self.send_all())
	{
		io::eprintn("connect: send failed");
		return false;
	}
	char type;
	char* body;
	usz blen;
	while (true)
	{
		if (!self.wait_msg(&type, &body, &blen))
		{
			io::eprintn("connect: no reply from the server");
			return false;
		}
		switch (type)
		{
			case 'R':
				int code = blen >= 4 ? be32(body) : -1;
				switch (code)
				{
					case 0:
						break;
					case 3:
						at = self.msg('p');
						self.wstr(pg_pass);
						self.end(at);
						if (!self.send_all()) return false;
					case 5:
						if (blen < 8 || !self.md5auth(body + 4)) return false;
					case 10:
						self.advance();
						if (!self.scram())
						{
							io::eprintn("connect: SCRAM-SHA-256 authentication failed");
							return false;
						}
						continue;  // scram() consumed the SASLContinue too
					case 11:
						return false;  // SASL continue without a SASL start
					case 12:
						break;  // SASLFinal: the server's signature; trusted, the link is private
					default:
						io::eprintfn("connect: unsupported auth method %d", code);
						return false;
				}
			case 'E':
				io::eprintfn("connect: %s", err_msg(body, blen));
				return false;
			case 'Z':
				self.advance();
				self.compact();
				return true;
			default:
				break;  // ParameterStatus, BackendKeyData, NoticeResponse
		}
		self.advance();
	}
}

struct St
{
	String name;
	String sql;
	uint[2] oids;
	usz np;
}

// Parse the five statements, then wait for ReadyForQuery.
fn bool PgConn.prepare_all(&self)
{
	St[5] st = {
		{ "f", SQL_FEED, { 0, 0 }, 0 },
		{ "p", SQL_POST, { OID_INT8, 0 }, 1 },
		{ "i", SQL_INS, { OID_INT8, OID_TEXT }, 2 },
		{ "l", SQL_LIKE, { OID_INT8, OID_INT8 }, 2 },
		{ "h", "SELECT 1", { 0, 0 }, 0 },
	};
	foreach (&s : st)
	{
		usz at = self.msg('P');
		self.wstr(s.name);
		self.wstr(s.sql);
		self.w16((ushort)s.np);
		for (usz i = 0; i < s.np; i++) self.w32(s.oids[i]);
		self.end(at);
	}
	self.sync();
	if (!self.send_all()) return false;
	char type;
	char* body;
	usz blen;
	while (true)
	{
		if (!self.wait_msg(&type, &body, &blen))
		{
			io::eprintn("prepare: no reply from the server");
			return false;
		}
		if (type == 'E')
		{
			io::eprintfn("prepare: %s", err_msg(body, blen));
			return false;
		}
		self.advance();
		if (type == 'Z')
		{
			self.compact();
			return true;
		}
	}
}

fn bool pg_connect(Th* t, int i)
{
	PgConn* p = &t.pg[i];
	int fd = pg_socket();
	if (fd < 0) return false;
	p.fd = fd;
	p.rlen = p.rpos = p.wlen = p.woff = 0;
	if (p.rb == null) p.grow_rb();
	if (!p.setup() || !p.prepare_all())
	{
		close(fd);
		return false;
	}
	TimeVal tv = { 0, 0 };
	setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, TimeVal::size);
	setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, TimeVal::size);
	set_nonblock(fd);
	p.up = true;
	p.has_held = p.in_rows = false;
	ep_set(t.ep, EPOLL_CTL_ADD, fd, TAG_PG | (ulong)i, EPOLLIN);
	return true;
}

// ---------- the ring of statements in flight ----------
fn Qe* PgConn.push(&self)
{
	if (self.n == self.cap)
	{
		uint nc = self.cap != 0 ? self.cap * 2 : 256;
		Qe* nq = malloc(nc * Qe::size);
		for (uint k = 0; k < self.n; k++) nq[k] = self.q[(self.head + k) & (self.cap - 1)];
		free(self.q);
		self.q = nq;
		self.cap = nc;
		self.head = 0;
	}
	return &self.q[(self.head + self.n++) & (self.cap - 1)];
}

fn Qe PgConn.pop(&self)
{
	Qe q = self.q[self.head];
	self.head = (self.head + 1) & (self.cap - 1);
	self.n--;
	return q;
}

// The connection is gone: every request on it gets a 500.
fn void pg_fail(Th* t, PgConn* p)
{
	io::eprintfn("postgres connection lost");
	epoll_ctl(t.ep, EPOLL_CTL_DEL, p.fd, null);
	close(p.fd);
	p.up = false;
	PgRes r = { .status = 'F' };
	if (p.has_held)
	{
		p.has_held = false;
		respond(t, &p.held, &r);
	}
	while (p.n != 0)
	{
		Qe q = p.pop();
		if (q.kind != Q_SYNC) respond(t, &q, &r);
	}
	p.dirty = p.unsynced = p.wout = false;
	p.inbatch = 0;
	p.rlen = p.rpos = p.wlen = p.woff = 0;
	p.in_rows = false;
}

// Reads fill one connection's batch for this loop iteration (up to PACK statements), so a batch is one
// write, one round trip and one Sync on one connection; the next batch goes to the connection with the
// shortest queue. Writes get their own connections (the last quarter), spread by queue length, so a slow
// commit never holds up the reads pipelined behind it.
fn PgConn* pick(Th* t, bool is_write)
{
	PgConn* b = null;
	int nw = t.npg / 4 != 0 ? t.npg / 4 : 1;
	for (int i = 0; i < t.npg; i++)
	{
		PgConn* p = &t.pg[i];
		if (!p.up && t.retry_at != now_s)  // reconnect one lost connection a second
		{
			t.retry_at = now_s;
			pg_connect(t, i);
		}
		if (!p.up || (i >= t.npg - nw) != is_write) continue;
		if (!is_write && p.dirty && p.inbatch < PACK) return p;
		if (b == null || p.n < b.n) b = p;
	}
	return b;
}

fn void pg_sync(PgConn* p)
{
	p.sync();
	Qe* q = p.push();
	*q = {};
	q.kind = Q_SYNC;
	p.unsynced = false;
}

// Sends one statement. Returns false if no connection is available (the caller answers 500/503).
fn bool enqueue(Th* t, int fd, char kind, bool do_close, long id, long uid, char* s, usz sl, char* un, usz unl)
{
	PgConn* p = pick(t, kind == Q_INS || kind == Q_LIKE);
	if (p == null) return false;
	long[2] a = { uid, id };
	switch (kind)
	{
		case Q_FEED: p.exec("f", null, 0, null, 0);
		case Q_HEALTH: p.exec("h", null, 0, null, 0);
		case Q_POST: p.exec("p", &a[1], 1, null, 0);
		case Q_LIKE: p.exec("l", &a, 2, null, 0);
		default: p.exec("i", &a, 1, s, sl);
	}
	Qe* q = p.push();
	*q = { .fd = fd, .gen = conns[fd].gen, .kind = kind, .close = do_close, .gz = want_gz, .id = id };
	if (kind == Q_INS)  // the response echoes the body and the token's username
	{
		q.extra = malloc(sl + unl + 1);
		memcpy(q.extra, s, sl);
		memcpy(q.extra + sl, un, unl);
		q.blen = (uint)sl;
		q.ulen = (uint)unl;
	}
	p.dirty = true;
	p.unsynced = true;
	p.inbatch++;
	if (kind == Q_INS || kind == Q_LIKE) pg_sync(p);
	return true;
}

fn void pg_flush(Th* t, PgConn* p)
{
	while (p.woff < p.wlen)
	{
		sz w = write(p.fd, p.wb + p.woff, p.wlen - p.woff);
		if (w < 0)
		{
			if (errno() == EAGAIN) break;
			pg_fail(t, p);
			return;
		}
		p.woff += (usz)w;
	}
	bool pending = p.woff < p.wlen;
	if (!pending) p.wlen = p.woff = 0;
	if (pending != p.wout)
	{
		p.wout = pending;
		ep_set(t.ep, EPOLL_CTL_MOD, p.fd, TAG_PG | (ulong)(p - t.pg), EPOLLIN | (pending ? EPOLLOUT : 0));
	}
}

// Field i of a DataRow (row on its 'D' byte): pointer and length.
fn char* row_field(char* row, int i, int* len)
{
	char* p = row + 7;  // 'D', int32 len, int16 ncols
	for (int k = 0; k < i; k++)
	{
		int l = be32(p);
		p += 4 + (sz)(l > 0 ? l : 0);
	}
	*len = be32(p);
	return p + 4;
}

fn char* row_next(char* row) @inline => row + 1 + be32(row + 1);

// Reads every result that has arrived. Statements and Syncs come back in the order they were sent.
fn void pg_read(Th* t, PgConn* p)
{
	if (p.rcap - p.rlen < 64 * 1024) p.grow_rb();
	sz r = read(p.fd, p.rb + p.rlen, p.rcap - p.rlen);
	if (r <= 0)
	{
		if (r < 0 && errno() == EAGAIN) return;
		pg_fail(t, p);
		return;
	}
	p.rlen += (usz)r;
	char type;
	char* body;
	usz blen;
	while (p.up && p.peek(&type, &body, &blen))
	{
		switch (type)
		{
			case 0:
				pg_fail(t, p);
				return;
			case '2':  // BindComplete: rows follow
				p.rowstart = p.rpos + 1 + (usz)be32(p.rb + p.rpos + 1);
				p.nrows = 0;
				p.in_rows = true;
			case 'D':
				p.nrows++;
			case 'C':  // CommandComplete: the statement at the head is done
				p.in_rows = false;
				if (p.n == 0 || p.q[p.head].kind == Q_SYNC)
				{
					pg_fail(t, p);
					return;
				}
				Qe q = p.pop();
				char* rows = p.rb + p.rowstart;
				if (q.kind == Q_INS || q.kind == Q_LIKE)  // answered after the commit, at ReadyForQuery
				{
					p.held = q;
					p.has_held = true;
					if (q.kind == Q_INS && p.nrows == 1)
					{
						int l;
						p.held_a = be64(row_field(rows, 0, &l));
						p.held_b = be64(row_field(rows, 1, &l));
					}
					else
					{
						p.held_a = 0;
					}
					p.held_created = blen >= 2 && body[blen - 2] == '1';  // "INSERT 0 1\0" vs "INSERT 0 0\0"
				}
				else
				{
					PgRes res = { .status = 'T', .rows = rows, .nrows = p.nrows };
					respond(t, &q, &res);
				}
			case 'E':  // ErrorResponse: the statement at the head failed; the rest of its batch is skipped until the Sync
				p.in_rows = false;
				PgRes res;
				parse_error(body, blen, &res);
				res.status = 'E';
				if (p.n != 0 && p.q[p.head].kind != Q_SYNC)
				{
					Qe q = p.pop();
					respond(t, &q, &res);
				}
			case 'Z':  // ReadyForQuery: the Sync at the head is done; a skipped batch is failed here
				PgRes lost = { .status = 'F' };
				while (p.n != 0 && p.q[p.head].kind != Q_SYNC)
				{
					Qe q = p.pop();
					respond(t, &q, &lost);
				}
				if (p.n != 0) p.pop();
				if (p.has_held)  // committed: now the write may be answered
				{
					p.has_held = false;
					PgRes res = { .status = 'C', .id = p.held_a, .ts = p.held_b, .created = p.held_created };
					if (p.held.kind == Q_INS && p.held_a == 0) res.status = 'F';
					respond(t, &p.held, &res);
				}
			default:
				break;  // NoticeResponse, ParameterStatus, NotificationResponse
		}
		p.advance();
	}
	if (p.up) p.compact();
}
