//! Explicit SOFTWARE FAKE lifecycle evidence; no physical input/native handles.
use cordgeist::{
    actor::TerminationReason,
    api::*,
    backend::{Cancel, Clock},
    lease_devices::LeaseDevices,
    session::*,
    session_input::*,
};
use std::{
    sync::{
        Arc, Mutex,
        atomic::{AtomicU64, AtomicUsize, Ordering},
        mpsc,
    },
    time::Duration,
};
#[derive(Default)]
struct FakeClock(AtomicU64);
impl Clock for FakeClock {
    fn now_ms(&self) -> u64 {
        self.0.load(Ordering::SeqCst)
    }
}
type Gate = (tokio::sync::oneshot::Sender<()>, mpsc::Receiver<()>);
#[derive(Default)]
struct Evidence {
    close_mode: AtomicUsize,
    effects: AtomicUsize,
    closes: Mutex<Vec<(Resource, std::thread::ThreadId)>>,
}
struct Fake {
    e: Arc<Evidence>,
    gate: Option<Gate>,
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
        if self.e.close_mode.load(Ordering::SeqCst) == 1 {
            return CloseOutcome::Pending;
        }
        self.e
            .closes
            .lock()
            .unwrap()
            .push((r, std::thread::current().id()));
        CloseOutcome::Consumed(
            if self.e.close_mode.load(Ordering::SeqCst) == 2 && r == Resource::KeyboardNode {
                Err(Failure::code(ErrorCode::BackendUnavailable))
            } else {
                Ok(())
            },
        )
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
        _: CallBudget,
        _: &Cancel,
        _: &Cancel,
    ) -> ReportResult {
        if matches!(
            t,
            Transition::Key { down: true, .. } | Transition::Contact { down: true, .. }
        ) {
            self.e.effects.fetch_add(1, Ordering::SeqCst);
        }
        if let Some((entered, release)) = self.gate.take() {
            entered.send(()).unwrap();
            release.recv_timeout(Duration::from_millis(500)).unwrap();
        }
        ReportResult {
            delivery: Delivery::Sent,
            error: None,
        }
    }
}
async fn setup(
    gate: Option<Gate>,
) -> (Arc<LeaseDevices>, Arc<FakeClock>, LeaseGrant, Arc<Evidence>) {
    let clock = Arc::new(FakeClock::default());
    let r = Arc::new(LeaseDevices::new(clock.clone()));
    let e = Arc::new(Evidence::default());
    let ec = e.clone();
    let s = Session::start_with_registration(
        Duration::from_secs(1),
        Registration {
            settling: Duration::ZERO,
        },
        move || Ok(Fake { e: ec, gate }),
    )
    .unwrap();
    s.ready(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    r.register(
        BootId("synthetic".into()),
        DeviceId("a".into()),
        AttachmentGeneration(1),
        Fence(1),
        s,
    )
    .unwrap();
    let g = r.acquire(&DeviceId("a".into()), 30_000).unwrap();
    (r, clock, g, e)
}
fn key() -> Transition {
    Transition::Key {
        usage: 4,
        down: true,
    }
}
#[derive(Clone, Copy)]
enum Reason {
    Release,
    OwnerCancel,
    Revoke,
    Expire,
    Disconnect,
    Shutdown,
}
impl Reason {
    fn expected(self) -> TerminationReason {
        match self {
            Self::Release => TerminationReason::Released,
            Self::OwnerCancel => TerminationReason::OwnerCancelled,
            Self::Revoke => TerminationReason::Revoked,
            Self::Expire => TerminationReason::IdleExpired,
            Self::Disconnect => TerminationReason::Disconnected,
            Self::Shutdown => TerminationReason::Shutdown,
        }
    }
}
async fn end(r: &LeaseDevices, c: &FakeClock, g: &LeaseGrant, why: Reason) -> Status {
    let budget = Duration::from_secs(1);
    match why {
        Reason::Release => r.release(&g.auth, budget).await.unwrap(),
        Reason::OwnerCancel => r.cancel_ownership(&g.auth, budget).await.unwrap(),
        Reason::Revoke => r.revoke(&g.auth.device_id, budget).await.unwrap(),
        Reason::Expire => {
            c.0.store(30_000, Ordering::SeqCst);
            assert!(r.status(&g.auth.device_id).unwrap().fenced);
            r.shutdown(&g.auth.device_id, budget).await.unwrap()
        }
        Reason::Disconnect => r
            .disconnect(&g.auth.device_id, g.auth.attachment, budget)
            .await
            .unwrap(),
        Reason::Shutdown => r.shutdown(&g.auth.device_id, budget).await.unwrap(),
    }
}
fn closed_once(e: &Evidence) {
    let closes = e.closes.lock().unwrap();
    assert_eq!(closes.len(), 6);
    let id = closes[0].1;
    assert!(closes.iter().all(|(_, t)| *t == id));
    for (i, (r, _)) in closes.iter().enumerate() {
        assert!(!closes[..i].iter().any(|(other, _)| other == r));
    }
}
#[tokio::test]
async fn every_idle_and_queued_reason_fences_before_cleanup_and_closes_once() {
    for why in [
        Reason::Release,
        Reason::OwnerCancel,
        Reason::Revoke,
        Reason::Expire,
        Reason::Disconnect,
        Reason::Shutdown,
    ] {
        for queued in [false, true] {
            let (r, c, g, e) = setup(None).await;
            let tickets = if queued {
                vec![
                    r.enqueue(g.auth.clone(), key(), Duration::from_secs(1))
                        .unwrap(),
                    r.enqueue(g.auth.clone(), key(), Duration::from_secs(1))
                        .unwrap(),
                ]
            } else {
                vec![]
            };
            if queued {
                assert_eq!(r.status(&g.auth.device_id).unwrap().queued, 2);
            }
            assert_eq!(end(&r, &c, &g, why).await.phase, Phase::Closed);
            for t in tickets {
                assert_eq!(t.wait().await.delivery, Delivery::NotSent);
            }
            assert_eq!(
                r.status(&g.auth.device_id).unwrap().termination,
                Some(why.expected())
            );
            assert_eq!(e.effects.load(Ordering::SeqCst), 0);
            closed_once(&e);
            r.release(&g.auth, Duration::from_secs(1)).await.unwrap();
            closed_once(&e);
        }
    }
}
#[tokio::test]
async fn held_key_and_contact_terminate_without_changing_sent_delivery() {
    for why in [
        Reason::Release,
        Reason::OwnerCancel,
        Reason::Revoke,
        Reason::Expire,
        Reason::Disconnect,
        Reason::Shutdown,
    ] {
        for input in [
            key(),
            Transition::Contact {
                down: true,
                x: 1,
                y: 1,
            },
        ] {
            let (r, c, g, e) = setup(None).await;
            let result = r
                .enqueue(g.auth.clone(), input, Duration::from_secs(1))
                .unwrap()
                .wait()
                .await;
            assert_eq!(result.delivery, Delivery::Sent);
            let cleanup = end(&r, &c, &g, why).await;
            assert_eq!(cleanup.phase, Phase::Closed);
            assert!(cleanup.cleanup_error.is_none());
            assert_eq!(result.delivery, Delivery::Sent);
            assert_eq!(e.effects.load(Ordering::SeqCst), 1);
            closed_once(&e);
        }
    }
}

#[tokio::test]
async fn active_saturated_termination_rejects_queue_and_overlaps_safely() {
    use std::{future::Future, task::Poll};
    for why in [
        Reason::Release,
        Reason::OwnerCancel,
        Reason::Revoke,
        Reason::Disconnect,
        Reason::Shutdown,
    ] {
        let (entered, ready) = tokio::sync::oneshot::channel();
        let (release, wait) = mpsc::channel();
        let (r, c, g, e) = setup(Some((entered, wait))).await;
        let active = r
            .enqueue(
                g.auth.clone(),
                Transition::Contact {
                    down: true,
                    x: 1,
                    y: 1,
                },
                Duration::from_secs(1),
            )
            .unwrap();
        tokio::time::timeout(Duration::from_secs(1), ready)
            .await
            .unwrap()
            .unwrap();
        let mut queued = Vec::new();
        for _ in 0..cordgeist::actor::QUEUE_CAPACITY {
            queued.push(
                r.enqueue(g.auth.clone(), key(), Duration::from_secs(1))
                    .unwrap(),
            );
        }
        assert!(matches!(
            r.enqueue(g.auth.clone(), key(), Duration::from_secs(1)),
            Err(ErrorCode::QueueFull)
        ));
        let mut ending = Box::pin(end(&r, &c, &g, why));
        std::future::poll_fn(|cx| {
            assert!(ending.as_mut().poll(cx).is_pending());
            Poll::Ready(())
        })
        .await;
        assert!(r.status(&g.auth.device_id).unwrap().fenced);
        assert_eq!(r.status(&g.auth.device_id).unwrap().queued, 0);
        for ticket in queued {
            assert_eq!(ticket.wait().await.delivery, Delivery::NotSent);
        }
        let mut concurrent = Box::pin(r.release(&g.auth, Duration::from_secs(1)));
        std::future::poll_fn(|cx| {
            assert!(concurrent.as_mut().poll(cx).is_pending());
            Poll::Ready(())
        })
        .await;
        release.send(()).unwrap();
        assert_eq!(ending.await.phase, Phase::Closed);
        assert_eq!(concurrent.await.unwrap().phase, Phase::Closed);
        assert_eq!(active.wait().await.delivery, Delivery::Unknown);
        assert_eq!(e.effects.load(Ordering::SeqCst), 1);
        closed_once(&e);
        assert_eq!(
            r.status(&g.auth.device_id).unwrap().termination,
            Some(why.expected())
        );
    }
}

#[tokio::test]
async fn queued_action_cancel_preserves_lease_and_does_not_send() {
    let (r, _, g, e) = setup(None).await;
    let ticket = r
        .enqueue(g.auth.clone(), key(), Duration::from_secs(1))
        .unwrap();
    r.cancel_action(&g.auth, &ticket).unwrap();
    assert_eq!(ticket.wait().await.delivery, Delivery::NotSent);
    assert!(!r.status(&g.auth.device_id).unwrap().fenced);
    r.refresh(&g.auth).unwrap();
    assert_eq!(e.effects.load(Ordering::SeqCst), 0);
    assert_eq!(
        r.enqueue(g.auth.clone(), key(), Duration::from_secs(1))
            .unwrap()
            .wait()
            .await
            .delivery,
        Delivery::Sent
    );
    r.release(&g.auth, Duration::from_secs(1)).await.unwrap();
    closed_once(&e);
}
#[tokio::test]
async fn active_deadline_preserves_lease_and_idle_ttl_does_not_interrupt_work() {
    let (entered, ready) = tokio::sync::oneshot::channel();
    let (release, wait) = mpsc::channel();
    let (r, c, g, e) = setup(Some((entered, wait))).await;
    let active = r
        .enqueue(g.auth.clone(), key(), Duration::from_millis(30))
        .unwrap();
    tokio::time::timeout(Duration::from_secs(1), ready)
        .await
        .unwrap()
        .unwrap();
    let queued = r
        .enqueue(g.auth.clone(), key(), Duration::from_secs(1))
        .unwrap();
    c.0.store(60_000, Ordering::SeqCst);
    assert!(!r.status(&g.auth.device_id).unwrap().fenced);
    let result = active.wait().await;
    assert_eq!(result.delivery, Delivery::Unknown);
    assert_eq!(result.error.unwrap().code, ErrorCode::ProcessTimeout);
    assert!(!r.status(&g.auth.device_id).unwrap().fenced);
    assert_eq!(r.status(&g.auth.device_id).unwrap().termination, None);
    // Cancel the distinct queued command; never replay the uncertain original.
    r.cancel_action(&g.auth, &queued).unwrap();
    release.send(()).unwrap();
    assert_eq!(queued.wait().await.delivery, Delivery::NotSent);
    r.shutdown(&g.auth.device_id, Duration::from_secs(1))
        .await
        .unwrap();
    assert_eq!(e.effects.load(Ordering::SeqCst), 1);
    assert!(
        r.status(&g.auth.device_id)
            .unwrap()
            .session
            .cleanup_error
            .is_none()
    );
}
#[tokio::test]
async fn stale_owner_cannot_terminate_or_cancel_successor() {
    let (r, _, old, old_e) = setup(None).await;
    r.release(&old.auth, Duration::from_secs(1)).await.unwrap();
    closed_once(&old_e);
    let e = Arc::new(Evidence::default());
    let ec = e.clone();
    let session = Session::start_with_registration(
        Duration::from_secs(1),
        Registration {
            settling: Duration::ZERO,
        },
        move || Ok(Fake { e: ec, gate: None }),
    )
    .unwrap();
    session
        .ready(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    r.replace_cleaned(
        old.auth.boot_id.clone(),
        old.auth.device_id.clone(),
        AttachmentGeneration(2),
        Fence(2),
        session,
    )
    .unwrap();
    let new = r.acquire(&old.auth.device_id, 30_000).unwrap();
    let ticket = r
        .enqueue(new.auth.clone(), key(), Duration::from_secs(1))
        .unwrap();
    assert_eq!(
        r.cancel_action(&old.auth, &ticket),
        Err(ErrorCode::LeaseInvalid)
    );
    assert!(matches!(
        r.release(&old.auth, Duration::from_secs(1)).await,
        Err(ErrorCode::LeaseInvalid)
    ));
    assert!(matches!(
        r.cancel_ownership(&old.auth, Duration::from_secs(1)).await,
        Err(ErrorCode::LeaseInvalid)
    ));
    assert!(matches!(
        r.disconnect(
            &old.auth.device_id,
            old.auth.attachment,
            Duration::from_secs(1)
        )
        .await,
        Err(ErrorCode::LeaseInvalid)
    ));
    assert_eq!(ticket.wait().await.delivery, Delivery::Sent);
    assert!(!r.status(&new.auth.device_id).unwrap().fenced);
    r.release(&new.auth, Duration::from_secs(1)).await.unwrap();
    assert_eq!(e.effects.load(Ordering::SeqCst), 1);
    closed_once(&e);
}

#[tokio::test]
async fn overlapping_pending_and_failed_cleanup_preserve_receipts_and_prevent_handover() {
    let (r, _, g, e) = setup(None).await;
    let sent = r
        .enqueue(
            g.auth.clone(),
            Transition::Contact {
                down: true,
                x: 1,
                y: 1,
            },
            Duration::from_secs(1),
        )
        .unwrap()
        .wait()
        .await;
    assert_eq!(sent.delivery, Delivery::Sent);
    e.close_mode.store(1, Ordering::SeqCst);
    let (a, b, c) = tokio::join!(
        r.release(&g.auth, Duration::from_millis(10)),
        r.cancel_ownership(&g.auth, Duration::from_millis(10)),
        r.revoke(&g.auth.device_id, Duration::from_millis(10))
    );
    for result in [a, b, c] {
        assert!(matches!(result, Err(ErrorCode::NativeCleanupPending)));
    }
    assert!(r.status(&g.auth.device_id).unwrap().fenced);
    assert!(e.closes.lock().unwrap().is_empty());
    assert!(matches!(
        r.acquire(&g.auth.device_id, 30_000),
        Err(ErrorCode::BackendUnavailable)
    ));
    e.close_mode.store(2, Ordering::SeqCst);
    let cleanup = r.release(&g.auth, Duration::from_secs(1)).await.unwrap();
    assert_eq!(cleanup.phase, Phase::Closed);
    assert!(cleanup.cleanup_error.is_some());
    assert_eq!(sent.delivery, Delivery::Sent);
    assert_eq!(e.effects.load(Ordering::SeqCst), 1);
    closed_once(&e);
    r.release(&g.auth, Duration::from_secs(1)).await.unwrap();
    closed_once(&e);
    assert!(matches!(
        r.acquire(&g.auth.device_id, 30_000),
        Err(ErrorCode::BackendUnavailable)
    ));
}
#[tokio::test]
async fn device_b_and_status_progress_during_saturated_a_termination() {
    use std::{future::Future, task::Poll};
    let (entered, ready) = tokio::sync::oneshot::channel();
    let (release, wait) = mpsc::channel();
    let (r, _, g, e) = setup(Some((entered, wait))).await;
    let be = Arc::new(Evidence::default());
    let bec = be.clone();
    let s = Session::start_with_registration(
        Duration::from_secs(1),
        Registration {
            settling: Duration::ZERO,
        },
        move || Ok(Fake { e: bec, gate: None }),
    )
    .unwrap();
    s.ready(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    let bid = DeviceId("b".into());
    r.register(
        g.auth.boot_id.clone(),
        bid.clone(),
        AttachmentGeneration(1),
        Fence(1),
        s,
    )
    .unwrap();
    let bg = r.acquire(&bid, 30_000).unwrap();
    let active = r
        .enqueue(g.auth.clone(), key(), Duration::from_secs(1))
        .unwrap();
    tokio::time::timeout(Duration::from_secs(1), ready)
        .await
        .unwrap()
        .unwrap();
    let mut tickets = Vec::new();
    for _ in 0..cordgeist::actor::QUEUE_CAPACITY {
        tickets.push(
            r.enqueue(g.auth.clone(), key(), Duration::from_secs(1))
                .unwrap(),
        );
    }
    let mut ending = Box::pin(r.release(&g.auth, Duration::from_secs(1)));
    std::future::poll_fn(|cx| {
        assert!(ending.as_mut().poll(cx).is_pending());
        Poll::Ready(())
    })
    .await;
    assert!(r.status(&g.auth.device_id).unwrap().fenced);
    assert!(!r.status(&bid).unwrap().fenced);
    assert_eq!(
        r.enqueue(bg.auth.clone(), key(), Duration::from_secs(1))
            .unwrap()
            .wait()
            .await
            .delivery,
        Delivery::Sent
    );
    for ticket in tickets {
        assert_eq!(ticket.wait().await.delivery, Delivery::NotSent);
    }
    release.send(()).unwrap();
    assert_eq!(ending.await.unwrap().phase, Phase::Closed);
    assert_eq!(active.wait().await.delivery, Delivery::Unknown);
    r.release(&bg.auth, Duration::from_secs(1)).await.unwrap();
    assert_eq!(e.effects.load(Ordering::SeqCst), 1);
    assert_eq!(be.effects.load(Ordering::SeqCst), 1);
    closed_once(&e);
    closed_once(&be);
}
