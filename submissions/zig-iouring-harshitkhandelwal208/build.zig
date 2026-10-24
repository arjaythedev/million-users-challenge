const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});
    const strip = b.option(bool, "strip", "Strip debug info (default: yes for release builds)") orelse (optimize != .debug);

    const exe = b.addExecutable(.{
        .name = "feed",
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/main.zig"),
            .target = target,
            .optimize = optimize,
            .single_threaded = false,
            .strip = strip,
        }),
    });
    b.installArtifact(exe);

    // Unit tests run optimized (with safety checks): the CRC kernel's inline assembly is meant for optimized builds.
    const tests = b.addTest(.{ .root_module = b.createModule(.{
        .root_source_file = b.path("src/all_tests.zig"),
        .target = target,
        .optimize = if (optimize == .debug) .safe else optimize,
    }) });
    const test_step = b.step("test", "Run unit tests");
    test_step.dependOn(&b.addRunArtifact(tests).step);

    // Standalone gzip tool for tests/gzip_check.py and benchmarking the encoder.
    const tool = b.addExecutable(.{ .name = "deflate-tool", .root_module = b.createModule(.{
        .root_source_file = b.path("src/deflate_tool.zig"),
        .target = target,
        .optimize = if (optimize == .debug) .safe else optimize,
    }) });
    b.step("deflate-tool", "Build the gzip test tool").dependOn(&b.addInstallArtifact(tool, .{}).step);
}
