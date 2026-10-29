
#include <gtest/gtest.h>

#include "http.hpp"
#include "test_util.hpp"

namespace feed::test
{
namespace
{

ParseResult parse(const Str &raw)
{
    return parse_http_request(view(raw));
}

Str get(const char *path, const char *extra_headers = "")
{
    return cat("GET ", path, " HTTP/1.1\r\nHost: x\r\n", extra_headers, "\r\n");
}

Str post(const char *path, const Str &body, const char *extra_headers = "")
{
    return cat("POST ", path, " HTTP/1.1\r\nHost: x\r\n", extra_headers,
               "Content-Type: application/json\r\nContent-Length: ", num(int64_t(body.size())), "\r\n\r\n", body);
}

TEST(Http, SimpleGet)
{
    const Str raw = get("/feed");
    const auto r = parse(raw);
    ASSERT_EQ(r.status, ParseStatus::Complete);
    EXPECT_EQ(r.consumed, raw.size());
    EXPECT_EQ(r.request.method, "GET");
    EXPECT_EQ(r.request.path, "/feed");
    EXPECT_EQ(r.request.body.size(), 0u);
    EXPECT_TRUE(r.request.keep_alive);
    EXPECT_FALSE(r.request.accepts_gzip);
}

TEST(Http, QueryStringIsNotPartOfThePath)
{
    const Str raw = get("/posts/5?utm=x&y=1");
    const auto r = parse(raw);
    ASSERT_EQ(r.status, ParseStatus::Complete);
    EXPECT_EQ(r.request.path, "/posts/5");
}

TEST(Http, PostWithBody)
{
    const Str body = R"({"body":"hello"})";
    const Str raw = post("/posts", body, "Authorization: Bearer abc.def.ghi\r\n");
    const auto r = parse(raw);
    ASSERT_EQ(r.status, ParseStatus::Complete);
    EXPECT_EQ(r.consumed, raw.size());
    EXPECT_EQ(r.request.method, "POST");
    EXPECT_EQ(r.request.path, "/posts");
    EXPECT_EQ(r.request.authorization, "Bearer abc.def.ghi");
    EXPECT_EQ(r.request.body, body);
}

TEST(Http, HeaderNamesAreCaseInsensitive)
{
    const Str raw = cat("POST /posts HTTP/1.1\r\nhost: x\r\nAUTHORIZATION: Bearer t\r\ncontent-length: "
                        "2\r\naccept-ENCODING: gzip\r\n\r\n{}");
    const auto r = parse(raw);
    ASSERT_EQ(r.status, ParseStatus::Complete);
    EXPECT_EQ(r.request.authorization, "Bearer t");
    EXPECT_EQ(r.request.body, "{}");
    EXPECT_TRUE(r.request.accepts_gzip);
}

TEST(Http, HeaderValueWhitespaceIsTrimmed)
{
    const Str raw = get("/feed", "Authorization:    Bearer t   \r\n");
    const auto r = parse(raw);
    ASSERT_EQ(r.status, ParseStatus::Complete);
    EXPECT_EQ(r.request.authorization, "Bearer t");
}

TEST(Http, AcceptEncodingListsGzip)
{
    EXPECT_TRUE(parse(get("/feed", "Accept-Encoding: gzip, deflate, br\r\n")).request.accepts_gzip);
    EXPECT_TRUE(parse(get("/feed", "Accept-Encoding: deflate, gzip\r\n")).request.accepts_gzip);
    EXPECT_FALSE(parse(get("/feed", "Accept-Encoding: deflate, br\r\n")).request.accepts_gzip);
    EXPECT_FALSE(parse(get("/feed", "Accept-Encoding: identity\r\n")).request.accepts_gzip);
}

TEST(Http, KeepAliveRules)
{
    EXPECT_TRUE(parse(get("/feed")).request.keep_alive);
    EXPECT_FALSE(parse(get("/feed", "Connection: close\r\n")).request.keep_alive);
    EXPECT_TRUE(parse(get("/feed", "Connection: keep-alive\r\n")).request.keep_alive);
    EXPECT_FALSE(parse(cat("GET /feed HTTP/1.0\r\nHost: x\r\n\r\n")).request.keep_alive);
    EXPECT_TRUE(parse(cat("GET /feed HTTP/1.0\r\nConnection: keep-alive\r\n\r\n")).request.keep_alive);
}

TEST(HttpIncomplete, EveryPrefixOfARequestIsIncomplete)
{
    const Str raw = post("/posts", R"({"body":"hello world"})", "Authorization: Bearer t\r\n");
    for (size_t n = 0; n < raw.size(); n++)
    {
        const auto r = parse_http_request(View(raw.data(), n));
        EXPECT_EQ(r.status, ParseStatus::Incomplete) << "prefix of " << n << " bytes";
    }
    EXPECT_EQ(parse(raw).status, ParseStatus::Complete);
}

TEST(HttpIncomplete, BodyNotFullyThereYet)
{
    const auto r = parse(cat("POST /posts HTTP/1.1\r\nContent-Length: 10\r\n\r\n12345"));
    EXPECT_EQ(r.status, ParseStatus::Incomplete);
}

TEST(HttpPipelining, ConsumesExactlyOneRequest)
{
    const Str first = get("/feed");
    const Str second = post("/posts", "{}");
    const Str both = cat(first, second);
    auto r = parse(both);
    ASSERT_EQ(r.status, ParseStatus::Complete);
    EXPECT_EQ(r.consumed, first.size());
    EXPECT_EQ(r.request.path, "/feed");

    r = parse_http_request(View(both.data() + r.consumed, both.size() - r.consumed));
    ASSERT_EQ(r.status, ParseStatus::Complete);
    EXPECT_EQ(r.request.method, "POST");
    EXPECT_EQ(r.request.body, "{}");
}

TEST(HttpPipelining, SecondRequestStillArrivingLeavesTheFirstComplete)
{
    const Str first = get("/health");
    const Str raw = cat(first, "GET /fe");
    const auto r = parse(raw);
    ASSERT_EQ(r.status, ParseStatus::Complete);
    EXPECT_EQ(r.consumed, first.size());
}

TEST(HttpBad, MalformedRequestLine)
{
    for (const char *raw : {"GARBAGE\r\n\r\n", "GET\r\n\r\n", "GET /feed\r\n\r\n", "GET /feed HTTP/9.9\r\n\r\n",
                            "\r\n\r\n", " / HTTP/1.1\r\n\r\n"})
    {
        EXPECT_EQ(parse(raw).status, ParseStatus::Bad) << raw;
    }
}

TEST(HttpBad, ContentLengthMustBeDigits)
{
    for (const char *len : {"abc", "-1", "1.5", "0x10", "1 2", ""})
    {
        const auto r = parse(cat("POST /posts HTTP/1.1\r\nContent-Length: ", len, "\r\n\r\n{}"));
        EXPECT_EQ(r.status, ParseStatus::Bad) << "Content-Length: " << len;
    }
}

TEST(HttpBad, ConflictingContentLengths)
{
    EXPECT_EQ(parse(cat("POST /posts HTTP/1.1\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\n{}x")).status,
              ParseStatus::Bad);
}

TEST(HttpBad, ChunkedTransferEncodingIsNotSupported)
{
    EXPECT_EQ(parse(cat("POST /posts HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n2\r\n{}\r\n0\r\n\r\n")).status,
              ParseStatus::Bad);
}

TEST(HttpBad, BareNewlinesAreNotALineEnding)
{
    EXPECT_NE(parse(cat("GET /feed HTTP/1.1\nHost: x\n\n")).status, ParseStatus::Complete);
}

TEST(HttpTooLarge, BodyAbove16KB)
{
    const auto r = parse(cat("POST /posts HTTP/1.1\r\nContent-Length: 20000\r\n\r\n"));
    EXPECT_EQ(r.status, ParseStatus::TooLarge);
}

TEST(HttpTooLarge, HeadersThatNeverEnd)
{
    Str raw = "GET /feed HTTP/1.1\r\n";
    for (int i = 0; i < 2000; i++)
        raw = cat(raw, "X-Padding-", num(i), ": aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa\r\n");
    const auto r = parse(raw);
    EXPECT_TRUE(r.status == ParseStatus::TooLarge || r.status == ParseStatus::Bad);
}

} // namespace
} // namespace feed::test
