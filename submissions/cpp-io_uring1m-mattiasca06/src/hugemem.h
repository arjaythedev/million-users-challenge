// Memory on transparent huge pages for the structures every request touches (coroutine frames, the connection table): on
// 4 KiB pages they cost about 15 page walks a request.
#pragma once
#include <sys/mman.h>

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
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


// The allocator's own arenas are big anonymous mappings that never ask for huge pages (mimalloc's option only tries
// hugetlbfs): advise every such mapping once, so the heap's pages (results, per-connection buffers) get 2 MB pages as they
// are first touched. Mappings already advised are remembered; call again later to pick up arenas created since.
inline void advise_heap_arenas() {
    static uintptr_t done[64];
    static int ndone = 0;
    FILE *f = fopen("/proc/self/maps", "r");
    if (!f) return;
    char line[512];
    while (fgets(line, sizeof line, f)) {
        unsigned long lo, hi;
        char perm[8];
        int off = 0;
        if (sscanf(line, "%lx-%lx %7s %*s %*s %*s %n", &lo, &hi, perm, &off) < 3) continue;
        if (strncmp(perm, "rw-p", 4) != 0 || hi - lo < (64ul << 20)) continue;
        if (off > 0 && line[off] != 10 && line[off] != 0) continue;  // file-backed or named ([heap], [stack])
        bool seen = false;
        for (int i = 0; i < ndone; i++) seen |= done[i] == lo;
        if (seen || ndone == 64) continue;
        done[ndone++] = lo;
        madvise(reinterpret_cast<void *>((lo + kHuge - 1) & ~(kHuge - 1)), (hi - lo) & ~(kHuge - 1), MADV_HUGEPAGE);
    }
    fclose(f);
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
