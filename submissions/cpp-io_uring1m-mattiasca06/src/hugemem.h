// Memory on transparent huge pages for the structures every request touches (coroutine frames, the connection table): on
// 4 KiB pages they cost about 15 page walks a request.
#pragma once
#include <sys/mman.h>

#include <cstddef>
#include <cstdlib>
#include <new>

namespace hugemem {
inline constexpr size_t kHuge = size_t(2) << 20;

// A block of `bytes` (rounded up to 2 MB), 2 MB aligned, advised to use huge pages. Freed with free().
inline void *alloc(size_t bytes) {
    bytes = (bytes + kHuge - 1) / kHuge * kHuge;
    void *p = std::aligned_alloc(kHuge, bytes);
    if (!p) throw std::bad_alloc();
    madvise(p, bytes, MADV_HUGEPAGE);
    return p;
}

// Per-thread bump allocation out of 2 MB chunks that are never returned (callers recycle what they take).
inline void *slab_alloc(size_t n) {
    thread_local char *cur = nullptr, *end = nullptr;
    if (size_t(end - cur) < n) [[unlikely]] {
        void *m = mmap(nullptr, 2 * kHuge, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (m == MAP_FAILED) throw std::bad_alloc();
        cur = reinterpret_cast<char *>((reinterpret_cast<size_t>(m) + kHuge - 1) & ~(kHuge - 1));
        end = cur + kHuge;
        madvise(cur, kHuge, MADV_HUGEPAGE);
    }
    void *r = cur;
    cur += n;
    return r;
}

// Vector storage: huge pages once a block is 2 MB or more, ordinary aligned memory below that.
template <class T> struct Alloc {
    using value_type = T;
    Alloc() = default;
    template <class U> Alloc(const Alloc<U> &) {}
    T *allocate(size_t n) {
        size_t bytes = n * sizeof(T);
        if (bytes >= kHuge) return static_cast<T *>(alloc(bytes));
        void *p = std::aligned_alloc(alignof(T) > 64 ? alignof(T) : 64, (bytes + 63) / 64 * 64);
        if (!p) throw std::bad_alloc();
        return static_cast<T *>(p);
    }
    void deallocate(T *p, size_t) { std::free(p); }
    template <class U> bool operator==(const Alloc<U> &) const { return true; }
};
}  // namespace hugemem
