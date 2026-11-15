#include "routes.hpp"

#include <string_view>

namespace feed
{
Route match_route(const View method_v, const View path_v)
{
    const std::string_view method(method_v.data(), method_v.size());
    const std::string_view path(path_v.data(), path_v.size());
    const bool get = method == "GET";
    const bool post = method == "POST";

    if (get && path == "/health")
        return {.kind = RouteKind::Health, .id = {}};
    if (get && path == "/feed")
        return {.kind = RouteKind::Feed, .id = {}};
    if (post && path == "/posts")
        return {.kind = RouteKind::CreatePost, .id = {}};

    constexpr std::string_view prefix = "/posts/";
    if (path.substr(0, prefix.size()) != prefix)
        return {};
    const std::string_view rest = path.substr(prefix.size());
    const size_t slash = rest.find('/');
    const std::string_view id = rest.substr(0, slash);
    if (id.empty())
        return {};
    const View id_view(id.data(), id.size());
    if (slash == std::string_view::npos)
        return get ? Route{.kind = RouteKind::GetPost, .id = id_view} : Route{};
    if (post && rest.substr(slash) == "/like")
        return {.kind = RouteKind::LikePost, .id = id_view};
    return {};
}

std::optional<int64_t> parse_post_id(const View text)
{
    if (text.empty() || text.size() > 18)
        return std::nullopt;
    int64_t id = 0;
    for (const char c : text)
    {
        if (c < '0' || c > '9')
            return std::nullopt;
        id = id * 10 + (c - '0');
    }
    if (id <= 0)
        return std::nullopt;
    return id;
}
}
