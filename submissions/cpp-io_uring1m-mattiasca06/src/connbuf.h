// 16-byte growable byte buffer for the per-connection in/out/sendbuf slots, so a whole Conn fits one cache line
// (three std::strings are 96 bytes). Idle connections hold nothing: storage goes to a per-thread spare list.
#pragma once
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string_view>
#include <utility>
#include <vector>

struct ConnBuf {
    char *p = nullptr;
    uint32_t len = 0, cap = 0;

    ConnBuf() = default;
    ConnBuf(const ConnBuf &) = delete;
    ConnBuf &operator=(const ConnBuf &) = delete;
    ConnBuf(ConnBuf &&o) noexcept : p(o.p), len(o.len), cap(o.cap) { o.p = nullptr, o.len = o.cap = 0; }
    ConnBuf &operator=(ConnBuf &&o) noexcept {
        if (this != &o) {
            std::free(p);
            p = o.p, len = o.len, cap = o.cap;
            o.p = nullptr, o.len = o.cap = 0;
        }
        return *this;
    }
    ~ConnBuf() { std::free(p); }

    const char *data() const { return p; }
    size_t size() const { return len; }
    bool empty() const { return len == 0; }
    operator std::string_view() const { return {p, len}; }
    void clear() { len = 0; }

    void reserve(size_t n) {
        if (n <= cap) return;
        size_t c = cap ? size_t(cap) * 2 : 256;
        if (c < n) c = n;
        char *q = static_cast<char *>(std::realloc(p, c));
        if (!q) throw std::bad_alloc();
        p = q, cap = uint32_t(c);
    }
    void append(const char *s, size_t n) {
        reserve(size_t(len) + n);
        if (n) std::memcpy(p + len, s, n);
        len += uint32_t(n);
    }
    void append(std::string_view s) { append(s.data(), s.size()); }
    void erase_front(size_t n) {
        if (n >= len) { len = 0; return; }
        std::memmove(p, p + n, len - n);
        len -= uint32_t(n);
    }
    void insert_front(std::string_view s) {
        reserve(size_t(len) + s.size());
        std::memmove(p + s.size(), p, len);
        std::memcpy(p, s.data(), s.size());
        len += uint32_t(s.size());
    }
    void swap(ConnBuf &o) { std::swap(p, o.p), std::swap(len, o.len), std::swap(cap, o.cap); }
};
static_assert(sizeof(ConnBuf) == 16);

inline std::vector<ConnBuf> &spare_conn_bufs() {
    thread_local std::vector<ConnBuf> v;
    return v;
}
// Empty buffer: its storage goes to the pool (or back to the allocator when big or the pool is full).
inline void release_big(ConnBuf &b) {
    if (b.len || !b.cap) return;
    auto &sp = spare_conn_bufs();
    if (b.cap <= 32768 && sp.size() < 1024) sp.push_back(std::move(b));
    else { std::free(b.p); b.p = nullptr; b.cap = 0; }
}
inline void take_buf(ConnBuf &b) {
    auto &sp = spare_conn_bufs();
    if (!b.cap && !sp.empty()) {
        b = std::move(sp.back());
        sp.pop_back();
    }
}
