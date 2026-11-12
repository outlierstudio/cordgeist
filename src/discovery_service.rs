//! Linux discovery/identity orchestration for a future single daemon owner.
//! No leases, native input, socket/CLI dispatch or implicit pairing writes.
use crate::{
    adb::{Adb, Attachment, parse_inventory},
    api::{BootId, DeviceId, ErrorCode},
    backend::Cancel,
    discovery::DiscoveryWorker,
    identity::{IdentityRegistry, PairingConfig, ResolvedRoute, RouteRef},
    pairing_store::{PairingStore, StoreError},
};
use std::{
    future::Future,
    sync::{Arc, Mutex},
    time::{Duration, Instant},
};
use tokio::sync::Semaphore;
use tokio_util::sync::CancellationToken;

pub trait InventorySource {
    fn list(
        &self,
        cancel: CancellationToken,
    ) -> impl Future<Output = Result<Vec<Attachment>, ErrorCode>> + Send;
}
impl InventorySource for Adb {
    async fn list(&self, cancel: CancellationToken) -> Result<Vec<Attachment>, ErrorCode> {
        parse_inventory(&self.inventory(Duration::from_secs(2), cancel).await?.stdout)
    }
}
struct CancelWork {
    native: Cancel,
    adb: CancellationToken,
}
impl Drop for CancelWork {
    fn drop(&mut self) {
        self.native.cancel();
        self.adb.cancel();
    }
}
pub struct DiscoveryService<I> {
    worker: DiscoveryWorker,
    inventory: I,
    store: Arc<PairingStore>,
    store_capacity: Arc<Semaphore>,
    registry: Arc<Mutex<IdentityRegistry>>,
    pairings: PairingConfig,
}
fn store_error(error: StoreError) -> ErrorCode {
    match error {
        StoreError::Busy => {
            crate::diagnostics::queue_full(crate::diagnostics::QueueSource::PairingFileLock)
        }
        StoreError::InvalidData | StoreError::Validation(_) => ErrorCode::DeviceIdentityAmbiguous,
        _ => ErrorCode::BackendUnavailable,
    }
}
impl<I: InventorySource> DiscoveryService<I> {
    pub fn new(
        worker: DiscoveryWorker,
        inventory: I,
        store: PairingStore,
    ) -> Result<Self, ErrorCode> {
        let mut boot = [0; 32];
        getrandom::fill(&mut boot).map_err(|_| ErrorCode::BackendUnavailable)?;
        let boot_id = BootId(boot.iter().map(|b| format!("{b:02x}")).collect());
        Ok(Self {
            worker,
            inventory,
            store: Arc::new(store),
            store_capacity: Arc::new(Semaphore::new(1)),
            registry: Arc::new(Mutex::new(IdentityRegistry::new(boot_id))),
            pairings: PairingConfig::default(),
        })
    }
    /// Shared identity source, not a second route cache. Invalidations immediately
    /// reach installed actors and every existing Session authorization guard.
    pub fn identities(&self) -> Arc<Mutex<IdentityRegistry>> {
        self.registry.clone()
    }
    async fn load(&self) -> Result<PairingConfig, ErrorCode> {
        // At most one blocking filesystem operation may outlive a cancelled wait.
        let permit = self
            .store_capacity
            .clone()
            .try_acquire_owned()
            .map_err(|_| {
                crate::diagnostics::queue_full(crate::diagnostics::QueueSource::StoreCapacity)
            })?;
        let store = self.store.clone();
        tokio::task::spawn_blocking(move || {
            let _permit = permit;
            store.load().map_err(store_error)
        })
        .await
        .map_err(|_| ErrorCode::BackendUnavailable)?
    }
    pub fn resolve(&self, reference: &RouteRef) -> Result<ResolvedRoute, ErrorCode> {
        self.registry.lock().unwrap().resolve(reference)
    }
    pub fn id_for_alias(&self, alias: &str) -> Result<DeviceId, ErrorCode> {
        self.pairings.id_for_alias(alias)
    }
    /// Deliberate reloading fences previous references before any file read.
    pub async fn reload_pairings(&mut self, budget: Duration) -> Result<(), ErrorCode> {
        self.registry.lock().unwrap().invalidate()?;
        self.pairings = PairingConfig::default();
        if budget.is_zero() || budget > Duration::from_secs(10) {
            return Err(ErrorCode::InvalidRequest);
        }
        let deadline = Instant::now() + budget;
        let pairings =
            tokio::time::timeout_at(tokio::time::Instant::from_std(deadline), self.load())
                .await
                .map_err(|_| ErrorCode::ProcessTimeout)??;
        // A ready filesystem result can be consumed only after executor delay.
        // Fence publication independently of timeout's inner-first polling order.
        if Instant::now() >= deadline {
            return Err(ErrorCode::ProcessTimeout);
        }
        self.pairings = pairings;
        Ok(())
    }
    pub async fn refresh(
        &mut self,
        budget: Duration,
        cancel: &Cancel,
    ) -> Result<Vec<RouteRef>, ErrorCode> {
        // Even a dropped future, failed read, malformed inventory or timeout must
        // leave no previously retained route usable.
        self.registry.lock().unwrap().invalidate()?;
        self.pairings = PairingConfig::default();
        if budget.is_zero() || budget > Duration::from_secs(10) {
            return Err(ErrorCode::InvalidRequest);
        }
        if cancel.cancelled() {
            return Err(ErrorCode::ProcessCancelled);
        }
        let sample = self.sample(budget, cancel).await;
        let (snapshot, after, config) = match sample {
            Ok(sample) => sample,
            Err(code) => return Err(code),
        };
        self.pairings = config.clone();
        let routes = self
            .registry
            .lock()
            .unwrap()
            .refresh(snapshot, &after, &config)?;
        Ok(routes)
    }
    /// Re-sample the exact phone without minting a new attachment generation.
    pub async fn confirm(&mut self, budget: Duration, cancel: &Cancel) -> Result<(), ErrorCode> {
        if budget.is_zero() || budget > Duration::from_secs(10) {
            return Err(ErrorCode::InvalidRequest);
        }
        if cancel.cancelled() {
            return Err(ErrorCode::ProcessCancelled);
        }
        match self.sample(budget, cancel).await {
            Ok((snapshot, after, config)) => self
                .registry
                .lock()
                .unwrap()
                .renew_if_same(snapshot, &after, &config),
            Err(code) => {
                let _ = self.registry.lock().unwrap().invalidate();
                Err(code)
            }
        }
    }
    async fn sample(
        &mut self,
        budget: Duration,
        cancel: &Cancel,
    ) -> Result<
        (
            crate::discovery::DiscoverySnapshot,
            Vec<Attachment>,
            crate::identity::PairingConfig,
        ),
        ErrorCode,
    > {
        let deadline = Instant::now() + budget;
        let work_cancel = CancelWork {
            native: Cancel::default(),
            adb: CancellationToken::new(),
        };
        let work = async {
            let config = self.load().await?;
            let mut before = self.inventory.list(work_cancel.adb.clone()).await?;
            let remaining = deadline
                .checked_duration_since(Instant::now())
                .ok_or(ErrorCode::ProcessTimeout)?;
            let snapshot = self.worker.snapshot(remaining, &work_cancel.native).await?;
            let mut after = self.inventory.list(work_cancel.adb.clone()).await?;
            before.sort_by(|a, b| {
                a.transport_id
                    .cmp(&b.transport_id)
                    .then(a.serial.cmp(&b.serial))
                    .then(a.usb.cmp(&b.usb))
            });
            after.sort_by(|a, b| {
                a.transport_id
                    .cmp(&b.transport_id)
                    .then(a.serial.cmp(&b.serial))
                    .then(a.usb.cmp(&b.usb))
            });
            if before != after || self.load().await? != config {
                return Err(ErrorCode::DeviceIdentityAmbiguous);
            }
            Ok((snapshot, after, config))
        };
        let wait = async {
            tokio::pin!(work);
            loop {
                tokio::select! {
                    result=&mut work=>break result,
                    _=tokio::time::sleep(Duration::from_millis(5))=>{if cancel.cancelled(){break Err(ErrorCode::ProcessCancelled);}}
                }
            }
        };
        let (snapshot, after, config) =
            tokio::time::timeout_at(tokio::time::Instant::from_std(deadline), wait)
                .await
                .map_err(|_| ErrorCode::ProcessTimeout)??;
        if cancel.cancelled() {
            return Err(ErrorCode::ProcessCancelled);
        }
        if Instant::now() >= deadline {
            return Err(ErrorCode::ProcessTimeout);
        }
        Ok((snapshot, after, config))
    }
    pub async fn shutdown(&mut self, budget: Duration) -> Result<(), ErrorCode> {
        self.registry.lock().unwrap().invalidate()?;
        self.pairings = PairingConfig::default();
        self.worker.shutdown(budget).await
    }
}
