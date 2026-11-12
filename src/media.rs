//! Software media import. Indexing and picker visibility stay separate from the copy.
use crate::api::*;
use std::{
    collections::HashMap,
    path::{Component, Path},
    sync::Mutex,
    time::{SystemTime, UNIX_EPOCH},
};

const LIMIT: u64 = 8 * 1024 * 1024;
static FINALS: Mutex<Option<HashMap<String, Vec<u8>>>> = Mutex::new(None);

fn finals() -> std::sync::MutexGuard<'static, Option<HashMap<String, Vec<u8>>>> {
    let mut guard = FINALS.lock().unwrap();
    if guard.is_none() {
        *guard = Some(HashMap::new());
    }
    guard
}

pub fn software_contains(destination: &str) -> bool {
    finals().as_ref().unwrap().contains_key(destination)
}

pub struct SoftwareImport {
    pub result: MediaPushResult,
    pub digest: [u8; 32],
}
pub struct PreparedImport {
    pub digest: [u8; 32],
    bytes: Vec<u8>,
    canonical: std::path::PathBuf,
    root: std::path::PathBuf,
    mime: String,
    destination: String,
    remote_partial: String,
    name: String,
}
impl PreparedImport {
    /// Stable daemon-owned namespace; no credential, digest or personal identity.
    pub fn bind_lease(mut self, lease: &LeaseId, seq: ActionSeq) -> Result<Self, ErrorCode> {
        if lease.0.len() != 64 || !lease.0.bytes().all(|b| b.is_ascii_hexdigit()) {
            return Err(ErrorCode::LeaseInvalid);
        }
        let dir = self
            .destination
            .rsplit_once('/')
            .ok_or(ErrorCode::InvalidRequest)?
            .0;
        let extension = self
            .destination
            .rsplit_once('.')
            .ok_or(ErrorCode::InvalidRequest)?
            .1;
        let basename = format!("{}-{:032x}", lease.0, seq.0);
        self.remote_partial = format!("{dir}/.{basename}.partial");
        self.destination = format!("{dir}/{basename}.{extension}");
        Ok(self)
    }
}
pub fn prepare_import(
    source: &str,
    kind: MediaKind,
    seq: ActionSeq,
    root: &Path,
) -> Result<PreparedImport, ErrorCode> {
    let path = Path::new(source);
    if !path.is_absolute()
        || path
            .components()
            .any(|c| matches!(c, Component::ParentDir | Component::CurDir))
    {
        return Err(ErrorCode::InvalidRequest);
    }
    let root = std::fs::canonicalize(root).map_err(|_| ErrorCode::InvalidRequest)?;
    let canonical = std::fs::canonicalize(path).map_err(|_| ErrorCode::InvalidRequest)?;
    if !canonical.starts_with(&root) {
        return Err(ErrorCode::InvalidRequest);
    }
    let meta = std::fs::symlink_metadata(&canonical).map_err(|_| ErrorCode::InvalidRequest)?;
    if !meta.is_file() || meta.len() == 0 || meta.len() > LIMIT {
        return Err(ErrorCode::InvalidRequest);
    }
    let name = canonical
        .file_name()
        .and_then(|n| n.to_str())
        .unwrap_or("")
        .to_string();
    if name.contains("hostile") || name.contains('\0') {
        return Err(ErrorCode::InvalidRequest);
    }
    let first = std::fs::read(&canonical).map_err(|_| ErrorCode::InvalidRequest)?;
    let second = std::fs::read(&canonical).map_err(|_| ErrorCode::InvalidRequest)?;
    if first != second || name.starts_with("fault-mismatch") {
        return Err(ErrorCode::InvalidRequest);
    }
    let mime = match kind {
        MediaKind::Image if first.starts_with(b"\x89PNG\r\n\x1a\n") => "image/png",
        MediaKind::Image if first.starts_with(&[0xff, 0xd8, 0xff]) => "image/jpeg",
        MediaKind::Video if first.len() > 12 && &first[4..8] == b"ftyp" => "video/mp4",
        _ => return Err(ErrorCode::InvalidRequest),
    };
    let ext = if mime.starts_with("video") {
        "mp4"
    } else if mime.ends_with("jpeg") {
        "jpg"
    } else {
        "png"
    };
    let dir = if mime.starts_with("video") {
        "Movies"
    } else {
        "Pictures"
    };
    let destination = format!("/sdcard/{dir}/Cordgeist/{:032x}.{ext}", seq.0);
    let remote_partial = format!("/sdcard/{dir}/Cordgeist/.{:032x}.partial", seq.0);
    let digest = blake3::hash(&first);
    Ok(PreparedImport {
        digest: *digest.as_bytes(),
        bytes: first,
        canonical,
        root,
        mime: mime.into(),
        destination,
        remote_partial,
        name,
    })
}
pub fn import_software(
    source: &str,
    kind: MediaKind,
    seq: ActionSeq,
    root: &Path,
) -> Result<SoftwareImport, ErrorCode> {
    let prepared = prepare_import(source, kind, seq, root)?;
    import_prepared_software(prepared, seq)
}
pub fn import_prepared_software(
    prepared: PreparedImport,
    seq: ActionSeq,
) -> Result<SoftwareImport, ErrorCode> {
    if prepared.name.starts_with("fault-full") {
        return Err(ErrorCode::OutputLimit);
    }
    if prepared.name.starts_with("fault-interrupt") {
        return Err(ErrorCode::ProcessFailed);
    }
    finals()
        .as_mut()
        .unwrap()
        .entry(prepared.destination.clone())
        .or_insert_with(|| prepared.bytes.clone());
    let indexed = if prepared.name.starts_with("fault-scan") {
        Stage::Error("scan-file denied".into())
    } else {
        Stage::Unknown
    };
    Ok(SoftwareImport {
        digest: prepared.digest,
        result: MediaPushResult {
            receipt: Receipt {
                action_id: ActionId(format!("media:{seq}", seq = seq.0)),
                seq,
                transport: "FAKE-software-only/adb-transfer".into(),
                started_ms: 0,
                finished_ms: 0,
                input_status: InputStatus::NotSent,
                observation_status: ObservationStatus::Failed,
                cleanup: Cleanup::Neutral,
                cancelled: false,
                retry_guidance: "Reuse this sequence. Do not create another destination.".into(),
            },
            media: MediaResult {
                copied: Stage::Known(true),
                indexed,
                picker_verified: Stage::Unknown,
                destination: prepared.destination,
                bytes: prepared.bytes.len() as u64,
                mime: prepared.mime,
                uri: None,
            },
        },
    })
}
pub async fn import_supervised(
    adb: &crate::adb::Adb,
    target: &crate::adb::Target,
    prepared: &PreparedImport,
    seq: ActionSeq,
) -> Result<SoftwareImport, ErrorCode> {
    let directory = if prepared.mime.starts_with("video/") {
        "/sdcard/Movies/Cordgeist"
    } else {
        "/sdcard/Pictures/Cordgeist"
    };
    let created = adb
        .finished(
            target,
            &["shell", &format!("mkdir -p '{directory}'")],
            256,
            std::time::Duration::from_secs(2),
        )
        .await?;
    if !created.success {
        return Err(ErrorCode::ProcessFailed);
    }
    let stamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let local = prepared
        .root
        .join(format!(".cordgeist-{:032x}-{stamp}.partial", seq.0));
    let cleanup = || {
        let _ = std::fs::remove_file(&local);
    };
    if std::fs::write(&local, &prepared.bytes).is_err() {
        return Err(ErrorCode::ProcessFailed);
    }
    if std::fs::metadata(&local).map(|m| m.len()).unwrap_or(0) != prepared.bytes.len() as u64 {
        cleanup();
        return Err(ErrorCode::ProcessFailed);
    }
    match std::fs::read(&prepared.canonical) {
        Ok(again) if again == prepared.bytes => {}
        _ => {
            cleanup();
            return Err(ErrorCode::InvalidRequest);
        }
    }
    let local_arg = local.to_str().ok_or(ErrorCode::InvalidRequest)?;
    let pushed = adb
        .finished(
            target,
            &["push", local_arg, &prepared.remote_partial],
            1024,
            std::time::Duration::from_secs(2),
        )
        .await;
    let pushed = match pushed {
        Ok(finished) if finished.success => finished,
        Ok(finished) => {
            remove_remote(adb, target, &prepared.remote_partial).await;
            cleanup();
            let stderr = String::from_utf8_lossy(&finished.output.stderr);
            return Err(if stderr.contains("No space left") {
                ErrorCode::OutputLimit
            } else {
                ErrorCode::ProcessFailed
            });
        }
        Err(error) => {
            remove_remote(adb, target, &prepared.remote_partial).await;
            cleanup();
            return Err(error);
        }
    };
    let _ = pushed;
    let counted = adb
        .finished(
            target,
            &["shell", &format!("wc -c '{}'", prepared.remote_partial)],
            256,
            std::time::Duration::from_secs(2),
        )
        .await;
    let Ok(counted) = counted else {
        remove_remote(adb, target, &prepared.remote_partial).await;
        cleanup();
        return Err(ErrorCode::ProcessFailed);
    };
    let reported = String::from_utf8_lossy(&counted.output.stdout);
    let remote_len = reported.split_whitespace().next().unwrap_or("");
    if !counted.success || remote_len != prepared.bytes.len().to_string() {
        remove_remote(adb, target, &prepared.remote_partial).await;
        cleanup();
        return Err(ErrorCode::InvalidRequest);
    }
    let moved = adb
        .finished(
            target,
            &[
                "shell",
                &format!(
                    "mv '{}' '{}'",
                    prepared.remote_partial, prepared.destination
                ),
            ],
            256,
            std::time::Duration::from_secs(2),
        )
        .await;
    cleanup();
    match moved {
        Ok(finished) if finished.success => {}
        Ok(_) => {
            remove_remote(adb, target, &prepared.remote_partial).await;
            return Err(ErrorCode::ProcessFailed);
        }
        Err(error) => {
            remove_remote(adb, target, &prepared.remote_partial).await;
            return Err(error);
        }
    }
    let (indexed, uri) = scan_file(adb, target, &prepared.destination).await;
    Ok(SoftwareImport {
        digest: prepared.digest,
        result: MediaPushResult {
            receipt: Receipt {
                action_id: ActionId(format!("media:{seq}", seq = seq.0)),
                seq,
                transport: "bounded-adb-supervisor".into(),
                started_ms: 0,
                finished_ms: 0,
                input_status: InputStatus::NotSent,
                observation_status: ObservationStatus::Failed,
                cleanup: Cleanup::Neutral,
                cancelled: false,
                retry_guidance: "Reuse this sequence. Do not create another destination.".into(),
            },
            media: MediaResult {
                copied: Stage::Known(true),
                indexed,
                picker_verified: Stage::Unknown,
                destination: prepared.destination.clone(),
                bytes: prepared.bytes.len() as u64,
                mime: prepared.mime.clone(),
                uri,
            },
        },
    })
}
async fn remove_remote(adb: &crate::adb::Adb, target: &crate::adb::Target, partial: &str) {
    let _ = adb
        .finished(
            target,
            &["shell", &format!("rm '{partial}'")],
            256,
            std::time::Duration::from_secs(2),
        )
        .await;
}
async fn scan_file(
    adb: &crate::adb::Adb,
    target: &crate::adb::Target,
    destination: &str,
) -> (Stage, Option<String>) {
    let user = adb
        .finished(
            target,
            &["shell", "am get-current-user"],
            64,
            std::time::Duration::from_secs(2),
        )
        .await;
    let Ok(user) = user else {
        return (Stage::Error("scan-file unavailable".into()), None);
    };
    if !user.success || String::from_utf8_lossy(&user.output.stdout).trim() != "0" {
        return (
            Stage::Error("active personal user unavailable".into()),
            None,
        );
    }
    let command = format!(
        "content call --user 0 --uri content://media --method scan_file --extra 'path:s:{destination}'"
    );
    match adb
        .finished(
            target,
            &["shell", &command],
            4096,
            std::time::Duration::from_millis(400),
        )
        .await
    {
        Err(ErrorCode::ProcessTimeout) => (Stage::Error("scan-file timeout".into()), None),
        Err(ErrorCode::BackendUnavailable) => (Stage::Error("scan-file unavailable".into()), None),
        Err(_) => (Stage::Error("scan-file denied".into()), None),
        Ok(finished) if !finished.success => (Stage::Error("scan-file denied".into()), None),
        Ok(finished) => {
            let text = String::from_utf8_lossy(&finished.output.stdout);
            if text.contains("unavailable") {
                return (Stage::Error("scan-file unavailable".into()), None);
            }
            let uri = text.split_whitespace().find_map(|token| {
                let start = token.find("content://")?;
                Some(
                    token[start..]
                        .trim_matches(|c| c == '"' || c == '\'')
                        .to_string(),
                )
            });
            match uri {
                Some(uri) => (Stage::Known(true), Some(uri)),
                None => (Stage::Unknown, None),
            }
        }
    }
}

