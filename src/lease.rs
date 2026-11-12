//! Private lease records for the actor's canonical authority state.
//! No device access, logging, persistence or alternate execution owner.
use crate::api::*;

pub(crate) struct Authority {
    boot: BootId,
    device: DeviceId,
    attachment: AttachmentGeneration,
    fence: Fence,
    grant: Option<LeaseAuth>,
    ttl_ms: u64,
    idle_deadline_ms: u64,
    invalid: bool,
}
fn random_id() -> Result<String, ErrorCode> {
    let mut bytes = [0; 32];
    getrandom::fill(&mut bytes).map_err(|_| ErrorCode::BackendUnavailable)?;
    Ok(bytes.iter().map(|byte| format!("{byte:02x}")).collect())
}
impl Authority {
    pub(crate) fn new(
        boot: BootId,
        device: DeviceId,
        attachment: AttachmentGeneration,
        fence: Fence,
    ) -> Result<Self, ErrorCode> {
        if boot.0.is_empty() || device.0.is_empty() || attachment.0 == 0 || fence.0 == 0 {
            return Err(ErrorCode::DeviceIdentityAmbiguous);
        }
        Ok(Self {
            boot,
            device,
            attachment,
            fence,
            grant: None,
            ttl_ms: 0,
            idle_deadline_ms: 0,
            invalid: false,
        })
    }
    pub(crate) fn acquire(
        &mut self,
        device: &DeviceId,
        now: u64,
        ttl_ms: u64,
    ) -> Result<LeaseGrant, ErrorCode> {
        if device != &self.device {
            return Err(ErrorCode::DeviceIdentityAmbiguous);
        }
        if self.invalid {
            return Err(ErrorCode::BackendUnavailable);
        }
        if self.grant.is_some() {
            return Err(ErrorCode::Leased);
        }
        if !(30_000..=3_600_000).contains(&ttl_ms) {
            return Err(ErrorCode::InvalidRequest);
        }
        let idle_deadline_ms = now.checked_add(ttl_ms).ok_or(ErrorCode::InvalidRequest)?;
        let auth = LeaseAuth {
            boot_id: self.boot.clone(),
            device_id: self.device.clone(),
            lease_id: LeaseId(random_id()?),
            credential: Credential(random_id()?),
            attachment: self.attachment,
            fence: self.fence,
        };
        self.grant = Some(auth.clone());
        self.ttl_ms = ttl_ms;
        self.idle_deadline_ms = idle_deadline_ms;
        Ok(LeaseGrant {
            auth,
            idle_deadline_ms,
            next_seq: ActionSeq(1),
        })
    }
    pub(crate) fn check(&self, auth: &LeaseAuth, now: u64, active: bool) -> Result<(), ErrorCode> {
        if self.invalid || !self.matches(auth) {
            return Err(ErrorCode::LeaseInvalid);
        }
        if !active && now >= self.idle_deadline_ms {
            return Err(ErrorCode::LeaseExpired);
        }
        Ok(())
    }
    pub(crate) fn refresh(
        &mut self,
        auth: &LeaseAuth,
        now: u64,
        active: bool,
    ) -> Result<(), ErrorCode> {
        self.check(auth, now, active)?;
        self.idle_deadline_ms = now
            .checked_add(self.ttl_ms)
            .ok_or(ErrorCode::InvalidRequest)?;
        Ok(())
    }
    pub(crate) fn expired(&self, now: u64, active: bool) -> bool {
        !active && self.grant.is_some() && now >= self.idle_deadline_ms
    }
    pub(crate) fn fence(&self) -> Fence {
        self.fence
    }
    pub(crate) fn invalidate(&mut self) {
        self.invalid = true;
    }
}

impl Authority {
    pub(crate) fn permits_successor(
        &self,
        boot: &BootId,
        attachment: AttachmentGeneration,
        fence: Fence,
    ) -> bool {
        self.invalid
            && boot == &self.boot
            && attachment.0 >= self.attachment.0
            && fence.0 > self.fence.0
    }
    pub(crate) fn matches(&self, auth: &LeaseAuth) -> bool {
        self.grant.as_ref().is_some_and(|original| {
            auth.boot_id == original.boot_id
                && auth.device_id == original.device_id
                && auth.lease_id == original.lease_id
                && auth.credential == original.credential
                && auth.attachment == original.attachment
                && auth.fence == original.fence
        })
    }
    pub(crate) fn attachment_matches(&self, attachment: AttachmentGeneration) -> bool {
        self.attachment == attachment
    }
}

impl Authority {
    pub(crate) fn reconnect_fence(
        &self,
        boot: &BootId,
        attachment: AttachmentGeneration,
    ) -> Result<Fence, ErrorCode> {
        if !self.invalid || boot != &self.boot || attachment.0 <= self.attachment.0 {
            return Err(ErrorCode::LeaseInvalid);
        }
        self.fence
            .0
            .checked_add(1)
            .map(Fence)
            .ok_or(ErrorCode::BackendUnavailable)
    }
}
