//! Bounded ADB adapter. Observation, one media push, and one scan-file shell
//! command. No arbitrary shell or input API is exposed.
use crate::api::{ErrorCode, Size};
use std::{path::PathBuf, process::Stdio, sync::Arc, time::Duration};
use tokio::{
    io::{AsyncRead, AsyncReadExt},
    process::Command,
};
use tokio_util::sync::CancellationToken;

#[derive(Clone, Debug)]
pub struct Adb {
    executable: PathBuf,
    port: u16,
    capacity: Arc<tokio::sync::Semaphore>,
}
#[derive(Clone, Debug)]
pub struct Target {
    pub transport_id: u64,
}
pub struct Output {
    pub stdout: Vec<u8>,
    pub stderr: Vec<u8>,
    pub stderr_truncated: bool,
}
pub struct Finished {
    pub output: Output,
    pub success: bool,
}
pub struct Observed {
    pub png: Vec<u8>,
    pub image: Size,
    pub metadata: crate::metadata::DisplayMetadata,
}
struct Decoded {
    png: Vec<u8>,
    image: Size,
    source: Size,
}
/// Resolve installed adb without changing process environment or downloading anything.
pub fn configured_path() -> Option<PathBuf> {
    if let Some(path) = std::env::var_os("CORDGEIST_ADB") {
        let path = PathBuf::from(path);
        return (path.is_absolute() && path.is_file()).then_some(path);
    }
    std::env::split_paths(&std::env::var_os("PATH")?)
        .filter(|p| p.is_absolute())
        .map(|p| p.join("adb"))
        .find(|p| p.is_file())
}
impl Adb {
    pub fn new(executable: PathBuf, port: u16) -> Result<Self, ErrorCode> {
        if !executable.is_absolute() || port == 0 || !executable.is_file() {
            return Err(ErrorCode::BackendUnavailable);
        }
        Ok(Self {
            executable,
            port,
            capacity: Arc::new(tokio::sync::Semaphore::new(8)),
        })
    }
    pub async fn version(&self, cancel: CancellationToken) -> Result<Output, ErrorCode> {
        self.run(
            None,
            &["version"],
            16 * 1024,
            Duration::from_secs(2),
            cancel,
        )
        .await
    }
    pub async fn inventory(
        &self,
        deadline: Duration,
        cancel: CancellationToken,
    ) -> Result<Output, ErrorCode> {
        self.run(None, &["devices", "-l"], 1024 * 1024, deadline, cancel)
            .await
    }
    /// Read-only build identity for trusted profile matching. Never diagnostic text.
    pub async fn build_identity(
        &self,
        target: &Target,
        cancel: CancellationToken,
    ) -> Result<String, ErrorCode> {
        let output = self
            .run(
                Some(target),
                &["shell", "getprop", "ro.build.fingerprint"],
                1024,
                Duration::from_secs(2),
                cancel,
            )
            .await?;
        let value = std::str::from_utf8(&output.stdout)
            .map_err(|_| ErrorCode::MetadataUnsupported)?
            .trim();
        if value.is_empty() || value.len() > 256 || value.chars().any(char::is_control) {
            return Err(ErrorCode::MetadataUnsupported);
        }
        Ok(value.to_owned())
    }
    /// Fixed read-only diagnostic property catalogue; never accepts a serial or arbitrary shell.
    pub async fn phone_property(
        &self,
        target: &Target,
        property: &str,
    ) -> Result<String, ErrorCode> {
        if !matches!(
            property,
            "ro.build.version.release" | "ro.product.model" | "ro.product.manufacturer"
        ) {
            return Err(ErrorCode::InvalidRequest);
        }
        let output = self
            .run(
                Some(target),
                &["shell", "getprop", property],
                1024,
                Duration::from_secs(2),
                CancellationToken::new(),
            )
            .await?;
        let text = std::str::from_utf8(&output.stdout)
            .map_err(|_| ErrorCode::MetadataUnsupported)?
            .trim();
        if text.is_empty() || text.len() > 128 || text.chars().any(char::is_control) {
            return Err(ErrorCode::MetadataUnsupported);
        }
        Ok(text.to_owned())
    }
    pub async fn wm_size(&self, target: &Target) -> Result<Size, ErrorCode> {
        let output = self
            .run(
                Some(target),
                &["shell", "wm", "size"],
                4096,
                Duration::from_secs(2),
                CancellationToken::new(),
            )
            .await?;
        crate::geometry::wm_size(&output.stdout)
    }
    pub async fn screenshot(
        &self,
        target: &Target,
        cancel: CancellationToken,
    ) -> Result<Output, ErrorCode> {
        self.run(
            Some(target),
            &["exec-out", "screencap", "-p"],
            32 * 1024 * 1024,
            Duration::from_secs(10),
            cancel,
        )
        .await
    }
    pub async fn display_metadata(
        &self,
        target: &Target,
        cancel: CancellationToken,
    ) -> Result<Output, ErrorCode> {
        self.run(
            Some(target),
            &["shell", "dumpsys", "input"],
            1024 * 1024,
            Duration::from_secs(5),
            cancel,
        )
        .await
    }
    /// Explicit setup only: exact-target USB loopback to owned synthetic content.
    /// Never replaces another existing reverse mapping.
    pub async fn prepare_test_area(
        &self,
        target: &Target,
        cancel: CancellationToken,
    ) -> Result<(), ErrorCode> {
        let listed = self
            .run(
                Some(target),
                &["reverse", "--list"],
                16 * 1024,
                Duration::from_secs(2),
                cancel.clone(),
            )
            .await?;
        let text =
            std::str::from_utf8(&listed.stdout).map_err(|_| ErrorCode::MetadataUnsupported)?;
        for line in text.lines() {
            let fields: Vec<_> = line.split_whitespace().collect();
            if fields.get(1) == Some(&"tcp:18764") {
                return if fields.len() == 3 && fields[2] == "tcp:18764" {
                    Ok(())
                } else {
                    Err(ErrorCode::BackendUnavailable)
                };
            }
        }
        self.run(
            Some(target),
            &["reverse", "--no-rebind", "tcp:18764", "tcp:18764"],
            1024,
            Duration::from_secs(2),
            cancel,
        )
        .await?;
        Ok(())
    }
    /// Read-only mapping evidence for the daemon's setup state. This never
    /// marks a profile verified or authorizes an input report.
    pub async fn calibration_axes(
        &self,
        target: &Target,
        display: Size,
        maxima: (u16, u16),
        cancel: CancellationToken,
    ) -> Result<crate::calibration::AxisEvidence, ErrorCode> {
        let output = self.display_metadata(target, cancel).await?;
        crate::calibration::android_axes(&output.stdout, display, maxima)
    }
    pub async fn observe(
        &self,
        expected: &Attachment,
        cancel: CancellationToken,
    ) -> Result<Observed, ErrorCode> {
        let target = Target {
            transport_id: expected
                .transport_id
                .ok_or(ErrorCode::DeviceIdentityAmbiguous)?,
        };
        if expected.serial.trim().is_empty()
            || expected.usb.is_none()
            || expected.state != Authorization::Authorized
        {
            return Err(ErrorCode::DeviceIdentityAmbiguous);
        }
        let work = async {
            self.check_attachment(expected, cancel.clone()).await?;
            let before = self.viewport(&target, cancel.clone()).await?;
            let png = self.screenshot(&target, cancel.clone()).await?.stdout;
            let after = self.viewport(&target, cancel.clone()).await?;
            self.check_attachment(expected, cancel.clone()).await?;
            if before != after {
                return Err(ErrorCode::GeometryChanged);
            }
            // Blocking decode is bounded by input/output dimensions and off the runtime.
            let decoded = tokio::task::spawn_blocking(move || decode_resize_png(&png, 0))
                .await
                .map_err(|_| ErrorCode::CaptureUnavailable)??;
            if decoded.source != after.logical {
                return Err(ErrorCode::GeometryChanged);
            }
            Ok(Observed {
                png: decoded.png,
                image: decoded.image,
                metadata: after,
            })
        };
        tokio::time::timeout(Duration::from_secs(10), work)
            .await
            .unwrap_or(Err(ErrorCode::ProcessTimeout))
    }
    pub(crate) async fn check_attachment(
        &self,
        expected: &Attachment,
        cancel: CancellationToken,
    ) -> Result<(), ErrorCode> {
        let output = self.inventory(Duration::from_secs(2), cancel).await?;
        let devices = parse_inventory(&output.stdout)?;
        let matched = devices
            .iter()
            .filter(|a| a.transport_id == expected.transport_id)
            .collect::<Vec<_>>();
        if matched.len() != 1
            || matched[0].serial != expected.serial
            || matched[0].usb != expected.usb
            || matched[0].state != Authorization::Authorized
        {
            return Err(ErrorCode::DeviceIdentityAmbiguous);
        }
        Ok(())
    }
    pub async fn viewport(
        &self,
        target: &Target,
        cancel: CancellationToken,
    ) -> Result<crate::metadata::DisplayMetadata, ErrorCode> {
        let dump = self.display_metadata(target, cancel.clone()).await?;
        let user = self
            .run(
                Some(target),
                &["shell", "am", "get-current-user"],
                32,
                Duration::from_secs(2),
                cancel.clone(),
            )
            .await?;
        let wm = self
            .run(
                Some(target),
                &["shell", "wm", "size"],
                4096,
                Duration::from_secs(2),
                cancel,
            )
            .await?;
        let effective = crate::geometry::wm_size(&wm.stdout)?;
        let mut metadata = crate::metadata::parse_aosp_viewport(&dump.stdout, &user.stdout)
            .or_else(|_| {
                crate::metadata::generic_viewport(&dump.stdout, &user.stdout, effective)
            })?;
        let effective = crate::geometry::orient_like(effective, metadata.logical);
        if effective != metadata.logical {
            return Err(ErrorCode::GeometryChanged);
        }
        metadata.logical = effective;
        Ok(metadata)
    }
    async fn run(
        &self,
        target: Option<&Target>,
        args: &[&str],
        cap: usize,
        deadline: Duration,
        cancel: CancellationToken,
    ) -> Result<Output, ErrorCode> {
        if cancel.is_cancelled() {
            return Err(ErrorCode::ProcessCancelled);
        }
        if deadline.is_zero()
            || deadline > Duration::from_secs(10)
            || target.is_some_and(|t| t.transport_id == 0)
        {
            return Err(ErrorCode::InvalidRequest);
        }
        let permit = self.capacity.clone().try_acquire_owned().map_err(|_| {
            crate::diagnostics::queue_full(crate::diagnostics::QueueSource::AdbCapacity)
        })?;
        let owned = self.clone();
        let target = target.cloned();
        let args = args.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        let cancellation = cancel.child_token();
        let _guard = CancelSupervisor(cancellation.clone());
        let supervisor = tokio::spawn(async move {
            let _permit = permit; // retained until kill AND explicit wait/reap complete
            let args = args.iter().map(String::as_str).collect::<Vec<_>>();
            owned
                .run_supervised(target.as_ref(), &args, cap, deadline, cancellation)
                .await
                .map(|finished| {
                    if finished.success {
                        Ok(finished.output)
                    } else {
                        Err(ErrorCode::ProcessFailed)
                    }
                })?
        });
        // Caller remains bounded if SIGKILL/reap itself cannot finish. The
        // supervisor keeps ownership and one of the eight slots until cleanup.
        tokio::time::timeout(deadline + Duration::from_secs(1), supervisor)
            .await
            .map_err(|_| ErrorCode::ProcessTimeout)?
            .map_err(|_| ErrorCode::ProcessFailed)?
    }
    /// One bounded command. A non-zero exit is returned with its output so media
    /// transfer can tell a full device from a denied scan. This is not a shell API.
    pub async fn finished(
        &self,
        target: &Target,
        args: &[&str],
        cap: usize,
        deadline: Duration,
    ) -> Result<Finished, ErrorCode> {
        if args.is_empty()
            || args.iter().any(|arg| arg.is_empty() || arg.contains('\0'))
            || !matches!(args[0], "push" | "shell")
        {
            return Err(ErrorCode::InvalidRequest);
        }
        if args[0] == "shell" && (args.len() != 2 || !shell_allowed(args[1])) {
            return Err(ErrorCode::InvalidRequest);
        }
        if args[0] == "push" && args.len() != 3 {
            return Err(ErrorCode::InvalidRequest);
        }
        self.finish(Some(target), args, cap, deadline, CancellationToken::new())
            .await
    }
    async fn finish(
        &self,
        target: Option<&Target>,
        args: &[&str],
        cap: usize,
        deadline: Duration,
        cancel: CancellationToken,
    ) -> Result<Finished, ErrorCode> {
        if cancel.is_cancelled() {
            return Err(ErrorCode::ProcessCancelled);
        }
        if deadline.is_zero()
            || deadline > Duration::from_secs(10)
            || target.is_some_and(|t| t.transport_id == 0)
        {
            return Err(ErrorCode::InvalidRequest);
        }
        let permit = self.capacity.clone().try_acquire_owned().map_err(|_| {
            crate::diagnostics::queue_full(crate::diagnostics::QueueSource::AdbCapacity)
        })?;
        let owned = self.clone();
        let target = target.cloned();
        let args = args.iter().map(|s| s.to_string()).collect::<Vec<_>>();
        let cancellation = cancel.child_token();
        let _guard = CancelSupervisor(cancellation.clone());
        let supervisor = tokio::spawn(async move {
            let _permit = permit;
            let args = args.iter().map(String::as_str).collect::<Vec<_>>();
            owned
                .run_supervised(target.as_ref(), &args, cap, deadline, cancellation)
                .await
        });
        tokio::time::timeout(deadline + Duration::from_secs(1), supervisor)
            .await
            .map_err(|_| ErrorCode::ProcessTimeout)?
            .map_err(|_| ErrorCode::ProcessFailed)?
    }
    async fn run_supervised(
        &self,
        target: Option<&Target>,
        args: &[&str],
        cap: usize,
        deadline: Duration,
        cancel: CancellationToken,
    ) -> Result<Finished, ErrorCode> {
        if cancel.is_cancelled() {
            return Err(ErrorCode::ProcessCancelled);
        }
        if deadline.is_zero() || target.is_some_and(|t| t.transport_id == 0) {
            return Err(ErrorCode::InvalidRequest);
        }
        let mut cmd = Command::new(&self.executable);
        // Remove routing overrides and all ambient environment. Retain only host
        // authentication/home/runtime essentials explicitly; never retain ADB_*.
        cmd.env_clear();
        for name in ["HOME", "USER", "LOGNAME", "TMPDIR", "XDG_RUNTIME_DIR"] {
            if let Some(value) = std::env::var_os(name) {
                cmd.env(name, value);
            }
        }
        cmd.env("LC_ALL", "C").env("PATH", "/usr/bin:/bin");
        cmd.args(["-H", "127.0.0.1", "-P", &self.port.to_string()]);
        if let Some(t) = target {
            cmd.args(["-t", &t.transport_id.to_string()]);
        }
        cmd.args(args)
            .stdin(Stdio::null())
            .stdout(Stdio::piped())
            .stderr(Stdio::piped())
            .kill_on_drop(true);
        // Kill/reap the entire subprocess group, including children holding pipes.
        #[cfg(unix)]
        cmd.process_group(0);
        let mut child = cmd.spawn().map_err(|error| {
            // Stable host error details only: never executable paths, argv,
            // environment or subprocess output. No retry or deadline extension.
            eprintln!(
                "adb-supervisor stage=spawn kind={:?} os-code={:?}",
                error.kind(),
                error.raw_os_error()
            );
            ErrorCode::BackendUnavailable
        })?;
        // A fast child can exit before Tokio records its pid. pid 0 must never
        // be used as a process-group signal.
        let pid = child.id().unwrap_or(0);
        let mut group = ProcessGroup {
            pid,
            armed: pid != 0,
        };
        let stdout = child.stdout.take().ok_or(ErrorCode::ProcessFailed)?;
        let stderr = child.stderr.take().ok_or(ErrorCode::ProcessFailed)?;
        let work = async {
            tokio::try_join!(
                read_stdout(stdout, cap),
                read_stderr(stderr, 256 * 1024),
                async { child.wait().await.map_err(|_| ErrorCode::ProcessFailed) }
            )
        };
        let result = tokio::select! {
            biased;
            _ = cancel.cancelled() => Err(ErrorCode::ProcessCancelled),
            r = tokio::time::timeout(deadline, work) => r.unwrap_or(Err(ErrorCode::ProcessTimeout)),
        };
        match result {
            Ok((stdout, (stderr, stderr_truncated), status)) => {
                group.armed = false;
                Ok(Finished {
                    success: status.success(),
                    output: Output {
                        stdout,
                        stderr,
                        stderr_truncated,
                    },
                })
            }
            Err(e) => {
                #[cfg(unix)]
                if pid != 0 {
                    unsafe {
                        kill(-(pid as i32), 9);
                    }
                }
                let _ = child.start_kill();
                child.wait().await.map_err(|_| ErrorCode::ProcessFailed)?;
                group.armed = false;
                Err(e)
            }
        }
    }
}
struct CancelSupervisor(CancellationToken);
impl Drop for CancelSupervisor {
    fn drop(&mut self) {
        self.0.cancel();
    }
}
// Last-resort safety if the supervisor task itself is terminated with its runtime.
// Normal caller cancellation leaves it alive to kill AND explicitly reap.
struct ProcessGroup {
    pid: u32,
    armed: bool,
}
impl Drop for ProcessGroup {
    fn drop(&mut self) {
        if self.armed && self.pid != 0 {
            #[cfg(unix)]
            unsafe {
                kill(-(self.pid as i32), 9);
            }
        }
    }
}
#[cfg(unix)]
unsafe extern "C" {
    fn kill(pid: i32, signal: i32) -> i32;
}
fn shell_allowed(command: &str) -> bool {
    if matches!(
        command,
        "am get-current-user"
            | "mkdir -p '/sdcard/Pictures/Cordgeist'"
            | "mkdir -p '/sdcard/Movies/Cordgeist'"
    ) {
        return true;
    }
    let Some(path) = command.strip_prefix("wc -c ") else {
        return quoted_pair("mv ", command)
            || command
                .strip_prefix("rm ")
                .is_some_and(|rest| quoted_path(rest).is_some())
            || scan_command(command);
    };
    quoted_path(path).is_some()
}
fn scan_command(command: &str) -> bool {
    let Some(extra) = command
        .strip_prefix("content call --user 0 --uri content://media --method scan_file --extra ")
    else {
        return false;
    };
    let Some(path) = extra.strip_prefix("'path:s:") else {
        return false;
    };
    let Some(path) = path.strip_suffix('\'') else {
        return false;
    };
    opaque_media_path(path)
}
fn quoted_pair(prefix: &str, command: &str) -> bool {
    let Some(rest) = command.strip_prefix(prefix) else {
        return false;
    };
    let Some((left, right)) = rest.split_once(' ') else {
        return false;
    };
    quoted_path(left).is_some() && quoted_path(right).is_some()
}
fn quoted_path(value: &str) -> Option<&str> {
    let path = value.strip_prefix('\'')?.strip_suffix('\'')?;
    opaque_media_path(path).then_some(path)
}
fn opaque_media_path(path: &str) -> bool {
    let Some(name) = path.strip_prefix("/sdcard/Pictures/Cordgeist/") else {
        return path
            .strip_prefix("/sdcard/Movies/Cordgeist/")
            .is_some_and(opaque_name);
    };
    opaque_name(name)
}
fn opaque_name(name: &str) -> bool {
    let name = name.strip_prefix('.').unwrap_or(name);
    let Some((hex, ext)) = name.split_once('.') else {
        return false;
    };
    (hex.len() == 32 && hex.bytes().all(|b| b.is_ascii_hexdigit())
        || hex.split_once('-').is_some_and(|(lease, seq)| {
            lease.len() == 64
                && seq.len() == 32
                && lease
                    .bytes()
                    .chain(seq.bytes())
                    .all(|b| b.is_ascii_hexdigit())
        }))
        && matches!(ext, "png" | "jpg" | "mp4" | "partial")
}
async fn read_stdout(mut r: impl AsyncRead + Unpin, cap: usize) -> Result<Vec<u8>, ErrorCode> {
    let mut bytes = Vec::new();
    let mut buf = [0; 8192];
    loop {
        let n = r
            .read(&mut buf)
            .await
            .map_err(|_| ErrorCode::ProcessFailed)?;
        if n == 0 {
            break;
        }
        if n > cap.saturating_sub(bytes.len()) {
            return Err(ErrorCode::OutputLimit);
        }
        bytes.extend_from_slice(&buf[..n]);
    }
    Ok(bytes)
}
async fn read_stderr(
    mut r: impl AsyncRead + Unpin,
    cap: usize,
) -> Result<(Vec<u8>, bool), ErrorCode> {
    let mut bytes = Vec::new();
    let mut truncated = false;
    let mut buf = [0; 8192];
    loop {
        let n = r
            .read(&mut buf)
            .await
            .map_err(|_| ErrorCode::ProcessFailed)?;
        if n == 0 {
            break;
        }
        let keep = n.min(cap.saturating_sub(bytes.len()));
        truncated |= keep != n;
        bytes.extend_from_slice(&buf[..keep]);
    }
    Ok((bytes, truncated))
}

