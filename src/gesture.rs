//! Bounded single-contact HID-space plans. No inferred calibration or payload logs.
use crate::{api::ErrorCode, session_input::Transition};
use std::time::Duration;
#[derive(Clone)]
pub enum Gesture {
    Tap {
        at: (u16, u16),
        duration: Duration,
    },
    Hold {
        at: (u16, u16),
        duration: Duration,
    },
    Swipe {
        from: (u16, u16),
        to: (u16, u16),
        duration: Duration,
    },
}
impl Gesture {
    pub fn plan(&self) -> Result<Vec<(Duration, Transition)>, ErrorCode> {
        let (from, to, duration, segments) = match *self {
            Self::Tap { at, duration } | Self::Hold { at, duration } => (at, at, duration, 1u32),
            Self::Swipe { from, to, duration } => (from, to, duration, 16u32),
        };
        if duration < Duration::from_millis(20)
            || duration > Duration::from_secs(10)
            || duration.as_nanos() % 1_000_000 != 0
        {
            return Err(ErrorCode::InvalidRequest);
        }
        let mut plan = Vec::with_capacity(segments as usize + 2);
        plan.push((
            Duration::ZERO,
            Transition::Contact {
                down: true,
                x: from.0,
                y: from.1,
            },
        ));
        if segments > 1 {
            for index in 1..=segments {
                let interpolate = |a: u16, b: u16| -> u16 {
                    ((i64::from(a) * (i64::from(segments - index))
                        + i64::from(b) * i64::from(index)
                        + i64::from(segments) / 2)
                        / i64::from(segments)) as u16
                };
                plan.push((
                    duration * index / segments,
                    Transition::Contact {
                        down: true,
                        x: interpolate(from.0, to.0),
                        y: interpolate(from.1, to.1),
                    },
                ));
            }
        }
        plan.push((
            duration,
            Transition::Contact {
                down: false,
                x: to.0,
                y: to.1,
            },
        ));
        Ok(plan)
    }
}
