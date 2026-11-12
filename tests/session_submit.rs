//! SOFTWARE FAKE public Session submissions; no native/USB operation.
use cordgeist::{api::ErrorCode, backend::Cancel, session::*, session_input::*};
use std::{
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
    },
    time::Duration,
};
struct Fake {
    calls: Arc<AtomicUsize>,
    result: ReportResult,
    fail_cleanup: bool,
    fail_after_first: bool,
}
impl SessionDriver for Fake {
    type Handle = ();
    fn open(
        &mut self,
        _: Resource,
        _: &Handles<()>,
        _: CallBudget,
        _: &Cancel,
    ) -> Result<(), Failure> {
        Ok(())
    }
    fn close(&mut self, r: Resource, _: &mut (), _: CallBudget) -> CloseOutcome {
        CloseOutcome::Consumed(if self.fail_cleanup && r == Resource::KeyboardNode {
            Err(Failure::code(ErrorCode::BackendUnavailable))
        } else {
            Ok(())
        })
    }
    fn apply_report(
        &mut self,
        _: &Transition,
        _: &Handles<()>,
        _: &Cancel,
        _: &Cancel,
    ) -> Result<(), Failure> {
        Ok(())
    }
    fn submit_report(
        &mut self,
        _: &Transition,
        _: &Handles<()>,
        b: CallBudget,
        _: &Cancel,
        _: &Cancel,
    ) -> ReportResult {
        assert!((1..=500).contains(&b.control_ms));
        let index = self.calls.fetch_add(1, Ordering::SeqCst);
        if self.fail_after_first && index == 0 {
            ReportResult {
                delivery: Delivery::Sent,
                error: None,
            }
        } else {
            self.result
        }
    }
}
fn key(down: bool) -> Transition {
    Transition::Key { usage: 4, down }
}
async fn owner(result: ReportResult, fail_cleanup: bool) -> (Session, Arc<AtomicUsize>) {
    let calls = Arc::new(AtomicUsize::new(0));
    let c = calls.clone();
    let session = Session::start(Duration::from_secs(1), move || {
        Ok(Fake {
            calls: c,
            result,
            fail_cleanup,
            fail_after_first: false,
        })
    })
    .unwrap();
    session
        .ready(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    (session, calls)
}
#[tokio::test]
async fn invalid_and_cancelled_requests_have_zero_submits() {
    let (s, c) = owner(
        ReportResult {
            delivery: Delivery::Sent,
            error: None,
        },
        false,
    )
    .await;
    let cancel = Cancel::default();
    cancel.cancel();
    assert_eq!(
        s.submit(key(true), Duration::from_secs(1), &cancel)
            .await
            .delivery,
        Delivery::NotSent
    );
    assert_eq!(
        s.submit(key(false), Duration::from_secs(1), &Cancel::default())
            .await
            .delivery,
        Delivery::NotSent
    );
    assert_eq!(
        s.submit(key(true), Duration::ZERO, &Cancel::default())
            .await
            .delivery,
        Delivery::NotSent
    );
    assert_eq!(c.load(Ordering::SeqCst), 0);
    s.shutdown(Duration::from_secs(1)).await.unwrap();
}
#[tokio::test]
async fn refused_and_unknown_reports_are_never_blindly_retried() {
    for delivery in [Delivery::NotSent, Delivery::Unknown] {
        let expected = ReportResult {
            delivery,
            error: Some(Failure {
                native_code: -7,
                transport_status: -7,
                ..Failure::code(ErrorCode::ProcessTimeout)
            }),
        };
        let (s, c) = owner(expected, false).await;
        assert_eq!(
            s.submit(key(true), Duration::from_secs(1), &Cancel::default())
                .await,
            expected
        );
        assert_eq!(
            s.submit(key(true), Duration::from_secs(1), &Cancel::default())
                .await
                .delivery,
            Delivery::NotSent
        );
        s.shutdown(Duration::from_secs(1)).await.unwrap();
        assert_eq!(c.load(Ordering::SeqCst), 1);
    }
}
#[tokio::test]
async fn tracked_duplicate_rejects_and_cleanup_failure_does_not_erase_delivery() {
    let (s, c) = owner(
        ReportResult {
            delivery: Delivery::Sent,
            error: None,
        },
        true,
    )
    .await;
    let cancel = Cancel::default();
    let receipt = s.submit(key(true), Duration::from_secs(1), &cancel).await;
    assert_eq!(receipt.delivery, Delivery::Sent);
    assert_eq!(
        s.submit(key(true), Duration::from_secs(1), &cancel)
            .await
            .delivery,
        Delivery::NotSent
    );
    let closed = s.shutdown(Duration::from_secs(1)).await.unwrap();
    assert!(closed.cleanup_error.is_some());
    assert_eq!(receipt.delivery, Delivery::Sent);
    assert!(
        !cancel.cancelled(),
        "owner shutdown must not cancel caller-owned token"
    );
    assert_eq!(c.load(Ordering::SeqCst), 1);
}

#[tokio::test]
async fn batch_prevalidates_before_effects_and_preserves_partial_delivery() {
    let calls = Arc::new(AtomicUsize::new(0));
    let c = calls.clone();
    let s = Session::start(Duration::from_secs(1), move || {
        Ok(Fake {
            calls: c,
            fail_cleanup: true,
            fail_after_first: true,
            result: ReportResult {
                delivery: Delivery::Unknown,
                error: Some(Failure::code(ErrorCode::ProcessTimeout)),
            },
        })
    })
    .unwrap();
    s.ready(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    let invalid = Transition::Batch(vec![key(true), key(true)]);
    assert_eq!(
        s.submit(invalid, Duration::from_secs(1), &Cancel::default())
            .await
            .delivery,
        Delivery::NotSent
    );
    assert_eq!(calls.load(Ordering::SeqCst), 0);
    let result = s
        .submit(
            Transition::Batch(vec![key(true), key(false)]),
            Duration::from_secs(1),
            &Cancel::default(),
        )
        .await;
    assert_eq!(result.delivery, Delivery::Partial);
    assert_eq!(
        s.submit(key(true), Duration::from_secs(1), &Cancel::default())
            .await
            .delivery,
        Delivery::NotSent
    );
    assert!(
        s.shutdown(Duration::from_secs(1))
            .await
            .unwrap()
            .cleanup_error
            .is_some()
    );
    assert_eq!(calls.load(Ordering::SeqCst), 2);
}

struct Blocking {
    inner: Fake,
    entered: std::sync::mpsc::Sender<()>,
    release: std::sync::mpsc::Receiver<()>,
}
impl SessionDriver for Blocking {
    type Handle = ();
    fn open(
        &mut self,
        r: Resource,
        h: &Handles<()>,
        b: CallBudget,
        c: &Cancel,
    ) -> Result<(), Failure> {
        self.inner.open(r, h, b, c)
    }
    fn close(&mut self, r: Resource, h: &mut (), b: CallBudget) -> CloseOutcome {
        self.inner.close(r, h, b)
    }
    fn apply_report(
        &mut self,
        t: &Transition,
        h: &Handles<()>,
        c: &Cancel,
        external: &Cancel,
    ) -> Result<(), Failure> {
        self.inner.apply_report(t, h, c, external)
    }
    fn submit_report(
        &mut self,
        t: &Transition,
        h: &Handles<()>,
        b: CallBudget,
        c: &Cancel,
        external: &Cancel,
    ) -> ReportResult {
        self.entered.send(()).unwrap();
        self.release.recv_timeout(Duration::from_secs(2)).unwrap();
        self.inner.submit_report(t, h, b, c, external)
    }
}
#[tokio::test]
async fn in_flight_cancellation_is_unknown_and_retains_owner_until_completion() {
    let calls = Arc::new(AtomicUsize::new(0));
    let c = calls.clone();
    let (entered, receive) = std::sync::mpsc::channel();
    let (release, wait) = std::sync::mpsc::channel();
    let s = Session::start(Duration::from_secs(1), move || {
        Ok(Blocking {
            inner: Fake {
                calls: c,
                fail_cleanup: true,
                fail_after_first: false,
                result: ReportResult {
                    delivery: Delivery::Sent,
                    error: None,
                },
            },
            entered,
            release: wait,
        })
    })
    .unwrap();
    s.ready(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    let cancel = Cancel::default();
    let owner = s.clone();
    let signal = cancel.clone();
    let task = tokio::spawn(async move {
        owner
            .submit(key(true), Duration::from_secs(1), &signal)
            .await
    });
    tokio::time::timeout(Duration::from_secs(1), async {
        loop {
            if receive.try_recv().is_ok() {
                break;
            }
            tokio::task::yield_now().await;
        }
    })
    .await
    .unwrap();
    cancel.cancel();
    assert_eq!(task.await.unwrap().delivery, Delivery::Unknown);
    assert_eq!(
        s.shutdown(Duration::from_millis(20)).await,
        Err(ErrorCode::NativeCleanupPending)
    );
    release.send(()).unwrap();
    assert!(
        s.shutdown(Duration::from_secs(1))
            .await
            .unwrap()
            .cleanup_error
            .is_some()
    );
    assert_eq!(
        s.submit(key(true), Duration::from_secs(1), &Cancel::default())
            .await
            .delivery,
        Delivery::NotSent
    );
    assert_eq!(calls.load(Ordering::SeqCst), 1);
}

#[tokio::test]
async fn descriptor_gap_is_rejected_before_any_batch_effect_and_boundaries_work() {
    let (s, c) = owner(
        ReportResult {
            delivery: Delivery::Sent,
            error: None,
        },
        false,
    )
    .await;
    for usage in [0xde, 0xdf] {
        let request = Transition::Batch(vec![key(true), Transition::Key { usage, down: true }]);
        assert_eq!(
            s.submit(request, Duration::from_secs(1), &Cancel::default())
                .await
                .delivery,
            Delivery::NotSent
        );
        assert_eq!(c.load(Ordering::SeqCst), 0);
    }
    for usage in [0x04, 0xdd, 0xe0, 0xe7] {
        let request = Transition::Batch(vec![
            Transition::Key { usage, down: true },
            Transition::Key { usage, down: false },
        ]);
        assert_eq!(
            s.submit(request, Duration::from_secs(1), &Cancel::default())
                .await
                .delivery,
            Delivery::Sent
        );
    }
    assert_eq!(c.load(Ordering::SeqCst), 8);
    s.shutdown(Duration::from_secs(1)).await.unwrap();
}

struct Guarded {
    inner: Fake,
    lost: Arc<std::sync::atomic::AtomicBool>,
    pending: Arc<std::sync::atomic::AtomicBool>,
    reason: ErrorCode,
}
impl SessionDriver for Guarded {
    type Handle = ();
    fn open(
        &mut self,
        r: Resource,
        h: &Handles<()>,
        b: CallBudget,
        c: &Cancel,
    ) -> Result<(), Failure> {
        self.inner.open(r, h, b, c)
    }
    fn validate_report(&mut self, _: &Transition, _: &Cancel, _: &Cancel) -> Result<(), Failure> {
        if self.lost.load(Ordering::SeqCst) {
            Err(Failure::code(self.reason))
        } else {
            Ok(())
        }
    }
    fn apply_report(
        &mut self,
        t: &Transition,
        h: &Handles<()>,
        c: &Cancel,
        e: &Cancel,
    ) -> Result<(), Failure> {
        self.inner.apply_report(t, h, c, e)
    }
    fn submit_report(
        &mut self,
        t: &Transition,
        h: &Handles<()>,
        b: CallBudget,
        c: &Cancel,
        e: &Cancel,
    ) -> ReportResult {
        self.inner.submit_report(t, h, b, c, e)
    }
    fn close(&mut self, r: Resource, h: &mut (), b: CallBudget) -> CloseOutcome {
        if r == Resource::KeyboardNode && self.pending.load(Ordering::SeqCst) {
            CloseOutcome::Pending
        } else {
            self.inner.close(r, h, b)
        }
    }
}
#[tokio::test]
async fn authority_loss_with_held_input_fences_and_automatically_cleans() {
    use std::sync::atomic::AtomicBool;
    for reason in [
        ErrorCode::LeaseInvalid,
        ErrorCode::DeviceDisconnected,
        ErrorCode::MappingUnverified,
    ] {
        let lost = Arc::new(AtomicBool::new(false));
        let pending = Arc::new(AtomicBool::new(true));
        let calls = Arc::new(AtomicUsize::new(0));
        let (l, p, c) = (lost.clone(), pending.clone(), calls.clone());
        let s = Session::start(Duration::from_secs(1), move || {
            Ok(Guarded {
                inner: Fake {
                    calls: c,
                    result: ReportResult {
                        delivery: Delivery::Sent,
                        error: None,
                    },
                    fail_cleanup: true,
                    fail_after_first: false,
                },
                lost: l,
                pending: p,
                reason,
            })
        })
        .unwrap();
        s.ready(Duration::from_secs(1), &Cancel::default())
            .await
            .unwrap();
        let held = s
            .submit(
                Transition::Batch(vec![
                    key(true),
                    Transition::Contact {
                        down: true,
                        x: 1,
                        y: 1,
                    },
                ]),
                Duration::from_secs(1),
                &Cancel::default(),
            )
            .await;
        assert_eq!(held.delivery, Delivery::Sent);
        lost.store(true, Ordering::SeqCst);
        let rejected = s
            .submit(key(false), Duration::from_secs(1), &Cancel::default())
            .await;
        assert_eq!(rejected.delivery, Delivery::NotSent);
        assert_eq!(rejected.error.unwrap().code, reason);
        tokio::time::sleep(Duration::from_millis(30)).await;
        assert_eq!(
            s.status().phase,
            Phase::Quarantined,
            "authority loss must automatically initiate cleanup"
        );
        lost.store(false, Ordering::SeqCst);
        assert_eq!(
            s.submit(key(false), Duration::from_secs(1), &Cancel::default())
                .await
                .delivery,
            Delivery::NotSent
        );
        assert_eq!(calls.load(Ordering::SeqCst), 2);
        pending.store(false, Ordering::SeqCst);
        let closed = s.shutdown(Duration::from_secs(1)).await.unwrap();
        assert!(closed.cleanup_error.is_some());
        assert_eq!(closed.terminal_error.unwrap().code, reason);
        assert_eq!(held.delivery, Delivery::Sent);
    }
}

#[tokio::test]
async fn adb_deadline_unknown_does_not_poison_hid_reports() {
    let (s, calls) = owner(
        ReportResult {
            delivery: Delivery::Unknown,
            error: Some(Failure::at(ErrorCode::ProcessTimeout, "adb-app-launch")),
        },
        false,
    )
    .await;
    let result = s
        .submit(
            Transition::Android(cordgeist::api::AndroidCommand::LaunchApp {
                package: "org.example.synthetic".into(),
            }),
            Duration::from_secs(1),
            &Cancel::default(),
        )
        .await;
    assert_eq!(result.delivery, Delivery::Unknown);
    assert_eq!(s.status().phase, Phase::Ready);
    assert!(s.status().terminal_error.is_none());
    assert_eq!(calls.load(Ordering::SeqCst), 1);
    s.shutdown(Duration::from_secs(1)).await.unwrap();
}