#[derive(Clone, Debug, PartialEq, Eq)]
pub enum Authorization {
    Authorized,
    Unauthorized,
    Offline,
    NoPermissions,
    Other(String),
}
#[derive(Clone, PartialEq, Eq)]
pub struct Attachment {
    pub serial: String,
    pub state: Authorization,
    pub transport_id: Option<u64>,
    pub usb: Option<String>,
}
pub fn parse_inventory(bytes: &[u8]) -> Result<Vec<Attachment>, ErrorCode> {
    let text = std::str::from_utf8(bytes).map_err(|_| ErrorCode::MetadataUnsupported)?;
    let mut lines = text.lines();
    if lines.next() != Some("List of devices attached") {
        return Err(ErrorCode::MetadataUnsupported);
    }
    let mut result = Vec::new();
    for line in lines.filter(|l| !l.trim().is_empty()) {
        let mut parts = line.split_whitespace();
        let serial = parts
            .next()
            .ok_or(ErrorCode::MetadataUnsupported)?
            .to_owned();
        let state = match parts.next().ok_or(ErrorCode::MetadataUnsupported)? {
            "device" => Authorization::Authorized,
            "unauthorized" => Authorization::Unauthorized,
            "offline" => Authorization::Offline,
            "no" if line.contains("no permissions") => Authorization::NoPermissions,
            other => Authorization::Other(other.into()),
        };
        let mut transport_id = None;
        let mut usb = None;
        for field in parts {
            if let Some(id) = field.strip_prefix("transport_id:") {
                if transport_id.is_some() {
                    return Err(ErrorCode::DeviceIdentityAmbiguous);
                }
                transport_id = Some(
                    id.parse::<u64>()
                        .ok()
                        .filter(|n| *n > 0)
                        .ok_or(ErrorCode::MetadataUnsupported)?,
                );
            }
            if let Some(path) = field.strip_prefix("usb:") {
                if usb.is_some() || path.is_empty() {
                    return Err(ErrorCode::DeviceIdentityAmbiguous);
                }
                usb = Some(path.into());
            }
        }
        if transport_id.is_some()
            && result
                .iter()
                .any(|a: &Attachment| a.transport_id == transport_id)
        {
            return Err(ErrorCode::DeviceIdentityAmbiguous);
        }
        result.push(Attachment {
            serial,
            state,
            transport_id,
            usb,
        });
    }
    Ok(result)
}
pub fn resize_png(bytes: &[u8], longest: u32) -> Result<(Vec<u8>, Size), ErrorCode> {
    if longest == 0 {
        return Err(ErrorCode::InvalidRequest);
    }
    let decoded = decode_resize_png(bytes, longest)?;
    Ok((decoded.png, decoded.image))
}
fn decode_resize_png(bytes: &[u8], longest: u32) -> Result<Decoded, ErrorCode> {
    use image::ImageDecoder;
    if bytes.len() > 32 * 1024 * 1024
        || !bytes.starts_with(b"\x89PNG\r\n\x1a\n")
        || (longest != 0 && !(2..=8192).contains(&longest))
    {
        return Err(ErrorCode::CaptureUnavailable);
    }
    let mut limits = image::Limits::default();
    limits.max_alloc = Some(64 * 1024 * 1024);
    limits.max_image_width = Some(16_000_000);
    limits.max_image_height = Some(16_000_000);
    let decoder = image::codecs::png::PngDecoder::with_limits(std::io::Cursor::new(bytes), limits)
        .map_err(|_| ErrorCode::CaptureUnavailable)?;
    let (w, h) = decoder.dimensions();
    if w < 2
        || h < 2
        || u64::from(w) * u64::from(h) > 16_000_000
        || decoder.total_bytes() > 64 * 1024 * 1024
    {
        return Err(ErrorCode::CaptureUnavailable);
    }
    let img =
        image::DynamicImage::from_decoder(decoder).map_err(|_| ErrorCode::CaptureUnavailable)?;
    if longest == 0 || longest >= w.max(h) {
        if longest != 0 && bytes.len() > 2 * 1024 * 1024 {
            return Err(ErrorCode::OutputLimit);
        }
        return Ok(Decoded {
            png: bytes.to_vec(),
            image: Size {
                width: w,
                height: h,
            },
            source: Size {
                width: w,
                height: h,
            },
        });
    }
    let img = if w.max(h) > longest {
        img.resize(longest, longest, image::imageops::FilterType::Triangle)
    } else {
        img
    };
    let size = Size {
        width: img.width(),
        height: img.height(),
    };
    if size.width < 2 || size.height < 2 {
        return Err(ErrorCode::CaptureUnavailable);
    }
    let mut out = std::io::Cursor::new(Vec::new());
    img.write_to(&mut out, image::ImageFormat::Png)
        .map_err(|_| ErrorCode::CaptureUnavailable)?;
    let out = out.into_inner();
    if out.len() > 2 * 1024 * 1024 {
        return Err(ErrorCode::OutputLimit);
    }
    Ok(Decoded {
        png: out,
        image: size,
        source: Size {
            width: w,
            height: h,
        },
    })
}

