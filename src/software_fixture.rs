//! Explicit labeled software backend for development tests. Not a production device.
#![cfg(feature = "fake")]
use crate::{
    adb::{Attachment, Authorization},
    api::*,
    backend::Cancel,
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
    path::Path,
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
    time::Duration,
};
use tokio_util::sync::CancellationToken;
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
struct FakeUsb;
impl DiscoveryDriver for FakeUsb {
    fn kind(&self) -> DiscoveryKind {
        DiscoveryKind::SoftwareFake
    }
    fn scan(&mut self, _: std::time::Instant, _: &Cancel) -> Result<Vec<UsbDevice>, ErrorCode> {
        Ok(vec![usb()])
    }
    fn close(&mut self) -> CloseStatus {
        CloseStatus::Consumed(Ok(()))
    }
}
struct FakeInventory;
impl InventorySource for FakeInventory {
    async fn list(&self, _: CancellationToken) -> Result<Vec<Attachment>, ErrorCode> {
        Ok(vec![adb()])
    }
}
struct FakeSession {
    effects: Arc<AtomicUsize>,
    stage: Arc<AtomicUsize>,
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
        CloseOutcome::Consumed(Ok(()))
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
        transition: &Transition,
        _: &Handles<()>,
        _: CallBudget,
        _: &Cancel,
        _: &Cancel,
    ) -> ReportResult {
        match transition {
            Transition::Android(AndroidCommand::LaunchApp { .. }) => {
                self.stage.store(1, Ordering::SeqCst);
            }
            Transition::Android(AndroidCommand::PressButton {
                button: AndroidButton::Home,
            }) => {
                self.stage.store(0, Ordering::SeqCst);
            }
            Transition::Android(AndroidCommand::PressButton {
                button: AndroidButton::Back,
            }) => {
                self.stage.store(1, Ordering::SeqCst);
            }
            Transition::Android(AndroidCommand::PressButton {
                button: AndroidButton::Enter,
            }) => {
                self.stage.store(3, Ordering::SeqCst);
            }
            Transition::Contact { down: true, .. } if self.stage.load(Ordering::SeqCst) == 1 => {
                self.stage.store(2, Ordering::SeqCst);
            }
            Transition::Key { down: true, .. } if self.stage.load(Ordering::SeqCst) >= 2 => {
                self.stage.store(3, Ordering::SeqCst);
            }
            _ => {}
        }
        if matches!(
            transition,
            Transition::Android(_)
                | Transition::Gesture(_)
                | Transition::Text(_)
                | Transition::Contact { down: true, .. }
                | Transition::Key { down: true, .. }
        ) {
            self.effects.fetch_add(1, Ordering::SeqCst);
            if let Ok(path) = std::env::var("CORDGEIST_EFFECT_LOG") {
                let _ = std::fs::OpenOptions::new()
                    .create(true)
                    .append(true)
                    .open(path)
                    .and_then(|mut f| {
                        use std::io::Write;
                        writeln!(f, "effect")
                    });
            }
            if let Ok(ms) = std::env::var("CORDGEIST_SOFTWARE_DELAY_MS")
                && let Ok(ms) = ms.parse::<u64>()
            {
                std::thread::sleep(Duration::from_millis(ms));
            }
        }
        ReportResult {
            delivery: Delivery::Sent,
            error: None,
        }
    }
}
struct FakeCapture {
    stage: Arc<AtomicUsize>,
}
impl CaptureSource for FakeCapture {
    fn elements<'a>(
        &'a self,
        _: ResolvedRoute,
        _: CancellationToken,
    ) -> std::pin::Pin<
        Box<dyn std::future::Future<Output = Result<ElementCapture, ErrorCode>> + Send + 'a>,
    > {
        Box::pin(async move {
            if std::env::var("CORDGEIST_SOFTWARE_UI_UNAVAILABLE").as_deref() == Ok("1") {
                return Err(ErrorCode::CaptureUnavailable);
            }
            if std::env::var("CORDGEIST_SOFTWARE_UI_UNAVAILABLE").as_deref() == Ok("slow") {
                tokio::time::sleep(Duration::from_millis(2200)).await;
            }
            let stage = self.stage.load(Ordering::SeqCst);
            let nodes = if stage == 0 {
                r#"<node text="Settings" clickable="true" bounds="[0,1][4,2]"/>"#
            } else if stage < 3 {
                r#"<node class="android.widget.EditText" text="SYNTHETIC_SECRET" content-desc="Search" resource-id="search" clickable="true" focused="true" bounds="[0,1][4,2]"/>"#
            } else {
                r#"<node class="android.widget.EditText" text="SYNTHETIC_SECRET" content-desc="Search" resource-id="search" clickable="true" focused="true" bounds="[0,1][4,2]"/><node text="Bluetooth" resource-id="bluetooth" bounds="[0,3][4,4]"/><node text="Off" bounds="[0,5][4,6]"/>"#
            };
            let xml = format!("<hierarchy>{nodes}</hierarchy>");
            Ok(ElementCapture {
                elements: crate::android_ui::parse(
                    xml.as_bytes(),
                    Size {
                        width: 4,
                        height: 8,
                    },
                )?,
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
    fn apps<'a>(
        &'a self,
        _: ResolvedRoute,
        _: CancellationToken,
    ) -> std::pin::Pin<
        Box<dyn std::future::Future<Output = Result<Vec<String>, ErrorCode>> + Send + 'a>,
    > {
        Box::pin(async { Ok(vec!["com.android.settings".into()]) })
    }

    fn capture<'a>(&'a self, _: ResolvedRoute, _: CancellationToken) -> CaptureFuture<'a> {
        Box::pin(async move {
            Ok(Capture {
                image: Size {
                    width: 4,
                    height: 8,
                },
                png: include_bytes!("../tests/fixtures/synthetic.png").to_vec(),
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
pub struct Hold;
pub(crate) struct Rearm {
    discovery: DiscoveryService<FakeInventory>,
    pending: Option<Session>,
    stage: Arc<AtomicUsize>,
}
impl Rearm {
    pub(crate) async fn rearm(
        &mut self,
        service: &Service,
        id: &DeviceId,
        profile: Profile,
        capture: Arc<dyn CaptureSource>,
        deadline: std::time::Instant,
    ) -> Result<(), ErrorCode> {
        let remaining = || {
            deadline
                .checked_duration_since(std::time::Instant::now())
                .filter(|d| !d.is_zero())
                .map(|d| d.min(Duration::from_secs(1)))
                .ok_or(ErrorCode::ProcessTimeout)
        };
        if let Some(session) = self.pending.as_ref() {
            let status = session.shutdown(remaining()?).await?;
            if status.phase != Phase::Closed || status.cleanup_error.is_some() {
                return Err(ErrorCode::NativeCleanupPending);
            }
            self.pending = None;
        }
        let routes = self
            .discovery
            .refresh(remaining()?, &Cancel::default())
            .await?;
        if routes.len() != 1 || routes[0].device_id != *id {
            return Err(ErrorCode::DeviceIdentityAmbiguous);
        }
        let session = Session::start_with_registration(
            remaining()?,
            Registration {
                settling: Duration::ZERO,
            },
            {
                let stage = self.stage.clone();
                move || {
                    Ok(FakeSession {
                        effects: Arc::new(AtomicUsize::new(0)),
                        stage,
                    })
                }
            },
        )?;
        self.pending = Some(session.clone());
        session.ready(remaining()?, &Cancel::default()).await?;
        service.reconnect_software(
            self.discovery.identities(),
            routes[0].clone(),
            session,
            profile,
            capture,
        )?;
        self.pending = None;
        Ok(())
    }
}
pub async fn install(
    parent: &Path,
    clock: Arc<dyn crate::backend::Clock + Send + Sync>,
) -> Result<(Arc<Service>, Hold), ErrorCode> {
    let dir = parent.join("software-pairing");
    std::fs::create_dir_all(&dir).map_err(|_| ErrorCode::BackendUnavailable)?;
    use std::os::unix::fs::PermissionsExt;
    std::fs::set_permissions(&dir, std::fs::Permissions::from_mode(0o700))
        .map_err(|_| ErrorCode::BackendUnavailable)?;
    let store = PairingStore::open(&dir).map_err(|_| ErrorCode::BackendUnavailable)?;
    store
        .set_pairing(Pairing::confirm(
            DeviceId("software-phone".into()),
            None,
            &usb(),
            &adb(),
            false,
        )?)
        .map_err(|_| ErrorCode::BackendUnavailable)?;
    let worker = DiscoveryWorker::start(|| Ok(FakeUsb))?;
    let mut discovery = DiscoveryService::new(worker, FakeInventory, store)?;
    let routes = discovery
        .refresh(Duration::from_secs(1), &Cancel::default())
        .await?;
    let route = routes
        .into_iter()
        .find(|r| r.device_id.0 == "software-phone")
        .ok_or(ErrorCode::BackendUnavailable)?;
    let service = Service::new(route.boot_id.clone(), clock, Limits::default())?;
    let stage = Arc::new(AtomicUsize::new(0));
    let driver_stage = stage.clone();
    let session = Session::start_with_registration(
        Duration::from_secs(1),
        Registration {
            settling: Duration::ZERO,
        },
        move || {
            Ok(FakeSession {
                effects: Arc::new(AtomicUsize::new(0)),
                stage: driver_stage,
            })
        },
    )?;
    session
        .ready(Duration::from_secs(1), &Cancel::default())
        .await?;
    service.install_software(
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
                verified: true,
                generic_screen: None,
            },
        },
        Arc::new(FakeCapture {
            stage: stage.clone(),
        }),
    )?;
    service
        .set_software_rearm(Rearm {
            discovery,
            pending: None,
            stage,
        })
        .await;
    Ok((service, Hold))
}
