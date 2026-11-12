//! SOFTWARE FAKE lifecycle evidence. No libusb, device discovery or HID input.
use cordgeist::{api::ErrorCode, backend::Cancel, session::*};
use std::{
    rc::Rc,
    sync::{
        Arc, Mutex,
        atomic::{AtomicBool, Ordering},
        mpsc,
    },
    thread::ThreadId,
    time::Duration,
};
const RESOURCES: [Resource; 6] = [
    Resource::Context,
    Resource::Device,
    Resource::TouchSpec,
    Resource::KeyboardSpec,
    Resource::TouchNode,
    Resource::KeyboardNode,
];
const TEARDOWN: [Resource; 6] = [
    Resource::KeyboardNode,
    Resource::TouchNode,
    Resource::KeyboardSpec,
    Resource::TouchSpec,
    Resource::Device,
    Resource::Context,
];
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
enum Event {
    Open(Resource),
    Close(Resource),
    DropDriver,
}
#[derive(Default)]
struct Recording {
    events: Vec<Event>,
    owner: Option<ThreadId>,
}
impl Recording {
    fn record(&mut self, event: Event) {
        let thread = std::thread::current().id();
        assert_eq!(*self.owner.get_or_insert(thread), thread);
        self.events.push(event);
    }
}
struct Handle {
    resource: Resource,
    consumed: bool,
    _not_send: Rc<()>,
}
struct Fake {
    recording: Arc<Mutex<Recording>>,
    pending: Arc<AtomicBool>,
    fail_open: Option<Resource>,
    fail_close: Option<Resource>,
    retained: bool,
    block: Option<(mpsc::Sender<()>, mpsc::Receiver<()>)>,
}
impl Drop for Fake {
    fn drop(&mut self) {
        self.recording.lock().unwrap().record(Event::DropDriver);
    }
}
fn failure() -> Failure {
    Failure {
        code: ErrorCode::BackendUnavailable,
        stage: "test",
        native_code: -7,
        transport_status: -9,
        request: 55,
        node_id: 12,
        report_id: 4,
        offset: 8,
        length: 16,
    }
}
impl SessionDriver for Fake {
    type Handle = Handle;
    fn open(
        &mut self,
        resource: Resource,
        handles: &Handles<Handle>,
        budget: CallBudget,
        _: &Cancel,
    ) -> Result<Handle, Failure> {
        assert!((1..=500).contains(&budget.control_ms));
        assert!((1..=1000).contains(&budget.drain_ms));
        self.recording.lock().unwrap().record(Event::Open(resource));
        if resource == Resource::Device {
            assert_eq!(
                handles.get(Resource::Context).unwrap().resource,
                Resource::Context
            );
            if let Some((entered, release)) = self.block.take() {
                entered.send(()).unwrap();
                release.recv_timeout(Duration::from_secs(2)).unwrap();
            }
        }
        if matches!(resource, Resource::TouchNode | Resource::KeyboardNode) {
            assert!(handles.get(Resource::Device).is_some());
            assert!(
                handles
                    .get(if resource == Resource::TouchNode {
                        Resource::TouchSpec
                    } else {
                        Resource::KeyboardSpec
                    })
                    .is_some()
            );
        }
        if self.fail_open == Some(resource) {
            return Err(failure());
        }
        Ok(Handle {
            resource,
            consumed: false,
            _not_send: Rc::new(()),
        })
    }
    fn close(
        &mut self,
        resource: Resource,
        handle: &mut Handle,
        budget: CallBudget,
    ) -> CloseOutcome {
        assert!(!handle.consumed, "consumed handle reused");
        assert_eq!(resource, handle.resource);
        assert!((1..=500).contains(&budget.control_ms));
        assert!((1..=1000).contains(&budget.drain_ms));
        self.recording
            .lock()
            .unwrap()
            .record(Event::Close(resource));
        if resource == Resource::KeyboardNode && self.pending.load(Ordering::SeqCst) {
            return CloseOutcome::Pending;
        }
        if self.retained {
            return CloseOutcome::Retained(failure());
        }
        handle.consumed = true;
        CloseOutcome::Consumed(if self.fail_close == Some(resource) {
            Err(failure())
        } else {
            Ok(())
        })
    }
}
fn fake(recording: Arc<Mutex<Recording>>, pending: Arc<AtomicBool>) -> Fake {
    Fake {
        recording,
        pending,
        fail_open: None,
        fail_close: None,
        retained: false,
        block: None,
    }
}
async fn ready(session: &Session) {
    session
        .ready(Duration::from_secs(1), &Cancel::default())
        .await
        .unwrap();
}
#[tokio::test]
async fn public_lifecycle_serializes_non_send_handles_and_never_reuses_consumed_handles() {
    let recording = Arc::new(Mutex::new(Recording::default()));
    let r = recording.clone();
    let session =
        Session::start(Duration::from_secs(1), move || Ok(fake(r, Arc::default()))).unwrap();
    ready(&session).await;
    let closed = session.shutdown(Duration::from_secs(1)).await.unwrap();
    assert_eq!(closed.phase, Phase::Closed);
    assert_eq!(closed.cleanup_error, None);
    assert_eq!(
        session
            .ready(Duration::from_secs(1), &Cancel::default())
            .await,
        Err(ErrorCode::BackendUnavailable)
    );
    assert_eq!(
        session.shutdown(Duration::from_secs(1)).await.unwrap(),
        closed
    );
    wait_dropped(&recording).await;
    let record = recording.lock().unwrap();
    assert_ne!(record.owner.unwrap(), std::thread::current().id());
    let expected = RESOURCES
        .map(Event::Open)
        .into_iter()
        .chain(TEARDOWN.map(Event::Close))
        .chain([Event::DropDriver])
        .collect::<Vec<_>>();
    assert_eq!(record.events, expected);
}
async fn wait_dropped(recording: &Arc<Mutex<Recording>>) {
    tokio::time::timeout(Duration::from_secs(1), async {
        loop {
            if recording
                .lock()
                .unwrap()
                .events
                .contains(&Event::DropDriver)
            {
                break;
            }
            tokio::time::sleep(Duration::from_millis(2)).await;
        }
    })
    .await
    .unwrap();
}
#[tokio::test]
async fn pending_cleanup_retains_owner_and_parents_after_bounded_caller_timeout() {
    let recording = Arc::new(Mutex::new(Recording::default()));
    let pending = Arc::new(AtomicBool::new(true));
    let (r, p) = (recording.clone(), pending.clone());
    let session = Session::start(Duration::from_secs(1), move || Ok(fake(r, p))).unwrap();
    ready(&session).await;
    assert_eq!(
        session.shutdown(Duration::from_millis(60)).await,
        Err(ErrorCode::NativeCleanupPending)
    );
    assert_eq!(session.status().phase, Phase::Quarantined);
    assert!(
        recording
            .lock()
            .unwrap()
            .events
            .iter()
            .all(|e| !matches!(e,Event::Close(r) if *r!=Resource::KeyboardNode))
    );
    assert!(
        !recording
            .lock()
            .unwrap()
            .events
            .contains(&Event::DropDriver)
    );
    assert!(
        session
            .ready(Duration::from_secs(1), &Cancel::default())
            .await
            .is_err()
    );
    pending.store(false, Ordering::SeqCst);
    assert_eq!(
        session
            .shutdown(Duration::from_secs(1))
            .await
            .unwrap()
            .phase,
        Phase::Closed
    );
    wait_dropped(&recording).await;
    for r in TEARDOWN.iter().skip(1) {
        assert_eq!(
            recording
                .lock()
                .unwrap()
                .events
                .iter()
                .filter(|e| **e == Event::Close(*r))
                .count(),
            1
        );
    }
}
#[tokio::test]
async fn consumed_cleanup_errors_are_preserved_and_all_parents_still_close_once() {
    for failed in TEARDOWN {
        let recording = Arc::new(Mutex::new(Recording::default()));
        let r = recording.clone();
        let session = Session::start(Duration::from_secs(1), move || {
            let mut f = fake(r, Arc::default());
            f.fail_close = Some(failed);
            Ok(f)
        })
        .unwrap();
        ready(&session).await;
        let report = session.shutdown(Duration::from_secs(1)).await.unwrap();
        assert_eq!(report.cleanup_error, Some(failure()));
        assert_eq!(report.phase, Phase::Closed);
        session.shutdown(Duration::from_secs(1)).await.unwrap();
        wait_dropped(&recording).await;
        let closes = recording
            .lock()
            .unwrap()
            .events
            .iter()
            .filter_map(|e| {
                if let Event::Close(r) = e {
                    Some(*r)
                } else {
                    None
                }
            })
            .collect::<Vec<_>>();
        assert_eq!(closes, TEARDOWN);
    }
}
#[tokio::test]
async fn partial_initialization_cleans_only_acquired_resources_in_dependency_order() {
    for (index, failed) in RESOURCES.iter().copied().enumerate() {
        let recording = Arc::new(Mutex::new(Recording::default()));
        let r = recording.clone();
        let session = Session::start(Duration::from_secs(1), move || {
            let mut f = fake(r, Arc::default());
            f.fail_open = Some(failed);
            Ok(f)
        })
        .unwrap();
        assert_eq!(
            session
                .ready(Duration::from_secs(1), &Cancel::default())
                .await,
            Err(ErrorCode::BackendUnavailable)
        );
        let closed = session.shutdown(Duration::from_secs(1)).await.unwrap();
        assert_eq!(closed.initialization_error, Some(failure()));
        wait_dropped(&recording).await;
        let record = recording.lock().unwrap();
        let expected = RESOURCES[..=index]
            .iter()
            .copied()
            .map(Event::Open)
            .chain(
                TEARDOWN
                    .into_iter()
                    .filter(|r| RESOURCES[..index].contains(r))
                    .map(Event::Close),
            )
            .chain([Event::DropDriver])
            .collect::<Vec<_>>();
        assert_eq!(record.events, expected);
    }
}
#[tokio::test]
async fn cancellation_retains_inflight_open_then_cleans_without_opening_more_resources() {
    let recording = Arc::new(Mutex::new(Recording::default()));
    let r = recording.clone();
    let (entered, enter) = mpsc::channel();
    let (release, wait) = mpsc::channel();
    let session = Session::start(Duration::from_secs(1), move || {
        let mut f = fake(r, Arc::default());
        f.block = Some((entered, wait));
        Ok(f)
    })
    .unwrap();
    enter.recv_timeout(Duration::from_secs(1)).unwrap();
    let cancel = Cancel::default();
    cancel.cancel();
    assert_eq!(
        session.ready(Duration::from_secs(1), &cancel).await,
        Err(ErrorCode::ProcessCancelled)
    );
    assert_eq!(
        session.shutdown(Duration::from_millis(20)).await,
        Err(ErrorCode::NativeCleanupPending)
    );
    assert_eq!(
        recording.lock().unwrap().events,
        [
            Event::Open(Resource::Context),
            Event::Open(Resource::Device)
        ]
    );
    release.send(()).unwrap();
    let closed = session.shutdown(Duration::from_secs(1)).await.unwrap();
    assert_eq!(
        closed.initialization_error.unwrap().code,
        ErrorCode::ProcessCancelled
    );
    wait_dropped(&recording).await;
    assert_eq!(
        recording.lock().unwrap().events,
        vec![
            Event::Open(Resource::Context),
            Event::Open(Resource::Device),
            Event::Close(Resource::Device),
            Event::Close(Resource::Context),
            Event::DropDriver
        ]
    );
}
#[tokio::test]
async fn last_client_drop_cleans_on_owner_without_an_async_waiter() {
    let recording = Arc::new(Mutex::new(Recording::default()));
    let r = recording.clone();
    let session =
        Session::start(Duration::from_secs(1), move || Ok(fake(r, Arc::default()))).unwrap();
    ready(&session).await;
    drop(session);
    wait_dropped(&recording).await;
    assert_eq!(
        recording
            .lock()
            .unwrap()
            .events
            .iter()
            .filter(|e| matches!(e, Event::Close(_)))
            .count(),
        6
    );
}

