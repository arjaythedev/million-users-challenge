#include "pg.h"
#include "sha256.h"
#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <fcntl.h>

// The SQL from SPEC.md, verbatim (rule 3).
#define SQL_POST_SELECT "SELECT p.id, p.body, p.created_at, u.username,\n       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count\n  FROM posts p JOIN users u ON u.id = p.user_id"
static const struct { const char *name, *sql; int nparams; uint32_t oids[2]; } stmts[] = {
    {"health", "SELECT 1", 0, {0, 0}},
    {"feed", SQL_POST_SELECT "\nORDER BY p.created_at DESC, p.id DESC LIMIT 20", 0, {0, 0}},
    {"post", SQL_POST_SELECT "\nWHERE p.id = $1", 1, {20, 0}},
    {"create", "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at", 2, {20, 25}},
    {"like", "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING", 2, {20, 20}},
};

static void be32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static uint32_t rd32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static uint64_t rd64(const uint8_t *p) { return (uint64_t)rd32(p) << 32 | rd32(p + 4); }
static uint16_t rd16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }

// --- URL ---
static int pct_decode(char *s) {
    char *w = s;
    for (char *r = s; *r; r++) {
        if (*r == '%' && r[1] && r[2]) { char h[3] = {r[1], r[2], 0}; *w++ = (char)strtol(h, NULL, 16); r += 2; }
        else *w++ = *r;
    }
    *w = 0;
    return 0;
}

int pg_parse_url(pgcfg_t *c, const char *url) {
    const char *p = strstr(url, "://");
    if (!p) return -1;
    p += 3;
    const char *at = strrchr(p, '@');
    const char *hostpart = p;
    memset(c, 0, sizeof *c);
    if (at) {
        const char *colon = memchr(p, ':', (size_t)(at - p));
        size_t ul = colon ? (size_t)(colon - p) : (size_t)(at - p);
        snprintf(c->user, sizeof c->user, "%.*s", (int)ul, p);
        if (colon) snprintf(c->password, sizeof c->password, "%.*s", (int)(at - colon - 1), colon + 1);
        hostpart = at + 1;
    }
    const char *slash = strchr(hostpart, '/');
    const char *q = strchr(hostpart, '?');
    const char *hend = slash ? slash : q ? q : hostpart + strlen(hostpart);
    const char *pc = memchr(hostpart, ':', (size_t)(hend - hostpart));
    snprintf(c->host, sizeof c->host, "%.*s", (int)((pc ? pc : hend) - hostpart), hostpart);
    c->port = pc ? atoi(pc + 1) : 5432;
    if (slash) {
        const char *dend = q && q > slash ? q : slash + strlen(slash);
        snprintf(c->dbname, sizeof c->dbname, "%.*s", (int)(dend - slash - 1), slash + 1);
    }
    pct_decode(c->user); pct_decode(c->password); pct_decode(c->dbname);
    struct addrinfo hints = {0}, *res;
    hints.ai_socktype = SOCK_STREAM;
    char ports[8]; snprintf(ports, sizeof ports, "%d", c->port);
    if (getaddrinfo(c->host, ports, &hints, &res)) return -1;
    c->family = res->ai_family;
    if (res->ai_family == AF_INET) { memcpy(c->addr, &((struct sockaddr_in *)res->ai_addr)->sin_addr, 4); c->addrlen = 4; }
    else { memcpy(c->addr, &((struct sockaddr_in6 *)res->ai_addr)->sin6_addr, 16); c->addrlen = 16; }
    freeaddrinfo(res);
    return 0;
}

// --- blocking helpers for the handshake ---
static int read_full(int fd, uint8_t *b, size_t n) { while (n) { ssize_t r = read(fd, b, n); if (r <= 0) return -1; b += r; n -= (size_t)r; } return 0; }
static int write_full(int fd, const uint8_t *b, size_t n) { while (n) { ssize_t r = write(fd, b, n); if (r <= 0) return -1; b += r; n -= (size_t)r; } return 0; }
// reads one message into buf (type + payload); returns type or -1
static int read_msg(int fd, buf_t *buf) {
    uint8_t h[5];
    if (read_full(fd, h, 5)) return -1;
    uint32_t len = rd32(h + 1);
    if (len < 4 || len > 1 << 20) return -1;
    buf_reset(buf); buf_reserve(buf, len - 4);
    if (read_full(fd, buf->p, len - 4)) return -1;
    buf->len = len - 4;
    return h[0];
}
static void msg_begin(buf_t *b, char type, size_t *lenpos) { if (type) { uint8_t t = (uint8_t)type; buf_append(b, &t, 1); } *lenpos = b->len; uint8_t z[4] = {0}; buf_append(b, z, 4); }
static void msg_end(buf_t *b, size_t lenpos) { be32(b->p + lenpos, (uint32_t)(b->len - lenpos)); }
static void msg_cstr(buf_t *b, const char *s) { buf_append(b, s, strlen(s) + 1); }

