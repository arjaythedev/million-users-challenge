#include "api.hpp"

#include "json.hpp"
#include "jwt.hpp"
#include "routes.hpp"

namespace feed
{
namespace
{
void put(String &out, const View s)
{
    out.append(s.data(), s.size());
}

ApiResponse error(const int status, const View message)
{
    return {.status = status, .body = error_body(message)};
}

ApiResponse internal_error()
{
    return error(500, "internal server error");
}

ApiResponse auth_error(const AuthStatus s)
{
    switch (s)
    {
        case AuthStatus::InvalidToken:
            return error(401, "invalid or expired token");
        case AuthStatus::InvalidPayload:
            return error(401, "invalid token payload");
        default:
            return error(401, "missing bearer token");
    }
}

Plan done(ApiResponse response)
{
    Plan p;
    p.done = true;
    p.response = eastl::move(response);
    return p;
}

Plan needs_db(DbOp op)
{
    Plan p;
    p.op = eastl::move(op);
    return p;
}

Plan plan_create(const View secret, const int64_t now, const HttpRequest &req)
{
    const AuthResult auth = authenticate(req.authorization, secret, now);
    if (auth.status != AuthStatus::Ok)
        return done(auth_error(auth.status));

    auto body = parse_create_post_body(req.body);
    if (!body)
    {
        switch (body.error())
        {
            case BodyError::MalformedJson:
                return done(error(400, "malformed JSON body"));
            case BodyError::BodyRequired:
                return done(error(400, "body is required"));
            case BodyError::BodyTooLong:
                return done(error(400, "body must be at most 500 characters"));
        }
    }
    DbOp op;
    op.kind = DbKind::Create;
    op.user_id = auth.claims.user_id;
    op.body = eastl::move(*body);
    op.username = auth.claims.username;
    return needs_db(eastl::move(op));
}

Plan plan_like(const View secret, const int64_t now, const HttpRequest &req, const View id_text)
{
    const AuthResult auth = authenticate(req.authorization, secret, now);
    if (auth.status != AuthStatus::Ok)
        return done(auth_error(auth.status));
    const auto id = parse_post_id(id_text);
    if (!id)
        return done(error(400, "invalid post id"));

    DbOp op;
    op.kind = DbKind::Like;
    op.user_id = auth.claims.user_id;
    op.id = *id;
    return needs_db(eastl::move(op));
}

ApiResponse post_response(const int status, const Post &post)
{
    ApiResponse r;
    r.status = status;
    r.body = "{\"post\":";
    append_post(r.body, post);
    r.body.push_back('}');
    return r;
}
}

Plan plan_request(const View secret, const int64_t now, const HttpRequest &req)
{
    const Route route = match_route(req.method, req.path);
    DbOp op;
    switch (route.kind)
    {
        case RouteKind::Health:
            op.kind = DbKind::Ping;
            return needs_db(eastl::move(op));
        case RouteKind::Feed:
            op.kind = DbKind::Feed;
            return needs_db(eastl::move(op));
        case RouteKind::GetPost: {
            const auto id = parse_post_id(route.id);
            if (!id)
                return done(error(400, "invalid post id"));
            op.kind = DbKind::Get;
            op.id = *id;
            return needs_db(eastl::move(op));
        }
        case RouteKind::CreatePost:
            return plan_create(secret, now, req);
        case RouteKind::LikePost:
            return plan_like(secret, now, req, route.id);
        case RouteKind::NotFound:
            break;
    }
    return done(error(404, "not found"));
}

ApiResponse finish_request(const DbOp &op, DbReply &&reply, int64_t uptime_s)
{
    if (!reply.ok)
    {
        if (op.kind != DbKind::Ping)
            return internal_error();
        ApiResponse r;
        r.status = 503;
        r.body = R"({"status":"degraded","db":"unreachable","error":)";
        append_json_string(r.body, View(reply.error));
        r.body.push_back('}');
        return r;
    }

    ApiResponse r;
    switch (op.kind)
    {
        case DbKind::Ping:
            r.body = R"({"status":"ok","db":"ok","uptime_s":)";
            append_number(r.body, uptime_s);
            r.body.push_back('}');
            return r;
        case DbKind::Feed: {
            size_t estimate = 16;
            for (const Post &p : reply.posts)
                estimate += p.body.size() + p.author.size() + 110;
            r.body.reserve(estimate);
            put(r.body, "{\"posts\":[");
            bool first = true;
            for (const Post &p : reply.posts)
            {
                if (!first)
                    r.body.push_back(',');
                first = false;
                append_post(r.body, p);
            }
            put(r.body, "]}");
            return r;
        }
        case DbKind::Get:
            if (reply.posts.empty())
                return error(404, "post not found");
            return post_response(200, reply.posts[0]);
        case DbKind::Create: {
            Post post;
            post.id = reply.created.id;
            post.body = op.body;
            post.created_at = reply.created.created_at;
            post.author = op.username;
            return post_response(201, post);
        }
        case DbKind::Like: {
            if (reply.like == LikeOutcome::PostMissing)
                return error(404, "post not found");
            const bool already = reply.like == LikeOutcome::AlreadyLiked;
            r.status = already ? 200 : 201;
            r.body = already ? R"({"liked":true,"already_liked":true,"post_id":)"
                             : R"({"liked":true,"already_liked":false,"post_id":)";
            append_number(r.body, op.id);
            r.body.push_back('}');
            return r;
        }
    }
    return internal_error();
}

DbReply run_on_store(Store &store, const DbOp &op)
{
    DbReply reply;
    auto fail = [&reply](DbError &&e) {
        reply.ok = false;
        reply.error = eastl::move(e.message);
    };
    switch (op.kind)
    {
        case DbKind::Ping:
            if (auto r = store.ping(); !r)
                fail(eastl::move(r.error()));
            break;
        case DbKind::Feed:
            if (auto r = store.feed())
                reply.posts = eastl::move(*r);
            else
                fail(eastl::move(r.error()));
            break;
        case DbKind::Get:
            if (auto r = store.get(op.id))
            {
                if (*r)
                    reply.posts.push_back(eastl::move(**r));
            }
            else
            {
                fail(eastl::move(r.error()));
            }
            break;
        case DbKind::Create:
            if (auto r = store.create(op.user_id, View(op.body)))
                reply.created = eastl::move(*r);
            else
                fail(eastl::move(r.error()));
            break;
        case DbKind::Like:
            if (auto r = store.like(op.user_id, op.id))
                reply.like = *r;
            else
                fail(eastl::move(r.error()));
            break;
    }
    return reply;
}

ApiResponse handle(const ApiContext &ctx, const HttpRequest &req)
{
    Plan plan = plan_request(ctx.jwt_secret, ctx.now_unix, req);
    if (plan.done)
        return eastl::move(plan.response);
    DbReply reply = run_on_store(ctx.store, plan.op);
    return finish_request(plan.op, eastl::move(reply), ctx.uptime_s);
}
}
