//! Software-only daemon-owned exact-device registry. No IPC/native wiring.
use crate::{
    actor::{ActorStatus, DeviceActor, Ticket},
    api::*,
    backend::Clock,
    lease::Authority,
    session::Session,
    session_input::Transition,
};
use std::{
    collections::HashMap,
    sync::{Arc, Mutex},
    time::Duration,
};
/// Private grants intentionally cannot be logged using Debug.
/// ```compile_fail
/// fn diagnostic<T: std::fmt::Debug>() {}
/// diagnostic::<cordgeist::api::LeaseGrant>();
/// ```
/// ```compile_fail
/// fn diagnostic<T: std::fmt::Debug>() {}
/// diagnostic::<cordgeist::api::LeaseAuth>();
/// ```
/// ```compile_fail
/// fn diagnostic<T: std::fmt::Debug>() {}
/// diagnostic::<cordgeist::lease_devices::LeaseDevices>();
/// ```
pub struct LeaseDevices {
    devices: Mutex<HashMap<DeviceId, DeviceActor>>,
    clock: Arc<dyn Clock + Send + Sync>,
}
impl LeaseDevices {
    pub fn new(clock: Arc<dyn Clock + Send + Sync>) -> Self {
        Self {
            devices: Mutex::new(HashMap::new()),
            clock,
        }
    }
    pub fn register(
        &self,
        boot: BootId,
        device: DeviceId,
        attachment: AttachmentGeneration,
        fence: Fence,
        session: Session,
    ) -> Result<(), ErrorCode> {
        let mut devices = self.devices.lock().unwrap();
        if devices.contains_key(&device) {
            return Err(ErrorCode::Leased);
        }
        if devices.len() >= 16 {
            return Err(ErrorCode::QueueFull);
        }
        let authority = Authority::new(boot, device.clone(), attachment, fence)?;
        devices.insert(
            device,
            DeviceActor::build(session, Some(authority), self.clock.clone()),
        );
        Ok(())
    }
    fn actor(&self, device: &DeviceId) -> Result<DeviceActor, ErrorCode> {
        self.devices
            .lock()
            .unwrap()
            .get(device)
            .cloned()
            .ok_or(ErrorCode::BackendUnavailable)
    }
    pub fn acquire(&self, device: &DeviceId, ttl_ms: u64) -> Result<LeaseGrant, ErrorCode> {
        self.actor(device)?.acquire(device, ttl_ms)
    }
    pub fn refresh(&self, auth: &LeaseAuth) -> Result<(), ErrorCode> {
        self.actor(&auth.device_id)?.refresh_owner(auth)
    }
    pub fn enqueue(
        &self,
        auth: LeaseAuth,
        transition: Transition,
        budget: Duration,
    ) -> Result<Ticket, ErrorCode> {
        self.actor(&auth.device_id)?
            .enqueue_owner(auth, transition, budget)
    }
    pub fn status(&self, device: &DeviceId) -> Result<ActorStatus, ErrorCode> {
        let a = self.actor(device)?;
        a.expire_idle();
        Ok(a.status())
    }
    pub fn invalidate(&self, device: &DeviceId) -> Result<(), ErrorCode> {
        self.actor(device)?.invalidate();
        Ok(())
    }
    pub async fn shutdown(
        &self,
        device: &DeviceId,
        budget: Duration,
    ) -> Result<crate::session::Status, ErrorCode> {
        self.actor(device)?.shutdown(budget).await
    }
}

impl LeaseDevices {
    /// Trusted owner setup only: replace a retired, successfully cleaned Session.
    /// Does not reopen the old actor or transfer its grant to the successor.
    pub fn replace_cleaned(
        &self,
        boot: BootId,
        device: DeviceId,
        attachment: AttachmentGeneration,
        fence: Fence,
        session: Session,
    ) -> Result<(), ErrorCode> {
        let mut devices = self.devices.lock().unwrap();
        let previous = devices.get(&device).ok_or(ErrorCode::BackendUnavailable)?;
        if !previous.permits_successor(&boot, attachment, fence)
            || session.status().phase != crate::session::Phase::Ready
        {
            return Err(ErrorCode::BackendUnavailable);
        }
        let authority = Authority::new(boot, device.clone(), attachment, fence)?;
        devices.insert(
            device,
            DeviceActor::build(session, Some(authority), self.clock.clone()),
        );
        Ok(())
    }
}

