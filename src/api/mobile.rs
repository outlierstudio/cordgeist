use super::*;
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(deny_unknown_fields)]
pub struct Rect {
    pub x: u32,
    pub y: u32,
    pub width: u32,
    pub height: u32,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct UiElement {
    #[serde(rename = "type")]
    pub kind: String,
    pub text: String,
    pub label: String,
    pub identifier: String,
    pub rect: Rect,
    pub focused: bool,
}

#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(tag = "kind", rename_all = "kebab-case", deny_unknown_fields)]
pub enum AndroidCommand {
    LaunchApp { package: String },
    PressButton { button: AndroidButton },
}
#[derive(Clone, Copy, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "SCREAMING_SNAKE_CASE")]
pub enum AndroidButton {
    Back,
    Home,
    Enter,
}
impl AndroidCommand {
    pub fn validate(&self) -> Result<(), ErrorCode> {
        match self {
            Self::LaunchApp { package } if !valid_app_name(package) => {
                Err(ErrorCode::InvalidRequest)
            }
            _ => Ok(()),
        }
    }
    #[cfg(feature = "native-aoa")]
    pub(crate) fn shell_command(&self) -> Result<String, ErrorCode> {
        self.validate()?;
        Ok(match self {
            Self::LaunchApp { package } => {
                if !valid_package(package) || !package.contains('.') {
                    return Err(ErrorCode::InvalidRequest);
                }
                format!("monkey -p {package} -c android.intent.category.LAUNCHER 1")
            }
            Self::PressButton { button } => format!(
                "input keyevent {}",
                match button {
                    AndroidButton::Back => 4,
                    AndroidButton::Home => 3,
                    AndroidButton::Enter => 66,
                }
            ),
        })
    }
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct ElementsResult {
    pub frame: Frame,
    pub elements: Vec<UiElement>,
    pub warnings: Vec<String>,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct InstalledApp {
    pub package_name: String,
    pub app_name: String,
}
#[derive(Clone, Serialize, Deserialize, JsonSchema)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct AppsResult {
    pub apps: Vec<InstalledApp>,
}

pub fn valid_package(package: &str) -> bool {
    !package.is_empty()
        && package.len() <= 255
        && package
            .bytes()
            .all(|b| b.is_ascii_alphanumeric() || matches!(b, b'.' | b'_'))
}

/// Friendly names are data; only resolved, validated package IDs reach a shell.
pub fn valid_app_name(value: &str) -> bool {
    !value.trim().is_empty()
        && value.len() <= 255
        && value
            .chars()
            .all(|c| c.is_alphanumeric() || matches!(c, ' ' | '.' | '_' | '-'))
}
/// Android framework intents, not manufacturer/model package constants.
/// https://developer.android.com/reference/android/provider/Settings#ACTION_SETTINGS
/// https://developer.android.com/reference/android/provider/MediaStore#INTENT_ACTION_STILL_IMAGE_CAMERA
pub fn common_app_action(name: &str) -> Option<&'static str> {
    if name.trim().eq_ignore_ascii_case("settings")
        || name.trim().eq_ignore_ascii_case("system settings")
    {
        Some("android.settings.SETTINGS")
    } else if name.trim().eq_ignore_ascii_case("camera") {
        Some("android.media.action.STILL_IMAGE_CAMERA")
    } else {
        None
    }
}

/// Four times the measured Note9 cold-launch maximum (896.441 ms), rounded
/// up to a whole second. Includes routing/geometry/name-resolution margin.
/// This is an operation budget, never a phone/model allowlist.
pub const APP_LAUNCH_TIMEOUT_MS: u64 = 4_000;
impl Call {
    pub fn is_app_launch(&self) -> bool {
        matches!(
            self,
            Self::Android {
                command: AndroidCommand::LaunchApp { .. },
                ..
            }
        )
    }
}

#[cfg(test)]
mod launch_budget_tests {
    use super::*;
    #[test]
    fn longer_budget_is_exclusive_to_launch() {
        let auth = LeaseAuth {
            boot_id: BootId("b".into()),
            device_id: DeviceId("d".into()),
            lease_id: LeaseId("l".into()),
            credential: Credential("private".into()),
            attachment: AttachmentGeneration(1),
            fence: Fence(1),
        };
        let launch = Call::Android {
            auth: auth.clone(),
            seq: ActionSeq(1),
            command: AndroidCommand::LaunchApp {
                package: "Chrome".into(),
            },
        };
        let button = Call::Android {
            auth,
            seq: ActionSeq(2),
            command: AndroidCommand::PressButton {
                button: AndroidButton::Back,
            },
        };
        assert!(launch.is_app_launch());
        assert!(!button.is_app_launch());
        assert!(!Call::Status.is_app_launch());
        assert_eq!(APP_LAUNCH_TIMEOUT_MS, 4_000);
    }
}
