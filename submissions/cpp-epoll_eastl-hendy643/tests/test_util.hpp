#pragma once

#include <mbedtls/md.h>

#include <charconv>
#include <cstdint>
#include <cstring>

#include "types.hpp"

namespace feed::test
{

using Str = String;

inline constexpr const char *kSecret = "million-users-challenge";
inline constexpr int64_t kNow = 1'800'000'000;

inline void append_part(Str &out, const char *s)
{
    out.append(s, std::strlen(s));
}
inline void append_part(Str &out, const Str &s)
{
    out.append(s.data(), s.size());
}
inline void append_part(Str &out, char c)
{
    out.push_back(c);
}

template <class... Parts> Str cat(const Parts &...parts)
{
    Str out;
    (append_part(out, parts), ...);
    return out;
}

inline Str num(int64_t v)
{
    char buf[24];
    const auto r = std::to_chars(buf, buf + sizeof buf, v);
    return Str(buf, size_t(r.ptr - buf));
}

inline Str repeat(const Str &unit, size_t n)
{
    Str out;
    for (size_t i = 0; i < n; i++)
        append_part(out, unit);
    return out;
}

inline View view(const Str &s)
{
    return View(s.data(), s.size());
}

inline Str base64url(const Str &in)
{
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    Str out;
    size_t i = 0;
    for (; i + 2 < in.size(); i += 3)
    {
        const uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8) | uint8_t(in[i + 2]);
        for (int shift : {18, 12, 6, 0})
            out.push_back(kAlphabet[(v >> shift) & 63]);
    }
    if (i + 1 == in.size())
    {
        const uint32_t v = uint8_t(in[i]) << 16;
        out.push_back(kAlphabet[v >> 18]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
    }
    else if (i + 2 == in.size())
    {
        const uint32_t v = (uint8_t(in[i]) << 16) | (uint8_t(in[i + 1]) << 8);
        out.push_back(kAlphabet[v >> 18]);
        out.push_back(kAlphabet[(v >> 12) & 63]);
        out.push_back(kAlphabet[(v >> 6) & 63]);
    }
    return out;
}

inline Str hmac_sha256(const Str &data, const Str &secret)
{
    unsigned char mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    reinterpret_cast<const unsigned char *>(secret.data()), secret.size(),
                    reinterpret_cast<const unsigned char *>(data.data()), data.size(), mac);
    return Str(reinterpret_cast<const char *>(mac), sizeof mac);
}

inline Str make_jwt(const Str &payload_json, const Str &secret = kSecret,
                    const Str &header_json = R"({"alg":"HS256","typ":"JWT"})")
{
    const Str signing_input = cat(base64url(header_json), '.', base64url(payload_json));
    return cat(signing_input, '.', base64url(hmac_sha256(signing_input, secret)));
}

inline Str payload(const Str &sub, const Str &username, const Str &exp_json = num(kNow + 3600))
{
    return cat(R"({"sub":")", sub, R"(","username":")", username, R"(","iat":)", num(kNow), R"(,"exp":)", exp_json,
               '}');
}

inline Str bearer(const Str &jwt)
{
    return cat("Bearer ", jwt);
}

} // namespace feed::test
