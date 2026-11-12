//! Process-level CLI against the real executable and socket.
#![cfg(target_os = "linux")]
#![allow(dead_code, unused_imports)]
use std::{
    io::Write,
    os::fd::{AsRawFd, FromRawFd},
    os::unix::fs::PermissionsExt,
    path::{Path, PathBuf},
    process::Command,
    time::{Duration, Instant},
};
fn dir() -> tempfile::TempDir {
    let dir = tempfile::tempdir().unwrap();
    std::fs::set_permissions(dir.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
    dir
}
fn bin() -> Command {
    Command::new(env!("CARGO_BIN_EXE_cordgeist"))
}
fn run(runtime: &Path, args: &[&str]) -> std::process::Output {
    let mut command = bin();
    command.arg("--runtime").arg(runtime).args(args);
    command.output().unwrap()
}
struct Stop<'a>(&'a Path);
impl Drop for Stop<'_> {
    fn drop(&mut self) {
        let _ = run(self.0, &["daemon", "stop", "--json"]);
    }
}
#[test]
#[cfg(not(feature = "fake"))]
fn production_binary_refuses_labeled_software_backend() {
    let dir = dir();
    let out = bin()
        .arg("--runtime")
        .arg(dir.path())
        .args(["daemon", "start", "--json"])
        .env("CORDGEIST_SOFTWARE_BACKEND", "labeled-software-only")
        .output()
        .unwrap();
    assert_ne!(out.status.code(), Some(0));
    let text = String::from_utf8(out.stdout).unwrap() + &String::from_utf8(out.stderr).unwrap();
    assert!(!text.contains("SYNTHETIC"));
    assert!(!dir.path().join("cordgeist/daemon.sock").exists());
}
#[test]
#[cfg(feature = "fake")]
fn labeled_software_workflow_and_safety() {
    let dir = dir();
    let runtime = dir.path();
    let _stop = Stop(runtime);
    let log = runtime.join("effects.log");
    let start = bin()
        .arg("--runtime")
        .arg(runtime)
        .args(["daemon", "start", "--json"])
        .env("CORDGEIST_SOFTWARE_BACKEND", "labeled-software-only")
        .env("CORDGEIST_EFFECT_LOG", &log)
        .output()
        .unwrap();
    assert_eq!(
        start.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&start.stdout)
    );
    let again = bin()
        .arg("--runtime")
        .arg(runtime)
        .args(["daemon", "start", "--json"])
        .env("CORDGEIST_SOFTWARE_BACKEND", "labeled-software-only")
        .output()
        .unwrap();
    assert_eq!(again.status.code(), Some(0));
    let listed = run(runtime, &["--json", "list"]);
    assert_eq!(listed.status.code(), Some(0));
    let list_text = String::from_utf8(listed.stdout.clone()).unwrap();
    assert!(list_text.contains("software-phone"));
    assert!(listed.stderr.is_empty());
    let lease = runtime.join("lease.json");
    let acquired = run(
        runtime,
        &[
            "--json",
            "acquire",
            "--device",
            "software-phone",
            "--reason",
            "m2",
            "--lease-file",
            lease.to_str().unwrap(),
        ],
    );
    assert_eq!(
        acquired.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&acquired.stdout)
    );
    let body = String::from_utf8(acquired.stdout).unwrap();
    assert!(body.contains("redacted"));
    assert!(!body.contains("credential-secret"));
    let stored = std::fs::read_to_string(&lease).unwrap();
    assert!(!body.contains(&credential(&stored)));
    let shot = runtime.join("shot.png");
    let screen = run(
        runtime,
        &[
            "--json",
            "screenshot",
            "--lease-file",
            lease.to_str().unwrap(),
            "--output",
            shot.to_str().unwrap(),
            "--longest-edge",
            "8",
        ],
    );
    assert_eq!(
        screen.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&screen.stdout)
    );
    assert!(screen.stderr.is_empty());
    let screen_json: serde_json::Value = serde_json::from_slice(&screen.stdout).unwrap();
    assert_eq!(
        screen_json["outcome"]["result"]["value"]["image"]["base64"],
        "redacted"
    );
    let meta: serde_json::Value =
        serde_json::from_slice(&std::fs::read(export_meta(&shot)).unwrap()).unwrap();
    let png = std::fs::read(&shot).unwrap();
    assert_eq!(&png[12..16], b"IHDR");
    let width = u32::from_be_bytes(png[16..20].try_into().unwrap());
    let height = u32::from_be_bytes(png[20..24].try_into().unwrap());
    assert_eq!(
        width,
        meta["frame"]["image"]["width"].as_u64().unwrap() as u32
    );
    assert_eq!(
        height,
        meta["frame"]["image"]["height"].as_u64().unwrap() as u32
    );
    let frame = meta["frame"]["id"].as_str().unwrap().to_owned();
    let stale = run(
        runtime,
        &[
            "--json",
            "tap",
            "--lease-file",
            lease.to_str().unwrap(),
            "--frame",
            "missing",
            "--x",
            "1",
            "--y",
            "1",
        ],
    );
    assert_eq!(stale.status.code(), Some(2));
    assert!(stale.stderr.is_empty());
    assert_eq!(
        std::fs::read_to_string(&log)
            .unwrap_or_default()
            .lines()
            .count(),
        0
    );
    let tap = run(
        runtime,
        &[
            "--json",
            "tap",
            "--lease-file",
            lease.to_str().unwrap(),
            "--frame",
            &frame,
            "--x",
            "1",
            "--y",
            "1",
        ],
    );
    assert_eq!(
        tap.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&tap.stdout)
    );
    let effects = std::fs::read_to_string(&log).unwrap().lines().count();
    assert!(effects >= 1);
    let status = run(
        runtime,
        &[
            "--json",
            "action",
            "status",
            "--lease-file",
            lease.to_str().unwrap(),
            "--seq",
            "2",
        ],
    );
    assert_eq!(
        status.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&status.stdout)
    );
    assert!(String::from_utf8_lossy(&status.stdout).contains("\"seq\":2"));
    assert_eq!(
        std::fs::read_to_string(&log).unwrap().lines().count(),
        effects
    );
    let mut type_cmd = bin();
    type_cmd.arg("--runtime").arg(runtime).args([
        "--json",
        "type",
        "--lease-file",
        lease.to_str().unwrap(),
        "--frame",
        &frame,
        "--stdin",
    ]);
    type_cmd
        .stdin(std::process::Stdio::piped())
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped());
    let mut child = type_cmd.spawn().unwrap();
    child
        .stdin
        .take()
        .unwrap()
        .write_all(b"ZzSecretToken99\n")
        .unwrap();
    let typed = child.wait_with_output().unwrap();
    let typed_text = String::from_utf8(typed.stdout).unwrap()
        + &String::from_utf8(typed.stderr.clone()).unwrap();
    assert!(!typed_text.contains("ZzSecretToken99"));
    assert!(typed.stderr.is_empty());
    let before_calibration = std::fs::read_to_string(&log).unwrap();
    let calibrate = run(runtime, &["--json", "setup", "calibrate"]);
    assert_eq!(calibrate.status.code(), Some(2));
    assert!(String::from_utf8_lossy(&calibrate.stdout).contains("leased"));
    assert_eq!(std::fs::read_to_string(&log).unwrap(), before_calibration);
    let released = run(
        runtime,
        &["--json", "release", "--lease-file", lease.to_str().unwrap()],
    );
    assert_eq!(
        released.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&released.stdout)
    );
    let stopped = run(runtime, &["daemon", "stop", "--json"]);
    assert_eq!(
        stopped.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&stopped.stdout)
    );
    let end = Instant::now() + Duration::from_secs(2);
    while runtime.join("cordgeist/daemon.sock").exists() && Instant::now() < end {
        std::thread::sleep(Duration::from_millis(20));
    }
    assert!(!runtime.join("cordgeist/daemon.sock").exists());
}
#[test]
#[cfg(feature = "fake")]
fn timeout_keeps_the_original_sequence() {
    let dir = dir();
    let runtime = dir.path();
    let _stop = Stop(runtime);
    let log = runtime.join("effects.log");
    let start = bin()
        .arg("--runtime")
        .arg(runtime)
        .args(["--deadline-ms", "1200", "daemon", "start", "--json"])
        .env("CORDGEIST_SOFTWARE_BACKEND", "labeled-software-only")
        .env("CORDGEIST_SOFTWARE_DELAY_MS", "900")
        .env("CORDGEIST_EFFECT_LOG", &log)
        .output()
        .unwrap();
    assert_eq!(
        start.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&start.stdout)
    );
    let lease = runtime.join("lease.json");
    let acquired = run(
        runtime,
        &[
            "--json",
            "acquire",
            "--device",
            "software-phone",
            "--reason",
            "m2",
            "--lease-file",
            lease.to_str().unwrap(),
        ],
    );
    assert_eq!(
        acquired.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&acquired.stdout)
    );
    let shot = runtime.join("shot.png");
    let screen = run(
        runtime,
        &[
            "--json",
            "screenshot",
            "--lease-file",
            lease.to_str().unwrap(),
            "--output",
            shot.to_str().unwrap(),
            "--longest-edge",
            "8",
        ],
    );
    assert_eq!(
        screen.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&screen.stdout)
    );
    let meta: serde_json::Value =
        serde_json::from_slice(&std::fs::read(export_meta(&shot)).unwrap()).unwrap();
    let frame = meta["frame"]["id"].as_str().unwrap();
    let tap = bin()
        .arg("--runtime")
        .arg(runtime)
        .args([
            "--json",
            "--deadline-ms",
            "300",
            "tap",
            "--lease-file",
            lease.to_str().unwrap(),
            "--frame",
            frame,
            "--x",
            "1",
            "--y",
            "1",
        ])
        .env("CORDGEIST_SOFTWARE_BACKEND", "labeled-software-only")
        .output()
        .unwrap();
    assert_eq!(
        tap.status.code(),
        Some(3),
        "{}",
        String::from_utf8_lossy(&tap.stdout)
    );
    let effects = std::fs::read_to_string(&log).unwrap().lines().count();
    assert!(effects >= 1);
    std::thread::sleep(Duration::from_millis(800));
    let status = run(
        runtime,
        &[
            "--json",
            "action",
            "status",
            "--lease-file",
            lease.to_str().unwrap(),
            "--seq",
            "1",
        ],
    );
    assert_eq!(
        status.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&status.stdout)
    );
    assert!(String::from_utf8_lossy(&status.stdout).contains("\"seq\":1"));
    assert_eq!(
        std::fs::read_to_string(&log).unwrap().lines().count(),
        effects
    );
}
#[test]
fn concurrent_start_and_stale_socket_file() {
    let dir = dir();
    let runtime = dir.path();
    let _stop = Stop(runtime);
    let a = std::thread::spawn({
        let runtime = runtime.to_path_buf();
        move || {
            bin()
                .arg("--runtime")
                .arg(&runtime)
                .args(["daemon", "start", "--json"])
                .output()
                .unwrap()
                .status
                .code()
        }
    });
    let b = std::thread::spawn({
        let runtime = runtime.to_path_buf();
        move || {
            bin()
                .arg("--runtime")
                .arg(&runtime)
                .args(["daemon", "start", "--json"])
                .output()
                .unwrap()
                .status
                .code()
        }
    });
    let mut codes = [a.join().unwrap(), b.join().unwrap()];
    codes.sort();
    assert_eq!(codes[0], Some(0));
    assert!(codes[1] == Some(0) || codes[1] == Some(2));
    run(runtime, &["daemon", "stop", "--json"]);
    let sock_dir = runtime.join("cordgeist");
    std::fs::create_dir_all(&sock_dir).unwrap();
    std::fs::set_permissions(&sock_dir, std::fs::Permissions::from_mode(0o700)).unwrap();
    let stale = sock_dir.join("daemon.sock");
    std::fs::write(&stale, b"not-a-socket").unwrap();
    std::fs::set_permissions(&stale, std::fs::Permissions::from_mode(0o600)).unwrap();
    let failed = run(runtime, &["daemon", "start", "--json"]);
    assert_ne!(failed.status.code(), Some(0));
    assert_eq!(std::fs::read(&stale).unwrap(), b"not-a-socket");
}
#[test]
#[cfg(feature = "fake")]
fn lost_acquire_is_exit_3_and_writes_no_lease() {
    let dir = dir();
    let runtime = dir.path();
    let _stop = Stop(runtime);
    let start = bin()
        .arg("--runtime")
        .arg(runtime)
        .args(["daemon", "start", "--json"])
        .env("CORDGEIST_SOFTWARE_BACKEND", "labeled-software-only")
        .env("CORDGEIST_SOFTWARE_HOLD_MS", "1500")
        .output()
        .unwrap();
    assert_eq!(
        start.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&start.stdout)
    );
    let lease = runtime.join("lease.json");
    let acquired = bin()
        .arg("--runtime")
        .arg(runtime)
        .args([
            "--json",
            "--deadline-ms",
            "400",
            "acquire",
            "--device",
            "software-phone",
            "--reason",
            "m2",
            "--lease-file",
            lease.to_str().unwrap(),
        ])
        .env("CORDGEIST_SOFTWARE_BACKEND", "labeled-software-only")
        .output()
        .unwrap();
    assert_eq!(
        acquired.status.code(),
        Some(3),
        "{}",
        String::from_utf8_lossy(&acquired.stdout)
    );
    assert!(acquired.stderr.is_empty());
    let text = String::from_utf8(acquired.stdout).unwrap();
    assert_eq!(text.lines().count(), 1);
    assert!(text.contains("lease revoke --device software-phone"));
    assert!(!lease.exists());
}
#[test]
#[cfg(feature = "fake")]
fn lost_tap_names_the_sequence_without_the_credential() {
    let dir = dir();
    let runtime = dir.path();
    let _stop = Stop(runtime);
    let start = bin()
        .arg("--runtime")
        .arg(runtime)
        .args(["daemon", "start", "--json"])
        .env("CORDGEIST_SOFTWARE_BACKEND", "labeled-software-only")
        .env("CORDGEIST_SOFTWARE_DELAY_MS", "1200")
        .output()
        .unwrap();
    assert_eq!(
        start.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&start.stdout)
    );
    let lease = runtime.join("lease.json");
    let acquired = run(
        runtime,
        &[
            "--json",
            "acquire",
            "--device",
            "software-phone",
            "--reason",
            "m2",
            "--lease-file",
            lease.to_str().unwrap(),
        ],
    );
    assert_eq!(
        acquired.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&acquired.stdout)
    );
    let secret = credential(&std::fs::read_to_string(&lease).unwrap());
    let shot = runtime.join("shot.png");
    let screen = run(
        runtime,
        &[
            "--json",
            "screenshot",
            "--lease-file",
            lease.to_str().unwrap(),
            "--output",
            shot.to_str().unwrap(),
            "--longest-edge",
            "8",
        ],
    );
    assert_eq!(
        screen.status.code(),
        Some(0),
        "{}",
        String::from_utf8_lossy(&screen.stdout)
    );
    let meta: serde_json::Value =
        serde_json::from_slice(&std::fs::read(export_meta(&shot)).unwrap()).unwrap();
    let frame = meta["frame"]["id"].as_str().unwrap();
    let tap = bin()
        .arg("--runtime")
        .arg(runtime)
        .args([
            "--deadline-ms",
            "400",
            "tap",
            "--lease-file",
            lease.to_str().unwrap(),
            "--frame",
            frame,
            "--x",
            "1",
            "--y",
            "1",
        ])
        .env("CORDGEIST_SOFTWARE_BACKEND", "labeled-software-only")
        .output()
        .unwrap();
    assert_eq!(
        tap.status.code(),
        Some(3),
        "stdout {} stderr {}",
        String::from_utf8_lossy(&tap.stdout),
        String::from_utf8_lossy(&tap.stderr)
    );
    let err = String::from_utf8(tap.stderr).unwrap();
    assert!(err.contains("seq 1"), "{err}");
    assert!(!err.contains(&secret));
    assert!(!String::from_utf8(tap.stdout).unwrap().contains(&secret));
}
#[test]
#[cfg(feature = "fake")]
fn autostart_failure_does_not_send_and_stop_requires_the_socket_to_go() {
    let dir = dir();
    let runtime = dir.path();
    let sock_dir = runtime.join("cordgeist");
    std::fs::create_dir_all(&sock_dir).unwrap();
    std::fs::set_permissions(&sock_dir, std::fs::Permissions::from_mode(0o700)).unwrap();
    let stale = sock_dir.join("daemon.sock");
    std::fs::write(&stale, b"not-a-socket").unwrap();
    std::fs::set_permissions(&stale, std::fs::Permissions::from_mode(0o600)).unwrap();
    let listed = bin()
        .arg("--runtime")
        .arg(runtime)
        .args(["--json", "--autostart", "list"])
        .env("CORDGEIST_SOFTWARE_BACKEND", "labeled-software-only")
        .output()
        .unwrap();
    assert_eq!(listed.status.code(), Some(2));
    let text = String::from_utf8(listed.stdout).unwrap();
    assert_eq!(text.lines().count(), 1);
    assert!(!text.contains("software-phone"));
    assert!(!text.contains("devices.list"));
    std::fs::remove_file(&stale).unwrap();
    let listener = std::os::unix::net::UnixListener::bind(sock_dir.join("daemon.sock")).unwrap();
    let stopped = bin()
        .arg("--runtime")
        .arg(runtime)
        .args(["daemon", "stop", "--json"])
        .env("CORDGEIST_SOFTWARE_BACKEND", "labeled-software-only")
        .output()
        .unwrap();
    assert_eq!(
        stopped.status.code(),
        Some(2),
        "{}",
        String::from_utf8_lossy(&stopped.stdout)
    );
    let stop_text = String::from_utf8(stopped.stdout).unwrap();
    assert!(!stop_text.contains("\"daemon\":\"stopped\""));
    assert!(stop_text.contains("still present"));
    drop(listener);
}
#[test]
#[cfg(feature = "fake")]
fn failed_secret_read_restores_terminal_echo() {
    let master = unsafe { libc::posix_openpt(libc::O_RDWR | libc::O_NOCTTY) };
    assert!(master >= 0);
    assert_eq!(unsafe { libc::grantpt(master) }, 0);
    assert_eq!(unsafe { libc::unlockpt(master) }, 0);
    let name = unsafe { libc::ptsname(master) };
    assert!(!name.is_null());
    let slave = unsafe { libc::open(name, libc::O_RDWR | libc::O_NOCTTY) };
    assert!(slave >= 0);
    let mut term = unsafe { std::mem::zeroed() };
    assert_eq!(unsafe { libc::tcgetattr(slave, &mut term) }, 0);
    term.c_lflag |= libc::ECHO;
    term.c_lflag &= !libc::ICANON;
    term.c_cc[libc::VMIN] = 1;
    term.c_cc[libc::VTIME] = 0;
    assert_eq!(unsafe { libc::tcsetattr(slave, libc::TCSANOW, &term) }, 0);
    let slave_stdio = unsafe { std::fs::File::from_raw_fd(slave) };
    let probe = slave_stdio.try_clone().unwrap();
    let child = bin()
        .args([
            "type",
            "--lease-file",
            "/tmp/cordgeist-not-a-lease.json",
            "--frame",
            "frame",
            "--stdin",
        ])
        .env("CORDGEIST_SOFTWARE_BACKEND", "labeled-software-only")
        .stdin(std::process::Stdio::from(slave_stdio))
        .stdout(std::process::Stdio::piped())
        .stderr(std::process::Stdio::piped())
        .spawn()
        .unwrap();
    let mut master = unsafe { std::fs::File::from_raw_fd(master) };
    master.write_all(&[b'a'; 4096]).unwrap();
    let output = child.wait_with_output().unwrap();
    assert_ne!(output.status.code(), Some(0));
    let mut after = unsafe { std::mem::zeroed() };
    assert_eq!(unsafe { libc::tcgetattr(probe.as_raw_fd(), &mut after) }, 0);
    assert_ne!(after.c_lflag & libc::ECHO, 0);
}
fn credential(document: &str) -> String {
    let value: serde_json::Value = serde_json::from_str(document).unwrap();
    value["auth"]["credential"].as_str().unwrap().to_owned()
}
fn export_meta(png: &Path) -> PathBuf {
    let mut name = png.file_name().unwrap().to_os_string();
    name.push(".meta.json");
    png.with_file_name(name)
}