// Adapted from mobile-mcp (Apache-2.0) src/android.ts at
// 3b51952bcd83955f3a743f731f5a9beaebecdab7. Bounded Rust collection differs:
// one idle retry, owned temporary fallback, cancellation, explicit cleanup.
impl Adb {
    pub async fn ui_xml(
        &self,
        target: &Target,
        cancel: CancellationToken,
    ) -> Result<Vec<u8>, ErrorCode> {
        let start = std::time::Instant::now();
        let result = tokio::time::timeout(
            Duration::from_millis(6500),
            self.ui_xml_bounded(target, cancel),
        )
        .await
        .unwrap_or(Err(ErrorCode::ProcessTimeout));
        eprintln!(
            "cordgeist ui_dump duration_ms={} outcome={}",
            start.elapsed().as_millis(),
            if result.is_ok() { "ok" } else { "unavailable" }
        );
        result
    }
    async fn ui_xml_bounded(
        &self,
        target: &Target,
        cancel: CancellationToken,
    ) -> Result<Vec<u8>, ErrorCode> {
        let first = self
            .finish(
                Some(target),
                &[
                    "exec-out",
                    "timeout",
                    "3",
                    "uiautomator",
                    "dump",
                    "/dev/tty",
                ],
                crate::android_ui::MAX_XML,
                Duration::from_secs(3),
                cancel.clone(),
            )
            .await;
        if let Ok(finished) = first {
            if let Some(body) = crate::android_ui::xml_body(&finished.output.stdout) {
                return Ok(body.to_vec());
            }
            let idle = finished
                .output
                .stdout
                .windows(24)
                .chain(finished.output.stderr.windows(24))
                .any(|v| v.eq_ignore_ascii_case(b"could not get idle state"));
            if idle {
                tokio::select! {_=cancel.cancelled()=>return Err(ErrorCode::ProcessCancelled),_=tokio::time::sleep(Duration::from_millis(300))=>{}}
            }
        }
        if cancel.is_cancelled() {
            return Err(ErrorCode::ProcessCancelled);
        }
        // Exactly one retry: compressed dump to our owned fallback file.
        let mut random = [0u8; 16];
        getrandom::fill(&mut random).map_err(|_| ErrorCode::BackendUnavailable)?;
        let id = random
            .iter()
            .map(|b| format!("{b:02x}"))
            .collect::<String>();
        let path = format!("/sdcard/Cordgeist-ui-{id}.xml");
        let mut guard = UiTemporary {
            adb: self.clone(),
            target: target.clone(),
            path: path.clone(),
            armed: true,
        };
        let work = async {
            self.run(
                Some(target),
                &[
                    "exec-out",
                    "timeout",
                    "3",
                    "uiautomator",
                    "dump",
                    "--compressed",
                    &path,
                ],
                crate::android_ui::MAX_XML,
                Duration::from_secs(3),
                cancel.clone(),
            )
            .await?;
            let output = self
                .run(
                    Some(target),
                    &["exec-out", "cat", &path],
                    crate::android_ui::MAX_XML,
                    Duration::from_secs(1),
                    cancel.clone(),
                )
                .await?;
            crate::android_ui::xml_body(&output.stdout)
                .map(|v| v.to_vec())
                .ok_or(ErrorCode::CaptureUnavailable)
        }
        .await;
        guard.cleanup().await?;
        work
    }
    /// Resolve friendly names through framework roles or a unique installed
    /// package suffix. Never interpolate a caller's friendly name into shell.
    pub async fn resolve_app(
        &self,
        target: &Target,
        name: &str,
        cancel: CancellationToken,
    ) -> Result<String, ErrorCode> {
        let name = name.trim();
        if !crate::api::valid_app_name(name) {
            return Err(ErrorCode::InvalidRequest);
        }
        if name.contains('.') {
            return if valid_package(name) {
                Ok(name.into())
            } else {
                Err(ErrorCode::InvalidRequest)
            };
        }
        let mut packages = if let Some(action) = crate::api::common_app_action(name) {
            let output = self
                .run(
                    Some(target),
                    &[
                        "shell",
                        "cmd",
                        "package",
                        "resolve-activity",
                        "--brief",
                        "-a",
                        action,
                    ],
                    4096,
                    Duration::from_secs(1),
                    cancel,
                )
                .await?;
            std::str::from_utf8(&output.stdout)
                .map_err(|_| ErrorCode::MetadataUnsupported)?
                .lines()
                .filter_map(|line| line.trim().split_once('/'))
                .filter(|(package, activity)| {
                    valid_package(package)
                        && !activity.is_empty()
                        && !activity.contains(char::is_whitespace)
                })
                .map(|(package, _)| package.to_owned())
                .collect::<Vec<_>>()
        } else {
            let normalise = |v: &str| {
                v.chars()
                    .filter(|c| c.is_alphanumeric())
                    .flat_map(char::to_lowercase)
                    .collect::<String>()
            };
            let wanted = normalise(name);
            self.list_apps(target, cancel)
                .await?
                .into_iter()
                .filter(|package| {
                    package
                        .rsplit('.')
                        .next()
                        .is_some_and(|last| normalise(last) == wanted)
                })
                .collect()
        };
        packages.sort();
        packages.dedup();
        if packages.len() != 1 || packages[0] == "android" {
            return Err(ErrorCode::InvalidRequest);
        }
        Ok(packages.remove(0))
    }
    pub async fn list_apps(
        &self,
        target: &Target,
        cancel: CancellationToken,
    ) -> Result<Vec<String>, ErrorCode> {
        let output = self
            .run(
                Some(target),
                &["shell", "pm list packages"],
                512 * 1024,
                Duration::from_secs(2),
                cancel,
            )
            .await?;
        let text =
            std::str::from_utf8(&output.stdout).map_err(|_| ErrorCode::MetadataUnsupported)?;
        let mut apps = vec![];
        for line in text.lines() {
            let Some(package) = line.trim().strip_prefix("package:") else {
                continue;
            };
            if valid_package(package) {
                apps.push(package.to_owned())
            }
            if apps.len() > 4096 {
                return Err(ErrorCode::OutputLimit);
            }
        }
        apps.sort();
        apps.dedup();
        Ok(apps)
    }
}
pub use crate::api::valid_package;

