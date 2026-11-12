#![cfg(target_os = "linux")]
//! Isolated SOFTWARE test service process. No device/native paths.
use cordgeist::ipc::server::Server;
use std::{
    io::{BufRead, Write},
    os::unix::fs::PermissionsExt,
    process::{Child, Command, Stdio},
    time::Duration,
};
#[test]
#[ignore = "subprocess fixture, launched only by process tests"]
fn software_service_fixture() {
    let root = std::env::var_os("CORDGEIST_TEST_IPC_ROOT").expect("explicit software fixture root");
    let server = match Server::bind(std::path::Path::new(&root)) {
        Ok(s) => s,
        Err(e) => {
            assert_eq!(e.kind(), std::io::ErrorKind::AddrInUse);
            std::process::exit(42);
        }
    };
    let stop = server.stop_handle();
    let owner = std::thread::spawn(move || server.run().unwrap());
    println!("SOFTWARE SERVICE READY");
    std::io::stdout().flush().unwrap();
    let mut line = String::new();
    std::io::stdin().read_line(&mut line).unwrap();
    assert_eq!(line, "stop\n");
    stop.request();
    owner.join().unwrap();
}
struct Process(Child);
impl Drop for Process {
    fn drop(&mut self) {
        let _ = self.0.kill();
        let _ = self.0.wait();
    }
}
fn spawn(root: &std::path::Path) -> Process {
    Process(
        Command::new(std::env::current_exe().unwrap())
            .args([
                "--ignored",
                "--exact",
                "software_service_fixture",
                "--nocapture",
            ])
            .env("CORDGEIST_TEST_IPC_ROOT", root)
            .stdin(Stdio::piped())
            .stdout(Stdio::piped())
            .stderr(Stdio::null())
            .spawn()
            .unwrap(),
    )
}
fn ready(p: &mut Process) {
    let out = p.0.stdout.take().unwrap();
    let (tx, rx) = std::sync::mpsc::channel();
    std::thread::spawn(move || {
        for line in std::io::BufReader::new(out).lines() {
            if line.unwrap() == "SOFTWARE SERVICE READY" {
                tx.send(()).unwrap();
            }
        }
    });
    rx.recv_timeout(Duration::from_secs(3)).unwrap();
}
fn wait(p: &mut Process) -> std::process::ExitStatus {
    let end = std::time::Instant::now() + Duration::from_secs(4);
    loop {
        if let Some(s) = p.0.try_wait().unwrap() {
            return s;
        }
        assert!(
            std::time::Instant::now() < end,
            "fixture process exceeded wait budget"
        );
        std::thread::sleep(Duration::from_millis(5));
    }
}
#[test]
fn real_process_exclusion_crash_stale_restart_and_stop() {
    let d = tempfile::tempdir().unwrap();
    std::fs::set_permissions(d.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
    let mut live = spawn(d.path());
    ready(&mut live);
    let path = d.path().join("cordgeist/daemon.sock");
    let mut first = spawn(d.path());
    let mut second = spawn(d.path());
    assert_eq!(wait(&mut first).code(), Some(42));
    assert_eq!(wait(&mut second).code(), Some(42));
    assert!(path.exists());
    live.0.kill().unwrap();
    assert!(!wait(&mut live).success());
    assert!(path.exists());
    let mut restart = spawn(d.path());
    ready(&mut restart);
    restart
        .0
        .stdin
        .as_mut()
        .unwrap()
        .write_all(b"stop\n")
        .unwrap();
    assert!(wait(&mut restart).success());
    assert!(!path.exists());
}
