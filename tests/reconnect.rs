//! SOFTWARE FAKE identity, observation and Session recovery. No USB/phone proof.
use cordgeist::{
    adb::{Attachment, Authorization},
    api::*,
    backend::{Cancel, Clock},
    discovery::*,
    geometry::Transform,
    identity::*,
    lease_devices::LeaseDevices,
    session::*,
    session_input::*,
};
use std::{
    sync::{
        Arc, Mutex,
        atomic::{AtomicUsize, Ordering},
    },
    time::{Duration, Instant},
};
struct FakeClock;
impl Clock for FakeClock {
    fn now_ms(&self) -> u64 {
        100
    }
}
type Gate = (
    tokio::sync::oneshot::Sender<()>,
    std::sync::mpsc::Receiver<()>,
);
struct Fake {
    mode: Arc<AtomicUsize>,
    gate: Option<Gate>,
    effects: Arc<AtomicUsize>,
}
impl SessionDriver for Fake {
    type Handle = ();
    fn open(
        &mut self,
        _: Resource,
        _: &Handles<()>,
        _: CallBudget,
        _: &Cancel,
    ) -> Result<(), Failure> {
        Ok(())
    }
    fn close(&mut self, _: Resource, _: &mut (), _: CallBudget) -> CloseOutcome {
        match self.mode.load(Ordering::SeqCst) {
            1 => CloseOutcome::Pending,
            2 => CloseOutcome::Consumed(Err(Failure::code(ErrorCode::BackendUnavailable))),
            _ => CloseOutcome::Consumed(Ok(())),
        }
    }
    fn apply_report(
        &mut self,
        _: &Transition,
        _: &Handles<()>,
        _: &Cancel,
        _: &Cancel,
    ) -> Result<(), Failure> {
        if let Some((entered, release)) = self.gate.take() {
            entered.send(()).unwrap();
            release.recv_timeout(Duration::from_millis(500)).unwrap();
        }
        Ok(())
    }
    fn submit_report(
        &mut self,
        t: &Transition,
        _: &Handles<()>,
        _: CallBudget,
        _: &Cancel,
        _: &Cancel,
    ) -> ReportResult {
        if matches!(
            t,
            Transition::Key { down: true, .. } | Transition::Contact { down: true, .. }
        ) {
            self.effects.fetch_add(1, Ordering::SeqCst);
        }
        ReportResult {
            delivery: Delivery::Sent,
            error: None,
        }
    }
}
async fn session(effects: Arc<AtomicUsize>) -> Session {
    session_config(effects, Arc::new(AtomicUsize::new(0)), None).await
}
async fn session_config(
    effects: Arc<AtomicUsize>,
    mode: Arc<AtomicUsize>,
    gate: Option<Gate>,
) -> Session {
    let s = Session::start_with_registration(
        Duration::from_secs(1),
        Registration {
            settling: Duration::ZERO,
        },
        move || {
            Ok(Fake {
                effects,
                mode,
                gate,
            })
        },
    )
    .unwrap();
    s.ready(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    s
}
fn usb(port: u8) -> UsbDevice {
    UsbDevice {
        bus: 1,
        address: 7,
        ports: vec![port],
        vendor_id: 1,
        product_id: 2,
        aoa_protocol: 2,
        serial: Some("SYNTHETIC_A".into()),
        product: None,
    }
}
fn adb(port: u8) -> Attachment {
    Attachment {
        serial: "SYNTHETIC_A".into(),
        state: Authorization::Authorized,
        transport_id: Some(8),
        usb: Some(format!("1-{port}")),
    }
}
fn snapshot(port: u8) -> DiscoverySnapshot {
    DiscoverySnapshot {
        kind: DiscoveryKind::SoftwareFake,
        captured_at: Instant::now(),
        devices: vec![usb(port)],
    }
}
fn mapping(verified: bool) -> Transform {
    Transform {
        rotation: Rotation::R0,
        max_x: 32767,
        max_y: 32767,
        verified,
        generic_screen: None,
    }
}
fn frame(auth: &LeaseAuth, id: &str) -> Frame {
    Frame {
        id: FrameId(id.into()),
        provenance: Provenance {
            boot_id: auth.boot_id.clone(),
            device_id: auth.device_id.clone(),
            lease_id: auth.lease_id.clone(),
            attachment: auth.attachment,
            fence: auth.fence,
            geometry: GeometryVersion(1),
            mapping: MappingVersion(1),
        },
        captured_ms: 100,
        image: Size {
            width: 100,
            height: 100,
        },
        display: Size {
            width: 100,
            height: 100,
        },
        rotation: Rotation::R0,
        display_id: 0,
        coordinate_space: CoordinateSpace::ReturnedImagePixels,
    }
}
fn key() -> Transition {
    Transition::Key {
        usage: 4,
        down: true,
    }
}
#[tokio::test]
async fn matching_reconnect_requires_new_grant_observation_and_rejects_old_worker_results() {
    let boot = BootId("synthetic-boot".into());
    let identities = Arc::new(Mutex::new(IdentityRegistry::new(boot.clone())));
    let config = PairingConfig::default();
    let old_route = identities
        .lock()
        .unwrap()
        .refresh(snapshot(2), &[adb(2)], &config)
        .unwrap()
        .remove(0);
    let registry = LeaseDevices::new(Arc::new(FakeClock));
    let effects = Arc::new(AtomicUsize::new(0));
    registry
        .register(
            boot,
            old_route.device_id.clone(),
            old_route.attachment,
            Fence(1),
            session(effects.clone()).await,
        )
        .unwrap();
    let old_worker = registry.worker_token(&old_route.device_id).unwrap();
    let old = registry.acquire(&old_route.device_id, 30_000).unwrap();
    let old_frame = frame(&old.auth, "old");
    assert!(matches!(
        registry.enqueue_frame(
            old.auth.clone(),
            old_frame.id.clone(),
            key(),
            Duration::from_secs(1)
        ),
        Err(ErrorCode::FrameExpired)
    ));
    let queued = registry
        .enqueue(old.auth.clone(), key(), Duration::from_secs(1))
        .unwrap();
    registry
        .disconnect(
            &old_route.device_id,
            old_route.attachment,
            Duration::from_secs(1),
        )
        .await
        .unwrap();
    assert_eq!(queued.wait().await.delivery, Delivery::NotSent);
    let new_route = identities
        .lock()
        .unwrap()
        .refresh(snapshot(2), &[adb(2)], &config)
        .unwrap()
        .remove(0);
    assert!(new_route.attachment.0 > old_route.attachment.0);
    let worker = registry
        .reconnect(
            identities.clone(),
            new_route.clone(),
            session(effects.clone()).await,
        )
        .unwrap();
    assert!(
        !registry
            .status(&new_route.device_id)
            .unwrap()
            .observation_ready
    );
    let fresh = registry.acquire(&new_route.device_id, 30_000).unwrap();
    assert!(matches!(
        registry.enqueue(fresh.auth.clone(), key(), Duration::from_secs(1)),
        Err(ErrorCode::FrameExpired)
    ));
    assert_eq!(
        registry.publish_observation(&worker, &fresh.auth, old_frame, mapping(true)),
        Err(ErrorCode::FrameExpired)
    );
    assert_eq!(
        registry.publish_observation(
            &worker,
            &fresh.auth,
            frame(&fresh.auth, "fresh"),
            mapping(false)
        ),
        Ok(())
    );
    let observed = frame(&fresh.auth, "fresh");
    registry
        .publish_observation(&worker, &fresh.auth, observed.clone(), mapping(true))
        .unwrap();
    assert!(
        registry
            .status(&new_route.device_id)
            .unwrap()
            .observation_ready
    );
    assert!(matches!(
        registry.enqueue_frame(old.auth, observed.id.clone(), key(), Duration::from_secs(1)),
        Err(ErrorCode::LeaseInvalid)
    ));
    assert!(matches!(
        registry.enqueue_frame(
            fresh.auth.clone(),
            FrameId("old".into()),
            key(),
            Duration::from_secs(1)
        ),
        Err(ErrorCode::FrameExpired)
    ));
    assert_eq!(
        registry.worker_failed(&new_route.device_id, &old_worker),
        Err(ErrorCode::LeaseInvalid)
    );
    assert_eq!(
        registry.publish_observation(&old_worker, &fresh.auth, observed.clone(), mapping(true)),
        Err(ErrorCode::LeaseInvalid)
    );
    assert!(!registry.status(&new_route.device_id).unwrap().fenced);
    assert_eq!(effects.load(Ordering::SeqCst), 0);
    assert_eq!(
        registry
            .enqueue_frame(fresh.auth, observed.id, key(), Duration::from_secs(1))
            .unwrap()
            .wait()
            .await
            .delivery,
        Delivery::Sent
    );
    registry
        .worker_failed(&new_route.device_id, &worker)
        .unwrap();
    assert!(registry.status(&new_route.device_id).unwrap().fenced);
    registry
        .shutdown(&new_route.device_id, Duration::from_secs(1))
        .await
        .unwrap();
    assert_eq!(effects.load(Ordering::SeqCst), 1);
}
#[test]
fn moved_pairing_requires_explicit_confirmation_and_ambiguous_identity_fails_closed() {
    let boot = BootId("synthetic-boot".into());
    let mut identities = IdentityRegistry::new(boot);
    let mut config = PairingConfig::default();
    config
        .set_pairing(
            Pairing::confirm(DeviceId("explicit".into()), None, &usb(2), &adb(2), false).unwrap(),
        )
        .unwrap();
    let old = identities
        .refresh(snapshot(2), &[adb(2)], &config)
        .unwrap()
        .remove(0);
    assert!(matches!(
        identities.refresh(snapshot(3), &[adb(3)], &config),
        Err(ErrorCode::DeviceIdentityAmbiguous)
    ));
    assert!(identities.resolve(&old).is_err());
    config
        .set_pairing(
            Pairing::confirm(DeviceId("explicit".into()), None, &usb(3), &adb(3), false).unwrap(),
        )
        .unwrap();
    let moved = identities
        .refresh(snapshot(3), &[adb(3)], &config)
        .unwrap()
        .remove(0);
    assert_eq!(moved.device_id.0, "explicit");
    assert!(moved.attachment.0 > old.attachment.0);
    assert!(matches!(
        identities.refresh(snapshot(3), &[adb(3), adb(3)], &config),
        Err(ErrorCode::DeviceIdentityAmbiguous)
    ));
    assert!(identities.resolve(&moved).is_err());
}

#[tokio::test]
async fn delayed_failed_worker_cannot_be_replaced_early_or_invalidate_successor_and_b_progresses() {
    let boot = BootId("synthetic-boot".into());
    let identities = Arc::new(Mutex::new(IdentityRegistry::new(boot.clone())));
    let old = identities
        .lock()
        .unwrap()
        .refresh(snapshot(2), &[adb(2)], &PairingConfig::default())
        .unwrap()
        .remove(0);
    let r = LeaseDevices::new(Arc::new(FakeClock));
    let effects = Arc::new(AtomicUsize::new(0));
    let (entered, ready) = tokio::sync::oneshot::channel();
    let (release, wait) = std::sync::mpsc::channel();
    r.register(
        boot.clone(),
        old.device_id.clone(),
        old.attachment,
        Fence(1),
        session_config(
            effects.clone(),
            Arc::new(AtomicUsize::new(0)),
            Some((entered, wait)),
        )
        .await,
    )
    .unwrap();
    let token = r.worker_token(&old.device_id).unwrap();
    let grant = r.acquire(&old.device_id, 30_000).unwrap();
    let bid = DeviceId("independent".into());
    let be = Arc::new(AtomicUsize::new(0));
    r.register(
        boot,
        bid.clone(),
        AttachmentGeneration(1),
        Fence(1),
        session(be.clone()).await,
    )
    .unwrap();
    let bg = r.acquire(&bid, 30_000).unwrap();
    let active = r
        .enqueue(grant.auth.clone(), key(), Duration::from_secs(1))
        .unwrap();
    tokio::time::timeout(Duration::from_secs(1), ready)
        .await
        .unwrap()
        .unwrap();
    let queued = r
        .enqueue(grant.auth, key(), Duration::from_secs(1))
        .unwrap();
    r.worker_failed(&old.device_id, &token).unwrap();
    assert_eq!(queued.wait().await.delivery, Delivery::NotSent);
    let route = identities
        .lock()
        .unwrap()
        .refresh(snapshot(2), &[adb(2)], &PairingConfig::default())
        .unwrap()
        .remove(0);
    let fresh = session(effects.clone()).await;
    assert!(matches!(
        r.reconnect(identities.clone(), route.clone(), fresh.clone()),
        Err(ErrorCode::BackendUnavailable)
    ));
    assert!(matches!(
        r.replace_cleaned(
            route.boot_id.clone(),
            route.device_id.clone(),
            route.attachment,
            Fence(2),
            fresh.clone()
        ),
        Err(ErrorCode::BackendUnavailable)
    ));
    assert_eq!(
        r.enqueue(bg.auth.clone(), key(), Duration::from_secs(1))
            .unwrap()
            .wait()
            .await
            .delivery,
        Delivery::Sent
    );
    release.send(()).unwrap();
    let result = active.wait().await;
    assert!(matches!(
        result.delivery,
        Delivery::NotSent | Delivery::Unknown
    ));
    r.shutdown(&old.device_id, Duration::from_secs(1))
        .await
        .unwrap();
    assert_eq!(effects.load(Ordering::SeqCst), 0);
    let current = r.reconnect(identities, route.clone(), fresh).unwrap();
    assert_eq!(
        r.worker_failed(&route.device_id, &token),
        Err(ErrorCode::LeaseInvalid)
    );
    assert!(!r.status(&route.device_id).unwrap().fenced);
    assert!(!r.status(&route.device_id).unwrap().observation_ready);
    let new = r.acquire(&route.device_id, 30_000).unwrap();
    let observed = frame(&new.auth, "new");
    r.publish_observation(&current, &new.auth, observed.clone(), mapping(true))
        .unwrap();
    assert_eq!(
        r.enqueue_frame(new.auth, observed.id, key(), Duration::from_secs(1))
            .unwrap()
            .wait()
            .await
            .delivery,
        Delivery::Sent
    );
    r.shutdown(&route.device_id, Duration::from_secs(1))
        .await
        .unwrap();
    r.shutdown(&bid, Duration::from_secs(1)).await.unwrap();
    assert_eq!(be.load(Ordering::SeqCst), 1);
    assert_eq!(effects.load(Ordering::SeqCst), 1);
}
#[tokio::test]
async fn unresolved_or_failed_cleanup_keeps_reconnect_uncontrollable() {
    let boot = BootId("synthetic-boot".into());
    let identities = Arc::new(Mutex::new(IdentityRegistry::new(boot.clone())));
    let old = identities
        .lock()
        .unwrap()
        .refresh(snapshot(2), &[adb(2)], &PairingConfig::default())
        .unwrap()
        .remove(0);
    let r = LeaseDevices::new(Arc::new(FakeClock));
    let effects = Arc::new(AtomicUsize::new(0));
    let mode = Arc::new(AtomicUsize::new(1));
    r.register(
        boot,
        old.device_id.clone(),
        old.attachment,
        Fence(1),
        session_config(effects.clone(), mode.clone(), None).await,
    )
    .unwrap();
    let token = r.worker_token(&old.device_id).unwrap();
    r.worker_failed(&old.device_id, &token).unwrap();
    assert!(matches!(
        r.shutdown(&old.device_id, Duration::from_millis(10)).await,
        Err(ErrorCode::NativeCleanupPending)
    ));
    let route = identities
        .lock()
        .unwrap()
        .refresh(snapshot(2), &[adb(2)], &PairingConfig::default())
        .unwrap()
        .remove(0);
    let fresh = session(effects.clone()).await;
    assert!(matches!(
        r.reconnect(identities.clone(), route.clone(), fresh.clone()),
        Err(ErrorCode::BackendUnavailable)
    ));
    mode.store(2, Ordering::SeqCst);
    let closed = r
        .shutdown(&old.device_id, Duration::from_secs(1))
        .await
        .unwrap();
    assert!(closed.cleanup_error.is_some());
    assert!(matches!(
        r.reconnect(identities, route, fresh.clone()),
        Err(ErrorCode::BackendUnavailable)
    ));
    assert!(matches!(
        r.acquire(&old.device_id, 30_000),
        Err(ErrorCode::BackendUnavailable)
    ));
    fresh.shutdown(Duration::from_secs(1)).await.unwrap();
    assert_eq!(effects.load(Ordering::SeqCst), 0);
}

#[tokio::test]
async fn moved_and_ambiguous_evidence_gate_the_public_reconnect_path() {
    let boot = BootId("synthetic-boot".into());
    let identities = Arc::new(Mutex::new(IdentityRegistry::new(boot.clone())));
    let mut config = PairingConfig::default();
    config
        .set_pairing(
            Pairing::confirm(DeviceId("explicit".into()), None, &usb(2), &adb(2), false).unwrap(),
        )
        .unwrap();
    let old = identities
        .lock()
        .unwrap()
        .refresh(snapshot(2), &[adb(2)], &config)
        .unwrap()
        .remove(0);
    let r = LeaseDevices::new(Arc::new(FakeClock));
    let effects = Arc::new(AtomicUsize::new(0));
    r.register(
        boot,
        old.device_id.clone(),
        old.attachment,
        Fence(1),
        session(effects.clone()).await,
    )
    .unwrap();
    let original = r.acquire(&old.device_id, 30_000).unwrap();
    r.disconnect(&old.device_id, old.attachment, Duration::from_secs(1))
        .await
        .unwrap();
    let fresh = session(effects.clone()).await;
    assert!(matches!(
        identities
            .lock()
            .unwrap()
            .refresh(snapshot(3), &[adb(3)], &config),
        Err(ErrorCode::DeviceIdentityAmbiguous)
    ));
    assert!(
        r.reconnect(identities.clone(), old.clone(), fresh.clone())
            .is_err()
    );
    assert_eq!(effects.load(Ordering::SeqCst), 0);
    config
        .set_pairing(
            Pairing::confirm(old.device_id.clone(), None, &usb(3), &adb(3), false).unwrap(),
        )
        .unwrap();
    let moved = identities
        .lock()
        .unwrap()
        .refresh(snapshot(3), &[adb(3)], &config)
        .unwrap()
        .remove(0);
    let worker = r
        .reconnect(identities.clone(), moved.clone(), fresh)
        .unwrap();
    let owner = r.acquire(&moved.device_id, 30_000).unwrap();
    assert!(matches!(
        r.enqueue_frame(
            original.auth,
            FrameId("old".into()),
            key(),
            Duration::from_secs(1)
        ),
        Err(ErrorCode::LeaseInvalid)
    ));
    assert!(matches!(
        r.enqueue_frame(
            owner.auth.clone(),
            FrameId("new".into()),
            key(),
            Duration::from_secs(1)
        ),
        Err(ErrorCode::FrameExpired)
    ));
    let observed = frame(&owner.auth, "new");
    r.publish_observation(&worker, &owner.auth, observed.clone(), mapping(true))
        .unwrap();
    assert!(matches!(
        identities
            .lock()
            .unwrap()
            .refresh(snapshot(3), &[adb(3), adb(3)], &config),
        Err(ErrorCode::DeviceIdentityAmbiguous)
    ));
    assert!(
        r.enqueue_frame(owner.auth, observed.id, key(), Duration::from_secs(1))
            .is_err()
    );
    assert!(!r.status(&moved.device_id).unwrap().observation_ready);
    assert_eq!(effects.load(Ordering::SeqCst), 0);
    r.worker_failed(&moved.device_id, &worker).unwrap();
    r.shutdown(&moved.device_id, Duration::from_secs(1))
        .await
        .unwrap();
}

#[tokio::test]
async fn identity_change_after_apply_guard_prevents_submission_and_fences_queued_work() {
    let boot = BootId("synthetic-boot".into());
    let identities = Arc::new(Mutex::new(IdentityRegistry::new(boot.clone())));
    let old = identities
        .lock()
        .unwrap()
        .refresh(snapshot(2), &[adb(2)], &PairingConfig::default())
        .unwrap()
        .remove(0);
    let r = LeaseDevices::new(Arc::new(FakeClock));
    let effects = Arc::new(AtomicUsize::new(0));
    r.register(
        boot,
        old.device_id.clone(),
        old.attachment,
        Fence(1),
        session(effects.clone()).await,
    )
    .unwrap();
    r.disconnect(&old.device_id, old.attachment, Duration::from_secs(1))
        .await
        .unwrap();
    let route = identities
        .lock()
        .unwrap()
        .refresh(snapshot(2), &[adb(2)], &PairingConfig::default())
        .unwrap()
        .remove(0);
    let (entered, ready) = tokio::sync::oneshot::channel();
    let (release, wait) = std::sync::mpsc::channel();
    let worker = r
        .reconnect(
            identities.clone(),
            route.clone(),
            session_config(
                effects.clone(),
                Arc::new(AtomicUsize::new(0)),
                Some((entered, wait)),
            )
            .await,
        )
        .unwrap();
    let owner = r.acquire(&route.device_id, 30_000).unwrap();
    let observed = frame(&owner.auth, "fresh");
    r.publish_observation(&worker, &owner.auth, observed.clone(), mapping(true))
        .unwrap();
    let active = r
        .enqueue_frame(
            owner.auth.clone(),
            observed.id.clone(),
            key(),
            Duration::from_secs(1),
        )
        .unwrap();
    tokio::time::timeout(Duration::from_secs(1), ready)
        .await
        .unwrap()
        .unwrap();
    let queued = r
        .enqueue_frame(owner.auth, observed.id, key(), Duration::from_secs(1))
        .unwrap();
    assert!(
        identities
            .lock()
            .unwrap()
            .refresh(snapshot(2), &[adb(2), adb(2)], &PairingConfig::default())
            .is_err()
    );
    release.send(()).unwrap();
    assert_eq!(active.wait().await.delivery, Delivery::NotSent);
    assert_eq!(queued.wait().await.delivery, Delivery::NotSent);
    assert!(r.status(&route.device_id).unwrap().fenced);
    assert_eq!(effects.load(Ordering::SeqCst), 0);
    r.shutdown(&route.device_id, Duration::from_secs(1))
        .await
        .unwrap();
}

struct ObservationClock(std::sync::atomic::AtomicU64);
impl Clock for ObservationClock {
    fn now_ms(&self) -> u64 {
        self.0.load(Ordering::SeqCst)
    }
}
async fn observation_replacement_case(case: u8) {
    let boot = BootId("synthetic-boot".into());
    let ids = Arc::new(Mutex::new(IdentityRegistry::new(boot.clone())));
    let old = ids
        .lock()
        .unwrap()
        .refresh(snapshot(2), &[adb(2)], &PairingConfig::default())
        .unwrap()
        .remove(0);
    let clock = Arc::new(ObservationClock(std::sync::atomic::AtomicU64::new(100)));
    let r = LeaseDevices::new(clock.clone());
    let effects = Arc::new(AtomicUsize::new(0));
    r.register(
        boot,
        old.device_id.clone(),
        old.attachment,
        Fence(1),
        session(effects.clone()).await,
    )
    .unwrap();
    r.disconnect(&old.device_id, old.attachment, Duration::from_secs(1))
        .await
        .unwrap();
    let route = ids
        .lock()
        .unwrap()
        .refresh(snapshot(2), &[adb(2)], &PairingConfig::default())
        .unwrap()
        .remove(0);
    let (entered, ready) = tokio::sync::oneshot::channel();
    let (release, wait) = std::sync::mpsc::channel();
    let gate = if case == 3 {
        Some((entered, wait))
    } else {
        None
    };
    let worker = r
        .reconnect(
            ids,
            route.clone(),
            session_config(effects.clone(), Arc::new(AtomicUsize::new(0)), gate).await,
        )
        .unwrap();
    let grant = r.acquire(&route.device_id, 3_600_000).unwrap();
    let first = frame(&grant.auth, "A");
    r.publish_observation(&worker, &grant.auth, first.clone(), mapping(true))
        .unwrap();
    let ticket = r
        .enqueue_frame(
            grant.auth.clone(),
            first.id.clone(),
            Transition::Contact {
                down: true,
                x: 1,
                y: 2,
            },
            Duration::from_secs(1),
        )
        .unwrap();
    if case == 3 {
        tokio::time::timeout(Duration::from_secs(1), ready)
            .await
            .unwrap()
            .unwrap();
    }
    if case != 4 {
        let mut replacement = first.clone();
        let mut transform = mapping(true);
        if case == 1 {
            clock.0.store(30_101, Ordering::SeqCst);
            replacement.captured_ms = 30_101;
        } else {
            replacement.image.width = 200;
            replacement.provenance.geometry = GeometryVersion(2);
            replacement.provenance.mapping = MappingVersion(2);
            transform.max_x = 16383;
        }
        if case == 2 {
            replacement.id = FrameId("B".into());
        }
        r.publish_observation(&worker, &grant.auth, replacement, transform)
            .unwrap();
        if case == 2 {
            r.publish_observation(&worker, &grant.auth, first, mapping(true))
                .unwrap();
        }
    }
    if case == 3 {
        release.send(()).unwrap();
    }
    let result = ticket.wait().await;
    assert_eq!(r.status(&route.device_id).unwrap().fenced, case != 4);
    r.shutdown(&route.device_id, Duration::from_secs(1))
        .await
        .unwrap();
    assert_eq!(
        effects.load(Ordering::SeqCst),
        usize::from(case == 4),
        "SOFTWARE FAKE observation replacement must not retarget admitted work"
    );
    assert_eq!(
        result.delivery,
        if case == 4 {
            Delivery::Sent
        } else {
            Delivery::NotSent
        }
    );
}
#[tokio::test]
async fn same_id_metadata_replacement_cannot_retarget_queued_work() {
    observation_replacement_case(0).await;
}
#[tokio::test]
async fn fresh_same_id_cannot_refresh_expired_original_age() {
    observation_replacement_case(1).await;
}
#[tokio::test]
async fn a_b_a_identifier_reuse_cannot_restore_queued_authority() {
    observation_replacement_case(2).await;
}
#[tokio::test]
async fn observation_replaced_between_apply_and_submit_has_zero_effects() {
    observation_replacement_case(3).await;
}
#[tokio::test]
async fn unchanged_observation_still_executes_successfully() {
    observation_replacement_case(4).await;
}
