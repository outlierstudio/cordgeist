//! SOFTWARE FAKE registration settling; never device-readiness evidence.
use cordgeist::{api::ErrorCode, backend::Cancel, session::*, session_input::*};
use std::{
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, AtomicUsize, Ordering},
        mpsc,
    },
    time::{Duration, Instant},
};
struct Fake {
    registered: Arc<Mutex<Option<Instant>>>,
    reports: Arc<AtomicUsize>,
    lost: Arc<AtomicBool>,
    pending: Arc<AtomicBool>,
    block: Option<(mpsc::Sender<()>, mpsc::Receiver<()>)>,
}
impl SessionDriver for Fake {
    type Handle = ();
    fn open(
        &mut self,
        r: Resource,
        _: &Handles<()>,
        _: CallBudget,
        _: &Cancel,
    ) -> Result<(), Failure> {
        if r == Resource::KeyboardNode {
            *self.registered.lock().unwrap() = Some(Instant::now());
        }
        Ok(())
    }
    fn check_settling(&mut self, _: &Cancel) -> Result<(), Failure> {
        if let Some((entered, release)) = self.block.take() {
            entered.send(()).unwrap();
            release.recv_timeout(Duration::from_secs(2)).unwrap();
        }
        if self.lost.load(Ordering::SeqCst) {
            Err(Failure::code(ErrorCode::LeaseInvalid))
        } else {
            Ok(())
        }
    }
    fn close(&mut self, r: Resource, _: &mut (), _: CallBudget) -> CloseOutcome {
        if r == Resource::KeyboardNode && self.pending.load(Ordering::SeqCst) {
            CloseOutcome::Pending
        } else {
            CloseOutcome::Consumed(
                if self.lost.load(Ordering::SeqCst) && r == Resource::KeyboardNode {
                    Err(Failure::code(ErrorCode::BackendUnavailable))
                } else {
                    Ok(())
                },
            )
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
        _: &Transition,
        _: &Handles<()>,
        _: CallBudget,
        _: &Cancel,
        _: &Cancel,
    ) -> ReportResult {
        self.reports.fetch_add(1, Ordering::SeqCst);
        ReportResult {
            delivery: Delivery::Sent,
            error: None,
        }
    }
}
fn key() -> Transition {
    Transition::Key {
        usage: 4,
        down: true,
    }
}
async fn phase(s: &Session, wanted: Phase) {
    tokio::time::timeout(Duration::from_secs(1), async {
        while s.status().phase != wanted {
            tokio::time::sleep(Duration::from_millis(1)).await;
        }
    })
    .await
    .unwrap();
}
fn driver() -> Fake {
    Fake {
        registered: Arc::default(),
        reports: Arc::default(),
        lost: Arc::default(),
        pending: Arc::default(),
        block: None,
    }
}
#[tokio::test]
async fn default_settling_blocks_reports_and_never_claims_hardware_verification() {
    let f = driver();
    let registered = f.registered.clone();
    let reports = f.reports.clone();
    let s = Session::start(Duration::from_secs(1), move || Ok(f)).unwrap();
    phase(&s, Phase::Settling).await;
    assert!(s.status().resources_acquired);
    assert!(!s.status().settling_elapsed);
    assert!(!s.status().hardware_verified);
    assert_eq!(
        s.submit(key(), Duration::from_secs(1), &Cancel::default())
            .await
            .delivery,
        Delivery::NotSent
    );
    s.ready(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    assert!(registered.lock().unwrap().unwrap().elapsed() >= Duration::from_millis(100));
    assert!(s.status().settling_elapsed);
    assert!(!s.status().hardware_verified);
    assert_eq!(reports.load(Ordering::SeqCst), 0);
    assert_eq!(
        s.submit(key(), Duration::from_secs(1), &Cancel::default())
            .await
            .delivery,
        Delivery::Sent
    );
    s.shutdown(Duration::from_secs(1)).await.unwrap();
}
#[tokio::test]
async fn cancellation_shutdown_and_expired_late_validation_cannot_revive() {
    for mode in 0..4 {
        let mut f = driver();
        let reports = f.reports.clone();
        let (entered, receive) = mpsc::channel();
        let (release, wait) = mpsc::channel();
        f.block = Some((entered, wait));
        let s = Session::start(
            if mode == 2 {
                Duration::from_millis(40)
            } else {
                Duration::from_secs(1)
            },
            move || Ok(f),
        )
        .unwrap();
        receive.recv_timeout(Duration::from_secs(1)).unwrap();
        if mode == 0 {
            let cancel = Cancel::default();
            cancel.cancel();
            assert_eq!(
                s.ready(Duration::from_secs(1), &cancel).await,
                Err(ErrorCode::ProcessCancelled)
            );
        }
        if mode == 1 {
            assert_eq!(
                s.shutdown(Duration::from_millis(10)).await,
                Err(ErrorCode::NativeCleanupPending)
            );
        }
        if mode == 2 {
            std::thread::sleep(Duration::from_millis(60));
        }
        if mode == 3 {
            assert_eq!(
                s.ready(Duration::from_millis(10), &Cancel::default()).await,
                Err(ErrorCode::ProcessTimeout)
            );
        }
        release.send(()).unwrap();
        if mode == 2 {
            assert_eq!(
                s.ready(Duration::from_secs(1), &Cancel::default()).await,
                Err(ErrorCode::ProcessTimeout)
            );
        }
        let closed = s.shutdown(Duration::from_secs(1)).await.unwrap();
        assert_eq!(closed.phase, Phase::Closed);
        assert!(!closed.settling_elapsed);
        assert!(
            s.ready(Duration::from_secs(1), &Cancel::default())
                .await
                .is_err()
        );
        assert_eq!(reports.load(Ordering::SeqCst), 0);
    }
}
#[tokio::test]
async fn guard_loss_during_settling_automatically_cleans_and_retains_pending_owner() {
    let f = driver();
    let lost = f.lost.clone();
    let pending = f.pending.clone();
    let reports = f.reports.clone();
    pending.store(true, Ordering::SeqCst);
    let s = Session::start_with_registration(
        Duration::from_secs(1),
        Registration {
            settling: Duration::from_millis(300),
        },
        move || Ok(f),
    )
    .unwrap();
    phase(&s, Phase::Settling).await;
    lost.store(true, Ordering::SeqCst);
    phase(&s, Phase::Quarantined).await;
    lost.store(false, Ordering::SeqCst);
    assert_eq!(
        s.submit(key(), Duration::from_secs(1), &Cancel::default())
            .await
            .delivery,
        Delivery::NotSent
    );
    lost.store(true, Ordering::SeqCst);
    pending.store(false, Ordering::SeqCst);
    let closed = s.shutdown(Duration::from_secs(1)).await.unwrap();
    assert_eq!(closed.terminal_error.unwrap().code, ErrorCode::LeaseInvalid);
    assert!(closed.cleanup_error.is_some());
    assert!(!closed.settling_elapsed);
    assert_eq!(reports.load(Ordering::SeqCst), 0);
}

#[tokio::test]
async fn configuration_bounds_reject_before_factory_and_zero_still_requires_guard() {
    let calls = Arc::new(AtomicUsize::new(0));
    let count = calls.clone();
    assert!(matches!(
        Session::start_with_registration(
            Duration::from_secs(1),
            Registration {
                settling: Duration::from_secs(6)
            },
            move || {
                count.fetch_add(1, Ordering::SeqCst);
                Ok(driver())
            }
        ),
        Err(ErrorCode::InvalidRequest)
    ));
    assert_eq!(calls.load(Ordering::SeqCst), 0);
    let f = driver();
    let lost = f.lost.clone();
    lost.store(true, Ordering::SeqCst);
    let s = Session::start_with_registration(
        Duration::from_secs(1),
        Registration {
            settling: Duration::ZERO,
        },
        move || Ok(f),
    )
    .unwrap();
    assert_eq!(
        s.ready(Duration::from_secs(1), &Cancel::default()).await,
        Err(ErrorCode::LeaseInvalid)
    );
    let closed = s.shutdown(Duration::from_secs(1)).await.unwrap();
    assert!(!closed.settling_elapsed);
    assert!(!closed.hardware_verified);
}