// A permanently retained native handle deliberately retains its OS owner thread.
// Isolate that process-lifetime quarantine so the test runner never leaks owners.
#[test]
fn retained_failure_keeps_owner_and_never_retries_or_drops_live_handles() {
    const CHILD: &str = "CORDGEIST_TEST_RETAINED_SESSION_CHILD";
    if std::env::var_os(CHILD).is_none() {
        let output = std::process::Command::new(std::env::current_exe().unwrap())
            .args([
                "--exact",
                "retained_failure_keeps_owner_and_never_retries_or_drops_live_handles",
                "--nocapture",
            ])
            .env(CHILD, "1")
            .output()
            .unwrap();
        assert!(
            output.status.success(),
            "{}",
            String::from_utf8_lossy(&output.stdout)
        );
        return;
    }
    tokio::runtime::Builder::new_current_thread()
        .enable_all()
        .build()
        .unwrap()
        .block_on(async {
            let recording = Arc::new(Mutex::new(Recording::default()));
            let r = recording.clone();
            let session = Session::start(Duration::from_secs(1), move || {
                let mut f = fake(r, Arc::default());
                f.retained = true;
                Ok(f)
            })
            .unwrap();
            ready(&session).await;
            assert_eq!(
                session.shutdown(Duration::from_millis(40)).await,
                Err(ErrorCode::NativeCleanupPending)
            );
            assert_eq!(session.status().phase, Phase::Quarantined);
            assert_eq!(session.status().cleanup_error, Some(failure()));
            assert_eq!(
                session.shutdown(Duration::from_millis(40)).await,
                Err(ErrorCode::NativeCleanupPending)
            );
            drop(session);
            let record = recording.lock().unwrap();
            assert_eq!(
                record
                    .events
                    .iter()
                    .filter(|e| matches!(e, Event::Close(_)))
                    .count(),
                1
            );
            assert!(!record.events.contains(&Event::DropDriver));
        });
}
