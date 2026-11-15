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

// The executable's code on one 2 MB page: its hot path spans ~80 4 KiB pages, more than the instruction TLB holds, so a
// third of the page walks are instruction fetches. Needs the text segment to start on a 2 MB boundary and nothing mapped in the
// rest of that 2 MB (build.sh links with -z max-page-size=2M); otherwise it does nothing. The code is copied into an
// anonymous huge page that is moved over the original mapping at the same address, so a thread that is running in it does not
// notice. Call before any other thread starts. HT=0 skips it, HT=v reports what happened.
inline void hugify_text() {
    const char *e = getenv("HT");
    if (e && e[0] == '0') return;
    const bool verbose = e && e[0] == 'v';
    const uintptr_t self = reinterpret_cast<uintptr_t>(&hugify_text);
    uintptr_t lo = 0, hi = 0, next = ~uintptr_t(0);
    if (FILE *f = fopen("/proc/self/maps", "r")) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            unsigned long l, h;
            if (sscanf(line, "%lx-%lx", &l, &h) != 2) continue;
            if (hi) {
                next = l;
                break;
            }
            if (self >= l && self < h) lo = l, hi = h;
        }
        fclose(f);
    }
    if (!hi || (lo & (kHuge - 1)) || hi - lo > kHuge || next < lo + kHuge) {
        if (verbose) fprintf(stderr, "hugify_text: text %lx-%lx next %lx does not fit one 2 MB window, skipped\n", lo, hi, next);
        return;
    }
    void *t = mmap(nullptr, 2 * kHuge, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (t == MAP_FAILED) return;
    char *a = reinterpret_cast<char *>((reinterpret_cast<uintptr_t>(t) + kHuge - 1) & ~(kHuge - 1));
    // Every step before the move can fail harmlessly (no huge pages, executable anonymous memory refused): the original
    // mapping is untouched until mremap succeeds, and then the server simply runs on 4 KiB code pages as before.
    if (madvise(a, kHuge, MADV_HUGEPAGE) != 0) {
        munmap(t, 2 * kHuge);
        return;
    }
    memcpy(a, reinterpret_cast<void *>(lo), hi - lo);
    if (mprotect(a, kHuge, PROT_READ | PROT_EXEC) != 0) {
        munmap(t, 2 * kHuge);
        return;
    }
    void *r = mremap(a, kHuge, kHuge, MREMAP_MAYMOVE | MREMAP_FIXED, reinterpret_cast<void *>(lo));
    if (r == MAP_FAILED) {
        if (verbose) perror("hugify_text: mremap");
        munmap(t, 2 * kHuge);
        return;
    }
    if (a > static_cast<char *>(t)) munmap(t, a - static_cast<char *>(t));
    char *tail = static_cast<char *>(t) + 2 * kHuge;
    if (tail > a + kHuge) munmap(a + kHuge, tail - (a + kHuge));
    if (verbose) fprintf(stderr, "hugify_text: %lx-%lx now on a 2 MB page\n", lo, hi);
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
