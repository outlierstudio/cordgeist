//! One locked daemon process. Production builds never install a software phone.
use crate::{
    api::{Call, ErrorCode, Request, Response},
    backend::Clock,
    ipc::server::{Dispatcher, Server, ServiceDispatcher},
    service::Service,
};
use std::{
    io,
    path::Path,
    sync::{Arc, atomic::AtomicBool},
    time::{Duration, Instant},
};
struct Wall(Instant);
impl Clock for Wall {
    fn now_ms(&self) -> u64 {
        self.0.elapsed().as_millis().min(u128::from(u64::MAX)) as u64
    }
}
pub const SOFTWARE_SELECTION: &str = "labeled-software-only";
fn software_hold(selected: bool) -> Option<Duration> {
    if !selected {
        return None;
    }
    #[cfg(not(feature = "fake"))]
    {
        None
    }
    #[cfg(feature = "fake")]
    {
        let Ok(text) = std::env::var("CORDGEIST_SOFTWARE_HOLD_MS") else {
            return None;
        };
        let ms: u64 = text.parse().ok()?;
        if ms == 0 || ms > 1500 {
            return None;
        }
        Some(Duration::from_millis(ms))
    }
}
struct HeldDispatch {
    inner: ServiceDispatcher,
    hold: Option<Duration>,
}
impl Dispatcher for HeldDispatch {
    fn boot_id(&self) -> &crate::api::BootId {
        self.inner.boot_id()
    }
    fn dispatch(&self, request: Request, exchange_deadline: Instant) -> Response {
        if let Some(hold) = self.hold
            && !matches!(request.call, Call::Status)
        {
            std::thread::sleep(hold);
        }
        self.inner.dispatch(request, exchange_deadline)
    }
    fn begin_stop(&self) {
        self.inner.begin_stop();
    }
    fn shutdown(&self) {
        self.inner.shutdown();
    }
}
pub fn run(parent: &Path) -> io::Result<()> {
    if !parent.is_absolute() {
        return Err(io::ErrorKind::PermissionDenied.into());
    }
    let selected = std::env::var("CORDGEIST_SOFTWARE_BACKEND").ok();
    if selected.is_some() && selected.as_deref() != Some(SOFTWARE_SELECTION) {
        return Err(io::ErrorKind::InvalidInput.into());
    }
    #[cfg(not(feature = "fake"))]
    if selected.is_some() {
        return Err(io::ErrorKind::Unsupported.into());
    }
    let runtime = tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_time()
        .enable_io()
        .build()?;
    let clock = Arc::new(Wall(Instant::now()));
    let (service, phone) = runtime.block_on(start_service(parent, clock, selected.is_some()))?;
    let hold = software_hold(selected.is_some());
    let server = Server::bind_dispatcher(
        parent,
        Arc::new(HeldDispatch {
            inner: ServiceDispatcher {
                service,
                runtime: runtime.handle().clone(),
            },
            hold,
        }),
    )?;
    let stop = server.stop_handle();
    let flag = parent.join("cordgeist").join("stop");
    let _ = std::fs::remove_file(&flag);
    let watch = flag.clone();
    let polling = Arc::new(AtomicBool::new(true));
    let polling_stop = polling.clone();
    std::thread::spawn(move || {
        while polling_stop.load(std::sync::atomic::Ordering::SeqCst) {
            if watch.exists() {
                stop.request();
                break;
            }
            std::thread::sleep(std::time::Duration::from_millis(20));
        }
    });
    let result = server.run();
    polling.store(false, std::sync::atomic::Ordering::SeqCst);
    let _held = phone;
    result
}
async fn start_service(
    parent: &Path,
    clock: Arc<dyn Clock + Send + Sync>,
    software: bool,
) -> io::Result<(Arc<Service>, Hold)> {
    if !software {
        let mut bytes = [0u8; 32];
        getrandom::fill(&mut bytes).map_err(|_| io::Error::other("boot"))?;
        let boot = crate::api::BootId(bytes.iter().map(|b| format!("{b:02x}")).collect());
        return arm_phone(parent, clock, boot).await;
    }
    #[cfg(feature = "fake")]
    {
        let (service, hold) = crate::software_fixture::install(parent, clock)
            .await
            .map_err(service_error)?;
        service.set_media_root(parent.to_path_buf());
        Ok((service, Hold::Software(hold)))
    }
    #[cfg(not(feature = "fake"))]
    {
        let _ = (parent, clock);
        Err(io::ErrorKind::Unsupported.into())
    }
}
#[allow(dead_code)]
enum Hold {
    Empty,
    #[cfg(feature = "fake")]
    Software(crate::software_fixture::Hold),
}
async fn arm_phone(
    parent: &Path,
    clock: Arc<dyn Clock + Send + Sync>,
    fallback_boot: crate::api::BootId,
) -> io::Result<(Arc<Service>, Hold)> {
    let failed = |clock: Arc<dyn Clock + Send + Sync>,
                  boot: crate::api::BootId,
                  code: crate::api::ErrorCode|
     -> io::Result<(Arc<Service>, Hold)> {
        let service =
            Service::new(boot, clock, crate::frames::Limits::default()).map_err(service_error)?;
        service.set_media_root(parent.to_path_buf());
        service.record_discovery(Err(code));
        Ok((service, Hold::Empty))
    };
    let Some(adb_path) = crate::adb::configured_path() else {
        return failed(
            clock,
            fallback_boot,
            crate::api::ErrorCode::BackendUnavailable,
        );
    };
    let port = std::env::var("CORDGEIST_ADB_PORT")
        .ok()
        .and_then(|value| value.parse().ok())
        .unwrap_or(5037);
    let adb = match crate::adb::Adb::new(adb_path, port) {
        Ok(adb) => adb,
        Err(code) => return failed(clock, fallback_boot, code),
    };
    #[cfg(not(feature = "native-aoa"))]
    {
        let _ = (parent, adb);
        failed(
            clock,
            fallback_boot,
            crate::api::ErrorCode::BackendUnavailable,
        )
    }
    #[cfg(feature = "native-aoa")]
    {
        match observe_native(parent, adb).await {
            Ok(observed) => install_observed(parent, clock, observed).await,
            Err(code) => failed(clock, fallback_boot, code),
        }
    }
}
#[cfg(feature = "native-aoa")]
struct ObservedPhone {
    discovery: crate::discovery_service::DiscoveryService<crate::adb::Adb>,
    route: crate::identity::RouteRef,
    capture: crate::adb::Adb,
    build: String,
    display: crate::api::Size,
    rotation: crate::api::Rotation,
}
#[cfg(feature = "native-aoa")]
async fn observe_native(parent: &Path, adb: crate::adb::Adb) -> Result<ObservedPhone, ErrorCode> {
    let pairings = parent.join("pairings");
    std::fs::create_dir_all(&pairings).map_err(|_| ErrorCode::BackendUnavailable)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&pairings, std::fs::Permissions::from_mode(0o700))
            .map_err(|_| ErrorCode::BackendUnavailable)?;
    }
    let store = crate::pairing_store::PairingStore::open(&pairings)
        .map_err(|_| ErrorCode::BackendUnavailable)?;
    let worker = crate::native::discovery::worker()?;
    let capture = adb.clone();
    let mut discovery = crate::discovery_service::DiscoveryService::new(worker, adb, store)?;
    let refreshed = discovery
        .refresh(Duration::from_secs(5), &crate::backend::Cancel::default())
        .await;
    let route = match refreshed {
        Ok(routes) => {
            let kind = if routes.len() == 1 {
                discovery.resolve(&routes[0])?.kind()
            } else {
                crate::discovery::DiscoveryKind::Native
            };
            crate::discovery::exact_native(routes.len(), kind)?;
            routes
                .into_iter()
                .next()
                .ok_or(ErrorCode::DeviceDisconnected)?
        }
        Err(code) => {
            let _ = discovery.shutdown(Duration::from_secs(2)).await;
            return Err(code);
        }
    };
    let resolved = match discovery.resolve(&route) {
        Ok(resolved) => resolved,
        Err(code) => {
            let _ = discovery.shutdown(Duration::from_secs(2)).await;
            return Err(code);
        }
    };
    let cancel = tokio_util::sync::CancellationToken::new();
    let build = match capture
        .build_identity(&resolved.target(), cancel.clone())
        .await
    {
        Ok(build) => build,
        Err(code) => {
            let _ = discovery.shutdown(Duration::from_secs(2)).await;
            return Err(code);
        }
    };
    let viewport = match capture.viewport(&resolved.target(), cancel).await {
        Ok(viewport) => viewport,
        Err(code) => {
            let _ = discovery.shutdown(Duration::from_secs(2)).await;
            return Err(code);
        }
    };
    let route = match discovery
        .refresh(Duration::from_secs(3), &crate::backend::Cancel::default())
        .await
    {
        Ok(routes) => {
            let kind = if routes.len() == 1 {
                match discovery.resolve(&routes[0]) {
                    Ok(resolved) => resolved.kind(),
                    Err(code) => {
                        let _ = discovery.shutdown(Duration::from_secs(2)).await;
                        return Err(code);
                    }
                }
            } else {
                crate::discovery::DiscoveryKind::Native
            };
            if let Err(code) = crate::discovery::exact_native(routes.len(), kind) {
                let _ = discovery.shutdown(Duration::from_secs(2)).await;
                return Err(code);
            }
            match routes.into_iter().next() {
                Some(route) => route,
                None => {
                    let _ = discovery.shutdown(Duration::from_secs(2)).await;
                    return Err(ErrorCode::DeviceDisconnected);
                }
            }
        }
        Err(code) => {
            let _ = discovery.shutdown(Duration::from_secs(2)).await;
            return Err(code);
        }
    };
    Ok(ObservedPhone {
        discovery,
        route,
        capture,
        build,
        display: viewport.logical,
        rotation: viewport.rotation,
    })
}
#[cfg(feature = "native-aoa")]
async fn install_observed(
    parent: &Path,
    clock: Arc<dyn Clock + Send + Sync>,
    observed: ObservedPhone,
) -> io::Result<(Arc<Service>, Hold)> {
    let ObservedPhone {
        mut discovery,
        route,
        capture,
        build,
        display,
        rotation,
    } = observed;
    let service = Service::new(
        route.boot_id.clone(),
        clock,
        crate::frames::Limits::default(),
    )
    .map_err(service_error)?;
    service.set_media_root(parent.to_path_buf());
    let options = crate::native::touchscreen_options();
    let logical_max = u16::try_from(options.x.logical_maximum)
        .map_err(|_| service_error(ErrorCode::InvalidRequest))?;
    let logical_max_y = u16::try_from(options.y.logical_maximum)
        .map_err(|_| service_error(ErrorCode::InvalidRequest))?;
    let mut profile = crate::frames::Profile {
        build,
        descriptor: crate::native::descriptor_identity(logical_max, logical_max_y)
            .map_err(service_error)?,
        backend: "adb-observe".into(),
        display,
        rotation,
        geometry: crate::api::GeometryVersion(1),
        mapping: crate::api::MappingVersion(1),
        transform: crate::geometry::Transform {
            rotation,
            max_x: logical_max,
            max_y: logical_max_y,
            verified: false,
            generic_screen: Some(display),
        },
    };
    let store = crate::pairing_store::PairingStore::open(&parent.join("pairings"))
        .map_err(|_| service_error(ErrorCode::MappingUnverified))?;
    if let Ok(Some(saved)) = store.load_calibration(&route.device_id, &profile) {
        profile = saved;
    }
    let mapping = profile.transform.clone();
    let verified = mapping.usable();
    let identities = discovery.identities();
    let route_for_guard = route.clone();
    let guard = move |reference: &crate::identity::RouteRef| {
        if reference != &route_for_guard {
            return Err(ErrorCode::LeaseInvalid);
        }
        let resolved = identities.lock().unwrap().resolve(reference)?;
        Ok(crate::native::session::Binding {
            route: resolved,
            mapping: mapping.clone(),
        })
    };
    let session = if verified {
        crate::native::session::start(route.clone(), Duration::from_secs(2), guard)
    } else {
        crate::native::session::start_descriptor_probe(route.clone(), Duration::from_secs(2), guard)
    }
    .map_err(service_error)?;
    let session = match session
        .ready(Duration::from_secs(2), &crate::backend::Cancel::default())
        .await
    {
        Ok(()) => session,
        Err(code) => {
            let _ = std::fs::write(parent.join("hid-ready.txt"), code.to_string());
            let fallback = crate::session::Session::start_with_registration(
                Duration::from_secs(2),
                crate::session::Registration {
                    settling: Duration::ZERO,
                },
                || Ok(crate::observation::ObserveSession),
            )
            .map_err(service_error)?;
            if let Err(code) = fallback
                .ready(Duration::from_secs(2), &crate::backend::Cancel::default())
                .await
            {
                let _ = discovery.shutdown(Duration::from_secs(2)).await;
                service.record_discovery(Err(code));
                return Ok((service, Hold::Empty));
            }
            profile.transform.verified = false;
            profile.transform.generic_screen = None;
            fallback
        }
    };
    let descriptor = profile.descriptor.clone();
    if let Err(code) = service.install_observed(
        discovery.identities(),
        route.clone(),
        session,
        profile,
        Arc::new(crate::service::AdbCapture {
            adb: capture,
            descriptor,
            backend: "adb-observe".into(),
        }),
    ) {
        let _ = discovery.shutdown(Duration::from_secs(2)).await;
        service.record_discovery(Err(code));
        return Ok((service, Hold::Empty));
    }
    service.record_discovery(Ok(route));
    service.attach_phone(discovery);
    Ok((service, Hold::Empty))
}
fn service_error(code: ErrorCode) -> io::Error {
    io::Error::other(code.to_string())
}