impl LeaseDevices {
    /// End ownership, fence immediately, then wait only the requested budget.
    pub async fn release(
        &self,
        auth: &LeaseAuth,
        budget: Duration,
    ) -> Result<crate::session::Status, ErrorCode> {
        self.end_owner(auth, crate::actor::TerminationReason::Released, budget)
            .await
    }
    pub async fn cancel_ownership(
        &self,
        auth: &LeaseAuth,
        budget: Duration,
    ) -> Result<crate::session::Status, ErrorCode> {
        self.end_owner(
            auth,
            crate::actor::TerminationReason::OwnerCancelled,
            budget,
        )
        .await
    }
    async fn end_owner(
        &self,
        auth: &LeaseAuth,
        reason: crate::actor::TerminationReason,
        budget: Duration,
    ) -> Result<crate::session::Status, ErrorCode> {
        valid_wait(budget)?;
        let actor = self.actor(&auth.device_id)?;
        actor.terminate_owner(auth, reason)?;
        actor.shutdown(budget).await
    }
    /// Action cancellation is not deliberate ownership termination. Session may
    /// still become terminal if cancellation leaves delivery/cleanup uncertain.
    pub fn cancel_action(&self, auth: &LeaseAuth, ticket: &Ticket) -> Result<(), ErrorCode> {
        self.actor(&auth.device_id)?
            .cancel_owner_action(auth, ticket)
    }
    /// Deliberate trusted same-user administrative revoke of one exact device.
    pub async fn revoke(
        &self,
        device: &DeviceId,
        budget: Duration,
    ) -> Result<crate::session::Status, ErrorCode> {
        valid_wait(budget)?;
        let actor = self.actor(device)?;
        actor.terminate(crate::actor::TerminationReason::Revoked);
        actor.shutdown(budget).await
    }
    /// Generation-bound health event cannot terminate a successor attachment.
    pub async fn disconnect(
        &self,
        device: &DeviceId,
        attachment: AttachmentGeneration,
        budget: Duration,
    ) -> Result<crate::session::Status, ErrorCode> {
        valid_wait(budget)?;
        let actor = self.actor(device)?;
        actor.disconnect(attachment)?;
        actor.shutdown(budget).await
    }
}
fn valid_wait(budget: Duration) -> Result<(), ErrorCode> {
    if budget.is_zero() || budget > Duration::from_secs(10) {
        Err(ErrorCode::InvalidRequest)
    } else {
        Ok(())
    }
}

impl LeaseDevices {
    /// Bind observation/worker notifications to this exact actor instance.
    pub fn worker_token(&self, device: &DeviceId) -> Result<crate::actor::WorkerToken, ErrorCode> {
        Ok(self.actor(device)?.worker_token())
    }
    /// A late failure for a retired worker cannot target its device's successor.
    pub fn worker_failed(
        &self,
        device: &DeviceId,
        token: &crate::actor::WorkerToken,
    ) -> Result<(), ErrorCode> {
        self.actor(device)?.worker_failed(token)
    }
}

