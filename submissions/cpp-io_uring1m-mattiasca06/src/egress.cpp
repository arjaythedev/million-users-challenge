#include "egress.h"

#include <linux/tcp.h>
#include <netinet/in.h>
#include <sched.h>
#include <sys/resource.h>
#include <sys/socket.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

namespace egress {

Counters counters[kMaxWorkers];
std::atomic<uint8_t> mode{kNone};

void sample_tcp(Counters &c, int fd) {
    tcp_info ti;
    socklen_t l = sizeof ti;
    if (getsockopt(fd, IPPROTO_TCP, TCP_INFO, &ti, &l) != 0) return;
    constexpr socklen_t kNeed = offsetof(tcp_info, tcpi_notsent_bytes) + sizeof ti.tcpi_notsent_bytes;
    auto bump = [](std::atomic<uint64_t> &a, uint64_t v) { a.store(a.load(std::memory_order_relaxed) + v, std::memory_order_relaxed); };
    bump(c.tcp_n, 1);
    if (l >= kNeed && ti.tcpi_notsent_bytes > 0) bump(c.tcp_backlog, 1);
    if (ti.tcpi_rtt) c.rtt[c.rtt_i++ & 15].store(ti.tcpi_rtt, std::memory_order_relaxed);
}

namespace {

std::string slurp(const std::string &path) {
    std::string s;
    if (FILE *f = fopen(path.c_str(), "r")) {
        char b[4096];
        size_t n;
        while ((n = fread(b, 1, sizeof b, f)) > 0) s.append(b, n);
        fclose(f);
    }
    return s;
}

long long read_ll(const std::string &path) {
    std::string s = slurp(path);
    return s.empty() ? 0 : atoll(s.c_str());
}

// Interface of the default route.
std::string default_iface() {
    std::string t = slurp("/proc/net/route");
    size_t p = t.find('\n');
    while (p != std::string::npos && p + 1 < t.size()) {
        size_t e = t.find('\n', p + 1);
        char name[64] = {};
        unsigned dest = 1, flags = 0;
        if (sscanf(t.c_str() + p + 1, "%63s %x %*x %x", name, &dest, &flags) == 3 && dest == 0 && (flags & 2)) return name;
        p = e;
    }
    return {};
}

struct Tcp {
    uint64_t out_segs = 0, retrans = 0;
};
Tcp read_tcp() {
    Tcp r;
    std::string t = slurp("/proc/net/snmp");
    size_t h = t.find("Tcp:");
    if (h == std::string::npos) return r;
    size_t h2 = t.find("Tcp:", t.find('\n', h) + 1);
    if (h2 == std::string::npos) return r;
    auto fields = [&](size_t at) {
        std::string line = t.substr(at, t.find('\n', at) - at);
        std::vector<std::string> v;
        size_t i = 0;
        while (i < line.size()) {
            size_t j = line.find(' ', i);
            if (j == std::string::npos) j = line.size();
            if (j > i) v.push_back(line.substr(i, j - i));
            i = j + 1;
        }
        return v;
    };
    auto names = fields(h), vals = fields(h2);
    for (size_t i = 1; i < names.size() && i < vals.size(); i++) {
        if (names[i] == "OutSegs") r.out_segs = strtoull(vals[i].c_str(), nullptr, 10);
        else if (names[i] == "RetransSegs") r.retrans = strtoull(vals[i].c_str(), nullptr, 10);
    }
    return r;
}

uint64_t sum(std::atomic<uint64_t> Counters::*m) {
    uint64_t s = 0;
    for (auto &c : counters) s += (c.*m).load(std::memory_order_relaxed);
    return s;
}
uint64_t sum2(std::atomic<uint64_t> (Counters::*m)[3], int k) {
    uint64_t s = 0;
    for (auto &c : counters) s += (c.*m)[k].load(std::memory_order_relaxed);
    return s;
}

double process_cpu_seconds() {
    rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return double(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) + double(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1e-6;
}

const char *name(uint8_t m) { return m == kNone ? "none" : m == kLight ? "light (ISA-L)" : "full (libdeflate)"; }

void monitor() {
    const char *ei = getenv("EGRESS_IFACE");
    const std::string iface = ei && *ei ? ei : default_iface();
    const std::string drops_path = "/sys/class/net/" + iface + "/statistics/tx_dropped";
    cpu_set_t set;
    const double ncpu = sched_getaffinity(0, sizeof set, &set) == 0 ? double(CPU_COUNT(&set)) : double(std::thread::hardware_concurrency());
    fprintf(stderr, "egress: gzip auto (none by default), watching interface '%s', %.0f cpus\n", iface.c_str(), ncpu);

    using clock = std::chrono::steady_clock;
    auto last = clock::now();
    uint64_t dr0 = iface.empty() ? 0 : (uint64_t)read_ll(drops_path);
    Tcp t0 = read_tcp();
    uint64_t n0 = sum(&Counters::eligible_n), tn0 = sum(&Counters::tcp_n), tb0 = sum(&Counters::tcp_backlog);
    uint64_t cn0 = sum(&Counters::cal_n), cg0[3], cs0[3];
    for (int k = 0; k < 3; k++) cg0[k] = sum2(&Counters::cal_gz, k), cs0[k] = sum2(&Counters::cal_ns, k);
    double cpu0 = process_cpu_seconds();

    // Cost model inputs, learned from calibration samples (per body): bytes sent and CPU nanoseconds of each mode.
    double gz[3] = {0, 0, 0}, ns[3] = {0, 0, 0};
    bool calibrated = false;
    double base_rtt = 0;  // lowest recent median RTT (us), creeping up 2% a second
    int up = 0, calm = 0;
    long hold = 30;  // seconds full is kept after going up; doubles on every relapse (up to 10 min)
    clock::time_point hold_until = last, last_up = last;

    for (;;) {
        std::this_thread::sleep_for(std::chrono::seconds(1));
        auto now = clock::now();
        const double dt = std::chrono::duration<double>(now - last).count();
        last = now;

        // --- evidence that the path to the clients is congested ---
        const uint64_t dr = iface.empty() ? 0 : (uint64_t)read_ll(drops_path);
        const Tcp t1 = read_tcp();
        const uint64_t segs = t1.out_segs - t0.out_segs;
        const bool loss = segs > 1000 && double(t1.retrans - t0.retrans) > 0.01 * double(segs);
        const bool dropped = dr > dr0;
        dr0 = dr; t0 = t1;
        const uint64_t tn = sum(&Counters::tcp_n), tb = sum(&Counters::tcp_backlog);
        const bool backlog = tn - tn0 >= 64 && double(tb - tb0) > 0.3 * double(tn - tn0);
        tn0 = tn; tb0 = tb;
        std::vector<uint32_t> rtts;
        for (auto &c : counters)
            for (auto &r : c.rtt)
                if (uint32_t v = r.exchange(0, std::memory_order_relaxed)) rtts.push_back(v);
        double med = 0;
        if (rtts.size() >= 8) {
            std::nth_element(rtts.begin(), rtts.begin() + rtts.size() / 2, rtts.end());
            med = rtts[rtts.size() / 2];
            base_rtt = base_rtt == 0 ? med : std::min(med, base_rtt * 1.02);
        }
        const bool queued = med > 0 && (med > 20000 || (med > 3000 && med > 8 * base_rtt));
        const bool evidence = loss || dropped || backlog || queued;

        // --- cost model ---
        const uint64_t n = sum(&Counters::eligible_n), cn = sum(&Counters::cal_n);
        const double rate = double(n - n0) / dt;  // compressible responses per second
        n0 = n;
        if (cn >= cn0 + 20) {
            for (int k = 0; k < 3; k++) {
                const uint64_t g = sum2(&Counters::cal_gz, k), s = sum2(&Counters::cal_ns, k);
                const double ng = double(g - cg0[k]) / double(cn - cn0), nn = double(s - cs0[k]) / double(cn - cn0);
                gz[k] = calibrated ? 0.7 * gz[k] + 0.3 * ng : ng;
                ns[k] = calibrated ? 0.7 * ns[k] + 0.3 * nn : nn;
                cg0[k] = g; cs0[k] = s;
            }
            calibrated = true;
            cn0 = cn;
        }
        const double cpu = process_cpu_seconds();
        const double spare = ncpu - (cpu - cpu0) / dt;  // idle cores
        cpu0 = cpu;
        // Cost of stepping from mode m up to m+1: body bytes per second not sent, and the cores the dearer mode needs.
        auto saved = [&](int m) { return rate * (gz[m] - gz[m + 1]); };
        auto extra = [&](int m) { return rate * (ns[m + 1] - ns[m]) * 1e-9; };
        auto pays = [&](int m) { return calibrated && saved(m) > 0 && extra(m) <= 0.5 * spare; };  // worth it, and fits in what is idle

        int cur = mode.load(std::memory_order_relaxed);
        if (cur < kFull) {
            up = (evidence && pays(cur)) ? up + 1 : 0;
            if (up >= 2) {
                mode.store(uint8_t(cur + 1), std::memory_order_relaxed);
                if (now - last_up > std::chrono::seconds(600)) hold = 30;
                hold_until = now + std::chrono::seconds(hold);
                hold = std::min(hold * 2, 600L);
                last_up = now; up = 0; calm = 0;
                fprintf(stderr, "egress: gzip %s: %s%s%s%s; saves %.1f MB/s for %.2f cores (%.1f idle)\n", name(uint8_t(cur + 1)),
                        loss ? "retransmits " : "", dropped ? "drops " : "", backlog ? "unsent backlog " : "", queued ? "rtt inflated" : "",
                        saved(cur) / 1e6, extra(cur), spare);
                continue;
            }
        }
        if (cur > kNone) {
            calm = evidence ? 0 : calm + 1;
            const bool unaffordable = calibrated && extra(cur - 1) > spare;  // CPU is the bottleneck now: take the cheaper mode back
            if (unaffordable || (calm >= 10 && now >= hold_until)) {
                mode.store(uint8_t(cur - 1), std::memory_order_relaxed);
                calm = 0;
                fprintf(stderr, "egress: gzip %s: %s\n", name(uint8_t(cur - 1)), unaffordable ? "no idle cpu for the dearer mode" : "congestion gone");
            }
        }
    }
}

}  // namespace

void start() {
    const char *m = getenv("GZIP_MODE");
    if (m && *m && strcmp(m, "auto")) {
        mode.store(!strcmp(m, "full") ? kFull : !strcmp(m, "light") ? kLight : kNone);
        fprintf(stderr, "egress: gzip pinned to %s\n", name(mode.load()));
        return;
    }
    std::thread(monitor).detach();
}

}  // namespace egress
