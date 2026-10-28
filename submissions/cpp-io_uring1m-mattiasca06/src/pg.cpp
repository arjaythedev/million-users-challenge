#include "pg.h"

#include <fcntl.h>
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>

#include "sql.h"


namespace {

inline void put32(char *p, uint32_t v) {
    v = __builtin_bswap32(v);
    memcpy(p, &v, 4);
}
inline void put16(char *p, uint16_t v) {
    v = __builtin_bswap16(v);
    memcpy(p, &v, 2);
}
inline uint32_t get32(const char *p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return __builtin_bswap32(v);
}
inline uint16_t get16(const char *p) {
    uint16_t v;
    memcpy(&v, p, 2);
    return __builtin_bswap16(v);
}

bool encrypted(PGconn *c) { return PQsslInUse(c) || PQgssEncInUse(c); }
}  // namespace

void PgConn::connect(const char *url) {
    c = PQconnectdb(url);
    if (PQstatus(c) != CONNECTION_OK) throw std::runtime_error(std::string("postgres: ") + PQerrorMessage(c));
    if (encrypted(c)) {
        // The socket is used raw from here on, so a TLS/GSS session cannot be continued. Retry once without; an explicit
        // sslmode=require in the URL still wins and then fails with a clear message.
        PQfinish(c);
        setenv("PGSSLMODE", "disable", 1);
        setenv("PGGSSENCMODE", "disable", 1);
        c = PQconnectdb(url);
        if (PQstatus(c) != CONNECTION_OK) throw std::runtime_error(std::string("postgres: ") + PQerrorMessage(c));
        if (encrypted(c)) throw std::runtime_error("postgres: the direct wire client needs sslmode=disable");
    }
    // Prepared statements reuse one generic plan: re-planning every execution was a large share of backend CPU.
    const char *pcm = getenv("PG_PLAN_CACHE");  // override for A/B runs (auto, force_custom_plan, ...)
    if (!pcm) pcm = "force_generic_plan";
    std::string pcm_sql = std::string("SET plan_cache_mode = ") + pcm;
    // Every statement has an index path (posts_created_at_id_idx, primary keys, likes_post_id_idx); keep the planner
    // off sequential scans.
    const char *seq_sql = getenv("PG_SEQSCAN") ? "SET enable_seqscan = on" : "SET enable_seqscan = off";
    for (const char *sql : {"SET jit = off", "SET max_parallel_workers_per_gather = 0", pcm_sql.c_str(), seq_sql}) {
        PGresult *r = PQexec(c, sql);
        bool ok = PQresultStatus(r) == PGRES_COMMAND_OK;
        PQclear(r);
        if (!ok) throw std::runtime_error(std::string("postgres: ") + sql + ": " + PQerrorMessage(c));
    }
    for (const Stmt &s : kStatements) {
        PGresult *r = PQprepare(c, s.name, s.sql, 0, nullptr);
        bool ok = PQresultStatus(r) == PGRES_COMMAND_OK;
        PQclear(r);
        if (!ok) throw std::runtime_error(std::string("postgres: prepare ") + s.name + ": " + PQerrorMessage(c));
    }
    if (PQtransactionStatus(c) != PQTRANS_IDLE) throw std::runtime_error("postgres: connection not idle after setup");
    sock = PQsocket(c);
    // Left blocking: io_uring send/recv on a blocking socket wait inside the kernel (internal poll) instead of failing with EAGAIN.
    in.resize(64 * 1024);
    out.resize(16 * 1024);
}

// Bind (unnamed portal, text parameters, binary results) + Execute per statement, then one Sync. No Describe: the
// column layout is fixed per statement, so the server need not build a RowDescription for every execution.
bool PgConn::send_segment(void *tag, const std::vector<const Query *> &qs) {
    size_t used = out_len;
    for (const Query *qp : qs) {
        const Query &q = *qp;
        const size_t sl = strlen(q.stmt) + 1;
        size_t need = 1 + 4 + 1 + sl + 2 + 2 + 2 + 2 + 10 + 5;  // Bind + Execute (+ room for the closing Sync)
        for (const Param &p : q.params) need += 4 + (p.v ? p.v->size() : 0);
        if (used + need > out.size()) [[unlikely]] out.resize(std::max(out.size() * 2, used + need));
        char *w = out.data() + used;
        char *start = w;
        *w++ = 'B';
        w += 4;  // length, patched below
        *w++ = 0;  // unnamed portal
        memcpy(w, q.stmt, sl);
        w += sl;
        put16(w, 0);  // parameter formats: all text
        w += 2;
        put16(w, uint16_t(q.params.size()));
        w += 2;
        for (const Param &p : q.params) {
            if (!p.v) {
                put32(w, 0xFFFFFFFFu);
                w += 4;
            } else {
                put32(w, uint32_t(p.v->size()));
                w += 4;
                copy_small(w, p.v->data(), p.v->size());
                w += p.v->size();
            }
        }
        put16(w, 1);  // result formats: one code, applied to every column
        w += 2;
        put16(w, 1);  // binary
        w += 2;
        put32(start + 1, uint32_t(w - start - 1));
        *w++ = 'E';
        put32(w, 9);
        w += 4;
        *w++ = 0;  // unnamed portal
        put32(w, 0);  // no row limit
        w += 4;
        used = size_t(w - out.data());
    }
    if (used + 5 > out.size()) [[unlikely]] out.resize(used + 4096);
    char *w = out.data() + used;
    *w++ = 'S';
    put32(w, 4);
    w += 4;
    out_len = size_t(w - out.data());
    inflight.push_back({tag, {}, false, false, uint32_t(qs.size())});
    inflight.back().res.reserve(qs.size());
    return true;
}

