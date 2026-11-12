#![cfg(target_os = "linux")]
//! Public orchestration with a SOFTWARE FAKE USB driver and fake ADB executable.
//! Uses real parsing, exact argv, owner worker, private store and identity registry.
mod common;
#[path = "support/fixtures.rs"]
mod fixtures;
use cordgeist::{
    adb::{Adb, Attachment, Authorization},
    api::{DeviceId, ErrorCode},
    backend::Cancel,
    discovery::*,
    discovery_service::DiscoveryService,
    identity::Pairing,
    pairing_store::PairingStore,
};
use std::{
    fs,
    rc::Rc,
    time::{Duration, Instant},
};
fn device() -> UsbDevice {
    UsbDevice {
        bus: 1,
        address: 7,
        ports: vec![2],
        vendor_id: 1,
        product_id: 2,
        aoa_protocol: 2,
        serial: None,
        product: None,
    }
}
struct FakeUsb(Rc<()>);
impl DiscoveryDriver for FakeUsb {
    fn kind(&self) -> DiscoveryKind {
        DiscoveryKind::SoftwareFake
    }
    fn scan(&mut self, _: Instant, _: &Cancel) -> Result<Vec<UsbDevice>, ErrorCode> {
        let _ = &self.0;
        Ok(vec![device()])
    }
    fn close(&mut self) -> CloseStatus {
        CloseStatus::Consumed(Ok(()))
    }
}
fn directory() -> tempfile::TempDir {
    fixtures::private_tempdir()
}
fn setup() -> (tempfile::TempDir, DiscoveryService<Adb>, PairingStore) {
    let dir = directory();
    let path = dir.path().join("fake adb ; discovery");
    common::python(
        &path,
        r#"#!/usr/bin/python3
import sys,pathlib,time,os
assert sys.argv[1:]==['-H','127.0.0.1','-P','5039','devices','-l'], 'unexpected non-inventory command'
base=pathlib.Path(__file__)
mode=base.with_suffix('.mode').read_text() if base.with_suffix('.mode').exists() else 'stable'
counter=base.with_suffix('.count')
n=int(counter.read_text())+1 if counter.exists() else 1
counter.write_text(str(n))
if mode=='slow':
 base.with_suffix('.pid').write_text(str(os.getpid()))
 time.sleep(2)
if mode=='invalid':print('malformed inventory')
else:
 t=99 if mode=='swap' and n%2==0 else 8
 print('List of devices attached\nSYNTHETIC_ADB device usb:1-2 transport_id:'+str(t)+'\n')
"#,
    );
    let store = PairingStore::open(dir.path()).unwrap();
    let adb = Attachment {
        serial: "SYNTHETIC_ADB".into(),
        state: Authorization::Authorized,
        transport_id: Some(8),
        usb: Some("1-2".into()),
    };
    store
        .set_pairing(
            Pairing::confirm(
                DeviceId("explicit_phone".into()),
                Some("human-label".into()),
                &device(),
                &adb,
                true,
            )
            .unwrap(),
        )
        .unwrap();
    let worker = DiscoveryWorker::start(|| Ok(FakeUsb(Rc::new(())))).unwrap();
    let service = DiscoveryService::new(
        worker,
        Adb::new(path, 5039).unwrap(),
        PairingStore::open(dir.path()).unwrap(),
    )
    .unwrap();
    (dir, service, store)
}
#[tokio::test]
async fn public_refresh_uses_persisted_pairing_and_exact_targets_and_fences_old_work() {
    let (_dir, mut service, store) = setup();
    let cancel = Cancel::default();
    let first = service
        .refresh(Duration::from_secs(2), &cancel)
        .await
        .unwrap()
        .remove(0);
    assert_eq!(first.device_id.0, "explicit_phone");
    assert_eq!(
        service.id_for_alias("human-label").unwrap(),
        first.device_id
    );
    let route = service.resolve(&first).unwrap();
    assert_eq!(route.kind(), DiscoveryKind::SoftwareFake);
    assert_eq!(route.target().transport_id, 8);
    let fresh = service
        .refresh(Duration::from_secs(2), &cancel)
        .await
        .unwrap()
        .remove(0);
    assert!(matches!(
        service.resolve(&first),
        Err(ErrorCode::LeaseInvalid)
    ));
    assert!(fresh.attachment.0 > first.attachment.0);
    store
        .set_alias(&first.device_id, Some("new-label".into()))
        .unwrap();
    service
        .reload_pairings(Duration::from_secs(1))
        .await
        .unwrap();
    assert!(service.resolve(&fresh).is_err());
    assert_eq!(service.id_for_alias("new-label").unwrap(), first.device_id);
    assert!(service.id_for_alias("human-label").is_err());
    let final_ref = service
        .refresh(Duration::from_secs(2), &cancel)
        .await
        .unwrap()
        .remove(0);
    service.shutdown(Duration::from_secs(1)).await.unwrap();
    assert!(service.resolve(&final_ref).is_err());
}
#[tokio::test]
async fn inventory_swap_malformed_output_and_bad_store_never_retain_old_routes() {
    for mode in ["swap", "invalid", "bad-store"] {
        let (dir, mut service, _) = setup();
        let cancel = Cancel::default();
        let first = service
            .refresh(Duration::from_secs(2), &cancel)
            .await
            .unwrap()
            .remove(0);
        if mode == "bad-store" {
            fs::write(dir.path().join("pairings.json"), b"malformed config").unwrap();
        } else {
            fs::write(dir.path().join("fake adb ; discovery.mode"), mode).unwrap();
        }
        assert!(
            service
                .refresh(Duration::from_secs(2), &cancel)
                .await
                .is_err()
        );
        assert!(service.resolve(&first).is_err());
        service.shutdown(Duration::from_secs(1)).await.unwrap();
    }
}
#[tokio::test]
async fn cancellation_and_restart_never_reuse_old_route_authority() {
    let (_dir, mut service, _) = setup();
    let cancel = Cancel::default();
    let old = service
        .refresh(Duration::from_secs(2), &cancel)
        .await
        .unwrap()
        .remove(0);
    cancel.cancel();
    assert!(matches!(
        service.refresh(Duration::from_secs(2), &cancel).await,
        Err(ErrorCode::ProcessCancelled)
    ));
    assert!(service.resolve(&old).is_err());
    service.shutdown(Duration::from_secs(1)).await.unwrap();
    let (_otherdir, mut restarted, _) = setup();
    let fresh = restarted
        .refresh(Duration::from_secs(2), &Cancel::default())
        .await
        .unwrap()
        .remove(0);
    assert_ne!(old.boot_id, fresh.boot_id);
    assert!(matches!(
        restarted.resolve(&old),
        Err(ErrorCode::LeaseInvalid)
    ));
    restarted.shutdown(Duration::from_secs(1)).await.unwrap();
}

