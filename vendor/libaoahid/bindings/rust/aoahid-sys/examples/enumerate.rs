// SPDX-License-Identifier: MIT
// Copyright (c) 2026 libaoahid contributors
//! Demonstrates literal Rust FFI discovery without supplying native options.

use aoahid_sys::*;
use core::ptr;
use std::ffi::CStr;

fn main() {
    let mut arguments = std::env::args().skip(1);
    let timeout_ms = arguments
        .next()
        .and_then(|value| value.parse::<u32>().ok())
        .filter(|value| *value != 0);
    if arguments.next().is_some() || timeout_ms.is_none() {
        eprintln!("usage: enumerate CONTROL_TIMEOUT_MS");
        std::process::exit(2);
    }
    let timeout_ms = timeout_ms.expect("validated above");
    let options = aoahid_context_options {
        struct_size: u32::try_from(std::mem::size_of::<aoahid_context_options>())
            .expect("context option structure fits its ABI field"),
        reserved: 0,
        event_mode: AOAHID_EVENT_CALLER_POLL,
        log_level: AOAHID_LOG_DISABLED,
        log_sink: None,
        log_user: ptr::null_mut(),
    };
    let mut context = ptr::null_mut();
    let create = unsafe { aoahid_context_create(&options, &mut context) };
    if create != AOAHID_OK {
        std::process::exit(create);
    }
    let mut discovery = ptr::null_mut();
    let mut discover = unsafe { aoahid_discover(context, timeout_ms, &mut discovery) };
    if discover == AOAHID_OK {
        let count = unsafe { aoahid_discovery_count(discovery) };
        if count == 0 {
            let detail = unsafe { aoahid_last_error() };
            if detail.is_null() {
                discover = AOAHID_ERR_INTERNAL;
            } else {
                let count_status = unsafe { (*detail).code };
                if count_status != AOAHID_OK {
                    discover = count_status;
                }
            }
        }
        for index in 0..count {
            let pointer = unsafe { aoahid_discovery_get(discovery, index) };
            if pointer.is_null() {
                let detail = unsafe { aoahid_last_error() };
                discover = if detail.is_null() {
                    AOAHID_ERR_INTERNAL
                } else {
                    unsafe { (*detail).code }
                };
                break;
            }
            let device = unsafe { &*pointer };
            let product = if device.product.is_null() {
                ""
            } else {
                unsafe { CStr::from_ptr(device.product) }
                    .to_str()
                    .unwrap_or("")
            };
            let serial = if device.serial.is_null() {
                ""
            } else {
                unsafe { CStr::from_ptr(device.serial) }
                    .to_str()
                    .unwrap_or("")
            };
            println!(
                "{:04x}:{:04x} {product} {serial}",
                device.vendor_id, device.product_id
            );
        }
        unsafe { aoahid_discovery_destroy(discovery) };
    }
    let close =
        unsafe { aoahid_context_destroy_blocking(context, timeout_ms, timeout_ms, timeout_ms) };
    std::process::exit(if discover == AOAHID_OK {
        close
    } else {
        discover
    });
}
