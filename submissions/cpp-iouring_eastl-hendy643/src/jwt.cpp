#include "jwt.hpp"

#include <mbedtls/md.h>

#include <algorithm>
#include <cstring>

#include "json.hpp"

namespace feed
{
namespace
{

int b64_value(const char c)
{
    if (c >= 'A' && c <= 'Z')
        return c - 'A';
    if (c >= 'a' && c <= 'z')
        return c - 'a' + 26;
    if (c >= '0' && c <= '9')
        return c - '0' + 52;
    if (c == '-')
        return 62;
    if (c == '_')
        return 63;
    return -1;
}

bool b64url_decode(const View in, String &out)
{
    if (in.size() % 4 == 1)
        return false;
    unsigned acc = 0;
    int bits = 0;
    for (const char c : in)
    {
        const int v = b64_value(c);
        if (v < 0)
            return false;
        acc = (acc << 6) | static_cast<unsigned>(v);
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back(static_cast<char>((acc >> bits) & 0xFF));
        }
    }
    return true;
}

bool all_digits(const View s)
{
    return !s.empty() && std::all_of(s.begin(), s.end(), [](const char c) { return c >= '0' && c <= '9'; });
}

bool to_int64(const View digits, int64_t &out)
{
    if (digits.size() > 18)
        return false;
    out = 0;
    for (const char c : digits)
        out = out * 10 + (c - '0');
    return true;
}

AuthResult fail(const AuthStatus s)
{
    AuthResult r;
    r.status = s;
    return r;
}

}

AuthResult authenticate(View authorization, View secret, int64_t now)
{
    constexpr View prefix = "Bearer ";
    if (authorization.size() < prefix.size() || authorization.substr(0, prefix.size()) != prefix)
    {
        return fail(AuthStatus::MissingBearer);
    }
    const View token = authorization.substr(prefix.size());

    const size_t dot1 = token.find('.');
    if (dot1 == View::npos)
        return fail(AuthStatus::InvalidToken);
    const size_t dot2 = token.find('.', dot1 + 1);
    if (dot2 == View::npos || token.find('.', dot2 + 1) != View::npos)
        return fail(AuthStatus::InvalidToken);
    const View header_b64 = token.substr(0, dot1);
    const View payload_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
    const View sig_b64 = token.substr(dot2 + 1);
    if (header_b64.empty() || sig_b64.empty())
        return fail(AuthStatus::InvalidToken);

    String header, payload, sig;
    if (!b64url_decode(header_b64, header) || !b64url_decode(payload_b64, payload) || !b64url_decode(sig_b64, sig))
    {
        return fail(AuthStatus::InvalidToken);
    }

    JsonDocument hdoc;
    if (!parse_json(View(header), hdoc) || !hdoc.is_object)
        return fail(AuthStatus::InvalidToken);
    const JsonField *alg = hdoc.find("alg");
    if (!alg || alg->type != JsonType::String || View(alg->text) != View("HS256"))
    {
        return fail(AuthStatus::InvalidToken);
    }

    unsigned char mac[32];
    if (mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                        reinterpret_cast<const unsigned char *>(secret.data()), secret.size(),
                        reinterpret_cast<const unsigned char *>(token.data()), dot2, mac) != 0)
    {
        return fail(AuthStatus::InvalidToken);
    }
    if (sig.size() != sizeof mac)
        return fail(AuthStatus::InvalidToken);
    unsigned char diff = 0;
    for (size_t i = 0; i < sizeof mac; i++)
        diff |= static_cast<unsigned char>(sig[i]) ^ mac[i];
    if (diff != 0)
        return fail(AuthStatus::InvalidToken);

    JsonDocument pdoc;
    if (!parse_json(View(payload), pdoc) || !pdoc.is_object)
        return fail(AuthStatus::InvalidPayload);

    const JsonField *exp = pdoc.find("exp");
    if (!exp || exp->type != JsonType::Number)
        return fail(AuthStatus::InvalidPayload);
    {
        int64_t exp_seconds = 0;
        if (all_digits(View(exp->text)) && to_int64(View(exp->text), exp_seconds))
        {
            if (now >= exp_seconds)
                return fail(AuthStatus::InvalidToken);
        }
        else
        {
            const double e = std::strtod(String(exp->text).c_str(), nullptr);
            if (!(e < 1e15) || !(static_cast<double>(now) < e))
                return fail(AuthStatus::InvalidToken);
        }
    }

    const JsonField *sub = pdoc.find("sub");
    const JsonField *username = pdoc.find("username");
    if (!sub || sub->type != JsonType::String || !all_digits(View(sub->text)))
        return fail(AuthStatus::InvalidPayload);
    if (!username || username->type != JsonType::String)
        return fail(AuthStatus::InvalidPayload);

    AuthResult r;
    if (!to_int64(View(sub->text), r.claims.user_id) || r.claims.user_id <= 0)
        return fail(AuthStatus::InvalidPayload);
    r.claims.username = username->text;
    r.status = AuthStatus::Ok;
    return r;
}

}
