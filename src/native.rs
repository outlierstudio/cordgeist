//! Native ABI initializers, owned discovery and lifecycle-only Session.
use aoahid_sys as sys;
unsafe extern "C" {
    fn cg_error_detail_size() -> usize;
    fn cg_error_detail_align() -> usize;
    fn cg_device_info_size() -> usize;
    fn cg_device_info_align() -> usize;
    fn cg_context_init(p: *mut sys::aoahid_context_options);
    fn cg_context_size() -> usize;
    fn cg_context_align() -> usize;
    fn cg_device_init(p: *mut sys::aoahid_device_options);
    fn cg_device_size() -> usize;
    fn cg_device_align() -> usize;
    fn cg_node_init(p: *mut sys::aoahid_node_options);
    fn cg_node_size() -> usize;
    fn cg_node_align() -> usize;
    fn cg_keyboard_init(p: *mut sys::aoahid_keyboard_options);
    fn cg_keyboard_size() -> usize;
    fn cg_keyboard_align() -> usize;
    fn cg_touchscreen_init(p: *mut sys::aoahid_touchscreen_options);
    fn cg_touchscreen_size() -> usize;
    fn cg_touchscreen_align() -> usize;
}
pub fn verify_abi() -> bool {
    unsafe {
        sys::aoahid_version() == (2 << 24 | 3)
            && cg_error_detail_size() == std::mem::size_of::<sys::aoahid_error_detail>()
            && cg_error_detail_align() == std::mem::align_of::<sys::aoahid_error_detail>()
            && cg_device_info_size() == std::mem::size_of::<sys::aoahid_device_info>()
            && cg_device_info_align() == std::mem::align_of::<sys::aoahid_device_info>()
            && cg_context_size() == std::mem::size_of::<sys::aoahid_context_options>()
            && cg_context_align() == std::mem::align_of::<sys::aoahid_context_options>()
            && cg_device_size() == std::mem::size_of::<sys::aoahid_device_options>()
            && cg_device_align() == std::mem::align_of::<sys::aoahid_device_options>()
            && cg_node_size() == std::mem::size_of::<sys::aoahid_node_options>()
            && cg_node_align() == std::mem::align_of::<sys::aoahid_node_options>()
            && cg_keyboard_size() == std::mem::size_of::<sys::aoahid_keyboard_options>()
            && cg_keyboard_align() == std::mem::align_of::<sys::aoahid_keyboard_options>()
            && cg_touchscreen_size() == std::mem::size_of::<sys::aoahid_touchscreen_options>()
            && cg_touchscreen_align() == std::mem::align_of::<sys::aoahid_touchscreen_options>()
    }
}
pub fn context_options() -> sys::aoahid_context_options {
    let mut p = std::mem::MaybeUninit::uninit();
    unsafe {
        cg_context_init(p.as_mut_ptr());
        p.assume_init()
    }
}
pub fn device_options() -> sys::aoahid_device_options {
    let mut p = std::mem::MaybeUninit::uninit();
    unsafe {
        cg_device_init(p.as_mut_ptr());
        p.assume_init()
    }
}
pub fn node_options() -> sys::aoahid_node_options {
    let mut p = std::mem::MaybeUninit::uninit();
    unsafe {
        cg_node_init(p.as_mut_ptr());
        p.assume_init()
    }
}
pub fn keyboard_options() -> sys::aoahid_keyboard_options {
    let mut p = std::mem::MaybeUninit::uninit();
    unsafe {
        cg_keyboard_init(p.as_mut_ptr());
        p.assume_init()
    }
}
pub fn touchscreen_options() -> sys::aoahid_touchscreen_options {
    let mut p = std::mem::MaybeUninit::uninit();
    unsafe {
        cg_touchscreen_init(p.as_mut_ptr());
        p.assume_init()
    }
}
#[test]
fn header_runtime_and_initializers_match() {
    assert!(verify_abi());
    assert_eq!(
        context_options().struct_size as usize,
        std::mem::size_of::<sys::aoahid_context_options>()
    );
    assert_eq!(
        device_options().struct_size as usize,
        std::mem::size_of::<sys::aoahid_device_options>()
    );
    assert_eq!(
        node_options().struct_size as usize,
        std::mem::size_of::<sys::aoahid_node_options>()
    );
    assert_eq!(
        keyboard_options().struct_size as usize,
        std::mem::size_of::<sys::aoahid_keyboard_options>()
    );
    assert_eq!(
        touchscreen_options().struct_size as usize,
        std::mem::size_of::<sys::aoahid_touchscreen_options>()
    );
}

pub mod discovery;

pub mod session;

pub fn single_contact_options(max_x: u16, max_y: u16) -> sys::aoahid_touchscreen_options {
    let mut options = touchscreen_options();
    options.maximum_contacts = 1;
    options.contacts_per_report = 1;
    options.x.logical_maximum = i32::from(max_x);
    options.y.logical_maximum = i32::from(max_y);
    options
}
/// Specs are created, inspected and released on this calling owner thread.
/// Only a digest leaves; no native handle crosses a thread boundary.
pub fn descriptor_identity(max_x: u16, max_y: u16) -> Result<String, crate::api::ErrorCode> {
    use crate::api::ErrorCode;
    let touch = aoahid::TouchscreenSpec::create(&single_contact_options(max_x, max_y))
        .map_err(|_| ErrorCode::MappingUnverified)?;
    let keyboard = aoahid::KeyboardSpec::create(&keyboard_options())
        .map_err(|_| ErrorCode::MappingUnverified)?;
    let mut hash = blake3::Hasher::new();
    for spec in [touch.as_ptr(), keyboard.as_ptr()] {
        let mut bytes = std::ptr::null();
        let mut len = 0usize;
        // The pinned binding const-qualifies the output slot. C fills it;
        // pass a raw mutable pointer, never a shared Rust reference.
        let status = unsafe { sys::aoahid_spec_descriptor(spec, &raw mut bytes, &mut len) };
        if status != sys::AOAHID_OK || bytes.is_null() || len == 0 || len > 8192 {
            return Err(ErrorCode::MappingUnverified);
        }
        hash.update(&(len as u64).to_be_bytes());
        hash.update(unsafe { std::slice::from_raw_parts(bytes, len) });
    }
    Ok(format!("aoa2-{}", hash.finalize().to_hex()))
}

#[test]
fn descriptor_identity_tracks_generated_axis_settings() {
    let original = descriptor_identity(32767, 32767).unwrap();
    assert_eq!(original, descriptor_identity(32767, 32767).unwrap());
    assert_ne!(original, descriptor_identity(4095, 32767).unwrap());
    assert_ne!(original, descriptor_identity(32767, 4095).unwrap());
}
