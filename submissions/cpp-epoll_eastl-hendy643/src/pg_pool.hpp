#pragma once

#include <EASTL/deque.h>
#include <EASTL/vector.h>

#include <chrono>
#include <cstdint>
#include <expected>
#include <memory>

#include "api.hpp"
#include "pg_wire.hpp"
#include "tag.hpp"
#include "tuning.hpp"
#include "watcher.hpp"

namespace feed
{
class DbSink
{
  public:
    virtual ~DbSink() = default;

    virtual void on_db_reply(void *token, const DbOp &op, DbReply &&reply) = 0;
};

struct PgSlot;

// The queries one loop iteration sends to a connection share a Sync, so they commit together, and none is answered
// before the Sync's ReadyForQuery (after the commit). If the server rejects one, the batch is rolled back and the
// others are run again on their own (pg::batch_fates).
class PgPool
{
  public:
    PgPool(Watcher &watcher, DbSink &sink, const String &url, size_t connections);

    ~PgPool();

    PgPool(const PgPool &) = delete;

    PgPool &operator=(const PgPool &) = delete;

    void submit(DbOp op, void *token);

    void on_event(Tagged *tagged, uint32_t events);

    void poll();

    [[nodiscard]] int timeout_ms() const;

  private:
    using Clock = std::chrono::steady_clock;

    struct Pending
    {
        DbOp op;
        void *token;
        bool alone;
    };

    struct Failed
    {
        DbOp op;
        void *token;
        String error;
    };

    Watcher &watcher_;
    DbSink &sink_;
    std::expected<pg::Target, String> target_;
    String last_error_;
    eastl::vector<PgSlot *> slots_;
    eastl::vector<PgSlot *> dirty_;
    eastl::deque<Pending> pending_;
    eastl::vector<Failed> failed_;
    std::unique_ptr<char[]> scratch_;

    [[nodiscard]] bool any_alive() const;

    void start_connect(PgSlot *slot);

    void fail(PgSlot *slot, const String &message);

    void fail_all_pending(const String &message);

    void watch(PgSlot *slot, uint32_t events);

    void unwatch(PgSlot *slot) const;

    bool flush(PgSlot *slot);

    void finish_connect(PgSlot *slot);

    void on_readable(PgSlot *slot);

    bool consume(PgSlot *slot, View bytes);

    bool on_frame(PgSlot *slot, const pg::Frame &frame);

    bool on_startup_frame(PgSlot *slot, const pg::Frame &frame);

    bool on_authentication(PgSlot *slot, View body);

    bool on_prepare_frame(PgSlot *slot, const pg::Frame &frame);

    bool on_ready_frame(PgSlot *slot, const pg::Frame &frame);

    static bool add_row(PgSlot *slot, View body);

    static bool finish_command(PgSlot *slot, View tag);

    void finish_batch(PgSlot *slot);

    void send_prepares(PgSlot *slot);

    [[nodiscard]] PgSlot *best_slot() const;

    void start_query(PgSlot *slot, DbOp op, void *token, bool alone);

    static void close_batch(PgSlot *slot);

    void dispatch_pending();
};
} // namespace feed
