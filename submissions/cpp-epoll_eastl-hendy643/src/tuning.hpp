#pragma once

#include <cstddef>

namespace feed::tuning
{
inline constexpr size_t kThreadsPerCore = 1;
inline constexpr size_t kMaxThreads = 512;

inline constexpr size_t kDbConnections = 128;

inline constexpr size_t kPipelineDepth = 32;

// Sent in the startup message of our own connections; jit and enable_memoize match box/postgresql.conf.
struct Setting
{
    const char *name;
    const char *value;
};

inline constexpr Setting kSessionSettings[] = {
    {"client_encoding", "UTF8"},
    {"jit", "off"},
    {"enable_memoize", "off"},
    {"plan_cache_mode", "force_generic_plan"},
    {"max_parallel_workers_per_gather", "0"},
};

inline constexpr int kDbRetrySeconds = 1;

inline constexpr size_t kMinGzipBody = 512;
inline constexpr int kGzipLevel = 1;

inline constexpr size_t kMaxQueuedInput = 65536;

inline constexpr unsigned kUringEntries = 4096;
inline constexpr unsigned kUringCompletions = 32768;
inline constexpr unsigned kRecvBuffers = 1024;
inline constexpr unsigned kRecvBufferSize = 4096;
inline constexpr int kListenBacklog = 65535;
} // namespace feed::tuning
