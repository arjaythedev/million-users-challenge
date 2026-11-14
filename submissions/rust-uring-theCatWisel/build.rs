use std::env;
use std::path::PathBuf;
use std::process::Command;

fn is_pre_vaes_avx512_host() -> bool {
    #[cfg(target_arch = "x86_64")]
    {
        std::arch::is_x86_feature_detected!("avx512f")
            && !std::arch::is_x86_feature_detected!("vaes")
    }
    #[cfg(not(target_arch = "x86_64"))]
    {
        false
    }
}

fn main() {
    println!("cargo:rerun-if-changed=csrc/igzip_shim.c");
    println!("cargo:rerun-if-env-changed=FEED_NO_AVX512");
    let out_dir = PathBuf::from(env::var("OUT_DIR").expect("OUT_DIR"));
    let obj = out_dir.join("igzip_shim.o");
    let lib = out_dir.join("libigzip_shim.a");
    let cc = env::var("CC").unwrap_or_else(|_| "cc".to_string());
    let disable_avx512 = env::var("FEED_NO_AVX512")
        .map(|v| v == "1")
        .unwrap_or(false);

    let mut cmd = Command::new(&cc);
    cmd.args(["-O3", "-march=native"]);
    if disable_avx512 || is_pre_vaes_avx512_host() {
        cmd.args(["-mno-avx512f", "-mprefer-vector-width=256"]);
    }
    cmd.args(["-fno-plt", "-fPIC", "-Wall", "-Wextra"]);

    if PathBuf::from("/usr/local/include/isa-l.h").exists() {
        cmd.arg("-I/usr/local/include");
    }
    let status = cmd
        .args(["-c", "csrc/igzip_shim.c", "-o"])
        .arg(&obj)
        .status()
        .expect("failed to invoke C compiler for igzip_shim.c");
    assert!(status.success(), "compiling igzip_shim.c failed");

    let ar_status = Command::new("ar")
        .args(["rcs"])
        .arg(&lib)
        .arg(&obj)
        .status()
        .expect("failed to invoke ar");
    assert!(ar_status.success(), "archiving libigzip_shim.a failed");

    println!("cargo:rustc-link-search=native={}", out_dir.display());
    println!("cargo:rustc-link-lib=static=igzip_shim");
    if PathBuf::from("/usr/local/lib/libisal.a").exists() {
        println!("cargo:rustc-link-search=native=/usr/local/lib");
        println!("cargo:rustc-link-lib=static=isal");
    } else {
        println!("cargo:rustc-link-lib=isal");
    }
}
