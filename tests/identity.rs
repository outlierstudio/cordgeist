//! SOFTWARE FAKE inventory only. Exercises public identity paths, never phone input.
use cordgeist::{
    adb::{Attachment, Authorization},
    api::{BootId, DeviceId, ErrorCode},
    discovery::*,
    identity::*,
};
use std::time::Instant;
fn usb(serial: Option<&str>, port: u8, address: u8) -> UsbDevice {
    UsbDevice {
        bus: 1,
        address,
        ports: vec![port],
        vendor_id: 1,
        product_id: 2,
        aoa_protocol: 2,
        serial: serial.map(str::to_owned),
        product: None,
    }
}
fn adb(serial: &str, port: u8, transport: u64) -> Attachment {
    Attachment {
        serial: serial.into(),
        state: Authorization::Authorized,
        transport_id: Some(transport),
        usb: Some(format!("1-{port}")),
    }
}
fn snapshot(devices: Vec<UsbDevice>) -> DiscoverySnapshot {
    DiscoverySnapshot {
        kind: DiscoveryKind::SoftwareFake,
        captured_at: Instant::now(),
        devices,
    }
}
fn registry() -> IdentityRegistry {
    IdentityRegistry::new(BootId("synthetic-boot".into()))
}
#[test]
fn exact_routes_survive_reordering_but_each_refresh_fences_all_old_references() {
    let mut r = registry();
    let config = PairingConfig::default();
    let first = r
        .refresh(
            snapshot(vec![
                usb(Some("SYNTHETIC_A"), 2, 7),
                usb(Some("SYNTHETIC_B"), 3, 8),
            ]),
            &[adb("SYNTHETIC_B", 3, 9), adb("SYNTHETIC_A", 2, 8)],
            &config,
        )
        .unwrap();
    let a = first
        .iter()
        .find(|v| r.resolve(v).unwrap().target().transport_id == 8)
        .unwrap()
        .clone();
    let next = r
        .refresh(
            snapshot(vec![
                usb(Some("SYNTHETIC_B"), 3, 8),
                usb(Some("SYNTHETIC_A"), 2, 7),
            ]),
            &[adb("SYNTHETIC_A", 2, 8), adb("SYNTHETIC_B", 3, 9)],
            &config,
        )
        .unwrap();
    assert!(matches!(r.resolve(&a), Err(ErrorCode::LeaseInvalid)));
    let fresh = next.iter().find(|v| v.device_id == a.device_id).unwrap();
    assert!(fresh.attachment.0 > a.attachment.0);
    assert_eq!(r.resolve(fresh).unwrap().target().transport_id, 8);
    let mut old_boot = fresh.clone();
    old_boot.boot_id = BootId("other".into());
    assert!(r.resolve(&old_boot).is_err());
    r.invalidate().unwrap();
    assert!(r.resolve(fresh).is_err());
}
#[test]
fn duplicate_and_blank_serials_require_explicit_exact_port_pairing() {
    for serial in [Some("SYNTHETIC_DUPLICATE"), None, Some("")] {
        let devices = vec![usb(serial, 2, 7), usb(serial, 3, 8)];
        let adb_serial = serial
            .filter(|s| !s.is_empty())
            .unwrap_or("SYNTHETIC_ADB_DUPLICATE");
        let attachments = vec![adb(adb_serial, 2, 8), adb(adb_serial, 3, 9)];
        let mut r = registry();
        assert!(
            r.refresh(
                snapshot(devices.clone()),
                &attachments,
                &PairingConfig::default()
            )
            .is_err()
        );
        let mut config = PairingConfig::default();
        for i in 0..2 {
            config
                .set_pairing(
                    Pairing::confirm(
                        DeviceId(format!("phone_{i}")),
                        Some("same-human-label".into()),
                        &devices[i],
                        &attachments[i],
                        true,
                    )
                    .unwrap(),
                )
                .unwrap();
        }
        assert!(config.id_for_alias("same-human-label").is_err()); // labels never choose first
        let refs = r.refresh(snapshot(devices), &attachments, &config).unwrap();
        assert_eq!(refs.len(), 2);
        for reference in refs {
            let expected = if reference.device_id.0 == "phone_0" {
                8
            } else {
                9
            };
            assert_eq!(
                r.resolve(&reference).unwrap().target().transport_id,
                expected
            );
        }
    }
}
#[test]
fn failed_refresh_discards_previous_routes_and_moved_pairings_are_not_guessed() {
    let u = usb(Some("SYNTHETIC_A"), 2, 7);
    let a = adb("SYNTHETIC_A", 2, 8);
    let pairing = Pairing::confirm(
        DeviceId("explicit-phone".into()),
        Some("human-label".into()),
        &u,
        &a,
        false,
    )
    .unwrap();
    let mut config = PairingConfig::default();
    config.set_pairing(pairing).unwrap();
    let mut r = registry();
    let first = r
        .refresh(snapshot(vec![u]), &[a], &config)
        .unwrap()
        .remove(0);
    assert_eq!(
        config.id_for_alias("human-label").unwrap().0,
        "explicit-phone"
    );
    assert!(
        r.refresh(
            snapshot(vec![usb(Some("SYNTHETIC_A"), 3, 9)]),
            &[adb("SYNTHETIC_A", 3, 10)],
            &config
        )
        .is_err()
    );
    assert!(r.resolve(&first).is_err());
    let refreshed = r
        .refresh(
            snapshot(vec![usb(Some("SYNTHETIC_A"), 2, 11)]),
            &[adb("SYNTHETIC_A", 2, 99)],
            &config,
        )
        .unwrap()
        .remove(0);
    assert!(refreshed.attachment.0 > first.attachment.0);
    assert_eq!(r.resolve(&refreshed).unwrap().target().transport_id, 99);
}
#[test]
fn disagreements_unauthorized_duplicate_targets_and_stale_evidence_fail_closed() {
    for mode in 0..6 {
        let mut r = registry();
        let mut a = adb("SYNTHETIC_A", 2, 8);
        let mut snap = snapshot(vec![usb(Some("SYNTHETIC_A"), 2, 7)]);
        let mut attachments = vec![];
        match mode {
            0 => a.serial = "SYNTHETIC_OTHER".into(),
            1 => a.state = Authorization::Unauthorized,
            2 => a.usb = None,
            3 => snap.captured_at -= ROUTE_MAX_AGE + std::time::Duration::from_secs(1),
            4 => attachments.push(adb("SYNTHETIC_B", 3, 8)),
            _ => snap.devices.push(usb(Some("SYNTHETIC_A"), 2, 9)),
        }
        attachments.push(a);
        assert!(
            r.refresh(snap, &attachments, &PairingConfig::default())
                .is_err()
        );
    }
}
#[test]
fn alias_changes_do_not_change_identity_and_invalid_pairings_do_not_modify_config() {
    let u = usb(Some("SYNTHETIC_A"), 2, 7);
    let a = adb("SYNTHETIC_A", 2, 8);
    let id = DeviceId("phone".into());
    let mut c = PairingConfig::default();
    c.set_pairing(Pairing::confirm(id.clone(), Some("first".into()), &u, &a, false).unwrap())
        .unwrap();
    c.set_alias(&id, Some("second".into())).unwrap();
    assert!(c.id_for_alias("first").is_err());
    assert_eq!(c.id_for_alias("second").unwrap(), id);
    let before = serde_json::to_value(&c).unwrap();
    assert!(c.set_alias(&id, Some("invalid\nlabel".into())).is_err());
    assert_eq!(serde_json::to_value(&c).unwrap(), before);
    let mut disagreement = adb("SYNTHETIC_OTHER", 2, 8);
    assert!(Pairing::confirm(id.clone(), None, &u, &disagreement, true).is_err());
    disagreement.serial = "SYNTHETIC_A".into();
    assert!(Pairing::confirm(id, None, &usb(None, 2, 7), &disagreement, false).is_err());
}
