// Strict JSON (RFC 8259): validate a document, decode one string, and find the raw spans of wanted keys.
module server;

struct Js
{
	char* p;
	char* e;
}

fn void ws(Js* j) @inline
{
	while (j.p < j.e && (*j.p == ' ' || *j.p == '\t' || *j.p == '\n' || *j.p == '\r')) j.p++;
}

fn int hexv(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

// Length of one valid UTF-8 sequence at p, 0 if invalid.
fn int utf8_len(char* p, char* e)
{
	uint c = p[0];
	if (c < 0x80) return 1;
	if (c >= 0xc2 && c <= 0xdf) return e - p >= 2 && (p[1] & 0xc0) == 0x80 ? 2 : 0;
	if (c >= 0xe0 && c <= 0xef)
	{
		if (e - p < 3 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80) return 0;
		if ((c == 0xe0 && p[1] < 0xa0) || (c == 0xed && p[1] > 0x9f)) return 0;
		return 3;
	}
	if (c >= 0xf0 && c <= 0xf4)
	{
		if (e - p < 4 || (p[1] & 0xc0) != 0x80 || (p[2] & 0xc0) != 0x80 || (p[3] & 0xc0) != 0x80) return 0;
		if ((c == 0xf0 && p[1] < 0x90) || (c == 0xf4 && p[1] > 0x8f)) return 0;
		return 4;
	}
	return 0;
}

// Parses the string at j.p (on its opening quote), decoding UTF-8 into out when out is non-null.
// Returns the decoded length, -1 on a syntax error, -2 if it doesn't fit in cap (the string is still
// validated).
fn long jstr(Js* j, char* out, usz cap)
{
	char* p = j.p + 1;
	char* e = j.e;
	usz o = 0;
	bool over = false;
	while (p < e)
	{
		char c = *p;
		if (c == '"')
		{
			j.p = p + 1;
			return over ? -2 : (long)o;
		}
		if (c < 0x20) return -1;
		if (c == '\\')
		{
			if (++p >= e) return -1;
			char ch;
			switch (*p)
			{
				case '"': ch = '"';
				case '\\': ch = '\\';
				case '/': ch = '/';
				case 'b': ch = '\b';
				case 'f': ch = '\f';
				case 'n': ch = '\n';
				case 'r': ch = '\r';
				case 't': ch = '\t';
				case 'u':
					if (e - p < 5) return -1;
					int a = hexv(p[1]);
					int b = hexv(p[2]);
					int c2 = hexv(p[3]);
					int d = hexv(p[4]);
					if ((a | b | c2 | d) < 0) return -1;
					uint cp = (uint)(a << 12 | b << 8 | c2 << 4 | d);
					p += 4;
					if (cp >= 0xd800 && cp <= 0xdbff && e - p >= 7 && p[1] == '\\' && p[2] == 'u')
					{
						int a2 = hexv(p[3]);
						int b2 = hexv(p[4]);
						int c3 = hexv(p[5]);
						int d2 = hexv(p[6]);
						if ((a2 | b2 | c3 | d2) < 0) return -1;
						uint lo = (uint)(a2 << 12 | b2 << 8 | c3 << 4 | d2);
						if (lo >= 0xdc00 && lo <= 0xdfff)
						{
							cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
							p += 6;
						}
					}
					if (cp >= 0xd800 && cp <= 0xdfff) cp = 0xfffd;  // lone surrogate: U+FFFD, as JS TextEncoder does
					char[4] u;
					usz ul;
					if (cp < 0x80)
					{
						u[0] = (char)cp;
						ul = 1;
					}
					else if (cp < 0x800)
					{
						u[0] = (char)(0xc0 | cp >> 6);
						u[1] = (char)(0x80 | (cp & 63));
						ul = 2;
					}
					else if (cp < 0x10000)
					{
						u[0] = (char)(0xe0 | cp >> 12);
						u[1] = (char)(0x80 | (cp >> 6 & 63));
						u[2] = (char)(0x80 | (cp & 63));
						ul = 3;
					}
					else
					{
						u[0] = (char)(0xf0 | cp >> 18);
						u[1] = (char)(0x80 | (cp >> 12 & 63));
						u[2] = (char)(0x80 | (cp >> 6 & 63));
						u[3] = (char)(0x80 | (cp & 63));
						ul = 4;
					}
					if (out != null && o + ul > cap)
					{
						out = null;
						over = true;
					}
					if (out != null) memcpy(out + o, &u, ul);
					o += ul;
					p++;
					continue;
				default: return -1;
			}
			if (out != null && o >= cap)
			{
				out = null;
				over = true;
			}
			if (out != null) out[o] = ch;
			o++;
			p++;
			continue;
		}
		usz l = (usz)utf8_len(p, e);
		if (l == 0) return -1;
		if (out != null && o + l > cap)
		{
			out = null;
			over = true;
		}
		if (out != null) memcpy(out + o, p, l);
		o += l;
		p += (sz)l;
	}
	return -1;
}

fn bool jnumber(Js* j)
{
	char* p = j.p;
	char* e = j.e;
	if (p < e && *p == '-') p++;
	if (p >= e) return false;
	if (*p == '0')
	{
		p++;
	}
	else if (*p >= '1' && *p <= '9')
	{
		while (p < e && *p >= '0' && *p <= '9') p++;
	}
	else
	{
		return false;
	}
	if (p < e && *p == '.')
	{
		p++;
		if (p >= e || *p < '0' || *p > '9') return false;
		while (p < e && *p >= '0' && *p <= '9') p++;
	}
	if (p < e && (*p == 'e' || *p == 'E'))
	{
		p++;
		if (p < e && (*p == '+' || *p == '-')) p++;
		if (p >= e || *p < '0' || *p > '9') return false;
		while (p < e && *p >= '0' && *p <= '9') p++;
	}
	j.p = p;
	return true;
}

fn bool jlit(Js* j, String s)
{
	if ((usz)(j.e - j.p) < s.len || memcmp(j.p, s.ptr, s.len) != 0) return false;
	j.p += s.len;
	return true;
}

// Validates and skips one value.
fn bool jvalue(Js* j, int depth)
{
	if (depth > 64) return false;
	ws(j);
	if (j.p >= j.e) return false;
	switch (*j.p)
	{
		case '"': return jstr(j, null, 0) >= 0;
		case '{':
			j.p++;
			ws(j);
			if (j.p < j.e && *j.p == '}')
			{
				j.p++;
				return true;
			}
			while (true)
			{
				ws(j);
				if (j.p >= j.e || *j.p != '"' || jstr(j, null, 0) < 0) return false;
				ws(j);
				if (j.p >= j.e || *j.p++ != ':') return false;
				if (!jvalue(j, depth + 1)) return false;
				ws(j);
				if (j.p >= j.e) return false;
				if (*j.p == ',')
				{
					j.p++;
					continue;
				}
				if (*j.p == '}')
				{
					j.p++;
					return true;
				}
				return false;
			}
		case '[':
			j.p++;
			ws(j);
			if (j.p < j.e && *j.p == ']')
			{
				j.p++;
				return true;
			}
			while (true)
			{
				if (!jvalue(j, depth + 1)) return false;
				ws(j);
				if (j.p >= j.e) return false;
				if (*j.p == ',')
				{
					j.p++;
					continue;
				}
				if (*j.p == ']')
				{
					j.p++;
					return true;
				}
				return false;
			}
		case 't': return jlit(j, "true");
		case 'f': return jlit(j, "false");
		case 'n': return jlit(j, "null");
		default: return jnumber(j);
	}
}

// A key to look for in a top-level object, and the raw span of its value when found.
struct Want
{
	String key;
	char* v;
	char* ve;
}

// Validates a whole document. For a top-level object it records the raw span of the last value of
// each wanted key (the last duplicate wins, as in JSON.parse). Returns false if the document is invalid.
fn bool jdoc(char* s, usz n, Want[] w)
{
	Js j = { s, s + n };
	foreach (&x : w) x.v = null;
	ws(&j);
	if (j.p < j.e && *j.p == '{')
	{
		j.p++;
		ws(&j);
		if (j.p < j.e && *j.p == '}')
		{
			j.p++;
		}
		else
		{
			while (true)
			{
				char[32] kb;
				ws(&j);
				if (j.p >= j.e || *j.p != '"') return false;
				long kl = jstr(&j, &kb, 32);
				if (kl == -1) return false;
				ws(&j);
				if (j.p >= j.e || *j.p++ != ':') return false;
				ws(&j);
				char* vs = j.p;
				if (!jvalue(&j, 1)) return false;
				if (kl >= 0)
				{
					foreach (&x : w)
					{
						if ((usz)kl == x.key.len && memcmp(&kb, x.key.ptr, (usz)kl) == 0)
						{
							x.v = vs;
							x.ve = j.p;
						}
					}
				}
				ws(&j);
				if (j.p >= j.e) return false;
				if (*j.p == ',')
				{
					j.p++;
					continue;
				}
				if (*j.p == '}')
				{
					j.p++;
					break;
				}
				return false;
			}
		}
	}
	else if (!jvalue(&j, 0))
	{
		return false;
	}
	ws(&j);
	return j.p == j.e;
}
