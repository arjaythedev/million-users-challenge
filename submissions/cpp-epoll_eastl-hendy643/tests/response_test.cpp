
#include <gtest/gtest.h>
#include <libdeflate.h>

#include "response.hpp"
#include "test_util.hpp"

namespace feed::test
{
namespace
{

Str created_at(const char *pg)
{
    return Str(format_created_at(View(pg)));
}

Str json_string(const Str &raw)
{
    Str out;
    append_json_string(out, view(raw));
    return out;
}

TEST(CreatedAt, TruncatesMicrosecondsToMilliseconds)
{
    EXPECT_EQ(created_at("2025-12-31 23:59:19.409123+00"), "2025-12-31T23:59:19.409Z");
}

TEST(CreatedAt, TruncatesInsteadOfRounding)
{
    EXPECT_EQ(created_at("2025-12-31 23:59:19.409999+00"), "2025-12-31T23:59:19.409Z");
    EXPECT_EQ(created_at("2025-12-31 23:59:19.999999+00"), "2025-12-31T23:59:19.999Z");
}

TEST(CreatedAt, PostgresDropsTrailingZerosSoThePaddingMustComeBack)
{
    EXPECT_EQ(created_at("2025-12-31 23:59:19.4+00"), "2025-12-31T23:59:19.400Z");
    EXPECT_EQ(created_at("2025-12-31 23:59:19.05+00"), "2025-12-31T23:59:19.050Z");
    EXPECT_EQ(created_at("2025-12-31 23:59:19.409+00"), "2025-12-31T23:59:19.409Z");
    EXPECT_EQ(created_at("2025-12-31 23:59:19+00"), "2025-12-31T23:59:19.000Z");
}

TEST(CreatedAt, SingleDigitsNotMistakenForRounding)
{
    EXPECT_EQ(created_at("2025-01-02 03:04:05.000999+00"), "2025-01-02T03:04:05.000Z");
    EXPECT_EQ(created_at("2025-01-02 03:04:05.001+00"), "2025-01-02T03:04:05.001Z");
}

TEST(JsonString, PlainAscii)
{
    EXPECT_EQ(json_string("hello"), "\"hello\"");
    EXPECT_EQ(json_string(""), "\"\"");
}

TEST(JsonString, QuotesAndBackslashesAreEscaped)
{
    EXPECT_EQ(json_string("say \"hi\""), R"("say \"hi\"")");
    EXPECT_EQ(json_string("a\\b"), R"("a\\b")");
}

TEST(JsonString, ControlCharactersAreEscaped)
{
    EXPECT_EQ(json_string("a\nb\tc\rd"), R"("a\nb\tc\rd")");
    EXPECT_EQ(json_string(Str("\x01\x1f", 2)), R"("\u0001\u001f")");
    EXPECT_EQ(json_string("\b\f"), R"("\b\f")");
}

TEST(JsonString, NonAsciiAndHtmlCharactersPassThrough)
{
    EXPECT_EQ(json_string("caf\xc3\xa9 \xe2\x9c\x93"), "\"caf\xc3\xa9 \xe2\x9c\x93\"");
    EXPECT_EQ(json_string("<b>&amp;</b> / x"), "\"<b>&amp;</b> / x\"");
}

TEST(JsonString, AppendsAfterWhatIsAlreadyThere)
{
    Str out = "{\"k\":";
    append_json_string(out, View("v"));
    EXPECT_EQ(out, "{\"k\":\"v\"");
}

TEST(PostJson, KeyOrderAndCompactForm)
{
    Str out;
    append_post(out, Post{5, "hello", "2025-12-31 23:59:19.409123+00", "bob", 3});
    EXPECT_EQ(out, R"({"id":5,"body":"hello","created_at":"2025-12-31T23:59:19.409Z","author":"bob","like_count":3})");
}

TEST(PostJson, EscapesBodyAndAuthor)
{
    Str out;
    append_post(out, Post{1, "a \"b\"\n", "2025-01-01 00:00:00+00", "x\"y", 0});
    EXPECT_EQ(out,
              R"({"id":1,"body":"a \"b\"\n","created_at":"2025-01-01T00:00:00.000Z","author":"x\"y","like_count":0})");
}

TEST(PostJson, LargeIdsAndCounts)
{
    Str out;
    append_post(out, Post{9007199254740991, "b", "2025-01-01 00:00:00+00", "a", 2147483647});
    EXPECT_EQ(
        out,
        R"({"id":9007199254740991,"body":"b","created_at":"2025-01-01T00:00:00.000Z","author":"a","like_count":2147483647})");
}

TEST(ErrorBody, Shape)
{
    EXPECT_EQ(error_body(View("not found")), R"({"error":"not found"})");
    EXPECT_EQ(error_body(View("body must be at most 500 characters")),
              R"({"error":"body must be at most 500 characters"})");
}

TEST(HttpResponse, PlainJson)
{
    Str out;
    append_http_response(out, 201, View("{\"a\":1}"), true, true);
    EXPECT_EQ(out, "HTTP/1.1 201 Created\r\nContent-Type: application/json\r\nContent-Length: 7\r\n\r\n{\"a\":1}");
}

TEST(HttpResponse, ConnectionCloseIsAnnounced)
{
    Str out;
    append_http_response(out, 400, View("{}"), false, false);
    EXPECT_EQ(out, "HTTP/1.1 400 Bad Request\r\nContent-Type: application/json\r\nContent-Length: 2\r\nConnection: "
                   "close\r\n\r\n{}");
}

TEST(HttpResponse, GzipRoundTrips)
{
    const Str body = cat(R"({"posts":[)", repeat(R"({"id":1,"body":"hello"},)", 40), "]}");
    Str out;
    append_http_response(out, 200, view(body), true, true);

    const size_t split = View(out).find("\r\n\r\n");
    ASSERT_NE(split, View::npos);
    const Str head = Str(View(out).substr(0, split));
    EXPECT_NE(View(head).find("Content-Encoding: gzip"), View::npos);

    libdeflate_decompressor *decompressor = libdeflate_alloc_decompressor();
    ASSERT_NE(decompressor, nullptr);
    Str decoded(body.size(), '\0');
    size_t produced = 0;
    EXPECT_EQ(libdeflate_gzip_decompress(decompressor, out.data() + split + 4, out.size() - split - 4, decoded.data(),
                                         decoded.size(), &produced),
              LIBDEFLATE_SUCCESS);
    libdeflate_free_decompressor(decompressor);
    EXPECT_EQ(produced, body.size());
    EXPECT_EQ(decoded, body);
    EXPECT_LT(out.size(), body.size());
}

TEST(HttpResponse, NoGzipUnlessAsked)
{
    const Str body = repeat("0123456789", 100);
    Str out;
    append_http_response(out, 200, view(body), true, false);
    EXPECT_EQ(View(out).find("Content-Encoding"), View::npos);
    EXPECT_NE(View(out).find(body), View::npos);
}

} // namespace
} // namespace feed::test
