#include <thread>

#include "worker.h"
#include "bufpool.h"
#include "connbuf.h"
#include "egress.h"
#include <chrono>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string_view>

#include <libdeflate.h>
#ifdef USE_ISAL
#include <isa-l/igzip_lib.h>
#endif

#include "picohttpparser.h"
#include "codec.h"

namespace {

enum Op : uint64_t { kAccept, kRecv, kSend, kPgIn, kPgOut };

// user_data: op in bits 0..3, slot index in bits 4..35, generation in bits 36..63
constexpr uint64_t pack(Op op, uint32_t idx, uint32_t gen = 0) {
    return uint64_t(op) | (uint64_t(idx) << 4) | (uint64_t(gen) << 36);
}
constexpr Op op_of(uint64_t u) { return Op(u & 15); }
constexpr uint32_t idx_of(uint64_t u) { return uint32_t((u >> 4) & 0xffffffffu); }
constexpr uint32_t gen_of(uint64_t u) { return uint32_t(u >> 36); }

constexpr size_t kMaxRequest = 1 << 20;

const char *reason(int status) {
    switch (status) {
    case 200: return "OK";
    case 201: return "Created";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    default: return "Internal Server Error";
    }
}

bool ieq(const char *a, size_t n, const char *lit) { return n == strlen(lit) && strncasecmp(a, lit, n) == 0; }

// True if an Accept-Encoding value lists gzip (a "gzip;q=0" refusal is not honoured specially: no client sends it).
bool has_gzip(const char *v, size_t n) {
    for (size_t i = 0; i + 4 <= n; i++)
        if (strncasecmp(v + i, "gzip", 4) == 0) return true;
    return false;
}

}  // namespace

void DbAwait::await_suspend(std::coroutine_handle<> c) {
    h = c;
    w.enqueue(this);
}

Worker::Worker(const Config &cfg, int id) : cfg_(cfg), id_(id) {
    if (const char *e = getenv("PF")) frame_pool::g_pf = unsigned(atoi(e));
    conns_.reserve(hugemem::kHuge / sizeof(Conn));
    for (int i = 0; i < cfg_.read_conns; i++) {
        auto pg = std::make_unique<PgConn>();
        pg->connect(cfg_.database_url.c_str());
        read_idx_.push_back(uint32_t(pgs_.size()));
        pgs_.push_back(std::move(pg));
    }
    {  // the dedicated write connection follows the initial read connections; grown read connections come after it
        wi_ = uint32_t(pgs_.size());
        auto pg = std::make_unique<PgConn>();
        pg->connect(cfg_.database_url.c_str());
        pgs_.push_back(std::move(pg));
    }
}

Worker::~Worker() {
    for (auto &pg : pgs_) pg->close();
}

io_uring_sqe *Worker::sqe() {
    io_uring_sqe *s = io_uring_get_sqe(&ring_);
    if (!s) [[unlikely]] {
        io_uring_submit(&ring_);
        s = io_uring_get_sqe(&ring_);
    }
    return s;
}

void Worker::arm_accept(int which) {
    io_uring_sqe *s = sqe();
    io_uring_prep_multishot_accept(s, listen_fd_[which], nullptr, nullptr, 0);
    io_uring_sqe_set_data64(s, pack(kAccept, uint32_t(which)));
}

void Worker::arm_recv(uint32_t idx) {
    Conn &c = conns_[idx];
    io_uring_sqe *s = sqe();
    io_uring_prep_recv_multishot(s, c.fd, nullptr, 0, 0);
    s->flags |= IOSQE_BUFFER_SELECT;
    s->buf_group = kBgid;
    io_uring_sqe_set_data64(s, pack(kRecv, idx, c.gen));
    c.recv_active = true;
}

// One-shot (level-checked at arm time) rather than multishot: a multishot poll can miss data that was already
// readable, which left requests waiting forever. Armed whenever a segment is in flight.
void Worker::arm_pg(uint32_t pg_idx) {
    PgConn &pg = *pgs_[pg_idx];
    if (pg.in_armed) return;
    if (pg.in.size() - pg.in_len < 4096) pg.in.resize(pg.in.size() * 2);
    pg.in_armed = true;
    io_uring_sqe *s = sqe();
    io_uring_prep_recv(s, pg.sock, pg.in.data() + pg.in_len, pg.in.size() - pg.in_len, 0);
    io_uring_sqe_set_data64(s, pack(kPgIn, pg_idx));
}

// Queues the unsent part of the connection's output as an io_uring send (no syscall of its own: it goes out with the
// next submit, together with the recv armed for the answer).
void Worker::arm_pg_out(uint32_t pg_idx) {
    PgConn &pg = *pgs_[pg_idx];
    if (pg.sending || pg.out_off >= pg.out_len) return;
    pg.sending = true;
    io_uring_sqe *s = sqe();
    io_uring_prep_send(s, pg.sock, pg.out.data() + pg.out_off, pg.out_len - pg.out_off, MSG_NOSIGNAL);
    io_uring_sqe_set_data64(s, pack(kPgOut, pg_idx));
}


void Worker::on_accept(int fd) {
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    uint32_t idx;
    if (!free_.empty()) {
        idx = free_.back();
        free_.pop_back();
    } else {
        idx = uint32_t(conns_.size());
        conns_.emplace_back();
    }
    Conn &c = conns_[idx];
    c.fd = fd;
    c.recv_active = c.sending = c.busy = c.closing = c.close_after = false;
    c.in.clear();
    c.out.clear();
    c.sendbuf.clear();
    arm_recv(idx);
}

