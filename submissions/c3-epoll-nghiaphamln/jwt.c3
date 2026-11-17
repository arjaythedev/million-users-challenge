// HS256 JWT verification with the standard library HMAC; the key schedule is computed once and copied per
// request.
module server;
import std::hash::sha256;

HmacSha256 hm_base;

fn void hmac(char* m, usz n, char[32]* out)
{
	HmacSha256 h = hm_base;
	h.update(m[:n]);
	*out = h.final();
}

// ---------- base64url ----------
ichar[256] b64t;

// Decodes n base64url characters into out. Returns the byte count, or -1 on bad input.
fn long b64dec(char* s, usz n, char* out)
{
	if (n % 4 == 1) return -1;
	uint acc = 0;
	int bits = 0;
	long o = 0;
	for (usz i = 0; i < n; i++)
	{
		int v = b64t[s[i]];
		if (v < 0) return -1;
		acc = acc << 6 | (uint)v;
		bits += 6;
		if (bits >= 8)
		{
			bits -= 8;
			out[o++] = (char)(acc >> bits);
		}
	}
	return o;
}

// ---------- auth ----------
const String BAD_TOKEN = "invalid or expired token";
const String BAD_PAYLOAD = "invalid token payload";

// Returns an empty string and fills uid/uname on success, else the message to send with the 401.
fn String auth(char* h, usz hl, long* uid, char* uname, usz* unl)
{
	if (h == null || hl < 7 || memcmp(h, "Bearer ".ptr, 7) != 0) return "missing bearer token";
	char* t = h + 7;
	char* te = h + hl;
	char* d1 = memchr(t, '.', (usz)(te - t));
	if (d1 == null) return BAD_TOKEN;
	char* d2 = memchr(d1 + 1, '.', (usz)(te - d1 - 1));
	if (d2 == null || memchr(d2 + 1, '.', (usz)(te - d2 - 1)) != null) return BAD_TOKEN;

	// the signature first: anything else is work done for an unauthenticated caller
	char[48] sig;
	char[32] mac;
	if (te - d2 - 1 != 43 || b64dec(d2 + 1, 43, &sig) != 32) return BAD_TOKEN;
	hmac(t, (usz)(d2 - t), &mac);
	uint diff = 0;
	for (int i = 0; i < 32; i++) diff |= (uint)(sig[i] ^ mac[i]);
	if (diff != 0) return BAD_TOKEN;

	if (d1 - t > 680 || d2 - d1 - 1 > 2700) return BAD_TOKEN;
	char[512] hdr;
	char[2048] pl;
	long hn = b64dec(t, (usz)(d1 - t), &hdr);
	long pn = b64dec(d1 + 1, (usz)(d2 - d1 - 1), &pl);
	if (hn < 0 || pn < 0) return BAD_TOKEN;

	Want[1] hw = { { "alg", null, null } };
	if (!jdoc(&hdr, (usz)hn, &hw) || hw[0].v == null || *hw[0].v != '"') return BAD_TOKEN;
	char[8] alg;
	Js aj = { hw[0].v, hw[0].ve };
	long al = jstr(&aj, &alg, 8);
	if (al != 5 || memcmp(&alg, "HS256".ptr, 5) != 0) return BAD_TOKEN;

	Want[4] pw = { { "exp", null, null }, { "nbf", null, null }, { "sub", null, null }, { "username", null, null } };
	if (!jdoc(&pl, (usz)pn, &pw) || pw[0].v == null) return BAD_TOKEN;  // exp is required
	for (int i = 0; i < 2; i++)  // exp / nbf must be numbers, checked against now
	{
		if (pw[i].v == null) continue;
		if (*pw[i].v != '-' && (*pw[i].v < '0' || *pw[i].v > '9')) return BAD_TOKEN;
		char[64] nb;
		usz l = (usz)(pw[i].ve - pw[i].v);
		if (l >= 64) return BAD_TOKEN;
		memcpy(&nb, pw[i].v, l);
		nb[l] = 0;
		double v = strtod(&nb, null);
		if (i == 0 ? (double)now_s >= v : (double)now_s < v) return BAD_TOKEN;
	}

	if (pw[2].v == null || *pw[2].v != '"' || pw[3].v == null || *pw[3].v != '"') return BAD_PAYLOAD;
	char[24] sb;
	Js sj = { pw[2].v, pw[2].ve };
	long sl = jstr(&sj, &sb, 24);
	if (sl <= 0 || sl > 15) return BAD_PAYLOAD;
	long v = 0;
	for (long i = 0; i < sl; i++)
	{
		if (sb[i] < '0' || sb[i] > '9') return BAD_PAYLOAD;
		v = v * 10 + (long)(sb[i] - '0');
	}
	if (v <= 0) return BAD_PAYLOAD;
	Js uj = { pw[3].v, pw[3].ve };
	long ul = jstr(&uj, uname, 1024);
	if (ul < 0) return BAD_PAYLOAD;
	*uid = v;
	*unl = (usz)ul;
	return "";
}
