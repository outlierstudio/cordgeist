use crate::adb::{Attachment, Authorization};
use serde::Serialize;
#[derive(Clone, Debug, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Diagnostic {
    pub code: &'static str,
    pub recovery: &'static str,
}
pub fn attachment_diagnostics(a: &Attachment) -> Vec<Diagnostic> {
    let mut out = Vec::new();
    match a.state {
        Authorization::Unauthorized=>out.push(Diagnostic{code:"adb-unauthorized",recovery:"Unlock the phone and deliberately authorize this host's debugging key."}),
        Authorization::Offline=>out.push(Diagnostic{code:"adb-offline",recovery:"Inspect the cable and phone state; reconnect this phone deliberately. Do not kill the shared ADB server."}),
        Authorization::NoPermissions=>out.push(Diagnostic{code:"usb-permission-denied",recovery:"Inspect this phone's USB ownership and applicable udev rules. Install targeted rules deliberately; do not run the controller as root."}),
        Authorization::Other(_)=>out.push(Diagnostic{code:"adb-state-unsupported",recovery:"Use a normal unlocked Android USB debugging attachment; recovery/bootloader modes are unsupported."}),
        Authorization::Authorized=>{},
    }
    if a.transport_id.is_none() || a.usb.is_none() || a.serial.trim().is_empty() {
        out.push(Diagnostic{code:"device-identity-ambiguous",recovery:"Require exact current USB topology and ADB transport evidence before input. Network attachments are not the USB control path."});
    }
    out
}

/// Static origin only; explicit software-fake diagnostics never include payload,
/// device identity, filesystem paths or process environments.
#[derive(Clone, Copy, Debug)]
pub(crate) enum QueueSource {
    StoreCapacity,
    PairingFileLock,
    DiscoveryWorker,
    AdbCapacity,
}
pub(crate) fn queue_full(source: QueueSource) -> crate::api::ErrorCode {
    #[cfg(feature = "fake")]
    eprintln!("SOFTWARE FAKE QueueFull source={source:?}");
    #[cfg(not(feature = "fake"))]
    let _ = source;
    crate::api::ErrorCode::QueueFull
}
