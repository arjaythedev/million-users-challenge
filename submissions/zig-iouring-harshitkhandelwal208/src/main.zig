//! Feed API server (Million Users Challenge): Zig, io_uring with direct descriptors, hand-written HTTP/1.1,
//! PostgreSQL wire protocol and gzip.
const std = @import("std");
const linux = std.os.linux;
const server = @import("server.zig");
const pg = @import("pg.zig");
const jwt = @import("jwt.zig");
const sys = @import("sys.zig");

pub const std_options: std.Options = .{ .log_level = .info };

fn envInt(init: std.process.Init, name: []const u8, default: u32) u32 {
    const v = init.environ_map.get(name) orelse return default;
    return std.fmt.parseInt(u32, v, 10) catch default;
}

fn parseHost(s: []const u8) ![4]u8 {
    var host: [4]u8 = undefined;
    var it = std.mem.splitScalar(u8, s, '.');
    var i: usize = 0;
    while (it.next()) |p| : (i += 1) {
        if (i >= 4) return error.BadHost;
        host[i] = try std.fmt.parseInt(u8, p, 10);
    }
    if (i != 4) return error.BadHost;
    return host;
}

pub fn main(init: std.process.Init) !void {
    const gpa = @import("buf.zig").gpa;
    const url = init.environ_map.get("DATABASE_URL") orelse return error.MissingDatabaseUrl;
    const secret = init.environ_map.get("JWT_SECRET") orelse return error.MissingJwtSecret;
    const host = try parseHost(init.environ_map.get("HOST") orelse "0.0.0.0");
    const port: u16 = @intCast(envInt(init, "PORT", 80));
    const extra: u16 = @intCast(envInt(init, "EXTRA_PORT", 81));

    // A write to a connection the peer has closed must fail with EPIPE, not kill the process (the Postgres handshake
    // uses plain writes; everything else already passes MSG_NOSIGNAL).
    const ignore: linux.Sigaction = .{ .handler = .{ .handler = linux.SIG.IGN }, .mask = linux.sigemptyset(), .flags = 0 };
    _ = linux.sigaction(.PIPE, &ignore, null);

    // The process needs the whole descriptor allowance for the direct-descriptor tables.
    var rl: linux.rlimit = undefined;
    _ = linux.getrlimit(.NOFILE, &rl);
    rl.cur = rl.max;
    _ = linux.setrlimit(.NOFILE, &rl);

    const url_scratch = try gpa.alloc(u8, 1024);
    var db = try pg.parseUrl(url, url_scratch);
    // Resolve a hostname once; the database may still be starting, so keep trying for most of the 60 s allowance.
    var tries: u32 = 0;
    while (true) : (tries += 1) {
        if (pg.resolve(db)) |a| {
            db.addr = a;
            break;
        } else |e| {
            if (tries >= 50) {
                std.log.err("cannot resolve the database host '{s}': {s}", .{ db.host, @errorName(e) });
                return e;
            }
            sys.sleepMs(1000);
        }
    }

    // Two workers on any number of cores: fewer, busier workers batch more completions per system call (measured on four
    // logical CPUs: 15 us of CPU per request with two workers, 17-19 with four; one worker was cheaper still but caps a
    // process at 2^20 direct descriptors). At the benchmark load each worker is far from saturation, and the remaining
    // cores are left to the kernel's network processing.
    const workers = @max(1, envInt(init, "WORKERS", 2));
    // Postgres connections (at most 512 in total): reads are pipelined over a few connections per worker, writes
    // go to a few more where they are committed in groups.
    const read_total = envInt(init, "DB_READ_CONNS", 64);
    const write_total = envInt(init, "DB_WRITE_CONNS", 16);
    const pg_read = @max(1, (read_total + workers - 1) / workers);
    const pg_write = @max(1, (write_total + workers - 1) / workers);
    if ((pg_read + pg_write) * workers > 500) return error.TooManyDbConnections;
    // Direct descriptors per ring: room for the whole load with some slack for an uneven spread across workers.
    const default_slots: u32 = @intCast(@min(@as(u64, 1 << 20), @max(@as(u64, 4096), (@as(u64, 2_100_000) + workers - 1) / workers)));
    const cfg = server.Config{
        .host = host,
        .ports = .{ port, extra },
        .db = db,
        .workers = workers,
        .pg_read = pg_read,
        .pg_write = pg_write,
        .pg_depth = std.math.clamp(envInt(init, "PG_DEPTH", 32), 1, 100),
        .pg_pack = std.math.clamp(envInt(init, "PG_PACK", 32), 1, 100),
        .slots = std.math.clamp(@min(envInt(init, "SLOTS", default_slots), @as(u32, @intCast(@min(rl.cur, 1 << 20)))), 1024, 1 << 20),
        .coalesce_us = envInt(init, "COALESCE_US", 1000),
        .coalesce_min = @max(1, envInt(init, "COALESCE_MIN", 32)),
        .stats = init.environ_map.get("STATS") != null,
        .test_fail_groups = init.environ_map.get("TEST_FAIL_GROUPS") != null,
        .test_fail_packs = init.environ_map.get("TEST_FAIL_PACKS") != null,
    };
    std.log.info("{d} workers, {d}+{d} postgres connections each, {d} descriptors each, ports {d} and {d}", .{ workers, pg_read, pg_write, cfg.slots, port, extra });

    const shared = try gpa.create(server.Shared);
    shared.* = .{ .cfg = cfg, .jwt = jwt.Key.init(secret), .start_ns = sys.nowNs() };
    var i: usize = 1;
    while (i < workers) : (i += 1) {
        const t = try std.Thread.spawn(.{ .stack_size = 1 << 20 }, server.workerMain, .{ shared, i });
        t.detach();
    }
    server.workerMain(shared, 0);
}
