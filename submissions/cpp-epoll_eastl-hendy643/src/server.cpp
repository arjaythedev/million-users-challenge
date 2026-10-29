#include "server.hpp"

#include <EASTL/vector.h>
#include <liburing.h>
#include <sys/mman.h>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <future>
#include <memory>
#include <thread>

#include "api.hpp"
#include "net.hpp"
#include "pg_pool.hpp"
#include "response.hpp"
#include "tag.hpp"
#include "tuning.hpp"
#include "watcher.hpp"

namespace feed
{
namespace
{
using Clock = std::chrono::steady_clock;

using tuning::kMaxQueuedInput;

struct Listener : Tagged
{
    net::Socket fd;

    explicit Listener(const net::Socket s) :
        Tagged(Tag::Listener),
        fd(s)
    {
    }
};

struct Conn : Tagged
{
    net::Socket fd = net::kInvalid;
    bool close_after = false;
    bool waiting = false;
    bool closed = false;
    bool queued_free = false;
    bool keep_alive = true;
    bool gzip = false;
    String in;
    String out;
    bool recv_active = false;
    bool send_active = false;
    String sending;
    size_t send_off = 0;

    Conn() :
        Tagged(Tag::Client)
    {
    }
};

// The low three bits of user_data say what a request is; the rest is its 8-byte aligned object.
enum Op : unsigned
{
    kOpAccept = 0,
    kOpRecv = 1,
    kOpSend = 2,
    kOpPoll = 3,
    kOpIgnore = 7
};

uint64_t user_data(const Tagged *object, const unsigned op)
{
    return reinterpret_cast<uintptr_t>(object) | op;
}

template <class T> T *downcast(Tagged *object)
{
    return static_cast<T *>(object); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
}

class Worker final : public DbSink, public Watcher
{
  public:
    Worker(const ServerConfig &config, const eastl::vector<net::Socket> &listeners, const Clock::time_point started,
           const size_t db_connections) :
        config_(config),
        started_(started)
    {
        if (!open_loop(listeners))
            return;
        pool_ = std::make_unique<PgPool>(*this, *this, config.database_url, db_connections);
        ready_ = true;
    }

    [[nodiscard]] bool ok() const
    {
        return ready_;
    }

    void on_db_reply(void *token, const DbOp &op, DbReply &&reply) override
    {
        const auto c = static_cast<Conn *>(token);
        c->waiting = false;
        if (c->closed)
        {
            release(c);
            return;
        }
        if (!reply.ok && op.kind != DbKind::Ping)
        {
            std::fprintf(stderr, "database error: %s\n", reply.error.c_str());
        }
        const ApiResponse r = finish_request(op, eastl::move(reply), uptime_s());
        append_http_response(c->out, r.status, View(r.body), c->keep_alive, c->gzip);
        if (!c->keep_alive)
            c->close_after = true;

        if (c->close_after || c->in.empty())
        {
            c->in = String();
            return flush(c);
        }
        const String queued = eastl::move(c->in);
        c->in = String();
        process(c, View(queued));
    }

  private:
    const ServerConfig &config_;
    Clock::time_point started_;
    bool ready_ = false;
    std::unique_ptr<PgPool> pool_;
    eastl::vector<Conn *> graveyard_;

    [[nodiscard]] int64_t uptime_s() const
    {
        return std::chrono::duration_cast<std::chrono::seconds>(Clock::now() - started_).count();
    }

    [[nodiscard]] int wait_ms() const
    {
        return extra_wait_ms(pool_->timeout_ms());
    }

    void on_data(Conn *c, const char *data, const size_t n)
    {
        if (c->waiting)
        {
            if (c->in.size() + n > kMaxQueuedInput)
                return close_conn(c);
            c->in.append(data, n);
            return;
        }
        if (c->in.empty())
        {
            process(c, View(data, n));
        }
        else
        {
            c->in.append(data, n);
            process(c, View(c->in));
        }
    }

