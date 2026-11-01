
#include <gtest/gtest.h>

#include "jwt.hpp"
#include "test_util.hpp"

namespace feed::test
{
namespace
{

AuthResult auth(const Str &header, int64_t now = kNow, const Str &secret = kSecret)
{
    return authenticate(view(header), view(secret), now);
}

AuthResult auth_token(const Str &jwt)
{
    return auth(bearer(jwt));
}

AuthStatus status_of_payload(const Str &payload_json)
{
    return auth_token(make_jwt(payload_json)).status;
}

TEST(Jwt, ValidTokenYieldsUserIdAndUsername)
{
    const auto r = auth_token(make_jwt(payload("20000", "golden_ember_1")));
    ASSERT_EQ(r.status, AuthStatus::Ok);
    EXPECT_EQ(r.claims.user_id, 20000);
    EXPECT_EQ(r.claims.username, "golden_ember_1");
}

TEST(Jwt, UsernameIsUsedAsIs)
{
    const auto r = auth_token(make_jwt(payload("1", "Mixed Case_\xc3\xbcn\xc3\xaf")));
    ASSERT_EQ(r.status, AuthStatus::Ok);
    EXPECT_EQ(r.claims.username, "Mixed Case_\xc3\xbcn\xc3\xaf");
}

TEST(Jwt, EscapedUsernameIsDecoded)
{
    const auto r = auth_token(make_jwt(payload("1", R"(café \"x\")")));
    ASSERT_EQ(r.status, AuthStatus::Ok);
    EXPECT_EQ(r.claims.username, "caf\xc3\xa9 \"x\"");
}

TEST(Jwt, NonCanonicalButValidHeaderIsAccepted)
{
    EXPECT_EQ(auth_token(make_jwt(payload("1", "u"), kSecret, R"({ "typ": "JWT", "alg": "HS256" })")).status,
              AuthStatus::Ok);
    EXPECT_EQ(auth_token(make_jwt(payload("1", "u"), kSecret, R"({"alg":"HS256"})")).status, AuthStatus::Ok);
}

TEST(Jwt, ExtraClaimsAreIgnored)
{
    const Str p = cat(R"({"sub":"7","username":"u","role":"admin","nested":{"a":[1,2]},"exp":)", num(kNow + 3600), '}');
    const auto r = auth_token(make_jwt(p));
    ASSERT_EQ(r.status, AuthStatus::Ok);
    EXPECT_EQ(r.claims.user_id, 7);
}

TEST(JwtHeader, AbsentHeader)
{
    EXPECT_EQ(auth("").status, AuthStatus::MissingBearer);
}

TEST(JwtHeader, NotBearerScheme)
{
    EXPECT_EQ(auth("Basic abc").status, AuthStatus::MissingBearer);
    EXPECT_EQ(auth("Token abc").status, AuthStatus::MissingBearer);
    EXPECT_EQ(auth("bearer abc").status, AuthStatus::MissingBearer);
    EXPECT_EQ(auth(make_jwt(payload("1", "u"))).status, AuthStatus::MissingBearer);
}

TEST(JwtHeader, BearerWithoutTheSpace)
{
    EXPECT_EQ(auth("Bearer").status, AuthStatus::MissingBearer);
    EXPECT_EQ(auth(cat("Bearer", make_jwt(payload("1", "u")))).status, AuthStatus::MissingBearer);
}

TEST(JwtToken, Garbage)
{
    for (const char *t : {"abc", "", "a.b", "a.b.c.d", "...", ".", "a..c"})
    {
        EXPECT_EQ(auth_token(t).status, AuthStatus::InvalidToken) << t;
    }
}

TEST(JwtToken, NotBase64Url)
{
    const Str good = make_jwt(payload("1", "u"));
    const Str rest = good.substr(good.find('.') + 1);
    EXPECT_EQ(auth_token(cat("!!!.", rest)).status, AuthStatus::InvalidToken);
    EXPECT_EQ(auth_token(cat("ab+/.", rest)).status, AuthStatus::InvalidToken);
}

TEST(JwtToken, PaddedSegmentsAreRejected)
{
    const Str good = make_jwt(payload("1", "u"));
    EXPECT_EQ(auth_token(cat(good, "=")).status, AuthStatus::InvalidToken);
}

TEST(JwtToken, WrongSecret)
{
    EXPECT_EQ(auth_token(make_jwt(payload("1", "u"), "another-secret")).status, AuthStatus::InvalidToken);
    EXPECT_EQ(auth(bearer(make_jwt(payload("1", "u"))), kNow, "another-secret").status, AuthStatus::InvalidToken);
}

TEST(JwtToken, TamperedPayloadKeepsOldSignature)
{
    const Str a = make_jwt(payload("1", "u"));
    const Str b = make_jwt(payload("2", "u"));
    const auto a1 = a.find('.'), a2 = a.rfind('.');
    const auto b1 = b.find('.'), b2 = b.rfind('.');
    const Str spliced = cat(a.substr(0, a1 + 1), b.substr(b1 + 1, b2 - b1 - 1), a.substr(a2));
    EXPECT_EQ(auth_token(spliced).status, AuthStatus::InvalidToken);
}

TEST(JwtToken, EverySingleCharacterChangeInTheSignatureIsCaught)
{
    const Str good = make_jwt(payload("1", "u"));
    const size_t sig = good.rfind('.') + 1;
    for (size_t i = sig; i < good.size(); i++)
    {
        Str bad = good;
        bad[i] = bad[i] == 'A' ? 'B' : 'A';
        if (i + 1 < good.size())
            EXPECT_EQ(auth_token(bad).status, AuthStatus::InvalidToken) << "position " << i;
    }
}

TEST(JwtToken, TruncatedOrEmptySignature)
{
    Str t = make_jwt(payload("1", "u"));
    EXPECT_EQ(auth_token(t.substr(0, t.rfind('.') + 1)).status, AuthStatus::InvalidToken);
    t.pop_back();
    EXPECT_EQ(auth_token(t).status, AuthStatus::InvalidToken);
}

TEST(JwtToken, AlgNoneIsRejected)
{
    const Str t = cat(base64url(R"({"alg":"none","typ":"JWT"})"), '.', base64url(payload("1", "u")), '.');
    EXPECT_EQ(auth_token(t).status, AuthStatus::InvalidToken);
}

TEST(JwtToken, OnlyHs256IsAccepted)
{
    for (const char *alg : {"HS384", "HS512", "RS256", "ES256", "none", "None", "hs256", ""})
    {
        const Str header = cat(R"({"alg":")", alg, R"(","typ":"JWT"})");
        EXPECT_EQ(auth_token(make_jwt(payload("1", "u"), kSecret, header)).status, AuthStatus::InvalidToken) << alg;
    }
}

TEST(JwtToken, AlgMustBeAString)
{
    EXPECT_EQ(auth_token(make_jwt(payload("1", "u"), kSecret, R"({"alg":null})")).status, AuthStatus::InvalidToken);
    EXPECT_EQ(auth_token(make_jwt(payload("1", "u"), kSecret, R"({"alg":["HS256"]})")).status,
              AuthStatus::InvalidToken);
}

TEST(JwtToken, HeaderWithoutAlgOrNotJson)
{
    EXPECT_EQ(auth_token(make_jwt(payload("1", "u"), kSecret, R"({"typ":"JWT"})")).status, AuthStatus::InvalidToken);
    EXPECT_EQ(auth_token(make_jwt(payload("1", "u"), kSecret, "not json")).status, AuthStatus::InvalidToken);
    EXPECT_EQ(auth_token(make_jwt(payload("1", "u"), kSecret, "[]")).status, AuthStatus::InvalidToken);
}

TEST(JwtToken, SignatureIsCheckedBeforeThePayload)
{
    EXPECT_EQ(auth_token(make_jwt(payload("abc", "u"), "another-secret")).status, AuthStatus::InvalidToken);
    EXPECT_EQ(auth_token(make_jwt(R"({"sub":"1","username":"u"})", "another-secret")).status, AuthStatus::InvalidToken);
}

TEST(JwtToken, HmacHelperMatchesRfc4231)
{
    const Str mac = hmac_sha256("what do ya want for nothing?", "Jefe");
    static constexpr char hex[] = "0123456789abcdef";
    Str got;
    for (const char c : mac)
    {
        got.push_back(hex[(uint8_t(c) >> 4)]);
        got.push_back(hex[uint8_t(c) & 15]);
    }
    EXPECT_EQ(got, "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843");
}

TEST(JwtToken, SecretOfAnyLengthIsAValidHmacKey)
{
    for (size_t n : {size_t(0), size_t(1), size_t(63), size_t(64), size_t(65), size_t(128), size_t(200)})
    {
        Str secret;
        for (size_t i = 0; i < n; i++)
            secret.push_back(char('a' + i % 26));
        const Str t = make_jwt(payload("7", "zed"), secret);
        EXPECT_EQ(auth(bearer(t), kNow, secret).status, AuthStatus::Ok) << "secret length " << n;
        EXPECT_EQ(auth(bearer(t), kNow, "different").status, AuthStatus::InvalidToken) << "secret length " << n;
    }
}

TEST(JwtExp, ExpiredTokenIsInvalid)
{
    EXPECT_EQ(auth_token(make_jwt(payload("1", "u", num(kNow - 3600)))).status, AuthStatus::InvalidToken);
    EXPECT_EQ(auth_token(make_jwt(payload("1", "u", num(kNow - 1)))).status, AuthStatus::InvalidToken);
    EXPECT_EQ(auth_token(make_jwt(payload("1", "u", "0"))).status, AuthStatus::InvalidToken);
    EXPECT_EQ(auth_token(make_jwt(payload("1", "u", "-5"))).status, AuthStatus::InvalidToken);
}

TEST(JwtExp, FutureTokenIsValid)
{
    EXPECT_EQ(auth_token(make_jwt(payload("1", "u", num(kNow + 1)))).status, AuthStatus::Ok);
    EXPECT_EQ(auth_token(make_jwt(payload("1", "u", "99999999999"))).status, AuthStatus::Ok);
}

TEST(JwtExp, VerdictFollowsTheClockPassedIn)
{
    const Str t = make_jwt(payload("1", "u", num(kNow + 10)));
    EXPECT_EQ(auth(bearer(t), kNow).status, AuthStatus::Ok);
    EXPECT_EQ(auth(bearer(t), kNow + 3600).status, AuthStatus::InvalidToken);
    EXPECT_EQ(auth(bearer(t), kNow).status, AuthStatus::Ok);
}

TEST(JwtExp, MissingExpIsRejected)
{
    EXPECT_EQ(status_of_payload(R"({"sub":"1","username":"u"})"), AuthStatus::InvalidPayload);
    EXPECT_EQ(status_of_payload(R"({"sub":"1","username":"u","iat":1})"), AuthStatus::InvalidPayload);
}

TEST(JwtExp, NonNumericExpIsRejected)
{
    for (const char *exp :
         {"\"soon\"", "\"99999999999\"", "null", "true", "false", "[]", "[99999999999]", "{}", "\"\""})
    {
        EXPECT_EQ(auth_token(make_jwt(payload("1", "u", exp))).status, AuthStatus::InvalidPayload) << "exp=" << exp;
    }
}

TEST(JwtPayload, SubMustBeAPositiveIntegerString)
{
    for (const char *sub :
         {"abc", "0", "-1", "1.5", "", " 1", "1 ", "0x10", "1e3", "+1", "1_000", "99999999999999999999"})
    {
        EXPECT_EQ(auth_token(make_jwt(payload(sub, "u"))).status, AuthStatus::InvalidPayload) << "sub=" << sub;
    }
}

TEST(JwtPayload, SubAsJsonNumberIsNotAString)
{
    EXPECT_EQ(status_of_payload(cat(R"({"sub":5,"username":"u","exp":)", num(kNow + 3600), '}')),
              AuthStatus::InvalidPayload);
}

TEST(JwtPayload, MissingOrNullSub)
{
    EXPECT_EQ(status_of_payload(cat(R"({"username":"u","exp":)", num(kNow + 3600), '}')), AuthStatus::InvalidPayload);
    EXPECT_EQ(status_of_payload(cat(R"({"sub":null,"username":"u","exp":)", num(kNow + 3600), '}')),
              AuthStatus::InvalidPayload);
}

TEST(JwtPayload, UsernameMustBeAString)
{
    const Str exp = num(kNow + 3600);
    EXPECT_EQ(status_of_payload(cat(R"({"sub":"1","exp":)", exp, '}')), AuthStatus::InvalidPayload);
    EXPECT_EQ(status_of_payload(cat(R"({"sub":"1","username":123,"exp":)", exp, '}')), AuthStatus::InvalidPayload);
    EXPECT_EQ(status_of_payload(cat(R"({"sub":"1","username":null,"exp":)", exp, '}')), AuthStatus::InvalidPayload);
    EXPECT_EQ(status_of_payload(cat(R"({"sub":"1","username":["a"],"exp":)", exp, '}')), AuthStatus::InvalidPayload);
}

TEST(JwtPayload, PayloadThatIsNotAnObject)
{
    for (const char *p :
         {"[1,2]", "\"text\"", "42", "null", "", "not json", R"({"sub":"1","username":)", R"({"sub":"1")"})
    {
        EXPECT_EQ(status_of_payload(p), AuthStatus::InvalidPayload) << p;
    }
}

}
}
