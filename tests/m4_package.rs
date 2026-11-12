//! Extracted private package. No phone, no platform-tools, and no publish step.
#![cfg(target_os = "linux")]
#[path = "support/fixtures.rs"]
mod fixtures;
use std::{os::unix::fs::PermissionsExt, path::Path, process::Command};
#[test]
fn extracted_archive_runs_doctor_and_daemon() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR"));
    let build = fixtures::build_root();
    let packaged = Command::new("sh")
        .arg(root.join("scripts/package-linux.sh"))
        .arg(env!("CARGO_BIN_EXE_cordgeist"))
        .current_dir(root)
        .status()
        .unwrap();
    assert!(packaged.success());
    let extract = build.join("m4-extracted");
    let _ = std::fs::remove_dir_all(&extract);
    std::fs::create_dir_all(&extract).unwrap();
    let unpacked = Command::new("tar")
        .arg("-C")
        .arg(&extract)
        .arg("-xzf")
        .arg(build.join("cordgeist-linux-x86_64.tar.gz"))
        .status()
        .unwrap();
    assert!(unpacked.success());
    let package = extract.join("cordgeist-linux-x86_64");
    assert!(package.join("LICENSE").is_file());
    assert!(package.join("NOTICE").is_file());
    assert!(package.join("THIRD_PARTY_NOTICES.md").is_file());
    assert!(package.join("third-party-notices").is_dir());
    assert!(!package.join("platform-tools").exists());
    let bin = package.join("cordgeist");
    let runtime = extract.join("runtime");
    std::fs::create_dir_all(&runtime).unwrap();
    std::fs::set_permissions(&runtime, std::fs::Permissions::from_mode(0o700)).unwrap();
    let doctor = Command::new(&bin)
        .env("CORDGEIST_ADB", "/definitely/missing/adb")
        .current_dir(&build)
        .arg("--runtime")
        .arg(&runtime)
        .args(["doctor", "--json"])
        .env_remove("CARGO_TARGET_DIR")
        .env_remove("LD_LIBRARY_PATH")
        .env_remove("AOAHID_LIB_DIR")
        .env_remove("CORDGEIST_SOFTWARE_BACKEND")
        .output()
        .unwrap();
    assert_eq!(
        doctor.status.code(),
        Some(2),
        "{}",
        String::from_utf8_lossy(&doctor.stdout)
    );
    assert!(String::from_utf8_lossy(&doctor.stdout).contains("backend"));
    let start = Command::new(&bin)
        .env("CORDGEIST_ADB", "/definitely/missing/adb")
        .current_dir(&build)
        .arg("--runtime")
        .arg(&runtime)
        .args(["daemon", "start", "--json"])
        .env_remove("CARGO_TARGET_DIR")
        .env_remove("LD_LIBRARY_PATH")
        .env_remove("AOAHID_LIB_DIR")
        .output()
        .unwrap();
    assert_eq!(
        start.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&start.stdout)
    );
    let _ = Command::new(&bin)
        .env("CORDGEIST_ADB", "/definitely/missing/adb")
        .arg("--runtime")
        .arg(&runtime)
        .args(["daemon", "stop", "--json"])
        .output();
}