struct UiTemporary {
    adb: Adb,
    target: Target,
    path: String,
    armed: bool,
}
impl UiTemporary {
    async fn cleanup(&mut self) -> Result<(), ErrorCode> {
        let command = format!("rm '{}'", self.path);
        self.adb
            .run(
                Some(&self.target),
                &["shell", &command],
                256,
                Duration::from_secs(1),
                CancellationToken::new(),
            )
            .await?;
        self.armed = false;
        Ok(())
    }
}
impl Drop for UiTemporary {
    fn drop(&mut self) {
        if self.armed {
            let adb = self.adb.clone();
            let target = self.target.clone();
            let command = format!("rm '{}'", self.path);
            if let Ok(runtime) = tokio::runtime::Handle::try_current() {
                runtime.spawn(async move {
                    let _ = adb
                        .run(
                            Some(&target),
                            &["shell", &command],
                            256,
                            Duration::from_secs(1),
                            CancellationToken::new(),
                        )
                        .await;
                });
            }
        }
    }
}

impl Adb {
    /// Fixed typed control commands only; this is not an arbitrary shell API.
    #[cfg(feature = "native-aoa")]
    pub async fn android_command(
        &self,
        target: &Target,
        command: &crate::api::AndroidCommand,
        deadline: std::time::Instant,
        cancel: CancellationToken,
    ) -> Result<Finished, ErrorCode> {
        let shell = command.shell_command()?;
        let remaining = deadline
            .checked_duration_since(std::time::Instant::now())
            .filter(|d| !d.is_zero())
            .ok_or(ErrorCode::ProcessTimeout)?;
        self.finish(
            Some(target),
            &["shell", &shell],
            4096,
            remaining.min(
                if matches!(command, crate::api::AndroidCommand::LaunchApp { .. }) {
                    Duration::from_millis(crate::api::APP_LAUNCH_TIMEOUT_MS)
                } else {
                    Duration::from_secs(2)
                },
            ),
            cancel,
        )
        .await
    }
}