    // shortcut: a client that pipelines without ever reading grows `out` without bound; cap it if that matters.
    void process(Conn *c, const View data)
    {
        size_t off = 0;
        while (off < data.size() && !c->close_after && !c->waiting)
        {
            const ParseResult parsed = parse_http_request(data.substr(off));
            if (parsed.status == ParseStatus::Incomplete)
                break;
            if (parsed.status == ParseStatus::Complete)
            {
                off += parsed.consumed;
                dispatch(c, parsed.request);
                continue;
            }
            const bool too_large = parsed.status == ParseStatus::TooLarge;
            append_http_response(c->out, too_large ? 413 : 400,
                                 View(error_body(too_large ? "request too large" : "bad request")), false, false);
            c->close_after = true;
        }

        if (c->close_after || off == data.size())
        {
            c->in = String();
        }
        else if (off > 0 || data.data() != c->in.data())
        {
            String rest(data.substr(off));
            c->in = eastl::move(rest);
        }
        flush(c);
    }

    void dispatch(Conn *c, const HttpRequest &req)
    {
        Plan plan = plan_request(View(config_.jwt_secret), std::time(nullptr), req);
        if (plan.done)
        {
            append_http_response(c->out, plan.response.status, View(plan.response.body), req.keep_alive,
                                 req.accepts_gzip);
            if (!req.keep_alive)
                c->close_after = true;
            return;
        }
        c->waiting = true;
        c->keep_alive = req.keep_alive;
        c->gzip = req.accepts_gzip;
        pool_->submit(eastl::move(plan.op), static_cast<Tagged *>(c));
    }

    void release(Conn *c)
    {
        if (!c->closed || c->recv_active || c->send_active)
            return;
        if (c->fd != net::kInvalid)
        {
            net::close_socket(c->fd);
            c->fd = net::kInvalid;
        }
        if (c->waiting || c->queued_free)
            return;
        c->queued_free = true;
        graveyard_.push_back(c);
    }

    void free_graveyard()
    {
        for (const Conn *c : graveyard_)
            delete c;
        graveyard_.clear();
    }

    io_uring ring_{};
    io_uring_buf_ring *buf_ring_ = nullptr;
    std::unique_ptr<char[]> buffers_;
    eastl::vector<Listener *> listeners_;
    eastl::vector<Listener *> accept_retry_;

    static constexpr unsigned kBufferMask = tuning::kRecvBuffers - 1;
    static_assert((tuning::kRecvBuffers & kBufferMask) == 0, "kRecvBuffers must be a power of two");

    bool open_loop(const eastl::vector<net::Socket> &listeners)
    {
        io_uring_params p{};
        p.cq_entries = tuning::kUringCompletions;
        p.flags = IORING_SETUP_CQSIZE | IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_DEFER_TASKRUN;
        int rc = io_uring_queue_init_params(tuning::kUringEntries, &ring_, &p);
        if (rc < 0)
        {
            p = io_uring_params{};
            p.cq_entries = tuning::kUringCompletions;
            p.flags = IORING_SETUP_CQSIZE;
            rc = io_uring_queue_init_params(tuning::kUringEntries, &ring_, &p);
        }
        if (rc < 0)
        {
            std::fprintf(stderr, "io_uring_queue_init failed: %s\n", std::strerror(-rc));
            return false;
        }

        int err = 0;
        buf_ring_ = io_uring_setup_buf_ring(&ring_, tuning::kRecvBuffers, 0, 0, &err);
        if (!buf_ring_ && err == -EINVAL)
            buf_ring_ = register_buf_ring_for_buggy_kernel(&err);
        if (!buf_ring_)
        {
            std::fprintf(stderr, "io_uring_setup_buf_ring failed: %s\n", std::strerror(-err));
            return false;
        }
        buffers_ = std::make_unique<char[]>(static_cast<size_t>(tuning::kRecvBuffers) * tuning::kRecvBufferSize);
        for (unsigned i = 0; i < tuning::kRecvBuffers; i++)
        {
            io_uring_buf_ring_add(buf_ring_, buffers_.get() + static_cast<size_t>(i) * tuning::kRecvBufferSize,
                                  tuning::kRecvBufferSize, static_cast<unsigned short>(i), kBufferMask,
                                  static_cast<int>(i));
        }
        io_uring_buf_ring_advance(buf_ring_, tuning::kRecvBuffers);

        for (const net::Socket s : listeners)
            listeners_.push_back(new Listener(s));
        return true;
    }

