#![cfg(target_os = "linux")]
//! Explicit SOFTWARE FAKE USB, ADB inventory, capture and Session. No phone proof.
mod common;
#[path = "support/fixtures.rs"]
mod fixtures;
use cordgeist::{
    adb::{Attachment, Authorization},
    api::*,
    backend::{Cancel, Clock},
    discovery::*,
    discovery_service::{DiscoveryService, InventorySource},
    frames::{Limits, Profile},
    geometry::Transform,
    identity::*,
    pairing_store::PairingStore,
    service::*,
    session::*,
    session_input::*,
};
use std::{
    sync::{
        Arc,
        atomic::{AtomicU64, AtomicUsize, Ordering},
    },
    time::{Duration, Instant},
};
use tokio_util::sync::CancellationToken;
#[derive(Default)]
struct FakeClock(AtomicU64);
impl Clock for FakeClock {
    fn now_ms(&self) -> u64 {
        100 + self.0.load(Ordering::SeqCst)
    }
}
fn usb() -> UsbDevice {
    UsbDevice {
        bus: 1,
        address: 7,
        ports: vec![2],
        vendor_id: 1,
        product_id: 2,
        aoa_protocol: 2,
        serial: Some("SYNTHETIC".into()),
        product: None,
    }
}
fn adb() -> Attachment {
    Attachment {
        serial: "SYNTHETIC".into(),
        state: Authorization::Authorized,
        transport_id: Some(8),
        usb: Some("1-2".into()),
    }
}
fn usb_b() -> UsbDevice {
    let mut u = usb();
    u.ports = vec![3];
    u.serial = Some("SYNTHETIC_B".into());
    u
}
fn adb_b() -> Attachment {
    Attachment {
        serial: "SYNTHETIC_B".into(),
        state: Authorization::Authorized,
        transport_id: Some(9),
        usb: Some("1-3".into()),
    }
}
struct FakeUsb;
impl DiscoveryDriver for FakeUsb {
    fn kind(&self) -> DiscoveryKind {
        DiscoveryKind::SoftwareFake
    }
    fn scan(&mut self, _: Instant, _: &Cancel) -> Result<Vec<UsbDevice>, ErrorCode> {
        Ok(vec![usb(), usb_b()])
    }
    fn close(&mut self) -> CloseStatus {
        CloseStatus::Consumed(Ok(()))
    }
}
struct FakeInventory;
impl InventorySource for FakeInventory {
    async fn list(&self, _: CancellationToken) -> Result<Vec<Attachment>, ErrorCode> {
        Ok(vec![adb(), adb_b()])
    }
}
type Gate = (
    tokio::sync::oneshot::Sender<()>,
    std::sync::mpsc::Receiver<()>,
);
struct FakeSession {
    effects: Arc<AtomicUsize>,
    gate: Option<Gate>,
    cleanup: Arc<AtomicUsize>,
    report: Arc<AtomicUsize>,
    apply: bool,
}
impl SessionDriver for FakeSession {
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
        match self.cleanup.load(Ordering::SeqCst) {
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
        if self.apply
            && let Some((entered, release)) = self.gate.take()
        {
            entered.send(()).unwrap();
            release.recv_timeout(Duration::from_secs(1)).unwrap();
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
            Transition::Contact { down: true, .. } | Transition::Key { down: true, .. }
        ) {
            self.effects.fetch_add(1, Ordering::SeqCst);
            if !self.apply
                && let Some((entered, release)) = self.gate.take()
            {
                entered.send(()).unwrap();
                release.recv_timeout(Duration::from_secs(1)).unwrap();
            }
        }
        if self.report.load(Ordering::SeqCst) == 1 {
            return ReportResult {
                delivery: Delivery::Unknown,
                error: Some(Failure::code(ErrorCode::InputOutcomeUnknown)),
            };
        }
        ReportResult {
            delivery: Delivery::Sent,
            error: None,
        }
    }
}
struct FakeCapture {
    mode: Arc<AtomicUsize>,
}
impl CaptureSource for FakeCapture {
    fn capture<'a>(&'a self, route: ResolvedRoute, _: CancellationToken) -> CaptureFuture<'a> {
        Box::pin(async move {
            assert!([8, 9].contains(&route.target().transport_id));
            if self.mode.load(Ordering::SeqCst) == 3 {
                tokio::time::sleep(Duration::from_secs(2)).await;
            }
            if self.mode.load(Ordering::SeqCst) == 1 {
                return Err(ErrorCode::CaptureUnavailable);
            }
            Ok(Capture {
                image: Size {
                    width: 4,
                    height: 8,
                },
                png: include_bytes!("fixtures/synthetic.png").to_vec(),
                display: Size {
                    width: 4,
                    height: 8,
                },
                rotation: if self.mode.load(Ordering::SeqCst) == 2 {
                    Rotation::R180
                } else {
                    Rotation::R0
                },
                build: "synthetic-build".into(),
                descriptor: "pinned-default".into(),
                backend: "software-fake".into(),
            })
        })
    }
}
fn profile() -> Profile {
    Profile {
        build: "synthetic-build".into(),
        descriptor: "pinned-default".into(),
        backend: "software-fake".into(),
        display: Size {
            width: 4,
            height: 8,
        },
        rotation: Rotation::R0,
        geometry: GeometryVersion(1),
        mapping: MappingVersion(1),
        transform: Transform {
            rotation: Rotation::R0,
            max_x: 32767,
            max_y: 32767,
            verified: true,
            generic_screen: None,
        },
    }
}
struct Fixture {
    service: Arc<Service>,
    discovery: DiscoveryService<FakeInventory>,
    _dir: tempfile::TempDir,
    clock: Arc<FakeClock>,
    effects: Arc<AtomicUsize>,
    mode: Arc<AtomicUsize>,
    cleanup: Arc<AtomicUsize>,
    report: Arc<AtomicUsize>,
}
async fn setup(receipts: usize, gate: Option<Gate>, apply: bool) -> Fixture {
    setup_capture(receipts, gate, apply, None).await
}
async fn setup_capture(
    receipts: usize,
    gate: Option<Gate>,
    apply: bool,
    source: Option<Arc<dyn CaptureSource>>,
) -> Fixture {
    setup_mapping(receipts, gate, apply, source, true).await
}
async fn setup_mapping(
    receipts: usize,
    gate: Option<Gate>,
    apply: bool,
    source: Option<Arc<dyn CaptureSource>>,
    verified: bool,
) -> Fixture {
    setup_generic(receipts, gate, apply, source, verified, false).await
}
async fn setup_generic(
    receipts: usize,
    gate: Option<Gate>,
    apply: bool,
    source: Option<Arc<dyn CaptureSource>>,
    verified: bool,
    generic: bool,
) -> Fixture {
    let dir = fixtures::private_tempdir();
    let store = PairingStore::open(dir.path()).unwrap();
    store
        .set_pairing(
            Pairing::confirm(DeviceId("phone".into()), None, &usb(), &adb(), false).unwrap(),
        )
        .unwrap();
    store
        .set_pairing(
            Pairing::confirm(DeviceId("phone_b".into()), None, &usb_b(), &adb_b(), false).unwrap(),
        )
        .unwrap();
    let worker = DiscoveryWorker::start(|| Ok(FakeUsb)).unwrap();
    let mut discovery = DiscoveryService::new(worker, FakeInventory, store).unwrap();
    let routes = discovery
        .refresh(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    let route = routes
        .iter()
        .find(|r| r.device_id.0 == "phone")
        .unwrap()
        .clone();
    let route_b = routes
        .iter()
        .find(|r| r.device_id.0 == "phone_b")
        .unwrap()
        .clone();
    let clock = Arc::new(FakeClock::default());
    let service = Service::new(
        route.boot_id.clone(),
        clock.clone(),
        Limits {
            receipts,
            ..Limits::default()
        },
    )
    .unwrap();
    let effects = Arc::new(AtomicUsize::new(0));
    let cleanup = Arc::new(AtomicUsize::new(0));
    let mode = Arc::new(AtomicUsize::new(0));
    let report = Arc::new(AtomicUsize::new(0));
    let report_copy = report.clone();
    let e = effects.clone();
    let c = cleanup.clone();
    let session = Session::start_with_registration(
        Duration::from_secs(1),
        Registration {
            settling: Duration::ZERO,
        },
        move || {
            Ok(FakeSession {
                effects: e,
                gate,
                cleanup: c,
                report: report_copy,
                apply,
            })
        },
    )
    .unwrap();
    session
        .ready(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    service
        .install_software(
            discovery.identities(),
            route,
            session,
            Profile {
                build: "synthetic-build".into(),
                descriptor: "pinned-default".into(),
                backend: "software-fake".into(),
                display: Size {
                    width: 4,
                    height: 8,
                },
                rotation: Rotation::R0,
                geometry: GeometryVersion(1),
                mapping: MappingVersion(1),
                transform: Transform {
                    rotation: Rotation::R0,
                    max_x: 32767,
                    max_y: 32767,
                    verified,
                    generic_screen: generic.then_some(Size {
                        width: 4,
                        height: 8,
                    }),
                },
            },
            source.unwrap_or_else(|| Arc::new(FakeCapture { mode: mode.clone() })),
        )
        .unwrap();
    let second = Session::start_with_registration(
        Duration::from_secs(1),
        Registration {
            settling: Duration::ZERO,
        },
        || {
            Ok(FakeSession {
                effects: Arc::new(AtomicUsize::new(0)),
                gate: None,
                cleanup: Arc::new(AtomicUsize::new(0)),
                report: Arc::new(AtomicUsize::new(0)),
                apply: false,
            })
        },
    )
    .unwrap();
    second
        .ready(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    service
        .install_software(
            discovery.identities(),
            route_b,
            second,
            profile(),
            Arc::new(FakeCapture {
                mode: Arc::new(AtomicUsize::new(0)),
            }),
        )
        .unwrap();
    Fixture {
        service,
        discovery,
        _dir: dir,
        clock,
        effects,
        mode,
        cleanup,
        report,
    }
}
fn request(call: Call) -> Request {
    Request {
        coordinate_space: CoordinateSpace::ReturnedImagePixels,
        observe_after: true,
        version: 1,
        request_id: "synthetic-request".into(),
        deadline_ms: 1000,
        call,
    }
}
async fn result(s: &Arc<Service>, call: Call) -> OperationResult {
    match s.handle(request(call)).await.outcome {
        ResponseOutcome::Success { result } => *result,
        ResponseOutcome::Error { error } => panic!("safe code {:?}", error.code),
    }
}
async fn error(s: &Arc<Service>, call: Call) -> ErrorCode {
    match s.handle(request(call)).await.outcome {
        ResponseOutcome::Error { error } => error.code,
        _ => panic!("expected safe error"),
    }
}
async fn acquire(s: &Arc<Service>) -> LeaseAuth {
    match result(
        s,
        Call::Acquire {
            device: DeviceId("phone".into()),
            reason: "software test".into(),
            label: None,
            idle_ttl_s: 30,
        },
    )
    .await
    {
        OperationResult::Acquire(g) => g.auth,
        _ => panic!(),
    }
}
async fn screenshot(s: &Arc<Service>, auth: &LeaseAuth) -> ScreenshotResult {
    match result(
        s,
        Call::Screenshot {
            auth: auth.clone(),
            longest_edge: 1280,
            settle_ms: 0,
        },
    )
    .await
    {
        OperationResult::Screenshot(v) => v,
        _ => panic!(),
    }
}
fn tap(auth: &LeaseAuth, frame: &Frame, seq: u64) -> Call {
    Call::Tap {
        auth: auth.clone(),
        frame_id: frame.id.clone(),
        seq: ActionSeq(seq),
        point: Point { x: 1., y: 1. },
    }
}
fn action(r: OperationResult) -> ActionResult {
    match r {
        OperationResult::Tap(a) | OperationResult::Type(a) => a,
        _ => panic!(),
    }
}
async fn final_receipt(s: &Arc<Service>, auth: &LeaseAuth, seq: u64) -> ActionResult {
    tokio::time::timeout(Duration::from_secs(2), async {
        loop {
            if let OperationResult::ActionStatus(ActionStatusResult::Final { action }) = result(
                s,
                Call::ActionStatus {
                    auth: auth.clone(),
                    seq: ActionSeq(seq),
                },
            )
            .await
            {
                return *action;
            }
            tokio::task::yield_now().await;
        }
    })
    .await
    .unwrap()
}
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn public_ipc_acquire_observe_act_receipt_status_release_and_eviction() {
    use cordgeist::ipc::{framing::*, server::*};
    use std::os::unix::{fs::PermissionsExt, net::UnixStream};
    let f = setup(1, None, false).await;
    let root = tempfile::tempdir().unwrap();
    std::fs::set_permissions(root.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
    let server = Server::bind_dispatcher(
        root.path(),
        Arc::new(ServiceDispatcher {
            service: f.service.clone(),
            runtime: tokio::runtime::Handle::current(),
        }),
    )
    .unwrap();
    let stop = server.stop_handle();
    let path = root.path().join("cordgeist/daemon.sock");
    let run = std::thread::spawn(move || server.run().unwrap());
    let exchange = move |call| {
        let mut stream = UnixStream::connect(&path).unwrap();
        let deadline = Instant::now() + MESSAGE_TIMEOUT;
        send(&mut stream, &Hello { version: 1 }, deadline).unwrap();
        let welcome: Welcome = receive(&mut stream, deadline).unwrap();
        assert!(!welcome.device_control_available);
        let req = request(call);
        send(&mut stream, &req, deadline).unwrap();
        let response: Response = receive(&mut stream, deadline).unwrap();
        response.validate_for(&req, &welcome.boot_id).unwrap();
        match response.outcome {
            ResponseOutcome::Success { result } => *result,
            ResponseOutcome::Error { error } => panic!("safe IPC error {:?}", error.code),
        }
    };
    let effects = f.effects.clone();
    tokio::task::spawn_blocking(move || {
        let auth = match exchange(Call::Acquire {
            device: DeviceId("phone".into()),
            reason: "software IPC".into(),
            label: None,
            idle_ttl_s: 30,
        }) {
            OperationResult::Acquire(g) => g.auth,
            _ => panic!(),
        };
        let frame = match exchange(Call::Screenshot {
            auth: auth.clone(),
            longest_edge: 1280,
            settle_ms: 0,
        }) {
            OperationResult::Screenshot(v) => {
                assert!(v.image.base64.starts_with("iVBORw0KGgo"));
                v.frame
            }
            _ => panic!(),
        };
        let original = tap(&auth, &frame, 1);
        let first = action(exchange(original.clone()));
        assert_eq!(first.receipt.input_status, InputStatus::Sent);
        assert_eq!(
            first.receipt.observation_status,
            ObservationStatus::Captured
        );
        assert!(!first.receipt.cancelled);
        let duplicate = action(exchange(original));
        assert_eq!(first.receipt.action_id, duplicate.receipt.action_id);
        assert_eq!(effects.load(Ordering::SeqCst), 1);
        assert!(matches!(
            exchange(Call::ActionStatus {
                auth: auth.clone(),
                seq: ActionSeq(1)
            }),
            OperationResult::ActionStatus(ActionStatusResult::Final { .. })
        ));
        let second = action(exchange(tap(&auth, &first.observation.unwrap().frame, 2)));
        assert_eq!(second.receipt.input_status, InputStatus::Sent);
        assert_eq!(effects.load(Ordering::SeqCst), 2);
        assert!(matches!(
            exchange(Call::Release { auth }),
            OperationResult::Release(LeaseEndResult {
                cleanup: Cleanup::Neutral,
                ..
            })
        ));
    })
    .await
    .unwrap();
    stop.request();
    tokio::task::spawn_blocking(move || run.join().unwrap())
        .await
        .unwrap();
    let _ = f;
}
#[tokio::test]
async fn duplicate_lookup_precedes_fresh_frame_and_eviction_never_replays() {
    let f = setup(1, None, false).await;
    let a = acquire(&f.service).await;
    let frame = screenshot(&f.service, &a).await.frame;
    let call = tap(&a, &frame, 1);
    let first = action(result(&f.service, call.clone()).await);
    let mut mismatch = call.clone();
    if let Call::Tap { point, .. } = &mut mismatch {
        point.x = 2.;
    }
    assert_eq!(
        error(&f.service, mismatch).await,
        ErrorCode::SequenceMismatch
    );
    assert_eq!(
        action(result(&f.service, call.clone()).await)
            .receipt
            .action_id,
        first.receipt.action_id
    );
    let next = first.observation.unwrap().frame;
    result(&f.service, tap(&a, &next, 2)).await;
    assert_eq!(error(&f.service, call).await, ErrorCode::ReceiptExpired);
    assert_eq!(f.effects.load(Ordering::SeqCst), 2);
    f.service.shutdown().await;
}
#[tokio::test]
async fn complete_payload_preflight_and_stale_authority_send_nothing() {
    let f = setup(2, None, false).await;
    let a = acquire(&f.service).await;
    let frame = screenshot(&f.service, &a).await.frame;
    let mut bad = a.clone();
    bad.fence = Fence(2);
    assert_eq!(
        error(&f.service, tap(&bad, &frame, 1)).await,
        ErrorCode::LeaseInvalid
    );
    let invalid = Call::Type {
        submit: false,
        auth: a.clone(),
        seq: ActionSeq(1),
        frame_id: frame.id.clone(),
        text: "Aé".into(),
    };
    assert_eq!(
        error(&f.service, invalid).await,
        ErrorCode::UnsupportedCharacter
    );
    assert_eq!(f.effects.load(Ordering::SeqCst), 0);
    assert_eq!(
        action(result(&f.service, tap(&a, &frame, 1)).await)
            .receipt
            .input_status,
        InputStatus::Sent
    );
    f.service.shutdown().await;
}
#[tokio::test]
async fn capture_failure_preserves_delivery_and_matching_receipt() {
    let f = setup(2, None, false).await;
    let a = acquire(&f.service).await;
    let frame = screenshot(&f.service, &a).await.frame;
    f.mode.store(1, Ordering::SeqCst);
    let call = tap(&a, &frame, 1);
    let r = action(result(&f.service, call.clone()).await);
    assert_eq!(r.receipt.input_status, InputStatus::Sent);
    assert_eq!(r.receipt.observation_status, ObservationStatus::Failed);
    assert!(r.observation.is_none());
    assert_eq!(
        action(result(&f.service, call).await).receipt.action_id,
        r.receipt.action_id
    );
    assert_eq!(f.effects.load(Ordering::SeqCst), 1);
    f.service.shutdown().await;
}
#[tokio::test]
async fn observation_replacement_between_apply_and_submit_has_zero_effects() {
    let (entered, rx) = tokio::sync::oneshot::channel();
    let (release, gate) = std::sync::mpsc::channel();
    let f = setup(2, Some((entered, gate)), true).await;
    let a = acquire(&f.service).await;
    let frame = screenshot(&f.service, &a).await.frame;
    let s = f.service.clone();
    let call = tap(&a, &frame, 1);
    let task = tokio::spawn(async move { s.handle(request(call)).await });
    rx.await.unwrap();
    screenshot(&f.service, &a).await;
    release.send(()).unwrap();
    task.await.unwrap();
    let r = final_receipt(&f.service, &a, 1).await;
    assert_eq!(r.receipt.input_status, InputStatus::NotSent);
    assert_eq!(f.effects.load(Ordering::SeqCst), 0);
    f.service.shutdown().await;
}
#[tokio::test]
async fn client_loss_and_inflight_duplicate_join_do_not_repeat_effects() {
    let (entered, rx) = tokio::sync::oneshot::channel();
    let (release, gate) = std::sync::mpsc::channel();
    let f = setup(2, Some((entered, gate)), false).await;
    let a = acquire(&f.service).await;
    let frame = screenshot(&f.service, &a).await.frame;
    let s = f.service.clone();
    let call = tap(&a, &frame, 1);
    let first = call.clone();
    let task = tokio::spawn(async move { s.handle(request(first)).await });
    rx.await.unwrap();
    task.abort();
    let _ = task.await;
    let s = f.service.clone();
    let duplicate = tokio::spawn(async move { s.handle(request(call)).await });
    release.send(()).unwrap();
    duplicate.await.unwrap();
    let r = final_receipt(&f.service, &a, 1).await;
    assert_eq!(r.receipt.input_status, InputStatus::Sent);
    assert_eq!(f.effects.load(Ordering::SeqCst), 1);
    f.service.shutdown().await;
}
#[tokio::test]
async fn cancellation_and_pending_failed_cleanup_prevent_handover() {
    for mode in [1, 2] {
        let (entered, rx) = tokio::sync::oneshot::channel();
        let (release, gate) = std::sync::mpsc::channel();
        let f = setup(2, Some((entered, gate)), false).await;
        let a = acquire(&f.service).await;
        let frame = screenshot(&f.service, &a).await.frame;
        f.cleanup.store(mode, Ordering::SeqCst);
        let s = f.service.clone();
        let task = tokio::spawn(async move { s.handle(request(tap(&a, &frame, 1))).await });
        // Recover the grant from this fixture through the owned request copy below.
        rx.await.unwrap();
        let service = f.service.clone();
        let revoke = tokio::spawn(async move {
            service
                .handle(request(Call::Revoke {
                    device: DeviceId("phone".into()),
                }))
                .await
        });
        tokio::time::timeout(Duration::from_millis(500), async {
            loop {
                if let OperationResult::List(v) = result(&f.service, Call::List).await
                    && v.devices.iter().any(|d| {
                        d.device_id.0 == "phone"
                            && d.reason_codes.contains(&ErrorCode::NativeCleanupPending)
                    })
                {
                    break;
                }
                tokio::task::yield_now().await;
            }
        })
        .await
        .unwrap();
        release.send(()).unwrap();
        let _ = revoke.await.unwrap();
        task.await.unwrap();
        assert_eq!(
            error(
                &f.service,
                Call::Acquire {
                    device: DeviceId("phone".into()),
                    reason: "successor".into(),
                    label: None,
                    idle_ttl_s: 30
                }
            )
            .await,
            // The fake feature attempts its software rearm before acquisition;
            // pending/failed cleanup must still deny that path explicitly.
            if cfg!(feature = "fake") {
                ErrorCode::NativeCleanupPending
            } else {
                ErrorCode::BackendUnavailable
            }
        );
        f.cleanup.store(0, Ordering::SeqCst);
        f.service.shutdown().await;
    }
}
#[tokio::test]
async fn changed_geometry_and_discovery_invalidation_fence_cached_actions() {
    for geometry in [false, true] {
        let mut f = setup(2, None, false).await;
        let a = acquire(&f.service).await;
        let frame = screenshot(&f.service, &a).await.frame;
        if geometry {
            f.mode.store(2, Ordering::SeqCst);
            assert_eq!(
                error(
                    &f.service,
                    Call::Screenshot {
                        auth: a.clone(),
                        longest_edge: 1280,
                        settle_ms: 0
                    }
                )
                .await,
                ErrorCode::GeometryChanged
            );
        } else {
            f.discovery
                .refresh(Duration::from_secs(1), &Cancel::default())
                .await
                .unwrap();
        }
        let r = f.service.handle(request(tap(&a, &frame, 1))).await;
        match r.outcome {
            ResponseOutcome::Error { .. } => {}
            ResponseOutcome::Success { result } => {
                assert_eq!(action(*result).receipt.input_status, InputStatus::NotSent)
            }
        }
        assert_eq!(f.effects.load(Ordering::SeqCst), 0);
        f.service.shutdown().await;
    }
}
#[tokio::test]
async fn idle_expiry_and_settling_are_bounded_without_input() {
    let f = setup(2, None, false).await;
    let a = acquire(&f.service).await;
    let r = result(
        &f.service,
        Call::Screenshot {
            auth: a.clone(),
            longest_edge: 1280,
            settle_ms: 20,
        },
    )
    .await;
    assert!(matches!(
        r,
        OperationResult::Screenshot(ScreenshotResult {
            settled: Some(true),
            ..
        })
    ));
    f.clock.0.store(30000, Ordering::SeqCst);
    assert!(matches!(
        error(
            &f.service,
            Call::Screenshot {
                auth: a,
                longest_edge: 1280,
                settle_ms: 0
            }
        )
        .await,
        ErrorCode::LeaseInvalid | ErrorCode::LeaseExpired
    ));
    assert_eq!(f.effects.load(Ordering::SeqCst), 0);
    f.service.shutdown().await;
}

#[tokio::test]
async fn priority_cancel_invalidates_queued_work_and_other_device_progresses() {
    let (entered, rx) = tokio::sync::oneshot::channel();
    let (release, gate) = std::sync::mpsc::channel();
    let f = setup(4, Some((entered, gate)), false).await;
    let a = acquire(&f.service).await;
    let frame = screenshot(&f.service, &a).await.frame;
    let s = f.service.clone();
    let first = tap(&a, &frame, 1);
    let active = tokio::spawn(async move { s.handle(request(first)).await });
    rx.await.unwrap();
    // Poll once through public admission, holding A's native call, then admit a
    // queued command. No sleep or private state injection establishes saturation.
    let s = f.service.clone();
    let queued_call = tap(&a, &frame, 2);
    let queued = tokio::spawn(async move { s.handle(request(queued_call)).await });
    tokio::time::timeout(Duration::from_millis(500), async {
        loop {
            let response = f
                .service
                .handle(request(Call::ActionStatus {
                    auth: a.clone(),
                    seq: ActionSeq(2),
                }))
                .await;
            if matches!(response.outcome, ResponseOutcome::Success { .. }) {
                break;
            }
            tokio::task::yield_now().await;
        }
    })
    .await
    .unwrap();
    let mut saturated = Vec::new();
    for seq in [3, 4] {
        let service = f.service.clone();
        let call = tap(&a, &frame, seq);
        saturated.push(tokio::spawn(
            async move { service.handle(request(call)).await },
        ));
        tokio::time::timeout(Duration::from_millis(500), async {
            loop {
                let response = f
                    .service
                    .handle(request(Call::ActionStatus {
                        auth: a.clone(),
                        seq: ActionSeq(seq),
                    }))
                    .await;
                if matches!(response.outcome, ResponseOutcome::Success { .. }) {
                    break;
                }
                tokio::task::yield_now().await;
            }
        })
        .await
        .unwrap();
    }
    assert_eq!(
        error(&f.service, tap(&a, &frame, 5)).await,
        ErrorCode::QueueFull
    );
    let b = match result(
        &f.service,
        Call::Acquire {
            device: DeviceId("phone_b".into()),
            reason: "independent".into(),
            label: None,
            idle_ttl_s: 30,
        },
    )
    .await
    {
        OperationResult::Acquire(g) => g.auth,
        _ => panic!(),
    };
    let bf = screenshot(&f.service, &b).await.frame;
    assert_eq!(
        action(result(&f.service, tap(&b, &bf, 1)).await)
            .receipt
            .input_status,
        InputStatus::Sent
    );
    assert!(matches!(
        result(
            &f.service,
            Call::Cancel {
                auth: a.clone(),
                seq: ActionSeq(1)
            }
        )
        .await,
        OperationResult::Cancel(CancelResult {
            state: CancellationState::Requested,
            ..
        })
    ));
    release.send(()).unwrap();
    active.await.unwrap();
    queued.await.unwrap();
    for pending in saturated {
        pending.await.unwrap();
    }
    let first = final_receipt(&f.service, &a, 1).await;
    assert!(first.receipt.cancelled);
    assert_eq!(first.receipt.input_status, InputStatus::Unknown);
    assert_eq!(
        final_receipt(&f.service, &a, 2).await.receipt.input_status,
        InputStatus::NotSent
    );
    for seq in [3, 4] {
        assert_eq!(
            final_receipt(&f.service, &a, seq)
                .await
                .receipt
                .input_status,
            InputStatus::NotSent
        );
    }
    assert_eq!(f.effects.load(Ordering::SeqCst), 1);
    f.service.shutdown().await;
}
#[tokio::test]
async fn simultaneous_acquire_has_one_canonical_owner_and_unknown_frame_has_no_effects() {
    let f = setup(2, None, false).await;
    let call = Call::Acquire {
        device: DeviceId("phone".into()),
        reason: "race".into(),
        label: None,
        idle_ttl_s: 30,
    };
    let (a, b) = tokio::join!(
        f.service.handle(request(call.clone())),
        f.service.handle(request(call))
    );
    let mut owners = Vec::new();
    let mut busy = 0;
    for r in [a, b] {
        match r.outcome {
            ResponseOutcome::Success { result } => {
                if let OperationResult::Acquire(g) = *result {
                    owners.push(g.auth);
                } else {
                    panic!()
                }
            }
            ResponseOutcome::Error { error } => {
                assert_eq!(error.code, ErrorCode::Leased);
                busy += 1;
            }
        }
    }
    assert_eq!(owners.len(), 1);
    assert_eq!(busy, 1);
    let a = &owners[0];
    let mut frame = screenshot(&f.service, a).await.frame;
    frame.id = FrameId("unknown".into());
    assert_eq!(
        error(&f.service, tap(a, &frame, 1)).await,
        ErrorCode::FrameExpired
    );
    assert_eq!(f.effects.load(Ordering::SeqCst), 0);
    f.service.shutdown().await;
}
#[tokio::test]
async fn bounded_receipts_images_and_safe_events_do_not_export_payloads() {
    let f = setup(1, None, false).await;
    let a = acquire(&f.service).await;
    let frame = screenshot(&f.service, &a).await.frame;
    result(
        &f.service,
        Call::Type {
            submit: false,
            auth: a.clone(),
            seq: ActionSeq(1),
            frame_id: frame.id,
            text: "PRIVATE_PAYLOAD".into(),
        },
    )
    .await;
    for _ in 0..70 {
        result(&f.service, Call::Status).await;
    }
    let usage = f.service.usage();
    assert_eq!(usage.events, 64);
    assert_eq!(usage.receipts, 1);
    assert!(usage.png_bytes <= 2 * Limits::default().frame_bytes);
    assert!(usage.receipt_image_bytes <= 2 * Limits::default().frame_bytes);
    let events = format!("{:?}", f.service.events());
    assert!(!events.contains("PRIVATE_PAYLOAD"));
    assert!(!events.contains(&a.credential.0));
    assert!(!events.contains("SYNTHETIC"));
    f.service.shutdown().await;
}
#[tokio::test]
async fn real_adb_observation_adapter_failure_after_input_retains_final_receipt() {
    let dir = tempfile::tempdir().unwrap();
    let path = dir.path().join("fake-adb");
    common::python(
        &path,
        r#"#!/usr/bin/python3
import sys,pathlib
base=pathlib.Path(__file__)
a=sys.argv[1:]
assert a[:4]==['-H','127.0.0.1','-P','5039']
if a[4:]==['devices','-l']:print('List of devices attached\nSYNTHETIC device usb:1-2 transport_id:8\n')
elif a[4:]==['-t','8','shell','getprop','ro.build.fingerprint']:print('synthetic-build')
elif a[4:]==['-t','8','shell','dumpsys','input']:print(base.with_suffix('.viewport').read_text())
elif a[4:]==['-t','8','shell','am','get-current-user']:print(0)
elif a[4:]==['-t','8','shell','wm','size']:print('Physical size: 4x8')
elif a[4:]==['-t','8','exec-out','screencap','-p']:
 if base.with_suffix('.fail').exists():raise SystemExit(1)
 sys.stdout.buffer.write(base.with_suffix('.png').read_bytes())
else:raise SystemExit(2)
"#,
    );
    std::fs::write(
        path.with_extension("viewport"),
        include_bytes!("fixtures/aosp-viewport.txt"),
    )
    .unwrap();
    std::fs::write(
        path.with_extension("png"),
        include_bytes!("fixtures/synthetic.png"),
    )
    .unwrap();
    let source = Arc::new(AdbCapture {
        adb: cordgeist::adb::Adb::new(path.clone(), 5039).unwrap(),
        descriptor: "pinned-default".into(),
        backend: "software-fake".into(),
    });
    let f = setup_capture(2, None, false, Some(source)).await;
    let a = acquire(&f.service).await;
    let frame = screenshot(&f.service, &a).await.frame;
    std::fs::write(path.with_extension("fail"), b"software capture failure").unwrap();
    let original = tap(&a, &frame, 1);
    let r = action(result(&f.service, original.clone()).await);
    assert_eq!(r.receipt.input_status, InputStatus::Sent);
    assert_eq!(r.receipt.observation_status, ObservationStatus::Failed);
    assert_eq!(r.receipt.cleanup, Cleanup::Neutral);
    assert_eq!(
        action(result(&f.service, original).await).receipt.action_id,
        r.receipt.action_id
    );
    assert_eq!(f.effects.load(Ordering::SeqCst), 1);
    f.service.shutdown().await;
}
#[tokio::test]
async fn unknown_delivery_and_cleanup_failure_stay_separate_and_never_retry() {
    let f = setup(2, None, false).await;
    let a = acquire(&f.service).await;
    let frame = screenshot(&f.service, &a).await.frame;
    f.report.store(1, Ordering::SeqCst);
    f.cleanup.store(2, Ordering::SeqCst);
    let call = tap(&a, &frame, 1);
    let r = action(result(&f.service, call.clone()).await);
    assert_eq!(r.receipt.input_status, InputStatus::Unknown);
    assert_eq!(r.receipt.observation_status, ObservationStatus::Failed);
    assert_eq!(
        action(result(&f.service, call).await).receipt.action_id,
        r.receipt.action_id
    );
    assert_eq!(f.effects.load(Ordering::SeqCst), 1);
    assert!(matches!(
        result(&f.service, Call::Release { auth: a }).await,
        OperationResult::Release(LeaseEndResult {
            cleanup: Cleanup::Failed,
            ..
        })
    ));
    assert_eq!(
        error(
            &f.service,
            Call::Acquire {
                device: DeviceId("phone".into()),
                reason: "successor".into(),
                label: None,
                idle_ttl_s: 30
            }
        )
        .await,
        // The fake feature attempts its software rearm before acquisition;
        // pending/failed cleanup must still deny that path explicitly.
        if cfg!(feature = "fake") {
            ErrorCode::NativeCleanupPending
        } else {
            ErrorCode::BackendUnavailable
        }
    );
    f.service.shutdown().await;
}
#[tokio::test]
async fn changing_frames_return_settled_false_at_bounded_window_without_input() {
    struct Animation(AtomicUsize);
    impl CaptureSource for Animation {
        fn capture<'a>(&'a self, _: ResolvedRoute, _: CancellationToken) -> CaptureFuture<'a> {
            Box::pin(async move {
                let tick = self.0.fetch_add(1, Ordering::SeqCst) as u8;
                let mut output = std::io::Cursor::new(Vec::new());
                image::RgbaImage::from_pixel(4, 8, image::Rgba([tick, 0, 0, 255]))
                    .write_to(&mut output, image::ImageFormat::Png)
                    .unwrap();
                Ok(Capture {
                    png: output.into_inner(),
                    image: Size {
                        width: 4,
                        height: 8,
                    },
                    display: Size {
                        width: 4,
                        height: 8,
                    },
                    rotation: Rotation::R0,
                    build: "synthetic-build".into(),
                    descriptor: "pinned-default".into(),
                    backend: "software-fake".into(),
                })
            })
        }
    }
    let f = setup_capture(
        2,
        None,
        false,
        Some(Arc::new(Animation(AtomicUsize::new(0)))),
    )
    .await;
    let a = acquire(&f.service).await;
    let mut req = request(Call::Screenshot {
        auth: a,
        longest_edge: 1280,
        settle_ms: 500,
    });
    req.deadline_ms = 150;
    let r = f.service.handle(req).await;
    assert!(
        matches!(r.outcome,ResponseOutcome::Success{result} if matches!(*result,OperationResult::Screenshot(ScreenshotResult{settled:Some(false),..})))
    );
    assert_eq!(f.effects.load(Ordering::SeqCst), 0);
    f.service.shutdown().await;
}
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn ipc_client_disconnect_after_admission_preserves_original_action_identity() {
    use cordgeist::ipc::{framing::*, server::*};
    use std::os::unix::{fs::PermissionsExt, net::UnixStream};
    let (entered, rx) = tokio::sync::oneshot::channel();
    let (release, gate) = std::sync::mpsc::channel();
    let f = setup(2, Some((entered, gate)), false).await;
    let a = acquire(&f.service).await;
    let frame = screenshot(&f.service, &a).await.frame;
    let call = tap(&a, &frame, 1);
    let dir = tempfile::tempdir().unwrap();
    std::fs::set_permissions(dir.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
    let server = Server::bind_dispatcher(
        dir.path(),
        Arc::new(ServiceDispatcher {
            service: f.service.clone(),
            runtime: tokio::runtime::Handle::current(),
        }),
    )
    .unwrap();
    let stop = server.stop_handle();
    let path = dir.path().join("cordgeist/daemon.sock");
    let run = std::thread::spawn(move || server.run().unwrap());
    let mut client = UnixStream::connect(path).unwrap();
    let deadline = Instant::now() + MESSAGE_TIMEOUT;
    send(&mut client, &Hello { version: 1 }, deadline).unwrap();
    let _: Welcome = receive(&mut client, deadline).unwrap();
    send(&mut client, &request(call.clone()), deadline).unwrap();
    rx.await.unwrap();
    drop(client);
    release.send(()).unwrap();
    let original = final_receipt(&f.service, &a, 1).await;
    let duplicate = action(result(&f.service, call).await);
    assert_eq!(duplicate.receipt.action_id, original.receipt.action_id);
    assert_eq!(f.effects.load(Ordering::SeqCst), 1);
    stop.request();
    tokio::task::spawn_blocking(move || run.join().unwrap())
        .await
        .unwrap();
}
#[tokio::test]
async fn explicit_cleaned_reconnect_requires_fresh_grant_and_observation() {
    let mut f = setup(2, None, false).await;
    let a = acquire(&f.service).await;
    let old_frame = screenshot(&f.service, &a).await.frame;
    result(&f.service, Call::Release { auth: a.clone() }).await;
    let routes = f
        .discovery
        .refresh(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    let route = routes
        .into_iter()
        .find(|r| r.device_id.0 == "phone")
        .unwrap();
    let effects = f.effects.clone();
    let session = Session::start_with_registration(
        Duration::from_secs(1),
        Registration {
            settling: Duration::ZERO,
        },
        move || {
            Ok(FakeSession {
                effects,
                gate: None,
                cleanup: Arc::new(AtomicUsize::new(0)),
                report: Arc::new(AtomicUsize::new(0)),
                apply: false,
            })
        },
    )
    .unwrap();
    session
        .ready(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    f.service
        .reconnect_software(
            f.discovery.identities(),
            route,
            session,
            profile(),
            Arc::new(FakeCapture {
                mode: f.mode.clone(),
            }),
        )
        .unwrap();
    let b = acquire(&f.service).await;
    assert!(b.attachment.0 > a.attachment.0);
    assert_ne!(b.lease_id, a.lease_id);
    assert_eq!(
        error(&f.service, tap(&a, &old_frame, 1)).await,
        ErrorCode::LeaseInvalid
    );
    assert_eq!(
        error(&f.service, tap(&b, &old_frame, 1)).await,
        ErrorCode::FrameExpired
    );
    assert_eq!(f.effects.load(Ordering::SeqCst), 0);
    let fresh = screenshot(&f.service, &b).await.frame;
    assert_eq!(
        action(result(&f.service, tap(&b, &fresh, 1)).await)
            .receipt
            .input_status,
        InputStatus::Sent
    );
    f.service.shutdown().await;
}
#[test]
fn service_runtime_loss_retains_unknown_and_native_owner_cleanup() {
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()
        .unwrap();
    let (entered, rx) = tokio::sync::oneshot::channel();
    let (release, gate) = std::sync::mpsc::channel();
    let (f, a) = runtime.block_on(async {
        let f = setup(2, Some((entered, gate)), false).await;
        let a = acquire(&f.service).await;
        let frame = screenshot(&f.service, &a).await.frame;
        let s = f.service.clone();
        let call = tap(&a, &frame, 1);
        tokio::spawn(async move { s.handle(request(call)).await });
        rx.await.unwrap();
        (f, a)
    });
    runtime.shutdown_timeout(Duration::from_millis(100));
    let observer = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap();
    observer.block_on(async {
        let receipt = final_receipt(&f.service, &a, 1).await;
        assert_eq!(receipt.receipt.input_status, InputStatus::Unknown);
        assert_eq!(receipt.receipt.cleanup, Cleanup::Pending);
        assert_eq!(f.effects.load(Ordering::SeqCst), 1);
        release.send(()).unwrap();
        let released = result(&f.service, Call::Release { auth: a }).await;
        assert!(matches!(
            released,
            OperationResult::Release(LeaseEndResult {
                cleanup: Cleanup::Neutral,
                ..
            })
        ));
        f.service.shutdown().await;
    });
}
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn ipc_stop_fences_before_waiting_for_an_incomplete_client() {
    use cordgeist::ipc::{framing::*, server::*};
    use std::os::unix::{fs::PermissionsExt, net::UnixStream};
    let f = setup(2, None, false).await;
    let dir = tempfile::tempdir().unwrap();
    std::fs::set_permissions(dir.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
    let server = Server::bind_dispatcher(
        dir.path(),
        Arc::new(ServiceDispatcher {
            service: f.service.clone(),
            runtime: tokio::runtime::Handle::current(),
        }),
    )
    .unwrap();
    let stop = server.stop_handle();
    let path = dir.path().join("cordgeist/daemon.sock");
    let run = std::thread::spawn(move || server.run().unwrap());
    let mut held = UnixStream::connect(path).unwrap();
    let deadline = Instant::now() + MESSAGE_TIMEOUT;
    send(&mut held, &Hello { version: 1 }, deadline).unwrap();
    let _: Welcome = receive(&mut held, deadline).unwrap();
    stop.request();
    let response = f
        .service
        .handle(request(Call::Acquire {
            device: DeviceId("phone".into()),
            reason: "after stop".into(),
            label: None,
            idle_ttl_s: 30,
        }))
        .await;
    drop(held);
    tokio::task::spawn_blocking(move || run.join().unwrap())
        .await
        .unwrap();
    assert!(matches!(
        response.outcome,
        ResponseOutcome::Error {
            error: ApiError {
                code: ErrorCode::BackendUnavailable,
                ..
            }
        }
    ));
    assert_eq!(f.effects.load(Ordering::SeqCst), 0);
}
#[tokio::test]
async fn delayed_capture_cannot_publish_a_fresh_age_for_an_expired_observation() {
    struct DelayedCapture(
        std::sync::Mutex<
            Option<(
                tokio::sync::oneshot::Sender<()>,
                tokio::sync::oneshot::Receiver<()>,
            )>,
        >,
    );
    impl CaptureSource for DelayedCapture {
        fn capture<'a>(
            &'a self,
            route: ResolvedRoute,
            cancel: CancellationToken,
        ) -> CaptureFuture<'a> {
            Box::pin(async move {
                let gate = { self.0.lock().unwrap().take() };
                if let Some((entered, release)) = gate {
                    entered.send(()).unwrap();
                    release.await.unwrap();
                }
                FakeCapture {
                    mode: Arc::new(AtomicUsize::new(0)),
                }
                .capture(route, cancel)
                .await
            })
        }
    }
    let (entered, rx) = tokio::sync::oneshot::channel();
    let (release, gate) = tokio::sync::oneshot::channel();
    let f = setup_capture(
        2,
        None,
        false,
        Some(Arc::new(DelayedCapture(std::sync::Mutex::new(Some((
            entered, gate,
        )))))),
    )
    .await;
    let a = match result(
        &f.service,
        Call::Acquire {
            device: DeviceId("phone".into()),
            reason: "capture clock".into(),
            label: None,
            idle_ttl_s: 3600,
        },
    )
    .await
    {
        OperationResult::Acquire(g) => g.auth,
        _ => panic!(),
    };
    let service = f.service.clone();
    let task = tokio::spawn(async move {
        service
            .handle(request(Call::Screenshot {
                auth: a,
                longest_edge: 1280,
                settle_ms: 0,
            }))
            .await
    });
    rx.await.unwrap();
    f.clock.0.store(30001, Ordering::SeqCst);
    release.send(()).unwrap();
    assert!(matches!(
        task.await.unwrap().outcome,
        ResponseOutcome::Error {
            error: ApiError {
                code: ErrorCode::FrameExpired,
                ..
            }
        }
    ));
    assert_eq!(f.effects.load(Ordering::SeqCst), 0);
    assert_eq!(f.service.usage().png_bytes, 0);
    f.service.shutdown().await;
}
#[test]
fn decode_pool_delay_is_bounded_and_retains_capacity_without_publishing() {
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .max_blocking_threads(1)
        .enable_all()
        .build()
        .unwrap();
    runtime.block_on(async {
        let f = setup(2, None, false).await;
        let a = acquire(&f.service).await;
        let (entered, rx) = tokio::sync::oneshot::channel();
        let (release, gate) = std::sync::mpsc::channel();
        let held = tokio::task::spawn_blocking(move || {
            entered.send(()).unwrap();
            gate.recv_timeout(Duration::from_secs(1)).unwrap();
        });
        rx.await.unwrap();
        let mut req = request(Call::Screenshot {
            auth: a.clone(),
            longest_edge: 1280,
            settle_ms: 0,
        });
        req.deadline_ms = 100;
        let response = f.service.handle(req).await;
        let capacity = f
            .service
            .handle(request(Call::Screenshot {
                auth: a,
                longest_edge: 1280,
                settle_ms: 0,
            }))
            .await;
        let status = f.service.handle(request(Call::Status)).await;
        release.send(()).unwrap();
        held.await.unwrap();
        assert!(matches!(
            response.outcome,
            ResponseOutcome::Error {
                error: ApiError {
                    code: ErrorCode::ProcessTimeout,
                    ..
                }
            }
        ));
        assert!(matches!(
            capacity.outcome,
            ResponseOutcome::Error {
                error: ApiError {
                    code: ErrorCode::QueueFull,
                    ..
                }
            }
        ));
        assert!(matches!(status.outcome, ResponseOutcome::Success { .. }));
        assert_eq!(f.service.usage().png_bytes, 0);
        assert_eq!(f.effects.load(Ordering::SeqCst), 0);
        f.service.shutdown().await;
    });
}

// Intentionally consume exchange time on the wire, not a sleep hiding an owner
// race. The longer observer read lets the red test see the old late EOF/result;
// it never changes the Server's two-second exchange deadline.
fn deadline_exchange(
    path: &std::path::Path,
    request: Request,
    after_handshake: Duration,
) -> std::io::Result<Response> {
    use cordgeist::ipc::{framing::*, server::*};
    use std::os::unix::net::UnixStream;
    let start = Instant::now();
    let deadline = start + MESSAGE_TIMEOUT;
    let mut client = UnixStream::connect(path)?;
    send(&mut client, &Hello { version: 1 }, deadline)?;
    let welcome: Welcome = receive(&mut client, deadline)?;
    assert!(welcome.compatible);
    if !after_handshake.is_zero() {
        std::thread::sleep(after_handshake);
    }
    send(&mut client, &request, deadline)?;
    let response: Response = receive(&mut client, start + Duration::from_secs(4))?;
    response.validate_for(&request, &welcome.boot_id).unwrap();
    Ok(response)
}
fn deadline_result(response: Response) -> OperationResult {
    match response.outcome {
        ResponseOutcome::Success { result } => *result,
        ResponseOutcome::Error { error } => panic!("safe deadline-probe code {:?}", error.code),
    }
}
#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn ipc_exchange_deadline_preflights_late_gesture_and_recovers_original_receipt() {
    use cordgeist::ipc::server::*;
    use std::os::unix::fs::PermissionsExt;
    let f = setup(4, None, false).await;
    let auth = acquire(&f.service).await;
    let frame = screenshot(&f.service, &auth).await.frame;
    let dir = tempfile::tempdir().unwrap();
    std::fs::set_permissions(dir.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
    let server = Server::bind_dispatcher(
        dir.path(),
        Arc::new(ServiceDispatcher {
            service: f.service.clone(),
            runtime: tokio::runtime::Handle::current(),
        }),
    )
    .unwrap();
    let stop = server.stop_handle();
    let path = dir.path().join("cordgeist/daemon.sock");
    let run = std::thread::spawn(move || server.run().unwrap());
    let effects = f.effects.clone();
    let (timely, late, duplicate, final_input) = tokio::task::spawn_blocking(move || {
        let hold = |seq, frame_id| {
            let mut request = request(Call::Hold {
                auth: auth.clone(),
                seq: ActionSeq(seq),
                frame_id,
                point: Point { x: 1., y: 1. },
                duration_ms: 800,
            });
            request.deadline_ms = 1500;
            request
        };
        // Same supported gesture succeeds with a timely request. A blanket hold
        // rejection must not be able to satisfy the late preflight assertion.
        let timely = match deadline_result(
            deadline_exchange(&path, hold(1, frame.id), Duration::ZERO).unwrap(),
        ) {
            OperationResult::Hold(a) => a,
            _ => panic!(),
        };
        let original = hold(2, timely.observation.as_ref().unwrap().frame.id.clone());
        let delayed = deadline_exchange(&path, original.clone(), Duration::from_millis(1500));
        eprintln!(
            "SOFTWARE deadline probe: delayed reply error={:?}, contact effects={}",
            delayed.as_ref().err().map(std::io::Error::kind),
            effects.load(Ordering::SeqCst)
        );
        // Recover the original admitted identity, even when the old implementation
        // loses its late reply. No retry is assigned a new sequence.
        let late = match deadline_result(
            deadline_exchange(
                &path,
                request(Call::ActionStatus {
                    auth: auth.clone(),
                    seq: ActionSeq(2),
                }),
                Duration::ZERO,
            )
            .unwrap(),
        ) {
            OperationResult::ActionStatus(ActionStatusResult::Final { action }) => *action,
            _ => panic!(),
        };
        let duplicate =
            match deadline_result(deadline_exchange(&path, original, Duration::ZERO).unwrap()) {
                OperationResult::Hold(a) => a,
                _ => panic!(),
            };
        let fresh = match deadline_result(
            deadline_exchange(
                &path,
                request(Call::Screenshot {
                    auth: auth.clone(),
                    longest_edge: 1280,
                    settle_ms: 0,
                }),
                Duration::ZERO,
            )
            .unwrap(),
        ) {
            OperationResult::Screenshot(s) => s.frame,
            _ => panic!(),
        };
        let final_input = action(deadline_result(
            deadline_exchange(&path, request(tap(&auth, &fresh, 3)), Duration::ZERO).unwrap(),
        ));
        (timely, late, duplicate, final_input)
    })
    .await
    .unwrap();
    stop.request();
    tokio::task::spawn_blocking(move || run.join().unwrap())
        .await
        .unwrap();
    assert_eq!(timely.receipt.input_status, InputStatus::Sent);
    assert_eq!(late.receipt.input_status, InputStatus::NotSent);
    assert_eq!(duplicate.receipt.action_id, late.receipt.action_id);
    assert_eq!(duplicate.receipt.input_status, InputStatus::NotSent);
    assert_eq!(final_input.receipt.input_status, InputStatus::Sent);
    assert_eq!(
        f.effects.load(Ordering::SeqCst),
        2,
        "only timely hold and final tap may have effects"
    );
}

#[tokio::test(flavor = "multi_thread", worker_threads = 2)]
async fn ipc_started_action_timeout_recovers_unknown_without_a_new_sequence() {
    use cordgeist::ipc::server::*;
    use std::os::unix::fs::PermissionsExt;
    // Exercise both requested-budget expiry and the earlier transport expiry.
    for after_handshake in [Duration::ZERO, Duration::from_millis(1500)] {
        let (entered, rx) = tokio::sync::oneshot::channel();
        let (release, gate) = std::sync::mpsc::channel();
        let f = setup(2, Some((entered, gate)), false).await;
        let auth = acquire(&f.service).await;
        let frame = screenshot(&f.service, &auth).await.frame;
        let dir = tempfile::tempdir().unwrap();
        std::fs::set_permissions(dir.path(), std::fs::Permissions::from_mode(0o700)).unwrap();
        let server = Server::bind_dispatcher(
            dir.path(),
            Arc::new(ServiceDispatcher {
                service: f.service.clone(),
                runtime: tokio::runtime::Handle::current(),
            }),
        )
        .unwrap();
        let stop = server.stop_handle();
        let path = dir.path().join("cordgeist/daemon.sock");
        let run = std::thread::spawn(move || server.run().unwrap());
        let mut original = request(tap(&auth, &frame, 1));
        original.deadline_ms = if after_handshake.is_zero() { 100 } else { 1500 };
        let client_path = path.clone();
        let client_request = original.clone();
        let client = tokio::task::spawn_blocking(move || {
            deadline_exchange(&client_path, client_request, after_handshake)
        });
        rx.await.unwrap(); // actual fake effect recorded inside the claimed call
        let reply = client.await.unwrap();
        release.send(()).unwrap();
        let (recovered, duplicate) = tokio::task::spawn_blocking(move || {
            // This is bounded status observation, not an input retry loop.
            let lookup_deadline = Instant::now() + Duration::from_secs(2);
            let recovered = loop {
                let result = deadline_result(
                    deadline_exchange(
                        &path,
                        request(Call::ActionStatus {
                            auth: auth.clone(),
                            seq: ActionSeq(1),
                        }),
                        Duration::ZERO,
                    )
                    .unwrap(),
                );
                match result {
                    OperationResult::ActionStatus(ActionStatusResult::Final { action }) => {
                        break *action;
                    }
                    OperationResult::ActionStatus(_) => assert!(
                        Instant::now() < lookup_deadline,
                        "original receipt must finalize"
                    ),
                    _ => panic!(),
                }
            };
            // A new connection/wait budget never grants a new execution deadline to
            // the original action, nor does an old frame force a new action identity.
            original.deadline_ms = 1000;
            let duplicate = action(deadline_result(
                deadline_exchange(&path, original, Duration::ZERO).unwrap(),
            ));
            (recovered, duplicate)
        })
        .await
        .unwrap();
        stop.request();
        tokio::task::spawn_blocking(move || run.join().unwrap())
            .await
            .unwrap();
        match reply {
            Ok(reply) => assert!(matches!(
                reply.outcome,
                ResponseOutcome::Error {
                    error: ApiError {
                        code: ErrorCode::ProcessTimeout,
                        ..
                    }
                }
            )),
            Err(error) => {
                assert!(
                    !after_handshake.is_zero(),
                    "requested budget has time for a typed timeout reply"
                );
                assert!(
                    matches!(
                        error.kind(),
                        std::io::ErrorKind::UnexpectedEof | std::io::ErrorKind::ConnectionReset
                    ),
                    "safe reply loss at original exchange deadline"
                );
            }
        }
        assert_eq!(recovered.receipt.input_status, InputStatus::Unknown);
        assert_eq!(duplicate.receipt.input_status, InputStatus::Unknown);
        assert_eq!(duplicate.receipt.action_id, recovered.receipt.action_id);
        assert_eq!(duplicate.receipt.seq, ActionSeq(1));
        assert_eq!(f.effects.load(Ordering::SeqCst), 1);
    }
}
#[tokio::test]
async fn expired_external_deadline_rejects_admission_and_budget_cap_stays_validated() {
    let f = setup(2, None, false).await;
    let acquire_call = Call::Acquire {
        device: DeviceId("phone".into()),
        reason: "deadline admission".into(),
        label: None,
        idle_ttl_s: 30,
    };
    let response = f
        .service
        .handle_until(request(acquire_call.clone()), Instant::now())
        .await;
    assert!(matches!(
        response.outcome,
        ResponseOutcome::Error {
            error: ApiError {
                code: ErrorCode::ProcessTimeout,
                ..
            }
        }
    ));
    let mut invalid = request(acquire_call.clone());
    invalid.deadline_ms = 1501;
    let response = f
        .service
        .handle_until(invalid, Instant::now() + Duration::from_millis(100))
        .await;
    assert!(matches!(
        response.outcome,
        ResponseOutcome::Error {
            error: ApiError {
                code: ErrorCode::InvalidRequest,
                ..
            }
        }
    ));
    assert!(matches!(
        result(&f.service, acquire_call).await,
        OperationResult::Acquire(_)
    ));
    assert_eq!(f.effects.load(Ordering::SeqCst), 0);
    f.service.shutdown().await;
}

#[tokio::test]
async fn unverified_mapping_fences_touch_and_keyboard_before_effects() {
    let f = setup_mapping(4, None, false, None, false).await;
    let auth = acquire(&f.service).await;
    let frame = screenshot(&f.service, &auth).await.frame;
    let calls = [
        tap(&auth, &frame, 1),
        Call::Type {
            submit: false,
            auth: auth.clone(),
            seq: ActionSeq(1),
            frame_id: frame.id.clone(),
            text: "synthetic".into(),
        },
        Call::Key {
            auth: auth.clone(),
            seq: ActionSeq(1),
            frame_id: frame.id.clone(),
            key: "enter".into(),
        },
    ];
    for call in calls {
        assert_eq!(error(&f.service, call).await, ErrorCode::MappingUnverified);
    }
    assert_eq!(f.effects.load(Ordering::SeqCst), 0);
    f.service.shutdown().await;
}

#[tokio::test]
async fn calibration_setup_cannot_take_over_a_held_phone_or_send_input() {
    let f = setup(4, None, false).await;
    let auth = acquire(&f.service).await;
    assert_eq!(
        error(
            &f.service,
            Call::Calibrate {
                device: Some(auth.device_id.clone()),
                probe: false,
            }
        )
        .await,
        ErrorCode::Leased
    );
    let frame = screenshot(&f.service, &auth).await.frame;
    assert_eq!(
        action(result(&f.service, tap(&auth, &frame, 1)).await)
            .receipt
            .input_status,
        InputStatus::Sent
    );
    f.service.shutdown().await;
    let f = setup(4, None, false).await;
    assert_eq!(
        error(
            &f.service,
            Call::Calibrate {
                device: Some(DeviceId("phone".into())),
                probe: false,
            }
        )
        .await,
        ErrorCode::MappingUnverified
    );
    assert_eq!(f.effects.load(Ordering::SeqCst), 0);
    f.service.shutdown().await;
}

struct PendingSetup;
impl CaptureSource for PendingSetup {
    fn capture<'a>(&'a self, _: ResolvedRoute, _: CancellationToken) -> CaptureFuture<'a> {
        Box::pin(async { Err(ErrorCode::CaptureUnavailable) })
    }
    fn calibration_axes<'a>(
        &'a self,
        _: ResolvedRoute,
        _: Size,
        _: (u16, u16),
        _: CancellationToken,
    ) -> std::pin::Pin<
        Box<
            dyn std::future::Future<
                    Output = Result<cordgeist::calibration::AxisEvidence, ErrorCode>,
                > + Send
                + 'a,
        >,
    > {
        Box::pin(std::future::pending())
    }
}
#[tokio::test]
async fn abandoned_setup_waiter_releases_hidden_ownership_without_input() {
    let f = setup_capture(4, None, false, Some(Arc::new(PendingSetup))).await;
    let mut req = request(Call::Calibrate {
        device: Some(DeviceId("phone".into())),
        probe: false,
    });
    req.deadline_ms = 30;
    assert!(
        matches!(f.service.handle(req).await.outcome, ResponseOutcome::Error { error } if error.code == ErrorCode::ProcessTimeout)
    );
    tokio::time::timeout(Duration::from_secs(1), async {
        loop {
            let reply = f.service.handle(request(Call::Acquire { device: DeviceId("phone".into()), reason: "synthetic cleanup check".into(), label: None, idle_ttl_s: 30 })).await;
            if !matches!(reply.outcome, ResponseOutcome::Error { error } if error.code == ErrorCode::Leased) { break; }
            tokio::task::yield_now().await;
        }
    }).await.unwrap();
    assert_eq!(f.effects.load(Ordering::SeqCst), 0);
    f.service.shutdown().await;
}

struct AxesWithoutMarker;
impl CaptureSource for AxesWithoutMarker {
    fn capture<'a>(&'a self, route: ResolvedRoute, cancel: CancellationToken) -> CaptureFuture<'a> {
        Box::pin(async move {
            FakeCapture {
                mode: Arc::new(AtomicUsize::new(0)),
            }
            .capture(route, cancel)
            .await
        })
    }
    fn calibration_axes<'a>(
        &'a self,
        _: ResolvedRoute,
        display: Size,
        maxima: (u16, u16),
        _: CancellationToken,
    ) -> std::pin::Pin<
        Box<
            dyn std::future::Future<
                    Output = Result<cordgeist::calibration::AxisEvidence, ErrorCode>,
                > + Send
                + 'a,
        >,
    > {
        Box::pin(async move {
            Ok(cordgeist::calibration::AxisEvidence {
                maximum_x: maxima.0,
                maximum_y: maxima.1,
                display,
            })
        })
    }
}
#[tokio::test]
async fn explicit_probe_requires_visible_owned_marker_before_any_input() {
    let f = setup_capture(4, None, false, Some(Arc::new(AxesWithoutMarker))).await;
    assert_eq!(
        error(
            &f.service,
            Call::Calibrate {
                device: Some(DeviceId("phone".into())),
                probe: true
            }
        )
        .await,
        ErrorCode::MappingUnverified
    );
    assert_eq!(f.effects.load(Ordering::SeqCst), 0);
    f.service.shutdown().await;
}

