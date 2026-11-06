//! CPU load measurement for the adaptive gzip level.
//!
//! Busy time comes from two sources, and the higher one wins:
//! - /proc/stat for the CPUs this process may run on. This counts the kernel's
//!   network processing (softirq) on those CPUs, which competes with the server.
//! - The cgroup v2 CPU quota, when one is set. A quota throttles the process
//!   before its CPUs look busy, so usage is compared with the quota instead.
const std = @import("std");
const linux = std.os.linux;

pub const Ticks = struct {
    busy: u64 = 0,
    total: u64 = 0,
};

/// Sums busy and total ticks of the `cpuN` lines in /proc/stat whose N is in
/// `allowed`, or of the aggregate `cpu` line when `allowed` is null.
pub fn parseProcStat(text: []const u8, allowed: ?*const linux.cpu_set_t) Ticks {
    var sum = Ticks{};
    var lines = std.mem.splitScalar(u8, text, '\n');
    while (lines.next()) |line| {
        if (!std.mem.startsWith(u8, line, "cpu")) break;
        var fields = std.mem.tokenizeScalar(u8, line, ' ');
        const name = fields.next() orelse continue;
        if (allowed) |set| {
            if (name.len == 3) continue;
            const cpu = std.fmt.parseInt(usize, name[3..], 10) catch continue;
            if (!cpuIsSet(set, cpu)) continue;
        } else if (name.len != 3) continue;
        // user nice system idle iowait irq softirq steal; guest time is
        // already included in user and nice.
        var values: [8]u64 = .{0} ** 8;
        for (&values) |*value| {
            const field = fields.next() orelse break;
            value.* = std.fmt.parseInt(u64, field, 10) catch 0;
        }
        var total: u64 = 0;
        for (values) |value| total += value;
        sum.total += total;
        sum.busy += total - values[3] - values[4];
    }
    return sum;
}

fn cpuIsSet(set: *const linux.cpu_set_t, cpu: usize) bool {
    const bits = @bitSizeOf(usize);
    if (cpu / bits >= set.len) return false;
    return (set[cpu / bits] >> @intCast(cpu % bits)) & 1 == 1;
}

/// The CPU count a cgroup v2 `cpu.max` file allows, or null for "max".
pub fn parseCpuMax(text: []const u8) ?f64 {
    var fields = std.mem.tokenizeAny(u8, text, " \n");
    const quota = fields.next() orelse return null;
    if (std.mem.eql(u8, quota, "max")) return null;
    const period = fields.next() orelse return null;
    const q = std.fmt.parseInt(u64, quota, 10) catch return null;
    const p = std.fmt.parseInt(u64, period, 10) catch return null;
    if (q == 0 or p == 0) return null;
    return @as(f64, @floatFromInt(q)) / @as(f64, @floatFromInt(p));
}

/// `usage_usec` from a cgroup v2 `cpu.stat` file.
pub fn parseUsageUsec(text: []const u8) ?u64 {
    var lines = std.mem.splitScalar(u8, text, '\n');
    while (lines.next()) |line| {
        if (std.mem.startsWith(u8, line, "usage_usec ")) {
            return std.fmt.parseInt(u64, std.mem.trim(u8, line["usage_usec ".len..], " "), 10) catch null;
        }
    }
    return null;
}

/// The unified hierarchy path from /proc/self/cgroup ("0::/path").
pub fn parseCgroupPath(text: []const u8) ?[]const u8 {
    var lines = std.mem.splitScalar(u8, text, '\n');
    while (lines.next()) |line| {
        if (std.mem.startsWith(u8, line, "0::/")) return line[3..];
    }
    return null;
}

pub const Meter = struct {
    allowed: ?linux.cpu_set_t = null,
    cgroup_dir: [std.fs.max_path_bytes]u8 = undefined,
    cgroup_len: usize = 0,
    last_ticks: Ticks = .{},
    last_usage_us: ?u64 = null,
    last_wall_us: i64 = 0,
    buffer: [32 * 1024]u8 = undefined,

    pub fn init() Meter {
        var meter = Meter{};
        meter.allowed = std.posix.sched_getaffinity(0) catch null;
        if (read(&meter.buffer, "/proc/self/cgroup")) |text| {
            if (parseCgroupPath(text)) |path| {
                const dir = std.fmt.bufPrint(&meter.cgroup_dir, "/sys/fs/cgroup{s}", .{std.mem.trimRight(u8, path, "/")}) catch "";
                meter.cgroup_len = dir.len;
            }
        }
        _ = meter.busyPercent();
        return meter;
    }

    /// Busy percentage since the previous call, or null on the first call or
    /// when nothing could be read.
    pub fn busyPercent(self: *Meter) ?u32 {
        var best: ?f64 = null;
        if (read(&self.buffer, "/proc/stat")) |text| {
            const ticks = parseProcStat(text, if (self.allowed) |*set| set else null);
            if (self.last_ticks.total != 0 and ticks.total > self.last_ticks.total and ticks.busy >= self.last_ticks.busy) {
                best = @as(f64, @floatFromInt(ticks.busy - self.last_ticks.busy)) * 100 /
                    @as(f64, @floatFromInt(ticks.total - self.last_ticks.total));
            }
            self.last_ticks = ticks;
        }
        if (self.cgroupPercent()) |percent| {
            if (best == null or percent > best.?) best = percent;
        }
        const value = best orelse return null;
        return @intFromFloat(@min(@max(value, 0), 100));
    }

    fn cgroupPercent(self: *Meter) ?f64 {
        if (self.cgroup_len == 0) return null;
        const now_us = std.time.microTimestamp();
        var path_buffer: [std.fs.max_path_bytes]u8 = undefined;
        const dir = self.cgroup_dir[0..self.cgroup_len];
        const stat_path = std.fmt.bufPrint(&path_buffer, "{s}/cpu.stat", .{dir}) catch return null;
        const usage = parseUsageUsec(read(&self.buffer, stat_path) orelse return null) orelse return null;
        const previous_usage = self.last_usage_us;
        const previous_wall = self.last_wall_us;
        self.last_usage_us = usage;
        self.last_wall_us = now_us;
        // Read the quota every time so a changed limit takes effect.
        const max_path = std.fmt.bufPrint(&path_buffer, "{s}/cpu.max", .{dir}) catch return null;
        const cpus = parseCpuMax(read(&self.buffer, max_path) orelse return null) orelse return null;
        const before = previous_usage orelse return null;
        if (usage < before or now_us <= previous_wall) return null;
        const wall: f64 = @floatFromInt(now_us - previous_wall);
        return @as(f64, @floatFromInt(usage - before)) * 100 / (wall * cpus);
    }
};

