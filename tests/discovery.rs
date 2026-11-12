//! Explicit SOFTWARE FAKE discovery. No real Context or USB device is created.
use cordgeist::{api::ErrorCode, backend::Cancel, discovery::*};
use std::{
    rc::Rc,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
    },
    thread::ThreadId,
    time::{Duration, Instant},
};
fn device() -> UsbDevice {
    UsbDevice {
        bus: 1,
        address: 7,
        ports: vec![2, 3],
        vendor_id: 1,
        product_id: 2,
        aoa_protocol: 2,
        serial: Some("SYNTHETIC_SERIAL".into()),
        product: None,
    }
}
struct FakeDiscovery {
    calls: Arc<Mutex<Vec<(&'static str, ThreadId)>>>,
    release: Arc<AtomicBool>,
    pending_close: Arc<AtomicBool>,
    _not_send: Rc<()>,
}
impl FakeDiscovery {
    fn record(&self, name: &'static str) {
        self.calls
            .lock()
            .unwrap()
            .push((name, std::thread::current().id()));
    }
}
impl DiscoveryDriver for FakeDiscovery {
    fn kind(&self) -> DiscoveryKind {
        DiscoveryKind::SoftwareFake
    }
    fn scan(&mut self, _: Instant, _: &Cancel) -> Result<Vec<UsbDevice>, ErrorCode> {
        self.record("scan");
        while !self.release.load(Ordering::SeqCst) {
            std::thread::sleep(Duration::from_millis(2));
        }
        Ok(vec![device()])
    }
    fn close(&mut self) -> CloseStatus {
        self.record("close");
        if self.pending_close.load(Ordering::SeqCst) {
            CloseStatus::Pending
        } else {
            CloseStatus::Consumed(Ok(()))
        }
    }
}
impl Drop for FakeDiscovery {
    fn drop(&mut self) {
        self.record("drop");
    }
}
type Calls = Arc<Mutex<Vec<(&'static str, ThreadId)>>>;
fn worker(release: Arc<AtomicBool>, pending: Arc<AtomicBool>) -> (DiscoveryWorker, Calls) {
    let calls = Arc::new(Mutex::new(vec![]));
    let log = calls.clone();
    let worker = DiscoveryWorker::start(move || {
        log.lock()
            .unwrap()
            .push(("create", std::thread::current().id()));
        Ok(FakeDiscovery {
            calls: log,
            release,
            pending_close: pending,
            _not_send: Rc::new(()),
        })
    })
    .unwrap();
    (worker, calls)
}
async fn until_call(calls: &Calls, name: &str) {
    tokio::time::timeout(Duration::from_secs(2), async {
        loop {
            if calls.lock().unwrap().iter().any(|(n, _)| *n == name) {
                break;
            }
            tokio::task::yield_now().await;
        }
    })
    .await
    .unwrap();
}
#[tokio::test]
async fn owned_snapshot_survives_driver_shutdown_on_its_single_owner_thread() {
    let caller = std::thread::current().id();
    let (worker, calls) = worker(
        Arc::new(AtomicBool::new(true)),
        Arc::new(AtomicBool::new(false)),
    );
    let snapshot = worker
        .snapshot(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
    worker.shutdown(Duration::from_secs(1)).await.unwrap();
    assert_eq!(snapshot.kind, DiscoveryKind::SoftwareFake);
    assert_eq!(
        snapshot.devices[0].serial.as_deref(),
        Some("SYNTHETIC_SERIAL")
    );
    assert_eq!(snapshot.devices[0].topology().unwrap(), "1-2.3");
    let calls = calls.lock().unwrap();
    assert_eq!(
        calls.iter().map(|(name, _)| *name).collect::<Vec<_>>(),
        ["create", "scan", "close", "drop"]
    );
    let owner = calls[0].1;
    assert_ne!(owner, caller);
    assert!(calls.iter().all(|(_, id)| *id == owner));
}
#[tokio::test]
async fn outstanding_scan_rejects_concurrency_and_expired_result_cannot_escape() {
    let release = Arc::new(AtomicBool::new(false));
    let pending = Arc::new(AtomicBool::new(true));
    let (worker, calls) = worker(release.clone(), pending.clone());
    let clone = worker.clone();
    let task = tokio::spawn(async move {
        clone
            .snapshot(Duration::from_millis(100), &Cancel::default())
            .await
    });
    until_call(&calls, "scan").await;
    assert!(matches!(
        worker
            .snapshot(Duration::from_secs(1), &Cancel::default())
            .await,
        Err(ErrorCode::QueueFull)
    ));
    assert!(matches!(
        task.await.unwrap(),
        Err(ErrorCode::ProcessTimeout)
    ));
    assert!(worker.quarantined());
    assert!(matches!(
        worker
            .snapshot(Duration::from_secs(1), &Cancel::default())
            .await,
        Err(ErrorCode::BackendUnavailable)
    ));
    release.store(true, Ordering::SeqCst);
    until_call(&calls, "close").await;
    assert_eq!(
        worker.shutdown(Duration::from_millis(20)).await,
        Err(ErrorCode::NativeCleanupPending)
    );
    assert!(
        !calls
            .lock()
            .unwrap()
            .iter()
            .any(|(name, _)| *name == "drop")
    );
    pending.store(false, Ordering::SeqCst);
    worker.shutdown(Duration::from_secs(1)).await.unwrap();
    let log = calls.lock().unwrap();
    assert_eq!(log.iter().filter(|(name, _)| *name == "scan").count(), 1);
    assert_eq!(log.iter().filter(|(name, _)| *name == "drop").count(), 1);
}
#[tokio::test]
async fn dropping_a_waiter_cancels_late_work_and_preserves_owner_cleanup() {
    let release = Arc::new(AtomicBool::new(false));
    let (worker, calls) = worker(release.clone(), Arc::new(AtomicBool::new(false)));
    let clone = worker.clone();
    let task = tokio::spawn(async move {
        clone
            .snapshot(Duration::from_secs(1), &Cancel::default())
            .await
    });
    until_call(&calls, "scan").await;
    task.abort();
    let _ = task.await;
    assert!(worker.quarantined());
    release.store(true, Ordering::SeqCst);
    worker.shutdown(Duration::from_secs(1)).await.unwrap();
    assert_eq!(
        calls
            .lock()
            .unwrap()
            .iter()
            .filter(|(name, _)| *name == "scan")
            .count(),
        1
    );
}
#[tokio::test]
async fn invalid_or_cancelled_discovery_has_no_scan() {
    let (worker, calls) = worker(
        Arc::new(AtomicBool::new(true)),
        Arc::new(AtomicBool::new(false)),
    );
    assert_eq!(
        worker.shutdown(Duration::ZERO).await,
        Err(ErrorCode::InvalidRequest)
    );
    assert_eq!(
        worker.shutdown(Duration::MAX).await,
        Err(ErrorCode::InvalidRequest)
    );
    let cancel = Cancel::default();
    cancel.cancel();
    assert!(matches!(
        worker.snapshot(Duration::from_secs(1), &cancel).await,
        Err(ErrorCode::ProcessCancelled)
    ));
    assert!(matches!(
        worker.snapshot(Duration::ZERO, &Cancel::default()).await,
        Err(ErrorCode::InvalidRequest)
    ));
    worker.shutdown(Duration::from_secs(1)).await.unwrap();
    assert!(
        !calls
            .lock()
            .unwrap()
            .iter()
            .any(|(name, _)| *name == "scan")
    );
}
#[tokio::test]
async fn initialization_failure_never_returns_an_empty_successful_inventory() {
    struct Never;
    impl DiscoveryDriver for Never {
        fn kind(&self) -> DiscoveryKind {
            unreachable!()
        }
        fn scan(&mut self, _: Instant, _: &Cancel) -> Result<Vec<UsbDevice>, ErrorCode> {
            unreachable!()
        }
        fn close(&mut self) -> CloseStatus {
            unreachable!()
        }
    }
    let worker = DiscoveryWorker::start::<Never, _>(|| Err(ErrorCode::BackendUnavailable)).unwrap();
    assert!(matches!(
        worker
            .snapshot(Duration::from_secs(1), &Cancel::default())
            .await,
        Err(ErrorCode::BackendUnavailable)
    ));
    assert!(worker.shutdown(Duration::from_secs(1)).await.is_err());
}

#[tokio::test]
async fn ready_reply_is_rejected_if_executor_resumes_past_caller_deadline() {
    let release = Arc::new(AtomicBool::new(false));
    let (worker, calls) = worker(release.clone(), Arc::new(AtomicBool::new(false)));
    let clone = worker.clone();
    let task = tokio::spawn(async move {
        clone
            .snapshot(Duration::from_millis(100), &Cancel::default())
            .await
    });
    until_call(&calls, "scan").await;
    release.store(true, Ordering::SeqCst);
    // Deliberate executor stall: the owner can finish and queue the reply, but the
    // caller cannot consume it until after its deadline. No phone/native driver.
    std::thread::sleep(Duration::from_millis(130));
    assert!(matches!(
        task.await.unwrap(),
        Err(ErrorCode::ProcessTimeout)
    ));
    assert!(worker.quarantined());
    worker.shutdown(Duration::from_secs(1)).await.unwrap();
}

#[tokio::test]
async fn consumed_cleanup_error_is_reported_without_retrying_close() {
    use std::sync::atomic::AtomicUsize;
    struct ConsumedFailure(Arc<AtomicUsize>);
    impl DiscoveryDriver for ConsumedFailure {
        fn kind(&self) -> DiscoveryKind {
            DiscoveryKind::SoftwareFake
        }
        fn scan(&mut self, _: Instant, _: &Cancel) -> Result<Vec<UsbDevice>, ErrorCode> {
            Ok(vec![])
        }
        fn close(&mut self) -> CloseStatus {
            self.0.fetch_add(1, Ordering::SeqCst);
            CloseStatus::Consumed(Err(ErrorCode::BackendUnavailable))
        }
    }
    let closes = Arc::new(AtomicUsize::new(0));
    let count = closes.clone();
    let worker = DiscoveryWorker::start(move || Ok(ConsumedFailure(count))).unwrap();
    assert_eq!(
        worker
            .snapshot(Duration::from_secs(1), &Cancel::default())
            .await
            .unwrap()
            .kind,
        DiscoveryKind::SoftwareFake
    );
    assert_eq!(
        worker.shutdown(Duration::from_secs(1)).await,
        Err(ErrorCode::BackendUnavailable)
    );
    assert_eq!(
        worker.shutdown(Duration::from_secs(1)).await,
        Err(ErrorCode::BackendUnavailable)
    );
    assert_eq!(closes.load(Ordering::SeqCst), 1);
}
