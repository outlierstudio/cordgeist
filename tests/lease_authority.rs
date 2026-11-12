//! SOFTWARE FAKE lease authority/Session tests. No USB, IPC or private phone data.
use cordgeist::{
    api::*,
    backend::{Cancel, Clock},
    lease_devices::LeaseDevices,
    session::*,
    session_input::*,
};
use std::{
    sync::{
        Arc,
        atomic::{AtomicU64, AtomicUsize, Ordering},
        mpsc,
    },
    time::Duration,
};
#[derive(Default)]
struct FakeClock(AtomicU64);
impl Clock for FakeClock {
    fn now_ms(&self) -> u64 {
        self.0.load(Ordering::SeqCst)
    }
}
type Gate = (tokio::sync::oneshot::Sender<()>, mpsc::Receiver<()>);
struct Fake {
    close_mode: Arc<AtomicUsize>,
    effects: Arc<AtomicUsize>,
    gate: Option<Gate>,
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
        match self.close_mode.load(Ordering::SeqCst) {
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
        if let Some((entered, wait)) = self.gate.take() {
            entered.send(()).unwrap();
            wait.recv_timeout(Duration::from_millis(500)).unwrap();
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
        if matches!(t, Transition::Key { down: true, .. }) {
            self.effects.fetch_add(1, Ordering::SeqCst);
        }
        ReportResult {
            delivery: Delivery::Sent,
            error: None,
        }
    }
}
async fn register(registry: &LeaseDevices, name: &str, gate: Option<Gate>) -> Arc<AtomicUsize> {
    register_mode(registry, name, gate, Arc::new(AtomicUsize::new(0))).await
}
async fn register_mode(
    registry: &LeaseDevices,
    name: &str,
    gate: Option<Gate>,
    close_mode: Arc<AtomicUsize>,
) -> Arc<AtomicUsize> {
    let effects = Arc::new(AtomicUsize::new(0));
    let e = effects.clone();
    let session = Session::start_with_registration(
        Duration::from_secs(1),
        Registration {
            settling: Duration::ZERO,
        },
        move || {
            Ok(Fake {
                effects: e,
                gate,
                close_mode,
            })
        },
    )
    .unwrap();
    session
        .ready(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    registry
        .register(
            BootId("synthetic-boot".into()),
            DeviceId(name.into()),
            AttachmentGeneration(1),
            Fence(1),
            session,
        )
        .unwrap();
    effects
}
fn key() -> Transition {
    Transition::Key {
        usage: 4,
        down: true,
    }
}
fn device() -> DeviceId {
    DeviceId("synthetic-a".into())
}
#[tokio::test]
async fn public_concurrent_acquire_has_one_owner_and_bad_authority_sends_nothing() {
    let clock = Arc::new(FakeClock::default());
    let registry = Arc::new(LeaseDevices::new(clock));
    let effects = register(&registry, &device().0, None).await;
    let barrier = Arc::new(std::sync::Barrier::new(8));
    let mut threads = Vec::new();
    for _ in 0..8 {
        let r = registry.clone();
        let b = barrier.clone();
        threads.push(std::thread::spawn(move || {
            b.wait();
            r.acquire(&device(), 30_000)
        }));
    }
    let mut owners = Vec::new();
    for t in threads {
        match t.join().unwrap() {
            Ok(grant) => owners.push(grant),
            Err(e) => assert_eq!(e, ErrorCode::Leased),
        }
    }
    assert_eq!(owners.len(), 1);
    let grant = owners.pop().unwrap();
    assert_eq!(grant.auth.credential.0.len(), 64);
    let public_status = format!("{:?}", registry.status(&device()).unwrap());
    assert!(!public_status.contains(&grant.auth.credential.0));
    assert!(!public_status.contains(&grant.auth.lease_id.0));
    for variant in 0..5 {
        let mut bad = grant.auth.clone();
        match variant {
            0 => bad.credential = Credential("invalid".into()),
            1 => bad.boot_id = BootId("stale".into()),
            2 => bad.lease_id = LeaseId("stale".into()),
            3 => bad.attachment = AttachmentGeneration(2),
            _ => bad.fence = Fence(2),
        }
        assert!(matches!(
            registry.enqueue(bad, key(), Duration::from_secs(1)),
            Err(ErrorCode::LeaseInvalid)
        ));
    }
    assert_eq!(effects.load(Ordering::SeqCst), 0);
    assert_eq!(
        registry
            .enqueue(grant.auth, key(), Duration::from_secs(1))
            .unwrap()
            .wait()
            .await
            .delivery,
        Delivery::Sent
    );
    registry
        .shutdown(&device(), Duration::from_secs(1))
        .await
        .unwrap();
}
#[tokio::test]
async fn valid_refresh_extends_idle_but_unauthenticated_traffic_does_not() {
    let clock = Arc::new(FakeClock::default());
    let registry = LeaseDevices::new(clock.clone());
    register(&registry, &device().0, None).await;
    let grant = registry.acquire(&device(), 30_000).unwrap();
    clock.0.store(29_000, Ordering::SeqCst);
    registry.refresh(&grant.auth).unwrap();
    let mut bad = grant.auth.clone();
    bad.credential = Credential("invalid".into());
    clock.0.store(58_999, Ordering::SeqCst);
    assert_eq!(registry.refresh(&bad), Err(ErrorCode::LeaseInvalid));
    clock.0.store(59_000, Ordering::SeqCst);
    assert!(registry.refresh(&grant.auth).is_err());
    assert!(registry.status(&device()).unwrap().fenced);
    assert!(matches!(
        registry.acquire(&device(), 30_000),
        Err(ErrorCode::BackendUnavailable)
    ));
    registry
        .shutdown(&device(), Duration::from_secs(1))
        .await
        .unwrap();
}
#[tokio::test]
async fn healthy_active_work_outlives_idle_and_other_device_progresses() {
    let clock = Arc::new(FakeClock::default());
    let registry = LeaseDevices::new(clock.clone());
    let (entered, ready) = tokio::sync::oneshot::channel();
    let (release, wait) = mpsc::channel();
    register(&registry, &device().0, Some((entered, wait))).await;
    let b = DeviceId("synthetic-b".into());
    register(&registry, &b.0, None).await;
    let a = registry.acquire(&device(), 30_000).unwrap();
    let bg = registry.acquire(&b, 30_000).unwrap();
    let active = registry
        .enqueue(a.auth.clone(), key(), Duration::from_secs(1))
        .unwrap();
    tokio::time::timeout(Duration::from_secs(1), ready)
        .await
        .unwrap()
        .unwrap();
    assert_eq!(
        registry
            .enqueue(bg.auth.clone(), key(), Duration::from_secs(1))
            .unwrap()
            .wait()
            .await
            .delivery,
        Delivery::Sent
    );
    clock.0.store(60_000, Ordering::SeqCst);
    assert!(!registry.status(&device()).unwrap().fenced);
    registry.refresh(&a.auth).unwrap();
    // B's independent idle lease expired; A remains healthy and authoritative.
    assert!(registry.refresh(&bg.auth).is_err());
    release.send(()).unwrap();
    assert_eq!(active.wait().await.delivery, Delivery::Sent);
    clock.0.store(89_999, Ordering::SeqCst);
    registry.refresh(&a.auth).unwrap();
    registry
        .shutdown(&device(), Duration::from_secs(1))
        .await
        .unwrap();
    registry.shutdown(&b, Duration::from_secs(1)).await.unwrap();
}
#[tokio::test]
async fn presend_guard_and_queued_invalidation_prevent_effects() {
    let clock = Arc::new(FakeClock::default());
    let registry = LeaseDevices::new(clock);
    let (entered, ready) = tokio::sync::oneshot::channel();
    let (release, wait) = mpsc::channel();
    let effects = register(&registry, &device().0, Some((entered, wait))).await;
    let grant = registry.acquire(&device(), 30_000).unwrap();
    let active = registry
        .enqueue(grant.auth.clone(), key(), Duration::from_secs(1))
        .unwrap();
    tokio::time::timeout(Duration::from_secs(1), ready)
        .await
        .unwrap()
        .unwrap();
    let queued = registry
        .enqueue(grant.auth, key(), Duration::from_secs(1))
        .unwrap();
    registry.invalidate(&device()).unwrap();
    assert_eq!(queued.wait().await.delivery, Delivery::NotSent);
    release.send(()).unwrap();
    let _ = active.wait().await;
    registry
        .shutdown(&device(), Duration::from_secs(1))
        .await
        .unwrap();
    assert_eq!(effects.load(Ordering::SeqCst), 0);
}

#[tokio::test]
async fn pending_and_failed_cleanup_never_allow_new_owner() {
    let clock = Arc::new(FakeClock::default());
    let registry = LeaseDevices::new(clock);
    let mode = Arc::new(AtomicUsize::new(1));
    register_mode(&registry, &device().0, None, mode.clone()).await;
    let grant = registry.acquire(&device(), 30_000).unwrap();
    assert_eq!(
        registry
            .enqueue(grant.auth, key(), Duration::from_secs(1))
            .unwrap()
            .wait()
            .await
            .delivery,
        Delivery::Sent
    );
    assert!(matches!(
        registry
            .shutdown(&device(), Duration::from_millis(10))
            .await,
        Err(ErrorCode::NativeCleanupPending)
    ));
    assert!(matches!(
        registry.acquire(&device(), 30_000),
        Err(ErrorCode::BackendUnavailable)
    ));
    let successor = Session::start_with_registration(
        Duration::from_secs(1),
        Registration {
            settling: Duration::ZERO,
        },
        || {
            Ok(Fake {
                effects: Arc::new(AtomicUsize::new(0)),
                gate: None,
                close_mode: Arc::new(AtomicUsize::new(0)),
            })
        },
    )
    .unwrap();
    successor
        .ready(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    assert!(matches!(
        registry.replace_cleaned(
            BootId("synthetic-boot".into()),
            device(),
            AttachmentGeneration(2),
            Fence(2),
            successor.clone()
        ),
        Err(ErrorCode::BackendUnavailable)
    ));
    mode.store(2, Ordering::SeqCst);
    let status = registry
        .shutdown(&device(), Duration::from_secs(1))
        .await
        .unwrap();
    assert!(status.cleanup_error.is_some());
    assert!(matches!(
        registry.replace_cleaned(
            BootId("synthetic-boot".into()),
            device(),
            AttachmentGeneration(2),
            Fence(2),
            successor.clone()
        ),
        Err(ErrorCode::BackendUnavailable)
    ));
    successor.shutdown(Duration::from_secs(1)).await.unwrap();
    assert!(matches!(
        registry.acquire(&device(), 30_000),
        Err(ErrorCode::BackendUnavailable)
    ));
}
#[tokio::test]
async fn finite_active_deadline_is_independent_of_idle_ttl() {
    let clock = Arc::new(FakeClock::default());
    let registry = LeaseDevices::new(clock);
    let (entered, ready) = tokio::sync::oneshot::channel();
    let (release, wait) = mpsc::channel();
    let effects = register(&registry, &device().0, Some((entered, wait))).await;
    let grant = registry.acquire(&device(), 30_000).unwrap();
    let ticket = registry
        .enqueue(grant.auth, key(), Duration::from_millis(30))
        .unwrap();
    tokio::time::timeout(Duration::from_secs(1), ready)
        .await
        .unwrap()
        .unwrap();
    let result = ticket.wait().await;
    release.send(()).unwrap();
    registry
        .shutdown(&device(), Duration::from_secs(1))
        .await
        .unwrap();
    assert_eq!(result.delivery, Delivery::Unknown);
    assert_eq!(result.error.unwrap().code, ErrorCode::ProcessTimeout);
    assert_eq!(effects.load(Ordering::SeqCst), 0);
    assert!(matches!(
        registry.acquire(&device(), 30_000),
        Err(ErrorCode::BackendUnavailable)
    ));
}

#[tokio::test]
async fn cleaned_successor_has_fresh_grant_and_cannot_upgrade_old_credentials() {
    let clock = Arc::new(FakeClock::default());
    let registry = LeaseDevices::new(clock);
    register(&registry, &device().0, None).await;
    let old = registry.acquire(&device(), 30_000).unwrap();
    registry
        .shutdown(&device(), Duration::from_secs(1))
        .await
        .unwrap();
    let effects = Arc::new(AtomicUsize::new(0));
    let e = effects.clone();
    let session = Session::start_with_registration(
        Duration::from_secs(1),
        Registration {
            settling: Duration::ZERO,
        },
        move || {
            Ok(Fake {
                effects: e,
                gate: None,
                close_mode: Arc::new(AtomicUsize::new(0)),
            })
        },
    )
    .unwrap();
    session
        .ready(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    registry
        .replace_cleaned(
            BootId("synthetic-boot".into()),
            device(),
            AttachmentGeneration(2),
            Fence(2),
            session,
        )
        .unwrap();
    let new = registry.acquire(&device(), 30_000).unwrap();
    let mut amended = old.auth;
    amended.attachment = AttachmentGeneration(2);
    amended.fence = Fence(2);
    amended.lease_id = new.auth.lease_id.clone();
    assert!(matches!(
        registry.enqueue(amended, key(), Duration::from_secs(1)),
        Err(ErrorCode::LeaseInvalid)
    ));
    assert_eq!(effects.load(Ordering::SeqCst), 0);
    assert_eq!(
        registry
            .enqueue(new.auth, key(), Duration::from_secs(1))
            .unwrap()
            .wait()
            .await
            .delivery,
        Delivery::Sent
    );
    registry
        .shutdown(&device(), Duration::from_secs(1))
        .await
        .unwrap();
}

struct AfterEffect(Fake);
impl SessionDriver for AfterEffect {
    type Handle = ();
    fn open(
        &mut self,
        r: Resource,
        h: &Handles<()>,
        b: CallBudget,
        c: &Cancel,
    ) -> Result<(), Failure> {
        self.0.open(r, h, b, c)
    }
    fn close(&mut self, r: Resource, h: &mut (), b: CallBudget) -> CloseOutcome {
        self.0.close(r, h, b)
    }
    fn apply_report(
        &mut self,
        _: &Transition,
        _: &Handles<()>,
        _: &Cancel,
        _: &Cancel,
    ) -> Result<(), Failure> {
        Ok(())
    }
    fn submit_report(
        &mut self,
        t: &Transition,
        h: &Handles<()>,
        b: CallBudget,
        c: &Cancel,
        e: &Cancel,
    ) -> ReportResult {
        let result = self.0.submit_report(t, h, b, c, e);
        if let Some((entered, wait)) = self.0.gate.take() {
            entered.send(()).unwrap();
            wait.recv_timeout(Duration::from_millis(500)).unwrap();
        }
        result
    }
}
#[test]
fn leased_lost_reply_after_claimed_effect_is_unknown_without_replay() {
    let registry = LeaseDevices::new(Arc::new(FakeClock::default()));
    let effects = Arc::new(AtomicUsize::new(0));
    let e = effects.clone();
    let (entered, ready) = tokio::sync::oneshot::channel();
    let (release, wait) = mpsc::channel();
    let runtime = tokio::runtime::Runtime::new().unwrap();
    let ticket = runtime.block_on(async {
        let session = Session::start_with_registration(
            Duration::from_secs(1),
            Registration {
                settling: Duration::ZERO,
            },
            move || {
                Ok(AfterEffect(Fake {
                    effects: e,
                    gate: Some((entered, wait)),
                    close_mode: Arc::new(AtomicUsize::new(0)),
                }))
            },
        )
        .unwrap();
        session
            .ready(Duration::from_secs(1), &Cancel::default())
            .await
            .unwrap();
        registry
            .register(
                BootId("synthetic-boot".into()),
                device(),
                AttachmentGeneration(1),
                Fence(1),
                session,
            )
            .unwrap();
        let grant = registry.acquire(&device(), 30_000).unwrap();
        let ticket = registry
            .enqueue(grant.auth, key(), Duration::from_secs(1))
            .unwrap();
        tokio::time::timeout(Duration::from_secs(1), ready)
            .await
            .unwrap()
            .unwrap();
        ticket
    });
    runtime.shutdown_background();
    let other = tokio::runtime::Runtime::new().unwrap();
    let result = other.block_on(ticket.wait());
    release.send(()).unwrap();
    other
        .block_on(registry.shutdown(&device(), Duration::from_secs(1)))
        .unwrap();
    assert_eq!(effects.load(Ordering::SeqCst), 1);
    assert_eq!(result.delivery, Delivery::Unknown);
    assert_eq!(result.error.unwrap().code, ErrorCode::BackendUnavailable);
}
