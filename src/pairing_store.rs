//! Linux private, FD-relative explicit pairing persistence. No automatic writes.
use crate::{
    api::{DeviceId, ErrorCode},
    identity::{Pairing, PairingConfig},
};
use std::{
    ffi::{CStr, CString},
    fs::File,
    io::{Read, Write},
    os::{
        fd::{AsRawFd, FromRawFd},
        unix::{ffi::OsStrExt, fs::MetadataExt},
    },
    path::{Component, Path},
};
const LIMIT: u64 = 64 * 1024;
#[derive(Debug, PartialEq, Eq, thiserror::Error)]
pub enum StoreError {
    #[error("pairing path must be owned, private and free of symlinks")]
    PrivatePath,
    #[error("pairing file is invalid or too large")]
    InvalidData,
    #[error("pairing store busy")]
    Busy,
    #[error("pairing I/O failed")]
    Io,
    #[error("pairing rename completed but durability uncertain; reload before retry")]
    CommitUncertain,
    #[error("pairing validation failed: {0}")]
    Validation(ErrorCode),
}
pub struct PairingStore {
    directory: File,
    #[cfg(test)]
    after_lock: std::sync::Mutex<Option<Box<dyn FnOnce() + Send>>>,
}
fn open_at(
    dir: &File,
    name: &CStr,
    flags: i32,
    mode: libc::mode_t,
) -> Result<File, std::io::Error> {
    let fd = unsafe {
        libc::openat(
            dir.as_raw_fd(),
            name.as_ptr(),
            flags | libc::O_CLOEXEC | libc::O_NOFOLLOW | libc::O_NONBLOCK,
            mode,
        )
    };
    if fd < 0 {
        return Err(std::io::Error::last_os_error());
    }
    Ok(unsafe { File::from_raw_fd(fd) })
}
fn private_file(file: &File) -> Result<(), StoreError> {
    let m = file.metadata().map_err(|_| StoreError::Io)?;
    if !m.is_file()
        || m.uid() != unsafe { libc::geteuid() }
        || m.mode() & 0o777 != 0o600
        || m.nlink() != 1
    {
        return Err(StoreError::PrivatePath);
    }
    Ok(())
}
// flock belongs to an open file description, which a fork inherits until exec.
// Closing only our descriptor can leave a completed operation locked by a child.
struct StoreLock(File);
impl StoreLock {
    fn finish<T>(
        self,
        result: Result<T, StoreError>,
        unlock_error: StoreError,
    ) -> Result<T, StoreError> {
        let unlocked = self.0.unlock().map_err(|_| unlock_error);
        match result {
            Err(error) => Err(error),
            Ok(value) => unlocked.map(|()| value),
        }
    }
}
impl Drop for StoreLock {
    fn drop(&mut self) {
        // Also release on early return/unwind. finish reports normal-path errors.
        let _ = self.0.unlock();
    }
}
struct Temp<'a> {
    name: CString,
    directory: &'a File,
}
impl Drop for Temp<'_> {
    fn drop(&mut self) {
        unsafe {
            libc::unlinkat(self.directory.as_raw_fd(), self.name.as_ptr(), 0);
        }
    }
}
impl PairingStore {
    /// Directory must already exist, be private/owned, and use an absolute path.
    /// Every path component is opened O_NOFOLLOW, then future I/O is FD-relative.
    pub fn open(path: &Path) -> Result<Self, StoreError> {
        if !path.is_absolute() {
            return Err(StoreError::PrivatePath);
        }
        let mut directory = File::open("/").map_err(|_| StoreError::Io)?;
        for part in path.components() {
            match part {
                Component::RootDir => {}
                Component::Normal(name) => {
                    let name =
                        CString::new(name.as_bytes()).map_err(|_| StoreError::PrivatePath)?;
                    directory = open_at(&directory, &name, libc::O_RDONLY | libc::O_DIRECTORY, 0)
                        .map_err(|_| StoreError::PrivatePath)?;
                }
                _ => return Err(StoreError::PrivatePath),
            }
        }
        let m = directory.metadata().map_err(|_| StoreError::Io)?;
        if !m.is_dir() || m.uid() != unsafe { libc::geteuid() } || m.mode() & 0o777 != 0o700 {
            return Err(StoreError::PrivatePath);
        }
        Ok(Self {
            directory,
            #[cfg(test)]
            after_lock: std::sync::Mutex::new(None),
        })
    }
    fn lock(&self) -> Result<StoreLock, StoreError> {
        let file = open_at(
            &self.directory,
            c".pairings.lock",
            libc::O_RDWR | libc::O_CREAT,
            0o600,
        )
        .map_err(|_| StoreError::PrivatePath)?;
        private_file(&file)?;
        match file.try_lock() {
            Ok(()) => {
                #[cfg(test)]
                if let Some(hook) = self.after_lock.lock().unwrap().take() {
                    hook();
                }
                Ok(StoreLock(file))
            }
            Err(std::fs::TryLockError::WouldBlock) => Err(StoreError::Busy),
            Err(_) => Err(StoreError::Io),
        }
    }
    fn read_locked(&self) -> Result<PairingConfig, StoreError> {
        let file = match open_at(&self.directory, c"pairings.json", libc::O_RDONLY, 0) {
            Ok(file) => file,
            Err(e) if e.kind() == std::io::ErrorKind::NotFound => {
                return Ok(PairingConfig::default());
            }
            Err(_) => return Err(StoreError::PrivatePath),
        };
        private_file(&file)?;
        if file.metadata().map_err(|_| StoreError::Io)?.len() > LIMIT {
            return Err(StoreError::InvalidData);
        }
        let mut bytes = Vec::new();
        file.take(LIMIT + 1)
            .read_to_end(&mut bytes)
            .map_err(|_| StoreError::Io)?;
        if bytes.len() as u64 > LIMIT {
            return Err(StoreError::InvalidData);
        }
        let config: PairingConfig =
            serde_json::from_slice(&bytes).map_err(|_| StoreError::InvalidData)?;
        config.validate().map_err(StoreError::Validation)?;
        Ok(config)
    }
    pub fn load(&self) -> Result<PairingConfig, StoreError> {
        let lock = self.lock()?;
        lock.finish(self.read_locked(), StoreError::Io)
    }
    pub fn set_pairing(&self, pairing: Pairing) -> Result<PairingConfig, StoreError> {
        self.update(move |config| config.set_pairing(pairing))
    }
    pub fn set_alias(
        &self,
        id: &DeviceId,
        alias: Option<String>,
    ) -> Result<PairingConfig, StoreError> {
        self.update(move |config| config.set_alias(id, alias))
    }
    pub fn remove_pairing(&self, id: &DeviceId) -> Result<PairingConfig, StoreError> {
        self.update(move |config| {
            config.pairings.retain(|p| &p.device_id != id);
            Ok(())
        })
    }
    fn update(
        &self,
        update: impl FnOnce(&mut PairingConfig) -> Result<(), ErrorCode>,
    ) -> Result<PairingConfig, StoreError> {
        let lock = self.lock()?;
        lock.finish(self.update_locked(update), StoreError::CommitUncertain)
    }
    fn update_locked(
        &self,
        update: impl FnOnce(&mut PairingConfig) -> Result<(), ErrorCode>,
    ) -> Result<PairingConfig, StoreError> {
        let mut config = self.read_locked()?;
        update(&mut config).map_err(StoreError::Validation)?;
        config.validate().map_err(StoreError::Validation)?;
        let bytes = serde_json::to_vec_pretty(&config).map_err(|_| StoreError::InvalidData)?;
        if bytes.len() as u64 > LIMIT {
            return Err(StoreError::InvalidData);
        }
        let mut nonce = [0; 16];
        getrandom::fill(&mut nonce).map_err(|_| StoreError::Io)?;
        let suffix = nonce.iter().map(|b| format!("{b:02x}")).collect::<String>();
        let name = CString::new(format!(".pairings-{suffix}.tmp")).map_err(|_| StoreError::Io)?;
        let mut file = open_at(
            &self.directory,
            &name,
            libc::O_WRONLY | libc::O_CREAT | libc::O_EXCL,
            0o600,
        )
        .map_err(|_| StoreError::Io)?;
        // Own cleanup only after O_EXCL proved this operation created the file.
        let temp = Temp {
            name,
            directory: &self.directory,
        };
        private_file(&file)?;
        file.write_all(&bytes).map_err(|_| StoreError::Io)?;
        file.sync_all().map_err(|_| StoreError::Io)?;
        let result = unsafe {
            libc::renameat(
                self.directory.as_raw_fd(),
                temp.name.as_ptr(),
                self.directory.as_raw_fd(),
                c"pairings.json".as_ptr(),
            )
        };
        if result != 0 {
            return Err(StoreError::Io);
        }
        // temp's unlink is harmless after rename; it never unlinks the final file.
        self.directory
            .sync_all()
            .map_err(|_| StoreError::CommitUncertain)?;
        Ok(config)
    }
}