#[tokio::test]
async fn slow_observation_returns_known_sent_and_duplicate_never_repeats() {
    let f = setup(4, None, false).await;
    let auth = acquire(&f.service).await;
    let frame = screenshot(&f.service, &auth).await.frame;
    f.mode.store(3, Ordering::SeqCst);
    let call = tap(&auth, &frame, 1);
    let mut req = request(call.clone());
    req.deadline_ms = 500;
    let response = f.service.handle(req).await;
    let action = match response.outcome {
        ResponseOutcome::Success { result } => action(*result),
        ResponseOutcome::Error { error } => panic!("known delivery was hidden: {}", error.code),
    };
    assert_eq!(action.receipt.input_status, InputStatus::Sent);
    assert_eq!(
        action.receipt.observation_status,
        ObservationStatus::Timeout
    );
    assert_eq!(f.effects.load(Ordering::SeqCst), 1);
    let again = match result(&f.service, call).await {
        OperationResult::Tap(a) => a,
        _ => panic!("wrong method"),
    };
    assert_eq!(again.receipt.action_id, action.receipt.action_id);
    assert_eq!(again.receipt.input_status, InputStatus::Sent);
    assert_eq!(f.effects.load(Ordering::SeqCst), 1);
    f.service.shutdown().await;
}

#[tokio::test]
async fn automatic_setup_refuses_ambiguous_selection_without_input() {
    let f = setup(4, None, false).await;
    assert_eq!(
        error(
            &f.service,
            Call::Calibrate {
                device: None,
                probe: true
            }
        )
        .await,
        ErrorCode::DeviceIdentityAmbiguous
    );
    assert_eq!(f.effects.load(Ordering::SeqCst), 0);
    f.service.shutdown().await;
}

