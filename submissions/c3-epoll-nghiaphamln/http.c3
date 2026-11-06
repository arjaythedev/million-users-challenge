// Connections and HTTP/1.1: requests are parsed in place, one statement is sent per request, and the
// response is built in the thread's arena once the result arrives. Pipelined requests behind a busy one
// wait.
module server;

const usz RBUF = 64 * 1024;
const usz MAXREQ = 64 * 1024;  // header + body cap per request

// One per descriptor, so a million idle keep-alive connections cost 40 MB here. Buffers exist only
// while a request or response is split across reads or writes.
struct Conn
{
	char* in;      // unparsed request bytes carried between reads
	uint inlen;
	uint incap;
	char* out;     // unsent response bytes after a short write
	uint outlen;
	uint outoff;
	uint gen;      // bumped on close, so a late query result is dropped
	bool open;
	bool busy;     // waiting for its query; requests after it wait too
	bool closing;
}

Conn* conns;
long maxfd;
tlocal char* rbuf;
tlocal char* txt;  // MAXREQ: the decoded body of a POST /posts

fn void conn_close(int fd)
{
	Conn* c = &conns[fd];
	free(c.in);
	free(c.out);
	uint g = c.gen + 1;
	*c = {};
	c.gen = g;
	close(fd);  // also leaves the epoll set
}

// Sends arena[0..alen) on fd: whatever doesn't fit in the socket buffer waits for EPOLLOUT.
fn void send_resp(Th* t, int fd, bool do_close)
{
	Conn* c = &conns[fd];
	sz w = write(fd, arena, alen);
	if (w < 0)
	{
		if (errno() != EAGAIN)
		{
			conn_close(fd);
			return;
		}
		w = 0;
	}
	if (do_close) c.closing = true;
	if ((usz)w < alen)
	{
		c.outlen = (uint)(alen - (usz)w);
		c.outoff = 0;
		c.out = malloc(c.outlen);
		memcpy(c.out, arena + w, c.outlen);
		ep_set(t.ep, EPOLL_CTL_MOD, fd, (ulong)fd, EPOLLOUT);
	}
	else if (do_close)
	{
		conn_close(fd);
	}
}

// A DataRow of the post query: id, body, created_at, username, like_count.
fn void put_post_row(char* row)
{
	int l;
	puts("{\"id\":");
	char* id = row_field(row, 0, &l);
	put(id, (usz)l);
	char* body = row_field(row, 1, &l);
	puts(",\"body\":\"");
	putjs(body, (usz)l);
	puts("\",\"created_at\":\"");
	put_ts(be64(row_field(row, 2, &l)));
	char* user = row_field(row, 3, &l);
	puts("\",\"author\":\"");
	putjs(user, (usz)l);
	puts("\",\"like_count\":");
	char* count = row_field(row, 4, &l);
	put(count, (usz)l);
	puts("}");
}

