use schemars::JsonSchema;
use serde::{Deserialize, Serialize};

macro_rules! id {
    ($($name:ident),+) => {$(
        #[derive(Clone, Debug, PartialEq, Eq, Hash, Serialize, Deserialize, JsonSchema)]
        #[serde(transparent)]
        pub struct $name(pub String);
    )+};
}
id!(DeviceId, BootId, FrameId, ActionId, LeaseId);
macro_rules! generation {
    ($($name:ident),+) => {$(
        #[derive(Clone, Copy, Debug, PartialEq, Eq, Hash, Serialize, Deserialize, JsonSchema)]
        #[serde(transparent)]
        pub struct $name(pub u64);
    )+};
}
generation!(
    AttachmentGeneration,
    Fence,
    GeometryVersion,
    MappingVersion,
    ActionSeq
);

// Deliberately no Debug: credentials and input payloads must not enter logs.
#[derive(Clone, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(transparent)]
pub struct Credential(pub String);
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct LeaseAuth {
    pub boot_id: BootId,
    pub device_id: DeviceId,
    pub lease_id: LeaseId,
    pub credential: Credential,
    pub attachment: AttachmentGeneration,
    pub fence: Fence,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "kebab-case")]
pub enum Evidence {
    Unavailable,
    Detected,
    Verified,
}
#[derive(Clone, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase")]
pub struct Capability {
    pub evidence: Evidence,
    pub reason: String,
    pub profile: Option<String>,
}
#[derive(Clone, Debug, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Provenance {
    pub boot_id: BootId,
    pub device_id: DeviceId,
    pub lease_id: LeaseId,
    pub attachment: AttachmentGeneration,
    pub fence: Fence,
    pub geometry: GeometryVersion,
    pub mapping: MappingVersion,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "kebab-case")]
pub enum Rotation {
    R0,
    R90,
    R180,
    R270,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Size {
    pub width: u32,
    pub height: u32,
}
#[derive(Clone, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Frame {
    pub id: FrameId,
    pub provenance: Provenance,
    pub captured_ms: u64,
    pub image: Size,
    pub display: Size,
    pub rotation: Rotation,
    pub display_id: u32,
    pub coordinate_space: CoordinateSpace,
}
#[derive(Clone, Copy, Debug, Default, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "kebab-case")]
pub enum CoordinateSpace {
    #[default]
    ReturnedImagePixels,
    ScreenPixels,
}

#[derive(Clone, Copy, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Point {
    pub x: f64,
    pub y: f64,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(tag = "kind", rename_all = "kebab-case", deny_unknown_fields)]
pub enum Input {
    Tap {
        point: Point,
    },
    Swipe {
        from: Point,
        to: Point,
        duration_ms: u32,
    },
    Hold {
        point: Point,
        duration_ms: u32,
    },
    Type {
        text: String,
    },
    Key {
        key: String,
    },
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct ActionRequest {
    pub auth: LeaseAuth,
    pub seq: ActionSeq,
    pub frame_id: FrameId,
    pub input: Input,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "kebab-case")]
pub enum InputStatus {
    NotSent,
    Sent,
    Partial,
    Unknown,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "kebab-case")]
pub enum ObservationStatus {
    NotRequested,
    Captured,
    Timeout,
    Failed,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "kebab-case")]
