//! Serialized discovery ownership. No device opening, HID registration or input.
use crate::{api::ErrorCode, backend::Cancel};
use std::{
    sync::{
        Arc,
        atomic::{AtomicBool, AtomicU8, Ordering},
        mpsc,
    },
    time::{Duration, Instant},
};
use tokio::sync::{Notify, oneshot};

pub const MAX_CANDIDATES: usize = 64;
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum DiscoveryKind {
    Native,
    SoftwareFake,
}
/// One native phone, or a closed failure. Never the first of several, and never
/// a software-fake route installed as if it were USB.
pub fn exact_native(count: usize, kind: DiscoveryKind) -> Result<(), ErrorCode> {
    if kind == DiscoveryKind::SoftwareFake {
        return Err(ErrorCode::BackendUnavailable);
    }
    match count {
        0 => Err(ErrorCode::DeviceDisconnected),
        1 => Ok(()),
        _ => Err(ErrorCode::DeviceIdentityAmbiguous),
    }
}
/// Owned identity evidence. Deliberately not Debug: serial/product strings are private.
#[derive(Clone, PartialEq, Eq)]
pub struct UsbDevice {
    pub bus: u8,
    pub address: u8,
    pub ports: Vec<u8>,
    pub vendor_id: u16,
    pub product_id: u16,
    pub aoa_protocol: u16,
    pub serial: Option<String>,
    pub product: Option<String>,
}
impl UsbDevice {
    pub fn topology(&self) -> Result<String, ErrorCode> {
        if self.bus == 0
            || self.address == 0
            || self.address > 127
            || self.ports.is_empty()
            || self.ports.len() > 7
            || self.ports.contains(&0)
            || self.serial.as_ref().is_some_and(|s| s.len() > 256)
            || self.product.as_ref().is_some_and(|s| s.len() > 256)
        {
            return Err(ErrorCode::DeviceIdentityAmbiguous);
        }
        Ok(format!(
            "{}-{}",
            self.bus,
            self.ports
                .iter()
                .map(u8::to_string)
                .collect::<Vec<_>>()
                .join(".")
        ))
    }
}
pub struct DiscoverySnapshot {
    pub kind: DiscoveryKind,
    pub captured_at: Instant,
    pub devices: Vec<UsbDevice>,
}
#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum CloseStatus {
    Pending,
    Consumed(Result<(), ErrorCode>),
}
/// Created, called and destroyed on the owner thread; intentionally no Send bound.
pub trait DiscoveryDriver {
    fn kind(&self) -> DiscoveryKind;
    fn scan(&mut self, deadline: Instant, cancel: &Cancel) -> Result<Vec<UsbDevice>, ErrorCode>;
    /// Pending retains ownership. Consumed must never be retried.
    fn close(&mut self) -> CloseStatus;
}
struct Request {
    deadline: Instant,
    cancel: Cancel,
    reply: oneshot::Sender<Result<DiscoverySnapshot, ErrorCode>>,
}
#[cfg(test)]
type ScanPause = (oneshot::Sender<()>, mpsc::Receiver<()>);
struct State {
    busy: AtomicBool,
    lifecycle: AtomicU8,
    quarantined: AtomicBool,
    finished: AtomicBool,
    close_error: std::sync::Mutex<Option<ErrorCode>>,
    closed: Notify,
    #[cfg(test)]
    before_scan: std::sync::Mutex<Option<ScanPause>>,
    #[cfg(test)]
    after_reply: std::sync::Mutex<Option<ScanPause>>,
}
// Shutdown and scan admission share one atomic linearization point. No lock is
// held across foreign calls; a scan admitted first retains its owner until close.
const CLOSING: u8 = 1;
const RUNNING: u8 = 2;
impl State {
    fn closing(&self) -> bool {
        self.lifecycle.load(Ordering::SeqCst) & CLOSING != 0
    }
    fn close(&self) {
        self.lifecycle.fetch_or(CLOSING, Ordering::SeqCst);
    }
    fn begin_scan(&self) -> bool {
        self.lifecycle
            .compare_exchange(0, RUNNING, Ordering::SeqCst, Ordering::SeqCst)
            .is_ok()
    }
    fn end_scan(&self) {
        self.lifecycle.fetch_and(!RUNNING, Ordering::SeqCst);
    }
}
struct Client {
    sender: mpsc::SyncSender<Request>,
    state: Arc<State>,
}
impl Drop for Client {
    fn drop(&mut self) {
        self.state.close();
    }
}
#[derive(Clone)]
pub struct DiscoveryWorker {
    client: Arc<Client>,
}
// A cancelled/dropped caller cannot leave discovery publishing a late snapshot.
struct WaitGuard {
    state: Arc<State>,
    cancel: Cancel,
    armed: bool,
}
impl Drop for WaitGuard {
    fn drop(&mut self) {
        if self.armed {
            self.cancel.cancel();
            self.state.quarantined.store(true, Ordering::SeqCst);
            self.state.close();
        }
    }
}
impl DiscoveryWorker {
    pub fn start<D, F>(factory: F) -> Result<Self, ErrorCode>
    where
        D: DiscoveryDriver + 'static,
        F: FnOnce() -> Result<D, ErrorCode> + Send + 'static,
    {
        let (sender, receiver) = mpsc::sync_channel::<Request>(1);
        let state = Arc::new(State {
            busy: AtomicBool::new(false),
            lifecycle: AtomicU8::new(0),
            quarantined: AtomicBool::new(false),
            finished: AtomicBool::new(false),
            close_error: std::sync::Mutex::new(None),
            closed: Notify::new(),
            #[cfg(test)]
            before_scan: std::sync::Mutex::new(None),
            #[cfg(test)]
            after_reply: std::sync::Mutex::new(None),
        });
        let owned = state.clone();
        std::thread::Builder::new()
            .name("cordgeist-discovery".into())
            .spawn(move || {
                match factory() {
                    Ok(mut driver) => {
                        while !owned.closing() {
                            let request = match receiver.recv_timeout(Duration::from_millis(10)) {
                                Ok(r) => r,
                                Err(mpsc::RecvTimeoutError::Timeout) => continue,
                                Err(mpsc::RecvTimeoutError::Disconnected) => break,
                            };
                            #[cfg(test)]
                            {
                                let pause = owned.before_scan.lock().unwrap().take();
                                if let Some((reached, release)) = pause {
                                    let _ = reached.send(());
                                    release.recv_timeout(Duration::from_secs(2)).unwrap();
                                }
                            }
                            let mut result = if request.cancel.cancelled()
                                || Instant::now() >= request.deadline
                            {
                                Err(ErrorCode::ProcessTimeout)
                            } else {
                                if !owned.begin_scan() {
                                    let _ = request.reply.send(Err(ErrorCode::BackendUnavailable));
                                    owned.busy.store(false, Ordering::SeqCst);
                                    break;
                                }
                                let result = driver.scan(request.deadline, &request.cancel);
                                owned.end_scan();
                                result
                            };
                            if request.cancel.cancelled()
                                || owned.closing()
                                || Instant::now() >= request.deadline
                            {
                                result = Err(ErrorCode::ProcessTimeout);
                                owned.close();
                                owned.quarantined.store(true, Ordering::SeqCst);
                            }
                            let result = result.and_then(|devices| {
                                if devices.len() > MAX_CANDIDATES {
                                    return Err(ErrorCode::OutputLimit);
                                }
                                for d in &devices {
                                    d.topology()?;
                                }
                                Ok(DiscoverySnapshot {
                                    kind: driver.kind(),
                                    captured_at: Instant::now(),
                                    devices,
                                })
                            });
                            // The completed native work no longer owns admission.
                            // Publish capacity before waking a follow-up caller.
                            owned.busy.store(false, Ordering::SeqCst);
                            let _ = request.reply.send(result);
                            #[cfg(test)]
                            {
                                let pause = owned.after_reply.lock().unwrap().take();
                                if let Some((reached, release)) = pause {
                                    let _ = reached.send(());
                                    release.recv_timeout(Duration::from_secs(2)).unwrap();
                                }
                            }
                        }
                        loop {
                            match driver.close() {
                                CloseStatus::Pending => {
                                    owned.quarantined.store(true, Ordering::SeqCst);
                                    // Retain the driver on this owner thread; never free pending handles.
                                    std::thread::sleep(Duration::from_millis(25));
                                }
                                CloseStatus::Consumed(result) => {
                                    if let Err(e) = result {
                                        *owned.close_error.lock().unwrap() = Some(e);
                                    }
                                    break;
                                }
                            }
                        }
                    }
                    Err(e) => {
                        owned.quarantined.store(true, Ordering::SeqCst);
                        *owned.close_error.lock().unwrap() = Some(e);
                        // Deliver initialization failure to an admitted caller, if any.
                        if let Ok(r) = receiver.recv_timeout(Duration::from_millis(10)) {
                            let _ = r.reply.send(Err(e));
                        }
                    }
                }
                owned.finished.store(true, Ordering::SeqCst);
                owned.closed.notify_waiters();
            })
            .map_err(|_| ErrorCode::BackendUnavailable)?;
        Ok(Self {
            client: Arc::new(Client { sender, state }),
        })
    }
    pub fn quarantined(&self) -> bool {
        self.client.state.quarantined.load(Ordering::SeqCst)
    }
    pub async fn snapshot(
        &self,
        budget: Duration,
        cancel: &Cancel,
    ) -> Result<DiscoverySnapshot, ErrorCode> {
        if budget.is_zero() || budget > Duration::from_secs(10) {
            return Err(ErrorCode::InvalidRequest);
        }
        if cancel.cancelled() {
            return Err(ErrorCode::ProcessCancelled);
        }
        let state = &self.client.state;
        if state.closing() || state.finished.load(Ordering::SeqCst) || self.quarantined() {
            return Err(ErrorCode::BackendUnavailable);
        }
        if state
            .busy
            .compare_exchange(false, true, Ordering::SeqCst, Ordering::SeqCst)
            .is_err()
        {
            return Err(crate::diagnostics::queue_full(
                crate::diagnostics::QueueSource::DiscoveryWorker,
            ));
        }
        let deadline = Instant::now() + budget;
        let (reply, receive) = oneshot::channel();
        let mut guard = WaitGuard {
            state: state.clone(),
            cancel: cancel.clone(),
            armed: true,
        };
        if self
            .client
            .sender
            .try_send(Request {
                deadline,
                cancel: cancel.clone(),
                reply,
            })
            .is_err()
        {
            state.busy.store(false, Ordering::SeqCst);
            return Err(ErrorCode::BackendUnavailable);
        }
        // Poll the inexpensive cancellation flag without blocking the executor.
        let work = async {
            tokio::pin!(receive);
            loop {
                tokio::select! {
                    result = &mut receive => break result.unwrap_or(Err(ErrorCode::BackendUnavailable)),
                    _ = tokio::time::sleep(Duration::from_millis(5)) => {
                        if cancel.cancelled() { break Err(ErrorCode::ProcessCancelled); }
                    }
                }
            }
        };
        let mut result = tokio::time::timeout_at(tokio::time::Instant::from_std(deadline), work)
            .await
            .unwrap_or(Err(ErrorCode::ProcessTimeout));
        // A reply may already be ready when an executor resumes after its deadline.
        // Do not rely on timeout future polling order to fence late publication.
        if cancel.cancelled() {
            result = Err(ErrorCode::ProcessCancelled);
        } else if Instant::now() >= deadline {
            result = Err(ErrorCode::ProcessTimeout);
        }
        // A returned timeout is uncertain even if the driver supplied it itself.
        guard.armed = matches!(
            result,
            Err(ErrorCode::ProcessTimeout
                | ErrorCode::ProcessCancelled
                | ErrorCode::BackendUnavailable)
        );
        result
    }
    /// Shutdown has a bounded caller wait; pending cleanup stays on the owner.
    pub async fn shutdown(&self, budget: Duration) -> Result<(), ErrorCode> {
        if budget.is_zero() || budget > Duration::from_secs(10) {
            return Err(ErrorCode::InvalidRequest);
        }
        let state = &self.client.state;
        state.close();
        let wait = async {
            loop {
                let notified = state.closed.notified();
                tokio::pin!(notified);
                notified.as_mut().enable();
                if state.finished.load(Ordering::SeqCst) {
                    break;
                }
                notified.await;
            }
        };
        tokio::time::timeout(budget, wait)
            .await
            .map_err(|_| ErrorCode::NativeCleanupPending)?;
        state.close_error.lock().unwrap().map_or(Ok(()), Err)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::{future::Future, sync::atomic::AtomicUsize, task::Poll};

    #[test]
    fn exact_native_refuses_zero_many_and_software_fake() {
        assert!(exact_native(1, DiscoveryKind::Native).is_ok());
        assert_eq!(
            exact_native(0, DiscoveryKind::Native),
            Err(ErrorCode::DeviceDisconnected)
        );
        assert_eq!(
            exact_native(2, DiscoveryKind::Native),
            Err(ErrorCode::DeviceIdentityAmbiguous)
        );
        assert_eq!(
            exact_native(1, DiscoveryKind::SoftwareFake),
            Err(ErrorCode::BackendUnavailable)
        );
    }
    // SOFTWARE FAKE: no native context, USB enumeration or phone input.
    struct Fake(Arc<AtomicUsize>);
    impl DiscoveryDriver for Fake {
        fn kind(&self) -> DiscoveryKind {
            DiscoveryKind::SoftwareFake
        }
        fn scan(&mut self, _: Instant, _: &Cancel) -> Result<Vec<UsbDevice>, ErrorCode> {
            self.0.fetch_add(1, Ordering::SeqCst);
            Ok(vec![])
        }
        fn close(&mut self) -> CloseStatus {
            CloseStatus::Consumed(Ok(()))
        }
    }
    #[tokio::test]
    async fn shutdown_rejects_dequeued_snapshot_before_scan() {
        let scans = Arc::new(AtomicUsize::new(0));
        let count = scans.clone();
        let worker = DiscoveryWorker::start(move || Ok(Fake(count))).unwrap();
        let (reached, receive) = oneshot::channel();
        let (release, wait) = mpsc::channel();
        // Scheduler pause only: all admission and shutdown use public APIs.
        *worker.client.state.before_scan.lock().unwrap() = Some((reached, wait));
        let client = worker.clone();
        let snapshot = tokio::spawn(async move {
            client
                .snapshot(Duration::from_secs(2), &Cancel::default())
                .await
        });
        receive.await.unwrap();
        let mut shutdown = Box::pin(worker.shutdown(Duration::from_secs(1)));
        let first = std::future::poll_fn(|cx| Poll::Ready(shutdown.as_mut().poll(cx))).await;
        assert!(first.is_pending());
        release.send(()).unwrap();
        shutdown.await.unwrap();
        assert!(snapshot.await.unwrap().is_err());
        assert_eq!(scans.load(Ordering::SeqCst), 0);
    }
    #[tokio::test]
    async fn completed_snapshot_releases_admission_before_publishing_reply() {
        let worker = DiscoveryWorker::start(|| Ok(Fake(Arc::new(AtomicUsize::new(0))))).unwrap();
        let (reached, receive) = oneshot::channel();
        let (release, wait) = mpsc::channel();
        // Scheduler pause only, after successful public reply publication.
        *worker.client.state.after_reply.lock().unwrap() = Some((reached, wait));
        worker
            .snapshot(Duration::from_secs(1), &Cancel::default())
            .await
            .unwrap();
        receive.await.unwrap();
        let cancel = Cancel::default();
        let mut next = Box::pin(worker.snapshot(Duration::from_secs(1), &cancel));
        let first = std::future::poll_fn(|cx| Poll::Ready(next.as_mut().poll(cx))).await;
        release.send(()).unwrap();
        assert!(
            first.is_pending(),
            "completed snapshot must not retain admission"
        );
        next.await.unwrap();
        worker.shutdown(Duration::from_secs(1)).await.unwrap();
    }
}
