//! Private pinned-C handles behind the lifecycle-only Session owner.
//! No discovery, accessory switching, gestures or implicit authorization.
use crate::session_input::{Delivery, ReportResult, Transition};
use crate::{
    api::ErrorCode,
    backend::Cancel,
    discovery::DiscoveryKind,
    geometry::Transform,
    identity::{ResolvedRoute, RouteRef},
    session::{CallBudget, CloseOutcome, Failure, Handles, Resource, Session, SessionDriver},
};
use aoahid_sys as sys;
use std::{ffi::CString, marker::PhantomData, ptr::NonNull, rc::Rc, time::Duration};

/// Owned evidence returned by the future daemon's authoritative execution check.
/// This does not grant a lease. The guard must resolve the immutable reference and
/// validate health/fence/mapping each time; no cached route is a live capability.
pub struct Binding {
    pub route: ResolvedRoute,
    pub mapping: Transform,
}
/// Low-level lifecycle initialization only. Never call from CLI/MCP directly.
/// Cleanup deliberately bypasses expired authority so already-owned input can be
/// neutralized. Tracked reports use the same guarded owner; no gesture/daemon API is supplied.
pub fn start_descriptor_probe<G>(
    reference: RouteRef,
    budget: Duration,
    guard: G,
) -> Result<Session, ErrorCode>
where
    G: FnMut(&RouteRef) -> Result<Binding, ErrorCode> + Send + 'static,
{
    Session::start(budget, move || Ok(NativeSession::probe(reference, guard)))
}
pub fn start<G>(reference: RouteRef, budget: Duration, guard: G) -> Result<Session, ErrorCode>
where
    G: FnMut(&RouteRef) -> Result<Binding, ErrorCode> + Send + 'static,
{
    Session::start(budget, move || Ok(NativeSession::new(reference, guard)))
}
pub fn start_with_registration<G>(
    reference: RouteRef,
    budget: Duration,
    registration: crate::session::Registration,
    guard: G,
) -> Result<Session, ErrorCode>
where
    G: FnMut(&RouteRef) -> Result<Binding, ErrorCode> + Send + 'static,
{
    Session::start_with_registration(budget, registration, move || {
        Ok(NativeSession::new(reference, guard))
    })
}
struct NativeSession<G> {
    reference: RouteRef,
    guard: G,
    binding: Option<Binding>,
    /// Allows one descriptor-range probe before a screenshot has shown an effect.
    descriptor_probe: bool,
    _owner: PhantomData<Rc<()>>,
}
// These handles never leave the owner or implement Send/Sync. Slot removal is
// performed by Session only for a consuming close result.
enum Handle {
    Context(NonNull<sys::aoahid_context>),
    Device(NonNull<sys::aoahid_device>),
    Spec(NonNull<sys::aoahid_spec>),
    Node(NonNull<sys::aoahid_node>),
}
fn code(status: i32) -> ErrorCode {
    match status {
        sys::AOAHID_ERR_NO_DEVICE => ErrorCode::DeviceDisconnected,
        sys::AOAHID_ERR_TIMEOUT => ErrorCode::ProcessTimeout,
        sys::AOAHID_ERR_OVERFLOW => ErrorCode::OutputLimit,
        sys::AOAHID_CLOSE_PENDING => ErrorCode::NativeCleanupPending,
        _ => ErrorCode::BackendUnavailable,
    }
}
fn failure(status: i32) -> Failure {
    // Copy every scalar before another call (even successful spec release)
    // replaces TLS. Never retain pointers or raw field/reason strings.
    let mut result = Failure::at(code(status), "aoa-hid");
    result.native_code = status;
    if let Some(detail) = unsafe { sys::aoahid_last_error().as_ref() } {
        result.transport_status = detail.transport_status;
        result.request = detail.aoa_request;
        result.node_id = detail.node_id;
        result.report_id = detail.report_id;
        result.offset = detail.offset;
        result.length = detail.length;
    }
    result
}
fn opened<T>(status: i32, raw: *mut T) -> Result<NonNull<T>, Failure> {
    if status != sys::AOAHID_OK {
        return Err(failure(status));
    }
    NonNull::new(raw).ok_or(Failure::code(ErrorCode::BackendUnavailable))
}
fn close_outcome(status: i32) -> CloseOutcome {
    match status {
        sys::AOAHID_CLOSE_PENDING => CloseOutcome::Pending,
        sys::AOAHID_ERR_PARAM | sys::AOAHID_ERR_UNSET_FIELD => {
            CloseOutcome::Retained(failure(status))
        }
        sys::AOAHID_OK => CloseOutcome::Consumed(Ok(())),
        _ => CloseOutcome::Consumed(Err(failure(status))),
    }
}
impl<G> NativeSession<G> {
    fn new(reference: RouteRef, guard: G) -> Self {
        Self {
            reference,
            guard,
            binding: None,
            descriptor_probe: false,
            _owner: PhantomData,
        }
    }
    fn probe(reference: RouteRef, guard: G) -> Self {
        let mut session = Self::new(reference, guard);
        session.descriptor_probe = true;
        session
    }
}
fn fake_route_allowed() -> bool {
    // Only compiled library tests may accept explicitly SOFTWARE FAKE identity.
    // Resolve the isolated marker before any native Context/Device is created.
    #[cfg(all(test, target_os = "linux"))]
    {
        !unsafe { libc::dlsym(libc::RTLD_DEFAULT, c"cg_test_discovery_seed".as_ptr()) }.is_null()
    }
    #[cfg(not(all(test, target_os = "linux")))]
    {
        false
    }
}
impl<G> NativeSession<G>
where
    G: FnMut(&RouteRef) -> Result<Binding, ErrorCode>,
{
    fn validate(&mut self, cancel: &Cancel) -> Result<(), Failure> {
        if cancel.cancelled() {
            return Err(Failure::code(ErrorCode::ProcessCancelled));
        }
        let binding = (self.guard)(&self.reference).map_err(Failure::code)?;
        if binding.route.reference() != &self.reference {
            return Err(Failure::code(ErrorCode::LeaseInvalid));
        }
        binding.route.usb().topology().map_err(Failure::code)?;
        if binding.route.kind() == DiscoveryKind::SoftwareFake && !fake_route_allowed() {
            return Err(Failure::code(ErrorCode::BackendUnavailable));
        }
        if binding.route.usb().aoa_protocol < 2 {
            return Err(Failure::code(ErrorCode::BackendUnavailable));
        }
        if binding.mapping.max_x == 0 || binding.mapping.max_y == 0 {
            return Err(Failure::code(ErrorCode::MappingUnverified));
        }
        if !binding.mapping.usable() {
            let descriptor = super::touchscreen_options().x.logical_maximum as u16;
            if !self.descriptor_probe
                || binding.mapping.max_x != descriptor
                || binding.mapping.max_y != descriptor
            {
                return Err(Failure::code(ErrorCode::MappingUnverified));
            }
        }
        if let Some(original) = &self.binding {
            if binding.route.usb() != original.route.usb()
                || binding.route.adb_attachment() != original.route.adb_attachment()
            {
                return Err(Failure::code(ErrorCode::DeviceIdentityAmbiguous));
            }
            if binding.mapping.rotation != original.mapping.rotation
                || binding.mapping.max_x != original.mapping.max_x
                || binding.mapping.max_y != original.mapping.max_y
            {
                return Err(Failure::code(ErrorCode::GeometryChanged));
            }
        }
        if cancel.cancelled() {
            return Err(Failure::code(ErrorCode::ProcessCancelled));
        }
        self.binding = Some(binding);
        Ok(())
    }
}
impl<G> SessionDriver for NativeSession<G>
where
    G: FnMut(&RouteRef) -> Result<Binding, ErrorCode>,
{
    type Handle = Handle;
    fn check_settling(&mut self, cancel: &Cancel) -> Result<(), Failure> {
        self.validate(cancel)
    }
    fn validate_report(
        &mut self,
        t: &Transition,
        cancel: &Cancel,
        external: &Cancel,
    ) -> Result<(), Failure> {
        self.validate(cancel)?;
        if external.cancelled() {
            return Err(Failure::code(ErrorCode::ProcessCancelled));
        }
        let mapping = &self.binding.as_ref().unwrap().mapping;
        let valid = |item: &Transition| match item {
            Transition::Android(command) => command.validate().is_ok(),
            Transition::Key { usage, .. } => crate::session_input::supported_key(*usage),
            Transition::Contact { x, y, .. } => *x <= mapping.max_x && *y <= mapping.max_y,
            Transition::Batch(_)
            | Transition::Gesture(_)
            | Transition::Text(_)
            | Transition::TextSubmit(_) => false,
        };
        let accepted = match t {
            Transition::Text(text) | Transition::TextSubmit(text) => {
                let plan = text.plan().map_err(Failure::code)?;
                plan.iter().flatten().all(valid)
            }
            Transition::Gesture(gesture) => gesture
                .plan()
                .is_ok_and(|plan| plan.iter().all(|(_, item)| valid(item))),
            Transition::Batch(items) => {
                !items.is_empty() && items.len() <= 64 && items.iter().all(valid)
            }
            _ => valid(t),
        };
        if accepted {
            Ok(())
        } else {
            Err(Failure::code(ErrorCode::InvalidRequest))
        }
    }
    fn apply_report(
        &mut self,
        t: &Transition,
        h: &Handles<Handle>,
        cancel: &Cancel,
        external: &Cancel,
    ) -> Result<(), Failure> {
        self.validate(cancel)?;
        if external.cancelled() {
            return Err(Failure::code(ErrorCode::ProcessCancelled));
        }
        if let Transition::Android(command) = t {
            return command.validate().map_err(Failure::code);
        }
        let resource = match t {
            Transition::Key { .. } => Resource::KeyboardNode,
            _ => Resource::TouchNode,
        };
        let Some(Handle::Node(node)) = h.get(resource) else {
            return Err(Failure::code(ErrorCode::BackendUnavailable));
        };
        let status = unsafe {
            match t {
                Transition::Android(_)
                | Transition::TextSubmit(_)
                | Transition::Batch(_)
                | Transition::Gesture(_)
                | Transition::Text(_) => {
                    return Err(Failure::code(ErrorCode::InvalidRequest));
                }
                Transition::Key { usage, down } => {
                    sys::aoahid_keyboard_key(node.as_ptr(), *usage, u32::from(*down))
                }
                Transition::Contact { down, x, y } => {
                    let mapping = &self.binding.as_ref().unwrap().mapping;
                    if *x > mapping.max_x || *y > mapping.max_y {
                        return Err(Failure::code(ErrorCode::InvalidRequest));
                    }
                    sys::aoahid_touchscreen_contact(
                        node.as_ptr(),
                        0,
                        u32::from(*down),
                        i32::from(*x),
                        i32::from(*y),
                        std::ptr::null(),
                    )
                }
            }
        };
        if status == sys::AOAHID_OK {
            Ok(())
        } else {
            Err(failure(status))
        }
    }
    fn submit_report(
        &mut self,
        t: &Transition,
        h: &Handles<Handle>,
        budget: CallBudget,
        cancel: &Cancel,
        external: &Cancel,
    ) -> ReportResult {
        let denied = |error| ReportResult {
            delivery: Delivery::NotSent,
            error: Some(error),
        };
        if let Err(error) = self.validate(cancel) {
            return denied(error);
        }
        if external.cancelled() {
            return denied(Failure::code(ErrorCode::ProcessCancelled));
        }
        if let Transition::Android(command) = t {
            let binding = self.binding.as_ref().unwrap();
            return crate::android_control::submit(
                &binding.route.target(),
                command,
                budget.deadline(),
                cancel,
                external,
            );
        }
        let budget = match budget.remaining() {
            Ok(b) => b,
            Err(e) => return denied(e),
        };
        let resource = match t {
            Transition::Key { .. } => Resource::KeyboardNode,
            _ => Resource::TouchNode,
        };
        let Some(Handle::Node(node)) = h.get(resource) else {
            return denied(Failure::code(ErrorCode::BackendUnavailable));
        };
        let status = unsafe { sys::aoahid_node_submit_blocking(node.as_ptr(), budget.control_ms) };
        if status == sys::AOAHID_OK {
            ReportResult {
                delivery: Delivery::Sent,
                error: None,
            }
        } else {
            ReportResult {
                delivery: if status == sys::AOAHID_ERR_STALL {
                    Delivery::NotSent
                } else {
                    Delivery::Unknown
                },
                error: Some(failure(status)),
            }
        }
    }

    fn open(
        &mut self,
        resource: Resource,
        handles: &Handles<Handle>,
        budget: CallBudget,
        cancel: &Cancel,
    ) -> Result<Handle, Failure> {
        self.validate(cancel)?;
        // Recheck time after authoritative validation, before each foreign phase.
        let budget = budget.remaining()?;
        let mut raw = std::ptr::null_mut();
        match resource {
            Resource::Context => {
                if std::env::var_os("LIBUSB_DEBUG").is_some_and(|v| v != "0")
                    || !super::verify_abi()
                {
                    return Err(Failure::code(ErrorCode::BackendUnavailable));
                }
                let mut options = super::context_options();
                options.event_mode = sys::AOAHID_EVENT_INTERNAL_THREAD;
                options.log_level = sys::AOAHID_LOG_DISABLED;
                let status = unsafe { sys::aoahid_context_create(&options, &mut raw) };
                opened(status, raw).map(Handle::Context)
            }
            Resource::Device => {
                let Some(Handle::Context(context)) = handles.get(Resource::Context) else {
                    return Err(Failure::code(ErrorCode::BackendUnavailable));
                };
                let usb = self.binding.as_ref().unwrap().route.usb();
                let serial = usb
                    .serial
                    .as_deref()
                    .map(CString::new)
                    .transpose()
                    .map_err(|_| Failure::code(ErrorCode::DeviceIdentityAmbiguous))?;
                let selected = sys::aoahid_device_info {
                    bus_number: usb.bus,
                    device_address: usb.address,
                    // Pinned upstream prefers topology over address if ports are set.
                    // Use its bus/address branch so a replacement at this port
                    // with a new address cannot inherit this attachment's open.
                    // The authoritative guard still validates the full topology.
                    port_path: std::ptr::null(),
                    port_path_length: 0,
                    vendor_id: usb.vendor_id,
                    product_id: usb.product_id,
                    protocol_version: usb.aoa_protocol,
                    serial: serial.as_ref().map_or(std::ptr::null(), |s| s.as_ptr()),
                    product: std::ptr::null(),
                };
                let mut options = super::device_options();
                options.interface_claim_policy = sys::AOAHID_INTERFACE_CLAIM_NONE;
                options.validate_reports = 1;
                let mut device = std::ptr::null_mut();
                let status = unsafe {
                    sys::aoahid_device_open(context.as_ptr(), &selected, &options, &mut device)
                };
                opened(status, device).map(Handle::Device)
            }
            Resource::TouchSpec => {
                let mapping = &self.binding.as_ref().unwrap().mapping;
                let options = super::single_contact_options(mapping.max_x, mapping.max_y);
                let mut spec = std::ptr::null_mut();
                let status = unsafe { sys::aoahid_spec_create_touchscreen(&options, &mut spec) };
                opened(status, spec).map(Handle::Spec)
            }
            Resource::KeyboardSpec => {
                let options = super::keyboard_options();
                let mut spec = std::ptr::null_mut();
                let status = unsafe { sys::aoahid_spec_create_keyboard(&options, &mut spec) };
                opened(status, spec).map(Handle::Spec)
            }
            Resource::TouchNode | Resource::KeyboardNode => {
                let spec = if resource == Resource::TouchNode {
                    Resource::TouchSpec
                } else {
                    Resource::KeyboardSpec
                };
                let (Some(Handle::Device(device)), Some(Handle::Spec(spec))) =
                    (handles.get(Resource::Device), handles.get(spec))
                else {
                    return Err(Failure::code(ErrorCode::BackendUnavailable));
                };
                let options = super::node_options();
                let mut node = std::ptr::null_mut();
                let status = unsafe {
                    sys::aoahid_node_open(
                        device.as_ptr(),
                        spec.as_ptr(),
                        &options,
                        budget.control_ms,
                        &mut node,
                    )
                };
                opened(status, node).map(Handle::Node)
            }
        }
    }
    fn close(
        &mut self,
        resource: Resource,
        handle: &mut Handle,
        budget: CallBudget,
    ) -> CloseOutcome {
        let status = unsafe {
            match (resource, handle) {
                (Resource::Context, Handle::Context(p)) => sys::aoahid_context_destroy_blocking(
                    p.as_ptr(),
                    budget.control_ms,
                    budget.drain_ms,
                    1000,
                ),
                (Resource::Device, Handle::Device(p)) => {
                    sys::aoahid_device_close(p.as_ptr(), budget.control_ms, budget.drain_ms)
                }
                (Resource::TouchNode | Resource::KeyboardNode, Handle::Node(p)) => {
                    sys::aoahid_node_close(p.as_ptr(), budget.control_ms, budget.drain_ms)
                }
                (Resource::TouchSpec | Resource::KeyboardSpec, Handle::Spec(p)) => {
                    sys::aoahid_spec_release(p.as_ptr());
                    sys::AOAHID_OK
                }
                _ => return CloseOutcome::Retained(Failure::code(ErrorCode::BackendUnavailable)),
            }
        };
        close_outcome(status)
    }
}