    // Some Ubuntu 24.04 6.8 kernels check the reserved field of IORING_REGISTER_PBUF_RING the wrong way round:
    // liburing's call (field zero) gets EINVAL and a non-zero field is accepted. A correct kernel rejects that.
    io_uring_buf_ring *register_buf_ring_for_buggy_kernel(int *err)
    {
        const size_t bytes = static_cast<size_t>(tuning::kRecvBuffers) * sizeof(io_uring_buf);
        void *memory = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
        if (memory == MAP_FAILED)
        {
            *err = -errno;
            return nullptr;
        }
        io_uring_buf_reg reg{};
        reg.ring_addr = reinterpret_cast<uintptr_t>(memory);
        reg.ring_entries = tuning::kRecvBuffers;
        reg.bgid = 0;
        reg.resv[0] = 1;
        if (const int rc = io_uring_register_buf_ring(&ring_, &reg, 0); rc < 0)
        {
            munmap(memory, bytes);
            *err = rc;
            return nullptr;
        }
        auto *ring = static_cast<io_uring_buf_ring *>(memory);
        io_uring_buf_ring_init(ring);
        return ring;
    }

    [[nodiscard]] int extra_wait_ms(const int wait) const
    {
        if (accept_retry_.empty())
            return wait;
        return wait < 0 ? 10 : std::min(wait, 10);
    }

    io_uring_sqe *sqe()
    {
        io_uring_sqe *s = io_uring_get_sqe(&ring_);
        if (!s)
        {
            io_uring_submit(&ring_);
            s = io_uring_get_sqe(&ring_);
        }
        return s;
    }

    void arm_accept(Listener *l)
    {
        io_uring_sqe *s = sqe();
        io_uring_prep_multishot_accept(s, l->fd, nullptr, nullptr, SOCK_CLOEXEC);
        io_uring_sqe_set_data64(s, user_data(l, kOpAccept));
    }

    void start_recv(Conn *c)
    {
        io_uring_sqe *s = sqe();
        io_uring_prep_recv_multishot(s, c->fd, nullptr, 0, 0);
        s->flags |= IOSQE_BUFFER_SELECT;
        s->buf_group = 0;
        io_uring_sqe_set_data64(s, user_data(c, kOpRecv));
        c->recv_active = true;
    }

    void submit_send(Conn *c)
    {
        io_uring_sqe *s = sqe();
        io_uring_prep_send(s, c->fd, c->sending.data() + c->send_off, c->sending.size() - c->send_off, MSG_NOSIGNAL);
        io_uring_sqe_set_data64(s, user_data(c, kOpSend));
        c->send_active = true;
    }

    void cancel(const uint64_t target)
    {
        io_uring_sqe *s = sqe();
        io_uring_prep_cancel64(s, target, 0);
        io_uring_sqe_set_data64(s, kOpIgnore);
    }

    void recycle(const unsigned buffer) const
    {
        io_uring_buf_ring_add(buf_ring_, buffers_.get() + static_cast<size_t>(buffer) * tuning::kRecvBufferSize,
                              tuning::kRecvBufferSize, static_cast<unsigned short>(buffer), kBufferMask, 0);
        io_uring_buf_ring_advance(buf_ring_, 1);
    }

    void arm(Tagged *who)
    {
        who->gen = static_cast<unsigned char>((who->gen + 1) & 3);
        io_uring_sqe *s = sqe();
        io_uring_prep_poll_add(s, who->watch_fd, who->watch_events);
        io_uring_sqe_set_data64(s, user_data(who, kOpPoll + who->gen));
        who->poll_live = true;
    }

