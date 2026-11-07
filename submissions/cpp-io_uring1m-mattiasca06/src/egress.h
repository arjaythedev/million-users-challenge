// Chooses how responses are compressed from what the network is doing:
//   none  - uncompressed (the default: no CPU spent while the link has room)
//   light - ISA-L igzip level 0 (cheapest CPU per byte saved)
//   full  - libdeflate level 1 (about 15% smaller bodies than light for about twice the CPU per body)
// Each step up needs hard evidence that the path to the clients is congested (retransmits, local drops, TCP
// round-trip times far above their baseline, data piling up unsent in socket buffers) and a cost model, fed by timing
// both engines on one body in 256 on this CPU and these payloads, that says the extra CPU fits in the idle cores.
// Workers only bump counters of their own and read one atomic; a monitor thread does the rest once a second.
#pragma once

#include <atomic>
#include <cstdint>

namespace egress {

enum Mode : uint8_t { kNone = 0, kLight = 1, kFull = 2 };

// Written by one worker only (relaxed load + store, no atomic read-modify-write on the hot path), read by the monitor.
struct alignas(64) Counters {
    std::atomic<uint64_t> eligible_n{0}, eligible_raw{0};  // responses that could be compressed, and their body bytes
    std::atomic<uint64_t> cal_n{0};                        // calibration samples: bodies compressed by both engines
    std::atomic<uint64_t> cal_gz[3]{}, cal_ns[3]{};        //   bytes and nanoseconds per mode (none: the body size, 0 ns)
    std::atomic<uint64_t> tcp_n{0}, tcp_backlog{0};        // TCP_INFO samples; samples with unsent bytes
    std::atomic<uint32_t> rtt[16]{};                       // smoothed RTT (us) of sampled connections; the monitor drains them
    uint32_t tick = 0, rtt_i = 0;                          // owner only
};

constexpr int kMaxWorkers = 256;
extern Counters counters[kMaxWorkers];
extern std::atomic<uint8_t> mode;

void sample_tcp(Counters &c, int fd);

// GZIP_MODE=auto (default, starts and rests at light, the floor; GZIP_FLOOR=none|light|full) | none | light | full pins the mode. EGRESS_IFACE overrides the default-route interface used for the
// drop counter.
void start();

}  // namespace egress
