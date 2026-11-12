#![cfg(target_os = "linux")]
//! SOFTWARE FAKE identity evidence and private test files; no USB/device access.
#[path = "support/fixtures.rs"]
mod fixtures;
use cordgeist::{
    adb::{Attachment, Authorization},
    api::DeviceId,
    discovery::UsbDevice,
    identity::Pairing,
    pairing_store::{PairingStore, StoreError},
};
use std::{
    fs,
    os::unix::fs::{MetadataExt, PermissionsExt, symlink},
    path::Path,
};
fn directory() -> tempfile::TempDir {
    fixtures::private_tempdir()
}
fn pairing(id: &str, port: u8) -> Pairing {
    let serial = format!("SYNTHETIC_{id}");
    let usb = UsbDevice {
        bus: 1,
        address: 7,
        ports: vec![port],
        vendor_id: 1,
        product_id: 2,
        aoa_protocol: 2,
        serial: Some(serial.clone()),
        product: None,
    };
    let adb = Attachment {
        serial,
        state: Authorization::Authorized,
        transport_id: Some(8),
        usb: Some(format!("1-{port}")),
    };
    Pairing::confirm(
        DeviceId(id.into()),
        Some(format!("alias_{id}")),
        &usb,
        &adb,
        false,
    )
    .unwrap()
}
fn private_write(path: &Path, bytes: &[u8]) {
    fs::write(path, bytes).unwrap();
    fs::set_permissions(path, fs::Permissions::from_mode(0o600)).unwrap();
}
#[test]
fn only_explicit_pairings_and_aliases_persist_and_updates_preserve_other_records() {
    let dir = directory();
    let store = PairingStore::open(dir.path()).unwrap();
    private_write(
        &dir.path().join(".pairings-unrelated.tmp"),
        b"unrelated synthetic file",
    );
    assert!(store.load().unwrap().pairings.is_empty());
    assert!(!dir.path().join("pairings.json").exists());
    store.set_pairing(pairing("phone_a", 2)).unwrap();
    store.set_pairing(pairing("phone_b", 3)).unwrap();
    let a = DeviceId("phone_a".into());
    store
        .set_alias(&a, Some("renamed human label".into()))
        .unwrap();
    let reopened = PairingStore::open(dir.path()).unwrap().load().unwrap();
    assert_eq!(reopened.pairings.len(), 2);
    assert_eq!(reopened.id_for_alias("renamed human label").unwrap(), a);
    assert_eq!(
        fs::metadata(dir.path().join("pairings.json"))
            .unwrap()
            .mode()
            & 0o777,
        0o600
    );
    let value: serde_json::Value =
        serde_json::from_slice(&fs::read(dir.path().join("pairings.json")).unwrap()).unwrap();
    assert!(value.get("credential").is_none());
    assert!(value.get("frames").is_none());
    assert_eq!(value["version"], 1);
    assert_eq!(store.remove_pairing(&a).unwrap().pairings.len(), 1);
    assert_eq!(
        fs::read(dir.path().join(".pairings-unrelated.tmp")).unwrap(),
        b"unrelated synthetic file"
    );
    let temps = fs::read_dir(dir.path())
        .unwrap()
        .filter_map(|e| {
            let name = e.unwrap().file_name();
            name.to_string_lossy().ends_with(".tmp").then_some(name)
        })
        .collect::<Vec<_>>();
    assert_eq!(temps, [std::ffi::OsString::from(".pairings-unrelated.tmp")]);
}
#[test]
fn malformed_or_unsupported_config_is_never_silently_replaced() {
    for bytes in [
        b"not JSON".as_slice(),
        br#"{"version":2,"pairings":[]}"#,
        br#"{"version":1,"pairings":[],"credential":"SYNTHETIC_FORBIDDEN_FIELD"}"#,
    ] {
        let dir = directory();
        let path = dir.path().join("pairings.json");
        private_write(&path, bytes);
        let store = PairingStore::open(dir.path()).unwrap();
        assert!(store.load().is_err());
        assert!(store.set_pairing(pairing("phone", 2)).is_err());
        assert_eq!(fs::read(path).unwrap(), bytes);
    }
}
#[test]
fn unsafe_permissions_symlinks_hardlinks_and_large_files_fail_closed() {
    let dir = directory();
    let store = PairingStore::open(dir.path()).unwrap();
    let path = dir.path().join("pairings.json");
    private_write(&path, &vec![b' '; 65537]);
    assert_eq!(store.load().err(), Some(StoreError::InvalidData));
    fs::remove_file(&path).unwrap();
    let victim = directory();
    let victim_file = victim.path().join("untouched");
    private_write(&victim_file, b"private synthetic contents");
    symlink(&victim_file, &path).unwrap();
    assert!(store.set_pairing(pairing("phone", 2)).is_err());
    assert_eq!(
        fs::read(&victim_file).unwrap(),
        b"private synthetic contents"
    );
    fs::remove_file(&path).unwrap();
    fs::hard_link(&victim_file, &path).unwrap();
    assert!(store.load().is_err());
    fs::remove_file(&path).unwrap();
    private_write(&path, br#"{"version":1,"pairings":[]}"#);
    fs::set_permissions(&path, fs::Permissions::from_mode(0o644)).unwrap();
    assert!(store.load().is_err());
    fs::set_permissions(dir.path(), fs::Permissions::from_mode(0o755)).unwrap();
    assert!(PairingStore::open(dir.path()).is_err());
    fs::set_permissions(dir.path(), fs::Permissions::from_mode(0o700)).unwrap();
    let link = victim.path().join("linked-directory");
    symlink(dir.path(), &link).unwrap();
    assert!(PairingStore::open(&link).is_err());
}
#[test]
fn held_lock_is_busy_and_invalid_updates_preserve_bytes() {
    let dir = directory();
    let store = PairingStore::open(dir.path()).unwrap();
    store.set_pairing(pairing("phone", 2)).unwrap();
    let before = fs::read(dir.path().join("pairings.json")).unwrap();
    assert!(
        store
            .set_alias(&DeviceId("phone".into()), Some("invalid\nlabel".into()))
            .is_err()
    );
    assert_eq!(fs::read(dir.path().join("pairings.json")).unwrap(), before);
    let lock = fs::OpenOptions::new()
        .read(true)
        .write(true)
        .open(dir.path().join(".pairings.lock"))
        .unwrap();
    lock.lock().unwrap();
    assert_eq!(store.load().err(), Some(StoreError::Busy));
    assert_eq!(
        store.set_pairing(pairing("other", 3)).err(),
        Some(StoreError::Busy)
    );
    lock.unlock().unwrap();
    assert_eq!(store.load().unwrap().pairings.len(), 1);
}
