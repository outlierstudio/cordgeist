//! Explicit SOFTWARE FAKE. These effects are never evidence of device delivery.
use crate::{api::*, backend::*};
use std::{
    collections::VecDeque,
    sync::{
        Arc,
        atomic::{AtomicU64, Ordering},
    },
};
#[derive(Clone, Default)]
pub struct FakeClock(pub Arc<AtomicU64>);
impl FakeClock {
    pub fn advance(&self, ms: u64) {
        self.0.fetch_add(ms, Ordering::SeqCst);
    }
}
impl Clock for FakeClock {
    fn now_ms(&self) -> u64 {
        self.0.load(Ordering::SeqCst)
    }
}
#[derive(Clone, Copy)]
pub enum Fault {
    BeforeSend,
    Completed,
    Partial,
    Disconnect,
    CleanupPending,
}
pub struct ScriptedInput {
    pub script: VecDeque<Fault>,
    pub effects: usize,
    pub cleanups: usize,
}
impl ScriptedInput {
    pub fn new(script: impl IntoIterator<Item = Fault>) -> Self {
        Self {
            script: script.into_iter().collect(),
            effects: 0,
            cleanups: 0,
        }
    }
}
impl InputBackend for ScriptedInput {
    fn label(&self) -> &'static str {
        "FAKE-software-only"
    }
    fn execute(&mut self, _: PreparedInput, _: u64, _: &Cancel) -> InputOutcome {
        let fault = self.script.pop_front().expect("fake script exhausted");
        if !matches!(fault, Fault::BeforeSend) {
            self.effects += 1;
        }
        InputOutcome {
            status: match fault {
                Fault::BeforeSend => InputStatus::NotSent,
                Fault::Completed | Fault::CleanupPending => InputStatus::Sent,
                Fault::Partial => InputStatus::Partial,
                Fault::Disconnect => InputStatus::Unknown,
            },
            cleanup: if matches!(fault, Fault::CleanupPending) {
                Cleanup::Pending
            } else {
                Cleanup::Neutral
            },
        }
    }
    fn neutralize(&mut self, _: u64) -> Cleanup {
        self.cleanups += 1;
        Cleanup::Neutral
    }
}
pub struct ScriptedObservation(pub VecDeque<Result<Captured, ErrorCode>>);
impl Observation for ScriptedObservation {
    fn capture(&mut self, _: u64, _: &Cancel) -> Result<Captured, ErrorCode> {
        self.0.pop_front().expect("fake capture script exhausted")
    }
}
