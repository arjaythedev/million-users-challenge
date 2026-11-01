// Test helper: gzip stdin to stdout with the project's compressor (used by tests/gzip_check.py).
const std = @import("std");
const Deflater = @import("deflate.zig").Deflater;
const bound = @import("deflate.zig").bound;
const dz = @import("deflate.zig");

pub fn main(init: std.process.Init) !void {
    if (init.environ_map.get("FORCE_ZIG") != null) dz.force_zig = true;
    const bench_n: usize = if (init.environ_map.get("BENCH")) |v| std.fmt.parseInt(usize, v, 10) catch 0 else 0;
    const gpa = std.heap.page_allocator;
    var input = std.ArrayList(u8).empty;
    var buf: [65536]u8 = undefined;
    while (true) {
        const rc = std.os.linux.read(0, &buf, buf.len);
        if (rc == 0) break;
        try input.appendSlice(gpa, buf[0..rc]);
    }
    const out = try gpa.alloc(u8, bound(input.items.len));
    const d = try gpa.create(Deflater);
    d.* = .{};
    if (init.environ_map.get("CORPUS") != null and init.environ_map.get("DUMP") != null) {
        // Verification mode: gzip every record of the corpus with the selected encoder and write [u32 length][gzip]...
        const pstr = init.environ_map.get("CORPUS").?;
        const path = try gpa.allocSentinel(u8, pstr.len, 0);
        @memcpy(path[0..pstr.len], pstr);
        const fd: i32 = @intCast(std.os.linux.open(path, .{ .ACCMODE = .RDONLY }, 0));
        var all = std.ArrayList(u8).empty;
        while (true) {
            const rc = std.os.linux.read(fd, &buf, buf.len);
            if (rc == 0) break;
            try all.appendSlice(gpa, buf[0..rc]);
        }
        const big = try gpa.alloc(u8, bound(1 << 20));
        var res = std.ArrayList(u8).empty;
        var p: usize = 0;
        while (p + 4 <= all.items.len) {
            const len = std.mem.readInt(u32, all.items[p..][0..4], .little);
            const n = d.gzip(big, all.items[p + 4 ..][0..len]);
            var hdr: [4]u8 = undefined;
            std.mem.writeInt(u32, &hdr, @intCast(n), .little);
            try res.appendSlice(gpa, &hdr);
            try res.appendSlice(gpa, big[0..n]);
            p += 4 + len;
        }
        var off: usize = 0;
        while (off < res.items.len) off += std.os.linux.write(1, res.items[off..].ptr, res.items.len - off);
        return;
    }
    if (bench_n > 0 and init.environ_map.get("CORPUS") != null) {
        // Realistic benchmark: a file of [u32 length][body]... records, compressed round-robin, so the branch
        // predictor cannot memorise one input.
        const pstr = init.environ_map.get("CORPUS").?;
        const path = try gpa.allocSentinel(u8, pstr.len, 0);
        @memcpy(path[0..pstr.len], pstr);
        const rc0 = std.os.linux.open(path, .{ .ACCMODE = .RDONLY }, 0);
        const fd: i32 = @intCast(rc0);
        var all = std.ArrayList(u8).empty;
        while (true) {
            const rc = std.os.linux.read(fd, &buf, buf.len);
            if (rc == 0) break;
            try all.appendSlice(gpa, buf[0..rc]);
        }
        var offs = std.ArrayList([]const u8).empty;
        var p: usize = 0;
        while (p + 4 <= all.items.len) {
            const len = std.mem.readInt(u32, all.items[p..][0..4], .little);
            try offs.append(gpa, all.items[p + 4 ..][0..len]);
            p += 4 + len;
        }
        const repeat: usize = if (init.environ_map.get("REPEAT")) |v| (std.fmt.parseInt(usize, v, 10) catch 1) else 1;
        const big = try gpa.alloc(u8, bound(1 << 20));
        var ts: std.os.linux.timespec = undefined;
        _ = std.os.linux.clock_gettime(.MONOTONIC, &ts);
        const t0 = @as(u64, @intCast(ts.sec)) * 1_000_000_000 + @as(u64, @intCast(ts.nsec));
        var out_bytes: usize = 0;
        var in_bytes: usize = 0;
        for (0..bench_n) |k| {
            const item = offs.items[(k / repeat) % offs.items.len];
            out_bytes +%= d.gzip(big, item);
            in_bytes += item.len;
        }
        _ = std.os.linux.clock_gettime(.MONOTONIC, &ts);
        const t1 = @as(u64, @intCast(ts.sec)) * 1_000_000_000 + @as(u64, @intCast(ts.nsec));
        std.debug.print("corpus {d} feeds: {d:.1} -> {d:.1} bytes avg: {d:.2} us per call\n", .{ offs.items.len, @as(f64, @floatFromInt(in_bytes)) / @as(f64, @floatFromInt(bench_n)), @as(f64, @floatFromInt(out_bytes)) / @as(f64, @floatFromInt(bench_n)), @as(f64, @floatFromInt(t1 - t0)) / @as(f64, @floatFromInt(bench_n)) / 1000.0 });
        return;
    }
    if (bench_n > 0) {
        var ts: std.os.linux.timespec = undefined;
        _ = std.os.linux.clock_gettime(.MONOTONIC, &ts);
        const t0 = @as(u64, @intCast(ts.sec)) * 1_000_000_000 + @as(u64, @intCast(ts.nsec));
        var sink: usize = 0;
        for (0..bench_n) |_| sink +%= d.gzip(out, input.items);
        _ = std.os.linux.clock_gettime(.MONOTONIC, &ts);
        const t1 = @as(u64, @intCast(ts.sec)) * 1_000_000_000 + @as(u64, @intCast(ts.nsec));
        std.debug.print("{d} bytes -> {d} bytes: {d:.2} us per call ({d:.0} MB/s)\n", .{ input.items.len, sink / bench_n, @as(f64, @floatFromInt(t1 - t0)) / @as(f64, @floatFromInt(bench_n)) / 1000.0, @as(f64, @floatFromInt(input.items.len * bench_n)) * 1000.0 / @as(f64, @floatFromInt(t1 - t0)) });
        return;
    }
    const n = d.gzip(out, input.items);
    var off: usize = 0;
    while (off < n) off += std.os.linux.write(1, out[off..].ptr, n - off);
}
