//! Reusable local client. A mutation is never resent with a new identity.
use super::{
    framing::{self, MESSAGE_TIMEOUT, SCREENSHOT_TIMEOUT},
    server::{Hello, Welcome},
};
use crate::api::{BootId, Call, ErrorCode, ProtocolVersion, Request, Response};
use std::{
    io,
    os::unix::net::UnixStream,
    path::Path,
    time::{Duration, Instant},
};
const MAX_CONNECTS: u8 = 3;
/// Returned only after the request bytes were written. Callers keep the same
/// sequence and must inspect status instead of submitting a new action.
#[derive(Debug)]
pub struct Uncertain {
    pub request_id: String,
    pub boot_id: Option<BootId>,
}
#[derive(Debug)]
pub enum ClientError {
    Expired,
    Unavailable(io::Error),
    Incompatible,
    Rejected(ErrorCode),
    Uncertain(Uncertain),
}
pub fn exchange(path: &Path, call: Call, deadline: Instant) -> Result<Response, ClientError> {
    exchange_observing(path, call, deadline, false)
}
pub fn exchange_observing(
    path: &Path,
    call: Call,
    deadline: Instant,
    observe_after: bool,
) -> Result<Response, ClientError> {
    exchange_options(
        path,
        call,
        deadline,
        observe_after,
        crate::api::CoordinateSpace::ReturnedImagePixels,
    )
}
pub fn exchange_options(
    path: &Path,
    call: Call,
    deadline: Instant,
    observe_after: bool,
    coordinate_space: crate::api::CoordinateSpace,
) -> Result<Response, ClientError> {
    let mut attempts = 0;
    loop {
        if Instant::now() >= deadline {
            return Err(ClientError::Expired);
        }
        attempts += 1;
        match once(
            path,
            call.clone(),
            deadline,
            observe_after,
            coordinate_space,
        ) {
            Ok(response) => return Ok(response),
            Err(
                error @ (ClientError::Uncertain(_)
                | ClientError::Incompatible
                | ClientError::Rejected(_)),
            ) => {
                return Err(error);
            }
            Err(error) if attempts < MAX_CONNECTS => {
                let _ = error;
                std::thread::sleep(
                    Duration::from_millis(15)
                        .min(deadline.saturating_duration_since(Instant::now())),
                );
            }
            Err(error) => return Err(error),
        }
    }
}
fn once(
    path: &Path,
    call: Call,
    deadline: Instant,
    observe_after: bool,
    coordinate_space: crate::api::CoordinateSpace,
) -> Result<Response, ClientError> {
    // Handshake and dispatch share the daemon's existing exchange budget.
    let exchange_deadline = Instant::now()
        + if call.is_app_launch() {
            std::time::Duration::from_millis(crate::api::APP_LAUNCH_TIMEOUT_MS)
        } else if matches!(call, Call::Calibrate { .. }) {
            super::framing::CALIBRATION_TIMEOUT
        } else if matches!(call, Call::Screenshot { .. } | Call::Elements { .. }) {
            SCREENSHOT_TIMEOUT
        } else {
            MESSAGE_TIMEOUT
        };
    let deadline = deadline.min(exchange_deadline);
    if Instant::now() >= deadline {
        return Err(ClientError::Expired);
    }
    let mut stream = UnixStream::connect(path).map_err(ClientError::Unavailable)?;
    framing::send(&mut stream, &Hello { version: 1 }, deadline).map_err(before_send)?;
    let welcome: Welcome = framing::receive(&mut stream, deadline).map_err(before_send)?;
    if !welcome.compatible || welcome.version != ProtocolVersion {
        return Err(ClientError::Incompatible);
    }
    let remaining = deadline
        .saturating_duration_since(Instant::now())
        .as_millis();
    if remaining == 0 || remaining > u64::MAX as u128 {
        return Err(ClientError::Expired);
    }
    let mut id = [0u8; 16];
    getrandom::fill(&mut id).map_err(|_| ClientError::Unavailable(io::ErrorKind::Other.into()))?;
    let request_id = id.iter().map(|b| format!("{b:02x}")).collect::<String>();
    let request = Request {
        coordinate_space,
        observe_after,
        version: 1,
        request_id: request_id.clone(),
        deadline_ms: remaining as u64,
        call,
    };
    framing::send(&mut stream, &request, deadline).map_err(|error| {
        if error.kind() == io::ErrorKind::TimedOut {
            ClientError::Uncertain(Uncertain {
                request_id: request_id.clone(),
                boot_id: Some(welcome.boot_id.clone()),
            })
        } else {
            before_send(error)
        }
    })?;
    let response: Response = framing::receive(&mut stream, deadline).map_err(|_| {
        ClientError::Uncertain(Uncertain {
            request_id,
            boot_id: Some(welcome.boot_id.clone()),
        })
    })?;
    response
        .validate_for(&request, &welcome.boot_id)
        .map_err(ClientError::Rejected)?;
    Ok(response)
}
fn before_send(error: io::Error) -> ClientError {
    if error.kind() == io::ErrorKind::TimedOut {
        ClientError::Expired
    } else {
        ClientError::Unavailable(error)
    }
}