static const char b64chars[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
static size_t b64_encode(const uint8_t *s, size_t n, char *out) {
    size_t o = 0;
    for (size_t i = 0; i < n; i += 3) {
        uint32_t v = (uint32_t)s[i] << 16 | (i + 1 < n ? (uint32_t)s[i + 1] << 8 : 0) | (i + 2 < n ? s[i + 2] : 0);
        out[o++] = b64chars[v >> 18 & 63]; out[o++] = b64chars[v >> 12 & 63];
        out[o++] = i + 1 < n ? b64chars[v >> 6 & 63] : '='; out[o++] = i + 2 < n ? b64chars[v & 63] : '=';
    }
    out[o] = 0;
    return o;
}
static long b64_decode(const char *s, size_t n, uint8_t *out) {
    uint32_t acc = 0; int bits = 0; long o = 0;
    for (size_t i = 0; i < n; i++) {
        char c = s[i]; int v;
        if (c == '=') break;
        const char *p = strchr(b64chars, c);
        if (!p) return -1;
        v = (int)(p - b64chars);
        acc = acc << 6 | (uint32_t)v; bits += 6;
        if (bits >= 8) { bits -= 8; out[o++] = (uint8_t)(acc >> bits); }
    }
    return o;
}

static int scram(int fd, buf_t *msg, const pgcfg_t *c) {
    // client-first
    uint8_t nonce_raw[18]; char nonce[32];
    for (int i = 0; i < 18; i++) nonce_raw[i] = (uint8_t)rand();
    b64_encode(nonce_raw, 18, nonce);
    char cfirst_bare[128]; snprintf(cfirst_bare, sizeof cfirst_bare, "n=,r=%s", nonce);
    char cfirst[160]; snprintf(cfirst, sizeof cfirst, "n,,%s", cfirst_bare);
    buf_t out = {0};
    size_t lp; msg_begin(&out, 'p', &lp);
    msg_cstr(&out, "SCRAM-SHA-256");
    uint8_t l4[4]; be32(l4, (uint32_t)strlen(cfirst)); buf_append(&out, l4, 4);
    buf_append(&out, cfirst, strlen(cfirst));
    msg_end(&out, lp);
    if (write_full(fd, out.p, out.len)) { free(out.p); return -1; }
    // server-first
    int t = read_msg(fd, msg);
    if (t != 'R' || rd32(msg->p) != 11) { free(out.p); return -1; }
    char sfirst[512]; snprintf(sfirst, sizeof sfirst, "%.*s", (int)(msg->len - 4), (char *)msg->p + 4);
    char *r = strstr(sfirst, "r="), *s = strstr(sfirst, "s="), *it = strstr(sfirst, "i=");
    if (!r || !s || !it) { free(out.p); return -1; }
    char snonce[128]; sscanf(r + 2, "%127[^,]", snonce);
    char salt64[128]; sscanf(s + 2, "%127[^,]", salt64);
    int iters = atoi(it + 2);
    if (strncmp(snonce, nonce, strlen(nonce))) { free(out.p); return -1; }
    uint8_t salt[64]; long saltlen = b64_decode(salt64, strlen(salt64), salt);
    if (saltlen < 0) { free(out.p); return -1; }
    uint8_t salted[32], ckey[32], skey[32], csig[32], proof[32];
    pbkdf2_hmac_sha256(c->password, strlen(c->password), salt, (size_t)saltlen, iters, salted);
    hmac_sha256_raw(salted, 32, "Client Key", 10, ckey);
    sha256(ckey, 32, skey);
    char cfinal_bare[256]; snprintf(cfinal_bare, sizeof cfinal_bare, "c=biws,r=%s", snonce);
    char authmsg[1024]; snprintf(authmsg, sizeof authmsg, "%s,%s,%s", cfirst_bare, sfirst, cfinal_bare);
    hmac_sha256_raw(skey, 32, authmsg, strlen(authmsg), csig);
    for (int i = 0; i < 32; i++) proof[i] = ckey[i] ^ csig[i];
    char proof64[64]; b64_encode(proof, 32, proof64);
    char cfinal[384]; snprintf(cfinal, sizeof cfinal, "%s,p=%s", cfinal_bare, proof64);
    buf_reset(&out); msg_begin(&out, 'p', &lp); buf_append(&out, cfinal, strlen(cfinal)); msg_end(&out, lp);
    int rc = write_full(fd, out.p, out.len);
    free(out.p);
    if (rc) return -1;
    t = read_msg(fd, msg); // SASLFinal (12)
    if (t != 'R' || rd32(msg->p) != 12) return -1;
    t = read_msg(fd, msg); // AuthenticationOk
    if (t != 'R' || rd32(msg->p) != 0) return -1;
    return 0;
}

int pg_open(pgconn_t *p) {
    const pgcfg_t *c = &cfg.pg;
    int fd = socket(c->family, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    struct timeval tv = {5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    int rc;
    if (c->family == AF_INET) {
        struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons((uint16_t)c->port); memcpy(&a.sin_addr, c->addr, 4);
        rc = connect(fd, (struct sockaddr *)&a, sizeof a);
    } else {
        struct sockaddr_in6 a = {0}; a.sin6_family = AF_INET6; a.sin6_port = htons((uint16_t)c->port); memcpy(&a.sin6_addr, c->addr, 16);
        rc = connect(fd, (struct sockaddr *)&a, sizeof a);
    }
    if (rc) { close(fd); return -1; }
    buf_t out = {0}, msg = {0};
    size_t lp;
    msg_begin(&out, 0, &lp);
    uint8_t ver[4] = {0, 3, 0, 0}; buf_append(&out, ver, 4);
    msg_cstr(&out, "user"); msg_cstr(&out, c->user);
    msg_cstr(&out, "database"); msg_cstr(&out, c->dbname);
    msg_cstr(&out, "application_name"); msg_cstr(&out, "feed");
    uint8_t z = 0; buf_append(&out, &z, 1);
    msg_end(&out, lp);
    rc = write_full(fd, out.p, out.len);
    // authentication
    for (;;) {
        if (rc) break;
        int t = read_msg(fd, &msg);
        if (t == 'E') { fprintf(stderr, "postgres: %s\n", (char *)msg.p + 1); rc = -1; break; }
        if (t != 'R') { rc = -1; break; }
        uint32_t kind = rd32(msg.p);
        if (kind == 0) break;
        if (kind == 3) { // cleartext password
            buf_reset(&out); msg_begin(&out, 'p', &lp); msg_cstr(&out, c->password); msg_end(&out, lp);
            rc = write_full(fd, out.p, out.len);
        } else if (kind == 10) {
            if (!strstr((char *)msg.p + 4, "SCRAM-SHA-256")) { rc = -1; break; }
            rc = scram(fd, &msg, c);
            if (!rc) break;
        } else { fprintf(stderr, "postgres: unsupported auth method %u\n", kind); rc = -1; break; }
    }
    // startup params until ReadyForQuery
    while (!rc) {
        int t = read_msg(fd, &msg);
        if (t < 0 || t == 'E') { rc = -1; break; }
        if (t == 'Z') break;
    }
    // prepare the statements
    if (!rc) {
        buf_reset(&out);
        for (size_t i = 0; i < sizeof stmts / sizeof *stmts; i++) {
            msg_begin(&out, 'P', &lp);
            msg_cstr(&out, stmts[i].name); msg_cstr(&out, stmts[i].sql);
            uint8_t np[2] = {0, (uint8_t)stmts[i].nparams}; buf_append(&out, np, 2);
            for (int k = 0; k < stmts[i].nparams; k++) { uint8_t o[4]; be32(o, stmts[i].oids[k]); buf_append(&out, o, 4); }
            msg_end(&out, lp);
        }
        uint8_t sync[5] = {'S', 0, 0, 0, 4}; buf_append(&out, sync, 5);
        rc = write_full(fd, out.p, out.len);
        while (!rc) {
            int t = read_msg(fd, &msg);
            if (t < 0) { rc = -1; break; }
            if (t == 'E') { fprintf(stderr, "postgres: prepare: %s\n", (char *)msg.p + 1); rc = -1; }
            if (t == 'Z') break;
        }
    }
    free(out.p); free(msg.p);
    if (rc) { close(fd); return -1; }
    tv.tv_sec = 0; setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv); setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    p->fd = fd; p->alive = true; p->want_out = p->dirty = false;
    buf_reset(&p->wbuf); buf_reset(&p->rbuf); p->rbuf_off = 0;
    p->qhead = p->qlen = 0; p->inflight = 0; p->sync_seq = p->zseq = 0;
    return 0;
}

void pg_init(pgconn_t *p, struct loop *l, bool write) {
    memset(p, 0, sizeof *p);
    p->l = l; p->write = write; p->fd = -1;
    buf_reserve(&p->wbuf, 16384); buf_reserve(&p->rbuf, 256 * 1024); buf_reserve(&p->body, 8192);
    p->qcap = 256; p->queue = calloc(p->qcap, sizeof *p->queue); p->qseq = calloc(p->qcap, sizeof *p->qseq);
}

// --- sending ---
static void append_bind(pgconn_t *p, const char *stmt, int np, const uint8_t *a, size_t al, const uint8_t *b, size_t bl) {
    buf_t *w = &p->wbuf;
    size_t lp;
    msg_begin(w, 'B', &lp);
    uint8_t z = 0; buf_append(w, &z, 1);        // unnamed portal
    msg_cstr(w, stmt);
    if (np == 0) { uint8_t x[4] = {0, 0, 0, 0}; buf_append(w, x, 4); }
    else {
        uint8_t x[6] = {0, 1, 0, 1, 0, (uint8_t)np}; buf_append(w, x, 6); // one format code (binary) for all params
        uint8_t l4[4];
        be32(l4, (uint32_t)al); buf_append(w, l4, 4); buf_append(w, a, al);
        if (np == 2) { be32(l4, (uint32_t)bl); buf_append(w, l4, 4); buf_append(w, b, bl); }
    }
    uint8_t rf[4] = {0, 1, 0, 1}; buf_append(w, rf, 4); // all result columns binary
    msg_end(w, lp);
    static const uint8_t exec[10] = {'E', 0, 0, 0, 9, 0, 0, 0, 0, 0};
    buf_append(w, exec, 10);
}
static void append_sync(pgconn_t *p) { static const uint8_t s[5] = {'S', 0, 0, 0, 4}; buf_append(&p->wbuf, s, 5); p->sync_seq++; }

void pg_enqueue(pgconn_t *p, const job_t *j) {
    uint8_t a[8], b[8];
    switch (j->kind) {
    case K_HEALTH: append_bind(p, "health", 0, NULL, 0, NULL, 0); break;
    case K_FEED: append_bind(p, "feed", 0, NULL, 0, NULL, 0); break;
    case K_POST: be32(a, (uint32_t)((uint64_t)j->id >> 32)); be32(a + 4, (uint32_t)j->id); append_bind(p, "post", 1, a, 8, NULL, 0); break;
    case K_CREATE: be32(a, (uint32_t)((uint64_t)j->user >> 32)); be32(a + 4, (uint32_t)j->user); append_bind(p, "create", 2, a, 8, j->body, j->blen); break;
    case K_LIKE:
        be32(a, (uint32_t)((uint64_t)j->user >> 32)); be32(a + 4, (uint32_t)j->user);
        be32(b, (uint32_t)((uint64_t)j->id >> 32)); be32(b + 4, (uint32_t)j->id);
        append_bind(p, "like", 2, a, 8, b, 8); break;
    }
    if (p->qhead + p->qlen == p->qcap) {
        if (p->qhead) { memmove(p->queue, p->queue + p->qhead, p->qlen * sizeof *p->queue); memmove(p->qseq, p->qseq + p->qhead, p->qlen * sizeof *p->qseq); p->qhead = 0; }
        else { p->qcap *= 2; p->queue = realloc(p->queue, p->qcap * sizeof *p->queue); p->qseq = realloc(p->qseq, p->qcap * sizeof *p->qseq); }
    }
    p->queue[p->qhead + p->qlen] = *j;
    p->qseq[p->qhead + p->qlen] = p->sync_seq + 1;
    p->qlen++;
    p->inflight++;
    p->dirty = true;
    if (p->write) append_sync(p);
}

void loop_epoll_mod_pg(struct loop *l, int fd, bool want_out); // loop.c
bool uring_pg_send(struct loop *l, pgconn_t *p);             // uring.c: submit sendbuf as a SEND request

static void write_out(pgconn_t *p) {
    if (cfg.pg_sqe && !p->send_inflight && p->wbuf.len) { // io_uring: hand the batch to the ring instead of a write syscall
        buf_t t = p->sendbuf; p->sendbuf = p->wbuf; p->wbuf = t; p->wbuf.len = 0;
        if (uring_pg_send(p->l, p)) { p->send_inflight = true; return; }
        t = p->sendbuf; p->sendbuf = p->wbuf; p->wbuf = t; // not on io_uring: fall through to write()
    }
    if (cfg.pg_sqe && p->send_inflight) return; // the rest goes out when this SEND completes
    size_t sent = 0;
    while (sent < p->wbuf.len) {
        ssize_t n = write(p->fd, p->wbuf.p + sent, p->wbuf.len - sent);
        if (n < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) break;
            pg_die(p, "write");
            return;
        }
        sent += (size_t)n;
    }
    if (sent == p->wbuf.len) {
        p->wbuf.len = 0;
        if (p->want_out) { p->want_out = false; loop_epoll_mod_pg(p->l, p->fd, false); }
        return;
    }
    memmove(p->wbuf.p, p->wbuf.p + sent, p->wbuf.len - sent);
    p->wbuf.len -= sent;
    if (!p->want_out) { p->want_out = true; loop_epoll_mod_pg(p->l, p->fd, true); }
}

void pg_flush(pgconn_t *p) {
    if (!p->dirty || !p->alive) return;
    if (!p->write) append_sync(p);
    p->dirty = false;
    write_out(p);
}

void pg_on_writable(pgconn_t *p) { if (p->alive && p->wbuf.len) write_out(p); }

void pg_send_done(pgconn_t *p, int res) {
    p->send_inflight = false;
    if (!p->alive) return;
    if (res < 0) { pg_die(p, "send"); return; }
    if ((size_t)res < p->sendbuf.len) { // short send: the rest first, then whatever queued meanwhile
        memmove(p->sendbuf.p, p->sendbuf.p + res, p->sendbuf.len - (size_t)res);
        p->sendbuf.len -= (size_t)res;
        if (p->wbuf.len) buf_append(&p->sendbuf, p->wbuf.p, p->wbuf.len), p->wbuf.len = 0;
        if (uring_pg_send(p->l, p)) p->send_inflight = true;
        return;
    }
    p->sendbuf.len = 0;
    if (p->wbuf.len) write_out(p);
}

// --- receiving ---
static void fail_job(pgconn_t *p, job_t *j, const char *code, const char *msg) {
    if (j->kind == K_LIKE && code && !strcmp(code, "23503")) loop_finish_job(p->l, j, 404, (const uint8_t *)BODY_POST_NOT_FOUND, sizeof BODY_POST_NOT_FOUND - 1);
    else if (j->kind == K_HEALTH) { buf_reset(&p->body); append_health_degraded(&p->body, msg ? msg : "database error"); loop_finish_job(p->l, j, 503, p->body.p, p->body.len); }
    else loop_finish_job(p->l, j, 500, (const uint8_t *)BODY_INTERNAL, sizeof BODY_INTERNAL - 1);
    job_free(j);
}

static void pop_head(pgconn_t *p) { p->qhead++; p->qlen--; p->inflight--; if (!p->qlen) p->qhead = 0; }

static int rows_affected(const uint8_t *tag, size_t n) {
    int v = 0, mul = 1;
    long i = (long)n - 1;
    if (i >= 0 && tag[i] == 0) i--;
    for (; i >= 0 && tag[i] >= '0' && tag[i] <= '9'; i--) { v += (tag[i] - '0') * mul; mul *= 10; }
    return v;
}

static void handle(pgconn_t *p, uint8_t t, const uint8_t *m, size_t n) {
    switch (t) {
    case '2': // BindComplete
        p->rows = 0; buf_reset(&p->body);
        if (p->qlen && p->queue[p->qhead].kind == K_FEED) buf_str(&p->body, "{\"posts\":[");
        break;
    case 'D': { // DataRow
        if (!p->qlen) return;
        job_t *j = &p->queue[p->qhead];
        const uint8_t *col[5] = {0}; size_t clen[5] = {0};
        int nc = rd16(m); size_t off = 2;
        for (int i = 0; i < nc && i < 5; i++) {
            int32_t l = (int32_t)rd32(m + off); off += 4;
            if (l >= 0) { col[i] = m + off; clen[i] = (size_t)l; off += (size_t)l; }
        }
        p->rows++;
        switch (j->kind) {
        case K_FEED:
            if (p->rows > 1) buf_str(&p->body, ",");
            append_post(&p->body, (int64_t)rd64(col[0]), col[1], clen[1], (int64_t)rd64(col[2]), col[3], clen[3], (int32_t)rd32(col[4]));
            break;
        case K_POST:
            buf_str(&p->body, "{\"post\":");
            append_post(&p->body, (int64_t)rd64(col[0]), col[1], clen[1], (int64_t)rd64(col[2]), col[3], clen[3], (int32_t)rd32(col[4]));
            buf_str(&p->body, "}");
            break;
        case K_CREATE:
            buf_str(&p->body, "{\"post\":");
            append_post(&p->body, (int64_t)rd64(col[0]), j->body, j->blen, (int64_t)rd64(col[1]), j->username, j->ulen, 0);
            buf_str(&p->body, "}");
            break;
        }
        break;
    }
    case 'C': { // CommandComplete
        if (!p->qlen) return;
        job_t *j = &p->queue[p->qhead];
        int status = 200;
        switch (j->kind) {
        case K_HEALTH: buf_reset(&p->body); append_health_ok(&p->body, now_sec() - start_time.tv_sec); break;
        case K_FEED: buf_str(&p->body, "]}"); break;
        case K_POST: if (!p->rows) { status = 404; buf_reset(&p->body); buf_str(&p->body, BODY_POST_NOT_FOUND); } break;
        case K_CREATE: if (!p->rows) { status = 500; buf_reset(&p->body); buf_str(&p->body, BODY_INTERNAL); } else status = 201; break;
        case K_LIKE: { bool already = rows_affected(m, n) == 0; if (!already) status = 201; buf_reset(&p->body); append_like(&p->body, already, j->id); break; }
        }
        loop_finish_job(p->l, j, status, p->body.p, p->body.len);
        job_free(j);
        pop_head(p);
        break;
    }
    case 'E': { // ErrorResponse: the job at the head; the rest of its transaction is skipped
        char code[8] = "", msg[200] = "";
        for (size_t i = 0; i < n && m[i];) {
            uint8_t f = m[i];
            size_t e = i + 1; while (e < n && m[e]) e++;
            if (f == 'C') snprintf(code, sizeof code, "%.*s", (int)(e - i - 1), m + i + 1);
            if (f == 'M') snprintf(msg, sizeof msg, "%.*s", (int)(e - i - 1), m + i + 1);
            i = e + 1;
        }
        if (p->qlen) { fail_job(p, &p->queue[p->qhead], code, msg); pop_head(p); }
        break;
    }
    case 'Z': // ReadyForQuery
        p->zseq++;
        while (p->qlen && p->qseq[p->qhead] == p->zseq) { fail_job(p, &p->queue[p->qhead], NULL, "skipped after an error in the same batch"); pop_head(p); }
        break;
    default: break; // '1', 'n', 'T', 'S', 'N'
    }
}

void pg_on_readable(pgconn_t *p) {
    buf_t *r = &p->rbuf;
    if (r->cap - r->len < 32 * 1024) buf_reserve(r, 32 * 1024);
    for (;;) {
        ssize_t n = read(p->fd, r->p + r->len, r->cap - r->len);
        if (n < 0) { if (errno == EINTR) continue; if (errno == EAGAIN) return; pg_die(p, "read"); return; }
        if (n == 0) { pg_die(p, "closed"); return; }
        r->len += (size_t)n;
        break;
    }
    size_t off = 0;
    while (r->len - off >= 5) {
        uint32_t len = rd32(r->p + off + 1);
        if (len < 4 || len > 64u << 20) { pg_die(p, "protocol"); return; }
        if (r->len - off < 1 + len) break;
        handle(p, r->p[off], r->p + off + 5, len - 4);
        if (!p->alive) return;
        off += 1 + len;
    }
    if (off) { memmove(r->p, r->p + off, r->len - off); r->len -= off; }
}

void pg_die(pgconn_t *p, const char *why) {
    if (!p->alive) return;
    fprintf(stderr, "postgres: connection lost (%s)\n", why);
    p->alive = false;
    close(p->fd); p->fd = -1;
    while (p->qlen) { fail_job(p, &p->queue[p->qhead], NULL, "connection lost"); pop_head(p); }
    p->wbuf.len = 0; p->rbuf.len = 0; p->dirty = false; p->want_out = false; p->sendbuf.len = 0; p->send_inflight = false;
    p->next_retry = now_sec() + 1;
}
