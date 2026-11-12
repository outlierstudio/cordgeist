//! Private lease files. Sequence allocation is locked and never reused.
use crate::api::{ActionSeq, LeaseAuth, LeaseGrant};
use serde::{Deserialize, Serialize};
use std::{
    fs::{File, OpenOptions},
    io::{self, Read, Seek, SeekFrom, Write},
    os::unix::fs::{MetadataExt, OpenOptionsExt},
    path::{Component, Path},
};
#[derive(Clone, Serialize, Deserialize)]
#[serde(rename_all = "camelCase", deny_unknown_fields)]
struct Document {
    auth: LeaseAuth,
    next_seq: u64,
    idle_deadline_ms: u64,
    uncertain: Vec<u64>,
}
pub struct LeaseFile {
    path: std::path::PathBuf,
}
pub struct Allocation {
    file: File,
    doc: Document,
    seq: u64,
}
fn denied() -> io::Error {
    io::ErrorKind::PermissionDenied.into()
}
fn check_path(path: &Path) -> io::Result<()> {
    if !path.is_absolute()
        || path.components().any(|c| {
            matches!(
                c,
                Component::ParentDir | Component::CurDir | Component::Prefix(_)
            )
        })
    {
        return Err(denied());
    }
    Ok(())
}
fn owned_mode(path: &Path, mode: u32) -> io::Result<()> {
    let meta = std::fs::symlink_metadata(path)?;
    if !meta.is_file()
        || meta.nlink() != 1
        || meta.uid() != unsafe { libc::geteuid() }
        || meta.mode() & 0o777 != mode
    {
        return Err(denied());
    }
    Ok(())
}
impl LeaseFile {
    pub fn create(path: &Path, grant: &LeaseGrant) -> io::Result<Self> {
        check_path(path)?;
        if let Some(parent) = path.parent() {
            owned_mode_dir(parent)?;
        }
        let doc = Document {
            auth: grant.auth.clone(),
            next_seq: grant.next_seq.0,
            idle_deadline_ms: grant.idle_deadline_ms,
            uncertain: Vec::new(),
        };
        let mut file = OpenOptions::new()
            .write(true)
            .create_new(true)
            .mode(0o600)
            .custom_flags(libc::O_NOFOLLOW)
            .open(path)?;
        owned_mode(path, 0o600)?;
        write_doc(&mut file, &doc)?;
        Ok(Self { path: path.into() })
    }
    pub fn open(path: &Path) -> io::Result<Self> {
        check_path(path)?;
        let _ = open_locked(path)?;
        Ok(Self { path: path.into() })
    }
    pub fn auth(&self) -> io::Result<LeaseAuth> {
        Ok(read_locked(&self.path)?.auth)
    }
    pub fn allocate(&self) -> io::Result<Allocation> {
        let (file, doc) = open_locked(&self.path)?;
        if doc.next_seq == 0 {
            return Err(io::ErrorKind::InvalidData.into());
        }
        Ok(Allocation {
            file,
            seq: doc.next_seq,
            doc,
        })
    }
}
impl Allocation {
    pub fn seq(&self) -> ActionSeq {
        ActionSeq(self.seq)
    }
    pub fn auth(&self) -> &LeaseAuth {
        &self.doc.auth
    }
    /// Call only after the IPC request was fully written.
    pub fn commit_sent(&mut self, uncertain: bool) -> io::Result<()> {
        if uncertain {
            self.doc.uncertain.push(self.seq);
        }
        self.doc.next_seq = self.seq.saturating_add(1);
        write_doc(&mut self.file, &self.doc)?;
        Ok(())
    }
}
fn owned_mode_dir(path: &Path) -> io::Result<()> {
    let meta = std::fs::symlink_metadata(path)?;
    if !meta.is_dir() || meta.uid() != unsafe { libc::geteuid() } || meta.mode() & 0o777 != 0o700 {
        return Err(denied());
    }
    Ok(())
}
fn open_locked(path: &Path) -> io::Result<(File, Document)> {
    check_path(path)?;
    let mut file = OpenOptions::new()
        .read(true)
        .write(true)
        .custom_flags(libc::O_NOFOLLOW)
        .open(path)?;
    owned_mode(path, 0o600)?;
    file.lock()
        .map_err(|_| io::Error::from(io::ErrorKind::WouldBlock))?;
    let doc = read_doc(&mut file)?;
    Ok((file, doc))
}
fn read_locked(path: &Path) -> io::Result<Document> {
    Ok(open_locked(path)?.1)
}
fn read_doc(file: &mut File) -> io::Result<Document> {
    file.seek(SeekFrom::Start(0))?;
    let mut bytes = Vec::new();
    file.read_to_end(&mut bytes)?;
    if bytes.len() > 64 * 1024 {
        return Err(io::ErrorKind::InvalidData.into());
    }
    serde_json::from_slice(&bytes).map_err(|_| io::ErrorKind::InvalidData.into())
}
fn write_doc(file: &mut File, doc: &Document) -> io::Result<()> {
    let bytes = serde_json::to_vec(doc).map_err(|_| io::Error::from(io::ErrorKind::InvalidData))?;
    file.seek(SeekFrom::Start(0))?;
    file.set_len(0)?;
    file.write_all(&bytes)?;
    file.sync_all()?;
    Ok(())
}