#[derive(serde::Serialize, serde::Deserialize)]
#[serde(deny_unknown_fields)]
struct CalibrationRecord {
    device: DeviceId,
    profile: crate::frames::Profile,
}
fn calibration_name(id: &DeviceId) -> Result<CString, StoreError> {
    if id.0.is_empty() || id.0.len() > 128 {
        return Err(StoreError::InvalidData);
    }
    CString::new(format!(
        "calibration-{}.json",
        blake3::hash(id.0.as_bytes()).to_hex()
    ))
    .map_err(|_| StoreError::InvalidData)
}
impl PairingStore {
    /// Private daemon setup only. A caller must first obtain observed target proof.
    /// Serialization never makes an unverified profile verified.
    pub fn save_calibration(
        &self,
        device: &DeviceId,
        profile: &crate::frames::Profile,
    ) -> Result<(), StoreError> {
        if !profile.transform.verified {
            return Err(StoreError::Validation(ErrorCode::MappingUnverified));
        }
        profile
            .validate(
                &profile.build,
                &profile.descriptor,
                &profile.backend,
                profile.display,
                profile.rotation,
            )
            .map_err(StoreError::Validation)?;
        let name = calibration_name(device)?;
        let bytes = serde_json::to_vec(&CalibrationRecord {
            device: device.clone(),
            profile: profile.clone(),
        })
        .map_err(|_| StoreError::InvalidData)?;
        if bytes.len() > 4096 {
            return Err(StoreError::InvalidData);
        }
        let lock = self.lock()?;
        let result = (|| {
            let mut nonce = [0u8; 16];
            getrandom::fill(&mut nonce).map_err(|_| StoreError::Io)?;
            let temp_name = CString::new(format!(
                ".calibration-{}.tmp",
                blake3::hash(&nonce).to_hex()
            ))
            .map_err(|_| StoreError::Io)?;
            let mut file = open_at(
                &self.directory,
                &temp_name,
                libc::O_WRONLY | libc::O_CREAT | libc::O_EXCL,
                0o600,
            )
            .map_err(|_| StoreError::Io)?;
            let temp = Temp {
                name: temp_name,
                directory: &self.directory,
            };
            private_file(&file)?;
            file.write_all(&bytes).map_err(|_| StoreError::Io)?;
            file.sync_all().map_err(|_| StoreError::Io)?;
            if unsafe {
                libc::renameat(
                    self.directory.as_raw_fd(),
                    temp.name.as_ptr(),
                    self.directory.as_raw_fd(),
                    name.as_ptr(),
                )
            } != 0
            {
                return Err(StoreError::Io);
            }
            self.directory
                .sync_all()
                .map_err(|_| StoreError::CommitUncertain)
        })();
        lock.finish(result, StoreError::CommitUncertain)
    }
    pub fn load_calibration(
        &self,
        device: &DeviceId,
        expected: &crate::frames::Profile,
    ) -> Result<Option<crate::frames::Profile>, StoreError> {
        let name = calibration_name(device)?;
        let lock = self.lock()?;
        let result = (|| {
            let file = match open_at(&self.directory, &name, libc::O_RDONLY, 0) {
                Ok(file) => file,
                Err(e) if e.kind() == std::io::ErrorKind::NotFound => return Ok(None),
                Err(_) => return Err(StoreError::PrivatePath),
            };
            private_file(&file)?;
            let mut bytes = Vec::new();
            file.take(4097)
                .read_to_end(&mut bytes)
                .map_err(|_| StoreError::Io)?;
            if bytes.len() > 4096 {
                return Err(StoreError::InvalidData);
            }
            let record: CalibrationRecord =
                serde_json::from_slice(&bytes).map_err(|_| StoreError::InvalidData)?;
            if &record.device != device
                || !record.profile.transform.verified
                || record.profile.transform.max_x != expected.transform.max_x
                || record.profile.transform.max_y != expected.transform.max_y
            {
                return Err(StoreError::Validation(ErrorCode::MappingUnverified));
            }
            record
                .profile
                .validate(
                    &expected.build,
                    &expected.descriptor,
                    &expected.backend,
                    expected.display,
                    expected.rotation,
                )
                .map_err(StoreError::Validation)?;
            Ok(Some(record.profile))
        })();
        lock.finish(result, StoreError::Io)
    }
}

