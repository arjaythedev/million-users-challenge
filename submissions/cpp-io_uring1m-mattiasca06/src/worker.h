// One event loop: its own io_uring, SO_REUSEPORT listeners, connection slab and Postgres connections.
// Each request runs as a coroutine that awaits Postgres segments.
#pragma once

#include <liburing.h>

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <cstdlib>
#include <new>
#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "coro.h"
#include "hugemem.h"
#include "pg.h"

// Allocator for page-aligned storage.
template <class T> struct Aligned4k {
    using value_type = T;
    Aligned4k() = default;
    template <class U> Aligned4k(const Aligned4k<U> &) {}
    T *allocate(size_t n) {
        void *p = std::aligned_alloc(4096, (n * sizeof(T) + 4095) / 4096 * 4096);
        if (!p) throw std::bad_alloc();
        return static_cast<T *>(p);
    }
    void deallocate(T *p, size_t) { std::free(p); }
    template <class U> bool operator==(const Aligned4k<U> &) const { return true; }
};

struct Config {
    std::string host, database_url, jwt_secret;
    int port = 80, extra_port = 0, workers = 4, read_conns = 1, read_conns_max = 4;
    int pool_wait_us = 0;  // 0 = automatic: congestion is a wait of over two segment service times; else a fixed limit
    int read_group_max = 64;  // most reads one pipeline segment carries (they travel together only if they queued up)
    int gzip_min = 512;       // compress responses of at least this many bytes when the client accepts gzip (0 = never)
};

struct Request {
    std::string method, path, body, auth;
    bool has_auth = false;
    bool accept_gzip = false;
};

struct Reply {
    int status = 200;
    std::string body;
};

struct Worker;

// co_await-ed from a request coroutine: one pipeline segment on any free Postgres connection.
struct DbAwait {
    DbAwait(Worker &w, QVec qs) : w(w), qs(std::move(qs)) {}
    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> c);
    Rows await_resume() { return std::move(rows); }

    Worker &w;
    QVec qs;
    bool write = false;  // goes to the dedicated write connection
    bool solo = false;   // never shares a segment with other requests
    Rows rows;
    std::coroutine_handle<> h;
    uint64_t t_enq = 0;  // when a read joined the queue (ns, the worker's loop clock)
};

struct alignas(64) Worker {
    Worker(const Config &cfg, int id);
    ~Worker();
    void run();

    // used by awaitables
    void enqueue(DbAwait *a);

  private:
    struct alignas(64) Conn {  // two cache lines per connection, never straddling a third
        int fd = -1;
        uint32_t gen = 0;
        bool recv_active = false, sending = false, busy = false, closing = false, close_after = false;
        std::string in, out, sendbuf;
    };

    // event loop and HTTP
    io_uring_sqe *sqe();
    void arm_accept(int which);
    void arm_recv(uint32_t idx);
    void arm_pg(uint32_t pg_idx);
    void arm_pg_out(uint32_t pg_idx);
    void on_accept(int fd);
    void on_recv(uint32_t idx, uint32_t gen, io_uring_cqe *cqe);
    void on_send(uint32_t idx, uint32_t gen, int res);
    void on_pg(uint32_t pg_idx, long res);
    void process(uint32_t idx);
    void process_one(uint32_t idx);
    void respond(Conn &c, uint32_t idx, int status, const std::string &body, bool gz);
    void start_send(uint32_t idx);
    void kill(uint32_t idx);
    void maybe_free(uint32_t idx);
    void dispatch();
    void dispatch_writes();
    int listen_on(int port);
    struct Group;
    Detached serve(uint32_t idx, uint32_t gen, Request rq);
    void finish(uint32_t idx, uint32_t gen, Reply r, bool gz);

    // request handlers (handlers.cpp)
    Task<Reply> route(const Request &rq);
    Task<Reply> h_health();
    Task<Reply> h_feed();
    Task<Reply> h_get_post(int64_t id);
    Task<Reply> h_create_post(const Request &rq, int64_t user, std::string username);
    Task<Reply> h_like(int64_t user, int64_t post_id);

    DbAwait db(QVec qs) { return DbAwait(*this, std::move(qs)); }
    // Writes commit before the awaiting request resumes. Group-able ones share one Sync with their neighbours.
    DbAwait db_write(QVec qs, bool solo = false) {
        DbAwait a(*this, std::move(qs));
        a.write = true;
        a.solo = solo;
        return a;
    }

    Config cfg_;
    int id_;
    int listen_fd_[2] = {-1, -1};
    io_uring ring_{};
    io_uring_buf_ring *br_ = nullptr;
    std::vector<char, Aligned4k<char>> bufs_;  // page-aligned: every 4 KiB receive buffer starts on a cache line
    std::vector<Conn, hugemem::Alloc<Conn>> conns_;
    std::vector<uint32_t> free_;
    std::vector<std::unique_ptr<PgConn>> pgs_;
    std::deque<DbAwait *> waitq_, writeq_;
    struct Group {
        std::vector<DbAwait *> members;
    };
    // Adaptive read pool: starts with cfg.read_conns, adds one connection (up to read_conns_max) when the oldest waiting
    // read has stayed over pool_wait_us for 10 consecutive 100 ms windows while the read connections were busy. New connections are opened on a helper thread
    // (blocking libpq connect + prepare) and picked up by control(); the event loop never waits for them.
    std::vector<uint32_t> read_idx_;  // indices into pgs_ of the read connections, in preference order
    uint32_t wi_ = 0;                 // index of the write connection
    std::atomic<PgConn *> pending_{nullptr};
    std::atomic<int> grow_state_{0};  // 0 idle, 1 connecting, 2 connected (in pending_), 3 failed (growth stops)
    uint64_t now_ns_ = 0, win_wait_max_ = 0, next_ctl_ = 0;
    int congested_ = 0, hot_ = 0, idle_windows_ = 0;
    std::vector<uint32_t> spare_;  // retired read connections, connected and idle, ready to be taken back
    uint64_t ctl_prev_ns_ = 0;
    double svc_ewma_ns_ = 0;  // moving average of how long a read segment takes (send to last result)
    void control();
    static constexpr size_t kMaxGroup = 48;  // writes per segment; stays well below 64 in-flight xids per transaction
    std::vector<Group *> group_pool_;  // spare Groups with their vectors' capacity
    std::vector<const Query *> all_;   // scratch: statements of the segment being built (they stay in the requests' frames)
    std::vector<PgConn::Seg> seg_done_;  // scratch for on_pg
    std::vector<std::coroutine_handle<>> ready_;
    Group *new_group();
    void free_group(Group *g);
    bool in_process_ = false;
    std::vector<uint32_t> deferred_;
    static constexpr unsigned kBufCount = 4096, kBufSize = 4096, kBgid = 1;
};