#[cfg(all(test, target_os = "linux"))]
mod tests {
    use super::*;
    use crate::{
        adb::{Attachment, Authorization},
        api::{BootId, Rotation},
        discovery::{DiscoverySnapshot, UsbDevice},
        identity::{IdentityRegistry, PairingConfig},
        session::Phase,
    };
    use std::{
        sync::{Arc, Mutex},
        time::Instant,
    };
    type Guard = Box<dyn FnMut(&RouteRef) -> Result<Binding, ErrorCode> + Send>;
    fn setup(address: u8, verified: bool) -> (RouteRef, Guard) {
        let usb = UsbDevice {
            bus: 1,
            address,
            ports: vec![2, 3],
            vendor_id: 1,
            product_id: 2,
            aoa_protocol: 2,
            serial: Some("SYNTHETIC_NATIVE_SERIAL".into()),
            product: Some("SYNTHETIC_NATIVE_PRODUCT".into()),
        };
        let adb = Attachment {
            serial: usb.serial.clone().unwrap(),
            state: Authorization::Authorized,
            transport_id: Some(8),
            usb: Some("1-2.3".into()),
        };
        let mut registry = IdentityRegistry::new(BootId("synthetic_boot".into()));
        let reference = registry
            .refresh(
                DiscoverySnapshot {
                    kind: DiscoveryKind::SoftwareFake,
                    captured_at: Instant::now(),
                    devices: vec![usb],
                },
                &[adb],
                &PairingConfig::default(),
            )
            .unwrap()
            .remove(0);
        let registry = Arc::new(Mutex::new(registry));
        (
            reference,
            Box::new(move |reference| {
                Ok(Binding {
                    route: registry.lock().unwrap().resolve(reference)?,
                    mapping: Transform {
                        rotation: Rotation::R0,
                        max_x: 32767,
                        max_y: 32767,
                        verified,
                        generic_screen: None,
                    },
                })
            }),
        )
    }
    unsafe fn symbol(name: &std::ffi::CStr) -> *mut std::ffi::c_void {
        let p = unsafe { libc::dlsym(libc::RTLD_DEFAULT, name.as_ptr()) };
        assert!(
            !p.is_null(),
            "isolated SOFTWARE FAKE marker/control missing"
        );
        p
    }
    #[tokio::test]
    #[ignore = "requires isolated marked SOFTWARE FAKE libusb ABI library"]
    async fn software_fake_native_abi_session() {
        // Marker resolution precedes even a public factory call. A real loaded
        // libusb library cannot proceed into Context/Device initialization here.
        let seed: unsafe extern "C" fn(i32) -> i32 =
            unsafe { std::mem::transmute(symbol(c"cg_test_discovery_seed")) };
        let verify: unsafe extern "C" fn(i32) -> i32 =
            unsafe { std::mem::transmute(symbol(c"cg_test_session_verify")) };
        let release: unsafe extern "C" fn() =
            unsafe { std::mem::transmute(symbol(c"cg_test_session_release_callback")) };
        let fail: unsafe extern "C" fn() =
            unsafe { std::mem::transmute(symbol(c"cg_test_session_close_error")) };
        if std::env::var_os("LIBUSB_DEBUG").is_some_and(|v| v != "0") {
            assert_eq!(unsafe { seed(0) }, 0);
            let (reference, guard) = setup(7, true);
            let session = start(reference, Duration::from_secs(2), guard).unwrap();
            assert_eq!(
                session
                    .ready(Duration::from_secs(2), &Cancel::default())
                    .await,
                Err(ErrorCode::BackendUnavailable)
            );
            session.shutdown(Duration::from_secs(2)).await.unwrap();
            assert_eq!(unsafe { verify(2) }, 1);
            return;
        }
        assert_eq!(unsafe { seed(0) }, 0);
        let (reference, guard) = setup(7, true);
        let session = start(reference, Duration::from_secs(2), guard).unwrap();
        session
            .ready(Duration::from_secs(2), &Cancel::default())
            .await
            .unwrap();
        let invalid = Transition::Batch(vec![
            Transition::Key {
                usage: 4,
                down: true,
            },
            Transition::Contact {
                down: true,
                x: 65535,
                y: 0,
            },
        ]);
        assert_eq!(
            session
                .submit(invalid, Duration::from_secs(1), &Cancel::default())
                .await
                .delivery,
            Delivery::NotSent
        );
        session.shutdown(Duration::from_secs(2)).await.unwrap();
        assert_eq!(
            unsafe { verify(0) },
            1,
            "whole native batch rejects before any report"
        );
        assert_eq!(unsafe { seed(0) }, 0);
        let (reference, mut original) = setup(7, true);
        let cancel = Cancel::default();
        let signal = cancel.clone();
        let armed = Arc::new(std::sync::atomic::AtomicBool::new(false));
        let trigger = armed.clone();
        let mut validations = 0;
        let guard = move |r: &RouteRef| {
            if trigger.load(std::sync::atomic::Ordering::SeqCst) {
                validations += 1;
            }
            let binding = original(r)?;
            if validations == 3 {
                signal.cancel();
            }
            Ok(binding)
        };
        let session = start(reference, Duration::from_secs(2), guard).unwrap();
        session
            .ready(Duration::from_secs(2), &Cancel::default())
            .await
            .unwrap();
        armed.store(true, std::sync::atomic::Ordering::SeqCst);
        let result = session
            .submit(
                Transition::Key {
                    usage: 4,
                    down: true,
                },
                Duration::from_secs(1),
                &cancel,
            )
            .await;
        assert_ne!(result.delivery, Delivery::Sent);
        session.shutdown(Duration::from_secs(2)).await.unwrap();
        let neutral_only: unsafe extern "C" fn() -> i32 =
            unsafe { std::mem::transmute(symbol(c"cg_test_session_neutral_only_verify")) };
        assert_eq!(
            unsafe { neutral_only() },
            1,
            "cancelled guard may produce only neutral cleanup"
        );
        assert_eq!(unsafe { seed(0) }, 0);
        let (reference, guard) = setup(7, true);
        let session = start(reference, Duration::from_secs(2), guard).unwrap();
        session
            .ready(Duration::from_secs(2), &Cancel::default())
            .await
            .unwrap();
        let report_count: unsafe extern "C" fn() -> usize =
            unsafe { std::mem::transmute(symbol(c"cg_test_session_report_count")) };
        for usage in [0xde, 0xdf] {
            let request = Transition::Batch(vec![
                Transition::Key {
                    usage: 4,
                    down: true,
                },
                Transition::Key { usage, down: true },
            ]);
            assert_eq!(
                session
                    .submit(request, Duration::from_secs(1), &Cancel::default())
                    .await
                    .delivery,
                Delivery::NotSent
            );
        }
        assert_eq!(
            unsafe { report_count() },
            0,
            "invalid trailing key must cause zero reports"
        );
        for usage in [0x04, 0xdd, 0xe0, 0xe7] {
            let request = Transition::Batch(vec![
                Transition::Key { usage, down: true },
                Transition::Key { usage, down: false },
            ]);
            assert_eq!(
                session
                    .submit(request, Duration::from_secs(1), &Cancel::default())
                    .await
                    .delivery,
                Delivery::Sent
            );
        }
        session.shutdown(Duration::from_secs(2)).await.unwrap();
        let cleanup_fault: unsafe extern "C" fn(i32) =
            unsafe { std::mem::transmute(symbol(c"cg_test_session_submit_fault")) };
        let delay_cleanup: unsafe extern "C" fn() =
            unsafe { std::mem::transmute(symbol(c"cg_test_session_delay_callback")) };
        let clean: unsafe extern "C" fn() -> i32 =
            unsafe { std::mem::transmute(symbol(c"cg_test_session_clean_verify")) };
        for (mode, reason) in [
            ErrorCode::LeaseInvalid,
            ErrorCode::DeviceDisconnected,
            ErrorCode::MappingUnverified,
        ]
        .into_iter()
        .enumerate()
        {
            assert_eq!(unsafe { seed(0) }, 0);
            let (reference, mut original) = setup(7, true);
            let lost = Arc::new(std::sync::atomic::AtomicBool::new(false));
            let l = lost.clone();
            let guard = move |r: &RouteRef| {
                if l.load(std::sync::atomic::Ordering::SeqCst) {
                    Err(reason)
                } else {
                    original(r)
                }
            };
            let session = start(reference, Duration::from_secs(2), guard).unwrap();
            session
                .ready(Duration::from_secs(2), &Cancel::default())
                .await
                .unwrap();
            let held = session
                .submit(
                    Transition::Batch(vec![
                        Transition::Key {
                            usage: 4,
                            down: true,
                        },
                        Transition::Contact {
                            down: true,
                            x: 1,
                            y: 1,
                        },
                    ]),
                    Duration::from_secs(2),
                    &Cancel::default(),
                )
                .await;
            assert_eq!(held.delivery, Delivery::Sent);
            if mode == 1 {
                unsafe {
                    delay_cleanup();
                }
            }
            if mode == 2 {
                unsafe {
                    cleanup_fault(1);
                }
            }
            lost.store(true, std::sync::atomic::Ordering::SeqCst);
            let refused = session
                .submit(
                    Transition::Key {
                        usage: 4,
                        down: false,
                    },
                    Duration::from_secs(1),
                    &Cancel::default(),
                )
                .await;
            assert_eq!(refused.delivery, Delivery::NotSent);
            assert_eq!(refused.error.unwrap().code, reason);
            tokio::time::timeout(Duration::from_secs(3), async {
                loop {
                    let phase = session.status().phase;
                    if phase == Phase::Closed || phase == Phase::Quarantined {
                        break;
                    }
                    tokio::time::sleep(Duration::from_millis(5)).await;
                }
            })
            .await
            .unwrap();
            if mode == 1 {
                assert_eq!(session.status().phase, Phase::Quarantined);
                unsafe {
                    release();
                }
            }
            lost.store(false, std::sync::atomic::Ordering::SeqCst);
            assert_eq!(
                session
                    .submit(
                        Transition::Key {
                            usage: 4,
                            down: false
                        },
                        Duration::from_secs(1),
                        &Cancel::default()
                    )
                    .await
                    .delivery,
                Delivery::NotSent
            );
            let closed = session.shutdown(Duration::from_secs(3)).await.unwrap();
            assert_eq!(closed.terminal_error.unwrap().code, reason);
            if mode == 2 {
                let error = closed.cleanup_error.unwrap();
                assert_eq!(error.native_code, sys::AOAHID_ERR_STALL);
                assert_eq!(error.request, 57);
            }
            assert_eq!(held.delivery, Delivery::Sent);
            assert_eq!(unsafe { clean() }, 1);
        }
        // Mapping becomes unverified only after normal public initialization.
        // Gesture preflight must fence before any report or desired contact state.
        assert_eq!(unsafe { seed(0) }, 0);
        let (reference, mut original) = setup(7, true);
        let armed = Arc::new(std::sync::atomic::AtomicBool::new(false));
        let trigger = armed.clone();

        let guard = move |r: &RouteRef| {
            let mut binding = original(r)?;
            if trigger.load(std::sync::atomic::Ordering::SeqCst) {
                binding.mapping.verified = false;
            }
            Ok(binding)
        };
        let session = start(reference, Duration::from_secs(2), guard).unwrap();
        session
            .ready(Duration::from_secs(2), &Cancel::default())
            .await
            .unwrap();
        armed.store(true, std::sync::atomic::Ordering::SeqCst);
        let request = Transition::Gesture(crate::gesture::Gesture::Tap {
            at: (1, 2),
            duration: Duration::from_millis(20),
        });
        let refused = session
            .submit(request, Duration::from_secs(1), &Cancel::default())
            .await;
        assert_eq!(refused.delivery, Delivery::NotSent);
        assert_eq!(refused.error.unwrap().code, ErrorCode::MappingUnverified);
        session.shutdown(Duration::from_secs(2)).await.unwrap();
        assert_eq!(unsafe { report_count() }, 0);
        // Explicit private ASCII policy through the actual public Session/native path.
        assert_eq!(unsafe { seed(0) }, 0);
        let (reference, guard) = setup(7, true);
        let session = start(reference, Duration::from_secs(2), guard).unwrap();
        session
            .ready(Duration::from_secs(2), &Cancel::default())
            .await
            .unwrap();
        for payload in ["aé", "a\r", "a\0"] {
            let result = session
                .submit(
                    Transition::Text(crate::ascii::Text::new("us-ansi", payload)),
                    Duration::from_secs(1),
                    &Cancel::default(),
                )
                .await;
            assert_eq!(result.delivery, Delivery::NotSent);
            assert_eq!(unsafe { report_count() }, 0);
        }
        assert_eq!(
            session
                .submit(
                    Transition::Text(crate::ascii::Text::new("unknown", "a")),
                    Duration::from_secs(1),
                    &Cancel::default()
                )
                .await
                .delivery,
            Delivery::NotSent
        );
        assert_eq!(unsafe { report_count() }, 0);
        assert_eq!(
            session
                .submit(
                    Transition::Text(crate::ascii::Text::new("us-ansi", "A!\t\n")),
                    Duration::from_secs(120),
                    &Cancel::default()
                )
                .await
                .delivery,
            Delivery::Sent
        );
        session.shutdown(Duration::from_secs(2)).await.unwrap();
        let ascii_verify: unsafe extern "C" fn() -> i32 =
            unsafe { std::mem::transmute(symbol(c"cg_test_ascii_policy_verify")) };
        assert_eq!(
            unsafe { ascii_verify() },
            1,
            "synthetic modifier/key wire policy and neutral state"
        );
        assert_eq!(unsafe { seed(0) }, 0);
        let (reference, guard) = setup(7, true);
        let session = start(reference, Duration::from_secs(2), guard).unwrap();
        session
            .ready(Duration::from_secs(2), &Cancel::default())
            .await
            .unwrap();
        session
            .submit(
                Transition::Key {
                    usage: 0xe1,
                    down: true,
                },
                Duration::from_secs(1),
                &Cancel::default(),
            )
            .await;
        assert_eq!(
            session
                .submit(
                    Transition::Text(crate::ascii::Text::new("us-ansi", "a")),
                    Duration::from_secs(1),
                    &Cancel::default()
                )
                .await
                .delivery,
            Delivery::NotSent
        );
        assert_eq!(unsafe { report_count() }, 1);
        session
            .submit(
                Transition::Key {
                    usage: 0xe1,
                    down: false,
                },
                Duration::from_secs(1),
                &Cancel::default(),
            )
            .await;
        session.shutdown(Duration::from_secs(2)).await.unwrap();
        assert_eq!(unsafe { clean() }, 1);
        let ascii_fault: unsafe extern "C" fn(i32) =
            unsafe { std::mem::transmute(symbol(c"cg_test_session_submit_fault")) };
        let ascii_release_fault: unsafe extern "C" fn() =
            unsafe { std::mem::transmute(symbol(c"cg_test_gesture_release_fault")) };
        for mode in 0..3 {
            assert_eq!(unsafe { seed(0) }, 0);
            let (reference, mut original) = setup(7, true);
            let cancel = Cancel::default();
            let signal = cancel.clone();
            let armed = Arc::new(std::sync::atomic::AtomicBool::new(false));
            let trigger = armed.clone();
            let mut validations = 0;
            let guard = move |r: &RouteRef| {
                if trigger.load(std::sync::atomic::Ordering::SeqCst) {
                    validations += 1;
                }
                let binding = original(r)?;
                if mode == 2 && validations == 4 {
                    signal.cancel();
                }
                Ok(binding)
            };
            let session = start(reference, Duration::from_secs(2), guard).unwrap();
            session
                .ready(Duration::from_secs(2), &Cancel::default())
                .await
                .unwrap();
            armed.store(true, std::sync::atomic::Ordering::SeqCst);
            if mode == 0 {
                unsafe {
                    ascii_fault(2);
                }
            }
            if mode == 1 {
                unsafe {
                    ascii_release_fault();
                }
            }
            let result = session
                .submit(
                    Transition::Text(crate::ascii::Text::new("us-ansi", "AB")),
                    Duration::from_secs(2),
                    &cancel,
                )
                .await;
            assert_eq!(
                result.delivery,
                if mode == 1 {
                    Delivery::Partial
                } else {
                    Delivery::Unknown
                }
            );
            assert_eq!(
                session
                    .submit(
                        Transition::Text(crate::ascii::Text::new("us-ansi", "AB")),
                        Duration::from_secs(1),
                        &Cancel::default()
                    )
                    .await
                    .delivery,
                Delivery::NotSent
            );
            let closed = session.shutdown(Duration::from_secs(3)).await.unwrap();
            if mode == 1 {
                assert!(closed.cleanup_error.is_some());
            }
            assert_eq!(unsafe { clean() }, 1);
            assert!(
                unsafe { report_count() } <= 3,
                "failed shifted text must not repeat its prefix"
            );
        }
        let gesture_fault: unsafe extern "C" fn(i32) =
            unsafe { std::mem::transmute(symbol(c"cg_test_session_submit_fault")) };
        let release_fault: unsafe extern "C" fn() =
            unsafe { std::mem::transmute(symbol(c"cg_test_gesture_release_fault")) };
        for mode in 0..5 {
            use crate::gesture::Gesture;
            assert_eq!(unsafe { seed(0) }, 0);
            let (reference, guard) = setup(7, true);
            let session = start(reference, Duration::from_secs(2), guard).unwrap();
            session
                .ready(Duration::from_secs(2), &Cancel::default())
                .await
                .unwrap();
            let invalid = Gesture::Swipe {
                from: (1, 2),
                to: (65535, 3),
                duration: Duration::from_millis(20),
            };
            assert_eq!(
                session
                    .submit(
                        Transition::Gesture(invalid),
                        Duration::from_secs(1),
                        &Cancel::default()
                    )
                    .await
                    .delivery,
                Delivery::NotSent
            );
            assert_eq!(
                unsafe { report_count() },
                0,
                "all gesture coordinates preflight before reports"
            );
            if mode == 3 {
                unsafe {
                    gesture_fault(2);
                }
            }
            if mode == 4 {
                unsafe {
                    release_fault();
                }
            }
            let gesture = match mode {
                1 => Gesture::Hold {
                    at: (1, 2),
                    duration: Duration::from_millis(20),
                },
                2 => Gesture::Swipe {
                    from: (1, 2),
                    to: (200, 300),
                    duration: Duration::from_millis(40),
                },
                _ => Gesture::Tap {
                    at: (1, 2),
                    duration: Duration::from_millis(20),
                },
            };
            let result = session
                .submit(
                    Transition::Gesture(gesture.clone()),
                    Duration::from_secs(2),
                    &Cancel::default(),
                )
                .await;
            assert_eq!(
                result.delivery,
                match mode {
                    3 => Delivery::Unknown,
                    4 => Delivery::Partial,
                    _ => Delivery::Sent,
                }
            );
            if mode < 3 {
                assert_eq!(unsafe { report_count() }, if mode == 2 { 18 } else { 2 });
            } else {
                assert_eq!(
                    session
                        .submit(
                            Transition::Gesture(gesture),
                            Duration::from_secs(1),
                            &Cancel::default()
                        )
                        .await
                        .delivery,
                    Delivery::NotSent
                );
            }
            let closed = session.shutdown(Duration::from_secs(3)).await.unwrap();
            if mode == 4 {
                assert!(closed.cleanup_error.is_some());
            }
            assert_eq!(
                unsafe { clean() },
                1,
                "gesture cleanup drains all fake handles"
            );
        }
        let fault: unsafe extern "C" fn(i32) =
            unsafe { std::mem::transmute(symbol(c"cg_test_session_submit_fault")) };
        let reports_verify: unsafe extern "C" fn() -> i32 =
            unsafe { std::mem::transmute(symbol(c"cg_test_session_reports_verify")) };
        for mode in 0..4 {
            assert_eq!(unsafe { seed(0) }, 0);
            let (reference, guard) = setup(7, true);
            let session = start(reference, Duration::from_secs(2), guard).unwrap();
            session
                .ready(Duration::from_secs(2), &Cancel::default())
                .await
                .unwrap();
            unsafe {
                fault(mode);
            }
            let result = session
                .submit(
                    Transition::Key {
                        usage: 4,
                        down: true,
                    },
                    Duration::from_secs(2),
                    &Cancel::default(),
                )
                .await;
            assert_eq!(
                result.delivery,
                match mode {
                    0 => Delivery::Sent,
                    1 => Delivery::NotSent,
                    _ => Delivery::Unknown,
                }
            );
            if mode != 0 {
                assert!(result.error.is_some());
                assert_eq!(
                    session
                        .submit(
                            Transition::Key {
                                usage: 4,
                                down: true
                            },
                            Duration::from_secs(1),
                            &Cancel::default()
                        )
                        .await
                        .delivery,
                    Delivery::NotSent
                );
            }
            session.shutdown(Duration::from_secs(3)).await.unwrap();
            assert_eq!(
                unsafe { reports_verify() },
                1,
                "no nonneutral retry or fake handle leak"
            );
        }
        let count_reports: unsafe extern "C" fn() -> usize =
            unsafe { std::mem::transmute(symbol(c"cg_test_session_report_count")) };
        for mode in 0..5 {
            assert_eq!(unsafe { seed(0) }, 0);
            let (reference, mut original) = setup(7, true);
            let (entered, receive) = std::sync::mpsc::channel();
            let (release_guard, wait) = std::sync::mpsc::channel();
            let mut calls = 0;
            let guard = move |r: &RouteRef| {
                calls += 1;
                if calls == 7 {
                    entered.send(()).unwrap();
                    wait.recv_timeout(Duration::from_secs(2)).unwrap();
                    if mode == 4 {
                        return Err(ErrorCode::LeaseInvalid);
                    }
                }
                original(r)
            };
            let session = start_with_registration(
                reference,
                if mode == 3 {
                    Duration::from_millis(40)
                } else {
                    Duration::from_secs(2)
                },
                crate::session::Registration {
                    settling: Duration::from_millis(150),
                },
                guard,
            )
            .unwrap();
            receive.recv_timeout(Duration::from_secs(2)).unwrap();
            assert_eq!(session.status().phase, Phase::Settling);
            assert!(session.status().resources_acquired);
            assert!(!session.status().settling_elapsed);
            assert!(!session.status().hardware_verified);
            assert_eq!(
                session
                    .submit(
                        Transition::Key {
                            usage: 4,
                            down: true
                        },
                        Duration::from_secs(1),
                        &Cancel::default()
                    )
                    .await
                    .delivery,
                Delivery::NotSent
            );
            assert_eq!(unsafe { count_reports() }, 0);
            if mode == 1 {
                let c = Cancel::default();
                c.cancel();
                assert_eq!(
                    session.ready(Duration::from_secs(1), &c).await,
                    Err(ErrorCode::ProcessCancelled)
                );
            }
            if mode == 2 {
                assert_eq!(
                    session.shutdown(Duration::from_millis(10)).await,
                    Err(ErrorCode::NativeCleanupPending)
                );
            }
            if mode == 3 {
                std::thread::sleep(Duration::from_millis(60));
            }
            if mode == 4 {
                unsafe {
                    fail();
                }
            }
            release_guard.send(()).unwrap();
            if mode == 0 {
                session
                    .ready(Duration::from_secs(1), &Cancel::default())
                    .await
                    .unwrap();
                assert!(session.status().settling_elapsed);
            }
            if mode == 3 {
                assert_eq!(
                    session
                        .ready(Duration::from_secs(1), &Cancel::default())
                        .await,
                    Err(ErrorCode::ProcessTimeout)
                );
            }
            if mode == 4 {
                assert_eq!(
                    session
                        .ready(Duration::from_secs(1), &Cancel::default())
                        .await,
                    Err(ErrorCode::LeaseInvalid)
                );
            }
            let closed = session.shutdown(Duration::from_secs(2)).await.unwrap();
            if mode != 0 {
                assert!(!closed.settling_elapsed);
                assert!(
                    session
                        .ready(Duration::from_secs(1), &Cancel::default())
                        .await
                        .is_err()
                );
            }
            if mode == 4 {
                assert!(closed.cleanup_error.is_some());
                assert_eq!(closed.terminal_error.unwrap().code, ErrorCode::LeaseInvalid);
            }
            assert_eq!(unsafe { count_reports() }, 0);
            assert_eq!(
                unsafe { verify(0) },
                1,
                "registration settling has no hidden input or leaked fake handles"
            );
        }
        // Success, consumed unregister failure, and missing physical address at
        // the same topology. Every case goes through public native start/Session.
        for mode in 0..4 {
            assert_eq!(unsafe { seed(0) }, 0);
            let (reference, guard) = setup(if mode == 2 { 8 } else { 7 }, mode != 3);
            let session = start(reference, Duration::from_secs(2), guard).unwrap();
            let initialized = session
                .ready(Duration::from_secs(2), &Cancel::default())
                .await;
            if mode == 2 {
                assert_eq!(initialized, Err(ErrorCode::DeviceDisconnected));
            } else if mode == 3 {
                assert_eq!(initialized, Err(ErrorCode::MappingUnverified));
            } else {
                initialized.unwrap();
            }
            if mode == 1 {
                unsafe {
                    fail();
                }
            }
            let closed = session.shutdown(Duration::from_secs(3)).await.unwrap();
            assert_eq!(closed.phase, Phase::Closed);
            if mode == 1 {
                let e = closed.cleanup_error.unwrap();
                assert_eq!(e.native_code, sys::AOAHID_ERR_STALL);
                assert_eq!(e.request, 55);
                assert_ne!(e.transport_status, 0);
            } else {
                assert_eq!(closed.cleanup_error, None);
            }
            assert!(
                session
                    .ready(Duration::from_secs(1), &Cancel::default())
                    .await
                    .is_err()
            );
            assert_eq!(
                session.shutdown(Duration::from_secs(1)).await.unwrap(),
                closed
            );
            assert_eq!(unsafe { verify(if mode >= 2 { 2 } else { 0 }) }, 1);
        }
        // Execution-time authority changes and a validation callback consumed
        // after the initialization deadline must stop before foreign effects.
        for mode in 0..3 {
            assert_eq!(unsafe { seed(0) }, 0);
            let (reference, mut original) = setup(7, true);
            let mut calls = 0;
            let guard = move |r: &RouteRef| {
                calls += 1;
                if mode == 0 && calls == 2 {
                    return Err(ErrorCode::LeaseInvalid);
                }
                let mut binding = original(r)?;
                if mode == 1 && calls == 3 {
                    binding.mapping.max_x -= 1;
                }
                if mode == 2 {
                    std::thread::sleep(Duration::from_millis(120));
                }
                Ok(binding)
            };
            let session = start(
                reference,
                if mode == 2 {
                    Duration::from_millis(60)
                } else {
                    Duration::from_secs(2)
                },
                guard,
            )
            .unwrap();
            assert_eq!(
                session
                    .ready(Duration::from_secs(2), &Cancel::default())
                    .await,
                Err(match mode {
                    0 => ErrorCode::LeaseInvalid,
                    1 => ErrorCode::GeometryChanged,
                    _ => ErrorCode::ProcessTimeout,
                })
            );
            session.shutdown(Duration::from_secs(2)).await.unwrap();
            assert_eq!(
                unsafe { verify(2) },
                1,
                "expired/changed authority cannot register nodes"
            );
        }
        assert_eq!(unsafe { seed(0) }, 0);
        let (reference, guard) = setup(7, true);
        let session = start(reference, Duration::from_secs(2), guard).unwrap();
        session
            .ready(Duration::from_secs(2), &Cancel::default())
            .await
            .unwrap();
        assert_eq!(
            unsafe { count_reports() },
            0,
            "no report before explicit post-settling submission"
        );
        let delay: unsafe extern "C" fn() =
            unsafe { std::mem::transmute(symbol(c"cg_test_session_delay_callback")) };
        unsafe {
            delay();
        }
        assert_eq!(
            session
                .submit(
                    Transition::Key {
                        usage: 4,
                        down: true
                    },
                    Duration::from_millis(60),
                    &Cancel::default()
                )
                .await
                .delivery,
            Delivery::Unknown
        );
        assert_eq!(
            session.shutdown(Duration::from_millis(30)).await,
            Err(ErrorCode::NativeCleanupPending)
        );
        // Wait until the real foreign close has returned PENDING, not merely
        // until the caller's shorter timeout expires while the close runs.
        tokio::time::timeout(Duration::from_secs(2), async {
            while session.status().phase != Phase::Quarantined {
                tokio::time::sleep(Duration::from_millis(5)).await;
            }
        })
        .await
        .unwrap();
        unsafe {
            release();
        }
        let closed = session.shutdown(Duration::from_secs(3)).await.unwrap();
        assert_eq!(closed.phase, Phase::Closed);
        // Native wait timeout may become the consuming close's honest error.
        if let Some(e) = closed.cleanup_error {
            assert_eq!(e.code, ErrorCode::ProcessTimeout);
        }
        assert_eq!(
            unsafe { verify(1) },
            1,
            "callback drained, neutral report, reverse unregister, no claims/switch/leaks"
        );
    }
}
