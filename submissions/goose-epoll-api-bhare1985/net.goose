// The process and compression, through the C shim (shim.c). The sockets are in ring.goose.
namespace net;

extern fn cpus() -> i32 = "shim_cpus";
extern fn init() = "shim_init";

// ISA-L igzip at level 0 into out[..cap]. Returns the gzip size, or 0 if it did not fit.
extern fn gzip(out: u8&, cap: i64, src: const u8[:]) -> i64 = "shim_gzip";