pub enum Cleanup {
    Neutral,
    Pending,
    Failed,
}
#[derive(Clone, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase")]
pub struct Receipt {
    pub action_id: ActionId,
    pub seq: ActionSeq,
    pub transport: String,
    pub started_ms: u64,
    pub finished_ms: u64,
    pub input_status: InputStatus,
    pub observation_status: ObservationStatus,
    pub cleanup: Cleanup,
    pub cancelled: bool,
    pub retry_guidance: String,
}
#[derive(Clone, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(tag = "state", content = "detail", rename_all = "kebab-case")]
pub enum Stage {
    Known(bool),
    Unknown,
    Error(String),
}
#[derive(Clone, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase")]
pub struct MediaResult {
    pub copied: Stage,
    pub indexed: Stage,
    pub picker_verified: Stage,
    pub destination: String,
    pub bytes: u64,
    pub mime: String,
    pub uri: Option<String>,
}
#[derive(
    Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize, JsonSchema, thiserror::Error,
)]
#[serde(rename_all = "kebab-case")]
pub enum ErrorCode {
    #[error("protocol-version-unsupported")]
    ProtocolVersionUnsupported,
    #[error("backend-unavailable")]
    BackendUnavailable,
    #[error("device-identity-ambiguous")]
    DeviceIdentityAmbiguous,
    #[error("lease-invalid")]
    LeaseInvalid,
    #[error("lease-expired")]
    LeaseExpired,
    #[error("leased")]
    Leased,
    #[error("geometry-changed")]
    GeometryChanged,
    #[error("frame-expired")]
    FrameExpired,
    #[error("invalid-request")]
    InvalidRequest,
    #[error("mapping-unverified")]
    MappingUnverified,
    #[error("unsupported-character")]
    UnsupportedCharacter,
    #[error("unsupported-key")]
    UnsupportedKey,
    #[error("receipt-expired")]
    ReceiptExpired,
    #[error("sequence-mismatch")]
    SequenceMismatch,
    #[error("queue-full")]
    QueueFull,
    #[error("native-cleanup-pending")]
    NativeCleanupPending,
    #[error("input-outcome-unknown")]
    InputOutcomeUnknown,
    #[error("capture-unavailable")]
    CaptureUnavailable,
    #[error("metadata-unsupported")]
    MetadataUnsupported,
    #[error("ocr-unavailable")]
    OcrUnavailable,
    #[error("ocr-ambiguous")]
    OcrAmbiguous,
    #[error("media-partial-result")]
    MediaPartialResult,
    #[error("process-timeout")]
    ProcessTimeout,
    #[error("process-cancelled")]
    ProcessCancelled,
    #[error("output-limit")]
    OutputLimit,
    #[error("process-failed")]
    ProcessFailed,
    #[error("adb-unauthorized")]
    AdbUnauthorized,
    #[error("device-disconnected")]
    DeviceDisconnected,
}
#[derive(Clone, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct ApiError {
    pub code: ErrorCode,
    pub phase: String,
    pub recovery: String,
}
#[derive(Clone, Copy, Debug, Serialize, JsonSchema)]
#[serde(rename_all = "kebab-case")]
pub enum Effect {
    Read,
    Control,
    Input,
    Transfer,
}
#[derive(Clone, Debug, Serialize, JsonSchema)]
pub struct Operation {
    pub method: &'static str,
    pub effect: Effect,
    pub lease: bool,
    pub frame: bool,
    pub mcp: bool,
}
pub const OPERATIONS: &[Operation] = &[
    Operation {
        method: "observations.elements",
        effect: Effect::Read,
        lease: true,
        frame: false,
        mcp: false,
    },
    Operation {
        method: "apps.list",
        effect: Effect::Read,
        lease: true,
        frame: false,
        mcp: false,
    },
    Operation {
        method: "input.android",
        effect: Effect::Input,
        lease: true,
        frame: false,
        mcp: false,
    },
    Operation {
        method: "setup.calibrate",
        effect: Effect::Control,
        lease: false,
        frame: false,
        mcp: false,
    },
    Operation {
        method: "system.status",
        effect: Effect::Read,
        lease: false,
        frame: false,
        mcp: true,
    },
    Operation {
        method: "system.doctor",
        effect: Effect::Read,
        lease: false,
        frame: false,
        mcp: true,
    },
    Operation {
        method: "devices.list",
        effect: Effect::Read,
        lease: false,
        frame: false,
        mcp: true,
    },
    Operation {
        method: "leases.acquire",
        effect: Effect::Control,
        lease: false,
        frame: false,
        mcp: true,
    },
    Operation {
        method: "leases.release",
        effect: Effect::Control,
        lease: true,
        frame: false,
        mcp: true,
    },
    Operation {
        method: "leases.revoke",
        effect: Effect::Control,
        lease: false,
        frame: false,
        mcp: false,
    },
    Operation {
        method: "observations.screenshot",
        effect: Effect::Read,
        lease: true,
        frame: false,
        mcp: true,
    },
    Operation {
        method: "input.tap",
        effect: Effect::Input,
        lease: true,
        frame: true,
        mcp: true,
    },
    Operation {
        method: "input.swipe",
        effect: Effect::Input,
        lease: true,
        frame: true,
        mcp: true,
    },
    Operation {
        method: "input.hold",
        effect: Effect::Input,
        lease: true,
        frame: true,
        mcp: true,
    },
    Operation {
        method: "input.type",
        effect: Effect::Input,
        lease: true,
        frame: true,
        mcp: true,
    },
    Operation {
        method: "input.key",
        effect: Effect::Input,
        lease: true,
        frame: true,
        mcp: true,
    },
    Operation {
        method: "actions.status",
        effect: Effect::Read,
        lease: true,
        frame: false,
        mcp: true,
    },
    Operation {
        method: "actions.cancel",
        effect: Effect::Control,
        lease: true,
        frame: false,
        mcp: true,
    },
    Operation {
        method: "observations.text",
        effect: Effect::Read,
        lease: true,
        frame: false,
        mcp: false,
    },
    Operation {
        method: "media.push",
        effect: Effect::Transfer,
        lease: true,
        frame: false,
        mcp: false,
    },
];

