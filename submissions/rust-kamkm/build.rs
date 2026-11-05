// Links the ISA-L static library built by build.sh into deps/isa-l-2.31.1/bin. On a machine
// where build.sh has not run (e.g. a plain `cargo check` of the sources) the library simply is
// not there and this prints nothing: check still works, only a real link would fail.
use std::path::Path;

fn main() {
    let dir = Path::new(&std::env::var("CARGO_MANIFEST_DIR").unwrap()).join("deps/isa-l-2.31.1/bin");
    println!("cargo:rerun-if-changed=deps/isa-l-2.31.1/bin/libisa-l.a");
    if dir.join("libisa-l.a").exists() {
        println!("cargo:rustc-link-search=native={}", dir.display());
        println!("cargo:rustc-link-lib=static=isa-l");
    }
}