void Worker::maybe_free(uint32_t idx) {
    Conn &c = conns_[idx];
    if (!c.closing || c.recv_active || c.sending) return;
    close(c.fd);
    c.fd = -1;
    c.gen++;
    c.in.clear();
    c.out.clear();
    c.sendbuf.clear();
    free_.push_back(idx);
}

// shutdown() makes the pending multishot recv complete with 0, which lets the slot go once nothing is in flight.
void Worker::kill(uint32_t idx) {
    Conn &c = conns_[idx];
    if (!c.closing) {
        c.closing = true;
        shutdown(c.fd, SHUT_RDWR);
    }
    maybe_free(idx);
}

void Worker::on_recv(uint32_t idx, uint32_t gen, io_uring_cqe *cqe) {
    Conn &c = conns_[idx];
    unsigned bid = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
    bool has_buf = cqe->flags & IORING_CQE_F_BUFFER;
    auto give_back = [&] {
        io_uring_buf_ring_add(br_, &bufs_[size_t(bid) * kBufSize], kBufSize, uint16_t(bid),
                              io_uring_buf_ring_mask(kBufCount), 0);
        io_uring_buf_ring_advance(br_, 1);
    };
    if (c.gen != gen) [[unlikely]] {  // stale completion for a reused slot: only give the buffer back
        if (has_buf) give_back();
        return;
    }
    if (!(cqe->flags & IORING_CQE_F_MORE)) c.recv_active = false;
    if (has_buf) {
        if (cqe->res > 0 && !c.closing) take_buf(c.in), c.in.append(&bufs_[size_t(bid) * kBufSize], size_t(cqe->res));
        give_back();
    }
    if (cqe->res <= 0 && cqe->res != -ENOBUFS) [[unlikely]] {
        kill(idx);
        return;
    }
    if (!c.recv_active && !c.closing) arm_recv(idx);
    if (cqe->res > 0 && !c.closing) process(idx);
    release_big(conns_[idx].in);
    maybe_free(idx);
}

void Worker::start_send(uint32_t idx) {
    Conn &c = conns_[idx];
    if (c.sending || c.out.empty()) return;
    c.sendbuf.swap(c.out);
    c.sending = true;
    io_uring_sqe *s = sqe();
    io_uring_prep_send(s, c.fd, c.sendbuf.data(), c.sendbuf.size(), MSG_NOSIGNAL);
    io_uring_sqe_set_data64(s, pack(kSend, idx, c.gen));
}

void Worker::on_send(uint32_t idx, uint32_t gen, int res) {
    Conn &c = conns_[idx];
    if (c.gen != gen) [[unlikely]] return;
    c.sending = false;
    if (res <= 0) [[unlikely]] {
        kill(idx);
        return;
    }
    c.sendbuf.erase_front(size_t(res));
    if (!c.sendbuf.empty()) [[unlikely]] {  // partial send: keep the rest ahead of anything newer
        c.out.insert_front(c.sendbuf);
        c.sendbuf.clear();
    }
    // An idle keep-alive connection must not hold on to its largest response: with ~100k connections the retained
    // capacity of sendbuf/out (swapped back and forth) added up to over a gigabyte.
    release_big(c.sendbuf);
    if (c.out.empty()) release_big(c.out);
    if (!c.out.empty()) {
        start_send(idx);
    } else if (c.close_after) {
        kill(idx);
    } else {
        maybe_free(idx);
    }
}

