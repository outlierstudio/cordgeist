fn main() {
    println!("cargo:rerun-if-env-changed=CORDGEIST_NATIVE_INCLUDE");
    println!("cargo:rerun-if-env-changed=AOAHID_LIB_DIR");
    println!("cargo:rerun-if-env-changed=CORDGEIST_RELOCATABLE");
    if std::env::var_os("CARGO_FEATURE_NATIVE_AOA").is_some() {
        let root = std::path::PathBuf::from(std::env::var_os("CARGO_MANIFEST_DIR").unwrap());
        let include = std::env::var_os("CORDGEIST_NATIVE_INCLUDE")
            .map(std::path::PathBuf::from)
            .unwrap_or_else(|| root.join("vendor/libaoahid/include"));
        let lib = std::env::var_os("AOAHID_LIB_DIR")
            .map(std::path::PathBuf::from)
            .expect("native-aoa requires AOAHID_LIB_DIR; run scripts/build-native.sh");
        assert!(lib.is_absolute(), "AOAHID_LIB_DIR must be absolute");
        cc::Build::new()
            .file("native/abi.c")
            .include(include)
            .warnings_into_errors(true)
            .compile("cordgeist_abi");
        println!("cargo:rerun-if-changed=native/abi.c");
        println!("cargo:rustc-link-arg=-Wl,-rpath,$ORIGIN/../lib");
        if std::env::var("CORDGEIST_RELOCATABLE").as_deref() != Ok("1") {
            println!("cargo:rustc-link-arg=-Wl,-rpath,{}", lib.display());
        }
    }
}
