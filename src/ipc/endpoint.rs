//! Linux private endpoint ownership. All mutations are relative to a pinned FD.
use std::{
    fs::File,
    io,
    os::{
        fd::{AsRawFd, FromRawFd},
        unix::{fs::MetadataExt, net::UnixListener},
    },
    path::{Component, Path},
};
fn denied() -> io::Error {
    io::ErrorKind::PermissionDenied.into()
}
fn open(dir: &File, name: &std::ffi::CStr, flags: i32, mode: u32) -> io::Result<File> {
    let fd = unsafe {
        libc::openat(
            dir.as_raw_fd(),
            name.as_ptr(),
            flags | libc::O_CLOEXEC | libc::O_NOFOLLOW | libc::O_NONBLOCK,
            mode,
        )
    };
    if fd < 0 {
        Err(io::Error::last_os_error())
    } else {
        Ok(unsafe { File::from_raw_fd(fd) })
    }
}
fn private(file: &File, mode: u32) -> io::Result<()> {
    let m = file.metadata()?;
    if m.uid() != unsafe { libc::geteuid() } || m.mode() & 0o777 != mode {
        return Err(denied());
    }
    Ok(())
}
// A full live listener backlog must not turn startup into an unbounded wait.
fn probe(path: &Path) -> io::Result<()> {
    use std::os::unix::ffi::OsStrExt;
    let bytes = path.as_os_str().as_bytes();
    let mut address: libc::sockaddr_un = unsafe { std::mem::zeroed() };
    if bytes.len() >= address.sun_path.len() {
        return Err(denied());
    }
    address.sun_family = libc::AF_UNIX as _;
    for (target, byte) in address.sun_path.iter_mut().zip(bytes) {
        *target = *byte as _;
    }
    let fd = unsafe {
        libc::socket(
            libc::AF_UNIX,
            libc::SOCK_STREAM | libc::SOCK_CLOEXEC | libc::SOCK_NONBLOCK,
            0,
        )
    };
    if fd < 0 {
        return Err(io::Error::last_os_error());
    }
    let owned = unsafe { File::from_raw_fd(fd) };
    let result = unsafe {
        libc::connect(
            owned.as_raw_fd(),
            (&address as *const libc::sockaddr_un).cast(),
            std::mem::size_of_val(&address) as _,
        )
    };
    if result == 0 {
        Ok(())
    } else {
        Err(io::Error::last_os_error())
    }
}
pub struct Endpoint {
    directory: File,
    lock: File,
    listener: UnixListener,
    identity: (u64, u64),
}
impl Endpoint {
    /// The parent directory must exist. Creates only its private `cordgeist` child.
    pub fn bind(parent: &Path) -> io::Result<Self> {
        if !parent.is_absolute() {
            return Err(denied());
        }
        let mut directory = File::open("/")?;
        for part in parent.components() {
            match part {
                Component::RootDir => {}
                Component::Normal(n) => {
                    use std::os::unix::ffi::OsStrExt;
                    let name = std::ffi::CString::new(n.as_bytes()).map_err(|_| denied())?;
                    directory = open(&directory, &name, libc::O_RDONLY | libc::O_DIRECTORY, 0)?;
                }
                _ => return Err(denied()),
            }
        }
        private(&directory, 0o700)?;
        let made = unsafe { libc::mkdirat(directory.as_raw_fd(), c"cordgeist".as_ptr(), 0o700) };
        if made != 0 && io::Error::last_os_error().kind() != io::ErrorKind::AlreadyExists {
            return Err(io::Error::last_os_error());
        }
        directory = open(
            &directory,
            c"cordgeist",
            libc::O_RDONLY | libc::O_DIRECTORY,
            0,
        )?;
        private(&directory, 0o700)?;
        let lock = open(
            &directory,
            c"instance.lock",
            libc::O_RDWR | libc::O_CREAT,
            0o600,
        )?;
        private(&lock, 0o600)?;
        let m = lock.metadata()?;
        if !m.is_file() || m.nlink() != 1 {
            return Err(denied());
        }
        lock.try_lock()
            .map_err(|_| io::Error::from(io::ErrorKind::AddrInUse))?;
        let base = std::path::PathBuf::from(format!("/proc/self/fd/{}", directory.as_raw_fd()));
        let path = base.join("daemon.sock");
        match std::fs::symlink_metadata(&path) {
            Ok(m) => {
                use std::os::unix::fs::FileTypeExt;
                if !m.file_type().is_socket()
                    || m.uid() != unsafe { libc::geteuid() }
                    || m.mode() & 0o777 != 0o600
                {
                    return Err(denied());
                }
                match probe(&path) {
                    Ok(_) => return Err(io::ErrorKind::AddrInUse.into()),
                    Err(e) if e.kind() == io::ErrorKind::ConnectionRefused => {}
                    Err(e) => return Err(e),
                }
                std::fs::remove_file(&path)?;
            }
            Err(e) if e.kind() == io::ErrorKind::NotFound => {}
            Err(e) => return Err(e),
        }
        let listener = UnixListener::bind(&path)?;
        // Directory is private throughout bind; expose a 0600 socket before readiness.
        use std::os::unix::fs::PermissionsExt;
        let m = std::fs::symlink_metadata(&path)?;
        let endpoint = Self {
            directory,
            lock,
            listener,
            identity: (m.dev(), m.ino()),
        };
        // Arm inode-checked cleanup before subsequent fallible initialization.
        std::fs::set_permissions(&path, std::fs::Permissions::from_mode(0o600))?;
        endpoint.listener.set_nonblocking(true)?;
        Ok(endpoint)
    }
    pub fn listener(&self) -> &UnixListener {
        &self.listener
    }
}
impl Drop for Endpoint {
    fn drop(&mut self) {
        let path = std::path::PathBuf::from(format!(
            "/proc/self/fd/{}/daemon.sock",
            self.directory.as_raw_fd()
        ));
        if let Ok(m) = std::fs::symlink_metadata(&path)
            && (m.dev(), m.ino()) == self.identity
        {
            let _ = std::fs::remove_file(path);
        }
        // Explicit unlock avoids inherited descriptors extending the logical lock.
        let _ = self.lock.unlock();
    }
}
