use crate::api::*;
use std::{
    sync::{
        Arc,
        atomic::{AtomicBool, Ordering},
    },
    time::Duration,
};

#[derive(Clone, Default)]
pub struct Cancel(pub Arc<AtomicBool>);
impl Cancel {
    pub fn cancel(&self) {
        self.0.store(true, Ordering::SeqCst);
    }
    pub fn cancelled(&self) -> bool {
        self.0.load(Ordering::SeqCst)
    }
}
#[derive(Clone)]
pub enum PreparedInput {
    Tap(u16, u16),
    Swipe {
        from: (u16, u16),
        to: (u16, u16),
        duration: Duration,
    },
    Hold {
        at: (u16, u16),
        duration: Duration,
    },
    Ascii(Vec<u8>),
    Key(String),
}
#[derive(Clone, Debug)]
pub struct InputOutcome {
    pub status: InputStatus,
    pub cleanup: Cleanup,
}
// Owned messages only; no raw/native references may cross the boundary.
// Methods run on a dedicated serialized owner thread; implementations need not be Send.
pub trait InputBackend {
    fn label(&self) -> &'static str;
    fn execute(&mut self, input: PreparedInput, deadline_ms: u64, cancel: &Cancel) -> InputOutcome;
    fn neutralize(&mut self, deadline_ms: u64) -> Cleanup;
}
pub trait Clock {
    fn now_ms(&self) -> u64;
}
pub struct Captured {
    pub png: Vec<u8>,
    pub frame: Frame,
}
pub trait Observation {
    fn capture(&mut self, deadline_ms: u64, cancel: &Cancel) -> Result<Captured, ErrorCode>;
}
pub struct ProcessRequest {
    pub args: Vec<String>,
    pub deadline_ms: u64,
    pub stdout_limit: usize,
}
pub struct ProcessOutput {
    pub stdout: Vec<u8>,
    pub stderr: Vec<u8>,
}
pub trait AdbProcess {
    fn run(&mut self, request: ProcessRequest, cancel: &Cancel)
    -> Result<ProcessOutput, ErrorCode>;
}
pub struct Word {
    pub text: String,
    pub confidence: f32,
    pub origin: Point,
    pub size: Size,
}
pub trait Ocr {
    fn recognize(
        &mut self,
        png: Vec<u8>,
        language: String,
        deadline_ms: u64,
        cancel: &Cancel,
    ) -> Result<Vec<Word>, ErrorCode>;
}
pub trait MediaIndexer {
    fn index(
        &mut self,
        destination: String,
        user: u32,
        deadline_ms: u64,
        cancel: &Cancel,
    ) -> Result<Stage, ErrorCode>;
}
pub struct Unavailable;
impl InputBackend for Unavailable {
    fn label(&self) -> &'static str {
        "unavailable"
    }
    fn execute(&mut self, _: PreparedInput, _: u64, _: &Cancel) -> InputOutcome {
        InputOutcome {
            status: InputStatus::NotSent,
            cleanup: Cleanup::Neutral,
        }
    }
    fn neutralize(&mut self, _: u64) -> Cleanup {
        Cleanup::Neutral
    }
}
