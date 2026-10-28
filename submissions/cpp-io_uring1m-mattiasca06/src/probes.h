// Build-time telemetry (-DPROBES): per-span hardware counters read in user space with rdpmc, so the cost of each stage of the
// event loop (cycles, instructions, branch misses, L1d misses, page walks) is attributed without sampling. Not compiled in
// normal builds. Counters count user mode only and are per worker thread.
#pragma once
#ifdef PROBES
#include <linux/perf_event.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <x86intrin.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

namespace probes {
constexpr int kN = 5;  // cycles, instructions, branch mispredicts, L1d load misses, page walks (L2 TLB miss)
struct Snap {
    uint64_t t, v[kN];
};

struct Pmu {
    int fd[kN] = {-1, -1, -1, -1, -1};
    perf_event_mmap_page *pg[kN] = {};
    bool ok = false;
    void init() {
        static const struct {
            uint32_t type;
            uint64_t cfg;
        } ev[kN] = {{PERF_TYPE_HARDWARE, PERF_COUNT_HW_CPU_CYCLES},
                    {PERF_TYPE_HARDWARE, PERF_COUNT_HW_INSTRUCTIONS},
                    {PERF_TYPE_RAW, 0x00C3},
                    {PERF_TYPE_HW_CACHE, PERF_COUNT_HW_CACHE_L1D | (PERF_COUNT_HW_CACHE_OP_READ << 8) | (PERF_COUNT_HW_CACHE_RESULT_MISS << 16)},
                    {PERF_TYPE_RAW, 0x1045}};
        ok = true;
        for (int i = 0; i < kN && ok; i++) {
            perf_event_attr a{};
            a.size = sizeof a;
            a.type = ev[i].type;
            a.config = ev[i].cfg;
            a.exclude_kernel = 1;
            a.exclude_hv = 1;
            fd[i] = int(syscall(SYS_perf_event_open, &a, 0, -1, -1, 0));
            if (fd[i] < 0) {
                ok = false;
                break;
            }
            void *m = mmap(nullptr, size_t(sysconf(_SC_PAGESIZE)), PROT_READ, MAP_SHARED, fd[i], 0);
            if (m == MAP_FAILED) {
                ok = false;
                break;
            }
            pg[i] = static_cast<perf_event_mmap_page *>(m);
            if (!pg[i]->cap_user_rdpmc) ok = false;
        }
        if (!ok) fprintf(stderr, "probes: hardware counters unavailable, timing only\n");
    }
    static inline uint64_t rd(const perf_event_mmap_page *p) {
        uint32_t seq, idx;
        int64_t pmc = 0, off;
        do {
            seq = p->lock;
            asm volatile("" ::: "memory");
            idx = p->index;
            off = p->offset;
            if (idx) {
                unsigned w = p->pmc_width;
                pmc = int64_t(__builtin_ia32_rdpmc(int(idx) - 1) << (64 - w)) >> (64 - w);
            }
            asm volatile("" ::: "memory");
        } while (p->lock != seq);
        return uint64_t(off + pmc);
    }
    inline void snap(Snap &s) const {
        s.t = __rdtsc();
        if (ok)
            for (int i = 0; i < kN; i++) s.v[i] = rd(pg[i]);
    }
};

struct Stat {
    uint64_t calls = 0, t = 0, v[kN] = {};
};

struct Span {
    const Pmu &pmu;
    Stat &st;
    Snap a;
    Span(const Pmu &p, Stat &s) : pmu(p), st(s) { pmu.snap(a); }
    ~Span() {
        Snap b;
        pmu.snap(b);
        st.calls++;
        st.t += b.t - a.t;
        if (pmu.ok)
            for (int i = 0; i < kN; i++) st.v[i] += b.v[i] - a.v[i];
    }
};

enum { kOps = 5 };
struct Probes {
    Pmu pmu;
    Stat pass, wait, prefetch, rearm, onpg, pg_parse, pg_assemble, pg_resume, pg_dispatch, op[kOps];
    uint64_t pg_rows = 0, pg_bytes = 0, pg_members = 0, pg_segs = 0, cqes = 0, hist[16] = {};
};

inline void line(const char *name, const Stat &s, const Stat &p, double req) {
    const uint64_t dc = s.calls - p.calls;
    if (!dc) return;
    double d[kN];
    for (int i = 0; i < kN; i++) d[i] = double(s.v[i] - p.v[i]);
    const double cyc = d[0] ? d[0] : 1;
    printf("  %-16s calls/req %6.3f | cyc/req %7.0f  ins/req %7.0f  IPC %4.2f | brmiss/req %6.2f  L1d/req %6.1f  walks/req %5.2f | cyc/call %8.0f\n", name,
           double(dc) / req, d[0] / req, d[1] / req, d[1] / cyc, d[2] / req, d[3] / req, d[4] / req, d[0] / double(dc));
}
}  // namespace probes
#endif
