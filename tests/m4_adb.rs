//! Labeled fake ADB executable. These tests show supervisor arguments.
//! They do not transfer to a phone or verify a picker.
#![cfg(all(target_os = "linux", feature = "fake"))]
#[path = "support/fixtures.rs"]
mod fixtures;
use std::{
    io::Write,
    os::unix::fs::PermissionsExt,
    path::{Path, PathBuf},
    process::{Command, Stdio},
};

struct Stop(PathBuf);
impl Drop for Stop {
    fn drop(&mut self) {
        let _ = Command::new(env!("CARGO_BIN_EXE_cordgeist"))
            .arg("--runtime")
            .arg(&self.0)
            .args(["daemon", "stop", "--json"])
            .output();
    }
}
fn png() -> Vec<u8> {
    b"\x89PNG\r\n\x1a\nrest".to_vec()
}
fn adb(dir: &Path) -> PathBuf {
    let path = dir.join("fake-adb");
    let mut file = std::fs::File::create(&path).unwrap();
    file.write_all(
        br#"#!/usr/bin/python3
import os, sys, time
here = os.path.dirname(os.path.abspath(sys.argv[0]))
with open(os.path.join(here, "argv.log"), "a") as log:
    log.write("\0".join(sys.argv) + "\n")
mode_path = os.path.join(here, "mode")
mode = open(mode_path).read().strip() if os.path.exists(mode_path) else "ok"
text = " ".join(sys.argv)
if "mkdir -p '/sdcard/" in text:
    if mode == "mkdir-denied": sys.exit(1)
    assert text.endswith("mkdir -p '/sdcard/Pictures/Cordgeist'") or text.endswith("mkdir -p '/sdcard/Movies/Cordgeist'")
    open(os.path.join(here, "directory-ready"), "w").close()
    sys.exit(0)
if " push " in " " + text:
    assert os.path.exists(os.path.join(here, "directory-ready")), "first copy requires directory preparation"
    if mode == "hold":
        deadline = time.time() + 1.0
        while time.time() < deadline and not os.path.exists(os.path.join(here, "release")):
            time.sleep(0.05)
    if mode == "interrupt":
        sys.exit(1)
    if mode == "full":
        sys.stderr.write("No space left on device\n")
        sys.exit(1)
    local, remote = sys.argv[-2], sys.argv[-1]
    size = os.path.getsize(local)
    if mode == "mutate":
        size += 1
    with open(os.path.join(here, "sizes"), "a") as sizes:
        sizes.write(remote + " " + str(size) + "\n")
    sys.exit(0)
if "wc -c" in text:
    remote = text.split("'", 2)[1]
    size = "0"
    for line in open(os.path.join(here, "sizes")):
        left, right = line.rsplit(" ", 1)
        if left == remote:
            size = right.strip()
    print(size)
    sys.exit(0)
if "get-current-user" in text:
    print("0")
    sys.exit(0)
if "scan_file" in text:
    if mode == "deny":
        sys.exit(1)
    if mode == "timeout":
        time.sleep(5)
    if mode == "unavailable":
        print("scan_file unavailable")
        sys.exit(0)
    print("result=content://media/external/images/media/7")
    sys.exit(0)
sys.exit(0)
"#,
    )
    .unwrap();
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o755)).unwrap();
    path
}
fn mode(dir: &Path, value: &str) {
    std::fs::write(dir.join("mode"), value).unwrap();
}
fn argv(dir: &Path) -> String {
    std::fs::read_to_string(dir.join("argv.log")).unwrap_or_default()
}
fn daemon(runtime: &Path, adb_path: &Path) {
    let out = Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .arg("--runtime")
        .arg(runtime)
        .args(["daemon", "start", "--json"])
        .env("CORDGEIST_SOFTWARE_BACKEND", "labeled-software-only")
        .env("CORDGEIST_ADB", adb_path)
        .env("CORDGEIST_ADB_PORT", "27183")
        .output()
        .unwrap();
    assert_eq!(
        out.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&out.stdout)
    );
}
fn lease(runtime: &Path) -> PathBuf {
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
            "m4-adb",
            "--lease-file",
        ])
        .arg(&path)
        .output()
        .unwrap();
    assert_eq!(out.status.code(), Some(0));
    path
}
fn push(runtime: &Path, lease: &Path, source: &Path, seq: u64) -> std::process::Output {
    Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .arg("--runtime")
        .arg(runtime)
        .args(["--json", "media", "push", "--lease-file"])
        .arg(lease)
        .arg("--source")
        .arg(source)
        .args(["--kind", "image", "--seq"])
        .arg(seq.to_string())
        .output()
        .unwrap()
}
fn media(out: &std::process::Output) -> serde_json::Value {
    let json: serde_json::Value = serde_json::from_slice(&out.stdout).unwrap();
    json["outcome"]["result"]["value"]["media"].clone()
}
fn assert_supervisor(log: &str) {
    let line = log.lines().next().unwrap();
    let args: Vec<_> = line.split('\0').collect();
    assert!(
        args.windows(2).any(|pair| pair == ["-H", "127.0.0.1"]),
        "{args:?}"
    );
    assert!(
        args.windows(2).any(|pair| pair == ["-P", "27183"]),
        "{args:?}"
    );
    assert!(args.windows(2).any(|pair| pair == ["-t", "8"]), "{args:?}");
}
#[test]
fn fake_adb_supervisor_covers_transfer_faults_and_scan_denial() {
    let kept = fixtures::build_root().join(format!("m4-adb-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&kept);
    std::fs::create_dir_all(&kept).unwrap();
    let root = kept.as_path();
    let runtime = root.join("runtime");
    std::fs::create_dir(&runtime).unwrap();
    std::fs::set_permissions(&runtime, std::fs::Permissions::from_mode(0o700)).unwrap();
    let _stop = Stop(runtime.clone());
    let tool = adb(root);
    daemon(&runtime, &tool);
    let lease = lease(&runtime);
    let image = runtime.join("photo.png");
    std::fs::write(&image, png()).unwrap();
    mode(root, "mkdir-denied");
    let blocked = push(&runtime, &lease, &image, 6);
    assert_ne!(blocked.status.code(), Some(0));
    assert!(!argv(root).contains("\0push\0"));
    mode(root, "ok");
    let first = push(&runtime, &lease, &image, 7);
    assert_eq!(
        first.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&first.stdout)
    );
    let body = media(&first);
    assert_eq!(body["copied"]["state"], "known");
    assert_eq!(body["indexed"]["state"], "known");
    assert_eq!(body["pickerVerified"]["state"], "unknown");
    assert_eq!(body["uri"], "content://media/external/images/media/7");
    let log = argv(root);
    assert_supervisor(&log);
    assert!(log.contains("push"));
    assert!(log.contains("scan_file"));
    assert!(log.contains("'path:s:/sdcard/Pictures/Cordgeist/"));
    let again = push(&runtime, &lease, &image, 7);
    assert_eq!(again.status.code(), Some(0));
    assert_eq!(media(&again)["destination"], body["destination"]);
    assert_eq!(argv(root).matches("push").count(), 1);
    mode(root, "interrupt");
    let interrupted = push(&runtime, &lease, &image, 8);
    assert_ne!(interrupted.status.code(), Some(0));
    let after = argv(root);
    assert!(after.matches("\0push\0").count() >= 2 || after.matches("push").count() >= 2);
    assert!(after.contains("\0rm\0") || after.contains("rm '"));
    mode(root, "full");
    let full = push(&runtime, &lease, &image, 9);
    assert_ne!(full.status.code(), Some(0));
    mode(root, "mutate");
    let before_mutation = argv(root);
    let mutated = push(&runtime, &lease, &image, 10);
    assert_ne!(mutated.status.code(), Some(0));
    let after_mutation = argv(root);
    assert!(!after_mutation[before_mutation.len()..].contains("\0shell\0mv '"));
    mode(root, "deny");
    let denied = push(&runtime, &lease, &image, 11);
    assert_eq!(
        denied.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&denied.stdout)
    );
    let denied_media = media(&denied);
    assert_eq!(denied_media["copied"]["state"], "known");
    assert_eq!(denied_media["indexed"]["state"], "error");
    assert_eq!(denied_media["pickerVerified"]["state"], "unknown");
    assert!(denied_media["uri"].is_null());
    mode(root, "timeout");
    let timed = push(&runtime, &lease, &image, 12);
    assert_eq!(timed.status.code(), Some(0));
    assert_eq!(media(&timed)["indexed"]["detail"], "scan-file timeout");
    assert_eq!(media(&timed)["copied"]["state"], "known");
    mode(root, "unavailable");
    let missing = push(&runtime, &lease, &image, 13);
    assert_eq!(missing.status.code(), Some(0));
    assert_eq!(
        media(&missing)["indexed"]["detail"],
        "scan-file unavailable"
    );
    assert!(media(&missing)["uri"].is_null());
    let hostile = runtime.join("hostile.png");
    std::fs::write(&hostile, png()).unwrap();
    let before = argv(root).len();
    let hostile_out = push(&runtime, &lease, &hostile, 14);
    assert_ne!(hostile_out.status.code(), Some(0));
    let notes = runtime.join("notes.txt");
    std::fs::write(&notes, b"hello").unwrap();
    let mime = push(&runtime, &lease, &notes, 15);
    assert_ne!(mime.status.code(), Some(0));
    assert_eq!(argv(root).len(), before);
}
#[test]
fn one_sequence_reserves_one_transfer() {
    let kept = fixtures::build_root().join(format!("m4-adb-reserve-{}", std::process::id()));
    let _ = std::fs::remove_dir_all(&kept);
    std::fs::create_dir_all(&kept).unwrap();
    let root = kept.as_path();
    let runtime = root.join("runtime");
    std::fs::create_dir(&runtime).unwrap();
    std::fs::set_permissions(&runtime, std::fs::Permissions::from_mode(0o700)).unwrap();
    let _stop = Stop(runtime.clone());
    let tool = adb(root);
    daemon(&runtime, &tool);
    let lease = lease(&runtime);
    let first_file = runtime.join("one.png");
    let second_file = runtime.join("two.png");
    std::fs::write(&first_file, png()).unwrap();
    std::fs::write(&second_file, b"\x89PNG\r\n\x1a\nOTHER").unwrap();
    mode(root, "hold");
    let overlapping = Command::new(env!("CARGO_BIN_EXE_cordgeist"))
        .arg("--runtime")
        .arg(&runtime)
        .args(["--json", "media", "push", "--lease-file"])
        .arg(&lease)
        .arg("--source")
        .arg(&first_file)
        .args(["--kind", "image", "--seq", "21"])
        .stdout(Stdio::piped())
        .stderr(Stdio::null())
        .spawn()
        .unwrap();
    let started = std::time::Instant::now();
    while !argv(root).contains("\0push\0") {
        assert!(started.elapsed().as_secs() < 2, "push did not start");
        std::thread::sleep(std::time::Duration::from_millis(20));
    }
    let other = push(&runtime, &lease, &second_file, 21);
    assert_ne!(other.status.code(), Some(0));
    assert!(String::from_utf8_lossy(&other.stdout).contains("sequence-mismatch"));
    assert_eq!(argv(root).matches("\0push\0").count(), 1);
    std::fs::write(root.join("release"), b"1").unwrap();
    let finished = overlapping.wait_with_output().unwrap();
    assert_eq!(
        finished.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&finished.stdout)
    );
    let destination = media(&finished)["destination"].clone();
    let repeated = push(&runtime, &lease, &first_file, 21);
    assert_eq!(repeated.status.code(), Some(0));
    assert_eq!(media(&repeated)["destination"], destination);
    assert_eq!(argv(root).matches("\0push\0").count(), 1);
    mode(root, "interrupt");
    let interrupted = push(&runtime, &lease, &first_file, 22);
    assert_ne!(interrupted.status.code(), Some(0));
    mode(root, "ok");
    let retried = push(&runtime, &lease, &first_file, 22);
    assert_eq!(
        retried.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&retried.stdout)
    );
    assert_eq!(argv(root).matches("\0push\0").count(), 3);
}
