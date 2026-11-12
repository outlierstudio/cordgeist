//! Wire contracts only: these types neither dispatch operations nor access devices.
use super::*;

/// The only supported wire version. Unknown numeric versions fail deserialization.
#[derive(Clone, Copy, Debug, PartialEq, Eq, Serialize, Deserialize)]
#[serde(try_from = "u32", into = "u32")]
pub struct ProtocolVersion;
impl TryFrom<u32> for ProtocolVersion {
    type Error = &'static str;
    fn try_from(value: u32) -> Result<Self, Self::Error> {
        if value == 1 {
            Ok(Self)
        } else {
            Err("unsupported protocol version")
        }
    }
}
impl From<ProtocolVersion> for u32 {
    fn from(_: ProtocolVersion) -> Self {
        1
    }
}
impl JsonSchema for ProtocolVersion {
    fn schema_name() -> std::borrow::Cow<'static, str> {
        "ProtocolVersion".into()
    }
    fn json_schema(_: &mut schemars::SchemaGenerator) -> schemars::Schema {
        serde_json::json!({"type":"integer", "const":1})
            .try_into()
            .expect("constant schema")
    }
}

/// Contains private image/OCR data or a lease grant. Intentionally not Debug.
/// ```compile_fail
/// fn require_debug<T: std::fmt::Debug>() {}
/// require_debug::<cordgeist::api::Response>();
/// ```
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Response {
    pub version: ProtocolVersion,
    pub request_id: String,
    pub boot_id: BootId,
    pub outcome: ResponseOutcome,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(tag = "status", rename_all = "kebab-case", deny_unknown_fields)]
pub enum ResponseOutcome {
    Success { result: Box<OperationResult> },
    Error { error: ApiError },
}
impl Response {
    /// Consumers must correlate decoded replies before accepting their contents.
    pub fn validate_for(&self, request: &Request, expected_boot: &BootId) -> Result<(), ErrorCode> {
        if request.request_id.is_empty()
            || self.request_id != request.request_id
            || &self.boot_id != expected_boot
        {
            return Err(ErrorCode::InvalidRequest);
        }
        if request.version != 1
            && !matches!(&self.outcome,
            ResponseOutcome::Error { error } if error.code == ErrorCode::ProtocolVersionUnsupported)
        {
            return Err(ErrorCode::ProtocolVersionUnsupported);
        }
        if let ResponseOutcome::Success { result } = &self.outcome
            && result.method() != request.call.method()
        {
            return Err(ErrorCode::InvalidRequest);
        }
        Ok(())
    }
    pub fn for_request(
        request: &Request,
        boot_id: BootId,
        outcome: ResponseOutcome,
    ) -> Result<Self, ErrorCode> {
        let response = Self {
            version: ProtocolVersion,
            request_id: request.request_id.clone(),
            boot_id,
            outcome,
        };
        response.validate_for(request, &response.boot_id)?;
        Ok(response)
    }
}

