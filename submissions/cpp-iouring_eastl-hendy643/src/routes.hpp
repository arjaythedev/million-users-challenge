#pragma once

#include <optional>

#include "types.hpp"

namespace feed
{
enum class RouteKind
{
    Health,
    Feed,
    GetPost,
    CreatePost,
    LikePost,
    NotFound
};

struct Route
{
    RouteKind kind = RouteKind::NotFound;
    View id;
};

Route match_route(View method_v, View path_v);

std::optional<int64_t> parse_post_id(View text);
}
