//! SOFTWARE FAKE public gesture execution, never USB/phone evidence.
use cordgeist::{api::ErrorCode, backend::Cancel, gesture::Gesture, session::*, session_input::*};
use std::{
    rc::Rc,
    sync::{Arc, Mutex},
    time::Duration,
};
#[derive(Clone, Copy)]
enum Fault {
    None,
    UnknownFirst,
    Release,
    SlowFirst,
}
struct Fake {
    events: Arc<Mutex<Vec<(bool, u16, u16)>>>,
    fault: Fault,
    entered: Option<tokio::sync::oneshot::Sender<()>>,
    completed: Option<Arc<Mutex<Vec<std::time::Instant>>>>,
}
impl SessionDriver for Fake {
    type Handle = Rc<()>;
    fn open(
        &mut self,
        _: Resource,
        _: &Handles<Self::Handle>,
        _: CallBudget,
        _: &Cancel,
    ) -> Result<Self::Handle, Failure> {
        Ok(Rc::new(()))
    }
    fn close(&mut self, r: Resource, _: &mut Self::Handle, _: CallBudget) -> CloseOutcome {
        CloseOutcome::Consumed(
            if matches!(self.fault, Fault::Release) && r == Resource::TouchNode {
                Err(Failure::code(ErrorCode::BackendUnavailable))
            } else {
                Ok(())
            },
        )
    }
    fn apply_report(
        &mut self,
        _: &Transition,
        _: &Handles<Self::Handle>,
        _: &Cancel,
        _: &Cancel,
    ) -> Result<(), Failure> {
        Ok(())
    }
    fn submit_report(
        &mut self,
        t: &Transition,
        _: &Handles<Self::Handle>,
        b: CallBudget,
        _: &Cancel,
        _: &Cancel,
    ) -> ReportResult {
        assert!((1..=500).contains(&b.control_ms));
        let Transition::Contact { down, x, y } = t else {
            panic!("expected contact")
        };
        let first = {
            let mut e = self.events.lock().unwrap();
            let first = e.is_empty();
            e.push((*down, *x, *y));
            first
        };
        if let Some(entered) = self.entered.take() {
            let _ = entered.send(());
        }
        if first && matches!(self.fault, Fault::SlowFirst) {
            std::thread::sleep(Duration::from_millis(100));
        }
        if let Some(completed) = &self.completed {
            completed.lock().unwrap().push(std::time::Instant::now());
        }
        if (first && matches!(self.fault, Fault::UnknownFirst))
            || (!*down && matches!(self.fault, Fault::Release))
        {
            ReportResult {
                delivery: Delivery::Unknown,
                // SOFTWARE FAKE transport timeout, distinct from a caller deadline.
                error: Some(Failure {
                    native_code: -7,
                    transport_status: -7,
                    ..Failure::code(ErrorCode::ProcessTimeout)
                }),
            }
        } else {
            ReportResult {
                delivery: Delivery::Sent,
                error: None,
            }
        }
    }
}
type Events = Arc<Mutex<Vec<(bool, u16, u16)>>>;
async fn owner(
    fault: Fault,
    entered: Option<tokio::sync::oneshot::Sender<()>>,
) -> (Session, Events) {
    let events = Arc::new(Mutex::new(vec![]));
    let e = events.clone();
    let s = Session::start(Duration::from_secs(1), move || {
        Ok(Fake {
            events: e,
            fault,
            entered,
            completed: None,
        })
    })
    .unwrap();
    s.ready(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    (s, events)
}
fn tap() -> Transition {
    Transition::Gesture(Gesture::Tap {
        at: (10, 20),
        duration: Duration::from_millis(20),
    })
}
#[tokio::test]
async fn public_gestures_complete_and_release_exact_endpoints() {
    let (s, e) = owner(Fault::None, None).await;
    for gesture in [
        Gesture::Tap {
            at: (10, 20),
            duration: Duration::from_millis(20),
        },
        Gesture::Hold {
            at: (10, 20),
            duration: Duration::from_millis(30),
        },
        Gesture::Swipe {
            from: (10, 20),
            to: (100, 200),
            duration: Duration::from_millis(40),
        },
    ] {
        e.lock().unwrap().clear();
        assert_eq!(
            s.submit(
                Transition::Gesture(gesture),
                Duration::from_secs(1),
                &Cancel::default()
            )
            .await
            .delivery,
            Delivery::Sent
        );
        let events = e.lock().unwrap();
        assert_eq!(events.first(), Some(&(true, 10, 20)));
        assert!(!events.last().unwrap().0);
        if events.len() > 2 {
            assert_eq!(events.last(), Some(&(false, 100, 200)));
            assert_eq!(events.len(), 18);
        }
    }
    s.shutdown(Duration::from_secs(1)).await.unwrap();
}
#[tokio::test]
async fn invalid_duration_and_insufficient_deadline_have_zero_effects() {
    let (s, e) = owner(Fault::None, None).await;
    for duration in [
        Duration::ZERO,
        Duration::from_millis(19),
        Duration::from_secs(11),
        Duration::from_nanos(20_000_001),
    ] {
        assert_eq!(
            s.submit(
                Transition::Gesture(Gesture::Hold {
                    at: (1, 2),
                    duration
                }),
                Duration::from_secs(1),
                &Cancel::default()
            )
            .await
            .delivery,
            Delivery::NotSent
        );
    }
    assert_eq!(
        s.submit(tap(), Duration::from_millis(10), &Cancel::default())
            .await
            .delivery,
        Delivery::NotSent
    );
    assert!(e.lock().unwrap().is_empty());
    s.shutdown(Duration::from_secs(1)).await.unwrap();
}
#[tokio::test]
async fn unknown_and_release_failure_fence_without_replaying_gesture() {
    for fault in [Fault::UnknownFirst, Fault::Release] {
        let (s, e) = owner(fault, None).await;
        let result = s
            .submit(tap(), Duration::from_secs(1), &Cancel::default())
            .await;
        assert_eq!(
            result.delivery,
            if matches!(fault, Fault::Release) {
                Delivery::Partial
            } else {
                Delivery::Unknown
            }
        );
        let count = e.lock().unwrap().len();
        assert_eq!(
            s.submit(tap(), Duration::from_secs(1), &Cancel::default())
                .await
                .delivery,
            Delivery::NotSent
        );
        let closed = s.shutdown(Duration::from_secs(1)).await.unwrap();
        if matches!(fault, Fault::Release) {
            assert!(closed.cleanup_error.is_some());
        }
        assert_eq!(e.lock().unwrap().len(), count);
    }
}
#[tokio::test]
async fn mid_gesture_cancel_and_deadline_preserve_uncertainty_and_cleanup() {
    let (entered, receive) = tokio::sync::oneshot::channel();
    let (s, e) = owner(Fault::None, Some(entered)).await;
    let c = Cancel::default();
    let cancel = c.clone();
    let client = s.clone();
    let task = tokio::spawn(async move {
        client
            .submit(
                Transition::Gesture(Gesture::Hold {
                    at: (1, 2),
                    duration: Duration::from_millis(200),
                }),
                Duration::from_secs(1),
                &cancel,
            )
            .await
    });
    receive.await.unwrap();
    c.cancel();
    assert_eq!(task.await.unwrap().delivery, Delivery::Unknown);
    s.shutdown(Duration::from_secs(1)).await.unwrap();
    assert_eq!(e.lock().unwrap().len(), 1);
    let (s, e) = owner(Fault::SlowFirst, None).await;
    assert_eq!(
        s.submit(tap(), Duration::from_millis(60), &Cancel::default())
            .await
            .delivery,
        Delivery::Unknown
    );
    s.shutdown(Duration::from_secs(1)).await.unwrap();
    assert_eq!(e.lock().unwrap().len(), 1);
}

#[tokio::test]
async fn hold_duration_begins_after_down_completion() {
    let completed = Arc::new(Mutex::new(vec![]));
    let times = completed.clone();
    let s = Session::start(Duration::from_secs(1), move || {
        Ok(Fake {
            events: Arc::new(Mutex::new(vec![])),
            fault: Fault::SlowFirst,
            entered: None,
            completed: Some(times),
        })
    })
    .unwrap();
    s.ready(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    let result = s
        .submit(
            Transition::Gesture(Gesture::Hold {
                at: (1, 2),
                duration: Duration::from_millis(80),
            }),
            Duration::from_secs(1),
            &Cancel::default(),
        )
        .await;
    assert_eq!(result.delivery, Delivery::Sent);
    {
        let times = completed.lock().unwrap();
        assert_eq!(times.len(), 2);
        assert!(times[1].duration_since(times[0]) >= Duration::from_millis(80));
    }
    s.shutdown(Duration::from_secs(1)).await.unwrap();
}
