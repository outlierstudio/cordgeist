//! Runnable local CLI. Same-user IPC only; no network API and no silent input replay.
use crate::{
    api::*,
    daemon::{self, SOFTWARE_SELECTION},
    export, ipc,
    lease_file::LeaseFile,
};
use clap::{Parser, Subcommand};
use std::{
    fs::OpenOptions,
    io::{self, Read},
    os::unix::{
        fs::{OpenOptionsExt, PermissionsExt},
        process::CommandExt,
    },
    path::{Path, PathBuf},
    process::Command,
    time::{Duration, Instant},
};
#[derive(Parser)]
#[command(
    name = "cordgeist",
    version,
    about = "Local USB phone controller — run setup to begin",
    long_about = "Single-phone workflow: daemon start → doctor → acquire → screenshot → tap/type → action status → release → daemon stop.\nSetup selects the single connected phone and validates its private mapping. A labeled software backend is never the production default."
)]
struct Cli {
    #[arg(long, global = true)]
    json: bool,
    #[arg(long, global = true)]
    runtime: Option<PathBuf>,
    #[arg(long, global = true)]
    autostart: bool,
    #[arg(long, global = true, default_value_t = 1200)]
    deadline_ms: u64,
    /// Opt in to a bounded screenshot after an action. Default returns receipt only.
    #[arg(long, global = true)]
    observe_after: bool,
    #[command(subcommand)]
    command: CommandKind,
}
#[derive(Subcommand)]
enum CommandKind {
    /// Version, boot and backend readiness. No credentials or images.
    Status,
    /// Missing requirements. Passing this command does not verify a phone.
    Doctor {
        #[arg(long)]
        device: Option<String>,
        #[arg(long)]
        adb: Option<PathBuf>,
        #[arg(long, default_value_t = 5037)]
        adb_port: u16,
    },
    /// Device inventory.
    #[command(alias = "inventory")]
    List,
    Daemon {
        #[command(subcommand)]
        action: DaemonCmd,
    },
    Acquire {
        #[arg(long)]
        device: String,
        #[arg(long)]
        reason: String,
        #[arg(long)]
        lease_file: PathBuf,
        #[arg(long, default_value_t = 60)]
        idle_ttl_s: u32,
    },
    Release {
        #[arg(long)]
        lease_file: PathBuf,
    },
    Lease {
        #[command(subcommand)]
        action: LeaseCmd,
    },
    Screenshot {
        #[arg(long)]
        lease_file: PathBuf,
        #[arg(long)]
        output: PathBuf,
        #[arg(long, default_value_t = 1280)]
        longest_edge: u32,
        #[arg(long, default_value_t = 0)]
        settle_ms: u32,
    },
    Tap {
        #[arg(long)]
        lease_file: PathBuf,
        #[arg(long)]
        frame: String,
        #[arg(long)]
        x: f64,
        #[arg(long)]
        y: f64,
    },
    Swipe {
        #[arg(long)]
        lease_file: PathBuf,
        #[arg(long)]
        frame: String,
        #[arg(long)]
        from: String,
        #[arg(long)]
        to: String,
        #[arg(long, default_value_t = 50)]
        duration_ms: u32,
    },
    Hold {
        #[arg(long)]
        lease_file: PathBuf,
        #[arg(long)]
        frame: String,
        #[arg(long)]
        x: f64,
        #[arg(long)]
        y: f64,
        #[arg(long)]
        duration_ms: u32,
    },
    /// Read the entire corpus from stdin without echo. The text is not logged.
    Type {
        #[arg(long)]
        lease_file: PathBuf,
        #[arg(long)]
        frame: String,
        #[arg(long)]
        stdin: bool,
    },
    Key {
        #[arg(long)]
        lease_file: PathBuf,
        #[arg(long)]
        frame: String,
        #[arg(long)]
        key: String,
    },
    Action {
        #[command(subcommand)]
        action: ActionCmd,
    },
    Setup {
        #[command(subcommand)]
        action: Option<SetupCmd>,
    },
    Media {
        #[command(subcommand)]
        action: MediaCmd,
    },
    /// Accept bytes on stdin into a private host staging file. Not a URL fetch.
    Stage {
        #[arg(long)]
        token_file: PathBuf,
        #[arg(long)]
        kind: String,
    },
    /// Official MCP adapter. Stdio is the default. Remote is loopback-only.
    Mcp {
        #[command(subcommand)]
        action: Option<McpCmd>,
    },
}
#[derive(Subcommand)]
enum MediaCmd {
    Push {
        #[arg(long)]
        lease_file: PathBuf,
        #[arg(long)]
        source: PathBuf,
        #[arg(long)]
        kind: String,
        #[arg(long)]
        seq: Option<u64>,
    },
}
#[derive(Subcommand)]
enum McpCmd {
    /// Authenticated loopback bridge. Put HTTPS in front with your own tunnel.
    Remote {
        #[arg(long)]
        bind: String,
        #[arg(long)]
        token_file: PathBuf,
    },
}
#[derive(Subcommand)]
enum DaemonCmd {
    Start,
    Run,
    Status,
    Stop,
}
#[derive(Subcommand)]
enum LeaseCmd {
    Revoke {
        #[arg(long)]
        device: String,
    },
}
#[derive(Subcommand)]
enum ActionCmd {
    Status {
        #[arg(long)]
        lease_file: PathBuf,
        #[arg(long)]
        seq: u64,
    },
    Cancel {
        #[arg(long)]
        lease_file: PathBuf,
        #[arg(long)]
        seq: u64,
    },
}
#[derive(Subcommand)]
enum SetupCmd {
    /// Serve the bundled synthetic test page; no phone input or runtime data.
    TestArea {
        #[arg(long, default_value_t = 18764)]
        port: u16,
        #[arg(long, default_value_t = 900)]
        seconds: u64,
    },
    /// Optional synthetic target verification; generic mapping needs no calibration.
    Calibrate {
        #[arg(long)]
        device: Option<String>,
        /// Probe only the deliberately opened synthetic calibration page.
        #[arg(long, alias = "verify")]
        probe: bool,
        #[arg(long, default_value_t = 60_000)]
        budget_ms: u64,
    },
}
pub fn run() -> i32 {
    let cli = Cli::parse();
    match execute(cli) {
        Ok(code) => code,
        Err(error) => {
            emit(
                false,
                &serde_json::json!({"ok": false, "code": "cli-error", "recovery": error.to_string()}),
            );
            2
        }
    }
}
fn emit(json: bool, value: &serde_json::Value) {
    if json {
        println!("{value}");
    }
}
fn execute(cli: Cli) -> io::Result<i32> {
    let Cli {
        json,
        runtime,
        autostart,
        deadline_ms,
        observe_after,
        command,
    } = cli;
    match command {
        CommandKind::Daemon {
            action: DaemonCmd::Run,
        } => {
            daemon::run(&runtime_dir(runtime)?)?;
            Ok(0)
        }
        CommandKind::Daemon {
            action: DaemonCmd::Start,
        } => {
            let runtime = runtime_dir(runtime)?;
            start_daemon(&runtime, json, true)
        }
        CommandKind::Daemon {
            action: DaemonCmd::Stop,
        } => {
            let runtime = runtime_dir(runtime)?;
            stop_daemon(&runtime, json)
        }
        CommandKind::Daemon {
            action: DaemonCmd::Status,
        }
        | CommandKind::Status => status(runtime, json),
        CommandKind::Doctor {
            device,
            adb,
            adb_port,
        } => doctor(runtime, device, adb, adb_port, json),
        CommandKind::Media {
            action:
                MediaCmd::Push {
                    lease_file,
                    source,
                    kind,
                    seq,
                },
        } => {
            let kind = media_kind(&kind)?;
            if let Some(seq) = seq {
                let Some(dir) = ensure(runtime, autostart, json)? else {
                    return Ok(2);
                };
                let auth = LeaseFile::open(&lease_file)?.auth()?;
                return call(
                    json,
                    &dir,
                    deadline_ms,
                    Call::MediaPush {
                        auth,
                        seq: ActionSeq(seq),
                        source: source.display().to_string(),
                        kind,
                    },
                    Some(seq),
                );
            }
            action_call(
                json,
                runtime,
                autostart,
                deadline_ms,
                observe_after,
                lease_file,
                |auth, seq| Call::MediaPush {
                    auth,
                    seq,
                    source: source.display().to_string(),
                    kind,
                },
            )
        }
        CommandKind::Stage { token_file, kind } => {
            let runtime = runtime_dir(runtime)?;
            let mut bytes = Vec::new();
            std::io::Read::read_to_end(
                &mut std::io::stdin().take(8 * 1024 * 1024 + 1),
                &mut bytes,
            )?;
            let Some(split) = bytes.iter().position(|b| *b == b'\n') else {
                return finish(
                    json,
                    &serde_json::json!({"ok":false,"code":"invalid-request","recovery":"Staging stdin is the token line, then the bytes."}),
                    2,
                );
            };
            let presented = &bytes[..split];
            let payload = &bytes[split + 1..];
            if !token_matches(&token_file, presented)? {
                return finish(
                    json,
                    &serde_json::json!({"ok":false,"code":"lease-invalid","recovery":"The staging token does not match the mode 0600 file."}),
                    2,
                );
            }
            let staged = crate::media::stage_bytes(&runtime, payload, media_kind(&kind)?)
                .map_err(|e| std::io::Error::other(e.to_string()))?;
            finish(
                json,
                &serde_json::json!({"ok":true,"staged": staged.display().to_string(), "imported": false}),
                0,
            )
        }
        CommandKind::Mcp { action } => {
            let runtime = runtime_dir(runtime)?;
            let result = match action {
                None => tokio::runtime::Runtime::new()?.block_on(crate::mcp::serve_stdio(runtime)),
                Some(McpCmd::Remote { bind, token_file }) => tokio::runtime::Runtime::new()?
                    .block_on(crate::mcp::serve_remote(runtime, &bind, &token_file)),
            };
            match result {
                Ok(()) => Ok(0),
                Err(error) => finish(
                    json,
                    &serde_json::json!({"ok": false, "code": "backend-unavailable", "recovery": error.to_string()}),
                    2,
                ),
            }
        }
        CommandKind::Setup { action: None } => {
            let Some(dir) = ensure(runtime, true, json)? else {
                return Ok(2);
            };
            call(json, &dir, 10_000, Call::Doctor { device: None }, None)
        }
        CommandKind::Setup {
            action: Some(SetupCmd::TestArea { port, seconds }),
        } => {
            crate::test_area::serve(port, seconds)?;
            Ok(0)
        }
        CommandKind::Setup {
            action:
                Some(SetupCmd::Calibrate {
                    device,
                    probe,
                    budget_ms,
                }),
        } => {
            let Some(dir) = ensure(runtime, autostart, json)? else {
                return Ok(2);
            };
            call(
                json,
                &dir,
                budget_ms,
                Call::Calibrate {
                    device: device.map(DeviceId),
                    probe,
                },
                None,
            )
        }
        CommandKind::List => {
            let Some(dir) = ensure(runtime, autostart, json)? else {
                return Ok(2);
            };
            call(json, &dir, deadline_ms, Call::List, None)
        }
        CommandKind::Acquire {
            device,
            reason,
            lease_file,
            idle_ttl_s,
        } => {
            let Some(dir) = ensure(runtime, autostart, json)? else {
                return Ok(2);
            };
            let device_id = device.clone();
            match exchange(
                &dir,
                deadline_ms,
                Call::Acquire {
                    device: DeviceId(device),
                    reason,
                    label: None,
                    idle_ttl_s,
                },
            )? {
                Reply::Ready(response) => {
                    if let ResponseOutcome::Success { result } = &response.outcome
                        && let OperationResult::Acquire(grant) = result.as_ref()
                    {
                        LeaseFile::create(&lease_file, grant)?;
                    }
                    present(json, response, None)
                }
                Reply::Lost { request_id } => lost(
                    json,
                    &request_id,
                    None,
                    &format!(
                        "The device may be leased. Run lease revoke --device {device_id}. Do not acquire again."
                    ),
                ),
            }
        }
        CommandKind::Release { lease_file } => {
            let Some(dir) = ensure(runtime, autostart, json)? else {
                return Ok(2);
            };
            let auth = LeaseFile::open(&lease_file)?.auth()?;
            call(json, &dir, deadline_ms, Call::Release { auth }, None)
        }
        CommandKind::Lease {
            action: LeaseCmd::Revoke { device },
        } => {
            let Some(dir) = ensure(runtime, autostart, json)? else {
                return Ok(2);
            };
            call(
                json,
                &dir,
                deadline_ms,
                Call::Revoke {
                    device: DeviceId(device),
                },
                None,
            )
        }
        CommandKind::Screenshot {
            lease_file,
            output,
            longest_edge,
            settle_ms,
        } => {
            let Some(dir) = ensure(runtime, autostart, json)? else {
                return Ok(2);
            };
            let auth = LeaseFile::open(&lease_file)?.auth()?;
            match exchange(
                &dir,
                10_000,
                Call::Screenshot {
                    auth,
                    longest_edge,
                    settle_ms,
                },
            )? {
                Reply::Ready(response) => {
                    if let ResponseOutcome::Success { result } = &response.outcome
                        && let OperationResult::Screenshot(shot) = result.as_ref()
                    {
                        export::publish(&shot.image, &shot.frame, &output, false)?;
                    }
                    present(json, response, None)
                }
                Reply::Lost { request_id } => lost(
                    json,
                    &request_id,
                    None,
                    "The reply was lost after the screenshot request was written. Do not send another capture automatically.",
                ),
            }
        }
        CommandKind::Tap {
            lease_file,
            frame,
            x,
            y,
        } => action_call(
            json,
            runtime,
            autostart,
            deadline_ms,
            observe_after,
            lease_file,
            |auth, seq| Call::Tap {
                auth,
                seq,
                frame_id: FrameId(frame),
                point: Point { x, y },
            },
        ),
        CommandKind::Swipe {
            lease_file,
            frame,
            from,
            to,
            duration_ms,
        } => {
            let from = parse_point(&from)?;
            let to = parse_point(&to)?;
            action_call(
                json,
                runtime,
                autostart,
                deadline_ms,
                observe_after,
                lease_file,
                |auth, seq| Call::Swipe {
                    auth,
                    seq,
                    frame_id: FrameId(frame),
                    from,
                    to,
                    duration_ms,
                },
            )
        }
        CommandKind::Hold {
            lease_file,
            frame,
            x,
            y,
            duration_ms,
        } => action_call(
            json,
            runtime.clone(),
            autostart,
            deadline_ms,
            observe_after,
            lease_file,
            |auth, seq| Call::Hold {
                auth,
                seq,
                frame_id: FrameId(frame),
                point: Point { x, y },
                duration_ms,
            },
        ),
        CommandKind::Type {
            lease_file,
            frame,
            stdin,
        } => {
            if !stdin {
                return finish(
                    json,
                    &serde_json::json!({"ok": false, "code": "invalid-request", "recovery": "Type requires --stdin."}),
                    2,
                );
            }
            let text = read_secret()?;
            action_call(
                json,
                runtime,
                autostart,
                deadline_ms,
                observe_after,
                lease_file,
                |auth, seq| Call::Type {
                    submit: false,
                    auth,
                    seq,
                    frame_id: FrameId(frame),
                    text,
                },
            )
        }
        CommandKind::Key {
            lease_file,
            frame,
            key,
        } => action_call(
            json,
            runtime,
            autostart,
            deadline_ms,
            observe_after,
            lease_file,
            |auth, seq| Call::Key {
                auth,
                seq,
                frame_id: FrameId(frame),
                key,
            },
        ),
        CommandKind::Action {
            action: ActionCmd::Status { lease_file, seq },
        } => {
            let Some(dir) = ensure(runtime, autostart, json)? else {
                return Ok(2);
            };
            let auth = LeaseFile::open(&lease_file)?.auth()?;
            call(
                json,
                &dir,
                deadline_ms,
                Call::ActionStatus {
                    auth,
                    seq: ActionSeq(seq),
                },
                Some(seq),
            )
        }
        CommandKind::Action {
            action: ActionCmd::Cancel { lease_file, seq },
        } => {
            let Some(dir) = ensure(runtime, autostart, json)? else {
                return Ok(2);
            };
            let auth = LeaseFile::open(&lease_file)?.auth()?;
            call(
                json,
                &dir,
                deadline_ms,
                Call::Cancel {
                    auth,
                    seq: ActionSeq(seq),
                },
                Some(seq),
            )
        }
    }
}
fn action_call(
    json: bool,
    runtime: Option<PathBuf>,
    autostart: bool,
    deadline_ms: u64,
    observe_after: bool,
    lease_file: PathBuf,
    build: impl FnOnce(LeaseAuth, ActionSeq) -> Call,
) -> io::Result<i32> {
    let Some(runtime) = ensure(runtime, autostart, json)? else {
        return Ok(2);
    };
    if !(1..=1500).contains(&deadline_ms) {
        return Err(io::ErrorKind::InvalidInput.into());
    }
    let lease = LeaseFile::open(&lease_file)?;
    let mut allocation = lease.allocate()?;
    let seq = allocation.seq();
    let call = build(allocation.auth().clone(), seq);
    let deadline = Instant::now() + Duration::from_millis(deadline_ms);
    match ipc::client::exchange_observing(&socket(&runtime), call, deadline, observe_after) {
        Ok(response) => {
            let uncertain = matches!(&response.outcome, ResponseOutcome::Error { error } if error.code == ErrorCode::ProcessTimeout);
            allocation.commit_sent(uncertain)?;
            present(json, response, Some(seq.0))
        }
        Err(ipc::client::ClientError::Uncertain(uncertain)) => {
            allocation.commit_sent(true)?;
            finish(
                json,
                &serde_json::json!({
                    "ok": false,
                    "code": "process-timeout",
                    "uncertain": true,
                    "requestId": uncertain.request_id,
                    "seq": seq.0,
                    "recovery": "Inspect action status for this sequence. Do not submit a new sequence."
                }),
                3,
            )
        }
        Err(error) => Err(io::Error::other(format!("{error:?}"))),
    }
}
enum Reply {
    Ready(Response),
    Lost { request_id: String },
}
fn call(
    json: bool,
    runtime: &Path,
    deadline_ms: u64,
    call: Call,
    seq: Option<u64>,
) -> io::Result<i32> {
    match exchange(runtime, deadline_ms, call)? {
        Reply::Ready(response) => present(json, response, seq),
        Reply::Lost { request_id } => lost(
            json,
            &request_id,
            seq,
            "The reply was lost after the request was written. Do not send it again.",
        ),
    }
}
fn exchange(runtime: &Path, deadline_ms: u64, call: Call) -> io::Result<Reply> {
    let cap = if call.is_app_launch() {
        APP_LAUNCH_TIMEOUT_MS
    } else if matches!(call, Call::Calibrate { .. }) {
        60_000
    } else if matches!(call, Call::Screenshot { .. }) {
        10_000
    } else {
        1500
    };
    if !(1..=cap).contains(&deadline_ms) {
        return Err(io::ErrorKind::InvalidInput.into());
    }
    let deadline = Instant::now() + Duration::from_millis(deadline_ms);
    match ipc::client::exchange(&socket(runtime), call, deadline) {
        Ok(response) => Ok(Reply::Ready(response)),
        Err(ipc::client::ClientError::Uncertain(uncertain)) => Ok(Reply::Lost {
            request_id: uncertain.request_id,
        }),
        Err(ipc::client::ClientError::Expired) => Err(io::ErrorKind::TimedOut.into()),
        Err(ipc::client::ClientError::Incompatible) => Err(io::Error::other("incompatible daemon")),
        Err(ipc::client::ClientError::Rejected(code)) => Err(io::Error::other(code.to_string())),
        Err(ipc::client::ClientError::Unavailable(error)) => Err(error),
    }
}
fn lost(json: bool, request_id: &str, seq: Option<u64>, recovery: &str) -> io::Result<i32> {
    let mut value = serde_json::json!({
        "ok": false,
        "code": "process-timeout",
        "uncertain": true,
        "requestId": request_id,
        "recovery": recovery,
    });
    if let Some(seq) = seq {
        value["seq"] = serde_json::json!(seq);
    }
    finish(json, &value, 3)
}
fn present(json: bool, response: Response, seq: Option<u64>) -> io::Result<i32> {
    let mut value = serde_json::to_value(&response).map_err(|_| io::Error::other("encode"))?;
    redact(&mut value);
    let code = match &response.outcome {
        ResponseOutcome::Success { result } if matches!(result.as_ref(), OperationResult::Calibrate(r) if !r.verified) => {
            2
        }
        ResponseOutcome::Success { .. } => 0,
        ResponseOutcome::Error { error } if error.code == ErrorCode::ProcessTimeout => 3,
        ResponseOutcome::Error { .. } => 2,
    };
    if json {
        println!("{value}");
    } else if let ResponseOutcome::Error { error } = &response.outcome {
        if code == 3
            && let Some(seq) = seq
        {
            eprintln!("seq {seq}");
        }
        eprintln!(
            "kind={} stage={}: {}",
            error.code, error.phase, error.recovery
        );
    } else if let ResponseOutcome::Success { result } = &response.outcome
        && let OperationResult::Calibrate(calibration) = result.as_ref()
    {
        println!("{}", calibration.recovery);
    } else {
        if let ResponseOutcome::Success { result } = &response.outcome
            && let OperationResult::Tap(action)
            | OperationResult::Swipe(action)
            | OperationResult::Hold(action)
            | OperationResult::Type(action)
            | OperationResult::Key(action)
            | OperationResult::Android(action) = result.as_ref()
        {
            for warning in &action.warnings {
                eprintln!("{warning}");
            }
        }
        println!("ok {}", response_method(&response));
    }
    Ok(code)
}
fn response_method(response: &Response) -> &str {
    match &response.outcome {
        ResponseOutcome::Success { result } => result.method(),
        ResponseOutcome::Error { .. } => "error",
    }
}
fn finish(json: bool, value: &serde_json::Value, code: i32) -> io::Result<i32> {
    if json {
        println!("{value}");
    } else {
        if code == 3
            && let Some(seq) = value.get("seq").and_then(|v| v.as_u64())
        {
            eprintln!("seq {seq}");
        }
        if let Some(recovery) = value.get("recovery").and_then(|v| v.as_str()) {
            eprintln!("{recovery}");
        }
    }
    Ok(code)
}
fn redact(value: &mut serde_json::Value) {
    match value {
        serde_json::Value::Object(map) => {
            for (key, child) in map.iter_mut() {
                if matches!(
                    key.as_str(),
                    "credential" | "base64" | "text" | "serial" | "product"
                ) {
                    *child = serde_json::Value::String("redacted".into());
                } else {
                    redact(child);
                }
            }
        }
        serde_json::Value::Array(items) => items.iter_mut().for_each(redact),
        _ => {}
    }
}
fn parse_point(text: &str) -> io::Result<Point> {
    let (x, y) = text
        .split_once(',')
        .ok_or_else(|| io::Error::from(io::ErrorKind::InvalidInput))?;
    Ok(Point {
        x: x.parse()
            .map_err(|_| io::Error::from(io::ErrorKind::InvalidInput))?,
        y: y.parse()
            .map_err(|_| io::Error::from(io::ErrorKind::InvalidInput))?,
    })
}
struct EchoRestore(Option<libc::termios>);
impl Drop for EchoRestore {
    fn drop(&mut self) {
        if let Some(saved) = self.0.take() {
            unsafe {
                libc::tcsetattr(0, libc::TCSANOW, &saved);
            }
        }
    }
}
fn media_kind(kind: &str) -> io::Result<MediaKind> {
    match kind {
        "image" => Ok(MediaKind::Image),
        "video" => Ok(MediaKind::Video),
        _ => Err(io::Error::new(
            io::ErrorKind::InvalidInput,
            "kind must be image or video",
        )),
    }
}
fn token_matches(path: &Path, presented: &[u8]) -> io::Result<bool> {
    use std::os::unix::fs::MetadataExt;
    let meta = std::fs::symlink_metadata(path)?;
    if !meta.is_file() || meta.uid() != unsafe { libc::geteuid() } || meta.mode() & 0o777 != 0o600 {
        return Err(io::Error::from(io::ErrorKind::PermissionDenied));
    }
    let mut token = std::fs::read(path)?;
    while token.last() == Some(&b'\n') {
        token.pop();
    }
    if token.len() != presented.len() {
        return Ok(false);
    }
    let mut diff = 0u8;
    for (a, b) in token.iter().zip(presented) {
        diff |= a ^ b;
    }
    Ok(diff == 0)
}
fn read_secret() -> io::Result<String> {
    let mut saved = std::mem::MaybeUninit::<libc::termios>::uninit();
    let tty = unsafe { libc::tcgetattr(0, saved.as_mut_ptr()) } == 0;
    let _restore = EchoRestore(if tty {
        let saved = unsafe { saved.assume_init() };
        let mut quiet = saved;
        quiet.c_lflag &= !libc::ECHO;
        unsafe {
            libc::tcsetattr(0, libc::TCSANOW, &quiet);
        }
        Some(saved)
    } else {
        None
    });
    let mut bytes = Vec::new();
    io::stdin().take(4096).read_to_end(&mut bytes)?;
    if bytes.len() == 4096 {
        return Err(io::ErrorKind::InvalidData.into());
    }
    while bytes.last() == Some(&b'\n') {
        bytes.pop();
    }
    if bytes.is_empty() || bytes.contains(&b'\n') || !bytes.is_ascii() {
        return Err(io::ErrorKind::InvalidData.into());
    }
    String::from_utf8(bytes).map_err(|_| io::ErrorKind::InvalidData.into())
}
fn socket(runtime: &Path) -> PathBuf {
    runtime.join("cordgeist").join("daemon.sock")
}
fn runtime_dir(given: Option<PathBuf>) -> io::Result<PathBuf> {
    if let Some(path) = given {
        prepare(&path)?;
        return Ok(path);
    }
    if let Some(path) = std::env::var_os("XDG_RUNTIME_DIR") {
        let path = PathBuf::from(path);
        if prepare(&path).is_ok() {
            return Ok(path);
        }
    }
    Err(io::Error::other(
        "pass --runtime DIR, mode 0700 and owned by you",
    ))
}
fn prepare(path: &Path) -> io::Result<()> {
    if !path.is_absolute() {
        return Err(io::ErrorKind::PermissionDenied.into());
    }
    std::fs::create_dir_all(path)?;
    std::fs::set_permissions(path, std::fs::Permissions::from_mode(0o700))?;
    let meta = std::fs::symlink_metadata(path)?;
    if !meta.is_dir() || meta.permissions().mode() & 0o777 != 0o700 {
        return Err(io::ErrorKind::PermissionDenied.into());
    }
    Ok(())
}
fn ensure(runtime: Option<PathBuf>, autostart: bool, json: bool) -> io::Result<Option<PathBuf>> {
    let runtime = runtime_dir(runtime)?;
    if autostart && start_daemon(&runtime, json, false)? == 2 {
        return Ok(None);
    }
    Ok(Some(runtime))
}
fn start_daemon(runtime: &Path, json: bool, announce: bool) -> io::Result<i32> {
    if ipc::client::exchange(
        &socket(runtime),
        Call::Status,
        Instant::now() + Duration::from_millis(200),
    )
    .is_ok()
    {
        return if announce {
            finish(
                json,
                &serde_json::json!({"ok": true, "daemon": "running"}),
                0,
            )
        } else {
            Ok(0)
        };
    }
    let mut child = Command::new(std::env::current_exe()?);
    child
        .args(["daemon", "run", "--runtime"])
        .arg(runtime)
        .env_clear()
        .env("PATH", std::env::var_os("PATH").unwrap_or_default())
        .stdin(std::process::Stdio::null())
        .stdout(std::process::Stdio::null())
        .stderr(std::process::Stdio::null());
    if std::env::var("CORDGEIST_SOFTWARE_BACKEND").as_deref() == Ok(SOFTWARE_SELECTION) {
        child.env("CORDGEIST_SOFTWARE_BACKEND", SOFTWARE_SELECTION);
        #[cfg(feature = "fake")]
        if let Ok(mode) = std::env::var("CORDGEIST_SOFTWARE_UI_UNAVAILABLE")
            && matches!(mode.as_str(), "1" | "slow")
        {
            child.env("CORDGEIST_SOFTWARE_UI_UNAVAILABLE", mode);
        }
        if let Ok(hold) = std::env::var("CORDGEIST_SOFTWARE_HOLD_MS")
            && hold.len() < 8
            && hold.bytes().all(|b| b.is_ascii_digit())
        {
            child.env("CORDGEIST_SOFTWARE_HOLD_MS", hold);
        }
        if let Ok(delay) = std::env::var("CORDGEIST_SOFTWARE_DELAY_MS")
            && delay.len() < 8
            && delay.bytes().all(|b| b.is_ascii_digit())
        {
            child.env("CORDGEIST_SOFTWARE_DELAY_MS", delay);
        }
        if let Ok(log) = std::env::var("CORDGEIST_EFFECT_LOG")
            && Path::new(&log).is_absolute()
        {
            child.env("CORDGEIST_EFFECT_LOG", log);
        }
    }
    if let Ok(adb) = std::env::var("CORDGEIST_ADB")
        && Path::new(&adb).is_absolute()
    {
        child.env("CORDGEIST_ADB", adb);
    }
    if let Ok(port) = std::env::var("CORDGEIST_ADB_PORT")
        && port.len() < 8
        && port.bytes().all(|b| b.is_ascii_digit())
    {
        child.env("CORDGEIST_ADB_PORT", port);
    }
    // SAFETY: setsid runs only in the forked child, before exec.
    unsafe {
        child.pre_exec(|| {
            if libc::setsid() < 0 {
                return Err(io::Error::last_os_error());
            }
            Ok(())
        });
    }
    let mut child = child.spawn()?;
    let ready_ms = if std::env::var("CORDGEIST_ADB").is_ok() {
        20_000
    } else {
        2000
    };
    let end = Instant::now() + Duration::from_millis(ready_ms);
    loop {
        if ipc::client::exchange(
            &socket(runtime),
            Call::Status,
            Instant::now() + Duration::from_millis(200),
        )
        .is_ok()
        {
            return if announce {
                finish(
                    json,
                    &serde_json::json!({"ok": true, "daemon": "started"}),
                    0,
                )
            } else {
                Ok(0)
            };
        }
        if let Some(status) = child.try_wait()?
            && !status.success()
        {
            return finish(
                json,
                &serde_json::json!({"ok": false, "code": "backend-unavailable", "recovery": "Daemon did not stay running."}),
                2,
            );
        }
        if Instant::now() >= end {
            let _ = child.kill();
            return finish(
                json,
                &serde_json::json!({"ok": false, "code": "process-timeout", "recovery": "Daemon did not become ready."}),
                2,
            );
        }
        std::thread::sleep(Duration::from_millis(20));
    }
}
fn stop_daemon(runtime: &Path, json: bool) -> io::Result<i32> {
    let path = runtime.join("cordgeist").join("stop");
    if !socket(runtime).exists() {
        return finish(
            json,
            &serde_json::json!({"ok": false, "code": "backend-unavailable", "recovery": "No daemon socket is present."}),
            2,
        );
    }
    OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .open(&path)?;
    let end = Instant::now() + Duration::from_millis(2000);
    while socket(runtime).exists() && Instant::now() < end {
        std::thread::sleep(Duration::from_millis(20));
    }
    if socket(runtime).exists() {
        return finish(
            json,
            &serde_json::json!({"ok": false, "code": "backend-unavailable", "recovery": "The daemon socket is still present."}),
            2,
        );
    }
    finish(
        json,
        &serde_json::json!({"ok": true, "daemon": "stopped"}),
        0,
    )
}
fn status(runtime: Option<PathBuf>, json: bool) -> io::Result<i32> {
    let running = runtime.as_ref().is_some_and(|dir| socket(dir).exists());
    let mut report = serde_json::json!({
        "version": env!("CARGO_PKG_VERSION"),
        "backend": "unavailable",
        "nativeCompiled": cfg!(feature = "native-aoa"),
        "nativeSessionImplemented": cfg!(feature = "native-aoa"),
        "hardwareVerified": false,
        "daemonImplemented": true,
        "daemonRunning": running,
        "usbfsVisible": Path::new("/dev/bus/usb").is_dir(),
    });
    if let Some(dir) = runtime
        && let Ok(response) = ipc::client::exchange(
            &socket(&dir),
            Call::Status,
            Instant::now() + Duration::from_millis(1500),
        )
        && let ResponseOutcome::Success { result } = &response.outcome
        && let OperationResult::Status(status) = result.as_ref()
    {
        report["backend"] = serde_json::json!(status.backend);
        report["ready"] = serde_json::json!(status.ready);
        report["daemonRunning"] = serde_json::json!(true);
    }
    if json {
        println!("{report}");
    } else {
        println!(
            "Cordgeist {}: backend {}; hardware unverified.",
            env!("CARGO_PKG_VERSION"),
            report["backend"].as_str().unwrap_or("unavailable")
        );
    }
    Ok(if report["ready"].as_bool() == Some(true) {
        0
    } else {
        2
    })
}
fn doctor(
    _runtime_path: Option<PathBuf>,
    device: Option<String>,
    adb_path: Option<PathBuf>,
    port: u16,
    json: bool,
) -> io::Result<i32> {
    if device.is_some() {
        return finish(
            json,
            &serde_json::json!({"ok":false,"code":"invalid-request","recovery":"doctor inspects exactly one attached phone; omit --device. Named selection remains available through the daemon API."}),
            2,
        );
    }
    let report = tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()?
        .block_on(crate::doctor::collect(
            adb_path.or_else(crate::adb::configured_path),
            port,
        ));
    if json {
        println!("{}", serde_json::to_string_pretty(&report)?);
    } else {
        for check in &report.checks {
            println!(
                "{}. {} {}: {}. {}",
                check.number, check.status, check.code, check.detail, check.fix
            );
        }
    }
    Ok(if report.ready { 0 } else { 2 })
}
