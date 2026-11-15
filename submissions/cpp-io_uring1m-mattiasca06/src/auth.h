// HS256 JWT with precomputed HMAC inner/outer SHA-256 states. Read-only after init, so safe to share between workers.
#pragma once
#include <openssl/sha.h>

#include <cstdint>
#include <string>
#include <string_view>

enum class JwtStatus { Ok, Invalid, BadPayload };

struct Jwt {
    void init(std::string_view secret);
    // Verifies alg (HS256 only), signature and `exp` (against `now`). Invalid: malformed, wrong alg, bad signature or
    // expired. BadPayload: valid token whose `sub` is not a positive integer or whose `username` is not a string.
    // On Ok, `sub` and `username` are set.
    JwtStatus verify(std::string_view token, int64_t now, int64_t &sub, std::string &username) const;

  private:
    void mac(std::string_view data, unsigned char out[32]) const;
    SHA256_CTX inner_, outer_;
};

extern Jwt g_jwt;
