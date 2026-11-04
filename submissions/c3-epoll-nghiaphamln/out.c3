// The response arena: one per thread, the response is built in place and sent with one write.
module server;

const usz OBUF = 256 * 1024;  // one response; the largest (20 posts of 500 escaped chars) is ~70 KB
const usz GZMIN = 1024;       // smaller bodies (everything but the feed) go out uncompressed

tlocal char* arena;
tlocal usz alen;
tlocal usz body_start;
tlocal usz cl_pos;
tlocal bool want_gz;
tlocal long now_s;
long started;

fn void put(void* p, usz n) @inline
{
	memcpy(arena + alen, p, n);
	alen += n;
}

macro puts(String $s)
{
	put($s.ptr, $s.len);
}

fn void putu(ulong v)
{
	char[24] b;
	usz i = 24;
	do
	{
		b[--i] = (char)('0' + v % 10);
		v /= 10;
	} while (v != 0);
	put(&b[i], 24 - i);
}

const String HEXD = "0123456789abcdef";

// True if any byte of w needs escaping in a JSON string: < 0x20, '"' or '\\' (SWAR: a byte that
// equals x makes a zero in w ^ x * 0x0101..., and a zero byte sets its high bit in (v - 0x01..) & ~v & 0x80..).
fn bool needs_escape(ulong w) @inline
{
	ulong q = w ^ 0x2222222222222222;
	ulong bs = w ^ 0x5c5c5c5c5c5c5c5c;
	ulong zero = ((q - 0x0101010101010101) & ~q) | ((bs - 0x0101010101010101) & ~bs);
	ulong ctl = w - 0x2020202020202020;  // a byte < 0x20 borrows: its high bit is set unless the byte was >= 0x80
	return ((zero | (ctl & ~w)) & 0x8080808080808080) != 0;
}

// JSON string contents, escaped. Eight bytes are checked at a time; a clean word is copied as is.
fn void putjs(char* s, usz n)
{
	usz run = 0;
	usz i = 0;
	while (i < n)
	{
		if (n - i >= 8)
		{
			ulong w;
			memcpy(&w, s + i, 8);
			if (!needs_escape(w))
			{
				i += 8;
				continue;
			}
		}
		char c = s[i];
		if (c >= 0x20 && c != '"' && c != '\\')
		{
			i++;
			continue;
		}
		put(s + run, i - run);
		run = i + 1;
		char[6] e = { '\\', 0, 0, 0, 0, 0 };
		switch (c)
		{
			case '"':
				e[1] = '"';
				put(&e, 2);
			case '\\':
				e[1] = '\\';
				put(&e, 2);
			case '\n':
				e[1] = 'n';
				put(&e, 2);
			case '\r':
				e[1] = 'r';
				put(&e, 2);
			case '\t':
				e[1] = 't';
				put(&e, 2);
			default:
				e[1] = 'u';
				e[2] = '0';
				e[3] = '0';
				e[4] = HEXD[c >> 4];
				e[5] = HEXD[c & 15];
				put(&e, 6);
		}
		i++;
	}
	put(s + run, n - run);
}

fn void put2(ulong v) @inline
{
	char[2] b = { (char)('0' + v / 10), (char)('0' + v % 10) };
	put(&b, 2);
}

// timestamptz in binary: microseconds since 2000-01-01 UTC -> 2025-12-31T23:59:19.409Z (truncated to ms)
fn void put_ts(long us)
{
	long s = us / 1000000;
	long r = us % 1000000;
	if (r < 0)
	{
		r += 1000000;
		s--;
	}
	s += 946684800;
	long days = s / 86400;
	long sod = s % 86400;
	if (sod < 0)
	{
		sod += 86400;
		days--;
	}
	days += 719468;  // civil_from_days (H. Hinnant)
	long era = (days >= 0 ? days : days - 146096) / 146097;
	ulong doe = (ulong)(days - era * 146097);
	ulong yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	ulong doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	ulong mp = (5 * doy + 2) / 153;
	ulong d = doy - (153 * mp + 2) / 5 + 1;
	ulong m = mp < 10 ? mp + 3 : mp - 9;
	ulong y = yoe + (ulong)(era * 400) + (ulong)(m <= 2 ? 1 : 0);
	put2(y / 100);
	put2(y % 100);
	puts("-");
	put2(m);
	puts("-");
	put2(d);
	puts("T");
	put2((ulong)sod / 3600);
	puts(":");
	put2((ulong)sod / 60 % 60);
	puts(":");
	put2((ulong)sod % 60);
	ulong ms = (ulong)r / 1000;
	char[5] b = { '.', (char)('0' + ms / 100), (char)('0' + ms / 10 % 10), (char)('0' + ms % 10), 'Z' };
	put(&b, 5);
}

fn void begin_resp(int status, bool do_close)
{
	alen = 0;
	switch (status)
	{
		case 200: puts("HTTP/1.1 200 OK\r\n");
		case 201: puts("HTTP/1.1 201 Created\r\n");
		case 400: puts("HTTP/1.1 400 Bad Request\r\n");
		case 401: puts("HTTP/1.1 401 Unauthorized\r\n");
		case 404: puts("HTTP/1.1 404 Not Found\r\n");
		case 503: puts("HTTP/1.1 503 Service Unavailable\r\n");
		default: puts("HTTP/1.1 500 Internal Server Error\r\n");
	}
	if (do_close) puts("Connection: close\r\n");
	puts("Content-Type: application/json\r\nContent-Length:       \r\n\r\n");
	cl_pos = alen - 10;
	body_start = alen;
}

// Fills in Content-Length; with Accept-Encoding: gzip, a large body is compressed in place (still one
// buffer, one write).
fn void finish()
{
	usz n = alen - body_start;
	if (want_gz && n >= GZMIN)
	{
		usz zn = gzip(arena + body_start, n, zbuf, OBUF);
		if (zn != 0)
		{
			alen = cl_pos - 16;  // rewrite from the Content-Length line on
			puts("Content-Encoding: gzip\r\nContent-Length: ");
			putu(zn);
			puts("\r\n\r\n");
			put(zbuf, zn);
			return;
		}
	}
	char* p = arena + cl_pos + 6;
	do
	{
		*--p = (char)('0' + n % 10);
		n /= 10;
	} while (n != 0);
}

fn void err(int status, String msg, bool do_close)
{
	begin_resp(status, do_close);
	puts("{\"error\":\"");
	put(msg.ptr, msg.len);
	puts("\"}");
	finish();
}
