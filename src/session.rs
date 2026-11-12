//! Serialized native owner with bounded tracked reports; no gestures or lease authority.
//! Drivers and all their handles are created, called and dropped on this thread.
use crate::{
    api::ErrorCode,
    backend::Cancel,
    session_input::{Delivery, ReportDriver, ReportResult, TrackedInput, Transition},
};
use std::{
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicU8, Ordering},
        mpsc,
    },
    time::{Duration, Instant},
};
use tokio::sync::Notify;

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Resource {
    Context,
    Device,
    TouchSpec,
    KeyboardSpec,
    TouchNode,
    KeyboardNode,
}
const OPEN: [Resource; 6] = [
    Resource::Context,
    Resource::Device,
    Resource::TouchSpec,
    Resource::KeyboardSpec,
    Resource::TouchNode,
    Resource::KeyboardNode,
];
const CLOSE: [Resource; 6] = [
    Resource::KeyboardNode,
    Resource::TouchNode,
    Resource::KeyboardSpec,
    Resource::TouchSpec,
    Resource::Device,
    Resource::Context,
];
/// Scalar diagnostics copied before another native call replaces thread-local data.
/// Raw diagnostic strings and private identity/payload data are never included.
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Failure {
    pub code: ErrorCode,
    pub stage: &'static str,
    pub native_code: i32,
    pub transport_status: i32,
    pub request: i32,
    pub node_id: u16,
    pub report_id: u16,
    pub offset: u32,
    pub length: u32,
}
impl Failure {
    pub fn code(code: ErrorCode) -> Self {
        Self {
            code,
            stage: "session",
            native_code: 0,
            transport_status: 0,
            request: 0,
            node_id: 0,
            report_id: 0,
            offset: 0,
            length: 0,
        }
    }
    pub fn at(code: ErrorCode, stage: &'static str) -> Self {
        Self {
            stage,
            ..Self::code(code)
        }
    }
    /// Only owned stage names and numeric status codes; no foreign strings,
    /// identifiers, offsets, lengths, buffers or payloads.
    pub fn is_backend_failure(&self) -> bool {
        self.native_code != 0
            || self.transport_status != 0
            || matches!(
                self.code,
                ErrorCode::BackendUnavailable | ErrorCode::DeviceDisconnected
            )
    }
    pub fn soft_deadline(&self) -> bool {
        self.native_code == 0
            && self.transport_status == 0
            && matches!(
                self.code,
                ErrorCode::ProcessTimeout | ErrorCode::ProcessCancelled
            )
    }
    pub fn reason(&self) -> String {
        format!(
            "kind={} stage={} native-code={} libusb-status={} aoa-request={} deadline={}",
            self.code,
            self.stage,
            self.native_code,
            self.transport_status,
            self.request,
            if self.code == ErrorCode::ProcessTimeout {
                "action deadline exhausted"
            } else {
                "not hit"
            }
        )
    }
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CloseOutcome {
    Pending,
    /// Parameter/unset-field errors retain ownership too. Never free or retry
    /// these automatically: the adapter invariant must be repaired first.
    Retained(Failure),
    Consumed(Result<(), Failure>),
}
/// Each foreign call receives nonzero bounded arguments. The foreign library may
/// have per-request/per-object bounds rather than a whole-call deadline.
#[derive(Clone, Copy, Debug)]
pub struct CallBudget {
    #[cfg_attr(
        not(feature = "native-aoa"),
        expect(dead_code, reason = "native adapter rechecks after validation")
    )]
    deadline: Instant,
    pub control_ms: u32,
    pub drain_ms: u32,
}
impl CallBudget {
    fn before(deadline: Instant) -> Result<Self, Failure> {
        let ms = deadline
            .saturating_duration_since(Instant::now())
            .as_millis();
        if ms == 0 {
            return Err(Failure::code(ErrorCode::ProcessTimeout));
        }
        Ok(Self {
            deadline,
            control_ms: ms.min(500) as u32,
            drain_ms: ms.min(1000) as u32,
        })
    }
    #[cfg(feature = "native-aoa")]
    pub(crate) fn remaining(self) -> Result<Self, Failure> {
        Self::before(self.deadline)
    }
    #[cfg(feature = "native-aoa")]
    pub(crate) fn deadline(self) -> Instant {
        self.deadline
    }
    fn cleanup() -> Self {
        Self {
            deadline: Instant::now() + Duration::from_secs(2),
            control_ms: 500,
            drain_ms: 1000,
        }
    }
}
pub struct Handles<H>([Option<H>; 6]);
impl<H> Handles<H> {
    pub fn get(&self, resource: Resource) -> Option<&H> {
        self.0[resource as usize].as_ref()
    }
}
/// Trusted adapter boundary, not a client extension/authorization mechanism.
/// Intentionally no Send bound on either driver or handle.
pub trait SessionDriver {
    type Handle;
    fn check_settling(&mut self, _: &Cancel) -> Result<(), Failure> {
        Ok(())
    }
    fn validate_report(&mut self, _: &Transition, _: &Cancel, _: &Cancel) -> Result<(), Failure> {
        Ok(())
    }
    fn apply_report(
        &mut self,
        _: &Transition,
        _: &Handles<Self::Handle>,
        _: &Cancel,
        _: &Cancel,
    ) -> Result<(), Failure> {
        Err(Failure::code(ErrorCode::BackendUnavailable))
    }
    fn submit_report(
        &mut self,
        _: &Transition,
        _: &Handles<Self::Handle>,
        _: CallBudget,
        _: &Cancel,
        _: &Cancel,
    ) -> ReportResult {
        ReportResult {
            delivery: Delivery::NotSent,
            error: Some(Failure::code(ErrorCode::BackendUnavailable)),
        }
    }
    fn open(
        &mut self,
        resource: Resource,
        handles: &Handles<Self::Handle>,
        budget: CallBudget,
        cancel: &Cancel,
    ) -> Result<Self::Handle, Failure>;
    fn close(
        &mut self,
        resource: Resource,
        handle: &mut Self::Handle,
        budget: CallBudget,
    ) -> CloseOutcome;
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Phase {
    Initializing,
    Settling,
    Ready,
    Closing,
    Quarantined,
    Closed,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct Status {
    pub resources_acquired: bool,
    pub settling_elapsed: bool,
    pub hardware_verified: bool,
    pub phase: Phase,
    pub initialization_error: Option<Failure>,
    pub cleanup_error: Option<Failure>,
    pub terminal_error: Option<Failure>,
}
const CLOSING: u8 = 1;
const RUNNING: u8 = 2;
pub(crate) type Authorization = Arc<dyn Fn() -> Result<(), ErrorCode> + Send + Sync>;
struct Command {
    authorization: Option<Authorization>,
    external: Cancel,
    transition: Transition,
    deadline: Instant,
    cancel: Cancel,
    reply: tokio::sync::oneshot::Sender<ReportResult>,
}
struct Reports<'a, D: SessionDriver> {
    authorization: &'a Option<Authorization>,
    driver: &'a mut D,
    handles: &'a Handles<D::Handle>,
    cancel: &'a Cancel,
    deadline: Instant,
    external: &'a Cancel,
}
impl<D: SessionDriver> ReportDriver for Reports<'_, D> {
    fn cancelled(&self) -> bool {
        self.cancel.cancelled() || self.external.cancelled()
    }
    fn apply(&mut self, t: &Transition) -> Result<(), Failure> {
        if let Some(check) = self.authorization {
            check().map_err(Failure::code)?;
        }
        self.driver
            .apply_report(t, self.handles, self.cancel, self.external)
    }
    fn submit(&mut self, t: &Transition, ms: u32) -> ReportResult {
        let _ = ms;
        if let Some(check) = self.authorization
            && let Err(code) = check()
        {
            return ReportResult {
                delivery: Delivery::NotSent,
                error: Some(Failure::code(code)),
            };
        }
        match CallBudget::before(self.deadline) {
            Ok(budget) => {
                self.driver
                    .submit_report(t, self.handles, budget, self.cancel, self.external)
            }
            Err(error) => ReportResult {
                delivery: Delivery::NotSent,
                error: Some(error),
            },
        }
    }
}
struct Shared {
    current: Mutex<Option<Cancel>>,
    busy: AtomicBool,
    lifecycle: AtomicU8,
    status: Mutex<Status>,
    changed: Notify,
    cancel: Cancel,
}
impl Shared {
    fn stop(&self) {
        self.lifecycle.fetch_or(CLOSING, Ordering::SeqCst);
        self.cancel.cancel();
        if let Some(cancel) = self.current.lock().unwrap().as_ref() {
            cancel.cancel();
        }
    }
    fn publish(&self, f: impl FnOnce(&mut Status)) {
        f(&mut self.status.lock().unwrap());
        self.changed.notify_waiters();
    }
}
struct Client(Arc<Shared>, mpsc::SyncSender<Command>);
impl Drop for Client {
    fn drop(&mut self) {
        self.0.stop();
    }
}
#[derive(Clone)]
pub struct Session {
    client: Arc<Client>,
}
fn valid(budget: Duration) -> bool {
    !budget.is_zero() && budget <= Duration::from_secs(10)
}
#[derive(Clone, Copy)]
pub struct Registration {
    pub settling: Duration,
}
impl Default for Registration {
    fn default() -> Self {
        Self {
            settling: Duration::from_millis(100),
        }
    }
}
impl Session {
    pub fn start<D, F>(budget: Duration, factory: F) -> Result<Self, ErrorCode>
    where
        D: SessionDriver + 'static,
        F: FnOnce() -> Result<D, Failure> + Send + 'static,
    {
        Self::start_with_registration(budget, Registration::default(), factory)
    }
    pub fn start_with_registration<D, F>(
        budget: Duration,
        registration: Registration,
        factory: F,
    ) -> Result<Self, ErrorCode>
    where
        D: SessionDriver + 'static,
        F: FnOnce() -> Result<D, Failure> + Send + 'static,
    {
        if !valid(budget) || registration.settling > Duration::from_secs(5) {
            return Err(ErrorCode::InvalidRequest);
        }
        let (sender, receiver) = mpsc::sync_channel::<Command>(1);
        let shared = Arc::new(Shared {
            busy: AtomicBool::new(false),
            current: Mutex::new(None),
            lifecycle: AtomicU8::new(0),
            cancel: Cancel::default(),
            changed: Notify::new(),
            status: Mutex::new(Status {
                resources_acquired: false,
                settling_elapsed: false,
                hardware_verified: false,
                phase: Phase::Initializing,
                initialization_error: None,
                cleanup_error: None,
                terminal_error: None,
            }),
        });
        let owner = shared.clone();
        let deadline = Instant::now() + budget;
        std::thread::Builder::new()
            .name("cordgeist-session".into())
            .spawn(move || {
                let mut driver = match factory() {
                    Ok(d) => d,
                    Err(e) => {
                        owner.publish(|s| {
                            s.initialization_error = Some(e);
                            s.phase = Phase::Closed;
                        });
                        return;
                    }
                };
                let mut handles = Handles(std::array::from_fn(|_| None));
                for resource in OPEN {
                    let result = CallBudget::before(deadline).and_then(|budget| {
                        // Linearizes admission against shutdown; no lock spans native I/O.
                        if owner
                            .lifecycle
                            .compare_exchange(0, RUNNING, Ordering::SeqCst, Ordering::SeqCst)
                            .is_err()
                        {
                            return Err(Failure::code(ErrorCode::ProcessCancelled));
                        }
                        let result = driver.open(resource, &handles, budget, &owner.cancel);
                        owner.lifecycle.fetch_and(!RUNNING, Ordering::SeqCst);
                        result
                    });
                    match result {
                        Ok(handle) => handles.0[resource as usize] = Some(handle),
                        Err(e) => {
                            owner.publish(|s| s.initialization_error = Some(e));
                            owner.stop();
                            break;
                        }
                    }
                    if Instant::now() >= deadline || owner.cancel.cancelled() {
                        owner.publish(|s| {
                            s.initialization_error =
                                Some(Failure::code(if owner.cancel.cancelled() {
                                    ErrorCode::ProcessCancelled
                                } else {
                                    ErrorCode::ProcessTimeout
                                }))
                        });
                        owner.stop();
                        break;
                    }
                }
                if handles.0.iter().all(Option::is_some)
                    && owner.lifecycle.load(Ordering::SeqCst) & CLOSING == 0
                {
                    owner.publish(|s| {
                        s.resources_acquired = true;
                        s.phase = Phase::Settling;
                    });
                    let until = Instant::now() + registration.settling;
                    loop {
                        if owner.cancel.cancelled()
                            || owner.lifecycle.load(Ordering::SeqCst) & CLOSING != 0
                        {
                            break;
                        }
                        if Instant::now() >= deadline {
                            owner.publish(|s| {
                                s.initialization_error =
                                    Some(Failure::code(ErrorCode::ProcessTimeout))
                            });
                            owner.stop();
                            break;
                        }
                        if owner
                            .lifecycle
                            .compare_exchange(0, RUNNING, Ordering::SeqCst, Ordering::SeqCst)
                            .is_err()
                        {
                            break;
                        }
                        let result = driver.check_settling(&owner.cancel);
                        owner.lifecycle.fetch_and(!RUNNING, Ordering::SeqCst);
                        if let Err(error) = result {
                            owner.publish(|s| {
                                s.terminal_error.get_or_insert(error);
                            });
                            owner.stop();
                            break;
                        }
                        if owner.cancel.cancelled()
                            || owner.lifecycle.load(Ordering::SeqCst) & CLOSING != 0
                        {
                            break;
                        }
                        if Instant::now() >= deadline {
                            owner.publish(|s| {
                                s.initialization_error =
                                    Some(Failure::code(ErrorCode::ProcessTimeout))
                            });
                            owner.stop();
                            break;
                        }
                        if Instant::now() >= until {
                            owner.publish(|s| {
                                if owner.lifecycle.load(Ordering::SeqCst) & CLOSING == 0 {
                                    s.settling_elapsed = true;
                                }
                            });
                            break;
                        }
                        std::thread::park_timeout(
                            until
                                .saturating_duration_since(Instant::now())
                                .min(Duration::from_millis(5)),
                        );
                    }
                }
                owner.publish(|s| {
                    // Final promotion check: foreign validation or executor/OS
                    // delay must not publish readiness past initialization expiry.
                    if Instant::now() >= deadline
                        && owner.lifecycle.load(Ordering::SeqCst) & CLOSING == 0
                    {
                        s.initialization_error = Some(Failure::code(ErrorCode::ProcessTimeout));
                        owner.stop();
                    }
                    if !s.settling_elapsed {
                        owner.stop();
                    }
                    s.phase = if owner.lifecycle.load(Ordering::SeqCst) & CLOSING != 0
                        || !s.settling_elapsed
                    {
                        Phase::Closing
                    } else {
                        Phase::Ready
                    };
                });
                let mut tracked = TrackedInput::default();
                while owner.lifecycle.load(Ordering::SeqCst) & CLOSING == 0 {
                    let command = match receiver.recv_timeout(Duration::from_millis(5)) {
                        Ok(c) => c,
                        Err(mpsc::RecvTimeoutError::Timeout) => continue,
                        Err(mpsc::RecvTimeoutError::Disconnected) => break,
                    };
                    let result = if owner
                        .lifecycle
                        .compare_exchange(0, RUNNING, Ordering::SeqCst, Ordering::SeqCst)
                        .is_ok()
                    {
                        let android = matches!(command.transition, Transition::Android(_));
                        let validation = command
                            .authorization
                            .as_ref()
                            .map_or(Ok(()), |check| check().map_err(Failure::code))
                            .and_then(|()| {
                                driver.validate_report(
                                    &command.transition,
                                    &command.cancel,
                                    &command.external,
                                )
                            });
                        let mut result = match validation {
                            Err(error) => {
                                // InvalidRequest/UnsupportedCharacter are reserved for harmless payload preflight.
                                // Authority/health/cancellation failures permanently fence
                                // the owner even when this command itself sent nothing.
                                if !error.soft_deadline()
                                    && !matches!(
                                        error.code,
                                        ErrorCode::InvalidRequest | ErrorCode::UnsupportedCharacter
                                    )
                                {
                                    owner.publish(|s| {
                                        s.terminal_error.get_or_insert(error);
                                    });
                                    owner.stop();
                                }
                                ReportResult {
                                    delivery: Delivery::NotSent,
                                    error: Some(error),
                                }
                            }
                            Ok(()) => tracked.execute(
                                &mut Reports {
                                    authorization: &command.authorization,
                                    driver: &mut driver,
                                    handles: &handles,
                                    cancel: &command.cancel,
                                    deadline: command.deadline,
                                    external: &command.external,
                                },
                                command.transition,
                                command.deadline,
                                &command.cancel,
                            ),
                        };
                        owner.lifecycle.fetch_and(!RUNNING, Ordering::SeqCst);
                        if Instant::now() >= command.deadline
                            && !result.error.is_some_and(|e| e.is_backend_failure())
                        {
                            result.delivery = Delivery::Unknown;
                            result.error =
                                Some(Failure::at(ErrorCode::ProcessTimeout, "owner-completion"));
                        }
                        let mut recovery_failed = false;
                        if !android && result.error.is_some_and(|e| e.soft_deadline()) {
                            // The owner drains/neutralizes and replaces ONLY its input
                            // nodes. Never replay the failed command or share authority.
                            match reset_input_nodes(&mut driver, &mut handles) {
                                Ok(()) => tracked = TrackedInput::default(),
                                Err(error) => {
                                    result.error = Some(error);
                                    recovery_failed = true;
                                    owner.publish(|s| {
                                        s.cleanup_error.get_or_insert(error);
                                    });
                                }
                            }
                        }
                        if tracked.quarantined() || recovery_failed {
                            if let Some(error) = result.error {
                                owner.publish(|s| {
                                    s.terminal_error.get_or_insert(error);
                                });
                            }
                            owner.stop();
                        }
                        result
                    } else {
                        ReportResult {
                            delivery: Delivery::NotSent,
                            error: Some(Failure::code(ErrorCode::ProcessCancelled)),
                        }
                    };
                    *owner.current.lock().unwrap() = None;
                    owner.busy.store(false, Ordering::SeqCst);
                    let _ = command.reply.send(result);
                }
                owner.publish(|s| s.phase = Phase::Closing);
                for resource in CLOSE {
                    while let Some(handle) = handles.0[resource as usize].as_mut() {
                        match driver.close(resource, handle, CallBudget::cleanup()) {
                            CloseOutcome::Pending => {
                                owner.publish(|s| s.phase = Phase::Quarantined);
                                std::thread::park_timeout(Duration::from_millis(25));
                            }
                            CloseOutcome::Retained(error) => {
                                owner.publish(|s| {
                                    s.phase = Phase::Quarantined;
                                    s.cleanup_error.get_or_insert(error);
                                });
                                // Keep the driver and every live handle on this owner.
                                // A retained parameter error is not safe to retry or free.
                                loop {
                                    std::thread::park();
                                }
                            }
                            CloseOutcome::Consumed(result) => {
                                // Clear exactly once, including consumed failures. Never retry.
                                handles.0[resource as usize] = None;
                                if let Err(error) = result {
                                    owner.publish(|s| {
                                        s.cleanup_error.get_or_insert(error);
                                    });
                                }
                            }
                        }
                    }
                }
                owner.publish(|s| s.phase = Phase::Closed);
            })
            .map_err(|_| ErrorCode::BackendUnavailable)?;
        Ok(Self {
            client: Arc::new(Client(shared, sender)),
        })
    }
    pub async fn submit(
        &self,
        transition: Transition,
        budget: Duration,
        cancel: &Cancel,
    ) -> ReportResult {
        let max_budget = if matches!(&transition, Transition::Text(_)) {
            Duration::from_secs(120)
        } else {
            Duration::from_secs(10)
        };
        if budget.is_zero() || budget > max_budget {
            return ReportResult {
                delivery: Delivery::NotSent,
                error: Some(Failure::code(ErrorCode::InvalidRequest)),
            };
        }
        self.submit_authorized_until(transition, Instant::now() + budget, cancel, None)
            .await
    }
    pub(crate) async fn submit_authorized_until(
        &self,
        transition: Transition,
        deadline: Instant,
        cancel: &Cancel,
        authorization: Option<Authorization>,
    ) -> ReportResult {
        let reject = |code| ReportResult {
            delivery: Delivery::NotSent,
            error: Some(Failure::code(code)),
        };
        let max_budget = if matches!(&transition, Transition::Text(_)) {
            Duration::from_secs(120)
        } else {
            Duration::from_secs(10)
        };
        let Some(remaining) = deadline
            .checked_duration_since(Instant::now())
            .filter(|d| !d.is_zero())
        else {
            return reject(ErrorCode::ProcessTimeout);
        };
        if remaining > max_budget {
            return reject(ErrorCode::InvalidRequest);
        }
        if cancel.cancelled() {
            return reject(ErrorCode::ProcessCancelled);
        }
        if self.status().phase != Phase::Ready
            || self.client.0.lifecycle.load(Ordering::SeqCst) & CLOSING != 0
        {
            return reject(ErrorCode::BackendUnavailable);
        }
        if self
            .client
            .0
            .busy
            .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
            .is_err()
        {
            return reject(ErrorCode::QueueFull);
        }
        if Instant::now() >= deadline {
            self.client.0.busy.store(false, Ordering::SeqCst);
            return reject(ErrorCode::ProcessTimeout);
        }
        let work_cancel = Cancel::default();
        *self.client.0.current.lock().unwrap() = Some(work_cancel.clone());
        let (reply, mut receive) = tokio::sync::oneshot::channel();
        let mut guard = ReadyGuard {
            shared: self.client.0.clone(),
            armed: true,
        };
        if self
            .client
            .1
            .try_send(Command {
                authorization,
                external: cancel.clone(),
                transition,
                deadline,
                cancel: work_cancel.clone(),
                reply,
            })
            .is_err()
        {
            self.client.0.busy.store(false, Ordering::SeqCst);
            return reject(ErrorCode::BackendUnavailable);
        }
        loop {
            if cancel.cancelled() || Instant::now() >= deadline {
                if Instant::now() >= deadline {
                    // Deadline uncertainty cancels this command, not the owner.
                    // The owner finishes bounded drain/neutralization before reuse.
                    guard.armed = false;
                    work_cancel.cancel();
                }
                return ReportResult {
                    delivery: Delivery::Unknown,
                    error: Some(Failure::at(
                        if Instant::now() >= deadline {
                            ErrorCode::ProcessTimeout
                        } else {
                            ErrorCode::ProcessCancelled
                        },
                        "session-wait",
                    )),
                };
            }
            tokio::select! {
                result=&mut receive=>{
                    if cancel.cancelled() || Instant::now()>=deadline {continue;}
                    guard.armed=false;
                    return result.unwrap_or(ReportResult {delivery:Delivery::Unknown,error:Some(Failure::code(ErrorCode::BackendUnavailable))});
                },
                _=tokio::time::sleep(Duration::from_millis(2))=>{}
            }
        }
    }
    pub fn status(&self) -> Status {
        let mut status = *self.client.0.status.lock().unwrap();
        if status.phase == Phase::Ready
            && self.client.0.lifecycle.load(Ordering::SeqCst) & CLOSING != 0
        {
            status.phase = Phase::Closing;
        }
        status
    }
    pub async fn ready(&self, budget: Duration, cancel: &Cancel) -> Result<(), ErrorCode> {
        if !valid(budget) {
            return Err(ErrorCode::InvalidRequest);
        }
        let deadline = Instant::now() + budget;
        let mut guard = ReadyGuard {
            shared: self.client.0.clone(),
            armed: true,
        };
        loop {
            if cancel.cancelled() {
                return Err(ErrorCode::ProcessCancelled);
            }
            if Instant::now() >= deadline {
                return Err(ErrorCode::ProcessTimeout);
            }
            let status = self.status();
            if let Some(e) = status.initialization_error.or(status.terminal_error) {
                return Err(e.code);
            }
            match status.phase {
                Phase::Ready if self.client.0.lifecycle.load(Ordering::SeqCst) & CLOSING == 0 => {
                    guard.armed = false;
                    return Ok(());
                }
                Phase::Initializing | Phase::Settling => {
                    tokio::time::sleep(Duration::from_millis(2)).await
                }
                _ => return Err(ErrorCode::BackendUnavailable),
            }
        }
    }
    /// Caller wait is bounded; cleanup continues on the sole owner after timeout.
    /// Repeated shutdown returns cached status, never reuses consumed pointers.
    /// Queue-independent signal only; cleanup still runs on the existing owner.
    pub(crate) fn request_shutdown(&self) {
        self.client.0.stop();
    }
    pub async fn shutdown(&self, budget: Duration) -> Result<Status, ErrorCode> {
        if !valid(budget) {
            return Err(ErrorCode::InvalidRequest);
        }
        self.client.0.stop();
        let deadline = Instant::now() + budget;
        loop {
            let notified = self.client.0.changed.notified();
            tokio::pin!(notified);
            notified.as_mut().enable();
            if Instant::now() >= deadline {
                return Err(ErrorCode::NativeCleanupPending);
            }
            let status = self.status();
            if status.phase == Phase::Closed {
                return Ok(status);
            }
            if tokio::time::timeout_at(tokio::time::Instant::from_std(deadline), notified)
                .await
                .is_err()
            {
                return Err(ErrorCode::NativeCleanupPending);
            }
        }
    }
}
/// Close drains callbacks and sends neutral reports before unregistering. A
/// bounded failure retains ownership and is a real cleanup/backend failure.
fn reset_input_nodes<D: SessionDriver>(
    driver: &mut D,
    handles: &mut Handles<D::Handle>,
) -> Result<(), Failure> {
    let deadline = Instant::now() + Duration::from_secs(2);
    for resource in [Resource::KeyboardNode, Resource::TouchNode] {
        while let Some(handle) = handles.0[resource as usize].as_mut() {
            if Instant::now() >= deadline {
                return Err(Failure::at(
                    ErrorCode::BackendUnavailable,
                    "deadline-neutralization",
                ));
            }
            match driver.close(resource, handle, CallBudget::cleanup()) {
                CloseOutcome::Pending => std::thread::sleep(Duration::from_millis(2)),
                CloseOutcome::Retained(error) => return Err(error),
                CloseOutcome::Consumed(result) => {
                    handles.0[resource as usize] = None;
                    result?;
                }
            }
        }
    }
    let cancel = Cancel::default();
    for resource in [Resource::TouchNode, Resource::KeyboardNode] {
        let handle = driver.open(resource, handles, CallBudget::before(deadline)?, &cancel)?;
        handles.0[resource as usize] = Some(handle);
    }
    Ok(())
}

struct ReadyGuard {
    shared: Arc<Shared>,
    armed: bool,
}
impl Drop for ReadyGuard {
    fn drop(&mut self) {
        if self.armed {
            self.shared.stop();
        }
    }
}
