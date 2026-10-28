#include "auth.h"

#include <openssl/crypto.h>

#include <cstring>

#include "codec.h"
#include "yyjson.h"

Jwt g_jwt;

void Jwt::init(std::string_view secret) {
    unsigned char key[64] = {0};
    if (secret.size() > 64) {
        SHA256(reinterpret_cast<const unsigned char *>(secret.data()), secret.size(), key);
    } else {
        memcpy(key, secret.data(), secret.size());
    }
    unsigned char pad[64];
    for (int i = 0; i < 64; i++) pad[i] = key[i] ^ 0x36;
    SHA256_Init(&inner_);
    SHA256_Update(&inner_, pad, 64);
    for (int i = 0; i < 64; i++) pad[i] = key[i] ^ 0x5c;
    SHA256_Init(&outer_);
    SHA256_Update(&outer_, pad, 64);
}

void Jwt::mac(std::string_view data, unsigned char out[32]) const {
    SHA256_CTX c = inner_;
    SHA256_Update(&c, data.data(), data.size());
    unsigned char d[32];
    SHA256_Final(d, &c);
    c = outer_;
    SHA256_Update(&c, d, 32);
    SHA256_Final(out, &c);
}

JwtStatus Jwt::verify(std::string_view token, int64_t now, int64_t &sub, std::string &username) const {
    size_t d1 = token.find('.');
    if (d1 == std::string_view::npos) return JwtStatus::Invalid;
    size_t d2 = token.find('.', d1 + 1);
    if (d2 == std::string_view::npos || token.find('.', d2 + 1) != std::string_view::npos) return JwtStatus::Invalid;

    // reused per thread: verify() runs on the worker threads only, one call at a time
    thread_local std::string hdr, sig, payload;
    static constexpr std::string_view kStdHdr = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";  // {"alg":"HS256","typ":"JWT"}
    if (token.substr(0, d1) != kStdHdr) {  // anyone else's header: check the algorithm the slow way
        if (!codec::b64url_decode(token.substr(0, d1), hdr)) return JwtStatus::Invalid;
        yyjson_doc *doc = yyjson_read(hdr.data(), hdr.size(), 0);
        if (!doc) return JwtStatus::Invalid;
        yyjson_val *alg = yyjson_obj_get(yyjson_doc_get_root(doc), "alg");
        bool ok = yyjson_is_str(alg) && strcmp(yyjson_get_str(alg), "HS256") == 0;
        yyjson_doc_free(doc);
        if (!ok) return JwtStatus::Invalid;
    }

    if (!codec::b64url_decode(token.substr(d2 + 1), sig) || sig.size() != 32) return JwtStatus::Invalid;
    unsigned char m[32];
    mac(token.substr(0, d2), m);
    if (CRYPTO_memcmp(m, sig.data(), 32) != 0) return JwtStatus::Invalid;

    if (!codec::b64url_decode(token.substr(d1 + 1, d2 - d1 - 1), payload)) return JwtStatus::Invalid;
    // the payload is tiny: parse it in stack memory instead of the heap
    char pool[2048];
    yyjson_alc alc;
    yyjson_doc *doc;
    if (yyjson_read_max_memory_usage(payload.size(), 0) <= sizeof pool && yyjson_alc_pool_init(&alc, pool, sizeof pool))
        doc = yyjson_read_opts(payload.data(), payload.size(), 0, &alc, nullptr);
    else
        doc = yyjson_read(payload.data(), payload.size(), 0);
    if (!doc) return JwtStatus::Invalid;
    yyjson_val *root = yyjson_doc_get_root(doc);
    JwtStatus st = JwtStatus::Invalid;
    yyjson_val *e = yyjson_obj_get(root, "exp");
    if (yyjson_is_obj(root) && (!e || (yyjson_is_num(e) && yyjson_get_num(e) > double(now)))) {
        st = JwtStatus::BadPayload;
        yyjson_val *s = yyjson_obj_get(root, "sub");
        yyjson_val *u = yyjson_obj_get(root, "username");
        int64_t v = 0;
        bool good = false;
        if (yyjson_is_str(s)) {  // "sub":"123" (a number is accepted too)
            const char *p = yyjson_get_str(s);
            size_t n = yyjson_get_len(s);
            good = n > 0 && n < 19;
            for (size_t i = 0; i < n && good; i++) {
                if (p[i] < '0' || p[i] > '9') good = false;
                else v = v * 10 + (p[i] - '0');
            }
        } else if (yyjson_is_uint(s) || yyjson_is_sint(s)) {
            v = yyjson_get_sint(s);
            good = true;
        }
        if (good && v > 0 && yyjson_is_str(u)) {
            sub = v;
            username.assign(yyjson_get_str(u), yyjson_get_len(u));
            st = JwtStatus::Ok;
        }
    }
    yyjson_doc_free(doc);
    return st;
}
