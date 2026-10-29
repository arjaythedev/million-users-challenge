#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "api.hpp"
#include "test_util.hpp"

namespace feed::test
{
namespace
{

using ::testing::_;
using ::testing::Return;
using ::testing::StrictMock;

MATCHER_P(ViewIs, expected, "")
{
    return arg == View(expected);
}

class MockStore : public Store
{
  public:
    MOCK_METHOD((std::expected<eastl::vector<Post>, DbError>), feed, (), (override));
    MOCK_METHOD((std::expected<std::optional<Post>, DbError>), get, (int64_t), (override));
    MOCK_METHOD((std::expected<NewPost, DbError>), create, (int64_t, View), (override));
    MOCK_METHOD((std::expected<LikeOutcome, DbError>), like, (int64_t, int64_t), (override));
    MOCK_METHOD((std::expected<void, DbError>), ping, (), (override));
};

const Post kPost{5, "hello", "2025-12-31 23:59:19.409123+00", "bob", 3};
constexpr const char *kPostJson =
    R"({"id":5,"body":"hello","created_at":"2025-12-31T23:59:19.409Z","author":"bob","like_count":3})";

struct Request
{
    Str method, path, authorization, body;

    [[nodiscard]] HttpRequest build() const
    {
        HttpRequest r{};
        r.method = view(method);
        r.path = view(path);
        r.authorization = view(authorization);
        r.body = view(body);
        r.keep_alive = true;
        return r;
    }
};

class Api : public ::testing::Test
{
  protected:
    StrictMock<MockStore> store;
    int64_t now = kNow;

    ApiResponse call(const Request &req)
    {
        const HttpRequest http = req.build();
        return handle(ApiContext{store, View(kSecret), now, 12}, http);
    }
    ApiResponse get(const char *path)
    {
        return call({"GET", path, "", ""});
    }
    ApiResponse post(const char *path, const Str &authorization, const Str &body = "")
    {
        return call({"POST", path, authorization, body});
    }

    static Str token(const Str &sub = "1", const Str &username = "bob")
    {
        return bearer(make_jwt(payload(sub, username)));
    }

