//! One MCP connection's private authority. No model-supplied credentials.
use crate::{
    api::*,
    ipc::client::{self, ClientError},
};
use std::{
    path::PathBuf,
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};

pub(crate) struct State {
    pub auth: Option<LeaseAuth>,
    pub retired: Option<LeaseAuth>,
    pub frame: Option<Frame>,
    pub elements: Vec<UiElement>,
    pub next: u64,
    pub pending: Option<ActionSeq>,
    pub last: Option<ActionSeq>,
    touched: Instant,
}
pub(crate) struct Authority {
    pub state: Mutex<State>,
    runtime: PathBuf,
}
impl Authority {
    pub fn new(runtime: PathBuf) -> Arc<Self> {
        let owner = Arc::new(Self {
            runtime,
            state: Mutex::new(State {
                auth: None,
                retired: None,
                frame: None,
                elements: Vec::new(),
                next: 1,
                pending: None,
                last: None,
                touched: Instant::now(),
            }),
        });
        let weak = Arc::downgrade(&owner);
        // Independent of async task cancellation/Drop. Daemon TTL remains the
        // backstop if the entire MCP process is killed before this worker runs.
        std::thread::spawn(move || {
            loop {
                std::thread::sleep(Duration::from_secs(1));
                let Some(owner) = weak.upgrade() else { break };
                let Ok(mut state) = owner.state.try_lock() else {
                    continue;
                };
                if state.touched.elapsed() >= Duration::from_secs(45) {
                    owner.release(&mut state);
                }
            }
        });
        owner
    }
    pub fn exchange(&self, call: Call, budget: Duration) -> Result<Response, ClientError> {
        client::exchange(
            &self.runtime.join("cordgeist/daemon.sock"),
            call,
            Instant::now() + budget,
        )
    }
    pub fn touch(&self, state: &mut State) {
        state.touched = Instant::now();
    }
    pub fn ensure(&self, state: &mut State, selected: Option<&str>) -> Result<(), String> {
        self.touch(state);
        if let Some(auth) = state.auth.as_ref() {
            if selected.is_some_and(|id| id != auth.device_id.0) {
                return Err("device-identity-ambiguous".into());
            }
            return Ok(());
        }
        if state.pending.is_some() {
            return Err("Pending outcome belongs to retired authority; inspect original status, never replay.".into());
        }
        let response = self
            .exchange(Call::List, Duration::from_millis(1500))
            .map_err(|_| "Inventory unavailable")?;
        let ResponseOutcome::Success { result } = response.outcome else {
            return Err("Inventory unavailable".into());
        };
        let OperationResult::List(list) = *result else {
            return Err("Invalid inventory".into());
        };
        if list.devices.len() != 1
            || selected.is_some_and(|id| list.devices.first().is_none_or(|d| d.device_id.0 != id))
        {
            return Err("Connect exactly one authorized USB phone".into());
        }
        let response = self
            .exchange(
                Call::Acquire {
                    device: list.devices[0].device_id.clone(),
                    reason: "MCP session".into(),
                    label: None,
                    idle_ttl_s: 60,
                },
                Duration::from_millis(1500),
            )
            .map_err(|_| "Acquire uncertain; wait for daemon expiry before recovery")?;
        match response.outcome {
            ResponseOutcome::Success { result } => match *result {
                OperationResult::Acquire(grant) => {
                    state.next = grant.next_seq.0;
                    state.last = None;
                    state.frame = None;
                    state.elements.clear();
                    state.retired = None;
                    state.auth = Some(grant.auth);
                    Ok(())
                }
                _ => Err("Invalid acquire response".into()),
            },
            ResponseOutcome::Error { error } => Err(error.code.to_string()),
        }
    }
    pub fn release(&self, state: &mut State) {
        if let Some(auth) = state.auth.as_ref()
            && matches!(
                self.exchange(
                    Call::Release { auth: auth.clone() },
                    Duration::from_millis(1500)
                ),
                Ok(Response {
                    outcome: ResponseOutcome::Success { .. },
                    ..
                })
            )
        {
            if state.pending.is_some() {
                state.retired = state.auth.clone();
            }
            state.auth = None;
            state.frame = None;
            state.elements.clear();
        }
    }
}
impl Drop for Authority {
    fn drop(&mut self) {
        if let Ok(mut state) = self.state.lock() {
            self.release(&mut state);
        }
    }
}
