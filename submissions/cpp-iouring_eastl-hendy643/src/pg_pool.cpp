#include "pg_pool.hpp"

#include <netdb.h>

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <iterator>

#include "net.hpp"
#include "tuning.hpp"

namespace feed
{
namespace
{
#define FEED_SELECT_POST                                                                                               \
    "SELECT p.id, p.body, p.created_at, u.username, "                                                                  \
    "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count "                                        \
    "FROM posts p JOIN users u ON u.id = p.user_id"

constexpr uint32_t kInt8 = 20;
constexpr uint32_t kText = 25;
const uint32_t kOneInt[] = {kInt8};
const uint32_t kTwoInt[] = {kInt8, kInt8};
const uint32_t kIntText[] = {kInt8, kText};

struct Statement
{
    const char *name;
    const char *sql;
    size_t params;
    const uint32_t *types;
};

const Statement kStatements[] = {
    {"ping", "SELECT 1", 0, nullptr},
    {"feed", FEED_SELECT_POST " ORDER BY p.created_at DESC, p.id DESC LIMIT 20", 0, nullptr},
    {"get", FEED_SELECT_POST " WHERE p.id = $1", 1, kOneInt},
    {"create", "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at", 2, kIntText},
    {"like", "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING", 2,
     kTwoInt},
};
constexpr size_t kStatementCount = std::size(kStatements);

const char *statement_name(const DbKind kind)
{
    switch (kind)
    {
        case DbKind::Ping:
            return "ping";
        case DbKind::Feed:
            return "feed";
        case DbKind::Get:
            return "get";
        case DbKind::Create:
            return "create";
        case DbKind::Like:
            return "like";
    }
    return "ping";
}

constexpr auto kRetryAfter = std::chrono::seconds(tuning::kDbRetrySeconds);
constexpr size_t kScratchBytes = 64 * 1024;

pg::Value int8_value(char (&buf)[8], const int64_t n)
{
    const auto v = static_cast<uint64_t>(n);
    for (int i = 0; i < 8; i++)
        buf[i] = static_cast<char>(v >> (56 - 8 * i));
    return {buf, 8};
}

DbReply error_reply(String message)
{
    DbReply r;
    r.ok = false;
    r.error = eastl::move(message);
    return r;
}

DbReply rejected(const DbKind kind, const pg::ErrorInfo &error)
{
    if (kind == DbKind::Like && View(error.sqlstate) == View("23503"))
    {
        DbReply r;
        r.like = LikeOutcome::PostMissing;
        return r;
    }
    return error_reply(error.message.empty() ? String("database error") : error.message);
}

String with_detail(const char *what, const char *detail)
{
    String s(what);
    s.append(": ", 2);
    s.append(detail);
    return s;
}

String errno_text(const char *what, const int err)
{
    return with_detail(what, std::strerror(err));
}
}

struct PgSlot : Tagged
{
    enum class State : unsigned char
    {
        Dead,
        Connecting,
        Startup,
        Preparing,
        Ready
    };

    struct Inflight
    {
        DbOp op;
        void *token;
    };

    State state = State::Dead;
    net::Socket sock = net::kInvalid;
    bool watched = false;
    uint32_t interest = 0;
    std::chrono::steady_clock::time_point retry_at{};

    String rx;
    String tx;
    size_t tx_off = 0;

    std::unique_ptr<pg::Scram> scram;
    size_t prepared = 0;

    eastl::deque<Inflight> inflight;
    eastl::deque<uint32_t> batches;
    uint32_t open = 0;
    bool dirty = false;

    uint32_t done = 0;
    eastl::vector<DbReply> replies;
    DbReply current;
    uint32_t current_rows = 0;
    bool has_error = false;
    uint32_t error_at = 0;
    pg::ErrorInfo error;