    void disarm(Tagged *who)
    {
        if (!who->poll_live)
            return;
        who->poll_live = false;
        io_uring_sqe *s = sqe();
        io_uring_prep_poll_remove(s, user_data(who, kOpPoll + who->gen));
        io_uring_sqe_set_data64(s, kOpIgnore);
    }

  public:
    bool add(Tagged *who, const net::Socket fd, const uint32_t events) override
    {
        who->watch_fd = fd;
        who->watch_events = events;
        arm(who);
        return true;
    }

    void modify(Tagged *who, net::Socket, const uint32_t events) override
    {
        disarm(who);
        who->watch_events = events;
        arm(who);
    }

    void remove(Tagged *who, net::Socket) override
    {
        disarm(who);
        who->watch_events = 0;
        who->watch_fd = -1;
    }

    void run()
    {
        for (Listener *l : listeners_)
            arm_accept(l);
        for (;;)
        {
            for (Listener *l : accept_retry_)
                arm_accept(l);
            accept_retry_.clear();

            const int wait = wait_ms();
            if (wait == 0)
            {
                io_uring_submit_and_get_events(&ring_);
            }
            else if (wait < 0)
            {
                io_uring_submit_and_wait(&ring_, 1);
            }
            else
            {
                __kernel_timespec ts{};
                ts.tv_sec = wait / 1000;
                ts.tv_nsec = static_cast<long long>(wait % 1000) * 1000000;
                io_uring_cqe *first = nullptr;
                io_uring_submit_and_wait_timeout(&ring_, &first, 1, &ts, nullptr);
            }

            unsigned head = 0, seen = 0;
            io_uring_cqe *cqe = nullptr;
            io_uring_for_each_cqe(&ring_, head, cqe)
            {
                on_completion(cqe);
                seen++;
            }
            io_uring_cq_advance(&ring_, seen);

            pool_->poll();
            free_graveyard();
        }
    }

  private:
    void on_completion(const io_uring_cqe *cqe)
    {
        const uint64_t ud = cqe->user_data;
        const auto op = static_cast<unsigned>(ud & 7);
        if (op == kOpIgnore)
            return;
        const auto object = reinterpret_cast<Tagged *>(ud & ~uint64_t{7});
        switch (op)
        {
            case kOpAccept:
                return on_accept(downcast<Listener>(object), cqe);
            case kOpRecv:
                return on_recv(downcast<Conn>(object), cqe);
            case kOpSend:
                return on_send(downcast<Conn>(object), cqe);
            default:
                return on_poll(object, op - kOpPoll, cqe);
        }
    }

    void on_accept(Listener *l, const io_uring_cqe *cqe)
    {
        if (cqe->res >= 0)
        {
            const int fd = cqe->res;
            const int on = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
            const auto c = new Conn;
            c->fd = fd;
            start_recv(c);
        }
        if (!(cqe->flags & IORING_CQE_F_MORE))
            accept_retry_.push_back(l);
    }

    void on_recv(Conn *c, const io_uring_cqe *cqe)
    {
        const int res = cqe->res;
        if (res > 0)
        {
            const unsigned buffer = cqe->flags >> IORING_CQE_BUFFER_SHIFT;
            if (!c->closed)
            {
                on_data(c, buffers_.get() + static_cast<size_t>(buffer) * tuning::kRecvBufferSize,
                        static_cast<size_t>(res));
            }
            recycle(buffer);
        }
        if (cqe->flags & IORING_CQE_F_MORE)
            return;
        c->recv_active = false;
        if (!c->closed)
        {
            if (res > 0 || res == -ENOBUFS)
            {
                start_recv(c);
            }
            else
            {
                close_conn(c);
            }
        }
        release(c);
    }

