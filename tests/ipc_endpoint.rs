#![cfg(target_os = "linux")]
//! SOFTWARE-only Unix endpoints; no devices.
use cordgeist::ipc::endpoint::Endpoint;
use std::{
    fs,
    os::unix::{
        fs::{MetadataExt, PermissionsExt},
        net::UnixListener,
    },
};
fn directory() -> tempfile::TempDir {
    let d = tempfile::tempdir().unwrap();
    fs::set_permissions(d.path(), fs::Permissions::from_mode(0o700)).unwrap();
    d
}
#[test]
fn exclusive_start_private_permissions_and_graceful_cleanup() {
    let d = directory();
    let e = Endpoint::bind(d.path()).unwrap();
    let p = d.path().join("cordgeist/daemon.sock");
    assert_eq!(fs::metadata(&p).unwrap().mode() & 0o777, 0o600);
    assert!(Endpoint::bind(d.path()).is_err());
    assert!(p.exists());
    drop(e);
    assert!(!p.exists());
    Endpoint::bind(d.path()).unwrap();
}
#[test]
fn stale_socket_reclaimed_but_live_socket_and_replacements_preserved() {
    let d = directory();
    let e = Endpoint::bind(d.path()).unwrap();
    let p = d.path().join("cordgeist/daemon.sock");
    fs::remove_file(&p).unwrap();
    let replacement = UnixListener::bind(&p).unwrap();
    fs::set_permissions(&p, fs::Permissions::from_mode(0o600)).unwrap();
    drop(e);
    assert!(p.exists());
    assert!(Endpoint::bind(d.path()).is_err());
    drop(replacement);
    let e = Endpoint::bind(d.path()).unwrap();
    assert!(p.exists());
    drop(e);
}
#[test]
fn unsafe_parent_and_endpoint_fail_closed() {
    let d = directory();
    fs::set_permissions(d.path(), fs::Permissions::from_mode(0o755)).unwrap();
    assert!(Endpoint::bind(d.path()).is_err());
    fs::set_permissions(d.path(), fs::Permissions::from_mode(0o700)).unwrap();
    let e = Endpoint::bind(d.path()).unwrap();
    drop(e);
    let p = d.path().join("cordgeist/daemon.sock");
    fs::write(&p, b"unrelated").unwrap();
    assert!(Endpoint::bind(d.path()).is_err());
    assert_eq!(fs::read(&p).unwrap(), b"unrelated");
}

#[test]
fn foreign_or_public_parent_symlinks_and_unsafe_lock_are_rejected() {
    // Actual system root is foreign-owned or public; fail before any mutation.
    assert!(Endpoint::bind(std::path::Path::new("/")).is_err());
    let d = directory();
    let link = d.path().join("link");
    std::os::unix::fs::symlink(d.path(), &link).unwrap();
    assert!(Endpoint::bind(&link).is_err());
    let e = Endpoint::bind(d.path()).unwrap();
    drop(e);
    let lock = d.path().join("cordgeist/instance.lock");
    fs::set_permissions(&lock, fs::Permissions::from_mode(0o644)).unwrap();
    assert!(Endpoint::bind(d.path()).is_err());
    fs::set_permissions(&lock, fs::Permissions::from_mode(0o600)).unwrap();
    fs::hard_link(&lock, d.path().join("extra-lock-link")).unwrap();
    assert!(Endpoint::bind(d.path()).is_err());
}

#[test]
fn saturated_live_endpoint_probe_is_bounded_and_preserves_socket() {
    use std::{
        os::{fd::AsRawFd, unix::net::UnixStream},
        time::Duration,
    };
    let d = directory();
    drop(Endpoint::bind(d.path()).unwrap());
    let path = d.path().join("cordgeist/daemon.sock");
    let listener = UnixListener::bind(&path).unwrap();
    fs::set_permissions(&path, fs::Permissions::from_mode(0o600)).unwrap();
    assert_eq!(unsafe { libc::listen(listener.as_raw_fd(), 0) }, 0);
    let client = UnixStream::connect(&path).unwrap();
    let root = d.path().to_owned();
    let (tx, rx) = std::sync::mpsc::channel();
    let task = std::thread::spawn(move || {
        tx.send(Endpoint::bind(&root).is_err()).unwrap();
    });
    let result = rx.recv_timeout(Duration::from_millis(500));
    assert!(path.exists());
    drop(client);
    drop(listener);
    task.join().unwrap();
    assert_eq!(result, Ok(true));
    assert!(path.exists());
}