/// Complete protocol-level call shapes. Catalogue entries describe the target API,
/// not a claim that the daemon or every adapter is implemented yet.
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(tag = "method", content = "params", deny_unknown_fields)]
pub enum Call {
    #[serde(rename = "observations.elements")]
    Elements { auth: LeaseAuth },
    #[serde(rename = "apps.list")]
    Apps { auth: LeaseAuth },
    #[serde(rename = "input.android")]
    Android {
        auth: LeaseAuth,
        seq: ActionSeq,
        command: AndroidCommand,
    },
    #[serde(rename = "setup.calibrate")]
    Calibrate {
        #[serde(default)]
        device: Option<DeviceId>,
        #[serde(default)]
        probe: bool,
    },
    #[serde(rename = "system.status")]
    Status,
    #[serde(rename = "system.doctor")]
    Doctor { device: Option<DeviceId> },
    #[serde(rename = "devices.list")]
    List,
    #[serde(rename = "leases.acquire")]
    Acquire {
        device: DeviceId,
        reason: String,
        label: Option<String>,
        idle_ttl_s: u32,
    },
    #[serde(rename = "leases.release")]
    Release { auth: LeaseAuth },
    #[serde(rename = "leases.revoke")]
    Revoke { device: DeviceId },
    #[serde(rename = "observations.screenshot")]
    Screenshot {
        auth: LeaseAuth,
        longest_edge: u32,
        settle_ms: u32,
    },
    #[serde(rename = "input.tap")]
    Tap {
        auth: LeaseAuth,
        seq: ActionSeq,
        frame_id: FrameId,
        point: Point,
    },
    #[serde(rename = "input.swipe")]
    Swipe {
        auth: LeaseAuth,
        seq: ActionSeq,
        frame_id: FrameId,
        from: Point,
        to: Point,
        duration_ms: u32,
    },
    #[serde(rename = "input.hold")]
    Hold {
        auth: LeaseAuth,
        seq: ActionSeq,
        frame_id: FrameId,
        point: Point,
        duration_ms: u32,
    },
    #[serde(rename = "input.type")]
    Type {
        auth: LeaseAuth,
        seq: ActionSeq,
        frame_id: FrameId,
        text: String,
        #[serde(default)]
        submit: bool,
    },
    #[serde(rename = "input.key")]
    Key {
        auth: LeaseAuth,
        seq: ActionSeq,
        frame_id: FrameId,
        key: String,
    },
    #[serde(rename = "actions.status")]
    ActionStatus { auth: LeaseAuth, seq: ActionSeq },
    #[serde(rename = "actions.cancel")]
    Cancel { auth: LeaseAuth, seq: ActionSeq },
    #[serde(rename = "observations.text")]
    Text {
        auth: LeaseAuth,
        frame_id: Option<FrameId>,
        language: String,
    },
    #[serde(rename = "media.push")]
    MediaPush {
        auth: LeaseAuth,
        seq: ActionSeq,
        source: String,
        kind: MediaKind,
    },
}
#[derive(Clone, Copy, PartialEq, Eq, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "kebab-case")]
pub enum MediaKind {
    Image,
    Video,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Request {
    #[serde(default)]
    pub coordinate_space: CoordinateSpace,
    /// Optional post-action screenshot. Explicit observations are independent.
    #[serde(default)]
    pub observe_after: bool,
    pub version: u32,
    pub request_id: String,
    pub deadline_ms: u64,
    pub call: Call,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct LeaseGrant {
    pub auth: LeaseAuth,
    pub idle_deadline_ms: u64,
    pub next_seq: ActionSeq,
}

mod response;
pub use response::*;

mod mobile;
pub use mobile::*;
