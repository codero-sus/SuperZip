// Build the C++ engine (native/codec.cpp) without any crates.io dependencies:
// shells out to g++ and ar directly.
use std::env;
use std::path::PathBuf;
use std::process::Command;

fn main() {
    let manifest = PathBuf::from(env::var("CARGO_MANIFEST_DIR").unwrap());
    let native = manifest.join("../../native");
    let out = PathBuf::from(env::var("OUT_DIR").unwrap());

    println!("cargo:rerun-if-changed={}", native.join("codec.cpp").display());
    println!("cargo:rerun-if-changed={}", native.join("szip.h").display());
    println!("cargo:rerun-if-changed={}", native.join("rc.hpp").display());
    println!("cargo:rerun-if-changed={}", native.join("crc32.hpp").display());
    println!("cargo:rerun-if-changed={}", native.join("lz77.hpp").display());

    let obj = out.join("szip_codec.o");
    let status = Command::new("g++")
        .args(["-std=c++17", "-O2", "-fPIC", "-DNDEBUG", "-c"])
        .arg(format!("-I{}", native.display()))
        .arg(native.join("codec.cpp"))
        .arg("-o")
        .arg(&obj)
        .status()
        .expect("failed to run g++ — a C++17 compiler is required");
    assert!(status.success(), "g++ failed to compile native/codec.cpp");

    let lib = out.join("libszip_native.a");
    let _ = std::fs::remove_file(&lib);
    let status = Command::new("ar")
        .arg("rcs")
        .arg(&lib)
        .arg(&obj)
        .status()
        .expect("failed to run ar");
    assert!(status.success(), "ar failed to create libszip_native.a");

    println!("cargo:rustc-link-search=native={}", out.display());
    println!("cargo:rustc-link-lib=static=szip_native");
    println!("cargo:rustc-link-lib=stdc++");
}
