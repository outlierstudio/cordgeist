//! Locked lease files. No daemon.
#![cfg(target_os = "linux")]
use cordgeist::{api::*, lease_file::LeaseFile};
use std::{
    os::unix::fs::PermissionsExt,
    sync::{Arc, Barrier},
    thread,
};
fn dir() -> tempfile::TempDir {
    let dir = tempfile::tempdir().unwrap();
    std::fs::set_permissions(dir.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
    dir
}
fn grant() -> LeaseGrant {
    LeaseGrant {
        auth: LeaseAuth {
            boot_id: BootId("boot".into()),
            device_id: DeviceId("phone".into()),
            lease_id: LeaseId("lease".into()),
            credential: Credential("credential-secret".into()),
            attachment: AttachmentGeneration(1),
            fence: Fence(1),
        },
        idle_deadline_ms: 60_000,
        next_seq: ActionSeq(1),
    }
}
#[test]
fn concurrent_allocation_is_unique_and_permissions_are_checked() {
    let dir = dir();
    let path = dir.path().join("lease.json");
    LeaseFile::create(&path, &grant()).unwrap();
    let mode = std::fs::metadata(&path).unwrap().permissions().mode() & 0o777;
    assert_eq!(mode, 0o600);
    let barrier = Arc::new(Barrier::new(2));
    let path_a = path.clone();
    let path_b = path.clone();
    let barrier_a = Arc::clone(&barrier);
    let one = thread::spawn(move || {
        barrier_a.wait();
        let file = LeaseFile::open(&path_a).unwrap();
        let mut alloc = file.allocate().unwrap();
        let seq = alloc.seq().0;
        alloc.commit_sent(false).unwrap();
        seq
    });
    let two = thread::spawn(move || {
        barrier.wait();
        let file = LeaseFile::open(&path_b).unwrap();
        let mut alloc = file.allocate().unwrap();
        let seq = alloc.seq().0;
        alloc.commit_sent(true).unwrap();
        seq
    });
    let mut seqs = [one.join().unwrap(), two.join().unwrap()];
    seqs.sort_unstable();
    assert_eq!(seqs, [1, 2]);
    std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o644)).unwrap();
    assert!(LeaseFile::open(&path).is_err());
    let odd = dir.path().join("nested/../lease.json");
    assert!(LeaseFile::open(&odd).is_err());
}