#[tokio::test]
async fn global_deadline_kills_inventory_and_invalidates_prior_routes() {
    let (dir, mut service, _) = setup();
    let old = service
        .refresh(Duration::from_secs(2), &Cancel::default())
        .await
        .unwrap()
        .remove(0);
    fs::write(dir.path().join("fake adb ; discovery.mode"), "slow").unwrap();
    assert!(matches!(
        service
            .refresh(Duration::from_millis(250), &Cancel::default())
            .await,
        Err(ErrorCode::ProcessTimeout)
    ));
    assert!(service.resolve(&old).is_err());
    let pid = fs::read_to_string(dir.path().join("fake adb ; discovery.pid")).unwrap();
    tokio::time::timeout(Duration::from_secs(1), async {
        while std::path::Path::new(&format!("/proc/{pid}")).exists() {
            tokio::time::sleep(Duration::from_millis(5)).await;
        }
    })
    .await
    .unwrap();
    fs::write(dir.path().join("fake adb ; discovery.mode"), "stable").unwrap();
    let new = service
        .refresh(Duration::from_secs(2), &Cancel::default())
        .await
        .unwrap()
        .remove(0);
    assert!(new.attachment.0 > old.attachment.0);
    service.shutdown(Duration::from_secs(1)).await.unwrap();
}

#[test]
fn ready_pairing_reload_consumed_after_deadline_never_publishes() {
    // SOFTWARE FAKE USB/ADB, real private filesystem store. A sole blocking
    // worker makes the marker prove the queued load completed before resumption.
    let runtime = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .max_blocking_threads(1)
        .build()
        .unwrap();
    runtime.block_on(async {
        let (_dir, mut service, store) = setup();
        let old = service
            .refresh(Duration::from_secs(2), &Cancel::default())
            .await
            .unwrap()
            .remove(0);
        store
            .set_alias(&old.device_id, Some("late-label".into()))
            .unwrap();
        let (entered, enter) = std::sync::mpsc::channel();
        let (release, wait) = std::sync::mpsc::channel();
        tokio::task::spawn_blocking(move || {
            entered.send(()).unwrap();
            wait.recv_timeout(Duration::from_secs(2)).unwrap();
        });
        enter.recv_timeout(Duration::from_secs(2)).unwrap();
        let mut reload = Box::pin(service.reload_pairings(Duration::from_millis(100)));
        let first = std::future::poll_fn(|cx| {
            std::task::Poll::Ready(std::future::Future::poll(reload.as_mut(), cx))
        })
        .await;
        assert!(first.is_pending());
        let (ready, receive) = std::sync::mpsc::channel();
        tokio::task::spawn_blocking(move || ready.send(()).unwrap());
        release.send(()).unwrap();
        // Deliberately prevent executor polling while the real load completes.
        receive.recv_timeout(Duration::from_secs(2)).unwrap();
        std::thread::sleep(Duration::from_millis(150));
        assert_eq!(reload.await, Err(ErrorCode::ProcessTimeout));
        assert!(service.id_for_alias("late-label").is_err());
        assert!(service.id_for_alias("human-label").is_err());
        assert!(service.resolve(&old).is_err());
        service.shutdown(Duration::from_secs(1)).await.unwrap();
    });
}
