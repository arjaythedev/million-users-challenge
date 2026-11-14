// Hand-written JSON output helpers.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#if defined(__x86_64__)
#include <immintrin.h>
#endif

namespace json {

inline constexpr char kHex[] = "0123456789abcdef";

// Appends a string literal. std::string::append is an out-of-line libstdc++ call (strlen + memcpy through the PLT); with the
// length known at compile time this writes the bytes inline instead.
template <size_t N> inline void lit(std::string &o, const char (&s)[N]) {
#if defined(_GLIBCXX_RELEASE) && _GLIBCXX_RELEASE >= 15
    const size_t at = o.size();
    o.__resize_and_overwrite(at + N - 1, [&](char *p, size_t) {
        memcpy(p + at, s, N - 1);
        return at + N - 1;
    });
#else
    o.append(s, N - 1);
#endif
}

// First index >= i whose byte needs escaping (`"`, `\`, or < 0x20); n if none.
// Strings shorter than 16 bytes (most author names) are scanned inline, 8 bytes at a time, without a stack frame. Longer ones go
// to an out-of-line vector loop: AVX2 32 bytes at a time when the build enables it (-march=native defines __AVX2__; there is no
// run-time dispatch, the binary is built for the machine it runs on), otherwise SSE2 16 bytes at a time. Other targets: plain loop.

// Fewer than 16 bytes left: two 8-byte words with the "has a byte < 0x20 / == '"' / == '\\'" bit tricks. The lowest flagged
// byte of each test is exact, so the lowest bit of their OR is the first special byte. A second, overlapping word covers the
// last bytes; the overlap is already known to be clean.
inline size_t next_special_short(const char *s, size_t i, size_t n) {
    auto swar8 = [](uint64_t x) -> uint64_t {
        constexpr uint64_t ones = 0x0101010101010101ull, high = 0x8080808080808080ull;
        const uint64_t q = x ^ (ones * '"'), b = x ^ (ones * '\\');
        return (((x - ones * 0x20) & ~x) | ((q - ones) & ~q) | ((b - ones) & ~b)) & high;
    };
    if (n - i >= 8) {
        uint64_t w;
        memcpy(&w, s + i, 8);
        if (uint64_t m = swar8(w)) return i + size_t(__builtin_ctzll(m) >> 3);
        if (i + 8 == n) return n;
        memcpy(&w, s + n - 8, 8);
        if (uint64_t m = swar8(w)) return n - 8 + size_t(__builtin_ctzll(m) >> 3);
        return n;
    }
    for (; i < n; i++) {
        unsigned char ch = static_cast<unsigned char>(s[i]);
        if (ch < 0x20 || ch == '"' || ch == '\\') return i;
    }
    return n;
}

__attribute__((noinline)) inline size_t next_special_long(const char *s, size_t i, size_t n) {
#if defined(__x86_64__)
#if defined(__AVX2__)
    const __m256i quote = _mm256_set1_epi8('"'), bslash = _mm256_set1_epi8('\\'), ctl = _mm256_set1_epi8(0x1f);
    for (; i + 32 <= n; i += 32) {
        __m256i v = _mm256_loadu_si256(reinterpret_cast<const __m256i *>(s + i));
        // byte < 0x20  <=>  min_epu8(v, 0x1f) == v
        __m256i m = _mm256_or_si256(_mm256_or_si256(_mm256_cmpeq_epi8(v, quote), _mm256_cmpeq_epi8(v, bslash)),
                                    _mm256_cmpeq_epi8(_mm256_min_epu8(v, ctl), v));
        uint32_t bits = static_cast<uint32_t>(_mm256_movemask_epi8(m));
        if (bits) return i + __builtin_ctz(bits);
    }
#endif
    const __m128i quote16 = _mm_set1_epi8('"'), bslash16 = _mm_set1_epi8('\\'), ctl16 = _mm_set1_epi8(0x1f);
    for (; i + 16 <= n; i += 16) {
        __m128i v = _mm_loadu_si128(reinterpret_cast<const __m128i *>(s + i));
        __m128i m = _mm_or_si128(_mm_or_si128(_mm_cmpeq_epi8(v, quote16), _mm_cmpeq_epi8(v, bslash16)),
                                 _mm_cmpeq_epi8(_mm_min_epu8(v, ctl16), v));
        uint32_t bits = static_cast<uint32_t>(_mm_movemask_epi8(m));
        if (bits) return i + __builtin_ctz(bits);
    }
    return next_special_short(s, i, n);
#else
    for (; i < n; i++) {
        unsigned char ch = static_cast<unsigned char>(s[i]);
        if (ch < 0x20 || ch == '"' || ch == '\\') return i;
    }
    return n;
#endif
}

inline size_t next_special(const char *s, size_t i, size_t n) {
    if (n - i < 16) return next_special_short(s, i, n);
    return next_special_long(s, i, n);
}

// Appends `s` as a quoted JSON string. Only `"`, `\` and bytes < 0x20 are escaped; other bytes (UTF-8) pass through.
inline void str(std::string &o, const char *s, size_t n) {
    size_t i = next_special(s, 0, n);
#if defined(_GLIBCXX_RELEASE) && _GLIBCXX_RELEASE >= 15
    if (i == n) [[likely]] {  // nothing to escape (the common case): quotes and bytes in one write
        const size_t at = o.size();
        o.__resize_and_overwrite(at + n + 2, [&](char *p, size_t) {
            p[at] = '"';
            memcpy(p + at + 1, s, n);
            p[at + n + 1] = '"';
            return at + n + 2;
        });
        return;
    }
#endif
    o.push_back('"');
    size_t run = 0;
    for (; i < n; i = next_special(s, i + 1, n)) {
        unsigned char ch = static_cast<unsigned char>(s[i]);
        o.append(s + run, i - run);
        run = i + 1;
        switch (ch) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        case '\b': o += "\\b"; break;
        case '\f': o += "\\f"; break;
        default:
            o += "\\u00";
            o.push_back(kHex[ch >> 4]);
            o.push_back(kHex[ch & 15]);
        }
    }
    o.append(s + run, n - run);
    o.push_back('"');
}

}  // namespace json
