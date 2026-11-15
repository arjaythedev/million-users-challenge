#pragma once

#include <EASTL/vector.h>

#include <cstdint>
#include <expected>
#include <optional>

#include "http.hpp"
#include "response.hpp"

namespace feed
{

struct DbError
{
    String message;
};

struct NewPost
{
    int64_t id = 0;
    String created_at;
};

enum class LikeOutcome
{
    Created,
    AlreadyLiked,
    PostMissing
};

class Store
{
  public:
    virtual ~Store() = default;
    virtual std::expected<eastl::vector<Post>, DbError> feed() = 0;
    virtual std::expected<std::optional<Post>, DbError> get(int64_t id) = 0;
    virtual std::expected<NewPost, DbError> create(int64_t user_id, View body) = 0;
    virtual std::expected<LikeOutcome, DbError> like(int64_t user_id, int64_t post_id) = 0;
    virtual std::expected<void, DbError> ping() = 0;
};

struct ApiContext
{
    Store &store;
    View jwt_secret;
    int64_t now_unix;
    int64_t uptime_s;
};

struct ApiResponse
{
    int status = 200;
    String body;
};

enum class DbKind
{
    Ping,
    Feed,
    Get,
    Create,
    Like
};

struct DbOp
{
    DbKind kind = DbKind::Ping;
    int64_t user_id = 0;
    int64_t id = 0;
    String body;
    String username;
};

struct DbReply
{
    bool ok = true;
    String error;
    eastl::vector<Post> posts;
    NewPost created;
    LikeOutcome like = LikeOutcome::Created;
};

struct Plan
{
    bool done = false;
    ApiResponse response;
    DbOp op;
};

Plan plan_request(View jwt_secret, int64_t now_unix, const HttpRequest &req);

ApiResponse finish_request(const DbOp &op, DbReply &&reply, int64_t uptime_s);

DbReply run_on_store(Store &store, const DbOp &op);

ApiResponse handle(const ApiContext &ctx, const HttpRequest &req);

}
