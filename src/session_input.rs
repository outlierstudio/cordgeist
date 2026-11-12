//! Owner-local tracked report transitions. No gestures or automatic retries.
use crate::{api::ErrorCode, backend::Cancel, session::Failure};
use std::{collections::BTreeSet, time::Instant};

/// Exact ranges in the pinned default keyboard descriptor: ordinary usages and modifiers.
pub fn supported_key(usage: u16) -> bool {
    (0x04..=0xdd).contains(&usage) || (0xe0..=0xe7).contains(&usage)
}

/// Deliberately not Debug: keyboard/contact state is private payload data.
#[derive(Clone)]
pub enum Transition {
    Android(crate::api::AndroidCommand),
    Text(crate::ascii::Text),
    TextSubmit(crate::ascii::Text),
    Gesture(crate::gesture::Gesture),
    Batch(Vec<Transition>),
    Key { usage: u16, down: bool },
    Contact { down: bool, x: u16, y: u16 },
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Delivery {
    NotSent,
    Sent,
    Partial,
    Unknown,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub struct ReportResult {
    pub delivery: Delivery,
    pub error: Option<Failure>,
}
/// Trusted owner-thread adapter. An error after submission begins must report
/// Unknown unless the transport establishes that nothing was sent.
pub trait ReportDriver {
    fn cancelled(&self) -> bool {
        false
    }
    fn apply(&mut self, transition: &Transition) -> Result<(), Failure>;
    fn submit(&mut self, transition: &Transition, timeout_ms: u32) -> ReportResult;
}
#[derive(Default, Clone)]
pub struct TrackedInput {
    keys: BTreeSet<u16>,
    contact: bool,
    quarantined: bool,
}
impl TrackedInput {
    pub fn quarantined(&self) -> bool {
        self.quarantined
    }
    pub fn execute<D: ReportDriver>(
        &mut self,
        driver: &mut D,
        transition: Transition,
        deadline: Instant,
        cancel: &Cancel,
    ) -> ReportResult {
        let rejected = |code| ReportResult {
            delivery: Delivery::NotSent,
            error: Some(Failure::code(code)),
        };
        if let Transition::TextSubmit(text) = transition {
            let had_text = match text.plan() {
                Ok(plan) => !plan.is_empty(),
                Err(code) => return rejected(code),
            };
            let first = self.execute(driver, Transition::Text(text), deadline, cancel);
            if first.delivery != Delivery::Sent || first.error.is_some() {
                return first;
            }
            let mut submit = self.execute(
                driver,
                Transition::Android(crate::api::AndroidCommand::PressButton {
                    button: crate::api::AndroidButton::Enter,
                }),
                deadline,
                cancel,
            );
            if had_text && (submit.delivery != Delivery::Sent || submit.error.is_some()) {
                submit.delivery = Delivery::Partial;
                self.quarantined = true;
            }
            return submit;
        }
        if let Transition::Text(text) = transition {
            if self.quarantined {
                return rejected(ErrorCode::BackendUnavailable);
            }
            if self.contact || !self.keys.is_empty() {
                return rejected(ErrorCode::InvalidRequest);
            }
            let plan = match text.plan() {
                Ok(p) => p,
                Err(e) => return rejected(e),
            };
            let mut sent = false;
            for chord in plan {
                let mut result = self.execute(driver, Transition::Batch(chord), deadline, cancel);
                if result.delivery != Delivery::Sent || result.error.is_some() {
                    if sent {
                        result.delivery = Delivery::Partial;
                        self.quarantined = true;
                    }
                    return result;
                }
                sent = true;
            }
            return ReportResult {
                delivery: Delivery::Sent,
                error: None,
            };
        }
        if let Transition::Gesture(gesture) = transition {
            if self.quarantined {
                return rejected(ErrorCode::BackendUnavailable);
            }
            if self.contact {
                return rejected(ErrorCode::InvalidRequest);
            }
            let plan = match gesture.plan() {
                Ok(p) => p,
                Err(e) => return rejected(e),
            };
            let mut start = Instant::now();
            if start
                .checked_add(plan.last().unwrap().0)
                .is_none_or(|end| end >= deadline)
            {
                return rejected(ErrorCode::ProcessTimeout);
            }
            let mut sent = false;
            for (offset, item) in plan {
                loop {
                    if cancel.cancelled() || driver.cancelled() || Instant::now() >= deadline {
                        if sent {
                            self.quarantined = true;
                        }
                        return ReportResult {
                            delivery: if sent {
                                Delivery::Partial
                            } else {
                                Delivery::NotSent
                            },
                            error: Some(Failure::code(
                                if cancel.cancelled() || driver.cancelled() {
                                    ErrorCode::ProcessCancelled
                                } else {
                                    ErrorCode::ProcessTimeout
                                },
                            )),
                        };
                    }
                    let now = Instant::now();
                    let target = start + offset;
                    if now >= target {
                        break;
                    }
                    std::thread::sleep((target - now).min(std::time::Duration::from_millis(2)));
                }
                let mut result = self.execute(driver, item, deadline, cancel);
                if result.delivery != Delivery::Sent || result.error.is_some() {
                    if sent {
                        result.delivery = Delivery::Partial;
                        self.quarantined = true;
                    }
                    return result;
                }
                // Dwell/interpolation duration begins at completed contact-down,
                // not before potentially slow guard/USB initialization work.
                if !sent {
                    start = Instant::now();
                }
                sent = true;
            }
            return ReportResult {
                delivery: Delivery::Sent,
                error: None,
            };
        }
        if let Transition::Batch(items) = transition {
            if items.is_empty() || items.len() > 64 {
                return rejected(ErrorCode::InvalidRequest);
            }
            let mut preview = self.clone();
            for item in &items {
                match item {
                    Transition::Key { usage, down }
                        if supported_key(*usage) && preview.keys.contains(usage) != *down =>
                    {
                        if *down {
                            preview.keys.insert(*usage);
                        } else {
                            preview.keys.remove(usage);
                        }
                    }
                    Transition::Contact { down, .. } if *down || preview.contact => {
                        preview.contact = *down
                    }
                    _ => return rejected(ErrorCode::InvalidRequest),
                }
            }
            let mut sent = false;
            for item in items {
                let mut result = self.execute(driver, item, deadline, cancel);
                if result.delivery != Delivery::Sent || result.error.is_some() {
                    if sent {
                        result.delivery = Delivery::Partial;
                        self.quarantined = true;
                    }
                    return result;
                }
                sent = true;
            }
            return ReportResult {
                delivery: Delivery::Sent,
                error: None,
            };
        }
        if self.quarantined {
            return rejected(ErrorCode::BackendUnavailable);
        }
        if cancel.cancelled() {
            return rejected(ErrorCode::ProcessCancelled);
        }
        if Instant::now() >= deadline {
            return rejected(ErrorCode::ProcessTimeout);
        }
        let valid = match &transition {
            Transition::Android(command) => {
                !self.contact && self.keys.is_empty() && command.validate().is_ok()
            }
            Transition::Key { usage, down } => {
                supported_key(*usage) && self.keys.contains(usage) != *down
            }
            Transition::Contact { down, .. } => *down || self.contact,
            Transition::Batch(_)
            | Transition::Gesture(_)
            | Transition::Text(_)
            | Transition::TextSubmit(_) => unreachable!(),
        };
        if !valid {
            return rejected(ErrorCode::InvalidRequest);
        }
        if let Err(e) = driver.apply(&transition) {
            // State setters may have partially changed driver state. Stop all
            // future submits until the owner has closed/neutralized the nodes.
            self.quarantined = true;
            return ReportResult {
                delivery: Delivery::NotSent,
                error: Some(e),
            };
        }
        let remaining = deadline
            .saturating_duration_since(Instant::now())
            .as_millis();
        if cancel.cancelled() || remaining == 0 {
            self.quarantined = true;
            return rejected(if cancel.cancelled() {
                ErrorCode::ProcessCancelled
            } else {
                ErrorCode::ProcessTimeout
            });
        }
        let result = driver.submit(&transition, remaining.min(500) as u32);
        if result.delivery != Delivery::Sent || result.error.is_some() {
            // Even a refused report can remain pending in libaoahid: a later
            // unrelated submit must never flush it accidentally.
            self.quarantined = !matches!(transition, Transition::Android(_))
                || result.error.is_some_and(|e| e.is_backend_failure());
        } else {
            match transition {
                Transition::Android(_) => {}
                Transition::Key { usage, down } => {
                    if down {
                        self.keys.insert(usage);
                    } else {
                        self.keys.remove(&usage);
                    }
                }
                Transition::Contact { down, .. } => self.contact = down,
                Transition::Batch(_)
                | Transition::Gesture(_)
                | Transition::Text(_)
                | Transition::TextSubmit(_) => {
                    unreachable!()
                }
            }
        }
        result
    }
}
