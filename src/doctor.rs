//! Nine bounded read-only checks. No daemon start, device activation, lease,
//! HID registration, app launch, key event, tap or persisted screen content.
use crate::{
    adb::{Adb, Attachment, Authorization, Target},
    api::*,
};
use serde::Serialize;
use std::{path::PathBuf, time::Duration};
use tokio_util::sync::CancellationToken;
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Check {
    pub number: u8,
    pub status: &'static str,
    pub code: &'static str,
    pub detail: String,
    pub fix: &'static str,
}
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Report {
    pub version: &'static str,
    pub ready: bool,
    pub backend: &'static str,
    pub hardware_verified: bool,
    pub daemon_implemented: bool,
    pub adb_detected: bool,
    pub checks: Vec<Check>,
}
fn check(
    number: u8,
    status: &'static str,
    code: &'static str,
    detail: impl Into<String>,
    fix: &'static str,
) -> Check {
    Check {
        number,
        status,
        code,
        detail: detail.into(),
        fix,
    }
}
fn skipped(number: u8, code: &'static str) -> Check {
    check(
        number,
        "fail",
        code,
        "Not checked: exact authorized phone unavailable",
        "Connect only one phone, unlock it, enable USB debugging and tap Allow; rerun doctor.",
    )
}
pub async fn collect(path: Option<PathBuf>, port: u16) -> Report {
    let mut checks = vec![
        skipped(1, "adb-unavailable"),
        skipped(2, "device-disconnected"),
        skipped(3, "usb-unavailable"),
        skipped(4, "aoa-unavailable"),
        skipped(5, "android-unavailable"),
        skipped(6, "wm-size-unavailable"),
        skipped(7, "rotation-unavailable"),
        skipped(8, "screenshot-unavailable"),
        skipped(9, "elements-unavailable"),
    ];
    let adb = path.and_then(|p| Adb::new(p, port).ok());
    let mut selected = None;
    let mut adb_detected = false;
    if let Some(adb) = &adb {
        checks[0] = check(
            1,
            "fail",
            "adb-version-failed",
            "Installed adb did not return a valid version",
            "Select a working Google platform-tools executable with --adb.",
        );
        checks[1] = check(
            2,
            "fail",
            "adb-discovery-failed",
            "ADB inventory failed",
            "Check the loopback ADB server and USB connection; do not kill the global server.",
        );
        if let Ok(output) = adb.version(CancellationToken::new()).await {
            // Print only the version line, never "Installed as" with private paths.
            if let Ok(text) = std::str::from_utf8(&output.stdout)
                && let Some(version) = text
                    .lines()
                    .find(|s| s.starts_with("Android Debug Bridge version "))
                    .filter(|s| s.len() < 128 && !s.chars().any(char::is_control))
            {
                adb_detected = true;
                checks[0] = check(1, "pass", "adb-version", version, "Installed adb found.");
            }
        }
        if let Ok(items) = adb
            .inventory(Duration::from_secs(3), CancellationToken::new())
            .await
            .and_then(|o| crate::adb::parse_inventory(&o.stdout))
        {
            checks[1] = match items.as_slice() {
                [item]
                    if item.state == Authorization::Authorized
                        && item.transport_id.is_some()
                        && item.usb.is_some() =>
                {
                    selected = Some(item.clone());
                    check(
                        2,
                        "pass",
                        "adb-authorized",
                        "One directly attached authorized phone",
                        "Keep the phone unlocked.",
                    )
                }
                [item] if item.state == Authorization::Unauthorized => check(
                    2,
                    "fail",
                    "adb-unauthorized",
                    "Host key is not authorized",
                    "Unlock the phone and tap Allow for USB debugging.",
                ),
                [] => skipped(2, "device-disconnected"),
                [_] => check(
                    2,
                    "fail",
                    "adb-offline-or-unroutable",
                    "Phone offline or USB transport identity absent",
                    "Reconnect USB, unlock and authorize debugging; use a direct USB connection.",
                ),
                _ => check(
                    2,
                    "fail",
                    "device-identity-ambiguous",
                    "More than one ADB device",
                    "Disconnect other devices; v1 requires exactly one phone.",
                ),
            };
        }
    } else {
        checks[0] = check(
            1,
            "fail",
            "adb-unavailable",
            "Installed adb not found",
            "Install Google platform-tools; put adb on PATH or pass --adb /absolute/path/to/adb.",
        );
    }
    let (version, protocol) = usb_check(selected.as_ref()).await;
    checks[2] = check(
        3,
        if protocol.is_some() { "pass" } else { "fail" },
        "libusb-usb",
        version,
        "For USB permission errors, explicitly install the packaged scoped udev rule and reconnect. No sudo or driver changes run automatically.",
    );
    checks[3] = match protocol {
        Some(p) if p >= 2 => check(
            4,
            "pass",
            "aoa-protocol",
            format!("AOA protocol {p}"),
            "AOA 2 HID protocol available; this is not proof of delivered input.",
        ),
        Some(p) => check(
            4,
            "fail",
            "aoa-unsupported",
            format!("AOA protocol {p}"),
            "This phone does not expose AOA 2 HID; unsupported.",
        ),
        None => check(
            4,
            "fail",
            "aoa-unavailable",
            "Protocol could not be established for the exact phone",
            "Check USB permissions and direct attachment; no protocol-support claim is made.",
        ),
    };
    if let (Some(adb), Some(phone)) = (&adb, &selected) {
        for (number, code, detail, fix) in [
            (
                5,
                "android-unavailable",
                "Android properties could not be read",
                "Unlock and authorize USB debugging; rerun doctor.",
            ),
            (
                6,
                "wm-size-unavailable",
                "Effective wm size is missing or invalid",
                "Valid default-display geometry is required; do not guess sizes.",
            ),
            (
                7,
                "rotation-unavailable",
                "Display rotation metadata is missing or ambiguous",
                "Use the primary personal-user display and include this report in an issue.",
            ),
            (
                8,
                "screenshot-unavailable",
                "Screenshot capture or validation failed",
                "Unlock the phone; protected screens may restrict screenshots.",
            ),
            (
                9,
                "elements-unavailable",
                "UI dump or validation failed",
                "Unlock and open a native app; canvas and WebView content may require screenshots.",
            ),
        ] {
            checks[number - 1] = check(number as u8, "fail", code, detail, fix);
        }
        let target = Target {
            transport_id: phone.transport_id.unwrap(),
        };
        let properties = async {
            let android = adb
                .phone_property(&target, "ro.build.version.release")
                .await?;
            let model = adb.phone_property(&target, "ro.product.model").await?;
            let manufacturer = adb
                .phone_property(&target, "ro.product.manufacturer")
                .await?;
            adb.check_attachment(phone, CancellationToken::new())
                .await?;
            Ok::<_, ErrorCode>(format!("{manufacturer} {model}; Android {android}"))
        }
        .await;
        if let Ok(detail) = properties {
            checks[4] = check(
                5,
                "pass",
                "android-version",
                detail,
                "Only measured phones belong in the tested table.",
            );
        }
        if let Ok(size) = adb.wm_size(&target).await {
            checks[5] = check(
                6,
                "pass",
                "wm-size",
                format!(
                    "Effective wm size {}x{} (override preferred)",
                    size.width, size.height
                ),
                "Generic mapping uses this geometry, not a model allowlist.",
            );
        }
        if let Ok(view) = adb.viewport(&target, CancellationToken::new()).await {
            checks[6] = check(
                7,
                if view.rotation == Rotation::R0 {
                    "pass"
                } else {
                    "warn"
                },
                "rotation",
                format!("{:?}", view.rotation),
                if view.rotation == Rotation::R0 {
                    "Portrait rotation observed."
                } else {
                    "landscape is experimental; taps may be rotated"
                },
            );
            if let Ok(observed) = adb.observe(phone, CancellationToken::new()).await {
                checks[7] = check(
                    8,
                    "pass",
                    "screenshot-dimensions",
                    format!(
                        "{}x{}; memory only, not persisted",
                        observed.image.width, observed.image.height
                    ),
                    "Screenshots explicitly requested through MCP are sent to your model provider.",
                );
            }
            let elements = async {
                let xml = adb.ui_xml(&target, CancellationToken::new()).await?;
                let elements = crate::android_ui::parse(&xml, view.logical)?;
                let after = adb.viewport(&target, CancellationToken::new()).await?;
                adb.check_attachment(phone, CancellationToken::new())
                    .await?;
                if view != after {
                    return Err(ErrorCode::GeometryChanged);
                }
                Ok::<_, ErrorCode>(elements.len())
            }
            .await;
            if let Ok(count) = elements {
                checks[8] = check(
                    9,
                    if count > 0 { "pass" } else { "fail" },
                    "ui-elements",
                    format!("{count} elements; phone text withheld"),
                    if count > 0 {
                        "UI text is untrusted data. No input sent."
                    } else {
                        "Unlock the phone and open a native app; canvas/WebView screens may require an explicit screenshot."
                    },
                );
            }
        }
    }
    let ready = checks.iter().all(|c| c.status != "fail");
    Report {
        version: env!("CARGO_PKG_VERSION"),
        ready,
        backend: if ready {
            "native-diagnostics"
        } else {
            "unavailable"
        },
        hardware_verified: false,
        daemon_implemented: true,
        adb_detected,
        checks,
    }
}
#[cfg(not(feature = "native-aoa"))]
async fn usb_check(_: Option<&Attachment>) -> (String, Option<u16>) {
    (
        "Native libusb backend not compiled; use the native release build".into(),
        None,
    )
}
#[cfg(feature = "native-aoa")]
async fn usb_check(phone: Option<&Attachment>) -> (String, Option<u16>) {
    let runtime = libusb_version();
    let version = runtime
        .map(|(a, b, c)| format!("{a}.{b}.{c}"))
        .unwrap_or_else(|| "runtime version unavailable".into());
    if runtime.is_none_or(|v| v < (1, 0, 30)) {
        return (format!("libusb {version}; >=1.0.30 required"), None);
    }
    if phone.is_none() {
        return (
            format!("libusb {version}; no exact authorized phone selected"),
            None,
        );
    }
    let Ok(worker) = crate::native::discovery::worker() else {
        return (
            format!("libusb {version}; native context unavailable"),
            None,
        );
    };
    let snapshot = worker
        .snapshot(Duration::from_secs(3), &crate::backend::Cancel::default())
        .await;
    let protocol = snapshot
        .ok()
        .filter(|s| s.kind == crate::discovery::DiscoveryKind::Native)
        .and_then(|s| {
            let phone = phone?;
            let topology = phone.usb.as_ref()?;
            let matches = s
                .devices
                .iter()
                .filter(|d| d.topology().as_ref() == Ok(topology))
                .collect::<Vec<_>>();
            if matches.len() == 1 {
                Some(matches[0].aoa_protocol)
            } else {
                None
            }
        });
    let closed = worker.shutdown(Duration::from_secs(2)).await.is_ok();
    (
        format!(
            "libusb {version}; exact USB phone {}",
            if protocol.is_some() {
                "reachable"
            } else {
                "not established"
            }
        ),
        if closed { protocol } else { None },
    )
}
#[cfg(feature = "native-aoa")]
fn libusb_version() -> Option<(u16, u16, u16)> {
    // Public libusb ABI prefix: four uint16 fields. Resolve the runtime already
    // loaded by libaoahid; do not substitute pkg-config's build-time version.
    #[repr(C)]
    struct Version {
        major: u16,
        minor: u16,
        micro: u16,
        nano: u16,
    }
    unsafe {
        let symbol = libc::dlsym(libc::RTLD_DEFAULT, c"libusb_get_version".as_ptr());
        if symbol.is_null() {
            return None;
        }
        let get: unsafe extern "C" fn() -> *const Version = std::mem::transmute(symbol);
        let version = get().as_ref()?;
        Some((version.major, version.minor, version.micro))
    }
}