struct CountCapture {
    count: Arc<AtomicUsize>,
    inner: FakeCapture,
}
impl CaptureSource for CountCapture {
    fn capture<'a>(&'a self, route: ResolvedRoute, cancel: CancellationToken) -> CaptureFuture<'a> {
        self.count.fetch_add(1, Ordering::SeqCst);
        self.inner.capture(route, cancel)
    }
}
#[tokio::test]
async fn default_action_has_no_capture_and_observation_policy_is_replay_identity() {
    let count = Arc::new(AtomicUsize::new(0));
    let source = CountCapture {
        count: count.clone(),
        inner: FakeCapture {
            mode: Arc::new(AtomicUsize::new(0)),
        },
    };
    let f = setup_capture(4, None, true, Some(Arc::new(source))).await;
    let auth = acquire(&f.service).await;
    let frame = screenshot(&f.service, &auth).await.frame;
    assert_eq!(count.load(Ordering::SeqCst), 1);
    let call = tap(&auth, &frame, 1);
    let mut req = request(call.clone());
    req.observe_after = false;
    let response = f.service.handle(req.clone()).await;
    let ResponseOutcome::Success { result } = response.outcome else {
        panic!("action rejected")
    };
    let done = action(*result);
    assert_eq!(done.receipt.input_status, InputStatus::Sent);
    assert_eq!(
        done.receipt.observation_status,
        ObservationStatus::NotRequested
    );
    assert!(done.observation.is_none());
    assert_eq!(
        count.load(Ordering::SeqCst),
        1,
        "default must not secretly capture"
    );
    req.observe_after = true;
    assert!(
        matches!(f.service.handle(req).await.outcome,ResponseOutcome::Error{error} if error.code==ErrorCode::SequenceMismatch)
    );
    let mut req = request(tap(&auth, &frame, 2));
    req.observe_after = true;
    assert!(
        matches!(f.service.handle(req).await.outcome,ResponseOutcome::Success{result} if matches!(result.as_ref(),OperationResult::Tap(a) if a.receipt.observation_status==ObservationStatus::Captured))
    );
    assert_eq!(count.load(Ordering::SeqCst), 2);
    screenshot(&f.service, &auth).await;
    assert_eq!(count.load(Ordering::SeqCst), 3);
    f.service.shutdown().await;
}