// Response head by hand: no format-string parsing on the hot path. Same bytes the snprintf version produced.
static inline char *put(char *p, std::string_view s) {
    memcpy(p, s.data(), s.size());
    return p + s.size();
}
static inline char *put_uint(char *p, size_t v) {
    char t[20];
    int n = 0;
    do {
        t[n++] = char('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) *p++ = t[--n];
    return p;
}

// Gzip into a per-thread scratch buffer; empty when the result would not be smaller.
// Full: libdeflate level 1.
[[maybe_unused]] static std::string_view gzip_deflate(const std::string &body) {
    thread_local libdeflate_compressor *comp = libdeflate_alloc_compressor(1);
    thread_local std::vector<char> buf;
    const size_t bound = libdeflate_gzip_compress_bound(comp, body.size());
    if (buf.size() < bound) buf.resize(bound);
    const size_t n = libdeflate_gzip_compress(comp, body.data(), body.size(), buf.data(), buf.size());
    if (n == 0 || n >= body.size()) return {};
    return {buf.data(), n};
}

#ifdef USE_ISAL
// Light: ISA-L igzip level 0 (fixed-size state, no allocation per call; the library picks its AVX2/AVX-512 code by CPUID).
static std::string_view gzip_light(const std::string &body) {
    thread_local std::vector<char> buf;
    thread_local std::vector<uint8_t> lvl(ISAL_DEF_LVL0_DEFAULT);
    const size_t bound = body.size() + body.size() / 4 + 256;
    if (buf.size() < bound) buf.resize(bound);
    isal_zstream z;
    isal_deflate_stateless_init(&z);
    z.level = 0;
    z.level_buf = lvl.data();
    z.level_buf_size = uint32_t(lvl.size());
    z.gzip_flag = IGZIP_GZIP;
    z.flush = NO_FLUSH;
    z.end_of_stream = 1;
    z.next_in = reinterpret_cast<uint8_t *>(const_cast<char *>(body.data()));
    z.avail_in = uint32_t(body.size());
    z.next_out = reinterpret_cast<uint8_t *>(buf.data());
    z.avail_out = uint32_t(buf.size());
    if (isal_deflate_stateless(&z) != COMP_OK || z.avail_in != 0) return {};
    const size_t n = z.total_out;
    if (n == 0 || n >= body.size()) return {};
    return {buf.data(), n};
}
#else
static std::string_view gzip_light(const std::string &body) { return gzip_deflate(body); }
#endif

void Worker::respond(Conn &c, uint32_t idx, int status, const std::string &body, bool gz) {
    std::string_view payload = body;
    std::string_view zipped;
    if (gz && cfg_.gzip_min > 0 && status == 200 && body.size() >= size_t(cfg_.gzip_min)) {
        // Always gzip with ISA-L igzip level 0 (the adaptive egress controller below is disabled for now).
        zipped = gzip_light(body);
        if (!zipped.empty()) payload = zipped;
#if 0  // adaptive egress (egress.h): none -> light (ISA-L) -> full (libdeflate) from TCP_INFO and system counters; disabled
        egress::Counters &ec = egress::counters[id_ & (egress::kMaxWorkers - 1)];
        auto bump = [](std::atomic<uint64_t> &a, uint64_t v) { a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed); };
        bump(ec.eligible_n, 1);
        bump(ec.eligible_raw, body.size());
        const uint8_t level = egress::mode.load(std::memory_order_relaxed);
        const uint32_t tick = ++ec.tick;
        if ((tick & 255) == 0) {
            using clk = std::chrono::steady_clock;
            const auto t0 = clk::now();
            const std::string_view a = gzip_light(body);
            const auto t1 = clk::now();
            const std::string_view b = gzip_deflate(body);
            const auto t2 = clk::now();
            if (!a.empty() && !b.empty()) {
                bump(ec.cal_n, 1);
                bump(ec.cal_gz[egress::kNone], body.size());
                bump(ec.cal_gz[egress::kLight], a.size());
                bump(ec.cal_gz[egress::kFull], b.size());
                bump(ec.cal_ns[egress::kLight], uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count()));
                bump(ec.cal_ns[egress::kFull], uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(t2 - t1).count()));
            }
            zipped = level == egress::kFull ? b : level == egress::kLight ? a : std::string_view();
        } else if (level != egress::kNone) {
            zipped = level == egress::kFull ? gzip_deflate(body) : gzip_light(body);
        }
        if (!zipped.empty()) payload = zipped;
        if ((tick & 255) == 128) egress::sample_tcp(ec, c.fd);
#endif
    }
    char head[256];
    char *p = head;
    p = put(p, "HTTP/1.1 ");
    p = put_uint(p, size_t(status));
    *p++ = ' ';
    p = put(p, reason(status));
    p = put(p, "\r\nContent-Type: application/json\r\n");
    if (!zipped.empty()) p = put(p, "Content-Encoding: gzip\r\n");
    p = put(p, "Content-Length: ");
    p = put_uint(p, payload.size());
    p = put(p, "\r\n");
    if (c.close_after) p = put(p, "Connection: close\r\n");
    p = put(p, "\r\n");
    const size_t n = size_t(p - head);
    take_buf(c.out);
    c.out.reserve(c.out.size() + n + payload.size());
    c.out.append(head, n);
    c.out.append(payload);
    start_send(idx);
}

// Handlers may complete synchronously (inside process_one) or later from the event loop. `in_process_` keeps a
// completion that happens inside process_one from starting a nested parse; those connections are revisited afterwards.
void Worker::process(uint32_t idx) {
    if (in_process_) {
        deferred_.push_back(idx);
        return;
    }
    in_process_ = true;
    process_one(idx);
    while (!deferred_.empty()) {
        uint32_t d = deferred_.back();
        deferred_.pop_back();
        process_one(d);
    }
    in_process_ = false;
}

// Parses and starts as many buffered requests as it can, strictly in order: while one is running the rest stay
// buffered.
void Worker::process_one(uint32_t idx) {
    for (;;) {
        Conn &c = conns_[idx];
        if (c.fd < 0 || c.busy || c.closing || c.in.empty()) return;
        const char *method, *path;
        size_t mlen, plen;
        int minor;
        phr_header hdrs[64];
        size_t nh = 64;
        int pr = phr_parse_request(c.in.data(), c.in.size(), &method, &mlen, &path, &plen, &minor, hdrs, &nh, 0);
        if (pr == -2) [[unlikely]] {
            if (c.in.size() > kMaxRequest) {
                c.close_after = true;
                c.in.clear();
                respond(c, idx, 413, R"({"error":"request too large"})", false);
            }
            return;
        }
        if (pr < 0) [[unlikely]] {
            c.close_after = true;
            c.in.clear();
            respond(c, idx, 400, R"({"error":"bad request"})", false);
            return;
        }
        Request rq;
        size_t content_len = 0;
        bool close_hdr = minor == 0;
        for (size_t i = 0; i < nh; i++) {
            const phr_header &h = hdrs[i];
            if (ieq(h.name, h.name_len, "content-length")) {
                content_len = strtoull(std::string(h.value, h.value_len).c_str(), nullptr, 10);
            } else if (ieq(h.name, h.name_len, "connection")) {
                close_hdr = ieq(h.value, h.value_len, "close");
            } else if (ieq(h.name, h.name_len, "accept-encoding")) {
                rq.accept_gzip = has_gzip(h.value, h.value_len);
            } else if (ieq(h.name, h.name_len, "authorization")) {
                rq.has_auth = true;
                take_buf(rq.auth);
                rq.auth.assign(h.value, h.value_len);
            }
        }
        if (content_len > kMaxRequest) [[unlikely]] {
            c.close_after = true;
            c.in.clear();
            respond(c, idx, 413, R"({"error":"request too large"})", false);
            return;
        }
        if (c.in.size() < size_t(pr) + content_len) [[unlikely]] return;  // body still arriving
        if (close_hdr) c.close_after = true;

        rq.method.assign(method, mlen);
        std::string_view target(path, plen);
        take_buf(rq.path);
        rq.path.assign(target.substr(0, target.find('?')));
        if (content_len) take_buf(rq.body);
        rq.body.assign(c.in.data() + pr, content_len);
        c.in.erase_front(size_t(pr) + content_len);
        c.busy = true;
        if (frame_pool::g_pf & 64) {  // the handler will reach dispatch(): have the read connections' lines in cache by then
            for (uint32_t i : read_idx_) {
                const char *b = reinterpret_cast<const char *>(pgs_[i].get());
                __builtin_prefetch(b);
                __builtin_prefetch(b + 64);
                __builtin_prefetch(b + 128);
            }
        }
        {
            serve(idx, c.gen, std::move(rq));
        }
    }
}