impl LeaseDevices {
    /// Trusted setup: resolve fresh owned identity, never guess a moved device.
    /// A fresh lease may observe, but input remains gated until publication.
    pub fn reconnect(
        &self,
        identities: Arc<Mutex<crate::identity::IdentityRegistry>>,
        route: crate::identity::RouteRef,
        session: Session,
    ) -> Result<crate::actor::WorkerToken, ErrorCode> {
        let prerequisites =
            crate::reconnect::Prerequisites::new(identities, route.clone(), self.clock.now_ms())?;
        if session.status().phase != crate::session::Phase::Ready {
            return Err(ErrorCode::BackendUnavailable);
        }
        let mut devices = self.devices.lock().unwrap();
        let previous = devices
            .get(&route.device_id)
            .ok_or(ErrorCode::BackendUnavailable)?;
        let fence = previous.reconnect_fence(&route.boot_id, route.attachment)?;
        let authority = Authority::new(
            route.boot_id,
            route.device_id.clone(),
            route.attachment,
            fence,
        )?;
        let actor = DeviceActor::build_inner(
            session,
            Some(authority),
            self.clock.clone(),
            Some(prerequisites),
        );
        let token = actor.worker_token();
        devices.insert(route.device_id, actor);
        Ok(token)
    }
    /// Trusted observation adapter publication, bound to worker and new lease.
    pub(crate) fn retire_observation(
        &self,
        token: &crate::actor::WorkerToken,
        auth: &LeaseAuth,
    ) -> Result<(), ErrorCode> {
        self.actor(&auth.device_id)?.retire_observation(token, auth)
    }
    pub fn publish_observation(
        &self,
        token: &crate::actor::WorkerToken,
        auth: &LeaseAuth,
        frame: Frame,
        mapping: crate::geometry::Transform,
    ) -> Result<(), ErrorCode> {
        self.actor(&auth.device_id)?
            .publish_observation(token, auth, frame, mapping)
    }
    pub fn enqueue_frame(
        &self,
        auth: LeaseAuth,
        frame_id: FrameId,
        transition: Transition,
        budget: Duration,
    ) -> Result<Ticket, ErrorCode> {
        self.actor(&auth.device_id)?
            .enqueue_frame(auth, frame_id, transition, budget)
    }
}

impl LeaseDevices {
    pub(crate) fn enqueue_owner_until(
        &self,
        auth: LeaseAuth,
        transition: Transition,
        deadline: std::time::Instant,
    ) -> Result<Ticket, ErrorCode> {
        self.actor(&auth.device_id)?
            .enqueue_owner_until(auth, transition, deadline)
    }
    pub(crate) fn enqueue_frame_until(
        &self,
        auth: LeaseAuth,
        frame_id: FrameId,
        transition: Transition,
        deadline: std::time::Instant,
    ) -> Result<Ticket, ErrorCode> {
        self.actor(&auth.device_id)?
            .enqueue_frame_until(auth, frame_id, transition, deadline)
    }
    /// Trusted initial setup through the same identity/Session path as reconnect.
    pub fn register_routed(
        &self,
        identities: Arc<Mutex<crate::identity::IdentityRegistry>>,
        route: crate::identity::RouteRef,
        session: Session,
    ) -> Result<crate::actor::WorkerToken, ErrorCode> {
        let prerequisites =
            crate::reconnect::Prerequisites::new(identities, route.clone(), self.clock.now_ms())?;
        if session.status().phase != crate::session::Phase::Ready {
            return Err(ErrorCode::BackendUnavailable);
        }
        let mut devices = self.devices.lock().unwrap();
        if devices.contains_key(&route.device_id) {
            return Err(ErrorCode::Leased);
        }
        if devices.len() >= 16 {
            return Err(ErrorCode::QueueFull);
        }
        let authority = Authority::new(
            route.boot_id,
            route.device_id.clone(),
            route.attachment,
            Fence(1),
        )?;
        let actor = DeviceActor::build_inner(
            session,
            Some(authority),
            self.clock.clone(),
            Some(prerequisites),
        );
        let token = actor.worker_token();
        devices.insert(route.device_id, actor);
        Ok(token)
    }
}

impl LeaseDevices {
    pub(crate) fn authority_fence(&self, device: &DeviceId) -> Result<Fence, ErrorCode> {
        self.actor(device)?.authority_fence()
    }
}

impl LeaseDevices {
    pub(crate) fn receipt_owner_worker(
        &self,
        auth: &LeaseAuth,
        token: &crate::actor::WorkerToken,
    ) -> Result<(), ErrorCode> {
        let actor = self.actor(&auth.device_id)?;
        if actor.worker_matches(token) && actor.receipt_owner(auth) {
            Ok(())
        } else {
            Err(ErrorCode::LeaseInvalid)
        }
    }
    pub(crate) async fn revoke_worker(
        &self,
        device: &DeviceId,
        token: &crate::actor::WorkerToken,
        budget: Duration,
    ) -> Result<crate::session::Status, ErrorCode> {
        valid_wait(budget)?;
        let actor = self.actor(device)?;
        if !actor.worker_matches(token) {
            return Err(ErrorCode::LeaseInvalid);
        }
        actor.terminate(crate::actor::TerminationReason::Revoked);
        actor.shutdown(budget).await
    }
}