#[tokio::test]
async fn screen_pixels_convert_in_dispatcher_and_legacy_pixels_do_not_mix() {
    let f = setup(4, None, true).await;
    let auth = acquire(&f.service).await;
    let result = result(
        &f.service,
        Call::Screenshot {
            auth: auth.clone(),
            longest_edge: 4,
            settle_ms: 0,
        },
    )
    .await;
    let OperationResult::Screenshot(shot) = result else {
        panic!()
    };
    assert_eq!(
        shot.frame.image,
        Size {
            width: 2,
            height: 4
        }
    );
    assert_eq!(
        shot.frame.display,
        Size {
            width: 4,
            height: 8
        }
    );
    let mut req = request(Call::Tap {
        auth: auth.clone(),
        seq: ActionSeq(1),
        frame_id: shot.frame.id.clone(),
        point: Point { x: 3., y: 7. },
    });
    req.coordinate_space = CoordinateSpace::ScreenPixels;
    req.observe_after = false;
    assert!(
        matches!(f.service.handle(req).await.outcome,ResponseOutcome::Success{result} if matches!(result.as_ref(),OperationResult::Tap(a) if a.receipt.input_status==InputStatus::Sent))
    );
    assert_eq!(
        error(
            &f.service,
            Call::Tap {
                auth,
                seq: ActionSeq(2),
                frame_id: shot.frame.id,
                point: Point { x: 3., y: 7. }
            }
        )
        .await,
        ErrorCode::InvalidRequest
    );
    assert_eq!(f.effects.load(Ordering::SeqCst), 1);
    f.service.shutdown().await;
}

