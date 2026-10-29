
#include <gtest/gtest.h>

#include "json.hpp"
#include "test_util.hpp"

namespace feed::test
{
namespace
{

std::expected<String, BodyError> parse(const Str &raw)
{
    return parse_create_post_body(view(raw));
}

void expect_body(const Str &raw, const Str &want)
{
    const auto r = parse(raw);
    ASSERT_TRUE(r.has_value()) << raw.c_str();
    EXPECT_EQ(*r, want) << raw.c_str();
}

void expect_error(const Str &raw, BodyError want)
{
    const auto r = parse(raw);
    ASSERT_FALSE(r.has_value()) << raw.c_str();
    EXPECT_EQ(r.error(), want) << raw.c_str();
}

Str body_of(const Str &text)
{
    return cat(R"({"body":")", text, R"("})");
}

TEST(JsonBody, PlainBody)
{
    expect_body(R"({"body":"hello"})", "hello");
}

TEST(JsonBody, TrimsSurroundingWhitespace)
{
    expect_body(R"({"body":"  hello  "})", "hello");
    expect_body(R"({"body":"\t\n hello \r\n"})", "hello");
    expect_body(R"({"body":"  inner  spaces  kept  "})", "inner  spaces  kept");
}

TEST(JsonBody, WhitespaceAroundTheDocument)
{
    expect_body("  \n{ \"body\" : \"x\" }\r\n ", "x");
}

TEST(JsonBody, EscapesAreDecoded)
{
    expect_body(R"({"body":"a \"quoted\" \\ back \/ slash"})", R"(a "quoted" \ back / slash)");
    expect_body(R"({"body":"line1\nline2\ttab"})", "line1\nline2\ttab");
}

TEST(JsonBody, UnicodeEscapesBecomeUtf8)
{
    expect_body(R"({"body":"café ✓"})", "caf\xc3\xa9 \xe2\x9c\x93");
    expect_body(R"({"body":"😀"})", "\xf0\x9f\x98\x80");
}

TEST(JsonBody, RawUtf8PassesThrough)
{
    expect_body("{\"body\":\"caf\xc3\xa9 \xe2\x9c\x93\"}", "caf\xc3\xa9 \xe2\x9c\x93");
}

TEST(JsonBody, SpecialCharactersAreKept)
{
    expect_body(R"({"body":"<b>&amp;</b> / ok"})", "<b>&amp;</b> / ok");
}

TEST(JsonBody, OtherFieldsAreIgnored)
{
    expect_body(R"({"extra":1,"body":"x","more":[1,2,{"a":null}]})", "x");
    expect_body(R"({"a":{"b":[1,2,{"c":null}]},"body":"x"})", "x");
}

TEST(JsonBody, LastDuplicateKeyWins)
{
    expect_body(R"({"body":"first","body":"second"})", "second");
}

TEST(JsonBody, EmptyOrWhitespaceOnly)
{
    expect_error(R"({"body":""})", BodyError::BodyRequired);
    expect_error(R"({"body":"   "})", BodyError::BodyRequired);
    expect_error(R"({"body":"\n\t "})", BodyError::BodyRequired);
}

TEST(JsonBody, MissingOrWrongType)
{
    expect_error("{}", BodyError::BodyRequired);
    expect_error(R"({"other":"x"})", BodyError::BodyRequired);
    expect_error(R"({"body":123})", BodyError::BodyRequired);
    expect_error(R"({"body":null})", BodyError::BodyRequired);
    expect_error(R"({"body":true})", BodyError::BodyRequired);
    expect_error(R"({"body":["x"]})", BodyError::BodyRequired);
    expect_error(R"({"body":{"a":"x"}})", BodyError::BodyRequired);
}

TEST(JsonBody, ValidJsonThatIsNotAnObject)
{
    expect_error("[]", BodyError::BodyRequired);
    expect_error(R"("body")", BodyError::BodyRequired);
    expect_error("42", BodyError::BodyRequired);
    expect_error("null", BodyError::BodyRequired);
}

TEST(JsonBody, Malformed)
{
    expect_error("", BodyError::MalformedJson);
    expect_error("   ", BodyError::MalformedJson);
    expect_error("{bad json", BodyError::MalformedJson);
    expect_error(R"({"body":"x")", BodyError::MalformedJson);
    expect_error(R"({"body":"x)", BodyError::MalformedJson);
    expect_error(R"({"body":"x",})", BodyError::MalformedJson);
    expect_error(R"({"body" "x"})", BodyError::MalformedJson);
    expect_error(R"({body:"x"})", BodyError::MalformedJson);
    expect_error(R"({'body':'x'})", BodyError::MalformedJson);
    expect_error(R"({"body":"x"} trailing)", BodyError::MalformedJson);
    expect_error(R"({"body":"x"}{"body":"y"})", BodyError::MalformedJson);
}

TEST(JsonBody, MalformedStrings)
{
    expect_error(R"({"body":"bad \q escape"})", BodyError::MalformedJson);
    expect_error(R"({"body":"bad \u12 escape"})", BodyError::MalformedJson);
    expect_error(R"({"body":"lone \ud83d surrogate"})", BodyError::MalformedJson);
    expect_error(R"({"body":"lone \ude00 surrogate"})", BodyError::MalformedJson);
    expect_error(Str("{\"body\":\"raw\ncontrol\"}"), BodyError::MalformedJson);
}

TEST(JsonBody, MalformedNumbersInIgnoredFields)
{
    expect_error(R"({"n":01,"body":"x"})", BodyError::MalformedJson);
    expect_error(R"({"n":1.,"body":"x"})", BodyError::MalformedJson);
    expect_error(R"({"n":-,"body":"x"})", BodyError::MalformedJson);
}

TEST(JsonBody, DeepNestingDoesNotCrash)
{
    const auto r = parse(cat(Str(100000, '['), Str(100000, ']')));
    ASSERT_FALSE(r.has_value());
    EXPECT_TRUE(r.error() == BodyError::BodyRequired || r.error() == BodyError::MalformedJson);
}

TEST(JsonBody, FiveHundredCharactersIsTheLimit)
{
    expect_body(body_of(repeat("a", 500)), repeat("a", 500));
    expect_error(body_of(repeat("a", 501)), BodyError::BodyTooLong);
}

TEST(JsonBody, LimitAppliesAfterTrimming)
{
    expect_body(body_of(cat("  ", repeat("b", 500), " ")), repeat("b", 500));
    expect_error(body_of(cat("  ", repeat("b", 501), " ")), BodyError::BodyTooLong);
}

TEST(JsonBody, LimitCountsCodePointsNotBytes)
{
    expect_body(body_of(repeat("\xc3\xa9", 500)), repeat("\xc3\xa9", 500));
    expect_error(body_of(repeat("\xc3\xa9", 501)), BodyError::BodyTooLong);
    expect_body(body_of(repeat("\xf0\x9f\x98\x80", 500)), repeat("\xf0\x9f\x98\x80", 500));
    expect_error(body_of(repeat("\xf0\x9f\x98\x80", 501)), BodyError::BodyTooLong);
}

TEST(JsonBody, LimitCountsDecodedEscapes)
{
    expect_body(body_of(repeat(R"(é)", 500)), repeat("\xc3\xa9", 500));
    expect_error(body_of(repeat(R"(é)", 501)), BodyError::BodyTooLong);
}

} // namespace
} // namespace feed::test