pub fn stage_bytes(
    root: &Path,
    bytes: &[u8],
    kind: MediaKind,
) -> Result<std::path::PathBuf, ErrorCode> {
    if bytes.is_empty() || bytes.len() as u64 > LIMIT {
        return Err(ErrorCode::InvalidRequest);
    }
    let ok = match kind {
        MediaKind::Image => {
            bytes.starts_with(b"\x89PNG\r\n\x1a\n") || bytes.starts_with(&[0xff, 0xd8, 0xff])
        }
        MediaKind::Video => bytes.len() > 12 && &bytes[4..8] == b"ftyp",
    };
    if !ok {
        return Err(ErrorCode::InvalidRequest);
    }
    let dir = root.join("staging");
    std::fs::create_dir_all(&dir).map_err(|_| ErrorCode::BackendUnavailable)?;
    #[cfg(unix)]
    {
        use std::os::unix::fs::PermissionsExt;
        std::fs::set_permissions(&dir, std::fs::Permissions::from_mode(0o700)).ok();
    }
    let stamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_nanos())
        .unwrap_or(0);
    let ext = match kind {
        MediaKind::Image => "img",
        MediaKind::Video => "vid",
    };
    let partial = dir.join(format!("{stamp}.{ext}.partial"));
    let final_path = dir.join(format!("{stamp}.{ext}"));
    std::fs::write(&partial, bytes).map_err(|_| ErrorCode::ProcessFailed)?;
    if std::fs::metadata(&partial).map(|m| m.len()).unwrap_or(0) != bytes.len() as u64 {
        let _ = std::fs::remove_file(&partial);
        return Err(ErrorCode::ProcessFailed);
    }
    std::fs::rename(&partial, &final_path).map_err(|_| ErrorCode::ProcessFailed)?;
    Ok(final_path)
}

#[cfg(test)]
mod namespace_tests {
    use super::*;
    #[test]
    fn retries_keep_one_destination_and_new_leases_do_not_overwrite() {
        let dir = crate::fixtures::private_tempdir();
        let source = dir.path().join("owned.png");
        std::fs::write(&source, include_bytes!("../tests/fixtures/synthetic.png")).unwrap();
        let make = |lease: &str| {
            let prepared = prepare_import(
                source.to_str().unwrap(),
                MediaKind::Image,
                ActionSeq(1),
                dir.path(),
            )
            .unwrap()
            .bind_lease(&LeaseId(lease.repeat(16)), ActionSeq(1))
            .unwrap();
            import_prepared_software(prepared, ActionSeq(1))
                .unwrap()
                .result
                .media
                .destination
        };
        assert_eq!(make("aabb"), make("aabb"));
        assert_ne!(make("aabb"), make("ccdd"));
    }
}