#[tokio::test]
async fn generic_rotation_refresh_warns_retires_frames_and_retains_receipts() {
    // Software evidence only: dispatcher/actor geometry and receipt invariants.
    let f = setup_generic(4, None, true, None, false, true).await;
    let auth = acquire(&f.service).await;
    let first = screenshot(&f.service, &auth).await;
    let sent = action(result(&f.service, tap(&auth, &first.frame, 1)).await);
    assert_eq!(sent.receipt.input_status, InputStatus::Sent);
    f.mode.store(2, Ordering::SeqCst);
    let rotated = screenshot(&f.service, &auth).await;
    assert_eq!(rotated.frame.rotation, Rotation::R180);
    assert!(!rotated.warnings.is_empty());
    assert!(rotated.frame.provenance.geometry.0 > first.frame.provenance.geometry.0);
    assert_eq!(
        error(&f.service, tap(&auth, &first.frame, 2)).await,
        ErrorCode::FrameExpired
    );
    let sent = action(result(&f.service, tap(&auth, &rotated.frame, 3)).await);
    assert!(!sent.warnings.is_empty());
    assert_eq!(sent.receipt.input_status, InputStatus::Sent);
    let old = final_receipt(&f.service, &auth, 1).await;
    assert_eq!(old.receipt.seq, ActionSeq(1));
    assert_eq!(f.effects.load(Ordering::SeqCst), 2);
    f.service.shutdown().await;
}