bool PgConn::flush() {
    while (out_off < out_len) {
        ssize_t n = ::send(sock, out.data() + out_off, out_len - out_off, MSG_NOSIGNAL);
        if (n > 0) {
            out_off += size_t(n);
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return true;
        dead = true;
        return false;
    }
    out_len = out_off = 0;
    return false;
}

void PgConn::on_data(long n, std::vector<Seg> &done) {
    if (n > 0) in_len += size_t(n);
    else if (n == 0 || (n != -EINTR && n != -EAGAIN)) {
        dead = true;
    }
    parse(done);
    if (dead) [[unlikely]] {
        if (cur) pgres_free(cur);
        cur = nullptr;
        for (Seg &s : inflight) {
            for (PgResult *r : s.res) pgres_free(r);
            s.res.clear();
            s.failed = true;
            done.push_back(std::move(s));
        }
        inflight.clear();
    }
}

void PgConn::drain(std::vector<Seg> &done) {
    while (!dead) {
        if (in.size() - in_len < 4096) in.resize(in.size() * 2);
        size_t room = in.size() - in_len;
        ssize_t n = ::recv(sock, in.data() + in_len, room, 0);
        if (n > 0) {
            in_len += size_t(n);
            if (size_t(n) < room) break;  // the socket is empty
            continue;
        }
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
        dead = true;
    }
    parse(done);
    if (dead) [[unlikely]] {
        if (cur) pgres_free(cur);
        cur = nullptr;
        for (Seg &s : inflight) {
            for (PgResult *r : s.res) pgres_free(r);
            s.res.clear();
            s.failed = true;
            done.push_back(std::move(s));
        }
        inflight.clear();
    }
}

void PgConn::parse(std::vector<Seg> &done) {
    size_t pos = 0;
    while (!dead && in_len - pos >= 5) {
        const char *m = in.data() + pos;
        uint32_t len = get32(m + 1);  // counts itself, not the type byte
        if (len < 4) {
            dead = true;
            break;
        }
        if (in_len - pos < size_t(len) + 1) {
            if (size_t(len) + 1 > in.size()) in.resize(size_t(len) + 1 + 4096);  // big message: grow so it can arrive whole
            break;
        }
        if (!on_message(m[0], m + 5, len - 4, done)) {
            dead = true;
        }
        pos += size_t(len) + 1;
    }
    if (pos == in_len) in_len = 0;
    else if (pos) {
        memmove(in.data(), in.data() + pos, in_len - pos);
        in_len -= pos;
    }
}

bool PgConn::on_message(char type, const char *b, size_t n, std::vector<Seg> &done) {
    switch (type) {
    case '2':  // BindComplete: the next statement's result starts
        if (inflight.empty()) return false;
        if (!cur) cur = pgres_new();
        return true;
    case 'D': [[likely]] {  // DataRow
        if (!cur || n < 2) [[unlikely]] return false;
        int nc = get16(b);
        const char *p = b + 2, *end = b + n;
        if (cur->nrows == 0) cur->ncols = nc;
        else if (nc != cur->ncols) [[unlikely]] return false;
        // The row goes in with one copy (the 4-byte length words stay in it, unused); the field table points into it.
        cur->reserve_row(n, size_t(nc));
        const size_t base = cur->dsz;
        memcpy(cur->data + base, b + 2, n - 2);
        PgResult::Fld *f = cur->f + cur->fsz;
        for (int i = 0; i < nc; i++) {
            if (end - p < 4) [[unlikely]] return false;
            int32_t l = int32_t(get32(p));
            p += 4;
            if (l < 0) {
                f[i] = {0, -1};
                continue;
            }
            if (end - p < l) [[unlikely]] return false;
            f[i] = {uint32_t(base + size_t(p - (b + 2))), l};
            p += l;
        }
        cur->fsz += size_t(nc);
        cur->dsz = base + (n - 2);
        cur->nrows++;
        return true;
    }
    case 'C':    // CommandComplete
    case 'I':    // EmptyQueryResponse
        if (inflight.empty()) return false;
        if (!cur) cur = pgres_new();
        inflight.front().res.push_back(cur);
        cur = nullptr;
        return true;
    case 'E': {  // ErrorResponse: this statement failed and the server skips the rest of the segment up to Sync
        if (inflight.empty()) return false;
        if (cur) pgres_free(cur);
        cur = pgres_new();
        cur->status = PgStatus::Error;
        for (const char *p = b, *end = b + n; p < end && *p;) {
            char code = *p++;
            const char *s = p;
            while (p < end && *p) p++;
            if (code == 'C' && p - s == 5) memcpy(cur->sqlstate, s, 5), cur->sqlstate[5] = 0;
            if (p < end) p++;
        }
        inflight.front().res.push_back(cur);
        cur = nullptr;
        return true;
    }
    case 'Z': {  // ReadyForQuery: the segment is complete
        if (inflight.empty()) return false;
        Seg &s = inflight.front();
        if (cur) pgres_free(cur), cur = nullptr;
        while (s.res.size() < s.n) {
            PgResult *r = pgres_new();
            r->status = PgStatus::Aborted;
            s.res.push_back(r);
        }
        done.push_back(std::move(s));
        inflight.pop_front();
        return true;
    }
    default:  // NoticeResponse, ParameterStatus, NotificationResponse, ...
        return true;
    }
}

void PgConn::close() {
    if (cur) pgres_free(cur);
    cur = nullptr;
    if (c) PQfinish(c);
    c = nullptr;
    sock = -1;
}
