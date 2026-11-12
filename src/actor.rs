//! Software scheduling over existing Session owners. No leases or native wiring.
use crate::{
    api::{DeviceId, ErrorCode, LeaseAuth, LeaseGrant},
    backend::{Cancel, Clock},
    lease::Authority,
    session::{Failure, Session, Status},
    session_input::{Delivery, ReportResult, Transition},
};
use std::{
    collections::VecDeque,
    sync::{
        Arc, Mutex,
        atomic::{AtomicU8, Ordering},
    },
    time::{Duration, Instant},
};
use tokio::sync::{Notify, oneshot};
pub const QUEUE_CAPACITY: usize = 16;
struct Work {
    observation_revision: Option<u64>,
    frame_id: Option<crate::api::FrameId>,
    auth: Option<LeaseAuth>,
    execution: Arc<AtomicU8>,
    transition: Transition,
    deadline: Instant,
    cancel: Cancel,
    reply: oneshot::Sender<ReportResult>,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TerminationReason {
    Released,
    OwnerCancelled,
    Revoked,
    IdleExpired,
    Disconnected,
    Shutdown,
    HealthInvalid,
    ActionDeadline,
    BackendFailure,
}
struct State {
    requires_reconnect: bool,
    reconnect: Option<crate::reconnect::Prerequisites>,
    termination: Option<TerminationReason>,
    authority: Option<Authority>,
    queue: VecDeque<Work>,
    active: Option<Cancel>,
    fenced: bool,
}
struct Shared {
    clock: Arc<dyn Clock + Send + Sync>,
    state: Mutex<State>,
    wake: Notify,
    session: Session,
}
#[derive(Clone)]
pub struct DeviceActor(Arc<Client>);
struct Client(Arc<Shared>);
impl std::ops::Deref for Client {
    type Target = Shared;
    fn deref(&self) -> &Shared {
        &self.0
    }
}
impl Drop for Client {
    fn drop(&mut self) {
        invalidate(&self.0);
    }
}
#[derive(Clone, Copy, Debug)]
pub struct ActorStatus {
    pub observation_ready: bool,
    pub termination: Option<TerminationReason>,
    pub fenced: bool,
    pub queued: usize,
    pub active: bool,
    pub session: Status,
}
fn invalidate(shared: &Shared) {
    let mut state = shared.state.lock().unwrap();
    fence(&mut state, TerminationReason::HealthInvalid);
    shared.wake.notify_one();
}
fn fence(state: &mut State, reason: TerminationReason) {
    state.termination.get_or_insert(reason);
    if matches!(
        reason,
        TerminationReason::Disconnected | TerminationReason::BackendFailure
    ) {
        state.requires_reconnect = true;
    }
    state.fenced = true;
    if let Some(authority) = &mut state.authority {
        authority.invalidate();
    }
    if let Some(cancel) = &state.active {
        cancel.cancel();
    }
    for work in state.queue.drain(..) {
        let _ = work.reply.send(reject(ErrorCode::LeaseInvalid));
    }
}
pub struct Ticket {
    origin: std::sync::Weak<Shared>,
    execution: Arc<AtomicU8>,
    reply: Option<oneshot::Receiver<ReportResult>>,
    cancel: Cancel,
    deadline: Instant,
}
fn reject(code: ErrorCode) -> ReportResult {
    ReportResult {
        delivery: Delivery::NotSent,
        error: Some(Failure::code(code)),
    }
}
impl Ticket {
    pub(crate) fn cancellation(&self) -> Cancel {
        self.cancel.clone()
    }
    pub(crate) fn claimed(&self) -> Arc<AtomicU8> {
        self.execution.clone()
    }
    pub fn cancel(&self) {
        self.cancel.cancel();
    }
    pub async fn wait(mut self) -> ReportResult {
        tokio::time::timeout_at(
            tokio::time::Instant::from_std(self.deadline),
            self.reply.take().unwrap(),
        )
        .await
        .map_or_else(
            |_| self.cancel_without_reply(ErrorCode::ProcessTimeout),
            |r| r.unwrap_or_else(|_| self.cancel_without_reply(ErrorCode::BackendUnavailable)),
        )
    }
    fn cancel_without_reply(&self, code: ErrorCode) -> ReportResult {
        self.cancel.cancel();
        let prevented = self
            .execution
            .compare_exchange(0, 2, Ordering::SeqCst, Ordering::SeqCst)
            .is_ok();
        ReportResult {
            delivery: if prevented {
                Delivery::NotSent
            } else {
                Delivery::Unknown
            },
            error: Some(Failure::at(code, "actor-wait")),
        }
    }
}
impl Drop for Ticket {
    fn drop(&mut self) {
        self.cancel.cancel();
    }
}
impl DeviceActor {
    /// Session already owns all handles; this task schedules owned commands only.
    pub fn start(session: Session) -> Self {
        Self::build(session, None, Arc::new(Monotonic(Instant::now())))
    }
    pub(crate) fn build(
        session: Session,
        authority: Option<Authority>,
        clock: Arc<dyn Clock + Send + Sync>,
    ) -> Self {
        Self::build_inner(session, authority, clock, None)
    }
    pub(crate) fn build_inner(
        session: Session,
        authority: Option<Authority>,
        clock: Arc<dyn Clock + Send + Sync>,
        reconnect: Option<crate::reconnect::Prerequisites>,
    ) -> Self {
        let shared = Arc::new(Shared {
            clock,
            state: Mutex::new(State {
                requires_reconnect: reconnect.is_some(),
                reconnect,
                termination: None,
                authority,
                queue: VecDeque::new(),
                active: None,
                fenced: false,
            }),
            wake: Notify::new(),
            session,
        });
        tokio::spawn(run(shared.clone()));
        Self(Arc::new(Client(shared)))
    }
    pub fn status(&self) -> ActorStatus {
        let state = self.0.state.lock().unwrap();
        ActorStatus {
            observation_ready: !state.fenced
                && state
                    .reconnect
                    .as_ref()
                    .is_none_or(|r| r.ready(self.0.clock.now_ms())),
            termination: state.termination,
            fenced: state.fenced,
            queued: state.queue.len(),
            active: state.active.is_some(),
            session: self.0.session.status(),
        }
    }
    pub fn enqueue(&self, transition: Transition, budget: Duration) -> Result<Ticket, ErrorCode> {
        self.enqueue_with_auth(None, None, transition, budget)
    }
    pub(crate) fn enqueue_owner(
        &self,
        auth: LeaseAuth,
        transition: Transition,
        budget: Duration,
    ) -> Result<Ticket, ErrorCode> {
        self.enqueue_with_auth(Some(auth), None, transition, budget)
    }
    pub(crate) fn enqueue_owner_until(
        &self,
        auth: LeaseAuth,
        transition: Transition,
        deadline: Instant,
    ) -> Result<Ticket, ErrorCode> {
        let remaining = deadline
            .checked_duration_since(Instant::now())
            .filter(|d| !d.is_zero())
            .ok_or(ErrorCode::ProcessTimeout)?;
        if remaining > Duration::from_secs(10) {
            return Err(ErrorCode::InvalidRequest);
        }
        self.enqueue_with_deadline(Some(auth), None, transition, remaining, Some(deadline))
    }
    fn enqueue_with_auth(
        &self,
        auth: Option<LeaseAuth>,
        frame_id: Option<crate::api::FrameId>,
        transition: Transition,
        budget: Duration,
    ) -> Result<Ticket, ErrorCode> {
        if budget.is_zero() || budget > Duration::from_secs(10) {
            return Err(ErrorCode::InvalidRequest);
        }
        self.enqueue_with_deadline(auth, frame_id, transition, budget, None)
    }
    // The legacy relative-budget API starts at admission completion. Integrated
    // requests supply Some(original_deadline), which can never be renewed here.
    fn enqueue_with_deadline(
        &self,
        auth: Option<LeaseAuth>,
        frame_id: Option<crate::api::FrameId>,
        transition: Transition,
        budget: Duration,
        deadline: Option<Instant>,
    ) -> Result<Ticket, ErrorCode> {
        self.expire_idle();
        validate_payload(&transition)?;
        let mut state = self.0.state.lock().unwrap();
        if state.fenced || self.0.session.status().phase != crate::session::Phase::Ready {
            return Err(ErrorCode::LeaseInvalid);
        }
        let active = state.active.is_some();
        if let Some(authority) = &mut state.authority {
            authority.check(
                auth.as_ref().ok_or(ErrorCode::LeaseInvalid)?,
                self.0.clock.now_ms(),
                active,
            )?;
        }
        let observation_revision = if let Some(reconnect) = &state.reconnect {
            if matches!(&transition, Transition::Android(_)) {
                reconnect.check_identity(auth.as_ref().ok_or(ErrorCode::LeaseInvalid)?)?;
                None
            } else {
                Some(reconnect.bind(
                    auth.as_ref().ok_or(ErrorCode::LeaseInvalid)?,
                    frame_id.as_ref(),
                    self.0.clock.now_ms(),
                )?)
            }
        } else {
            None
        };
        if state.queue.len() >= QUEUE_CAPACITY {
            return Err(ErrorCode::QueueFull);
        }
        if let Some(authority) = &mut state.authority {
            authority.refresh(
                auth.as_ref().ok_or(ErrorCode::LeaseInvalid)?,
                self.0.clock.now_ms(),
                active,
            )?;
        }
        let deadline = match deadline {
            Some(deadline) if Instant::now() >= deadline => return Err(ErrorCode::ProcessTimeout),
            Some(deadline) => deadline,
            None => Instant::now() + budget,
        };
        let cancel = Cancel::default();
        let (tx, rx) = oneshot::channel();
        let execution = Arc::new(AtomicU8::new(0));
        state.queue.push_back(Work {
            observation_revision,
            frame_id,
            auth,
            execution: execution.clone(),
            transition,
            deadline,
            cancel: cancel.clone(),
            reply: tx,
        });
        self.0.wake.notify_one();
        Ok(Ticket {
            origin: Arc::downgrade(&self.0.0),
            execution,
            reply: Some(rx),
            cancel,
            deadline,
        })
    }
    /// Constant-space, queue-independent terminal authority/health/shutdown signal.
    pub fn invalidate(&self) {
        invalidate(&self.0);
    }
    pub async fn shutdown(&self, budget: Duration) -> Result<Status, ErrorCode> {
        self.terminate(TerminationReason::Shutdown);
        let status = self.0.session.shutdown(budget).await?;
        // Closed comes only from the existing owner after all consuming closes.
        // Also resolves stale scheduling state if its Tokio task was aborted.
        if status.phase == crate::session::Phase::Closed {
            self.0.state.lock().unwrap().active = None;
        }
        Ok(status)
    }
}
async fn run(shared: Arc<Shared>) {
    loop {
        expire(&shared);
        let notified = shared.wake.notified();
        let (work, fenced) = {
            let mut state = shared.state.lock().unwrap();
            let session = shared.session.status();
            if session.cleanup_error.is_some()
                || session
                    .terminal_error
                    .is_some_and(|e| e.is_backend_failure())
            {
                fence(&mut state, TerminationReason::BackendFailure);
            }
            if state
                .authority
                .as_ref()
                .is_some_and(|a| a.expired(shared.clock.now_ms(), state.active.is_some()))
            {
                fence(&mut state, TerminationReason::IdleExpired);
            }
            let work = state.queue.pop_front();
            if let Some(w) = &work {
                state.active = Some(w.cancel.clone());
            }
            (work, state.fenced)
        };
        if fenced {
            let _ = shared.session.shutdown(Duration::from_secs(1)).await;
            return;
        }
        if let Some(work) = work {
            let route_only = matches!(&work.transition, Transition::Android(_));
            let retire_after = route_only || matches!(&work.transition, Transition::TextSubmit(_));
            let authorization: Option<crate::session::Authorization> =
                work.auth.clone().map(|auth| {
                    let weak = Arc::downgrade(&shared);
                    let frame_id = work.frame_id.clone();
                    let observation_revision = work.observation_revision;
                    Arc::new(move || {
                        let shared = weak.upgrade().ok_or(ErrorCode::LeaseInvalid)?;
                        let state = shared.state.lock().unwrap();
                        if state.fenced {
                            return Err(ErrorCode::LeaseInvalid);
                        }
                        if let Some(reconnect) = &state.reconnect {
                            if route_only {
                                reconnect.check_identity(&auth)?;
                            } else {
                                reconnect.check_revision(
                                    &auth,
                                    frame_id.as_ref(),
                                    observation_revision,
                                    shared.clock.now_ms(),
                                )?;
                            }
                        }
                        state
                            .authority
                            .as_ref()
                            .ok_or(ErrorCode::LeaseInvalid)?
                            .check(&auth, shared.clock.now_ms(), state.active.is_some())
                    }) as crate::session::Authorization
                });
            let mut dispatch_rejected = false;
            let result = if work
                .execution
                .compare_exchange(0, 1, Ordering::SeqCst, Ordering::SeqCst)
                .is_err()
            {
                reject(ErrorCode::ProcessTimeout)
            } else if work.cancel.cancelled() {
                reject(ErrorCode::ProcessCancelled)
            } else if Instant::now() >= work.deadline {
                reject(ErrorCode::ProcessTimeout)
            } else {
                match authorization.as_ref().map_or(Ok(()), |check| check()) {
                    Err(code) => {
                        dispatch_rejected = true;
                        reject(code)
                    }
                    Ok(()) => {
                        shared
                            .session
                            .submit_authorized_until(
                                work.transition,
                                work.deadline,
                                &work.cancel,
                                authorization,
                            )
                            .await
                    }
                }
            };
            let backend_failure = result.error.is_some_and(|e| e.is_backend_failure())
                || shared
                    .session
                    .status()
                    .terminal_error
                    .is_some_and(|e| e.is_backend_failure());
            {
                let mut state = shared.state.lock().unwrap();
                if let (Some(authority), Some(auth)) = (&mut state.authority, &work.auth) {
                    let _ = authority.refresh(auth, shared.clock.now_ms(), true);
                }
                state.active = None;
            }
            if dispatch_rejected
                || backend_failure
                || shared.session.status().phase != crate::session::Phase::Ready
            {
                let reason = if backend_failure || shared.session.status().cleanup_error.is_some() {
                    TerminationReason::BackendFailure
                } else {
                    // Authority rejection/clean cancellation fences ownership,
                    // but is not evidence that the USB backend failed.
                    TerminationReason::Released
                };
                let mut state = shared.state.lock().unwrap();
                fence(&mut state, reason);
                shared.wake.notify_one();
            }
            if retire_after {
                let mut state = shared.state.lock().unwrap();
                if let Some(reconnect) = state.reconnect.as_mut() {
                    let _ = reconnect.retire_observation();
                }
            }
            let _ = work.reply.send(result);
        } else {
            let _ = tokio::time::timeout(Duration::from_millis(5), notified).await;
        }
    }
}

/// Admission limits only; Session remains authoritative for state/descriptor checks.
fn validate_payload(t: &Transition) -> Result<(), ErrorCode> {
    match t {
        Transition::Android(command) => command.validate()?,
        Transition::Text(text) | Transition::TextSubmit(text) => {
            text.plan()?;
        }
        Transition::Gesture(gesture) => {
            gesture.plan()?;
        }
        Transition::Batch(items)
            if items.is_empty()
                || items.len() > 64
                || items
                    .iter()
                    .any(|t| !matches!(t, Transition::Key { .. } | Transition::Contact { .. })) =>
        {
            return Err(ErrorCode::InvalidRequest);
        }
        _ => {}
    }
    Ok(())
}
#[derive(Clone, Copy)]
pub enum Control {
    CancelActive,
    Revoke,
    HealthInvalid,
    Shutdown,
}
impl DeviceActor {
    /// Trusted owner control, not lease authorization. Coalesced, constant space.
    pub fn control(&self, signal: Control) {
        match signal {
            Control::CancelActive => {
                if let Some(cancel) = &self.0.state.lock().unwrap().active {
                    cancel.cancel();
                }
            }
            Control::Revoke => self.terminate(TerminationReason::Revoked),
            Control::HealthInvalid => self.terminate(TerminationReason::HealthInvalid),
            Control::Shutdown => self.terminate(TerminationReason::Shutdown),
        }
    }
}

struct Monotonic(Instant);
impl Clock for Monotonic {
    fn now_ms(&self) -> u64 {
        self.0.elapsed().as_millis().min(u128::from(u64::MAX)) as u64
    }
}
fn expire(shared: &Shared) {
    let mut state = shared.state.lock().unwrap();
    if state
        .authority
        .as_ref()
        .is_some_and(|a| a.expired(shared.clock.now_ms(), state.active.is_some()))
    {
        fence(&mut state, TerminationReason::IdleExpired);
        shared.wake.notify_one();
    }
}

impl DeviceActor {
    pub(crate) fn expire_idle(&self) {
        expire(&self.0);
    }
    pub(crate) fn acquire(&self, device: &DeviceId, ttl_ms: u64) -> Result<LeaseGrant, ErrorCode> {
        self.expire_idle();
        let mut state = self.0.state.lock().unwrap();
        let status = self.0.session.status();
        if state.fenced
            || status.phase != crate::session::Phase::Ready
            || status.cleanup_error.is_some()
        {
            return Err(ErrorCode::BackendUnavailable);
        }
        state
            .authority
            .as_mut()
            .ok_or(ErrorCode::BackendUnavailable)?
            .acquire(device, self.0.clock.now_ms(), ttl_ms)
    }
    pub(crate) fn refresh_owner(&self, auth: &LeaseAuth) -> Result<(), ErrorCode> {
        self.expire_idle();
        let mut state = self.0.state.lock().unwrap();
        if state.fenced || self.0.session.status().phase != crate::session::Phase::Ready {
            return Err(ErrorCode::LeaseInvalid);
        }
        let active = state.active.is_some();
        state
            .authority
            .as_mut()
            .ok_or(ErrorCode::LeaseInvalid)?
            .refresh(auth, self.0.clock.now_ms(), active)
    }
}

impl DeviceActor {
    pub(crate) fn permits_successor(
        &self,
        boot: &crate::api::BootId,
        attachment: crate::api::AttachmentGeneration,
        fence: crate::api::Fence,
    ) -> bool {
        let state = self.0.state.lock().unwrap();
        let status = self.0.session.status();
        state.fenced
            && !state.requires_reconnect
            && state.active.is_none()
            && state.queue.is_empty()
            && status.phase == crate::session::Phase::Closed
            && status.cleanup_error.is_none()
            && state
                .authority
                .as_ref()
                .is_some_and(|a| a.permits_successor(boot, attachment, fence))
    }
}

impl DeviceActor {
    pub(crate) fn terminate(&self, reason: TerminationReason) {
        let mut state = self.0.state.lock().unwrap();
        fence(&mut state, reason);
        self.0.wake.notify_one();
    }
    pub(crate) fn terminate_owner(
        &self,
        auth: &LeaseAuth,
        reason: TerminationReason,
    ) -> Result<(), ErrorCode> {
        let mut state = self.0.state.lock().unwrap();
        if !state.authority.as_ref().is_some_and(|a| a.matches(auth)) {
            return Err(ErrorCode::LeaseInvalid);
        }
        fence(&mut state, reason);
        self.0.wake.notify_one();
        Ok(())
    }
    pub(crate) fn disconnect(
        &self,
        attachment: crate::api::AttachmentGeneration,
    ) -> Result<(), ErrorCode> {
        let mut state = self.0.state.lock().unwrap();
        if !state
            .authority
            .as_ref()
            .is_some_and(|a| a.attachment_matches(attachment))
        {
            return Err(ErrorCode::LeaseInvalid);
        }
        fence(&mut state, TerminationReason::Disconnected);
        self.0.wake.notify_one();
        Ok(())
    }
    pub(crate) fn cancel_owner_action(
        &self,
        auth: &LeaseAuth,
        ticket: &Ticket,
    ) -> Result<(), ErrorCode> {
        let state = self.0.state.lock().unwrap();
        if state.fenced
            || !ticket
                .origin
                .upgrade()
                .is_some_and(|origin| Arc::ptr_eq(&origin, &self.0.0))
        {
            return Err(ErrorCode::LeaseInvalid);
        }
        state
            .authority
            .as_ref()
            .ok_or(ErrorCode::LeaseInvalid)?
            .check(auth, self.0.clock.now_ms(), state.active.is_some())?;
        ticket.cancel();
        Ok(())
    }
}

/// Opaque worker-instance binding. It neither owns native handles nor authorizes input.
#[derive(Clone)]
pub struct WorkerToken(std::sync::Weak<Shared>);
impl DeviceActor {
    pub(crate) fn worker_token(&self) -> WorkerToken {
        WorkerToken(Arc::downgrade(&self.0.0))
    }
    pub(crate) fn worker_matches(&self, token: &WorkerToken) -> bool {
        token
            .0
            .upgrade()
            .is_some_and(|origin| Arc::ptr_eq(&origin, &self.0.0))
    }
    pub(crate) fn worker_failed(&self, token: &WorkerToken) -> Result<(), ErrorCode> {
        if !self.worker_matches(token) {
            return Err(ErrorCode::LeaseInvalid);
        }
        self.terminate(TerminationReason::BackendFailure);
        self.0.session.request_shutdown();
        Ok(())
    }
}

impl DeviceActor {
    pub(crate) fn enqueue_frame(
        &self,
        auth: LeaseAuth,
        frame_id: crate::api::FrameId,
        transition: Transition,
        budget: Duration,
    ) -> Result<Ticket, ErrorCode> {
        if self.0.state.lock().unwrap().reconnect.is_none() {
            return Err(ErrorCode::FrameExpired);
        }
        self.enqueue_with_auth(Some(auth), Some(frame_id), transition, budget)
    }
    pub(crate) fn enqueue_frame_until(
        &self,
        auth: LeaseAuth,
        frame_id: crate::api::FrameId,
        transition: Transition,
        deadline: Instant,
    ) -> Result<Ticket, ErrorCode> {
        if self.0.state.lock().unwrap().reconnect.is_none() {
            return Err(ErrorCode::FrameExpired);
        }
        let remaining = deadline
            .checked_duration_since(Instant::now())
            .filter(|d| !d.is_zero())
            .ok_or(ErrorCode::ProcessTimeout)?;
        if remaining > Duration::from_secs(10) {
            return Err(ErrorCode::InvalidRequest);
        }
        self.enqueue_with_deadline(
            Some(auth),
            Some(frame_id),
            transition,
            remaining,
            Some(deadline),
        )
    }
    pub(crate) fn retire_observation(
        &self,
        token: &WorkerToken,
        auth: &crate::api::LeaseAuth,
    ) -> Result<(), ErrorCode> {
        if !self.worker_matches(token) {
            return Err(ErrorCode::LeaseInvalid);
        }
        let mut state = self.0.state.lock().unwrap();
        if state.fenced {
            return Err(ErrorCode::LeaseInvalid);
        }
        state
            .authority
            .as_ref()
            .ok_or(ErrorCode::LeaseInvalid)?
            .check(auth, self.0.clock.now_ms(), state.active.is_some())?;
        state
            .reconnect
            .as_mut()
            .ok_or(ErrorCode::InvalidRequest)?
            .retire_observation()
    }
    pub(crate) fn publish_observation(
        &self,
        token: &WorkerToken,
        auth: &LeaseAuth,
        frame: crate::api::Frame,
        mapping: crate::geometry::Transform,
    ) -> Result<(), ErrorCode> {
        if !self.worker_matches(token) {
            return Err(ErrorCode::LeaseInvalid);
        }
        let mut state = self.0.state.lock().unwrap();
        if state.fenced {
            return Err(ErrorCode::LeaseInvalid);
        }
        state
            .authority
            .as_ref()
            .ok_or(ErrorCode::LeaseInvalid)?
            .check(auth, self.0.clock.now_ms(), state.active.is_some())?;
        state
            .reconnect
            .as_mut()
            .ok_or(ErrorCode::InvalidRequest)?
            .publish(auth, frame, mapping, self.0.clock.now_ms())
    }
    pub(crate) fn reconnect_fence(
        &self,
        boot: &crate::api::BootId,
        attachment: crate::api::AttachmentGeneration,
    ) -> Result<crate::api::Fence, ErrorCode> {
        let state = self.0.state.lock().unwrap();
        let status = self.0.session.status();
        if !state.fenced
            || state.active.is_some()
            || !state.queue.is_empty()
            || status.phase != crate::session::Phase::Closed
            || status.cleanup_error.is_some()
        {
            return Err(ErrorCode::BackendUnavailable);
        }
        state
            .authority
            .as_ref()
            .ok_or(ErrorCode::BackendUnavailable)?
            .reconnect_fence(boot, attachment)
    }
}

impl DeviceActor {
    pub(crate) fn receipt_owner(&self, auth: &LeaseAuth) -> bool {
        self.0
            .state
            .lock()
            .unwrap()
            .authority
            .as_ref()
            .is_some_and(|a| a.matches(auth))
    }
}

impl DeviceActor {
    pub(crate) fn authority_fence(&self) -> Result<crate::api::Fence, ErrorCode> {
        self.0
            .state
            .lock()
            .unwrap()
            .authority
            .as_ref()
            .map(|a| a.fence())
            .ok_or(ErrorCode::BackendUnavailable)
    }
}