// Answers a request whose statement finished, then serves any request queued behind it.
fn void respond(Th* t, Qe* q, PgRes* r)
{
	int fd = q.fd;
	Conn* c = &conns[fd];
	if (q.gen != c.gen || !c.open)  // client left
	{
		free(q.extra);
		return;
	}
	bool do_close = q.close;
	want_gz = q.gz;
	switch (q.kind)
	{
		case Q_FEED:
			if (r.status != 'T')
			{
				err(500, "internal server error", do_close);
				break;
			}
			begin_resp(200, do_close);
			puts("{\"posts\":[");
			char* row = r.rows;
			for (uint i = 0; i < r.nrows; i++)
			{
				if (i != 0) puts(",");
				put_post_row(row);
				row = row_next(row);
			}
			puts("]}");
			finish();
		case Q_POST:
			if (r.status != 'T')
			{
				err(500, "internal server error", do_close);
			}
			else if (r.nrows == 0)
			{
				err(404, "post not found", do_close);
			}
			else
			{
				begin_resp(200, do_close);
				puts("{\"post\":");
				put_post_row(r.rows);
				puts("}");
				finish();
			}
		case Q_INS:
			if (r.status != 'C')
			{
				err(500, "internal server error", do_close);
				break;
			}
			begin_resp(201, do_close);
			puts("{\"post\":{\"id\":");
			putu((ulong)r.id);
			puts(",\"body\":\"");
			putjs(q.extra, q.blen);
			puts("\",\"created_at\":\"");
			put_ts(r.ts);
			puts("\",\"author\":\"");
			putjs(q.extra + q.blen, q.ulen);
			puts("\",\"like_count\":0}}");
			finish();
		case Q_LIKE:
			if (r.status == 'C')
			{
				begin_resp(r.created ? 201 : 200, do_close);
				puts("{\"liked\":true,\"already_liked\":");
				if (r.created)
				{
					puts("false");
				}
				else
				{
					puts("true");
				}
				puts(",\"post_id\":");
				putu((ulong)q.id);
				puts("}");
				finish();
			}
			else if (r.status == 'E' && (String)r.sqlstate[..] == "23503")
			{
				err(404, "post not found", do_close);
			}
			else
			{
				err(500, "internal server error", do_close);
			}
		case Q_HEALTH:
			if (r.status == 'T')
			{
				begin_resp(200, do_close);
				puts("{\"status\":\"ok\",\"db\":\"ok\",\"uptime_s\":");
				putu((ulong)(now_s - started));
				puts("}");
				finish();
			}
			else
			{
				begin_resp(503, do_close);
				puts("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"");
				if (r.status == 'E')
				{
					putjs(r.msg, r.msglen);
				}
				else
				{
					puts("connection lost");
				}
				puts("\"}");
				finish();
			}
		default:
			err(500, "internal server error", do_close);
	}
	free(q.extra);
	c.busy = false;
	send_resp(t, fd, do_close);
	if (c.open && c.out == null && !c.closing && c.inlen != 0) process(t, fd, c.in, c.inlen);
}

// ---------- request handling ----------

// A positive decimal integer, else -1 (huge ids map to 0: not found).
fn long parse_id(char* s, usz n)
{
	if (n == 0) return -1;
	long v = 0;
	for (usz i = 0; i < n; i++)
	{
		if (s[i] < '0' || s[i] > '9') return -1;
		if (v < 100000000000000000)
		{
			v = v * 10 + (long)(s[i] - '0');
		}
		else
		{
			v = 0x7fffffffffffffff;
		}
	}
	return v > 0 ? v : -1;
}

// Byte length of JS String.trim whitespace at p, or 0.
fn int jsws_at(char* p, char* e)
{
	if (*p == ' ' || (*p >= 9 && *p <= 13)) return 1;
	if (e - p >= 2 && p[0] == 0xc2 && p[1] == 0xa0) return 2;
	if (e - p >= 3)
	{
		uint c = (uint)p[0] << 16 | (uint)p[1] << 8 | (uint)p[2];
		if (c == 0xe19a80 || (c >= 0xe28080 && c <= 0xe2808a) || c == 0xe280a8 || c == 0xe280a9 || c == 0xe280af
			|| c == 0xe2819f || c == 0xe38080 || c == 0xefbbbf) return 3;
	}
	return 0;
}

fn int jsws_before(char* s, char* e)
{
	if (e[-1] == ' ' || (e[-1] >= 9 && e[-1] <= 13)) return 1;
	if (e - s >= 2 && e[-2] == 0xc2 && e[-1] == 0xa0) return 2;
	if (e - s >= 3 && jsws_at(e - 3, e) == 3) return 3;
	return 0;
}

// Answers into the arena, or sends a statement and marks the connection busy.
fn void handle(Th* t, int fd, char* m, usz ml, char* path, usz pl, char* authh, usz al, char* body, usz bl, bool do_close)
{
	char* qm = memchr(path, '?', pl);
	if (qm != null) pl = (usz)(qm - path);
	bool get = ml == 3 && memcmp(m, "GET".ptr, 3) == 0;
	bool post = ml == 4 && memcmp(m, "POST".ptr, 4) == 0;
	int kind = -1;
	long id = 0;
	long uid = 0;
	char[1024] un;
	usz unl = 0;
	char* s = null;
	char* se = null;
	if (get && pl == 5 && memcmp(path, "/feed".ptr, 5) == 0)
	{
		kind = Q_FEED;
	}
	else if (get && pl == 7 && memcmp(path, "/health".ptr, 7) == 0)
	{
		kind = Q_HEALTH;
	}
	else if (pl > 7 && memcmp(path, "/posts/".ptr, 7) == 0)
	{
		char* seg = path + 7;
		char* sl = memchr(seg, '/', pl - 7);
		if (get && sl == null)
		{
			if ((id = parse_id(seg, pl - 7)) < 0)
			{
				err(400, "invalid post id", do_close);
				return;
			}
			kind = Q_POST;
		}
		else if (post && sl != null && (usz)(path + pl - sl) == 5 && memcmp(sl, "/like".ptr, 5) == 0)
		{
			String e = auth(authh, al, &uid, &un, &unl);
			if (e.len != 0)
			{
				err(401, e, do_close);
				return;
			}
			if ((id = parse_id(seg, (usz)(sl - seg))) < 0)
			{
				err(400, "invalid post id", do_close);
				return;
			}
			kind = Q_LIKE;
		}
	}
	else if (post && pl == 6 && memcmp(path, "/posts".ptr, 6) == 0)
	{
		String e = auth(authh, al, &uid, &un, &unl);
		if (e.len != 0)
		{
			err(401, e, do_close);
			return;
		}
		Want[1] w = { { "body", null, null } };
		if (!jdoc(body, bl, &w))
		{
			err(400, "malformed JSON body", do_close);
			return;
		}
		if (w[0].v == null || *w[0].v != '"')
		{
			err(400, "body is required", do_close);
			return;
		}
		Js bj = { w[0].v, w[0].ve };
		long n = jstr(&bj, txt, MAXREQ);
		if (n < 0)
		{
			err(400, "malformed JSON body", do_close);
			return;
		}
		s = txt;
		se = s + n;
		int l;
		while (s < se && (l = jsws_at(s, se)) != 0) s += l;
		while (se > s && (l = jsws_before(s, se)) != 0) se -= l;
		if (s == se)
		{
			err(400, "body is required", do_close);
			return;
		}
		usz chars = 0;
		for (char* c = s; c < se; c++)
		{
			if ((*c & 0xc0) != 0x80) chars++;
		}
		if (chars > 500)
		{
			err(400, "body must be at most 500 characters", do_close);
			return;
		}
		kind = Q_INS;
	}
	if (kind < 0)
	{
		err(404, "not found", do_close);
		return;
	}
	if (enqueue(t, fd, (char)kind, do_close, id, uid, s, (usz)(se - s), &un, unl))
	{
		conns[fd].busy = true;
		return;
	}
	if (kind != Q_HEALTH)
	{
		err(500, "internal server error", do_close);
		return;
	}
	begin_resp(503, do_close);
	puts("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"no database connection\"}");
	finish();
}

// "gzip" listed, and not with q=0.
fn bool accepts_gzip(char* v, char* ve)
{
	char* g = memmem(v, (usz)(ve - v), "gzip".ptr, 4);
	if (g == null) return false;
	for (g += 4; g < ve && *g == ' '; g++) {}
	if (g + 2 < ve && *g == ';')
	{
		g = memmem(g, (usz)(ve - g), "q=".ptr, 2);
		if (g != null)
		{
			char[8] b = {};
			usz n = (usz)(ve - g - 2);
			memcpy(&b, g + 2, n < 7 ? n : 7);
			return strtod(&b, null) > 0;
		}
	}
	return true;
}

fn bool hdr_is(char* p, usz n, String name) @inline
{
	return n == name.len && strncasecmp(p, name.ptr, n) == 0;
}

// Serves complete requests in buf until one waits for Postgres. Returns bytes consumed, or -1 to drop
// the connection.
fn long serve(Th* t, int fd, char* buf, usz n)
{
	Conn* c = &conns[fd];
	usz used = 0;
	while (used < n && c.open && !c.busy && !c.closing && c.out == null)
	{
		char* p = buf + used;
		char* e = buf + n;
		char* he = memmem(p, (usz)(e - p), "\r\n\r\n".ptr, 4);
		if (he == null)
		{
			if ((usz)(e - p) > MAXREQ) return -1;
			break;
		}
		char* le = memchr(p, '\r', (usz)(he + 2 - p));
		char* m = p;
		char* sp1 = memchr(m, ' ', (usz)(le - m));
		if (sp1 == null) return -1;
		char* path = sp1 + 1;
		char* sp2 = memchr(path, ' ', (usz)(le - path));
		if (sp2 == null) return -1;
		bool http10 = le - sp2 - 1 == 8 && memcmp(sp2 + 1, "HTTP/1.0".ptr, 8) == 0;
		char* authh = null;
		usz al = 0;
		usz clen = 0;
		bool do_close = http10;
		bool expect = false;
		want_gz = false;
		for (char* h = le + 2; h < he + 2;)
		{
			char* hl = memchr(h, '\r', (usz)(he + 2 - h));
			char* col = memchr(h, ':', (usz)(hl - h));
			if (col != null)
			{
				char* v = col + 1;
				while (v < hl && (*v == ' ' || *v == '\t')) v++;
				char* ve = hl;
				while (ve > v && (ve[-1] == ' ' || ve[-1] == '\t')) ve--;
				usz kn = (usz)(col - h);
				if (hdr_is(h, kn, "content-length"))
				{
					clen = 0;
					for (char* d = v; d < ve; d++)
					{
						if (*d < '0' || *d > '9') return -1;
						clen = clen * 10 + (usz)(*d - '0');
						if (clen > MAXREQ) return -1;
					}
				}
				else if (hdr_is(h, kn, "authorization"))
				{
					authh = v;
					al = (usz)(ve - v);
				}
				else if (hdr_is(h, kn, "connection"))
				{
					if (ve - v == 5 && strncasecmp(v, "close", 5) == 0)
					{
						do_close = true;
					}
					else if (ve - v == 10 && strncasecmp(v, "keep-alive", 10) == 0)
					{
						do_close = false;
					}
				}
				else if (hdr_is(h, kn, "accept-encoding"))
				{
					want_gz = accepts_gzip(v, ve);
				}
				else if (hdr_is(h, kn, "expect"))
				{
					expect = ve - v == 12 && strncasecmp(v, "100-continue", 12) == 0;
				}
				else if (hdr_is(h, kn, "transfer-encoding"))
				{
					return -1;  // chunked uploads aren't supported
				}
			}
			h = hl + 2;
		}
		char* body = he + 4;
		if ((usz)(e - body) < clen)
		{
			if (expect && body == e)
			{
				String cont = "HTTP/1.1 100 Continue\r\n\r\n";
				if (write(fd, cont.ptr, cont.len) < 0) return -1;
			}
			break;
		}
		used = (usz)(body + clen - buf);
		handle(t, fd, m, (usz)(sp1 - m), path, (usz)(sp2 - path), authh, al, body, clen, do_close);
		if (!c.busy) send_resp(t, fd, do_close);
	}
	return (long)used;
}

fn void process(Th* t, int fd, char* buf, usz n)
{
	Conn* c = &conns[fd];
	long used = serve(t, fd, buf, n);
	if (!c.open) return;  // closed while serving (Connection: close); buf may be freed
	if (used < 0)
	{
		conn_close(fd);
		return;
	}
	usz left = c.closing ? 0 : n - (usz)used;
	if (left != 0)
	{
		if (buf != c.in)
		{
			c.incap = (uint)(left + RBUF);
			c.in = malloc(c.incap);
			memcpy(c.in, buf + used, left);
		}
		else if (used != 0)
		{
			memmove(c.in, c.in + used, left);
		}
		c.inlen = (uint)left;
	}
	else if (c.in != null)
	{
		free(c.in);
		c.in = null;
		c.inlen = c.incap = 0;
	}
}

fn void on_readable(Th* t, int fd)
{
	Conn* c = &conns[fd];
	sz r;
	if (c.inlen != 0 || c.busy)  // append to the carried bytes; parse them only if nothing is in flight
	{
		if (c.incap - c.inlen < 4096)
		{
			if (c.incap > 4 * MAXREQ)
			{
				conn_close(fd);
				return;
			}
			c.incap = (uint)((usz)c.incap * 2 + RBUF);
			c.in = realloc(c.in, c.incap);
		}
		r = read(fd, c.in + c.inlen, (usz)(c.incap - c.inlen));
		if (r <= 0)
		{
			if (r < 0 && errno() == EAGAIN) return;
			conn_close(fd);
			return;
		}
		c.inlen += (uint)r;
		if (!c.busy) process(t, fd, c.in, c.inlen);
	}
	else
	{
		r = read(fd, rbuf, RBUF);
		if (r <= 0)
		{
			if (r < 0 && errno() == EAGAIN) return;
			conn_close(fd);
			return;
		}
		process(t, fd, rbuf, (usz)r);
	}
}

fn void on_writable(Th* t, int fd)
{
	Conn* c = &conns[fd];
	if (c.out == null) return;
	sz w = write(fd, c.out + c.outoff, (usz)(c.outlen - c.outoff));
	if (w < 0)
	{
		if (errno() == EAGAIN) return;
		conn_close(fd);
		return;
	}
	c.outoff += (uint)w;
	if (c.outoff < c.outlen) return;
	free(c.out);
	c.out = null;
	c.outlen = c.outoff = 0;
	if (c.closing)
	{
		conn_close(fd);
		return;
	}
	ep_set(t.ep, EPOLL_CTL_MOD, fd, (ulong)fd, EPOLLIN);
	if (c.inlen != 0 && !c.busy) process(t, fd, c.in, c.inlen);
}
