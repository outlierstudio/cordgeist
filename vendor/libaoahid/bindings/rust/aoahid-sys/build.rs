// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
// Links only the externally installed native ABI; this crate does not build or
// configure libaoahid on the caller's behalf.
fn main() {
    if let Ok(directory) = std::env::var("AOAHID_LIB_DIR") {
        println!("cargo:rustc-link-search=native={directory}");
    }
    println!("cargo:rustc-link-lib=dylib=aoahid");
    println!("cargo:rerun-if-env-changed=AOAHID_LIB_DIR");
}