#[cfg(test)]
mod fork_regression {
    use super::*;
    use crate::{
        adb::Attachment,
        backend::Cancel,
        discovery::*,
        discovery_service::{DiscoveryService, InventorySource},
    };
    use std::time::{Duration, Instant};
    use tokio_util::sync::CancellationToken;

    // SOFTWARE FAKE inventory/USB; reload exercises the real private file store.
    struct FakeInventory;
    impl InventorySource for FakeInventory {
        async fn list(&self, _: CancellationToken) -> Result<Vec<Attachment>, ErrorCode> {
            Ok(vec![])
        }
    }
    struct FakeUsb;
    impl DiscoveryDriver for FakeUsb {
        fn kind(&self) -> DiscoveryKind {
            DiscoveryKind::SoftwareFake
        }
        fn scan(&mut self, _: Instant, _: &Cancel) -> Result<Vec<UsbDevice>, ErrorCode> {
            Ok(vec![])
        }
        fn close(&mut self) -> CloseStatus {
            CloseStatus::Consumed(Ok(()))
        }
    }
    struct Child {
        pid: libc::pid_t,
        release: i32,
    }
    struct ChildExit {
        wake_written: isize,
        expected: libc::pid_t,
        waited: libc::pid_t,
        status: i32,
    }
    impl ChildExit {
        fn assert_success(self) {
            assert_eq!(
                self.wake_written, 1,
                "fixture must write its explicit wake byte"
            );
            assert_eq!(
                self.waited, self.expected,
                "fixture child must be explicitly reaped"
            );
            assert!(
                libc::WIFEXITED(self.status),
                "fixture child must exit normally"
            );
            assert_eq!(
                libc::WEXITSTATUS(self.status),
                0,
                "fixture child must observe release before its deadline"
            );
        }
    }
    impl Child {
        fn release_and_reap(mut self) -> ChildExit {
            self.finish()
        }
        fn finish(&mut self) -> ChildExit {
            let expected = self.pid;
            // EOF is not a release signal: another pre-exec child may still
            // hold an inherited writer. A byte wakes this reader independently.
            let wake_written = unsafe { libc::write(self.release, [1_u8].as_ptr().cast(), 1) };
            unsafe {
                libc::close(self.release);
            }
            self.release = -1;
            let mut status = 0;
            let waited = unsafe { libc::waitpid(expected, &mut status, 0) };
            self.pid = 0;
            ChildExit {
                wake_written,
                expected,
                waited,
                status,
            }
        }
    }
    impl Drop for Child {
        fn drop(&mut self) {
            if self.pid != 0 {
                self.finish().assert_success();
            }
        }
    }
    struct ReleasePipe {
        read: std::os::fd::OwnedFd,
        write: std::os::fd::OwnedFd,
    }
    impl ReleasePipe {
        fn new() -> Self {
            let mut pipe = [-1; 2];
            assert_eq!(
                unsafe { libc::pipe2(pipe.as_mut_ptr(), libc::O_CLOEXEC) },
                0
            );
            Self {
                read: unsafe { std::os::fd::OwnedFd::from_raw_fd(pipe[0]) },
                write: unsafe { std::os::fd::OwnedFd::from_raw_fd(pipe[1]) },
            }
        }
    }
    fn inherit_until_released() -> Child {
        inherit_with_pipe(ReleasePipe::new())
    }
    fn inherit_with_pipe(pipe: ReleasePipe) -> Child {
        // Model an ADB fork paused before exec: CLOEXEC has not fired yet.
        // After multithreaded fork the child calls only async-signal-safe libc.
        let pid = unsafe { libc::fork() };
        assert!(pid >= 0);
        if pid == 0 {
            unsafe {
                libc::close(pipe.write.as_raw_fd());
                let mut fd = libc::pollfd {
                    fd: pipe.read.as_raw_fd(),
                    events: libc::POLLIN,
                    revents: 0,
                };
                let result = libc::poll(&mut fd, 1, 5000);
                if result <= 0 {
                    libc::_exit(1);
                }
                let mut wake = 0_u8;
                let received = libc::read(fd.fd, (&mut wake as *mut u8).cast(), 1);
                // HUP/EOF, an invalid byte or a failed read must not pass.
                libc::_exit(if received == 1 && wake == 1 { 0 } else { 2 });
            }
        }
        use std::os::fd::IntoRawFd;
        drop(pipe.read);
        Child {
            pid,
            release: pipe.write.into_raw_fd(),
        }
    }
    #[tokio::test]
    async fn public_reload_releases_lock_even_with_pre_exec_child_alive() {
        let dir = directory();
        let store = PairingStore::open(dir.path()).unwrap();
        let (send, receive) = std::sync::mpsc::channel();
        *store.after_lock.lock().unwrap() = Some(Box::new(move || {
            send.send(inherit_until_released()).unwrap();
        }));
        let worker = DiscoveryWorker::start(|| Ok(FakeUsb)).unwrap();
        let mut service = DiscoveryService::new(worker, FakeInventory, store).unwrap();
        service
            .reload_pairings(Duration::from_secs(1))
            .await
            .unwrap();
        let child = receive.recv_timeout(Duration::from_secs(1)).unwrap();
        let result = service.reload_pairings(Duration::from_secs(1)).await;
        drop(child);
        service.shutdown(Duration::from_secs(1)).await.unwrap();
        assert_eq!(
            result,
            Ok(()),
            "completed load must release its logical lock before child exec"
        );
    }
    fn directory() -> tempfile::TempDir {
        crate::fixtures::private_tempdir()
    }
    #[tokio::test]
    async fn public_reload_rejects_live_file_lock() {
        let dir = directory();
        let store = PairingStore::open(dir.path()).unwrap();
        let held = store.lock().unwrap();
        let worker = DiscoveryWorker::start(|| Ok(FakeUsb)).unwrap();
        let mut service = DiscoveryService::new(worker, FakeInventory, store).unwrap();
        assert_eq!(
            service.reload_pairings(Duration::from_secs(1)).await,
            Err(ErrorCode::QueueFull)
        );
        drop(held);
        assert_eq!(
            service.reload_pairings(Duration::from_secs(1)).await,
            Ok(())
        );
        service.shutdown(Duration::from_secs(1)).await.unwrap();
    }
    #[test]
    fn public_update_releases_inherited_lock() {
        let dir = directory();
        let store = PairingStore::open(dir.path()).unwrap();
        let (send, receive) = std::sync::mpsc::channel();
        *store.after_lock.lock().unwrap() = Some(Box::new(move || {
            send.send(inherit_until_released()).unwrap();
        }));
        store
            .remove_pairing(&DeviceId("synthetic_absent".into()))
            .unwrap();
        let child = receive.recv_timeout(Duration::from_secs(1)).unwrap();
        let result = store.load();
        drop(child);
        assert!(result.is_ok());
    }
    #[tokio::test]
    async fn dropped_public_reload_retains_store_capacity_until_load_finishes() {
        let dir = directory();
        let store = PairingStore::open(dir.path()).unwrap();
        let (entered, ready) = tokio::sync::oneshot::channel();
        let (release, wait) = std::sync::mpsc::channel();
        *store.after_lock.lock().unwrap() = Some(Box::new(move || {
            entered.send(()).unwrap();
            wait.recv_timeout(Duration::from_secs(2)).unwrap();
        }));
        let worker = DiscoveryWorker::start(|| Ok(FakeUsb)).unwrap();
        let mut service = DiscoveryService::new(worker, FakeInventory, store).unwrap();
        let mut pending = Box::pin(service.reload_pairings(Duration::from_secs(1)));
        tokio::select! {
            result = &mut pending => panic!("blocked load unexpectedly returned {result:?}"),
            result = ready => result.unwrap(),
        }
        drop(pending);
        let result = service.reload_pairings(Duration::from_secs(1)).await;
        release.send(()).unwrap();
        assert_eq!(result, Err(ErrorCode::QueueFull));
        service.shutdown(Duration::from_secs(1)).await.unwrap();
    }
    #[tokio::test]
    async fn overlapping_public_reload_and_update_fixtures_release_independently() {
        // Both writers exist before either fork, so each child inherits the
        // other fixture's writer. No child is released until both forks finish.
        let reload_pipe = ReleasePipe::new();
        let update_pipe = ReleasePipe::new();
        let reload_dir = directory();
        let update_dir = directory();
        let reload_store = PairingStore::open(reload_dir.path()).unwrap();
        let update_store = PairingStore::open(update_dir.path()).unwrap();
        let (reload_send, reload_receive) = std::sync::mpsc::channel();
        let (update_send, update_receive) = std::sync::mpsc::channel();
        *reload_store.after_lock.lock().unwrap() = Some(Box::new(move || {
            reload_send.send(inherit_with_pipe(reload_pipe)).unwrap();
        }));
        *update_store.after_lock.lock().unwrap() = Some(Box::new(move || {
            update_send.send(inherit_with_pipe(update_pipe)).unwrap();
        }));
        let worker = DiscoveryWorker::start(|| Ok(FakeUsb)).unwrap();
        let mut service = DiscoveryService::new(worker, FakeInventory, reload_store).unwrap();
        service
            .reload_pairings(Duration::from_secs(1))
            .await
            .unwrap();
        let reload_child = reload_receive.recv_timeout(Duration::from_secs(1)).unwrap();
        update_store
            .remove_pairing(&DeviceId("synthetic_absent".into()))
            .unwrap();
        let update_child = update_receive.recv_timeout(Duration::from_secs(1)).unwrap();
        let reload_result = service.reload_pairings(Duration::from_secs(1)).await;
        let update_result = update_store.load();
        // Reap both before asserting either outcome: failure must not strand the
        // other child or cause a second panic while unwinding its guard.
        let reload_exit = reload_child.release_and_reap();
        let mut status = 0;
        let update_still_waiting =
            unsafe { libc::waitpid(update_child.pid, &mut status, libc::WNOHANG) };
        let update_exit = update_child.release_and_reap();
        service.shutdown(Duration::from_secs(1)).await.unwrap();
        reload_exit.assert_success();
        update_exit.assert_success();
        assert_eq!(
            update_still_waiting, 0,
            "releasing one fixture must not release the other"
        );
        assert_eq!(reload_result, Ok(()));
        assert!(update_result.is_ok());
    }
}

