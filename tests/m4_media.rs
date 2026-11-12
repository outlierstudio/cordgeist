//! Software media import and host staging. No phone and no picker claim.
#![cfg(all(target_os = "linux", feature = "fake"))]
#[path = "support/fixtures.rs"]
mod fixtures;
use std::{
    io::Write,
    os::unix::fs::PermissionsExt,
    process::{Command, Stdio},
};
fn dir() -> tempfile::TempDir {
    let dir = tempfile::tempdir().unwrap();
    std::fs::set_permissions(dir.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
    dir
}
fn png() -> Vec<u8> {
    b"\x89PNG\r\n\x1a\nrest".to_vec()
}
fn daemon(runtime: &std::path::Path) {
    let out = Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .arg("--runtime")
        .arg(runtime)
        .args(["daemon", "start", "--json"])
        .env("CORDGEIST_SOFTWARE_BACKEND", "labeled-software-only")
        .output()
        .unwrap();
    assert_eq!(
        out.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&out.stdout)
    );
}
fn lease(runtime: &std::path::Path) -> std::path::PathBuf {
    let path = runtime.join("lease.json");
    let out = Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .arg("--runtime")
        .arg(runtime)
        .args([
            "--json",
            "acquire",
            "--device",
            "software-phone",
            "--reason",
            "m4",
            "--lease-file",
        ])
        .arg(&path)
        .output()
        .unwrap();
    assert_eq!(
        out.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&out.stdout)
    );
    path
}
fn push(
    runtime: &std::path::Path,
    lease: &std::path::Path,
    source: &std::path::Path,
) -> std::process::Output {
    Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .arg("--runtime")
        .arg(runtime)
        .args(["--json", "media", "push", "--lease-file"])
        .arg(lease)
        .arg("--source")
        .arg(source)
        .args(["--kind", "image"])
        .output()
        .unwrap()
}
#[test]
fn same_sequence_rejects_a_different_file_and_keeps_one_destination() {
    let dir = dir();
    let runtime = dir.path();
    daemon(runtime);
    let lease = lease(runtime);
    let first = runtime.join("one.png");
    let second = runtime.join("two.png");
    std::fs::write(&first, png()).unwrap();
    let mut other = png();
    other.extend_from_slice(b"other");
    std::fs::write(&second, other).unwrap();
    let original = push(runtime, &lease, &first);
    assert_eq!(
        original.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&original.stdout)
    );
    let json: serde_json::Value = serde_json::from_slice(&original.stdout).unwrap();
    let dest = json["outcome"]["result"]["value"]["media"]["destination"]
        .as_str()
        .unwrap()
        .to_string();
    let seq = json["outcome"]["result"]["value"]["receipt"]["seq"]
        .as_u64()
        .unwrap();
    let mismatch = Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .arg("--runtime")
        .arg(runtime)
        .args(["--json", "media", "push", "--lease-file"])
        .arg(&lease)
        .arg("--source")
        .arg(&second)
        .args(["--kind", "image", "--seq"])
        .arg(seq.to_string())
        .output()
        .unwrap();
    assert_ne!(mismatch.status.code(), Some(0));
    assert!(String::from_utf8_lossy(&mismatch.stdout).contains("sequence-mismatch"));
    let again = Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .arg("--runtime")
        .arg(runtime)
        .args(["--json", "media", "push", "--lease-file"])
        .arg(&lease)
        .arg("--source")
        .arg(&first)
        .args(["--kind", "image", "--seq"])
        .arg(seq.to_string())
        .output()
        .unwrap();
    assert_eq!(
        again.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&again.stdout)
    );
    let again: serde_json::Value = serde_json::from_slice(&again.stdout).unwrap();
    assert_eq!(
        again["outcome"]["result"]["value"]["media"]["destination"],
        dest
    );
}
#[test]
fn import_reads_only_the_daemon_runtime() {
    let dir = dir();
    let runtime = dir.path();
    daemon(runtime);
    let lease = lease(runtime);
    let inside = runtime.join("inside.png");
    std::fs::write(&inside, png()).unwrap();
    assert_eq!(push(runtime, &lease, &inside).status.code(), Some(0));
    let outside = fixtures::build_root().join("m4-outside.png");
    std::fs::create_dir_all(outside.parent().unwrap()).unwrap();
    std::fs::write(&outside, png()).unwrap();
    assert_ne!(push(runtime, &lease, &outside).status.code(), Some(0));
    let link = runtime.join("leave.png");
    std::os::unix::fs::symlink(&outside, &link).unwrap();
    assert_ne!(push(runtime, &lease, &link).status.code(), Some(0));
}
#[test]
fn import_faults_keep_copy_and_index_distinct() {
    let dir = dir();
    let runtime = dir.path();
    daemon(runtime);
    let lease = lease(runtime);
    let good = runtime.join("shot.png");
    std::fs::write(&good, png()).unwrap();
    let first = push(runtime, &lease, &good);
    assert_eq!(
        first.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&first.stdout)
    );
    let json: serde_json::Value = serde_json::from_slice(&first.stdout).unwrap();
    let media = &json["outcome"]["result"]["value"]["media"];
    assert_eq!(media["copied"]["state"], "known");
    assert_eq!(media["indexed"]["state"], "unknown");
    assert_eq!(media["pickerVerified"]["state"], "unknown");
    let dest = media["destination"].as_str().unwrap().to_string();
    assert!(dest.contains("/sdcard/Pictures/Cordgeist/"));
    let seq = json["outcome"]["result"]["value"]["receipt"]["seq"]
        .as_u64()
        .unwrap();
    let again = Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .arg("--runtime")
        .arg(runtime)
        .args(["--json", "media", "push", "--lease-file"])
        .arg(&lease)
        .arg("--source")
        .arg(&good)
        .args(["--kind", "image", "--seq"])
        .arg(seq.to_string())
        .output()
        .unwrap();
    assert_eq!(again.status.code(), Some(0));
    let again: serde_json::Value = serde_json::from_slice(&again.stdout).unwrap();
    assert_eq!(
        again["outcome"]["result"]["value"]["media"]["destination"],
        dest
    );
    let scan = runtime.join("fault-scan.png");
    std::fs::write(&scan, png()).unwrap();
    let scanned = push(runtime, &lease, &scan);
    let scanned: serde_json::Value = serde_json::from_slice(&scanned.stdout).unwrap();
    let media = &scanned["outcome"]["result"]["value"]["media"];
    assert_eq!(media["copied"]["state"], "known");
    assert_eq!(media["indexed"]["state"], "error");
    assert_eq!(media["pickerVerified"]["state"], "unknown");
    assert!(
        media["destination"]
            .as_str()
            .unwrap()
            .contains("/sdcard/Pictures/Cordgeist/")
    );
    for name in [
        "fault-interrupt.png",
        "fault-full.png",
        "fault-mismatch.png",
        "hostile.png",
        "notes.txt",
    ] {
        let path = runtime.join(name);
        let bytes = if name.ends_with("txt") {
            b"hello".to_vec()
        } else {
            png()
        };
        std::fs::write(&path, bytes).unwrap();
        let out = push(runtime, &lease, &path);
        assert_ne!(out.status.code(), Some(0), "{name}");
        assert!(!String::from_utf8_lossy(&out.stdout).contains(name));
    }
    let dup_a = runtime.join("same.png");
    let dup_b = runtime.join("nested");
    std::fs::create_dir(&dup_b).unwrap();
    std::fs::write(&dup_a, png()).unwrap();
    std::fs::write(dup_b.join("same.png"), png()).unwrap();
    let a = push(runtime, &lease, &dup_a);
    let b = push(runtime, &lease, &dup_b.join("same.png"));
    let a: serde_json::Value = serde_json::from_slice(&a.stdout).unwrap();
    let b: serde_json::Value = serde_json::from_slice(&b.stdout).unwrap();
    assert_ne!(
        a["outcome"]["result"]["value"]["media"]["destination"],
        b["outcome"]["result"]["value"]["media"]["destination"]
    );
}
#[test]
fn stage_is_not_import_and_package_starts() {
    let runtime = fixtures::build_root().join("m4-stage");
    std::fs::create_dir_all(&runtime).unwrap();
    std::fs::set_permissions(&runtime, std::fs::Permissions::from_mode(0o700)).unwrap();
    let token = runtime.join("token");
    std::fs::write(&token, b"stage-token\n").unwrap();
    std::fs::set_permissions(&token, std::fs::Permissions::from_mode(0o600)).unwrap();
    let mut child = Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .arg("--runtime")
        .arg(&runtime)
        .args(["--json", "stage", "--token-file"])
        .arg(&token)
        .args(["--kind", "image"])
        .stdin(Stdio::piped())
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    child
        .stdin
        .take()
        .unwrap()
        .write_all(b"stage-token\n\x89PNG\r\n\x1a\nrest")
        .unwrap();
    let out = child.wait_with_output().unwrap();
    assert_eq!(
        out.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&out.stdout)
    );
    let json: serde_json::Value = serde_json::from_slice(&out.stdout).unwrap();
    assert_eq!(json["imported"], false);
    let staged = json["staged"].as_str().unwrap();
    assert!(std::path::Path::new(staged).is_file());
    let pkg_root = fixtures::build_root().join("m4-candidate");
    std::fs::create_dir_all(&pkg_root).unwrap();
    let copy = pkg_root.join("cordgeist");
    std::fs::copy(env!("CARGO_BIN_EXE_cordgeist"), &copy).unwrap();
    std::fs::set_permissions(&copy, std::fs::Permissions::from_mode(0o755)).unwrap();
    let doctor = Command::new(&copy)
        .arg("--runtime")
        .arg(runtime)
        .args(["doctor", "--json"])
        .env_remove("CARGO_TARGET_DIR")
        .output()
        .unwrap();
    assert_eq!(doctor.status.code(), Some(2));
    assert!(String::from_utf8_lossy(&doctor.stdout).contains("backend"));
    let pkg_runtime = pkg_root.join("runtime");
    std::fs::create_dir_all(&pkg_runtime).unwrap();
    std::fs::set_permissions(&pkg_runtime, std::fs::Permissions::from_mode(0o700)).unwrap();
    let start = Command::new(&copy)
        .arg("--runtime")
        .arg(&pkg_runtime)
        .args(["daemon", "start", "--json"])
        .env_remove("CARGO_TARGET_DIR")
        .output()
        .unwrap();
    assert_eq!(
        start.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&start.stdout)
    );
    let _ = Command::new(&copy)
        .arg("--runtime")
        .arg(&pkg_runtime)
        .args(["daemon", "stop", "--json"])
        .output();
}
