//! Software-only local lifecycle. No backend, leases or native handles are wired.
use super::{
    endpoint::Endpoint,
    framing::{self, MESSAGE_TIMEOUT, SCREENSHOT_TIMEOUT},
};
use crate::api::{
    ApiError, BootId, Call, ErrorCode, ProtocolVersion, Request, Response, ResponseOutcome,
};
use serde::{Deserialize, Serialize};
use std::{
    io,
    os::unix::net::UnixStream,
    path::Path,
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    thread,
    time::{Duration, Instant},
};

pub const MAX_CONNECTIONS: usize = 8;
#[derive(Serialize, Deserialize)]
#[serde(deny_unknown_fields)]
pub struct Hello {
    pub version: u32,
}
#[derive(Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
pub struct Welcome {
    pub version: ProtocolVersion,
    pub boot_id: BootId,
    pub compatible: bool,
    pub device_control_available: bool,
}
#[derive(Clone)]
pub struct Stop(Arc<AtomicBool>, Option<Arc<dyn Dispatcher>>);
impl Stop {
    pub fn request(&self) {
        if let Some(dispatcher) = &self.1 {
            dispatcher.begin_stop();
        }
        self.0.store(true, Ordering::SeqCst);
    }
}
pub struct Server {
    endpoint: Endpoint,
    stop: Stop,
    boot: BootId,
    dispatcher: Option<Arc<dyn Dispatcher>>,
}
impl Server {
    /// Successful return publishes readiness: endpoint and boot identity exist.
    pub fn bind(parent: &Path) -> io::Result<Self> {
        let mut bytes = [0u8; 32];
        getrandom::fill(&mut bytes).map_err(|_| io::Error::from(io::ErrorKind::Other))?;
        let boot = BootId(bytes.iter().map(|b| format!("{b:02x}")).collect());
        Ok(Self {
            endpoint: Endpoint::bind(parent)?,
            stop: Stop(Arc::new(AtomicBool::new(false)), None),
            boot,
            dispatcher: None,
        })
    }
    /// The dispatcher shares the endpoint boot identity and owns no socket or
    /// native handles. Default bind remains honestly unavailable.
    pub fn bind_dispatcher(parent: &Path, dispatcher: Arc<dyn Dispatcher>) -> io::Result<Self> {
        let mut server = Self::bind(parent)?;
        server.boot = dispatcher.boot_id().clone();
        server.stop.1 = Some(dispatcher.clone());
        server.dispatcher = Some(dispatcher);
        Ok(server)
    }
    pub fn stop_handle(&self) -> Stop {
        self.stop.clone()
    }
    pub fn boot_id(&self) -> &BootId {
        &self.boot
    }
    pub fn run(self) -> io::Result<()> {
        let mut workers: Vec<thread::JoinHandle<()>> = Vec::new();
        let result = loop {
            let mut i = 0;
            while i < workers.len() {
                if workers[i].is_finished() {
                    let _ = workers.swap_remove(i).join();
                } else {
                    i += 1;
                }
            }
            if self.stop.0.load(Ordering::SeqCst) {
                break Ok(());
            }
            match self.endpoint.listener().accept() {
                Ok((stream, _)) => {
                    if workers.len() >= MAX_CONNECTIONS {
                        drop(stream);
                        continue;
                    }
                    let boot = self.boot.clone();
                    let dispatcher = self.dispatcher.clone();
                    match thread::Builder::new()
                        .name("cordgeist-ipc".into())
                        .spawn(move || {
                            let _ = serve(stream, boot, dispatcher);
                        }) {
                        Ok(worker) => workers.push(worker),
                        Err(e) => break Err(e),
                    }
                }
                Err(e) if e.kind() == io::ErrorKind::WouldBlock => {
                    thread::sleep(Duration::from_millis(5))
                }
                Err(e) => break Err(e),
            }
        };
        if let Some(dispatcher) = &self.dispatcher {
            dispatcher.begin_stop();
        }
        // Each worker has one absolute exchange deadline; no unbounded idle clients.
        for worker in workers {
            let _ = worker.join();
        }
        if let Some(dispatcher) = &self.dispatcher {
            dispatcher.shutdown();
        }
        result
    }
}
pub trait Dispatcher: Send + Sync {
    fn boot_id(&self) -> &BootId;
    fn dispatch(&self, request: Request, exchange_deadline: Instant) -> Response;
    fn begin_stop(&self) {}
    fn shutdown(&self);
}
/// Calls enter the dedicated service runtime; IPC workers never become native
/// owners. Runtime lifetime is owned by the embedding service, not a client.
pub struct ServiceDispatcher {
    pub service: Arc<crate::service::Service>,
    pub runtime: tokio::runtime::Handle,
}
impl Dispatcher for ServiceDispatcher {
    fn boot_id(&self) -> &BootId {
        self.service.boot_id()
    }
    fn dispatch(&self, request: Request, exchange_deadline: Instant) -> Response {
        self.runtime
            .block_on(self.service.handle_until(request, exchange_deadline))
    }
    fn begin_stop(&self) {
        self.service.begin_stop();
    }
    fn shutdown(&self) {
        self.runtime.block_on(self.service.shutdown());
    }
}
fn serve(
    mut stream: UnixStream,
    boot: BootId,
    dispatcher: Option<Arc<dyn Dispatcher>>,
) -> io::Result<()> {
    let deadline = Instant::now() + MESSAGE_TIMEOUT;
    let hello: Hello = framing::receive(&mut stream, deadline)?;
    framing::send(
        &mut stream,
        &Welcome {
            version: ProtocolVersion,
            boot_id: boot.clone(),
            compatible: hello.version == 1,
            device_control_available: false,
        },
        deadline,
    )?;
    if hello.version != 1 {
        return Ok(());
    }
    let request: Request = framing::receive(&mut stream, deadline)?;
    if request.request_id.is_empty() || request.request_id.len() > 128 {
        return Err(io::ErrorKind::InvalidData.into());
    }
    if request.version == 1
        && request.deadline_ms > 0
        && let Some(dispatcher) = dispatcher
    {
        let response_deadline = if request.call.is_app_launch() {
            Instant::now()
                + Duration::from_millis(crate::api::APP_LAUNCH_TIMEOUT_MS.min(request.deadline_ms))
        } else if matches!(request.call, Call::Calibrate { .. }) {
            Instant::now()
                + super::framing::CALIBRATION_TIMEOUT
                    .min(Duration::from_millis(request.deadline_ms))
        } else if matches!(
            request.call,
            Call::Screenshot { .. } | Call::Elements { .. }
        ) {
            Instant::now() + SCREENSHOT_TIMEOUT.min(Duration::from_millis(request.deadline_ms))
        } else {
            deadline
        };
        let response = dispatcher.dispatch(request, response_deadline);
        return framing::send(&mut stream, &response, response_deadline);
    }
    let code = if request.version != 1 {
        ErrorCode::ProtocolVersionUnsupported
    } else if request.deadline_ms == 0 {
        ErrorCode::InvalidRequest
    } else {
        ErrorCode::BackendUnavailable
    };
    let response = Response::for_request(
        &request,
        boot,
        ResponseOutcome::Error {
            error: ApiError {
                code,
                phase: "ipc-foundation".into(),
                recovery: "Device control is unavailable in this software-only foundation.".into(),
            },
        },
    )
    .map_err(|_| io::Error::from(io::ErrorKind::InvalidData))?;
    framing::send(&mut stream, &response, deadline)
}