    PgSlot() :
        Tagged(Tag::Pg)
    {
    }
};

PgPool::PgPool(Watcher &watcher, DbSink &sink, const String &url, const size_t connections) :
    watcher_(watcher),
    sink_(sink),
    target_(pg::parse_url(View(url))),
    scratch_(new char[kScratchBytes])
{
    for (size_t i = 0; i < connections; i++)
    {
        slots_.push_back(new PgSlot);
        start_connect(slots_.back());
    }
}

PgPool::~PgPool()
{
    for (PgSlot *slot : slots_)
    {
        if (slot->sock != net::kInvalid)
            net::close_socket(slot->sock);
        delete slot;
    }
}

bool PgPool::any_alive() const
{
    return std::any_of(slots_.begin(), slots_.end(),
                       [](const PgSlot *slot) { return slot->state != PgSlot::State::Dead; });
}

PgSlot *PgPool::best_slot() const
{
    PgSlot *best = nullptr;
    for (PgSlot *slot : slots_)
    {
        if (slot->state != PgSlot::State::Ready)
            continue;
        if (!best || slot->inflight.size() < best->inflight.size())
            best = slot;
    }
    return best && best->inflight.size() < tuning::kPipelineDepth ? best : nullptr;
}

void PgPool::watch(PgSlot *slot, const uint32_t events)
{
    if (!slot->watched)
    {
        if (!watcher_.add(slot, slot->sock, events))
            return fail(slot, "cannot watch a database socket");
        slot->watched = true;
    }
    else if (events != slot->interest)
    {
        watcher_.modify(slot, slot->sock, events);
    }
    slot->interest = events;
}

void PgPool::unwatch(PgSlot *slot) const
{
    if (!slot->watched)
        return;
    watcher_.remove(slot, slot->sock);
    slot->watched = false;
    slot->interest = 0;
}

bool PgPool::flush(PgSlot *slot)
{
    while (slot->tx_off < slot->tx.size())
    {
        const ssize_t n =
            send(slot->sock, slot->tx.data() + slot->tx_off, slot->tx.size() - slot->tx_off, MSG_NOSIGNAL);
        if (n > 0)
        {
            slot->tx_off += static_cast<size_t>(n);
        }
        else if (n < 0 && errno == EINTR)
        {
        }
        else if (n < 0 && errno == EAGAIN)
        {
            watch(slot, kWatchIn | kWatchOut);
            return slot->state != PgSlot::State::Dead;
        }
        else
        {
            fail(slot, errno_text("sending to the database", n < 0 ? errno : EPIPE));
            return false;
        }
    }
    slot->tx.clear();
    slot->tx_off = 0;
    watch(slot, kWatchIn);
    return slot->state != PgSlot::State::Dead;
}

void PgPool::start_connect(PgSlot *slot)
{
    if (!target_)
        return fail(slot, target_.error());
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    addrinfo *found = nullptr;
    if (const int rc = getaddrinfo(target_->host.c_str(), target_->port.c_str(), &hints, &found); rc != 0)
    {
        return fail(slot, with_detail("cannot resolve the database host", gai_strerror(rc)));
    }
    slot->sock = socket(found->ai_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (slot->sock == net::kInvalid)
    {
        freeaddrinfo(found);
        return fail(slot, errno_text("socket", errno));
    }
    const int on = 1;
    setsockopt(slot->sock, IPPROTO_TCP, TCP_NODELAY, &on, sizeof on);
    const int rc = connect(slot->sock, found->ai_addr, found->ai_addrlen);
    const int err = errno;
    freeaddrinfo(found);
    if (rc != 0 && err != EINPROGRESS)
        return fail(slot, errno_text("connecting to the database", err));
    slot->state = PgSlot::State::Connecting;
    watch(slot, kWatchOut);
}

void PgPool::fail(PgSlot *slot, const String &message)
{
    last_error_ = message;
    for (auto &f : slot->inflight)
        failed_.push_back({eastl::move(f.op), f.token, message});
    slot->inflight.clear();
    slot->batches.clear();
    slot->open = 0;
    slot->done = 0;
    slot->replies.clear();
    slot->current = DbReply();
    slot->current_rows = 0;
    slot->has_error = false;
    slot->error = pg::ErrorInfo();
    slot->rx = String();
    slot->tx = String();
    slot->tx_off = 0;
    slot->scram.reset();
    slot->prepared = 0;
    slot->dirty = false;
    unwatch(slot);
    if (slot->sock != net::kInvalid)
    {
        net::close_socket(slot->sock);
        slot->sock = net::kInvalid;
    }
    slot->state = PgSlot::State::Dead;
    slot->retry_at = Clock::now() + kRetryAfter;
    if (!any_alive())
        fail_all_pending(message);
}

void PgPool::fail_all_pending(const String &message)
{
    for (Pending &p : pending_)
        failed_.push_back({eastl::move(p.op), p.token, message});
    pending_.clear();
}

void PgPool::finish_connect(PgSlot *slot)
{
    int err = 0;
    socklen_t len = sizeof err;
    if (getsockopt(slot->sock, SOL_SOCKET, SO_ERROR, &err, &len) != 0)
        err = errno;
    if (err != 0)
        return fail(slot, errno_text("connecting to the database", err));
    slot->state = PgSlot::State::Startup;
    pg::append_startup(slot->tx, View(target_->user), View(target_->database), tuning::kSessionSettings,
                       std::size(tuning::kSessionSettings));
    flush(slot);
}

void PgPool::send_prepares(PgSlot *slot)
{
    slot->state = PgSlot::State::Preparing;
    slot->prepared = 0;
    for (const Statement &s : kStatements)
        pg::append_parse(slot->tx, s.name, s.sql, s.types, s.params);
    pg::append_sync(slot->tx);
    flush(slot);
}

bool PgPool::on_authentication(PgSlot *slot, const View body)
{
    if (body.size() < 4)
    {
        fail(slot, "malformed authentication message");
        return false;
    }
    const uint32_t code = (static_cast<uint32_t>(static_cast<unsigned char>(body[0])) << 24) |
                          (static_cast<uint32_t>(static_cast<unsigned char>(body[1])) << 16) |
                          (static_cast<uint32_t>(static_cast<unsigned char>(body[2])) << 8) |
                          static_cast<uint32_t>(static_cast<unsigned char>(body[3]));
    const View rest = body.substr(4);
    switch (code)
    {
        case 0:
            return true;
        case 3:
            pg::append_password(slot->tx, View(target_->password));
            return flush(slot);
        case 10: {
            bool scram_offered = false;
            for (size_t i = 0; i < rest.size();)
            {
                size_t e = i;
                while (e < rest.size() && rest[e] != '\0')
                    e++;
                if (rest.substr(i, e - i) == View("SCRAM-SHA-256"))
                    scram_offered = true;
                i = e + 1;
            }
            if (!scram_offered)
            {
                fail(slot, "the database offers no SASL mechanism but SCRAM-SHA-256, the only one supported");
                return false;
            }
            slot->scram = std::make_unique<pg::Scram>(View(), View(target_->password), View(pg::random_nonce()));
            pg::append_sasl_initial(slot->tx, "SCRAM-SHA-256", View(slot->scram->first_message()));
            return flush(slot);
        }
        case 11: {
            if (!slot->scram)
                break;
            auto answer = slot->scram->final_message(rest);
            if (!answer)
            {
                fail(slot, answer.error());
                return false;
            }
            pg::append_sasl_response(slot->tx, View(*answer));
            return flush(slot);
        }
        case 12:
            if (!slot->scram || !slot->scram->verify(rest))
            {
                fail(slot, "SCRAM: the server's signature does not match");
                return false;
            }
            slot->scram.reset();
            return true;
        default:
            break;
    }
    fail(slot, "the database asked for an authentication method that is not supported (only trust, password and "
               "SCRAM-SHA-256 are)");
    return false;
}

bool PgPool::on_startup_frame(PgSlot *slot, const pg::Frame &frame)
{
    switch (frame.type)
    {
        case 'R':
            return on_authentication(slot, frame.body);
        case 'E':
            fail(slot, pg::parse_error(frame.body).message);
            return false;
        case 'Z':
            send_prepares(slot);
            return slot->state != PgSlot::State::Dead;
        default:
            return true;
    }
}

bool PgPool::on_prepare_frame(PgSlot *slot, const pg::Frame &frame)
{
    switch (frame.type)
    {
        case '1':
            slot->prepared++;
            return true;
        case 'E':
            fail(slot, with_detail("preparing a statement failed", pg::parse_error(frame.body).message.c_str()));
            return false;
        case 'Z':
            if (slot->prepared != kStatementCount)
            {
                fail(slot, "the database did not prepare every statement");
                return false;
            }
            slot->state = PgSlot::State::Ready;
            watch(slot, kWatchIn);
            dispatch_pending();
            return slot->state != PgSlot::State::Dead;
        default:
            return true;
    }
}

bool PgPool::add_row(PgSlot *slot, const View body)
{
    const DbKind kind = slot->inflight[slot->done].op.kind;
    switch (kind)
    {
        case DbKind::Feed:
        case DbKind::Get: {
            Post post;
            if (!pg::decode_post(body, post))
                return false;
            if (slot->current.posts.empty())
                slot->current.posts.reserve(kind == DbKind::Feed ? 20 : 1);
            slot->current.posts.push_back(eastl::move(post));
            return true;
        }
        case DbKind::Create:
            slot->current_rows++;
            return pg::decode_created(body, slot->current.created);
        default:
            return true;
    }
}

bool PgPool::finish_command(PgSlot *slot, const View tag)
{
    const DbKind kind = slot->inflight[slot->done].op.kind;
    DbReply reply = eastl::move(slot->current);
    if (kind == DbKind::Like)
    {
        reply.like = pg::command_rows(tag) == 1 ? LikeOutcome::Created : LikeOutcome::AlreadyLiked;
    }
    else if (kind == DbKind::Create && slot->current_rows != 1)
    {
        reply = error_reply("INSERT returned no row");
    }
    slot->current = DbReply();
    slot->current_rows = 0;
    slot->replies.push_back(eastl::move(reply));
    slot->done++;
    return true;
}

void PgPool::finish_batch(PgSlot *slot)
{
    if (slot->batches.empty())
        return;
    const size_t n = slot->batches.front();
    slot->batches.pop_front();

    eastl::vector<PgSlot::Inflight> queries;
    queries.reserve(n);
    for (size_t i = 0; i < n; i++)
    {
        queries.push_back(eastl::move(slot->inflight.front()));
        slot->inflight.pop_front();
    }
    eastl::vector<DbReply> answers = eastl::move(slot->replies);
    slot->replies.clear();
    const bool errored = slot->has_error;
    const size_t error_at = errored ? slot->error_at : n;
    const pg::ErrorInfo error = eastl::move(slot->error);
    slot->error = pg::ErrorInfo();
    slot->has_error = false;
    slot->done = 0;
    slot->current = DbReply();
    slot->current_rows = 0;

    if (answers.size() < (errored ? error_at : n))
    {
        for (auto &q : queries)
            failed_.push_back({eastl::move(q.op), q.token, String("malformed answer from the database")});
        return fail(slot, "malformed answer from the database");
    }

    eastl::vector<DbKind> kinds;
    eastl::vector<pg::Fate> fates(n);
    kinds.reserve(n);
    for (const auto &q : queries)
        kinds.push_back(q.op.kind);
    pg::batch_fates(kinds.data(), n, error_at, errored ? &error : nullptr, fates.data());

    for (size_t i = n; i-- > 0;)
    {
        if (fates[i] == pg::Fate::Replay)
            pending_.push_front({eastl::move(queries[i].op), queries[i].token, true});
    }
    dispatch_pending();

    for (size_t i = 0; i < n; i++)
    {
        if (fates[i] == pg::Fate::Deliver)
        {
            sink_.on_db_reply(queries[i].token, queries[i].op, eastl::move(answers[i]));
        }
        else if (fates[i] == pg::Fate::Error)
        {
            sink_.on_db_reply(queries[i].token, queries[i].op, rejected(queries[i].op.kind, error));
        }
    }
}

bool PgPool::on_ready_frame(PgSlot *slot, const pg::Frame &frame)
{
    const bool in_batch = !slot->batches.empty() && slot->done < slot->batches.front();
    switch (frame.type)
    {
        case 'D':
            if (!in_batch || slot->has_error || !add_row(slot, frame.body))
                break;
            return true;
        case 'C':
            if (!in_batch || slot->has_error)
                break;
            return finish_command(slot, frame.body);
        case 'E': {
            pg::ErrorInfo info = pg::parse_error(frame.body);
            if (slot->batches.empty())
            {
                fail(slot, info.message.empty() ? String("the database reported an error") : info.message);
                return false;
            }
            if (!slot->has_error)
            {
                slot->has_error = true;
                slot->error_at = slot->done;
                slot->error = eastl::move(info);
            }
            slot->current = DbReply();
            slot->current_rows = 0;
            return true;
        }
        case 'Z':
            finish_batch(slot);
            return slot->state != PgSlot::State::Dead;
        default:
            return true;
    }
    fail(slot, "unexpected message from the database");
    return false;
}

bool PgPool::on_frame(PgSlot *slot, const pg::Frame &frame)
{
    switch (slot->state)
    {
        case PgSlot::State::Startup:
            return on_startup_frame(slot, frame);
        case PgSlot::State::Preparing:
            return on_prepare_frame(slot, frame);
        case PgSlot::State::Ready:
            return on_ready_frame(slot, frame);
        default:
            return false;
    }
}

bool PgPool::consume(PgSlot *slot, const View bytes)
{
    View data = bytes;
    const bool buffered = !slot->rx.empty();
    if (buffered)
    {
        slot->rx.append(bytes.data(), bytes.size());
        data = View(slot->rx);
    }
    size_t off = 0;
    for (;;)
    {
        pg::Frame frame;
        const pg::FrameStatus status = pg::next_frame(data, off, frame);
        if (status == pg::FrameStatus::Incomplete)
            break;
        if (status == pg::FrameStatus::Bad)
        {
            fail(slot, "malformed message from the database");
            return false;
        }
        if (!on_frame(slot, frame))
            return false;
    }
    if (off == data.size())
    {
        if (buffered)
            slot->rx.clear();
    }
    else if (off > 0 || !buffered)
    {
        String tail(data.substr(off));
        slot->rx = eastl::move(tail);
    }
    return true;
}

void PgPool::on_readable(PgSlot *slot)
{
    for (;;)
    {
        const ssize_t n = recv(slot->sock, scratch_.get(), kScratchBytes, 0);
        if (n > 0)
        {
            if (!consume(slot, View(scratch_.get(), static_cast<size_t>(n))))
                return;
            if (static_cast<size_t>(n) < kScratchBytes)
                return;
        }
        else if (n == 0)
        {
            return fail(slot, "the database closed the connection");
        }
        else if (errno == EINTR)
        {
        }
        else if (errno == EAGAIN)
        {
            return;
        }
        else
        {
            return fail(slot, errno_text("receiving from the database", errno));
        }
    }
}

void PgPool::close_batch(PgSlot *slot)
{
    if (slot->open == 0)
        return;
    pg::append_sync(slot->tx);
    slot->batches.push_back(slot->open);
    slot->open = 0;
}

void PgPool::start_query(PgSlot *slot, DbOp op, void *token, const bool alone)
{
    char a[8], b[8];
    pg::Value values[2] = {};
    size_t count = 0;
    switch (op.kind)
    {
        case DbKind::Get:
            values[count++] = int8_value(a, op.id);
            break;
        case DbKind::Create:
            if (std::memchr(op.body.data(), '\0', op.body.size()))
            {
                failed_.push_back({eastl::move(op), token, String("body contains a NUL character")});
                return;
            }
            values[count++] = int8_value(a, op.user_id);
            values[count++] = {op.body.data(), static_cast<int32_t>(op.body.size())};
            break;
        case DbKind::Like:
            values[count++] = int8_value(a, op.user_id);
            values[count++] = int8_value(b, op.id);
            break;
        default:
            break;
    }

    if (alone)
        close_batch(slot);
    pg::append_bind(slot->tx, statement_name(op.kind), values, count);
    pg::append_execute(slot->tx);
    slot->inflight.push_back({eastl::move(op), token});
    slot->open++;
    if (alone)
        close_batch(slot);
    if (!slot->dirty)
    {
        slot->dirty = true;
        dirty_.push_back(slot);
    }
}

void PgPool::dispatch_pending()
{
    while (!pending_.empty())
    {
        PgSlot *slot = best_slot();
        if (!slot)
            return;
        Pending p = eastl::move(pending_.front());
        pending_.pop_front();
        start_query(slot, eastl::move(p.op), p.token, p.alone);
    }
}

void PgPool::submit(DbOp op, void *token)
{
    if (PgSlot *slot = best_slot())
        return start_query(slot, eastl::move(op), token, false);
    if (any_alive())
    {
        pending_.push_back({eastl::move(op), token, false});
    }
    else
    {
        failed_.push_back({eastl::move(op), token, last_error_.empty() ? String("database unavailable") : last_error_});
    }
}

void PgPool::on_event(Tagged *tagged, const uint32_t events)
{
    auto *slot = static_cast<PgSlot *>(tagged); // NOLINT(cppcoreguidelines-pro-type-static-cast-downcast)
    switch (slot->state)
    {
        case PgSlot::State::Dead:
            return;
        case PgSlot::State::Connecting:
            return finish_connect(slot);
        default:
            break;
    }
    if ((events & kWatchOut) && slot->tx_off < slot->tx.size() && !flush(slot))
        return;
    if (events & ~kWatchOut)
        on_readable(slot);
}

void PgPool::poll()
{
    while (!failed_.empty())
    {
        eastl::vector<Failed> local;
        local.swap(failed_);
        for (Failed &f : local)
        {
            sink_.on_db_reply(f.token, f.op, error_reply(eastl::move(f.error)));
        }
    }
    const auto now = Clock::now();
    for (PgSlot *slot : slots_)
    {
        if (slot->state == PgSlot::State::Dead && slot->retry_at <= now)
            start_connect(slot);
    }
    while (!dirty_.empty())
    {
        eastl::vector<PgSlot *> local;
        local.swap(dirty_);
        for (PgSlot *slot : local)
        {
            if (!slot->dirty)
                continue;
            slot->dirty = false;
            if (slot->state != PgSlot::State::Ready)
                continue;
            close_batch(slot);
            flush(slot);
        }
    }
}

int PgPool::timeout_ms() const
{
    if (!failed_.empty() || !dirty_.empty())
        return 0;
    int timeout = -1;
    const auto now = Clock::now();
    for (const PgSlot *slot : slots_)
    {
        if (slot->state != PgSlot::State::Dead)
            continue;
        const auto wait = std::chrono::duration_cast<std::chrono::milliseconds>(slot->retry_at - now).count();
        const int ms = wait < 0 ? 0 : static_cast<int>(wait);
        if (timeout < 0 || ms < timeout)
            timeout = ms;
    }
    return timeout;
}
}