Detached Worker::serve(uint32_t idx, uint32_t gen, Request rq) {
    Reply r;
    try {
        r = co_await route(rq);
    } catch (const std::exception &) {
        r = {500, R"({"error":"internal server error"})"};
    }
    recycle(rq.auth);
    recycle(rq.path);
    recycle(rq.body);
    finish(idx, gen, std::move(r), rq.accept_gzip);
}

void Worker::finish(uint32_t idx, uint32_t gen, Reply r, bool gz) {
    Conn &c = conns_[idx];
    if (c.gen != gen) [[unlikely]] return;  // client went away while the request ran
    c.busy = false;
    respond(c, idx, r.status, r.body, gz);
    recycle(r.body);
    process(idx);
}

void Worker::enqueue(DbAwait *a) {
    if (a->write) {
        writeq_.push_back(a);
        dispatch_writes();
    } else {
        a->t_enq = now_ns_;
        waitq_.push_back(a);
        dispatch();
    }
}

// Natural group commit: while a write segment is in flight, writes queue; the queue then goes out as one segment
// with a single Sync (one implicit transaction). Solo requests always travel alone.
void Worker::dispatch_writes() {
    const uint32_t wi = wi_;
    PgConn &pg = *pgs_[wi];
    if (writeq_.empty()) return;
    if (pg.dead) {
        std::deque<DbAwait *> q;
        q.swap(writeq_);
        for (DbAwait *a : q) a->rows.ok = false;
        for (DbAwait *a : q) a->h.resume();
        return;
    }
    if (!pg.inflight.empty()) return;
    Group *g = new_group();
    g->members.push_back(writeq_.front());
    writeq_.pop_front();
    if (!g->members[0]->solo)
        while (!writeq_.empty() && !writeq_.front()->solo && g->members.size() < kMaxGroup) {
            g->members.push_back(writeq_.front());
            writeq_.pop_front();
        }
    std::vector<const Query *> &all = all_;
    all.clear();
    for (DbAwait *a : g->members)
            for (const Query &q : a->qs) all.push_back(&q);
    if (!pg.send_segment(g, all)) {
        std::vector<DbAwait *> ms = std::move(g->members);
        g->members.clear();
        free_group(g);
        for (DbAwait *a : ms) a->rows.ok = false;
        for (DbAwait *a : ms) a->h.resume();
        return;
    }
    {
        arm_pg_out(wi);
    }
}

// Hands queued segments to idle Postgres connections (one segment in flight per connection).
// Every 100 ms of activity: adopt a freshly connected read connection, then size the read pool.
//  - grow when the oldest waiting read has waited more than two segment service times and the connections were busy, for 10 windows (1 s) in a row
//    (queue delay as the congestion signal, as in TCP Vegas / delay-based concurrency limits);
//  - grow at once (2 windows) when reads wait over 100 ms on busy connections: a saturated or stuck connection;
//  - shrink when the load, measured as busy connections (Little's law: sum of busy time / elapsed), would still sit
//    under 60% utilisation on one connection fewer, for 30 windows (3 s) in a row.
// Fewer connections mean fuller read segments (more sharing, fewer syscalls), so the pool only grows while it must.
void Worker::control() {
    if (id_ == 0 && (++arena_tick_ % 10) == 0) hugemem::advise_heap_arenas();
#ifdef PROBES
    if (id_ == 0 && ++probe_tick_ % 100 == 0) probe_report();
#endif  // once a second: arenas created since start
    if (grow_state_.load(std::memory_order_acquire) == 2) {
        read_idx_.push_back(uint32_t(pgs_.size()));
        pgs_.emplace_back(pending_.exchange(nullptr));
        grow_state_.store(0, std::memory_order_release);
    }
    uint64_t w = win_wait_max_;
    win_wait_max_ = 0;
    if (!waitq_.empty() && now_ns_ >= waitq_.front()->t_enq) w = std::max(w, now_ns_ - waitq_.front()->t_enq);
    const double limit_ns = cfg_.pool_wait_us > 0 ? double(cfg_.pool_wait_us) * 1000 : std::max(200000.0, 2.0 * svc_ewma_ns_);

    const uint64_t span = ctl_prev_ns_ && now_ns_ > ctl_prev_ns_ ? now_ns_ - ctl_prev_ns_ : 0;
    ctl_prev_ns_ = now_ns_;
    double busy_conns = 0;  // connections busy on average over this window
    for (uint32_t i : read_idx_) {
        PgConn &p = *pgs_[i];
        uint64_t b = p.busy_ns + (p.busy_since && now_ns_ > p.busy_since ? now_ns_ - p.busy_since : 0);
        if (span && b >= p.busy_prev) busy_conns += double(b - p.busy_prev) / double(span);
        p.busy_prev = b;
    }
    const size_t n = read_idx_.size();
    // A wait only means the connections are the bottleneck if they were also busy: while the event loop itself is slow
    // (accepting a burst of connections) reads wait with idle connections, and another connection would only split segments.
    congested_ = double(w) > limit_ns && span && busy_conns >= 0.9 * double(n) ? congested_ + 1 : 0;
    // Fast path for a connection that is saturated or stuck (a retransmit, one slow backend): reads waiting over 100 ms
    // (a fifth of the p95 limit) on busy connections for two windows. A second connection then takes the new reads.
    hot_ = w > 100000000ull && span && busy_conns >= 0.9 * double(n) ? hot_ + 1 : 0;
    idle_windows_ = span && n > size_t(cfg_.read_conns) && busy_conns / double(n - 1) < 0.6 ? idle_windows_ + 1 : 0;
    if (idle_windows_ >= 30 && pgs_[read_idx_.back()]->inflight.empty()) {
        spare_.push_back(read_idx_.back());
        read_idx_.pop_back();
        idle_windows_ = 0;
    }
    if ((congested_ >= 10 || hot_ >= 2) && read_idx_.size() < size_t(cfg_.read_conns_max)) {
        if (!spare_.empty()) {
            congested_ = hot_ = 0;
            read_idx_.push_back(spare_.back());
            spare_.pop_back();
            idle_windows_ = 0;
        } else if (grow_state_.load() == 0) {
            congested_ = hot_ = 0;
            grow_state_.store(1);
            std::thread([this] {
                try {
                    auto *p = new PgConn;
                    p->connect(cfg_.database_url.c_str());
                    pending_.store(p, std::memory_order_release);
                    grow_state_.store(2, std::memory_order_release);
                } catch (const std::exception &e) {
                    fprintf(stderr, "worker %d: could not grow the read pool: %s\n", id_, e.what());
                    grow_state_.store(3);
                }
            }).detach();
        }
    }
}

