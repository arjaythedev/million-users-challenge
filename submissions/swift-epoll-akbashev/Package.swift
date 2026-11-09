// swift-tools-version:6.2
import PackageDescription

// The host is the machine the server runs on (build.sh builds on the box), so its architecture picks the flags; on
// others SHAExtensions is empty.
#if arch(x86_64)
let shaFlags = ["-enable-builtin-module", "-Xcc", "-msha", "-Xcc", "-mssse3", "-Xcc", "-msse4.1"]
let crcFlags = ["-enable-builtin-module", "-Xcc", "-mpclmul", "-Xcc", "-msse4.1"]
let crc512Flags = ["-enable-builtin-module", "-Xcc", "-mavx512f", "-Xcc", "-mvpclmulqdq", "-Xcc", "-mpclmul"]
#else
let shaFlags: [String] = []
let crcFlags: [String] = []
let crc512Flags: [String] = []
#endif

let package = Package(
    name: "feed",
    // Linux only (epoll, eventfd, accept4, unshare). No dependencies: Swift's standard library, Synchronization and
    // Glibc.
    targets: [
        // SHA-256 on the x86 SHA extensions, reached through Swift's `Builtin` module (-enable-builtin-module), with
        // the instructions enabled for this module alone; Feed calls it only on a CPU that has them.
        .target(
            name: "SHAExtensions",
            path: "Sources/SHAExtensions",
            swiftSettings: [.unsafeFlags(shaFlags), .unsafeFlags(["-Ounchecked"], .when(configuration: .release))]
        ),
        // The Linux calls Glibc's module leaves out, declared in Swift with `@_extern(c)`.
        .target(
            name: "Syscalls",
            path: "Sources/Syscalls",
            swiftSettings: [.enableExperimentalFeature("Extern")]
        ),
        // CRC-32 on the x86 carry-less multiply, the same way, for gzip; Gzip calls it only on a CPU that has it.
        .target(
            name: "CRCExtensions",
            path: "Sources/CRCExtensions",
            swiftSettings: [.unsafeFlags(crcFlags), .unsafeFlags(["-Ounchecked"], .when(configuration: .release))]
        ),
        // The same with AVX-512's VPCLMULQDQ: a module of its own, as the compiler may use AVX-512 anywhere in it.
        .target(
            name: "CRCExtensions512",
            path: "Sources/CRCExtensions512",
            swiftSettings: [.unsafeFlags(crc512Flags), .unsafeFlags(["-Ounchecked"], .when(configuration: .release))]
        ),
        // gzip in Swift, for the responses of clients that accept it.
        .target(
            name: "Gzip",
            dependencies: ["Syscalls", "CRCExtensions", "CRCExtensions512"],
            path: "Sources/Gzip",
            swiftSettings: [
                .unsafeFlags(["-Ounchecked", "-enforce-exclusivity=unchecked"], .when(configuration: .release)),
            ]
        ),
        .executableTarget(
            name: "Feed",
            dependencies: ["Syscalls", "Gzip", "SHAExtensions"],
            path: "Sources/Feed",
            swiftSettings: [
                // Nonisolated by default: the server is one actor per core (the reactors), each its own isolation
                // domain; nothing belongs to the main actor.
                .defaultIsolation(nil),
                .enableUpcomingFeature("NonisolatedNonsendingByDefault"),
                .enableUpcomingFeature("InferIsolatedConformances"),
                .enableUpcomingFeature("ExistentialAny"),
                .enableUpcomingFeature("MemberImportVisibility"),
                // `@_lifetime` on the ~Escapable views (an HTTP request, the JSON parser) and on the Spans the
                // noncopyable buffers lend out.
                .enableExperimentalFeature("Lifetimes"),
                .unsafeFlags(["-Ounchecked", "-enforce-exclusivity=unchecked"], .when(configuration: .release)),
            ]
        ),
    ]
)
