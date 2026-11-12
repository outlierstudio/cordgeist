//! Client-side PNG publication. Metadata is written last and is not one transaction.
//! An interrupted export can leave a PNG without its sidecar; that pair is incomplete
//! and must not be treated as a successful screenshot. Existing paths are never replaced.
use crate::api::{Frame, PngImage};
use serde::Serialize;
use std::{
    fs::OpenOptions,
    io,
    os::unix::fs::{MetadataExt, OpenOptionsExt},
    path::{Component, Path},
};
#[derive(Serialize)]
#[serde(rename_all = "camelCase")]
struct Sidecar<'a> {
    frame: &'a Frame,
    mime_type: &'static str,
    interrupted_export: &'static str,
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
    if let Some(parent) = path.parent() {
        let meta = std::fs::symlink_metadata(parent)?;
        if !meta.is_dir()
            || meta.uid() != unsafe { libc::geteuid() }
            || meta.mode() & 0o777 != 0o700
        {
            return Err(denied());
        }
    }
    Ok(())
}
pub fn metadata_path(png: &Path) -> std::path::PathBuf {
    let mut name = png.file_name().unwrap_or_default().to_os_string();
    name.push(".meta.json");
    png.with_file_name(name)
}
fn decode(image: &PngImage) -> io::Result<Vec<u8>> {
    fn val(c: u8) -> io::Result<u8> {
        match c {
            b'A'..=b'Z' => Ok(c - b'A'),
            b'a'..=b'z' => Ok(c - b'a' + 26),
            b'0'..=b'9' => Ok(c - b'0' + 52),
            b'+' => Ok(62),
            b'/' => Ok(63),
            _ => Err(io::ErrorKind::InvalidData.into()),
        }
    }
    let bytes = image.base64.as_bytes();
    if !bytes.len().is_multiple_of(4) {
        return Err(io::ErrorKind::InvalidData.into());
    }
    let mut out = Vec::with_capacity(bytes.len() / 4 * 3);
    for chunk in bytes.chunks(4) {
        let pad = chunk.iter().rev().take_while(|c| **c == b'=').count();
        if pad > 2 {
            return Err(io::ErrorKind::InvalidData.into());
        }
        let n = (u32::from(val(chunk[0])?) << 18)
            | (u32::from(val(if chunk[1] == b'=' { b'A' } else { chunk[1] })?) << 12)
            | (u32::from(val(if chunk[2] == b'=' { b'A' } else { chunk[2] })?) << 6)
            | u32::from(val(if chunk[3] == b'=' { b'A' } else { chunk[3] })?);
        out.push((n >> 16) as u8);
        if pad < 2 {
            out.push((n >> 8) as u8);
        }
        if pad < 1 {
            out.push(n as u8);
        }
    }
    Ok(out)
}
fn absent(path: &Path) -> io::Result<()> {
    match std::fs::symlink_metadata(path) {
        Err(e) if e.kind() == io::ErrorKind::NotFound => Ok(()),
        Ok(_) => Err(io::ErrorKind::AlreadyExists.into()),
        Err(e) => Err(e),
    }
}
fn write_new(path: &Path, bytes: &[u8]) -> io::Result<()> {
    let mut file = OpenOptions::new()
        .write(true)
        .create_new(true)
        .mode(0o600)
        .custom_flags(libc::O_NOFOLLOW)
        .open(path)?;
    io::Write::write_all(&mut file, bytes)?;
    file.sync_all()?;
    Ok(())
}
/// Publishes the PNG, then the sidecar. `stop_after_png` leaves the incomplete pair.
pub fn publish(
    image: &PngImage,
    frame: &Frame,
    dest: &Path,
    stop_after_png: bool,
) -> io::Result<()> {
    check_path(dest)?;
    let meta = metadata_path(dest);
    absent(dest)?;
    absent(&meta)?;
    let png = decode(image)?;
    let loaded =
        image::load_from_memory(&png).map_err(|_| io::Error::from(io::ErrorKind::InvalidData))?;
    if loaded.width() != frame.image.width || loaded.height() != frame.image.height {
        return Err(io::ErrorKind::InvalidData.into());
    }
    let partial = dest.with_extension("png.partial");
    write_new(&partial, &png)?;
    std::fs::rename(&partial, dest)?;
    if stop_after_png {
        return Ok(());
    }
    let side = serde_json::to_vec_pretty(&Sidecar {
        frame,
        mime_type: "image/png",
        interrupted_export: "A PNG without this sidecar is an interrupted export and is not a successful screenshot.",
    })
    .map_err(|_| io::Error::from(io::ErrorKind::InvalidData))?;
    let partial_meta = meta.with_extension("partial");
    write_new(&partial_meta, &side)?;
    std::fs::rename(&partial_meta, &meta)?;
    Ok(())
}