void Worker::dispatch() {
    std::vector<DbAwait *> failed;
    while (!waitq_.empty()) {
        PgConn *pg = nullptr;
        uint32_t pi = 0;
        bool any_alive = false;
        for (uint32_t i : read_idx_) {
            if (pgs_[i]->dead) continue;
            any_alive = true;
            if (pgs_[i]->inflight.empty()) {
                pg = pgs_[i].get();
                pi = i;
                break;
            }
        }
        if (!any_alive) {
            for (DbAwait *a : waitq_) {
                a->rows.ok = false;
                failed.push_back(a);
            }
            waitq_.clear();
            break;
        }
        if (!pg) break;
        // Natural batching: everything that queued while the connections were busy goes out as one segment
        // (one Sync, one implicit read-only transaction).
        Group *g = new_group();
        g->members.push_back(waitq_.front());
        waitq_.pop_front();
        if (now_ns_ >= g->members[0]->t_enq && now_ns_ - g->members[0]->t_enq > win_wait_max_)
            win_wait_max_ = now_ns_ - g->members[0]->t_enq;
        if (!g->members[0]->solo)
            while (!waitq_.empty() && !waitq_.front()->solo && g->members.size() < size_t(cfg_.read_group_max)) {
                g->members.push_back(waitq_.front());
                waitq_.pop_front();
            }
        std::vector<const Query *> &all = all_;
        all.clear();
        for (DbAwait *a : g->members)
            for (const Query &q : a->qs) all.push_back(&q);
        pg->busy_since = now_ns_;
        if (!pg->send_segment(g, all)) {
            for (DbAwait *m : g->members) {
                m->rows.ok = false;
                failed.push_back(m);
            }
            free_group(g);
            continue;
        }
        {
            arm_pg_out(pi);
        }
    }
    for (DbAwait *a : failed) a->h.resume();
}