    void on_send(Conn *c, const io_uring_cqe *cqe)
    {
        c->send_active = false;
        if (c->closed)
            return release(c);
        const int res = cqe->res;
        if (res == -EAGAIN || res == -EINTR)
            return submit_send(c);
        if (res <= 0)
            return close_conn(c);
        c->send_off += static_cast<size_t>(res);
        if (c->send_off < c->sending.size())
            return submit_send(c);
        c->sending = String();
        c->send_off = 0;
        flush(c);
    }

    void on_poll(Tagged *who, const unsigned generation, const io_uring_cqe *cqe)
    {
        if (generation != who->gen || !who->poll_live)
            return;
        who->poll_live = false;
        pool_->on_event(who, cqe->res > 0 ? static_cast<uint32_t>(cqe->res) : 8u /* POLLERR */);
        if (!who->poll_live && who->watch_events != 0)
            arm(who);
    }

    void close_conn(Conn *c)
    {
        if (c->closed)
            return;
        c->closed = true;
        shutdown(c->fd, SHUT_RDWR);
        if (c->recv_active)
            cancel(user_data(c, kOpRecv));
        if (c->send_active)
            cancel(user_data(c, kOpSend));
        release(c);
    }

    void flush(Conn *c)
    {
        if (c->closed || c->send_active)
            return;
        if (c->out.empty())
        {
            if (c->close_after)
                close_conn(c);
            return;
        }
        c->sending = eastl::move(c->out);
        c->out = String();
        c->send_off = 0;
        submit_send(c);
    }
};

net::Socket listen_on(const ServerConfig &config, const int port, const bool reuse_port)
{
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(static_cast<uint16_t>(port));
    if (inet_pton(AF_INET, config.host.c_str(), &addr.sin_addr) != 1)
    {
        std::fprintf(stderr, "HOST must be an IPv4 address: %s\n", config.host.c_str());
        return net::kInvalid;
    }
    const net::Socket s = socket(AF_INET, SOCK_STREAM, 0);
    if (s == net::kInvalid)
        return s;
    const int on = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &on, sizeof on);
    if (reuse_port)
        setsockopt(s, SOL_SOCKET, SO_REUSEPORT, &on, sizeof on);
    // The listener stays blocking, because io_uring reports -EAGAIN for a non-blocking socket instead of waiting.
    if (bind(s, reinterpret_cast<sockaddr *>(&addr), sizeof addr) != 0 || listen(s, tuning::kListenBacklog) != 0)
    {
        std::fprintf(stderr, "cannot listen on %s:%d\n", config.host.c_str(), port);
        net::close_socket(s);
        return net::kInvalid;
    }
    return s;
}
} // namespace

int run_server(const ServerConfig &config)
{
    const size_t cores = std::max<size_t>(1, std::thread::hardware_concurrency());
    const size_t threads = std::min(tuning::kMaxThreads, std::max<size_t>(1, tuning::kThreadsPerCore * cores));
    const size_t per_thread = std::max<size_t>(1, tuning::kDbConnections / threads);
    const Clock::time_point started = Clock::now();
    std::printf("serving on %s:%d and :%d with %zu threads, %zu database connections each\n", config.host.c_str(),
                config.port, config.extra_port, threads, per_thread);
    std::fflush(stdout);

    const auto run_loop = [&] {
        eastl::vector<net::Socket> listeners;
        for (const int port : {config.port, config.extra_port})
        {
            const net::Socket s = listen_on(config, port, true);
            if (s != net::kInvalid)
                listeners.push_back(s);
        }
        Worker worker(config, listeners, started, per_thread);
        if (worker.ok())
        {
            worker.run();
        }
        else
        {
            std::fprintf(stderr, "the event loop could not start\n");
        }
    };
    eastl::vector<std::future<void>> loops;
    for (size_t i = 0; i < threads; i++)
        loops.push_back(std::async(std::launch::async, run_loop));
    for (auto &loop : loops)
        loop.get();
    return 1;
}
} // namespace feed
