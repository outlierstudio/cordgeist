//! Serialized admission kernel. The daemon actor layer will own this state in P5.
use crate::{api::*, backend::*, geometry::Transform};
use std::collections::VecDeque;

struct Record {
    seq: ActionSeq,
    digest: blake3::Hash,
    receipt: Receipt,
}
pub struct Controller<B, C> {
    backend: B,
    clock: C,
    auth: LeaseAuth,
    current: Provenance,
    frames: VecDeque<(Frame, Transform)>,
    records: VecDeque<Record>,
    key: [u8; 32],
    high_water: u64,
    receipt_limit: usize,
    expires_ms: u64,
    ttl_ms: u64,
    revoked: bool,
    quarantined: bool,
}
impl<B: InputBackend, C: Clock> Controller<B, C> {
    pub fn new(
        backend: B,
        clock: C,
        auth: LeaseAuth,
        current: Provenance,
        ttl_ms: u64,
        receipt_limit: usize,
    ) -> Result<Self, ErrorCode> {
        if !(30_000..=3_600_000).contains(&ttl_ms)
            || !(1..=256).contains(&receipt_limit)
            || auth.boot_id != current.boot_id
            || auth.device_id != current.device_id
            || auth.lease_id != current.lease_id
            || auth.attachment != current.attachment
            || auth.fence != current.fence
        {
            return Err(ErrorCode::InvalidRequest);
        }
        let expires_ms = clock
            .now_ms()
            .checked_add(ttl_ms)
            .ok_or(ErrorCode::InvalidRequest)?;
        let mut key = [0; 32];
        getrandom::fill(&mut key).map_err(|_| ErrorCode::BackendUnavailable)?;
        Ok(Self {
            backend,
            clock,
            auth,
            current,
            frames: VecDeque::new(),
            records: VecDeque::new(),
            key,
            high_water: 0,
            receipt_limit,
            expires_ms,
            ttl_ms,
            revoked: false,
            quarantined: false,
        })
    }
    // Read-only test evidence of admission; never changes production state.
    #[cfg(test)]
    pub(crate) fn admitted_sequence(&self) -> u64 {
        self.high_water
    }
    pub fn backend(&self) -> &B {
        &self.backend
    }
    pub fn retain_frame(&mut self, frame: Frame, transform: Transform) -> Result<(), ErrorCode> {
        if frame.provenance != self.current
            || frame.captured_ms > self.clock.now_ms()
            || frame.image.width < 2
            || frame.image.height < 2
            || frame.display.width < 2
            || frame.display.height < 2
            || self.frames.iter().any(|(f, _)| f.id == frame.id)
            || frame.display_id != 0
        {
            return Err(ErrorCode::GeometryChanged);
        }
        if self.frames.len() == 16 {
            self.frames.pop_front();
        }
        self.frames.push_back((frame, transform));
        Ok(())
    }
    fn authenticate(&self, auth: &LeaseAuth) -> Result<(), ErrorCode> {
        self.authenticate_at(auth, self.clock.now_ms())
    }
    fn authenticate_at(&self, auth: &LeaseAuth, now_ms: u64) -> Result<(), ErrorCode> {
        if self.revoked
            || auth.boot_id != self.auth.boot_id
            || auth.device_id != self.auth.device_id
            || auth.lease_id != self.auth.lease_id
            || auth.credential != self.auth.credential
            || auth.attachment != self.auth.attachment
            || auth.fence != self.auth.fence
            || auth.boot_id != self.current.boot_id
            || auth.device_id != self.current.device_id
            || auth.lease_id != self.current.lease_id
            || auth.fence != self.current.fence
            || auth.attachment != self.current.attachment
        {
            return Err(ErrorCode::LeaseInvalid);
        }
        if now_ms >= self.expires_ms {
            return Err(ErrorCode::LeaseExpired);
        }
        Ok(())
    }
    pub fn revoke(&mut self) -> Cleanup {
        self.revoked = true;
        self.current.fence.0 = self.current.fence.0.saturating_add(1);
        self.frames.clear();
        let cleanup = self
            .backend
            .neutralize(self.clock.now_ms().saturating_add(1000));
        self.quarantined = cleanup != Cleanup::Neutral;
        cleanup
    }
    pub fn reconfigure(&mut self, current: Provenance) {
        // A changed authorization domain permanently invalidates this grant.
        // Public request fields cannot upgrade it or revive it after restoration.
        self.revoked |= current.boot_id != self.auth.boot_id
            || current.device_id != self.auth.device_id
            || current.lease_id != self.auth.lease_id
            || current.attachment != self.auth.attachment
            || current.fence != self.auth.fence;
        self.current = current;
        self.frames.clear();
    }
    pub fn act(&mut self, req: ActionRequest, cancel: &Cancel) -> Result<Receipt, ErrorCode> {
        self.authenticate(&req.auth)?;
        // Keyed, memory-only digest: payload (including secrets) never enters receipts/logs.
        let mut hasher = blake3::Hasher::new_keyed(&self.key);
        serde_json::to_writer(&mut hasher, &req).map_err(|_| ErrorCode::InvalidRequest)?;
        let digest = hasher.finalize();
        if let Some(record) = self.records.iter().find(|r| r.seq == req.seq) {
            return if record.digest == digest {
                Ok(record.receipt.clone())
            } else {
                Err(ErrorCode::SequenceMismatch)
            };
        }
        if req.seq.0 <= self.high_water {
            return Err(ErrorCode::ReceiptExpired);
        }
        if self.quarantined {
            return Err(ErrorCode::NativeCleanupPending);
        }
        if self.backend.label() == "unavailable" {
            return Err(ErrorCode::BackendUnavailable);
        }
        let (frame, transform) = self
            .frames
            .iter()
            .find(|(f, _)| f.id == req.frame_id)
            .ok_or(ErrorCode::FrameExpired)?;
        if frame.provenance != self.current {
            return Err(ErrorCode::GeometryChanged);
        }
        let age = self
            .clock
            .now_ms()
            .checked_sub(frame.captured_ms)
            .ok_or(ErrorCode::FrameExpired)?;
        if age > 30_000 {
            return Err(ErrorCode::FrameExpired);
        }
        let map = |p| transform.map(frame.image, p);
        let duration = |ms| {
            if (20..=10_000).contains(&ms) {
                Ok(std::time::Duration::from_millis(u64::from(ms)))
            } else {
                Err(ErrorCode::InvalidRequest)
            }
        };
        // All visual input, including keys/text, needs a verified mapping/provenance.
        if !transform.verified {
            return Err(ErrorCode::MappingUnverified);
        }
        let input = match req.input {
            Input::Tap { point } => {
                let (x, y) = map(point)?;
                PreparedInput::Tap(x, y)
            }
            Input::Swipe {
                from,
                to,
                duration_ms,
            } => PreparedInput::Swipe {
                from: map(from)?,
                to: map(to)?,
                duration: duration(duration_ms)?,
            },
            Input::Hold { point, duration_ms } => PreparedInput::Hold {
                at: map(point)?,
                duration: duration(duration_ms)?,
            },
            Input::Type { text } => {
                if text.len() > 4096
                    || text.is_empty()
                    || !text
                        .bytes()
                        .all(|b| (32..=126).contains(&b) || b == b'\t' || b == b'\n')
                {
                    return Err(ErrorCode::UnsupportedCharacter);
                }
                PreparedInput::Ascii(text.into_bytes())
            }
            Input::Key { key } => {
                // Initial corpus only; Android navigation keys require device evidence later.
                if !["enter", "tab", "escape", "backspace"].contains(&key.as_str()) {
                    return Err(ErrorCode::UnsupportedKey);
                }
                PreparedInput::Key(key)
            }
        };
        if cancel.cancelled() {
            return Err(ErrorCode::ProcessCancelled);
        }
        let started_ms = self.clock.now_ms();
        let deadline_ms = started_ms
            .checked_add(120_000)
            .ok_or(ErrorCode::InvalidRequest)?;
        // Last admission barrier, after hashing/preparation, before reservation.
        // All checks use the same monotonic instant; no clock reads or callbacks
        // occur between this barrier and the serialized backend invocation.
        self.authenticate_at(&req.auth, started_ms)?;
        if self.quarantined {
            return Err(ErrorCode::NativeCleanupPending);
        }
        if frame.provenance != self.current {
            return Err(ErrorCode::GeometryChanged);
        }
        if started_ms
            .checked_sub(frame.captured_ms)
            .ok_or(ErrorCode::FrameExpired)?
            > 30_000
        {
            return Err(ErrorCode::FrameExpired);
        }
        self.high_water = req.seq.0; // reserve before backend effects; serialized actor only
        let outcome = self.backend.execute(input, deadline_ms, cancel);
        self.quarantined = outcome.cleanup != Cleanup::Neutral
            || matches!(outcome.status, InputStatus::Unknown | InputStatus::Partial);
        let receipt = Receipt {
            action_id: ActionId(format!(
                "{}:{}:{}",
                self.auth.boot_id.0, self.auth.lease_id.0, req.seq.0
            )),
            seq: req.seq,
            transport: self.backend.label().into(),
            started_ms,
            finished_ms: self.clock.now_ms(),
            input_status: outcome.status,
            observation_status: ObservationStatus::NotRequested,
            cleanup: outcome.cleanup,
            cancelled: cancel.cancelled(),
            retry_guidance: "Query this sequence; never repeat uncertain input with a new sequence"
                .into(),
        };
        self.expires_ms = self.clock.now_ms().saturating_add(self.ttl_ms);
        if self.records.len() == self.receipt_limit {
            self.records.pop_front();
        }
        self.records.push_back(Record {
            seq: req.seq,
            digest,
            receipt: receipt.clone(),
        });
        Ok(receipt)
    }
}