#[cfg(test)]
mod calibration_tests {
    use super::*;
    use crate::{api::*, frames::Profile, geometry::Transform};
    #[test]
    fn calibration_roundtrip_is_bound_private_and_never_upgrades_unverified() {
        use std::os::unix::fs::symlink;
        let dir = crate::fixtures::private_tempdir();
        let store = PairingStore::open(dir.path()).unwrap();
        let id = DeviceId("synthetic-phone".into());
        let mut profile = Profile {
            build: "synthetic-build".into(),
            descriptor: "synthetic-descriptor".into(),
            backend: "synthetic-backend".into(),
            display: Size {
                width: 1080,
                height: 2220,
            },
            rotation: Rotation::R0,
            geometry: GeometryVersion(1),
            mapping: MappingVersion(1),
            transform: Transform {
                rotation: Rotation::R0,
                max_x: 32767,
                max_y: 32767,
                verified: false,
                generic_screen: None,
            },
        };
        assert!(store.save_calibration(&id, &profile).is_err());
        assert!(store.load_calibration(&id, &profile).unwrap().is_none());
        profile.transform.verified = true;
        store.save_calibration(&id, &profile).unwrap();
        assert!(
            store
                .load_calibration(&id, &profile)
                .unwrap()
                .unwrap()
                .transform
                .verified
        );
        assert!(
            store
                .load_calibration(&DeviceId("another-phone".into()), &profile)
                .unwrap()
                .is_none()
        );
        let mut changed = profile.clone();
        changed.build = "another-build".into();
        assert!(store.load_calibration(&id, &changed).is_err());
        changed = profile.clone();
        changed.transform.max_x = 4095;
        assert!(store.load_calibration(&id, &changed).is_err());
        let file = dir
            .path()
            .join(calibration_name(&id).unwrap().to_str().unwrap());
        let backup = dir.path().join("private-backup.json");
        std::fs::rename(&file, &backup).unwrap();
        symlink(&backup, &file).unwrap();
        assert!(store.load_calibration(&id, &profile).is_err());
    }
}