fn read(buffer: []u8, path: []const u8) ?[]const u8 {
    const file = std.fs.openFileAbsolute(path, .{}) catch return null;
    defer file.close();
    const n = file.readAll(buffer) catch return null;
    return buffer[0..n];
}

/// Steps the gzip level down when the CPUs stay busy and back up when they
/// have room. The gap between the thresholds must exceed the CPU that one
/// level adds, or the level would flap.
pub const Governor = struct {
    max_level: u32,
    high: u32,
    low: u32,
    level: u32,
    high_samples: u32 = 0,
    low_samples: u32 = 0,

    /// Consecutive one-second samples needed before a change.
    pub const down_after = 2;
    pub const up_after = 10;

    /// Returns the new level when it changes.
    pub fn observe(self: *Governor, busy: u32) ?u32 {
        if (busy >= self.high) {
            self.high_samples += 1;
            self.low_samples = 0;
        } else if (busy <= self.low) {
            self.low_samples += 1;
            self.high_samples = 0;
        } else {
            self.high_samples = 0;
            self.low_samples = 0;
        }
        if (self.high_samples >= down_after) {
            self.high_samples = 0;
            if (self.level > 0) {
                self.level -= 1;
                return self.level;
            }
        } else if (self.low_samples >= up_after) {
            self.low_samples = 0;
            if (self.level < self.max_level) {
                self.level += 1;
                return self.level;
            }
        }
        return null;
    }
};

test "proc stat sums allowed CPUs" {
    const text =
        \\cpu  1000 0 500 8000 100 0 400 0 0 0
        \\cpu0 300 0 100 1000 50 0 100 0 0 0
        \\cpu1 200 0 100 3000 50 0 100 0 0 0
        \\cpu2 500 0 300 4000 0 0 200 0 0 0
        \\intr 12345 1 2 3
        \\cpu9 999 999 999 999 999 999 999 999 0 0
    ;
    const all = parseProcStat(text, null);
    try std.testing.expectEqual(@as(u64, 10000), all.total);
    try std.testing.expectEqual(@as(u64, 1900), all.busy);
    var set: linux.cpu_set_t = std.mem.zeroes(linux.cpu_set_t);
    set[0] = 0b101;
    const some = parseProcStat(text, &set);
    try std.testing.expectEqual(@as(u64, 1550 + 5000), some.total);
    try std.testing.expectEqual(@as(u64, 500 + 1000), some.busy);
}

test "cgroup files" {
    try std.testing.expectEqual(@as(?f64, null), parseCpuMax("max 100000\n"));
    try std.testing.expectEqual(@as(?f64, 0.5), parseCpuMax("50000 100000\n"));
    try std.testing.expectEqual(@as(?f64, 2.0), parseCpuMax("200000 100000"));
    try std.testing.expectEqual(@as(?u64, 123456), parseUsageUsec("usage_usec 123456\nuser_usec 1\n"));
    try std.testing.expectEqual(@as(?u64, null), parseUsageUsec("user_usec 1\n"));
    try std.testing.expectEqualStrings("/system.slice/run-r1.scope", parseCgroupPath("0::/system.slice/run-r1.scope\n").?);
    try std.testing.expectEqualStrings("/", parseCgroupPath("0::/\n").?);
    try std.testing.expectEqual(@as(?[]const u8, null), parseCgroupPath("1:name=systemd:/x\n"));
}

test "governor steps down fast, up slowly, within bounds" {
    var g = Governor{ .max_level = 1, .high = 80, .low = 60, .level = 1 };
    try std.testing.expectEqual(@as(?u32, null), g.observe(85));
    try std.testing.expectEqual(@as(?u32, 0), g.observe(90));
    try std.testing.expectEqual(@as(?u32, null), g.observe(95));
    try std.testing.expectEqual(@as(?u32, null), g.observe(95));
    // A sample between the thresholds resets the count.
    for (0..9) |_| try std.testing.expectEqual(@as(?u32, null), g.observe(50));
    try std.testing.expectEqual(@as(?u32, null), g.observe(70));
    for (0..9) |_| try std.testing.expectEqual(@as(?u32, null), g.observe(50));
    try std.testing.expectEqual(@as(?u32, 1), g.observe(50));
    for (0..20) |_| try std.testing.expectEqual(@as(?u32, null), g.observe(10));
    try std.testing.expectEqual(@as(u32, 1), g.level);
}
