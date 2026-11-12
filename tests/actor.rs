//! SOFTWARE FAKE Session driver; no native handles or device proof.
use cordgeist::{actor::*, api::ErrorCode, backend::Cancel, session::*, session_input::*};
use std::{
    sync::{
        Arc,
        atomic::{AtomicUsize, Ordering},
        mpsc,
    },
    time::Duration,
};
struct Fake {
    mode: Arc<AtomicUsize>,
    effects: Arc<AtomicUsize>,
    gate: Option<(tokio::sync::oneshot::Sender<()>, mpsc::Receiver<()>)>,
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
    fn close(&mut self, _: Resource, _: &mut (), _: CallBudget) -> CloseOutcome {
        match self.mode.load(Ordering::SeqCst) {
            1 => CloseOutcome::Pending,
            2 => CloseOutcome::Consumed(Err(Failure::code(ErrorCode::BackendUnavailable))),
            _ => CloseOutcome::Consumed(Ok(())),
        }
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
        t: &Transition,
        _: &Handles<()>,
        b: CallBudget,
        _: &Cancel,
        cancel: &Cancel,
    ) -> ReportResult {
        assert!((1..=500).contains(&b.control_ms));
        if let Some((entered, release)) = self.gate.take() {
            entered.send(()).unwrap();
            release.recv_timeout(Duration::from_millis(500)).unwrap();
        }
        if self.mode.load(Ordering::SeqCst) == 3 {
            return ReportResult {
                delivery: Delivery::Unknown,
                error: Some(Failure::code(ErrorCode::BackendUnavailable)),
            };
        }
        if cancel.cancelled() {
            return ReportResult {
                delivery: Delivery::NotSent,
                error: Some(Failure::code(ErrorCode::ProcessCancelled)),
            };
        }
        if matches!(t, Transition::Key { down: true, .. }) {
            self.effects.fetch_add(1, Ordering::SeqCst);
        }
        ReportResult {
            delivery: Delivery::Sent,
            error: None,
        }
    }
}
async fn actor(
    gate: Option<(tokio::sync::oneshot::Sender<()>, mpsc::Receiver<()>)>,
) -> (DeviceActor, Arc<AtomicUsize>) {
    actor_mode(gate, Arc::new(AtomicUsize::new(0))).await
}
async fn actor_mode(
    gate: Option<(tokio::sync::oneshot::Sender<()>, mpsc::Receiver<()>)>,
    mode: Arc<AtomicUsize>,
) -> (DeviceActor, Arc<AtomicUsize>) {
    let effects = Arc::new(AtomicUsize::new(0));
    let e = effects.clone();
    let session = Session::start_with_registration(
        Duration::from_secs(1),
        Registration {
            settling: Duration::ZERO,
        },
        move || {
            Ok(Fake {
                effects: e,
                gate,
                mode,
            })
        },
    )
    .unwrap();
    session
        .ready(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    (DeviceActor::start(session), effects)
}
fn key() -> Transition {
    Transition::Key {
        usage: 4,
        down: true,
    }
}
#[tokio::test]
async fn saturation_does_not_block_other_device_status_or_invalidation() {
    let (entered, ready) = tokio::sync::oneshot::channel();
    let (release, wait) = mpsc::channel();
    let (a, ae) = actor(Some((entered, wait))).await;
    let (b, be) = actor(None).await;
    let active = a.enqueue(key(), Duration::from_secs(1)).unwrap();
    tokio::time::timeout(Duration::from_secs(1), ready)
        .await
        .unwrap()
        .unwrap();
    let mut queued = Vec::new();
    for _ in 0..QUEUE_CAPACITY {
        queued.push(a.enqueue(key(), Duration::from_secs(1)).unwrap());
    }
    assert!(matches!(
        a.enqueue(key(), Duration::from_secs(1)),
        Err(ErrorCode::QueueFull)
    ));
    assert_eq!(a.status().queued, QUEUE_CAPACITY);
    assert!(a.status().active);
    assert_eq!(
        b.enqueue(key(), Duration::from_secs(1))
            .unwrap()
            .wait()
            .await
            .delivery,
        Delivery::Sent
    );
    assert_eq!(be.load(Ordering::SeqCst), 1);
    a.control(Control::HealthInvalid);
    assert!(a.status().fenced);
    for ticket in queued {
        assert_eq!(ticket.wait().await.delivery, Delivery::NotSent);
    }
    release.send(()).unwrap();
    assert_eq!(active.wait().await.delivery, Delivery::Unknown);
    assert_eq!(ae.load(Ordering::SeqCst), 0);
    assert!(a.enqueue(key(), Duration::from_secs(1)).is_err());
    a.shutdown(Duration::from_secs(1)).await.unwrap();
    b.shutdown(Duration::from_secs(1)).await.unwrap();
}

#[tokio::test]
async fn pending_and_failed_cleanup_remain_fenced_and_separate_from_delivery() {
    let mode = Arc::new(AtomicUsize::new(1));
    let (a, e) = actor_mode(None, mode.clone()).await;
    assert_eq!(
        a.enqueue(key(), Duration::from_secs(1))
            .unwrap()
            .wait()
            .await
            .delivery,
        Delivery::Sent
    );
    assert_eq!(e.load(Ordering::SeqCst), 1);
    assert!(matches!(
        a.shutdown(Duration::from_millis(10)).await,
        Err(ErrorCode::NativeCleanupPending)
    ));
    assert!(a.status().fenced);
    assert!(a.enqueue(key(), Duration::from_secs(1)).is_err());
    mode.store(2, Ordering::SeqCst);
    let status = a.shutdown(Duration::from_secs(1)).await.unwrap();
    assert_eq!(status.phase, Phase::Closed);
    assert!(status.cleanup_error.is_some());
    assert_eq!(e.load(Ordering::SeqCst), 1);
}
#[tokio::test]
async fn queued_cancel_and_expiry_have_no_backend_effects() {
    let (entered, ready) = tokio::sync::oneshot::channel();
    let (release, wait) = mpsc::channel();
    let (a, e) = actor(Some((entered, wait))).await;
    let active = a.enqueue(key(), Duration::from_secs(1)).unwrap();
    tokio::time::timeout(Duration::from_secs(1), ready)
        .await
        .unwrap()
        .unwrap();
    let cancelled = a.enqueue(key(), Duration::from_secs(1)).unwrap();
    cancelled.cancel();
    let expired = a.enqueue(key(), Duration::from_nanos(1)).unwrap();
    let expired_result = expired.wait().await;
    assert_eq!(expired_result.delivery, Delivery::NotSent);
    assert_eq!(
        expired_result.error.unwrap().code,
        ErrorCode::ProcessTimeout
    );
    release.send(()).unwrap();
    assert_eq!(active.wait().await.delivery, Delivery::Sent);
    assert_eq!(cancelled.wait().await.delivery, Delivery::NotSent);
    assert_eq!(e.load(Ordering::SeqCst), 1);
    a.shutdown(Duration::from_secs(1)).await.unwrap();
}

#[tokio::test]
async fn terminal_backend_failure_fences_and_rejects_unsafe_followup() {
    let (a, e) = actor_mode(None, Arc::new(AtomicUsize::new(3))).await;
    let result = a
        .enqueue(key(), Duration::from_secs(1))
        .unwrap()
        .wait()
        .await;
    assert_eq!(result.delivery, Delivery::Unknown);
    assert_eq!(result.error.unwrap().code, ErrorCode::BackendUnavailable);
    assert!(a.status().fenced);
    assert!(!a.status().active);
    assert!(a.enqueue(key(), Duration::from_secs(1)).is_err());
    assert_eq!(e.load(Ordering::SeqCst), 0);
    a.shutdown(Duration::from_secs(1)).await.unwrap();
}
#[tokio::test]
async fn excessive_payload_is_rejected_without_queue_or_backend_mutation() {
    let (a, e) = actor(None).await;
    assert!(matches!(
        a.enqueue(Transition::Batch(vec![key(); 65]), Duration::from_secs(1)),
        Err(ErrorCode::InvalidRequest)
    ));
    assert!(
        a.enqueue(
            Transition::Text(cordgeist::ascii::Text::new("us-ansi", "x".repeat(4097))),
            Duration::from_secs(1)
        )
        .is_err()
    );
    assert_eq!(a.status().queued, 0);
    assert_eq!(e.load(Ordering::SeqCst), 0);
    a.shutdown(Duration::from_secs(1)).await.unwrap();
}

// SOFTWARE FAKE records one effect before the bounded foreign-call gate.
struct EffectBeforeBlock {
    effects: Arc<AtomicUsize>,
    entered: Option<tokio::sync::oneshot::Sender<()>>,
    release: mpsc::Receiver<()>,
    cancellation_seen: Arc<std::sync::atomic::AtomicBool>,
}
impl SessionDriver for EffectBeforeBlock {
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
    fn close(&mut self, _: Resource, _: &mut (), _: CallBudget) -> CloseOutcome {
        CloseOutcome::Consumed(Ok(()))
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
        _: CallBudget,
        internal: &Cancel,
        external: &Cancel,
    ) -> ReportResult {
        if let Some(entered) = self.entered.take() {
            self.effects.fetch_add(1, Ordering::SeqCst);
            entered.send(()).unwrap();
            self.release
                .recv_timeout(Duration::from_millis(500))
                .unwrap();
            self.cancellation_seen.store(
                internal.cancelled() || external.cancelled(),
                Ordering::SeqCst,
            );
        }
        ReportResult {
            delivery: Delivery::Sent,
            error: None,
        }
    }
}
#[test]
fn lost_reply_after_effect_and_runtime_shutdown_remains_unknown() {
    let effects = Arc::new(AtomicUsize::new(0));
    let seen = Arc::new(std::sync::atomic::AtomicBool::new(false));
    let e = effects.clone();
    let c = seen.clone();
    let (entered, ready) = tokio::sync::oneshot::channel();
    let (release, wait) = mpsc::channel();
    let runtime = tokio::runtime::Runtime::new().unwrap();
    let (actor, ticket) = runtime.block_on(async {
        let session = Session::start_with_registration(
            Duration::from_secs(1),
            Registration {
                settling: Duration::ZERO,
            },
            move || {
                Ok(EffectBeforeBlock {
                    effects: e,
                    entered: Some(entered),
                    release: wait,
                    cancellation_seen: c,
                })
            },
        )
        .unwrap();
        session
            .ready(Duration::from_secs(1), &Cancel::default())
            .await
            .unwrap();
        let actor = DeviceActor::start(session);
        let ticket = actor.enqueue(key(), Duration::from_secs(1)).unwrap();
        tokio::time::timeout(Duration::from_secs(1), ready)
            .await
            .unwrap()
            .unwrap();
        (actor, ticket)
    });
    runtime.shutdown_background();
    let other = tokio::runtime::Runtime::new().unwrap();
    let result = other.block_on(ticket.wait());
    // Always release and consume cleanup before assertions, including red runs.
    release.send(()).unwrap();
    let cleanup = other
        .block_on(actor.shutdown(Duration::from_secs(1)))
        .unwrap();
    assert_eq!(cleanup.phase, Phase::Closed);
    assert_eq!(effects.load(Ordering::SeqCst), 1);
    assert!(seen.load(Ordering::SeqCst));
    assert_eq!(result.error.unwrap().code, ErrorCode::BackendUnavailable);
    assert_eq!(
        result.delivery,
        Delivery::Unknown,
        "lost receipt cannot prove an already-claimed report was not sent"
    );
}

#[tokio::test]
async fn deadline_uncertainty_keeps_owner_ready_after_neutralization() {
    let (entered, ready) = tokio::sync::oneshot::channel();
    let (release, wait) = mpsc::channel();
    let (actor, _) = actor(Some((entered, wait))).await;
    let ticket = actor.enqueue(key(), Duration::from_millis(60)).unwrap();
    ready.await.unwrap();
    let result = ticket.wait().await;
    assert_eq!(result.delivery, Delivery::Unknown);
    assert_eq!(result.error.unwrap().code, ErrorCode::ProcessTimeout);
    assert!(!actor.status().fenced);
    release.send(()).unwrap();
    tokio::time::sleep(Duration::from_millis(50)).await;
    let status = actor.status();
    assert!(!status.fenced);
    assert_eq!(status.session.phase, Phase::Ready);
    assert!(status.session.cleanup_error.is_none());
    actor.shutdown(Duration::from_secs(1)).await.unwrap();
}

#[tokio::test]
async fn real_backend_error_still_fences_and_closes_owner() {
    let mode = Arc::new(AtomicUsize::new(3));
    let (actor, _) = actor_mode(None, mode).await;
    let result = actor
        .enqueue(key(), Duration::from_secs(1))
        .unwrap()
        .wait()
        .await;
    assert_eq!(result.delivery, Delivery::Unknown);
    tokio::time::sleep(Duration::from_millis(30)).await;
    assert!(actor.status().fenced);
    assert_eq!(
        actor.status().termination,
        Some(TerminationReason::BackendFailure)
    );
    actor.shutdown(Duration::from_secs(1)).await.unwrap();
}
