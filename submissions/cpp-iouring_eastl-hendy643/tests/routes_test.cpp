
#include <gtest/gtest.h>

#include "routes.hpp"
#include "test_util.hpp"

namespace feed::test
{
namespace
{

Route route(const char *method, const char *path)
{
    return match_route(View(method), View(path));
}

TEST(Routes, TheFiveEndpoints)
{
    EXPECT_EQ(route("GET", "/health").kind, RouteKind::Health);
    EXPECT_EQ(route("GET", "/feed").kind, RouteKind::Feed);
    EXPECT_EQ(route("POST", "/posts").kind, RouteKind::CreatePost);

    const auto get = route("GET", "/posts/42");
    EXPECT_EQ(get.kind, RouteKind::GetPost);
    EXPECT_EQ(get.id, "42");

    const auto like = route("POST", "/posts/42/like");
    EXPECT_EQ(like.kind, RouteKind::LikePost);
    EXPECT_EQ(like.id, "42");
}

TEST(Routes, BadIdsStillRouteSoAuthCanComeFirst)
{
    for (const char *id : {"abc", "0", "-1", "1.5", "99999999999999999999"})
    {
        EXPECT_EQ(match_route(View("GET"), view(cat("/posts/", id))).kind, RouteKind::GetPost) << id;
        EXPECT_EQ(match_route(View("POST"), view(cat("/posts/", id, "/like"))).kind, RouteKind::LikePost) << id;
    }
}

TEST(Routes, UnknownPathsAreNotFound)
{
    for (const char *path : {"/", "/nope", "/healthz", "/feeds", "/posts/", "/posts/1/likes", "/posts/1/like/x",
                             "/posts/1/2", "/post/1", "//feed"})
    {
        EXPECT_EQ(route("GET", path).kind, RouteKind::NotFound) << path;
        EXPECT_EQ(route("POST", path).kind, RouteKind::NotFound) << path;
    }
}

TEST(Routes, KnownPathsWithTheWrongMethodAreNotFound)
{
    EXPECT_EQ(route("POST", "/feed").kind, RouteKind::NotFound);
    EXPECT_EQ(route("POST", "/health").kind, RouteKind::NotFound);
    EXPECT_EQ(route("GET", "/posts").kind, RouteKind::NotFound);
    EXPECT_EQ(route("GET", "/posts/1/like").kind, RouteKind::NotFound);
    EXPECT_EQ(route("POST", "/posts/1").kind, RouteKind::NotFound);
    EXPECT_EQ(route("DELETE", "/posts/1").kind, RouteKind::NotFound);
}

TEST(Routes, PathsAreCaseSensitive)
{
    EXPECT_EQ(route("GET", "/Feed").kind, RouteKind::NotFound);
    EXPECT_EQ(route("GET", "/HEALTH").kind, RouteKind::NotFound);
}

TEST(PostId, PositiveIntegers)
{
    EXPECT_EQ(parse_post_id("1"), 1);
    EXPECT_EQ(parse_post_id("42"), 42);
    EXPECT_EQ(parse_post_id("500000"), 500000);
    EXPECT_EQ(parse_post_id("9007199254740991"), 9007199254740991);
}

TEST(PostId, EverythingElseIsInvalid)
{
    for (const char *id : {"0", "-1", "-0", "abc", "1.5", "1e3", "0x1f", "+1", " 1", "1 ", "", "1a", "a1", "１２"})
    {
        EXPECT_FALSE(parse_post_id(id).has_value()) << "[" << id << "]";
    }
}

TEST(PostId, HugeValuesNeverOverflowIntoValidIds)
{
    const auto id = parse_post_id("18446744073709551617");
    EXPECT_FALSE(id.has_value() && *id == 1);
}

}
}
