//! Integrated software service. One canonical actor authority and Session owner.
//! A discovered phone can be installed for ADB observation. HID stays closed
//! until a measured mapping exists.
use crate::{
    actor::WorkerToken,
    api::*,
    backend::{Cancel, Clock},
    frames::{FrameCache, Limits, Profile},
    identity::{IdentityRegistry, ResolvedRoute, RouteRef},
    lease_devices::LeaseDevices,
    session::{Phase, Session, Status},
    session_input::{Delivery, Transition},
};
use std::{
    collections::{HashMap, VecDeque},
    future::Future,
    pin::Pin,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicU8, Ordering},
    },
    time::{Duration, Instant},
};
use tokio::sync::{Semaphore, watch};
use tokio_util::sync::CancellationToken;

pub type CaptureFuture<'a> = Pin<Box<dyn Future<Output = Result<Capture, ErrorCode>> + Send + 'a>>;
/// Trusted owned observation boundary. Implementations must honor cancellation;
/// service additionally bounds callers and concurrent operations.
pub trait CaptureSource: Send + Sync {
    fn capture<'a>(&'a self, route: ResolvedRoute, cancel: CancellationToken) -> CaptureFuture<'a>;
    fn elements<'a>(
        &'a self,
        _route: ResolvedRoute,
        _cancel: CancellationToken,
    ) -> Pin<Box<dyn Future<Output = Result<ElementCapture, ErrorCode>> + Send + 'a>> {
        Box::pin(async { Err(ErrorCode::CaptureUnavailable) })
    }
    fn apps<'a>(
        &'a self,
        _route: ResolvedRoute,
        _cancel: CancellationToken,
    ) -> Pin<Box<dyn Future<Output = Result<Vec<String>, ErrorCode>> + Send + 'a>> {
        Box::pin(async { Err(ErrorCode::BackendUnavailable) })
    }
    fn calibration_axes<'a>(
        &'a self,
        _route: ResolvedRoute,
        _display: Size,
        _maxima: (u16, u16),
        _cancel: CancellationToken,
    ) -> Pin<
        Box<dyn Future<Output = Result<crate::calibration::AxisEvidence, ErrorCode>> + Send + 'a>,
    > {
        Box::pin(async { Err(ErrorCode::MappingUnverified) })
    }
}
pub struct ElementCapture {
    pub elements: Vec<UiElement>,
    pub display: Size,
    pub rotation: Rotation,
    pub build: String,
    pub descriptor: String,
    pub backend: String,
}
pub struct Capture {
    pub png: Vec<u8>,
    pub image: Size,
    pub display: Size,
    pub rotation: Rotation,
    pub build: String,
    pub descriptor: String,
    pub backend: String,
}
/// Actual observation adapter; exact target comes from current discovery, never
/// request arguments. This does not enable installation of a real input Session.
pub struct AdbCapture {
    pub adb: crate::adb::Adb,
    pub descriptor: String,
    pub backend: String,
}
impl CaptureSource for AdbCapture {
    fn elements<'a>(
        &'a self,
        route: ResolvedRoute,
        cancel: CancellationToken,
    ) -> Pin<Box<dyn Future<Output = Result<ElementCapture, ErrorCode>> + Send + 'a>> {
        Box::pin(async move {
            self.adb
                .check_attachment(route.adb_attachment(), cancel.clone())
                .await?;
            let before = self.adb.viewport(&route.target(), cancel.clone()).await?;
            let build = self
                .adb
                .build_identity(&route.target(), cancel.clone())
                .await?;
            let xml = self.adb.ui_xml(&route.target(), cancel.clone()).await?;
            let after = self.adb.viewport(&route.target(), cancel.clone()).await?;
            let build_after = self
                .adb
                .build_identity(&route.target(), cancel.clone())
                .await?;
            self.adb
                .check_attachment(route.adb_attachment(), cancel)
                .await?;
            if before != after || build != build_after {
                return Err(ErrorCode::GeometryChanged);
            }
            Ok(ElementCapture {
                elements: crate::android_ui::parse(&xml, after.logical)?,
                display: after.logical,
                rotation: after.rotation,
                build,
                descriptor: self.descriptor.clone(),
                backend: self.backend.clone(),
            })
        })
    }
    fn apps<'a>(
        &'a self,
        route: ResolvedRoute,
        cancel: CancellationToken,
    ) -> Pin<Box<dyn Future<Output = Result<Vec<String>, ErrorCode>> + Send + 'a>> {
        Box::pin(async move {
            self.adb
                .check_attachment(route.adb_attachment(), cancel.clone())
                .await?;
            let apps = self.adb.list_apps(&route.target(), cancel.clone()).await?;
            self.adb
                .check_attachment(route.adb_attachment(), cancel)
                .await?;
            Ok(apps)
        })
    }
    fn calibration_axes<'a>(
        &'a self,
        route: ResolvedRoute,
        display: Size,
        maxima: (u16, u16),
        cancel: CancellationToken,
    ) -> Pin<
        Box<dyn Future<Output = Result<crate::calibration::AxisEvidence, ErrorCode>> + Send + 'a>,
    > {
        Box::pin(async move {
            self.adb
                .prepare_test_area(&route.target(), cancel.clone())
                .await?;
            self.adb
                .calibration_axes(&route.target(), display, maxima, cancel)
                .await
        })
    }

    fn capture<'a>(&'a self, route: ResolvedRoute, cancel: CancellationToken) -> CaptureFuture<'a> {
        Box::pin(async move {
            let before = self
                .adb
                .build_identity(&route.target(), cancel.clone())
                .await?;
            let observed = self
                .adb
                .observe(route.adb_attachment(), cancel.clone())
                .await?;
            let after = self.adb.build_identity(&route.target(), cancel).await?;
            if before != after {
                return Err(ErrorCode::GeometryChanged);
            }
            Ok(Capture {
                png: observed.png,
                image: observed.image,
                display: observed.metadata.logical,
                rotation: observed.metadata.rotation,
                build: before,
                descriptor: self.descriptor.clone(),
                backend: self.backend.clone(),
            })
        })
    }
}
// Keeps hidden setup authority owned even when the IPC waiter times out.
// Cleanup runs independently of the abandoned caller and never grants a successor.
struct SetupOwnership {
    service: Arc<Service>,
    auth: Option<LeaseAuth>,
}
impl Drop for SetupOwnership {
    fn drop(&mut self) {
        if let Some(auth) = self.auth.take() {
            let service = self.service.clone();
            tokio::spawn(async move {
                let _ = service.leases.release(&auth, Duration::from_secs(2)).await;
            });
        }
    }
}
#[cfg(all(target_os = "linux", feature = "native-aoa"))]
struct RearmAttempt {
    session: Session,
    armed: bool,
}
#[cfg(all(target_os = "linux", feature = "native-aoa"))]
impl Drop for RearmAttempt {
    fn drop(&mut self) {
        if self.armed {
            let session = self.session.clone();
            tokio::spawn(async move {
                let _ = session.shutdown(Duration::from_secs(2)).await;
            });
        }
    }
}
struct CancelCapture(CancellationToken);
impl Drop for CancelCapture {
    fn drop(&mut self) {
        self.0.cancel();
    }
}
struct Record {
    seq: ActionSeq,
    digest: blake3::Hash,
    id: ActionId,
    cancel: Cancel,
    requested: Arc<AtomicBool>,
    execution: Arc<AtomicU8>,
    result: watch::Receiver<Option<ActionResult>>,
}
// This guard is captured before spawning: even an unpolled/aborted completion
// future retains a final no-replay receipt and signals the existing cleanup owner.
struct Completion {
    sender: watch::Sender<Option<ActionResult>>,
    service: Arc<Service>,
    device: Arc<Device>,
    execution: Arc<AtomicU8>,
    cancel: Cancel,
    fallback: Option<ActionResult>,
}
impl Completion {
    fn finish(&mut self, action: ActionResult) {
        self.sender.send_replace(Some(action));
        self.fallback = None;
    }
}
impl Drop for Completion {
    fn drop(&mut self) {
        if let Some(mut action) = self.fallback.take() {
            self.cancel.cancel();
            action.receipt.input_status = if self
                .execution
                .compare_exchange(0, 2, Ordering::SeqCst, Ordering::SeqCst)
                .is_ok()
            {
                InputStatus::NotSent
            } else {
                InputStatus::Unknown
            };
            action.receipt.finished_ms = self.service.clock.now_ms();
            let _ = self
                .service
                .leases
                .worker_failed(&self.device.route.device_id, &self.device.token);
            self.sender.send_replace(Some(action));
        }
    }
}
struct DeviceState {
    frames: FrameCache,
    high_water: u64,
    records: VecDeque<Record>,
    media: HashMap<u64, StoredMedia>,
    /// Claimed before a transfer starts. Dropped if that transfer does not finish.
    media_reserved: HashMap<u64, Reservation>,
}
struct Reservation {
    digest: [u8; 32],
    kind: MediaKind,
}
struct ClearReservation<'a> {
    state: &'a Mutex<DeviceState>,
    gate: &'a std::sync::Condvar,
    seq: u64,
}
impl Drop for ClearReservation<'_> {
    fn drop(&mut self) {
        self.state.lock().unwrap().media_reserved.remove(&self.seq);
        self.gate.notify_all();
    }
}
struct StoredMedia {
    result: MediaPushResult,
    digest: [u8; 32],
    kind: MediaKind,
}
struct Device {
    route: RouteRef,
    identities: Arc<Mutex<IdentityRegistry>>,
    profile: Mutex<Profile>,
    capture: Arc<dyn CaptureSource>,
    capture_capacity: Arc<Semaphore>,
    ordinary: Arc<Semaphore>,
    token: WorkerToken,
    state: Mutex<DeviceState>,
    media_gate: std::sync::Condvar,
}
impl Device {
    fn profile(&self) -> Profile {
        self.profile.lock().unwrap().clone()
    }
}
pub struct Service {
    #[cfg(feature = "fake")]
    software_rearm: tokio::sync::Mutex<Option<crate::software_fixture::Rearm>>,
    boot: BootId,
    closing: AtomicBool,
    clock: Arc<dyn Clock + Send + Sync>,
    limits: Limits,
    leases: LeaseDevices,
    devices: Mutex<HashMap<DeviceId, Arc<Device>>>,
    key: [u8; 32],
    events: Mutex<VecDeque<SafeEvent>>,
    ordinary: Arc<Semaphore>,
    media_root: Mutex<Option<std::path::PathBuf>>,
    /// Redacted discovery result. No serial, USB path, or session.
    discovery: Mutex<DiscoveryNote>,
    #[cfg(all(target_os = "linux", feature = "native-aoa"))]
    phone: tokio::sync::Mutex<Option<crate::discovery_service::DiscoveryService<crate::adb::Adb>>>,
    #[cfg(all(target_os = "linux", feature = "native-aoa"))]
    rearm_gate: tokio::sync::Mutex<()>,
    #[cfg(all(target_os = "linux", feature = "native-aoa"))]
    pending_rearm: Mutex<Option<Session>>,
}
#[derive(Clone, Debug)]
struct DiscoveryNote {
    code: String,
    device_id: Option<DeviceId>,
    attachment: Option<AttachmentGeneration>,
}
fn random() -> Result<String, ErrorCode> {
    let mut b = [0; 32];
    getrandom::fill(&mut b).map_err(|_| ErrorCode::BackendUnavailable)?;
    Ok(b.iter().map(|v| format!("{v:02x}")).collect())
}
fn cleanup(s: Status, active: bool) -> Cleanup {
    if s.cleanup_error.is_some() {
        Cleanup::Failed
    } else if active
        || matches!(
            s.phase,
            Phase::Closing | Phase::Quarantined | Phase::Initializing | Phase::Settling
        )
    {
        Cleanup::Pending
    } else {
        Cleanup::Neutral
    }
}
/// Diagnostics are a finite ring of catalogue names and stable codes only.
#[derive(Clone, Debug)]
pub struct SafeEvent {
    pub method: &'static str,
    pub error: Option<ErrorCode>,
}
#[derive(Clone, Copy, Debug)]
pub struct Usage {
    pub devices: usize,
    pub png_bytes: usize,
    pub receipt_image_bytes: usize,
    pub receipts: usize,
    pub events: usize,
}
impl Service {
    pub fn new(
        boot: BootId,
        clock: Arc<dyn Clock + Send + Sync>,
        limits: Limits,
    ) -> Result<Arc<Self>, ErrorCode> {
        limits.validate()?;
        if boot.0.is_empty() || boot.0.len() > 128 {
            return Err(ErrorCode::InvalidRequest);
        }
        let mut key = [0; 32];
        getrandom::fill(&mut key).map_err(|_| ErrorCode::BackendUnavailable)?;
        Ok(Arc::new(Self {
            #[cfg(feature = "fake")]
            software_rearm: tokio::sync::Mutex::new(None),
            boot,
            closing: AtomicBool::new(false),
            leases: LeaseDevices::new(clock.clone()),
            clock,
            limits,
            devices: Mutex::new(HashMap::new()),
            key,
            events: Mutex::new(VecDeque::new()),
            ordinary: Arc::new(Semaphore::new(6)),
            media_root: Mutex::new(None),
            discovery: Mutex::new(DiscoveryNote {
                code: "software-only-live-input-gated".into(),
                device_id: None,
                attachment: None,
            }),
            #[cfg(all(target_os = "linux", feature = "native-aoa"))]
            phone: tokio::sync::Mutex::new(None),
            #[cfg(all(target_os = "linux", feature = "native-aoa"))]
            rearm_gate: tokio::sync::Mutex::new(()),
            #[cfg(all(target_os = "linux", feature = "native-aoa"))]
            pending_rearm: Mutex::new(None),
        }))
    }
    #[cfg(all(target_os = "linux", feature = "native-aoa"))]
    pub fn attach_phone(&self, phone: crate::discovery_service::DiscoveryService<crate::adb::Adb>) {
        *self.phone.try_lock().expect("initial discovery attachment") = Some(phone);
    }
    async fn confirm_phone(&self) -> Result<(), ErrorCode> {
        #[cfg(all(target_os = "linux", feature = "native-aoa"))]
        {
            let mut owner = self.phone.try_lock().map_err(|_| ErrorCode::QueueFull)?;
            if let Some(phone) = owner.as_mut() {
                return phone
                    .confirm(
                        Duration::from_millis(900),
                        &crate::backend::Cancel::default(),
                    )
                    .await;
            }
        }
        Ok(())
    }
    /// Records one exact native identity, or a stable failure. Does not open a session.
    pub fn record_discovery(&self, note: Result<crate::identity::RouteRef, ErrorCode>) {
        let mut slot = self.discovery.lock().unwrap();
        match note {
            Ok(route) => {
                slot.code = "identity-observed".into();
                slot.attachment = Some(route.attachment);
                slot.device_id = Some(route.device_id);
            }
            Err(code) => {
                slot.code = code.to_string();
                slot.device_id = None;
                slot.attachment = None;
            }
        }
    }
    pub fn events(&self) -> Vec<SafeEvent> {
        self.events.lock().unwrap().iter().cloned().collect()
    }
    pub fn usage(&self) -> Usage {
        let devices = self.devices.lock().unwrap();
        let mut usage = Usage {
            devices: devices.len(),
            png_bytes: 0,
            receipt_image_bytes: 0,
            receipts: 0,
            events: self.events.lock().unwrap().len(),
        };
        for d in devices.values() {
            let s = d.state.lock().unwrap();
            usage.png_bytes += s.frames.bytes();
            usage.receipts += s.records.len();
            usage.receipt_image_bytes += s
                .records
                .iter()
                .filter_map(|r| {
                    r.result
                        .borrow()
                        .as_ref()
                        .and_then(|a| a.observation.as_ref())
                        .map(|o| o.image.base64.len())
                })
                .sum::<usize>();
        }
        usage
    }
    pub fn boot_id(&self) -> &BootId {
        &self.boot
    }
    pub fn set_media_root(&self, path: std::path::PathBuf) {
        *self.media_root.lock().unwrap() = Some(path);
    }
    fn native_ready(&self, d: &Device) -> bool {
        d.profile().backend != "software-fake"
            && d.profile().transform.usable()
            && d.identities.lock().unwrap().resolve(&d.route).is_ok()
            && self.leases.status(&d.route.device_id).is_ok_and(|s| {
                (!s.fenced && s.session.phase == Phase::Ready)
                    || (s.fenced
                        && s.session.phase == Phase::Closed
                        && !s.active
                        && s.queued == 0
                        && s.session.cleanup_error.is_none()
                        && matches!(
                            s.termination,
                            Some(
                                crate::actor::TerminationReason::Released
                                    | crate::actor::TerminationReason::IdleExpired
                                    | crate::actor::TerminationReason::Revoked
                            )
                        ))
            })
    }
    fn current_capabilities(&self, d: &Device) -> DeviceCapabilities {
        let mut caps = capabilities();
        if self.native_ready(d) {
            caps.screenshot = Capability {
                evidence: Evidence::Detected,
                reason: "Exact native phone observation adapter".into(),
                profile: None,
            };
            caps.keyboard = Capability {
                evidence: Evidence::Detected,
                reason:
                    "US-ANSI ASCII backend; discrete keys restricted pending per-phone measurements"
                        .into(),
                profile: None,
            };
            caps.keyboard_layout = Some("us-ansi".into());
            caps.supported_keys.clear();
            caps.aoa_touch = Capability {
                evidence: if d.profile().transform.verified {
                    Evidence::Verified
                } else {
                    Evidence::Detected
                },
                reason: if d.profile().transform.verified {
                    "Per-phone synthetic targets verified"
                } else {
                    "Generic wm-size/descriptor mapping; not physically verified"
                }
                .into(),
                profile: Some(d.profile().descriptor.clone()),
            };
        }
        caps
    }
    fn backend_kind(&self) -> BackendKind {
        let devices = self.devices.lock().unwrap();
        if devices.values().any(|d| self.native_ready(d)) {
            BackendKind::AoaHid
        } else if devices.is_empty() {
            BackendKind::Unavailable
        } else if devices
            .values()
            .all(|device| device.profile().backend == "software-fake")
        {
            BackendKind::FakeSoftwareOnly
        } else {
            BackendKind::Unavailable
        }
    }
    /// Explicit SOFTWARE ONLY setup; real discovery is rejected, regardless of
    /// public profile flags. IPC has no install or profile-verification operation.
    pub fn install_software(
        &self,
        identities: Arc<Mutex<IdentityRegistry>>,
        route: RouteRef,
        session: Session,
        profile: Profile,
        capture: Arc<dyn CaptureSource>,
    ) -> Result<(), ErrorCode> {
        if self.closing.load(Ordering::SeqCst)
            || route.boot_id != self.boot
            || identities.lock().unwrap().resolve(&route)?.kind()
                != crate::discovery::DiscoveryKind::SoftwareFake
        {
            return Err(ErrorCode::BackendUnavailable);
        }
        profile.validate(
            &profile.build,
            &profile.descriptor,
            &profile.backend,
            profile.display,
            profile.rotation,
        )?;
        let mut devices = self.devices.lock().unwrap();
        if self.closing.load(Ordering::SeqCst) {
            return Err(ErrorCode::BackendUnavailable);
        }
        if devices.len() >= self.limits.devices {
            return Err(ErrorCode::QueueFull);
        }
        if devices.contains_key(&route.device_id) {
            return Err(ErrorCode::Leased);
        }
        let token = self
            .leases
            .register_routed(identities.clone(), route.clone(), session)?;
        devices.insert(
            route.device_id.clone(),
            Arc::new(Device {
                route,
                identities,
                profile: Mutex::new(profile),
                capture,
                capture_capacity: Arc::new(Semaphore::new(1)),
                ordinary: Arc::new(Semaphore::new(4)),
                token,
                state: Mutex::new(DeviceState {
                    frames: FrameCache::new(self.limits.clone())?,
                    high_water: 0,
                    records: VecDeque::new(),
                    media: HashMap::new(),
                    media_reserved: HashMap::new(),
                }),
                media_gate: std::sync::Condvar::new(),
            }),
        );
        Ok(())
    }
    /// Trusted daemon installation after per-phone stored-profile validation.
    /// Unverified profiles remain observation/setup-only at ordinary admission.
    pub fn install_observed(
        &self,
        identities: Arc<Mutex<IdentityRegistry>>,
        route: RouteRef,
        session: Session,
        profile: Profile,
        capture: Arc<dyn CaptureSource>,
    ) -> Result<(), ErrorCode> {
        if self.closing.load(Ordering::SeqCst)
            || route.boot_id != self.boot
            || identities.lock().unwrap().resolve(&route)?.kind()
                != crate::discovery::DiscoveryKind::Native
            || profile.backend == "software-fake"
        {
            return Err(ErrorCode::BackendUnavailable);
        }
        profile.validate(
            &profile.build,
            &profile.descriptor,
            &profile.backend,
            profile.display,
            profile.rotation,
        )?;
        let mut devices = self.devices.lock().unwrap();
        if self.closing.load(Ordering::SeqCst) {
            return Err(ErrorCode::BackendUnavailable);
        }
        if devices.len() >= self.limits.devices {
            return Err(ErrorCode::QueueFull);
        }
        if devices.contains_key(&route.device_id) {
            return Err(ErrorCode::Leased);
        }
        let token = self
            .leases
            .register_routed(identities.clone(), route.clone(), session)?;
        devices.insert(
            route.device_id.clone(),
            Arc::new(Device {
                route,
                identities,
                profile: Mutex::new(profile),
                capture,
                capture_capacity: Arc::new(Semaphore::new(1)),
                ordinary: Arc::new(Semaphore::new(4)),
                token,
                state: Mutex::new(DeviceState {
                    frames: FrameCache::new(self.limits.clone())?,
                    high_water: 0,
                    records: VecDeque::new(),
                    media: HashMap::new(),
                    media_reserved: HashMap::new(),
                }),
                media_gate: std::sync::Condvar::new(),
            }),
        );
        Ok(())
    }
    /// Deliberate fresh software attachment only. The canonical registry refuses
    /// replacement until the old Session has consumed every handle successfully.
    pub fn reconnect_software(
        &self,
        identities: Arc<Mutex<IdentityRegistry>>,
        route: RouteRef,
        session: Session,
        profile: Profile,
        capture: Arc<dyn CaptureSource>,
    ) -> Result<(), ErrorCode> {
        self.reconnect_trusted(
            identities,
            route,
            session,
            profile,
            capture,
            crate::discovery::DiscoveryKind::SoftwareFake,
        )
    }
    fn reconnect_trusted(
        &self,
        identities: Arc<Mutex<IdentityRegistry>>,
        route: RouteRef,
        session: Session,
        profile: Profile,
        capture: Arc<dyn CaptureSource>,
        expected_kind: crate::discovery::DiscoveryKind,
    ) -> Result<(), ErrorCode> {
        if self.closing.load(Ordering::SeqCst)
            || route.boot_id != self.boot
            || identities.lock().unwrap().resolve(&route)?.kind() != expected_kind
        {
            return Err(ErrorCode::BackendUnavailable);
        }
        profile.validate(
            &profile.build,
            &profile.descriptor,
            &profile.backend,
            profile.display,
            profile.rotation,
        )?;
        let mut devices = self.devices.lock().unwrap();
        if self.closing.load(Ordering::SeqCst) {
            return Err(ErrorCode::BackendUnavailable);
        }
        if !devices.contains_key(&route.device_id) {
            return Err(ErrorCode::BackendUnavailable);
        }
        let token = self
            .leases
            .reconnect(identities.clone(), route.clone(), session)?;
        devices.insert(
            route.device_id.clone(),
            Arc::new(Device {
                route,
                identities,
                profile: Mutex::new(profile),
                capture,
                capture_capacity: Arc::new(Semaphore::new(1)),
                ordinary: Arc::new(Semaphore::new(4)),
                token,
                state: Mutex::new(DeviceState {
                    frames: FrameCache::new(self.limits.clone())?,
                    high_water: 0,
                    records: VecDeque::new(),
                    media: HashMap::new(),
                    media_reserved: HashMap::new(),
                }),
                media_gate: std::sync::Condvar::new(),
            }),
        );
        Ok(())
    }
    #[cfg(feature = "fake")]
    pub(crate) async fn set_software_rearm(&self, rearm: crate::software_fixture::Rearm) {
        *self.software_rearm.lock().await = Some(rearm);
    }
    #[cfg(feature = "fake")]
    async fn rearm_software(&self, id: &DeviceId, deadline: Instant) -> Result<(), ErrorCode> {
        let old = self.device(id)?;
        if old.profile().backend != "software-fake" {
            return Ok(());
        }
        let status = self.leases.status(id)?;
        if !status.fenced && status.session.phase == Phase::Ready {
            return Ok(());
        }
        if !status.fenced
            || status.active
            || status.queued != 0
            || status.session.phase != Phase::Closed
            || status.session.cleanup_error.is_some()
            || !matches!(
                status.termination,
                Some(
                    crate::actor::TerminationReason::Released
                        | crate::actor::TerminationReason::Revoked
                        | crate::actor::TerminationReason::IdleExpired
                )
            )
        {
            return Err(ErrorCode::NativeCleanupPending);
        }
        let mut guard = self
            .software_rearm
            .try_lock()
            .map_err(|_| ErrorCode::QueueFull)?;
        if let Some(factory) = guard.as_mut() {
            factory
                .rearm(
                    self,
                    id,
                    old.profile().clone(),
                    old.capture.clone(),
                    deadline,
                )
                .await?;
        }
        Ok(())
    }
    #[cfg(all(target_os = "linux", feature = "native-aoa"))]
    async fn rearm_native(
        &self,
        id: &DeviceId,
        deadline: Instant,
        setup: bool,
    ) -> Result<(), ErrorCode> {
        let _gate = self
            .rearm_gate
            .try_lock()
            .map_err(|_| ErrorCode::QueueFull)?;
        if self.closing.load(Ordering::SeqCst) {
            return Err(ErrorCode::BackendUnavailable);
        }
        let pending = self.pending_rearm.lock().unwrap().clone();
        if let Some(session) = pending {
            let status = session
                .shutdown(self.budget(deadline)?.min(Duration::from_secs(2)))
                .await?;
            if status.phase != Phase::Closed || status.cleanup_error.is_some() {
                return Err(ErrorCode::NativeCleanupPending);
            }
            self.pending_rearm.lock().unwrap().take();
        }
        let old = self.device(id)?;
        if old.profile().backend == "software-fake" {
            return Ok(());
        }
        let status = self.leases.status(id)?;
        if !status.fenced && status.session.phase == Phase::Ready {
            return Ok(());
        }
        // A failed/unverified observation fallback must never reopen native HID
        // implicitly. Only explicit setup can reopen a clean unverified probe session.
        if !setup && !old.profile().transform.usable() {
            return Err(ErrorCode::MappingUnverified);
        }
        if !status.fenced
            || status.active
            || status.queued != 0
            || status.session.phase != Phase::Closed
            || status.session.cleanup_error.is_some()
            || !matches!(
                status.termination,
                Some(
                    crate::actor::TerminationReason::Released
                        | crate::actor::TerminationReason::Revoked
                        | crate::actor::TerminationReason::IdleExpired
                )
            )
        {
            return Err(ErrorCode::NativeCleanupPending);
        }
        let (route, identities) = {
            let mut owner = self.phone.try_lock().map_err(|_| ErrorCode::QueueFull)?;
            let phone = owner.as_mut().ok_or(ErrorCode::BackendUnavailable)?;
            let routes = phone
                .refresh(
                    self.budget(deadline)?.min(Duration::from_millis(900)),
                    &Cancel::default(),
                )
                .await?;
            if routes.len() != 1 || routes[0].device_id != *id {
                return Err(ErrorCode::DeviceIdentityAmbiguous);
            }
            (routes[0].clone(), phone.identities())
        };
        let resolved = identities.lock().unwrap().resolve(&route)?;
        let adb = Self::software_adb().ok_or(ErrorCode::BackendUnavailable)?;
        let build = adb
            .build_identity(&resolved.target(), CancellationToken::new())
            .await?;
        let display = adb
            .viewport(&resolved.target(), CancellationToken::new())
            .await?;
        let mut profile = old.profile().clone();
        if let Some(root) = self.media_root.lock().unwrap().clone()
            && let Ok(Some(saved)) =
                crate::pairing_store::PairingStore::open(&root.join("pairings"))
                    .and_then(|store| store.load_calibration(id, &profile))
        {
            profile = saved;
        }
        profile.validate(
            &build,
            &old.profile().descriptor,
            &old.profile().backend,
            display.logical,
            display.rotation,
        )?;
        let reference = route.clone();
        let guard_identities = identities.clone();
        let mapping = profile.transform.clone();
        let guard = move |r: &RouteRef| {
            if r != &reference {
                return Err(ErrorCode::LeaseInvalid);
            }
            Ok(crate::native::session::Binding {
                route: guard_identities.lock().unwrap().resolve(r)?,
                mapping: mapping.clone(),
            })
        };
        let budget = self.budget(deadline)?.min(Duration::from_secs(2));
        let session = if profile.transform.usable() {
            crate::native::session::start(route.clone(), budget, guard)
        } else {
            crate::native::session::start_descriptor_probe(route.clone(), budget, guard)
        }?;
        *self.pending_rearm.lock().unwrap() = Some(session.clone());
        let mut attempt = RearmAttempt {
            session: session.clone(),
            armed: true,
        };
        session
            .ready(
                self.budget(deadline)?.min(Duration::from_secs(2)),
                &Cancel::default(),
            )
            .await?;
        self.budget(deadline)?;
        self.reconnect_trusted(
            identities,
            route.clone(),
            session,
            profile,
            old.capture.clone(),
            crate::discovery::DiscoveryKind::Native,
        )?;
        attempt.armed = false;
        self.pending_rearm.lock().unwrap().take();
        self.record_discovery(Ok(route));
        Ok(())
    }
    fn device(&self, id: &DeviceId) -> Result<Arc<Device>, ErrorCode> {
        self.devices
            .lock()
            .unwrap()
            .get(id)
            .cloned()
            .ok_or(ErrorCode::BackendUnavailable)
    }
    fn budget(&self, deadline: Instant) -> Result<Duration, ErrorCode> {
        deadline
            .checked_duration_since(Instant::now())
            .filter(|v| !v.is_zero())
            .ok_or(ErrorCode::ProcessTimeout)
    }
    fn owner(&self, auth: &LeaseAuth) -> Result<Arc<Device>, ErrorCode> {
        let device = self.device(&auth.device_id)?;
        self.leases.receipt_owner_worker(auth, &device.token)?;
        Ok(device)
    }
    fn device_cleanup(&self, id: &DeviceId) -> Cleanup {
        self.leases
            .status(id)
            .map(|s| cleanup(s.session, s.active))
            .unwrap_or(Cleanup::Pending)
    }
    /// Direct embedding starts a new operation; IPC must use handle_until with
    /// the original exchange deadline, including time spent reading the request.
    pub async fn handle(self: &Arc<Self>, request: Request) -> Response {
        let cap = if request.call.is_app_launch() {
            APP_LAUNCH_TIMEOUT_MS
        } else {
            self.limits.operation_ms
        };
        let deadline = Instant::now() + Duration::from_millis(cap);
        self.handle_until(request, deadline).await
    }
    pub async fn handle_until(
        self: &Arc<Self>,
        request: Request,
        exchange_deadline: Instant,
    ) -> Response {
        let result = if request.version != 1 {
            Err(ErrorCode::ProtocolVersionUnsupported)
        } else if request.request_id.is_empty()
            || request.request_id.len() > 128
            || request.deadline_ms == 0
            || request.deadline_ms
                > if request.call.is_app_launch() {
                    APP_LAUNCH_TIMEOUT_MS
                } else if matches!(request.call, Call::Calibrate { .. }) {
                    60_000
                } else if matches!(
                    request.call,
                    Call::Screenshot { .. } | Call::Elements { .. }
                ) {
                    10_000
                } else {
                    self.limits.operation_ms
                }
        {
            Err(ErrorCode::InvalidRequest)
        } else {
            let deadline =
                exchange_deadline.min(Instant::now() + Duration::from_millis(request.deadline_ms));
            // Bounds every operation's wait, including cleanup/control waits.
            // Admitted input completion and native cleanup keep their owners;
            // dropping this caller cannot create or relabel a receipt.
            let result = tokio::time::timeout_at(
                tokio::time::Instant::from_std(deadline),
                self.dispatch(
                    request.call.clone(),
                    deadline,
                    request.observe_after,
                    request.coordinate_space,
                ),
            )
            .await
            .unwrap_or(Err(ErrorCode::ProcessTimeout));
            if Instant::now() >= deadline {
                Err(ErrorCode::ProcessTimeout)
            } else {
                result
            }
        };
        let safe_error = result.as_ref().err().copied();
        {
            let mut events = self.events.lock().unwrap();
            if events.len() == 64 {
                events.pop_front();
            }
            events.push_back(SafeEvent {
                method: request.call.method(),
                error: safe_error,
            });
        }
        let outcome = match result {
            Ok(result) => ResponseOutcome::Success {
                result: Box::new(result),
            },
            Err(code) => ResponseOutcome::Error {
                error: ApiError {
                    code,
                    phase: format!("service/{}", request.call.method()),
                    recovery: if matches!(request.call, Call::Calibrate { .. }) {
                        match code {
                            ErrorCode::MappingUnverified => "Open http://127.0.0.1:18764/ on the unlocked phone. Refresh once, leave target 1 untouched, then run setup again. If a target was missed, stop and inspect; do not guess calibration numbers.",
                            ErrorCode::DeviceDisconnected | ErrorCode::BackendUnavailable => "Connect one unlocked phone, enable USB debugging and authorize this host. Set CORDGEIST_ADB to your installed adb path, then run setup again. Run doctor --adb /absolute/path/to/adb for connection diagnostics.",
                            ErrorCode::DeviceIdentityAmbiguous => "Connect only the phone you intend to control, using USB debugging (not a network ADB attachment), then run setup again.",
                            ErrorCode::Leased => "Another client owns this phone. Ask that client to release it before setup.",
                            ErrorCode::MetadataUnsupported | ErrorCode::GeometryChanged => "Use the primary display in portrait. This phone's current display/input evidence could not be validated; keep normal input disabled and inspect doctor diagnostics. No guessed mapping is allowed.",
                            ErrorCode::NativeCleanupPending => "Cleanup is not confirmed. Stop control, reconnect the phone deliberately and restart the daemon before setup.",
                            _ => "Setup stopped safely. Run doctor with your installed adb path; inspect the reported error before retrying. Never replay uncertain input.",
                        }.into()
                    } else if code == ErrorCode::InvalidRequest && request.call.is_app_launch() {
                        "kind=invalid-request stage=app-name-resolution deadline=not hit; unknown, ambiguous or invalid app name. Use a unique installed common name (Chrome, Settings, Camera) or exact packageName. No launch admitted.".into()
                    } else {
                        format!(
                            "kind={code} stage=service/{} deadline={}; inspect actions_status for the original mutation; never replay with a fresh sequence.",
                            request.call.method(),
                            if code == ErrorCode::ProcessTimeout {
                                "request deadline exhausted"
                            } else {
                                "not hit"
                            }
                        )
                    },
                },
            },
        };
        Response {
            version: ProtocolVersion,
            request_id: request.request_id,
            boot_id: self.boot.clone(),
            outcome,
        }
    }
    fn software_adb() -> Option<crate::adb::Adb> {
        let path = crate::adb::configured_path()?;
        let port = std::env::var("CORDGEIST_ADB_PORT")
            .ok()
            .and_then(|value| value.parse().ok())
            .unwrap_or(5037);
        crate::adb::Adb::new(path, port).ok()
    }
    async fn dispatch(
        self: &Arc<Self>,
        call: Call,
        deadline: Instant,
        observe_after: bool,
        coordinate_space: CoordinateSpace,
    ) -> Result<OperationResult, ErrorCode> {
        use OperationResult as R;
        self.budget(deadline)?;
        if self.closing.load(Ordering::SeqCst)
            && !matches!(
                &call,
                Call::Status
                    | Call::Doctor { .. }
                    | Call::List
                    | Call::Release { .. }
                    | Call::Revoke { .. }
                    | Call::ActionStatus { .. }
                    | Call::Cancel { .. }
                    | Call::Tap { .. }
                    | Call::Swipe { .. }
                    | Call::Hold { .. }
                    | Call::Type { .. }
                    | Call::Key { .. }
            )
        {
            return Err(ErrorCode::BackendUnavailable);
        }
        match call {
            Call::Elements { auth } => {
                self.confirm_phone().await?;
                let d = self.owner(&auth)?;
                self.leases.refresh(&auth)?;
                let _permit = d
                    .capture_capacity
                    .clone()
                    .try_acquire_owned()
                    .map_err(|_| ErrorCode::QueueFull)?;
                let captured = self.clock.now_ms();
                let route = d.identities.lock().unwrap().resolve(&d.route)?;
                let cancel = CancellationToken::new();
                let _cancel = CancelCapture(cancel.clone());
                let observed = tokio::time::timeout_at(
                    tokio::time::Instant::from_std(deadline),
                    d.capture.elements(route, cancel),
                )
                .await
                .map_err(|_| ErrorCode::ProcessTimeout)??;
                self.confirm_phone().await?;
                self.leases.refresh(&auth)?;
                self.observe_geometry(
                    &d,
                    &auth,
                    (&observed.build, &observed.descriptor, &observed.backend),
                    observed.display,
                    observed.rotation,
                )?;
                let frame = Frame {
                    id: FrameId(random()?),
                    provenance: Provenance {
                        boot_id: auth.boot_id.clone(),
                        device_id: auth.device_id.clone(),
                        lease_id: auth.lease_id.clone(),
                        attachment: auth.attachment,
                        fence: auth.fence,
                        geometry: d.profile().geometry,
                        mapping: d.profile().mapping,
                    },
                    captured_ms: captured,
                    image: observed.display,
                    display: observed.display,
                    rotation: observed.rotation,
                    display_id: 0,
                    coordinate_space: CoordinateSpace::ScreenPixels,
                };
                let prepared = crate::frames::PreparedFrame::geometry(
                    frame.clone(),
                    d.profile().transform.clone(),
                    self.clock.now_ms(),
                )?;
                d.state
                    .lock()
                    .unwrap()
                    .frames
                    .insert_prepared(prepared, self.clock.now_ms())?;
                self.leases.publish_observation(
                    &d.token,
                    &auth,
                    frame.clone(),
                    d.profile().transform.clone(),
                )?;
                Ok(R::Elements(ElementsResult {
                    frame,
                    elements: observed.elements,
                    warnings: if d.profile().backend == "software-fake" {
                        vec!["SOFTWARE FAKE UI DATA; not hardware evidence".into()]
                    } else {
                        crate::geometry::rotation_warnings(d.profile().rotation)
                    },
                }))
            }
            Call::Apps { auth } => {
                self.confirm_phone().await?;
                let d = self.owner(&auth)?;
                self.leases.refresh(&auth)?;
                let route = d.identities.lock().unwrap().resolve(&d.route)?;
                let cancel = CancellationToken::new();
                let _cancel = CancelCapture(cancel.clone());
                let apps = tokio::time::timeout_at(
                    tokio::time::Instant::from_std(deadline),
                    d.capture.apps(route, cancel),
                )
                .await
                .map_err(|_| ErrorCode::ProcessTimeout)??;
                self.confirm_phone().await?;
                self.leases.refresh(&auth)?;
                Ok(R::Apps(AppsResult {
                    apps: apps
                        .into_iter()
                        .map(|package| InstalledApp {
                            app_name: package.clone(),
                            package_name: package,
                        })
                        .collect(),
                }))
            }
            Call::Calibrate { device, probe } => {
                let device = match device {
                    Some(device) => device,
                    None => {
                        let devices = self.devices.lock().unwrap();
                        if devices.is_empty() {
                            return Err(ErrorCode::DeviceDisconnected);
                        }
                        if devices.len() != 1 {
                            return Err(ErrorCode::DeviceIdentityAmbiguous);
                        }
                        devices.keys().next().unwrap().clone()
                    }
                };
                #[cfg(all(target_os = "linux", feature = "native-aoa"))]
                self.rearm_native(&device, deadline, true).await?;
                self.confirm_phone().await?;
                let d = self.device(&device)?;
                let route = d.identities.lock().unwrap().resolve(&d.route)?;
                let grant = self.leases.acquire(&device, 30_000)?;
                let mut ownership = SetupOwnership {
                    service: self.clone(),
                    auth: Some(grant.auth.clone()),
                };
                let cancel = CancellationToken::new();
                let _cancel = CancelCapture(cancel.clone());
                let evidence = tokio::time::timeout_at(
                    tokio::time::Instant::from_std(deadline),
                    d.capture.calibration_axes(
                        route,
                        d.profile().display,
                        (d.profile().transform.max_x, d.profile().transform.max_y),
                        cancel,
                    ),
                )
                .await
                .map_err(|_| ErrorCode::ProcessTimeout)
                .and_then(|r| r);
                let already_verified = d.profile().transform.verified;
                let parser_failed = evidence.is_err();
                let evidence = match evidence {
                    Ok(evidence) if probe => self
                        .probe_targets(&grant.auth, deadline)
                        .await
                        .map(|()| evidence),
                    other => other,
                };
                let status = self
                    .leases
                    .release(&grant.auth, Duration::from_secs(2))
                    .await?;
                ownership.auth.take();
                if cleanup(status, false) != Cleanup::Neutral {
                    return Err(ErrorCode::NativeCleanupPending);
                }
                let evidence = match evidence {
                    Ok(evidence) => evidence,
                    Err(ErrorCode::MetadataUnsupported | ErrorCode::MappingUnverified)
                        if parser_failed && d.profile().transform.generic_screen.is_some() =>
                    {
                        #[cfg(all(target_os = "linux", feature = "native-aoa"))]
                        self.rearm_native(&device, deadline, true).await?;
                        return Ok(R::Calibrate(CalibrationResult { display: d.profile().display,
                            maximum_x: d.profile().transform.max_x, maximum_y: d.profile().transform.max_y,
                            verified: already_verified,
                            recovery: "Warning: optional input-mapper parser unavailable; generic geometry remains enabled, not calibration-verified.".into() }));
                    }
                    Err(code) => return Err(code),
                };
                if probe && !already_verified {
                    let root = self
                        .media_root
                        .lock()
                        .unwrap()
                        .clone()
                        .ok_or(ErrorCode::BackendUnavailable)?;
                    let mut profile = d.profile().clone();
                    profile.transform.verified = true;
                    self.budget(deadline)?;
                    crate::pairing_store::PairingStore::open(&root.join("pairings"))
                        .and_then(|store| store.save_calibration(&device, &profile))
                        .map_err(|_| ErrorCode::MappingUnverified)?;
                }
                #[cfg(all(target_os = "linux", feature = "native-aoa"))]
                if probe || already_verified {
                    self.rearm_native(&device, deadline, true).await?;
                }
                Ok(R::Calibrate(CalibrationResult {
                    display: evidence.display,
                    maximum_x: evidence.maximum_x,
                    maximum_y: evidence.maximum_y,
                    verified: probe || already_verified,
                    recovery: if probe || already_verified {
                        "Profile validated. Ready for control; acquire the phone when needed."
                            .into()
                    } else {
                        "Open http://127.0.0.1:18764/ on the unlocked phone, leave the first magenta target untouched, then run setup again. Normal input remains disabled.".into()
                    },
                }))
            }
            Call::Status => {
                let _ = self.confirm_phone().await;
                let devices = self.devices.lock().unwrap();
                let ready = devices.values().any(|d| self.native_ready(d));
                let caps = devices
                    .values()
                    .find(|d| self.native_ready(d))
                    .map(|d| self.current_capabilities(d))
                    .unwrap_or_else(capabilities);
                drop(devices);
                Ok(R::Status(StatusResult {
                    software_version: env!("CARGO_PKG_VERSION").into(),
                    backend: self.backend_kind(),
                    ready,
                    capabilities: caps,
                }))
            }
            Call::Doctor { device } => {
                let _ = self.confirm_phone().await;
                let note = self.discovery.lock().unwrap().clone();
                let selected = device.or(note.device_id.clone());
                let ready = selected
                    .as_ref()
                    .and_then(|id| self.device(id).ok())
                    .filter(|d| self.native_ready(d));
                Ok(R::Doctor(DoctorResult {
                    device_id: selected,
                    checks: vec![DiagnosticResult {
                        code: if ready.is_some() {
                            "phone-ready".into()
                        } else {
                            note.code
                        },
                        evidence: if ready.is_some() || note.device_id.is_some() {
                            Evidence::Detected
                        } else {
                            Evidence::Unavailable
                        },
                        recovery: if ready.is_some() {
                            "Exact native phone and usable mapping; session ready or cleanly released and rearmable. Availability is not delivery or calibration verification.".into()
                        } else {
                            "Native readiness unavailable. Check connection, current geometry and cleanup diagnostics; generic setup does not require calibration.".into()
                        },
                    }],
                    capabilities: ready
                        .as_ref()
                        .map(|d| self.current_capabilities(d))
                        .unwrap_or_else(capabilities),
                }))
            }
            Call::List => {
                let _ = self.confirm_phone().await;
                let mut devices: Vec<_> =
                    self.devices
                        .lock()
                        .unwrap()
                        .iter()
                        .map(|(id, d)| {
                            let s = self.leases.status(id).unwrap();
                            DeviceSummary {
                                device_id: id.clone(),
                                attachment: d.route.attachment,
                                health: if self.native_ready(d) {
                                    DeviceHealth::Ready
                                } else if s.fenced
                                    && (s.session.cleanup_error.is_some()
                                        || s.session.phase == Phase::Quarantined
                                        || matches!(
                                    s.termination,
                                    Some(
                                        crate::actor::TerminationReason::BackendFailure
                                            | crate::actor::TerminationReason::HealthInvalid
                                            | crate::actor::TerminationReason::ActionDeadline
                                            | crate::actor::TerminationReason::Disconnected
                                    )
                                )) {
                                    DeviceHealth::Quarantined
                                } else {
                                    DeviceHealth::IdentityMatched
                                },
                                reason_codes: if s.fenced && !self.native_ready(d) {
                                    vec![if s.session.cleanup_error.is_some()
                                        || s.active
                                        || s.queued != 0
                                        || s.session.phase != Phase::Closed
                                    {
                                        ErrorCode::NativeCleanupPending
                                    } else {
                                        ErrorCode::BackendUnavailable
                                    }]
                                } else {
                                    vec![]
                                },
                                capabilities: self.current_capabilities(d),
                                owner_label: None,
                            }
                        })
                        .collect();
                let note = self.discovery.lock().unwrap().clone();
                if let Some(device_id) = note.device_id
                    && !devices.iter().any(|d| d.device_id == device_id)
                {
                    devices.push(DeviceSummary {
                        device_id,
                        attachment: note.attachment.unwrap_or(AttachmentGeneration(0)),
                        health: DeviceHealth::Discovered,
                        reason_codes: vec![ErrorCode::MappingUnverified],
                        capabilities: capabilities(),
                        owner_label: None,
                    });
                }
                Ok(R::List(DevicesResult { devices }))
            }
            Call::Acquire {
                device,
                reason,
                label,
                idle_ttl_s,
            } => {
                if reason.is_empty()
                    || reason.len() > 256
                    || label.as_ref().is_some_and(|v| v.len() > 128)
                {
                    return Err(ErrorCode::InvalidRequest);
                }
                if self
                    .discovery
                    .lock()
                    .unwrap()
                    .device_id
                    .as_ref()
                    .is_some_and(|id| id == &device)
                    && !self.devices.lock().unwrap().contains_key(&device)
                {
                    return Err(ErrorCode::MappingUnverified);
                }
                #[cfg(feature = "fake")]
                self.rearm_software(&device, deadline).await?;
                #[cfg(all(target_os = "linux", feature = "native-aoa"))]
                self.rearm_native(&device, deadline, false).await?;
                self.confirm_phone().await?;
                let d = self.device(&device)?;
                d.identities.lock().unwrap().resolve(&d.route)?;
                self.budget(deadline)?;
                let grant = self.leases.acquire(&device, u64::from(idle_ttl_s) * 1000)?;
                let mut state = d.state.lock().unwrap();
                state.frames.clear();
                state.records.clear();
                state.high_water = 0;
                Ok(R::Acquire(grant))
            }
            Call::Release { auth } => {
                let device = self.owner(&auth)?;
                let status = match self.leases.release(&auth, self.budget(deadline)?).await {
                    Ok(s) => s,
                    Err(ErrorCode::NativeCleanupPending) => {
                        self.leases.status(&auth.device_id)?.session
                    }
                    Err(e) => return Err(e),
                };
                device.state.lock().unwrap().frames.clear();
                Ok(R::Release(LeaseEndResult {
                    device_id: auth.device_id,
                    lease_id: Some(auth.lease_id),
                    fence: auth.fence,
                    cleanup: cleanup(status, false),
                }))
            }
            Call::Revoke { device } => {
                let selected = self.device(&device)?;
                let fence = self.leases.authority_fence(&device)?;
                let status = match self
                    .leases
                    .revoke_worker(&device, &selected.token, self.budget(deadline)?)
                    .await
                {
                    Ok(s) => s,
                    Err(ErrorCode::NativeCleanupPending) => self.leases.status(&device)?.session,
                    Err(e) => return Err(e),
                };
                selected.state.lock().unwrap().frames.clear();
                Ok(R::Revoke(LeaseEndResult {
                    fence,
                    device_id: device,
                    lease_id: None,
                    cleanup: cleanup(status, false),
                }))
            }
            Call::Screenshot {
                auth,
                longest_edge,
                settle_ms,
            } => {
                self.confirm_phone().await?;
                Ok(R::Screenshot(
                    self.capture(&auth, longest_edge, settle_ms, deadline)
                        .await?,
                ))
            }
            Call::ActionStatus { auth, seq } => {
                let d = self.owner(&auth)?;
                let state = d.state.lock().unwrap();
                let r = state.records.iter().find(|r| r.seq == seq).ok_or(
                    if seq.0 <= state.high_water {
                        ErrorCode::ReceiptExpired
                    } else {
                        ErrorCode::InvalidRequest
                    },
                )?;
                let result = if let Some(action) = r.result.borrow().clone() {
                    ActionStatusResult::Final {
                        action: Box::new(action),
                    }
                } else if r.execution.load(Ordering::SeqCst) == 1 {
                    ActionStatusResult::Executing {
                        action_id: r.id.clone(),
                        seq,
                    }
                } else {
                    ActionStatusResult::Queued {
                        action_id: r.id.clone(),
                        seq,
                    }
                };
                Ok(R::ActionStatus(result))
            }
            Call::Cancel { auth, seq } => {
                let d = self.owner(&auth)?;
                let state = d.state.lock().unwrap();
                let r = state
                    .records
                    .iter()
                    .find(|r| r.seq == seq)
                    .ok_or(ErrorCode::ReceiptExpired)?;
                let done = r.result.borrow().is_some();
                if !done {
                    self.leases.refresh(&auth)?;
                    r.requested.store(true, Ordering::SeqCst);
                    r.cancel.cancel();
                }
                Ok(R::Cancel(CancelResult {
                    action_id: r.id.clone(),
                    seq,
                    state: if done {
                        CancellationState::AlreadyFinal
                    } else {
                        CancellationState::Requested
                    },
                    cleanup: self.device_cleanup(&auth.device_id),
                }))
            }
            Call::Text { .. } => Err(ErrorCode::OcrUnavailable),
            Call::MediaPush {
                auth,
                seq,
                source,
                kind,
            } => {
                let device = self.owner(&auth)?;
                if device.profile().backend != "software-fake"
                    && device.profile().backend != "adb-observe"
                {
                    return Err(ErrorCode::BackendUnavailable);
                }
                let root = self
                    .media_root
                    .lock()
                    .unwrap()
                    .clone()
                    .ok_or(ErrorCode::BackendUnavailable)?;
                let prepared = crate::media::prepare_import(&source, kind, seq, &root)?
                    .bind_lease(&auth.lease_id, seq)?;
                loop {
                    let mut state = device.state.lock().unwrap();
                    if let Some(previous) = state.media.get(&seq.0) {
                        if previous.digest != prepared.digest || previous.kind != kind {
                            return Err(ErrorCode::SequenceMismatch);
                        }
                        return Ok(R::MediaPush(previous.result.clone()));
                    }
                    if let Some(reserved) = state.media_reserved.get(&seq.0) {
                        if reserved.digest != prepared.digest || reserved.kind != kind {
                            return Err(ErrorCode::SequenceMismatch);
                        }
                        let _waiting = device.media_gate.wait(state).unwrap();
                        continue;
                    }
                    if seq.0 <= state.high_water {
                        return Err(ErrorCode::ReceiptExpired);
                    }
                    state.media_reserved.insert(
                        seq.0,
                        Reservation {
                            digest: prepared.digest,
                            kind,
                        },
                    );
                    break;
                }
                let _reservation = ClearReservation {
                    state: &device.state,
                    gate: &device.media_gate,
                    seq: seq.0,
                };
                let imported = if device.profile().backend == "adb-observe" {
                    let Some(adb) = Self::software_adb() else {
                        return Err(ErrorCode::BackendUnavailable);
                    };
                    let target = device
                        .identities
                        .lock()
                        .unwrap()
                        .resolve(&device.route)?
                        .target();
                    crate::media::import_supervised(&adb, &target, &prepared, seq).await?
                } else if let Some(adb) = Self::software_adb() {
                    let target = device
                        .identities
                        .lock()
                        .unwrap()
                        .resolve(&device.route)?
                        .target();
                    crate::media::import_supervised(&adb, &target, &prepared, seq).await?
                } else {
                    crate::media::import_prepared_software(prepared, seq)?
                };
                let mut state = device.state.lock().unwrap();
                if let Some(previous) = state.media.get(&seq.0) {
                    if previous.digest != imported.digest || previous.kind != kind {
                        return Err(ErrorCode::SequenceMismatch);
                    }
                    return Ok(R::MediaPush(previous.result.clone()));
                }
                state.high_water = seq.0;
                state.media.insert(
                    seq.0,
                    StoredMedia {
                        result: imported.result.clone(),
                        digest: imported.digest,
                        kind,
                    },
                );
                Ok(R::MediaPush(imported.result))
            }
            action => {
                self.action(action, deadline, observe_after, coordinate_space)
                    .await
            }
        }
    }
    async fn probe_targets(&self, auth: &LeaseAuth, deadline: Instant) -> Result<(), ErrorCode> {
        let d = self.owner(auth)?;
        for index in 0..3 {
            let shot = self.capture(auth, 1280, 0, deadline).await?;
            let png = {
                let state = d.state.lock().unwrap();
                state
                    .frames
                    .get(auth, &shot.frame.id, self.clock.now_ms())?
                    .2
                    .to_vec()
            };
            let point = crate::calibration::synthetic_marker(&png, false)?;
            let x = point.x / f64::from(shot.frame.image.width);
            let y = point.y / f64::from(shot.frame.image.height);
            let expected = match index {
                0 => (0.35..0.65).contains(&x) && (0.35..0.65).contains(&y),
                1 => x < 0.25 && y < 0.35,
                _ => x > 0.75 && y > 0.65,
            };
            if !expected {
                return Err(ErrorCode::MappingUnverified);
            }
            self.confirm_phone().await?;
            let mapping = d.profile().transform;
            let at = if mapping.generic_screen.is_some() {
                mapping.map(shot.frame.image, point)?
            } else {
                mapping.place(shot.frame.image, point)?
            };
            let input_deadline =
                deadline.min(Instant::now() + Duration::from_millis(self.limits.operation_ms));
            let result = self
                .leases
                .enqueue_frame_until(
                    auth.clone(),
                    shot.frame.id,
                    Transition::Gesture(crate::gesture::Gesture::Tap {
                        at,
                        duration: Duration::from_millis(20),
                    }),
                    input_deadline,
                )?
                .wait()
                .await;
            if result.delivery != Delivery::Sent || result.error.is_some() {
                return Err(ErrorCode::InputOutcomeUnknown);
            }
        }
        let shot = self.capture(auth, 1280, 0, deadline).await?;
        let state = d.state.lock().unwrap();
        let png = state
            .frames
            .get(auth, &shot.frame.id, self.clock.now_ms())?
            .2;
        crate::calibration::synthetic_marker(png, true)?;
        Ok(())
    }
    fn observe_geometry(
        &self,
        d: &Device,
        auth: &LeaseAuth,
        identity: (&str, &str, &str),
        display: Size,
        rotation: Rotation,
    ) -> Result<(), ErrorCode> {
        let (build, descriptor, backend) = identity;
        let mut profile = d.profile();
        if profile.transform.generic_screen.is_some()
            && (profile.display != display || profile.rotation != rotation)
        {
            // Binding identity is unchanged; retire all old coordinate tokens before
            // publishing a new geometry. Receipt/high-water state is retained.
            profile.validate(
                build,
                descriptor,
                backend,
                profile.display,
                profile.rotation,
            )?;
            if display.width < 2 || display.height < 2 {
                return Err(ErrorCode::GeometryChanged);
            }
            self.leases.retire_observation(&d.token, auth)?;
            d.state.lock().unwrap().frames.clear();
            profile.display = display;
            profile.rotation = rotation;
            profile.transform.rotation = rotation;
            profile.transform.generic_screen = Some(display);
            profile.transform.verified = false;
            profile.geometry.0 = profile
                .geometry
                .0
                .checked_add(1)
                .ok_or(ErrorCode::GeometryChanged)?;
            profile.mapping.0 = profile
                .mapping
                .0
                .checked_add(1)
                .ok_or(ErrorCode::GeometryChanged)?;
            *d.profile.lock().unwrap() = profile.clone();
        }
        profile.validate(build, descriptor, backend, display, rotation)
    }
    async fn capture(
        &self,
        auth: &LeaseAuth,
        longest: u32,
        settle_ms: u32,
        deadline: Instant,
    ) -> Result<ScreenshotResult, ErrorCode> {
        self.confirm_phone().await?;
        let selected = self.owner(auth)?;
        let result = self.capture_inner(auth, longest, settle_ms, deadline).await;
        if matches!(
            result,
            Err(ErrorCode::GeometryChanged
                | ErrorCode::MappingUnverified
                | ErrorCode::DeviceIdentityAmbiguous)
        ) {
            let _ = self.leases.worker_failed(&auth.device_id, &selected.token);
        }
        result
    }
    async fn capture_inner(
        &self,
        auth: &LeaseAuth,
        longest: u32,
        settle_ms: u32,
        deadline: Instant,
    ) -> Result<ScreenshotResult, ErrorCode> {
        if !(2..=8192).contains(&longest) || settle_ms > 500 {
            return Err(ErrorCode::InvalidRequest);
        }
        let d = self.owner(auth)?;
        self.leases.refresh(auth)?;
        let mut permit = Some(
            d.capture_capacity
                .clone()
                .try_acquire_owned()
                .map_err(|_| ErrorCode::QueueFull)?,
        );
        let capture_deadline = deadline;
        let mut previous = None;
        let mut settled = if settle_ms == 0 { None } else { Some(false) };
        loop {
            // Conservative capture-start age survives processing/executor delay.
            let captured_ms = self.clock.now_ms();
            let route = d.identities.lock().unwrap().resolve(&d.route)?;
            let cancellation = CancelCapture(CancellationToken::new());
            self.budget(capture_deadline)?;
            let sample = tokio::time::timeout_at(
                tokio::time::Instant::from_std(capture_deadline),
                d.capture.capture(route, cancellation.0.clone()),
            )
            .await
            .map_err(|_| ErrorCode::ProcessTimeout)??;
            self.budget(deadline)?;
            self.confirm_phone().await?;
            self.leases.refresh(auth)?;
            d.identities.lock().unwrap().resolve(&d.route)?;
            self.observe_geometry(
                &d,
                auth,
                (&sample.build, &sample.descriptor, &sample.backend),
                sample.display,
                sample.rotation,
            )?;
            // Raw native capture has a separate bounded processing budget;
            // returned/retained resized PNGs still use the configured frame budget.
            if sample.png.len() > 32 * 1024 * 1024 {
                return Err(ErrorCode::OutputLimit);
            }
            let profile = d.profile().clone();
            let job_auth = auth.clone();
            let now_ms = self.clock.now_ms();
            let limit = self.limits.frame_bytes;
            let held = permit.take().expect("one owned capture permit");
            let job = tokio::task::spawn_blocking(move || {
                let result = (|| {
                    let source = image::ImageReader::with_format(
                        std::io::Cursor::new(&sample.png),
                        image::ImageFormat::Png,
                    )
                    .into_dimensions()
                    .map_err(|_| ErrorCode::CaptureUnavailable)?;
                    if source != (sample.image.width, sample.image.height) {
                        return Err(ErrorCode::GeometryChanged);
                    }
                    let (png, image) = crate::adb::resize_png(&sample.png, longest)?;
                    if png.len() > limit.min(4 * 1024 * 1024) {
                        return Err(ErrorCode::OutputLimit);
                    }
                    let frame = Frame {
                        id: FrameId(random()?),
                        provenance: Provenance {
                            boot_id: job_auth.boot_id.clone(),
                            device_id: job_auth.device_id.clone(),
                            lease_id: job_auth.lease_id.clone(),
                            attachment: job_auth.attachment,
                            fence: job_auth.fence,
                            geometry: profile.geometry,
                            mapping: profile.mapping,
                        },
                        captured_ms,
                        image,
                        display: sample.display,
                        rotation: sample.rotation,
                        display_id: 0,
                        coordinate_space: CoordinateSpace::ReturnedImagePixels,
                    };
                    crate::frames::PreparedFrame::prepare(
                        frame,
                        profile.transform,
                        png,
                        now_ms,
                        limit,
                    )
                })();
                (result, held)
            });
            self.budget(deadline)?;
            let (prepared, held) =
                tokio::time::timeout_at(tokio::time::Instant::from_std(deadline), job)
                    .await
                    .map_err(|_| ErrorCode::ProcessTimeout)?
                    .map_err(|_| ErrorCode::CaptureUnavailable)?;
            permit = Some(held);
            let prepared = prepared?;
            self.budget(deadline)?;
            self.leases.refresh(auth)?;
            let frame = prepared.frame().clone();
            let png_owned = prepared.png().to_vec();
            let png = png_owned.as_slice();
            let hash = blake3::keyed_hash(&self.key, png);
            let now = Instant::now();
            if settle_ms > 0 {
                if let Some((old, start)) = previous {
                    if hash == old
                        && now.duration_since(start) >= Duration::from_millis(u64::from(settle_ms))
                    {
                        settled = Some(true);
                    } else if hash != old {
                        previous = Some((hash, now));
                    }
                } else {
                    previous = Some((hash, now));
                }
            }
            if settle_ms == 0
                || settled == Some(true)
                || self.budget(deadline)? < Duration::from_millis(25)
            {
                let result = ScreenshotResult {
                    warnings: crate::geometry::rotation_warnings(d.profile().rotation),
                    frame: frame.clone(),
                    image: PngImage {
                        mime_type: PngMime::Png,
                        base64: base64(png),
                    },
                    settled,
                };
                let mut state = d.state.lock().unwrap();
                self.budget(deadline)?;
                state
                    .frames
                    .insert_prepared(prepared, self.clock.now_ms())?;
                self.budget(deadline)?;
                let published = d.profile().transform.clone();
                if published.max_x != 0 {
                    self.leases
                        .publish_observation(&d.token, auth, frame, published)?;
                }
                return Ok(result);
            }
            tokio::time::sleep(Duration::from_millis(20).min(self.budget(deadline)?)).await;
        }
    }
    async fn action(
        self: &Arc<Self>,
        call: Call,
        deadline: Instant,
        observe_after: bool,
        coordinate_space: CoordinateSpace,
    ) -> Result<OperationResult, ErrorCode> {
        let (auth, seq, frame_id) = match &call {
            Call::Android { auth, seq, .. } => (auth.clone(), *seq, None),
            Call::Tap {
                auth,
                seq,
                frame_id,
                ..
            }
            | Call::Swipe {
                auth,
                seq,
                frame_id,
                ..
            }
            | Call::Hold {
                auth,
                seq,
                frame_id,
                ..
            }
            | Call::Type {
                auth,
                seq,
                frame_id,
                ..
            }
            | Call::Key {
                auth,
                seq,
                frame_id,
                ..
            } => (auth.clone(), *seq, Some(frame_id.clone())),
            _ => return Err(ErrorCode::InvalidRequest),
        };
        self.confirm_phone().await?;
        let d = self.owner(&auth)?;
        let new_identity = {
            let state = d.state.lock().unwrap();
            seq.0 > state.high_water && !state.records.iter().any(|r| r.seq == seq)
        };
        if new_identity && d.profile().backend != "software-fake" {
            // Removing optional screenshots must not remove live geometry fencing.
            // This is read-only metadata, not a hidden capture or input fallback.
            let adb = Self::software_adb().ok_or(ErrorCode::BackendUnavailable)?;
            let target = d.identities.lock().unwrap().resolve(&d.route)?.target();
            let cancel = CancellationToken::new();
            let _cancel = CancelCapture(cancel.clone());
            let viewport = tokio::time::timeout_at(
                tokio::time::Instant::from_std(deadline),
                adb.viewport(&target, cancel),
            )
            .await
            .map_err(|_| ErrorCode::ProcessTimeout)??;
            d.profile().validate(
                &d.profile().build,
                &d.profile().descriptor,
                &d.profile().backend,
                viewport.logical,
                viewport.rotation,
            )?;
            self.budget(deadline)?;
        }
        let mut resolved_android = None;
        if new_identity
            && d.profile().backend != "software-fake"
            && let Call::Android {
                command: AndroidCommand::LaunchApp { package },
                ..
            } = &call
            && (!package.contains('.') || !valid_package(package))
        {
            let adb = Self::software_adb().ok_or(ErrorCode::BackendUnavailable)?;
            let target = d.identities.lock().unwrap().resolve(&d.route)?.target();
            let cancel = CancellationToken::new();
            let _cancel = CancelCapture(cancel.clone());
            let budget = self
                .budget(deadline)?
                .saturating_sub(Duration::from_millis(100));
            if budget.is_zero() {
                return Err(ErrorCode::ProcessTimeout);
            }
            let package = tokio::time::timeout(budget, adb.resolve_app(&target, package, cancel))
                .await
                .map_err(|_| ErrorCode::ProcessTimeout)??;
            self.confirm_phone().await?;
            self.owner(&auth)?;
            self.budget(deadline)?;
            resolved_android = Some(AndroidCommand::LaunchApp { package });
        }
        let mut hasher = blake3::Hasher::new_keyed(&self.key);
        serde_json::to_writer(&mut hasher, &(&call, observe_after, coordinate_space))
            .map_err(|_| ErrorCode::InvalidRequest)?;
        let digest = hasher.finalize();
        let mut rx = {
            let mut state = d.state.lock().unwrap();
            if let Some(r) = state.records.iter().find(|r| r.seq == seq) {
                if r.digest != digest {
                    return Err(ErrorCode::SequenceMismatch);
                }
                r.result.clone()
            } else {
                if self.closing.load(Ordering::SeqCst) {
                    return Err(ErrorCode::BackendUnavailable);
                }
                if seq.0 <= state.high_water {
                    return Err(ErrorCode::ReceiptExpired);
                }
                let transition = if let Call::Android { command, .. } = &call {
                    let command = resolved_android.as_ref().unwrap_or(command);
                    command.validate()?;
                    Transition::Android(command.clone())
                } else {
                    let (frame, mapping, _) = state.frames.get(
                        &auth,
                        frame_id.as_ref().ok_or(ErrorCode::FrameExpired)?,
                        self.clock.now_ms(),
                    )?;
                    let mapping = mapping.clone();
                    if !mapping.usable() {
                        return Err(ErrorCode::MappingUnverified);
                    }
                    if frame.coordinate_space == CoordinateSpace::ScreenPixels
                        && coordinate_space != CoordinateSpace::ScreenPixels
                    {
                        return Err(ErrorCode::InvalidRequest);
                    }
                    let map = |p: Point| {
                        let p = if coordinate_space == CoordinateSpace::ScreenPixels {
                            if !p.x.is_finite()
                                || !p.y.is_finite()
                                || p.x < 0.
                                || p.y < 0.
                                || p.x >= f64::from(frame.display.width)
                                || p.y >= f64::from(frame.display.height)
                            {
                                return Err(ErrorCode::InvalidRequest);
                            }
                            if mapping.generic_screen.is_some() {
                                Point {
                                    x: p.x * f64::from(frame.image.width)
                                        / f64::from(frame.display.width),
                                    y: p.y * f64::from(frame.image.height)
                                        / f64::from(frame.display.height),
                                }
                            } else {
                                Point {
                                    x: p.x.min(f64::from(frame.display.width - 1))
                                        * f64::from(frame.image.width - 1)
                                        / f64::from(frame.display.width - 1),
                                    y: p.y.min(f64::from(frame.display.height - 1))
                                        * f64::from(frame.image.height - 1)
                                        / f64::from(frame.display.height - 1),
                                }
                            }
                        } else {
                            p
                        };
                        mapping.map(frame.image, p)
                    };
                    match &call {
                        Call::Tap { point, .. } => {
                            Transition::Gesture(crate::gesture::Gesture::Tap {
                                at: map(*point)?,
                                duration: Duration::from_millis(20),
                            })
                        }
                        Call::Hold {
                            point, duration_ms, ..
                        } => Transition::Gesture(crate::gesture::Gesture::Hold {
                            at: map(*point)?,
                            duration: Duration::from_millis(u64::from(*duration_ms)),
                        }),
                        Call::Swipe {
                            from,
                            to,
                            duration_ms,
                            ..
                        } => Transition::Gesture(crate::gesture::Gesture::Swipe {
                            from: map(*from)?,
                            to: map(*to)?,
                            duration: Duration::from_millis(u64::from(*duration_ms)),
                        }),
                        Call::Type { text, submit, .. } => {
                            let text = crate::ascii::Text::new("us-ansi", text.clone());
                            if *submit {
                                Transition::TextSubmit(text)
                            } else {
                                Transition::Text(text)
                            }
                        }
                        Call::Key { key, .. } => {
                            if d.profile().backend != "software-fake" {
                                return Err(ErrorCode::UnsupportedKey);
                            }
                            let usage = match key.as_str() {
                                "enter" => 0x28,
                                "tab" => 0x2b,
                                "escape" => 0x29,
                                "backspace" => 0x2a,
                                _ => return Err(ErrorCode::UnsupportedKey),
                            };
                            Transition::Batch(vec![
                                Transition::Key { usage, down: true },
                                Transition::Key { usage, down: false },
                            ])
                        }
                        _ => unreachable!(),
                    }
                };
                // Complete validation occurs in the actor before enqueue and
                // before high-water reservation; no input can precede it.
                let eviction = if state.records.len() >= self.limits.receipts {
                    Some(
                        state
                            .records
                            .iter()
                            .position(|r| r.result.borrow().is_some())
                            .ok_or(ErrorCode::QueueFull)?,
                    )
                } else {
                    None
                };
                let device_permit = d
                    .ordinary
                    .clone()
                    .try_acquire_owned()
                    .map_err(|_| ErrorCode::QueueFull)?;
                let permit = self
                    .ordinary
                    .clone()
                    .try_acquire_owned()
                    .map_err(|_| ErrorCode::QueueFull)?;
                let ticket = if let Some(frame_id) = frame_id {
                    self.leases
                        .enqueue_frame_until(auth.clone(), frame_id, transition, deadline)?
                } else {
                    self.leases
                        .enqueue_owner_until(auth.clone(), transition, deadline)?
                };
                if let Some(index) = eviction {
                    state.records.remove(index);
                }
                let cancel = ticket.cancellation();
                let requested = Arc::new(AtomicBool::new(false));
                let execution = ticket.claimed();
                let id = ActionId(format!("{}:{}:{}", self.boot.0, auth.lease_id.0, seq.0));
                let (tx, rx) = watch::channel(None);
                state.high_water = seq.0;
                state.records.push_back(Record {
                    seq,
                    digest,
                    id: id.clone(),
                    cancel: cancel.clone(),
                    requested: requested.clone(),
                    execution: execution.clone(),
                    result: rx.clone(),
                });
                let service = self.clone();
                let auth = auth.clone();
                let started = self.clock.now_ms();
                let d = d.clone();
                let transport = if matches!(call, Call::Android { .. }) {
                    if d.profile().backend == "software-fake" {
                        "FAKE-software-only/adb"
                    } else {
                        "adb"
                    }
                } else if matches!(call, Call::Type { submit: true, .. }) {
                    if d.profile().backend == "software-fake" {
                        "FAKE-software-only/aoa-hid+adb"
                    } else {
                        "aoa-hid+adb"
                    }
                } else if d.profile().backend == "software-fake" {
                    "FAKE-software-only/aoa-tracked-session"
                } else {
                    "aoa-hid/aoa-tracked-session"
                };
                let mut completion=Completion {sender:tx,service:service.clone(),device:d.clone(),execution,cancel:cancel.clone(),fallback:Some(ActionResult{warnings:crate::geometry::rotation_warnings(d.profile().rotation),receipt:Receipt{action_id:id.clone(),seq,transport:transport.into(),started_ms:started,finished_ms:started,input_status:InputStatus::Unknown,observation_status:ObservationStatus::Failed,cleanup:Cleanup::Pending,cancelled:false,retry_guidance:"Worker completion lost; inspect cleanup and never replay with a fresh sequence.".into()},observation:None})};
                tokio::spawn(async move {
                    let _permit = permit;
                    let _device_permit = device_permit;
                    let result = ticket.wait().await;
                    let input_status = match result.delivery {
                        Delivery::NotSent => InputStatus::NotSent,
                        Delivery::Sent => InputStatus::Sent,
                        Delivery::Partial => InputStatus::Partial,
                        Delivery::Unknown => InputStatus::Unknown,
                    };
                    // Optional observation cannot consume the entire reply budget
                    // after a known input result. Never extend input admission.
                    let observation_deadline = deadline
                        .checked_sub(Duration::from_millis(100))
                        .unwrap_or(deadline);
                    let mut captured = if observe_after {
                        service.capture(&auth, 1280, 0, observation_deadline).await
                    } else {
                        Err(ErrorCode::CaptureUnavailable)
                    };
                    // Image-bearing receipts share the configured retained-image
                    // budget; metadata receipts/high-water marks remain intact.
                    let state = d.state.lock().unwrap();
                    if let Ok(image) = &captured {
                        let retained: usize = state
                            .records
                            .iter()
                            .filter_map(|r| {
                                r.result
                                    .borrow()
                                    .as_ref()
                                    .and_then(|a| a.observation.as_ref())
                                    .map(|o| o.image.base64.len())
                            })
                            .sum();
                        if retained.saturating_add(image.image.base64.len())
                            > service.limits.frame_bytes
                        {
                            captured = Err(ErrorCode::OutputLimit);
                        }
                    }
                    let observation_status = if !observe_after {
                        ObservationStatus::NotRequested
                    } else {
                        match &captured {
                            Ok(_) => ObservationStatus::Captured,
                            Err(ErrorCode::ProcessTimeout) => ObservationStatus::Timeout,
                            Err(_) => ObservationStatus::Failed,
                        }
                    };
                    let action = ActionResult {
                        warnings: {
                            let mut warnings =
                                crate::geometry::rotation_warnings(d.profile().rotation);
                            if let Some(error) = result.error {
                                warnings.push(error.reason());
                            }
                            warnings
                        },
                        receipt: Receipt {
                            action_id: id,
                            seq,
                            transport: transport.into(),
                            started_ms: started,
                            finished_ms: service.clock.now_ms(),
                            input_status,
                            observation_status,
                            cleanup: service.device_cleanup(&auth.device_id),
                            cancelled: requested.load(Ordering::SeqCst)
                                || result
                                    .error
                                    .is_some_and(|e| e.code == ErrorCode::ProcessCancelled),
                            retry_guidance:
                                "Inspect this receipt; never replay with a new sequence.".into(),
                        },
                        observation: captured.ok(),
                    };
                    completion.finish(action);
                    drop(state);
                });
                rx
            }
        };
        let final_action = loop {
            if let Some(action) = rx.borrow().clone() {
                break action;
            }
            self.budget(deadline)?;
            tokio::time::timeout_at(tokio::time::Instant::from_std(deadline), rx.changed())
                .await
                .map_err(|_| ErrorCode::ProcessTimeout)?
                .map_err(|_| ErrorCode::InputOutcomeUnknown)?;
        };
        Ok(match call {
            Call::Android { .. } => OperationResult::Android(final_action),
            Call::Tap { .. } => OperationResult::Tap(final_action),
            Call::Hold { .. } => OperationResult::Hold(final_action),
            Call::Swipe { .. } => OperationResult::Swipe(final_action),
            Call::Type { .. } => OperationResult::Type(final_action),
            Call::Key { .. } => OperationResult::Key(final_action),
            _ => unreachable!(),
        })
    }
    pub fn begin_stop(&self) {
        self.closing.store(true, Ordering::SeqCst);
        for id in self.devices.lock().unwrap().keys() {
            let _ = self.leases.invalidate(id);
        }
    }
    pub async fn shutdown(self: &Arc<Self>) {
        self.begin_stop();
        #[cfg(all(target_os = "linux", feature = "native-aoa"))]
        if let Ok(_gate) =
            tokio::time::timeout(Duration::from_secs(2), self.rearm_gate.lock()).await
        {
            let pending = self.pending_rearm.lock().unwrap().clone();
            if let Some(session) = pending {
                let _ = session.shutdown(Duration::from_secs(2)).await;
            }
        }

        let ids = self
            .devices
            .lock()
            .unwrap()
            .keys()
            .cloned()
            .collect::<Vec<_>>();
        for id in &ids {
            let _ = self.leases.invalidate(id);
        }
        let mut tasks = tokio::task::JoinSet::new();
        for id in ids {
            let service = self.clone();
            tasks.spawn(async move {
                service
                    .leases
                    .shutdown(&id, Duration::from_millis(service.limits.operation_ms))
                    .await
            });
        }
        while tasks.join_next().await.is_some() {}
    }
}
fn capabilities() -> DeviceCapabilities {
    let unavailable = || Capability {
        evidence: Evidence::Unavailable,
        reason: "Capability unavailable in the current backend; no hardware evidence claimed"
            .into(),
        profile: None,
    };
    DeviceCapabilities {
        screenshot: unavailable(),
        aoa_touch: unavailable(),
        keyboard: unavailable(),
        supported_keys: vec![],
        keyboard_layout: None,
        ocr: unavailable(),
        ocr_languages: vec![],
        media_scan: unavailable(),
    }
}
// Small encoder over already bounded owned PNG bytes; no additional dependency.
fn base64(bytes: &[u8]) -> String {
    const TABLE: &[u8] = b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    let mut out = String::with_capacity(bytes.len().div_ceil(3) * 4);
    for c in bytes.chunks(3) {
        let n = (u32::from(c[0]) << 16)
            | (u32::from(*c.get(1).unwrap_or(&0)) << 8)
            | u32::from(*c.get(2).unwrap_or(&0));
        out.push(TABLE[((n >> 18) & 63) as usize] as char);
        out.push(TABLE[((n >> 12) & 63) as usize] as char);
        out.push(if c.len() > 1 {
            TABLE[((n >> 6) & 63) as usize] as char
        } else {
            '='
        });
        out.push(if c.len() > 2 {
            TABLE[(n & 63) as usize] as char
        } else {
            '='
        });
    }
    out
}

#[cfg(all(test, target_os = "linux", feature = "native-aoa"))]
mod rearm_ownership_tests {
    use super::*;
    #[tokio::test]
    async fn abandoned_rearm_attempt_closes_the_retained_session() {
        // Explicit SOFTWARE observation driver; no USB/phone evidence.
        let session = Session::start_with_registration(
            Duration::from_secs(1),
            crate::session::Registration {
                settling: Duration::ZERO,
            },
            || Ok(crate::observation::ObserveSession),
        )
        .unwrap();
        session
            .ready(Duration::from_secs(1), &Cancel::default())
            .await
            .unwrap();
        let retained = session.clone();
        let attempt = RearmAttempt {
            session,
            armed: true,
        };
        drop(attempt);
        tokio::time::timeout(Duration::from_secs(1), async {
            while retained.status().phase != Phase::Closed {
                tokio::task::yield_now().await;
            }
        })
        .await
        .unwrap();
        assert!(retained.status().cleanup_error.is_none());
    }
}
