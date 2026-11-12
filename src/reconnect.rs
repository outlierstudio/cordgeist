//! Actor-local reconnect prerequisites, never a second authority store.
use crate::{
    api::*,
    geometry::Transform,
    identity::{IdentityRegistry, RouteRef},
};
use std::sync::{Arc, Mutex};
pub(crate) struct Prerequisites {
    identities: Arc<Mutex<IdentityRegistry>>,
    route: RouteRef,
    installed_ms: u64,
    revision: u64,
    observation: Option<(Frame, Transform)>,
}
impl Prerequisites {
    pub(crate) fn new(
        identities: Arc<Mutex<IdentityRegistry>>,
        route: RouteRef,
        now: u64,
    ) -> Result<Self, ErrorCode> {
        identities.lock().unwrap().resolve(&route)?;
        Ok(Self {
            identities,
            route,
            installed_ms: now,
            revision: 0,
            observation: None,
        })
    }
    fn identity(&self) -> Result<(), ErrorCode> {
        self.identities
            .lock()
            .unwrap()
            .resolve(&self.route)
            .map(|_| ())
    }
    pub(crate) fn publish(
        &mut self,
        auth: &LeaseAuth,
        frame: Frame,
        mapping: Transform,
        now: u64,
    ) -> Result<(), ErrorCode> {
        self.observation = None;
        // Every authenticated publication attempt retires the old observation.
        // Exhaustion never wraps or makes a previous capability current again.
        self.revision = self
            .revision
            .checked_add(1)
            .ok_or(ErrorCode::FrameExpired)?;
        self.identity()?;
        if mapping.max_x == 0 || mapping.max_y == 0 {
            return Err(ErrorCode::MappingUnverified);
        }
        if frame.provenance.boot_id != auth.boot_id
            || frame.provenance.device_id != auth.device_id
            || frame.provenance.lease_id != auth.lease_id
            || frame.provenance.attachment != auth.attachment
            || frame.provenance.fence != auth.fence
            || frame.provenance.geometry.0 == 0
            || frame.provenance.mapping.0 == 0
            || frame.id.0.is_empty()
            || frame.id.0.len() > 128
            || frame.display_id != 0
            || frame.display.width < 2
            || frame.display.height < 2
            || frame.captured_ms < self.installed_ms
            || now
                .checked_sub(frame.captured_ms)
                .is_none_or(|age| age > 30_000)
            || std::mem::discriminant(&frame.rotation) != std::mem::discriminant(&mapping.rotation)
        {
            return Err(ErrorCode::FrameExpired);
        }
        if mapping.usable() {
            mapping.map(frame.image, Point { x: 0.0, y: 0.0 })?;
        }
        self.observation = Some((frame, mapping));
        Ok(())
    }
    pub(crate) fn ready(&self, now: u64) -> bool {
        self.identity().is_ok()
            && self.observation.as_ref().is_some_and(|(f, _)| {
                now.checked_sub(f.captured_ms)
                    .is_some_and(|age| age <= 30_000)
            })
    }
    pub(crate) fn check(
        &self,
        auth: &LeaseAuth,
        frame_id: Option<&FrameId>,
        now: u64,
    ) -> Result<(), ErrorCode> {
        self.identity()?;
        if self.route.boot_id != auth.boot_id
            || self.route.device_id != auth.device_id
            || self.route.attachment != auth.attachment
        {
            return Err(ErrorCode::LeaseInvalid);
        }
        let (frame, _) = self.observation.as_ref().ok_or(ErrorCode::FrameExpired)?;
        if frame_id != Some(&frame.id)
            || frame.provenance.lease_id != auth.lease_id
            || frame.provenance.fence != auth.fence
            || !self.ready(now)
        {
            return Err(ErrorCode::FrameExpired);
        }
        Ok(())
    }
}

impl Prerequisites {
    /// Non-coordinate app/button controls need exact authority/route but no image.
    pub(crate) fn check_identity(&self, auth: &LeaseAuth) -> Result<(), ErrorCode> {
        self.identity()?;
        if self.route.boot_id != auth.boot_id
            || self.route.device_id != auth.device_id
            || self.route.attachment != auth.attachment
        {
            return Err(ErrorCode::LeaseInvalid);
        }
        Ok(())
    }
    pub(crate) fn retire_observation(&mut self) -> Result<(), ErrorCode> {
        self.observation = None;
        self.revision = self
            .revision
            .checked_add(1)
            .ok_or(ErrorCode::FrameExpired)?;
        Ok(())
    }
    pub(crate) fn bind(
        &self,
        auth: &LeaseAuth,
        frame_id: Option<&FrameId>,
        now: u64,
    ) -> Result<u64, ErrorCode> {
        self.check(auth, frame_id, now)?;
        Ok(self.revision)
    }
    pub(crate) fn check_revision(
        &self,
        auth: &LeaseAuth,
        frame_id: Option<&FrameId>,
        revision: Option<u64>,
        now: u64,
    ) -> Result<(), ErrorCode> {
        if revision != Some(self.revision) {
            return Err(ErrorCode::FrameExpired);
        }
        self.check(auth, frame_id, now)
    }
}