#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct DeviceCapabilities {
    pub screenshot: Capability,
    pub aoa_touch: Capability,
    pub keyboard: Capability,
    pub supported_keys: Vec<String>,
    pub keyboard_layout: Option<String>,
    pub ocr: Capability,
    pub ocr_languages: Vec<String>,
    pub media_scan: Capability,
}
#[derive(Clone, Copy, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "kebab-case")]
pub enum BackendKind {
    Unavailable,
    AoaHid,
    FakeSoftwareOnly,
}
#[derive(Clone, Copy, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "kebab-case")]
pub enum DeviceHealth {
    Discovered,
    IdentityMatched,
    Initializing,
    Ready,
    Leased,
    Degraded,
    Quarantined,
    Disconnected,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct DeviceSummary {
    pub device_id: DeviceId,
    pub attachment: AttachmentGeneration,
    pub health: DeviceHealth,
    pub reason_codes: Vec<ErrorCode>,
    pub capabilities: DeviceCapabilities,
    /// Informational label only, never an owner credential.
    pub owner_label: Option<String>,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct StatusResult {
    pub software_version: String,
    pub backend: BackendKind,
    pub ready: bool,
    pub capabilities: DeviceCapabilities,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct DoctorResult {
    pub device_id: Option<DeviceId>,
    pub checks: Vec<DiagnosticResult>,
    pub capabilities: DeviceCapabilities,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct DiagnosticResult {
    pub code: String,
    pub evidence: Evidence,
    pub recovery: String,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct DevicesResult {
    pub devices: Vec<DeviceSummary>,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct LeaseEndResult {
    pub device_id: DeviceId,
    pub lease_id: Option<LeaseId>,
    pub fence: Fence,
    pub cleanup: Cleanup,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct ScreenshotResult {
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub warnings: Vec<String>,
    pub frame: Frame,
    pub image: PngImage,
    pub settled: Option<bool>,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct PngImage {
    pub mime_type: PngMime,
    pub base64: String,
}
#[derive(Clone, Copy, Debug, Serialize, Deserialize, JsonSchema)]
pub enum PngMime {
    #[serde(rename = "image/png")]
    Png,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct ActionResult {
    #[serde(default, skip_serializing_if = "Vec::is_empty")]
    pub warnings: Vec<String>,
    pub receipt: Receipt,
    /// Present only for receipt.observationStatus=captured. No local-only path.
    pub observation: Option<ScreenshotResult>,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(
    tag = "phase",
    rename_all = "kebab-case",
    rename_all_fields = "camelCase",
    deny_unknown_fields
)]
pub enum ActionStatusResult {
    Queued { action_id: ActionId, seq: ActionSeq },
    Executing { action_id: ActionId, seq: ActionSeq },
    Final { action: Box<ActionResult> },
}
#[derive(Clone, Copy, Debug, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "kebab-case")]
pub enum CancellationState {
    Requested,
    Cancelled,
    AlreadyFinal,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct CancelResult {
    pub action_id: ActionId,
    pub seq: ActionSeq,
    pub state: CancellationState,
    pub cleanup: Cleanup,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct TextResult {
    pub frame: Frame,
    pub language: String,
    pub words: Vec<OcrWord>,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct OcrWord {
    pub text: String,
    pub confidence: f32,
    pub origin: Point,
    pub size: Size,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct MediaPushResult {
    pub receipt: Receipt,
    pub media: MediaResult,
}

#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct CalibrationResult {
    pub display: Size,
    pub maximum_x: u16,
    pub maximum_y: u16,
    pub verified: bool,
    pub recovery: String,
}

// Each result is method-tagged even when methods share a payload type. The
// exhaustive match supplies an auditable catalogue-to-wire method mapping.
macro_rules! results {
    ($($variant:ident : $payload:ty => $method:literal),+ $(,)?) => {
        #[derive(Clone, Serialize, Deserialize, JsonSchema)]
        #[serde(tag = "method", content = "value", deny_unknown_fields)]
        pub enum OperationResult { $(#[serde(rename = $method)] $variant($payload)),+ }
        impl OperationResult {
            pub fn method(&self) -> &'static str { match self { $(Self::$variant(_) => $method),+ } }
        }
    };
}
results! {
    Elements: ElementsResult => "observations.elements",
    Apps: AppsResult => "apps.list",
    Android: ActionResult => "input.android",
    Calibrate: CalibrationResult => "setup.calibrate",
    Status: StatusResult => "system.status",
    Doctor: DoctorResult => "system.doctor",
    List: DevicesResult => "devices.list",
    Acquire: LeaseGrant => "leases.acquire",
    Release: LeaseEndResult => "leases.release",
    Revoke: LeaseEndResult => "leases.revoke",
    Screenshot: ScreenshotResult => "observations.screenshot",
    Tap: ActionResult => "input.tap",
    Swipe: ActionResult => "input.swipe",
    Hold: ActionResult => "input.hold",
    Type: ActionResult => "input.type",
    Key: ActionResult => "input.key",
    ActionStatus: ActionStatusResult => "actions.status",
    Cancel: CancelResult => "actions.cancel",
    Text: TextResult => "observations.text",
    MediaPush: MediaPushResult => "media.push",
}
impl Call {
    pub fn method(&self) -> &'static str {
        match self {
            Self::Elements { .. } => "observations.elements",
            Self::Apps { .. } => "apps.list",
            Self::Android { .. } => "input.android",
            Self::Calibrate { .. } => "setup.calibrate",
            Self::Status => "system.status",
            Self::Doctor { .. } => "system.doctor",
            Self::List => "devices.list",
            Self::Acquire { .. } => "leases.acquire",
            Self::Release { .. } => "leases.release",
            Self::Revoke { .. } => "leases.revoke",
            Self::Screenshot { .. } => "observations.screenshot",
            Self::Tap { .. } => "input.tap",
            Self::Swipe { .. } => "input.swipe",
            Self::Hold { .. } => "input.hold",
            Self::Type { .. } => "input.type",
            Self::Key { .. } => "input.key",
            Self::ActionStatus { .. } => "actions.status",
            Self::Cancel { .. } => "actions.cancel",
            Self::Text { .. } => "observations.text",
            Self::MediaPush { .. } => "media.push",
        }
    }
}
