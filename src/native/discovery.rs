//! Pinned C ABI discovery adapter; no Device/Node handles, interface claims or HID.
use crate::{
    api::ErrorCode,
    backend::Cancel,
    discovery::{
        CloseStatus, DiscoveryDriver, DiscoveryKind, DiscoveryWorker, MAX_CANDIDATES, UsbDevice,
    },
};
use aoahid_sys as sys;
use std::{
    ffi::CStr,
    marker::PhantomData,
    ptr::NonNull,
    rc::Rc,
    time::{Duration, Instant},
};

/// This factory is invoked only on the serialized owner thread.
pub fn worker() -> Result<DiscoveryWorker, ErrorCode> {
    DiscoveryWorker::start(NativeDiscovery::create)
}
struct NativeDiscovery {
    context: Option<NonNull<sys::aoahid_context>>,
    kind: DiscoveryKind,
    _owner: PhantomData<Rc<()>>,
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
fn native_error(status: i32) -> ErrorCode {
    // Copy thread-local scalar detail immediately. Do not export raw diagnostic strings.
    let detail = unsafe {
        sys::aoahid_last_error()
            .as_ref()
            .map(|d| (d.code, d.transport_status, d.aoa_request))
    };
    code(detail.map_or(status, |d| if d.0 == sys::AOAHID_OK { status } else { d.0 }))
}
impl NativeDiscovery {
    fn create() -> Result<Self, ErrorCode> {
        // Pinned upstream warns LIBUSB_DEBUG can override context-local logging.
        // Refuse forced logging before creating a Context; never mutate process env.
        if std::env::var_os("LIBUSB_DEBUG").is_some_and(|v| v != "0") {
            return Err(ErrorCode::BackendUnavailable);
        }
        if !super::verify_abi() {
            return Err(ErrorCode::BackendUnavailable);
        }
        let mut options = super::context_options();
        options.event_mode = sys::AOAHID_EVENT_INTERNAL_THREAD;
        options.log_level = sys::AOAHID_LOG_DISABLED;
        let mut raw = std::ptr::null_mut();
        let status = unsafe { sys::aoahid_context_create(&options, &mut raw) };
        if status != sys::AOAHID_OK {
            return Err(native_error(status));
        }
        Ok(Self {
            context: Some(NonNull::new(raw).ok_or(ErrorCode::BackendUnavailable)?),
            _owner: PhantomData,
            kind: DiscoveryKind::Native,
        })
    }
}
struct Snapshot(NonNull<sys::aoahid_discovery>);
impl Drop for Snapshot {
    fn drop(&mut self) {
        unsafe {
            sys::aoahid_discovery_destroy(self.0.as_ptr());
        }
    }
}
unsafe fn string(raw: *const std::ffi::c_char) -> Result<Option<String>, ErrorCode> {
    if raw.is_null() {
        return Ok(None);
    }
    // The pinned ABI owns valid NUL-terminated strings until snapshot destruction.
    let s = unsafe { CStr::from_ptr(raw) }
        .to_str()
        .map_err(|_| ErrorCode::DeviceIdentityAmbiguous)?;
    if s.len() > 256 {
        return Err(ErrorCode::OutputLimit);
    }
    Ok(Some(s.to_owned()))
}
unsafe fn copy_device(raw: &sys::aoahid_device_info) -> Result<UsbDevice, ErrorCode> {
    if raw.port_path_length == 0 || raw.port_path_length > 7 || raw.port_path.is_null() {
        return Err(ErrorCode::DeviceIdentityAmbiguous);
    }
    let device = UsbDevice {
        bus: raw.bus_number,
        address: raw.device_address,
        ports: unsafe { std::slice::from_raw_parts(raw.port_path, raw.port_path_length) }.to_vec(),
        vendor_id: raw.vendor_id,
        product_id: raw.product_id,
        aoa_protocol: raw.protocol_version,
        serial: unsafe { string(raw.serial)? },
        product: unsafe { string(raw.product)? },
    };
    device.topology()?;
    Ok(device)
}
impl DiscoveryDriver for NativeDiscovery {
    fn kind(&self) -> DiscoveryKind {
        self.kind
    }
    fn scan(&mut self, deadline: Instant, cancel: &Cancel) -> Result<Vec<UsbDevice>, ErrorCode> {
        if cancel.cancelled() {
            return Err(ErrorCode::ProcessCancelled);
        }
        let remaining = deadline
            .checked_duration_since(Instant::now())
            .ok_or(ErrorCode::ProcessTimeout)?;
        let timeout = remaining.as_millis().min(500) as u32;
        if timeout == 0 {
            return Err(ErrorCode::ProcessTimeout);
        }
        let context = self.context.ok_or(ErrorCode::BackendUnavailable)?;
        let mut raw = std::ptr::null_mut();
        // Upstream probes every USB device; this timeout is PER DEVICE, not global.
        let status = unsafe { sys::aoahid_discover(context.as_ptr(), timeout, &mut raw) };
        if status != sys::AOAHID_OK {
            return Err(native_error(status));
        }
        let snapshot = Snapshot(NonNull::new(raw).ok_or(ErrorCode::BackendUnavailable)?);
        let count = unsafe { sys::aoahid_discovery_count(snapshot.0.as_ptr()) };
        if count > MAX_CANDIDATES {
            return Err(ErrorCode::OutputLimit);
        }
        let mut devices = Vec::with_capacity(count);
        for index in 0..count {
            if cancel.cancelled() || Instant::now() >= deadline {
                return Err(ErrorCode::ProcessTimeout);
            }
            let raw = unsafe { sys::aoahid_discovery_get(snapshot.0.as_ptr(), index).as_ref() }
                .ok_or(ErrorCode::BackendUnavailable)?;
            devices.push(unsafe { copy_device(raw)? });
        }
        Ok(devices) // snapshot RAII releases all native strings after owned copies
    }
    fn close(&mut self) -> CloseStatus {
        let Some(context) = self.context else {
            return CloseStatus::Consumed(Err(ErrorCode::BackendUnavailable));
        };
        let status =
            unsafe { sys::aoahid_context_destroy_blocking(context.as_ptr(), 500, 1000, 1000) };
        if status == sys::AOAHID_CLOSE_PENDING {
            return CloseStatus::Pending;
        }
        let result = if status == sys::AOAHID_OK {
            Ok(())
        } else {
            Err(native_error(status))
        };
        self.context = None; // every non-PENDING result consumes the handle
        CloseStatus::Consumed(result)
    }
}
impl Drop for NativeDiscovery {
    fn drop(&mut self) {
        // Last resort on this same owner thread; never free a pending Context.
        while self.context.is_some() {
            if self.close() == CloseStatus::Pending {
                std::thread::sleep(Duration::from_millis(25));
            }
        }
    }
}
#[test]
fn copies_owned_identity_before_source_storage_is_destroyed() {
    let device = {
        let serial = std::ffi::CString::new("SYNTHETIC_SERIAL").unwrap();
        let product = std::ffi::CString::new("SYNTHETIC_PRODUCT").unwrap();
        let ports = [2, 3];
        let raw = sys::aoahid_device_info {
            bus_number: 1,
            device_address: 7,
            port_path: ports.as_ptr(),
            port_path_length: ports.len(),
            vendor_id: 1,
            product_id: 2,
            protocol_version: 2,
            serial: serial.as_ptr(),
            product: product.as_ptr(),
        };
        unsafe { copy_device(&raw) }.unwrap()
    };
    assert_eq!(device.serial.as_deref(), Some("SYNTHETIC_SERIAL"));
    assert_eq!(device.product.as_deref(), Some("SYNTHETIC_PRODUCT"));
    assert_eq!(device.topology().unwrap(), "1-2.3");
}

/// Explicitly ignored in normal real-library tests. check-native.sh runs this
/// in a separate process loaded with the test-only marked fake-libusb library.
#[cfg(all(test, target_os = "linux"))]
#[tokio::test]
#[ignore = "requires isolated marked SOFTWARE FAKE libusb ABI library"]
async fn software_fake_native_abi_discovery() {
    // Resolve the marker BEFORE creating any Context. With the real library
    // loaded this test fails here and cannot probe a physical USB device.
    let seed = unsafe { libc::dlsym(libc::RTLD_DEFAULT, c"cg_test_discovery_seed".as_ptr()) };
    let verify = unsafe { libc::dlsym(libc::RTLD_DEFAULT, c"cg_test_discovery_verify".as_ptr()) };
    assert!(
        !seed.is_null() && !verify.is_null(),
        "SOFTWARE FAKE ABI marker missing"
    );
    let seed: unsafe extern "C" fn(i32) -> i32 = unsafe { std::mem::transmute(seed) };
    let verify: unsafe extern "C" fn(i32) -> i32 = unsafe { std::mem::transmute(verify) };
    if std::env::var_os("LIBUSB_DEBUG").is_some_and(|v| v != "0") {
        assert_eq!(unsafe { seed(0) }, 0);
        let owner = worker().unwrap(); // actual public native factory
        assert!(matches!(
            owner
                .snapshot(Duration::from_secs(1), &Cancel::default())
                .await,
            Err(ErrorCode::BackendUnavailable)
        ));
        assert_eq!(
            owner.shutdown(Duration::from_secs(1)).await,
            Err(ErrorCode::BackendUnavailable)
        );
        assert_eq!(
            unsafe { verify(2) },
            1,
            "forced debug must produce no probe/handles"
        );
        return;
    }
    for mode in 0..3 {
        assert_eq!(unsafe { seed(mode) }, 0);
        let worker = DiscoveryWorker::start(|| {
            let mut driver = NativeDiscovery::create()?;
            driver.kind = DiscoveryKind::SoftwareFake; // evidence label only, not handle-state injection
            Ok(driver)
        })
        .unwrap();
        let result = worker
            .snapshot(Duration::from_secs(2), &Cancel::default())
            .await;
        worker.shutdown(Duration::from_secs(2)).await.unwrap();
        match mode {
            0 => {
                let snapshot = result.unwrap();
                assert_eq!(snapshot.kind, DiscoveryKind::SoftwareFake);
                assert_eq!(snapshot.devices.len(), 1);
                assert_eq!(
                    snapshot.devices[0].serial.as_deref(),
                    Some("SYNTHETIC_NATIVE_SERIAL")
                );
                assert_eq!(snapshot.devices[0].topology().unwrap(), "1-2.3");
            }
            1 => assert!(matches!(result, Err(ErrorCode::DeviceIdentityAmbiguous))),
            _ => assert!(result.unwrap().devices.is_empty()), // upstream skips inaccessible candidates
        }
        assert_eq!(
            unsafe { verify(mode) },
            1,
            "no input, claims, mode changes or leaked fake handles"
        );
    }
}