    static void expect(const ApiResponse &r, int status, const Str &body)
    {
        EXPECT_EQ(r.status, status);
        EXPECT_EQ(r.body, body);
    }
};

TEST_F(Api, HealthOk)
{
    EXPECT_CALL(store, ping()).WillOnce(Return(std::expected<void, DbError>()));
    expect(get("/health"), 200, R"({"status":"ok","db":"ok","uptime_s":12})");
}

TEST_F(Api, HealthWhenTheDatabaseIsUnreachable)
{
    EXPECT_CALL(store, ping()).WillOnce(Return(std::unexpected(DbError{"connection refused"})));
    expect(get("/health"), 503, R"({"status":"degraded","db":"unreachable","error":"connection refused"})");
}

TEST_F(Api, HealthEscapesTheDatabaseErrorText)
{
    EXPECT_CALL(store, ping()).WillOnce(Return(std::unexpected(DbError{"bad \"thing\"\n"})));
    expect(get("/health"), 503, R"({"status":"degraded","db":"unreachable","error":"bad \"thing\"\n"})");
}

TEST_F(Api, FeedReturnsThePostsInTheOrderTheDatabaseGaveThem)
{
    eastl::vector<Post> posts{kPost, Post{4, "older", "2025-12-31 23:59:18.1+00", "amy", 0}};
    EXPECT_CALL(store, feed()).WillOnce(Return(std::expected<eastl::vector<Post>, DbError>(posts)));
    expect(get("/feed"), 200,
           cat(R"({"posts":[)", kPostJson,
               R"(,{"id":4,"body":"older","created_at":"2025-12-31T23:59:18.100Z","author":"amy","like_count":0}]})"));
}

TEST_F(Api, EmptyFeed)
{
    EXPECT_CALL(store, feed()).WillOnce(Return(std::expected<eastl::vector<Post>, DbError>(eastl::vector<Post>{})));
    expect(get("/feed"), 200, R"({"posts":[]})");
}

TEST_F(Api, FeedDatabaseFailureIs500)
{
    EXPECT_CALL(store, feed()).WillOnce(Return(std::unexpected(DbError{"boom"})));
    expect(get("/feed"), 500, R"({"error":"internal server error"})");
}

TEST_F(Api, EveryFeedRequestReadsTheDatabase)
{
    EXPECT_CALL(store, feed())
        .Times(3)
        .WillRepeatedly(Return(std::expected<eastl::vector<Post>, DbError>(eastl::vector<Post>{kPost})));
    for (int i = 0; i < 3; i++)
        EXPECT_EQ(get("/feed").status, 200);
}

TEST_F(Api, GetPost)
{
    EXPECT_CALL(store, get(5)).WillOnce(Return(std::expected<std::optional<Post>, DbError>(kPost)));
    expect(get("/posts/5"), 200, cat(R"({"post":)", kPostJson, '}'));
}

TEST_F(Api, GetMissingPost)
{
    EXPECT_CALL(store, get(999999999)).WillOnce(Return(std::expected<std::optional<Post>, DbError>(std::nullopt)));
    expect(get("/posts/999999999"), 404, R"({"error":"post not found"})");
}

TEST_F(Api, GetPostWithAnInvalidIdNeverReachesTheDatabase)
{
    for (const char *id : {"0", "-1", "abc", "1.5", "1e3", "0x1"})
    {
        expect(get(cat("/posts/", id).c_str()), 400, R"({"error":"invalid post id"})");
    }
}

TEST_F(Api, GetPostDatabaseFailureIs500)
{
    EXPECT_CALL(store, get(5)).WillOnce(Return(std::unexpected(DbError{"boom"})));
    expect(get("/posts/5"), 500, R"({"error":"internal server error"})");
}

TEST_F(Api, EveryGetPostReadsTheDatabaseAgain)
{
    EXPECT_CALL(store, get(5)).Times(2).WillRepeatedly(Return(std::expected<std::optional<Post>, DbError>(kPost)));
    EXPECT_EQ(get("/posts/5").status, 200);
    EXPECT_EQ(get("/posts/5").status, 200);
}

TEST_F(Api, CreatePost)
{
    EXPECT_CALL(store, create(1, ViewIs("hello")))
        .WillOnce(Return(std::expected<NewPost, DbError>(NewPost{77, "2025-12-31 23:59:19.409123+00"})));
    expect(
        post("/posts", token("1", "bob"), R"({"body":"  hello  "})"), 201,
        R"({"post":{"id":77,"body":"hello","created_at":"2025-12-31T23:59:19.409Z","author":"bob","like_count":0}})");
}

TEST_F(Api, CreatePostAuthorComesFromTheTokenNotTheDatabase)
{
    EXPECT_CALL(store, create(20000, _))
        .WillOnce(Return(std::expected<NewPost, DbError>(NewPost{1, "2025-01-01 00:00:00+00"})));
    const auto r = post("/posts", token("20000", "golden_ember_1"), R"({"body":"x"})");
    EXPECT_EQ(r.status, 201);
    EXPECT_NE(r.body.find(R"("author":"golden_ember_1")"), Str::npos);
}

TEST_F(Api, CreatePostWithSpecialCharactersRoundTrips)
{
    EXPECT_CALL(store, create(1, ViewIs("a \"q\" <b>&amp;</b> / caf\xc3\xa9 \xe2\x9c\x93")))
        .WillOnce(Return(std::expected<NewPost, DbError>(NewPost{2, "2025-01-01 00:00:00+00"})));
    const auto r = post("/posts", token(), "{\"body\":\"  a \\\"q\\\" <b>&amp;</b> / caf\xc3\xa9 \xe2\x9c\x93  \"}");
    EXPECT_EQ(r.status, 201);
    EXPECT_NE(r.body.find(R"("body":"a \"q\" <b>&amp;</b> / caf)"), Str::npos);
}

TEST_F(Api, CreatePostAuthIsCheckedBeforeTheBody)
{
    expect(post("/posts", "", "{bad json"), 401, R"({"error":"missing bearer token"})");
    expect(post("/posts", "Basic abc", R"({"body":"x"})"), 401, R"({"error":"missing bearer token"})");
    expect(post("/posts", "Bearer abc", R"({"body":"x"})"), 401, R"({"error":"invalid or expired token"})");
    expect(post("/posts", bearer(make_jwt(payload("abc", "bob"))), R"({"body":"x"})"), 401,
           R"({"error":"invalid token payload"})");
}

TEST_F(Api, CreatePostValidationErrorsNeverReachTheDatabase)
{
    expect(post("/posts", token(), R"({"body":""})"), 400, R"({"error":"body is required"})");
    expect(post("/posts", token(), R"({"body":"   "})"), 400, R"({"error":"body is required"})");
    expect(post("/posts", token(), R"({"body":123})"), 400, R"({"error":"body is required"})");
    expect(post("/posts", token(), "{}"), 400, R"({"error":"body is required"})");
    expect(post("/posts", token(), "{bad json"), 400, R"({"error":"malformed JSON body"})");
    expect(post("/posts", token(), ""), 400, R"({"error":"malformed JSON body"})");
    expect(post("/posts", token(), cat(R"({"body":")", repeat("a", 501), R"("})")), 400,
           R"({"error":"body must be at most 500 characters"})");
}

TEST_F(Api, CreatePostAtTheLimitReachesTheDatabase)
{
    EXPECT_CALL(store, create(1, _))
        .WillOnce(Return(std::expected<NewPost, DbError>(NewPost{3, "2025-01-01 00:00:00+00"})));
    EXPECT_EQ(post("/posts", token(), cat(R"({"body":")", repeat("b", 500), R"("})")).status, 201);
}

TEST_F(Api, CreatePostDatabaseFailureIs500)
{
    EXPECT_CALL(store, create(_, _)).WillOnce(Return(std::unexpected(DbError{"boom"})));
    expect(post("/posts", token(), R"({"body":"x"})"), 500, R"({"error":"internal server error"})");
}

TEST_F(Api, FirstLike)
{
    EXPECT_CALL(store, like(1, 5)).WillOnce(Return(std::expected<LikeOutcome, DbError>(LikeOutcome::Created)));
    expect(post("/posts/5/like", token()), 201, R"({"liked":true,"already_liked":false,"post_id":5})");
}

TEST_F(Api, RepeatedLike)
{
    EXPECT_CALL(store, like(1, 5)).WillOnce(Return(std::expected<LikeOutcome, DbError>(LikeOutcome::AlreadyLiked)));
    expect(post("/posts/5/like", token()), 200, R"({"liked":true,"already_liked":true,"post_id":5})");
}

TEST_F(Api, LikeOnAPostThatDoesNotExist)
{
    EXPECT_CALL(store, like(1, 999999999))
        .WillOnce(Return(std::expected<LikeOutcome, DbError>(LikeOutcome::PostMissing)));
    expect(post("/posts/999999999/like", token()), 404, R"({"error":"post not found"})");
}

TEST_F(Api, LikeChecksAuthBeforeTheId)
{
    expect(post("/posts/abc/like", ""), 401, R"({"error":"missing bearer token"})");
    expect(post("/posts/abc/like", "Bearer abc"), 401, R"({"error":"invalid or expired token"})");
    expect(post("/posts/abc/like", token()), 400, R"({"error":"invalid post id"})");
    expect(post("/posts/0/like", token()), 400, R"({"error":"invalid post id"})");
}

TEST_F(Api, LikeUsesTheUserIdFromTheToken)
{
    EXPECT_CALL(store, like(20000, 5)).WillOnce(Return(std::expected<LikeOutcome, DbError>(LikeOutcome::Created)));
    EXPECT_EQ(post("/posts/5/like", token("20000", "u")).status, 201);
}

TEST_F(Api, LikeDatabaseFailureIs500)
{
    EXPECT_CALL(store, like(_, _)).WillOnce(Return(std::unexpected(DbError{"boom"})));
    expect(post("/posts/5/like", token()), 500, R"({"error":"internal server error"})");
}

TEST_F(Api, AnExpiredTokenIsNotRememberedAsValid)
{
    const Str t = bearer(make_jwt(payload("1", "bob", num(kNow + 10))));
    EXPECT_CALL(store, like(1, 5)).WillOnce(Return(std::expected<LikeOutcome, DbError>(LikeOutcome::Created)));
    EXPECT_EQ(post("/posts/5/like", t).status, 201);
    now = kNow + 3600;
    expect(post("/posts/5/like", t), 401, R"({"error":"invalid or expired token"})");
}

TEST_F(Api, ATokenWithoutExpIsRefusedOnEveryRoute)
{
    const Str t = bearer(make_jwt(R"({"sub":"1","username":"bob"})"));
    EXPECT_EQ(post("/posts", t, R"({"body":"x"})").status, 401);
    EXPECT_EQ(post("/posts/5/like", t).status, 401);
}

TEST_F(Api, UnknownPaths)
{
    expect(get("/nope"), 404, R"({"error":"not found"})");
    expect(get("/"), 404, R"({"error":"not found"})");
    expect(get("/posts/"), 404, R"({"error":"not found"})");
    expect(post("/feed", token()), 404, R"({"error":"not found"})");
}

} // namespace
} // namespace feed::test