void Worker::on_pg(uint32_t pg_idx, long res) {
#ifdef PROBES
    probes::Span probe_sp(pr_.pmu, pr_.onpg);
    if (res > 0) pr_.pg_bytes += size_t(res);
#endif
    std::vector<PgConn::Seg> &done = seg_done_;
    done.clear();
    if (frame_pool::g_pf & 16) {  // the group whose answer is being parsed: its awaiters (inside the handler frames) are cold
        PgConn &p0 = *pgs_[pg_idx];
        if (!p0.inflight.empty())
            for (DbAwait *a : static_cast<Group *>(p0.inflight.front().tag)->members) {
                const char *b = reinterpret_cast<const char *>(a);
                __builtin_prefetch(b, 1);
                __builtin_prefetch(b + 64, 1);
                __builtin_prefetch(b + 128, 1);
                __builtin_prefetch(b + 192, 1);
            }
    }
    {
#ifdef PROBES
        probes::Span probe_pp(pr_.pmu, pr_.pg_parse);
#endif
        pgs_[pg_idx]->on_data(res, done);
        PgConn &pc = *pgs_[pg_idx];
        if (pc.inflight.empty() && pc.busy_since) {
            if (now_ns_ > pc.busy_since) {
                pc.busy_ns += now_ns_ - pc.busy_since;
                if (pg_idx != wi_) svc_ewma_ns_ = svc_ewma_ns_ == 0 ? double(now_ns_ - pc.busy_since) : 0.95 * svc_ewma_ns_ + 0.05 * double(now_ns_ - pc.busy_since);
            }
            pc.busy_since = 0;
        }
    }
    std::vector<std::coroutine_handle<>> &ready = ready_;
    ready.clear();
    const bool is_write = pg_idx == wi_;
#ifdef PROBES
    {
    probes::Span probe_pa(pr_.pmu, pr_.pg_assemble);
#endif
    for (PgConn::Seg &s : done) {
        Group *g = static_cast<Group *>(s.tag);
        struct Free { Worker *w; Group *g; ~Free() { w->free_group(g); } } free_g{this, g};
        size_t total = 0;
        for (DbAwait *a : g->members) total += a->qs.size();
#ifdef PROBES
        pr_.pg_segs++;
        pr_.pg_members += g->members.size();
        for (PgResult *r : s.res) pr_.pg_rows += size_t(r->nrows);
#endif
        bool clean = !s.failed && s.res.size() == total;
        for (PgResult *r : s.res)
            if (r->status != PgStatus::Ok) clean = false;
        if (s.failed) {  // connection lost: the commit may or may not have happened, so never retry
            for (PgResult *r : s.res) pgres_free(r);
            for (DbAwait *a : g->members) {
                a->rows.ok = false;
                ready.push_back(a->h);
            }
        } else if (!clean && g->members.size() > 1) {  // the group failed (writes: rolled back): replay each alone
            for (PgResult *r : s.res) pgres_free(r);
            std::deque<DbAwait *> &q = is_write ? writeq_ : waitq_;
            for (auto it = g->members.rbegin(); it != g->members.rend(); ++it) {
                (*it)->solo = true;
                q.push_front(*it);
            }
        } else {
            size_t pos = 0;
            for (DbAwait *a : g->members) {
                size_t n = a->qs.size();
                if (pos + n <= s.res.size())
                    for (size_t i = pos; i < pos + n; i++) a->rows.r.push_back(Res(s.res[i]));
                a->rows.ok = true;
                pos += n;
                ready.push_back(a->h);
            }
        }
    }
#ifdef PROBES
    }
    {
    probes::Span probe_pr(pr_.pmu, pr_.pg_resume);
#endif
    if (frame_pool::g_pf & 256) {
        // Rolling window: a handler frame is cold (it has waited for the database), and so is the parent frame it returns to.
        // Prefetch the frames kFrameAhead handlers on and the parents of the ones kParentAhead on, whose own lines are in by now.
        constexpr size_t kFrameAhead = 3, kParentAhead = 1;
        using P = Task<Reply>::promise_type;
        auto frame_lines = [&](const void *p, int lines) {
            const char *b = static_cast<const char *>(p);
            for (int l = 0; l < lines; l++) __builtin_prefetch(b + 64 * l, 1);
        };
        const size_t nr = ready.size();
        for (size_t i = 0; i < kFrameAhead && i < nr; i++) frame_lines(ready[i].address(), 4);
        for (size_t i = 0; i < nr; i++) {
            if (i + kFrameAhead < nr) frame_lines(ready[i + kFrameAhead].address(), 4);
            if (frame_pool::g_pf & 32)
                if (i + kParentAhead < nr)
                    if (const void *parent = std::coroutine_handle<P>::from_address(ready[i + kParentAhead].address()).promise().cont.address())
                        frame_lines(parent, 3);
            ready[i].resume();
        }
    } else {
    if (frame_pool::g_pf & 2) {
        for (auto h : ready) {
            const char *b = static_cast<const char *>(h.address());
            __builtin_prefetch(b, 1);
            __builtin_prefetch(b + 64, 1);
            __builtin_prefetch(b + 128, 1);
            __builtin_prefetch(b + 192, 1);
        }
        if (frame_pool::g_pf & 32) {  // each handler hands its result to its parent frame (serve): load that one too
            using P = Task<Reply>::promise_type;
            for (auto h : ready) {
                const void *parent = std::coroutine_handle<P>::from_address(h.address()).promise().cont.address();
                if (!parent) continue;
                const char *b = static_cast<const char *>(parent);
                __builtin_prefetch(b, 1);
                __builtin_prefetch(b + 64, 1);
                __builtin_prefetch(b + 128, 1);
            }
        }
    }
    for (auto h : ready) {
        h.resume();
    }
    }
#ifdef PROBES
    }
    probes::Span probe_pd(pr_.pmu, pr_.pg_dispatch);
#endif
    if (is_write) dispatch_writes();
    dispatch();
}

// Registers the provided-buffer ring. Ubuntu 24.04 kernels 6.8.0-1xx carry a backport whose reserved-field check is
// inverted (`if (!memchr_inv(reg.resv, 0, ...)) return -EINVAL`), so every registration with zeroed reserved fields
// fails with EINVAL, liburing's included. On EINVAL, register again by hand with a non-zero reserved byte, which that
// kernel accepts. A correct kernel never reaches the retry: it accepts the first call.
static io_uring_buf_ring *setup_buf_ring(io_uring *ring, unsigned entries, int bgid, int *err) {
    if (io_uring_buf_ring *br = io_uring_setup_buf_ring(ring, entries, bgid, 0, err)) return br;
    if (*err != -EINVAL) return nullptr;
    const size_t sz = size_t(entries) * sizeof(io_uring_buf);
    void *m = mmap(nullptr, sz, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
    if (m == MAP_FAILED) {
        *err = -errno;
        return nullptr;
    }
    io_uring_buf_reg reg{};
    reg.ring_addr = reinterpret_cast<uintptr_t>(m);
    reg.ring_entries = entries;
    reg.bgid = uint16_t(bgid);
    reg.resv[0] = 1;
    int rc = io_uring_register_buf_ring(ring, &reg, 0);
    if (rc < 0) {
        munmap(m, sz);
        *err = rc;
        return nullptr;
    }
    auto *br = static_cast<io_uring_buf_ring *>(m);
    io_uring_buf_ring_init(br);
    *err = 0;
    return br;
}

int Worker::listen_on(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &one, sizeof one);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(uint16_t(port));
    if (inet_pton(AF_INET, cfg_.host.c_str(), &addr.sin_addr) != 1) {
        fprintf(stderr, "bad HOST %s\n", cfg_.host.c_str());
        exit(1);
    }
    if (bind(fd, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0 || listen(fd, 65535) != 0) {
        perror("bind/listen");
        exit(1);
    }
    return fd;
}

void Worker::run() {
    listen_fd_[0] = listen_on(cfg_.port);
    if (cfg_.extra_port > 0) listen_fd_[1] = listen_on(cfg_.extra_port);

    // The ring is created here so that this thread is its single issuer.
    io_uring_params params{};
    params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN | IORING_SETUP_COOP_TASKRUN;
    int rc = io_uring_queue_init_params(4096, &ring_, &params);
    if (rc < 0) {
        fprintf(stderr, "io_uring_queue_init_params: %s\n", strerror(-rc));
        exit(1);
    }
    int err = 0;
    br_ = setup_buf_ring(&ring_, kBufCount, kBgid, &err);
    if (!br_) {
        fprintf(stderr, "io_uring_setup_buf_ring: %s\n", strerror(-err));
        exit(1);
    }
    bufs_.resize(size_t(kBufCount) * kBufSize);
    for (unsigned i = 0; i < kBufCount; i++)
        io_uring_buf_ring_add(br_, &bufs_[size_t(i) * kBufSize], kBufSize, uint16_t(i),
                              io_uring_buf_ring_mask(kBufCount), int(i));
    io_uring_buf_ring_advance(br_, int(kBufCount));

    arm_accept(0);
    if (listen_fd_[1] >= 0) arm_accept(1);

    io_uring_cqe *cqes[256];
    // Batching window: when the last pass found fewer than kBusyPass completions the worker waits for WAIT_N of them
    // (or WAIT_US) instead of waking for the first one, so one wake-up and one syscall serve several requests. A busy
    // worker (a pass with kBusyPass or more) never waits. WAIT_N=1 turns it off.
    constexpr unsigned kBusyPass = 32;
    const char *e_wn = getenv("WAIT_N"), *e_wu = getenv("WAIT_US");
    const unsigned wait_n = e_wn ? unsigned(atoi(e_wn)) : 32;
    const long wait_us = e_wu ? atol(e_wu) : 2000;
    __kernel_timespec wait_ts{0, wait_us * 1000};
    unsigned last_n = kBusyPass;
#ifdef PROBES
    pr_.pmu.init();
#endif
    for (;;) {
        int w;
#ifdef PROBES
        uint64_t probe_tw = __rdtsc();
#endif
        if (wait_n > 1 && last_n < kBusyPass) {
            io_uring_cqe *c1;
            w = io_uring_submit_and_wait_timeout(&ring_, &c1, wait_n, &wait_ts, nullptr);
        } else {
            w = io_uring_submit_and_wait(&ring_, 1);
        }
#ifdef PROBES
        pr_.wait.t += __rdtsc() - probe_tw;
        pr_.wait.calls++;
        probes::Span probe_pass(pr_.pmu, pr_.pass);
#endif
        if (w < 0 && w != -EINTR && w != -EBUSY && w != -ETIME) [[unlikely]] {
            fprintf(stderr, "worker %d: io_uring_submit_and_wait: %s\n", id_, strerror(-w));
            exit(1);
        }
        {
            timespec ts;
            clock_gettime(CLOCK_MONOTONIC, &ts);
            now_ns_ = uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
            if (now_ns_ >= next_ctl_) {
                next_ctl_ = now_ns_ + 100000000ull;
                control();
            }
        }
        unsigned n = io_uring_peek_batch_cqe(&ring_, cqes, 256);
        last_n = n;
#ifdef PROBES
        pr_.cqes += n;
#endif
        // With ~100k connections a connection's state is almost never in cache when its completion arrives. The whole
        // batch is known up front, so start loading all of those lines now and the misses overlap instead of queueing.
        const bool roll = frame_pool::g_pf & 256;
        // Rolling window (bit 8): the first kAhead connections now, then one more per completion handled.
        auto prefetch_conn = [&](unsigned i) {
            const uint64_t u = cqes[i]->user_data;
            if (op_of(u) == kRecv || op_of(u) == kSend) {
                const size_t k = idx_of(u);
                if (k < conns_.size()) {
                    const char *b = reinterpret_cast<const char *>(&conns_[k]);
                    __builtin_prefetch(b, 1);
                    __builtin_prefetch(b + 64, 1);
                }
            }
        };
        constexpr unsigned kConnAhead = 4, kBufAhead = 2;
        if (roll)
            for (unsigned i = 0; i < kConnAhead && i < n; i++) prefetch_conn(i);
#ifdef PROBES
        {
            probes::Span probe_pf(pr_.pmu, pr_.prefetch);
#endif
        for (unsigned i = 0; i < n && !roll; i++) {
            uint64_t u = cqes[i]->user_data;
            if (op_of(u) == kRecv || op_of(u) == kSend) {
                size_t k = idx_of(u);
                if (k < conns_.size()) {
                    if (frame_pool::g_pf & 128) {
                        __builtin_prefetch(&conns_[k], 1);
                        __builtin_prefetch(reinterpret_cast<const char *>(&conns_[k]) + 64, 1);
                    } else {
                        __builtin_prefetch(&conns_[k]);
                        __builtin_prefetch(reinterpret_cast<const char *>(&conns_[k]) + 64);
                    }
                }
            }
        }
#ifdef PROBES
        }
#endif
        for (unsigned i = 0; i < n; i++) {
            io_uring_cqe *cqe = cqes[i];
            uint64_t u = cqe->user_data;
            if (roll) {
                if (i + kConnAhead < n) prefetch_conn(i + kConnAhead);
                if (const unsigned j = i + kBufAhead; j < n && (frame_pool::g_pf & 4)) {
                    const uint64_t uj = cqes[j]->user_data;
                    if (op_of(uj) == kRecv || op_of(uj) == kSend) {
                        const size_t k = idx_of(uj);
                        if (k < conns_.size()) {
                            Conn &cj = conns_[k];
                            __builtin_prefetch(cj.in.data(), 1);
                            __builtin_prefetch(cj.out.data(), 1);
                            __builtin_prefetch(cj.sendbuf.data(), 1);
                        }
                    }
                }
            } else if (frame_pool::g_pf & 4) {  // second stage: the connection lines were prefetched above, so their buffer pointers are readable now
                const unsigned j = i + ((frame_pool::g_pf & 128) ? 8 : 4);
                if (j < n) {
                    const uint64_t uj = cqes[j]->user_data;
                    if (op_of(uj) == kRecv || op_of(uj) == kSend) {
                        const size_t k = idx_of(uj);
                        if (k < conns_.size()) {
                            Conn &cj = conns_[k];
                            __builtin_prefetch(cj.in.data(), 1);
                            __builtin_prefetch(cj.out.data(), 1);
                            __builtin_prefetch(cj.sendbuf.data(), 1);
                        }
                    }
                }
            }
#ifdef PROBES
            probes::Span probe_op(pr_.pmu, pr_.op[op_of(u)]);
#endif
            switch (op_of(u)) {
            case kAccept:
                if (cqe->res >= 0) on_accept(cqe->res);
                if (!(cqe->flags & IORING_CQE_F_MORE)) arm_accept(int(idx_of(u)));
                break;
            case kRecv: on_recv(idx_of(u), gen_of(u), cqe); break;
            case kSend: on_send(idx_of(u), gen_of(u), cqe->res); break;
            case kPgIn:
                pgs_[idx_of(u)]->in_armed = false;
                on_pg(idx_of(u), cqe->res);
                break;
            case kPgOut: {
                PgConn &pg = *pgs_[idx_of(u)];
                pg.sending = false;
                if (cqe->res > 0) {
                    pg.out_off += size_t(cqe->res);
                    if (pg.out_off >= pg.out_len) pg.out_len = pg.out_off = 0;
                    else arm_pg_out(idx_of(u));
                } else if (cqe->res != -EINTR && cqe->res != -EAGAIN) {
                    pg.dead = true;
                } else {
                    arm_pg_out(idx_of(u));
                }
                break;
            }
            }
        }
        io_uring_cq_advance(&ring_, n);
#ifdef PROBES
        probes::Span probe_ra(pr_.pmu, pr_.rearm);
#endif
        for (uint32_t i = 0; i < pgs_.size(); i++) {
            PgConn &pg = *pgs_[i];
            if (pg.inflight.empty()) continue;
            if (pg.dead) {
                on_pg(i, 0);  // a failed send: on_data() fails the segments in flight
            } else arm_pg(i);
        }
    }
}

#ifdef PROBES
void Worker::probe_report() {
    using namespace probes;
    const double req = double(pr_.op[kRecv].calls - pr_prev_.op[kRecv].calls);
    if (req < 1000) return;
    printf("== probes worker %d, last 10 s: %.0f recv completions (~requests), %.1f CQEs/pass, PG: %.2f members/seg, %.1f rows/req, %.0f B/req\n", id_, req,
           double(pr_.cqes - pr_prev_.cqes) / double(pr_.pass.calls - pr_prev_.pass.calls),
           double(pr_.pg_members - pr_prev_.pg_members) / double(std::max<uint64_t>(1, pr_.pg_segs - pr_prev_.pg_segs)),
           double(pr_.pg_rows - pr_prev_.pg_rows) / req, double(pr_.pg_bytes - pr_prev_.pg_bytes) / req);
    line("pass (all user)", pr_.pass, pr_prev_.pass, req);
    line("  prefetch loop", pr_.prefetch, pr_prev_.prefetch, req);
    line("  op accept", pr_.op[kAccept], pr_prev_.op[kAccept], req);
    line("  op recv", pr_.op[kRecv], pr_prev_.op[kRecv], req);
    line("  op send", pr_.op[kSend], pr_prev_.op[kSend], req);
    line("  op pg-in", pr_.op[kPgIn], pr_prev_.op[kPgIn], req);
    line("  op pg-out", pr_.op[kPgOut], pr_prev_.op[kPgOut], req);
    line("    of which on_pg", pr_.onpg, pr_prev_.onpg, req);
    line("      pg parse", pr_.pg_parse, pr_prev_.pg_parse, req);
    line("      pg assemble", pr_.pg_assemble, pr_prev_.pg_assemble, req);
    line("      pg resume (handlers)", pr_.pg_resume, pr_prev_.pg_resume, req);
    line("      pg dispatch", pr_.pg_dispatch, pr_prev_.pg_dispatch, req);
    line("  rearm loop", pr_.rearm, pr_prev_.rearm, req);
    Stat own = pr_.pass, ownp = pr_prev_.pass;  // the pass minus everything measured inside it: Worker::run's own code
    own.calls = pr_.pass.calls;
    for (int k = 0; k < 7; k++) {
        const Stat *s = k == 0 ? &pr_.prefetch : k == 1 ? &pr_.rearm : &pr_.op[k - 2];
        const Stat *q = k == 0 ? &pr_prev_.prefetch : k == 1 ? &pr_prev_.rearm : &pr_prev_.op[k - 2];
        for (int i = 0; i < kN; i++) {
            own.v[i] -= s->v[i];
            ownp.v[i] -= q->v[i];
        }
    }
    line("run() own code", own, ownp, req);
    fflush(stdout);
    pr_prev_ = pr_;
}
#endif

Worker::Group *Worker::new_group() {
    if (group_pool_.empty()) return new Group;
    Group *g = group_pool_.back();
    group_pool_.pop_back();
    return g;
}

void Worker::free_group(Group *g) {
    g->members.clear();
    if (group_pool_.size() < 64) group_pool_.push_back(g);
    else delete g;
}
